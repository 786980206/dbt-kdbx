#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
OUTPUT="$BUILD_DIR/libadbc_driver_kdbx.so"
BACKEND_B="$PROJECT_DIR/kdbx_adbc/_backend_b.so"

PYTHON_BIN="${PYTHON:-$(command -v python3 || command -v python)}"
PYTHON_INC="$($PYTHON_BIN -c 'import sysconfig; print(sysconfig.get_path("include"))')"
PYTHON_LIB="$($PYTHON_BIN -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')"

echo "=== adbc-driver-kdbx build ==="

# 1. Ensure submodule is initialized
if [ ! -f "$PROJECT_DIR/thirdparty/nanoarrow/CMakeLists.txt" ]; then
    echo "[1/4] Initializing nanoarrow submodule..."
    git -C "$PROJECT_DIR" submodule update --init --depth 1 thirdparty/nanoarrow
else
    echo "[1/4] nanoarrow submodule OK"
fi

# 2. Configure
echo "[2/4] CMake configure..."
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

# 3. Build backend A (native C)
echo "[3/5] Building backend A (native C)..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

# 4. Build backend B (embedded CPython + pykx shell)
echo "[4/5] Building backend B (embedded CPython + pykx)..."
mkdir -p "$PROJECT_DIR/kdbx_adbc"
PY_VERSION="$($PYTHON_BIN -c 'import sys; print("python%d.%d" % sys.version_info[:2])')"
gcc -fPIC -shared -O3 -fno-plt \
    -fdata-sections -ffunction-sections \
    -Wno-builtin-macro-redefined \
    -I"$PYTHON_INC" -I"$PROJECT_DIR/include" -L"$PYTHON_LIB" \
    -Wl,-rpath,"$PYTHON_LIB" \
    -Wl,--gc-sections -Wl,--strip-all \
    -o "$BACKEND_B" \
    "$PROJECT_DIR/src/cython/backend_b.c" \
    -l"$PY_VERSION" -ldl

# 5. Verify
echo "[5/5] Verifying..."
# Copy the real backend-A .so (dereferencing CMake symlinks) into the package
# dir so hatchling's force-include picks up an actual file, not a symlink.
REAL_A="$(readlink -f "$OUTPUT")"
if [ -f "$REAL_A" ]; then
    cp "$REAL_A" "$PROJECT_DIR/kdbx_adbc/libadbc_driver_kdbx.so"
    echo "OK: $REAL_A -> kdbx_adbc/libadbc_driver_kdbx.so ($(du -h "$PROJECT_DIR/kdbx_adbc/libadbc_driver_kdbx.so" | cut -f1))"
else
    echo "FAIL: $OUTPUT not found"
    exit 1
fi
if [ -f "$BACKEND_B" ]; then
    echo "OK: $BACKEND_B ($(du -h "$BACKEND_B" | cut -f1))"
    if nm -D "$BACKEND_B" | grep -q AdbcBackendBInit && nm -D "$BACKEND_B" | grep -q AdbcDriverKdbxBInit; then
        echo "  AdbcBackendBInit + AdbcDriverKdbxBInit exported"
    else
        echo "  WARN: expected entry points not found in dynamic symbols" >&2
    fi
else
    echo "FAIL: $BACKEND_B not found"
    exit 1
fi