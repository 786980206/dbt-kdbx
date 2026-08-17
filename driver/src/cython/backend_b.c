/*
 * adbc_kdbx_backend_b.c — Backend B (embedded CPython + pykx) bridge.
 *
 * Exports C functions callable from the ADBC shell. This .so may be dlopen'd
 * by a host process that has no Python interpreter (e.g. a Rust dbt adapter
 * or the C ADBC shell). We lazily Py_Initialize() and import kdbx_adbc.bridge
 * on first use. q results are converted to pyarrow and exported via the
 * Arrow C Data Interface (__arrow_c_stream__), handed out zero-copy.
 *
 * No Cython module-init dependency: this is plain C embedding CPython.
 */

#include <Python.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

/* Arrow C Data Interface — the subset we need (mirror of ArrowArrayStream) */
struct ArrowSchema;
struct ArrowArray;
struct ArrowArrayStream;
typedef void (*ArrowSchemaRelease)(struct ArrowSchema*);
typedef void (*ArrowArrayRelease)(struct ArrowArray*);
typedef int (*ArrowStreamGetSchema)(struct ArrowArrayStream*, struct ArrowSchema*);
typedef int (*ArrowStreamGetNext)(struct ArrowArrayStream*, struct ArrowArray*);
typedef const char* (*ArrowStreamGetLastError)(struct ArrowArrayStream*);
typedef void (*ArrowStreamRelease)(struct ArrowArrayStream*);

struct ArrowSchema {
    char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema** children;
    struct ArrowSchema* dictionary;
    void* private_data;
    ArrowSchemaRelease release;
};
struct ArrowArray {
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void** buffers;
    struct ArrowArray** children;
    struct ArrowArray* dictionary;
    void* private_data;
    ArrowArrayRelease release;
};
struct ArrowArrayStream {
    ArrowStreamGetSchema get_schema;
    ArrowStreamGetNext get_next;
    ArrowStreamGetLastError get_last_error;
    ArrowStreamRelease release;
    void* private_data;
};

static PyObject* g_bridge = NULL;
static PyObject* g_held_capsule = NULL;
static int g_init_done = 0;

#if defined(_WIN32)
  #define KDBX_B_EXPORT __declspec(dllexport)
#else
  #define KDBX_B_EXPORT __attribute__((visibility("default")))
#endif

KDBX_B_EXPORT int kdbx_b_init(void) {
    if (g_init_done) return 0;
    if (!Py_IsInitialized()) Py_Initialize();
    /* Locate this .so via dladdr and put its directory on sys.path so that
       kdbx_adbc/bridge.py (packaged beside the .so) is importable even when
       the host process (e.g. a Rust dbt adapter) sets no PYTHONPATH. */
    Dl_info info;
    if (dladdr((void*)&kdbx_b_init, &info) && info.dli_fname) {
        char dir[4096];
        char parent[4096];
        strncpy(dir, info.dli_fname, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        /* this .so lives inside the kdbx_adbc/ package; add both its own dir
           and its parent (the package root) to sys.path so that both layouts
           (flat .so, or .so inside a package dir) resolve. */
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
    if (g_bridge == NULL) {
        g_bridge = PyImport_ImportModule("kdbx_adbc.bridge");
        if (g_bridge == NULL) { PyErr_Print(); return -1; }
    }
    g_init_done = 1;
    return 0;
}

KDBX_B_EXPORT int kdbx_b_connect(const char* host, int port) {
    if (kdbx_b_init() != 0) return -1;
    PyObject* result = PyObject_CallMethod(g_bridge, "connect", "si", host, port);
    if (result == NULL) { PyErr_Print(); return -1; }
    Py_DECREF(result);
    return 0;
}

KDBX_B_EXPORT uintptr_t kdbx_b_execute_query(const char* sql) {
    if (kdbx_b_init() != 0) return 0;
    PyObject* table = PyObject_CallMethod(g_bridge, "execute_query", "s", sql);
    if (table == NULL) { PyErr_Print(); return 0; }
    PyObject* reader = PyObject_CallMethod(table, "to_reader", NULL);
    Py_DECREF(table);
    if (reader == NULL) { PyErr_Print(); return 0; }
    PyObject* capsule = PyObject_CallMethod(reader, "__arrow_c_stream__", NULL);
    Py_DECREF(reader);
    if (capsule == NULL) { PyErr_Print(); return 0; }
    if (g_held_capsule != NULL) Py_DECREF(g_held_capsule);
    g_held_capsule = capsule;
    uintptr_t stream_ptr = (uintptr_t)PyCapsule_GetPointer(capsule, "arrow_array_stream");
    if (stream_ptr == 0) { PyErr_Print(); return 0; }
    return stream_ptr;
}

KDBX_B_EXPORT void kdbx_b_release_stream(void) {
    if (g_held_capsule != NULL) { Py_DECREF(g_held_capsule); g_held_capsule = NULL; }
}

KDBX_B_EXPORT void kdbx_b_shutdown(void) {
    if (g_held_capsule != NULL) { Py_DECREF(g_held_capsule); g_held_capsule = NULL; }
    if (g_bridge != NULL) { Py_DECREF(g_bridge); g_bridge = NULL; }
    g_init_done = 0;
    if (Py_IsInitialized()) Py_Finalize();
}