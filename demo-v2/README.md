# dbt-core v2 (Rust) + kdbx ADBC driver demo

This directory is a runnable dbt v2 demo that exercises the **kdbx ADBC
driver** (backend A) through the Rust `dbt-sa-cli` binary. Models compile to
**q expressions** (not SQL) that the q server evaluates via `value`.

## Layout

```
demo-v2/
  dbt_project.yml   # project name: kdbx_v2_test
  profiles.yml      # type: kdbx -> localhost:19500
  models/
    trade.sql           # table  materialization: q table literal
    trade_summary.sql   # view   materialization: q select ... by (group-by)
```

## Prerequisites

1. A running q server on `localhost:19500` serving `driver/adbc.q`:
   ```bash
   cd driver && q adbc.q -p 19500
   ```
   Backend A needs the commercial `kx.arrowkdb` module on the server (it is
   what serializes q tables to Arrow IPC).

2. The kdbx ADBC driver `.so` built (`driver/build/libadbc_driver_kdbx.so`,
   see `driver/build.sh`).

3. The `dbt-sa-cli` binary built with the kdbx adapter registered
   (`../build_dbt_core.sh`). The binary is searched via `LD_LIBRARY_PATH`.

## Run

```bash
# from this directory
DBT_PROFILES_DIR="$(pwd)" \
LD_LIBRARY_PATH=/path/to/driver/build \
/path/to/dbt-sa-cli debug

DBT_PROFILES_DIR="$(pwd)" \
LD_LIBRARY_PATH=/path/to/driver/build \
/path/to/dbt-sa-cli run
```

`dbt debug` probes the connection with the q scalar `1` (the default
`select 1 as id` probe is not parseable q). `dbt run` builds both models;
results land as global q variables `trade` / `trade_summary`.

## How it works

- The adapter has no SQL DDL. A "table" is a global q variable, so
  `kdbx__create_table_as` emits `{{ identifier }}: {{ sql }}; {{ identifier }}`
  (assign, then return the table for the query path).
- DDL steps (`drop_relation`, `rename_relation`, schema ops) are wrapped in
  `{% call statement(...) %}` and executed via the ADBC update path, which
  expects a scalar (long).
- dbt prepends a C-style query comment to every statement; the C driver
  strips the leading `/* ... */` block in `AdbcKdbxStatementSetSqlQuery`
  because q has no comment syntax.
- dbt's statement splitter must NOT lex the q expression as SQL, so Kdbx is
  in the no-split bucket alongside BigQuery/DuckDB/Alt, and its
  empty-statement test only treats truly-blank input as empty.
