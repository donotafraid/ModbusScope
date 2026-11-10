#!/usr/bin/env bash
# LSan-only smoke/stress run for the current wrapper binaries.
#
#   BUILD_DIR=build-lsan scripts/run_lsan.sh
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build-lsan}
PORT=${PORT:-15061}
BIN_DIR="${ROOT}/${BUILD_DIR}/bin"

[[ -x "${BIN_DIR}/modbus_client_smoke" ]] || {
    echo "missing ${BIN_DIR}/modbus_client_smoke; build ${BUILD_DIR} first" >&2
    exit 2
}

python3 "${ROOT}/tests/modbus_server.py" --port "${PORT}" \
    >/tmp/modbus_lsan_server.log 2>&1 &
server_pid=$!
trap 'kill "${server_pid}" 2>/dev/null || true' EXIT
sleep 0.3

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:report_objects=1"
export LSAN_OPTIONS="halt_on_error=1:report_objects=1"

"${BIN_DIR}/modbus_client_smoke" 127.0.0.1 "${PORT}"
"${BIN_DIR}/modbus_client_stress" 127.0.0.1 "${PORT}" 8
echo "LSan exercises passed"
