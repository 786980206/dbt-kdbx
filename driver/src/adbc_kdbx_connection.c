#include "adbc_kdbx_private.h"
#include "nanoarrow/nanoarrow_ipc.h"
#include <stdio.h>

AdbcStatusCode AdbcKdbxConnectionNew(struct AdbcConnection* conn, struct AdbcError* error) {
  if (!conn) { SetError(error, "NULL connection"); return ADBC_STATUS_INVALID_ARGUMENT; }
  conn->private_data = calloc(1, sizeof(KdbxConnection));
  if (!conn->private_data) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxConnectionSetOption(struct AdbcConnection* conn, const char* k,
                                            const char* v, struct AdbcError* error) {
  (void)conn; (void)k; (void)v;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxConnectionInit(struct AdbcConnection* conn, struct AdbcDatabase* db,
                                      struct AdbcError* error) {
  if (!conn || !conn->private_data) { SetError(error, "NULL connection"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!db || !db->private_data) { SetError(error, "NULL database"); return ADBC_STATUS_INVALID_ARGUMENT; }
  KdbxDatabase* kdb = (KdbxDatabase*)db->private_data;
  if (!kdb->host || kdb->port <= 0) { SetError(error, "database not initialised"); return ADBC_STATUS_INVALID_STATE; }

  KdbxConnection* kc = (KdbxConnection*)conn->private_data;
  if (kc->handle != 0) { SetError(error, "already connected"); return ADBC_STATUS_INVALID_STATE; }

  int handle = khpu(kdb->host, kdb->port, kdb->user ? kdb->user : "");
  if (handle == 0) {
    SetError(error, "khpu connection failed");
    return ADBC_STATUS_IO;
  }
  kc->handle = handle;
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxConnectionRelease(struct AdbcConnection* conn, struct AdbcError* error) {
  (void)error;
  if (!conn) return ADBC_STATUS_OK;
  KdbxConnection* kc = (KdbxConnection*)conn->private_data;
  if (kc) {
    if (kc->handle != 0) {
      kclose(kc->handle);
      kc->handle = 0;
    }
    free(kc);
  }
  conn->private_data = NULL;
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxConnectionGetInfo(struct AdbcConnection* conn, const uint32_t* codes,
                                          size_t n, struct ArrowArrayStream* out,
                                          struct AdbcError* error) {
  if (!conn || !conn->private_data) { SetError(error, "NULL connection"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!out) { SetError(error, "NULL output stream"); return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxConnection* kc = (KdbxConnection*)conn->private_data;
  if (kc->handle == 0) { SetError(error, "not connected"); return ADBC_STATUS_INVALID_STATE; }

  K code_list = ktn(KI, (int)n);
  int32_t* dst = kI(code_list);
  for (size_t i = 0; i < n; i++) {
    dst[i] = (int32_t)codes[i];
  }

  K result = k(kc->handle, "AdbcConnectionGetInfo", code_list, (K)0);
  if (!result) {
    SetError(error, "network error calling AdbcConnectionGetInfo");
    return ADBC_STATUS_IO;
  }

  if (result->t == -128) {
    SetError(error, result->s ? result->s : "KDB+ error in AdbcConnectionGetInfo");
    r0(result);
    return ADBC_STATUS_IO;
  }

  if (result->t == KG || result->t == KC) {
    uint8_t* data = kG(result);
    int64_t data_len = result->n;
    struct ArrowBuffer buffer;
    ArrowBufferInit(&buffer);
    ArrowErrorCode ae = ArrowBufferAppend(&buffer, data, data_len);
    r0(result);
    if (ae != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "failed to copy IPC data"); return ADBC_STATUS_INTERNAL; }
    struct ArrowIpcInputStream input_stream;
    ae = ArrowIpcInputStreamInitBuffer(&input_stream, &buffer);
    if (ae != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "ArrowIpcInputStreamInitBuffer failed"); return ADBC_STATUS_INTERNAL; }
    struct ArrowIpcArrayStreamReaderOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.field_index = -1;
    opts.use_shared_buffers = 0;
    ae = ArrowIpcArrayStreamReaderInit(out, &input_stream, &opts);
    if (ae != NANOARROW_OK) { input_stream.release(&input_stream); SetError(error, "ArrowIpcArrayStreamReaderInit failed"); return ADBC_STATUS_INTERNAL; }
    return ADBC_STATUS_OK;
  }

  if (result->t != XD) {
    r0(result);
    SetError(error, "unexpected result type from KDB+");
    return ADBC_STATUS_INTERNAL;
  }

  K keys = kK(result)[0];
  K vals = kK(result)[1];
  int64_t row_count = keys->n;

  /* Build schema: struct { info_name: uint32, info_value: utf8 } */
  struct ArrowSchema schema;
  ArrowSchemaInit(&schema);
  ArrowSchemaSetTypeStruct(&schema, 2);
  ArrowSchemaInitFromType(schema.children[0], NANOARROW_TYPE_UINT32);
  ArrowSchemaSetName(schema.children[0], "info_name");
  ArrowSchemaInitFromType(schema.children[1], NANOARROW_TYPE_STRING);
  ArrowSchemaSetName(schema.children[1], "info_value");

  /* Build array using nanoarrow builder APIs */
  struct ArrowArray array;
  ArrowArrayInitFromSchema(&array, &schema, NULL);
  ArrowArrayStartAppending(&array);

  for (int64_t i = 0; i < row_count; i++) {
    int32_t key = kI(keys)[i];
    ArrowArrayAppendUInt(array.children[0], (uint64_t)key);

    K val = kK(vals)[i];
    const char* val_str = "";
    int32_t val_len = 0;
    char vbuf[64];
    if (val->t == -KS) {
      val_str = val->s ? val->s : "";
      val_len = (int32_t)strlen(val_str);
    } else if (val->t == KC) {
      val_str = kC(val);
      val_len = (int32_t)val->n;
    } else if (val->t == KJ) {
      val_len = snprintf(vbuf, sizeof(vbuf), "%lld", (long long)val->j);
      val_str = vbuf;
    } else if (val->t == KI) {
      val_len = snprintf(vbuf, sizeof(vbuf), "%d", val->i);
      val_str = vbuf;
    } else if (val->t == KB) {
      val_str = val->g ? "true" : "false";
      val_len = (int32_t)strlen(val_str);
    }
    struct ArrowStringView str_view = ArrowCharView(val_str);
    str_view.size_bytes = val_len;
    ArrowArrayAppendString(array.children[1], str_view);
    ArrowArrayFinishElement(&array);
  }

  ArrowArrayFinishBuildingDefault(&array, NULL);

  ArrowErrorCode ae = KdbxEncodeArrowIPC(&schema, &array, out, error);
  if (ae != NANOARROW_OK) {
    r0(result);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&array);
    return ADBC_STATUS_INTERNAL;
  }

  r0(result);
  ArrowSchemaRelease(&schema);
  ArrowArrayRelease(&array);
  return ADBC_STATUS_OK;
}

static int KdbxTypeMatch(const char* tbl_type, const char** types) {
  if (!types) return 1;
  if (!tbl_type) return 0;
  for (int i = 0; types[i] != NULL; i++) {
    if (strcmp(tbl_type, types[i]) == 0) return 1;
  }
  return 0;
}

AdbcStatusCode AdbcKdbxConnectionGetObjects(struct AdbcConnection* conn, int depth,
                                              const char* cat, const char* db_schema,
                                              const char* table_name, const char** types,
                                              const char* col_name, struct ArrowArrayStream* out,
                                              struct AdbcError* error) {
  if (!conn || !conn->private_data) { SetError(error, "NULL connection"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!out) { SetError(error, "NULL output stream"); return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxConnection* kc = (KdbxConnection*)conn->private_data;
  if (kc->handle == 0) { SetError(error, "not connected"); return ADBC_STATUS_INVALID_STATE; }

  K depth_k = ki(depth);
  K cat_k = kp((char*)(cat ? cat : ""));
  K schema_k = kp((char*)(db_schema ? db_schema : ""));
  K tbl_k = kp((char*)(table_name ? table_name : ""));
  K col_k = kp((char*)(col_name ? col_name : ""));

  K result = k(kc->handle, "AdbcConnectionGetObjects", depth_k, cat_k, schema_k, tbl_k, col_k, (K)0);
  if (!result) {
    SetError(error, "network error calling AdbcConnectionGetObjects");
    return ADBC_STATUS_IO;
  }

  if (result->t == -128) {
    SetError(error, result->s ? result->s : "KDB+ error in AdbcConnectionGetObjects");
    r0(result);
    return ADBC_STATUS_IO;
  }

  if (result->t == KG || result->t == KC) {
    uint8_t* data = kG(result);
    int64_t data_len = result->n;
    if (data_len == 0) {
      r0(result);
      memset(out, 0, sizeof(*out));
      return ADBC_STATUS_OK;
    }
    struct ArrowBuffer buffer;
    ArrowBufferInit(&buffer);
    ArrowErrorCode ae = ArrowBufferAppend(&buffer, data, data_len);
    r0(result);
    if (ae != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "failed to copy IPC data"); return ADBC_STATUS_INTERNAL; }
    struct ArrowIpcInputStream input_stream;
    ae = ArrowIpcInputStreamInitBuffer(&input_stream, &buffer);
    if (ae != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "ArrowIpcInputStreamInitBuffer failed"); return ADBC_STATUS_INTERNAL; }
    struct ArrowIpcArrayStreamReaderOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.field_index = -1;
    opts.use_shared_buffers = 0;
    ae = ArrowIpcArrayStreamReaderInit(out, &input_stream, &opts);
    if (ae != NANOARROW_OK) { input_stream.release(&input_stream); SetError(error, "ArrowIpcArrayStreamReaderInit failed"); return ADBC_STATUS_INTERNAL; }
    return ADBC_STATUS_OK;
  }

  if (result->t == XT) {
    K dict = result->k;
    K col_syms = kK(dict)[0];
    K col_vecs = kK(dict)[1];
    int64_t ncols = col_syms->n;
    int64_t nrows = col_vecs->n > 0 ? kK(col_vecs)[0]->n : 0;

    struct ArrowSchema schema;
    ArrowSchemaInit(&schema);
    ArrowSchemaSetTypeStruct(&schema, ncols);
    for (int64_t c = 0; c < ncols; c++) {
      ArrowSchemaInitFromType(schema.children[c], NANOARROW_TYPE_STRING);
      ArrowSchemaSetName(schema.children[c], kS(col_syms)[c]);
    }

    struct ArrowArray array;
    ArrowArrayInitFromSchema(&array, &schema, NULL);
    ArrowArrayStartAppending(&array);

    for (int64_t i = 0; i < nrows; i++) {
      for (int64_t c = 0; c < ncols; c++) {
        K vec = kK(col_vecs)[c];
        struct ArrowStringView sv = ArrowCharView("");
        char vbuf[64];
        if (vec->t == -KS) {
          sv = ArrowCharView(vec->s ? vec->s : "");
        } else if (vec->t == KS) {
          sv = ArrowCharView(kS(vec)[i] ? kS(vec)[i] : "");
        } else if (vec->t == KC) {
          sv.data = kC(vec);
          sv.size_bytes = vec->n;
        } else if (vec->t == KJ) {
          int r = snprintf(vbuf, sizeof(vbuf), "%lld", (long long)kJ(vec)[i]);
          sv = ArrowCharView(vbuf);
          sv.size_bytes = r;
        } else if (vec->t == -KJ) {
          int r = snprintf(vbuf, sizeof(vbuf), "%lld", (long long)vec->j);
          sv = ArrowCharView(vbuf);
          sv.size_bytes = r;
        } else if (vec->t == KI) {
          int r = snprintf(vbuf, sizeof(vbuf), "%d", kI(vec)[i]);
          sv = ArrowCharView(vbuf);
          sv.size_bytes = r;
        } else if (vec->t == -KI) {
          int r = snprintf(vbuf, sizeof(vbuf), "%d", vec->i);
          sv = ArrowCharView(vbuf);
          sv.size_bytes = r;
        } else if (vec->t == KB) {
          sv = ArrowCharView(kG(vec)[i] ? "true" : "false");
        } else if (vec->t == KF) {
          int r = snprintf(vbuf, sizeof(vbuf), "%g", kF(vec)[i]);
          sv = ArrowCharView(vbuf);
          sv.size_bytes = r;
        }
        ArrowArrayAppendString(array.children[c], sv);
      }
      ArrowArrayFinishElement(&array);
    }

    ArrowArrayFinishBuildingDefault(&array, NULL);

    ArrowErrorCode ae = KdbxEncodeArrowIPC(&schema, &array, out, error);
    if (ae != NANOARROW_OK) {
      r0(result);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      return ADBC_STATUS_INTERNAL;
    }

    r0(result);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&array);
    return ADBC_STATUS_OK;
  }

  if (result->t == XD) {
    K keys = kK(result)[0];
    K vals = kK(result)[1];

    const char* catalog_name = "";
    K schemas_val = NULL;
    for (int64_t i = 0; i < keys->n; i++) {
      const char* kname = kS(keys)[i];
      if (strcmp(kname, "catalog_name") == 0) {
        K v = kK(vals)[i];
        if (v->t == -KS) catalog_name = v->s ? v->s : "";
        else if (v->t == KS) catalog_name = kS(v)[0] ? kS(v)[0] : "";
      } else if (strcmp(kname, "catalog_db_schemas") == 0) {
        schemas_val = kK(vals)[i];
      }
    }

    struct ArrowSchema schema;
    ArrowSchemaInit(&schema);
    ArrowSchemaSetTypeStruct(&schema, 2);

    ArrowSchemaInitFromType(schema.children[0], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(schema.children[0], "catalog_name");

    ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_LIST);
    ArrowSchemaSetName(schema.children[1], "catalog_db_schemas");
    schema.children[1]->flags = 0;

    ArrowSchemaSetTypeStruct(schema.children[1]->children[0], 2);
    ArrowSchemaSetName(schema.children[1]->children[0], "item");

    ArrowSchemaInitFromType(schema.children[1]->children[0]->children[0], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(schema.children[1]->children[0]->children[0], "db_schema_name");

    ArrowSchemaSetType(schema.children[1]->children[0]->children[1], NANOARROW_TYPE_LIST);
    ArrowSchemaSetName(schema.children[1]->children[0]->children[1], "db_schema_tables");
    schema.children[1]->children[0]->children[1]->flags = 0;

    ArrowSchemaSetTypeStruct(schema.children[1]->children[0]->children[1]->children[0], 4);
    ArrowSchemaSetName(schema.children[1]->children[0]->children[1]->children[0], "item");

    ArrowSchemaInitFromType(
        schema.children[1]->children[0]->children[1]->children[0]->children[0],
        NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[0],
        "table_name");

    ArrowSchemaInitFromType(
        schema.children[1]->children[0]->children[1]->children[0]->children[1],
        NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[1],
        "table_type");

    ArrowSchemaSetType(
        schema.children[1]->children[0]->children[1]->children[0]->children[2],
        NANOARROW_TYPE_LIST);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[2],
        "table_columns");
    schema.children[1]->children[0]->children[1]->children[0]->children[2]->flags = 0;

    ArrowSchemaSetTypeStruct(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0],
        4);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0],
        "item");

    ArrowSchemaInitFromType(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[0],
        NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[0],
        "column_name");

    ArrowSchemaInitFromType(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[1],
        NANOARROW_TYPE_INT32);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[1],
        "ordinal_position");

    ArrowSchemaInitFromType(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[2],
        NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[2],
        "remarks");

    ArrowSchemaInitFromType(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[3],
        NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[3],
        "xdbc_type_name");

    ArrowSchemaSetType(
        schema.children[1]->children[0]->children[1]->children[0]->children[3],
        NANOARROW_TYPE_LIST);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[3],
        "table_constraints");
    schema.children[1]->children[0]->children[1]->children[0]->children[3]->flags = 0;

    ArrowSchemaSetTypeStruct(
        schema.children[1]->children[0]->children[1]->children[0]->children[3]->children[0],
        0);
    ArrowSchemaSetName(
        schema.children[1]->children[0]->children[1]->children[0]->children[3]->children[0],
        "item");

    /* Build arrays */
    struct ArrowArray array;
    ArrowArrayInitFromSchema(&array, &schema, NULL);
    ArrowArrayStartAppending(&array);

    /* Row 0: catalog_name */
    struct ArrowStringView cat_sv = ArrowCharView(catalog_name);
    ArrowArrayAppendString(array.children[0], cat_sv);

    /* schemas_val can be XT (table, type 98) or 0 (generic list of dicts) */
    int64_t schema_count = 0;
    if (schemas_val && schemas_val->t == XT) {
      K sd = schemas_val->k;
      K sckeys = kK(sd)[0];
      K scvals = kK(sd)[1];
      if (sckeys->n > 0) {
        K first_col = kK(scvals)[0];
        schema_count = first_col->n;
      }
    } else if (schemas_val && schemas_val->t == 0) {
      schema_count = schemas_val->n;
    }

    for (int64_t si = 0; si < schema_count; si++) {
      const char* schema_name = "";
      K tables_val = NULL;

      if (schemas_val->t == XT) {
        K ckeys = kK(schemas_val->k)[0];
        K cvecs = kK(schemas_val->k)[1];
        int64_t ncols = ckeys->n;
        int64_t idx_name = -1, idx_tables = -1;
        for (int64_t c = 0; c < ncols; c++) {
          const char* cn = kS(ckeys)[c];
          if (strcmp(cn, "db_schema_name") == 0) idx_name = c;
          else if (strcmp(cn, "db_schema_tables") == 0) idx_tables = c;
        }
        if (idx_name >= 0) {
          K v = kK(cvecs)[idx_name];
          if (v->t == -KS) schema_name = v->s ? v->s : "";
          else if (v->t == KS) schema_name = kS(v)[si] ? kS(v)[si] : "";
        }
        if (idx_tables >= 0) tables_val = kK(cvecs)[idx_tables];
      } else {
        /* Generic list of dicts */
        K schema_dict = kK(schemas_val)[si];
        if (!schema_dict || schema_dict->t != XD) continue;
        K skeys = kK(schema_dict)[0];
        K svals = kK(schema_dict)[1];
        for (int64_t k = 0; k < skeys->n; k++) {
          const char* sk = kS(skeys)[k];
          if (strcmp(sk, "db_schema_name") == 0) {
            K v = kK(svals)[k];
            if (v->t == -KS) schema_name = v->s ? v->s : "";
            else if (v->t == KS) schema_name = kS(v)[0] ? kS(v)[0] : "";
          } else if (strcmp(sk, "db_schema_tables") == 0) {
            tables_val = kK(svals)[k];
          }
        }
      }

      /* db_schema_name */
      ArrowArrayAppendString(array.children[1]->children[0]->children[0], ArrowCharView(schema_name));

      /* db_schema_tables: list — tables_val can be XT (table) or 0 (generic list) */
      int64_t table_count = 0;
      if (tables_val && tables_val->t == XT) {
        table_count = kK(tables_val->k)[0]->n > 0 ? kK(kK(tables_val->k)[1])[0]->n : 0;
      } else if (tables_val && tables_val->t == 0) {
        table_count = tables_val->n;
      }

      int64_t included_tables = 0;
      for (int64_t ti = 0; ti < table_count; ti++) {
        const char* tbl_name = "";
        const char* tbl_type = "";
        K cols_val = NULL;

        if (tables_val->t == XT) {
          K ckeys = kK(tables_val->k)[0];
          K cvecs = kK(tables_val->k)[1];
          int64_t ncols = ckeys->n;
          int64_t idx_name = -1, idx_type = -1, idx_cols = -1;
          for (int64_t c = 0; c < ncols; c++) {
            const char* cn = kS(ckeys)[c];
            if (strcmp(cn, "table_name") == 0) idx_name = c;
            else if (strcmp(cn, "table_type") == 0) idx_type = c;
            else if (strcmp(cn, "table_columns") == 0) idx_cols = c;
          }
          if (idx_name >= 0) {
            K v = kK(cvecs)[idx_name];
            if (v->t == -KS) tbl_name = v->s ? v->s : "";
            else if (v->t == KS) tbl_name = kS(v)[ti] ? kS(v)[ti] : "";
          }
          if (idx_type >= 0) {
            K v = kK(cvecs)[idx_type];
            if (v->t == -KS) tbl_type = v->s ? v->s : "";
            else if (v->t == KS) tbl_type = kS(v)[ti] ? kS(v)[ti] : "";
          }
          if (idx_cols >= 0) cols_val = kK(cvecs)[idx_cols];
        } else {
          K table_dict = kK(tables_val)[ti];
          if (!table_dict || (table_dict->t != XD && table_dict->t != XT)) {
            continue;
          }
          if (table_dict->t == XT) table_dict = table_dict->k;
          K tkeys = kK(table_dict)[0];
          K tvals = kK(table_dict)[1];
          for (int64_t k = 0; k < tkeys->n; k++) {
            const char* tk = kS(tkeys)[k];
            if (strcmp(tk, "table_name") == 0) {
              K v = kK(tvals)[k];
              if (v->t == -KS) tbl_name = v->s ? v->s : "";
              else if (v->t == KS) tbl_name = kS(v)[0] ? kS(v)[0] : "";
            } else if (strcmp(tk, "table_type") == 0) {
              K v = kK(tvals)[k];
              if (v->t == -KS) tbl_type = v->s ? v->s : "";
              else if (v->t == KS) tbl_type = kS(v)[0] ? kS(v)[0] : "";
            } else if (strcmp(tk, "table_columns") == 0) {
              cols_val = kK(tvals)[k];
            }
          }
        }

        if (!KdbxTypeMatch(tbl_type, types)) continue;

        /* table_name */
        ArrowArrayAppendString(
            array.children[1]->children[0]->children[1]->children[0]->children[0],
            ArrowCharView(tbl_name));
        /* table_type */
        ArrowArrayAppendString(
            array.children[1]->children[0]->children[1]->children[0]->children[1],
            ArrowCharView(tbl_type));

        /* table_columns: cols_val can be XT (table) or type 0 wrapping XT */
        int64_t col_count = 0;
        if (cols_val && cols_val->t == 0 && cols_val->n > 0 && kK(cols_val)[0]->t == XT) {
          cols_val = kK(cols_val)[0];
        }
        if (cols_val && cols_val->t == XT) {
          K cdict = cols_val->k;
          K ckeys = kK(cdict)[0];
          K cvecs = kK(cdict)[1];
          int64_t cn_cols = ckeys->n;
          col_count = cvecs->n > 0 ? kK(cvecs)[0]->n : 0;

          int64_t idx_colname = -1, idx_ordinal = -1, idx_remarks = -1, idx_typename = -1;
          for (int64_t c = 0; c < cn_cols; c++) {
            const char* cn = kS(ckeys)[c];
            if (strcmp(cn, "column_name") == 0) idx_colname = c;
            else if (strcmp(cn, "ordinal_position") == 0) idx_ordinal = c;
            else if (strcmp(cn, "remarks") == 0) idx_remarks = c;
            else if (strcmp(cn, "xdbc_type_name") == 0) idx_typename = c;
          }

          for (int64_t ri = 0; ri < col_count; ri++) {
            struct ArrowStringView cn_sv = ArrowCharView("");
            if (idx_colname >= 0) {
              K cv = kK(cvecs)[idx_colname];
              if (cv->t == -KS) cn_sv = ArrowCharView(cv->s ? cv->s : "");
              else if (cv->t == KS) cn_sv = ArrowCharView(kS(cv)[ri] ? kS(cv)[ri] : "");
            }
            ArrowArrayAppendString(
                array.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[0],
                cn_sv);

            int32_t ordinal = 0;
            if (idx_ordinal >= 0) {
              K ov = kK(cvecs)[idx_ordinal];
              if (ov->t == KI) ordinal = kI(ov)[ri];
              else if (ov->t == -KI) ordinal = ov->i;
              else if (ov->t == KJ) ordinal = (int32_t)kJ(ov)[ri];
              else if (ov->t == -KJ) ordinal = (int32_t)ov->j;
            }
            ArrowArrayAppendInt(
                array.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[1],
                ordinal);

            struct ArrowStringView rm_sv = ArrowCharView("");
            if (idx_remarks >= 0) {
              K rv = kK(cvecs)[idx_remarks];
              if (rv->t == -KS) rm_sv = ArrowCharView(rv->s ? rv->s : "");
              else if (rv->t == KS) rm_sv = ArrowCharView(kS(rv)[ri] ? kS(rv)[ri] : "");
              else if (rv->t == 0 && rv->n > ri) {
                K elem = kK(rv)[ri];
                if (elem->t == -KS) rm_sv = ArrowCharView(elem->s ? elem->s : "");
                else if (elem->t == KC) { rm_sv.data = kC(elem); rm_sv.size_bytes = elem->n; }
              }
            }
            ArrowArrayAppendString(
                array.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[2],
                rm_sv);

            struct ArrowStringView tn_sv = ArrowCharView("");
            if (idx_typename >= 0) {
              K tv = kK(cvecs)[idx_typename];
              if (tv->t == -KS) tn_sv = ArrowCharView(tv->s ? tv->s : "");
              else if (tv->t == KS) tn_sv = ArrowCharView(kS(tv)[ri] ? kS(tv)[ri] : "");
            }
            ArrowArrayAppendString(
                array.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]->children[3],
                tn_sv);

            ArrowArrayFinishElement(
                array.children[1]->children[0]->children[1]->children[0]->children[2]->children[0]);
          }
          ArrowArrayFinishElement(
              array.children[1]->children[0]->children[1]->children[0]->children[2]);
        } else {
          /* no columns - empty list element */
          ArrowArrayFinishElement(
              array.children[1]->children[0]->children[1]->children[0]->children[2]);
        }

        /* table_constraints: empty list element */
        ArrowArrayFinishElement(
            array.children[1]->children[0]->children[1]->children[0]->children[3]);

        ArrowArrayFinishElement(
            array.children[1]->children[0]->children[1]->children[0]);
        ArrowArrayFinishElement(
            array.children[1]->children[0]->children[1]);
        included_tables++;
      }

      if (included_tables == 0) {
        /* no tables - empty list element */
        ArrowArrayFinishElement(
            array.children[1]->children[0]->children[1]);
      }

      ArrowArrayFinishElement(array.children[1]->children[0]);
      ArrowArrayFinishElement(array.children[1]);
    }

    if (schema_count == 0) {
      /* no schemas - empty list element */
      ArrowArrayFinishElement(array.children[1]);
    }

    ArrowArrayFinishElement(&array);
    ArrowArrayFinishBuildingDefault(&array, NULL);

    ArrowErrorCode ae = KdbxEncodeArrowIPC(&schema, &array, out, error);
    if (ae != NANOARROW_OK) {
      r0(result);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      return ADBC_STATUS_INTERNAL;
    }

    r0(result);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&array);
    return ADBC_STATUS_OK;
  }

  r0(result);
  SetError(error, "unexpected result type from KDB+");
  return ADBC_STATUS_INTERNAL;
}

AdbcStatusCode AdbcKdbxConnectionGetTableSchema(struct AdbcConnection* conn,
                                                  const char* cat, const char* schema,
                                                  const char* table, struct ArrowSchema* out,
                                                  struct AdbcError* error) {
  (void)cat; (void)schema;
  if (!conn || !conn->private_data) { SetError(error, "NULL connection"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!table) { SetError(error, "NULL table"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!out)   { SetError(error, "NULL out");   return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxConnection* kc = (KdbxConnection*)conn->private_data;
  if (kc->handle == 0) { SetError(error, "not connected"); return ADBC_STATUS_INVALID_STATE; }

  K result = k(kc->handle, "AdbcConnectionGetTableSchema", kp((char*)table), (K)0);
  if (!result) { SetError(error, "network error"); return ADBC_STATUS_IO; }
  if (result->t == -128) {
    SetError(error, result->s ? result->s : "KDB+ error");
    r0(result);
    return ADBC_STATUS_IO;
  }
  if (result->t != KG && result->t != KC) {
    SetError(error, "unexpected result type");
    r0(result);
    return ADBC_STATUS_INTERNAL;
  }

  ArrowErrorCode rc = KdbxIpcToArrowSchema(kG(result), result->n, out, error);
  r0(result);
  return (rc == NANOARROW_OK) ? ADBC_STATUS_OK : ADBC_STATUS_INTERNAL;
}

AdbcStatusCode AdbcKdbxConnectionGetTableTypes(struct AdbcConnection* conn,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error) {
  if (!conn || !conn->private_data) { SetError(error, "NULL connection"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!out) { SetError(error, "NULL output stream"); return ADBC_STATUS_INVALID_ARGUMENT; }

  KdbxConnection* kc = (KdbxConnection*)conn->private_data;
  if (kc->handle == 0) { SetError(error, "not connected"); return ADBC_STATUS_INVALID_STATE; }

  K result = k(kc->handle, "AdbcConnectionGetTableTypes[]", (K)0);
  if (!result) { SetError(error, "network error calling AdbcConnectionGetTableTypes"); return ADBC_STATUS_IO; }
  if (result->t == -128) {
    SetError(error, result->s ? result->s : "KDB+ error in AdbcConnectionGetTableTypes");
    r0(result);
    return ADBC_STATUS_IO;
  }

  if (result->t == KG || result->t == KC) {
    uint8_t* data = kG(result);
    int64_t data_len = result->n;
    if (data_len == 0) {
      r0(result);
      memset(out, 0, sizeof(*out));
      return ADBC_STATUS_OK;
    }
    struct ArrowBuffer buffer;
    ArrowBufferInit(&buffer);
    ArrowErrorCode ae = ArrowBufferAppend(&buffer, (const void*)data, (int64_t)data_len);
    r0(result);
    if (ae != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "failed to copy IPC data"); return ADBC_STATUS_INTERNAL; }
    struct ArrowIpcInputStream input_stream;
    ae = ArrowIpcInputStreamInitBuffer(&input_stream, &buffer);
    if (ae != NANOARROW_OK) { ArrowBufferReset(&buffer); SetError(error, "ArrowIpcInputStreamInitBuffer failed"); return ADBC_STATUS_INTERNAL; }
    struct ArrowIpcArrayStreamReaderOptions options;
    memset(&options, 0, sizeof(options));
    options.field_index = -1;
    options.use_shared_buffers = 0;
    ae = ArrowIpcArrayStreamReaderInit(out, &input_stream, &options);
    if (ae != NANOARROW_OK) { input_stream.release(&input_stream); SetError(error, "ArrowIpcArrayStreamReaderInit failed"); return ADBC_STATUS_INTERNAL; }
    return ADBC_STATUS_OK;
  }

  if (result->t == 0 || result->t == 11) {
    int64_t n = result->n;

    struct ArrowSchema schema;
    ArrowSchemaInit(&schema);
    ArrowSchemaSetTypeStruct(&schema, 1);
    ArrowSchemaSetName(&schema, "");
    ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(schema.children[0], "table_type");

    struct ArrowArray array;
    ArrowArrayInitFromType(&array, NANOARROW_TYPE_STRUCT);
    ArrowArrayAllocateChildren(&array, 1);
    ArrowArrayInitFromType(array.children[0], NANOARROW_TYPE_STRING);
    ArrowArrayStartAppending(array.children[0]);

    for (int64_t i = 0; i < n; i++) {
      const char* sym = "";
      if (result->t == 11) {
        sym = kS(result)[i] ? kS(result)[i] : "";
      } else {
        K elem = kK(result)[i];
        if (elem && elem->t == -KS) sym = elem->s ? elem->s : "";
      }
      ArrowArrayAppendString(array.children[0], ArrowCharView(sym));
    }
    ArrowArrayFinishBuildingDefault(array.children[0], NULL);
    ArrowArrayFinishElement(&array);
    ArrowArrayFinishBuildingDefault(&array, NULL);

    ArrowErrorCode ae = KdbxEncodeArrowIPC(&schema, &array, out, error);
    if (ae != NANOARROW_OK) {
      r0(result);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      return ADBC_STATUS_INTERNAL;
    }

    r0(result);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&array);
    return ADBC_STATUS_OK;
  }

  r0(result);
  SetError(error, "unexpected result type from KDB+");
  return ADBC_STATUS_INTERNAL;
}

AdbcStatusCode AdbcKdbxConnectionCommit(struct AdbcConnection* conn, struct AdbcError* error) {
  (void)conn;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxConnectionRollback(struct AdbcConnection* conn, struct AdbcError* error) {
  (void)conn;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxConnectionReadPartition(struct AdbcConnection* conn,
                                                const uint8_t* data, size_t len,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error) {
  (void)conn; (void)data; (void)len; (void)out;
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxConnectionCancel(struct AdbcConnection* conn, struct AdbcError* error) {
  (void)conn;
  RETURN_NOT_IMPLEMENTED(error);
}
