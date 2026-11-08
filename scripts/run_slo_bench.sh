#!/usr/bin/env bash
# scripts/run_slo_bench.sh -- SLO 基准：独立 server、绑核、RUNS 次中位数、归档
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT=$(pwd)

# ---- bench 构建树：单一事实来源 ----
# 配置目录：含 CMakeCache.txt / compile_commands.json
BENCH_BUILD_DIR=${BENCH_BUILD_DIR:-$ROOT/build-bench}
# 二进制目录：CMAKE_RUNTIME_OUTPUT_DIRECTORY 指向的子目录
BENCH_BIN_DIR=${BENCH_BIN_DIR:-$BENCH_BUILD_DIR/bench-build}

BIN_LAT=${BIN_LAT:-$BENCH_BIN_DIR/bench_latency}
BIN_SRV=${BIN_SRV:-$BENCH_BIN_DIR/bench_server}

STAMP=$(date +%Y%m%d_%H%M%S)
OUT=bench-results/${STAMP}_slo
mkdir -p "$OUT"

RUNS=${RUNS:-15}
OPS=${OPS:-100000}
BATCH=${BATCH:-16}
SRV_PORT=${SRV_PORT:-4892}
# 4268 / 2864 是历史固定负载，不是“当前容量的 80%”。本机 8T 共享容量约
# 7~8k calls/s，4268 只约 53~61%；名字 RATE_80 仅为兼容已有归档脚本。
# 换机器/换构建后必须重新标定，不要把这两个值当成线性扩展承诺。
RATE_80=${RATE_80:-4268}           # historical fixed load, not 80% of 7032
RATE_80_PTC=${RATE_80_PTC:-2864}   # historical fixed load, not 80% of 21600

# ---- 绑核：server 独占 cpu7，client 用 0/2/4/6（4 个独立物理核）----
SRV_CPUS=${SRV_CPUS:-7}
C1_CPUS=${C1_CPUS:-0}
C4_CPUS=${C4_CPUS:-0,2,4,6}
C8_CPUS=${C8_CPUS:-0-7}

# ---- 构建树缺失时自动 configure + build（与 run_bench.sh 行为对齐）----
if [ ! -f "$BENCH_BUILD_DIR/CMakeCache.txt" ]; then
  echo "build tree missing: $BENCH_BUILD_DIR; configuring..." >&2
  CONAN_TOOLCHAIN="${CONAN_TOOLCHAIN:-$ROOT/conan_bench_build/conan_toolchain.cmake}"
  OPEN62541_ROOT="${OPEN62541_ROOT:-$ROOT/third_party/open62541-v1.4.14}"
  CMAKE_ARGS=(-S "$ROOT" -B "$BENCH_BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo)
  if [ -f "$CONAN_TOOLCHAIN" ]; then
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$CONAN_TOOLCHAIN")
    CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$(dirname "$CONAN_TOOLCHAIN")")
  fi
  if [ -n "${OPEN62541_ROOT:-}" ]; then
    CMAKE_ARGS+=(-DOPEN62541_ROOT="$OPEN62541_ROOT")
  fi
  CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CC:-clang-17}")
  CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="${CXX:-clang++-17}")
  cmake "${CMAKE_ARGS[@]}"
  cmake --build "$BENCH_BUILD_DIR" --target bench_latency bench_server -j
fi

# ---- 环境归档 ----
{
  date -Is
  echo "--- git ---"; git rev-parse HEAD 2>/dev/null || echo "no git"
  git status --short 2>/dev/null || true
  echo "--- cpu ---"; lscpu | grep -E "Model name|Core|Thread|NUMA"
  grep "cpu MHz" /proc/cpuinfo | head -8
  echo "--- bin vs src ---"
  ls -l --time-style=full-iso "$BIN_LAT" "$BIN_SRV" src/OPCUAPacking/ua.cpp
  echo "--- build ---"
  grep -E '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS):' \
      "$BENCH_BUILD_DIR/CMakeCache.txt" 2>/dev/null || true
  clang++-17 --version 2>/dev/null | head -1 || true
  grep -m1 -o -- '-O[0-9s]' "$BENCH_BUILD_DIR/compile_commands.json" 2>/dev/null || true
  grep -m1 -o -- '-O[0-9s]' build-bench/compile_commands.json 2>/dev/null || true
  echo "--- source hash ---"
  sha256sum src/OPCUAPacking/ua.cpp include/OPCUAPacking/*.h \
      include/Rust_error_deal/*.h src/OPCUAPacking/detail/*.h 2>/dev/null || true
  echo "--- binary hash ---"
  sha256sum "$BIN_LAT" "$BIN_SRV" 2>/dev/null || true
  echo "--- cgroup/load ---"
  cat /sys/fs/cgroup/cpu.max 2>/dev/null || true
  grep -E 'nr_throttled|throttled_usec' /sys/fs/cgroup/cpu.stat 2>/dev/null || true
  cat /proc/loadavg
  echo "--- dirty src/include ---"
  git diff --stat -- src include 2>/dev/null || true
  echo "--- params ---"
  echo "RUNS=$RUNS OPS=$OPS BATCH=$BATCH SRV_CPUS=$SRV_CPUS RATE_80=$RATE_80"
  echo "BENCH_PROFILE=PROFILE_LOCAL watchdogIntervalMs=1000 batch=$BATCH"
} > "$OUT/env.txt" 2>&1

# ---- 前置校验：构建树与二进制必须存在 ----
if [ ! -f "$BENCH_BUILD_DIR/CMakeCache.txt" ]; then
  echo "error: $BENCH_BUILD_DIR/CMakeCache.txt not found;" >&2
  echo "       BENCH_BUILD_DIR=$BENCH_BUILD_DIR" >&2
  echo "       configure first: cmake -S . -B \"$BENCH_BUILD_DIR\" -DCMAKE_BUILD_TYPE=RelWithDebInfo ..." >&2
  exit 2
fi
for BIN in "$BIN_LAT" "$BIN_SRV"; do
  if [ ! -x "$BIN" ]; then
    echo "error: bench binary missing or not executable: $BIN" >&2
    echo "       BENCH_BIN_DIR=$BENCH_BIN_DIR" >&2
    exit 2
  fi
done

# ---- 构建类型门禁：bench 数据必须来自优化构建 ----
# Debug(-O0) 的延迟/吞吐与发布构建差数倍，不能作为 SLO 证据。
BUILD_TYPE=$(grep -m1 '^CMAKE_BUILD_TYPE:' "$BENCH_BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
case "$BUILD_TYPE" in
  Release|RelWithDebInfo|MinSizeRel) ;;
  *)
    echo "error: bench build type is '$BUILD_TYPE'; expected Release/RelWithDebInfo/MinSizeRel." >&2
    echo "       -O0 Debug binaries produce non-representative results." >&2
    exit 6
    ;;
esac
# 二次确认实际编译命令没有 -O0（防止 cache 与实际 flags 不一致）
if grep -q -- '-O0' "$BENCH_BUILD_DIR/compile_commands.json" 2>/dev/null; then
  echo "error: compile_commands.json contains -O0; bench binaries are unoptimized." >&2
  exit 6
fi


# ---- 证据门禁：binary 必须不旧于源码 ----
# 2026-10-28 23:24 的归档就是因为 binary(13:38) 早于 ua.cpp(23:11)，结果
# 被误当成当前源码的证据。这里在跑之前直接拒绝，避免再产生不可审计的归档。
if ! git diff --quiet -- src include 2>/dev/null; then
  echo "warning: src/include 有未提交改动；env.txt 已记录 diff --stat 和源码哈希，" >&2
  echo "        但该归档不能仅凭 HEAD 复现。发布证据前先 commit 再重跑。" >&2
fi
for BIN in "$BIN_LAT" "$BIN_SRV"; do
  STALE_SRC=$(find src include -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
      -newer "$BIN" -print -quit 2>/dev/null || true)
  if [ -n "$STALE_SRC" ]; then
    echo "error: $BIN is older than source: $STALE_SRC" >&2
    echo "       rebuild (cmake --build \"$BENCH_BUILD_DIR\" --target bench_latency bench_server -j)" >&2
    echo "       or set BIN_LAT/BIN_SRV to a fresh build" >&2
    exit 4
  fi
done

# ---- 启动独立 server ----
taskset -c "$SRV_CPUS" "$BIN_SRV" --port "$SRV_PORT" > "$OUT/server.log" 2>&1 &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
  grep -q '^READY ' "$OUT/server.log" 2>/dev/null && break
  sleep 0.1
done
grep -q '^READY ' "$OUT/server.log" || { echo "server not ready"; exit 3; }
ENDPOINT="opc.tcp://127.0.0.1:${SRV_PORT}"

# ---- 跑一组：<label> <cpus> <threads> [rate] [extra_flag] ----
run_one() {
  local label=$1 cpus=$2 threads=$3 rate=${4:-0} flag=${5:-}
  local extra=()
  [ "$rate" != "0" ] && extra+=(--rate "$rate")
  [ -n "$flag" ] && extra+=("$flag")
  echo "=== $label: threads=$threads cpus=$cpus rate=${rate} flag=${flag:-none} ==="
  for i in $(seq 1 "$RUNS"); do
    # Keep stdout machine-readable. SDK/client logs go to a separate file; the
    # client log level is pinned to ERROR in bench_latency, so this is normally
    # empty and the 1.6 GB stderr problem cannot recur unnoticed.
    taskset -c "$cpus" "$BIN_LAT" --endpoint "$ENDPOINT" \
      --threads "$threads" --ops "$OPS" --batch "$BATCH" --repeats 1 \
      "${extra[@]}" > "$OUT/${label}_run${i}.txt" \
      2> "$OUT/${label}_run${i}.stderr.log" || true
    echo "  run $i: $(grep -o 'p99=[0-9.]*' "$OUT/${label}_run${i}.txt" || echo 'FAIL')"
  done
}

# ---- 5 组：共享 client 1T/4T/8T + 8T 80% 限速 + 8T 每线程独立 client ----
run_one 1t      "$C1_CPUS" 1
run_one 4t      "$C4_CPUS" 4
run_one 8t      "$C8_CPUS" 8
run_one 8t_r80  "$C8_CPUS" 8 "$RATE_80"
run_one 8t_ptc  "$C8_CPUS" 8 0 --per-thread-client
run_one 8t_ptc_r80 "$C8_CPUS" 8 "$RATE_80_PTC" --per-thread-client

# ---- errors 门禁：任何一次 run 有 errors>0 都不允许进入 median ----
if grep -h 'errors=' "$OUT"/*_run*.txt 2>/dev/null | grep -qv 'errors=0'; then
  echo "error: at least one benchmark run reported errors!=0" >&2
  grep -H 'errors=' "$OUT"/*_run*.txt | grep -v 'errors=0' >&2 || true
  exit 5
fi

# ---- 完整性门禁：每个 label 必须恰好有 RUNS 个可解析的 run ----
# 旧脚本只 grep 现有文件；一次 run 崩溃/漏写时，median 会安静地变成
# “median over 15 runs”其实只有 14 个样本。这里先拒绝缺样本/缺字段的归档。
LABELS=$(ls "$OUT"/*_run1.txt 2>/dev/null | xargs -n1 basename | sed 's/_run1.txt//' | sort)
if [ -z "$LABELS" ]; then
  echo "error: no benchmark run output found under $OUT" >&2
  exit 7
fi
for label in $LABELS; do
  count=$(find "$OUT" -maxdepth 1 -name "${label}_run*.txt" | wc -l)
  if [ "$count" -ne "$RUNS" ]; then
    echo "error: $label has $count run file(s), expected $RUNS" >&2
    exit 7
  fi
  for f in "$OUT/${label}_run"*.txt; do
    if ! grep -q 'calls_s=' "$f" || ! grep -q 'latency_ms p50=' "$f" \
       || ! grep -q 'errors=' "$f"; then
      echo "error: incomplete benchmark output: $f" >&2
      exit 7
    fi
  done
done

# ---- 中位数汇总（动态发现所有 label）----
med() { sort -n | awk '{a[NR]=$1} END{if(NR%2)print a[(NR+1)/2];else print (a[NR/2]+a[NR/2+1])/2}'; }
{
  for label in $LABELS; do
    echo "--- $label (median over $RUNS runs) ---"
    for m in p50 p90 p99 p999 max; do
      echo -n "$m: "
      grep -ho "${m}=[0-9.]*" "$OUT/${label}_run"*.txt | cut -d= -f2 | med
    done
    echo -n "calls_s: "
    grep -ho "calls_s=[0-9.]*" "$OUT/${label}_run"*.txt | cut -d= -f2 | med
    echo -n "errors: "
    grep -ho "errors=[0-9]*" "$OUT/${label}_run"*.txt | cut -d= -f2 |
      awk '{s+=$1} END {print s+0}'
    echo
  done
} | tee "$OUT/median.txt"

# 空 stderr 文件不留在归档里；非空说明有 warning，保留供排查。
find "$OUT" -name '*_run*.stderr.log' -empty -delete 2>/dev/null || true

# ---- bootstrap 95% CI（p99 / p999，所有 label）----
python3 scripts/bootstrap_ci.py "$OUT" | tee "$OUT/ci.txt"

echo "归档: $OUT/"