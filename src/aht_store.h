/*
 * aht_store — AHT_HOME 下的材料管理。
 *
 * 布局（整个目录可整份拷到另一台设备，免登录直接签名）：
 *   $AHT_HOME/
 *     auth.json                 登录态（jwt/accessToken/uid/teamId）
 *     sign.json                 手工导入的签名配置（sign import）
 *     profiles/<bundle>/
 *       key.p12                 私钥库（自动生成）
 *       keystore.pwd            钥库口令（与 p12 同权限域，0600）
 *       cert.cer                调试证书
 *       profile.p7b             调试 Profile
 *       meta.json               包名/UDID/证书 id/Profile 到期等，用于复用判定
 */
#ifndef AHT_STORE_H
#define AHT_STORE_H

#include <string>
#include <vector>

#include "aht_cloud.h"

namespace aht {

struct ProfileMeta {
    std::string bundle;
    std::vector<std::string> udids;
    std::vector<std::string> deviceIds;
    std::string certId;
    std::string certName;
    std::string keyAlias;          // 钥库里的条目名（复用 p12 时要用）
    long long profileExpiry = 0;   // unix 秒
    std::string updatedAt;
};

struct Materials {
    std::string keyP12;
    std::string keyPwd;
    std::string keyAlias;
    std::string cert;
    std::string profile;
};

// 登录态
bool loadAuth(const std::string& home, AuthToken& out);
bool saveAuth(const std::string& home, const AuthToken& token);
bool clearAuth(const std::string& home);

// 包名 → 材料目录（bundle 里的非法文件名字符替换为 _）
std::string profileDir(const std::string& home, const std::string& bundle);
std::string sanitizeBundle(const std::string& bundle);

bool loadMeta(const std::string& home, const std::string& bundle, ProfileMeta& out);
bool saveMeta(const std::string& home, const ProfileMeta& meta);

// .p7b 里读 not-after 与 bundle-name（明文 JSON 在信封内）
bool profileExpiry(const std::string& p7bPath, long long& notAfter,
                   std::string& bundleName);

// 材料齐、包名一致、UDID 集合一致、Profile 未过期（余量 1 天）
bool materialsReusable(const std::string& home, const std::string& bundle,
                       const std::vector<std::string>& udids, Materials& out);

// 列出有材料的包名
std::vector<std::string> listBundles(const std::string& home);

// 目录权限 0700 / 文件 0600
bool ensurePrivateDir(const std::string& path);
bool writePrivateFile(const std::string& path, const std::string& data);
bool readTextFile(const std::string& path, std::string& out);

} // namespace aht

#endif /* AHT_STORE_H */
