#!/usr/bin/env bash
# Run the P2 write-echo probe against a fault-injecting server.
#
#   tests/run_p2_regression.sh [build-dir]
#
# The script owns the server lifetime; no pre-running process is required.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${1:-build}
if [[ "$BUILD_DIR" = /* ]]; then
    BIN="${BUILD_DIR}/bin/modbus_client_p2_regression"
else
    BIN="${ROOT}/${BUILD_DIR}/bin/modbus_client_p2_regression"
fi
PORT=${P2_PORT:-15032}
SERVER="${ROOT}/tests/modbus_server.py"

[[ -x "$BIN" ]] || {
    echo "missing $BIN; run cmake --build $BUILD_DIR first" >&2
    exit 2
}

python3 "$SERVER" --host 127.0.0.1 --port "$PORT" --bad-write-echo \
    >/tmp/modbus_p2_server.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT

# Wait for the TCP listen socket rather than sleeping blindly.
for _ in $(seq 1 50); do
    if python3 - "$PORT" <<'PY'
import socket, sys
port = int(sys.argv[1])
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(("127.0.0.1", port))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
    then
        break
    fi
    sleep 0.1
done

"$BIN" write_echo 127.0.0.1 "$PORT"
