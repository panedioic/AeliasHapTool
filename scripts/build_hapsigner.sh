#!/bin/bash
# build_hapsigner.sh — OHOS **C++ 版签名工具**（developtools_hapsigner/hapsigntool_cpp）
# 编成 libhapsigner.so：haptool 的 hap sign / hap genkey 进程内调用（fork 子进程调
# HapSignerLibMain，见 compat/hapsigntool/hapsigner_entry.cpp）。
#
# 与 libhdc.so 同一套路：上游只读零补丁，差异收敛在 compat/
#（securec 替身 + HapSignerLibMain 入口替身 + host_compat_prefix.h）。
#
# ── 阶段 ────────────────────────────────────────────────────────────────
#   src    git clone 上游到 thirdparty/developtools_hapsigner（幂等；不改远端、不 pull）
#   build  源码副本 → 全部 TU → $OUT/libhapsigner.so
#   all    src + build
#
# ── 平台（PLATFORM 环境变量，默认 linux）───────────────────────────────
#   linux  宿主 g++（x86_64）；ohos  SDK clang --target=aarch64-linux-ohos
#
# 依赖：./build_deps.sh build 的 openssl/zlib 静态库（build/deps-$PLATFORM）
#
# 用法：PLATFORM=linux ./build_hapsigner.sh [src|build|all]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="${PLATFORM:-linux}"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

PHASE="${1:-all}"

HS_WORK="$BUILD/hapsigner/$PLATFORM/src"   # 上游源码副本（上游只读）
HS_OBJ="$BUILD/hapsigner/$PLATFORM/obj"
HS_LOGS="$BUILD/hapsigner/$PLATFORM/logs"
OUT_SO="$OUT/libhapsigner.so"

do_src() {
    clone_upstream "$GITCODE_HAPSIGNER" "$UP_HAPSIGNER" "developtools_hapsigner"
}

select_toolchain() {
    case "$PLATFORM" in
        linux)
            command -v g++ >/dev/null 2>&1 || err "linux：缺 g++"
            HS_CC=gcc
            HS_CXX=g++
            HS_CFLAGS=(-fPIC -O2 -g0 -w)
            HS_CXXFLAGS=(-std=c++17 -fPIC -O2 -g0 -w -fno-rtti
                         "-D__FILE_NAME__=__FILE__")
            HS_LDFLAGS=(-shared -fPIC)
            HS_LIBS=(-lpthread -ldl)
            ;;
        ohos)
            [ -x "$CLANGXX" ] || err "ohos：缺 SDK clang++：$CLANGXX"
            [ -d "$OHOS_SYSROOT" ] || err "ohos：缺 SDK sysroot：$OHOS_SYSROOT"
            HS_CC="$CLANG"
            HS_CXX="$CLANGXX"
            # -U_FORTIFY_SOURCE：OHOS 头把 FD_SET 展开成 vanilla musl 里没有的 __fd_chk
            HS_CFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" \
                       -fPIC -O2 -g0 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
            HS_CXXFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" -std=c++17 \
                         -fPIC -O2 -g0 -w -fno-rtti \
                         -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-c++11-narrowing \
                         "-D__FILE_NAME__=__FILE__")
            HS_LDFLAGS=(--target="$OHOS_TARGET" --sysroot="$OHOS_SYSROOT" -shared -fPIC \
                        -Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1 -Wl,-z,now)
            HS_LIBS=(-lpthread -ldl)
            ;;
        *) err "未知 PLATFORM：$PLATFORM（可用：linux|ohos）" ;;
    esac
}

do_build() {
    [ -d "$UP_HAPSIGNER/hapsigntool_cpp" ] || err "hapsigner：缺上游源码 $UP_HAPSIGNER（先跑 src 阶段）"
    [ -f "$BUILD/deps-$PLATFORM/lib/libcrypto.a" ] || err "hapsigner：缺 openssl（先跑 ./build_deps.sh build）"
    [ -f "$BUILD/deps-$PLATFORM/lib/libz.a" ] || err "hapsigner：缺 zlib（先跑 ./build_deps.sh build）"
    select_toolchain

    prepare_src "$UP_HAPSIGNER/hapsigntool_cpp" "$HS_WORK" "hapsigntool_cpp"
    apply_patches "$HS_WORK" "hapsigner"
    local SRC="$HS_WORK"

    # 编译清单逐条照上游 hapsigntool_cpp 的 GN 构建（与参考流水线一致）。
    # 唯一差异：main.cpp 不编 —— so 里不能有 main，入口由
    # compat/hapsigntool/hapsigner_entry.cpp 的 HapSignerLibMain 替代。
    INCLUDES=(
        "-I$SPEC/compat/hapsigntool"   # securec.h + host_compat_prefix.h + hapsigner_entry.h
        "-I$SPEC/thirdparty"           # cJSON.h + contrib/minizip/{unzip.h,...}
        "-I$BUILD/deps-$PLATFORM/include"  # openssl / zlib
        "-I$SRC/api/include" "-I$SRC/signer/include" "-I$SRC/utils/include"
        "-I$SRC/zip/include" "-I$SRC/codesigning/datastructure/include"
        "-I$SRC/codesigning/fsverity/include" "-I$SRC/codesigning/utils/include"
        "-I$SRC/codesigning/sign/include" "-I$SRC/common/include"
        "-I$SRC/hap/verify/include" "-I$SRC/hap/config/include"
        "-I$SRC/hap/entity/include" "-I$SRC/hap/provider/include"
        "-I$SRC/hap/sign/include" "-I$SRC/hap/utils/include"
        "-I$SRC/hap/ticket/include" "-I$SRC/profile/include" "-I$SRC/cmd/include"
    )
    local SRCS=(
        "$SRC/api/src/sign_tool_service_impl.cpp" "$SRC/api/src/cert_tools.cpp"
        "$SRC/signer/src/signer_factory.cpp" "$SRC/signer/src/local_signer.cpp"
        "$SRC/utils/src/cert_dn_utils.cpp" "$SRC/utils/src/verify_cert_openssl_utils.cpp"
        "$SRC/utils/src/key_store_helper.cpp"
        "$SRC/utils/src/hap_signer_block_utils.cpp" "$SRC/utils/src/string_utils.cpp"
        "$SRC/utils/src/verify_hap_openssl_utils.cpp" "$SRC/utils/src/byte_array_utils.cpp"
        "$SRC/utils/src/hash_utils.cpp"
        "$SRC/zip/src/central_directory.cpp" "$SRC/zip/src/data_descriptor.cpp"
        "$SRC/zip/src/endof_central_directory.cpp" "$SRC/zip/src/random_access_file_input.cpp"
        "$SRC/zip/src/random_access_file_output.cpp" "$SRC/zip/src/zip64_end_of_central_directory.cpp"
        "$SRC/zip/src/zip64_end_of_central_directory_locator.cpp" "$SRC/zip/src/zip64_extended_info.cpp"
        "$SRC/zip/src/zip_entry.cpp" "$SRC/zip/src/zip_entry_data.cpp"
        "$SRC/zip/src/zip_entry_header.cpp" "$SRC/zip/src/zip_signer.cpp" "$SRC/zip/src/zip_utils.cpp"
        "$SRC/codesigning/datastructure/src/code_sign_block.cpp"
        "$SRC/codesigning/datastructure/src/code_sign_block_header.cpp"
        "$SRC/codesigning/datastructure/src/extension.cpp"
        "$SRC/codesigning/datastructure/src/fs_verity_info_segment.cpp"
        "$SRC/codesigning/datastructure/src/hap_info_segment.cpp"
        "$SRC/codesigning/datastructure/src/merkle_tree_extension.cpp"
        "$SRC/codesigning/datastructure/src/native_lib_info_segment.cpp"
        "$SRC/codesigning/datastructure/src/segment_header.cpp"
        "$SRC/codesigning/datastructure/src/sign_info.cpp"
        "$SRC/codesigning/datastructure/src/signed_file_pos.cpp"
        "$SRC/codesigning/datastructure/src/elf_sign_block.cpp"
        "$SRC/codesigning/datastructure/src/unzip_handle_param.cpp"
        "$SRC/codesigning/datastructure/src/page_info_extension.cpp"
        "$SRC/codesigning/fsverity/src/fs_verity_descriptor.cpp"
        "$SRC/codesigning/fsverity/src/fs_verity_digest.cpp"
        "$SRC/codesigning/fsverity/src/fs_verity_generator.cpp"
        "$SRC/codesigning/fsverity/src/fs_verity_hash_algorithm.cpp"
        "$SRC/codesigning/fsverity/src/merkle_tree_builder.cpp"
        "$SRC/codesigning/fsverity/src/fs_verity_descriptor_with_sign.cpp"
        "$SRC/codesigning/utils/src/fs_digest_utils.cpp" "$SRC/codesigning/utils/src/cms_utils.cpp"
        "$SRC/codesigning/sign/src/bc_signeddata_generator.cpp"
        "$SRC/codesigning/sign/src/code_signing.cpp"
        "$SRC/codesigning/sign/src/page_info_generator.cpp"
        "$SRC/codesigning/sign/src/verify_code_signature.cpp"
        "$SRC/common/src/byte_buffer_data_source.cpp" "$SRC/common/src/byte_buffer.cpp"
        "$SRC/common/src/file_data_source.cpp" "$SRC/common/src/random_access_file.cpp"
        "$SRC/common/src/digest_parameter.cpp" "$SRC/common/src/digest_common.cpp"
        "$SRC/common/src/localization_adapter.cpp" "$SRC/common/src/options.cpp"
        "$SRC/common/src/password_guard.cpp"
        "$SRC/common/src/file_utils.cpp" "$SRC/common/src/error_message.cpp"
        "$SRC/hap/verify/src/verify_hap.cpp" "$SRC/hap/verify/src/verify_bin.cpp"
        "$SRC/hap/verify/src/verify_elf.cpp" "$SRC/hap/config/src/signer_config.cpp"
        "$SRC/hap/entity/src/content_digest_algorithm.cpp" "$SRC/hap/entity/src/param_constants.cpp"
        "$SRC/hap/entity/src/signature_algorithm_helper.cpp" "$SRC/hap/entity/src/block_data.cpp"
        "$SRC/hap/entity/src/block_head.cpp" "$SRC/hap/entity/src/sign_head.cpp"
        "$SRC/hap/entity/src/sign_block_info.cpp" "$SRC/hap/entity/src/sign_content_info.cpp"
        "$SRC/hap/entity/src/sign_block_data.cpp" "$SRC/hap/entity/src/signature_block_types.cpp"
        "$SRC/hap/entity/src/signing_block.cpp" "$SRC/hap/provider/src/local_sign_provider.cpp"
        "$SRC/hap/provider/src/remote_sign_provider.cpp" "$SRC/hap/provider/src/sign_provider.cpp"
        "$SRC/hap/sign/src/sign_hap.cpp" "$SRC/hap/sign/src/sign_bin.cpp"
        "$SRC/hap/sign/src/sign_elf.cpp" "$SRC/hap/utils/src/hap_utils.cpp"
        "$SRC/hap/utils/src/dynamic_lib_handle.cpp" "$SRC/hap/sign/src/bc_pkcs7_generator.cpp"
        "$SRC/hap/ticket/src/ticket_staple.cpp" "$SRC/hap/ticket/src/ticket_validate.cpp"
        "$SRC/profile/src/profile_info.cpp" "$SRC/profile/src/profile_verify.cpp"
        "$SRC/profile/src/pkcs7_data.cpp" "$SRC/profile/src/profile_sign_tool.cpp"
        "$SRC/cmd/src/cmd_util.cpp" "$SRC/cmd/src/params_run_tool.cpp"
        "$SRC/cmd/src/params_trust_list.cpp" "$SRC/cmd/src/params.cpp"
    )
    local CSRC=(
        "$SPEC/thirdparty/cJSON.c"
        "$SPEC/thirdparty/contrib/minizip/unzip.c"
        "$SPEC/thirdparty/contrib/minizip/ioapi.c"
    )
    local m
    for m in "${SRCS[@]}" "${CSRC[@]}" \
             "$SPEC/compat/hapsigntool/securec.cpp" ; do
        [ -f "$m" ] || err "build：缺源文件 $m"
    done

    # 宿主侧替身：securec / HapSignerLibMain 入口
    NAMES=()
    local f
    for f in "${SRCS[@]}"; do NAMES+=("cxx:$f"); done
    NAMES+=("cxx:$SPEC/compat/hapsigntool/securec.cpp")
    NAMES+=("cxx:$SPEC/compat/hapsigntool/hapsigner_entry.cpp")
    for f in "${CSRC[@]}"; do NAMES+=("cc:$f"); done

    # -include host_compat_prefix.h：上游几个头依赖 OHOS 工具链传递包含图才有的
    #   <vector>/<filesystem> 等，宿主/原生 musl 的图谱里没有；只补声明不补行为
    CFLAGS=("${HS_CFLAGS[@]}")
    CXXFLAGS=("${HS_CXXFLAGS[@]}" -include "$SPEC/compat/hapsigntool/host_compat_prefix.h")
    CC="$HS_CC"
    CXX="$HS_CXX"

    log "hapsigner：编 libhapsigner.so（${#NAMES[@]} 个 TU）→ $OUT_SO"
    mkdir -p "$OUT"
    compile_tus "$HS_OBJ" "$HS_LOGS"

    log "hapsigner：链接 $OUT_SO"
    "$HS_CXX" "${HS_LDFLAGS[@]}" -o "$OUT_SO" "${OBJS[@]}" \
        -L"$BUILD/deps-$PLATFORM/lib" \
        -lssl -lcrypto -lz \
        "${HS_LIBS[@]}"
    log "hapsigner：$(du -h "$OUT_SO" | cut -f1)  $OUT_SO"
}

case "$PHASE" in
    src)   do_src ;;
    build) do_build ;;
    all)   do_src; do_build ;;
    *)     err "未知阶段：$PHASE（可用：src|build|all）" ;;
esac
