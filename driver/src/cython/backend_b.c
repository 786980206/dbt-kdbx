/*
 * adbc_kdbx_backend_b.c — Backend B: ADBC driver with embedded CPython + pykx.
 *
 * This .so is a full ADBC C driver (exports AdbcDriverKdbxBInit). It embeds
 * CPython, imports kdbx_adbc.bridge, and drives pykx in *normal* IPC mode
 * (SyncQConnection to a running q server — no embedded q, no KX licence).
 * q results are converted to pyarrow and exported zero-copy through the
 * Arrow C Data Interface (__arrow_c_stream__).
 *
 * A host process (Rust dbt adapter or C ADBC shell) dlopens this with
 * RTLD_GLOBAL (Python extension symbols must resolve globally) and calls
 * AdbcDriverKdbxBInit. The .so adds its own directory to sys.path via
 * dladdr so the kdbx_adbc package is importable without PYTHONPATH.
 */

#define _GNU_SOURCE
#include "arrow-adbc/adbc.h"

#include <Python.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#if defined(_WIN32)
  #define KDBX_B_EXPORT __declspec(dllexport)
#else
  #define KDBX_B_EXPORT __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ */
/* Embedded CPython + pykx kernel (bridge to kdbx_adbc.bridge)        */
/* ------------------------------------------------------------------ */

static PyObject* g_bridge = NULL;

/* Schema capsules are kept alive in a small linked list (the caller owns the
   struct copied out of the capsule; the underlying pyarrow objects must stay
   alive until the process / driver shuts down). Avoids the single-slot
   overwrite that crashes when an earlier schema is still referenced. */
typedef struct KdbxBHold {
    PyObject* capsule;
    struct KdbxBHold* next;
} KdbxBHold;
static KdbxBHold* g_holds = NULL;
static int g_init_done = 0;
static int g_db_count = 0;

/* Acquire the GIL safely whether we are the embedded interpreter owner or a
   guest inside an already-running interpreter (e.g. adbc-driver-manager). */
static PyGILState_STATE b_gil_enter(void) {
    if (!Py_IsInitialized()) {
        Py_Initialize();
        return PyGILState_Ensure();
    }
    return PyGILState_Ensure();
}

static void b_gil_leave(PyGILState_STATE state) {
    PyGILState_Release(state);
}

static int kdbx_b_init(void) {
    if (g_init_done) return 0;
    PyGILState_STATE g = b_gil_enter();
    if (g_bridge == NULL) {
        /* Add this .so's directory (and its parent, the package root) to
           sys.path so kdbx_adbc.bridge is importable without PYTHONPATH. */
        Dl_info info;
        if (dladdr((void*)&kdbx_b_init, &info) && info.dli_fname) {
            char dir[4096];
            char parent[4096];
            strncpy(dir, info.dli_fname, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            char* slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            strncpy(parent, dir, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            PyObject* sys_mod = PyImport_ImportModule("sys");
            if (sys_mod) {
                PyObject* path = PyObject_GetAttrString(sys_mod, "path");
                if (path) {
                    PyObject* p1 = PyUnicode_FromString(dir);
                    PyObject* p2 = PyUnicode_FromString(parent);
                    if (p1) { PyList_Insert(path, 0, p1); Py_DECREF(p1); }
                    if (p2) { PyList_Insert(path, 0, p2); Py_DECREF(p2); }
                    Py_DECREF(path);
                }
                Py_DECREF(sys_mod);
            }
        }
        g_bridge = PyImport_ImportModule("kdbx_adbc.bridge");
        if (g_bridge == NULL) { PyErr_Print(); b_gil_leave(g); return -1; }
    }
    g_init_done = 1;
    b_gil_leave(g);
    return 0;
}

static int kdbx_b_connect(const char* host, int port) {
    if (kdbx_b_init() != 0) return -1;
    PyGILState_STATE g = b_gil_enter();
    PyObject* result = PyObject_CallMethod(g_bridge, "connect", "si", host, port);
    if (result == NULL) { PyErr_Print(); b_gil_leave(g); return -1; }
    Py_DECREF(result);
    b_gil_leave(g);
    return 0;
}

/* Per-stream owner: keeps the pyarrow stream capsule alive until the
   ArrowArrayStream is released by the consumer.

   The pyarrow stream we get from __arrow_c_stream__ uses its own
   private_data (and release/get_schema/get_next callbacks). We must NOT
   overwrite those; instead we return a *wrapper* ArrowArrayStream whose
   get_schema/get_next forward to the pyarrow stream, and whose release frees
   the capsule after the pyarrow release runs. */
typedef struct {
    struct ArrowArrayStream orig;
    PyObject* capsule;
} KdbxBStreamOwner;

static int b_stream_get_schema(struct ArrowArrayStream* stream,
                               struct ArrowSchema* out) {
    KdbxBStreamOwner* owner = (KdbxBStreamOwner*)stream->private_data;
    return owner->orig.get_schema(&owner->orig, out);
}

static int b_stream_get_next(struct ArrowArrayStream* stream,
                             struct ArrowArray* out) {
    KdbxBStreamOwner* owner = (KdbxBStreamOwner*)stream->private_data;
    return owner->orig.get_next(&owner->orig, out);
}

static const char* b_stream_get_last_error(struct ArrowArrayStream* stream) {
    KdbxBStreamOwner* owner = (KdbxBStreamOwner*)stream->private_data;
    if (owner->orig.get_last_error) {
        return owner->orig.get_last_error(&owner->orig);
    }
    return "";
}

static void b_stream_release(struct ArrowArrayStream* stream) {
    if (stream) {
        KdbxBStreamOwner* owner = (KdbxBStreamOwner*)stream->private_data;
        stream->release = NULL;
        stream->private_data = NULL;
        if (owner) {
            /* Drop our capsule reference. The capsule's own destructor calls
               the pyarrow stream release exactly once; we must NOT call
               orig.release here or the underlying Arrow stream is released
               twice (crash). */
            PyGILState_STATE g = b_gil_enter();
            if (owner->capsule) Py_DECREF(owner->capsule);
            b_gil_leave(g);
            free(owner);
        }
    }
}

/* Execute a bridge method that returns a pyarrow Table; export it as an
   ArrowArrayStream via __arrow_c_stream__ and return the stream struct.
   Each returned stream owns its capsule; the capsule is released when the
   consumer calls stream->release. */
static struct ArrowArrayStream* kdbx_b_stream(const char* method, const char* arg) {
    if (kdbx_b_init() != 0) return NULL;
    PyGILState_STATE g = b_gil_enter();
    PyObject* table;
    if (arg) {
        table = PyObject_CallMethod(g_bridge, method, "s", arg);
    } else {
        table = PyObject_CallMethod(g_bridge, method, NULL);
    }
    if (table == NULL) { PyErr_Print(); b_gil_leave(g); return NULL; }
    PyObject* reader = PyObject_CallMethod(table, "to_reader", NULL);
    Py_DECREF(table);
    if (reader == NULL) { PyErr_Print(); b_gil_leave(g); return NULL; }
    PyObject* capsule = PyObject_CallMethod(reader, "__arrow_c_stream__", NULL);
    Py_DECREF(reader);
    if (capsule == NULL) { PyErr_Print(); b_gil_leave(g); return NULL; }
    struct ArrowArrayStream* orig =
        (struct ArrowArrayStream*)PyCapsule_GetPointer(capsule, "arrow_array_stream");
    if (orig == NULL) { PyErr_Print(); Py_DECREF(capsule); b_gil_leave(g); return NULL; }
    /* Build a wrapper stream that forwards to the pyarrow stream and owns the
       capsule until released by the consumer. */
    KdbxBStreamOwner* owner = (KdbxBStreamOwner*)calloc(1, sizeof(KdbxBStreamOwner));
    if (!owner) { Py_DECREF(capsule); b_gil_leave(g); return NULL; }
    owner->orig = *orig;
    owner->capsule = capsule; /* own this reference */
    /* Return a wrapper stream: forwards to pyarrow's stream (whose own
       private_data/callbacks stay untouched) and owns the capsule until
       released. */
    struct ArrowArrayStream* wrapper = (struct ArrowArrayStream*)malloc(sizeof(struct ArrowArrayStream));
    if (!wrapper) { free(owner); Py_DECREF(capsule); b_gil_leave(g); return NULL; }
    wrapper->get_schema = b_stream_get_schema;
    wrapper->get_next = b_stream_get_next;
    wrapper->get_last_error = b_stream_get_last_error;
    wrapper->release = b_stream_release;
    wrapper->private_data = owner;
    b_gil_leave(g);
    return wrapper;
}

/* Execute a bridge method returning a long (e.g. execute_update). */
static int64_t kdbx_b_long(const char* method, const char* arg) {
    if (kdbx_b_init() != 0) return 0;
    PyGILState_STATE g = b_gil_enter();
    PyObject* r;
    if (arg) {
        r = PyObject_CallMethod(g_bridge, method, "s", arg);
    } else {
        r = PyObject_CallMethod(g_bridge, method, NULL);
    }
    if (r == NULL) { PyErr_Print(); b_gil_leave(g); return 0; }
    long long v = PyLong_AsLongLong(r);
    Py_DECREF(r);
    b_gil_leave(g);
    return (int64_t)v;
}

/* Execute a bridge method returning a pyarrow Schema; export as ArrowSchema. */
static int kdbx_b_schema(const char* method, const char* arg, struct ArrowSchema* out) {
    if (kdbx_b_init() != 0) return ADBC_STATUS_INTERNAL;
    PyGILState_STATE g = b_gil_enter();
    PyObject* schema_obj;
    if (arg) {
        schema_obj = PyObject_CallMethod(g_bridge, method, "s", arg);
    } else {
        schema_obj = PyObject_CallMethod(g_bridge, method, NULL);
    }
    if (schema_obj == NULL) { PyErr_Print(); b_gil_leave(g); return ADBC_STATUS_INTERNAL; }
    PyObject* capsule = PyObject_CallMethod(schema_obj, "__arrow_c_schema__", NULL);
    Py_DECREF(schema_obj);
    if (capsule == NULL) { PyErr_Print(); b_gil_leave(g); return ADBC_STATUS_INTERNAL; }
    struct ArrowSchema* schema =
        (struct ArrowSchema*)PyCapsule_GetPointer(capsule, "arrow_schema");
    if (schema == NULL) { PyErr_Print(); Py_DECREF(capsule); b_gil_leave(g); return ADBC_STATUS_INTERNAL; }
    /* Hold the capsule until shutdown; the struct we return is a shallow
       copy whose buffers live in pyarrow. */
    KdbxBHold* hold = (KdbxBHold*)malloc(sizeof(KdbxBHold));
    if (!hold) { Py_DECREF(capsule); b_gil_leave(g); return ADBC_STATUS_INTERNAL; }
    hold->capsule = capsule;
    hold->next = g_holds;
    g_holds = hold;
    memcpy(out, schema, sizeof(*out));
    b_gil_leave(g);
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* ADBC object state                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char* host;
    int   port;
} KdbxBDatabase;

typedef struct {
    int connected;
} KdbxBConnection;

typedef struct {
    char* sql;
} KdbxBStatement;

static void set_error(struct AdbcError* error, const char* msg) {
    if (error && msg) {
        if (error->message) free((char*)error->message);
        error->message = strdup(msg);
        error->release = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

static AdbcStatusCode b_db_new(struct AdbcDatabase* db, struct AdbcError* error) {
    KdbxBDatabase* d = (KdbxBDatabase*)calloc(1, sizeof(KdbxBDatabase));
    if (!d) { set_error(error, "out of memory"); return ADBC_STATUS_INTERNAL; }
    d->host = NULL;
    d->port = 0;
    db->private_data = d;
    g_db_count++;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_db_set_option(struct AdbcDatabase* db, const char* key,
                                      const char* value, struct AdbcError* error) {
    if (!db || !db->private_data) return ADBC_STATUS_INVALID_STATE;
    KdbxBDatabase* d = (KdbxBDatabase*)db->private_data;
    if (strcmp(key, "host") == 0) {
        if (d->host) free(d->host);
        d->host = strdup(value);
        return ADBC_STATUS_OK;
    }
    if (strcmp(key, "port") == 0) {
        char* end = NULL;
        long p = strtol(value, &end, 10);
        if (end == value || *end != '\0' || p <= 0 || p > 65535) {
            set_error(error, "invalid port");
            return ADBC_STATUS_INVALID_ARGUMENT;
        }
        d->port = (int)p;
        return ADBC_STATUS_OK;
    }
    if (strcmp(key, "uri") == 0) {
        /* parse kdbx://[user[:pass]@]host[:port][/db] */
        const char* p = strstr(value, "://");
        const char* host = p ? p + 3 : value;
        const char* at = strchr(host, '@');
        if (at) host = at + 1;
        char buf[256];
        size_t i = 0;
        while (host[i] && host[i] != ':' && host[i] != '/' && i < sizeof(buf) - 1) {
            buf[i] = host[i];
            i++;
        }
        buf[i] = '\0';
        if (buf[0] == '\0') {
            set_error(error, "empty host in URI");
            return ADBC_STATUS_INVALID_ARGUMENT;
        }
        if (host[i] == ':') {
            char* end = NULL;
            long port = strtol(host + i + 1, &end, 10);
            if (end == host + i + 1 || port <= 0 || port > 65535) {
                set_error(error, "invalid port in URI");
                return ADBC_STATUS_INVALID_ARGUMENT;
            }
            d->port = (int)port;
        }
        if (d->host) free(d->host);
        d->host = strdup(buf);
        return ADBC_STATUS_OK;
    }
    set_error(error, "unknown database option");
    return ADBC_STATUS_INVALID_ARGUMENT;
}

static AdbcStatusCode b_db_init(struct AdbcDatabase* db, struct AdbcError* error) {
    if (!db || !db->private_data) return ADBC_STATUS_INVALID_STATE;
    KdbxBDatabase* d = (KdbxBDatabase*)db->private_data;
    if (kdbx_b_connect(d->host ? d->host : "127.0.0.1", d->port ? d->port : 19500) != 0) {
        set_error(error, "failed to connect via pykx bridge");
        return ADBC_STATUS_IO;
    }
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_db_release(struct AdbcDatabase* db, struct AdbcError* error) {
    if (db && db->private_data) {
        KdbxBDatabase* d = (KdbxBDatabase*)db->private_data;
        if (d->host) free(d->host);
        free(d);
        db->private_data = NULL;
        /* Close the shared pykx bridge connection when the last database goes
           away; otherwise the embedded interpreter's pykx destructor blocks
           waiting on the q server at process exit. */
        if (g_db_count > 0 && --g_db_count == 0) {
            fprintf(stderr, "[backend_b] closing bridge (last db)\n");
            PyGILState_STATE g = b_gil_enter();
            PyObject* r = PyObject_CallMethod(g_bridge, "close", NULL);
            if (r) Py_DECREF(r); else PyErr_Print();
            b_gil_leave(g);
        }
    }
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

static AdbcStatusCode b_conn_new(struct AdbcConnection* conn, struct AdbcError* error) {
    KdbxBConnection* c = (KdbxBConnection*)calloc(1, sizeof(KdbxBConnection));
    if (!c) { set_error(error, "out of memory"); return ADBC_STATUS_INTERNAL; }
    c->connected = 0;
    conn->private_data = c;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_conn_init(struct AdbcConnection* conn, struct AdbcDatabase* db,
                                  struct AdbcError* error) {
    if (!conn || !conn->private_data) return ADBC_STATUS_INVALID_STATE;
    KdbxBConnection* c = (KdbxBConnection*)conn->private_data;
    c->connected = 1;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_conn_release(struct AdbcConnection* conn, struct AdbcError* error) {
    if (conn && conn->private_data) {
        free(conn->private_data);
        conn->private_data = NULL;
    }
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_conn_get_info(struct AdbcConnection* conn, const uint32_t* codes,
                                      size_t n, struct ArrowArrayStream* out,
                                      struct AdbcError* error) {
    (void)codes; (void)n;
    struct ArrowArrayStream* stream = kdbx_b_stream("get_info", NULL);
    if (!stream) { set_error(error, "get_info failed"); return ADBC_STATUS_IO; }
    *out = *stream;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_conn_get_objects(struct AdbcConnection* conn, int depth,
                                         const char* catalog, const char* schema,
                                         const char* table, const char** types,
                                         const char* column, struct ArrowArrayStream* out,
                                         struct AdbcError* error) {
    (void)depth; (void)catalog; (void)schema; (void)table; (void)types; (void)column;
    struct ArrowArrayStream* stream = kdbx_b_stream("get_objects", NULL);
    if (!stream) { set_error(error, "get_objects failed"); return ADBC_STATUS_IO; }
    *out = *stream;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_conn_get_table_schema(struct AdbcConnection* conn,
                                              const char* catalog, const char* schema,
                                              const char* table, struct ArrowSchema* out,
                                              struct AdbcError* error) {
    (void)catalog; (void)schema;
    if (!table) { set_error(error, "NULL table"); return ADBC_STATUS_INVALID_ARGUMENT; }
    return kdbx_b_schema("get_table_schema", table, out);
}

static AdbcStatusCode b_conn_get_table_types(struct AdbcConnection* conn,
                                             struct ArrowArrayStream* out,
                                             struct AdbcError* error) {
    struct ArrowArrayStream* stream = kdbx_b_stream("get_table_types", NULL);
    if (!stream) { set_error(error, "get_table_types failed"); return ADBC_STATUS_IO; }
    *out = *stream;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_conn_set_option(struct AdbcConnection* conn, const char* key,
                                        const char* value, struct AdbcError* error) {
    (void)conn; (void)value;
    if (strcmp(key, "adbc.connection.uri") == 0 ||
        strcmp(key, "adbc.connection.string") == 0) {
        return ADBC_STATUS_OK;
    }
    set_error(error, "unknown connection option");
    return ADBC_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------------ */
/* Statement                                                           */
/* ------------------------------------------------------------------ */

static AdbcStatusCode b_stmt_new(struct AdbcConnection* conn, struct AdbcStatement* stmt,
                                 struct AdbcError* error) {
    if (!stmt) return ADBC_STATUS_INVALID_ARGUMENT;
    KdbxBStatement* s = (KdbxBStatement*)calloc(1, sizeof(KdbxBStatement));
    if (!s) { set_error(error, "out of memory"); return ADBC_STATUS_INTERNAL; }
    s->sql = NULL;
    stmt->private_data = s;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_stmt_release(struct AdbcStatement* stmt, struct AdbcError* error) {
    if (stmt && stmt->private_data) {
        KdbxBStatement* s = (KdbxBStatement*)stmt->private_data;
        if (s->sql) free(s->sql);
        free(s);
        stmt->private_data = NULL;
    }
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_stmt_set_sql(struct AdbcStatement* stmt, const char* query,
                                     struct AdbcError* error) {
    if (!stmt || !stmt->private_data) return ADBC_STATUS_INVALID_STATE;
    KdbxBStatement* s = (KdbxBStatement*)stmt->private_data;
    if (s->sql) free(s->sql);
    s->sql = strdup(query);
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_stmt_execute_query(struct AdbcStatement* stmt,
                                           struct ArrowArrayStream* out, int64_t* affected,
                                           struct AdbcError* error) {
    if (!stmt || !stmt->private_data) return ADBC_STATUS_INVALID_STATE;
    KdbxBStatement* s = (KdbxBStatement*)stmt->private_data;
    if (!s->sql) { set_error(error, "no query set"); return ADBC_STATUS_INVALID_STATE; }
    /* ADBC execute_update is expressed as execute_query with out == NULL. */
    if (!out) {
        if (affected) *affected = kdbx_b_long("execute_update", s->sql);
        return ADBC_STATUS_OK;
    }
    struct ArrowArrayStream* stream = kdbx_b_stream("execute_query", s->sql);
    if (!stream) { set_error(error, "execute_query failed"); return ADBC_STATUS_IO; }
    *out = *stream;
    if (affected) *affected = -1;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode b_stmt_execute_schema(struct AdbcStatement* stmt,
                                            struct ArrowSchema* out, struct AdbcError* error) {
    if (!stmt || !stmt->private_data) return ADBC_STATUS_INVALID_STATE;
    KdbxBStatement* s = (KdbxBStatement*)stmt->private_data;
    if (!s->sql) { set_error(error, "no query set"); return ADBC_STATUS_INVALID_STATE; }
    return kdbx_b_schema("execute_schema", s->sql, out);
}

static AdbcStatusCode b_stmt_set_option(struct AdbcStatement* stmt, const char* key,
                                        const char* value, struct AdbcError* error) {
    (void)stmt; (void)value;
    if (strncmp(key, "adbc.ingest.", 12) == 0) return ADBC_STATUS_OK;
    set_error(error, "unknown statement option");
    return ADBC_STATUS_INVALID_ARGUMENT;
}

static AdbcStatusCode b_stmt_bind_stream(struct AdbcStatement* stmt,
                                         struct ArrowArrayStream* stream,
                                         struct AdbcError* error) {
    (void)stmt; (void)stream;
    set_error(error, "bind_stream not implemented in backend B");
    return ADBC_STATUS_NOT_IMPLEMENTED;
}

/* ------------------------------------------------------------------ */
/* Driver entry point                                                  */
/* ------------------------------------------------------------------ */

KDBX_B_EXPORT AdbcStatusCode AdbcDriverKdbxBInit(int version, void* raw_driver,
                                                 struct AdbcError* error) {
    if (version != ADBC_VERSION_1_1_0) {
        set_error(error, "unsupported ADBC version");
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }
    struct AdbcDriver* d = (struct AdbcDriver*)raw_driver;
    d->DatabaseNew = b_db_new;
    d->DatabaseSetOption = b_db_set_option;
    d->DatabaseInit = b_db_init;
    d->DatabaseRelease = b_db_release;
    d->ConnectionNew = b_conn_new;
    d->ConnectionSetOption = b_conn_set_option;
    d->ConnectionInit = b_conn_init;
    d->ConnectionRelease = b_conn_release;
    d->ConnectionGetInfo = b_conn_get_info;
    d->ConnectionGetObjects = b_conn_get_objects;
    d->ConnectionGetTableSchema = b_conn_get_table_schema;
    d->ConnectionGetTableTypes = b_conn_get_table_types;
    d->StatementNew = b_stmt_new;
    d->StatementRelease = b_stmt_release;
    d->StatementSetSqlQuery = b_stmt_set_sql;
    d->StatementExecuteQuery = b_stmt_execute_query;
    d->StatementExecuteSchema = b_stmt_execute_schema;
    d->StatementSetOption = b_stmt_set_option;
    d->StatementBindStream = b_stmt_bind_stream;
    return ADBC_STATUS_OK;
}

/* adbc-driver-manager derives the init symbol from the .so filename:
   `_backend_b.so` -> AdbcBackendBInit. Export it as an alias so the driver
   loads through the standard ADBC driver-manager path. */
KDBX_B_EXPORT AdbcStatusCode AdbcBackendBInit(int version, void* raw_driver,
                                              struct AdbcError* error) {
    return AdbcDriverKdbxBInit(version, raw_driver, error);
}