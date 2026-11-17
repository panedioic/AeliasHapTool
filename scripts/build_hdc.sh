#!/bin/bash
# build_hdc.sh — 把 OHOS 的 **hdc**（developtools_hdc）host 侧源码编成 libhdc.so，
# 供 haptool（scripts/build.sh）链接：连接/安装/卸载/打开 等 hdc 能力全部进程内调用，
# 不再依赖外部 hdc 可执行文件。
#
# ── 阶段 ────────────────────────────────────────────────────────────────
#   src    git clone 上游到 thirdparty/developtools_hdc（已存在则跳过；不改远端、不 pull）
#   build  源码副本 → 打补丁 → host+common 全部 TU → $OUT/libhdc.so
#   all    src + build
#
# ── 平台（PLATFORM 环境变量，默认 linux）───────────────────────────────
#   linux  宿主 g++（x86_64，动态链接，跑在构建机上）
#   ohos   OHOS SDK clang --target=aarch64-linux-ohos（动态 musl，跑在设备/沙箱里）
#
# 用法：PLATFORM=linux ./build_hdc.sh [src|build|all]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="${PLATFORM:-linux}"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

PHASE="${1:-all}"

HDC_WORK="$BUILD/hdc/$PLATFORM/src"      # 上游源码副本（打补丁在这份上）
HDC_OBJ="$BUILD/hdc/$PLATFORM/obj"
HDC_LOGS="$BUILD/hdc/$PLATFORM/logs"
OUT_SO="$OUT/libhdc.so"

do_src() {
    clone_upstream "$GITCODE_HDC" "$UP_HDC" "developtools_hdc"
}

select_toolchain() {
    case "$PLATFORM" in
        linux)
            command -v g++ >/dev/null 2>&1 || err "linux：缺 g++"
            HDC_CC=gcc
            HDC_CXX=g++
            HDC_CFLAGS=(-fPIC -O2 -g0 -w)
            HDC_CXXFLAGS=(-std=c++17 -fPIC -O2 -g0 -w)
            HDC_LDFLAGS=(-shared -fPIC)
            HDC_LIBS=(-lpthread -ldl)
            ;;
        ohos)
            [ -x "$CLANGXX" ] || err "ohos：缺 SDK clang++：$CLANGXX"
            [ -d "$OHOS_SYSROOT" ] || err "ohos：缺 SDK sysroot：$OHOS_SYSROOT"
            HDC_CC="$CLANG"
            HDC_CXX="$CLANGXX"
            # -U_FORTIFY_SOURCE：OHOS 头把 FD_SET 展开成 vanilla musl 里没有的 __fd_chk。
            # __OHOS__ 不在这里 -U（libc++ 头依赖它），由 -include 的 host_compat_prefix.h
            #   在标准库头解析完之后 #undef —— hdc 源码走纯 TCP host 分支，与 linux 同口径
            HDC_CFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" \
                        -fPIC -O2 -g0 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
            HDC_CXXFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" -std=c++17 \
                          -fPIC -O2 -g0 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
                          -Wno-c++11-narrowing)
            HDC_LDFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" -shared -fPIC \
                         -Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1 -Wl,-z,now)
            HDC_LIBS=(-lpthread -ldl)
            ;;
        *) err "未知 PLATFORM：$PLATFORM（可用：linux|ohos）" ;;
    esac
}

do_build() {
    [ -d "$UP_HDC/src" ] || err "hdc：缺上游源码 $UP_HDC（先跑 src 阶段）"
    [ -f "$BUILD/deps-$PLATFORM/lib/libcrypto.a" ] || err "hdc：缺依赖（先跑 ./build_deps.sh build）"
    select_toolchain

    # 源码副本（上游只读；将来打补丁也在这份上）
    prepare_src "$UP_HDC/src" "$HDC_WORK" "developtools_hdc/src"
    local work="$HDC_WORK"

    # 编译清单：口径照上游 src/host/CMakeLists.txt —— host/*.cpp + common/*.cpp
    # 全编（AUX_SOURCE_DIRECTORY）。差异只有一处：上游 main.cpp 不编（so 里不能有
    # main），入口由 compat/hdc/hdc_entry.cpp 的 HdcLibMain 替代（main() 的原样副本）。
    local f
    NAMES=()
    for f in "$work"/host/*.cpp; do
        [ "$(basename "$f")" = "main.cpp" ] && continue
        NAMES+=("cxx:$f")
    done
    for f in "$work"/host/subserver/*.cpp; do
        case "$f" in *_win.cpp) continue ;; esac
        NAMES+=("cxx:$f")
    done
    for f in "$work"/common/*.cpp; do
        NAMES+=("cxx:$f")
    done
    # 宿主侧替身：securec / HUKS stub / HdcLibMain 入口
    NAMES+=("cxx:$SPEC/compat/hdc/securec.cpp")
    NAMES+=("cxx:$SPEC/compat/hdc/hks_stub.cpp")
    NAMES+=("cxx:$SPEC/compat/hdc/hdc_entry.cpp")

    INCLUDES=(
        "-I$work/common" "-I$work/host"
        "-I$SPEC/compat/hdc"          # securec.h + singleton.h + hks_*.h + hdc_entry.h
        "-I$BUILD/deps-$PLATFORM/include"   # openssl / lz4 / libusb（桥接的 <libusb/libusb.h>）
    )
    local LIBUV_LINK="$BUILD/deps-$PLATFORM/lib/libuv.a"   # 显式 .a：cmake 会同时装 .so
    case "$PLATFORM" in
        linux)
            INCLUDES+=("-I$BUILD/deps-$PLATFORM/include/libusb-1.0")  # 裸写法 <libusb.h>
            ;;
        ohos)
            # libuv 直链 SDK sysroot 的 libuv.so；libusb 走 compat stub（无 USB host 场景）
            INCLUDES+=("-I$BUILD/deps-$PLATFORM/include/libusb")
            NAMES+=("cxx:$SPEC/compat/hdc/libusb_stub.cpp")
            LIBUV_LINK="-luv"
            ;;
    esac

    # 宏口径照上游 src/host/CMakeLists.txt 的 add_definitions：
    #   HDC_HOST/HOST_LINUX 平台、HARMONY_PROJECT 关掉 ut_command、
    #   TEST_HASH/HDC_MSG_HASH 与上游 CI 一致（不影响功能）。
    # -include host_compat_prefix.h：上游几个头依赖 OHOS 工具链传递包含图才有的
    #   <vector>/<cstdlib> 等，宿主 g++ 的图谱里没有；只补声明不补行为。
    local HDC_DEFS=(-DUSE_CONFIG_UV_THREADS -DSIZE_THREAD_POOL=128 -DHDC_HOST -DHOST_LINUX
                    -DHARMONY_PROJECT -DTEST_HASH '-DHDC_MSG_HASH="TEST"')
    CFLAGS=("${HDC_CFLAGS[@]}" "${HDC_DEFS[@]}")
    CXXFLAGS=("${HDC_CXXFLAGS[@]}" "${HDC_DEFS[@]}"
              -include "$SPEC/compat/hdc/host_compat_prefix.h")
    CC="$HDC_CC"
    CXX="$HDC_CXX"

    log "hdc：编 libhdc.so（${#NAMES[@]} 个 TU）→ $OUT_SO"
    mkdir -p "$OUT"
    compile_tus "$HDC_OBJ" "$HDC_LOGS"

    log "hdc：链接 $OUT_SO"
    # linux：libuv 显式传 .a（deps 目录里 cmake 同时装了 .a/.so，-luv 会链动态的），
    # libusb 链真静态库；ohos：libuv 用 sysroot 的 .so，libusb 由 stub TU 提供
    local -a LINK_EXTRA=(-lssl -lcrypto -llz4 "$LIBUV_LINK")
    [ "$PLATFORM" = "linux" ] && LINK_EXTRA+=(-lusb-1.0)
    "$HDC_CXX" "${HDC_LDFLAGS[@]}" -o "$OUT_SO" "${OBJS[@]}" \
        -L"$BUILD/deps-$PLATFORM/lib" \
        "${LINK_EXTRA[@]}" \
        "${HDC_LIBS[@]}"
    log "hdc：$(du -h "$OUT_SO" | cut -f1)  $OUT_SO"
}

case "$PHASE" in
    src)   do_src ;;
    build) do_build ;;
    all)   do_src; do_build ;;
    *)     err "未知阶段：$PHASE（可用：src|build|all）" ;;
esac
