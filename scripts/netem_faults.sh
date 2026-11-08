#!/usr/bin/env bash
# scripts/netem_faults.sh -- apply a Linux link-fault profile to loopback so the
# client talks over a degraded path, then run a benchmark against it.
#
# Requires root (tc / iptables). The client and the in-process server share
# loopback, so affecting "lo" affects exactly the connection under test.
#
#   sudo ./scripts/netem_faults.sh delay 200ms
#   sudo ./scripts/netem_faults.sh loss 5%
#   sudo ./scripts/netem_faults.sh jitter 50ms 20ms
#   sudo ./scripts/netem_faults.sh halfopen 4890     # silently drop the port
#   sudo ./scripts/netem_faults.sh show
#   sudo ./scripts/netem_faults.sh clear
#
# Or run one preset and restore automatically:
#   sudo ./scripts/netem_faults.sh with loss 5% -- \
#        ./build-bench/bench-build/bench_latency --threads 8
set -euo pipefail

DEV="${DEV:-lo}"
PORT="${PORT:-4890}"
# Remembers which port `halfopen` blackholed so `clear` removes exactly that
# rule. Without this, `halfopen 4891` followed by `clear` left the DROP rule on
# 4891 behind because clear always used the default PORT.
STATE_FILE="${TMPDIR:-/tmp}/opcua_netem_faults_port"

clear_all() {
  tc qdisc del dev "${DEV}" root 2>/dev/null || true
  local p="${PORT}"
  if [[ -f "${STATE_FILE}" ]]; then
    p="$(cat "${STATE_FILE}")"
    rm -f "${STATE_FILE}"
  fi
  iptables -D INPUT -p tcp --dport "${p}" -j DROP 2>/dev/null || true
}

usage() {
  sed -n '2,20p' "$0"
  exit 2
}

case "${1:-}" in
  clear)
    cleared_port="${PORT}"
    [[ -f "${STATE_FILE}" ]] && cleared_port="$(cat "${STATE_FILE}")"
    clear_all
    echo "cleared faults on ${DEV} (port ${cleared_port})"
    ;;
  delay)
    clear_all
    tc qdisc replace dev "${DEV}" root netem delay "${2:-200ms}"
    echo "netem delay ${2:-200ms} on ${DEV}"
    ;;
  loss)
    clear_all
    tc qdisc replace dev "${DEV}" root netem loss "${2:-5%}"
    echo "netem loss ${2:-5%} on ${DEV}"
    ;;
  jitter)
    clear_all
    tc qdisc replace dev "${DEV}" root netem delay "${2:-200ms}" "${3:-50ms}" distribution normal
    echo "netem delay ${2:-200ms} jitter ${3:-50ms} on ${DEV}"
    ;;
  halfopen)
    # Blackhole inbound SYNs/data for the port: the TCP connection is neither
    # reset nor closed, which is the half-open case the recovery path must survive.
    PORT="${2:-${PORT}}"
    clear_all
    printf '%s' "${PORT}" > "${STATE_FILE}"
    iptables -A INPUT -p tcp --dport "${PORT}" -j DROP
    echo "half-open blackhole on tcp/${PORT} (iptables DROP)"
    ;;
  show)
    tc qdisc show dev "${DEV}"
    iptables -L INPUT -n --line-numbers | head -20
    ;;
  with)
    # with <preset> [preset-args...] -- <command...>
    # Applies the preset, runs the command, then restores the link on exit.
    shift
    preset_args=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do preset_args+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    [[ $# -gt 0 ]] || usage
    trap clear_all EXIT
    "$0" "${preset_args[@]}"
    "$@"
    ;;
  *)
    usage
    ;;
esac
