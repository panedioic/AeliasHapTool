#include "aht_cloud.h"
#include "aht_http.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <random>

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"

namespace aht {

const char* const kBaseUrl = "https://devecostudio.huawei.com";
const char* const kCloudBaseUrl = "https://connect-api.cloud.huawei.com";
const char* const kAppId = "1007";
const char* const kLoginVersion = "5.0.5";

namespace {

const char* kLoginAuthPath = "/console/DevEcoIDE/apply";
const char* kLoginSuccessPath = "/console/DevEcoIDE/loginSuccess";
const char* kTempTokenCheckPath = "/authrouter/auth/api/temptoken/check";
const char* kJwtTokenCheckPath = "/authrouter/auth/api/jwToken/check";

const char* kApiUserTeam = "/api/ups/user-permission-service/v1/user-team-list";
const char* kApiCertList = "/api/cps/harmony-cert-manage/v1/cert/list";
const char* kApiCertAdd = "/api/cps/harmony-cert-manage/v1/cert/add";
const char* kApiDeviceList = "/api/cps/device-manage/v1/device/list";
const char* kApiDeviceAdd = "/api/cps/device-manage/v1/device/add";
const char* kApiTestProvisionAdd = "/api/cps/provision-manage/v1/ide/test/provision/add";
const char* kApiReapply = "/api/amis/app-manage/v1/objects/url/reapply";

std::string randomHex(size_t bytes)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    std::random_device rd;

    for (size_t i = 0; i < bytes; ++i) {
        unsigned char b = static_cast<unsigned char>(rd() & 0xFF);
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }

    return out;
}

std::string urlDecode(const std::string& in)
{
    std::string out;

    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            char hexBuf[3] = {in[i + 1], in[i + 2], 0};
            out += static_cast<char>(std::strtol(hexBuf, nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }

    return out;
}

// 解析 "a=1&b=2" 到 Pairs
Pairs parseQuery(const std::string& query)
{
    Pairs out;
    size_t pos = 0;

    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string item = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = item.find('=');

        if (eq != std::string::npos) {
            out.emplace_back(urlDecode(item.substr(0, eq)), urlDecode(item.substr(eq + 1)));
        }

        if (amp == std::string::npos) {
            break;
        }

        pos = amp + 1;
    }

    return out;
}

std::string findValue(const Pairs& pairs, const char* key)
{
    for (const auto& kv : pairs) {
        if (kv.first == key) {
            return kv.second;
        }
    }

    return "";
}

// 从 multipart/form-data 里取字段（回调可能是 multipart）
bool parseMultipart(const std::string& body, const std::string& contentType, Pairs& out)
{
    size_t bpos = contentType.find("boundary=");

    if (bpos == std::string::npos) {
        return false;
    }

    std::string boundary = contentType.substr(bpos + strlen("boundary="));

    if (!boundary.empty() && boundary.front() == '"') {
        boundary = boundary.substr(1, boundary.find('"', 1) - 1);
    }

    std::string delim = "--" + boundary;
    size_t pos = 0;

    while (true) {
        size_t start = body.find(delim, pos);

        if (start == std::string::npos) {
            break;
        }

        size_t headEnd = body.find("\r\n\r\n", start);

        if (headEnd == std::string::npos) {
            break;
        }

        size_t next = body.find(delim, headEnd);

        if (next == std::string::npos) {
            break;
        }

        std::string head = body.substr(start, headEnd - start);
        std::string name;
        size_t np = head.find("name=\"");

        if (np != std::string::npos) {
            size_t end = head.find('"', np + 6);
            name = head.substr(np + 6, end - np - 6);
        }

        std::string value = body.substr(headEnd + 4, next - headEnd - 4);

        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
        }

        if (!name.empty()) {
            out.emplace_back(name, value);
        }

        pos = next;
    }

    return true;
}

// base64url 解码（JWT payload）
bool base64UrlDecode(const std::string& in, std::string& out)
{
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;

    for (char c : in) {
        if (c == '-') {
            b64 += '+';
        } else if (c == '_') {
            b64 += '/';
        } else {
            b64 += c;
        }
    }

    while (b64.size() % 4 != 0) {
        b64 += '=';
    }

    int val = 0;
    int bits = 0;

    for (char c : b64) {
        if (c == '=') {
            break;
        }

        size_t idx = alphabet.find(c);

        if (idx == std::string::npos) {
            return false;
        }

        val = (val << 6) | static_cast<int>(idx);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((val >> bits) & 0xFF);
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

// 尝试用系统浏览器打开 URL；失败返回 false（调用方打印链接让用户手动打开）
bool openBrowser(const std::string& url)
{
    const char* candidates[] = {"xdg-open", "open"};

    for (const char* prog : candidates) {
        pid_t pid = fork();

        if (pid < 0) {
            continue;
        }

        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            execlp(prog, prog, url.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        int status = 0;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return true;
        }
    }

    return false;
}

} // namespace

// ─────────────────────────── 登录回调服务 ───────────────────────────

std::string loginWithBrowser(const std::string& siteId, int timeoutSec, std::string& error)
{
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);

    if (listenFd < 0) {
        error = "cannot create listen socket";
        return "";
    }

    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || listen(listenFd, 4) != 0) {
        error = "cannot bind loopback port";
        close(listenFd);
        return "";
    }

    socklen_t len = sizeof(addr);
    getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);

    std::string csrf = randomHex(16);
    std::string loginUrl = std::string(kBaseUrl) + kLoginAuthPath
                         + "?port=" + std::to_string(port)
                         + "&appid=" + kAppId
                         + "&code=" + csrf;

    std::cout << "登录地址（请在浏览器中完成华为开发者账号登录）：\n  "
              << loginUrl << "\n";

    if (!openBrowser(loginUrl)) {
        std::cout << "（未能自动打开浏览器，请手动复制上面的地址）\n";
    }

    std::cout << "等待回调（最多 " << timeoutSec << " 秒）…\n";
    std::cout.flush();

    std::string tempToken;
    time_t deadline = time(nullptr) + timeoutSec;

    while (tempToken.empty()) {
        time_t remain = deadline - time(nullptr);

        if (remain <= 0) {
            error = "等待登录回调超时";
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenFd, &rfds);
        timeval tv{};
        tv.tv_sec = static_cast<long>(remain < 5 ? remain : 5);

        int ready = select(listenFd + 1, &rfds, nullptr, nullptr, &tv);

        if (ready <= 0) {
            continue;
        }

        int connFd = accept(listenFd, nullptr, nullptr);

        if (connFd < 0) {
            continue;
        }

        // 读取整个请求（本地回调很小，一次读完；循环兜底）
        std::string raw;
        char buf[4096];
        ssize_t n = 0;

        while ((n = read(connFd, buf, sizeof(buf))) > 0) {
            raw.append(buf, static_cast<size_t>(n));

            if (raw.find("\r\n\r\n") != std::string::npos) {
                size_t headEnd = raw.find("\r\n\r\n");
                std::string lower = raw.substr(0, headEnd);
                for (char& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }

                size_t clPos = lower.find("content-length:");
                size_t haveBody = raw.size() - (headEnd + 4);

                if (clPos == std::string::npos) {
                    break;
                }

                size_t want = static_cast<size_t>(
                    std::atoi(raw.c_str() + clPos + strlen("content-length:")));

                if (haveBody >= want) {
                    break;
                }
            }
        }

        // 请求行 → query
        Pairs params;
        size_t firstSpace = raw.find(' ');
        size_t secondSpace = firstSpace == std::string::npos
                           ? std::string::npos : raw.find(' ', firstSpace + 1);

        if (secondSpace != std::string::npos) {
            std::string target = raw.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            size_t q = target.find('?');

            if (q != std::string::npos) {
                params = parseQuery(target.substr(q + 1));
            }
        }

        // 请求头 + body（multipart / urlencoded 都试）
        size_t headEnd = raw.find("\r\n\r\n");
        std::string contentType;
        std::string body;

        if (headEnd != std::string::npos) {
            std::string head = raw.substr(0, headEnd);
            body = raw.substr(headEnd + 4);

            std::string lower = head;
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            size_t ct = lower.find("content-type:");

            if (ct != std::string::npos) {
                size_t end = lower.find("\r\n", ct);
                contentType = head.substr(ct + strlen("content-type:"),
                                          end - ct - strlen("content-type:"));
            }
        }

        if (!body.empty()) {
            Pairs bodyParams;

            if (contentType.find("multipart/form-data") != std::string::npos) {
                parseMultipart(body, contentType, bodyParams);
            } else {
                bodyParams = parseQuery(body);
            }

            for (const auto& kv : bodyParams) {
                params.push_back(kv);
            }
        }

        std::string token = findValue(params, "tempToken");
        std::string code = findValue(params, "code");

        if (token.empty() && code.empty()) {
            // 探活请求
            const char* ready = "haptool callback server is ready\n";
            std::string resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n")
                             + "Content-Length: " + std::to_string(std::strlen(ready))
                             + "\r\nConnection: close\r\n\r\n" + ready;
            (void)!write(connFd, resp.data(), resp.size());
            close(connFd);
            continue;
        }

        if (code != csrf) {
            const char* bad = "CSRF code mismatch\n";
            std::string resp = std::string("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n")
                             + "Content-Length: " + std::to_string(std::strlen(bad))
                             + "\r\nConnection: close\r\n\r\n" + bad;
            (void)!write(connFd, resp.data(), resp.size());
            close(connFd);
            continue;
        }

        if (token.empty()) {
            error = "回调里没有 tempToken，授权可能被取消";
            close(connFd);
            break;
        }

        tempToken = token;

        // 回一个和 DevEco 一致的落地页
        std::string location = std::string(kBaseUrl) + kLoginSuccessPath;
        std::string resp = "HTTP/1.1 302 Found\r\nLocation: " + location
                         + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)!write(connFd, resp.data(), resp.size());
        close(connFd);
    }

    close(listenFd);
    (void)siteId;   // 站点在换 token 时使用
    return tempToken;
}

bool exchangeTempToken(const std::string& tempToken, const std::string& siteId,
                       AuthToken& out, std::string& error)
{
    Pairs params = {
        {"tempToken", tempToken},
        {"site", siteId},
        {"version", kLoginVersion},
        {"appid", kAppId},
    };

    HttpResponse resp = httpGet(std::string(kBaseUrl) + kTempTokenCheckPath, {}, params);

    if (resp.status != 200 || resp.body.empty()) {
        error = "tempToken 校验失败（HTTP " + std::to_string(resp.status) + "）："
              + (resp.error.empty() ? resp.body.substr(0, 300) : resp.error);
        return false;
    }

    // 响应正文本身就是 jwtToken 纯文本
    std::string jwt = resp.body;
    while (!jwt.empty() && (jwt.back() == '\n' || jwt.back() == '\r' || jwt.back() == ' ')) {
        jwt.pop_back();
    }

    HttpResponse checked = httpGet(std::string(kBaseUrl) + kJwtTokenCheckPath,
                                   {{"jwtToken", jwt}, {"refresh", "false"}}, {});

    if (checked.status != 200) {
        error = "jwtToken 校验失败（HTTP " + std::to_string(checked.status) + "）";
        return false;
    }

    cJSON* root = cJSON_Parse(checked.body.c_str());

    if (!root) {
        error = "jwtToken 校验返回非 JSON";
        return false;
    }

    bool ok = cJSON_IsTrue(cJSON_GetObjectItem(root, "status"));
    const cJSON* info = cJSON_GetObjectItem(root, "userInfo");

    if (!ok || !info) {
        cJSON_Delete(root);
        error = "登录状态校验失败（status=false）";
        return false;
    }

    if (cJSON_GetObjectItem(info, "realName") &&
        !cJSON_IsTrue(cJSON_GetObjectItem(info, "realName"))) {
        std::cout << "警告：该账号可能未完成实名认证，签名大概率会被服务端拒绝\n";
    }

    out.jwt = jwt;
    out.accessToken = jsonStr(info, "accessToken");
    out.refreshToken = jsonStr(info, "refreshToken");
    out.siteId = siteId;

    // userId / userName 在 JWT payload 里
    size_t dot1 = jwt.find('.');
    size_t dot2 = dot1 == std::string::npos ? std::string::npos : jwt.find('.', dot1 + 1);

    if (dot1 != std::string::npos && dot2 != std::string::npos) {
        std::string payload;
        if (base64UrlDecode(jwt.substr(dot1 + 1, dot2 - dot1 - 1), payload)) {
            cJSON* claims = cJSON_Parse(payload.c_str());
            if (claims) {
                out.uid = jsonStr(claims, "userId");
                out.userName = jsonStr(claims, "userName");
                cJSON_Delete(claims);
            }
        }
    }

    cJSON_Delete(root);

    if (out.accessToken.empty() || out.uid.empty()) {
        error = "登录信息不完整（缺 accessToken 或 userId）";
        return false;
    }

    return true;
}

bool refreshAccessToken(AuthToken& token, std::string& error)
{
    HttpResponse resp = httpGet(std::string(kBaseUrl) + kJwtTokenCheckPath,
                                {{"jwtToken", token.jwt}, {"refresh", "true"}}, {});

    if (resp.status != 200) {
        error = "accessToken 刷新失败（HTTP " + std::to_string(resp.status) + "）";
        return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());

    if (!root) {
        error = "accessToken 刷新返回非 JSON";
        return false;
    }

    std::string access = jsonStr(cJSON_GetObjectItem(root, "userInfo"), "accessToken");
    cJSON_Delete(root);

    if (access.empty()) {
        error = "accessToken 刷新结果为空，请重新 sign login";
        return false;
    }

    token.accessToken = access;
    return true;
}

// ─────────────────────────── Cloud ───────────────────────────

Cloud::Cloud(AuthToken token) : token_(std::move(token)) {}

std::string Cloud::url(const std::string& path) const
{
    if (path.compare(0, 5, "/auth") == 0) {
        return std::string(kBaseUrl) + path;
    }

    return std::string(kCloudBaseUrl) + path;
}

bool Cloud::request(const std::string& method, const std::string& path,
                    const std::string& body, const std::string& contentType,
                    std::string& out, std::string& error)
{
    Pairs headers = {
        {"oauth2Token", token_.accessToken},
        {"uid", token_.uid},
        {"Accept-Language", "zh-CN"},
    };

    if (!token_.teamId.empty()) {
        headers.emplace_back("teamId", token_.teamId);
    }

    auto send = [&]() {
        if (method == "GET") {
            return httpGet(url(path), headers, {});
        }
        if (contentType == "application/json") {
            return httpPostJson(url(path), headers, body);
        }
        return httpPostForm(url(path), headers, parseQuery(body));
    };

    HttpResponse resp = send();

    if (resp.status == 401 || resp.body.find("\"code\":4000") != std::string::npos) {
        if (!refreshAccessToken(token_, error)) {
            return false;
        }
        resp = send();
    }

    if (resp.status != 200) {
        error = method + " " + path + " failed (HTTP " + std::to_string(resp.status) + "): "
              + (resp.error.empty() ? resp.body.substr(0, 300) : resp.error);
        return false;
    }

    out = resp.body;
    return true;
}

bool Cloud::fetchTeamId(std::string& error)
{
    std::string body;

    if (!request("GET", kApiUserTeam, "", "", body, error)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* teams = root ? cJSON_GetObjectItem(root, "teams") : nullptr;

    if (!teams || !cJSON_IsArray(teams) || cJSON_GetArraySize(teams) == 0) {
        if (root) cJSON_Delete(root);
        error = "账号下没有任何团队，请先在华为开发者联盟完成开发者注册/实名认证";
        return false;
    }

    const cJSON* first = cJSON_GetArrayItem(teams, 0);
    const cJSON* id = cJSON_GetObjectItem(first, "id");

    if (id && cJSON_IsString(id) && id->valuestring) {
        token_.teamId = id->valuestring;
    } else if (id && cJSON_IsNumber(id)) {
        token_.teamId = std::to_string(id->valueint);
    }

    cJSON_Delete(root);
    return !token_.teamId.empty();
}

bool Cloud::listCerts(std::vector<CertEntry>& out, std::string& error)
{
    std::string body;

    if (!request("GET", kApiCertList, "", "", body, error)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* list = root ? cJSON_GetObjectItem(root, "certList") : nullptr;
    const cJSON* item = nullptr;

    out.clear();

    cJSON_ArrayForEach(item, list) {
        CertEntry entry;
        entry.certName = jsonStr(item, "certName");

        const cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id && cJSON_IsNumber(id)) {
            entry.id = std::to_string(id->valueint);
        } else {
            entry.id = jsonStr(item, "id");
        }

        entry.objectId = jsonStr(item, "certObjectId");
        out.push_back(entry);
    }

    if (root) {
        cJSON_Delete(root);
    }

    return true;
}

bool Cloud::addCert(const std::string& csr, const std::string& certName, std::string& error)
{
    std::string body = "csr=" + urlEncode(csr)
                     + "&certName=" + urlEncode(certName)
                     + "&certType=1";
    std::string out;

    if (!request("POST", kApiCertAdd, body, "", out, error)) {
        return false;
    }

    if (out.find("\"code\":0") == std::string::npos) {
        if (out.find("205389872") != std::string::npos) {
            error = "调试证书数量已达账号上限，请到开发者联盟删除旧证书后重试";
        } else {
            error = "申请调试证书失败：" + out.substr(0, 400);
        }
        return false;
    }

    return true;
}

bool Cloud::listDevices(std::vector<DeviceEntry>& out, std::string& error)
{
    std::string body;

    if (!request("GET", std::string(kApiDeviceList) + "?encodeFlag=0&start=1&pageSize=100",
                 "", "", body, error)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* list = root ? cJSON_GetObjectItem(root, "list") : nullptr;
    const cJSON* item = nullptr;

    out.clear();

    cJSON_ArrayForEach(item, list) {
        DeviceEntry entry;
        entry.udid = jsonStr(item, "udid");

        const cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id && cJSON_IsNumber(id)) {
            entry.id = std::to_string(id->valueint);
        } else {
            entry.id = jsonStr(item, "id");
        }

        out.push_back(entry);
    }

    if (root) {
        cJSON_Delete(root);
    }

    return true;
}

bool Cloud::addDevice(const std::string& udid, const std::string& deviceType,
                      const std::string& deviceName, std::string& error)
{
    std::string body = "deviceName=" + urlEncode(deviceName)
                     + "&udid=" + urlEncode(udid)
                     + "&deviceType=" + urlEncode(deviceType);
    std::string out;

    if (!request("POST", kApiDeviceAdd, body, "", out, error)) {
        return false;
    }

    // 已注册视为成功
    if (out.find("205389857") != std::string::npos
        || out.find("205389858") != std::string::npos) {
        return true;
    }

    if (out.find("\"code\":0") == std::string::npos
        && out.find("\"ret\":{\"code\":0") == std::string::npos) {
        error = "注册调试设备失败：" + out.substr(0, 400);
        return false;
    }

    return true;
}

bool Cloud::addTestProvision(const std::string& bundle, const std::string& certId,
                             const std::vector<std::string>& deviceIds,
                             const std::vector<std::string>& aclPermissions,
                             std::string& fileUrl, std::string& profileId,
                             std::string& error)
{
    std::string json = "{\"certList\":[\"" + jsonEscape(certId) + "\"],"
                       "\"packageName\":\"" + jsonEscape(bundle) + "\","
                       "\"provisionName\":\"" + jsonEscape(bundle) + "\","
                       "\"deviceList\":[";

    for (size_t i = 0; i < deviceIds.size(); ++i) {
        if (i) {
            json += ',';
        }
        json += "\"" + jsonEscape(deviceIds[i]) + "\"";
    }

    json += "],\"aclPermissionList\":[";

    for (size_t i = 0; i < aclPermissions.size(); ++i) {
        if (i) {
            json += ',';
        }
        json += "\"" + jsonEscape(aclPermissions[i]) + "\"";
    }

    json += "]}";

    std::string out;

    if (!request("POST", kApiTestProvisionAdd, json, "application/json", out, error)) {
        return false;
    }

    if (out.find("\"code\":0") == std::string::npos) {
        if (out.find("205389938") != std::string::npos
            || out.find("205389845") != std::string::npos) {
            error = "调试 Profile 数量已达账号上限，请到开发者联盟删除旧 Profile";
        } else {
            error = "申请调试 Profile 失败：" + out.substr(0, 400);
        }
        return false;
    }

    cJSON* root = cJSON_Parse(out.c_str());

    if (!root) {
        error = "Profile 响应不是 JSON";
        return false;
    }

    const cJSON* target = cJSON_GetObjectItem(root, "provisionInfo");

    if (!target) {
        target = root;
    }

    fileUrl = jsonStr(target, "provisionFileUrl");

    const cJSON* id = cJSON_GetObjectItem(target, "id");

    if (id && cJSON_IsNumber(id)) {
        profileId = std::to_string(id->valueint);
    } else {
        profileId = jsonStr(target, "id");
    }

    cJSON_Delete(root);

    if (fileUrl.empty()) {
        error = "Profile 响应里没有 provisionFileUrl";
        return false;
    }

    return true;
}

bool Cloud::downloadObject(const std::string& sourceUrl, const std::string& destPath,
                           std::string& error)
{
    std::string out;

    if (!request("POST", kApiReapply, "sourceUrls=" + urlEncode(sourceUrl), "", out, error)) {
        return false;
    }

    std::string newUrl;
    cJSON* root = cJSON_Parse(out.c_str());
    const cJSON* list = root ? cJSON_GetObjectItem(root, "urlsInfo") : nullptr;
    const cJSON* first = list && cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : nullptr;

    if (first) {
        newUrl = jsonStr(first, "newUrl");
    }

    if (root) {
        cJSON_Delete(root);
    }

    if (newUrl.empty()) {
        error = "获取下载地址失败：" + out.substr(0, 300);
        return false;
    }

    // 直链不再需要认证头
    return httpDownload(newUrl, destPath, error);
}

} // namespace aht
