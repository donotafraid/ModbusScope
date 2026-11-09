#!/usr/bin/env bash
# Starts the two fault-injection servers and runs the P1 regression probes.
# Usage: tests/run_p1_regression.sh [build-dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${ROOT_DIR}/${BUILD_DIR}/bin"
HOST=127.0.0.1
SLOW_PORT=15030
BLACKHOLE_PORT=15031

cleanup() {
    if [[ -n "${SLOW_PID:-}" ]]; then kill "${SLOW_PID}" 2>/dev/null || true; fi
    if [[ -n "${BLACKHOLE_PID:-}" ]]; then kill "${BLACKHOLE_PID}" 2>/dev/null || true; fi
}
trap cleanup EXIT

python3 "${ROOT_DIR}/tests/modbus_server.py" --port "${SLOW_PORT}" \
    --slow-ms 1200 >/tmp/p1_slow_server.log 2>&1 &
SLOW_PID=$!
python3 "${ROOT_DIR}/tests/modbus_server.py" --port "${BLACKHOLE_PORT}" \
    --blackhole >/tmp/p1_blackhole_server.log 2>&1 &
BLACKHOLE_PID=$!

for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/${HOST}/${SLOW_PORT}") 2>/dev/null &&
       (exec 3<>"/dev/tcp/${HOST}/${BLACKHOLE_PORT}") 2>/dev/null; then
        break
    fi
    sleep 0.1
done

"${BIN_DIR}/modbus_client_p1_regression" timeout_budget "${HOST}" "${SLOW_PORT}"
"${BIN_DIR}/modbus_client_p1_regression" half_open "${HOST}" "${BLACKHOLE_PORT}"
timeout 10 "${BIN_DIR}/modbus_client_p1_regression" no_qcoreapp
