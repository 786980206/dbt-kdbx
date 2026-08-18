# AGENTS.md

Guidance for AI agents and contributors working in the dbt-kdbx repository.

## Project overview

dbt adapter for kdb+/KDB-X. dbt models compile to q expressions that are
executed against a kdb+ database via [PyKX](https://code.kx.com/pykx/) and the
vendored [qtk](https://github.com/786980206/qtk) library. Storage is either
`serialized`, `splayed`, or `partitioned`.

Two layers live in this repo:

- `dbt/` — the **v1 Python adapter** (dbt-core 1.x / dbt-adapters 1.x API).
  **dbt-core v2.0 is a Rust rewrite and the Python adapter API is not supported
  there** — do not change the dependency bounds to v2 without a full port.
- `driver/` — the **v2 ADBC driver** (native C + embedded-pyky Cython shell,
  dual backend), which the v2 dbt-core Rust adapter will dlopen. See
  `driver/README.md` and `AGENTS.md` below.

## Layout

```
dbt/
  adapters/kdbx/            # adapter implementation (regular package)
    credentials.py          # KDBXCredentials profile dataclass
    connections.py          # KDBXConnectionManager + DB-API-style cursor
    impl.py                 # KDBXAdapter(SQLAdapter)
    relation.py             # KDBXRelation (bare identifiers)
    column.py               # q <-> dbt type map
    storage.py              # storage strategies -> qtk q lambdas
    mod/qtk/                # vendored qtk q library (do not modify casually)
    __version__.py          # version attribute must be named `version`
  include/kdbx/             # dbt plugin include package
    dbt_project.yml
    macros/adapters.sql     # kdbx__* materialization macros
demo/                       # runnable dbt project (models + profiles.yml)
driver/                     # v2 ADBC driver (dual backend, Rust dlopen target)
  src/                      # native C backend (adbc_kdbx_*.c)
    cython/backend_b.c      # embedded-CPython + pykx backend (exported symbols)
  kdbx_adbc/                # Python package: bridge.py (pykx bridge) + .so files
  adbc.q                    # q server-side ADBC functions (arrowkdb serialization)
  test/test.py              # ADBC C-API smoke tests
pyproject.toml              # hatchling build, dbt entry-point group
```

## Critical gotchas

- `dbt/` is a **namespace package**: `dbt/`, `dbt/adapters/`, and
  `dbt/include/` must **never** have `__init__.py` (they merge with the
  installed dbt-core). Only `dbt/adapters/kdbx/` and `dbt/include/kdbx/` have
  `__init__.py`. Verify imports with `dbt debug` (namespace plugins are not
  listed by `dbt --version`).
- Imports of adapter infrastructure come from `dbt.adapters.*` /
  `dbt_common.*`, not `dbt.*` (see imports in `impl.py`, `connections.py`).
- `KDBXCredentials` must define `database`/`schema` fields and `unique_field`.
- `KDBXAdapter.__init__` signature is `(self, config, mp_context)`.
- `KDBXConnectionManager` must implement `cancel()` and `get_response()`.
- `__version__.py` must expose `version = "..."` (variable name matters).
- **driver (v2)**: `driver/` builds a dual-backend ADBC driver.
  - Backend A (native): C ADBC driver, `driver/src/adbc_kdbx_*.c`, talks to a
    q server (arrowkdb on the server side for Arrow serialization).
  - Backend B (embedded): `driver/src/cython/backend_b.c` embeds CPython,
    imports `kdbx_adbc.bridge`, and drives pykx in **normal IPC mode**
    (`SyncQConnection`) — no embedded q, no KX license needed. Results go back
    to the C host zero-copy via the Arrow C Data Interface
    (`__arrow_c_stream__`).
  - A Rust host dlopens the .so with `RTLD_GLOBAL` (Python extension symbols
    must resolve globally) and calls the exported `kdbx_b_*` C functions. The
    `.so` uses `dladdr` to add its own directory to `sys.path` so the
    `kdbx_adbc` package is importable without PYTHONPATH.
  - q `update` uses the standard `update col:expr from t where ...` form; the
    `set` keyword form is not portable across q versions.

## Conventions

- Bilingual comments preferred (the existing code uses Chinese + English).
- Do **not** string-concatenate q SQL; build q lambdas and pass values as
  Python objects (see `storage.py`).
- No new heavy dependencies (no Pandas/Polars); PyKX + agate only.
- Keep qtk vendored code read-only; drive it through `.qtk.*` calls.

## Common commands

```bash
uv sync                       # install deps into .venv
uv pip install -e .           # re-register the adapter entry point
cd demo && dbt debug          # verify profile + connection (local mode)
cd demo && dbt run            # build all models into demo/data
cd demo && dbt run -s <model> # run one model
```

## driver (v2) build + verification

```bash
cd driver && ./build.sh       # builds both backends (-O3/LTO/strip)
cd driver && ./demo.sh        # one-shot verify: 41/41 per backend + Rust E2E
cd driver/rust && cargo build --release  # Rust ADBC host
KDBX_BACKEND=b KDBX_PORT=19500 python driver/test/test.py  # backend B suite
```

CI (`.github/workflows/ci.yml`) covers backend B 41/41 + Rust E2E on a pykx
Community-Edition q server (`driver/ci/start_server.sh`). Backend A runtime
needs the commercial `kx.arrowkdb` module (adbc.q), so CI builds/link-checks
it only.

## Testing

No automated test suite exists yet; the `demo/` project is the manual smoke
test. Run the models listed in `demo/README.md` after any change to the
adapter. There is no lint/typecheck config in the repo.
