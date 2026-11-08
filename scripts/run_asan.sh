#!/usr/bin/env bash
# scripts/run_asan.sh —— 在 build-asan/ 下运行 ASan + UBSan 用例
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-asan}"
TEST_BIN="${BUILD_DIR}/tests-build/tests"
LOG_DIR="${BUILD_DIR}/asan_logs"

[[ -x "${TEST_BIN}" ]] || { echo "缺少 ${TEST_BIN}，请先在 ${BUILD_DIR} 下 cmake --build"; exit 2; }
mkdir -p "${LOG_DIR}"

ARCH_FLAG="${ARCH_FLAG:-}"

# ASan + UBSan 覆盖：所有错误出口 + 生命周期 + 预算
FILTER='Client*'


run_one() {
local name="$1" filter="$2"
echo "=== ASan/UBSan: ${name} (filter=${filter}) ==="

local n
n=$("${TEST_BIN}" --gtest_list_tests --gtest_filter="${filter}" 2>/dev/null \
      | grep -cE '^\s+\S' || true)
if [[ "${n}" -eq 0 ]]; then
  echo "ERROR: filter '${filter}' matched 0 tests" >&2
  return 1
fi
echo "  -> ${n} tests selected"

ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:report_objects=1:log_path=${LOG_DIR}/asan_report_${name}" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:print_summary=1" \
  ${ARCH_FLAG} "${TEST_BIN}" \
    --gtest_filter="${filter}" \
    2>&1 | tee "${LOG_DIR}/asan_${name}.log"

return "${PIPESTATUS[0]}"
}

run_one Normal  "${FILTER}"
echo "ASan/UBSan 全部通过。日志在 ${LOG_DIR}/"