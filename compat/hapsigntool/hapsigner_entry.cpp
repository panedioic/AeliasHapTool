/*
 * 宿主侧替身：上游 hapsigntool_cpp/main.cpp 的 main() 原样等价（逻辑就一行：
 * ParamsRunTool::ProcessCmd），改名 HapSignerLibMain 导出。上游 main.cpp 本身
 * 不参与编译——一个 so 只能有一个入口，haptool 的 main 在外面。上游更新时
 * diff hapsigntool_cpp/main.cpp 同步此文件即可。
 */
#include "params_run_tool.h"

#include <openssl/err.h>

#include <cstdio>

#include "hapsigner_entry.h"

extern "C" int HapSignerLibMain(int argc, const char *argv[])
{
    // 这里**不**加载 legacy provider：本构建的 libcrypto 是 no-shared 静态库，
    // 没有 legacy 模块可加载，强行 load 只会在错误队列里留下 DSO 噪音，污染
    // 上游失败路径的诊断（verify-app 等会把它当成 openssl 错误打印）。
    // 代价：读不了 RC2/3DES 老格式 p12。p12 的默认加密已由
    // patches/hapsigner-0001-*.patch 改成 AES-256（PKCS12 的 OpenSSL 3 默认），
    // haptool hap genkey 产出的以及 keytool 转换出来的 p12 都是这种新格式。
    ERR_clear_error();

    // ProcessCmd(args[1] 起)解析 method 与 -option 值，只读不写；签名是 const 的
    if (OHOS::SignatureTools::ParamsRunTool::ProcessCmd(
            const_cast<char **>(argv), static_cast<size_t>(argc))) {
        return 0;
    }

    // 上游失败路径大多不打印 OpenSSL 错误队列（CreatePKCS12 一族尤其安静），
    // 失败时把队列倒出来，否则排障只能靠猜
    unsigned long code;
    while ((code = ERR_get_error()) != 0) {
        fprintf(stderr, "[openssl] %s\n", ERR_error_string(code, nullptr));
    }
    return 1;
}
