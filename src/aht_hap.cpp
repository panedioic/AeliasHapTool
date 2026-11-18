#include "aht_hap.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <set>

#include "unzip.h"
#include "cJSON.h"

namespace aht {

namespace {

// 可写进 Profile ACL 的权限白名单（与 DevEco 生成 Profile 的 allowed-acls 对齐）
const std::set<std::string>& aclWhitelist()
{
    static const std::set<std::string> kList = {
        "ohos.permission.READ_WRITE_DESKTOP_DIRECTORY",
        "ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY",
        // JIT 全开（可写可执行内存双门）需要它，与上面那条成对出现；
        // 白名单漏了它时 Profile 不带这条 ACL，装机报 9568289 grant failed。
        "ohos.permission.kernel.DISABLE_CODE_MEMORY_PROTECTION",
        "ohos.permission.READ_WRITE_DOCUMENTS_DIRECTORY",
        "ohos.permission.CUSTOM_SANDBOX",
        "ohos.permission.READ_PASTEBOARD",
        "ohos.permission.READ_WRITE_USER_FILE",
        "ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE",
        "ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY",
        "ohos.permission.READ_WRITE_USB_DEV",
        "ohos.permission.READ_CONTACTS",
        "ohos.permission.WRITE_CONTACTS",
        "ohos.permission.READ_AUDIO",
        "ohos.permission.WRITE_AUDIO",
        "ohos.permission.READ_IMAGEVIDEO",
        "ohos.permission.WRITE_IMAGEVIDEO",
        "ohos.permission.ACCESS_DDK_USB",
        "ohos.permission.ACCESS_DDK_HID",
        "ohos.permission.SYSTEM_FLOAT_WINDOW",
        "ohos.permission.FILE_ACCESS_PERSIST",
        "ohos.permission.INPUT_MONITORING",
        "ohos.permission.INTERCEPT_INPUT_EVENT",
        "ohos.permission.SHORT_TERM_WRITE_IMAGEVIDEO",
    };
    return kList;
}

bool endsWithIgnoreCase(const std::string& s, const char* suffix)
{
    size_t n = std::strlen(suffix);

    if (s.size() < n) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));

        if (a != b) {
            return false;
        }
    }

    return true;
}

// 从 zip 里读一个最浅的 module.json（根目录优先）
bool readModuleJson(unzFile uf, std::string& content)
{
    std::string bestName;
    std::string bestData;

    int rc = unzGoToFirstFile(uf);

    while (rc == UNZ_OK) {
        char nameBuf[1024] = "";
        unz_file_info info;

        if (unzGetCurrentFileInfo(uf, &info, nameBuf, sizeof(nameBuf), nullptr, 0,
                                  nullptr, 0) == UNZ_OK) {
            std::string name = nameBuf;
            size_t slash = name.rfind('/');

            if (endsWithIgnoreCase(name, "module.json")) {
                std::string base = slash == std::string::npos ? name : name.substr(slash + 1);

                if (base == "module.json") {
                    // 目录层级越浅越优先
                    if (bestName.empty() || name.size() < bestName.size()) {
                        if (unzOpenCurrentFile(uf) == UNZ_OK) {
                            std::string data;
                            char buf[8192];
                            int n = 0;

                            while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
                                data.append(buf, static_cast<size_t>(n));
                            }

                            unzCloseCurrentFile(uf);

                            if (n >= 0 && !data.empty()) {
                                bestName = name;
                                bestData.swap(data);
                            }
                        }
                    }
                }
            }
        }

        rc = unzGoToNextFile(uf);
    }

    if (bestName.empty()) {
        return false;
    }

    content.swap(bestData);
    return true;
}

bool containsToken(const char* buf, size_t len, const char* needle)
{
    size_t n = std::strlen(needle);

    if (len < n) {
        return false;
    }

    for (size_t i = 0; i + n <= len; ++i) {
        if (std::memcmp(buf + i, needle, n) == 0) {
            return true;
        }
    }

    return false;
}

std::string jsonString(const cJSON* obj, const char* key)
{
    const cJSON* item = cJSON_GetObjectItem(obj, key);

    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    return "";
}

} // namespace

bool hapIsSigned(const std::string& hapPath)
{
    std::ifstream in(hapPath, std::ios::binary);

    if (!in) {
        return false;
    }

    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();

    if (size < 64) {
        return false;
    }

    // EOCD 在最尾部 64K 内
    std::streamoff tailLen = size < (65557 + 22) ? size : (65557 + 22);
    std::string tail(static_cast<size_t>(tailLen), '\0');
    in.seekg(size - tailLen);
    in.read(&tail[0], static_cast<std::streamsize>(tailLen));

    size_t eocd = tail.rfind(std::string("PK\x05\x06", 4));

    if (eocd == std::string::npos || tail.size() < eocd + 22) {
        return false;
    }

    const auto* p = reinterpret_cast<const unsigned char*>(tail.data() + eocd);
    uint32_t cdOffset = static_cast<uint32_t>(p[16]) | (static_cast<uint32_t>(p[17]) << 8)
                      | (static_cast<uint32_t>(p[18]) << 16) | (static_cast<uint32_t>(p[19]) << 24);

    if (cdOffset == 0xFFFFFFFFu || cdOffset < 32) {
        return false;
    }

    // 签名块紧贴中央目录之前
    char header[32] = "";
    in.seekg(static_cast<std::streamoff>(cdOffset) - 32);
    in.read(header, sizeof(header));

    if (in.gcount() != sizeof(header)) {
        return false;
    }

    return containsToken(header, sizeof(header), "HAP Sig Block 24")
        || containsToken(header, sizeof(header), "<hap sign block>");
}

HapInfo inspectHap(const std::string& hapPath)
{
    HapInfo info;

    unzFile uf = unzOpen64(hapPath.c_str());

    if (!uf) {
        info.error = "not a valid zip/hap: " + hapPath;
        return info;
    }

    std::string content;

    if (!readModuleJson(uf, content)) {
        unzClose(uf);
        info.error = "no module.json inside hap: " + hapPath;
        return info;
    }

    unzClose(uf);

    cJSON* root = cJSON_Parse(content.c_str());

    if (!root) {
        info.error = "cannot parse module.json";
        return info;
    }

    const cJSON* app = cJSON_GetObjectItem(root, "app");

    if (app) {
        info.bundleName = jsonString(app, "bundleName");
    }

    if (info.bundleName.empty()) {
        // 兼容 stage 模型前的老包：直接在根上
        info.bundleName = jsonString(root, "bundleName");
    }

    const cJSON* module = cJSON_GetObjectItem(root, "module");
    const cJSON* perms = module ? cJSON_GetObjectItem(module, "requestPermissions") : nullptr;

    if (perms && cJSON_IsArray(perms)) {
        const std::set<std::string>& white = aclWhitelist();
        const cJSON* item = nullptr;

        cJSON_ArrayForEach(item, perms) {
            std::string name = jsonString(item, "name");

            if (!name.empty() && white.count(name)) {
                info.aclPermissions.push_back(name);
            }
        }
    }

    cJSON_Delete(root);

    if (info.bundleName.empty()) {
        info.error = "module.json has no app.bundleName";
    }

    info.signed_ = hapIsSigned(hapPath);

    return info;
}

} // namespace aht
