#include "aht_http.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

namespace aht {

namespace {

struct Url {
    bool tls = true;
    std::string host;
    int port = 443;
    std::string target = "/";   // path + query
    std::string error;
};

Url parseUrl(const std::string& url)
{
    Url out;

    if (url.compare(0, 8, "https://") == 0) {
        out.tls = true;
        out.port = 443;
    } else if (url.compare(0, 7, "http://") == 0) {
        out.tls = false;
        out.port = 80;
    } else {
        out.error = "unsupported URL scheme: " + url;
        return out;
    }

    size_t schemeLen = out.tls ? 8 : 7;
    size_t slash = url.find('/', schemeLen);
    std::string authority = url.substr(schemeLen,
        slash == std::string::npos ? std::string::npos : slash - schemeLen);

    size_t colon = authority.rfind(':');

    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.c_str() + colon + 1);
    } else {
        out.host = authority;
    }

    if (slash != std::string::npos) {
        out.target = url.substr(slash);
    }

    if (out.host.empty()) {
        out.error = "URL has no host: " + url;
    }

    return out;
}

int connectTcp(const Url& u, int timeoutSec, std::string& error)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", u.port);

    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(u.host.c_str(), portStr, &hints, &res);

    if (rc != 0 || !res) {
        error = "cannot resolve " + u.host + ": " + gai_strerror(rc);
        return -1;
    }

    int fd = -1;

    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (fd < 0) {
            continue;
        }

        struct timeval tv;
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        error = "cannot connect to " + u.host + ":" + std::to_string(u.port)
              + ": " + std::strerror(errno);
    }

    return fd;
}

// CA 路径：宿主标准位置 + OHOS 系统证书库。AHT_INSECURE_TLS=1 时不校验。
SSL_CTX* makeContext(std::string& error)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());

    if (!ctx) {
        error = "SSL_CTX_new failed";
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    const char* insecure = std::getenv("AHT_INSECURE_TLS");

    if (insecure && std::strcmp(insecure, "1") == 0) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        return ctx;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ctx);

    static const char* kCaDirs[] = {
        "/etc/ssl/certs",
        "/system/etc/security/cacerts",
        "/usr/local/share/certs",
    };

    for (const char* dir : kCaDirs) {
        if (access(dir, R_OK) == 0) {
            SSL_CTX_load_verify_locations(ctx, nullptr, dir);
        }
    }

    return ctx;
}

bool sendAll(int fd, const std::string& data)
{
    size_t sent = 0;

    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);

        if (n <= 0) {
            return false;
        }

        sent += static_cast<size_t>(n);
    }

    return true;
}

// 长度前缀的 chunked 解码；失败返回 false
bool decodeChunked(const std::string& raw, std::string& out)
{
    size_t pos = 0;

    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);

        if (eol == std::string::npos) {
            return false;
        }

        std::string sizeLine = raw.substr(pos, eol - pos);
        size_t semi = sizeLine.find(';');

        if (semi != std::string::npos) {
            sizeLine = sizeLine.substr(0, semi);
        }

        unsigned long len = std::strtoul(sizeLine.c_str(), nullptr, 16);
        pos = eol + 2;

        if (len == 0) {
            return true;
        }

        if (pos + len > raw.size()) {
            return false;
        }

        out.append(raw, pos, len);
        pos += len + 2;   // 跳过 chunk 末尾的 CRLF
    }

    return true;
}

HttpResponse doRequest(const std::string& method, const std::string& url,
                       const Pairs& headers, const Pairs& params,
                       const std::string& body, const std::string& contentType,
                       int timeoutSec)
{
    HttpResponse out;

    Url u = parseUrl(url);

    if (!u.error.empty()) {
        out.error = u.error;
        return out;
    }

    std::string target = u.target;

    if (!params.empty()) {
        std::string q;
        bool hasQuery = target.find('?') != std::string::npos;

        for (const auto& kv : params) {
            if (!q.empty() || hasQuery) {
                q += '&';
            }
            if (q.empty() && !hasQuery) {
                q += '?';
            }
            q += urlEncode(kv.first) + "=" + urlEncode(kv.second);
            hasQuery = true;
        }

        target += q;
    }

    int fd = connectTcp(u, timeoutSec, out.error);

    if (fd < 0) {
        return out;
    }

    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;

    if (u.tls) {
        ctx = makeContext(out.error);

        if (!ctx) {
            close(fd);
            return out;
        }

        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, u.host.c_str());
        SSL_set1_host(ssl, u.host.c_str());

        if (SSL_connect(ssl) != 1) {
            long verify = SSL_get_verify_result(ssl);
            if (verify != X509_V_OK) {
                out.error = std::string("TLS certificate verify failed: ")
                          + X509_verify_cert_error_string(verify);
            } else {
                out.error = "TLS handshake failed";
            }
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(fd);
            return out;
        }
    }

    std::string req = method + " " + target + " HTTP/1.1\r\n";
    req += "Host: " + u.host + "\r\n";
    req += "User-Agent: haptool/1.0\r\n";
    req += "Accept: */*\r\n";
    req += "Accept-Encoding: identity\r\n";
    req += "Connection: close\r\n";

    for (const auto& kv : headers) {
        req += kv.first + ": " + kv.second + "\r\n";
    }

    if (!body.empty() || method == "POST") {
        if (!contentType.empty()) {
            req += "Content-Type: " + contentType + "\r\n";
        }
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }

    req += "\r\n";
    req += body;

    bool sent = false;

    if (u.tls) {
        size_t off = 0;
        sent = true;

        while (off < req.size()) {
            int n = SSL_write(ssl, req.data() + off,
                              static_cast<int>(req.size() - off));
            if (n <= 0) {
                sent = false;
                break;
            }
            off += static_cast<size_t>(n);
        }
    } else {
        sent = sendAll(fd, req);
    }

    if (!sent) {
        out.error = "failed to send request";
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        close(fd);
        return out;
    }

    std::string raw;
    char buf[8192];

    for (;;) {
        int n = u.tls ? SSL_read(ssl, buf, sizeof(buf))
                      : static_cast<int>(read(fd, buf, sizeof(buf)));

        if (n <= 0) {
            break;
        }

        raw.append(buf, static_cast<size_t>(n));
    }

    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ctx) {
        SSL_CTX_free(ctx);
    }
    close(fd);

    size_t headerEnd = raw.find("\r\n\r\n");

    if (headerEnd == std::string::npos) {
        out.error = "malformed HTTP response";
        return out;
    }

    std::string head = raw.substr(0, headerEnd);
    std::string payload = raw.substr(headerEnd + 4);

    // 状态行：HTTP/1.1 200 OK
    size_t sp = head.find(' ');
    out.status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);

    std::string lowerHead = head;
    for (char& c : lowerHead) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lowerHead.find("\r\ntransfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        if (decodeChunked(payload, decoded)) {
            payload.swap(decoded);
        }
    }

    out.body.swap(payload);
    return out;
}

} // namespace

std::string urlEncode(const std::string& raw)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;

    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }

    return out;
}

std::string jsonEscape(const std::string& raw)
{
    std::string out;

    for (char c : raw) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }

    return out;
}

HttpResponse httpGet(const std::string& url, const Pairs& headers,
                     const Pairs& params, int timeoutSec)
{
    return doRequest("GET", url, headers, params, "", "", timeoutSec);
}

HttpResponse httpPostForm(const std::string& url, const Pairs& headers,
                          const Pairs& form, int timeoutSec)
{
    std::string body;

    for (const auto& kv : form) {
        if (!body.empty()) {
            body += '&';
        }
        body += urlEncode(kv.first) + "=" + urlEncode(kv.second);
    }

    return doRequest("POST", url, headers, {}, body,
                     "application/x-www-form-urlencoded", timeoutSec);
}

HttpResponse httpPostJson(const std::string& url, const Pairs& headers,
                          const std::string& json, int timeoutSec)
{
    return doRequest("POST", url, headers, {}, json, "application/json", timeoutSec);
}

bool httpDownload(const std::string& url, const std::string& destPath,
                  std::string& error, int timeoutSec)
{
    HttpResponse resp = httpGet(url, {}, {}, timeoutSec);

    if (resp.status != 200 || resp.body.empty()) {
        error = resp.error.empty()
            ? "download failed, HTTP " + std::to_string(resp.status)
            : resp.error;
        return false;
    }

    std::string tmp = destPath + ".part";
    FILE* fp = std::fopen(tmp.c_str(), "wb");

    if (!fp) {
        error = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }

    size_t written = std::fwrite(resp.body.data(), 1, resp.body.size(), fp);
    std::fclose(fp);

    if (written != resp.body.size()) {
        std::remove(tmp.c_str());
        error = "short write to " + tmp;
        return false;
    }

    if (std::rename(tmp.c_str(), destPath.c_str()) != 0) {
        std::remove(tmp.c_str());
        error = "cannot rename " + tmp;
        return false;
    }

    return true;
}

} // namespace aht
