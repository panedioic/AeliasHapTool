




local work="$BUILD/hapsigntool/src"
local out="$OUT/hap-sign-tool"
prepare_src "$UP_HAPSIGNER/hapsigntool_cpp" "$work" "hapsigntool_cpp"
# 签名工具没有补丁：它需要的差异全在 compat/ 里（securec/cJSON/minizip + -include 前缀头），
# patches/ 里那两个只针对打包工具的调用点（见 scripts/haptools-ohos/README.md）。
local SRC="$work"

INCLUDES=(
    "-I$SPEC/compat/hapsigntool"   # securec.h + host_compat_prefix.h
    "-I$SPEC/third_party"          # cJSON.h + contrib/minizip/unzip.h
    "-I$SRC/api/include" "-I$SRC/signer/include" "-I$SRC/utils/include"
    "-I$SRC/zip/include" "-I$SRC/codesigning/datastructure/include"
    "-I$SRC/codesigning/fsverity/include" "-I$SRC/codesigning/utils/include"
    "-I$SRC/codesigning/sign/include" "-I$SRC/common/include"
    "-I$SRC/hap/verify/include" "-I$SRC/hap/config/include"
    "-I$SRC/hap/entity/include" "-I$SRC/hap/provider/include"
    "-I$SRC/hap/sign/include" "-I$SRC/hap/utils/include"
    "-I$SRC/hap/ticket/include" "-I$SRC/profile/include" "-I$SRC/cmd/include"
    "-I$PREFIX/include"
)
local SRCS=(
    "$SRC/main.cpp" "$SRC/api/src/sign_tool_service_impl.cpp" "$SRC/api/src/cert_tools.cpp"
    "$SRC/signer/src/signer_factory.cpp" "$SRC/signer/src/local_signer.cpp"
    "$SRC/utils/src/cert_dn_utils.cpp" "$SRC/utils/src/verify_cert_openssl_utils.cpp"
    "$SRC/utils/src/file_utils.cpp" "$SRC/utils/src/key_store_helper.cpp"
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
    "$SRC/codesigning/fsverity/src/fs_verity_descriptor.cpp"
    "$SRC/codesigning/fsverity/src/fs_verity_digest.cpp"
    "$SRC/codesigning/fsverity/src/fs_verity_generator.cpp"
    "$SRC/codesigning/fsverity/src/fs_verity_hash_algorithm.cpp"
    "$SRC/codesigning/fsverity/src/merkle_tree_builder.cpp"
    "$SRC/codesigning/fsverity/src/fs_verity_descriptor_with_sign.cpp"
    "$SRC/codesigning/utils/src/fs_digest_utils.cpp" "$SRC/codesigning/utils/src/cms_utils.cpp"
    "$SRC/codesigning/sign/src/bc_signeddata_generator.cpp"
    "$SRC/codesigning/sign/src/code_signing.cpp"
    "$SRC/codesigning/sign/src/verify_code_signature.cpp"
    "$SRC/common/src/byte_buffer_data_source.cpp" "$SRC/common/src/byte_buffer.cpp"
    "$SRC/common/src/file_data_source.cpp" "$SRC/common/src/random_access_file.cpp"
    "$SRC/common/src/digest_parameter.cpp" "$SRC/common/src/digest_common.cpp"
    "$SRC/common/src/localization_adapter.cpp" "$SRC/common/src/options.cpp"
    "$SRC/common/src/password_guard.cpp"
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
local CXXSRC=(
    "$SPEC/compat/hapsigntool/securec.cpp"
)
local CSRC=(
    "$SPEC/third_party/cJSON.c"
    "$SPEC/third_party/contrib/minizip/unzip.c"
    "$SPEC/third_party/contrib/minizip/ioapi.c"
)
local m
for m in "${SRCS[@]}" "${CXXSRC[@]}" "${CSRC[@]}"; do
    [ -f "$m" ] || err "build：缺源文件 $m"
done

CC="${CROSS}gcc"
CXX="${CROSS}g++"
# -static/-no-pie：数据目录不允许可执行映射，这个程序由 elfloader 装进匿名内存再跳进去
#   执行，静态 + 无解释器最省事，宿主机上也能直接用 qemu-aarch64 跑（不需要任何 .so）。
# -fno-rtti：全树没有 dynamic_cast/typeid（与上游 GN 口径一致）。
# -include host_compat_prefix.h：上游几个头依赖 OHOS 工具链的传递包含图才有的
#   <vector>/<filesystem> 等，musl-gcc 的图谱里没有；只补声明不补行为。
CFLAGS=(-O2 -g0 -w -static -fno-pie)
CXXFLAGS=(-std=c++17 -fno-rtti -O2 -g0 -w -static -fno-pie
            "-D__FILE_NAME__=__FILE__"
            -include "$SPEC/compat/hapsigntool/host_compat_prefix.h")
LDFLAGS=(-static -no-pie -L"$PREFIX/lib" -lssl -lcrypto -lz -lpthread -ldl)

local NAMES=()
local f
for f in "${SRCS[@]}" "${CXXSRC[@]}"; do NAMES+=("cxx:$f"); done
for f in "${CSRC[@]}"; do NAMES+=("cc:$f"); done

compile_and_link "$BUILD/hapsigntool/obj" "$BUILD/hapsigntool/logs" "$out"
log "build：$(du -h "$out" | cut -f1)  $out"