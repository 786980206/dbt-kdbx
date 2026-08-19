#!/bin/bash
# Build the dbt-core v2 binary (dbt-sa-cli) with the kdbx adapter registered.
#
# Preconditions:
#   - Rust toolchain on PATH (rustup), minimum version per dbt-core's MSRV.
#   - protoc available (dbt-core needs prost-build). If system protoc is
#     missing, install Python's protoc wrapper and point PROTOC at it, e.g.:
#       pip install grpcio-tools
#       PROTOC="$(python3 -c 'import sys;print(sys.executable)')" ... # or a shim
#   - q server running with driver/adbc.q (backend A needs kx.arrowkdb).
#
# Produces: driver/dbt-core/target/{debug,release}/dbt-sa-cli
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
DBT_CORE="$PROJECT_DIR/driver/dbt-core"

PROFILE="${PROFILE:-release}"
PROTOC="${PROTOC:-$(command -v protoc || true)}"
CARGO="${CARGO:-$(command -v cargo)}"

if [ -z "$CARGO" ]; then
    echo "error: cargo not found" >&2
    exit 1
fi

if [ -z "$PROTOC" ]; then
    echo "error: protoc not found; set PROTOC or install grpcio-tools + shim" >&2
    exit 1
fi
echo "Using PROTOC=$PROTOC ($($PROTOC --version))"

echo "=== [1/2] dbt-core submodule check ==="
if [ ! -f "$DBT_CORE/Cargo.toml" ]; then
    git -C "$PROJECT_DIR" submodule update --init --depth 1 driver/dbt-core
fi

echo "=== [2/2] cargo build -p dbt-sa-cli ($PROFILE) ==="
cd "$DBT_CORE"
PATH="$HOME/.cargo/bin:$PATH" PROTOC="$PROTOC" cargo build -p dbt-sa-cli --profile "$PROFILE"

BIN="$DBT_CORE/target/$PROFILE/dbt-sa-cli"
echo "=== done ==="
echo "binary: $BIN"
echo "version: $("$BIN" --version 2>&1 | head -1)"
