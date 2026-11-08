#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

BUILD_DIR="${BUILD_DIR:-build-tsan}"
TEST_BIN="${PROJECT_ROOT}/${BUILD_DIR}/tests-build/tests"
LOG_DIR="${PROJECT_ROOT}/${BUILD_DIR}/tsan_logs"
TSAN_SUPP="${PROJECT_ROOT}/scripts/tsan.supp"

[[ -x "${TEST_BIN}" ]] || { echo "缺少 ${TEST_BIN}，请先在 ${BUILD_DIR} 下 cmake --build" >&2; exit 2; }
[[ -f "${TSAN_SUPP}" ]] || { echo "缺少 ${TSAN_SUPP}" >&2; exit 2; }
mkdir -p "${LOG_DIR}"

ARCH_FLAG=""
if command -v setarch >/dev/null 2>&1 \
   && setarch "$(uname -m)" -R true >/dev/null 2>&1; then
  ARCH_FLAG="setarch $(uname -m) -R"
else
  echo "WARN: setarch -R unavailable; TSan may report false positives" >&2
fi

TSAN_ENV="halt_on_error=1:exitcode=66:report_signal_unsafe=1:second_deadlock_stack=1:suppressions=${TSAN_SUPP}"

FILTER_CONCURRENCY='Client.T3_*:Client.TA2_*:Client.TC_*:ClientTest.T5_*:Client.T16_*:Client.T17_*:Client.T18_*:Client.T25_*:Client.T26_*:Client.T40_*:Client.T41_*:Client.T42_*:Client.T44_*:Client.T45_*:Client.T51_*:Client.T53_*:Client.T64_*:Client.T66_*:Client.T77_*:Client.T78_*:Client.T85_*:Client.T86_*:Client.T91_*:Client.T98_*:Client.T99_*:Client.T127_*:Client.T8B_*:Client.T8C2_*:ClientExtra.T108_*:ClientTest.T4_*:ClientTest.T9_*:ClientTest.TD_*:ClientTest.T112_*:ClientLifecycle.T150_*:ClientLifecycle.T152_*:ClientLifecycle.T153_*:ClientLifecycle.T154_*:ClientLifecycle.T169_*:ClientStateRace.*'
FILTER_API_LEASE='Client.T27_*'
FILTER_DESTRUCTOR='Client.T8C1*'
FILTER_CHECK_CONNECTED='Client.T31_*:Client.T32_*:Client.T33_*'
FILTER_ERROR_PROP='Client.T46_*:Client.T47_*'
FILTER_CLIENT_EXTRA='ClientExtra.*'
FILTER_CLIENT_BUG='ClientBug.*'
FILTER_CLIENT_SAFETY='ClientExceptionSafety.*'

ALL_FILTERS="${FILTER_CONCURRENCY}:${FILTER_API_LEASE}:${FILTER_DESTRUCTOR}:${FILTER_CHECK_CONNECTED}:${FILTER_ERROR_PROP}:${FILTER_CLIENT_EXTRA}:${FILTER_CLIENT_BUG}:${FILTER_CLIENT_SAFETY}"

trap 'rc=$?; [[ $rc -ne 0 ]] && echo "TSan FAILED (exit=${rc})，见 ${LOG_DIR}/" >&2' EXIT

run_one() {
  local name="$1" filter="$2"
  echo "=== TSan: ${name} (filter=${filter}) ==="

  local n
  n=$(${ARCH_FLAG} "${TEST_BIN}" --gtest_list_tests --gtest_filter="${filter}" 2>/dev/null \
      | grep -cE '^\s+\S' || true)
  [[ "${n}" -gt 0 ]] || { echo "ERROR: filter '${filter}' matched 0 tests" >&2; exit 2; }
  echo "  -> ${n} tests selected"

  # 失败时由 set -e + pipefail 直接退出；成功时继续
  TSAN_OPTIONS="${TSAN_ENV}" \
    ${ARCH_FLAG} "${TEST_BIN}" --gtest_filter="${filter}" \
    2>&1 | tee "${LOG_DIR}/${name}.log"
}

run_one concurrency      "${FILTER_CONCURRENCY}"
run_one api_lease        "${FILTER_API_LEASE}"
run_one destructor       "${FILTER_DESTRUCTOR}"
run_one checkConnected   "${FILTER_CHECK_CONNECTED}"
run_one error_propagation "${FILTER_ERROR_PROP}"
run_one extra "${FILTER_CLIENT_EXTRA}"
run_one bug "${FILTER_CLIENT_BUG}"
run_one safety "${FILTER_CLIENT_SAFETY}" 

echo "检查 TSan 并发过滤器是否漏测..."

ALL_TESTS=$(${ARCH_FLAG} env "${TEST_BIN}" --gtest_list_tests | awk '
  /^[^ ]/ {suite=$1}
  /^  / {print suite $1}
')

SELECTED_TESTS=$(${ARCH_FLAG} env "${TEST_BIN}" --gtest_list_tests \
  --gtest_filter="${ALL_FILTERS}" | awk '
  /^[^ ]/ {suite=$1}
  /^  / {print suite $1}
')

MISSING=0
while read -r t; do
  case "$t" in
    *Concurrent*|*Concurrency*|*Thread*|*Threads*|*Parallel*|*Race*|*Shutdown*|*Recreate*)
      if ! grep -qxF "$t" <<< "$SELECTED_TESTS"; then
        echo "TSan 可能漏测: $t"
        MISSING=1
      fi
      ;;
  esac
done <<< "$ALL_TESTS"

if [ "$MISSING" -ne 0 ]; then
  echo "请把上面的用例登记到 FILTER_CONCURRENCY，或确认它们不需要 TSan。"
  exit 1
fi


echo "TSan 全部通过。日志在 ${LOG_DIR}/"