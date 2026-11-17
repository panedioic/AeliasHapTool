/*
 * haptool — HAP 开发助手。
 *
 * 用法：
 *   haptool                  无参数 → 进入交互式 REPL
 *   haptool <cmd> [args...]  有参数 → 执行这一条命令后退出，
 *                            退出码取底层库的退出码（脚本可感知成败）
 *
 * hdc（连接/安装/卸载/打开）与 hapsigner（签名/密钥生成）都在进程内通过
 * fork 子进程调 libhdc.so / libhapsigner.so 的入口执行 —— hdc client 模式
 * 结尾会 _exit(0) 且残留 getopt/libuv 全局状态；签名工具则可能调用 exit()
 * 并持有 openssl 全局状态，进程隔离对两者都最稳妥。
 * 两种模式共用同一套命令分发；单次执行时缺的参数仍会从 stdin 问。
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

// 子进程入口统一签名：argc/argv 形态与 main 一致
using LibEntry = int (*)(int, const char**);

// 单次执行模式的进程退出码；REPL 模式不使用。runIsolated 每次把子进程
// 的退出码写进来，haptool 的 main 在单次模式下以它为进程退出码
static int g_lastExitCode = 0;

ProcessResult runIsolated(LibEntry entry, const std::string& progName,
                          const std::vector<std::string>& args) {
    ProcessResult result;

    if (args.empty() && progName.empty()) {
        return result;
    }

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
        // Child process
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(pipefd[1]);

        int ret = entry(static_cast<int>(argv.size() - 1), argv.data());

        // 库入口正常返回时把 stdio 缓冲冲掉再 _exit —— _exit 不跑
        // atexit/静态析构，直接退会把缓冲输出丢在管道里
        std::cout.flush();
        std::cerr.flush();
        fflush(nullptr);
        _exit(ret);
    }

    // Parent process
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

    // 单次执行模式靠它透传库的成败
    g_lastExitCode = result.exitCode;

    return result;
}

ProcessResult runHdc(const std::vector<std::string>& args) {
    return runIsolated(HdcLibMain, "hdc", args);
}

ProcessResult runSigner(const std::vector<std::string>& args) {
    return runIsolated(HapSignerLibMain, "hap-sign-tool", args);
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

// 工作主目录，启动时初始化：优先取环境变量 AHT_HOME，
// 未指定时默认 $HOME/.ahthome（HOME 也没有则退回当前目录）
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
// Sign Configs (persisted in $AHT_HOME/sign.json)
// ============================================================
//
// 只存非敏感字段；密码不落盘，签名时再问。
// 文件 0600，cJSON 读写，顶层就是 { "<id>": {...}, ... }。

struct SignConfig {
    std::string keystore;   // p12/jks 路径
    std::string alias;      // -keyAlias
    std::string alg;        // -signAlg（SHA256withECDSA 等）
    std::string profile;    // -profileFile（p7b）
    std::string cert;       // -appCertFile（pem/cer）
};

static std::string signConfigPath() {
    return AHT_HOME + "/sign.json";
}

static std::string jsonStr(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);

    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    return "";
}

static std::map<std::string, SignConfig> loadSignConfigs() {
    std::map<std::string, SignConfig> configs;

    std::ifstream in(signConfigPath());

    if (!in) {
        return configs;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();

    cJSON* root = cJSON_Parse(buffer.str().c_str());

    if (!root) {
        std::cerr << "Warning: cannot parse " << signConfigPath()
                  << ", treating as empty\n";
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

static void saveSignConfigs(const std::map<std::string, SignConfig>& configs) {
    std::filesystem::create_directories(AHT_HOME);

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

    std::ofstream out(signConfigPath(), std::ios::trunc);
    out << printed << '\n';
    out.close();

    ::chmod(signConfigPath().c_str(), 0600);

    cJSON_free(printed);
    cJSON_Delete(root);
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

// 取第 idx 个位置参数；没有就在 stdin 问一句。
// 单次执行和 REPL 都能用：haptool hap sign a.hap 与
// REPL 里敲 "hap sign" 再按提示输入，走的是同一份逻辑。
std::string takeOrPrompt(const std::vector<std::string>& args, size_t idx,
                         const std::string& message) {
    if (args.size() > idx) {
        return args[idx];
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
    std::cout << "===== commands =====\n";
    std::cout << "help            : get help\n";
    std::cout << "quit            : quit application\n";
    std::cout << "device list     : list connected devices\n";
    std::cout << "device connect [ip:port]        : connect to a device (tconn)\n";
    std::cout << "device shell <command...>       : run a shell command on device\n";
    std::cout << "hap install [hap] [deviceId]    : install a hap package\n";
    std::cout << "hap uninstall [bundle] [deviceId]: uninstall a bundle by name\n";
    std::cout << "hap open [bundle] [ability]     : open an app (aa start)\n";
    std::cout << "sign add [id]                   : add a sign config (saved to $AHT_HOME/sign.json)\n";
    std::cout << "sign info [id]                  : show sign config(s)\n";
    std::cout << "sign list                       : list sign config ids\n";
    std::cout << "sign remove [id]                : remove a sign config\n";
    std::cout << "hap genkey [alias] [out.p12]    : generate a key pair (ECC P-256, p12)\n";
    std::cout << "hap sign [hap] [id]             : sign a hap with a saved config\n";
    std::cout << "hap signer <args...>            : passthrough to hap-sign-tool\n";
    std::cout << "(missing args are read from stdin)\n";
}

void handleDeviceList() {
    ProcessResult result = runHdc({"list", "targets"});

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "Failed to list devices. Exit code: "
                  << result.exitCode << '\n';
    }

    // Update device list.
    DEVICES.clear();

    std::istringstream stream(result.stdoutText);
    std::string line;

    while (std::getline(stream, line)) {
        // Drop the "[Empty]" line and banner lines from hdc output.
        if (!line.empty() && line.find("[Empty]") == std::string::npos) {
            DEVICES.push_back(line);
        }
    }
}

void handleDeviceConnect(const std::vector<std::string>& args) {
    std::string deviceIpHost = takeOrPrompt(args, 0, "Please input device ip and port: ");

    if (deviceIpHost.empty()) {
        return;
    }

    ProcessResult result = runHdc({"tconn", deviceIpHost});

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "HDC command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

void handleDeviceShell(const std::vector<std::string>& args) {
    std::string command;

    if (!args.empty()) {
        // 余下参数按空格拼回一条命令：haptool device shell param get x
        std::ostringstream joined;

        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                joined << ' ';
            }
            joined << args[i];
        }

        command = joined.str();
    } else {
        command = prompt("Please input shell command: ");
    }

    if (command.empty()) {
        return;
    }

    ProcessResult result = runHdc({"shell", command});

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "HDC command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

void handleHapInstall(const std::vector<std::string>& args) {
    std::string hap = takeOrPrompt(args, 0, "Please input hap path: ");
    std::string deviceId = takeOrPrompt(args, 1, "Please input device id (empty = default): ");

    if (hap.empty()) {
        return;
    }

    std::vector<std::string> hdcArgs;

    if (!deviceId.empty()) {
        hdcArgs = {"-t", deviceId, "install", hap};
    } else {
        hdcArgs = {"install", hap};
    }

    ProcessResult result = runHdc(hdcArgs);

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "HDC command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

void handleHapUninstall(const std::vector<std::string>& args) {
    std::string bundleName = takeOrPrompt(args, 0, "Please input bundle name: ");
    std::string deviceId = takeOrPrompt(args, 1, "Please input device id (empty = default): ");

    if (bundleName.empty()) {
        return;
    }

    std::vector<std::string> hdcArgs;

    if (!deviceId.empty()) {
        hdcArgs = {"-t", deviceId, "uninstall", bundleName};
    } else {
        hdcArgs = {"uninstall", bundleName};
    }

    ProcessResult result = runHdc(hdcArgs);

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "HDC command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

void handleHapOpen(const std::vector<std::string>& args) {
    std::string bundleName = takeOrPrompt(args, 0, "Please input bundle name: ");
    std::string abilityName = takeOrPrompt(args, 1, "Please input ability name (default EntryAbility): ");

    if (bundleName.empty()) {
        return;
    }

    if (abilityName.empty()) {
        abilityName = "EntryAbility";
    }

    // hdc shell aa start -b <bundle> -a <ability>
    ProcessResult result = runHdc({
        "shell",
        "aa start -b " + bundleName + " -a " + abilityName
    });

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "HDC command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

// ============================================================
// Command Handlers — Signing (hapsigner)
// ============================================================

void handleSignAdd(const std::vector<std::string>& args) {
    std::string id = takeOrPrompt(args, 0, "Please input sign config id: ");

    if (id.empty()) {
        return;
    }

    SignConfig cfg;
    cfg.keystore = prompt("Keystore file (.p12/.jks): ");
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

    if (configs.count(id)) {
        std::cout << "Overwriting existing config '" << id << "'\n";
    }

    configs[id] = cfg;
    saveSignConfigs(configs);

    std::cout << "Sign config '" << id << "' saved to "
              << signConfigPath() << '\n';
}

void handleSignInfo(const std::vector<std::string>& args) {
    auto configs = loadSignConfigs();

    if (configs.empty()) {
        std::cout << "no sign configs, use 'sign add' to add!\n";
        return;
    }

    std::string id = args.empty() ? "" : args[0];

    if (!id.empty() && !configs.count(id)) {
        std::cout << "sign config '" << id << "' not exist\n";
        g_lastExitCode = 1;
        return;
    }

    for (const auto& [cfgId, cfg] : configs) {
        if (!id.empty() && cfgId != id) {
            continue;
        }

        std::cout << "===== " << cfgId << " =====\n";
        std::cout << "keystore : " << cfg.keystore << '\n';
        std::cout << "alias    : " << cfg.alias << '\n';
        std::cout << "alg      : " << cfg.alg << '\n';
        std::cout << "profile  : " << cfg.profile << '\n';
        std::cout << "cert     : " << cfg.cert << '\n';
    }
}

void handleSignList() {
    auto configs = loadSignConfigs();

    if (configs.empty()) {
        std::cout << "no sign configs, use 'sign add' to add!\n";
        return;
    }

    for (const auto& [id, cfg] : configs) {
        (void)cfg;
        std::cout << id << '\n';
    }
}

void handleSignRemove(const std::vector<std::string>& args) {
    std::string id = takeOrPrompt(args, 0, "Please input sign config id: ");

    if (id.empty()) {
        return;
    }

    auto configs = loadSignConfigs();

    if (configs.erase(id) == 0) {
        std::cout << "sign config '" << id << "' not exist\n";
        g_lastExitCode = 1;
        return;
    }

    saveSignConfigs(configs);
    std::cout << "Sign config '" << id << "' removed\n";
}

void handleHapGenKey(const std::vector<std::string>& args) {
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

    // ECC P-256：OpenHarmony 应用签名的主流口径
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

void handleHapSign(const std::vector<std::string>& args) {
    std::string hap = takeOrPrompt(args, 0, "Please input hap path: ");
    std::string id = takeOrPrompt(args, 1, "Please input sign config id: ");

    if (hap.empty() || id.empty()) {
        return;
    }

    auto configs = loadSignConfigs();
    auto it = configs.find(id);

    if (it == configs.end()) {
        std::cout << "sign config '" << id << "' not exist, use 'sign add'!\n";
        g_lastExitCode = 1;
        return;
    }

    const SignConfig& cfg = it->second;

    std::string keyPwd = prompt("Key password: ");
    std::string storePwd = prompt("Keystore password (empty = same as key): ");

    if (storePwd.empty()) {
        storePwd = keyPwd;
    }

    if (keyPwd.empty()) {
        std::cerr << "Password is required.\n";
        g_lastExitCode = 1;
        return;
    }

    // 输出默认 <in>-signed.hap
    std::string out = hap;
    const std::string suffix = ".hap";

    if (out.size() > suffix.size() &&
        out.compare(out.size() - suffix.size(), suffix.size(), suffix) == 0) {
        out = out.substr(0, out.size() - suffix.size());
    }

    out += "-signed.hap";

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
        "-inFile", hap,
        "-outFile", out
    });

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "sign-app failed. Exit code: "
                  << result.exitCode << '\n';
        return;
    }

    if (fs::exists(out)) {
        std::cout << "Signed: " << out << '\n';
    } else {
        std::cerr << "Output not found: " << out << '\n';
        g_lastExitCode = 1;
    }
}

// 透传全部参数给 hap-sign-tool：sign-profile / verify-app / generate-cert
// 等高级命令的逃生舱
void handleHapSigner(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: hap signer <method> [-option value ...]\n";
        std::cout << "e.g.   hap signer verify-app -inFile signed.hap\n";
        g_lastExitCode = 1;
        return;
    }

    ProcessResult result = runSigner(args);

    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "hap-sign-tool failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

// ============================================================
// Command Dispatcher
// ============================================================
//
// tokens[0] 是命令动词，其余是位置参数。REPL 的一行输入和 haptool 的
// argv[1..] 都在这里汇合，两个模式行为完全一致。

// 返回 false 表示程序应当退出（quit）
bool handleCommand(const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return true;
    }

    const std::string& cmd = tokens[0];
    std::vector<std::string> rest(tokens.begin() + 1, tokens.end());

    if (cmd == "quit" || cmd == "exit") {
        std::cout << "bye!\n";
        return false;
    }

    if (cmd == "help") {
        showHelp();
        return true;
    }

    if (cmd == "sign") {
        if (rest.empty()) {
            std::cout << "input 'sign add', 'sign info', 'sign list' or 'sign remove'!\n";
            return true;
        }

        std::vector<std::string> sub(rest.begin() + 1, rest.end());

        if (rest[0] == "add") {
            handleSignAdd(sub);
        }
        else if (rest[0] == "info") {
            handleSignInfo(sub);
        }
        else if (rest[0] == "list") {
            handleSignList();
        }
        else if (rest[0] == "remove") {
            handleSignRemove(sub);
        }
        else {
            unknownCommand();
        }

        return true;
    }

    if (cmd == "device") {
        if (rest.empty()) {
            unknownCommand();
            return true;
        }

        std::vector<std::string> sub(rest.begin() + 1, rest.end());

        if (rest[0] == "list") {
            handleDeviceList();
        }
        else if (rest[0] == "connect") {
            handleDeviceConnect(sub);
        }
        else if (rest[0] == "shell") {
            handleDeviceShell(sub);
        }
        else {
            unknownCommand();
        }

        return true;
    }

    if (cmd == "hap") {
        if (rest.empty()) {
            unknownCommand();
            return true;
        }

        std::vector<std::string> sub(rest.begin() + 1, rest.end());

        if (rest[0] == "install") {
            handleHapInstall(sub);
        }
        else if (rest[0] == "uninstall") {
            handleHapUninstall(sub);
        }
        else if (rest[0] == "open") {
            handleHapOpen(sub);
        }
        else if (rest[0] == "genkey") {
            handleHapGenKey(sub);
        }
        else if (rest[0] == "sign") {
            handleHapSign(sub);
        }
        else if (rest[0] == "signer") {
            handleHapSigner(sub);
        }
        else {
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

    // 带参数：单次执行模式 —— argv[1..] 视为一条完整命令，执行完即退出
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

    // 无参数：交互式 REPL
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
