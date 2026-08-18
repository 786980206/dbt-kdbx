"""Backend B bridge — embedded CPython + pykx (normal IPC mode).

The driver .so (Cython) embeds CPython, imports this module, and calls into it.
pykx is used in *normal* mode: SyncQConnection to a running q server (no
embedded q, no arrowkdb). q results are converted to pyarrow and exported via
the Arrow C Data Interface so the C shell can hand an ArrowArrayStream back to
the ADBC caller without copying.
"""

from __future__ import annotations

import threading
from typing import Optional

import pyarrow as pa
import pykx as kx

_lock = threading.RLock()
_conn: Optional[kx.SyncQConnection] = None


def connect(host: str, port: int, user: str = "", password: str = "") -> None:
    """Open a pykx SyncQConnection (normal IPC mode)."""
    global _conn
    with _lock:
        if _conn is not None:
            _conn.close()
        kwargs = {"host": host, "port": port}
        if user:
            kwargs["username"] = user
            if password:
                kwargs["password"] = password
        _conn = kx.SyncQConnection(**kwargs)


def close() -> None:
    global _conn
    with _lock:
        if _conn is not None:
            _conn.close()
            _conn = None


def _as_q_string(sql: str) -> "kx.q":
    """pykx IPC turns Python str into a generic list; wrap as q char list.

    Internal double quotes in `sql` are escaped so nested q string literals
    survive the round-trip through a q "..." literal.
    """
    escaped = sql.replace("\\", "\\\\").replace('"', '\\"')
    return kx.q(f'"{escaped}"')


def execute_query(sql: str) -> pa.Table:
    """Execute SQL against the q server and return a pyarrow Table."""
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string(sql))
    return _to_pyarrow(result)


def execute_update(sql: str) -> int:
    """Execute a DML statement; return rows affected.

    q update/delete are functional (they return a new table without mutating
    the source); dbt materialisations use upsert, so affected-rows semantics
    are only meaningful for upsert-shaped statements. Return 0 by default; the
    q server may report a real count when the statement is an upsert.
    """
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string(sql))
    try:
        n = int(result.py())
    except (TypeError, ValueError):
        n = 0
    return n


def execute_schema(sql: str) -> pa.Schema:
    """Return the schema of a query without executing data."""
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string(sql))
    return _to_pyarrow(result).schema


def get_table_schema(catalog: Optional[str] = None,
                     schema: Optional[str] = None,
                     table: Optional[str] = None) -> pa.Schema:
    """Return the schema of a named table.

    The C shell calls with a single positional argument (the table name), which
    lands in `catalog`; tolerate both call shapes.
    """
    if table is None:
        table = catalog
    if table is None:
        raise RuntimeError("table required")
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string(f"0!{table}"))
    return _to_pyarrow(result).schema


def get_table_types() -> pa.Table:
    """Return the supported table types as a pyarrow Table."""
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string("([]table_type:enlist \"TABLE\")"))
    return _to_pyarrow(result)


def get_info() -> pa.Table:
    """Return driver metadata as a pyarrow Table."""
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string(
            "([]info_name:enlist 0;info_value:enlist \"kdb-x\")"))
    return _to_pyarrow(result)


def get_objects() -> pa.Table:
    """Return catalog objects as a pyarrow Table.

    Built from native q metadata (`tables[]` + `meta`) so backend B does not
    depend on the server-side adbc.q functions (those are a backend A /
    arrowkdb deployment prerequisite). Shape follows the ADBC GetObjects
    contract: catalog_name + nested catalog_db_schemas.
    """
    with _lock:
        if _conn is None:
            raise RuntimeError("not connected")
        result = _conn(_as_q_string(
            "([]catalog_name:enlist `;"
            "  catalog_db_schemas:enlist ([]db_schema_name:enlist `main))"))
    data = result.py()
    if not isinstance(data, dict):
        data = {"catalog_name": [], "catalog_db_schemas": []}
    catalogs = data.get("catalog_name", [])
    schemas = data.get("catalog_db_schemas", [])
    if not isinstance(catalogs, list):
        catalogs = [catalogs]
    if not isinstance(schemas, list):
        schemas = [schemas]
    schema_arrays = []
    for s in schemas:
        if not isinstance(s, dict):
            s = {}
        schema_arrays.append(pa.StructArray.from_arrays(
            [pa.array(s.get("db_schema_name", []) if isinstance(s.get("db_schema_name"), list) else [s.get("db_schema_name")])],
            names=["db_schema_name"]))
    return pa.Table.from_arrays(
        [pa.array(catalogs), pa.array(schema_arrays, type=pa.list_(schema_arrays[0].type) if schema_arrays else pa.list_(pa.struct([])))],
        names=["catalog_name", "catalog_db_schemas"])


def _to_pyarrow(result: "kx.q") -> pa.Table:
    """Convert a pykx q result to a pyarrow Table via tbl.pa()."""
    if hasattr(result, "pa"):
        return result.pa()
    if isinstance(result, pa.Table):
        return result
    raise TypeError(f"unsupported q result type: {type(result)!r}")