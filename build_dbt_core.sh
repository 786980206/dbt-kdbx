#!/bin/bash
# Build the dbt-core v2 binary (dbt-sa-cli) with the kdbx adapter registered.
#
# dbt-core is pinned as a clean upstream submodule; the Kdbx adapter
# registration lives as a patch (driver/patches/dbt-core-kdbx.patch) that is
# re-applied here. To upgrade dbt-core: bump the submodule to the new upstream
# commit, then re-apply the patch (resolving conflicts if any).
#
# Preconditions:
#   - Rust toolchain on PATH (rustup), minimum version per dbt-core's MSRV.
#   - protoc available (dbt-core needs prost-build). If system protoc is
#     missing, install Python's protoc wrapper and point PROTOC at it, e.g.:
#       pip install grpcio-tools
#       PROTOC=/path/to/protoc-shim cargo build ...
#   - q server running with driver/adbc.q (backend A needs kx.arrowkdb).
#
# Produces: driver/dbt-core/target/{debug,release}/dbt-sa-cli
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
DBT_CORE="$PROJECT_DIR/driver/dbt-core"
PATCH="$PROJECT_DIR/driver/patches/dbt-core-kdbx.patch"

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

echo "=== [1/4] dbt-core submodule ==="
if [ ! -f "$DBT_CORE/Cargo.toml" ]; then
    git -C "$PROJECT_DIR" submodule update --init driver/dbt-core
else
    git -C "$PROJECT_DIR" submodule update --init driver/dbt-core
fi

echo "=== [2/4] apply kdbx patch ==="
cd "$DBT_CORE"
if git apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    echo "patch already applied, skipping"
elif git apply --check "$PATCH" >/dev/null 2>&1; then
    git apply "$PATCH"
    echo "patch applied"
else
    echo "error: cannot apply $PATCH (conflicts? wrong base?)" >&2
    exit 1
fi

echo "=== [3/4] cargo build -p dbt-sa-cli ($PROFILE) ==="
PATH="$HOME/.cargo/bin:$PATH" PROTOC="$PROTOC" cargo build -p dbt-sa-cli --profile "$PROFILE"

echo "=== [4/4] done ==="
BIN="$DBT_CORE/target/$PROFILE/dbt-sa-cli"
echo "binary: $BIN"
echo "version: $("$BIN" --version 2>&1 | head -1)"