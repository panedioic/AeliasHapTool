/*
 * aht_cloud — 华为开发者账号登录与调试签名材料申请。
 *
 * 流程（自动化的是 DevEco Studio 自己用的接口）：
 *   sign login  → 本机 127.0.0.1 起临时 HTTP 服务，浏览器登录后回调 tempToken
 *               → 换 accessToken/jwt，存 AHT_HOME/auth.json
 *   sign add    → 团队 → 调试证书 → 设备注册 → 调试 Profile(.p7b)
 *
 * 只在本机 loopback 收 tempToken；账号密码/验证码都在用户自己的浏览器里输入。
 * 接口属华为内部协议，随时可能变更；仅用于给自己有权使用的应用做本机调试签名。
 */
#ifndef AHT_CLOUD_H
#define AHT_CLOUD_H

#include <string>
#include <vector>

namespace aht {

// ── 协议常量（与 DevEco IdeSystem.properties / AGC 签名服务一致） ──
extern const char* const kBaseUrl;          // https://devecostudio.huawei.com
extern const char* const kCloudBaseUrl;     // https://connect-api.cloud.huawei.com
extern const char* const kAppId;            // 1007
extern const char* const kLoginVersion;     // 5.0.5

struct AuthToken {
    std::string accessToken;
    std::string refreshToken;
    std::string jwt;
    std::string uid;
    std::string userName;
    std::string teamId;
    std::string siteId = "CN";
};

struct CertEntry {
    std::string id;
    std::string certName;
    std::string objectId;
};

struct DeviceEntry {
    std::string id;
    std::string udid;
};

// 起本地回调服务并等浏览器登录；返回 tempToken。
// 手动模式（设备上无浏览器）直接调用 exchangeTempToken。
std::string loginWithBrowser(const std::string& siteId, int timeoutSec, std::string& error);

// tempToken → jwtToken → accessToken
bool exchangeTempToken(const std::string& tempToken, const std::string& siteId,
                       AuthToken& out, std::string& error);

// accessToken 过期时用 jwtToken 刷新
bool refreshAccessToken(AuthToken& token, std::string& error);

class Cloud {
public:
    explicit Cloud(AuthToken token);

    bool fetchTeamId(std::string& error);

    bool listCerts(std::vector<CertEntry>& out, std::string& error);
    bool addCert(const std::string& csr, const std::string& certName, std::string& error);

    bool listDevices(std::vector<DeviceEntry>& out, std::string& error);
    bool addDevice(const std::string& udid, const std::string& deviceType,
                   const std::string& deviceName, std::string& error);

    // 申请调试 Profile，返回 provisionFileUrl 与 profile id
    bool addTestProvision(const std::string& bundle, const std::string& certId,
                          const std::vector<std::string>& deviceIds,
                          const std::vector<std::string>& aclPermissions,
                          std::string& fileUrl, std::string& profileId, std::string& error);

    // 用 reapply 换直链后下载
    bool downloadObject(const std::string& sourceUrl, const std::string& destPath,
                        std::string& error);

    const AuthToken& token() const { return token_; }

private:
    bool request(const std::string& method, const std::string& path,
                 const std::string& body, const std::string& contentType,
                 std::string& out, std::string& error);
    std::string url(const std::string& path) const;

    AuthToken token_;
};

} // namespace aht

#endif /* AHT_CLOUD_H */
