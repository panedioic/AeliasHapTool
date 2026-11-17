/*
 * aht_hap — 从 HAP（zip）里取签名需要的元信息：
 *   - module.json 的 app.bundleName
 *   - module.requestPermissions 中需要写进 Profile ACL 的受限权限
 *   - 是否已带签名块（只看签名块 magic，不做验签）
 *
 * zip 读取复用 thirdparty/contrib/minizip（与 libhapsigner 同一份源码）。
 */
#ifndef AHT_HAP_H
#define AHT_HAP_H

#include <string>
#include <vector>

namespace aht {

struct HapInfo {
    std::string bundleName;
    std::vector<std::string> aclPermissions;   // 白名单内的受限权限
    bool signed_ = false;
    std::string error;
};

// 读 module.json（根目录或一层子目录里都认）
HapInfo inspectHap(const std::string& hapPath);

// 只判签名块，不解析 zip
bool hapIsSigned(const std::string& hapPath);

} // namespace aht

#endif /* AHT_HAP_H */
