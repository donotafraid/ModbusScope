#!/usr/bin/env bash
# Shared helpers for the scripts/run_*.sh entry points.
#
# The functions never change the caller's working directory. Callers cd to the
# repository root themselves, matching the opcuaNetWay scripts. Every helper is
# fail-fast: callers run under `set -euo pipefail`.

qm_project_root() {
    cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" && pwd
}

log() { printf '\n=== %s ===\n' "$*"; }

warn() { printf 'WARN: %s\n' "$*" >&2; }

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

# Configure a build tree with the pinned toolchain when one is available.
# Usage: qm_configure <build-dir> [extra cmake args...]
qm_configure() {
    local build_dir="$1"
    shift
    local root
    root=$(qm_project_root)

    # Qt on Ubuntu is built with libstdc++; an inherited libc++ selection would
    # change std::string's ABI across the Qt boundary.
    unset CXXFLAGS LDFLAGS

    local args=(
        -S "$root"
        -B "$build_dir"
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"
        -DCMAKE_C_COMPILER="${CC:-clang-17}"
        -DCMAKE_CXX_COMPILER="${CXX:-clang++-17}"
    )
    if [[ -n "${CONAN_TOOLCHAIN:-}" ]]; then
        args+=("-DCMAKE_TOOLCHAIN_FILE=${CONAN_TOOLCHAIN}")
    elif [[ -f "${root}/conan_debug_build/conan_toolchain.cmake" ]]; then
        args+=("-DCMAKE_TOOLCHAIN_FILE=${root}/conan_debug_build/conan_toolchain.cmake")
    fi

    # Match quickstart.sh: point Clang at the GCC that owns libstdc++.
    local gcc_install_dir
    gcc_install_dir="${GCC_INSTALL_DIR:-$(dirname "$(gcc -print-file-name=libstdc++.so 2>/dev/null || true)")}"
    if [[ -n "$gcc_install_dir" && "$gcc_install_dir" != "." ]]; then
        args+=("-DCMAKE_CXX_FLAGS=--gcc-install-dir=${gcc_install_dir}")
    fi

    log "Configuring ${build_dir}"
    cmake "${args[@]}" "$@"
}

# Configure when needed, then build. Extra arguments are CMake options.
# Usage: qm_ensure_build <build-dir> [cmake options...]
qm_ensure_build() {
    local build_dir="$1"
    shift
    if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
        qm_configure "$build_dir" "$@"
    fi
    log "Building ${build_dir}"
    cmake --build "$build_dir" -j "${JOBS:-$(nproc 2>/dev/null || echo 2)}"
}

qm_wait_port() {
    local port="$1"
    python3 - "$port" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
deadline = time.time() + 5.0
while time.time() < deadline:
    sock = socket.socket()
    sock.settimeout(0.2)
    try:
        sock.connect(("127.0.0.1", port))
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
    finally:
        sock.close()
sys.exit(1)
PY
}

# Start tests/modbus_server.py and wait for its listen socket.
# Sets QM_SERVER_PID; the caller owns the EXIT trap.
# Usage: qm_start_server <port> <log-file> [server args...]
qm_start_server() {
    local port="$1"
    local log_file="$2"
    shift 2
    local root
    root=$(qm_project_root)

    python3 "${root}/tests/modbus_server.py" \
        --host 127.0.0.1 --port "$port" "$@" >"$log_file" 2>&1 &
    QM_SERVER_PID=$!
    if ! qm_wait_port "$port"; then
        kill "$QM_SERVER_PID" 2>/dev/null || true
        wait "$QM_SERVER_PID" 2>/dev/null || true
        QM_SERVER_PID=""
        die "Modbus server did not listen on 127.0.0.1:${port}; see ${log_file}"
    fi
}

# Kill the server started by qm_start_server, if any.
qm_stop_server() {
    if [[ -n "${QM_SERVER_PID:-}" ]]; then
        kill "$QM_SERVER_PID" 2>/dev/null || true
        wait "$QM_SERVER_PID" 2>/dev/null || true
        QM_SERVER_PID=""
    fi
}
