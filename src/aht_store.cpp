#include "aht_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cJSON.h"

namespace aht {

namespace {

const char* kMetaFile = "meta.json";
const char* kKeyFile = "key.p12";
const char* kPwdFile = "keystore.pwd";
const char* kCertFile = "cert.cer";
const char* kProfileFile = "profile.p7b";

bool isValidBundleName(const std::string& name)
{
    // Profile 里的 bundle-name 只含常见包名字符
    if (name.empty() || name.size() > 256) {
        return false;
    }

    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }

    return true;
}


std::string jsonStr(const cJSON* obj, const char* key)
{
    const cJSON* item = obj ? cJSON_GetObjectItem(obj, key) : nullptr;

    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    return "";
}

std::string jsonNumOrStr(const cJSON* obj, const char* key)
{
    const cJSON* item = obj ? cJSON_GetObjectItem(obj, key) : nullptr;

    if (!item) {
        return "";
    }

    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    if (cJSON_IsNumber(item)) {
        return std::to_string(static_cast<long long>(item->valuedouble));
    }

    return "";
}

} // namespace

bool readTextFile(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        return false;
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

bool ensurePrivateDir(const std::string& path)
{
    struct stat st;

    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // 逐级创建
    std::string acc;

    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];

        if (path[i] == '/' || i + 1 == path.size()) {
            if (acc == "/" || acc.empty()) {
                continue;
            }
            mkdir(acc.c_str(), 0700);
        }
    }

    chmod(path.c_str(), 0700);
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool writePrivateFile(const std::string& path, const std::string& data)
{
    std::string tmp = path + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "wb");

    if (!fp) {
        return false;
    }

    size_t n = std::fwrite(data.data(), 1, data.size(), fp);
    std::fclose(fp);

    if (n != data.size()) {
        std::remove(tmp.c_str());
        return false;
    }

    chmod(tmp.c_str(), 0600);

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }

    return true;
}

// ─────────────────────────── 登录态 ───────────────────────────

bool loadAuth(const std::string& home, AuthToken& out)
{
    std::string text;

    if (!readTextFile(home + "/auth.json", text)) {
        return false;
    }

    cJSON* root = cJSON_Parse(text.c_str());

    if (!root) {
        return false;
    }

    out.accessToken = jsonStr(root, "accessToken");
    out.refreshToken = jsonStr(root, "refreshToken");
    out.jwt = jsonStr(root, "jwt");
    out.uid = jsonStr(root, "uid");
    out.userName = jsonStr(root, "userName");
    out.teamId = jsonStr(root, "teamId");
    out.siteId = jsonStr(root, "siteId");

    if (out.siteId.empty()) {
        out.siteId = "CN";
    }

    cJSON_Delete(root);
    return !out.jwt.empty() && !out.uid.empty();
}

bool saveAuth(const std::string& home, const AuthToken& token)
{
    if (!ensurePrivateDir(home)) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "accessToken", token.accessToken.c_str());
    cJSON_AddStringToObject(root, "refreshToken", token.refreshToken.c_str());
    cJSON_AddStringToObject(root, "jwt", token.jwt.c_str());
    cJSON_AddStringToObject(root, "uid", token.uid.c_str());
    cJSON_AddStringToObject(root, "userName", token.userName.c_str());
    cJSON_AddStringToObject(root, "teamId", token.teamId.c_str());
    cJSON_AddStringToObject(root, "siteId", token.siteId.c_str());
    cJSON_AddStringToObject(root, "savedAt",
        std::to_string(static_cast<long long>(std::time(nullptr))).c_str());

    char* printed = cJSON_Print(root);
    bool ok = writePrivateFile(home + "/auth.json", std::string(printed) + "\n");
    cJSON_free(printed);
    cJSON_Delete(root);
    return ok;
}

bool clearAuth(const std::string& home)
{
    return std::remove((home + "/auth.json").c_str()) == 0;
}

// ─────────────────────────── 材料目录 ───────────────────────────

std::string sanitizeBundle(const std::string& bundle)
{
    std::string out;

    for (char c : bundle) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }

    return out.empty() ? "_" : out;
}

std::string profileDir(const std::string& home, const std::string& bundle)
{
    return home + "/profiles/" + sanitizeBundle(bundle);
}

bool loadMeta(const std::string& home, const std::string& bundle, ProfileMeta& out)
{
    std::string text;

    if (!readTextFile(profileDir(home, bundle) + "/" + kMetaFile, text)) {
        return false;
    }

    cJSON* root = cJSON_Parse(text.c_str());

    if (!root) {
        return false;
    }

    out.bundle = jsonStr(root, "bundle");
    out.certId = jsonNumOrStr(root, "certId");
    out.certName = jsonStr(root, "certName");
    out.keyAlias = jsonStr(root, "keyAlias");
    out.updatedAt = jsonStr(root, "updatedAt");

    const cJSON* expiry = cJSON_GetObjectItem(root, "profileExpiry");
    out.profileExpiry = expiry && cJSON_IsNumber(expiry)
                      ? static_cast<long long>(expiry->valuedouble) : 0;

    const cJSON* udids = cJSON_GetObjectItem(root, "udids");
    const cJSON* item = nullptr;

    cJSON_ArrayForEach(item, udids) {
        if (cJSON_IsString(item) && item->valuestring) {
            out.udids.push_back(item->valuestring);
        }
    }

    const cJSON* deviceIds = cJSON_GetObjectItem(root, "deviceIds");

    cJSON_ArrayForEach(item, deviceIds) {
        if (cJSON_IsString(item) && item->valuestring) {
            out.deviceIds.push_back(item->valuestring);
        }
    }

    cJSON_Delete(root);
    return true;
}

bool saveMeta(const std::string& home, const ProfileMeta& meta)
{
    std::string dir = profileDir(home, meta.bundle);

    if (!ensurePrivateDir(dir)) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "bundle", meta.bundle.c_str());
    cJSON_AddStringToObject(root, "certId", meta.certId.c_str());
    cJSON_AddStringToObject(root, "certName", meta.certName.c_str());
    cJSON_AddStringToObject(root, "keyAlias", meta.keyAlias.c_str());
    cJSON_AddNumberToObject(root, "profileExpiry",
                            static_cast<double>(meta.profileExpiry));
    cJSON_AddStringToObject(root, "updatedAt", meta.updatedAt.c_str());

    cJSON* udids = cJSON_CreateArray();
    for (const auto& u : meta.udids) {
        cJSON_AddItemToArray(udids, cJSON_CreateString(u.c_str()));
    }
    cJSON_AddItemToObject(root, "udids", udids);

    cJSON* deviceIds = cJSON_CreateArray();
    for (const auto& d : meta.deviceIds) {
        cJSON_AddItemToArray(deviceIds, cJSON_CreateString(d.c_str()));
    }
    cJSON_AddItemToObject(root, "deviceIds", deviceIds);

    char* printed = cJSON_Print(root);
    bool ok = writePrivateFile(dir + "/" + kMetaFile, std::string(printed) + "\n");
    cJSON_free(printed);
    cJSON_Delete(root);
    return ok;
}

bool profileExpiry(const std::string& p7bPath, long long& notAfter,
                   std::string& bundleName)
{
    std::string data;

    if (!readTextFile(p7bPath, data)) {
        return false;
    }

    notAfter = 0;

    size_t pos = data.find("\"not-after\"");

    if (pos != std::string::npos) {
        size_t colon = data.find(':', pos);
        size_t start = data.find_first_of("0123456789", colon);

        if (start != std::string::npos) {
            size_t end = start;

            while (end < data.size() && std::isdigit(static_cast<unsigned char>(data[end]))) {
                ++end;
            }

            notAfter = std::atoll(data.substr(start, end - start).c_str());
        }
    }

    bundleName.clear();
    pos = data.find("\"bundle-name\"");

    if (pos != std::string::npos) {
        size_t colon = data.find(':', pos);
        size_t quote = data.find('"', colon);

        if (quote != std::string::npos) {
            size_t end = data.find('"', quote + 1);

            if (end != std::string::npos) {
                bundleName = data.substr(quote + 1, end - quote - 1);
            }
        }
    }

    return notAfter > 0;
}

bool materialsReusable(const std::string& home, const std::string& bundle,
                       const std::vector<std::string>& udids, Materials& out)
{
    ProfileMeta meta;

    if (!loadMeta(home, bundle, meta) || meta.bundle != bundle) {
        return false;
    }

    if (meta.udids.size() != udids.size()) {
        return false;
    }

    std::vector<std::string> a = meta.udids;
    std::vector<std::string> b = udids;
    std::transform(a.begin(), a.end(), a.begin(), [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    });
    std::transform(b.begin(), b.end(), b.begin(), [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    });
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());

    if (a != b) {
        return false;
    }

    std::string dir = profileDir(home, bundle);
    struct stat st;

    if (stat((dir + "/" + kKeyFile).c_str(), &st) != 0
        || stat((dir + "/" + kCertFile).c_str(), &st) != 0
        || stat((dir + "/" + kProfileFile).c_str(), &st) != 0) {
        return false;
    }

    long long expiry = 0;
    std::string profileBundle;

    if (!profileExpiry(dir + "/" + kProfileFile, expiry, profileBundle)) {
        return false;
    }

    if (isValidBundleName(profileBundle) && profileBundle != bundle) {
        return false;
    }

    // 不足一天就当过期
    if (expiry <= std::time(nullptr) + 86400) {
        return false;
    }

    std::string pwd;

    if (!readTextFile(dir + "/" + kPwdFile, pwd) || pwd.empty()) {
        return false;
    }

    while (!pwd.empty() && (pwd.back() == '\n' || pwd.back() == '\r')) {
        pwd.pop_back();
    }

    out.keyP12 = dir + "/" + kKeyFile;
    out.keyPwd = pwd;
    out.keyAlias = meta.keyAlias.empty() ? "aht_debug" : meta.keyAlias;
    out.cert = dir + "/" + kCertFile;
    out.profile = dir + "/" + kProfileFile;
    return true;
}

std::vector<std::string> listBundles(const std::string& home)
{
    std::vector<std::string> out;
    std::string root = home + "/profiles";
    DIR* dir = opendir(root.c_str());

    if (!dir) {
        return out;
    }

    struct dirent* ent = nullptr;

    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;

        if (name == "." || name == "..") {
            continue;
        }

        std::string metaPath = root + "/" + name + "/" + kMetaFile;
        struct stat st;

        if (stat(metaPath.c_str(), &st) == 0) {
            ProfileMeta meta;

            if (loadMeta(home, name, meta) && !meta.bundle.empty()) {
                out.push_back(meta.bundle);
            } else {
                out.push_back(name);
            }
        }
    }

    closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace aht
