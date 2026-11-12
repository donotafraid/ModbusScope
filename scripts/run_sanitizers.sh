#!/usr/bin/env bash
# Configure, build and run one sanitizer matrix.
#
#   ./scripts/run_sanitizers.sh asan
#   ./scripts/run_sanitizers.sh lsan
#   ./scripts/run_sanitizers.sh tsan
#   ./scripts/run_sanitizers.sh all
#
# Environment:
#   BUILD_DIR   override the per-mode build tree (default: build-<mode>)
#   JOBS        parallel build jobs
#   FILTER      CTest -R expression
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"

cd "$(qm_project_root)"
PROJECT_ROOT=$(qm_project_root)

MODE="${1:-all}"
[[ $# -gt 0 ]] && shift
FILTER=${FILTER:-}

case "$MODE" in
asan|lsan|tsan|all) ;;
*) die "usage: $0 {asan|lsan|tsan|all}" ;;
esac

run_mode() {
    local mode="$1"
    local build_dir="${BUILD_DIR:-build-${mode}}"
    local log_dir="${build_dir}/logs"
    local cmake_flag
    case "$mode" in
    asan) cmake_flag=-DENABLE_ASAN=ON ;;
    lsan) cmake_flag=-DENABLE_LSAN=ON ;;
    tsan) cmake_flag=-DENABLE_TSAN=ON ;;
    esac

    qm_ensure_build "$build_dir" "$cmake_flag"
    mkdir -p "$log_dir"
    local ctest_filter=()
    if [[ -n "$FILTER" ]]; then
        ctest_filter=(-R "$FILTER")
    fi

    log "Sanitizer: ${mode}"
    case "$mode" in
    asan)
        # UBSan reports first; leak detection is the separate lsan mode.
        ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:report_objects=1" \
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:print_summary=1" \
            ctest --test-dir "$build_dir" --output-on-failure \
                  "${ctest_filter[@]}" 2>&1 | tee "${log_dir}/ctest.log"
        ;;
    lsan)
        ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:report_objects=1" \
        LSAN_OPTIONS="suppressions=${PROJECT_ROOT}/tests/lsan.supp:halt_on_error=1:report_objects=1" \
            ctest --test-dir "$build_dir" --output-on-failure \
                  "${ctest_filter[@]}" 2>&1 | tee "${log_dir}/ctest.log"
        ;;
    tsan)
        local arch_prefix=()
        if command -v setarch >/dev/null 2>&1 &&
           setarch "$(uname -m)" -R true >/dev/null 2>&1; then
            arch_prefix=(setarch "$(uname -m)" -R)
        else
            warn "setarch -R unavailable; TSan may report environment noise"
        fi
        TSAN_OPTIONS="halt_on_error=1:exitcode=66:second_deadlock_stack=1" \
            "${arch_prefix[@]}" ctest --test-dir "$build_dir" \
            --output-on-failure "${ctest_filter[@]}" \
            2>&1 | tee "${log_dir}/ctest.log"
        ;;
    esac
    log "${mode} passed; log: ${log_dir}/ctest.log"
}

PROJECT_ROOT=$(qm_project_root)

case "$MODE" in
all)
    run_mode asan
    run_mode lsan
    run_mode tsan
    ;;
*)
    run_mode "$MODE"
    ;;
esac
