#!/bin/bash
# build_deps.sh — libhdc.so 的第三方依赖（openssl / libuv / lz4 / libusb），
# 全部编成 **静态库**（-fPIC）直接链进 libhdc.so，产物只有一个 so，部署时
# 不用带一堆运行时库。
#
# 为什么不用 apt：容器里没有 sudo，也没有 -dev 头文件；release tarball 自带
# configure，也不需要 autotools。
#
# ── 阶段 ────────────────────────────────────────────────────────────────
#   src    准备源码 tarball 到 thirdparty/（已有则复用，否则 github 下载）
#   build  解压 → 编静态库 → 装到 build/deps-$PLATFORM/{include,lib}
#   all    src + build
#
# ── 平台（PLATFORM 环境变量，默认 linux）───────────────────────────────
#   linux  宿主 gcc（libhdc.so x86_64 版的依赖）
#   ohos   SDK clang --target=aarch64-linux-ohos（待补：见 build_ohos_deps）
#
# 用法：PLATFORM=linux ./build_deps.sh [src|build|all]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="${PLATFORM:-linux}"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

PHASE="${1:-all}"

DEPS="$BUILD/deps-$PLATFORM"           # 安装根：include/ lib/
SRC="$BUILD/deps-$PLATFORM/src"        # 解压后的源码树

# 版本与下载地址（release tarball 自带 configure / Makefile）
OPENSSL_VER="3.0.22"
LIBUV_VER="1.53.0"
LIBUSB_VER="1.0.30"
LZ4_VER="1.9.4"
ZLIB_VER="1.3.1"
OPENSSL_TGZ="$THIRD_PARTY/openssl-$OPENSSL_VER.tar.gz"
LIBUV_TGZ="$THIRD_PARTY/libuv-$LIBUV_VER.tar.gz"
LIBUSB_TGZ="$THIRD_PARTY/libusb-$LIBUSB_VER.tar.bz2"
LZ4_TGZ="$THIRD_PARTY/lz4-$LZ4_VER.tar.gz"
ZLIB_TGZ="$THIRD_PARTY/zlib-$ZLIB_VER.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
# libuv 的 release 不带 tarball 资产，用 github 自动打包的 archive（无 configure，
# 但有 meson.build —— 系统里没有 autotools，有 meson/ninja，走 meson）
LIBUV_URL="https://github.com/libuv/libuv/archive/refs/tags/v$LIBUV_VER.tar.gz"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2"
LZ4_URL="https://github.com/lz4/lz4/releases/download/v$LZ4_VER/lz4-$LZ4_VER.tar.gz"
ZLIB_URL="https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz"

fetch() {   # $1=目标tarball $2=url $3=显示名
    [ -s "$1" ] && { log "src：已有 $(basename "$1")，跳过"; return 0; }
    log "src：下载 $3 → $1"
    curl -fL --retry 3 -o "$1" "$2" || err "src：下载失败 $2（可手动放一份到 $1）"
}

do_src() {
    mkdir -p "$THIRD_PARTY"
    fetch "$OPENSSL_TGZ" "$OPENSSL_URL" "openssl-$OPENSSL_VER"
    fetch "$LIBUV_TGZ" "$LIBUV_URL" "libuv-v$LIBUV_VER"
    fetch "$LIBUSB_TGZ" "$LIBUSB_URL" "libusb-$LIBUSB_VER"
    fetch "$LZ4_TGZ" "$LZ4_URL" "lz4-$LZ4_VER"
    fetch "$ZLIB_TGZ" "$ZLIB_URL" "zlib-$ZLIB_VER"
}

select_toolchain() {
    case "$PLATFORM" in
        linux)
            command -v gcc >/dev/null 2>&1 || err "linux：缺 gcc"
            DEPS_CC=gcc
            ;;
        ohos)
            # musl 目标：CC 自带 --target/--sysroot（openssl Configure / lz4 make 都吃这种 CC）
            [ -x "$CLANG" ] || err "ohos：缺 SDK clang：$CLANG"
            [ -d "$OHOS_SYSROOT" ] || err "ohos：缺 SDK sysroot：$OHOS_SYSROOT"
            DEPS_CC="$CLANG --target=$OHOS_TARGET --sysroot=$OHOS_SYSROOT"
            ;;
        *) err "未知 PLATFORM：$PLATFORM（可用：linux|ohos）" ;;
    esac
}

build_openssl() {
    [ -f "$DEPS/lib/libcrypto.a" ] && { log "build：openssl 已就位，跳过"; return 0; }
    local target=linux-x86_64
    [ "$PLATFORM" = "ohos" ] && target=linux-aarch64
    log "build：openssl-$OPENSSL_VER（$target，no-shared，只要 libcrypto/libssl）"
    rm -rf "$SRC/openssl"
    mkdir -p "$SRC/openssl"
    tar -xzf "$OPENSSL_TGZ" -C "$SRC/openssl" --strip-components=1
    ( cd "$SRC/openssl"
      CC="$DEPS_CC" perl ./Configure "$target" -fPIC no-shared no-tests \
          --prefix="$DEPS" --openssldir="$DEPS/ssl" --libdir=lib >configure.log 2>&1
      make -j"$JOBS" build_libs >make.log 2>&1
      make install_dev >install.log 2>&1 ) \
        || { cat "$SRC/openssl/configure.log" "$SRC/openssl/make.log" 2>/dev/null; err "build：openssl 编译失败"; }
}

build_libuv() {
    [ -f "$DEPS/lib/libuv.a" ] && { log "build：libuv 已就位，跳过"; return 0; }
    [ -n "$CMAKE" ] || err "build：libuv 需要 cmake（系统没有，SDK 里也没找到）"
    log "build：libuv-v$LIBUV_VER（cmake；archive 包没有 configure，新版也没有 meson）"
    rm -rf "$SRC/libuv"
    mkdir -p "$SRC/libuv"
    tar -xzf "$LIBUV_TGZ" -C "$SRC/libuv" --strip-components=1
    ( cd "$SRC/libuv"
      CC="$DEPS_CC" "$CMAKE" -S . -B builddir \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DLIBUV_BUILD_TESTS=OFF -DLIBUV_BUILD_BENCH=OFF \
          -DCMAKE_INSTALL_PREFIX="$DEPS" >configure.log 2>&1
      "$CMAKE" --build builddir -j"$JOBS" >make.log 2>&1
      "$CMAKE" --install builddir >install.log 2>&1 ) \
        || { cat "$SRC/libuv/configure.log" "$SRC/libuv/make.log" 2>/dev/null; err "build：libuv 编译失败"; }
}

build_libusb() {
    [ -f "$DEPS/lib/libusb-1.0.a" ] && { log "build：libusb 已就位，跳过"; return 0; }
    log "build：libusb-$LIBUSB_VER（--disable-udev：容器里没有 udev 开发头）"
    rm -rf "$SRC/libusb"
    mkdir -p "$SRC/libusb"
    tar -xjf "$LIBUSB_TGZ" -C "$SRC/libusb" --strip-components=1
    ( cd "$SRC/libusb"
      CC="$DEPS_CC" CFLAGS="-O2 -fPIC" ./configure --disable-shared --enable-static \
          --disable-udev --prefix="$DEPS" >configure.log 2>&1
      make -j"$JOBS" >make.log 2>&1
      make install >install.log 2>&1
      # hdc 里 <libusb.h> 与 <libusb/libusb.h> 两种写法都有，桥接一份
      mkdir -p "$DEPS/include/libusb"
      cp "$DEPS/include/libusb-1.0/libusb.h" "$DEPS/include/libusb/libusb.h" )
}

build_lz4() {
    [ -f "$DEPS/lib/liblz4.a" ] && { log "build：lz4 已就位，跳过"; return 0; }
    log "build：lz4-$LZ4_VER"
    rm -rf "$SRC/lz4"
    mkdir -p "$SRC/lz4"
    tar -xzf "$LZ4_TGZ" -C "$SRC/lz4" --strip-components=1
    ( cd "$SRC/lz4"
      # liblz4.a 的目标是 lib/ 子目录 Makefile 的，顶层 Makefile 不认识
      make -C lib -j"$JOBS" CC="$DEPS_CC" CFLAGS="-O2 -fPIC" liblz4.a >make.log 2>&1
      mkdir -p "$DEPS/lib" "$DEPS/include"
      cp lib/liblz4.a "$DEPS/lib/"
      cp lib/lz4.h lib/lz4hc.h lib/lz4frame.h "$DEPS/include/" ) \
        || { cat "$SRC/lz4/make.log" 2>/dev/null; err "build：lz4 编译失败"; }
}

# zlib 只用 minizip/hapsigner 需要的 inflate/deflate 一族，手工编 + ar：
# zlib 的 configure 对 SDK clang 交叉目标不友好，tarball 里自带预生成 zconf.h
build_zlib() {
    [ -f "$DEPS/lib/libz.a" ] && { log "build：zlib 已就位，跳过"; return 0; }
    log "build：zlib-$ZLIB_VER（手工编 + ar）"
    rm -rf "$SRC/zlib"
    mkdir -p "$SRC/zlib"
    tar -xzf "$ZLIB_TGZ" -C "$SRC/zlib" --strip-components=1
    ( cd "$SRC/zlib"
      mkdir -p objs
      local zobjs=() f
      for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite \
               infback inflate inffast inftrees trees uncompr zutil; do
          $DEPS_CC -O2 -fPIC -I. -c "$f.c" -o "objs/$f.o"
          zobjs+=("objs/$f.o")
      done
      ar rcs libz.a "${zobjs[@]}"
      mkdir -p "$DEPS/lib" "$DEPS/include"
      cp libz.a "$DEPS/lib/"
      cp zlib.h zconf.h "$DEPS/include/" ) \
        || err "build：zlib 编译失败"
}

# libusb 只装头文件（两种 include 写法都桥接好）：ohos 用 compat 里的 stub 实现，
# linux 编真库
install_libusb_headers() {
    [ -f "$DEPS/include/libusb/libusb.h" ] && return 0
    rm -rf "$SRC/libusb"
    mkdir -p "$SRC/libusb"
    tar -xjf "$LIBUSB_TGZ" -C "$SRC/libusb" --strip-components=1
    mkdir -p "$DEPS/include/libusb"
    cp "$SRC/libusb/libusb/libusb.h" "$DEPS/include/libusb/libusb.h"
}

do_build() {
    select_toolchain
    mkdir -p "$DEPS" "$SRC"
    case "$PLATFORM" in
        linux)
            build_openssl
            build_libuv
            build_libusb
            build_lz4
            build_zlib
            ;;
        ohos)
            # libuv：直链 SDK sysroot 的 libuv.so；libusb：compat stub（设备上无 USB host 场景）
            build_openssl
            build_lz4
            build_zlib
            install_libusb_headers
            ;;
    esac
    log "build：deps 完成（$DEPS/lib：$(ls "$DEPS/lib" 2>/dev/null | tr '\n' ' ')）"
}

case "$PHASE" in
    src)   do_src ;;
    build) do_build ;;
    all)   do_src; do_build ;;
    *)     err "未知阶段：$PHASE（可用：src|build|all）" ;;
esac
