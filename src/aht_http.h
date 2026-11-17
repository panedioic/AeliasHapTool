/*
 * aht_http — 最小 HTTPS 客户端（OpenSSL + 阻塞 socket，HTTP/1.1）。
 *
 * 只为华为开发者接口服务：GET/POST、表单或 JSON、Connection: close、
 * 支持 Content-Length / chunked / 读到 EOF 三种响应体形态。请求一律带
 * Accept-Encoding: identity，避免引入 gzip 解压依赖。
 *
 * 不引入 libcurl 的原因：hap tool 要能静态交叉编到 aarch64-musl（设备内运行），
 * 而 libssl/libcrypto 已经是现有依赖。
 */
#ifndef AHT_HTTP_H
#define AHT_HTTP_H

#include <string>
#include <vector>

namespace aht {

struct HttpResponse {
    int status = 0;          // HTTP 状态码；0 = 传输层失败（见 error）
    std::string body;
    std::string error;       // 传输层错误描述
};

// key=value 的有序集合；表单与请求头都用它
using Pairs = std::vector<std::pair<std::string, std::string>>;

std::string urlEncode(const std::string& raw);
std::string jsonEscape(const std::string& raw);

// params 非空时按 key=value&... 拼到 query string
HttpResponse httpGet(const std::string& url, const Pairs& headers,
                     const Pairs& params, int timeoutSec = 25);
HttpResponse httpPostForm(const std::string& url, const Pairs& headers,
                          const Pairs& form, int timeoutSec = 25);
HttpResponse httpPostJson(const std::string& url, const Pairs& headers,
                          const std::string& json, int timeoutSec = 25);

// 下载到文件（不跟随重定向；用于签名材料直链）
bool httpDownload(const std::string& url, const std::string& destPath,
                  std::string& error, int timeoutSec = 60);

} // namespace aht

#endif /* AHT_HTTP_H */
