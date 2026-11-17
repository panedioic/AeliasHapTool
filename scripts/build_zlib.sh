mkdir -p "$BUILD/zlib-build"
( cd "$BUILD/zlib-build"
CHOST=aarch64-linux-musl CC="${CROSS}gcc" \
    "$ZLIB_SRC/configure" --static --prefix="$PREFIX" >configure.log 2>&1
make -j"$JOBS" libz.a >make.log 2>&1
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp libz.a "$PREFIX/lib/"
cp "$ZLIB_SRC/zlib.h" "$ZLIB_SRC/zconf.h" "$PREFIX/include/" )