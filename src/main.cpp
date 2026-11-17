/*
 * haptool — HAP 开发助手。
 *
 * 用法：
 *   haptool                  无参数 → 进入交互式 REPL
 *   haptool <cmd> [args...]  有参数 → 执行这一条命令后退出，
 *                            退出码取 hdc 的退出码（脚本可感知成败）
 *
 * hdc 命令通过 fork 子进程调 libhdc.so 的 HdcLibMain 执行（hdc client 模式
 * 结尾会 _exit(0) 且残留 getopt/libuv 全局状态，必须进程隔离）。
 * 两种模式共用同一套命令分发；单次执行时缺的参数仍会从 stdin 问。
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "hdc_entry.h"

// ============================================================
// HDC (in-process library)
// ============================================================
//
// hdc's client mode always ends with _exit(0) and keeps getopt/libuv global
// state, so each hdc command runs in a forked child calling HdcLibMain()
// from libhdc.so — the parent REPL stays alive and untouched.
// argv[0] is skipped as the program name, "hdc" is fine.

struct ProcessResult {
    int exitCode = -1;
    std::string stdoutText;
};

ProcessResult runHdc(const std::vector<std::string>& args) {
    ProcessResult result;

    if (args.empty()) {
        return result;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        throw std::runtime_error("Failed to create pipe");
    }

    std::vector<const char*> argv;
    argv.push_back("hdc");

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

        HdcLibMain(static_cast<int>(argv.size() - 1), argv.data());

        // HdcLibMain returns only for options like -v / -h.
        _exit(0);
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

    return result;
}

// ============================================================
// Application State
// ============================================================

static std::vector<std::string> SIGN_INFO;
static std::vector<std::string> DEVICES;

// 单次执行模式的进程退出码；REPL 模式不使用
static int g_lastExitCode = 0;

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
// 单次执行和 REPL 都能用：haptool hap install a.hap 与
// REPL 里敲 "hap install" 再按提示输入，走的是同一份逻辑。
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
// Command Handlers
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
    std::cout << "sign            : show sign commands\n";
    std::cout << "sign info       : show sign information\n";
    std::cout << "sign add        : add sign information\n";
    std::cout << "device list     : list connected devices\n";
    std::cout << "device connect [ip:port]        : connect to a device (tconn)\n";
    std::cout << "device shell <command...>       : run a shell command on device\n";
    std::cout << "hap sign [hap] [signId]         : sign a hap package\n";
    std::cout << "hap install [hap] [deviceId]    : install a hap package\n";
    std::cout << "hap uninstall [bundle] [deviceId]: uninstall a bundle by name\n";
    std::cout << "hap open [bundle] [ability]     : open an app (aa start)\n";
    std::cout << "(missing args are read from stdin)\n";
}

void handleSignInfo() {
    if (SIGN_INFO.empty()) {
        std::cout
            << "sign info not exist, "
            << "please use 'sign add' to add!\n";

        return;
    }

    std::cout << SIGN_INFO[0] << '\n';
}

void handleSignAdd() {
    SIGN_INFO = {"stub"};

    std::cout << "stub add succeed!\n";
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

void handleHapSign(const std::vector<std::string>& args) {
    std::string hap = takeOrPrompt(args, 0, "Please input hap path: ");
    std::string signId = takeOrPrompt(args, 1, "Please input sign config id: ");

    // TODO:
    // Replace this with the actual HDC/HAP signing command.
    std::cout << "HAP signing is not implemented yet.\n";
    std::cout << "HAP: " << hap << '\n';
    std::cout << "Sign ID: " << signId << '\n';
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
            std::cout << "input 'sign info' or 'sign add'!\n";
            return true;
        }

        std::vector<std::string> sub(rest.begin() + 1, rest.end());

        if (rest[0] == "info") {
            handleSignInfo();
        }
        else if (rest[0] == "add") {
            handleSignAdd();
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

        if (rest[0] == "sign") {
            handleHapSign(sub);
        }
        else if (rest[0] == "install") {
            handleHapInstall(sub);
        }
        else if (rest[0] == "uninstall") {
            handleHapUninstall(sub);
        }
        else if (rest[0] == "open") {
            handleHapOpen(sub);
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
    std::cout << "hdc: libhdc.so (in-process)\n";
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
