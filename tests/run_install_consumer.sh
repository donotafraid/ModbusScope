#!/usr/bin/env bash
# Install the build tree into a throwaway prefix, then configure, build and run
# a consumer that only knows about find_package(modbusScope).
#
#   tests/run_install_consumer.sh <build-dir> <prefix>
#
# Owned by CTest as `install_consumer`.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${1:?usage: run_install_consumer.sh <build-dir> <prefix>}
PREFIX=${2:?usage: run_install_consumer.sh <build-dir> <prefix>}

[[ -f "${BUILD_DIR}/CMakeCache.txt" ]] || {
    echo "missing build tree ${BUILD_DIR}" >&2
    exit 2
}

# Qt on Ubuntu is built with libstdc++.
unset CXXFLAGS LDFLAGS

rm -rf "$PREFIX"
mkdir -p "$PREFIX"

echo "=== install ${BUILD_DIR} -> ${PREFIX}"
cmake --install "$BUILD_DIR" --prefix "$PREFIX"

echo "=== configure consumer"
GCC_INSTALL_DIR=${GCC_INSTALL_DIR:-$(dirname "$(gcc -print-file-name=libstdc++.so 2>/dev/null || true)")}
CMAKE_ARGS=(
    -S "${ROOT}/tests/consumer"
    -B "${PREFIX}/consumer-build"
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_PREFIX_PATH="$PREFIX"
    -DCMAKE_CXX_COMPILER="${CXX:-clang++-17}"
)
if [[ -n "$GCC_INSTALL_DIR" && "$GCC_INSTALL_DIR" != "." ]]; then
    CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS=--gcc-install-dir=${GCC_INSTALL_DIR}")
fi
cmake "${CMAKE_ARGS[@]}"

echo "=== build consumer"
cmake --build "${PREFIX}/consumer-build" -j "${JOBS:-2}"

echo "=== run consumer"
"${PREFIX}/consumer-build/modbus_consumer"
echo "consumer OK"
