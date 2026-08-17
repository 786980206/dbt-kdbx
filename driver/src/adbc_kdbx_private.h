#ifndef ADBC_KDBX_PRIVATE_H
#define ADBC_KDBX_PRIVATE_H

#include <stdlib.h>
#include <string.h>

#include "arrow-adbc/adbc.h"
#include "k.h"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow_ipc.h"

/* ------------------------------------------------------------------ */
/* Internal structs — stored in ADBC handle private_data              */
/* ------------------------------------------------------------------ */

typedef struct {
  char* host;    /* e.g. "127.0.0.1" */
  int   port;    /* e.g. 9500        */
  char* user;    /* "user:password" or NULL */
} KdbxDatabase;

typedef struct {
  int handle;    /* KDB+ socket handle from khpu(); 0 = disconnected */
} KdbxConnection;

typedef struct {
  char* sql;         /* SQL query string (NULL if not set) */
  int   conn_handle; /* KDB+ socket handle from khpu(); 0 = not connected */
  /* Bulk ingest state */
  char* ingest_table;          /* target table name (NULL if not set) */
  char* ingest_mode;           /* "create"/"replace"/"append"/"create_append" */
  struct ArrowBuffer ingest_ipc; /* serialized IPC bytes for bulk ingest */
  int   ingest_active;         /* 1 if ingest options are set and data is bound */
} KdbxStatement;

/* ------------------------------------------------------------------ */
/* Error helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void KdbxErrorRelease(struct AdbcError* error) {
  if (error && error->message) {
    free(error->message);
    error->message = NULL;
  }
}

static inline void SetError(struct AdbcError* error, const char* msg) {
  if (!error) return;
  if (error->release && error->message) error->release(error);
  error->message = strdup(msg);
  error->release = KdbxErrorRelease;
}

#define RETURN_NOT_IMPLEMENTED(error)          \
  do {                                         \
    SetError((error), "not implemented");       \
    return ADBC_STATUS_NOT_IMPLEMENTED;         \
  } while (0)

/* ------------------------------------------------------------------ */
/* IPC bytes → ArrowSchema helper                                     */
/*                                                                    */
/* Decodes IPC bytes from KDB+ (an empty table carrying the Arrow     */
/* schema) and extracts the schema into *out.                         */
/* ------------------------------------------------------------------ */

static inline ArrowErrorCode KdbxIpcToArrowSchema(
    const uint8_t* ipc_bytes, int64_t ipc_size,
    struct ArrowSchema* out, struct AdbcError* error) {

  if (!ipc_bytes || ipc_size <= 0) {
    SetError(error, "empty IPC data");
    return -1;
  }

  ArrowErrorCode rc;

  struct ArrowBuffer buffer;
  ArrowBufferInit(&buffer);
  rc = ArrowBufferAppend(&buffer, ipc_bytes, ipc_size);
  if (rc != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "buffer alloc"); return rc; }

  struct ArrowIpcInputStream input;
  rc = ArrowIpcInputStreamInitBuffer(&input, &buffer);
  if (rc != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "ipc input init"); return rc; }

  struct ArrowArrayStream stream;
  rc = ArrowIpcArrayStreamReaderInit(&stream, &input, NULL);
  if (rc != NANOARROW_OK) {
    input.release(&input);
    SetError(error, "ipc stream init");
    return rc;
  }

  memset(out, 0, sizeof(*out));
  rc = stream.get_schema(&stream, out);
  stream.release(&stream);

  if (rc != NANOARROW_OK) {
    SetError(error, "get_schema failed");
    return rc;
  }

  return NANOARROW_OK;
}

/* ------------------------------------------------------------------ */
/* Encode Arrow schema+array into an ArrowArrayStream via IPC          */
/*                                                                    */
/* Encodes the schema and record batch into IPC format and initializes */
/* *out as an ArrowArrayStream reading the encoded data.               */
/* ------------------------------------------------------------------ */

static inline ArrowErrorCode KdbxEncodeArrowIPC(
    struct ArrowSchema* schema,
    struct ArrowArray* array,
    struct ArrowArrayStream* out,
    struct AdbcError* error) {

  struct ArrowArrayView array_view;
  ArrowArrayViewInitFromSchema(&array_view, schema, NULL);
  ArrowArrayViewAllocateChildren(&array_view, schema->n_children);
  ArrowArrayViewSetArray(&array_view, array, NULL);

  struct ArrowIpcEncoder encoder;
  ArrowIpcEncoderInit(&encoder);
  struct ArrowError arrow_error;
  struct ArrowBuffer ipc_buffer;
  ArrowBufferInit(&ipc_buffer);

  ArrowIpcEncoderEncodeSchema(&encoder, schema, &arrow_error);
  ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &ipc_buffer);

  struct ArrowBuffer body_buffer;
  ArrowBufferInit(&body_buffer);
  ArrowIpcEncoderEncodeSimpleRecordBatch(&encoder, &array_view, &body_buffer, &arrow_error);
  ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &ipc_buffer);
  ArrowBufferAppend(&ipc_buffer, body_buffer.data, body_buffer.size_bytes);
  ArrowBufferReset(&body_buffer);
  ArrowIpcEncoderReset(&encoder);

  struct ArrowIpcInputStream input_stream;
  ArrowIpcInputStreamInitBuffer(&input_stream, &ipc_buffer);
  struct ArrowIpcArrayStreamReaderOptions stream_opts;
  memset(&stream_opts, 0, sizeof(stream_opts));
  stream_opts.field_index = -1;
  stream_opts.use_shared_buffers = 0;

  ArrowErrorCode rc = ArrowIpcArrayStreamReaderInit(out, &input_stream, &stream_opts);
  if (rc != NANOARROW_OK) {
    input_stream.release(&input_stream);
    ArrowArrayViewReset(&array_view);
    SetError(error, "KdbxEncodeArrowIPC: ArrowIpcArrayStreamReaderInit failed");
    return rc;
  }

  ArrowArrayViewReset(&array_view);
  return NANOARROW_OK;
}

#endif
