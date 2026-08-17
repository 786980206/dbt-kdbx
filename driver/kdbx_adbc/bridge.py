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
    """pykx IPC turns Python str into a generic list; wrap as q char list."""
    return kx.q(f'"{sql}"')


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


def _to_pyarrow(result: "kx.q") -> pa.Table:
    """Convert a pykx q result to a pyarrow Table via tbl.pa()."""
    if hasattr(result, "pa"):
        return result.pa()
    if isinstance(result, pa.Table):
        return result
    raise TypeError(f"unsupported q result type: {type(result)!r}")