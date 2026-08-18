# adbc-driver-kdbx

ADBC (Arrow Database Connectivity) driver for kdb+/KDB-X, **dual backend**.

This is the v2 driver layer that the dbt-core v2 Rust adapter dlopens. It
packages as a Python wheel (`adbc-driver-kdbx`) whose `kdbx_adbc` package
contains:

- `libadbc_driver_kdbx.so` — **Backend A** (native C): a plain ADBC C driver
  that talks to a q server. The server side (`adbc.q`) uses arrowkdb to
  serialize q tables as Arrow IPC.
- `_backend_b.so` — **Backend B** (embedded): a C shell that embeds CPython,
  imports `kdbx_adbc.bridge`, and drives pykx in normal IPC mode
  (`SyncQConnection`). Results are exported zero-copy through the Arrow C
  Data Interface (`__arrow_c_stream__`).
- `bridge.py` — the pykx bridge used by Backend B.
- `adbc.q` — q server-side ADBC functions (Backend A).

## Requirements

- Python >= 3.10
- `pyarrow`, `pykx`, `adbc-driver-manager`
- Backend A: a q server with `kx/arrowkdb` installed (load `adbc.q`).
- Backend B: pykx installed (normal IPC mode; no embedded q, no KX licence).

## Build

```bash
./build.sh                 # builds both backends into driver/build + kdbx_adbc/
pip install -e .           # or: uv pip install -e driver/
```

Release artifacts are compiled with `-O3` + LTO (`-flto` / `thin`) and
stripped; typical sizes: backend A ~280K, backend B ~20K, Rust host ~920K.

## Usage

Load a q server with the ADBC functions, then use any ADBC client:

```bash
q adbc.q -p 19500 &
```

Python (via adbc-driver-manager):

```python
import adbc_driver_manager as adm
with adm.connect("kdbx://127.0.0.1:19500") as conn:
    with conn.cursor() as cur:
        cur.execute("select from trade")
        table = cur.fetch_arrow_table()
```

The two backends share the same C ABI. The Rust dbt adapter selects the
backend at driver load; see `driver/src/` for the exported entry points.

## Tests

`test/test.py` is a C-API smoke test. Run a q server (see above) then:

```bash
# backend A (native C)
KDBX_BACKEND=a python test/test.py
# backend B (embedded CPython + pykx)
KDBX_BACKEND=b python test/test.py
```

`KDBX_PORT` overrides the server port (default 9500). Both backends run 41/41.

## One-shot demo

`./demo.sh` starts a q server if needed, runs both backends' test.py (41/41
each), then drives the Rust host (`rust/`) through the full ADBC surface and
executes dbt-compiled q expressions end-to-end:

```bash
./demo.sh
# or: KDBX_PORT=19001 ./demo.sh
```

Note: in shell, symbol literals in the query must not be escaped, e.g.
`$'select sym, price from trade where sym = `aapl'` (a backslash before the
backtick turns it into a string comparison and the filter silently matches
everything).

## CI

`.github/workflows/ci.yml` builds both backends and the Rust host and verifies
exported entry-point symbols. Runtime tests against a q server (backend B
41/41 suite, Rust E2E) are **not** run in CI — they need a q server, which is
flaky to provision on hosted runners (and backend A needs the commercial
`kx.arrowkdb`). Run them locally with `./demo.sh` or `ci/start_server.sh` +
`test/test.py`.