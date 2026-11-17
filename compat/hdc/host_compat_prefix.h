/*
 * Host-build compatibility prefix, force-included (-include) into every C++ TU of
 * the hdc manual build.
 *
 * Upstream hdc compiles inside the OpenHarmony GN/OHOS-SDK include graph, where a
 * few headers get their standard-library declarations transitively (hdc_huks.h
 * uses std::vector without including <vector>; memory_pool.h calls std::malloc
 * without <cstdlib>). The host g++ / vanilla-musl include graph does not provide
 * those. Rather than patching the checkout, this small prefix fills the gaps —
 * declarations only, no behavior.
 */
#ifndef HDC_HOST_COMPAT_PREFIX_H
#define HDC_HOST_COMPAT_PREFIX_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>

/* system_depend.cpp 的 #include "parameter.h" 在 __OHOS__ 里，而 GetParameter 调用
 * 只看 HARMONY_PROJECT —— 宿主构建在这里补上声明（返回默认值的 stub） */
#include "parameter.h"

/*
 * OHOS 的 third_party/libuv fork 给 uv_timer_start 用的两个毫秒常量，官方
 * libuv 没有（server.cpp 的 USB 预连接定时器）。宿主上走不到 USB 路径，
 * 给个 1s/1s 的定义只为编译通过。
 */
#ifndef UV_TIMEOUT
#define UV_TIMEOUT 1000
#endif
#ifndef UV_REPEAT
#define UV_REPEAT 1000
#endif

/*
 * Only takes effect when compiling with the OHOS SDK: clang predefines __OHOS__
 * for the aarch64-linux-ohos target, but the libc++ headers above depend on it
 * (strtoll_l compatibility declarations). So first let the standard library
 * headers finish parsing (above with __OHOS__ on), then undef it here, so that
 * the hdc source code after this point no longer sees __OHOS__ — same semantics
 * as the linux build (pure TCP host, no UDS/hishell/init_reboot device branches).
 */
#undef __OHOS__

#endif /* HDC_HOST_COMPAT_PREFIX_H */
