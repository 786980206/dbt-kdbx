#!/bin/bash
# demo.sh — 一键验证 adbc-driver-kdbx 双后端 (A/B) 的完整 ADBC 表面 + Rust E2E。
#
# 用法:
#   ./demo.sh                # 用现有 q server (默认 19500)，无则自动启动
#   KDBX_PORT=19001 ./demo.sh
#
# 验证内容:
#   1. q server 就绪 (adbc.q + arrowkdb)
#   2. test.py 双后端 41/41 (adbc-driver-manager)
#   3. Rust host (adbc_host) 执行 dbt 模型编译的 q 表达式 (E2E)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${KDBX_PORT:-19500}"
PY="${PYTHON:-/home/windsing/miniconda3/bin/python}"
HOST_ADDR=127.0.0.1

Q_PID=""
if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo "[demo] starting q server on port $PORT ..."
    setsid bash -c "q '$HERE/adbc.q' -p $PORT > /tmp/kdbx_demo_q.log 2>&1 & echo \$! > /tmp/kdbx_demo_q.pid"
    sleep 2
    if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
        echo "[demo] ERROR: q server did not start. Is 'q' on PATH? Check /tmp/kdbx_demo_q.log"
        exit 1
    fi
    Q_PID="$(cat /tmp/kdbx_demo_q.pid)"
    echo "[demo] q server up (pid $Q_PID)"
else
    echo "[demo] q server already listening on port $PORT"
fi

echo
echo "===== 1. ADBC C-API smoke tests (test.py) ====="

echo "--- Backend A (native C + arrowkdb) ---"
KDBX_BACKEND=a KDBX_PORT=$PORT timeout 120 "$PY" -u "$HERE/test/test.py" 2>&1 | tail -3

echo "--- Backend B (embedded CPython + pykx) ---"
KDBX_BACKEND=b KDBX_PORT=$PORT timeout 120 "$PY" -u "$HERE/test/test.py" 2>&1 | tail -3

echo
echo "===== 2. Rust host (adbc_host) E2E: dbt-compiled q expressions ====="
RUST_BIN="$HERE/rust/target/release/adbc_host"
if [ ! -x "$RUST_BIN" ]; then
    echo "[demo] building Rust host (first run)..."
    (cd "$HERE/rust" && cargo build --release >/dev/null 2>&1)
fi

echo "--- Backend A ---"
timeout 60 "$RUST_BIN" "$HERE/build/libadbc_driver_kdbx.so" AdbcDriverKdbxInit "$HOST_ADDR" "$PORT" --full \
    | grep -E "schema:|total rows|execute_update|get_table|get_info|get_objects|OK"
echo "  [model] select sym, price from trade"
timeout 60 "$RUST_BIN" "$HERE/build/libadbc_driver_kdbx.so" AdbcDriverKdbxInit "$HOST_ADDR" "$PORT" "select sym, price from trade" \
    | grep -E "total rows|OK"

echo "--- Backend B ---"
timeout 60 "$RUST_BIN" "$HERE/kdbx_adbc/_backend_b.so" AdbcBackendBInit "$HOST_ADDR" "$PORT" --full \
    | grep -E "schema:|total rows|execute_update|get_table|get_info|get_objects|OK"
echo "  [model] select sym, price from trade where sym = \`aapl"
timeout 60 "$RUST_BIN" "$HERE/kdbx_adbc/_backend_b.so" AdbcBackendBInit "$HOST_ADDR" "$PORT" $'select sym, price from trade where sym = `aapl' \
    | grep -E "total rows|OK"

echo
echo "[demo] ALL DONE. q server pid=$Q_PID still running on port $PORT"