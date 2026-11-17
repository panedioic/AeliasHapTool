
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

void reportHdcResult(const ProcessResult& result) {
    std::cout << result.stdoutText;

    if (result.exitCode != 0) {
        std::cerr << "HDC command failed. Exit code: "
                  << result.exitCode << '\n';
    }
}

// ============================================================
// Application State
// ============================================================

static std::vector<std::string> SIGN_INFO;
static std::vector<std::string> DEVICES;

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

// ============================================================
// Command Handlers
// ============================================================

void showHelp() {
    std::cout << "===== help =====\n";
    std::cout << "help            : get help\n";
    std::cout << "quit            : quit application\n";
    std::cout << "sign            : show sign commands\n";
    std::cout << "sign info       : show sign information\n";
    std::cout << "sign add        : add sign information\n";
    std::cout << "device list     : list connected devices\n";
    std::cout << "device connect  : connect to a device (tconn ip:port)\n";
    std::cout << "device shell    : run a shell command on device\n";
    std::cout << "hap sign        : sign a hap package\n";
    std::cout << "hap install     : install a hap package\n";
    std::cout << "hap uninstall   : uninstall a bundle by name\n";
    std::cout << "hap open        : open an app (aa start)\n";
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

void handleDeviceConnect() {
    std::string deviceIpHost = prompt("Please input device ip and port: ");

    if (deviceIpHost.empty()) {
        return;
    }

    reportHdcResult(runHdc({"tconn", deviceIpHost}));
}

void handleDeviceShell() {
    std::string command = prompt("Please input shell command: ");

    if (command.empty()) {
        return;
    }

    reportHdcResult(runHdc({"shell", command}));
}

void handleHapSign() {
    std::string hap = prompt("Please input hap path: ");
    std::string signId = prompt("Please input sign config id: ");

    // TODO:
    // Replace this with the actual HDC/HAP signing command.
    std::cout << "HAP signing is not implemented yet.\n";
    std::cout << "HAP: " << hap << '\n';
    std::cout << "Sign ID: " << signId << '\n';
}

void handleHapInstall() {
    std::string hap = prompt("Please input hap path: ");
    std::string deviceId = prompt("Please input device id (empty = default): ");

    if (hap.empty()) {
        return;
    }

    std::vector<std::string> args;

    if (!deviceId.empty()) {
        args = {"-t", deviceId, "install", hap};
    } else {
        args = {"install", hap};
    }

    reportHdcResult(runHdc(args));
}

void handleHapUninstall() {
    std::string bundleName = prompt("Please input bundle name: ");
    std::string deviceId = prompt("Please input device id (empty = default): ");

    if (bundleName.empty()) {
        return;
    }

    std::vector<std::string> args;

    if (!deviceId.empty()) {
        args = {"-t", deviceId, "uninstall", bundleName};
    } else {
        args = {"uninstall", bundleName};
    }

    reportHdcResult(runHdc(args));
}

void handleHapOpen() {
    std::string bundleName = prompt("Please input bundle name: ");
    std::string abilityName = prompt("Please input ability name (default EntryAbility): ");

    if (bundleName.empty()) {
        return;
    }

    if (abilityName.empty()) {
        abilityName = "EntryAbility";
    }

    // hdc shell aa start -b <bundle> -a <ability>
    reportHdcResult(runHdc({
        "shell",
        "aa start -b " + bundleName + " -a " + abilityName
    }));
}

// ============================================================
// Command Dispatcher
// ============================================================

bool handleCommand(const std::string& command) {
    if (command == "quit") {
        std::cout << "bye!\n";
        return false;
    }

    if (command == "help") {
        showHelp();
    }
    else if (command == "sign") {
        std::cout << "input 'sign info' or 'sign add'!\n";
    }
    else if (command == "sign info") {
        handleSignInfo();
    }
    else if (command == "sign add") {
        handleSignAdd();
    }
    else if (command == "device list") {
        handleDeviceList();
    }
    else if (command == "device connect") {
        handleDeviceConnect();
    }
    else if (command == "device shell") {
        handleDeviceShell();
    }
    else if (command == "hap sign") {
        handleHapSign();
    }
    else if (command == "hap install") {
        handleHapInstall();
    }
    else if (command == "hap uninstall") {
        handleHapUninstall();
    }
    else if (command == "hap open") {
        handleHapOpen();
    }
    else {
        std::cout << "command not found!\n";
    }

    return true;
}

// ============================================================
// Main
// ============================================================

int main() {
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
            if (!handleCommand(command)) {
                break;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << '\n';
        }
    }

    return 0;
}
