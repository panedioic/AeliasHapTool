/*
 * libhapsigner.so 的对外入口：与上游 hapsigntool_cpp/main.cpp 的 main() 等价
 * （ParamsRunTool::ProcessCmd → 0/1），只改名导出。argv[0] 按程序名跳过，
 * 传 "hap-sign-tool" 即可；命令照 C++ 版签名工具 CLI 拼，
 * 例：{ "hap-sign-tool", "sign-app", "-keystoreFile", "x.p12", ... }。
 */
#ifndef HAPSIGNER_COMPAT_ENTRY_H
#define HAPSIGNER_COMPAT_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

int HapSignerLibMain(int argc, const char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* HAPSIGNER_COMPAT_ENTRY_H */
