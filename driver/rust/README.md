# adbc_host — Rust ADBC host / dbt-core v2 integration reference

A minimal Rust host that dlopens the dual-backend kdbx ADBC driver and drives
the ADBC C API, exactly like the dbt-core v2 (Fusion) Rust adapter does. It
proves the driver works from a Rust process (no Python involved), which is the
integration point for the v2 adapter.

## Build

```bash
cargo build --release
```

Requires a Rust toolchain (rustup). This is the only Rust code in the repo; it
does not need Python or pykx to compile.

## Run

```bash
# Backend A (native C, q server + arrowkdb)
./target/release/adbc_host ../build/libadbc_driver_kdbx.so AdbcDriverKdbxInit 127.0.0.1 19500 --full

# Backend B (embedded CPython + pykx bridge)
./target/release/adbc_host ../kdbx_adbc/_backend_b.so AdbcBackendBInit 127.0.0.1 19500 --full

# Run a specific dbt-compiled q expression (positional: host port query)
./target/release/adbc_host ../build/libadbc_driver_kdbx.so AdbcDriverKdbxInit \
    127.0.0.1 19500 "select sym, price from trade where size > 2000"
```

With `--full` the host also exercises GetTableTypes / GetTableSchema / GetInfo
/ GetObjects before the query. Without it, it runs only ExecuteQuery +
ExecuteUpdate (the fifth positional argument is the query, defaulting to
`select from trade`).

Output verifies the full ADBC surface from Rust:

```
  get_table_types rows: 1
  get_table_schema(trade) format=+s children=4
    col[0] time : tsn:
    col[1] sym : u
    col[2] price : g
    col[3] size : l
  get_info rows: 2
  get_objects rows: 1
  schema: ["time", "sym", "price", "size"]
  total rows: 1000, affected: -1
  execute_update(count trade) -> 1000
```

## How it works

- `src/adbc.rs` — FFI bindings for the ADBC C ABI (`AdbcError`, `AdbcDatabase`,
  `AdbcConnection`, `AdbcStatement`, `AdbcDriver`, `ArrowArrayStream`,
  `ArrowSchema`, `ArrowArray`). Layouts match `driver/include/arrow-adbc/adbc.h`
  exactly. `Host::load` dlopens with `RTLD_NOW | RTLD_GLOBAL` and calls the
  init entry point with `ADBC_VERSION_1_1_0`.
- `src/main.rs` — drives Database -> Connection -> Statement, executes
  `select from trade`, and consumes the result through arrow-rs's
  `ArrowArrayStreamReader` (Arrow C data interface). `--full` also exercises
  GetTableTypes / GetTableSchema / GetInfo / GetObjects.

### Critical for backend B

`RTLD_GLOBAL` is mandatory. Backend B embeds CPython; Python extension modules
(`_posixsubprocess`, pyarrow) resolve internal CPython symbols from the global
namespace. Without `RTLD_GLOBAL` the embedded interpreter fails with
`undefined symbol: _Py_write_noraise`. This mirrors the dbt-core v2 `dbt-xdbc`
loading path — the driver must be loaded globally.

`DatabaseInit` is where backend B opens the pykx bridge connection (backend A
uses ConnectionInit). Hosts must call DatabaseNew -> SetOption(host/port) ->
DatabaseInit before creating connections. Release order matters: Statement ->
Connection -> Database (DatabaseRelease closes the shared pykx bridge when the
last database goes away).

## dbt-core v2 (Fusion) adapter integration

dbt-core v2 adapters live in the `dbt-core` monorepo and register the ADBC
driver in `crates/dbt-xdbc/src/driver.rs` (the `Backend` enum), with the
library name and init entry point. The kdbx registration would be:

| Item | Value |
| --- | --- |
| Backend variant | `Backend::Kdbx` (or a `Backend::Generic` entry) |
| ADBC library name | `adbc_driver_kdbx` (backend A) / `_backend_b` (backend B) |
| Init entry point | `AdbcDriverKdbxInit` (A) / `AdbcBackendBInit` (B) |
| FFI protocol | `FFIProtocol::Adbc` |

Because `libadbc_driver_kdbx.so` follows the standard ADBC driver naming, it
can be discovered by name (community-adapter path, no CDN). The v2 adapter then
uses the shared `dbt-xdbc` driver manager, which calls the same init entry point
this host exercises directly.

Other v2 touch points for the kdbx adapter:
- `crates/dbt-adapter-core/src/lib.rs` — `AdapterType::Kdbx`, `quote_char`
- `crates/dbt-schemas` — `DbConfig::Kdbx` (host/port/user/pass/database,
  storage_type, partition_field, mode)
- `crates/dbt-auth` — credential resolution / connection URI
- `crates/dbt-adapter` — relation quoting, catalog queries, match arms
- `crates/dbt-loader` — Jinja SQL macros (kdbx__* materializations)

The Python v1 adapter in `dbt/` remains supported (dbt-core 1.x); the v2 Rust
adapter is a separate effort tracked in `.omo/notepads/adbc-v2/`.