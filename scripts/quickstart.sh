#!/usr/bin/env bash
# One-command build + test for a fresh checkout.
#
#   ./scripts/quickstart.sh
#
# On Ubuntu/Debian this installs Qt5 dev packages and Clang when missing,
# then configures, builds and runs ctest.  It is intentionally boring:
# no hidden environment, no global install outside apt, idempotent.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$(pwd)

BUILD_DIR=${BUILD_DIR:-build}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
CC=${CC:-clang-17}
CXX=${CXX:-clang++-17}
export CC CXX

# 确保不使用 libc++：清掉可能从外部继承的 libc++ 选项。
# Clang 默认链接系统 libstdc++，与系统 Qt5 的 ABI 一致。
unset CXXFLAGS LDFLAGS 2>/dev/null || true

log() { printf '\n=== %s ===\n' "$*"; }
as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    sudo "$@"
  fi
}

# ---------------------------------------------------------------------------
# 1. Toolchain + Qt5.  clang-17 provides the C++ compiler; qtbase5-dev gives
#    Qt headers/libs/qmake; the Modbus module lives in libqt5serialbus5-dev.
# ---------------------------------------------------------------------------
missing=0
for tool in cmake git gcc "$CC" "$CXX" qmake; do
  command -v "$tool" >/dev/null 2>&1 || missing=1
done
# qmake can exist while the dev headers are absent; check a known header.
[ -f /usr/include/x86_64-linux-gnu/qt5/QtCore/qglobal.h ] || missing=1
# SerialBus 的 CMake 配置文件单独确认，避免只装 QtCore 就跳过安装。
[ -f /usr/lib/x86_64-linux-gnu/cmake/Qt5SerialBus/Qt5SerialBusConfig.cmake ] || missing=1

if [ "$missing" -eq 1 ]; then
  if ! command -v apt-get >/dev/null 2>&1; then
    echo "error: missing build tools and apt-get is unavailable." >&2
    echo "Install cmake, clang-17, qtbase5-dev and libqt5serialbus5-dev, then rerun." >&2
    exit 2
  fi

  log "Installing build dependencies (Ubuntu/Debian)"
  as_root apt-get update
  as_root apt-get install -y --no-install-recommends \
    build-essential cmake git \
    clang-17 \
    qtbase5-dev qt5-qmake libqt5serialbus5-dev
fi

# ---------------------------------------------------------------------------
# 2. GTest through Conan, then configure, build, test.
# ---------------------------------------------------------------------------
if [ -f "${ROOT}/conan_debug_build/conan_toolchain.cmake" ]; then
  log "Reusing conan_debug_build/conan_toolchain.cmake"
  CONAN_TOOLCHAIN="${ROOT}/conan_debug_build/conan_toolchain.cmake"
elif command -v conan >/dev/null 2>&1; then
  log "Installing Conan dependencies (GTest)"
  conan install "${ROOT}" -pr:a="${ROOT}/build.profile" \
        --output-folder="${ROOT}/conan_debug_build" --build=missing
  CONAN_TOOLCHAIN="${ROOT}/conan_debug_build/conan_toolchain.cmake"
else
  echo "warning: conan not found; the gtest unit suite will be skipped." >&2
  CONAN_TOOLCHAIN=""
fi

log "Configuring $BUILD_DIR"
# Clang can select a newer GCC installation than the one that owns the
# libstdc++ headers. Point it at the GCC that actually provides libstdc++ so
# Qt (built with libstdc++) and this library share the same C++ ABI.
GCC_INSTALL_DIR=${GCC_INSTALL_DIR:-$(dirname "$(gcc -print-file-name=libstdc++.so 2>/dev/null || true)")}
CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_C_COMPILER="$CC"
  -DCMAKE_CXX_COMPILER="$CXX"
)
if [[ -n "$GCC_INSTALL_DIR" && "$GCC_INSTALL_DIR" != "." ]]; then
  CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS=--gcc-install-dir=${GCC_INSTALL_DIR}")
fi
if [[ -n "$CONAN_TOOLCHAIN" ]]; then
  CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${CONAN_TOOLCHAIN}")
fi
cmake -S . -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"

log "Building"
cmake --build "$BUILD_DIR" -j "$JOBS"

log "Running tests"
ctest --test-dir "$BUILD_DIR" --output-on-failure

log "Done"