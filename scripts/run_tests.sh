#!/usr/bin/env bash
# Build the project and run the full local test matrix.
#
#   ./scripts/run_tests.sh                 # CTest + smoke/stress/recovery
#   ./scripts/run_tests.sh --no-integration
#   FILTER='ClientLifetime.*' ./scripts/run_tests.sh --no-integration
#
# Environment:
#   BUILD_DIR   build tree (default: build)
#   JOBS        parallel build jobs
#   PORT        first Modbus server port (default: 15020)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"

cd "$(qm_project_root)"

BUILD_DIR=${BUILD_DIR:-build}
PORT=${PORT:-15020}
RECOVERY_PORT=${RECOVERY_PORT:-15021}
RUN_INTEGRATION=1
FILTER=${FILTER:-}

for argument in "$@"; do
    case "$argument" in
    --no-integration) RUN_INTEGRATION=0 ;;
    --filter=*) FILTER=${argument#--filter=} ;;
    -h|--help)
        sed -n '2,12p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
    *)
        die "unknown argument: ${argument}"
        ;;
    esac
done

qm_ensure_build "$BUILD_DIR"

log "CTest (${BUILD_DIR})"
if [[ -n "$FILTER" ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure -R "$FILTER"
else
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

if [[ "$RUN_INTEGRATION" -eq 0 ]]; then
    log "Integration clients skipped (--no-integration)"
    exit 0
fi

BIN_DIR="${BUILD_DIR}/bin"
for binary in modbus_client_smoke modbus_client_stress modbus_client_recovery; do
    [[ -x "${BIN_DIR}/${binary}" ]] || die "missing ${BIN_DIR}/${binary}"
done

trap qm_stop_server EXIT

log "Integration: success path"
qm_start_server "$PORT" /tmp/modbus_run_tests_server.log
"${BIN_DIR}/modbus_client_smoke" 127.0.0.1 "$PORT"
"${BIN_DIR}/modbus_client_stress" 127.0.0.1 "$PORT" 8
qm_stop_server

log "Integration: disconnect after three requests"
qm_start_server "$RECOVERY_PORT" /tmp/modbus_run_tests_recovery.log --drop-after 3
"${BIN_DIR}/modbus_client_recovery" 127.0.0.1 "$RECOVERY_PORT"
qm_stop_server

log "All local tests passed"
