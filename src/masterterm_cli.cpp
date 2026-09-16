#include "NativeJsonDom.h"
#include "NativeDataDir.h"
#include "SshSession.h"

#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <wincred.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct ConnectionOptions
{
    std::string connection;
    std::string port = "22";
    std::string password;
    std::string keyPath;
    std::string proxyJump;
    std::string proxyPassword;
    std::string proxyKeyPath;
    std::string profileName;
    std::string command;
    std::string scpUpload;
    std::string scpDownload;
    std::string scpTargetPath;
    int timeoutSeconds = 0;
    bool jsonOutput = false;
    bool stripAnsi = false;
};

struct ParsedArguments
{
    std::unordered_map<std::string, std::string> options;
    std::vector<std::string> positionals;
    bool help = false;
    bool version = false;
    bool agent = false;
    bool json = false;
    bool stripAnsi = false;
    std::string error;
};

std::string wideToUtf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

HANDLE consoleOutput()
{
    return GetStdHandle(STD_OUTPUT_HANDLE);
}

void writeOutput(std::string_view bytes)
{
    if (bytes.empty())
        return;
    DWORD written = 0;
    WriteFile(
        consoleOutput(), bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr);
}

void writeError(std::string_view bytes)
{
    if (bytes.empty())
        return;
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    WriteFile(
        error, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void writeConsoleText(std::wstring_view text)
{
    const HANDLE output = consoleOutput();
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(output, &mode)) {
        WriteConsoleW(
            output, text.data(), static_cast<DWORD>(text.size()),
            &written, nullptr);
        return;
    }
    writeOutput(wideToUtf8(text));
}

std::string environmentUtf8(const wchar_t *name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0)
        return {};
    std::wstring value(static_cast<std::size_t>(required), L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
    if (length == 0 || length >= required)
        return {};
    value.resize(length);
    return wideToUtf8(value);
}

std::wstring credentialTarget(
    std::string_view scope, std::string_view address)
{
    std::wstring target = L"MasterSSH/";
    target += utf8ToWide(scope);
    target += L'/';
    target += utf8ToWide(address);
    return target;
}

std::string readStoredCredential(
    std::string_view scope, std::string_view address)
{
    PCREDENTIALW credential = nullptr;
    const std::wstring target = credentialTarget(scope, address);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)
        || !credential) {
        return {};
    }
    const std::string value(
        reinterpret_cast<const char *>(credential->CredentialBlob),
        static_cast<std::size_t>(credential->CredentialBlobSize));
    CredFree(credential);
    return value;
}

std::filesystem::path configPath()
{
    return NativeDataDir::configFile();
}

const NativeJsonDom::Value *lookupValue(
    const NativeJsonDom::Object &object, std::string_view key)
{
    const auto iterator = object.values.find(std::string(key));
    return iterator == object.values.end() ? nullptr : &iterator->second;
}

std::string valueText(
    const NativeJsonDom::Object &object, std::string_view key,
    std::string fallback = {})
{
    const NativeJsonDom::Value *value = lookupValue(object, key);
    if (!value)
        return fallback;
    if (value->isString())
        return value->string();
    if (value->isNumber())
        return std::to_string(static_cast<long long>(value->number()));
    return fallback;
}

bool loadProfile(std::string_view name, ConnectionOptions *options)
{
    std::ifstream input(configPath(), std::ios::binary);
    if (!input)
        return false;
    const std::string contents(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    NativeJsonDom::Value root;
    if (!NativeJsonDom::parse(contents, root) || !root.isObject())
        return false;
    const NativeJsonDom::Value *servers =
        lookupValue(root.object(), "servers");
    if (!servers || !servers->isArray())
        return false;
    for (const NativeJsonDom::Value &server : servers->array().values) {
        if (!server.isObject()
            || valueText(server.object(), "name") != name) {
            continue;
        }
        options->profileName = std::string(name);
        options->connection = valueText(server.object(), "address");
        options->port = valueText(server.object(), "port", "22");
        options->keyPath = valueText(server.object(), "keyPath");
        options->proxyJump = valueText(server.object(), "proxyJump");
        options->proxyKeyPath = valueText(server.object(), "proxyKeyPath");
        options->password = readStoredCredential("SSH", options->connection);
        if (!options->proxyJump.empty()) {
            options->proxyPassword = readStoredCredential(
                "ProxyJump", options->proxyJump);
        }
        return !options->connection.empty();
    }
    return false;
}

bool requiresValue(std::string_view option)
{
    return option == "profile" || option == "port"
        || option == "identity" || option == "proxy-jump"
        || option == "password" || option == "proxy-password"
        || option == "exec" || option == "timeout"
        || option == "scp-upload" || option == "scp-download";
}

ParsedArguments parseArguments(int argc, wchar_t *argv[])
{
    ParsedArguments result;
    bool parseOptions = true;
    for (int index = 1; index < argc; ++index) {
        std::string argument = wideToUtf8(argv[index]);
        if (parseOptions && argument == "--") {
            parseOptions = false;
            continue;
        }
        if (parseOptions && (argument == "-h" || argument == "--help"
                             || argument == "-?" || argument == "--help-all")) {
            result.help = true;
            continue;
        }
        if (parseOptions && (argument == "-v" || argument == "--version")) {
            result.version = true;
            continue;
        }
        if (parseOptions && argument == "--json") {
            result.json = true;
            continue;
        }
        if (parseOptions && argument == "--agent") {
            result.agent = true;
            continue;
        }
        if (parseOptions && argument == "--strip-ansi") {
            result.stripAnsi = true;
            continue;
        }
        if (parseOptions && argument.rfind("--", 0) == 0) {
            argument.erase(0, 2);
            const std::size_t equals = argument.find('=');
            const std::string name = argument.substr(0, equals);
            if (!requiresValue(name)) {
                result.error = "未知选项：--" + name;
                return result;
            }
            std::string value;
            if (equals != std::string::npos) {
                value = argument.substr(equals + 1);
            } else if (++index < argc) {
                value = wideToUtf8(argv[index]);
            } else {
                result.error = "选项缺少参数：--" + name;
                return result;
            }
            result.options[name] = std::move(value);
            continue;
        }
        result.positionals.push_back(std::move(argument));
    }
    return result;
}

bool resolveOptions(
    const ParsedArguments &arguments, ConnectionOptions *options,
    std::string *error)
{
    const auto fail = [error](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    const auto profile = arguments.options.find("profile");
    if (profile != arguments.options.end()) {
        if (!loadProfile(profile->second, options)) {
            return fail("找不到连接配置：" + profile->second);
        }
    } else if (!arguments.positionals.empty()) {
        std::size_t index =
            arguments.positionals.front() == "connect" ? 1 : 0;
        if (index < arguments.positionals.size())
            options->connection = arguments.positionals[index];
    }
    if (options->connection.empty()) {
        return fail("必须提供 user@host 或 --profile 配置名。");
    }
    const auto apply = [&](const char *name, std::string &target) {
        const auto iterator = arguments.options.find(name);
        if (iterator != arguments.options.end())
            target = iterator->second;
    };
    apply("port", options->port);
    apply("identity", options->keyPath);
    apply("proxy-jump", options->proxyJump);
    apply("password", options->password);
    apply("proxy-password", options->proxyPassword);
    apply("exec", options->command);
    if (arguments.options.find("exec") != arguments.options.end()
        && options->command.empty())
        return fail("--exec 命令不能为空。");
    apply("scp-upload", options->scpUpload);
    apply("scp-download", options->scpDownload);
    if (!options->scpUpload.empty() || !options->scpDownload.empty()) {
        // SCP syntax: <user@host> is the first positional, the "other side"
        // path is the second one.  --scp-upload <local> user@host <remote>;
        // --scp-download <remote> user@host <local>.
        if (arguments.positionals.size() < 2)
            return fail("SCP 需要 <user@host> 和另一端路径两个位置参数。");
        options->scpTargetPath = arguments.positionals[1];
    }
    const auto timeoutOption = arguments.options.find("timeout");
    if (timeoutOption != arguments.options.end()) {
        try {
            const long value = std::stol(timeoutOption->second);
            options->timeoutSeconds = value > 0 ? static_cast<int>(value) : 0;
        } catch (...) {
            options->timeoutSeconds = 0;
        }
    }
    options->jsonOutput = arguments.json || arguments.agent;
    options->stripAnsi = arguments.stripAnsi || arguments.agent;
    if (options->jsonOutput && options->command.empty())
        return fail("--json/--agent 只能与 --exec 一起使用。");
    return true;
}

void removeLastCodePoint(std::wstring &value)
{
    if (value.empty())
        return;
    const wchar_t last = value.back();
    value.pop_back();
    if (last >= 0xdc00 && last <= 0xdfff && !value.empty()
        && value.back() >= 0xd800 && value.back() <= 0xdbff) {
        value.pop_back();
    }
}

std::string readHiddenLine(std::string_view prompt)
{
    writeConsoleText(utf8ToWide(prompt));
    std::wstring result;
    for (;;) {
        const wchar_t value = static_cast<wchar_t>(_getwch());
        if (value == L'\r' || value == L'\n') {
            writeOutput("\r\n");
            return wideToUtf8(result);
        }
        if (value == L'\b') {
            removeLastCodePoint(result);
            continue;
        }
        if (value >= 32)
            result.push_back(value);
    }
}

void sendConsoleInput(SshSession *session)
{
    while (_kbhit()) {
        const int first = _getwch();
        std::string bytes;
        if (first == 0 || first == 0xe0) {
            const int special = _getwch();
            switch (special) {
            case 72: bytes = "\x1b[A"; break;
            case 80: bytes = "\x1b[B"; break;
            case 75: bytes = "\x1b[D"; break;
            case 77: bytes = "\x1b[C"; break;
            case 71: bytes = "\x1b[H"; break;
            case 79: bytes = "\x1b[F"; break;
            case 83: bytes = "\x1b[3~"; break;
            default: break;
            }
        } else if (first == '\r' || first == '\n') {
            bytes = "\r";
        } else if (first == 8) {
            bytes = "\x7f";
        } else if (first == 9) {
            bytes = "\t";
        } else if (first == 27) {
            bytes = "\x1b";
        } else if (first > 0 && first < 32) {
            bytes.push_back(static_cast<char>(first));
        } else if (first >= 32) {
            std::wstring character(
                1, static_cast<wchar_t>(first));
            if (first >= 0xd800 && first <= 0xdbff)
                character.push_back(static_cast<wchar_t>(_getwch()));
            bytes = wideToUtf8(character);
        }
        if (!bytes.empty())
            session->write(bytes);
    }
}

// Forward piped/redirected stdin to the SSH session. Unlike sendConsoleInput
// (conio keyboard), this uses the standard input handle so that
//   echo hello | MasterTerm-cli.exe --exec "cat" user@host
// works. Returns true when stdin is a pipe (as opposed to a console).
bool stdinIsPipe()
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || input == nullptr)
        return false;
    DWORD available = 0;
    return PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)
        != FALSE;
}

// SCP-style single-file transfer through the bundled MasterTermSftpWorker
// (same SFTP engine as the GUI).  Returns the worker exit code.
int runScpTransfer(const ConnectionOptions &options)
{
    const auto workerName = L"MasterTermSftpWorker.exe";
    wchar_t modulePath[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    std::filesystem::path workerPath = moduleLength > 0
        && moduleLength < std::size(modulePath)
        ? std::filesystem::path(modulePath).parent_path() / workerName
        : std::filesystem::path(workerName);
    std::error_code fileError;
    if (!std::filesystem::exists(workerPath, fileError)) {
        writeError("找不到 MasterTermSftpWorker.exe，SCP 功能需要它与 "
                   "MasterTerm-cli.exe 位于同一目录。\r\n");
        return 2;
    }
    const bool upload = !options.scpUpload.empty();
    const std::wstring command = upload ? L"--upload" : L"--download";
    const std::wstring sourcePath = utf8ToWide(
        upload ? options.scpUpload : options.scpDownload);
    const std::wstring targetPath = utf8ToWide(options.scpTargetPath);
    const std::wstring identity = utf8ToWide(options.keyPath);
    // Worker CLI: <command> <connection> <port> <source> <target> <keyPath>.
    std::wstring commandLine = L"\""
        + workerPath.wstring() + L"\" " + command + L" \""
        + utf8ToWide(options.connection) + L"\" \""
        + utf8ToWide(options.port) + L"\" \"" + sourcePath + L"\" \""
        + targetPath + L"\" \"" + identity + L"\"";
    // Credentials travel through environment variables exactly like the GUI
    // does, so they never appear on the command line.
    const std::wstring password = utf8ToWide(options.password);
    const std::wstring proxyJump = utf8ToWide(options.proxyJump);
    const std::wstring proxyPassword = utf8ToWide(options.proxyPassword);
    const std::wstring proxyKeyPath = utf8ToWide(options.proxyKeyPath);
    if (!password.empty())
        SetEnvironmentVariableW(L"MASTERSSH_SFTP_PASSWORD", password.c_str());
    SetEnvironmentVariableW(L"MASTERSSH_SFTP_CONFLICT", L"overwrite");
    if (!proxyJump.empty()) {
        SetEnvironmentVariableW(L"MASTERSSH_PROXY_JUMP", proxyJump.c_str());
        if (!proxyPassword.empty())
            SetEnvironmentVariableW(L"MASTERSSH_PROXY_PASSWORD", proxyPassword.c_str());
        if (!proxyKeyPath.empty())
            SetEnvironmentVariableW(L"MASTERSSH_PROXY_KEY_PATH", proxyKeyPath.c_str());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommandLine = commandLine;

    // Redirect the worker's stdout/stderr through pipes so the raw
    // "P\t<done>\t<total>\t..." progress lines can be rendered as a compact
    // percentage while every other line is forwarded unchanged.
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const bool redirected = CreatePipe(&stdoutRead, &stdoutWrite, &security, 0)
            && CreatePipe(&stderrRead, &stderrWrite, &security, 0);
    if (redirected) {
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = stdoutWrite;
        startup.hStdError = stderrWrite;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (!CreateProcessW(
            workerPath.c_str(), mutableCommandLine.data(),
            nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        writeError("无法启动 MasterTermSftpWorker.exe。\r\n");
        if (redirected) {
            CloseHandle(stdoutRead); CloseHandle(stdoutWrite);
            CloseHandle(stderrRead); CloseHandle(stderrWrite);
        }
        return 2;
    }
    CloseHandle(process.hThread);
    if (redirected) {
        CloseHandle(stdoutWrite);
        CloseHandle(stderrWrite);
    }

    // Drain both pipes in the background; parse progress lines in the main
    // thread so the progress display stays deterministic.
    std::mutex outputMutex;
    std::string stdoutBuffer;
    std::string stderrBuffer;
    bool stdoutDone = false;
    bool stderrDone = false;
    std::thread stdoutReader([&] {
        char buffer[16384];
        DWORD read = 0;
        while (ReadFile(stdoutRead, buffer, sizeof(buffer), &read, nullptr)
               && read > 0) {
            std::lock_guard<std::mutex> lock(outputMutex);
            stdoutBuffer.append(buffer, read);
        }
        CloseHandle(stdoutRead);
        std::lock_guard<std::mutex> lock(outputMutex);
        stdoutDone = true;
    });
    std::thread stderrReader([&] {
        char buffer[16384];
        DWORD read = 0;
        while (ReadFile(stderrRead, buffer, sizeof(buffer), &read, nullptr)
               && read > 0) {
            std::lock_guard<std::mutex> lock(outputMutex);
            stderrBuffer.append(buffer, read);
        }
        CloseHandle(stderrRead);
        std::lock_guard<std::mutex> lock(outputMutex);
        stderrDone = true;
    });
    int lastPercent = -1;
    int exitCode = 0;
    const auto scpStart = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            const std::size_t newline = stdoutBuffer.find('\n');
            if (newline != std::string::npos) {
                const std::string line =
                    stdoutBuffer.substr(0, newline);
                stdoutBuffer.erase(0, newline + 1);
                if (line.rfind("P\t", 0) == 0) {
                    // P <done> <total> <name> [counts]
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    while (start <= line.size()) {
                        const std::size_t end = line.find('\t', start);
                        fields.push_back(line.substr(
                            start, end == std::string::npos
                                ? std::string::npos : end - start));
                        if (end == std::string::npos)
                            break;
                        start = end + 1;
                    }
                    if (fields.size() >= 2) {
                        try {
                            const long long done = std::stoll(fields[0]);
                            const long long total = std::stoll(fields[1]);
                            const int percent = total > 0
                                ? static_cast<int>(done * 100 / total) : 0;
                            if (percent != lastPercent) {
                                lastPercent = percent;
                                writeOutput(
                                    "\r[SCP] 已传输 " + std::to_string(percent)
                                    + "%   ");
                            }
                        } catch (...) {
                            writeOutput(line + "\r\n");
                        }
                    }
                } else if (!line.empty() && line.back() == '\r') {
                    writeOutput(line);
                } else {
                    writeOutput(line + "\r\n");
                }
            }
        }
        DWORD currentExitCode = 0;
        if (GetExitCodeProcess(process.hProcess, &currentExitCode) == FALSE)
            break;
        if (currentExitCode != STILL_ACTIVE) {
            exitCode = static_cast<int>(currentExitCode);
            // Drain whatever is left in both pipes before finishing.
            while (true) {
                bool drained = false;
                {
                    std::lock_guard<std::mutex> lock(outputMutex);
                    const std::size_t newline = stdoutBuffer.find('\n');
                    if (newline != std::string::npos) {
                        const std::string line = stdoutBuffer.substr(0, newline);
                        stdoutBuffer.erase(0, newline + 1);
                        if (line.rfind("P\t", 0) != 0)
                            writeOutput(line + "\r\n");
                        drained = true;
                    }
                }
                if (!drained)
                    break;
            }
            if (lastPercent >= 0)
                writeOutput("\r[SCP] 已传输 100%\r\n");
            else
                writeOutput("\r\n");
            break;
        }
        if (options.timeoutSeconds > 0
            && std::chrono::steady_clock::now() - scpStart
                >= std::chrono::seconds(options.timeoutSeconds)) {
            writeError("[超时] 传输超过 " + std::to_string(options.timeoutSeconds)
                       + " 秒未完成。\r\n");
            TerminateProcess(process.hProcess, 124);
            exitCode = 124;
            break;
        }
        Sleep(10);
    }
    if (stdoutReader.joinable())
        stdoutReader.join();
    if (stderrReader.joinable())
        stderrReader.join();
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        if (!stderrBuffer.empty())
            writeError(stderrBuffer);
    }
    CloseHandle(process.hProcess);
    return exitCode;
}

// Strip ANSI/VT escape sequences from a byte stream so machine consumers get
// clean text. Handles CSI (ESC [... m/k/...), OSC (ESC ] ... BEL/ST), and
// single two-byte sequences (ESC D/E/H/M). Bytes are processed bytewise so
// multi-byte UTF-8 characters pass through untouched.
std::string stripAnsiEscapes(std::string_view input)
{
    std::string output;
    output.reserve(input.size());
    enum class State { Text, Escape, Csi, Osc };
    State state = State::Text;
    for (const unsigned char byte : input) {
        switch (state) {
        case State::Text:
            if (byte == 0x1b) {
                state = State::Escape;
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        case State::Escape:
            if (byte == '[') {
                state = State::Csi;
            } else if (byte == ']') {
                state = State::Osc;
            } else {
                // Two-character escape (ESC D/E/H/M etc.): consume both.
                state = State::Text;
            }
            break;
        case State::Csi:
            // CSI ends at an intermediate/final byte in 0x40..0x7e.
            if (byte >= 0x40 && byte <= 0x7e)
                state = State::Text;
            break;
        case State::Osc:
            // OSC ends at BEL (0x07) or ST (ESC \) — check for ESC, keep ESC.
            if (byte == 0x07) {
                state = State::Text;
            } else if (byte == 0x1b) {
                state = State::Escape;
            }
            break;
        }
    }
    return output;
}

// JSON-escape a UTF-8 string for embedding in a single-line JSON document.
std::string jsonEscape(std::string_view input)
{
    static const char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() + 16);
    for (const unsigned char byte : input) {
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20) {
                output += "\\u00";
                output.push_back(hex[(byte >> 4) & 0x0f]);
                output.push_back(hex[byte & 0x0f]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    return output;
}

// Build the final JSON result document. stdout/stderr are stripped of ANSI
// sequences when requested so the payload is clean machine-readable text.
std::string buildJsonResult(
    int exitCode, std::string_view stdoutText, std::string_view stderrText,
    long long durationMs, bool stripAnsi)
{
    std::string result;
    result.reserve(stdoutText.size() + stderrText.size() + 128);
    result += "{\"exitCode\":";
    result += std::to_string(exitCode);
    result += ",\"stdout\":\"";
    result += jsonEscape(stripAnsi ? stripAnsiEscapes(stdoutText) : stdoutText);
    result += "\",\"stderr\":\"";
    result += jsonEscape(stripAnsi ? stripAnsiEscapes(stderrText) : stderrText);
    result += "\",\"durationMs\":";
    result += std::to_string(durationMs);
    result += "}";
    return result;
}

void writeJsonFailure(int exitCode, std::string_view message)
{
    writeOutput(buildJsonResult(exitCode, {}, message, 0, true));
    writeOutput("\r\n");
}

class ConsoleConfiguration final
{
public:
    ConsoleConfiguration()
    {
        _setmode(_fileno(stdout), _O_BINARY);
        SetConsoleOutputCP(CP_UTF8);
        output_ = consoleOutput();
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        hasOutputMode_ = GetConsoleMode(output_, &outputMode_) != FALSE;
        if (hasOutputMode_)
            SetConsoleMode(
                output_, outputMode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        hasInputMode_ = GetConsoleMode(input_, &inputMode_) != FALSE;
        if (hasInputMode_) {
            SetConsoleMode(
                input_, inputMode_ & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
                                      | ENABLE_PROCESSED_INPUT));
        }
    }

    ~ConsoleConfiguration()
    {
        if (hasInputMode_)
            SetConsoleMode(input_, inputMode_);
        if (hasOutputMode_)
            SetConsoleMode(output_, outputMode_);
    }

private:
    HANDLE output_ = INVALID_HANDLE_VALUE;
    HANDLE input_ = INVALID_HANDLE_VALUE;
    DWORD outputMode_ = 0;
    DWORD inputMode_ = 0;
    bool hasOutputMode_ = false;
    bool hasInputMode_ = false;
};

void showHelp()
{
    writeConsoleText(
        L"用法：MasterTerm-cli.exe [选项] 用户名@主机\r\n\r\n"
        L"MasterTerm 无界面 SSH 连接器\r\n\r\n"
        L"选项：\r\n"
        L"  -h, --help                 显示帮助\r\n"
        L"  -v, --version              显示版本\r\n"
        L"  --profile <名称>           使用已保存的连接配置\r\n"
        L"  --port <端口>              SSH 端口，默认 22\r\n"
        L"  --identity <路径>          私钥文件路径\r\n"
        L"  --proxy-jump <地址>        跳板机 user@host[:port]\r\n"
        L"  --password <密码>          密码（不建议写在命令行中）\r\n"
        L"  --proxy-password <密码>    跳板机密码\r\n"
        L"  --exec <命令>              非交互模式：远程执行单条命令后退出\r\n"
        L"  --scp-upload <本地文件>    上传单个文件到远端\r\n"
        L"  --scp-download <远端文件>  从远端下载单个文件\r\n"
        L"  --timeout <秒>             命令超时（默认不限）\r\n"
        L"  --agent                    agent 模式：只输出一行 JSON（需与 --exec 一起）\r\n"
        L"  --json                     以 JSON 输出结果（需与 --exec 一起）\r\n"
        L"  --strip-ansi               剥离输出中的 ANSI 转义序列\r\n\r\n"
        L"示例：\r\n"
        L"  MasterTerm-cli.exe --profile 生产环境\r\n"
        L"  MasterTerm-cli.exe master@127.0.0.1\r\n"
        L"  MasterTerm-cli.exe --exec \"df -h\" master@127.0.0.1\r\n"
        L"  echo hi | MasterTerm-cli.exe --exec \"cat\" master@127.0.0.1\r\n"
        L"  MasterTerm-cli.exe --scp-upload C:\\notes.txt user@example.com /tmp/notes.txt\r\n"
        L"  MasterTerm-cli.exe --scp-download /tmp/notes.txt user@example.com C:\\notes.txt\r\n"
        L"  MasterTerm-cli.exe --agent --profile 生产环境 --exec \"uname -a\"\r\n"
        L"  MasterTerm-cli.exe --json --strip-ansi --exec \"df -h\" master@127.0.0.1\r\n");
}

} // namespace

int wmain(int argc, wchar_t *argv[])
{
    ConsoleConfiguration console;
    const ParsedArguments arguments = parseArguments(argc, argv);
    if (!arguments.error.empty()) {
        writeOutput(arguments.error + "\r\n");
        return 2;
    }
    if (arguments.help) {
        showHelp();
        return 0;
    }
    if (arguments.version) {
        writeOutput("MasterTerm-cli " MASTERTERM_VERSION "\r\n");
        return 0;
    }

    ConnectionOptions options;
    std::string optionsError;
    if (!resolveOptions(arguments, &options, &optionsError)) {
        if (arguments.agent || arguments.json)
            writeJsonFailure(2, optionsError);
        else
            writeOutput(optionsError + "\r\n");
        return 2;
    }
    if (options.password.empty())
        options.password = environmentUtf8(L"MASTERTERM_SSH_PASSWORD");
    if (options.password.empty() && options.keyPath.empty()) {
        if (options.jsonOutput) {
            writeJsonFailure(2, "未提供 SSH 密码或私钥；agent 模式不会打开交互式密码提示。");
            return 2;
        }
        options.password = readHiddenLine(
            options.connection + " 密码: ");
    }
    if (options.proxyPassword.empty()) {
        options.proxyPassword =
            environmentUtf8(L"MASTERTERM_PROXY_PASSWORD");
    }

    if (!options.scpUpload.empty() || !options.scpDownload.empty())
        return runScpTransfer(options);

    auto session = std::make_unique<SshSession>();
    bool finished = false;
    int exitCode = 0;
    const bool execMode = !options.command.empty();
    std::string stdoutText;
    std::string stderrText;

    if (execMode) {
        // Non-interactive single-command mode: route stdout/stderr to the
        // matching local handles (or buffer them for --json), keep banners on
        // stderr, and stop as soon as the remote channel closes (exit code
        // carries the remote status).
        if (options.jsonOutput) {
            session->setDataHandler(
                [&stdoutText](const std::string &output) { stdoutText += output; });
            session->setStderrHandler(
                [&stderrText](const std::string &output) { stderrText += output; });
            session->setStartedHandler([] {});
            session->setFinishedHandler([&](int code) {
                exitCode = code;
                finished = true;
            });
            session->setErrorHandler([&] {
                stderrText = session->errorString();
                exitCode = 255;
                finished = true;
            });
        } else if (options.stripAnsi) {
            session->setDataHandler(
                [](const std::string &output) { writeOutput(stripAnsiEscapes(output)); });
            session->setStderrHandler(
                [](const std::string &output) { writeError(stripAnsiEscapes(output)); });
            session->setStartedHandler(
                [] { writeError("[MasterTerm] SSH 已连接\r\n"); });
            session->setFinishedHandler([&](int code) {
                exitCode = code;
                finished = true;
            });
            session->setErrorHandler([&] {
                writeError("[连接失败] " + session->errorString() + "\r\n");
                exitCode = 255;
                finished = true;
            });
        } else {
            session->setDataHandler(
                [](const std::string &output) { writeOutput(output); });
            session->setStderrHandler(
                [](const std::string &output) { writeError(output); });
            session->setStartedHandler(
                [] { writeError("[MasterTerm] SSH 已连接\r\n"); });
            session->setFinishedHandler([&](int code) {
                exitCode = code;
                finished = true;
            });
            session->setErrorHandler([&] {
                writeError("[连接失败] " + session->errorString() + "\r\n");
                exitCode = 255;
                finished = true;
            });
        }
    } else {
        session->setDataHandler(
            [](const std::string &output) { writeOutput(output); });
        session->setStartedHandler(
            [] { writeOutput("\r\n[MasterTerm] SSH 已连接\r\n"); });
        session->setErrorHandler([&] {
            writeOutput("[连接失败] " + session->errorString() + "\r\n");
            exitCode = 255;
            finished = true;
        });
        session->setFinishedHandler([&](int code) {
            writeOutput(
                "\r\n[会话结束] exit code: " + std::to_string(code) + "\r\n");
            exitCode = code;
            finished = true;
        });
    }
    session->start(
        options.connection, options.port, options.password, options.keyPath,
        options.proxyJump, options.proxyPassword, options.proxyKeyPath,
        options.command);

    const auto startTime = std::chrono::steady_clock::now();
    std::string pendingStdin;
    std::mutex stdinMutex;
    bool stdinDone = false;
    std::thread stdinReader;
    if (execMode && stdinIsPipe()) {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        stdinReader = std::thread([input, &stdinMutex, &pendingStdin, &stdinDone] {
            char buffer[16384];
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr)
                    || read == 0) {
                    break;
                }
                std::lock_guard<std::mutex> lock(stdinMutex);
                pendingStdin.append(buffer, read);
            }
            {
                std::lock_guard<std::mutex> lock(stdinMutex);
                stdinDone = true;
            }
        });
    }
    while (!finished) {
        session->poll();
        if (execMode) {
            std::string piped;
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(stdinMutex);
                piped.swap(pendingStdin);
                done = stdinDone;
            }
            if (!piped.empty()) {
                if (session->isRunning()) {
                    session->write(piped);
                } else {
                    // The pipe may deliver all data before the SSH session
                    // reaches Running (connection setup takes hundreds of
                    // milliseconds).  Put it back so nothing is lost.
                    std::lock_guard<std::mutex> lock(stdinMutex);
                    pendingStdin.insert(0, piped);
                }
            }
            if (done && piped.empty() && session->isRunning())
                session->sendEof();
        } else {
            sendConsoleInput(session.get());
        }
        if (options.timeoutSeconds > 0
            && std::chrono::steady_clock::now() - startTime
                >= std::chrono::seconds(options.timeoutSeconds)) {
            if (!finished) {
                if (options.jsonOutput) {
                    stderrText = "命令超时：超过 "
                        + std::to_string(options.timeoutSeconds)
                        + " 秒未完成。";
                    exitCode = 124;
                } else {
                    writeError("[超时] 命令超过 " + std::to_string(options.timeoutSeconds)
                               + " 秒未完成。\r\n");
                    exitCode = 124;
                }
                finished = true;
            }
        }
        Sleep(10);
    }
    if (stdinReader.joinable()) {
        // An agent may keep its stdin pipe open while the remote command has
        // already failed or timed out. Cancel a pending ReadFile so cleanup
        // cannot turn a completed request into a hung process.
        CancelSynchronousIo(stdinReader.native_handle());
        stdinReader.join();
    }
    if (options.jsonOutput) {
        const long long durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        writeOutput(buildJsonResult(exitCode, stdoutText, stderrText,
                                    durationMs, options.stripAnsi));
        writeOutput("\r\n");
    }
    return exitCode;
}
