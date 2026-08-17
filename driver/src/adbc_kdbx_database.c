#include "adbc_kdbx_private.h"
#include <stdio.h>

static AdbcStatusCode ParseUri(const char* uri, KdbxDatabase* kdb, struct AdbcError* error) {
  const char* p = uri;

  if (strncmp(p, "kdbx://", 7) != 0) {
    SetError(error, "URI must start with kdbx://");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  p += 7;

  const char* at = strchr(p, '@');
  if (at) {
    const char* colon = memchr(p, ':', at - p);
    if (colon) {
      free(kdb->user);
      int ulen = (int)(colon - p);
      int plen = (int)(at - colon - 1);
      kdb->user = (char*)malloc(ulen + 1 + plen + 1);
      if (!kdb->user) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }
      memcpy(kdb->user, p, ulen);
      kdb->user[ulen] = ':';
      memcpy(kdb->user + ulen + 1, colon + 1, plen);
      kdb->user[ulen + 1 + plen] = '\0';
    } else {
      free(kdb->user);
      kdb->user = (char*)malloc((at - p) + 1);
      if (!kdb->user) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }
      memcpy(kdb->user, p, at - p);
      kdb->user[at - p] = '\0';
    }
    p = at + 1;
  }

  const char* slash = strchr(p, '/');
  const char* host_end = slash ? slash : p + strlen(p);

  const char* colon_port = memchr(p, ':', host_end - p);
  if (colon_port) {
    free(kdb->host);
    kdb->host = (char*)malloc(colon_port - p + 1);
    if (!kdb->host) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }
    memcpy(kdb->host, p, colon_port - p);
    kdb->host[colon_port - p] = '\0';

    char* end = NULL;
    long port = strtol(colon_port + 1, &end, 10);
    if (end == colon_port + 1 || port <= 0 || port > 65535) {
      SetError(error, "invalid port in URI");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    kdb->port = (int)port;
  } else {
    free(kdb->host);
    kdb->host = (char*)malloc(host_end - p + 1);
    if (!kdb->host) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }
    memcpy(kdb->host, p, host_end - p);
    kdb->host[host_end - p] = '\0';
  }

  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxDatabaseNew(struct AdbcDatabase* db, struct AdbcError* error) {
  if (!db) { SetError(error, "NULL database"); return ADBC_STATUS_INVALID_ARGUMENT; }
  db->private_data = calloc(1, sizeof(KdbxDatabase));
  if (!db->private_data) { SetError(error, "alloc failed"); return ADBC_STATUS_INTERNAL; }
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxDatabaseSetOption(struct AdbcDatabase* db, const char* k,
                                          const char* v, struct AdbcError* error) {
  if (!db || !db->private_data) { SetError(error, "NULL database"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!k || !v) { SetError(error, "NULL key or value"); return ADBC_STATUS_INVALID_ARGUMENT; }
  KdbxDatabase* kdb = (KdbxDatabase*)db->private_data;

  if (strcmp(k, "host") == 0) {
    free(kdb->host);
    kdb->host = strdup(v);
    return ADBC_STATUS_OK;
  }
  if (strcmp(k, "port") == 0) {
    char* end = NULL;
    long p = strtol(v, &end, 10);
    if (*end != '\0' || p <= 0 || p > 65535) {
      SetError(error, "invalid port");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    kdb->port = (int)p;
    return ADBC_STATUS_OK;
  }
  if (strcmp(k, "user") == 0) {
    free(kdb->user);
    kdb->user = strdup(v);
    return ADBC_STATUS_OK;
  }
  if (strcmp(k, "uri") == 0) {
    return ParseUri(v, kdb, error);
  }
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxDatabaseInit(struct AdbcDatabase* db, struct AdbcError* error) {
  if (!db || !db->private_data) { SetError(error, "NULL database"); return ADBC_STATUS_INVALID_ARGUMENT; }
  return ADBC_STATUS_OK;
}

AdbcStatusCode AdbcKdbxDatabaseSetOptionInt(struct AdbcDatabase* db, const char* k,
                                             int64_t v, struct AdbcError* error) {
  if (!db || !db->private_data) { SetError(error, "NULL database"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (!k) { SetError(error, "NULL key"); return ADBC_STATUS_INVALID_ARGUMENT; }
  KdbxDatabase* kdb = (KdbxDatabase*)db->private_data;
  if (strcmp(k, "port") == 0) {
    if (v <= 0 || v > 65535) { SetError(error, "invalid port"); return ADBC_STATUS_INVALID_ARGUMENT; }
    kdb->port = (int)v;
    return ADBC_STATUS_OK;
  }
  RETURN_NOT_IMPLEMENTED(error);
}

AdbcStatusCode AdbcKdbxDatabaseRelease(struct AdbcDatabase* db, struct AdbcError* error) {
  (void)error;
  if (!db) return ADBC_STATUS_OK;
  KdbxDatabase* kdb = (KdbxDatabase*)db->private_data;
  if (kdb) {
    free(kdb->host);
    free(kdb->user);
    free(kdb);
  }
  db->private_data = NULL;
  return ADBC_STATUS_OK;
}
