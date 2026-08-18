//! adbc_host — Rust ADBC host for the dual-backend kdbx driver.
//!
//! Usage: adbc_host <libadbc_driver_kdbx.so|_backend_b.so> <entrypoint> [host] [port] [--full]

mod adbc;

use adbc::{AdbcConnection, AdbcDatabase, AdbcError, AdbcStatement, ArrowArrayStream, ArrowSchema, Host, ADBC_STATUS_OK};
use arrow_array::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow_array::RecordBatchReader;
use std::ffi::CString;
use std::ptr;

fn check(rc: adbc::AdbcStatusCode, what: &str, error: &AdbcError) -> Result<(), String> {
    if rc != ADBC_STATUS_OK {
        Err(format!("{what} failed rc={rc} err={}", adbc_err(error)))
    } else {
        Ok(())
    }
}

fn adbc_err(error: &AdbcError) -> String {
    if error.message.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(error.message) }.to_string_lossy().into_owned()
    }
}

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap()
}

/// Drain an ADBC ArrowArrayStream, returning total rows.
fn drain(stream: &mut ArrowArrayStream, label: &str) -> Result<usize, String> {
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream as *mut _ as *mut FFI_ArrowArrayStream) }
        .map_err(|e| format!("{label} from_raw: {e}"))?;
    let mut n = 0usize;
    for b in reader {
        let b = b.map_err(|e| format!("{label} batch: {e}"))?;
        n += b.num_rows();
    }
    Ok(n)
}

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: adbc_host <driver.so> <entrypoint> [host] [port] [--full]");
        return Err("not enough args".into());
    }
    let path = &args[1];
    let entrypoint = &args[2];
    let host_addr = args.get(3).map(String::as_str).unwrap_or("127.0.0.1");
    let port = args.get(4).map(String::as_str).unwrap_or("19500");
    let full = args.iter().any(|a| a == "--full");
    let query = args.iter().skip(5).find(|a| !a.starts_with("--"))
        .cloned().unwrap_or_else(|| "select from trade".to_string());

    println!("== loading driver {path} entrypoint={entrypoint} ==");
    println!("  query: {query}");
    let host = unsafe { Host::load(path, entrypoint) }?;
    println!("  driver loaded, init OK");

    // Database
    let mut db: AdbcDatabase = unsafe { std::mem::zeroed() };
    let mut error = AdbcError::default();
    unsafe { check(host.driver.database_new.unwrap()(&mut db, &mut error), "DatabaseNew", &error)?; }
    unsafe { check(host.driver.database_set_option.unwrap()(&mut db, cstr("host").as_ptr(), cstr(host_addr).as_ptr(), &mut error), "SetOption(host)", &error)?; }
    unsafe { check(host.driver.database_set_option.unwrap()(&mut db, cstr("port").as_ptr(), cstr(port).as_ptr(), &mut error), "SetOption(port)", &error)?; }
    unsafe { check(host.driver.database_init.unwrap()(&mut db, &mut error), "DatabaseInit", &error)?; }

    // Connection
    let mut conn: AdbcConnection = unsafe { std::mem::zeroed() };
    unsafe { check(host.driver.connection_new.unwrap()(&mut conn, &mut error), "ConnectionNew", &error)?; }
    unsafe { check(host.driver.connection_init.unwrap()(&mut conn, &mut db, &mut error), "ConnectionInit", &error)?; }

    if full {
        // GetTableTypes
        let mut tstream: ArrowArrayStream = unsafe { std::mem::zeroed() };
        unsafe { check(host.driver.connection_get_table_types.unwrap()(&mut conn, &mut tstream, &mut error), "GetTableTypes", &error)?; }
        println!("  get_table_types rows: {}", drain(&mut tstream, "GetTableTypes")?);

        // GetTableSchema for trade
        let mut schema: ArrowSchema = unsafe { std::mem::zeroed() };
        unsafe { check(host.driver.connection_get_table_schema.unwrap()(&mut conn, ptr::null(), ptr::null(), cstr("trade").as_ptr(), &mut schema, &mut error), "GetTableSchema", &error)?; }
        if !schema.format.is_null() {
            let fmt = unsafe { std::ffi::CStr::from_ptr(schema.format) }.to_string_lossy();
            println!("  get_table_schema(trade) format={fmt} children={}", schema.n_children);
            if schema.n_children > 0 && !schema.children.is_null() {
                for i in 0..schema.n_children {
                    let child = unsafe { *schema.children.add(i as usize) };
                    if !child.is_null() {
                        let child_ref = unsafe { &*child };
                        let name = if child_ref.name.is_null() { "?".into() } else { unsafe { std::ffi::CStr::from_ptr(child_ref.name) }.to_string_lossy() };
                        let cfmt = if child_ref.format.is_null() { "?".into() } else { unsafe { std::ffi::CStr::from_ptr(child_ref.format) }.to_string_lossy() };
                        println!("    col[{i}] {name} : {cfmt}");
                    }
                }
            }
            if schema.release.is_some() {
                unsafe { (schema.release.unwrap())(&mut schema) };
            }
        }

        // GetInfo
        let mut istream: ArrowArrayStream = unsafe { std::mem::zeroed() };
        let codes = [0u32, 1u32];
        unsafe { check(host.driver.connection_get_info.unwrap()(&mut conn, codes.as_ptr(), codes.len(), &mut istream, &mut error), "GetInfo", &error)?; }
        println!("  get_info rows: {}", drain(&mut istream, "GetInfo")?);

        // GetObjects (depth 1)
        let mut ostream: ArrowArrayStream = unsafe { std::mem::zeroed() };
        unsafe { check(host.driver.connection_get_objects.unwrap()(&mut conn, 1, ptr::null(), ptr::null(), ptr::null(), ptr::null(), ptr::null(), &mut ostream, &mut error), "GetObjects", &error)?; }
        let oreader = unsafe { ArrowArrayStreamReader::from_raw(&mut ostream as *mut _ as *mut FFI_ArrowArrayStream) }
            .map_err(|e| format!("GetObjects from_raw: {e}"))?;
        let mut onrows = 0;
        for b in oreader {
            let b = b.map_err(|e| format!("GetObjects batch: {e}"))?;
            onrows += b.num_rows();
        }
        println!("  get_objects rows: {onrows}");
    }

    // Statement + ExecuteQuery
    let mut stmt: AdbcStatement = unsafe { std::mem::zeroed() };
    unsafe { check(host.driver.statement_new.unwrap()(&mut conn, &mut stmt, &mut error), "StatementNew", &error)?; }
    unsafe { check(host.driver.statement_set_sql_query.unwrap()(&mut stmt, cstr(&query).as_ptr(), &mut error), "SetSqlQuery", &error)?; }

    let mut stream: ArrowArrayStream = unsafe { std::mem::zeroed() };
    let mut affected: i64 = 0;
    unsafe { check(host.driver.statement_execute_query.unwrap()(&mut stmt, &mut stream, &mut affected, &mut error), "ExecuteQuery", &error)?; }

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _ as *mut FFI_ArrowArrayStream) }
        .map_err(|e| format!("from_raw: {e}"))?;
    let schema = reader.schema();
    println!("  schema: {:?}", schema.fields().iter().map(|f| f.name().clone()).collect::<Vec<_>>());
    println!("  schema types: {:?}", schema.fields().iter().map(|f| f.data_type().to_string()).collect::<Vec<_>>());

    let mut rows = 0usize;
    for batch in reader {
        let batch = batch.map_err(|e| format!("read batch: {e}"))?;
        rows += batch.num_rows();
        if rows <= 5 {
            println!("  batch: rows={} cols={}", batch.num_rows(), batch.num_columns());
        }
    }
    println!("  total rows: {rows}, affected: {affected}");

    // ExecuteUpdate: ADBC expresses it as execute_query with out == NULL.
    unsafe { check(host.driver.statement_set_sql_query.unwrap()(&mut stmt, cstr("count trade").as_ptr(), &mut error), "SetSqlQuery(count)", &error)?; }
    let mut affected2: i64 = 0;
    unsafe { check(host.driver.statement_execute_query.unwrap()(&mut stmt, ptr::null_mut(), &mut affected2, &mut error), "ExecuteUpdate(count)", &error)?; }
    println!("  execute_update(count trade) -> {affected2}");

    // Release statement, connection, database (order matters for backend B).
    unsafe { check(host.driver.statement_release.unwrap()(&mut stmt, &mut error), "StatementRelease", &error)?; }
    unsafe { check(host.driver.connection_release.unwrap()(&mut conn, &mut error), "ConnectionRelease", &error)?; }
    unsafe { check(host.driver.database_release.unwrap()(&mut db, &mut error), "DatabaseRelease", &error)?; }

    println!("== OK ==");
    Ok(())
}