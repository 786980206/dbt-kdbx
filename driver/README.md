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
python test/test.py
```