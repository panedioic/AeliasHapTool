#!/bin/bash
# build.sh — 编 haptool 主程序，链接 libhdc.so、libhapsigner.so 与静态 openssl/zlib。
#
# 源码组成：
#   src/main.cpp         命令层
#   src/aht_http.cpp     HTTPS 客户端（OpenSSL 手写，华为开发者接口用）
#   src/aht_cloud.cpp    登录回调 + 云端证书/设备/Profile
#   src/aht_hap.cpp      HAP 元信息（minizip + cJSON）
#   src/aht_store.cpp    AHT_HOME 材料管理
#   src/aht_sign.cpp     材料准备与本地签名
#   thirdparty/cJSON.c   JSON
#   thirdparty/contrib/minizip/{unzip,ioapi}.c   zip 读取
#
# ── 阶段 ────────────────────────────────────────────────────────────────
#   tool   编 $OUT/haptool（默认；等价 all）
#   all    tool
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

PHASE="${1:-all}"

OUT_TOOL="$OUT/haptool"
OBJDIR="$BUILD/tool-$PLATFORM"
DEPS_INC="$BUILD/deps-$PLATFORM/include"
DEPS_LIB="$BUILD/deps-$PLATFORM/lib"

select_toolchain() {
    case "$PLATFORM" in
        linux)
            command -v g++ >/dev/null 2>&1 || err "linux：缺 g++"
            TOOL_CC=gcc
            TOOL_CXX=g++
            TOOL_CFLAGS=(-O2 -g0 -w)
            TOOL_CXXFLAGS=(-std=c++17 -O2 -g0 -Wall -Wextra)
            TOOL_LDFLAGS=(-Wl,-rpath,'$ORIGIN')
            ;;
        ohos)
            [ -x "$CLANGXX" ] || err "ohos：缺 SDK clang++：$CLANGXX"
            TOOL_CC="$CLANG"
            TOOL_CXX="$CLANGXX"
            TOOL_CFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" \
                         -O2 -g0 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
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
    [ -f "$OUT/libhdc.so" ] || err "tool：缺 $OUT/libhdc.so（先跑 ./build_hdc.sh build）"
    [ -f "$OUT/libhapsigner.so" ] || err "tool：缺 $OUT/libhapsigner.so（先跑 ./build_hapsigner.sh build）"
    [ -f "$DEPS_LIB/libssl.a" ] || err "tool：缺静态 openssl（先跑 ./build_deps.sh build）"
    mkdir -p "$OBJDIR"

    local INC=(-I"$SPEC/compat/hdc" -I"$SPEC/compat/hapsigntool"
               -I"$SPEC/thirdparty" -I"$SPEC/thirdparty/contrib/minizip"
               -I"$ROOT/src" -I"$DEPS_INC")

    log "tool：编 C 源（cJSON + minizip）"
    local csrc=("$SPEC/thirdparty/cJSON.c"
                "$SPEC/thirdparty/contrib/minizip/unzip.c"
                "$SPEC/thirdparty/contrib/minizip/ioapi.c")
    local objs=() f o

    for f in "${csrc[@]}"; do
        o="$OBJDIR/$(basename "$f" .c).o"
        "$TOOL_CC" "${TOOL_CFLAGS[@]}" "${INC[@]}" -c "$f" -o "$o"
        objs+=("$o")
    done

    log "tool：编 C++ 源"
    local cxxsrc=("$ROOT/src/main.cpp" "$ROOT/src/aht_http.cpp" "$ROOT/src/aht_cloud.cpp"
                  "$ROOT/src/aht_hap.cpp" "$ROOT/src/aht_store.cpp" "$ROOT/src/aht_sign.cpp")

    for f in "${cxxsrc[@]}"; do
        o="$OBJDIR/$(basename "$f" .cpp).o"
        "$TOOL_CXX" "${TOOL_CXXFLAGS[@]}" "${INC[@]}" -c "$f" -o "$o"
        objs+=("$o")
    done

    log "tool：链接 $OUT_TOOL（libhdc.so + libhapsigner.so + 静态 openssl/zlib）"
    "$TOOL_CXX" "${objs[@]}" \
        "$OUT/libhdc.so" "$OUT/libhapsigner.so" \
        -L"$DEPS_LIB" -lssl -lcrypto -lz \
        "${TOOL_LDFLAGS[@]}" -lpthread -ldl -o "$OUT_TOOL"

    log "tool：$(du -h "$OUT_TOOL" | cut -f1)  $OUT_TOOL（rpath=\$ORIGIN，与两个 .so 同目录部署）"
}

case "$PHASE" in
    tool|all) do_tool ;;
    *)        err "未知阶段：$PHASE（可用：tool|all）" ;;
esac
