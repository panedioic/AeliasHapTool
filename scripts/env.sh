#!/bin/bash
# env.sh — AeliasHapTool 构建公共环境（被所有构建脚本 source，不要直接执行）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ROOT

# 构建期资产就在仓根：
#   compat/   宿主侧替身实现（securec / host_compat_prefix.h / ...）
#   patches/  打在上游源码副本上的补丁
#   thirdparty/  上游 git clone（只读）+ 随仓的第三方源码（cJSON/minizip）
SPEC="$ROOT"
export SPEC

# 产物目录：build/out/<platform>/{haptool,libhdc.so,...}
BUILD="$ROOT/build"
OUT="$BUILD/out/${PLATFORM:-linux}"
export BUILD OUT

# 上游（**只读参考**；src 阶段 clone 到这里，build 阶段整棵拷进 $BUILD/<tool>/src 再打补丁）
THIRD_PARTY="$ROOT/thirdparty"
export THIRD_PARTY
UP_HAPSIGNER="$ROOT/thirdparty/developtools_hapsigner"
UP_PACKING="$ROOT/thirdparty/developtools_packing_tool"
UP_HDC="$ROOT/thirdparty/developtools_hdc"
GITCODE_HAPSIGNER="${GITCODE_HAPSIGNER:-https://gitcode.com/openharmony/developtools_hapsigner.git}"
GITCODE_PACKING="${GITCODE_PACKING:-https://gitcode.com/openharmony/developtools_packing_tool.git}"
GITCODE_HDC="${GITCODE_HDC:-https://gitcode.com/openharmony/developtools_hdc.git}"

# 交叉工具链（与 build/hapsigntool_cpp/build-aarch64.sh、build_packing_tool.sh 同一套）
TC="${MUSL_TOOLCHAIN:-/workspace/dev/elfloader/ElfLoaderDemo/tools/aarch64-linux-musl-cross}"
CROSS="$TC/bin/aarch64-linux-musl-"

# OHOS SDK（DevEco command-line-tools 布局；hdc/haptool 动态 musl 目标用它编）
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export OHOS_SDK="${OHOS_SDK:-$TOOL_HOME/sdk/default/openharmony}"
CLANG_DIR="$OHOS_SDK/native/llvm/bin"
export CLANG="$CLANG_DIR/clang"
export CLANGXX="$CLANG_DIR/clang++"
export OHOS_SYSROOT="$OHOS_SDK/native/sysroot"
export OHOS_TARGET=aarch64-linux-ohos

# cmake：系统没有就用 SDK 自带的（third_party 源码树不少只有 CMakeLists.txt）
CMAKE="$(command -v cmake 2>/dev/null || true)"
[ -z "$CMAKE" ] && [ -x "$OHOS_SDK/native/build-tools/cmake/bin/cmake" ] \
    && CMAKE="$OHOS_SDK/native/build-tools/cmake/bin/cmake"
export CMAKE

# openssl/zlib 静态库安装根：默认复用 hapsigntool_cpp 编好的那份（同工具链同 ABI）
PREFIX="${OPENSSL_PREFIX:-$ROOT/build/hapsigntool_cpp/aarch64/sysroot}"
OPENSSL_VERSION="3.0.13"
OPENSSL_SRC="${OPENSSL_SRC:-$BUILD/openssl-$OPENSSL_VERSION}"
OPENSSL_TARBALL="$BUILD/openssl-$OPENSSL_VERSION.tar.gz"
OPENSSL_URL="${OPENSSL_URL:-https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz}"
ZLIB_SRC="${ZLIB_SRC:-$ROOT/thirdparty/zlib}"

# 并行度
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export JOBS

log()  { echo -e "\033[32m[haptool]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[haptool]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[haptool]\033[0m ERROR: $*" >&2; exit 1; }

# ══════════════════════════════════════════════════════════════════
# 公用函数
# ══════════════════════════════════════════════════════════════════

# clone 上游仓（幂等；不改远端、不 pull）  $1=url $2=目标目录 $3=显示名
clone_upstream() {
    if [ -d "$2/.git" ]; then
        log "src：已有 $3（$2），跳过"
        return 0
    fi
    [ -d "$2" ] && err "src：$2 存在但不是 git 仓 —— 先移走它再重跑"
    log "src：git clone --depth 1 $1 → $2"
    mkdir -p "$(dirname "$2")"
    git clone --depth 1 "$1" "$2" || err "src：clone $3 失败（网络/代理？）"
}

# 源码副本：上游只读，整棵拷到 $BUILD 下再打补丁  $1=上游子树 $2=副本目录 $3=显示名
prepare_src() {
    [ -d "$1" ] || err "缺上游源码 $1（先跑 src 阶段）"
    rm -rf "$2"
    mkdir -p "$2"
    cp -a "$1/." "$2/"
    log "build：源码副本 $3 → $2"
}

# 打补丁：正向能打就打、已打过就跳过、两边都不通就报错  $1=副本目录
apply_patches() {
    local dir="$1" p
    shopt -s nullglob
    for p in "$SPEC"/patches/*.patch; do
        if patch -p1 -d "$dir" --dry-run --silent < "$p" >/dev/null 2>&1; then
            log "build：打补丁 $(basename "$p")"
            patch -p1 -d "$dir" < "$p" >/dev/null
        elif patch -p1 -R -d "$dir" --dry-run --silent < "$p" >/dev/null 2>&1; then
            log "build：已是打过补丁的状态，跳过 $(basename "$p")"
        else
            shopt -u nullglob
            err "build：补丁既不能正向也不能反向应用（上游代码变过？）：$p"
        fi
    done
    shopt -u nullglob
}

# 逐 TU 并行编译 + 失败汇总；结果放进全局 OBJS，链接由调用方做。
# 读全局 NAMES（"cc|cxx:path"）、INCLUDES、CC/CXX、CFLAGS/CXXFLAGS。
compile_tus() {   # $1=objdir $2=logdir
    local objdir="$1" logdir="$2"
    rm -rf "$logdir"
    mkdir -p "$objdir" "$logdir"
    OBJS=()
    local i=0 spec lang f obj
    for spec in "${NAMES[@]}"; do
        lang="${spec%%:*}"
        f="${spec#*:}"
        obj="$objdir/$(printf '%03d' "$i")_$(basename "$f" | tr -c 'A-Za-z0-9._-' '_').o"
        OBJS+=("$obj")
        if [ "$lang" = "cc" ]; then
            ( "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$f" -o "$obj" \
                >"$logdir/$(printf '%03d' "$i").log" 2>&1 ) &
        else
            ( "$CXX" "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$f" -o "$obj" \
                >"$logdir/$(printf '%03d' "$i").log" 2>&1 ) &
        fi
        i=$((i + 1))
        while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done
    done
    wait || true

    local failed=0 j=0
    for spec in "${NAMES[@]}"; do
        if [ -s "$logdir/$(printf '%03d' "$j").log" ]; then
            echo "=== 编译失败：${spec#*:}" >&2
            cat "$logdir/$(printf '%03d' "$j").log" >&2
            failed=$((failed + 1))
        fi
        j=$((j + 1))
    done
    [ "$failed" -eq 0 ] || err "$failed 个 TU 编译失败（日志在 $logdir）"
}
