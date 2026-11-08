#!/usr/bin/env bash
# scripts/run_netem_matrix.sh -- run the degraded-network recovery matrix.
#
# Fills the "high latency / packet loss / half-open" row of the benchmark table:
# it injects each fault on loopback with scripts/netem_faults.sh, runs the
# relevant benchmark, archives the raw output under bench-results/netem/, and
# asserts the observable contract for that fault.
#
# Requires root (tc / iptables) and pre-built bench binaries:
#   SKIP_LONG=1 OPEN62541_ROOT=... ./scripts/run_bench.sh
#   sudo bash ./scripts/run_netem_matrix.sh
#
# Every injected fault is cleared on exit, including on failure.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

BUILD_DIR="${BUILD_DIR:-build-bench}"
BIN_DIR="${BENCH_BIN:-${BUILD_DIR}/bench-build}"
OUT_DIR="${OUT_DIR:-${PROJECT_ROOT}/bench-results/netem}"
NETEM="${PROJECT_ROOT}/scripts/netem_faults.sh"
PORT="${NETEM_PORT:-4890}"
RECOVERY_PORT="${NETEM_RECOVERY_PORT:-4891}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: this script injects tc/iptables faults and must run as root." >&2
  echo "       sudo OPEN62541_ROOT=... $0" >&2
  exit 2
fi
for b in bench_latency bench_recovery; do
  [[ -x "${BIN_DIR}/${b}" ]] || {
    echo "ERROR: missing ${BIN_DIR}/${b}. Run scripts/run_bench.sh first." >&2
    exit 2
  }
done
mkdir -p "${OUT_DIR}"

cleanup() { "${NETEM}" clear >/dev/null 2>&1 || true; }
trap cleanup EXIT

FAILED=0
declare -a SUMMARY=()

note() { SUMMARY+=("$1"); }

# run_latency <label> <expect: zero|any> <preset...>
run_latency() {
  local label="$1" expect="$2"; shift 2
  echo
  echo "=== ${label}: injecting '$*' ==="
  "${NETEM}" "$@" || { note "FAIL ${label}: fault injection failed"; FAILED=1; return; }

  local out="${OUT_DIR}/latency_${label}.txt"
  # The degraded link serialises every synchronous service call (~100 ms RTT at
  # 50 ms one-way delay, m_sdkMutex + SDK clientMutex). An 8x4000x3 workload
  # needs hours, so the old 180 s timeout always fired (rc=124) and left no
  # "median" line: every delay/loss case reported FAIL. A small workload still
  # produces a judgeable distribution and finishes inside the timeout.
  timeout 600 "${BIN_DIR}/bench_latency" --threads 2 --ops 40 --batch 8 \
    --repeats 2 >"${out}" 2>&1
  local rc=$?
  "${NETEM}" clear >/dev/null 2>&1 || true

  if [[ ${rc} -eq 124 ]]; then
    note "FAIL ${label}: timed out (not bounded)"
    FAILED=1
    return
  fi
  if ! grep -q '^median ' "${out}"; then
    note "FAIL ${label}: no median summary (see ${out})"
    FAILED=1
    return
  fi
  local errs
  errs="$(grep -oE 'errors=[0-9]+' "${out}" | tail -1 | cut -d= -f2)"
  if [[ "${expect}" == "zero" && "${errs}" != "0" ]]; then
    note "FAIL ${label}: errors=${errs} (expected 0)"
    FAILED=1
    return
  fi
  note "PASS ${label}: errors=${errs} ($(grep '^median ' "${out}" | tail -1))"
}

# 1) 50 ms one-way delay: latency rises, but every call must still succeed.
run_latency delay50 zero delay 50ms

# 2) 5% loss: TCP retransmits, so errors may be non-zero; the contract is that
#    the run terminates in bounded time and reports a distribution.
run_latency loss5 any loss 5%

# 3) 50 ms +- 20 ms jitter: the watchdog must still drive recovery.
echo
echo "=== jitter50_20: injecting 'jitter 50ms 20ms' ==="
"${NETEM}" jitter 50ms 20ms || { note "FAIL jitter50_20: injection failed"; FAILED=1; }
out_recovery="${OUT_DIR}/recovery_jitter50_20.txt"
# Each restart recovery costs one watchdog interval + connect; with 50 ms +/-
# 20 ms jitter the old 10/5 cycle count routinely exceeded 300 s and produced
# no restart_recovery_ms line.
timeout 600 "${BIN_DIR}/bench_recovery" --api-cycles 5 --restart-cycles 2 \
  --port "${RECOVERY_PORT}" >"${out_recovery}" 2>&1
rc=$?
"${NETEM}" clear >/dev/null 2>&1 || true
if [[ ${rc} -eq 124 ]]; then
  note "FAIL jitter50_20: recovery timed out"
  FAILED=1
elif grep -qE 'restart_recovery_ms n=[1-9][0-9]*' "${out_recovery}"; then
  note "PASS jitter50_20: $(grep 'restart_recovery_ms' "${out_recovery}" | tail -1)"
else
  note "FAIL jitter50_20: no restart_recovery_ms line (see ${out_recovery})"
  FAILED=1
fi

# 4) Half-open: blackhole the latency port, prove a call fails within a bounded
#    time (not a hang), then clear and prove the link is usable again.
echo
echo "=== halfopen: blackholing tcp/${PORT} ==="
"${NETEM}" halfopen "${PORT}" || { note "FAIL halfopen: injection failed"; FAILED=1; }
out_half="${OUT_DIR}/halfopen_bounded.txt"
start_ms=$(date +%s%3N)
timeout 60 "${BIN_DIR}/bench_latency" --threads 1 --ops 20 --batch 1 --port "${PORT}" \
  >"${out_half}" 2>&1
rc=$?
end_ms=$(date +%s%3N)
elapsed_ms=$((end_ms - start_ms))
"${NETEM}" clear >/dev/null 2>&1 || true

if [[ ${rc} -eq 124 ]]; then
  note "FAIL halfopen: call did not return within 60s (unbounded)"
  FAILED=1
elif [[ ${rc} -eq 0 ]] && grep -q 'errors=0' "${out_half}"; then
  note "FAIL halfopen: calls succeeded on the blackholed port; the fault was not applied"
  FAILED=1
else
  note "PASS halfopen: bounded failure in ${elapsed_ms}ms (rc=${rc}), link cleared"
fi

# 5) Post-clear smoke: the same binary must be healthy again on the clean link.
out_clean="${OUT_DIR}/after_clear_latency.txt"
timeout 120 "${BIN_DIR}/bench_latency" --threads 2 --ops 200 --batch 4 \
  >"${out_clean}" 2>&1
rc=$?
if [[ ${rc} -eq 0 ]] && grep -q 'errors=0' "${out_clean}"; then
  note "PASS after_clear: link usable again (errors=0)"
else
  note "FAIL after_clear: rc=${rc} (see ${out_clean})"
  FAILED=1
fi

echo
echo "================ netem matrix summary ================"
for line in "${SUMMARY[@]}"; do echo "  ${line}"; done
printf '%s\n' "${SUMMARY[@]}" > "${OUT_DIR}/summary.txt"
echo "raw outputs: ${OUT_DIR}/ (summary: ${OUT_DIR}/summary.txt)"
if [[ ${FAILED} -ne 0 ]]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS"
