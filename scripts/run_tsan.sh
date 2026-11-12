#!/usr/bin/env bash
# TSan CTest run with setarch -R when the kernel allows it.
#
#   BUILD_DIR=build-tsan scripts/run_tsan.sh
#
# Kept as a stable entry point; the matrix lives in run_sanitizers.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/run_sanitizers.sh" tsan "$@"
