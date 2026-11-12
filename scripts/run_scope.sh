#!/usr/bin/env bash
# Build and run the Qt Widgets Scope demo.
#
#   ./scripts/run_scope.sh                         # local server + window
#   ./scripts/run_scope.sh --host 192.0.2.10       # existing PLC, no server
#   ./scripts/run_scope.sh --screenshot scope.png  # offscreen evidence
#   ./scripts/run_scope.sh --paused                # start in editable pause
#
# Environment:
#   BUILD_DIR   build tree (default: build)
#   JOBS        parallel build jobs
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"

cd "$(qm_project_root)"

BUILD_DIR=${BUILD_DIR:-build}
HOST=127.0.0.1
PORT=15020
INTERVAL=250
SCREENSHOT=""
START_SERVER=1
PAUSED=0
SERVER_LOG=/tmp/modbus_scope_server.log

while [[ $# -gt 0 ]]; do
    case "$1" in
    --host) HOST=$2; START_SERVER=0; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --interval) INTERVAL=$2; shift 2 ;;
    --screenshot) SCREENSHOT=$2; shift 2 ;;
    --paused) PAUSED=1; shift ;;
    --no-server) START_SERVER=0; shift ;;
    -h|--help)
        sed -n '2,9p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
    *) die "unknown argument: $1" ;;
    esac
done

qm_ensure_build "$BUILD_DIR"
DEMO="${BUILD_DIR}/bin/modbus_scope_demo"
[[ -x "$DEMO" ]] || die "missing ${DEMO}; configure with -DMODBUSSCOPE_BUILD_GUI=ON"

EXTRA=()
if [[ "$PAUSED" -eq 1 ]]; then
    EXTRA+=(--paused)
fi

if [[ "$START_SERVER" -eq 1 ]]; then
    trap qm_stop_server EXIT
    qm_start_server "$PORT" "$SERVER_LOG"
fi

if [[ -n "$SCREENSHOT" ]]; then
    log "Rendering ${SCREENSHOT} offscreen"
    QT_QPA_PLATFORM=offscreen "$DEMO" \
        --host "$HOST" --port "$PORT" --interval "$INTERVAL" \
        --screenshot "$SCREENSHOT" "${EXTRA[@]}"
    log "Wrote ${SCREENSHOT}"
else
    exec "$DEMO" --host "$HOST" --port "$PORT" --interval "$INTERVAL" \
        "${EXTRA[@]}"
fi
