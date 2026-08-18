//! adbc_host — minimal Rust host for the dual-backend kdbx ADBC driver.
//!
//! This mirrors what the dbt-core v2 Rust adapter does: dlopen an ADBC driver
//! shared library with RTLD_GLOBAL, call its init entry point, and drive the
//! ADBC C API (Database -> Connection -> Statement). Results come back as an
//! Arrow C data-interface stream and are consumed via arrow-rs.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

// ---------------------------------------------------------------------------
// ADBC C ABI (subset used by this host). Layouts must match adbc.h exactly.
// ---------------------------------------------------------------------------

pub type AdbcStatusCode = u8;

pub const ADBC_STATUS_OK: AdbcStatusCode = 0;
pub const ADBC_VERSION_1_1_0: c_int = 1_001_000;

#[repr(C)]
pub struct AdbcError {
    pub message: *mut c_char,
    pub vendor_code: i32,
    pub sqlstate: [c_char; 5],
    pub release: Option<unsafe extern "C" fn(*mut AdbcError)>,
    pub private_data: *mut c_void,
    pub private_driver: *mut c_void,
}

impl Default for AdbcError {
    fn default() -> Self {
        Self {
            message: ptr::null_mut(),
            vendor_code: 0,
            sqlstate: [0; 5],
            release: None,
            private_data: ptr::null_mut(),
            private_driver: ptr::null_mut(),
        }
    }
}

#[repr(C)]
pub struct AdbcDatabase {
    pub private_data: *mut c_void,
    pub private_driver: *mut c_void,
}

#[repr(C)]
pub struct AdbcConnection {
    pub private_data: *mut c_void,
    pub private_driver: *mut c_void,
}

#[repr(C)]
pub struct AdbcStatement {
    pub private_data: *mut c_void,
    pub private_driver: *mut c_void,
}

// Arrow C data interface (we only consume streams).
#[repr(C)]
pub struct ArrowArrayStream {
    pub get_schema: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut ArrowSchema) -> c_int>,
    pub get_next: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut ArrowArray) -> c_int>,
    pub get_last_error: Option<unsafe extern "C" fn(*mut ArrowArrayStream) -> *const c_char>,
    pub release: Option<unsafe extern "C" fn(*mut ArrowArrayStream)>,
    pub private_data: *mut c_void,
}

#[repr(C)]
pub struct ArrowSchema {
    pub format: *const c_char,
    pub name: *const c_char,
    pub metadata: *const c_char,
    pub flags: i64,
    pub n_children: i64,
    pub children: *mut *mut ArrowSchema,
    pub dictionary: *mut ArrowSchema,
    pub release: Option<unsafe extern "C" fn(*mut ArrowSchema)>,
    pub private_data: *mut c_void,
}

#[repr(C)]
pub struct ArrowArray {
    pub length: i64,
    pub null_count: i64,
    pub offset: i64,
    pub n_buffers: i64,
    pub n_children: i64,
    pub buffers: *mut *const c_void,
    pub children: *mut *mut ArrowArray,
    pub dictionary: *mut ArrowArray,
    pub release: Option<unsafe extern "C" fn(*mut ArrowArray)>,
    pub private_data: *mut c_void,
}

type InitFn = unsafe extern "C" fn(c_int, *mut c_void, *mut AdbcError) -> AdbcStatusCode;

// AdbcDriver struct: function pointers in the exact C field order.
#[allow(clippy::type_complexity)]
#[repr(C)]
pub struct AdbcDriver {
    pub private_data: *mut c_void,
    pub private_manager: *mut c_void,
    pub release: Option<unsafe extern "C" fn(*mut AdbcDriver, *mut AdbcError) -> AdbcStatusCode>,
    pub database_init: Option<unsafe extern "C" fn(*mut AdbcDatabase, *mut AdbcError) -> AdbcStatusCode>,
    pub database_new: Option<unsafe extern "C" fn(*mut AdbcDatabase, *mut AdbcError) -> AdbcStatusCode>,
    pub database_set_option: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, *const c_char, *mut AdbcError) -> AdbcStatusCode>,
    pub database_release: Option<unsafe extern "C" fn(*mut AdbcDatabase, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_commit: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_info: Option<unsafe extern "C" fn(*mut AdbcConnection, *const u32, usize, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_objects: Option<unsafe extern "C" fn(*mut AdbcConnection, c_int, *const c_char, *const c_char, *const c_char, *const *const c_char, *const c_char, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_table_schema: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *const c_char, *const c_char, *mut ArrowSchema, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_table_types: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_init: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcDatabase, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_new: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_set_option: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *const c_char, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_read_partition: Option<unsafe extern "C" fn(*mut AdbcConnection, *const u8, usize, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_release: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_rollback: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_bind: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut ArrowArray, *mut ArrowSchema, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_bind_stream: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_execute_query: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut ArrowArrayStream, *mut i64, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_execute_partitions: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut ArrowSchema, *mut c_void, *mut i64, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_get_parameter_schema: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut ArrowSchema, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_new: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcStatement, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_prepare: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_release: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_set_option: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *const c_char, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_set_sql_query: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_set_substrait_plan: Option<unsafe extern "C" fn(*mut AdbcStatement, *const u8, usize, *mut AdbcError) -> AdbcStatusCode>,
    pub error_get_detail_count: Option<unsafe extern "C" fn(*const AdbcError) -> c_int>,
    pub error_get_detail: Option<unsafe extern "C" fn(*const AdbcError, c_int) -> c_void>,
    pub error_from_array_stream: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut AdbcStatusCode) -> *const AdbcError>,
    pub database_get_option: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, *mut c_char, *mut usize, *mut AdbcError) -> AdbcStatusCode>,
    pub database_get_option_bytes: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, *mut u8, *mut usize, *mut AdbcError) -> AdbcStatusCode>,
    pub database_get_option_double: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, *mut f64, *mut AdbcError) -> AdbcStatusCode>,
    pub database_get_option_int: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, *mut i64, *mut AdbcError) -> AdbcStatusCode>,
    pub database_set_option_bytes: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, *const u8, usize, *mut AdbcError) -> AdbcStatusCode>,
    pub database_set_option_double: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, f64, *mut AdbcError) -> AdbcStatusCode>,
    pub database_set_option_int: Option<unsafe extern "C" fn(*mut AdbcDatabase, *const c_char, i64, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_cancel: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_option: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *mut c_char, *mut usize, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_option_bytes: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *mut u8, *mut usize, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_option_double: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *mut f64, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_option_int: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *mut i64, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_statistics: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *const c_char, *const c_char, c_char, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_get_statistic_names: Option<unsafe extern "C" fn(*mut AdbcConnection, *mut ArrowArrayStream, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_set_option_bytes: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, *const u8, usize, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_set_option_double: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, f64, *mut AdbcError) -> AdbcStatusCode>,
    pub connection_set_option_int: Option<unsafe extern "C" fn(*mut AdbcConnection, *const c_char, i64, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_cancel: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_execute_schema: Option<unsafe extern "C" fn(*mut AdbcStatement, *mut ArrowSchema, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_get_option: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *mut c_char, *mut usize, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_get_option_bytes: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *mut u8, *mut usize, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_get_option_double: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *mut f64, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_get_option_int: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *mut i64, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_set_option_bytes: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, *const u8, usize, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_set_option_double: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, f64, *mut AdbcError) -> AdbcStatusCode>,
    pub statement_set_option_int: Option<unsafe extern "C" fn(*mut AdbcStatement, *const c_char, i64, *mut AdbcError) -> AdbcStatusCode>,
}

impl Default for AdbcDriver {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

// ---------------------------------------------------------------------------
// Helper: error handling
// ---------------------------------------------------------------------------

fn err_str(error: &AdbcError) -> String {
    if error.message.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(error.message) }.to_string_lossy().into_owned()
    }
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

pub struct Host {
    _lib: libloading::os::unix::Library,
    pub driver: AdbcDriver,
}

impl Host {
    /// dlopen the driver .so with RTLD_GLOBAL and call its init entry point.
    ///
    /// # Safety
    /// `path` must point to a valid ADBC driver shared library whose
    /// `entrypoint` symbol has the standard init signature.
    pub unsafe fn load(path: &str, entrypoint: &str) -> Result<Self, String> {
        // RTLD_GLOBAL is required: backend B embeds CPython whose extension
        // modules resolve internal CPython symbols from the global namespace.
        let lib = unsafe {
            libloading::os::unix::Library::open(Some(path), libloading::os::unix::RTLD_NOW | libloading::os::unix::RTLD_GLOBAL)
                .map_err(|e| format!("dlopen {path}: {e}"))?
        };
        let init: libloading::os::unix::Symbol<InitFn> = unsafe {
            lib.get(entrypoint.as_bytes())
                .map_err(|e| format!("dlsym {entrypoint}: {e}"))?
        };
        let mut driver: AdbcDriver = AdbcDriver::default();
        let mut error = AdbcError::default();
        let rc = unsafe { init(ADBC_VERSION_1_1_0, &mut driver as *mut _ as *mut c_void, &mut error) };
        if rc != ADBC_STATUS_OK {
            return Err(format!(
                "init failed rc={} err={}",
                rc,
                err_str(&error)
            ));
        }
        Ok(Self {
            _lib: lib,
            driver,
        })
    }
}

unsafe impl Send for Host {}
unsafe impl Sync for Host {}