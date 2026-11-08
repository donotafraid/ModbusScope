#!/usr/bin/env bash
# ============================================================
# scripts/build_open62541.sh
# ------------------------------------------------------------
# 在 CI 或本地克隆 + 构建 open62541 v1.4.14。
#
# 用法：
#   bash scripts/build_open62541.sh [目标目录]
#   bash scripts/build_open62541.sh third_party/open62541-v1.4.14
#
# 编译器：
#   优先使用环境变量 CC / CXX；
#   未设置时默认 clang-17 / clang++-17（与项目主线一致）。
#   如需用 GCC 构建：CC=gcc CXX=g++ bash scripts/build_open62541.sh ...
#
# 默认目标目录：third_party/open62541-v1.4.14
#
# 产出：
#   <目标目录>/build/bin/libopen62541.so
#   <目标目录>/build/src_generated/
#   <目标目录>/include/
#   <目标目录>/plugins/include/
#
# 幂等：如果 .so 已存在，直接退出 0（CI 缓存命中路径）。
# ============================================================
set -euo pipefail

# ---------- 参数 ----------
DEST="${1:-third_party/open62541-v1.4.14}"
VERSION="${OPEN62541_VERSION:-v1.4.14}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
CC_BIN="${CC:-clang-17}"
CXX_BIN="${CXX:-clang++-17}"

DEST="$(mkdir -p "$(dirname "$DEST")" && cd "$(dirname "$DEST")" && pwd)/$(basename "$DEST")"

echo "=== open62541 构建脚本 ==="
echo "版本     : $VERSION"
echo "目标目录 : $DEST"
echo "并行度   : $JOBS"
echo "CC       : $CC_BIN"
echo "CXX      : $CXX_BIN"
echo "==========================="

# ---------- 幂等检查 ----------
SO_PATH="${DEST}/build/bin/libopen62541.so"
if [[ -f "${SO_PATH}" ]]; then
    echo "[跳过] ${SO_PATH} 已存在，无需重新构建。"
    exit 0
fi

# ---------- 1. 克隆源码 ----------
if [[ ! -d "${DEST}/.git" ]]; then
    echo "[1/4] 克隆 open62541 ${VERSION} → ${DEST}"
    mkdir -p "$(dirname "${DEST}")"
    git clone --depth 1 --branch "${VERSION}" \
        https://github.com/open62541/open62541.git "${DEST}"
else
    echo "[1/4] ${DEST} 已存在，跳过克隆。"
fi

# ---------- 2. 配置 ----------
echo "[2/4] 配置 CMake"
cmake -S "${DEST}" -B "${DEST}/build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="${CC_BIN}" \
    -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
    -DUA_MULTITHREADING=100 \
    -DUA_ENABLE_AMALGAMATION=OFF \
    -DUA_BUILD_EXAMPLES=OFF \
    -DUA_BUILD_UNIT_TESTS=OFF \
    -DUA_ENABLE_ENCRYPTION=OFF \
    -DBUILD_SHARED_LIBS=ON

# ---------- 3. 构建 ----------
echo "[3/4] 构建（并行 ${JOBS}）"
cmake --build "${DEST}/build" -j "${JOBS}"

# ---------- 4. 验证产出 ----------
echo "[4/4] 验证产出"
if [[ ! -f "${SO_PATH}" ]]; then
    echo "错误：未找到 ${SO_PATH}"
    echo "搜索实际位置："
    find "${DEST}/build" -name "libopen62541*" || true
    exit 1
fi

echo ""
echo "=== 构建成功 ==="
echo "  .so      : ${SO_PATH}"
echo "  include  : ${DEST}/include"
echo "  generated: ${DEST}/build/src_generated"
echo "  plugins  : ${DEST}/plugins/include"
echo ""
echo "下一步："
echo "  cmake -B build-test -DCMAKE_BUILD_TYPE=Debug \\"
echo "        -DOPEN62541_ROOT=${DEST}"