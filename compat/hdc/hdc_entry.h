/*
 * libhdc.so 的对外入口：与上游 src/host/main.cpp 的 main() 逻辑一致
 * （haptool 用 fork 子进程的方式调用，见 src/main.cpp 的 runHdc）。
 * argv[0] 会当程序名跳过，传 "hdc" 即可；命令照 hdc CLI 拼，
 * 例：{ "hdc", "tconn", "127.0.0.1:8710" } / { "hdc", "install", "xxx.hap" }。
 */
#ifndef HDC_COMPAT_ENTRY_H
#define HDC_COMPAT_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

int HdcLibMain(int argc, const char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* HDC_COMPAT_ENTRY_H */
