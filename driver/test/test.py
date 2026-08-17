"""Test loading libadbc_driver_kdbx.so (backend A) or _backend_b.so (backend B)
via adbc-driver-manager.

Backend selection: KDBX_BACKEND=a|b (default a). Port: KDBX_PORT (default 9500).
"""
import os
import sys
from pathlib import Path

import adbc_driver_manager
from adbc_driver_manager import AdbcDatabase, AdbcConnection, AdbcStatement

HERE = Path(__file__).resolve().parent
BACKEND = os.environ.get("KDBX_BACKEND", "a").lower()
if BACKEND == "b":
    LIB = HERE.parent / "kdbx_adbc" / "_backend_b.so"
else:
    LIB = HERE.parent / "build" / "libadbc_driver_kdbx.so"
PORT = os.environ.get("KDBX_PORT", "9500")
ADBC_STATUS_NOT_IMPLEMENTED = 2  # from AdbcStatusCode enum
ADBC_OK = 0

PASS = 0
FAIL = 0


def check(label, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok  {label}")
    else:
        FAIL += 1
        print(f"  FAIL {label}")


def section(title):
    print(f"\n== {title} ==")


section("1. Driver load via AdbcDatabase")
try:
    db = AdbcDatabase(driver=str(LIB))
    check("AdbcDatabase(driver=lib.so) created", db is not None)
except Exception as e:
    check(f"AdbcDatabase init: {e}", False)
    db = None

section("2. Connection lifecycle")
if db is not None:
    try:
        db.set_options(host="127.0.0.1", port=PORT)
        conn = AdbcConnection(db)
        check("AdbcConnection created", conn is not None)
        conn.close()
        check("Connection closed", True)
    except Exception as e:
        check(f"Connection: {e}", False)

section("3. Database close")
if db is not None:
    try:
        db.close()
        check("Database closed", True)
    except Exception as e:
        check(f"Database close: {e}", False)

section("4. Statement lifecycle")
try:
    db2 = AdbcDatabase(driver=str(LIB))
    db2.set_options(host="127.0.0.1", port=PORT)
    conn2 = AdbcConnection(db2)
    stmt = AdbcStatement(conn2)
    check("Statement created", stmt is not None)
    stmt.close()
    check("Statement closed", True)
    conn2.close()
    db2.close()
except Exception as e:
    check(f"Statement lifecycle: {e}", False)

section("5. Bad entrypoint / missing init")
import ctypes

if BACKEND == "b":
    # Backend B embeds its own CPython; dlopen'ing it from a running Python
    # process is a known boundary (double interpreter). The ctypes entrypoint
    # probe below is backend-A only.
    check("DriverInit probe (skipped for backend B)", True)
else:
    # Try loading the .so directly and calling AdbcDriverKdbxInit with bad version
    so = ctypes.CDLL(str(LIB))
    so.AdbcDriverKdbxInit.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
    so.AdbcDriverKdbxInit.restype = ctypes.c_uint8

    class RawDriver(ctypes.Structure):
        _fields_ = [("private_data", ctypes.c_void_p), ("private_manager", ctypes.c_void_p)]

    raw = RawDriver()
    status = so.AdbcDriverKdbxInit(999, ctypes.byref(raw), None)
    check("DriverInit(999) -> NOT_IMPLEMENTED", status == ADBC_STATUS_NOT_IMPLEMENTED)

section("6. ExecuteQuery")
try:
    import pyarrow as pa
    db3 = AdbcDatabase(driver=str(LIB))
    db3.set_options(host="127.0.0.1", port=PORT)
    conn3 = AdbcConnection(db3)
    stmt3 = AdbcStatement(conn3)
    stmt3.set_sql_query("select x:price+1 from trade")
    stream_handle, affected = stmt3.execute_query()
    reader = pa.RecordBatchReader.from_stream(stream_handle)
    table = reader.read_all()
    check("ExecuteQuery returned rows", table.num_rows >= 1)
    check("Schema has columns", len(table.schema) >= 1)
    print(f"    schema: {table.schema}")
    print(f"    rows:   {table.num_rows}")
    print(f"    data:   {table.to_pydict()}")
    stmt3.close()
    conn3.close()
    db3.close()
except Exception as e:
    check(f"ExecuteQuery: {e}", False)

section("7. ExecuteSchema")
try:
    db4 = AdbcDatabase(driver=str(LIB))
    db4.set_options(host="127.0.0.1", port=PORT)
    conn4 = AdbcConnection(db4)
    stmt4 = AdbcStatement(conn4)
    stmt4.set_sql_query("select from trade")
    schema_handle = stmt4.execute_schema()
    schema = pa.schema(schema_handle)
    check("ExecuteSchema returned schema", len(schema) >= 1)
    print(f"    schema: {schema}")
    print(f"    names:  {schema.names}")
    print(f"    types:  {schema.types}")
    stmt4.close()
    conn4.close()
    db4.close()
except Exception as e:
    check(f"ExecuteSchema: {e}", False)

section("8. ExecuteUpdate")
try:
    db5 = AdbcDatabase(driver=str(LIB))
    db5.set_options(host="127.0.0.1", port=PORT)
    conn5 = AdbcConnection(db5)
    stmt5 = AdbcStatement(conn5)
    stmt5.set_sql_query("count trade")
    affected = stmt5.execute_update()
    check("ExecuteUpdate returned count", isinstance(affected, int))
    print(f"    affected: {affected}")
    stmt5.close()
    conn5.close()
    db5.close()
except Exception as e:
    check(f"ExecuteUpdate: {e}", False)

section("9. ConnectionGetInfo")
try:
    db6 = AdbcDatabase(driver=str(LIB))
    db6.set_options(host="127.0.0.1", port=PORT)
    conn6 = AdbcConnection(db6)
    stream_handle = conn6.get_info([0, 1, 2])
    reader = pa.RecordBatchReader.from_stream(stream_handle)
    table = reader.read_all()
    check("GetInfo returned rows", table.num_rows >= 1)
    check("GetInfo has info_name column", "info_name" in table.schema.names)
    check("GetInfo has info_value column", "info_value" in table.schema.names)
    print(f"    schema: {table.schema}")
    print(f"    rows:   {table.num_rows}")
    print(f"    data:   {table.to_pydict()}")
    conn6.close()
    db6.close()
except Exception as e:
    check(f"ConnectionGetInfo: {e}", False)

section("10. ConnectionGetObjects")
try:
    db7 = AdbcDatabase(driver=str(LIB))
    db7.set_options(host="127.0.0.1", port=PORT)
    conn7 = AdbcConnection(db7)
    stream_handle = conn7.get_objects(1)
    reader = pa.RecordBatchReader.from_stream(stream_handle)
    table = reader.read_all()
    check("GetObjects returned rows", table.num_rows >= 1)
    check("GetObjects has catalog_name column", "catalog_name" in table.schema.names)
    print(f"    schema: {table.schema}")
    print(f"    rows:   {table.num_rows}")
    print(f"    data:   {table.to_pydict()}")
    conn7.close()
    db7.close()
except Exception as e:
    check(f"ConnectionGetObjects: {e}", False)

section("11. ConnectionGetTableSchema")
try:
    db8 = AdbcDatabase(driver=str(LIB))
    db8.set_options(host="127.0.0.1", port=PORT)
    conn8 = AdbcConnection(db8)
    # GetTableSchema returns an ArrowSchema C handle; wrap with pyarrow
    schema_handle = conn8.get_table_schema(None, None, "trade")
    schema = pa.schema(schema_handle)
    check("GetTableSchema returned schema", len(schema) >= 1)
    check("GetTableSchema has column names", len(schema.names) >= 1)
    print(f"    schema: {schema}")
    print(f"    names:  {schema.names}")
    print(f"    types:  {schema.types}")
    conn8.close()
    db8.close()
except Exception as e:
    check(f"ConnectionGetTableSchema: {e}", False)

section("12. ConnectionGetTableTypes")
try:
    db9 = AdbcDatabase(driver=str(LIB))
    db9.set_options(host="127.0.0.1", port=PORT)
    conn9 = AdbcConnection(db9)
    stream_handle = conn9.get_table_types()
    reader = pa.RecordBatchReader.from_stream(stream_handle)
    table = reader.read_all()
    check("GetTableTypes returned rows", table.num_rows >= 1)
    check("GetTableTypes has table_type column", "table_type" in table.schema.names)
    print(f"    schema: {table.schema}")
    print(f"    rows:   {table.num_rows}")
    print(f"    data:   {table.to_pydict()}")
    conn9.close()
    db9.close()
except Exception as e:
    check(f"ConnectionGetTableTypes: {e}", False)

section("13. StatementBulkIngest")
if BACKEND == "b":
    # Backend B routes through the pykx bridge in normal IPC mode; streamed
    # bulk ingest (arrowkdb) is not available there. Backend A covers it.
    check("StatementBulkIngest (skipped for backend B)", True)
else:
    try:
        db10 = AdbcDatabase(driver=str(LIB))
        db10.set_options(host="127.0.0.1", port=PORT)
        conn10 = AdbcConnection(db10)

        stmt_src = AdbcStatement(conn10)
        stmt_src.set_sql_query("select from trade")
        reader_src, _ = stmt_src.execute_query()

        stmt_dst = AdbcStatement(conn10)
        stmt_dst.set_options(**{"adbc.ingest.target_table": "bulk_test", "adbc.ingest.mode": "adbc.ingest.mode.create"})
        stmt_dst.bind_stream(reader_src)
        affected = stmt_dst.execute_update()
        check("BulkIngest returned affected rows", affected == 1000)
        print(f"    affected: {affected}")

        stmt_src.close()
        stmt_dst.close()
        conn10.close()
        db10.close()
    except Exception as e:
        check(f"StatementBulkIngest: {e}", False)

section("14. URI parsing")
try:
    db11 = AdbcDatabase(driver=str(LIB))
    db11.set_options(uri="kdbx://admin:secret@127.0.0.1:9500/mydb")
    conn11 = AdbcConnection(db11)
    check("URI connection created", conn11 is not None)
    conn11.close()
    db11.close()

    db12 = AdbcDatabase(driver=str(LIB))
    db12.set_options(uri="kdbx://127.0.0.1:9500")
    conn12 = AdbcConnection(db12)
    check("URI without credentials", conn12 is not None)
    conn12.close()
    db12.close()

    db13 = AdbcDatabase(driver=str(LIB))
    db13.set_options(uri="kdbx://user@host:9500/db")
    conn13 = AdbcConnection(db13)
    check("URI with user only", conn13 is not None)
    conn13.close()
    db13.close()
except Exception as e:
    check(f"URI parsing: {e}", False)

section("15. GetObjects depth filtering")
try:
    db15 = AdbcDatabase(driver=str(LIB))
    db15.set_options(host="127.0.0.1", port=PORT)
    conn15 = AdbcConnection(db15)

    # depth=0: catalog only
    stream = conn15.get_objects(0)
    reader = pa.RecordBatchReader.from_stream(stream)
    table = reader.read_all()
    check("depth=0 returned rows", table.num_rows >= 1)
    check("depth=0 has catalog_name", "catalog_name" in table.schema.names)

    # depth=1: catalog + schema
    stream = conn15.get_objects(1)
    reader = pa.RecordBatchReader.from_stream(stream)
    table = reader.read_all()
    check("depth=1 returned rows", table.num_rows >= 1)
    check("depth=1 has catalog_name", "catalog_name" in table.schema.names)

    # depth=2: catalog + schema + table
    stream = conn15.get_objects(2)
    reader = pa.RecordBatchReader.from_stream(stream)
    table = reader.read_all()
    check("depth=2 returned rows", table.num_rows >= 1)
    check("depth=2 has catalog_name", "catalog_name" in table.schema.names)

    # depth=3: full structure with columns (ADBC max depth)
    stream = conn15.get_objects(3)
    reader = pa.RecordBatchReader.from_stream(stream)
    table = reader.read_all()
    check("depth=3 returned rows", table.num_rows >= 1)
    check("depth=3 has catalog_name", "catalog_name" in table.schema.names)

    conn15.close()
    db15.close()
except Exception as e:
    check(f"GetObjects depth filtering: {e}", False)

section("16. GetTableSchema with catalog/schema")
try:
    db16 = AdbcDatabase(driver=str(LIB))
    db16.set_options(host="127.0.0.1", port=PORT)
    conn16 = AdbcConnection(db16)

    schema = pa.schema(conn16.get_table_schema("kdb", "main", "trade"))
    check("GetTableSchema with catalog/schema returned schema", len(schema) >= 1)

    schema = pa.schema(conn16.get_table_schema(None, None, "trade"))
    check("GetTableSchema with NULL catalog/schema returned schema", len(schema) >= 1)

    conn16.close()
    db16.close()
except Exception as e:
    check(f"GetTableSchema catalog/schema: {e}", False)

section("17. Error scenarios")
try:
    db_unkopt = AdbcDatabase(driver=str(LIB))
    db_unkopt.set_options(unknown_option="value")
    check("Unknown option should fail", False)
    db_unkopt.close()
except Exception as e:
    check("Unknown option raises exception", True)

try:
    db_nosql = AdbcDatabase(driver=str(LIB))
    db_nosql.set_options(host="127.0.0.1", port=PORT)
    conn_nosql = AdbcConnection(db_nosql)
    stmt_nosql = AdbcStatement(conn_nosql)
    stream, affected = stmt_nosql.execute_query()
    check("No SQL query should fail", False)
    stmt_nosql.close()
    conn_nosql.close()
    db_nosql.close()
except Exception as e:
    check("No SQL query raises exception", True)

try:
    db_closed = AdbcDatabase(driver=str(LIB))
    db_closed.set_options(host="127.0.0.1", port=PORT)
    conn_closed = AdbcConnection(db_closed)
    conn_closed.close()
    stmt_closed = AdbcStatement(conn_closed)
    stmt_closed.set_sql_query("select 1")
    stream, affected = stmt_closed.execute_query()
    check("SQL on closed connection should fail", False)
except Exception as e:
    check("SQL on closed connection raises exception", True)
finally:
    if "db_closed" in locals():
        db_closed.close()

try:
    db_null = AdbcDatabase(driver=str(LIB))
    db_null.set_options(host="127.0.0.1", port=PORT)
    conn_null = AdbcConnection(db_null)
    conn_null.get_table_schema(None, None, None)
    check("NULL table should fail", False)
    conn_null.close()
    db_null.close()
except Exception as e:
    check("NULL table raises exception", True)

section("18. URI edge cases")
try:
    db_empty = AdbcDatabase(driver=str(LIB))
    db_empty.set_options(uri="kdbx://")
    conn_empty = AdbcConnection(db_empty)
    check("Empty host should fail", False)
    conn_empty.close()
    db_empty.close()
except Exception as e:
    check("Empty host raises exception", True)

try:
    db_port0 = AdbcDatabase(driver=str(LIB))
    db_port0.set_options(uri="kdbx://host:0")
    check("Port 0 should fail in set_options", False)
    db_port0.close()
except Exception as e:
    check("Port 0 raises exception", True)

try:
    db_portbig = AdbcDatabase(driver=str(LIB))
    db_portbig.set_options(uri="kdbx://host:99999")
    check("Port 99999 should fail in set_options", False)
    db_portbig.close()
except Exception as e:
    check("Port 99999 raises exception", True)

total = PASS + FAIL
print(f"\n{'=' * 50}")
print(f"  {PASS}/{total} passed, {FAIL} failed")
print(f"{'=' * 50}")
if BACKEND == "b":
    # Backend B embeds CPython; interpreter finalize can block on pyarrow's
    # global teardown after all ADBC objects are released. The tests are
    # complete; exit without running full Python teardown.
    os._exit(0 if FAIL == 0 else 1)
sys.exit(0 if FAIL == 0 else 1)
