# dbt-kdbx

dbt adapter for kdb+/KDB-X. Runs dbt models against a kdb+ database using
[PyKX](https://code.kx.com/pykx/) and the [qtk](https://github.com/786980206/qtk)
library, with three native kdb+ storage layouts:

- `serialized` — a single binary file per table (`set`)
- `splayed` — a directory with one column file per column
- `partitioned` — date-partitioned directories (column files per partition)

## Features

- Full `table`, `view` (degraded to table), `incremental` (insert / upsert),
  and `ephemeral` materializations
- Local embedded (`kx.q`) and remote IPC (`kx.SyncQConnection`) connection modes
- No SQL translation: compiled dbt queries are passed to q as-is via q lambdas
- Minimal dependencies: PyKX + agate (no Pandas / Polars)

## Requirements

- Python >= 3.10
- dbt-core >= 1.8, dbt-adapters >= 1.0
- PyKX >= 4.0.0 (includes an embedded KDB-X Community Edition for `local` mode)

## Installation

```bash
uv sync            # or: pip install -e .
```

The package registers itself through the `dbt` entry point group:

```
[project.entry-points.dbt]
kdbx = "dbt.adapters.kdbx"
```

## Configuration

Add a `kdbx` profile to `~/.dbt/profiles.yml`:

```yaml
your_project:
  outputs:
    dev_local:
      type: kdbx
      mode: local                    # local (EmbeddedQ) | remote (SyncQConnection)
      database: data                 # path to the kdb+ database directory
      storage_type: serialized       # serialized | splayed | partitioned
    dev_remote:
      type: kdbx
      mode: remote
      host: localhost
      port: 5001
      username: dev
      password: dev
      database: /path/to/data
      storage_type: serialized
  target: dev_local
```

| Key | Default | Description |
| --- | --- | --- |
| `mode` | `local` | `local` embeds q in-process via `kx.q`; `remote` connects over IPC |
| `database` | `data` | File-system path to the database; relative paths resolve against the dbt project root |
| `storage_type` | `serialized` | Default storage layout for tables |
| `partition_field` | `date` | Partition column used by `partitioned` storage |
| `host` / `port` | `localhost` / `5000` | Remote connection settings |
| `username` / `password` | empty | Remote connection credentials |

> Note: kdb+ has no schema concept; the `schema` field is unused and relation
> names render as bare identifiers.

## Usage

```bash
dbt run                    # build all models
dbt run -s model_incremental
dbt compile
dbt debug                  # verify profile / connection
```

Per-model storage settings are taken from model `config`:

```sql
-- serialized table
{{ config(materialized='table', storage_type='serialized') }}
select ...

-- date-partitioned table
{{ config(materialized='table', storage_type='partitioned', partition_field='date') }}
select ...

-- incremental upsert keyed on date + sym
{{ config(materialized='incremental', storage_type='serialized', unique_keys=['date', 'sym']) }}
select ...
```

Views are not natively supported by q; `materialized='view'` logs a warning and
falls back to a table.

## Demo project

See [`demo/`](demo/) for a runnable example project (models, profile, and sample
data already built):

```bash
cd demo
dbt run -s trade                    # base table
dbt run -s model_serialized         # serialized table
dbt run -s model_splayed            # splayed table
dbt run -s model_partitioned        # date-partitioned table
dbt run -s model_incremental        # incremental upsert
dbt run -s model_ephemeral          # ephemeral (CTE, no table written)
```

## How it works

- `dbt/adapters/kdbx/` — adapter package (namespace package sharing the
  installed `dbt` namespace):
  - `credentials.py` — `KDBXCredentials` profile dataclass
  - `connections.py` — `KDBXConnectionManager` + a DB-API-style cursor wrapping
    PyKX handles (local/remote routing)
  - `impl.py` — `KDBXAdapter(SQLAdapter)` with materialization overrides
  - `relation.py` — `KDBXRelation`, renders bare identifiers
  - `column.py` — q <-> dbt type mapping
  - `storage.py` — storage strategies that build q lambdas calling the qtk API
    (`.qtk.tbl.create/drop/rename/upsert/insert`); arguments are passed as
    Python objects, never string-concatenated
  - `mod/qtk/` — vendored qtk library
- `dbt/include/kdbx/` — dbt plugin include package (macros + `dbt_project.yml`)

## Known limitations

- No transaction support (kdb+ has none; `begin`/`commit`/`rollback` are no-ops)
- No `seed` loading (`load_seed_data` is a no-op)
- No query cancellation
- `dbt --version` does not list the namespace-registered plugin, though
  `dbt debug` recognizes it and connects correctly

## Development

```bash
uv sync --dev
cd demo && dbt debug && dbt run
```

## License

TBD
