#include <string.h>
#include "arrow-adbc/adbc.h"
#include "adbc_kdbx_private.h"

AdbcStatusCode AdbcKdbxDatabaseNew(struct AdbcDatabase*, struct AdbcError*);
AdbcStatusCode AdbcKdbxDatabaseSetOption(struct AdbcDatabase*, const char*, const char*, struct AdbcError*);
AdbcStatusCode AdbcKdbxDatabaseInit(struct AdbcDatabase*, struct AdbcError*);
AdbcStatusCode AdbcKdbxDatabaseRelease(struct AdbcDatabase*, struct AdbcError*);
AdbcStatusCode AdbcKdbxDatabaseSetOptionInt(struct AdbcDatabase*, const char*, int64_t, struct AdbcError*);

AdbcStatusCode AdbcKdbxConnectionNew(struct AdbcConnection*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionSetOption(struct AdbcConnection*, const char*, const char*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionInit(struct AdbcConnection*, struct AdbcDatabase*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionRelease(struct AdbcConnection*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionGetInfo(struct AdbcConnection*, const uint32_t*, size_t, struct ArrowArrayStream*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionGetObjects(struct AdbcConnection*, int, const char*, const char*, const char*, const char**, const char*, struct ArrowArrayStream*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionGetTableSchema(struct AdbcConnection*, const char*, const char*, const char*, struct ArrowSchema*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionGetTableTypes(struct AdbcConnection*, struct ArrowArrayStream*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionCommit(struct AdbcConnection*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionRollback(struct AdbcConnection*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionReadPartition(struct AdbcConnection*, const uint8_t*, size_t, struct ArrowArrayStream*, struct AdbcError*);
AdbcStatusCode AdbcKdbxConnectionCancel(struct AdbcConnection*, struct AdbcError*);

AdbcStatusCode AdbcKdbxStatementNew(struct AdbcConnection*, struct AdbcStatement*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementRelease(struct AdbcStatement*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementSetSqlQuery(struct AdbcStatement*, const char*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementSetSubstraitPlan(struct AdbcStatement*, const uint8_t*, size_t, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementPrepare(struct AdbcStatement*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementExecuteQuery(struct AdbcStatement*, struct ArrowArrayStream*, int64_t*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementExecutePartitions(struct AdbcStatement*, struct ArrowSchema*, struct AdbcPartitions*, int64_t*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementBind(struct AdbcStatement*, struct ArrowArray*, struct ArrowSchema*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementBindStream(struct AdbcStatement*, struct ArrowArrayStream*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementGetParameterSchema(struct AdbcStatement*, struct ArrowSchema*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementSetOption(struct AdbcStatement*, const char*, const char*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementSetOptionInt(struct AdbcStatement*, const char*, int64_t, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementSetOptionDouble(struct AdbcStatement*, const char*, double, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementGetOption(struct AdbcStatement*, const char*, char*, size_t*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementGetOptionInt(struct AdbcStatement*, const char*, int64_t*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementGetOptionDouble(struct AdbcStatement*, const char*, double*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementCancel(struct AdbcStatement*, struct AdbcError*);
AdbcStatusCode AdbcKdbxStatementExecuteSchema(struct AdbcStatement*, struct ArrowSchema*, struct AdbcError*);

ADBC_EXPORT
AdbcStatusCode AdbcDriverKdbxInit(int version, void* raw_driver, struct AdbcError* error) {
  if (version != ADBC_VERSION_1_1_0 && version != ADBC_VERSION_1_0_0) {
    SetError(error, "unsupported version");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  struct AdbcDriver* d = (struct AdbcDriver*)raw_driver;
  if (!d) { SetError(error, "NULL driver"); return ADBC_STATUS_INVALID_ARGUMENT; }
  const size_t driver_size = (version == ADBC_VERSION_1_1_0)
                                 ? ADBC_DRIVER_1_1_0_SIZE
                                 : ADBC_DRIVER_1_0_0_SIZE;
  memset(d, 0, driver_size);

  d->DatabaseNew            = AdbcKdbxDatabaseNew;
  d->DatabaseSetOption      = AdbcKdbxDatabaseSetOption;
  d->DatabaseSetOptionInt   = AdbcKdbxDatabaseSetOptionInt;
  d->DatabaseInit           = AdbcKdbxDatabaseInit;
  d->DatabaseRelease        = AdbcKdbxDatabaseRelease;

  d->ConnectionNew          = AdbcKdbxConnectionNew;
  d->ConnectionSetOption    = AdbcKdbxConnectionSetOption;
  d->ConnectionInit         = AdbcKdbxConnectionInit;
  d->ConnectionRelease      = AdbcKdbxConnectionRelease;
  d->ConnectionGetInfo      = AdbcKdbxConnectionGetInfo;
  d->ConnectionGetObjects   = AdbcKdbxConnectionGetObjects;
  d->ConnectionGetTableSchema = AdbcKdbxConnectionGetTableSchema;
  d->ConnectionGetTableTypes  = AdbcKdbxConnectionGetTableTypes;
  d->ConnectionCommit       = AdbcKdbxConnectionCommit;
  d->ConnectionRollback     = AdbcKdbxConnectionRollback;
  d->ConnectionReadPartition = AdbcKdbxConnectionReadPartition;
  d->ConnectionCancel       = AdbcKdbxConnectionCancel;

  d->StatementNew           = AdbcKdbxStatementNew;
  d->StatementRelease       = AdbcKdbxStatementRelease;
  d->StatementSetSqlQuery   = AdbcKdbxStatementSetSqlQuery;
  d->StatementSetSubstraitPlan = AdbcKdbxStatementSetSubstraitPlan;
  d->StatementPrepare       = AdbcKdbxStatementPrepare;
  d->StatementExecuteQuery  = AdbcKdbxStatementExecuteQuery;
  d->StatementExecutePartitions = AdbcKdbxStatementExecutePartitions;
  d->StatementBind          = AdbcKdbxStatementBind;
  d->StatementBindStream    = AdbcKdbxStatementBindStream;
  d->StatementGetParameterSchema = AdbcKdbxStatementGetParameterSchema;
  d->StatementSetOption     = AdbcKdbxStatementSetOption;
  d->StatementSetOptionInt  = AdbcKdbxStatementSetOptionInt;
  d->StatementSetOptionDouble = AdbcKdbxStatementSetOptionDouble;
  d->StatementGetOption     = AdbcKdbxStatementGetOption;
  d->StatementGetOptionInt  = AdbcKdbxStatementGetOptionInt;
  d->StatementGetOptionDouble = AdbcKdbxStatementGetOptionDouble;
  d->StatementCancel        = AdbcKdbxStatementCancel;
  d->StatementExecuteSchema = AdbcKdbxStatementExecuteSchema;

  return ADBC_STATUS_OK;
}
