#!/usr/bin/env bash
# One entry point for a fresh checkout.
#
#   ./scripts/run_all.sh                 # build + CTest + integration clients
#   ./scripts/run_all.sh --no-integration
#   ./scripts/run_all.sh --sanitizers    # ASan, LSan, TSan after the tests
#
# The sanitizer pass is intentionally opt-in: it rebuilds the tree three times
# and is the slowest part of the local matrix.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"

cd "$(qm_project_root)"

RUN_SANITIZERS=0
TEST_ARGS=()
for argument in "$@"; do
    case "$argument" in
    --sanitizers) RUN_SANITIZERS=1 ;;
    --no-integration) TEST_ARGS+=(--no-integration) ;;
    -h|--help)
        sed -n '2,10p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
    *) die "unknown argument: ${argument}" ;;
    esac
done

"${SCRIPT_DIR}/run_tests.sh" "${TEST_ARGS[@]}"

if [[ "$RUN_SANITIZERS" -eq 1 ]]; then
    "${SCRIPT_DIR}/run_sanitizers.sh" all
fi

log "run_all.sh completed"
