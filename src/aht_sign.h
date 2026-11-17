/*
 * aht_sign — 调试签名材料准备与本地签名。
 *
 * 材料准备（一次登录，之后复用）：
 *   1. 本地已有材料且包名/UDID/Profile 有效期都对得上 → 直接复用，不联网
 *   2. 否则走云端：调试证书（CSR 由 libhapsigner 生成）→ 注册设备 → 调试 Profile
 * 签名：
 *   用 libhapsigner.so 的 sign-app 本地签，不再依赖 Java/hap-sign-tool.jar。
 */
#ifndef AHT_SIGN_H
#define AHT_SIGN_H

#include <functional>
#include <string>
#include <vector>

#include "aht_store.h"

namespace aht {

// 调库入口（主程序注入：fork 子进程跑 libhapsigner/libhdc），返回退出码并带走输出
using LibRunner = std::function<int(const std::vector<std::string>& args,
                                    std::string& output)>;

struct MaterialsResult {
    Materials materials;
    bool reused = false;      // 是否命中本地缓存（未联网）
};

// 确保包名对应的签名材料可用；allowLogin=false 时未登录直接失败。
// log 收集过程信息（逐行追加）。
bool ensureMaterials(const std::string& home, const std::string& bundle,
                     const std::vector<std::string>& aclPermissions,
                     const std::vector<std::string>& udids,
                     const std::string& deviceType, bool allowLogin,
                     const LibRunner& runSigner, MaterialsResult& out,
                     std::string& log, std::string& error);

// 通过 hdc 读当前连接设备的 UDID（64 位 hex）
bool readDeviceUdid(const LibRunner& runHdc, std::string& udid, std::string& error);

// 本地签名；outPath 为空时用 <input>-signed.hap
bool signHap(const Materials& materials, const std::string& hapPath,
             const std::string& outPath, const LibRunner& runSigner,
             std::string& signedPath, std::string& log, std::string& error);

// 默认输出名：a.hap → a-signed.hap
std::string defaultSignedPath(const std::string& hapPath);

} // namespace aht

#endif /* AHT_SIGN_H */
