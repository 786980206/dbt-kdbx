#!/bin/bash
# ci/start_server.sh — start a q server for CI tests.
#
# Backend B (pykx IPC) only needs a plain q server with a `trade` table.
# Backend A additionally needs the KX commercial `kx.arrowkdb` module loaded
# (via adbc.q); that is only available on licensed environments, so CI runs the
# full suite for backend B and the build/link checks for backend A.
#
# Usage: start_server.sh [port]   (default 19500)

set -euo pipefail

PORT="${1:-19500}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DRIVER_DIR="$(cd "$HERE/.." && pwd)"

if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo "[start_server] port $PORT already serving"
    exit 0
fi

# Locate a q binary: $QHOME/bin/q (pykx CE) or `q` on PATH.
if [ -n "${QHOME:-}" ] && [ -x "$QHOME/bin/q" ]; then
    Q_BIN="$QHOME/bin/q"
elif [ -n "${PYTHON:-}" ] && "$PYTHON" -c "import pykx; print(pykx.qhome)" > /tmp/ci_qhome 2>/dev/null; then
    QHOME="$(cat /tmp/ci_qhome)"
    Q_BIN="$QHOME/bin/q"
elif command -v q > /dev/null 2>&1; then
    Q_BIN="$(command -v q)"
    QHOME="$(dirname "$(dirname "$Q_BIN")")"
else
    echo "[start_server] ERROR: no q binary found (set QHOME or PYTHON)" >&2
    exit 1
fi

# pykx CE q requires pykx.q as the startup script for IPC compatibility.
PYKX_Q="$QHOME/pykx.q"
if [ -f "$PYKX_Q" ]; then
    START="$PYKX_Q"
else
    START=""
fi

echo "[start_server] using q=$Q_BIN port=$PORT"
setsid bash -c "$Q_BIN $START -p $PORT > /tmp/kdbx_ci_q.log 2>&1 & echo \$! > /tmp/kdbx_ci_q.pid"
sleep 2

if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo "[start_server] q up (pid $(cat /tmp/kdbx_ci_q.pid))"
else
    echo "[start_server] FAILED; log:" >&2
    cat /tmp/kdbx_ci_q.log >&2
    exit 1
fi

# Seed the `trade` table used by test.py (same shape as adbc.q).
if [ -n "${PYTHON:-}" ]; then
    "$PYTHON" - "$PORT" <<'EOF'
import pykx as kx, sys
port = int(sys.argv[1])
q = kx.SyncQConnection('127.0.0.1', port)
q('trade:([]time:2024.01.01D09:30:00.000000000+til 1000; sym:1000?`msft`ibm`goog`aapl; price:100.5+1000?1.0; size:100+1000?900)')
print('seeded trade rows =', q('count trade').py())
q.close()
EOF
fi
echo "[start_server] done"