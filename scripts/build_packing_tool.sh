



local work="$BUILD/packing_tool/src"
local out="$OUT/app_packing_tool"
prepare_src "$UP_PACKING/packing_tool/frameworks" "$work" "packing_tool/frameworks"
apply_patches "$work"
local SRC="$work"
local COMPAT="$SPEC/compat/packing_tool"
local TP="$SPEC/third_party"
local MINIZIP="$ZLIB_SRC/contrib/minizip"

INCLUDES=(
    "-I$SRC/include" "-I$SRC/include/json" "-I$SRC/include/error" "-I$SRC/include/dedup"
    "-I$COMPAT"          # securec.h + nlohmann/json.hpp
    "-I$TP"              # cJSON.h
    "-I$ZLIB_SRC"        # "contrib/minizip/{zip,unzip}.h" 由 zlib 根解析
    "-I$MINIZIP"
    "-I$PREFIX/include"  # openssl
)
local SRCS=(
    "$SRC/src/app_packager.cpp" "$SRC/src/appqf_packager.cpp"
    "$SRC/src/dedup/device_calculator.cpp" "$SRC/src/dedup/exact_solver.cpp"
    "$SRC/src/dedup/greedy_solver.cpp" "$SRC/src/dedup/module_calculator.cpp"
    "$SRC/src/dedup/report_generator.cpp" "$SRC/src/dedup/so_deduplicator.cpp"
    "$SRC/src/error/error_msg.cpp" "$SRC/src/error/packing_tool_err_msg.cpp"
    "$SRC/src/error/scan_error_enum.cpp" "$SRC/src/fast_app_packager.cpp"
    "$SRC/src/general_normalize.cpp" "$SRC/src/hap_packager.cpp" "$SRC/src/hqf_packager.cpp"
    "$SRC/src/hqf_verify.cpp" "$SRC/src/hsp_packager.cpp"
    "$SRC/src/json/distro_filter.cpp" "$SRC/src/json/general_normalize_version_utils.cpp"
    "$SRC/src/json/hap_verify_info.cpp" "$SRC/src/json/hap_verify_utils.cpp"
    "$SRC/src/json/json_utils.cpp" "$SRC/src/json/module_json.cpp"
    "$SRC/src/json/module_json_fa.cpp" "$SRC/src/json/module_json_stage.cpp"
    "$SRC/src/json/module_json_utils.cpp" "$SRC/src/json/normalize_version_utils.cpp"
    "$SRC/src/json/pack_info.cpp" "$SRC/src/json/pack_info_utils.cpp"
    "$SRC/src/json/patch_json.cpp" "$SRC/src/json/patch_json_utils.cpp" "$SRC/src/json/pt_json.cpp"
    "$SRC/src/log.cpp" "$SRC/src/main.cpp" "$SRC/src/multiapp_packager.cpp"
    "$SRC/src/package_normalize.cpp" "$SRC/src/packager.cpp" "$SRC/src/res_packager.cpp"
    "$SRC/src/scan_statdulpicate.cpp" "$SRC/src/shell_command.cpp"
    "$SRC/src/skill_pack_helper.cpp" "$SRC/src/unzip_wrapper.cpp" "$SRC/src/utils.cpp"
    "$SRC/src/version_normalize.cpp" "$SRC/src/zip_utils.cpp" "$SRC/src/zip_wrapper.cpp"
    "$SRC/src/incremental_pack.cpp"
)
local CSRC=(
    "$TP/cJSON.c" "$MINIZIP/zip.c" "$MINIZIP/unzip.c" "$MINIZIP/ioapi.c"
)
local m
for m in "${SRCS[@]}" "${CSRC[@]}"; do
    [ -f "$m" ] || err "build：缺源文件 $m"
done

CC="${CROSS}gcc"
CXX="${CROSS}g++"
# -fexceptions：上游 BUILD.gn 就开了（general_normalize.cpp 等大量 try/catch）。
CFLAGS=(-O2 -g0 -w -static -fno-pie)
CXXFLAGS=(-std=c++17 -fno-rtti -fexceptions -O2 -g0 -w -static -fno-pie
            -include "$COMPAT/host_compat_prefix.h")
LDFLAGS=(-static -no-pie -L"$PREFIX/lib" -lcrypto -lz -lpthread -ldl)

local NAMES=()
local f
NAMES+=("cxx:$COMPAT/securec.cpp")
for f in "${SRCS[@]}"; do NAMES+=("cxx:$f"); done
for f in "${CSRC[@]}"; do NAMES+=("cc:$f"); done

compile_and_link "$BUILD/packing_tool/obj" "$BUILD/packing_tool/logs" "$out"
log "build：$(du -h "$out" | cut -f1)  $out"