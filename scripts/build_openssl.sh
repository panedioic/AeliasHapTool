
# openssl（no-shared，只要 libcrypto 里的 md5/sha）
if [ ! -d "$OPENSSL_SRC" ]; then
    # 优先用本仓 already-downloaded 的 openssl 源码（build_openssl.sh 的产物）
    local cand
    cand="$(ls -d "$ROOT"/thirdparty/openssl-*/ 2>/dev/null | head -1 || true)"
    if [ -n "$cand" ]; then
        OPENSSL_SRC="${cand%/}"
        log "deps：复用本仓已有 openssl 源码 $OPENSSL_SRC"
    else
        if [ ! -f "$OPENSSL_TARBALL" ]; then
            log "deps：下载 openssl-$OPENSSL_VERSION（走 scripts/env.sh 探测到的代理）"
            mkdir -p "$BUILD"
            curl -fL --retry 3 -o "$OPENSSL_TARBALL" "$OPENSSL_URL" \
                || err "deps：下载失败 $OPENSSL_URL（可手动放一份到 $OPENSSL_TARBALL）"
        fi
        tar -xzf "$OPENSSL_TARBALL" -C "$BUILD"
    fi
fi
mkdir -p "$BUILD/openssl-build"
( cd "$BUILD/openssl-build"
    CC="${CROSS}gcc" perl "$OPENSSL_SRC/Configure" linux-aarch64 \
        --prefix="$PREFIX" --openssldir="$PREFIX/ssl" --libdir=lib \
        no-shared no-tests >configure.log 2>&1
    make -j"$JOBS" build_libs >make.log 2>&1
    make install_dev >install.log 2>&1 )