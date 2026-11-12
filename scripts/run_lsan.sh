#!/usr/bin/env bash
# LSan-only CTest run, including the Qt offscreen ScopeWidget test.
#
#   BUILD_DIR=build-lsan scripts/run_lsan.sh
#
# Kept as a stable entry point; the matrix lives in run_sanitizers.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/run_sanitizers.sh" lsan "$@"
