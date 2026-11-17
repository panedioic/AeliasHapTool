/*
 * haptool — HAP 开发助手。
 *
 * 用法：
 *   haptool                  无参数 → 进入交互式 REPL
 *   haptool <cmd> [args...]  有参数 → 执行这一条命令后退出（退出码可脚本判断）
 *
 * 能力：
 *   device  连接/列设备/执行 shell          —— libhdc.so（进程内）
 *   hap     签名/安装/卸载/打开/生成密钥     —— libhapsigner.so + libhdc.so（进程内）
 *   sign    华为开发者账号登录与调试签名材料 —— 自包含 HTTPS 客户端
 *
 * 签名材料（p12/证书/Profile）全部落在 $AHT_HOME/profiles/<bundle>/ 下，
 * 一次登录后同包名/同设备可反复签名；整个 AHT_HOME 可拷到其他设备免登录使用。
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <filesystem>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "hdc_entry.h"
#include "hapsigner_entry.h"
#include "cJSON.h"

#include "aht_cloud.h"
#include "aht_hap.h"
#include "aht_sign.h"
#include "aht_store.h"

namespace fs = std::filesystem;

// ============================================================
// In-process Libraries (hdc / hapsigner)
// ============================================================
//
// Each command runs in a forked child calling the library entry; the parent
// REPL stays alive and untouched. argv[0] is skipped as the program name.

struct ProcessResult {
    int exitCode = -1;
    std::string stdoutText;
};

using LibEntry = int (*)(int, const char**);

// 单次执行模式的进程退出码；REPL 模式不使用
static int g_lastExitCode = 0;

ProcessResult runIsolated(LibEntry entry, const std::string& progName,
                          const std::vector<std::string>& args) {
    ProcessResult result;

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        throw std::runtime_error("Failed to create pipe");
    }

    std::vector<const char*> argv;
    argv.push_back(progName.c_str());

    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }

    argv.push_back(nullptr);

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("Failed to fork process");
    }

    if (pid == 0) {
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(pipefd[1]);

        int ret = entry(static_cast<int>(argv.size() - 1), argv.data());

        // _exit 不跑 atexit/静态析构，先冲掉 stdio 缓冲
        std::cout.flush();
        std::cerr.flush();
        fflush(nullptr);
        _exit(ret);
    }

    close(pipefd[1]);

    char buffer[4096];
    ssize_t bytesRead;

    while ((bytesRead = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        result.stdoutText.append(buffer, bytesRead);
    }

    close(pipefd[0]);

    int status = 0;

    if (waitpid(pid, &status, 0) == -1) {
        throw std::runtime_error("Failed to wait for process");
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }

    g_lastExitCode = result.exitCode;
    return result;
}

ProcessResult runHdc(const std::vector<std::string>& args) {
    return runIsolated(HdcLibMain, "hdc", args);
}

ProcessResult runSigner(const std::vector<std::string>& args) {
    return runIsolated(HapSignerLibMain, "hap-sign-tool", args);
}

// aht_sign 需要的回调形态
int signerRunner(const std::vector<std::string>& args, std::string& output) {
    ProcessResult r = runSigner(args);
    output = r.stdoutText;
    return r.exitCode;
}

int hdcRunner(const std::vector<std::string>& args, std::string& output) {
    ProcessResult r = runHdc(args);
    output = r.stdoutText;
    return r.exitCode;
}

void printResult(const ProcessResult& result) {
    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "Command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

// ============================================================
// Application State
// ============================================================

static std::vector<std::string> DEVICES;

// ============================================================
// Home Directory
// ============================================================

static std::string AHT_HOME;

void initAhtHome() {
    const char* env = std::getenv("AHT_HOME");

    if (env && env[0] != '\0') {
        AHT_HOME = env;
        return;
    }

    const char* home = std::getenv("HOME");

    if (home && home[0] != '\0') {
        AHT_HOME = std::string(home) + "/.ahthome";
        return;
    }

    AHT_HOME = ".";
    std::cerr << "Warning: neither AHT_HOME nor HOME is set, "
              << "AHT_HOME falls back to cwd\n";
}

// ============================================================
// Argument Parsing
// ============================================================

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    bool has(const std::string& name) const { return options.count(name) > 0; }

    std::string get(const std::string& name, const std::string& def = "") const {
        auto it = options.find(name);
        return it == options.end() ? def : it->second;
    }
};

Args parseArgs(const std::vector<std::string>& tokens) {
    Args out;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];

        if (t.size() > 2 && t[0] == '-' && t[1] == '-') {
            std::string name = t.substr(2);

            // --name=value 或 --name value
            size_t eq = name.find('=');

            if (eq != std::string::npos) {
                out.options[name.substr(0, eq)] = name.substr(eq + 1);
            } else if (i + 1 < tokens.size() && tokens[i + 1].compare(0, 2, "--") != 0) {
                out.options[name] = tokens[++i];
            } else {
                out.options[name] = "1";
            }
        } else {
            out.positional.push_back(t);
        }
    }

    return out;
}

// ============================================================
// Input Helpers
// ============================================================

std::string prompt(const std::string& message) {
    std::string line;

    std::cout << message;

    if (!std::getline(std::cin, line)) {
        return "";
    }

    return line;
}

std::string takeOrPrompt(const Args& args, size_t idx, const std::string& message) {
    if (args.positional.size() > idx) {
        return args.positional[idx];
    }

    return prompt(message);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string word;

    while (stream >> word) {
        tokens.push_back(word);
    }

    return tokens;
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream stream(s);
    std::string item;

    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }

    return out;
}

std::string maskId(const std::string& v, size_t keep = 8) {
    return v.size() <= keep ? v : v.substr(0, keep) + "…";
}

// ============================================================
// Command Handlers — Device (hdc)
// ============================================================

void unknownCommand() {
    std::cout << "command not found!\n";
    g_lastExitCode = 1;
}

void showHelp() {
    std::cout << "Usage:\n";
    std::cout << "  haptool                  : interactive REPL\n";
    std::cout << "  haptool <cmd> [args...]  : run one command and exit\n";
    std::cout << "===== sign (华为账号调试签名) =====\n";
    std::cout << "sign login [--country CN] [--token <tempToken>]  : 浏览器登录\n";
    std::cout << "sign logout                      : 清除登录态\n";
    std::cout << "sign status                      : 登录态与材料概览\n";
    std::cout << "sign add [bundle] [hap] [--udid u1,u2]  : 按包名准备证书/设备/Profile\n";
    std::cout << "sign list                        : 已备份材料的包名\n";
    std::cout << "sign info [bundle]               : 材料详情\n";
    std::cout << "sign remove [bundle]             : 删除包名材料\n";
    std::cout << "sign import [id]                 : 导入手工签名配置（keystore/证书/Profile）\n";
    std::cout << "===== hap =====\n";
    std::cout << "hap sign <hap> [bundle] [--out f] : 用 AHT_HOME 材料签名（缺材料先 sign add）\n";
    std::cout << "hap sign-install <hap> [bundle]  : 签名并安装\n";
    std::cout << "hap install <hap> [deviceId]     : 安装\n";
    std::cout << "hap uninstall <bundle> [deviceId]: 卸载\n";
    std::cout << "hap open <bundle> [ability]      : 启动应用\n";
    std::cout << "hap genkey [alias] [out.p12]     : 生成密钥对（不联网）\n";
    std::cout << "hap signer <args...>             : 透传 hap-sign-tool\n";
    std::cout << "===== device =====\n";
    std::cout << "device list                      : 列出设备\n";
    std::cout << "device connect [ip:port]         : tconn 连接\n";
    std::cout << "device shell <command...>        : 设备上执行命令\n";
    std::cout << "===== misc =====\n";
    std::cout << "help / quit\n";
    std::cout << "AHT_HOME=" << AHT_HOME << '\n';
}

void handleDeviceList() {
    ProcessResult result = runHdc({"list", "targets"});

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "Failed to list devices. Exit code: "
                  << result.exitCode << '\n';
    }

    DEVICES.clear();

    std::istringstream stream(result.stdoutText);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.find("[Empty]") == std::string::npos) {
            DEVICES.push_back(line);
        }
    }
}

void handleDeviceConnect(const Args& args) {
    std::string deviceIpHost = takeOrPrompt(args, 0, "Please input device ip and port: ");

    if (deviceIpHost.empty()) {
        return;
    }

    printResult(runHdc({"tconn", deviceIpHost}));
}

void handleDeviceShell(const Args& args) {
    std::string command;

    if (!args.positional.empty()) {
        std::ostringstream joined;

        for (size_t i = 0; i < args.positional.size(); ++i) {
            if (i > 0) {
                joined << ' ';
            }
            joined << args.positional[i];
        }

        command = joined.str();
    } else {
        command = prompt("Please input shell command: ");
    }

    if (command.empty()) {
        return;
    }

    printResult(runHdc({"shell", command}));
}

// ============================================================
// Command Handlers — Device UDID helpers
// ============================================================

// UDID 来源优先级：--udid > meta 缓存（材料仍匹配时）> hdc 读设备
std::vector<std::string> resolveUdids(const Args& args, const std::string& bundle,
                                      std::string& error) {
    if (args.has("udid")) {
        return splitComma(args.get("udid"));
    }

    if (!bundle.empty()) {
        aht::ProfileMeta meta;

        if (aht::loadMeta(AHT_HOME, bundle, meta) && !meta.udids.empty()) {
            return meta.udids;
        }
    }

    std::string udid;

    if (!aht::readDeviceUdid(hdcRunner, udid, error)) {
        return {};
    }

    std::cout << "读取到设备 UDID：" << maskId(udid) << '\n';
    return {udid};
}

// ============================================================
// Command Handlers — sign (cloud materials)
// ============================================================

void handleSignLogin(const Args& args) {
    std::string siteId = args.get("country", "CN");

    aht::AuthToken token;
    std::string error;
    std::string tempToken = args.get("token");

    if (tempToken.empty()) {
        int timeoutSec = 600;

        if (args.has("timeout")) {
            timeoutSec = std::atoi(args.get("timeout").c_str());
        }

        tempToken = aht::loginWithBrowser(siteId, timeoutSec, error);

        if (tempToken.empty()) {
            std::cerr << "登录失败：" << error << '\n';
            g_lastExitCode = 1;
            return;
        }
    }

    if (!aht::exchangeTempToken(tempToken, siteId, token, error)) {
        std::cerr << "登录失败：" << error << '\n';
        g_lastExitCode = 1;
        return;
    }

    // 团队 ID 顺手取一次，后续请求都要带
    aht::Cloud cloud(token);
    std::string teamError;

    if (cloud.fetchTeamId(teamError)) {
        token.teamId = cloud.token().teamId;
    } else {
        std::cerr << "警告：" << teamError << '\n';
    }

    if (!aht::saveAuth(AHT_HOME, token)) {
        std::cerr << "无法写入 " << AHT_HOME << "/auth.json\n";
        g_lastExitCode = 1;
        return;
    }

    std::cout << "登录成功：账号 " << maskId(token.userName.empty() ? token.uid : token.userName)
              << "，团队 ID " << maskId(token.teamId) << '\n';
    std::cout << "登录态已保存到 " << AHT_HOME << "/auth.json\n";
}

void handleSignLogout() {
    if (aht::clearAuth(AHT_HOME)) {
        std::cout << "已清除登录态（签名材料保留在 profiles/ 下）\n";
    } else {
        std::cout << "当前没有登录态\n";
    }
}

void handleSignStatus() {
    aht::AuthToken token;

    if (aht::loadAuth(AHT_HOME, token)) {
        std::cout << "已登录：账号 " << maskId(token.userName.empty() ? token.uid : token.userName)
                  << "，UID " << maskId(token.uid)
                  << "，团队 " << maskId(token.teamId) << '\n';
    } else {
        std::cout << "未登录（sign login 登录华为开发者账号）\n";
    }

    std::vector<std::string> bundles = aht::listBundles(AHT_HOME);

    std::cout << "AHT_HOME：" << AHT_HOME << '\n';

    if (bundles.empty()) {
        std::cout << "签名材料：无\n";
        return;
    }

    std::cout << "签名材料：" << bundles.size() << " 个包名\n";

    for (const auto& bundle : bundles) {
        aht::ProfileMeta meta;

        if (aht::loadMeta(AHT_HOME, bundle, meta)) {
            long long remainDays = meta.profileExpiry > 0
                ? (meta.profileExpiry - static_cast<long long>(time(nullptr))) / 86400
                : -1;

            std::cout << "  " << bundle
                      << "  UDID×" << meta.udids.size()
                      << "  Profile ";

            if (remainDays >= 0) {
                std::cout << "剩余 " << remainDays << " 天";
            } else {
                std::cout << "有效期未知";
            }

            std::cout << '\n';
        }
    }
}

void handleSignAdd(const Args& args) {
    std::string hapPath;
    std::string bundle;

    if (args.positional.size() > 0) {
        // 第一个位置参数是 hap 就当作包路径，否则当包名
        const std::string& first = args.positional[0];

        if (first.size() > 4 && first.compare(first.size() - 4, 4, ".hap") == 0
            && fs::exists(first)) {
            hapPath = first;
        } else {
            bundle = first;
        }
    }

    if (args.positional.size() > 1) {
        hapPath = args.positional[1];
    }

    std::vector<std::string> acl;

    if (!hapPath.empty()) {
        aht::HapInfo info = aht::inspectHap(hapPath);

        if (!info.error.empty()) {
            std::cerr << "读取 hap 失败：" << info.error << '\n';
            g_lastExitCode = 1;
            return;
        }

        if (bundle.empty()) {
            bundle = info.bundleName;
        }

        acl = info.aclPermissions;
        std::cout << "从 hap 读取包名：" << bundle << '\n';

        if (!acl.empty()) {
            std::cout << "需写入 Profile 的 ACL 权限 " << acl.size() << " 项\n";
        }
    }

    if (bundle.empty()) {
        bundle = prompt("Please input bundle name: ");
    }

    if (bundle.empty()) {
        std::cerr << "缺少包名\n";
        g_lastExitCode = 1;
        return;
    }

    std::string error;
    std::vector<std::string> udids = resolveUdids(args, bundle, error);

    if (udids.empty()) {
        std::cerr << error << '\n';
        g_lastExitCode = 1;
        return;
    }

    aht::MaterialsResult result;
    std::string log;

    bool interactive = isatty(STDIN_FILENO) != 0;

    if (!aht::ensureMaterials(AHT_HOME, bundle, acl, udids,
                              args.get("device-type", "4"), interactive,
                              signerRunner, result, log, error)) {
        std::cout << log;
        std::cerr << "准备签名材料失败：" << error << '\n';
        g_lastExitCode = 1;
        return;
    }

    std::cout << log;
    std::cout << "包名 " << bundle << " 的签名材料就绪：\n"
              << "  私钥  " << result.materials.keyP12 << '\n'
              << "  证书  " << result.materials.cert << '\n'
              << "  Profile " << result.materials.profile << '\n';

    if (result.reused) {
        std::cout << "（本地材料可复用，本次未联网）\n";
    }
}

void handleSignList() {
    std::vector<std::string> bundles = aht::listBundles(AHT_HOME);

    if (bundles.empty()) {
        std::cout << "no materials, use 'sign add' to create!\n";
        return;
    }

    for (const auto& bundle : bundles) {
        std::cout << bundle << '\n';
    }
}

void handleSignInfo(const Args& args) {
    std::vector<std::string> bundles = aht::listBundles(AHT_HOME);

    if (bundles.empty()) {
        std::cout << "no materials, use 'sign add' to create!\n";
        return;
    }

    std::string want = args.positional.empty() ? "" : args.positional[0];

    for (const auto& bundle : bundles) {
        if (!want.empty() && bundle != want) {
            continue;
        }

        aht::ProfileMeta meta;

        std::cout << "===== " << bundle << " =====\n";

        if (aht::loadMeta(AHT_HOME, bundle, meta)) {
            std::cout << "证书     : " << meta.certName << " (id " << meta.certId << ")\n";
            std::cout << "UDID     : ";

            for (size_t i = 0; i < meta.udids.size(); ++i) {
                std::cout << (i ? ", " : "") << maskId(meta.udids[i]);
            }

            std::cout << '\n';

            if (meta.profileExpiry > 0) {
                char buf[32] = "";
                std::time_t t = static_cast<std::time_t>(meta.profileExpiry);
                std::tm tm{};
                localtime_r(&t, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
                std::cout << "Profile  : 到期 " << buf << '\n';
            }

            std::cout << "更新     : " << meta.updatedAt << '\n';
        }

        std::cout << "目录     : " << aht::profileDir(AHT_HOME, bundle) << '\n';
    }
}

void handleSignRemove(const Args& args) {
    std::string bundle = takeOrPrompt(args, 0, "Please input bundle name: ");

    if (bundle.empty()) {
        return;
    }

    std::string dir = aht::profileDir(AHT_HOME, bundle);

    if (!fs::exists(dir)) {
        std::cout << "no materials for '" << bundle << "'\n";
        g_lastExitCode = 1;
        return;
    }

    std::error_code ec;
    fs::remove_all(dir, ec);

    if (ec) {
        std::cerr << "删除失败：" << ec.message() << '\n';
        g_lastExitCode = 1;
        return;
    }

    std::cout << "已删除 " << dir << '\n';
}

// ============================================================
// Command Handlers — 手工导入配置（sign import）
// ============================================================

struct SignConfig {
    std::string keystore;
    std::string alias;
    std::string alg;
    std::string profile;
    std::string cert;
};

std::string signConfigPath() {
    return AHT_HOME + "/sign.json";
}

std::string jsonStr(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);

    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    return "";
}

std::map<std::string, SignConfig> loadSignConfigs() {
    std::map<std::string, SignConfig> configs;
    std::ifstream in(signConfigPath());

    if (!in) {
        return configs;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();

    cJSON* root = cJSON_Parse(buffer.str().c_str());

    if (!root) {
        return configs;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!item->string) {
            continue;
        }

        SignConfig cfg;
        cfg.keystore = jsonStr(item, "keystore");
        cfg.alias = jsonStr(item, "alias");
        cfg.alg = jsonStr(item, "alg");
        cfg.profile = jsonStr(item, "profile");
        cfg.cert = jsonStr(item, "cert");
        configs[item->string] = cfg;
    }

    cJSON_Delete(root);
    return configs;
}

void saveSignConfigs(const std::map<std::string, SignConfig>& configs) {
    aht::ensurePrivateDir(AHT_HOME);

    cJSON* root = cJSON_CreateObject();

    for (const auto& [id, cfg] : configs) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "keystore", cfg.keystore.c_str());
        cJSON_AddStringToObject(item, "alias", cfg.alias.c_str());
        cJSON_AddStringToObject(item, "alg", cfg.alg.c_str());
        cJSON_AddStringToObject(item, "profile", cfg.profile.c_str());
        cJSON_AddStringToObject(item, "cert", cfg.cert.c_str());
        cJSON_AddItemToObject(root, id.c_str(), item);
    }

    char* printed = cJSON_Print(root);
    aht::writePrivateFile(signConfigPath(), std::string(printed) + "\n");
    cJSON_free(printed);
    cJSON_Delete(root);
}

void handleSignImport(const Args& args) {
    std::string id = takeOrPrompt(args, 0, "Please input sign config id: ");

    if (id.empty()) {
        return;
    }

    SignConfig cfg;
    cfg.keystore = prompt("Keystore file (.p12): ");
    cfg.alias = prompt("Key alias: ");
    cfg.alg = prompt("Sign algorithm [SHA256withECDSA]: ");
    cfg.profile = prompt("Profile file (.p7b): ");
    cfg.cert = prompt("App cert file (.pem/.cer): ");

    if (cfg.alg.empty()) {
        cfg.alg = "SHA256withECDSA";
    }

    if (cfg.keystore.empty() || cfg.alias.empty()) {
        std::cerr << "Keystore and key alias are required.\n";
        g_lastExitCode = 1;
        return;
    }

    auto configs = loadSignConfigs();
    configs[id] = cfg;
    saveSignConfigs(configs);

    std::cout << "Sign config '" << id << "' saved to " << signConfigPath() << '\n';
}

// ============================================================
// Command Handlers — hap (sign / install / ...)
// ============================================================

void handleHapGenKey(const Args& args) {
    std::string alias = takeOrPrompt(args, 0, "Please input key alias: ");
    std::string keystore = takeOrPrompt(args, 1, "Please input keystore file (.p12): ");

    if (alias.empty() || keystore.empty()) {
        return;
    }

    std::string password = prompt("Password: ");

    if (password.empty()) {
        std::cerr << "Password is required.\n";
        g_lastExitCode = 1;
        return;
    }

    ProcessResult result = runSigner({
        "generate-keypair",
        "-keyAlias", alias,
        "-keyAlg", "ECC", "-keySize", "NIST-P-256",
        "-keystoreFile", keystore,
        "-keyPwd", password, "-keystorePwd", password
    });

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "generate-keypair failed. Exit code: "
                  << result.exitCode << '\n';
        return;
    }

    if (fs::exists(keystore)) {
        std::cout << "Keystore created: " << keystore << '\n';
    }
}

// 手工配置签名（sign import 出来的条目）
bool signWithManualConfig(const SignConfig& cfg, const std::string& hapPath,
                          const std::string& outPath) {
    std::string keyPwd = prompt("Key password: ");
    std::string storePwd = prompt("Keystore password (empty = same as key): ");

    if (storePwd.empty()) {
        storePwd = keyPwd;
    }

    if (keyPwd.empty()) {
        std::cerr << "Password is required.\n";
        g_lastExitCode = 1;
        return false;
    }

    ProcessResult result = runSigner({
        "sign-app",
        "-mode", "localSign",
        "-keyAlias", cfg.alias,
        "-signAlg", cfg.alg,
        "-keystoreFile", cfg.keystore,
        "-keyPwd", keyPwd,
        "-keystorePwd", storePwd,
        "-appCertFile", cfg.cert,
        "-profileFile", cfg.profile,
        "-inFile", hapPath,
        "-outFile", outPath
    });

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "sign-app failed. Exit code: " << result.exitCode << '\n';
        return false;
    }

    return fs::exists(outPath);
}

void handleHapSign(const Args& args, bool installAfter) {
    if (args.positional.empty()) {
        std::cerr << "Usage: hap " << (installAfter ? "sign-install" : "sign")
                  << " <hap> [bundle] [--out file]\n";
        g_lastExitCode = 1;
        return;
    }

    std::string hapPath = args.positional[0];

    if (!fs::exists(hapPath)) {
        std::cerr << "文件不存在：" << hapPath << '\n';
        g_lastExitCode = 1;
        return;
    }

    aht::HapInfo info = aht::inspectHap(hapPath);

    // 解析失败但显式给了包名时继续（有些测试包/裁剪包没有 module.json）
    std::string bundle = args.positional.size() > 1 ? args.positional[1] : info.bundleName;

    if (bundle.empty()) {
        std::cerr << "读取 hap 失败：" << info.error << '\n';
        g_lastExitCode = 1;
        return;
    }

    if (!info.error.empty()) {
        std::cerr << "注意：未能解析 hap 元信息（" << info.error << "），按给定包名继续\n";
    }
    std::string outPath = args.get("out");

    if (outPath.empty()) {
        outPath = aht::defaultSignedPath(hapPath);
    }

    std::cout << "HAP    ：" << hapPath << '\n'
              << "包名   ：" << bundle << '\n';

    if (info.signed_) {
        std::cout << "注意   ：输入已带签名块，将重新签名\n";
    }

    // 1) 手工配置（sign import 的条目）优先：包名不是配置 id 时再看 profiles/
    auto configs = loadSignConfigs();
    auto manual = configs.find(bundle);

    if (manual != configs.end()) {
        if (signWithManualConfig(manual->second, hapPath, outPath)) {
            std::cout << "Signed: " << outPath << '\n';
        } else {
            g_lastExitCode = 1;
        }
    } else {
        // 2) AHT_HOME 自动材料
        std::string error;
        std::vector<std::string> udids = resolveUdids(args, bundle, error);

        if (udids.empty()) {
            std::cerr << error << '\n';
            g_lastExitCode = 1;
            return;
        }

        aht::MaterialsResult materials;
        std::string log;

        bool interactive = isatty(STDIN_FILENO) != 0;

        if (!aht::ensureMaterials(AHT_HOME, bundle, info.aclPermissions, udids,
                                  args.get("device-type", "4"), interactive,
                                  signerRunner, materials, log, error)) {
            std::cout << log;
            std::cerr << "准备签名材料失败：" << error << '\n';
            std::cerr << "提示：先执行 haptool sign login && haptool sign add " << bundle << '\n';
            g_lastExitCode = 1;
            return;
        }

        if (!log.empty()) {
            std::cout << log;
        }

        std::string signedPath;

        if (!aht::signHap(materials.materials, hapPath, outPath, signerRunner,
                          signedPath, log, error)) {
            std::cerr << error << '\n';
            g_lastExitCode = 1;
            return;
        }

        std::cout << "Signed: " << signedPath << '\n';
        outPath = signedPath;
    }

    if (installAfter) {
        std::string deviceId = args.get("device");
        std::vector<std::string> hdcArgs;

        if (!deviceId.empty()) {
            hdcArgs = {"-t", deviceId, "install", "-r", outPath};
        } else {
            hdcArgs = {"install", "-r", outPath};
        }

        ProcessResult result = runHdc(hdcArgs);
        std::cout << result.stdoutText;

        if (result.exitCode != 0
            || result.stdoutText.find("successfully") == std::string::npos) {
            std::cerr << "安装失败\n";
            g_lastExitCode = 1;
        }
    }
}

void handleHapInstall(const Args& args) {
    std::string hap = takeOrPrompt(args, 0, "Please input hap path: ");
    std::string deviceId = args.positional.size() > 1
        ? args.positional[1] : args.get("device");

    if (hap.empty()) {
        return;
    }

    std::vector<std::string> hdcArgs;

    if (!deviceId.empty()) {
        hdcArgs = {"-t", deviceId, "install", hap};
    } else {
        hdcArgs = {"install", hap};
    }

    printResult(runHdc(hdcArgs));
}

void handleHapUninstall(const Args& args) {
    std::string bundleName = takeOrPrompt(args, 0, "Please input bundle name: ");
    std::string deviceId = args.positional.size() > 1
        ? args.positional[1] : args.get("device");

    if (bundleName.empty()) {
        return;
    }

    std::vector<std::string> hdcArgs;

    if (!deviceId.empty()) {
        hdcArgs = {"-t", deviceId, "uninstall", bundleName};
    } else {
        hdcArgs = {"uninstall", bundleName};
    }

    printResult(runHdc(hdcArgs));
}

void handleHapOpen(const Args& args) {
    std::string bundleName = takeOrPrompt(args, 0, "Please input bundle name: ");
    std::string abilityName = takeOrPrompt(args, 1, "Please input ability name (default EntryAbility): ");

    if (bundleName.empty()) {
        return;
    }

    if (abilityName.empty()) {
        abilityName = "EntryAbility";
    }

    printResult(runHdc({"shell", "aa start -b " + bundleName + " -a " + abilityName}));
}

void handleHapSigner(const Args& args) {
    if (args.positional.empty()) {
        std::cout << "Usage: hap signer <method> [-option value ...]\n";
        std::cout << "e.g.   hap signer verify-app -inFile signed.hap\n";
        g_lastExitCode = 1;
        return;
    }

    // 透传原始参数（含 -- 选项），不做二次解析
    std::vector<std::string> raw = args.positional;

    for (const auto& kv : args.options) {
        raw.push_back("--" + kv.first);
        raw.push_back(kv.second);
    }

    printResult(runSigner(raw));
}

// ============================================================
// Command Dispatcher
// ============================================================

bool handleCommand(const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return true;
    }

    const std::string& cmd = tokens[0];
    std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
    Args args = parseArgs(rest);

    if (cmd == "quit" || cmd == "exit") {
        std::cout << "bye!\n";
        return false;
    }

    if (cmd == "help") {
        showHelp();
        return true;
    }

    if (cmd == "sign") {
        if (args.positional.empty()) {
            std::cout << "subcommands: login logout status add list info remove import\n";
            return true;
        }

        std::string sub = args.positional[0];
        Args subArgs = args;
        subArgs.positional.erase(subArgs.positional.begin());

        if (sub == "login") {
            handleSignLogin(subArgs);
        } else if (sub == "logout") {
            handleSignLogout();
        } else if (sub == "status") {
            handleSignStatus();
        } else if (sub == "add") {
            handleSignAdd(subArgs);
        } else if (sub == "list") {
            handleSignList();
        } else if (sub == "info") {
            handleSignInfo(subArgs);
        } else if (sub == "remove" || sub == "rm") {
            handleSignRemove(subArgs);
        } else if (sub == "import") {
            handleSignImport(subArgs);
        } else {
            unknownCommand();
        }

        return true;
    }

    if (cmd == "hap") {
        if (args.positional.empty()) {
            unknownCommand();
            return true;
        }

        std::string sub = args.positional[0];
        Args subArgs = args;
        subArgs.positional.erase(subArgs.positional.begin());

        if (sub == "sign") {
            handleHapSign(subArgs, false);
        } else if (sub == "sign-install") {
            handleHapSign(subArgs, true);
        } else if (sub == "install") {
            handleHapInstall(subArgs);
        } else if (sub == "uninstall") {
            handleHapUninstall(subArgs);
        } else if (sub == "open") {
            handleHapOpen(subArgs);
        } else if (sub == "genkey") {
            handleHapGenKey(subArgs);
        } else if (sub == "signer") {
            handleHapSigner(subArgs);
        } else {
            unknownCommand();
        }

        return true;
    }

    if (cmd == "device") {
        if (args.positional.empty()) {
            unknownCommand();
            return true;
        }

        std::string sub = args.positional[0];
        Args subArgs = args;
        subArgs.positional.erase(subArgs.positional.begin());

        if (sub == "list") {
            handleDeviceList();
        } else if (sub == "connect") {
            handleDeviceConnect(subArgs);
        } else if (sub == "shell") {
            handleDeviceShell(subArgs);
        } else {
            unknownCommand();
        }

        return true;
    }

    unknownCommand();
    return true;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    initAhtHome();

    if (argc > 1) {
        std::vector<std::string> tokens(argv + 1, argv + argc);

        try {
            handleCommand(tokens);
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << '\n';

            if (g_lastExitCode == 0) {
                g_lastExitCode = 1;
            }
        }

        return g_lastExitCode;
    }

    std::cout << "HAPTool started.\n";
    std::cout << "hdc: libhdc.so / hapsigner: libhapsigner.so (in-process)\n";
    std::cout << "AHT_HOME: " << AHT_HOME << '\n';
    std::cout << "Type 'help' to see available commands.\n";

    std::string command;

    while (true) {
        std::cout << "$ " << std::flush;

        if (!std::getline(std::cin, command)) {
            std::cout << "\nbye!\n";
            break;
        }

        try {
            if (!handleCommand(tokenize(command))) {
                break;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << '\n';
        }
    }

    return 0;
}
