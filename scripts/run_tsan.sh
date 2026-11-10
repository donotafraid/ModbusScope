#!/usr/bin/env bash
# TSan smoke/stress run for the current wrapper binaries.
#
#   BUILD_DIR=build-tsan scripts/run_tsan.sh
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build-tsan}
PORT=${PORT:-15062}
BIN_DIR="${ROOT}/${BUILD_DIR}/bin"

[[ -x "${BIN_DIR}/modbus_client_smoke" ]] || {
    echo "missing ${BIN_DIR}/modbus_client_smoke; build ${BUILD_DIR} first" >&2
    exit 2
}

ARCH_PREFIX=()
if command -v setarch >/dev/null 2>&1 &&
   setarch "$(uname -m)" -R true >/dev/null 2>&1; then
    ARCH_PREFIX=(setarch "$(uname -m)" -R)
else
    echo "WARN: setarch -R unavailable; TSan may report environment noise" >&2
fi

python3 "${ROOT}/tests/modbus_server.py" --port "${PORT}" \
    >/tmp/modbus_tsan_server.log 2>&1 &
server_pid=$!
trap 'kill "${server_pid}" 2>/dev/null || true' EXIT
sleep 0.3

export TSAN_OPTIONS="halt_on_error=1:exitcode=66:second_deadlock_stack=1"

"${ARCH_PREFIX[@]}" "${BIN_DIR}/modbus_client_smoke" 127.0.0.1 "${PORT}"
"${ARCH_PREFIX[@]}" "${BIN_DIR}/modbus_client_stress" 127.0.0.1 "${PORT}" 8
echo "TSan exercises passed"
