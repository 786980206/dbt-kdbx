#include "adbc_kdbx_private.h"
#include "nanoarrow/nanoarrow_ipc.h"

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

AdbcStatusCode AdbcKdbxStatementNew(struct AdbcConnection* conn, struct AdbcStatement* stmt,
                                    struct AdbcError* error) {
  if (!stmt) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }
  stmt->private_data = calloc(1, sizeof(KdbxStatement));
  if (!stmt->private_data) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }

  KdbxStatement* ks = (KdbxStatement*)stmt->private_data;
  if (conn && conn->private_data) {
    ks->conn_handle = ((KdbxConnection*)conn->private_data)->handle;
  }
  ks->ingest_table = NULL;
  ks->ingest_mode = NULL;
  ArrowBufferInit(&ks->ingest_ipc);
  ks->ingest_active = 0;
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxStatementRelease(struct AdbcStatement* stmt, struct AdbcError* error) {
  (void)error;
  if (!stmt) return ADBC_STATUS_OK;
  KdbxStatement* ks = (KdbxStatement*)stmt->private_data;
  if (ks) {
    free(ks->sql);
    free(ks->ingest_table);
    free(ks->ingest_mode);
    ArrowBufferReset(&ks->ingest_ipc);
    free(ks);
  }
  stmt->private_data = NULL;
  return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Query setup                                                        */
/* ------------------------------------------------------------------ */

AdbcStatusCode AdbcKdbxStatementSetSqlQuery(struct AdbcStatement* stmt, const char* query,
                                            struct AdbcError* error) {
  if (!stmt || !stmt->private_data) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!query) { SetError(error, "NULL query"); return ADBC_STATUS_INVALID_ARGUMENT; }
  KdbxStatement* ks = (KdbxStatement*)stmt->private_data;
  free(ks->sql);
  ks->sql = strdup(query);
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxStatementSetSubstraitPlan(struct AdbcStatement* stmt,
                                                 const uint8_t* plan, size_t len,
                                                 struct AdbcError* error) {
  (void)stmt; (void)plan; (void)len;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementPrepare(struct AdbcStatement* stmt, struct AdbcError* error) {
  (void)stmt;
  RETURN_NOT_IMPLEMENTED(error);
}

/* ------------------------------------------------------------------ */
/* Query execution — KDB+ IPC → Arrow IPC Stream                      */
/* ------------------------------------------------------------------ */

AdbcStatusCode AdbcKdbxStatementExecuteQuery(struct AdbcStatement* stmt,
                                             struct ArrowArrayStream* out, int64_t* affected,
                                             struct AdbcError* error) {
  if (!stmt || !stmt->private_data) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxStatement* kbs = (KdbxStatement*)stmt->private_data;
  if (kbs->conn_handle <= 0) { SetError(error, "not connected"); return ADBC_STATUS_INVALID_STATE; }

  if (kbs->ingest_active && !out) {
    const char* mode_short = kbs->ingest_mode;
    if (strncmp(mode_short, "adbc.ingest.mode.", 17) == 0) {
      mode_short += 17;
    }

    K k_table = ks((char*)kbs->ingest_table);
    K k_mode  = ks((char*)mode_short);
    K k_ipc   = ktn(KG, (int)kbs->ingest_ipc.size_bytes);
    memcpy(kG(k_ipc), kbs->ingest_ipc.data, kbs->ingest_ipc.size_bytes);

    K result = k(kbs->conn_handle, "AdbcStatementBulkIngest", k_table, k_mode, k_ipc, (K)0);
    if (!result) { SetError(error, "network error"); return ADBC_STATUS_IO; }
    if (result->t == -128) {
      SetError(error, result->s ? result->s : "KDB+ error");
      r0(result);
      return ADBC_STATUS_IO;
    }
    if (result->t != -KJ && result->t != -KI && result->t != -KH && result->t != -KB) {
      r0(result);
      SetError(error, "expected long from BulkIngest");
      return ADBC_STATUS_INTERNAL;
    }
    int64_t count = result->j;
    r0(result);
    if (affected) *affected = count;
    return ADBC_STATUS_OK;
  }

  if (!kbs->sql) { SetError(error, "no query set"); return ADBC_STATUS_INVALID_STATE; }

  if (!out) {
    K result = k(kbs->conn_handle, "AdbcStatementExecuteUpdate", kp(kbs->sql), (K)0);
    if (!result) {
      SetError(error, "network error querying KDB+");
      return ADBC_STATUS_IO;
    }
    if (result->t == -128) {
      SetError(error, result->s ? result->s : "KDB+ query error");
      r0(result);
      return ADBC_STATUS_IO;
    }
    if (result->t != -KJ && result->t != -KI && result->t != -KH && result->t != -KB) {
      r0(result);
      SetError(error, "unexpected result type from KDB+ server (expected long)");
      return ADBC_STATUS_INTERNAL;
    }
    int64_t count = result->j;
    r0(result);
    if (affected) *affected = count;
    return ADBC_STATUS_OK;
  }

  /* Call server-side AdbcStatementExecuteQuery via KDB+ IPC */
  K result = k(kbs->conn_handle, "AdbcStatementExecuteQuery", kp(kbs->sql), (K)0);
  if (!result) {
    SetError(error, "network error querying KDB+");
    return ADBC_STATUS_IO;
  }

  /* Server error? (q signals error as type -128, message in r->s) */
  if (result->t == -128) {
    SetError(error, result->s ? result->s : "KDB+ query error");
    r0(result);
    return ADBC_STATUS_IO;
  }

  /* Expected: byte list (KG=4) or char list (KC=10) — Arrow IPC Stream */
  if (result->t != KG && result->t != KC) {
    SetError(error, "unexpected result type from KDB+ server");
    r0(result);
    return ADBC_STATUS_INTERNAL;
  }

  uint8_t* data = kG(result);
  int64_t data_len = result->n;

  if (data_len == 0) {
    /* Empty result — return an empty (released) stream */
    r0(result);
    memset(out, 0, sizeof(*out));
    if (affected) *affected = 0;
    return ADBC_STATUS_OK;
  }

  /* Copy K bytes into an ArrowBuffer (nanoarrow takes ownership) */
  struct ArrowBuffer buffer;
  ArrowBufferInit(&buffer);
  ArrowErrorCode ae = ArrowBufferAppend(&buffer, (const void*)data, (int64_t)data_len);
  r0(result);

  if (ae != NANOARROW_OK) {
    ArrowBufferReset(&buffer);
    SetError(error, "failed to copy IPC data from KDB+");
    return ADBC_STATUS_INTERNAL;
  }

  /* Wrap buffer as an Arrow IPC input stream */
  struct ArrowIpcInputStream input_stream;
  ae = ArrowIpcInputStreamInitBuffer(&input_stream, &buffer);
  if (ae != NANOARROW_OK) {
    ArrowBufferReset(&buffer);
    SetError(error, "ArrowIpcInputStreamInitBuffer failed");
    return ADBC_STATUS_INTERNAL;
  }

  /* Create an ArrowArrayStream from the IPC stream bytes */
  struct ArrowIpcArrayStreamReaderOptions options;
  memset(&options, 0, sizeof(options));
  options.field_index = -1;       /* read all columns */
  options.use_shared_buffers = 0; /* copy buffers */

  ae = ArrowIpcArrayStreamReaderInit(out, &input_stream, &options);
  if (ae != NANOARROW_OK) {
    input_stream.release(&input_stream);
    SetError(error, "ArrowIpcArrayStreamReaderInit failed — invalid IPC stream");
    return ADBC_STATUS_INTERNAL;
  }

  if (affected) *affected = -1; /* row count unknown for SELECT-style queries */
  return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* ExecuteSchema — KDB+ IPC → ArrowSchema                              */
/* ------------------------------------------------------------------ */

AdbcStatusCode AdbcKdbxStatementExecuteSchema(struct AdbcStatement* stmt,
                                              struct ArrowSchema* schema,
                                              struct AdbcError* error) {
  if (!stmt || !stmt->private_data) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!schema) { SetError(error, "NULL schema"); return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxStatement* ks = (KdbxStatement*)stmt->private_data;
  if (!ks->sql) { SetError(error, "no query set"); return ADBC_STATUS_INVALID_STATE; }
  if (ks->conn_handle <= 0) { SetError(error, "not connected"); return ADBC_STATUS_INVALID_STATE; }

  K result = k(ks->conn_handle, "AdbcStatementExecuteSchema", kp(ks->sql), (K)0);
  if (!result) { SetError(error, "network error querying KDB+"); return ADBC_STATUS_IO; }
  if (result->t == -128) {
    SetError(error, result->s ? result->s : "KDB+ query error");
    r0(result);
    return ADBC_STATUS_IO;
  }
  if (result->t != KG && result->t != KC) {
    SetError(error, "unexpected result type from KDB+ server");
    r0(result);
    return ADBC_STATUS_INTERNAL;
  }

  ArrowErrorCode rc = KdbxIpcToArrowSchema(kG(result), result->n, schema, error);
  r0(result);
  return (rc == NANOARROW_OK) ? ADBC_STATUS_OK : ADBC_STATUS_INTERNAL;
}

/* ------------------------------------------------------------------ */
/* Stubs (not yet implemented)                                        */
/* ------------------------------------------------------------------ */

AdbcStatusCode AdbcKdbxStatementExecutePartitions(struct AdbcStatement* stmt,
                                                    struct ArrowSchema* schema,
                                                    struct AdbcPartitions* parts,
                                                    int64_t* affected, struct AdbcError* error) {
  (void)stmt; (void)schema; (void)parts; (void)affected;
  RETURN_NOT_IMPLEMENTED(error);
}

/* Forward declaration — BindStream is defined below this function */
AdbcStatusCode AdbcKdbxStatementBindStream(struct AdbcStatement* stmt,
                                            struct ArrowArrayStream* stream,
                                            struct AdbcError* error);

AdbcStatusCode AdbcKdbxStatementBind(struct AdbcStatement* stmt, struct ArrowArray* values,
                                      struct ArrowSchema* schema, struct AdbcError* error) {
  if (!stmt || !stmt->private_data) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!values) { SetError(error, "NULL values"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!schema) { SetError(error, "NULL schema"); return ADBC_STATUS_INVALID_ARGUMENT; }

  /* Wrap the single array+schema into a temporary ArrowArrayStream */
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));

  /* ArrowBasicArrayStreamInit takes ownership of schema on success */
  ArrowErrorCode ae = ArrowBasicArrayStreamInit(&stream, schema, 1);
  if (ae != NANOARROW_OK) {
    SetError(error, "ArrowBasicArrayStreamInit failed");
    return ADBC_STATUS_INTERNAL;
  }

  /* Move ownership of the array into the stream (index 0) */
  ArrowBasicArrayStreamSetArray(&stream, 0, values);

  /* Delegate to the existing BindStream logic */
  AdbcStatusCode rc = AdbcKdbxStatementBindStream(stmt, &stream, error);

  /* Release the temporary stream (it owns both schema and array) */
  stream.release(&stream);

  return rc;
}

AdbcStatusCode AdbcKdbxStatementBindStream(struct AdbcStatement* stmt,
                                            struct ArrowArrayStream* stream,
                                            struct AdbcError* error) {
  if (!stmt || !stmt->private_data) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!stream) { SetError(error, "NULL stream"); return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxStatement* ks = (KdbxStatement*)stmt->private_data;

  struct ArrowSchema schema;
  if (stream->get_schema(stream, &schema) != NANOARROW_OK || !schema.release) {
    SetError(error, "failed to get schema from stream");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  struct ArrowIpcEncoder encoder;
  ArrowIpcEncoderInit(&encoder);

  struct ArrowError arrow_error;
  ArrowIpcEncoderEncodeSchema(&encoder, &schema, &arrow_error);

  struct ArrowBuffer ipc_buffer;
  ArrowBufferInit(&ipc_buffer);
  ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &ipc_buffer);

  struct ArrowArray batch;
  while (stream->get_next(stream, &batch) == NANOARROW_OK && batch.release) {
    struct ArrowArrayView array_view;
    ArrowArrayViewInitFromSchema(&array_view, &schema, NULL);
    ArrowArrayViewAllocateChildren(&array_view, schema.n_children);
    ArrowArrayViewSetArray(&array_view, &batch, NULL);

    struct ArrowBuffer body_buffer;
    ArrowBufferInit(&body_buffer);
    ArrowIpcEncoderEncodeSimpleRecordBatch(&encoder, &array_view, &body_buffer, &arrow_error);
    ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &ipc_buffer);
    ArrowBufferAppend(&ipc_buffer, body_buffer.data, body_buffer.size_bytes);
    ArrowBufferReset(&body_buffer);

    ArrowArrayViewReset(&array_view);
    ArrowArrayRelease(&batch);
  }

  ArrowIpcEncoderReset(&encoder);
  schema.release(&schema);

  ArrowBufferReset(&ks->ingest_ipc);
  ks->ingest_ipc = ipc_buffer;
  ks->ingest_active = 1;
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxStatementGetParameterSchema(struct AdbcStatement* stmt,
                                                    struct ArrowSchema* schema,
                                                    struct AdbcError* error) {
  (void)stmt; (void)schema;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementSetOption(struct AdbcStatement* stmt, const char* k,
                                           const char* v, struct AdbcError* error) {
  if (!stmt || !stmt->private_data) { SetError(error, "NULL statement"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!k || !v) { SetError(error, "NULL key or value"); return ADBC_STATUS_INVALID_ARGUMENT; }
  KdbxStatement* ks = (KdbxStatement*)stmt->private_data;

  if (strcmp(k, "adbc.ingest.target_table") == 0) {
    free(ks->ingest_table);
    ks->ingest_table = strdup(v);
    return ADBC_STATUS_OK;
  }
  if (strcmp(k, "adbc.ingest.mode") == 0) {
    free(ks->ingest_mode);
    ks->ingest_mode = strdup(v);
    return ADBC_STATUS_OK;
  }

  SetError(error, "unknown option");
  return ADBC_STATUS_INVALID_ARGUMENT;
}

AdbcStatusCode AdbcKdbxStatementSetOptionInt(struct AdbcStatement* stmt, const char* k,
                                              int64_t v, struct AdbcError* error) {
  (void)stmt; (void)k; (void)v;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementSetOptionDouble(struct AdbcStatement* stmt, const char* k,
                                                 double v, struct AdbcError* error) {
  (void)stmt; (void)k; (void)v;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementGetOption(struct AdbcStatement* stmt, const char* k,
                                           char* v, size_t* len, struct AdbcError* error) {
  (void)stmt; (void)k; (void)v; (void)len;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementGetOptionInt(struct AdbcStatement* stmt, const char* k,
                                              int64_t* v, struct AdbcError* error) {
  (void)stmt; (void)k; (void)v;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementGetOptionDouble(struct AdbcStatement* stmt, const char* k,
                                                 double* v, struct AdbcError* error) {
  (void)stmt; (void)k; (void)v;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxStatementCancel(struct AdbcStatement* stmt, struct AdbcError* error) {
  (void)stmt;
  RETURN_NOT_IMPLEMENTED(error);
}
