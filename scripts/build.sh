#!/bin/bash
# build.sh — 编 haptool 主程序（src/main.cpp），链接 build_hdc.sh 产出的 libhdc.so。
# hdc 的 连接/安装/卸载/打开 等能力全部由 libhdc.so 在进程内提供（fork 子进程调
# HdcLibMain，见 src/main.cpp 头注释）。
#
# ── 阶段 ────────────────────────────────────────────────────────────────
#   tool   编 $OUT/haptool（默认；等价 all）
#   all    tool（deps/hdc 由各自脚本负责，这里不重复编）
#
# ── 平台（PLATFORM 环境变量，默认 linux）───────────────────────────────
#   linux  宿主 g++；ohos  SDK clang --target=aarch64-linux-ohos
#
# 用法：PLATFORM=linux ./build.sh [tool|all]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="${PLATFORM:-linux}"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

PHASE="${1:-tool}"

OUT_TOOL="$OUT/haptool"
OUT_SO="$OUT/libhdc.so"

select_toolchain() {
    case "$PLATFORM" in
        linux)
            command -v g++ >/dev/null 2>&1 || err "linux：缺 g++"
            TOOL_CXX=g++
            TOOL_CXXFLAGS=(-std=c++17 -O2 -g0 -Wall -Wextra)
            TOOL_LDFLAGS=(-Wl,-rpath,'$ORIGIN')
            ;;
        ohos)
            [ -x "$CLANGXX" ] || err "ohos：缺 SDK clang++：$CLANGXX"
            TOOL_CXX="$CLANGXX"
            TOOL_CXXFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" \
                           -std=c++17 -O2 -g0 -Wall -Wextra \
                           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
            TOOL_LDFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" \
                          -Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1 -Wl,-z,now \
                          -Wl,-rpath,'$ORIGIN')
            ;;
        *) err "未知 PLATFORM：$PLATFORM（可用：linux|ohos）" ;;
    esac
}

do_tool() {
    select_toolchain
    [ -f "$OUT_SO" ] || err "tool：缺 $OUT_SO（先跑 ./build_hdc.sh build）"
    log "tool：编 haptool → $OUT_TOOL（链接 $OUT_SO）"
    "$TOOL_CXX" "${TOOL_CXXFLAGS[@]}" \
        -I"$SPEC/compat/hdc" \
        "$ROOT/src/main.cpp" "$OUT_SO" \
        "${TOOL_LDFLAGS[@]}" -lpthread -o "$OUT_TOOL"
    log "tool：$(du -h "$OUT_TOOL" | cut -f1)  $OUT_TOOL（rpath=\$ORIGIN，与 libhdc.so 同目录部署）"
}

case "$PHASE" in
    tool|all) do_tool ;;
    *)        err "未知阶段：$PHASE（可用：tool|all）" ;;
esac
