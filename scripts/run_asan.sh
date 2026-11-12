#!/usr/bin/env bash
# ASan + UBSan CTest run.
#
#   BUILD_DIR=build-asan scripts/run_asan.sh
#
# Kept as a stable entry point; the matrix lives in run_sanitizers.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/run_sanitizers.sh" asan "$@"
