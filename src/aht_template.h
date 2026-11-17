/*
 * aht_template — 生成一个最小的 C++（Native/NAPI）HAP 工程模板。
 *
 * 生成的工程可直接用 hvigor 构建（hvigorw assembleHap），
 * ArkTS 侧通过 libentry.so 调 native add()，验证 NAPI 链路是通的。
 *
 * 模板全部内嵌在二进制里（不依赖仓里的 templates/ 目录），
 * 这样 haptool + 两个 .so 拷到设备上也能生成工程。
 */
#ifndef AHT_TEMPLATE_H
#define AHT_TEMPLATE_H

#include <string>

namespace aht {

struct TemplateOptions {
    std::string path;          // 目标目录（必填）
    std::string bundle;        // bundleName
    std::string apiVersion;    // 纯数字或 "x.y.z(n)"
    std::string moduleName;    // entry
    std::string appName;       // 显示名
    std::string modelVersion;  // oh-package modelVersion
    bool force = false;        // 目标目录非空时仍写入
};

// 纯数字 API（如 26）→ build-profile 认的 "platformVersion(api)"；空串取 SDK 默认
std::string normalizeApiVersion(const std::string& given);

// 生成工程；fileCount 返回写出的文件数
bool createProject(const TemplateOptions& opts, int& fileCount, std::string& error);

} // namespace aht

#endif /* AHT_TEMPLATE_H */
