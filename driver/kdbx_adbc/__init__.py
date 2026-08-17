"""adbc-driver-kdbx — ADBC driver for kdb+/KDB-X.

Dual backend:
- Backend A (native C): khpu()/k() IPC to a q server running adbc.q + arrowkdb.
- Backend B (Cython/pykx): embedded CPython + pykx.SyncQConnection to a plain
  q server (no arrowkdb needed); q results converted to pyarrow, exposed via
  the Arrow C Data Interface (__arrow_c_stream__).
"""

__version__ = "0.1.0"