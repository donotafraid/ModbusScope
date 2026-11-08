#!/usr/bin/env bash
# scripts/run_bench.sh -- build and run the performance / endurance benchmarks.
#
# Prerequisites are the same as the unit tests: Conan dependencies installed
# and an open62541 v1.4.14 build tree. Point OPEN62541_ROOT at that tree.
#
#   OPEN62541_ROOT=/path/to/open62541-v1.4.14 ./scripts/run_bench.sh
#   SKIP_LONG=1 ./scripts/run_bench.sh          # skip the 24h soak
#   SOAK_HOURS=0.01 ./scripts/run_bench.sh      # ~36 s soak smoke test
#
# Results are written to bench-results/ (override with OUT_DIR).
set -euo pipefail

# Honour a caller-provided OPEN62541_ROOT; only fall back to the local build
# tree when it exists. An unconditional export ignored the
# "OPEN62541_ROOT=/path/to/open62541-v1.4.14 ./scripts/run_bench.sh" form that
# this file documents.
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$(pwd)

OPEN62541_ROOT=${OPEN62541_ROOT:-$ROOT/third_party/open62541-v1.4.14}
export OPEN62541_ROOT
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

BUILD_DIR="${BUILD_DIR:-build-bench}"
OUT_DIR="${OUT_DIR:-${PROJECT_ROOT}/bench-results}"
BUILD_TYPE="${BENCH_BUILD_TYPE:-RelWithDebInfo}"
CC=${CC:-clang-17}
CXX=${CXX:-clang++-17}
mkdir -p "${OUT_DIR}"

CONAN_TOOLCHAIN="${CONAN_TOOLCHAIN:-${PROJECT_ROOT}/conan_bench_build/conan_toolchain.cmake}"

CMAKE_ARGS=(-B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}")
if [[ -n "${CONAN_TOOLCHAIN:-}" && -f "${CONAN_TOOLCHAIN}" ]]; then
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="${CONAN_TOOLCHAIN}")
  CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$(dirname "${CONAN_TOOLCHAIN}")")
fi
if [[ -n "${OPEN62541_ROOT:-}" ]]; then
  CMAKE_ARGS+=(-DOPEN62541_ROOT="${OPEN62541_ROOT}")
fi
if [[ -n "${CC:-}" ]]; then CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CC}"); fi
if [[ -n "${CXX:-}" ]]; then CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="${CXX}"); fi

echo "=== configure ${BUILD_DIR} (${BUILD_TYPE}) ==="
cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target bench_latency bench_recovery bench_rss bench_server -j  

BENCH_BIN="${BUILD_DIR}/bench-build"
LATENCY_REPEATS="${LATENCY_REPEATS:-3}"

echo
echo "=== [1/3] 8-thread batchRead latency (P50/P99, repeats=${LATENCY_REPEATS}) ==="
"${BENCH_BIN}/bench_latency" --threads 8 --ops 8000 --batch 16 \
  --repeats "${LATENCY_REPEATS}" \
  | tee "${OUT_DIR}/latency_8thread_read.txt"

echo
echo "=== [1b/3] 8-thread batchWrite latency (P50/P99, repeats=${LATENCY_REPEATS}) ==="
"${BENCH_BIN}/bench_latency" --threads 8 --ops 8000 --batch 16 --write \
  --repeats "${LATENCY_REPEATS}" \
  | tee "${OUT_DIR}/latency_8thread_write.txt"

echo
echo "=== [2/3] disconnect / watchdog recovery distribution ==="
"${BENCH_BIN}/bench_recovery" --api-cycles 30 --restart-cycles 20 \
  | tee "${OUT_DIR}/recovery_distribution.txt"

echo
if [[ "${SKIP_LONG:-0}" == "1" ]]; then
  echo "=== [3/3] long soak skipped (SKIP_LONG=1) ==="
else
  echo "=== [3/3] RSS soak: hours=${SOAK_HOURS:-24}, interval=60s (server out of process) ==="
  SOAK_PORT="${SOAK_PORT:-4892}"
  # Run the server in its own process. bench_rss reads /proc/self/statm; an
  # in-process server would put the server's allocations into the "client" RSS
  # curve, so the soak would not prove anything about client-side leaks.
  "${BENCH_BIN}/bench_server" --port "${SOAK_PORT}" \
    > "${OUT_DIR}/rss_server.log" 2>&1 &
  SOAK_SERVER_PID=$!
  cleanup_soak_server() {
    kill "${SOAK_SERVER_PID}" 2>/dev/null || true
    wait "${SOAK_SERVER_PID}" 2>/dev/null || true
  }
  trap cleanup_soak_server EXIT

  for _ in $(seq 1 100); do
    grep -q '^READY ' "${OUT_DIR}/rss_server.log" 2>/dev/null && break
    sleep 0.1
  done
  if ! grep -q '^READY ' "${OUT_DIR}/rss_server.log" 2>/dev/null; then
    echo "ERROR: bench_server did not become ready; see ${OUT_DIR}/rss_server.log" >&2
    exit 3
  fi

  "${BENCH_BIN}/bench_rss" --endpoint "opc.tcp://127.0.0.1:${SOAK_PORT}" \
      --hours "${SOAK_HOURS:-24}" --interval 60 --hz 1 \
      > "${OUT_DIR}/rss_curve.csv"

  cleanup_soak_server
  trap - EXIT
  echo "wrote ${OUT_DIR}/rss_curve.csv (server log: ${OUT_DIR}/rss_server.log)"

  echo "=== RSS acceptance check ==="
  # Reject a truncated curve: min-hours defaults to 98% of the requested window
  # (0 for sub-hour smoke runs). The verdict is archived, not discarded.
  RSS_MIN_HOURS="${RSS_MIN_HOURS:-$(awk -v h="${SOAK_HOURS:-24}" 'BEGIN { if (h >= 1) printf "%.2f", h*0.98; else print "0" }')}"
  if python3 "${PROJECT_ROOT}/scripts/check_rss.py" "${OUT_DIR}/rss_curve.csv" \
        --min-hours "${RSS_MIN_HOURS}" | tee "${OUT_DIR}/rss_check.txt"; then
    echo "RSS gate: PASS"
  else
    echo "RSS gate: FAIL (see ${OUT_DIR}/rss_check.txt)" >&2
    RSS_GATE_FAILED=1
  fi
fi

echo
echo "benchmarks done; results in ${OUT_DIR}/"
if [[ "${RSS_GATE_FAILED:-0}" == "1" ]]; then
  echo "RESULT: benchmarks ran, but the RSS acceptance gate FAILED" >&2
  exit 1
fi
