#include "aht_sign.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

#include <strings.h>

#include <openssl/evp.h>
#include <unistd.h>

namespace aht {

namespace {

const char* kKeyAlias = "aht_debug";

void append(std::string& log, const std::string& line)
{
    log += line;
    log += '\n';
}

std::string randomPassword()
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    FILE* fp = std::fopen("/dev/urandom", "rb");
    unsigned char buf[16] = {0};

    if (fp) {
        (void)!std::fread(buf, 1, sizeof(buf), fp);
        std::fclose(fp);
    }

    for (unsigned char b : buf) {
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }

    return out;
}

// 证书名带钥库指纹：同名证书必来自同一把私钥，复用它才不会"证书与密钥不匹配"
std::string certNameFor(const std::string& p12Path)
{
    unsigned char digest[32] = {0};
    unsigned int digestLen = 0;
    unsigned char buf[4096];

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    FILE* fp = std::fopen(p12Path.c_str(), "rb");

    if (ctx && fp && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) {
        size_t n = 0;

        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
            EVP_DigestUpdate(ctx, buf, n);
        }

        EVP_DigestFinal_ex(ctx, digest, &digestLen);
    }

    if (fp) {
        std::fclose(fp);
    }
    if (ctx) {
        EVP_MD_CTX_free(ctx);
    }

    char hex[11] = {0};

    for (int i = 0; i < 5 && i * 2 < static_cast<int>(digestLen); ++i) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }

    return std::string("aht_") + hex + ".cer";
}

std::time_t now()
{
    return std::time(nullptr);
}

std::string isoNow()
{
    char buf[32] = "";
    std::time_t t = now();
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

bool exists(const std::string& path)
{
    return access(path.c_str(), R_OK) == 0;
}

} // namespace

std::string defaultSignedPath(const std::string& hapPath)
{
    const std::string suffix = ".hap";
    std::string base = hapPath;

    if (base.size() > suffix.size()
        && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base = base.substr(0, base.size() - suffix.size());
    }

    return base + "-signed.hap";
}

bool readDeviceUdid(const LibRunner& runHdc, std::string& udid, std::string& error)
{
    const std::vector<std::vector<std::string>> probes = {
        {"shell", "bm", "get", "--udid"},
        {"shell", "bm", "get", "-u"},
        {"shell", "param", "get", "const.product.udid"},
    };

    for (const auto& args : probes) {
        std::string output;

        if (runHdc(args, output) != 0) {
            continue;
        }

        // 提取 64 位十六进制
        for (size_t i = 0; i + 64 <= output.size(); ++i) {
            bool match = true;

            for (size_t k = 0; k < 64; ++k) {
                if (!std::isxdigit(static_cast<unsigned char>(output[i + k]))) {
                    match = false;
                    break;
                }
            }

            if (match) {
                udid = output.substr(i, 64);
                return true;
            }
        }
    }

    error = "无法从设备读取 UDID（设备未授权调试？可用 --udid 手动指定）";
    return false;
}

bool ensureMaterials(const std::string& home, const std::string& bundle,
                     const std::vector<std::string>& aclPermissions,
                     const std::vector<std::string>& udids,
                     const std::string& deviceType, bool allowLogin,
                     const LibRunner& runSigner, MaterialsResult& out,
                     std::string& log, std::string& error)
{
    // 1) 本地材料可用 → 完全离线
    if (materialsReusable(home, bundle, udids, out.materials)) {
        out.reused = true;
        append(log, "复用本地签名材料（包名/设备/Profile 有效期均匹配），无需联网");
        return true;
    }

    // 2) 需要云端：先确认登录态；交互环境里直接拉起浏览器登录
    AuthToken token;

    if (!loadAuth(home, token)) {
        if (!allowLogin) {
            error = "尚未登录：请先执行 haptool sign login";
            return false;
        }

        append(log, "尚未登录，开始华为开发者账号登录 …");

        std::string tempToken = loginWithBrowser("CN", 600, error);

        if (tempToken.empty()) {
            return false;
        }

        if (!exchangeTempToken(tempToken, "CN", token, error)) {
            return false;
        }

        if (!saveAuth(home, token)) {
            error = "无法写入 " + home + "/auth.json";
            return false;
        }

        append(log, "登录成功：" + (token.userName.empty() ? token.uid : token.userName));
    }

    append(log, "本地材料缺失或已失效，向云端申请 …");

    Cloud cloud(token);

    if (!cloud.fetchTeamId(error)) {
        return false;
    }

    append(log, "团队 ID：" + cloud.token().teamId);

    std::string dir = profileDir(home, bundle);

    if (!ensurePrivateDir(dir)) {
        error = "无法创建材料目录：" + dir;
        return false;
    }

    // 2.1) 密钥对（没有就生成；密码落盘到 keystore.pwd）
    std::string p12 = dir + "/key.p12";
    std::string pwdPath = dir + "/keystore.pwd";
    std::string pwd;

    if (!exists(p12)) {
        pwd = randomPassword();

        std::string output;
        int rc = runSigner({"generate-keypair",
                            "-keyAlias", kKeyAlias,
                            "-keyAlg", "ECC", "-keySize", "NIST-P-256",
                            "-keystoreFile", p12,
                            "-keyPwd", pwd, "-keystorePwd", pwd}, output);

        if (rc != 0) {
            error = "生成本地密钥对失败：" + output.substr(0, 400);
            return false;
        }

        if (!writePrivateFile(pwdPath, pwd + "\n")) {
            error = "无法写入 " + pwdPath;
            return false;
        }

        append(log, "已生成本地密钥对：" + p12);
    } else {
        std::string text;

        if (!readTextFile(pwdPath, text)) {
            error = "缺少钥库口令文件：" + pwdPath;
            return false;
        }

        pwd = text.substr(0, text.find_first_of("\r\n"));
        append(log, "复用已有密钥对：" + p12);
    }

    // 2.2) CSR
    std::string csrPath = dir + "/request.csr";
    std::string certName = certNameFor(p12);

    if (!exists(csrPath)) {
        std::string subject = "C=CN,O=OpenHarmony,OU=OpenHarmony Community,CN=" + bundle;
        std::string output;
        int rc = runSigner({"generate-csr",
                            "-keyAlias", kKeyAlias, "-keyPwd", pwd,
                            "-subject", subject,
                            "-signAlg", "SHA256withECDSA",
                            "-keystoreFile", p12, "-keystorePwd", pwd,
                            "-outFile", csrPath}, output);

        if (rc != 0 || !exists(csrPath)) {
            error = "生成 CSR 失败：" + output.substr(0, 400);
            return false;
        }
    }

    std::string csr;

    if (!readTextFile(csrPath, csr)) {
        error = "无法读取 " + csrPath;
        return false;
    }

    // 2.3) 调试证书（同名证书 = 同一把私钥，直接复用）
    std::vector<CertEntry> certs;

    if (!cloud.listCerts(certs, error)) {
        return false;
    }

    auto found = std::find_if(certs.begin(), certs.end(),
                              [&](const CertEntry& c) { return c.certName == certName; });

    if (found == certs.end()) {
        append(log, "申请调试证书 " + certName + " …");

        if (!cloud.addCert(csr, certName, error)) {
            return false;
        }

        if (!cloud.listCerts(certs, error)) {
            return false;
        }

        found = std::find_if(certs.begin(), certs.end(),
                             [&](const CertEntry& c) { return c.certName == certName; });

        if (found == certs.end()) {
            error = "证书申请后仍查不到 " + certName;
            return false;
        }
    } else {
        append(log, "复用账号下已有调试证书 " + certName);
    }

    std::string certPath = dir + "/cert.cer";

    if (!exists(certPath)) {
        std::string objectId = found->objectId.empty() ? found->id : found->objectId;

        if (!cloud.downloadObject(objectId, certPath, error)) {
            return false;
        }
    }

    // 2.4) 设备注册
    std::vector<DeviceEntry> devices;

    if (!cloud.listDevices(devices, error)) {
        return false;
    }

    std::vector<std::string> deviceIds;

    for (const auto& udid : udids) {
        auto it = std::find_if(devices.begin(), devices.end(), [&](const DeviceEntry& d) {
            return strcasecmp(d.udid.c_str(), udid.c_str()) == 0;
        });

        if (it == devices.end()) {
            append(log, "注册调试设备 " + udid.substr(0, 8) + "…");

            std::string name = "aht_device_" + std::to_string(now() % 100000);

            if (!cloud.addDevice(udid, deviceType, name, error)) {
                return false;
            }

            if (!cloud.listDevices(devices, error)) {
                return false;
            }

            it = std::find_if(devices.begin(), devices.end(), [&](const DeviceEntry& d) {
                return strcasecmp(d.udid.c_str(), udid.c_str()) == 0;
            });

            if (it == devices.end()) {
                error = "设备注册后仍查不到 UDID " + udid.substr(0, 8);
                return false;
            }
        }

        deviceIds.push_back(it->id);
    }

    // 2.5) 调试 Profile
    std::string profilePath = dir + "/profile.p7b";
    long long expiry = 0;
    std::string profileBundle;

    bool profileOk = exists(profilePath)
                  && profileExpiry(profilePath, expiry, profileBundle)
                  && (profileBundle.empty() || profileBundle == bundle)
                  && expiry > now() + 86400;

    if (!profileOk) {
        append(log, "申请调试 Profile " + bundle + " …");

        std::string fileUrl;
        std::string profileId;

        if (!cloud.addTestProvision(bundle, found->id, deviceIds, aclPermissions,
                                    fileUrl, profileId, error)) {
            return false;
        }

        if (!cloud.downloadObject(fileUrl, profilePath, error)) {
            return false;
        }
    } else {
        append(log, "复用本地仍有效的 Profile");
    }

    long long notAfter = 0;
    std::string p7bBundle;
    profileExpiry(profilePath, notAfter, p7bBundle);

    // 2.6) 记录 meta（决定下次能否离线复用）
    ProfileMeta meta;
    meta.bundle = bundle;
    meta.udids = udids;
    meta.deviceIds = deviceIds;
    meta.certId = found->id;
    meta.certName = certName;
    meta.keyAlias = kKeyAlias;
    meta.profileExpiry = notAfter;
    meta.updatedAt = isoNow();

    if (!saveMeta(home, meta)) {
        error = "无法写入 meta.json";
        return false;
    }

    // 登录态里的 teamId 也落盘（下次免查）
    token.teamId = cloud.token().teamId;
    saveAuth(home, token);

    out.materials.keyP12 = p12;
    out.materials.keyPwd = pwd;
    out.materials.keyAlias = kKeyAlias;
    out.materials.cert = certPath;
    out.materials.profile = profilePath;
    out.reused = false;
    return true;
}

bool signHap(const Materials& materials, const std::string& hapPath,
             const std::string& outPath, const LibRunner& runSigner,
             std::string& signedPath, std::string& log, std::string& error)
{
    signedPath = outPath.empty() ? defaultSignedPath(hapPath) : outPath;

    std::string staging = signedPath + ".part";

    std::string output;
    int rc = runSigner({"sign-app",
                        "-mode", "localSign",
                        "-keyAlias", materials.keyAlias,
                        "-signAlg", "SHA256withECDSA",
                        "-appCertFile", materials.cert,
                        "-profileFile", materials.profile,
                        "-keystoreFile", materials.keyP12,
                        "-keystorePwd", materials.keyPwd,
                        "-keyPwd", materials.keyPwd,
                        "-compatibleVersion", "10",
                        "-signCode", "1",
                        "-inFile", hapPath,
                        "-outFile", staging}, output);

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.find("INFO") != std::string::npos
            || line.find("Error") != std::string::npos
            || line.find("error") != std::string::npos) {
            append(log, line);
        }
    }

    if (rc != 0 || !exists(staging)) {
        std::remove(staging.c_str());
        error = "签名失败（hap-sign-tool 退出码 " + std::to_string(rc) + "）";
        return false;
    }

    if (std::rename(staging.c_str(), signedPath.c_str()) != 0) {
        std::remove(staging.c_str());
        error = "无法写入 " + signedPath;
        return false;
    }

    return true;
}

} // namespace aht
