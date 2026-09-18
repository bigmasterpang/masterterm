#include "WebViewBackend.h"

#include "DiagnosticLog.h"
#include "NativeCrypto.h"
#include "SftpTransferProtocol.h"

#include "NativeProcess.h"
#include "NativeConfigFile.h"
#include "NativeFile.h"
#include "NativeJson.h"
#include "NativeJsonDom.h"
#include "NativeBase64.h"
#include "NativeDataDir.h"
#include "NativeHash.h"
#include "NativeKnownHosts.h"
#include "NativeMetrics.h"
#include "NativeString.h"
#include "LocalShellSession.h"
#include "RdpSession.h"
#include "SerialSession.h"
#include "SshSession.h"
#include "SshTunnel.h"

#include <vector>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <set>
#include <thread>
#include <winhttp.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl.h>
#include <wincred.h>
#include <wrl/client.h>
#endif

namespace {

std::filesystem::path nativePath(const NativeString &value)
{
    return std::filesystem::path(value.toStdWString());
}

NativeString pathText(const std::filesystem::path &value)
{
    return NativeString::fromStdWString(value.wstring());
}

bool openSystemClipboard()
{
#ifdef _WIN32
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(10);
    }
#endif
    return false;
}

NativeString cleanRemotePath(NativeString value)
{
    const NativeString trimmed = value.trimmed();
    if (trimmed.isEmpty() || trimmed == NativeString("."))
        return trimmed;
    value.replace(static_cast<char>('\\'), static_cast<char>('/'));
    const std::string input = value.toUtf8();
    std::vector<std::string> cleanParts;
    std::size_t start = 0;
    while (start <= input.size()) {
        const std::size_t end = input.find('/', start);
        const std::string_view part(
            input.data() + start,
            (end == std::string::npos ? input.size() : end) - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!cleanParts.empty())
                    cleanParts.pop_back();
            } else {
                cleanParts.emplace_back(part);
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    std::string output("/");
    for (std::size_t index = 0; index < cleanParts.size(); ++index) {
        if (index != 0) output.push_back('/');
        output += cleanParts[index];
    }
    return NativeString::fromUtf8(output.data(), static_cast<int>(output.size()));
}

NativeString remoteChildPath(const NativeString &directory, const NativeString &name)
{
    return cleanRemotePath(directory + static_cast<char>('/') + name);
}

NativeString fileNameText(NativeString value)
{
    value.replace(static_cast<char>('\\'), static_cast<char>('/'));
    while (value.endsWith(static_cast<char>('/')))
        value.chop(1);
    const int separator = value.lastIndexOf(static_cast<char>('/'));
    return separator < 0 ? value : value.mid(separator + 1);
}

NativeString remoteEditKey(int profileIndex, const NativeString &remotePath)
{
    return NativeString::number(profileIndex) + NativeString(":") + remotePath;
}

bool openLocalFileWithWindows(const NativeString &path, bool chooseApplication,
                              const NativeString &applicationPath = {})
{
#ifdef _WIN32
    const std::wstring nativeFile = path.toStdWString();
    const std::wstring nativeApplication = applicationPath.toStdWString();
    if (!nativeApplication.empty()) {
        const std::wstring parameters = L"\"" + nativeFile + L"\"";
        const HINSTANCE launched = ShellExecuteW(
            nullptr, L"open", nativeApplication.c_str(), parameters.c_str(),
            nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(launched) > 32;
    }
    if (chooseApplication) {
        OPENASINFO info{};
        info.pcszFile = nativeFile.c_str();
        info.oaifInFlags = OAIF_EXEC;
        return SUCCEEDED(SHOpenWithDialog(nullptr, &info));
    }
    const HINSTANCE launched = ShellExecuteW(
        nullptr, L"open", nativeFile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(launched) > 32;
#else
    (void)path;
    (void)chooseApplication;
    return false;
#endif
}

#ifdef _WIN32
int CALLBACK collectFixedPitchFont(const LOGFONTW *font, const TEXTMETRICW *metrics,
                                  DWORD, LPARAM user)
{
    auto *families = reinterpret_cast<std::set<std::wstring> *>(user);
    if (!font || !metrics || !families || font->lfFaceName[0] == L'@') return 1;
    // TMPF_FIXED_PITCH is counterintuitively set for variable-width fonts.
    if ((metrics->tmPitchAndFamily & TMPF_FIXED_PITCH) != 0) return 1;
    families->emplace(font->lfFaceName);
    return 1;
}

NativeJsonDom::Array installedTerminalFonts()
{
    std::set<std::wstring> families;
    HDC device = CreateCompatibleDC(nullptr);
    if (device) {
        LOGFONTW query{};
        query.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(device, &query,
            reinterpret_cast<FONTENUMPROCW>(collectFixedPitchFont),
            reinterpret_cast<LPARAM>(&families), 0);
        DeleteDC(device);
    }
    NativeJsonDom::Array result;
    for (const std::wstring &family : families)
        result.values.emplace_back(NativeString::fromStdWString(family).toUtf8());
    return result;
}
#else
NativeJsonDom::Array installedTerminalFonts()
{
    return {};
}
#endif

NativeString environmentText(const wchar_t *name, const char *fallbackName)
{
#ifdef _WIN32
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required > 1) {
        std::wstring value(required - 1, L'\0');
        if (GetEnvironmentVariableW(
                name, value.data(), static_cast<DWORD>(value.size() + 1))
            == value.size())
            return NativeString::fromStdWString(value);
    }
#endif
    const char *value = std::getenv(fallbackName);
    return value ? NativeString::fromLocal8Bit(value) : NativeString();
}

NativeString userHomePath()
{
    return environmentText(L"USERPROFILE", "USERPROFILE");
}

NativeString downloadsPath()
{
    return pathText(nativePath(userHomePath()) / L"Downloads");
}

NativeString temporaryPath()
{
    const NativeString temporary = environmentText(L"TEMP", "TEMP");
    return temporary.isEmpty() ? userHomePath() : temporary;
}

std::filesystem::path knownHostsFilePath()
{
    return NativeDataDir::knownHostsFile();
}

std::int64_t fileTimeSeconds(const std::filesystem::file_time_type &value)
{
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

std::wstring toWide(const NativeString &value)
{
    return value.toStdWString();
}

std::vector<std::wstring> toWideArguments(
    const std::vector<NativeString> &arguments)
{
    std::vector<std::wstring> result;
    result.reserve(arguments.size());
    for (const NativeString &argument : arguments)
        result.push_back(argument.toStdWString());
    return result;
}

NativeString nativeProcessError(const NativeProcess *process)
{
    if (!process || process->errorMessage().empty())
        return NativeString("未知 Windows 进程错误");
    return NativeString::fromStdWString(process->errorMessage());
}

struct HttpResult {
    int status = 0;
    std::string body;
};

HttpResult httpRequestUtf8(
    const std::wstring &host, unsigned short port, bool secure,
    const std::wstring &method, const std::wstring &path,
    const std::wstring &headers, const std::string &body)
{
    HttpResult result;
    const HINTERNET session = WinHttpOpen(
        L"MasterTerm", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        nullptr, nullptr, 0);
    if (!session)
        return result;
    // Fail fast on unreachable sync servers instead of waiting for the
    // default multi-minute timeouts: 10s resolve/connect, 15s send, 30s
    // receive.
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    const HINTERNET connection = WinHttpConnect(
        session, host.c_str(), port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return result;
    }
    const HINTERNET request = WinHttpOpenRequest(
        connection, method.c_str(), path.c_str(), nullptr, nullptr, nullptr,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }
    const void *bodyData = body.empty() ? nullptr : body.data();
    const DWORD bodyLength = static_cast<DWORD>(body.size());
    if (WinHttpSendRequest(
            request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                     : headers.c_str(),
            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
            const_cast<void *>(bodyData), bodyLength, bodyLength, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(
                request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                WINHTTP_NO_HEADER_INDEX))
            result.status = static_cast<int>(statusCode);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available)
               && available > 0) {
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(
                    request, buffer.data(), available, &read))
                break;
            result.body.append(buffer.data(), read);
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

std::string httpGetUtf8(
    const std::wstring &host, const std::wstring &path)
{
    return httpRequestUtf8(
        host, INTERNET_DEFAULT_HTTPS_PORT, true, L"GET", path,
        std::wstring(), std::string()).body;
}

std::vector<int> parseVersionParts(const std::string &version)
{
    std::vector<int> parts;
    std::string cleaned = version;
    if (!cleaned.empty()
        && (cleaned.front() == 'v' || cleaned.front() == 'V'))
        cleaned.erase(cleaned.begin());
    std::size_t start = 0;
    while (start < cleaned.size()) {
        const std::size_t dot = cleaned.find('.', start);
        const std::string part = cleaned.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        try {
            parts.push_back(std::stoi(part));
        } catch (...) {
            break;
        }
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return parts;
}

std::string stripUtf8Bom(std::string value)
{
    if (value.size() >= 3
        && static_cast<unsigned char>(value[0]) == 0xEF
        && static_cast<unsigned char>(value[1]) == 0xBB
        && static_cast<unsigned char>(value[2]) == 0xBF)
        value.erase(0, 3);
    return value;
}

std::string safeTruncateUtf8(std::string value, std::size_t maxBytes)
{
    if (value.size() <= maxBytes)
        return value;
    std::size_t i = maxBytes;
    while (i > 0 && (static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) {
        --i;
    }
    if (i < maxBytes) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        std::size_t seqLen = 1;
        if ((lead & 0xE0) == 0xC0) seqLen = 2;
        else if ((lead & 0xF0) == 0xE0) seqLen = 3;
        else if ((lead & 0xF8) == 0xF0) seqLen = 4;
        if (i + seqLen <= maxBytes) {
            value.resize(i + seqLen);
            return value;
        }
    }
    value.resize(i);
    return value;
}

NativeString applicationDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return pathText(std::filesystem::current_path());
    path.resize(length);
    return pathText(std::filesystem::path(path).parent_path());
}

// Parses "https://host[:port][/prefix]" / "http://..." into its parts.
// Returns false for malformed URLs.
bool parseCloudEndpoint(const std::wstring &url, std::wstring &host,
                        unsigned short &port, bool &secure,
                        std::wstring &pathPrefix)
{
    std::wstring rest = url;
    secure = true;
    if (rest.rfind(L"https://", 0) == 0) {
        rest = rest.substr(8);
    } else if (rest.rfind(L"http://", 0) == 0) {
        rest = rest.substr(7);
        secure = false;
    } else {
        return false;
    }
    const std::size_t slash = rest.find(L'/');
    const std::wstring authority = rest.substr(0, slash);
    pathPrefix = slash == std::wstring::npos
        ? std::wstring() : rest.substr(slash);
    if (authority.empty())
        return false;
    const std::size_t colon = authority.find(L':');
    host = colon == std::wstring::npos
        ? authority : authority.substr(0, colon);
    port = 0;
    if (colon != std::wstring::npos) {
        const std::wstring portText = authority.substr(colon + 1);
        for (wchar_t character : portText) {
            if (character < L'0' || character > L'9')
                return false;
        }
        port = static_cast<unsigned short>(std::stoi(portText));
    }
    if (port == 0)
        port = secure ? 443 : 80;
    if (host.empty() || port > 65535)
        return false;
    return true;
}

// Appends the cloud API path to a possibly empty URL prefix.
std::wstring cloudApiPath(const std::wstring &pathPrefix,
                          const wchar_t *endpoint)
{
    std::wstring result = pathPrefix;
    if (result.empty() || result.back() != L'/')
        result.push_back(L'/');
    result += endpoint;
    return result;
}

std::map<std::wstring, std::wstring> sftpEnvironment(
    const NativeString &password, const NativeString &proxyJump,
    const NativeString &proxyPassword, const NativeString &proxyKeyPath,
    const NativeString &conflict = {})
{
    std::map<std::wstring, std::wstring> environment{
        {L"MASTERSSH_SFTP_PASSWORD", toWide(password)},
        {L"MASTERSSH_PROXY_JUMP", toWide(proxyJump)},
        {L"MASTERSSH_PROXY_PASSWORD", toWide(proxyPassword)},
        {L"MASTERSSH_PROXY_KEY_PATH", toWide(proxyKeyPath)}
    };
    if (!conflict.isNull())
        environment[L"MASTERSSH_SFTP_CONFLICT"] = toWide(conflict);
    return environment;
}

template<typename Map, typename Key>
typename Map::mapped_type takeMapped(Map &map, const Key &key)
{
    const auto it = map.find(key);
    if (it == map.end())
        return {};
    typename Map::mapped_type value = std::move(it->second);
    map.erase(it);
    return value;
}

NativeString chooseWindowsPath(const NativeString &title, const NativeString &initialPath,
                          bool pickFolder, bool saveFile)
{
#ifdef _WIN32
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    HRESULT result = saveFile
        ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&dialog))
        : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&dialog));
    if (FAILED(result) || !dialog)
        return {};

    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (pickFolder)
            options |= FOS_PICKFOLDERS;
        if (!saveFile && !pickFolder)
            options |= FOS_FILEMUSTEXIST;
        dialog->SetOptions(options);
    }
    dialog->SetTitle(title.toStdWString().c_str());

    const std::filesystem::path initialInfo = nativePath(initialPath);
    const std::filesystem::path absoluteInitial = std::filesystem::absolute(initialInfo);
    const NativeString folderPath = pathText(std::filesystem::is_directory(initialInfo)
        ? absoluteInitial : absoluteInitial.parent_path());
    Microsoft::WRL::ComPtr<IShellItem> initialFolder;
    if (!folderPath.isEmpty()
        && SUCCEEDED(SHCreateItemFromParsingName(
            folderPath.toStdWString().c_str(), nullptr,
            IID_PPV_ARGS(&initialFolder)))) {
        dialog->SetFolder(initialFolder.Get());
    }
    const NativeString initialFileName = pathText(initialInfo.filename());
    if (saveFile && !initialFileName.isEmpty())
        dialog->SetFileName(initialFileName.toStdWString().c_str());

    if (FAILED(dialog->Show(nullptr)))
        return {};
    Microsoft::WRL::ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected)) || !selected)
        return {};
    PWSTR path = nullptr;
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
        return {};
    const NativeString selectedPath = NativeString::fromWCharArray(path);
    CoTaskMemFree(path);
    return selectedPath;
#else
    static_cast<void>(title);
    static_cast<void>(initialPath);
    static_cast<void>(pickFolder);
    static_cast<void>(saveFile);
    return {};
#endif
}

NativeString credentialTarget(const NativeString &scope, const NativeString &address)
{
    return NativeString("MasterSSH/") + scope + NativeString("/") + address;
}

NativeString readStoredCredential(const NativeString &scope, const NativeString &address)
{
#ifdef _WIN32
    PCREDENTIALW credential = nullptr;
    const std::wstring target = credentialTarget(scope, address).toStdWString();
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const NativeString password = NativeString::fromUtf8(
            reinterpret_cast<const char *>(credential->CredentialBlob), credential->CredentialBlobSize);
        CredFree(credential);
        return password;
    }
#else
    static_cast<void>(scope);
    static_cast<void>(address);
#endif
    return {};
}

NativeString readStoredPassword(const NativeString &address)
{
    return readStoredCredential(NativeString("SSH"), address);
}

void saveStoredCredential(const NativeString &scope, const NativeString &address, const NativeString &password)
{
#ifdef _WIN32
    const std::string bytes = password.toUtf8();
    const std::wstring target = credentialTarget(scope, address).toStdWString();
    const std::wstring user = address.toStdWString();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t *>(target.c_str());
    credential.UserName = const_cast<wchar_t *>(user.c_str());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.data()));
    CredWriteW(&credential, 0);
#else
    static_cast<void>(scope);
    static_cast<void>(address);
    static_cast<void>(password);
#endif
}

void saveStoredPassword(const NativeString &address, const NativeString &password)
{
    saveStoredCredential(NativeString("SSH"), address, password);
}

void deleteStoredCredential(const NativeString &scope, const NativeString &address)
{
#ifdef _WIN32
    const std::wstring target = credentialTarget(scope, address).toStdWString();
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
#else
    static_cast<void>(scope);
    static_cast<void>(address);
#endif
}

std::vector<std::string> splitBytes(const std::string &value, char separator)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = value.find(separator, start);
        fields.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

NativeString utf8Text(const std::string &value)
{
    return NativeString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

std::string sanitizeUtf8(const std::string &value, bool &changed)
{
    changed = false;
    if (value.empty()) return {};
#ifdef _WIN32
    const int strictLength = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (strictLength > 0) return value;
    changed = true;
    const int wideLength = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wideLength <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(),
            static_cast<int>(value.size()), wide.data(), wideLength) != wideLength)
        return {};
    const int utf8Length = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return {};
    std::string result(static_cast<std::size_t>(utf8Length), '\0');
    return WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
        result.data(), utf8Length, nullptr, nullptr) == utf8Length
        ? result : std::string();
#else
    return value;
#endif
}

NativeString decodeBase64Text(const std::string &value)
{
    const std::string decoded = NativeBase64::decode(value);
    return NativeString::fromUtf8(decoded.data(), static_cast<int>(decoded.size()));
}

NativeString droppedFileToken()
{
    static std::atomic_uint64_t sequence{0};
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto value = now ^ (++sequence);
    return NativeString::number(static_cast<unsigned long long>(value), 16);
}

std::filesystem::path nativeConfigPath()
{
    return NativeDataDir::configFile();
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

std::filesystem::path sessionLogDirectory()
{
    return NativeDataDir::sessionLogDirectory();
}

std::filesystem::path localCommandHistoryFilePath()
{
    return NativeDataDir::localCommandHistoryFile();
}

NativeJsonDom::Object readNativeConfig(bool *available = nullptr)
{
    return NativeConfigFile::readObject(nativeConfigPath(), available);
}

bool writeNativeConfig(const NativeJsonDom::Object &config)
{
    return NativeConfigFile::writeObject(nativeConfigPath(), config);
}

struct ServerRecord
{
    std::string connectionType = "ssh";
    std::string name;
    std::string address;
    std::string port;
    std::string tag;
    std::string tagMode = "auto";
    std::string workspace = "未分配";
    std::string color = "#8ab4f8";
    std::string iconKey;
    std::string keyPath;
    std::string keyPassphrase;
    std::string proxyJump;
    std::string proxyKeyPath;
    std::string password;
    std::string proxyPassword;
    int serialDataBits = 8;
    std::string serialParity = "none";
    std::string serialStopBits = "1";
    std::string serialFlowControl = "none";
    RdpConnectionOptions rdpOptions;
    struct TunnelConfig {
        std::string mode = "local";
        std::string listenHost;
        int listenPort = 0;
        std::string targetHost;
        int targetPort = 0;
    };
    std::vector<TunnelConfig> tunnels;
};

std::string jsonText(
    const NativeJsonDom::Object &object, const char *key,
    std::string fallback = {})
{
    const auto found = object.values.find(key);
    if (found == object.values.end())
        return fallback;
    const NativeJsonDom::Value &value = found->second;
    if (value.isString())
        return value.string();
    if (value.isNumber())
        return std::to_string(static_cast<long long>(value.number()));
    return fallback;
}

int jsonInteger(
    const NativeJsonDom::Object &object, const char *key, int fallback)
{
    const auto found = object.values.find(key);
    if (found == object.values.end())
        return fallback;
    const NativeJsonDom::Value &value = found->second;
    if (value.isNumber())
        return static_cast<int>(value.number());
    if (value.isString()) {
        try {
            std::size_t consumed = 0;
            const int result = std::stoi(value.string(), &consumed);
            if (consumed == value.string().size())
                return result;
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

void readRdpOptions(
    const NativeJsonDom::Object &object, RdpConnectionOptions &options)
{
    const auto found = object.values.find("rdpOptions");
    if (found == object.values.end() || !found->second.isObject())
        return;
    const NativeJsonDom::Object &value = found->second.object();
    options.colorDepth = jsonInteger(value, "colorDepth", options.colorDepth);
    if (options.colorDepth != -1 && options.colorDepth != 16
        && options.colorDepth != 24 && options.colorDepth != 32)
        options.colorDepth = -1;
    options.smartSizing = jsonInteger(
        value, "smartSizing", options.smartSizing);
    if (options.smartSizing < -1 || options.smartSizing > 1)
        options.smartSizing = -1;
    options.performanceFlags = std::clamp(
        jsonInteger(value, "performanceFlags", options.performanceFlags),
        -1, 4095);
    options.networkConnectionType = std::clamp(
        jsonInteger(value, "networkConnectionType",
            options.networkConnectionType), -1, 6);
    const auto readBinaryOption = [&value](
        const char *name, int current) {
        const int result = jsonInteger(value, name, current);
        return result < -1 || result > 1 ? -1 : result;
    };
    options.desktopBackground = readBinaryOption(
        "desktopBackground", options.desktopBackground);
    options.fontSmoothing = readBinaryOption(
        "fontSmoothing", options.fontSmoothing);
    options.desktopComposition = readBinaryOption(
        "desktopComposition", options.desktopComposition);
    options.fullWindowDrag = readBinaryOption(
        "fullWindowDrag", options.fullWindowDrag);
    options.menuAnimations = readBinaryOption(
        "menuAnimations", options.menuAnimations);
    options.visualStyles = readBinaryOption(
        "visualStyles", options.visualStyles);
    options.cursorShadow = readBinaryOption(
        "cursorShadow", options.cursorShadow);
    options.cursorSettings = readBinaryOption(
        "cursorSettings", options.cursorSettings);
    options.useMultimon = readBinaryOption(
        "useMultimon", options.useMultimon);
    options.keyboardHookMode = jsonInteger(
        value, "keyboardHookMode", options.keyboardHookMode);
    if (options.keyboardHookMode < -1 || options.keyboardHookMode > 2)
        options.keyboardHookMode = -1;
    options.redirectClipboard = jsonInteger(
        value, "redirectClipboard", options.redirectClipboard);
    if (options.redirectClipboard < -1 || options.redirectClipboard > 1)
        options.redirectClipboard = -1;
    options.audioRedirectionMode = jsonInteger(
        value, "audioRedirectionMode", options.audioRedirectionMode);
    if (options.audioRedirectionMode < -1
        || options.audioRedirectionMode > 2)
        options.audioRedirectionMode = -1;
    options.redirectDrives = jsonInteger(
        value, "redirectDrives", options.redirectDrives);
    if (options.redirectDrives < -1 || options.redirectDrives > 1)
        options.redirectDrives = -1;
    options.redirectPrinters = jsonInteger(
        value, "redirectPrinters", options.redirectPrinters);
    if (options.redirectPrinters < -1 || options.redirectPrinters > 1)
        options.redirectPrinters = -1;
    options.autoReconnect = jsonInteger(
        value, "autoReconnect", options.autoReconnect);
    if (options.autoReconnect < -1 || options.autoReconnect > 1)
        options.autoReconnect = -1;
    options.maxReconnectAttempts = std::clamp(
        jsonInteger(
            value, "maxReconnectAttempts", options.maxReconnectAttempts),
        1, 20);
}

NativeJsonDom::Object rdpOptionsJson(const RdpConnectionOptions &options)
{
    NativeJsonDom::Object value;
    value.values.emplace(
        "colorDepth", static_cast<double>(options.colorDepth));
    value.values.emplace(
        "smartSizing", static_cast<double>(options.smartSizing));
    value.values.emplace(
        "performanceFlags", static_cast<double>(options.performanceFlags));
    value.values.emplace(
        "networkConnectionType",
        static_cast<double>(options.networkConnectionType));
    value.values.emplace(
        "desktopBackground", static_cast<double>(options.desktopBackground));
    value.values.emplace(
        "fontSmoothing", static_cast<double>(options.fontSmoothing));
    value.values.emplace(
        "desktopComposition",
        static_cast<double>(options.desktopComposition));
    value.values.emplace(
        "fullWindowDrag", static_cast<double>(options.fullWindowDrag));
    value.values.emplace(
        "menuAnimations", static_cast<double>(options.menuAnimations));
    value.values.emplace(
        "visualStyles", static_cast<double>(options.visualStyles));
    value.values.emplace(
        "cursorShadow", static_cast<double>(options.cursorShadow));
    value.values.emplace(
        "cursorSettings", static_cast<double>(options.cursorSettings));
    value.values.emplace(
        "useMultimon", static_cast<double>(options.useMultimon));
    value.values.emplace(
        "keyboardHookMode", static_cast<double>(options.keyboardHookMode));
    value.values.emplace(
        "redirectClipboard",
        static_cast<double>(options.redirectClipboard));
    value.values.emplace(
        "audioRedirectionMode",
        static_cast<double>(options.audioRedirectionMode));
    value.values.emplace(
        "redirectDrives", static_cast<double>(options.redirectDrives));
    value.values.emplace(
        "redirectPrinters", static_cast<double>(options.redirectPrinters));
    value.values.emplace(
        "autoReconnect", static_cast<double>(options.autoReconnect));
    value.values.emplace(
        "maxReconnectAttempts",
        static_cast<double>(options.maxReconnectAttempts));
    return value;
}

NativeString nativeText(const std::string &value)
{
    return NativeString::fromUtf8(
        value.data(), static_cast<int>(value.size()));
}

std::string utf8Text(const NativeString &value)
{
    return value.toUtf8();
}

std::string defaultServerTag(const std::string &connectionType)
{
    if (connectionType == "rdp")
        return "RDP";
    if (connectionType == "serial")
        return "串口";
    return "SSH";
}

ServerRecord serverRecordFromJson(const NativeJsonDom::Object &object)
{
    ServerRecord record;
    record.tag = jsonText(object, "tag", "SSH");
    std::string normalizedTag = record.tag;
    std::transform(
        normalizedTag.begin(), normalizedTag.end(), normalizedTag.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    record.connectionType = jsonText(
        object, "connectionType",
        normalizedTag == "serial" || normalizedTag == "串口" ? "serial"
            : normalizedTag == "rdp" ? "rdp" : "ssh");
    const std::string storedTagMode = jsonText(object, "tagMode");
    if (storedTagMode == "custom" || storedTagMode == "auto")
        record.tagMode = storedTagMode;
    else if (normalizedTag.empty() || normalizedTag == "ssh"
        || normalizedTag == "rdp" || normalizedTag == "serial"
        || normalizedTag == "串口")
        record.tagMode = "auto";
    else
        record.tagMode = "custom";
    const bool serial = record.connectionType == "serial";
    record.name = jsonText(object, "name");
    record.address = jsonText(object, "address");
    record.port = jsonText(object, "port", serial ? "115200" : "22");
    record.workspace = jsonText(object, "workspace", "未分配");
    record.color = jsonText(object, "color", "#8ab4f8");
    record.iconKey = jsonText(object, "iconKey", serial ? "serial" : "server");
    record.keyPath = jsonText(object, "keyPath");
    record.keyPassphrase = jsonText(object, "keyPassphrase");
    record.proxyJump = jsonText(object, "proxyJump");
    record.proxyKeyPath = jsonText(object, "proxyKeyPath");
    record.password = jsonText(object, "password");
    record.proxyPassword = jsonText(object, "proxyPassword");
    record.serialDataBits = jsonInteger(object, "serialDataBits", 8);
    record.serialParity = jsonText(object, "serialParity", "none");
    record.serialStopBits = jsonText(object, "serialStopBits", "1");
    record.serialFlowControl =
        jsonText(object, "serialFlowControl", "none");
    readRdpOptions(object, record.rdpOptions);
    const auto tunnels = object.values.find("tunnels");
    if (tunnels != object.values.end() && tunnels->second.isArray()) {
        for (const NativeJsonDom::Value &value :
             tunnels->second.array().values) {
            if (!value.isObject())
                continue;
            ServerRecord::TunnelConfig config;
            config.mode = jsonText(value.object(), "mode", "local");
            config.listenHost =
                jsonText(value.object(), "listenHost");
            config.listenPort = jsonInteger(value.object(), "listenPort", 0);
            config.targetHost = jsonText(value.object(), "targetHost");
            config.targetPort = jsonInteger(value.object(), "targetPort", 0);
            if (config.targetHost.empty() || config.targetPort <= 0
                || config.listenPort < 0 || config.listenPort > 65535
                || config.targetPort > 65535)
                continue;
            if (config.mode != "local" && config.mode != "remote")
                config.mode = "local";
            record.tunnels.push_back(std::move(config));
        }
    }
    return record;
}

NativeJsonDom::Object serverRecordJson(const ServerRecord &record)
{
    NativeJsonDom::Object object;
    object.values.emplace("connectionType", record.connectionType);
    object.values.emplace("name", record.name);
    object.values.emplace("address", record.address);
    object.values.emplace("port", record.port);
    object.values.emplace("tag", record.tag);
    object.values.emplace("tagMode", record.tagMode);
    object.values.emplace("workspace", record.workspace);
    object.values.emplace("color", record.color);
    object.values.emplace("iconKey", record.iconKey);
    object.values.emplace("keyPath", record.keyPath);
    object.values.emplace("proxyJump", record.proxyJump);
    object.values.emplace("proxyKeyPath", record.proxyKeyPath);
    object.values.emplace(
        "serialDataBits", static_cast<double>(record.serialDataBits));
    object.values.emplace("serialParity", record.serialParity);
    object.values.emplace("serialStopBits", record.serialStopBits);
    object.values.emplace("serialFlowControl", record.serialFlowControl);
    if (record.connectionType == "rdp")
        object.values.emplace(
            "rdpOptions",
            NativeJsonDom::Value(rdpOptionsJson(record.rdpOptions)));
    NativeJsonDom::Array tunnels;
    for (const ServerRecord::TunnelConfig &config : record.tunnels) {
        NativeJsonDom::Object tunnel;
        tunnel.values.emplace("mode", config.mode);
        tunnel.values.emplace("listenHost", config.listenHost);
        tunnel.values.emplace(
            "listenPort", static_cast<double>(config.listenPort));
        tunnel.values.emplace("targetHost", config.targetHost);
        tunnel.values.emplace(
            "targetPort", static_cast<double>(config.targetPort));
        tunnels.values.emplace_back(std::move(tunnel));
    }
    object.values.emplace("tunnels", NativeJsonDom::Value(std::move(tunnels)));
    return object;
}

std::vector<ServerRecord> readServerRecords()
{
    bool available = false;
    const NativeJsonDom::Object config = readNativeConfig(&available);
    std::vector<ServerRecord> records;
    if (!available)
        return records;
    const auto servers = config.values.find("servers");
    if (servers == config.values.end() || !servers->second.isArray())
        return records;
    for (const NativeJsonDom::Value &value :
         servers->second.array().values) {
        if (value.isObject())
            records.push_back(serverRecordFromJson(value.object()));
    }
    return records;
}

void writeServerRecords(const std::vector<ServerRecord> &records)
{
    NativeJsonDom::Object config = readNativeConfig();
    NativeJsonDom::Array servers;
    for (const ServerRecord &record : records)
        servers.values.emplace_back(serverRecordJson(record));
    config.values["servers"] = NativeJsonDom::Value(std::move(servers));
    writeNativeConfig(config);
}

std::vector<std::string> readWorkspaceNames()
{
    bool available = false;
    const NativeJsonDom::Object config = readNativeConfig(&available);
    const auto workspaces = config.values.find("workspaces");
    if (available && workspaces != config.values.end()
        && workspaces->second.isArray()) {
        std::vector<std::string> names;
        for (const NativeJsonDom::Value &value :
             workspaces->second.array().values) {
            if (!value.isString() || value.string().empty())
                continue;
            const std::string &name = value.string();
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
        names.erase(
            std::remove(names.begin(), names.end(), "未分配"), names.end());
        names.insert(names.begin(), "未分配");
        return names;
    }
    return {"未分配"};
}

void writeWorkspaceNames(const std::vector<std::string> &names)
{
    NativeJsonDom::Object config = readNativeConfig();
    NativeJsonDom::Array workspaces;
    for (const std::string &name : names)
        workspaces.values.emplace_back(name);
    config.values["workspaces"] =
        NativeJsonDom::Value(std::move(workspaces));
    writeNativeConfig(config);
}

bool applyProfileParams(
    ServerRecord &record, const NativeJsonDom::Object &params,
    NativeString &error)
{
    const NativeString type = nativeText(NativeJsonDom::stringValue(
        params, "connectionType", record.connectionType))
                             .trimmed().toLower();
    if (type != NativeString("ssh") && type != NativeString("serial")
        && type != NativeString("rdp")) {
        error = NativeString("连接类型无效");
        return false;
    }
    const NativeString address = nativeText(NativeJsonDom::stringValue(
        params, "address", record.address)).trimmed();
    if (address.isEmpty()) {
        error = type == NativeString("serial")
            ? NativeString("串口名称不能为空") : NativeString("服务器地址不能为空");
        return false;
    }
    NativeString name = nativeText(NativeJsonDom::stringValue(
        params, "name", record.name)).trimmed();
    if (name.isEmpty())
        name = address;
    const int defaultPort = type == NativeString("serial") ? 115200
        : (type == NativeString("rdp") ? 3389 : 22);
    int port = NativeJsonDom::convertedIntegerValue(params, "port");
    if (port <= 0)
        port = nativeText(record.port).toInt();
    if (port <= 0)
        port = defaultPort;
    if (port <= 0 || port > (type == NativeString("serial") ? 4000000 : 65535)) {
        error = type == NativeString("serial")
            ? NativeString("波特率无效") : NativeString("端口号无效");
        return false;
    }

    record.connectionType = utf8Text(type);
    record.name = utf8Text(name);
    record.address = utf8Text(address);
    record.port = std::to_string(port);
    const NativeString tagMode = nativeText(NativeJsonDom::stringValue(
        params, "tagMode", record.tagMode)).trimmed().toLower();
    record.tagMode = tagMode == NativeString("custom") ? "custom" : "auto";
    if (record.tagMode == "auto") {
        record.tag = defaultServerTag(utf8Text(type));
    } else {
        record.tag = utf8Text(nativeText(NativeJsonDom::stringValue(
            params, "tag", record.tag)).trimmed());
    }
    NativeString workspace = nativeText(NativeJsonDom::stringValue(
        params, "workspace", record.workspace)).trimmed();
    if (workspace.isEmpty())
        workspace = NativeString("未分配");
    const std::vector<std::string> workspaces = readWorkspaceNames();
    const std::string workspaceText = utf8Text(workspace);
    if (std::find(workspaces.begin(), workspaces.end(), workspaceText)
        == workspaces.end()) {
        error = NativeString("工作区不存在，请先创建工作区");
        return false;
    }
    record.workspace = workspaceText;
    record.color = NativeJsonDom::stringValue(
        params, "color", record.color);
    record.iconKey = NativeJsonDom::stringValue(
        params, "icon",
        record.iconKey.empty()
            ? (type == NativeString("serial") ? "serial" : "server")
            : record.iconKey);
    if (type == NativeString("rdp"))
        readRdpOptions(params, record.rdpOptions);
    if (NativeJsonDom::contains(params, "keyPath"))
        record.keyPath = utf8Text(nativeText(
            NativeJsonDom::stringValue(params, "keyPath")).trimmed());
    if (NativeJsonDom::contains(params, "proxyJump"))
        record.proxyJump = utf8Text(nativeText(
            NativeJsonDom::stringValue(params, "proxyJump")).trimmed());
    if (NativeJsonDom::contains(params, "proxyKeyPath"))
        record.proxyKeyPath = utf8Text(nativeText(
            NativeJsonDom::stringValue(params, "proxyKeyPath")).trimmed());
    if (type == NativeString("serial")) {
        record.serialDataBits = std::clamp(
            NativeJsonDom::integerValue(
                params, "serialDataBits", record.serialDataBits),
            5, 8);
        record.serialParity = NativeJsonDom::stringValue(
            params, "serialParity", record.serialParity);
        record.serialStopBits = NativeJsonDom::stringValue(
            params, "serialStopBits", record.serialStopBits);
        record.serialFlowControl = NativeJsonDom::stringValue(
            params, "serialFlowControl", record.serialFlowControl);
    }
    return true;
}

} // namespace

WebViewBackend::WebViewBackend() = default;

void WebViewBackend::beginShutdown()
{
    m_shuttingDown.store(true, std::memory_order_release);
}

void WebViewBackend::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_release);
    if (m_shutdownComplete.exchange(true, std::memory_order_acq_rel))
        return;
    DiagnosticLog::write("backend-shutdown");
    const std::vector<NativeProcess *> processes(
        m_sftpProcesses.begin(), m_sftpProcesses.end());
    m_sftpProcesses.clear();
    m_sftpListRequests.clear();
    m_sftpOperationRequests.clear();
    m_sftpHistoryRequests.clear();
    m_remoteEditDownloads.clear();
    m_remoteEditUploads.clear();
    m_remoteEdits.clear();
    m_remoteMonitorRequests.clear();
    m_remoteMonitorBuffers.clear();
    m_remoteMonitors.clear();
    m_remoteLatencyRequests.clear();
    m_remoteLatencyProbes.clear();
    m_sftpTransferIds.clear();
    m_sftpTransferNames.clear();
    m_sftpTransferBuffers.clear();
    m_sftpStandardOutputs.clear();
    m_sftpStandardErrors.clear();
    m_sftpTransfers.clear();
    m_sftpStreamTransfers.clear();
    m_pendingConfigImports.clear();
    std::vector<NativeString> droppedTokens;
    droppedTokens.reserve(m_droppedFiles.size());
    for (const auto &entry : m_droppedFiles) droppedTokens.push_back(entry.first);
    for (const NativeString &token : droppedTokens)
        discardDroppedFile(token);
    // Tunnels borrow the libssh2 session pointer, so they must be released
    // before the SSH sessions themselves.
    m_tunnels.clear();
    for (const auto &entry : m_sshSessions) delete entry.second;
    m_sshSessions.clear();
    for (const auto &entry : m_serialSessions) delete entry.second;
    m_serialSessions.clear();
    for (const auto &entry : m_localSessions) delete entry.second;
    m_localSessions.clear();
    // SFTP worker processes are independent of the sessions above; terminate
    // them last so a worker finishing its final write still finds the
    // backend state it expects during the session teardown above.
    for (NativeProcess *process : processes) {
        const NativeString temporaryPath = takeMapped(m_sftpTemporaryFiles, process);
        delete process;
        if (!temporaryPath.isEmpty())
            std::filesystem::remove(nativePath(temporaryPath));
    }
}

WebViewBackend::~WebViewBackend()
{
    shutdown();
}

void WebViewBackend::startExitSelfTest(
    std::function<void(int exitCode)> completion)
{
    if (m_exitSelfTest)
        return;
    m_exitSelfTestCompletion = std::move(completion);
    m_exitSelfTest = std::make_unique<ExitSelfTestState>();
    m_exitSelfTest->stageStarted = std::chrono::steady_clock::now();
}

void WebViewBackend::finishExitSelfTest(int exitCode)
{
    if (!m_exitSelfTest)
        return;
    const NativeString sshSessionId = m_exitSelfTest->sshSessionId;
    const NativeString localSessionId = m_exitSelfTest->localSessionId;
    // Diagnostic trail for tools/connect-exit-stress.ps1 failures: keep the
    // stage and error visible without touching the user's session logs.
    {
        std::error_code directoryError;
        const std::filesystem::path directory =
            NativeDataDir::sessionLogDirectory();
        std::filesystem::create_directories(directory, directoryError);
        if (!directoryError) {
            std::ofstream file(
                directory / L"selftest.log",
                std::ios::binary | std::ios::app);
            if (file) {
                file << "exit=" << exitCode
                     << " stage=" << m_exitSelfTest->stage
                     << " profile=" << m_exitSelfTest->profileIndex
                     << " error=" << m_exitSelfTest->sshError
                     << " localError=" << m_exitSelfTest->localError
                     << '\n';
            }
        }
    }
    m_exitSelfTest.reset();
    // Clean up in the same order the normal shutdown path uses: SSH first,
    // then ConPTY.  SFTP workers spawned by the self test are already gone
    // (the state machine waits for m_sftpProcesses to empty).
    if (!sshSessionId.isEmpty())
        removeSession(sshSessionId);
    if (!localSessionId.isEmpty())
        removeSession(localSessionId);
    const auto completion = std::move(m_exitSelfTestCompletion);
    m_exitSelfTestCompletion = {};
    if (completion)
        completion(exitCode);
}

void WebViewBackend::pollExitSelfTest()
{
    if (!m_exitSelfTest)
        return;
    ExitSelfTestState &state = *m_exitSelfTest;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const auto expired = [&](int seconds) {
        return now - state.stageStarted > std::chrono::seconds(seconds);
    };
    switch (state.stage) {
    case 0: {
        // Pick the first SSH profile with usable credentials.  Prefer
        // profiles with an explicit password or key path (matching the
        // profile selection in tools/connect-exit-stress.ps1); only fall
        // back to Credential Manager stored passwords when none exists, so
        // a local profile with a stale stored credential does not shadow a
        // real reachable test server.
        const std::vector<ServerRecord> records = readServerRecords();
        int index = -1;
        for (std::size_t i = 0; i < records.size(); ++i) {
            const ServerRecord &record = records.at(i);
            if (record.connectionType != "ssh" || record.address.empty())
                continue;
            if (!record.password.empty() || !record.keyPath.empty()) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            for (std::size_t i = 0; i < records.size(); ++i) {
                const ServerRecord &record = records.at(i);
                if (record.connectionType != "ssh" || record.address.empty())
                    continue;
                if (!readStoredPassword(nativeText(record.address)).isEmpty()) {
                    index = static_cast<int>(i);
                    break;
                }
            }
        }
        if (index < 0) {
            finishExitSelfTest(10);
            return;
        }
        state.profileIndex = index;
        const ServerRecord &record =
            records.at(static_cast<std::size_t>(index));
        const NativeString address = nativeText(record.address);
        const NativeString configuredPassword = nativeText(record.password);
        const NativeString password = configuredPassword.isEmpty()
            ? readStoredPassword(address) : configuredPassword;
        const NativeString keyPath = nativeText(record.keyPath);
        const NativeString keyPassphrase =
            nativeText(record.keyPassphrase).isEmpty()
                ? readStoredCredential(NativeString("KeyPassphrase"), address)
                : nativeText(record.keyPassphrase);
        const NativeString proxyJump = nativeText(record.proxyJump);
        const NativeString configuredProxyPassword =
            nativeText(record.proxyPassword);
        const NativeString proxyPassword = configuredProxyPassword.isEmpty()
            ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
            : configuredProxyPassword;
        const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
        const std::string port = record.port.empty() ? "22" : record.port;

        auto *ssh = new SshSession();
        state.sshSessionId = NativeString(
            "selftest-ssh-" + std::to_string(m_nextSessionNumber++));
        m_sshSessions.emplace(state.sshSessionId, ssh);
        m_sessionProfiles.emplace(state.sshSessionId, index);
        ssh->setStartedHandler([this] {
            if (m_exitSelfTest) {
                m_exitSelfTest->sshConnected = true;
                DiagnosticLog::write(
                    "ssh-connected",
                    m_exitSelfTest->sshSessionId.toStdString());
            }
        });
        ssh->setDataHandler([this](const std::string &output) {
            if (!m_exitSelfTest || output.empty())
                return;
            m_exitSelfTest->sshOutput += output;
            if (m_exitSelfTest->sshOutput.find("MT_SELFTEST_READY")
                != std::string::npos)
                m_exitSelfTest->markerSeen = true;
        });
        // New host keys are auto-added by the session; only a changed key
        // pauses.  The self test never overrides a key mismatch, so a
        // changed fingerprint surfaces as a deterministic failure.
        DiagnosticLog::setSession(state.sshSessionId.toStdString());
        DiagnosticLog::setPhase("ssh-connect");
        DiagnosticLog::write("ssh-open", state.sshSessionId.toStdString());
        ssh->start(
            address.toStdString(), port, password.toStdString(),
            keyPath.toStdString(), proxyJump.toStdString(),
            proxyPassword.toStdString(), proxyKeyPath.toStdString(),
            std::string(), keyPassphrase.toStdString());
        state.stage = 1;
        state.stageStarted = now;
        return;
    }
    case 1: {
        SshSession *ssh = sshSession(state.sshSessionId);
        if (!ssh) {
            finishExitSelfTest(11);
            return;
        }
        if (ssh->hostKeyPending()) {
            state.sshError = "SSH 主机密钥已更改，请先手动确认后重试";
            finishExitSelfTest(11);
            return;
        }
        if (!ssh->isRunning() && !ssh->errorString().empty()) {
            state.sshError = ssh->errorString();
            finishExitSelfTest(11);
            return;
        }
        if (state.sshConnected && !state.markerRequested) {
            state.markerRequested = true;
            ssh->write("echo MT_SELFTEST_READY\r");
        }
        if (state.markerSeen) {
            state.stage = 2;
            state.stageStarted = now;
            // Spawn the real SFTP worker through the production path.  The
            // result response targets an unknown request id and is ignored
            // by the frontend; the lifecycle check only observes the worker
            // being spawned and fully released again.
            startSftpList(
                NativeString("selftest-sftp"), state.profileIndex,
                NativeString("/"));
            state.sftpStarted = !m_sftpProcesses.empty();
        } else if (expired(30)) {
            state.sshError = "SSH 连接/标记回读超时（30 秒）";
            finishExitSelfTest(11);
        }
        return;
    }
    case 2: {
        if (!state.sftpStarted) {
            state.sshError = "SFTP 工作进程未启动";
            finishExitSelfTest(11);
            return;
        }
        if (m_sftpProcesses.empty()) {
            state.stage = 3;
            state.stageStarted = now;
            auto *local = new LocalShellSession();
            state.localSessionId = NativeString(
                "selftest-local-" + std::to_string(m_nextSessionNumber++));
            m_localSessions.emplace(state.localSessionId, local);
            local->setStartedHandler([this] {
                if (m_exitSelfTest) {
                    m_exitSelfTest->localStarted = true;
                    DiagnosticLog::write(
                        "local-started",
                        m_exitSelfTest->localSessionId.toStdString());
                }
            });
            DiagnosticLog::write(
                "local-open", state.localSessionId.toStdString());
            local->start();
        } else if (expired(30)) {
            state.sshError = "SFTP 目录读取超时（30 秒）";
            finishExitSelfTest(11);
        }
        return;
    }
    case 3: {
        LocalShellSession *local = localSession(state.localSessionId);
        if (!local) {
            finishExitSelfTest(11);
            return;
        }
        if (state.localStarted) {
            state.stage = 4;
            state.stageStarted = now;
            return;
        }
        if (!local->errorString().empty()) {
            state.localError = local->errorString();
            finishExitSelfTest(11);
            return;
        }
        if (expired(10)) {
            state.localError = "ConPTY 本地终端启动超时（10 秒）";
            finishExitSelfTest(11);
        }
        return;
    }
    case 4: {
        // T2-2 验证：云同步导出的所有凭据字段必须是 DPAPI 密文或空
        // （服务器只见密文），该检查无任何副作用。
        const NativeJsonDom::Array exported = exportCloudServers();
        bool secretsEncrypted = true;
        for (const NativeJsonDom::Value &entry : exported.values) {
            if (!entry.isObject())
                continue;
            const NativeJsonDom::Object &profile = entry.object();
            for (const char *key :
                 {"password", "keyPassphrase", "proxyPassword",
                  "keyData", "proxyKeyData"}) {
                const auto found = profile.values.find(key);
                if (found == profile.values.end() || !found->second.isString())
                    continue;
                const std::string text = found->second.string();
                if (!text.empty() && !NativeCrypto::isEncrypted(text))
                    secretsEncrypted = false;
            }
        }
        if (!secretsEncrypted) {
            state.sshError = "云同步导出包含未加密的凭据";
            finishExitSelfTest(11);
            return;
        }
        finishExitSelfTest(0);
        return;
    }
    default:
        finishExitSelfTest(11);
        return;
    }
}

void WebViewBackend::poll()
{
    if (m_shuttingDown.load(std::memory_order_acquire))
        return;
    try {
    const std::vector<SshSession *> sshSessions = [&] {
        std::vector<SshSession *> result;
        result.reserve(m_sshSessions.size());
        for (const auto &entry : m_sshSessions)
            result.push_back(entry.second);
        return result;
    }();
    for (SshSession *session : sshSessions)
        session->poll();

    for (const auto &entry : m_tunnels) {
        const std::string &sessionId = entry.first;
        SshSession *session = sshSession(NativeString(sessionId));
        const bool sessionAlive = session && session->isRunning();
        for (const auto &tunnel : entry.second) {
            if (!sessionAlive) {
                if (tunnel->state() == SshTunnel::State::Starting
                    || tunnel->active())
                    tunnel->invalidate();
                continue;
            }
            tunnel->poll();
        }
    }

    const std::vector<SerialSession *> serialSessions = [&] {
        std::vector<SerialSession *> result;
        result.reserve(m_serialSessions.size());
        for (const auto &entry : m_serialSessions)
            result.push_back(entry.second);
        return result;
    }();
    for (SerialSession *session : serialSessions)
        session->poll();

    const std::vector<LocalShellSession *> localSessions = [&] {
        std::vector<LocalShellSession *> result;
        result.reserve(m_localSessions.size());
        for (const auto &entry : m_localSessions)
            result.push_back(entry.second);
        return result;
    }();
    for (LocalShellSession *session : localSessions)
        session->poll();

    const std::vector<NativeProcess *> processes(
        m_sftpProcesses.begin(), m_sftpProcesses.end());
    for (NativeProcess *process : processes) {
        if (m_sftpProcesses.find(process) == m_sftpProcesses.end())
            continue;
        const auto latency = m_remoteLatencyRequests.find(process);
        if (latency != m_remoteLatencyRequests.end()) {
            m_sftpStandardOutputs[process] += process->takeStandardOutput();
            m_sftpStandardErrors[process] += process->takeStandardError();
            DWORD latencyExitCode = 0;
            if (process->isFinished(latencyExitCode))
                finishRemoteLatencyProbe(process, static_cast<int>(latencyExitCode));
            continue;
        }
        const auto monitor = m_remoteMonitorRequests.find(process);
        if (monitor != m_remoteMonitorRequests.end()) {
            processRemoteMonitorOutput(process);
            m_sftpStandardErrors[process] += process->takeStandardError();
            DWORD monitorExitCode = 0;
            if (process->isFinished(monitorExitCode))
                finishRemoteMonitor(process, static_cast<int>(monitorExitCode));
            continue;
        }
        const auto remoteEditDownload = m_remoteEditDownloads.find(process);
        if (remoteEditDownload != m_remoteEditDownloads.end()) {
            processRemoteEditDownloadProgress(process, remoteEditDownload->second);
            m_sftpStandardErrors[process] += process->takeStandardError();
            DWORD editExitCode = 0;
            if (process->isFinished(editExitCode))
                finishRemoteEditDownload(process, static_cast<int>(editExitCode));
            continue;
        }
        const auto remoteEditUpload = m_remoteEditUploads.find(process);
        if (remoteEditUpload != m_remoteEditUploads.end()) {
            m_sftpStandardErrors[process] += process->takeStandardError();
            DWORD editExitCode = 0;
            if (process->isFinished(editExitCode))
                finishRemoteEditUpload(process, static_cast<int>(editExitCode));
            continue;
        }
        if (m_sftpTransferIds.find(process) != m_sftpTransferIds.end()) {
            processSftpTransferOutput(process);
        } else {
            m_sftpStandardOutputs[process] += process->takeStandardOutput();
        }
        m_sftpStandardErrors[process] += process->takeStandardError();

        // A transfer worker can get stuck in libssh2 shutdown after it has
        // confirmed every byte was written remotely.  The confirmation is the
        // meaningful completion boundary; release the worker promptly so the
        // UI can refresh the current directory.
        if (m_sftpTransferRemoteComplete.erase(process) != 0) {
            finishSftpTransfer(process, 0);
            continue;
        }

        DWORD exitCode = 0;
        const bool finished = process->isFinished(exitCode);
        const auto list = m_sftpListRequests.find(process);
        // The list worker flushes its complete result before it tears down the
        // libssh2 channel.  Some servers can block during that teardown.  Do
        // not leave a valid directory response pending just because cleanup is
        // slow or never returns.
        if (list != m_sftpListRequests.end()
            && m_sftpStandardOutputs[process].find("R\t") != std::string::npos) {
            const SftpListRequest request = list->second;
            finishSftpList(process, request.requestId, request.profileIndex,
                           request.path, 0);
            continue;
        }
        const auto history = m_sftpHistoryRequests.find(process);
        if (history != m_sftpHistoryRequests.end() && !finished) {
            const SftpHistoryRequest request = history->second;
            if (std::chrono::steady_clock::now() - request.startedAt
                > std::chrono::seconds(4)) {
                // The worker can block while closing its SSH channel even after
                // its history output has reached us. Return that complete or
                // partial output instead of silently discarding it.
                finishSftpHistory(process, request.requestId, request.profileIndex, 0);
            }
            continue;
        }
        const auto preview = m_sftpPreviewRequests.find(process);
        if (preview != m_sftpPreviewRequests.end()) {
            m_sftpStandardOutputs[process] += process->takeStandardOutput();
            m_sftpStandardErrors[process] += process->takeStandardError();
            DWORD previewExitCode = 0;
            if (process->isFinished(previewExitCode))
                finishSftpPreview(process, static_cast<int>(previewExitCode));
            continue;
        }
        if (!finished)
            continue;
        if (m_sftpTransferIds.find(process) != m_sftpTransferIds.end()) {
            finishSftpTransfer(process, static_cast<int>(exitCode));
            continue;
        }
        if (list != m_sftpListRequests.end()) {
            const SftpListRequest request = list->second;
            finishSftpList(process, request.requestId, request.profileIndex,
                           request.path, static_cast<int>(exitCode));
            continue;
        }
        if (history != m_sftpHistoryRequests.end()) {
            const SftpHistoryRequest request = history->second;
            finishSftpHistory(process, request.requestId, request.profileIndex,
                              static_cast<int>(exitCode));
            continue;
        }
        const auto operation = m_sftpOperationRequests.find(process);
        if (operation != m_sftpOperationRequests.end()) {
            const SftpOperationRequest request = operation->second;
            finishSftpOperation(
                process, request.requestId, request.operation, request.path,
                request.targetPath, static_cast<int>(exitCode));
        }
    }
    pollRemoteEdits();
    pollExitSelfTest();
    } catch (const std::exception &error) {
        // A malformed delayed worker response or an allocation failure must
        // not escape the 10 ms UI timer.  In particular, high-latency SSH
        // sessions can finish several SFTP/latency workers in the same poll.
        DiagnosticLog::write("backend-poll-exception", error.what());
    } catch (...) {
        DiagnosticLog::write("backend-poll-exception", "unknown exception");
    }
}

void WebViewBackend::receiveMessage(const std::string &message)
{
    if (m_shuttingDown.load(std::memory_order_acquire))
        return;
    std::string nativeError;
    NativeJsonDom::Value root;
    if (!NativeJsonDom::parse(message, root, &nativeError) || !root.isObject()) {
        sendError({}, NativeString("Invalid JSON request"));
        return;
    }

    const NativeJsonDom::Object &request = root.object();
    const NativeString requestId =
        nativeText(NativeJsonDom::stringValue(request, "id"));
    const NativeString method =
        nativeText(NativeJsonDom::stringValue(request, "method"));
    DiagnosticLog::setPhase("request:" + method.toStdString());
    const NativeJsonDom::Object emptyParams;
    const NativeJsonDom::Object *nativeParamsObject =
        NativeJsonDom::objectValue(request, "params");
    const NativeJsonDom::Object &nativeParams =
        nativeParamsObject ? *nativeParamsObject : emptyParams;
    if (method == NativeString("app.getInfo")) {
        sendNativeResult(requestId, NativeJsonDom::Value(appInfo()));
    } else if (method == NativeString("app.themeMenu")) {
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        const std::string currentTheme = NativeJsonDom::stringValue(
            nativeParams, "theme", "");
        const std::string currentPreset = NativeJsonDom::stringValue(
            nativeParams, "preset", "");
        if (m_themeMenuHandler)
            m_themeMenuHandler(x, y, currentTheme, currentPreset);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.toolsMenu")) {
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        const bool broadcastActive = NativeJsonDom::booleanValue(
            nativeParams, "broadcastActive", false);
        if (m_toolsMenuHandler)
            m_toolsMenuHandler(x, y, broadcastActive);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.helpMenu")) {
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        if (m_helpMenuHandler)
            m_helpMenuHandler(x, y);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.rdpKeyMenu")) {
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        if (m_rdpKeyMenuHandler)
            m_rdpKeyMenuHandler(x, y);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.serverContextMenu")) {
        const int profileIndex = NativeJsonDom::integerValue(
            nativeParams, "index", -1);
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        const bool canMoveUp = NativeJsonDom::booleanValue(
            nativeParams, "canMoveUp", false);
        const bool canMoveDown = NativeJsonDom::booleanValue(
            nativeParams, "canMoveDown", false);
        if (m_serverContextMenuHandler && profileIndex >= 0)
            m_serverContextMenuHandler(
                profileIndex, x, y, canMoveUp, canMoveDown);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.existingConnectionsMenu")) {
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        std::vector<std::pair<int, std::string>> items;
        const auto found = nativeParams.values.find("items");
        if (found != nativeParams.values.end() && found->second.isArray()) {
            for (const NativeJsonDom::Value &value :
                 found->second.array().values) {
                if (!value.isObject())
                    continue;
                const NativeJsonDom::Object &item = value.object();
                const auto label = item.values.find("label");
                if (label == item.values.end() || !label->second.isString())
                    continue;
                const int index = NativeJsonDom::integerValue(item, "index", -1);
                items.emplace_back(index, label->second.string());
            }
        }
        if (m_existingConnectionsMenuHandler && !items.empty())
            m_existingConnectionsMenuHandler(x, y, items);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.theme")) {
        const std::string theme = NativeJsonDom::stringValue(
            nativeParams, "theme");
        if (m_themeHandler
            && (theme == "dark" || theme == "light" || theme == "blue"))
            m_themeHandler(theme);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.closeBehavior")) {
        NativeJsonDom::Object result;
        result.values.emplace("behavior", closeBehavior());
        sendNativeResult(requestId, NativeJsonDom::Value(result));
    } else if (method == NativeString("app.setCloseBehavior")) {
        const std::string behavior =
            NativeJsonDom::stringValue(nativeParams, "behavior");
        sendNativeResult(
            requestId, NativeJsonDom::Value(setCloseBehavior(behavior)));
    } else if (method == NativeString("app.closeDecision")) {
        const std::string decision =
            NativeJsonDom::stringValue(nativeParams, "decision");
        if (m_closeDecisionHandler
            && (decision == "tray" || decision == "exit"))
            m_closeDecisionHandler(decision);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("app.ready")) {
        // The frontend has rendered its first content frame; the host can now
        // reveal the WebView2 controller in one step.
        if (m_uiReadyHandler)
            m_uiReadyHandler();
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("clipboard.write")) {
        const NativeString text = nativeText(
            NativeJsonDom::stringValue(nativeParams, "text"));
        if (writeSystemClipboard(text))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, NativeString("无法写入 Windows 剪贴板"));
    } else if (method == NativeString("clipboard.read")) {
        sendNativeResult(requestId,
            NativeJsonDom::Value(utf8Text(readSystemClipboard())));
    } else if (method == NativeString("config.export")) {
        NativeString configError;
        const NativeJsonDom::Object result = exportConfig(configError);
        configError.isEmpty()
            ? sendNativeResult(requestId, NativeJsonDom::Value(result))
            : sendError(requestId, configError);
    } else if (method == NativeString("config.import.preview")) {
        NativeString configError;
        const NativeJsonDom::Object result = previewConfigImport(configError);
        configError.isEmpty()
            ? sendNativeResult(requestId, NativeJsonDom::Value(result))
            : sendError(requestId, configError);
    } else if (method == NativeString("config.import.apply")) {
        NativeString configError;
        const NativeJsonDom::Object result = applyConfigImport(nativeParams, configError);
        configError.isEmpty()
            ? sendNativeResult(requestId, NativeJsonDom::Value(result))
            : sendError(requestId, configError);
    } else if (method == NativeString("config.backup.restore")) {
        NativeString configError;
        const NativeJsonDom::Object result = restoreConfigBackup(configError);
        configError.isEmpty()
            ? sendNativeResult(requestId, NativeJsonDom::Value(result))
            : sendError(requestId, configError);
    } else if (method == NativeString("knownHosts.list")) {
        sendNativeResult(requestId, NativeJsonDom::Value(knownHosts()));
    } else if (method == NativeString("local.history.load")) {
        sendNativeResult(requestId,
            NativeJsonDom::Value(localCommandHistory()));
    } else if (method == NativeString("local.history.append")) {
        NativeString historyError;
        if (appendLocalCommandHistory(nativeParams, historyError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, historyError);
    } else if (method == NativeString("local.history.replace")) {
        NativeString historyError;
        if (replaceLocalCommandHistory(nativeParams, historyError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, historyError);
    } else if (method == NativeString("cloud.register")
               || method == NativeString("cloud.login")) {
        startCloudAuthRequest(requestId, nativeParams,
            method == NativeString("cloud.register"));
    } else if (method == NativeString("cloud.push")
               || method == NativeString("cloud.pull")) {
        startCloudSyncRequest(requestId, nativeParams,
            method == NativeString("cloud.push"));
    } else if (method == NativeString("cloud.history")
               || method == NativeString("cloud.restore")
               || method == NativeString("cloud.history.rename")
               || method == NativeString("cloud.history.delete")) {
        const NativeString operation =
            method == NativeString("cloud.history") ? NativeString("list")
            : method == NativeString("cloud.restore") ? NativeString("restore")
            : method == NativeString("cloud.history.rename")
                ? NativeString("rename") : NativeString("delete");
        startCloudHistoryRequest(requestId, nativeParams,
            operation);
    } else if (method == NativeString("cloud.exportServers")) {
        try {
            sendNativeResult(requestId,
                NativeJsonDom::Value(exportCloudServers()));
        } catch (...) {
            sendError(requestId,
                NativeString("导出连接数据失败（请检查私钥文件可读性）"));
        }
    } else if (method == NativeString("cloud.importServers")) {
        NativeString cloudError;
        const NativeJsonDom::Value result =
            importCloudServers(nativeParams, cloudError);
        cloudError.isEmpty()
            ? sendNativeResult(requestId, result)
            : sendError(requestId, cloudError);
    } else if (method == NativeString("knownHosts.remove")) {
        NativeString knownHostsError;
        if (removeKnownHost(nativeParams, knownHostsError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, knownHostsError);
    } else if (method == NativeString("workspace.list")) {
        sendNativeResult(requestId, NativeJsonDom::Value(workspaceNames()));
    } else if (method == NativeString("workspace.create")) {
        std::string name;
        NativeString workspaceError;
        if (createWorkspace(nativeParams, name, workspaceError))
            sendNativeResult(requestId, NativeJsonDom::Value(name));
        else
            sendError(requestId, workspaceError);
    } else if (method == NativeString("workspace.rename")) {
        NativeString workspaceError;
        if (renameWorkspace(nativeParams, workspaceError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, workspaceError);
    } else if (method == NativeString("workspace.delete")) {
        NativeString workspaceError;
        if (deleteWorkspace(nativeParams, workspaceError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, workspaceError);
    } else if (method == NativeString("server.list")) {
        sendNativeResult(requestId, NativeJsonDom::Value(serverProfiles()));
    } else if (method == NativeString("server.hasPassword")) {
        const int index =
            NativeJsonDom::integerValue(nativeParams, "index", -1);
        const std::vector<ServerRecord> records = readServerRecords();
        NativeJsonDom::Object result;
        result.values.emplace("hasPassword", false);
        result.values.emplace("hasKeyPassphrase", false);
        result.values.emplace("hasProxyPassword", false);
        result.values.emplace("passwordLength", 0.0);
        result.values.emplace("keyPassphraseLength", 0.0);
        result.values.emplace("proxyPasswordLength", 0.0);
        if (index >= 0 && index < static_cast<int>(records.size())) {
            const ServerRecord &record =
                records.at(static_cast<std::size_t>(index));
            const NativeString address = nativeText(record.address);
            const NativeString proxyJump = nativeText(record.proxyJump);
            const NativeString storedPasswordValue =
                nativeText(record.password).isEmpty()
                    ? readStoredPassword(address)
                    : nativeText(record.password);
            const NativeString storedKeyPassphraseValue =
                nativeText(record.keyPassphrase).isEmpty()
                    ? readStoredCredential(
                        NativeString("KeyPassphrase"), address)
                    : nativeText(record.keyPassphrase);
            const NativeString storedProxyPasswordValue =
                nativeText(record.proxyPassword).isEmpty() && !proxyJump.isEmpty()
                    ? readStoredCredential(
                        NativeString("ProxyJump"), proxyJump)
                    : nativeText(record.proxyPassword);
            const bool storedPassword = !storedPasswordValue.isEmpty();
            const bool storedKeyPassphrase = !storedKeyPassphraseValue.isEmpty();
            const bool storedProxyPassword = !storedProxyPasswordValue.isEmpty();
            result.values.emplace("hasPassword", storedPassword);
            result.values.emplace("hasKeyPassphrase", storedKeyPassphrase);
            result.values.emplace("hasProxyPassword", storedProxyPassword);
            result.values.emplace(
                "passwordLength", static_cast<double>(storedPasswordValue.size()));
            result.values.emplace(
                "keyPassphraseLength",
                static_cast<double>(storedKeyPassphraseValue.size()));
            result.values.emplace(
                "proxyPasswordLength",
                static_cast<double>(storedProxyPasswordValue.size()));
        }
        sendNativeResult(requestId, NativeJsonDom::Value(result));
    } else if (method == NativeString("server.create")) {
        NativeString serverError;
        const NativeJsonDom::Value profile =
            createServerProfile(nativeParams, serverError);
        serverError.isEmpty()
            ? sendNativeResult(requestId, profile)
            : sendError(requestId, serverError);
    } else if (method == NativeString("server.update")) {
        NativeString serverError;
        const NativeJsonDom::Value profile =
            updateServerProfile(nativeParams, serverError);
        serverError.isEmpty()
            ? sendNativeResult(requestId, profile)
            : sendError(requestId, serverError);
    } else if (method == NativeString("server.copy")) {
        NativeString serverError;
        const NativeJsonDom::Value profile =
            copyServerProfile(nativeParams, serverError);
        serverError.isEmpty()
            ? sendNativeResult(requestId, profile)
            : sendError(requestId, serverError);
    } else if (method == NativeString("server.reorder")) {
        NativeString serverError;
        if (reorderServerProfile(nativeParams, serverError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, serverError);
    } else if (method == NativeString("server.delete")) {
        NativeString serverError;
        if (deleteServerProfile(nativeParams, serverError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, serverError);
    } else if (method == NativeString("session.connect")) {
        const int index =
            NativeJsonDom::integerValue(nativeParams, "index", -1);
        const NativeString suppliedPassword = nativeText(
            NativeJsonDom::stringValue(nativeParams, "password"));
        const std::vector<ServerRecord> records = readServerRecords();
        if (index < 0 || index >= static_cast<int>(records.size())) {
            sendError(requestId, NativeString("服务器配置不存在"));
            return;
        }
        const ServerRecord &record =
            records.at(static_cast<std::size_t>(index));
        const NativeString type = nativeText(record.connectionType);
        if (type == NativeString("serial")) {
            const NativeString name = nativeText(record.name);
            const NativeString portName = nativeText(record.address);
            const int baudRate = nativeMax(1, nativeText(record.port).toInt());
            const int dataBits = nativeBound(5, record.serialDataBits, 8);
            const NativeString parity = nativeText(record.serialParity);
            const NativeString stopBits = nativeText(record.serialStopBits);
            const NativeString flowControl = nativeText(record.serialFlowControl);

            auto *serial = new SerialSession();
            const NativeString sessionId = NativeString("serial-%1").arg(m_nextSessionNumber++);
            m_serialSessions.emplace(sessionId, serial);
            m_sessionProfiles.emplace(sessionId, index);
            serial->setStartedHandler([this, sessionId, serial, index, name, portName] {
                if (serialSession(sessionId) != serial)
                    return;
                DiagnosticLog::setSession(utf8Text(sessionId));
                DiagnosticLog::write("serial-connected", utf8Text(sessionId));
                NativeJsonDom::Object payload;
                payload.values.emplace("state", "connected");
                payload.values.emplace(
                    "profileIndex", static_cast<double>(index));
                payload.values.emplace("name", utf8Text(name));
                payload.values.emplace("connectionType", "serial");
                payload.values.emplace("address", utf8Text(portName));
                sendNativeEvent(
                    "session.state", utf8Text(sessionId), payload);
                openSessionLog(sessionId, utf8Text(name));
            });
            serial->setErrorHandler([this, sessionId, serial](const std::string &message) {
                if (serialSession(sessionId) != serial)
                    return;
                DiagnosticLog::write(
                    "serial-error", utf8Text(sessionId) + " "
                        + DiagnosticLog::sanitize(message));
                NativeJsonDom::Object payload;
                payload.values.emplace("state", "error");
                payload.values.emplace("connectionType", "serial");
                payload.values.emplace("message", message);
                sendNativeEvent(
                    "session.state", utf8Text(sessionId), payload);
            });
            serial->setDataHandler([this, sessionId, serial](const std::string &output) {
                if (serialSession(sessionId) != serial)
                    return;
                if (output.empty())
                    return;
                emitSessionOutput(sessionId, output);
            });
            NativeJsonDom::Object connectingPayload;
            connectingPayload.values.emplace("state", "connecting");
            connectingPayload.values.emplace(
                "profileIndex", static_cast<double>(index));
            connectingPayload.values.emplace("name", utf8Text(name));
            connectingPayload.values.emplace("connectionType", "serial");
            connectingPayload.values.emplace(
                "address", utf8Text(portName));
            connectingPayload.values.emplace(
                "baudRate", static_cast<double>(baudRate));
            sendNativeEvent(
                "session.state", utf8Text(sessionId), connectingPayload);
            DiagnosticLog::setSession(utf8Text(sessionId));
            DiagnosticLog::setPhase("serial-connect");
            DiagnosticLog::write("serial-open", utf8Text(sessionId));
            serial->start(
                portName.toStdWString(), baudRate, dataBits,
                parity.toStdString(), stopBits.toStdString(),
                flowControl.toStdString());
            NativeJsonDom::Object result;
            result.values.emplace("sessionId", utf8Text(sessionId));
            sendNativeResult(requestId, NativeJsonDom::Value(result));
            return;
        }
        if (type != NativeString("ssh")) {
            sendError(requestId, NativeString("不支持的连接类型：") + type);
            return;
        }
        const NativeString name = nativeText(record.name);
        const NativeString address = nativeText(record.address);
        const NativeString port = nativeText(record.port);
        const NativeString configuredPassword = nativeText(record.password);
        const NativeString password = suppliedPassword.isEmpty()
            ? (configuredPassword.isEmpty() ? readStoredPassword(address) : configuredPassword)
            : suppliedPassword;
        const NativeString suppliedKeyPath = nativeText(
            NativeJsonDom::stringValue(
                nativeParams, "keyPath")).trimmed();
        const NativeString keyPath = suppliedKeyPath.isEmpty()
            ? nativeText(record.keyPath) : suppliedKeyPath;
        const NativeString keyPassphrase = nativeText(record.keyPassphrase).isEmpty()
            ? readStoredCredential(NativeString("KeyPassphrase"), address)
            : nativeText(record.keyPassphrase);
        const NativeString proxyJump = nativeText(record.proxyJump);
        const NativeString configuredProxyPassword = nativeText(record.proxyPassword);
        const NativeString proxyPassword = configuredProxyPassword.isEmpty()
            ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
            : configuredProxyPassword;
        const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);

        // A key path synced from another machine may not exist locally
        // (different user profile / drive).  Ask the user to pick the local
        // key instead of failing with a cryptic auth error.
        std::error_code keyPathError;
        const bool keyPathExists = keyPath.isEmpty()
            || std::filesystem::exists(nativePath(keyPath), keyPathError);
        if (!keyPath.isEmpty() && (!keyPathExists || keyPathError)) {
            NativeJsonDom::Object keyPayload;
            keyPayload.values.emplace(
                "index", static_cast<double>(index));
            keyPayload.values.emplace("name", utf8Text(name));
            keyPayload.values.emplace("address", utf8Text(address));
            keyPayload.values.emplace("keyPath", utf8Text(keyPath));
            sendNativeEvent("auth.keyPathMissing", "", keyPayload);
            NativeJsonDom::Object result;
            result.values.emplace("pending", true);
            sendNativeResult(requestId, NativeJsonDom::Value(result));
            return;
        }

        if (password.isEmpty() && keyPath.isEmpty()) {
            NativeJsonDom::Object authPayload;
            authPayload.values.emplace(
                "index", static_cast<double>(index));
            authPayload.values.emplace("name", utf8Text(name));
            authPayload.values.emplace("address", utf8Text(address));
            sendNativeEvent("auth.required", "", authPayload);
            NativeJsonDom::Object result;
            result.values.emplace("pending", true);
            sendNativeResult(requestId, NativeJsonDom::Value(result));
            return;
        }

        auto *sshSession = new SshSession();
        const NativeString sessionId = NativeString("ssh-%1").arg(m_nextSessionNumber++);
        m_sshSessions.emplace(sessionId, sshSession);
        m_sessionProfiles.emplace(sessionId, index);
        sshSession->setStartedHandler([this, sessionId, sshSession, index, name, address, suppliedPassword, suppliedKeyPath, keyPath, record] {
            if (this->sshSession(sessionId) != sshSession)
                return;
            DiagnosticLog::setSession(utf8Text(sessionId));
            DiagnosticLog::write("ssh-connected", utf8Text(sessionId));
            if (!suppliedPassword.isEmpty()) {
                saveStoredPassword(address, suppliedPassword);
                if (!keyPath.isEmpty())
                    saveStoredCredential(
                        NativeString("KeyPassphrase"), address, suppliedPassword);
            }
            // Persist a key path the user picked in the "missing key" dialog
            // so the connection works on this machine from now on.
            if (!suppliedKeyPath.isEmpty()
                && suppliedKeyPath != nativeText(record.keyPath)) {
                std::vector<ServerRecord> updatedRecords =
                    readServerRecords();
                if (index >= 0
                    && index < static_cast<int>(updatedRecords.size())) {
                    updatedRecords[static_cast<std::size_t>(index)].keyPath =
                        utf8Text(suppliedKeyPath);
                    writeServerRecords(updatedRecords);
                }
            }
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "connected");
            payload.values.emplace(
                "profileIndex", static_cast<double>(index));
            payload.values.emplace("name", utf8Text(name));
            sendNativeEvent(
                "session.state", utf8Text(sessionId), payload);
            startRemoteMonitor(sessionId, index);
            startConfiguredTunnels(index, utf8Text(sessionId));
            openSessionLog(sessionId, utf8Text(name));
        });
        sshSession->setErrorHandler([this, sessionId, sshSession] {
            if (this->sshSession(sessionId) != sshSession)
                return;
            DiagnosticLog::setSession(utf8Text(sessionId));
            DiagnosticLog::write(
                "ssh-error", utf8Text(sessionId) + " "
                    + DiagnosticLog::sanitize(sshSession->errorString()));
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "error");
            payload.values.emplace(
                "message", sshSession->errorString());
            sendNativeEvent(
                "session.state", utf8Text(sessionId), payload);
        });
        sshSession->setFinishedHandler([this, sessionId, sshSession](int) {
            if (this->sshSession(sessionId) != sshSession)
                return;
            DiagnosticLog::write("ssh-closed", utf8Text(sessionId));
            closeSessionLog(sessionId);
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "closed");
            sendNativeEvent(
                "session.state", utf8Text(sessionId), payload);
        });
        sshSession->setHostKeyMismatchHandler([this, sessionId, sshSession, name] {
            if (this->sshSession(sessionId) != sshSession)
                return;
            DiagnosticLog::write("ssh-hostkey-mismatch", utf8Text(sessionId));
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "mismatch");
            payload.values.emplace("name", utf8Text(name));
            payload.values.emplace(
                "address", sshSession->hostKeyMismatchHost());
            payload.values.emplace(
                "oldFingerprint", sshSession->hostKeyOldFingerprint());
            payload.values.emplace(
                "newFingerprint", sshSession->hostKeyNewFingerprint());
            sendNativeEvent("hostkey.mismatch", utf8Text(sessionId), payload);
        });
        sshSession->setDataHandler([this, sessionId, sshSession](const std::string &output) {
            if (this->sshSession(sessionId) != sshSession)
                return;
            if (output.empty())
                return;
            emitSessionOutput(sessionId, output);
        });
        NativeJsonDom::Object connectingPayload;
        connectingPayload.values.emplace("state", "connecting");
        connectingPayload.values.emplace(
            "profileIndex", static_cast<double>(index));
        connectingPayload.values.emplace("name", utf8Text(name));
        connectingPayload.values.emplace("connectionType", "ssh");
        sendNativeEvent(
            "session.state", utf8Text(sessionId), connectingPayload);
        const int keepaliveInterval = nativeBound(
            0, jsonInteger(nativeParams, "keepalive", 30), 3600);
        sshSession->setKeepaliveInterval(keepaliveInterval);
        DiagnosticLog::setSession(utf8Text(sessionId));
        DiagnosticLog::setPhase("ssh-connect");
        DiagnosticLog::write("ssh-open", utf8Text(sessionId));
        sshSession->start(
            address.toUtf8(), port.toUtf8(), password.toUtf8(),
            keyPath.toUtf8(), proxyJump.toUtf8(), proxyPassword.toUtf8(),
            proxyKeyPath.toUtf8(), NativeString().toUtf8(),
            keyPassphrase.toUtf8());
        NativeJsonDom::Object result;
        result.values.emplace("sessionId", utf8Text(sessionId));
        sendNativeResult(requestId, NativeJsonDom::Value(result));
    } else if (method == NativeString("session.input")) {
        const NativeString sessionId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId"));
        SshSession *ssh = sshSession(sessionId);
        SerialSession *serial = serialSession(sessionId);
        LocalShellSession *local = localSession(sessionId);
        if (!ssh && !serial && !local) {
            sendError(requestId, NativeString("会话不存在"));
            return;
        }
        const std::string input =
            NativeJsonDom::stringValue(nativeParams, "data");
        if (ssh)
            ssh->write(input.data(), input.size());
        else if (serial)
            serial->write(input.data(), input.size());
        else
            local->write(input.data(), input.size());
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.resize")) {
        const NativeString sessionId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId"));
        SshSession *ssh = sshSession(sessionId);
        LocalShellSession *local = localSession(sessionId);
        if (!ssh && !serialSession(sessionId) && !local) {
            sendError(requestId, NativeString("会话不存在"));
            return;
        }
        if (ssh) {
            const int columns = std::clamp(
                NativeJsonDom::integerValue(
                    nativeParams, "columns", 80),
                20, 400);
            const int rows = std::clamp(
                NativeJsonDom::integerValue(nativeParams, "rows", 24),
                4, 200);
            ssh->resize(columns, rows);
        } else if (local) {
            const int columns = std::clamp(
                NativeJsonDom::integerValue(
                    nativeParams, "columns", 80),
                20, 400);
            const int rows = std::clamp(
                NativeJsonDom::integerValue(nativeParams, "rows", 24),
                4, 200);
            local->resize(columns, rows);
        }
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.disconnect")) {
        const NativeString sessionId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId"));
        if (!removeSession(sessionId)) {
            sendError(requestId, NativeString("SSH 会话不存在"));
            return;
        }
        NativeJsonDom::Object payload;
        payload.values.emplace("state", "closed");
        payload.values.emplace("requested", true);
        sendNativeEvent(
            "session.state", utf8Text(sessionId), payload);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.close")) {
        const NativeString sessionId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId"));
        // Closing an already disconnected/removed session is intentionally
        // idempotent so the frontend can always dispose its tab.
        removeSession(sessionId);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.rdpOpen")) {
        const int index =
            NativeJsonDom::integerValue(nativeParams, "index", -1);
        const std::vector<ServerRecord> records = readServerRecords();
        if (index < 0 || index >= static_cast<int>(records.size())) {
            sendError(requestId, NativeString("服务器配置不存在"));
            return;
        }
        const ServerRecord &record =
            records.at(static_cast<std::size_t>(index));
        if (record.connectionType != "rdp") {
            sendError(requestId, NativeString("该连接不是远程桌面类型"));
            return;
        }
        const NativeString address = nativeText(record.address);
        int port = nativeText(record.port).toInt();
        if (port <= 0 || port > 65535)
            port = 3389;
        const NativeString user = address.indexOf('@') >= 0
            ? address.left(address.indexOf('@'))
            : NativeString();
        const NativeString host = address.indexOf('@') >= 0
            ? address.mid(address.indexOf('@') + 1)
            : address;
        const NativeString password = nativeText(record.password).isEmpty()
            ? readStoredPassword(address)
            : nativeText(record.password);
        if (m_rdpOpenHandler) {
            const std::string rdpSessionId = utf8Text(nativeText(
                NativeJsonDom::stringValue(nativeParams, "sessionId")));
            const std::string sessionId = rdpSessionId.empty()
                ? "rdp-" + std::to_string(index) : rdpSessionId;
            m_rdpOpenHandler(sessionId, index, utf8Text(host), port,
                             utf8Text(user), utf8Text(password),
                             record.rdpOptions);
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        } else {
            sendError(requestId, NativeString("当前窗口不支持 RDP 嵌入"));
        }
    } else if (method == NativeString("session.rdpClose")) {
        const std::string sessionId = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId")));
        if (m_rdpCloseHandler)
            m_rdpCloseHandler(sessionId);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.rdpEditorTransition")) {
        const std::string sessionId = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId")));
        const bool expanded = NativeJsonDom::booleanValue(
            nativeParams, "expanded", false);
        if (!m_rdpAdvancedEditorTransitionHandler) {
            sendError(requestId, NativeString("当前窗口不支持 RDP 编辑器同步切换"));
            return;
        }
        // Unlike ordinary layout notifications, acknowledge this request only
        // after the already-rendered WebView2 target geometry has been applied
        // to the native RDP region. The frontend holds a compositor snapshot
        // until this completion arrives.
        m_rdpAdvancedEditorTransitionHandler(
            sessionId, expanded,
            [this, requestId](bool applied) {
                if (applied)
                    sendNativeResult(requestId, NativeJsonDom::Value(true));
                else
                    sendError(requestId, NativeString(
                        "无法同步 RDP 编辑器与远程桌面遮罩"));
            });
    } else if (method == NativeString("session.rdpLayout")) {
        const std::string sessionId = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId")));
        const bool visible = NativeJsonDom::booleanValue(
            nativeParams, "visible", true);
        const bool refresh = NativeJsonDom::booleanValue(
            nativeParams, "refresh", true);
        const bool clearOcclusion = NativeJsonDom::booleanValue(
            nativeParams, "clearOcclusion", false);
        const bool reflowAll = NativeJsonDom::booleanValue(
            nativeParams, "reflowAll", false);
        if (m_rdpLayoutHandler)
            m_rdpLayoutHandler(
                sessionId, visible, refresh, clearOcclusion, reflowAll);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.rdpFullscreen")) {
        const std::string sessionId = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId")));
        const bool enabled = NativeJsonDom::booleanValue(
            nativeParams, "enabled", false);
        if (m_rdpFullscreenHandler)
            m_rdpFullscreenHandler(sessionId, enabled);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.rdpFullscreenAction")
               || method == NativeString("session.rdpAction")) {
        const std::string sessionId = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId")));
        const std::string action = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "action")));
        if (m_rdpFullscreenActionHandler)
            m_rdpFullscreenActionHandler(sessionId, action);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.rdpContextMenu")) {
        const std::string sessionId = utf8Text(nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId")));
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        if (m_rdpContextMenuHandler)
            m_rdpContextMenuHandler(sessionId, x, y);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.rdpTabsContextMenu")) {
        const int x = NativeJsonDom::integerValue(nativeParams, "x", 0);
        const int y = NativeJsonDom::integerValue(nativeParams, "y", 0);
        const bool canCloseSplit = NativeJsonDom::booleanValue(
            nativeParams, "canCloseSplit", false);
        if (m_rdpTabsContextMenuHandler)
            m_rdpTabsContextMenuHandler(x, y, canCloseSplit);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("session.hostKeyConfirm")) {
        const NativeString sessionId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "sessionId"));
        SshSession *session = sshSession(sessionId);
        if (!session) {
            sendError(requestId, NativeString("SSH 会话不存在"));
            return;
        }
        const bool accept = NativeJsonDom::booleanValue(
            nativeParams, "accept", false);
        if (accept) {
            if (session->confirmHostKey())
                sendNativeResult(requestId, NativeJsonDom::Value(true));
            else
                sendError(requestId, NativeString("无法保存新的主机密钥，连接已保持暂停"));
        } else {
            session->abortHostKey();
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        }
    } else if (method == NativeString("tunnel.create")) {
        NativeString tunnelError;
        const NativeJsonDom::Value result = createTunnel(nativeParams, tunnelError);
        tunnelError.isEmpty()
            ? sendNativeResult(requestId, result)
            : sendError(requestId, tunnelError);
    } else if (method == NativeString("tunnel.list")) {
        NativeString tunnelError;
        const NativeJsonDom::Value result = listTunnels(nativeParams, tunnelError);
        tunnelError.isEmpty()
            ? sendNativeResult(requestId, result)
            : sendError(requestId, tunnelError);
    } else if (method == NativeString("tunnel.stop")) {
        NativeString tunnelError;
        if (stopTunnel(nativeParams, tunnelError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, tunnelError);
    } else if (method == NativeString("tunnel.saveConfig")) {
        NativeString tunnelError;
        if (saveTunnelConfig(nativeParams, tunnelError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, tunnelError);
    } else if (method == NativeString("tunnel.removeConfig")) {
        NativeString tunnelError;
        if (removeTunnelConfig(nativeParams, tunnelError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, tunnelError);
    } else if (method == NativeString("app.notify")) {
        const std::string title =
            jsonText(nativeParams, "title", "MasterTerm");
        const std::string message = jsonText(nativeParams, "message");
        if (m_notifyHandler && !message.empty())
            m_notifyHandler(title, message);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("logs.configure")) {
        m_sessionLoggingEnabled =
            NativeJsonDom::booleanValue(nativeParams, "enabled", false);
        if (!m_sessionLoggingEnabled) {
            for (auto &entry : m_sessionLogs) {
                entry.second.flush();
                entry.second.close();
            }
            m_sessionLogs.clear();
        }
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("logs.openDirectory")) {
        const std::filesystem::path directory = sessionLogDirectory();
        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);
        if (!directoryError)
            ShellExecuteW(
                nullptr, L"open", directory.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else if (method == NativeString("update.check")) {
        startSelfUpdateCheck(requestId, nativeParams);
    } else if (method == NativeString("update.download")) {
        startUpdateDownload(requestId, nativeParams);
    } else if (method == NativeString("update.install")) {
        NativeString updateError;
        if (installUpdate(nativeText(NativeJsonDom::stringValue(
                              nativeParams, "path")), updateError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, updateError);
    } else if (method == NativeString("dialog.openFile")) {
        const NativeString initial = nativeText(NativeJsonDom::stringValue(
            nativeParams, "initial"));
        const NativeString selected = chooseWindowsPath(
            NativeString("选择文件"), initial, false, false);
        NativeJsonDom::Object result;
        result.values.emplace("cancelled", selected.isEmpty());
        result.values.emplace("path", utf8Text(selected));
        sendNativeResult(requestId, NativeJsonDom::Value(result));
    } else if (method == NativeString("local.detectShells")) {
        auto checkPath = [](const wchar_t *exe) -> bool {
            return SearchPathW(nullptr, exe, L".exe", 0, nullptr, nullptr) > 0;
        };
        auto fileExists = [](const wchar_t *path) -> bool {
            const DWORD attr = GetFileAttributesW(path);
            return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
        };
        NativeJsonDom::Array shells;
        // CMD
        {
            NativeJsonDom::Object s;
            s.values.emplace("id", "cmd");
            s.values.emplace("name", "命令提示符 (CMD)");
            s.values.emplace("available", true);
            s.values.emplace("exists", true);
            s.values.emplace("path", "cmd.exe");
            shells.values.emplace_back(s);
        }
        // Windows PowerShell
        {
            const bool hasPs = checkPath(L"powershell") || fileExists(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
            NativeJsonDom::Object s;
            s.values.emplace("id", "powershell");
            s.values.emplace("name", "Windows PowerShell");
            s.values.emplace("available", hasPs);
            s.values.emplace("exists", hasPs);
            s.values.emplace("path", "powershell.exe");
            shells.values.emplace_back(s);
        }
        // PowerShell 7 (pwsh)
        {
            const bool hasPwsh = checkPath(L"pwsh") || fileExists(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe");
            NativeJsonDom::Object s;
            s.values.emplace("id", "pwsh");
            s.values.emplace("name", "PowerShell 7");
            s.values.emplace("available", hasPwsh);
            s.values.emplace("exists", hasPwsh);
            s.values.emplace("path", "pwsh.exe");
            shells.values.emplace_back(s);
        }
        // WSL
        {
            const bool hasWsl = checkPath(L"wsl") || fileExists(L"C:\\Windows\\System32\\wsl.exe");
            NativeJsonDom::Object s;
            s.values.emplace("id", "wsl");
            s.values.emplace("name", "WSL (Linux)");
            s.values.emplace("available", hasWsl);
            s.values.emplace("exists", hasWsl);
            s.values.emplace("path", "wsl.exe");
            shells.values.emplace_back(s);
        }
        // Git Bash
        {
            const bool hasGit = checkPath(L"bash")
                || fileExists(L"C:\\Program Files\\Git\\bin\\bash.exe")
                || fileExists(L"C:\\Program Files\\Git\\usr\\bin\\bash.exe");
            NativeJsonDom::Object s;
            s.values.emplace("id", "gitbash");
            s.values.emplace("name", "Git Bash");
            s.values.emplace("available", hasGit);
            s.values.emplace("exists", hasGit);
            s.values.emplace("path", fileExists(L"C:\\Program Files\\Git\\bin\\bash.exe")
                ? "C:\\Program Files\\Git\\bin\\bash.exe" : "bash.exe");
            shells.values.emplace_back(s);
        }
        sendNativeResult(requestId, NativeJsonDom::Value(shells));
    } else if (method == NativeString("local.connect")) {
        auto checkPath = [](const wchar_t *exe) -> bool {
            return SearchPathW(nullptr, exe, L".exe", 0, nullptr, nullptr) > 0;
        };
        auto fileExists = [](const wchar_t *path) -> bool {
            const DWORD attr = GetFileAttributesW(path);
            return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
        };
        const std::string shellType = NativeJsonDom::stringValue(nativeParams, "shellType");
        const NativeString shellPath = nativeText(NativeJsonDom::stringValue(nativeParams, "shellPath"));
        const NativeString shellArgs = nativeText(NativeJsonDom::stringValue(nativeParams, "shellArgs"));
        const NativeString workingDir = nativeText(NativeJsonDom::stringValue(nativeParams, "workingDir"));
        std::string terminalName = NativeJsonDom::stringValue(nativeParams, "name");

        std::wstring exe = shellPath.toStdWString();
        std::wstring args = shellArgs.toStdWString();
        if (exe.empty()) {
            if (shellType == "powershell") {
                exe = L"powershell.exe";
                if (terminalName.empty()) terminalName = "PowerShell";
            } else if (shellType == "pwsh") {
                if (checkPath(L"pwsh")) {
                    exe = L"pwsh.exe";
                } else if (fileExists(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe")) {
                    exe = L"C:\\Program Files\\PowerShell\\7\\pwsh.exe";
                } else {
                    sendError(requestId, NativeString("本地未安装 PowerShell 7 (pwsh.exe)，请先安装或选择 Windows PowerShell。"));
                    return;
                }
                if (terminalName.empty()) terminalName = "PowerShell 7";
            } else if (shellType == "wsl") {
                if (checkPath(L"wsl") || fileExists(L"C:\\Windows\\System32\\wsl.exe")) {
                    exe = L"wsl.exe";
                } else {
                    sendError(requestId, NativeString("本地未检测到 WSL (Linux 子系统)，请先安装并启用 WSL。"));
                    return;
                }
                if (terminalName.empty()) terminalName = "WSL";
            } else if (shellType == "gitbash") {
                if (fileExists(L"C:\\Program Files\\Git\\bin\\bash.exe"))
                    exe = L"C:\\Program Files\\Git\\bin\\bash.exe";
                else if (checkPath(L"bash"))
                    exe = L"bash.exe";
                else {
                    sendError(requestId, NativeString("本地未检测到 Git Bash (bash.exe)，请先安装 Git for Windows。"));
                    return;
                }
                if (terminalName.empty()) terminalName = "Git Bash";
            } else {
                exe = L"cmd.exe";
                if (terminalName.empty()) terminalName = "本地终端";
            }
        }
        if (terminalName.empty())
            terminalName = "本地终端";

        auto *local = new LocalShellSession();
        const NativeString sessionId =
            NativeString("local-%1").arg(m_nextSessionNumber++);
        m_localSessions.emplace(sessionId, local);
        local->setStartedHandler([this, sessionId, local, terminalName] {
            if (localSession(sessionId) != local)
                return;
            DiagnosticLog::setSession(utf8Text(sessionId));
            DiagnosticLog::write("local-started", utf8Text(sessionId));
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "connected");
            payload.values.emplace("connectionType", "local");
            payload.values.emplace("name", terminalName);
            sendNativeEvent(
                "session.state", utf8Text(sessionId), payload);
            openSessionLog(sessionId, terminalName);
        });
        local->setErrorHandler([this, sessionId, local] {
            if (localSession(sessionId) != local)
                return;
            DiagnosticLog::write(
                "local-error", utf8Text(sessionId) + " "
                    + DiagnosticLog::sanitize(local->errorString()));
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "error");
            payload.values.emplace("connectionType", "local");
            payload.values.emplace("message", local->errorString());
            sendNativeEvent(
                "session.state", utf8Text(sessionId), payload);
        });
        local->setFinishedHandler([this, sessionId, local](int) {
            if (localSession(sessionId) != local)
                return;
            DiagnosticLog::write("local-closed", utf8Text(sessionId));
            closeSessionLog(sessionId);
            NativeJsonDom::Object payload;
            payload.values.emplace("state", "closed");
            payload.values.emplace("connectionType", "local");
            sendNativeEvent(
                "session.state", utf8Text(sessionId), payload);
        });
        local->setDataHandler([this, sessionId, local](
            const std::string &output) {
            if (localSession(sessionId) != local || output.empty())
                return;
            emitSessionOutput(sessionId, output);
        });
        NativeJsonDom::Object connectingPayload;
        connectingPayload.values.emplace("state", "connecting");
        connectingPayload.values.emplace("connectionType", "local");
        connectingPayload.values.emplace("name", terminalName);
        sendNativeEvent(
            "session.state", utf8Text(sessionId), connectingPayload);
        DiagnosticLog::setSession(utf8Text(sessionId));
        DiagnosticLog::setPhase("local-connect");
        DiagnosticLog::write("local-open", utf8Text(sessionId));
        local->start(exe, args, workingDir.toStdWString());
        NativeJsonDom::Object result;
        result.values.emplace("sessionId", utf8Text(sessionId));
        sendNativeResult(requestId, NativeJsonDom::Value(result));
    } else if (method == NativeString("serial.list")) {
        NativeJsonDom::Array ports;
        for (const std::wstring &port : SerialSession::availablePorts()) {
            ports.values.emplace_back(
                NativeString::fromStdWString(port).toUtf8());
        }
        sendNativeResult(requestId, NativeJsonDom::Value(ports));
    } else if (method == NativeString("font.list")) {
        sendNativeResult(requestId, NativeJsonDom::Value(installedTerminalFonts()));
    } else if (method == NativeString("sftp.list")) {
        const int profileIndex =
            NativeJsonDom::integerValue(nativeParams, "index", -1);
        NativeString path = nativeText(
            NativeJsonDom::stringValue(nativeParams, "path")).trimmed();
        if (path.isEmpty()) {
            startSftpList(requestId, profileIndex, NativeString("."));
        } else {
            if (!path.startsWith(static_cast<char>('/')))
                path.prepend(static_cast<char>('/'));
            startSftpList(requestId, profileIndex, cleanRemotePath(path));
        }
    } else if (method == NativeString("history.load")) {
        startSftpHistory(requestId,
            NativeJsonDom::integerValue(nativeParams, "index", -1), nativeParams);
    } else if (method == NativeString("sftp.preview")) {
        startSftpPreview(requestId, nativeParams);
    } else if (method == NativeString("sftp.download")
               || method == NativeString("sftp.upload")) {
        startSftpTransfer(requestId, nativeParams);
    } else if (method == NativeString("sftp.open")) {
        openRemoteFile(requestId, nativeParams);
    } else if (method == NativeString("sftp.remoteEdit.retry")) {
        NativeString editError;
        if (retryRemoteEdit(nativeParams, editError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, editError);
    } else if (method == NativeString("sftp.remoteEdit.stop")) {
        NativeString editError;
        if (stopRemoteEdit(nativeParams, editError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, editError);
    } else if (method == NativeString("sftp.remoteEdit.openDirectory")) {
        NativeString editError;
        if (openRemoteEditDirectory(nativeParams, editError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, editError);
    } else if (method == NativeString("sftp.operation")) {
        startSftpOperation(requestId, nativeParams);
    } else if (method == NativeString("sftp.cancel")) {
        const NativeString transferId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "transferId"));
        if (cancelSftpTransfer(transferId))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, NativeString("传输任务不存在"));
    } else if (method == NativeString("sftp.pause")) {
        const NativeString transferId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "transferId"));
        if (setSftpTransferPaused(transferId, true))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, NativeString("传输任务不存在"));
    } else if (method == NativeString("sftp.resume")) {
        const NativeString transferId = nativeText(
            NativeJsonDom::stringValue(nativeParams, "transferId"));
        if (setSftpTransferPaused(transferId, false))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, NativeString("传输任务不存在"));
    } else if (method == NativeString("sftp.stream.chunk")) {
        NativeString streamError;
        std::int64_t bufferedBytes = 0;
        if (writeSftpStreamChunk(nativeParams, bufferedBytes, streamError)) {
            NativeJsonDom::Object result;
            result.values.emplace(
                "buffered", static_cast<double>(bufferedBytes));
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        } else {
            sendError(requestId, streamError);
        }
    } else if (method == NativeString("sftp.stream.status")) {
        NativeString streamError;
        std::int64_t bufferedBytes = 0;
        if (querySftpStream(nativeParams, bufferedBytes, streamError)) {
            NativeJsonDom::Object result;
            result.values.emplace(
                "buffered", static_cast<double>(bufferedBytes));
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        } else {
            sendError(requestId, streamError);
        }
    } else if (method == NativeString("sftp.stream.finish")) {
        NativeString streamError;
        if (finishSftpStream(nativeParams, streamError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, streamError);
    } else if (method == NativeString("local.list")) {
        NativeString localError;
        const NativeJsonDom::Object result = localDirectory(
            nativeText(NativeJsonDom::stringValue(nativeParams, "path")), localError);
        if (localError.isEmpty())
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("local.chooseDirectory")) {
        NativeString localError;
        const NativeJsonDom::Object result =
            chooseLocalDirectory(nativeParams, localError);
        if (localError.isEmpty())
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("local.chooseFile")) {
        sendNativeResult(
            requestId, NativeJsonDom::Value(chooseLocalFile(nativeParams)));
    } else if (method == NativeString("local.chooseProgram")) {
        sendNativeResult(
            requestId, NativeJsonDom::Value(chooseLocalProgram(nativeParams)));
    } else if (method == NativeString("local.operation")) {
        NativeString localError;
        const NativeJsonDom::Object result =
            operateLocalEntry(nativeParams, localError);
        if (localError.isEmpty())
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("local.remove")) {
        NativeString localError;
        if (removeLocalEntries(nativeParams, localError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("shell.openUrl")) {
        const NativeString url = nativeText(
            NativeJsonDom::stringValue(nativeParams, "url")).trimmed();
        if (url.isEmpty() || (!url.startsWith(NativeString("http://"))
                              && !url.startsWith(NativeString("https://")))) {
            sendError(requestId, NativeString("无效的链接"));
            return;
        }
#ifdef _WIN32
        const HINSTANCE launched = ShellExecuteW(
            nullptr, L"open", url.toStdWString().c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        sendNativeResult(requestId, NativeJsonDom::Value(
            reinterpret_cast<INT_PTR>(launched) > 32));
#else
        sendError(requestId, NativeString("当前平台不支持打开外部链接"));
#endif
    } else if (method == NativeString("local.stage.begin")) {
        NativeString localError;
        const NativeJsonDom::Object result =
            beginDroppedFile(nativeParams, localError);
        if (localError.isEmpty())
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("local.stage.chunk")) {
        NativeString localError;
        if (appendDroppedFile(nativeParams, localError))
            sendNativeResult(requestId, NativeJsonDom::Value(true));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("local.stage.finish")) {
        NativeString localError;
        const NativeJsonDom::Object result =
            finishDroppedFile(nativeParams, localError);
        if (localError.isEmpty())
            sendNativeResult(requestId, NativeJsonDom::Value(result));
        else
            sendError(requestId, localError);
    } else if (method == NativeString("local.stage.cancel")) {
        discardDroppedFile(nativeText(
            NativeJsonDom::stringValue(nativeParams, "token")));
        sendNativeResult(requestId, NativeJsonDom::Value(true));
    } else {
        sendError(requestId, NativeString("Unknown method: ") + method);
    }
}

NativeJsonDom::Object WebViewBackend::localDirectory(
    const NativeString &path, NativeString &error) const
{
    NativeString requested = path.trimmed();
    if (requested.isEmpty())
        requested = userHomePath();
    const std::filesystem::path directoryPath = nativePath(requested);
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(directoryPath, filesystemError)) {
        error = NativeString("本机目录不存在：") + requested;
        return {};
    }
    const std::filesystem::path absolutePath = std::filesystem::absolute(directoryPath, filesystemError).lexically_normal();
    if (filesystemError) {
        error = NativeString("无法解析本机目录：") + requested;
        return {};
    }
    NativeJsonDom::Array entries;
    std::vector<std::filesystem::directory_entry> items;
    for (const auto &entry : std::filesystem::directory_iterator(absolutePath, filesystemError))
        items.push_back(entry);
    std::sort(items.begin(), items.end(), [](const auto &left, const auto &right) {
        if (left.is_directory() != right.is_directory()) return left.is_directory();
        return NativeString::fromStdWString(left.path().filename().wstring()).toLower()
            < NativeString::fromStdWString(right.path().filename().wstring()).toLower();
    });
    for (const auto &entry : items) {
        const std::filesystem::path itemPath = entry.path();
        const NativeString itemName = pathText(itemPath.filename());
        const bool isDirectory = entry.is_directory(filesystemError);
        const auto size = isDirectory ? static_cast<std::uintmax_t>(0) : entry.file_size(filesystemError);
        NativeJsonDom::Object item;
        item.values.emplace("name", utf8Text(itemName));
        item.values.emplace("path", utf8Text(pathText(itemPath)));
        item.values.emplace("directory", isDirectory);
        item.values.emplace(
            "size", isDirectory ? -1.0 : static_cast<double>(size));
        item.values.emplace(
            "modified",
            static_cast<double>(
                fileTimeSeconds(entry.last_write_time(filesystemError))));
        entries.values.emplace_back(std::move(item));
    }
    const std::filesystem::path parentPath = absolutePath.parent_path();
    const bool hasParent = parentPath != absolutePath && !parentPath.empty();
    NativeJsonDom::Object result;
    result.values.emplace("path", utf8Text(pathText(absolutePath)));
    result.values.emplace(
        "parent", hasParent ? utf8Text(pathText(parentPath)) : std::string());
    result.values.emplace("entries", std::move(entries));
    return result;
}

bool WebViewBackend::writeSystemClipboard(const NativeString &text) const
{
#ifdef _WIN32
    if (!openSystemClipboard()) return false;
    EmptyClipboard();
    const std::wstring value = text.toStdWString();
    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void *target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(target, value.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
#else
    (void)text;
    return false;
#endif
}

NativeString WebViewBackend::readSystemClipboard() const
{
#ifdef _WIN32
    if (!openSystemClipboard()) return {};
    NativeString result;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const wchar_t *value = static_cast<const wchar_t *>(GlobalLock(handle));
            if (value) {
                result = NativeString::fromStdWString(value);
                GlobalUnlock(handle);
            }
        }
    }
    CloseClipboard();
    return result;
#else
    return {};
#endif
}

NativeJsonDom::Object WebViewBackend::chooseLocalDirectory(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    NativeString initialPath = nativeText(
        NativeJsonDom::stringValue(params, "path")).trimmed();
    if (initialPath.isEmpty() || !std::filesystem::is_directory(nativePath(initialPath)))
        initialPath = downloadsPath();
    const NativeString path = chooseWindowsPath(
        NativeString("选择下载保存目录"), initialPath, true, false);
    if (path.isEmpty()) {
        NativeJsonDom::Object result;
        result.values.emplace("cancelled", true);
        return result;
    }
    NativeJsonDom::Object result = localDirectory(path, error);
    if (error.isEmpty())
        result.values.emplace("cancelled", false);
    return result;
}

NativeJsonDom::Object WebViewBackend::chooseLocalFile(
    const NativeJsonDom::Object &params) const
{
    NativeString initialPath = nativeText(
        NativeJsonDom::stringValue(params, "path")).trimmed();
    if (initialPath.isEmpty() || !std::filesystem::is_directory(nativePath(initialPath)))
        initialPath = userHomePath();
    const NativeString path = chooseWindowsPath(
        NativeString("选择要上传的文件"), initialPath, false, false);
    NativeJsonDom::Object result;
    result.values.emplace("cancelled", path.isEmpty());
    if (!path.isEmpty())
        result.values.emplace(
            "path", utf8Text(pathText(nativePath(path))));
    return result;
}

NativeJsonDom::Object WebViewBackend::chooseLocalProgram(
    const NativeJsonDom::Object &params) const
{
    NativeString initialPath = nativeText(
        NativeJsonDom::stringValue(params, "path")).trimmed();
    if (initialPath.isEmpty() || !std::filesystem::is_directory(nativePath(initialPath)))
        initialPath = userHomePath();
    const NativeString path = chooseWindowsPath(
        NativeString("选择默认编辑程序"), initialPath, false, false);
    NativeJsonDom::Object result;
    result.values.emplace("cancelled", path.isEmpty());
    if (!path.isEmpty())
        result.values.emplace("path", utf8Text(pathText(nativePath(path))));
    return result;
}

NativeJsonDom::Object sanitizedConfig(const NativeJsonDom::Object &source)
{
    NativeJsonDom::Object result = source;
    NativeJsonDom::Array servers;
    const auto found = source.values.find("servers");
    if (found != source.values.end() && found->second.isArray()) {
        for (const NativeJsonDom::Value &value : found->second.array().values) {
            if (!value.isObject()) continue;
            ServerRecord record = serverRecordFromJson(value.object());
            record.password.clear();
            record.proxyPassword.clear();
            servers.values.emplace_back(serverRecordJson(record));
        }
    }
    result.values["servers"] = NativeJsonDom::Value(std::move(servers));
    return result;
}

NativeString configBackupPath(const std::filesystem::path &configPath)
{
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
#ifdef _WIN32
    const auto processId = GetCurrentProcessId();
#else
    const auto processId = 0;
#endif
    return pathText(configPath.parent_path() /
        (L"MasterSSH.backup-" + std::to_wstring(processId) + L"-"
         + std::to_wstring(millis) + L".json"));
}

bool isConfigBackupPath(const std::filesystem::path &path,
                        const std::filesystem::path &configPath)
{
    const std::filesystem::path normalized = path.lexically_normal();
    const std::filesystem::path parent = configPath.parent_path().lexically_normal();
    const std::wstring name = normalized.filename().wstring();
    return normalized.parent_path() == parent
        && name.rfind(L"MasterSSH.backup-", 0) == 0
        && normalized.extension() == L".json";
}

NativeJsonDom::Object WebViewBackend::exportConfig(NativeString &error) const
{
    const NativeString selected = chooseWindowsPath(
        NativeString("导出 MasterTerm 配置"), downloadsPath()
            + NativeString("/MasterTerm-config.json"), false, true);
    NativeJsonDom::Object result;
    if (selected.isEmpty()) {
        result.values.emplace("cancelled", true);
        return result;
    }
    const NativeJsonDom::Object config = sanitizedConfig(readNativeConfig());
    if (!NativeConfigFile::writeObject(nativePath(selected), config)) {
        error = NativeString("无法写入配置导出文件");
        return {};
    }
    result.values.emplace("cancelled", false);
    result.values.emplace("path", utf8Text(selected));
    return result;
}

NativeJsonDom::Object WebViewBackend::previewConfigImport(NativeString &error)
{
    const NativeString selected = chooseWindowsPath(
        NativeString("导入 MasterTerm 配置"), downloadsPath(), false, false);
    NativeJsonDom::Object result;
    if (selected.isEmpty()) {
        result.values.emplace("cancelled", true);
        return result;
    }
    bool available = false;
    const NativeJsonDom::Object source =
        NativeConfigFile::readObject(nativePath(selected), &available);
    if (!available || source.values.empty()) {
        error = NativeString("配置文件不存在、损坏或不是有效 JSON");
        return {};
    }
    NativeJsonDom::Array importedServers;
    int invalidServers = 0;
    const auto servers = source.values.find("servers");
    if (servers != source.values.end() && servers->second.isArray()) {
        for (const NativeJsonDom::Value &value : servers->second.array().values) {
            if (!value.isObject()) {
                ++invalidServers;
                continue;
            }
            const ServerRecord record = serverRecordFromJson(value.object());
            if (record.address.empty() || record.name.empty()
                || (record.connectionType != "ssh"
                    && record.connectionType != "serial")) {
                ++invalidServers;
                continue;
            }
            importedServers.values.emplace_back(serverRecordJson(record));
        }
    }
    NativeJsonDom::Array importedWorkspaces;
    const auto workspaces = source.values.find("workspaces");
    if (workspaces != source.values.end() && workspaces->second.isArray()) {
        for (const NativeJsonDom::Value &value : workspaces->second.array().values) {
            if (value.isString() && !value.string().empty()
                && value.string() != "未分配")
                importedWorkspaces.values.emplace_back(value.string());
        }
    }
    NativeJsonDom::Object pending;
    pending.values.emplace("servers", NativeJsonDom::Value(importedServers));
    pending.values.emplace("workspaces", NativeJsonDom::Value(importedWorkspaces));
    const NativeString token = droppedFileToken();
    m_pendingConfigImports[token] = pending;
    result.values.emplace("cancelled", false);
    result.values.emplace("token", utf8Text(token));
    result.values.emplace("path", utf8Text(selected));
    result.values.emplace("servers", static_cast<double>(
        pending.values.at("servers").array().values.size()));
    result.values.emplace("invalidServers", static_cast<double>(invalidServers));
    result.values.emplace("workspaces", static_cast<double>(
        pending.values.at("workspaces").array().values.size()));
    return result;
}

NativeJsonDom::Object WebViewBackend::applyConfigImport(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const NativeString token = nativeText(
        NativeJsonDom::stringValue(params, "token")).trimmed();
    const auto pending = m_pendingConfigImports.find(token);
    if (pending == m_pendingConfigImports.end()) {
        error = NativeString("导入预览已失效，请重新选择配置文件");
        return {};
    }
    const NativeJsonDom::Object imported = pending->second;
    m_pendingConfigImports.erase(pending);
    const NativeJsonDom::Object current = readNativeConfig();
    std::vector<ServerRecord> records = readServerRecords();
    const auto serverKey = [](const ServerRecord &record) {
        return record.connectionType + "\x1f" + record.address + "\x1f" + record.port;
    };
    int added = 0;
    int updated = 0;
    const auto importedServers = imported.values.find("servers");
    if (importedServers != imported.values.end() && importedServers->second.isArray()) {
        for (const NativeJsonDom::Value &value : importedServers->second.array().values) {
            if (!value.isObject()) continue;
            const ServerRecord incoming = serverRecordFromJson(value.object());
            const std::string key = serverKey(incoming);
            const auto existing = std::find_if(records.begin(), records.end(),
                [&](const ServerRecord &record) { return serverKey(record) == key; });
            if (existing == records.end()) {
                records.push_back(incoming);
                ++added;
            } else {
                *existing = incoming;
                ++updated;
            }
        }
    }
    std::vector<std::string> workspaces = readWorkspaceNames();
    const auto importedWorkspaces = imported.values.find("workspaces");
    if (importedWorkspaces != imported.values.end() && importedWorkspaces->second.isArray()) {
        for (const NativeJsonDom::Value &value : importedWorkspaces->second.array().values) {
            if (!value.isString() || value.string().empty()) continue;
            if (std::find(workspaces.begin(), workspaces.end(), value.string()) == workspaces.end())
                workspaces.push_back(value.string());
        }
    }
    NativeJsonDom::Object config = current;
    NativeJsonDom::Array serverArray;
    for (const ServerRecord &record : records)
        serverArray.values.emplace_back(serverRecordJson(record));
    config.values["servers"] = NativeJsonDom::Value(std::move(serverArray));
    NativeJsonDom::Array workspaceArray;
    for (const std::string &workspace : workspaces)
        workspaceArray.values.emplace_back(workspace);
    config.values["workspaces"] = NativeJsonDom::Value(std::move(workspaceArray));

    const std::filesystem::path target = nativeConfigPath();
    NativeString backupPath;
    bool available = false;
    readNativeConfig(&available);
    if (available) {
        backupPath = configBackupPath(target);
        if (!NativeConfigFile::writeObject(nativePath(backupPath), sanitizedConfig(current))) {
            error = NativeString("无法创建导入前的配置备份");
            return {};
        }
    }
    if (!writeNativeConfig(config)) {
        error = NativeString("无法写入导入后的配置");
        return {};
    }
    NativeJsonDom::Object result;
    result.values.emplace("added", static_cast<double>(added));
    result.values.emplace("updated", static_cast<double>(updated));
    result.values.emplace("backupPath", utf8Text(backupPath));
    return result;
}

NativeJsonDom::Object WebViewBackend::restoreConfigBackup(NativeString &error) const
{
    const std::filesystem::path configPath = nativeConfigPath();
    const NativeString selected = chooseWindowsPath(
        NativeString("恢复 MasterTerm 配置备份"), pathText(configPath.parent_path()), false, false);
    NativeJsonDom::Object result;
    if (selected.isEmpty()) {
        result.values.emplace("cancelled", true);
        return result;
    }
    const std::filesystem::path backup = nativePath(selected).lexically_normal();
    if (!isConfigBackupPath(backup, configPath)) {
        error = NativeString("只能恢复 MasterSSH 配置目录中的备份文件");
        return {};
    }
    bool available = false;
    const NativeJsonDom::Object backupConfig =
        NativeConfigFile::readObject(backup, &available);
    if (!available || backupConfig.values.empty()) {
        error = NativeString("备份文件无效或已损坏");
        return {};
    }
    if (!NativeConfigFile::writeObject(configPath, sanitizedConfig(backupConfig))) {
        error = NativeString("无法恢复配置备份");
        return {};
    }
    result.values.emplace("cancelled", false);
    result.values.emplace("path", utf8Text(selected));
    return result;
}

NativeJsonDom::Array WebViewBackend::knownHosts() const
{
    NativeJsonDom::Array result;
    std::ifstream input(knownHostsFilePath(), std::ios::binary);
    if (!input)
        return result;
    const std::vector<NativeKnownHosts::Entry> entries =
        NativeKnownHosts::parseText(std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()));
    int index = 0;
    for (const NativeKnownHosts::Entry &entry : entries) {
        NativeJsonDom::Object object;
        object.values.emplace("index", static_cast<double>(index++));
        object.values.emplace("line", entry.line);
        object.values.emplace("host", entry.host);
        if (!entry.keyType.empty()) object.values.emplace("keyType", entry.keyType);
        if (!entry.key.empty()) object.values.emplace("key", entry.key);
        result.values.emplace_back(std::move(object));
    }
    return result;
}

NativeJsonDom::Array WebViewBackend::localCommandHistory() const
{
    NativeJsonDom::Array result;
    std::ifstream input(localCommandHistoryFilePath(), std::ios::binary);
    if (!input)
        return result;

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            result.values.emplace_back(std::move(line));
    }
    return result;
}

bool WebViewBackend::appendLocalCommandHistory(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    std::string command = NativeJsonDom::stringValue(params, "command");
    while (!command.empty()
           && (command.back() == '\r' || command.back() == '\n'))
        command.pop_back();
    if (command.empty())
        return true;

    std::vector<std::string> commands;
    std::ifstream input(localCommandHistoryFilePath(), std::ios::binary);
    std::string line;
    while (input && std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line != command)
            commands.push_back(std::move(line));
    }
    commands.push_back(std::move(command));
    constexpr std::size_t maximumStoredCommands = 2000;
    if (commands.size() > maximumStoredCommands) {
        commands.erase(commands.begin(), commands.end()
            - static_cast<std::ptrdiff_t>(maximumStoredCommands));
    }

    const std::filesystem::path path = localCommandHistoryFilePath();
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = NativeString("无法创建本地历史命令目录：")
            + NativeString::fromStdString(directoryError.message());
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = NativeString("无法写入本地历史命令文件：") + pathText(path);
        return false;
    }
    for (const std::string &entry : commands)
        output << entry << '\n';
    if (!output) {
        error = NativeString("写入本地历史命令文件失败：") + pathText(path);
        return false;
    }
    return true;
}

bool WebViewBackend::removeKnownHost(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    const int target = NativeJsonDom::integerValue(params, "index", -1);
    if (target < 0) {
        error = NativeString("主机指纹索引无效");
        return false;
    }
    const std::filesystem::path path = knownHostsFilePath();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = NativeString("已知主机文件不存在");
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    int index = 0;
    bool removed = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = [&line] {
            const std::size_t begin = line.find_first_not_of(" \t");
            if (begin == std::string::npos) return std::string{};
            const std::size_t end = line.find_last_not_of(" \t");
            return line.substr(begin, end - begin + 1);
        }();
        const bool entry = NativeKnownHosts::isHostEntry(trimmed);
        if (entry && index++ == target) {
            removed = true;
            continue;
        }
        lines.push_back(line);
    }
    if (!removed) {
        error = NativeString("指定的主机指纹不存在");
        return false;
    }
    const std::filesystem::path temporary = NativeFile::uniqueTemporarySibling(path);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = NativeString("无法写入已知主机文件");
            return false;
        }
        for (const std::string &value : lines) output << value << '\n';
    }
    if (!NativeFile::replaceAtomically(temporary, path)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = NativeString("无法更新已知主机文件");
        return false;
    }
    return true;
}

NativeJsonDom::Object WebViewBackend::operateLocalEntry(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    const NativeString operation = nativeText(
        NativeJsonDom::stringValue(params, "operation")).toLower();
    const auto validName = [](const NativeString &name) {
        return !name.isEmpty() && name != NativeString(".") && name != NativeString("..")
            && !name.contains(static_cast<char>('/')) && !name.contains(static_cast<char>('\\'))
            && nativePath(name).filename() == nativePath(name);
    };

    if (operation == NativeString("mkdir") || operation == NativeString("touch")) {
        const NativeString parentPath = pathText(nativePath(
            nativeText(NativeJsonDom::stringValue(
                params, "parentPath")).trimmed()).lexically_normal());
        const NativeString name = nativeText(
            NativeJsonDom::stringValue(params, "name")).trimmed();
        const std::filesystem::path parent = nativePath(parentPath);
        if (!parent.is_absolute() || !std::filesystem::is_directory(parent)) {
            error = NativeString("本机父目录无效");
            return {};
        }
        if (!validName(name)) {
            error = NativeString("名称不能为空，且不能包含路径分隔符");
            return {};
        }
        const NativeString path = pathText(parent / name.toStdWString());
        if (std::filesystem::exists(nativePath(path))) {
            error = NativeString("同名项目已存在：") + name;
            return {};
        }
        bool succeeded = false;
        if (operation == NativeString("mkdir")) {
            succeeded = std::filesystem::create_directory(nativePath(path));
        } else {
            std::ofstream file(nativePath(path), std::ios::binary | std::ios::out);
            succeeded = file.good();
        }
        if (!succeeded) {
            error = operation == NativeString("mkdir")
                ? NativeString("无法创建本机文件夹：") + name
                : NativeString("无法创建本机文件：") + name;
            return {};
        }
        NativeJsonDom::Object result;
        result.values.emplace("operation", utf8Text(operation));
        result.values.emplace("path", utf8Text(path));
        return result;
    }

    if (operation == NativeString("rename")) {
        const NativeString path = pathText(nativePath(
            nativeText(NativeJsonDom::stringValue(
                params, "path")).trimmed()).lexically_normal());
        const NativeString name = nativeText(
            NativeJsonDom::stringValue(params, "name")).trimmed();
        const std::filesystem::path source = nativePath(path);
        if ((!std::filesystem::exists(source) && !std::filesystem::is_symlink(source))
            || !source.is_absolute() || source == source.root_path()) {
            error = NativeString("本机项目不存在或不允许重命名");
            return {};
        }
        if (!validName(name)) {
            error = NativeString("名称不能为空，且不能包含路径分隔符");
            return {};
        }
        const NativeString targetPath = pathText(source.parent_path() / name.toStdWString());
        if (std::filesystem::exists(nativePath(targetPath))) {
            error = NativeString("同名项目已存在：") + name;
            return {};
        }
        std::error_code renameError;
        std::filesystem::rename(source, nativePath(targetPath), renameError);
        if (renameError) {
            error = NativeString("无法重命名本机项目：") + pathText(source.filename());
            return {};
        }
        NativeJsonDom::Object result;
        result.values.emplace("operation", utf8Text(operation));
        result.values.emplace("path", utf8Text(path));
        result.values.emplace("targetPath", utf8Text(targetPath));
        return result;
    }

    error = NativeString("不支持的本机文件操作");
    return {};
}

NativeJsonDom::Object WebViewBackend::beginDroppedFile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const NativeString fileName = pathText(nativePath(
        nativeText(NativeJsonDom::stringValue(params, "name"))).filename()).trimmed();
    if (fileName.isEmpty() || fileName == NativeString(".")
        || fileName == NativeString("..")) {
        error = NativeString("拖入的文件名称无效");
        return {};
    }
    const std::filesystem::path directory = nativePath(temporaryPath())
        / L"MasterTerm" / L"dropped-files";
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        error = NativeString("无法创建拖放临时目录");
        return {};
    }
    const NativeString token = droppedFileToken();
    const NativeString path = pathText(directory / (token + NativeString("-") + fileName).toStdWString());
    auto file = std::make_unique<std::ofstream>(nativePath(path),
                                                std::ios::binary | std::ios::trunc);
    if (!file->is_open()) {
        error = NativeString("无法暂存拖入文件：") + path;
        return {};
    }
    m_droppedFiles.emplace(token, std::move(file));
    m_droppedFilePaths.emplace(token, path);
    NativeJsonDom::Object result;
    result.values.emplace("token", utf8Text(token));
    return result;
}

bool WebViewBackend::appendDroppedFile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const NativeString token = nativeText(
        NativeJsonDom::stringValue(params, "token"));
    const auto fileIt = m_droppedFiles.find(token);
    std::ofstream *file = fileIt == m_droppedFiles.end() || !fileIt->second
        ? nullptr : fileIt->second.get();
    if (!file || !file->is_open()) {
        error = NativeString("拖放临时文件已失效");
        return false;
    }
    const std::string bytes = NativeBase64::decode(
        NativeJsonDom::stringValue(params, "data"));
    file->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!*file) {
        error = NativeString("写入拖放临时文件失败：") +
            NativeString::fromStdString(std::strerror(errno));
        discardDroppedFile(token);
        return false;
    }
    return true;
}

NativeJsonDom::Object WebViewBackend::finishDroppedFile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const NativeString token = nativeText(
        NativeJsonDom::stringValue(params, "token"));
    std::unique_ptr<std::ofstream> file = takeMapped(m_droppedFiles, token);
    const NativeString path = takeMapped(m_droppedFilePaths, token);
    if (!file || path.isEmpty()) {
        error = NativeString("拖放临时文件已失效");
        return {};
    }
    file->flush();
    file->close();
    const std::int64_t expectedSize =
        static_cast<std::int64_t>(NativeJsonDom::numberValue(params, "size", -1));
    if (expectedSize >= 0 && static_cast<std::int64_t>(std::filesystem::file_size(nativePath(path))) != expectedSize) {
        std::filesystem::remove(nativePath(path));
        error = NativeString("拖入文件暂存不完整");
        return {};
    }
    NativeJsonDom::Object result;
    result.values.emplace("path", utf8Text(path));
    return result;
}

void WebViewBackend::discardDroppedFile(const NativeString &token)
{
    std::unique_ptr<std::ofstream> file = takeMapped(m_droppedFiles, token);
    const NativeString path = takeMapped(m_droppedFilePaths, token);
    if (file) {
        file->close();
    }
    if (!path.isEmpty())
        std::filesystem::remove(nativePath(path));
}

bool WebViewBackend::removeLocalEntries(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const NativeJsonDom::Array *paths =
        NativeJsonDom::arrayValue(params, "paths");
    if (!paths || paths->values.empty()) {
        error = NativeString("没有选择要删除的本机文件");
        return false;
    }
    for (const NativeJsonDom::Value &value : paths->values) {
        const NativeString text = value.isString()
            ? nativeText(value.string()).trimmed() : NativeString();
        const std::filesystem::path path = nativePath(text).lexically_normal();
        std::error_code fileError;
        if (!std::filesystem::exists(path, fileError) && !std::filesystem::is_symlink(path, fileError)) {
            error = NativeString("本机文件不存在：") + pathText(path);
            return false;
        }
        if (!path.is_absolute() || path == path.root_path()) {
            error = NativeString("禁止删除本机根目录");
            return false;
        }
        std::filesystem::remove_all(path, fileError);
        if (fileError) {
            error = NativeString("无法删除本机项目：") + pathText(path);
            return false;
        }
    }
    return true;
}

void WebViewBackend::startSftpList(const NativeString &requestId, int profileIndex, const NativeString &path)
{
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0 || profileIndex >= static_cast<int>(records.size())) {
        sendError(requestId, NativeString("服务器配置不存在"));
        return;
    }
    const ServerRecord &record =
        records.at(static_cast<std::size_t>(profileIndex));
    const NativeString type = nativeText(record.connectionType);
    const NativeString address = nativeText(record.address);
    const NativeString port = nativeText(record.port);
    const NativeString configuredPassword = nativeText(record.password);
    const NativeString password = configuredPassword.isEmpty()
        ? readStoredPassword(address) : configuredPassword;
    const NativeString keyPath = nativeText(record.keyPath);
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString configuredProxyPassword = nativeText(record.proxyPassword);
    const NativeString proxyPassword = configuredProxyPassword.isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : configuredProxyPassword;
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);

    if (type != NativeString("ssh")) {
        sendError(requestId, NativeString("SFTP 仅适用于 SSH 服务器"));
        return;
    }
    if (password.isEmpty() && keyPath.isEmpty()) {
        sendError(requestId, NativeString("未找到 SFTP 凭据，请先连接一次该 SSH 服务器"));
        return;
    }

    const NativeString workerPath =
        applicationDirectory() + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath))) {
        sendError(requestId, NativeString("找不到 MasterTermSftpWorker.exe"));
        return;
    }

    auto *process = new NativeProcess();
    if (!process->start(
            toWide(workerPath),
            toWideArguments({NativeString("--list"), address, port, path, keyPath}),
            sftpEnvironment(
                password, proxyJump, proxyPassword, proxyKeyPath))) {
        const NativeString message = NativeString("无法启动 SFTP 工作进程：")
            + nativeProcessError(process);
        delete process;
        sendError(requestId, message);
        return;
    }
    m_sftpProcesses.insert(process);
    m_sftpListRequests[process] = {requestId, profileIndex, path};
}

void WebViewBackend::finishSftpList(NativeProcess *process, const NativeString &requestId,
                                    int profileIndex, const NativeString &path, int exitCode)
{
    if (m_sftpProcesses.erase(process) == 0)
        return;
    m_sftpListRequests.erase(process);
    std::string standardOutput = takeMapped(m_sftpStandardOutputs, process);
    standardOutput += process->takeStandardOutput();
    std::string standardErrorBytes = takeMapped(m_sftpStandardErrors, process);
    standardErrorBytes += process->takeStandardError();
    const NativeString standardError = utf8Text(standardErrorBytes).trimmed();
    delete process;
    if (exitCode != 0) {
        sendError(requestId, standardError.isEmpty()
            ? NativeString("SFTP 目录读取失败") : standardError.section(static_cast<char>('\n'), -1));
        return;
    }

    NativeJsonDom::Array entries;
    NativeString resultPath = path;
    for (const std::string &line : splitBytes(standardOutput, '\n')) {
        if (utf8Text(line).trimmed().isEmpty())
            continue;
        const std::vector<std::string> fields = splitBytes(line, '\t');
        if (fields.size() >= 2 && fields.at(0) == "R") {
            const NativeString resolved = decodeBase64Text(fields.at(1));
            if (!resolved.isEmpty())
                resultPath = cleanRemotePath(resolved);
            continue;
        }
        if (fields.size() < 4)
            continue;
        const NativeString name = decodeBase64Text(fields.at(0));
        if (name.isEmpty())
            continue;
        const NativeString kind = NativeString::fromLatin1(fields.at(1).data(), static_cast<int>(fields.at(1).size()));
        const std::int64_t modified = utf8Text(fields.at(3)).toLongLong();
        const NativeString fullPath = resultPath == NativeString("/")
            ? resultPath + name : resultPath + static_cast<char>('/') + name;
        NativeJsonDom::Object entry;
        entry.values.emplace("name", utf8Text(name));
        entry.values.emplace("path", utf8Text(fullPath));
        entry.values.emplace("kind", utf8Text(kind));
        entry.values.emplace("directory", kind == NativeString("d"));
        entry.values.emplace(
            "size",
            static_cast<double>(utf8Text(fields.at(2)).toLongLong()));
        entry.values.emplace("modified", static_cast<double>(modified));
        entry.values.emplace(
            "permissions",
            fields.size() > 4 ? fields.at(4) : std::string("—"));
        entry.values.emplace(
            "owner",
            fields.size() > 5 ? fields.at(5) : std::string("—"));
        entries.values.emplace_back(std::move(entry));
    }
    NativeJsonDom::Object result;
    result.values.emplace("index", static_cast<double>(profileIndex));
    result.values.emplace("path", utf8Text(resultPath));
    result.values.emplace("entries", std::move(entries));
    sendNativeResult(requestId, NativeJsonDom::Value(result));
}

void WebViewBackend::startSftpHistory(const NativeString &requestId, int profileIndex,
                                      const NativeJsonDom::Object &options)
{
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0 || profileIndex >= static_cast<int>(records.size())) {
        sendError(requestId, NativeString("服务器配置不存在"));
        return;
    }
    const ServerRecord &record = records.at(static_cast<std::size_t>(profileIndex));
    const NativeString address = nativeText(record.address);
    const NativeString port = nativeText(record.port);
    const NativeString password = nativeText(record.password).isEmpty()
        ? readStoredPassword(address) : nativeText(record.password);
    const NativeString keyPath = nativeText(record.keyPath);
    if (nativeText(record.connectionType) != NativeString("ssh")
        || (password.isEmpty() && keyPath.isEmpty())) {
        sendNativeResult(requestId, NativeJsonDom::Value(NativeJsonDom::Array{}));
        return;
    }
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword = nativeText(record.proxyPassword).isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : nativeText(record.proxyPassword);
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    const int at = address.lastIndexOf('@');
    const NativeString user = at > 0 ? address.left(at) : NativeString();
    const NativeString homePath = user == NativeString("root") ? NativeString("/root")
        : (user.isEmpty() ? NativeString("/") : NativeString("/home/") + user);
    const NativeString workerPath = applicationDirectory()
        + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath))) {
        sendError(requestId, NativeString("找不到 MasterTermSftpWorker.exe"));
        return;
    }
    auto *process = new NativeProcess();
    auto environment = sftpEnvironment(password, proxyJump, proxyPassword, proxyKeyPath);
    const std::filesystem::path historyDirectory = nativePath(temporaryPath())
        / L"MasterTerm" / L"history";
    std::error_code directoryError;
    std::filesystem::create_directories(historyDirectory, directoryError);
    if (directoryError) {
        delete process;
        sendError(requestId, NativeString("无法创建历史命令临时目录"));
        return;
    }
    const NativeString historyOutputPath = pathText(historyDirectory /
        (L"history-" + std::to_wstring(GetCurrentProcessId()) + L"-"
         + std::to_wstring(m_nextTransferNumber++) + L".txt"));
    environment[L"MASTERSSH_HISTORY_OUTPUT_FILE"] = historyOutputPath.toStdWString();
    environment[L"MASTERSSH_HISTORY_LIMIT"] = std::to_wstring(
        std::max(0, NativeJsonDom::integerValue(options, "limit", 500)));
    environment[L"MASTERSSH_HISTORY_DAYS"] = std::to_wstring(
        std::max(0, NativeJsonDom::integerValue(options, "days", 0)));
    environment[L"MASTERSSH_HISTORY_BASH"] = NativeJsonDom::booleanValue(
        options, "readBash", true) ? L"1" : L"0";
    environment[L"MASTERSSH_HISTORY_ZSH"] = NativeJsonDom::booleanValue(
        options, "readZsh", true) ? L"1" : L"0";
    environment[L"MASTERSSH_HISTORY_DEDUP"] = NativeJsonDom::booleanValue(
        options, "deduplicate", true) ? L"1" : L"0";
    if (!process->start(toWide(workerPath),
            toWideArguments({NativeString("--read-history"), address, port, homePath, keyPath}),
            environment)) {
        const NativeString message = NativeString("无法读取远端历史：")
            + nativeProcessError(process);
        delete process;
        std::filesystem::remove(nativePath(historyOutputPath));
        sendError(requestId, message);
        return;
    }
    m_sftpProcesses.insert(process);
    m_sftpTemporaryFiles[process] = historyOutputPath;
    m_sftpHistoryRequests[process] = {
        requestId, profileIndex, std::chrono::steady_clock::now()};
}

void WebViewBackend::finishSftpHistory(NativeProcess *process, const NativeString &requestId,
                                       int, int exitCode)
{
    if (m_sftpProcesses.erase(process) == 0)
        return;
    m_sftpHistoryRequests.erase(process);
    std::string pipeOutput = takeMapped(m_sftpStandardOutputs, process);
    pipeOutput += process->takeStandardOutput();
    const std::size_t pipeOutputBytes = pipeOutput.size();
    const NativeString historyOutputPath = takeMapped(m_sftpTemporaryFiles, process);
    std::string output;
    bool usedResultFile = false;
    if (!historyOutputPath.isEmpty()) {
        std::ifstream historyFile(nativePath(historyOutputPath), std::ios::binary);
        if (historyFile) {
            output.assign(std::istreambuf_iterator<char>(historyFile), {});
            usedResultFile = !output.empty();
        }
        historyFile.close();
        std::filesystem::remove(nativePath(historyOutputPath));
    }
    if (output.empty()) output = std::move(pipeOutput);
    std::string errors = takeMapped(m_sftpStandardErrors, process);
    errors += process->takeStandardError();
    delete process;
    if (exitCode != 0) {
        sendError(requestId, utf8Text(errors).trimmed().isEmpty()
            ? NativeString("读取远端历史失败") : utf8Text(errors).trimmed());
        return;
    }
    NativeJsonDom::Array commands;
    double remoteBytes = 0;
    double workerRecords = 0;
    std::string sourcePath;
    double repairedRecords = 0;
    for (const std::string &line : splitBytes(output, '\n')) {
        const std::vector<std::string> fields = splitBytes(line, '\t');
        if (fields.size() == 4 && fields.at(0) == "M") {
            try {
                remoteBytes = static_cast<double>(std::stoull(fields.at(1)));
                workerRecords = static_cast<double>(std::stoull(fields.at(2)));
            } catch (...) {
                remoteBytes = 0;
                workerRecords = 0;
            }
            sourcePath = decodeBase64Text(fields.at(3)).toUtf8();
            continue;
        }
        if (fields.size() == 2 && fields.at(0) == "H") {
            const NativeString decoded = decodeBase64Text(fields.at(1)).trimmed();
            bool repaired = false;
            const std::string command = sanitizeUtf8(decoded.toUtf8(), repaired);
            if (!command.empty()) commands.values.emplace_back(command);
            if (repaired) repairedRecords += 1;
        }
    }
    NativeJsonDom::Object diagnostics;
    diagnostics.values.emplace("sourcePath", sourcePath);
    diagnostics.values.emplace("remoteBytes", remoteBytes);
    diagnostics.values.emplace("workerRecords", workerRecords);
    diagnostics.values.emplace("transportBytes", static_cast<double>(output.size()));
    diagnostics.values.emplace("pipeBytes", static_cast<double>(pipeOutputBytes));
    diagnostics.values.emplace("backendRecords", static_cast<double>(commands.values.size()));
    diagnostics.values.emplace("repairedRecords", repairedRecords);
    diagnostics.values.emplace("transport", usedResultFile ? "result-file" : "stdout-pipe");
    diagnostics.values.emplace("exitCode", static_cast<double>(exitCode));
    NativeJsonDom::Object result;
    result.values.emplace("commands", std::move(commands));
    result.values.emplace("diagnostics", std::move(diagnostics));
    sendNativeResult(requestId, NativeJsonDom::Value(std::move(result)));
}

void WebViewBackend::startSftpPreview(
    const NativeString &requestId, const NativeJsonDom::Object &params)
{
    const int profileIndex =
        NativeJsonDom::integerValue(params, "index", -1);
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0
        || profileIndex >= static_cast<int>(records.size())) {
        sendError(requestId, NativeString("服务器配置不存在"));
        return;
    }
    NativeString remotePath = nativeText(
        NativeJsonDom::stringValue(params, "remotePath")).trimmed();
    if (remotePath.isEmpty()) {
        sendError(requestId, NativeString("远程路径不能为空"));
        return;
    }
    if (!remotePath.startsWith(static_cast<char>('/')))
        remotePath.prepend(static_cast<char>('/'));
    remotePath = cleanRemotePath(remotePath);
    const ServerRecord &record =
        records.at(static_cast<std::size_t>(profileIndex));
    if (nativeText(record.connectionType) != NativeString("ssh")) {
        sendError(requestId, NativeString("SFTP 仅适用于 SSH 服务器"));
        return;
    }
    const NativeString address = nativeText(record.address);
    const NativeString port = nativeText(record.port);
    const NativeString password = nativeText(record.password).isEmpty()
        ? readStoredPassword(address) : nativeText(record.password);
    const NativeString keyPath = nativeText(record.keyPath);
    if (password.isEmpty() && keyPath.isEmpty()) {
        sendError(requestId, NativeString(
            "未找到 SFTP 凭据，请先连接一次该 SSH 服务器"));
        return;
    }
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword = nativeText(record.proxyPassword).isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : nativeText(record.proxyPassword);
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    const NativeString workerPath = applicationDirectory()
        + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath))) {
        sendError(requestId, NativeString("找不到 MasterTermSftpWorker.exe"));
        return;
    }
    const int previewBytes = std::max(1024, std::min(1048576,
        NativeJsonDom::integerValue(params, "maxBytes", 524288)));
    auto *process = new NativeProcess();
    if (!process->start(toWide(workerPath), toWideArguments({
            NativeString("--preview"), address, port, remotePath,
            NativeString::number(previewBytes), keyPath}),
            sftpEnvironment(password, proxyJump, proxyPassword, proxyKeyPath,
                            NativeString("overwrite")))) {
        const NativeString message = NativeString("无法预览远程文件：")
            + nativeProcessError(process);
        delete process;
        sendError(requestId, message);
        return;
    }
    m_sftpProcesses.insert(process);
    m_sftpPreviewRequests[process] = {requestId, profileIndex, remotePath};
}

void WebViewBackend::finishSftpPreview(NativeProcess *process, int exitCode)
{
    const auto request = m_sftpPreviewRequests.find(process);
    if (request == m_sftpPreviewRequests.end())
        return;
    const SftpPreviewRequest preview = request->second;
    m_sftpPreviewRequests.erase(request);
    m_sftpProcesses.erase(process);
    std::string output = takeMapped(m_sftpStandardOutputs, process);
    output += process->takeStandardOutput();
    std::string errors = takeMapped(m_sftpStandardErrors, process);
    errors += process->takeStandardError();
    delete process;
    if (exitCode != 0) {
        sendError(preview.requestId, utf8Text(errors).trimmed().isEmpty()
            ? NativeString("预览远程文件失败") : utf8Text(errors).trimmed());
        return;
    }
    std::string encoded;
    for (const std::string &line : splitBytes(output, '\n')) {
        const std::vector<std::string> fields = splitBytes(line, '\t');
        if (fields.size() == 2 && fields.at(0) == "B") {
            encoded = fields.at(1);
            break;
        }
    }
    NativeJsonDom::Object result;
    result.values.emplace(
        "index", static_cast<double>(preview.profileIndex));
    result.values.emplace("remotePath", utf8Text(preview.path));
    result.values.emplace("data", encoded);
    result.values.emplace("name", utf8Text(fileNameText(preview.path)));
    sendNativeResult(preview.requestId, NativeJsonDom::Value(result));
}

void WebViewBackend::openRemoteFile(
    const NativeString &requestId, const NativeJsonDom::Object &params)
{
    const int profileIndex = NativeJsonDom::integerValue(params, "index", -1);
    const NativeString remotePath = cleanRemotePath(nativeText(
        NativeJsonDom::stringValue(params, "remotePath")).trimmed());
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0 || profileIndex >= static_cast<int>(records.size())) {
        sendError(requestId, NativeString("服务器配置不存在"));
        return;
    }
    if (remotePath.isEmpty() || remotePath == NativeString("/")) {
        sendError(requestId, NativeString("远程文件路径无效"));
        return;
    }
    const ServerRecord &record = records.at(static_cast<std::size_t>(profileIndex));
    const NativeString address = nativeText(record.address);
    const NativeString password = nativeText(record.password).isEmpty()
        ? readStoredPassword(address) : nativeText(record.password);
    const NativeString keyPath = nativeText(record.keyPath);
    if (record.connectionType != "ssh") {
        sendError(requestId, NativeString("SFTP 仅适用于 SSH 服务器"));
        return;
    }
    if (password.isEmpty() && keyPath.isEmpty()) {
        sendError(requestId, NativeString("未找到 SFTP 凭据，请先连接一次该 SSH 服务器"));
        return;
    }

    const NativeString editKey = remoteEditKey(profileIndex, remotePath);
    const bool chooseApplication = NativeJsonDom::booleanValue(
        params, "chooseApplication", false);
    const NativeString applicationPath = nativeText(
        NativeJsonDom::stringValue(params, "applicationPath")).trimmed();
    NativeString conflict = nativeText(
        NativeJsonDom::stringValue(params, "autoSyncConflict", "overwrite")).toLower();
    if (conflict != NativeString("skip") && conflict != NativeString("rename"))
        conflict = NativeString("overwrite");
    const auto existing = m_remoteEdits.find(editKey);
    if (existing != m_remoteEdits.end()
        && std::filesystem::is_regular_file(nativePath(existing->second.localPath))) {
        if (!openLocalFileWithWindows(existing->second.localPath, chooseApplication,
                                      applicationPath)) {
            sendError(requestId, NativeString("无法使用 Windows 程序打开临时副本"));
            return;
        }
        NativeJsonDom::Object result;
        result.values.emplace("remotePath", utf8Text(remotePath));
        result.values.emplace("localPath", utf8Text(existing->second.localPath));
        result.values.emplace("reused", true);
        sendNativeResult(requestId, NativeJsonDom::Value(std::move(result)));
        return;
    }

    const NativeString workerPath = applicationDirectory()
        + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath))) {
        sendError(requestId, NativeString("找不到 MasterTermSftpWorker.exe"));
        return;
    }
    std::wstring localName = fileNameText(remotePath).toStdWString();
    for (wchar_t &character : localName) {
        if (character == L'<' || character == L'>' || character == L':'
            || character == L'"' || character == L'/' || character == L'\\'
            || character == L'|' || character == L'?' || character == L'*')
            character = L'_';
    }
    if (localName.empty() || localName == L"." || localName == L"..")
        localName = L"remote-file";
    const std::filesystem::path editDirectory = nativePath(temporaryPath())
        / L"MasterTerm" / L"remote-edits"
        / std::to_wstring(GetCurrentProcessId())
        / std::to_wstring(m_nextTransferNumber++);
    std::error_code directoryError;
    std::filesystem::create_directories(editDirectory, directoryError);
    if (directoryError) {
        sendError(requestId, NativeString("无法创建远程编辑临时目录"));
        return;
    }
    const NativeString localPath = pathText(editDirectory / localName);
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword = nativeText(record.proxyPassword).isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : nativeText(record.proxyPassword);
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    auto *process = new NativeProcess();
    if (!process->start(toWide(workerPath), toWideArguments({
            NativeString("--download"), address, nativeText(record.port),
            remotePath, localPath, keyPath}),
            sftpEnvironment(password, proxyJump, proxyPassword, proxyKeyPath,
                            NativeString("overwrite")))) {
        const NativeString message = NativeString("无法下载远程编辑副本：")
            + nativeProcessError(process);
        delete process;
        sendError(requestId, message);
        return;
    }
    m_sftpProcesses.insert(process);
    m_remoteEditDownloads[process] = {
        requestId, editKey, localPath, remotePath, profileIndex, chooseApplication,
        applicationPath, conflict};
    NativeJsonDom::Object result;
    result.values.emplace("remotePath", utf8Text(remotePath));
    result.values.emplace("localPath", utf8Text(localPath));
    result.values.emplace("pending", true);
    sendNativeResult(requestId, NativeJsonDom::Value(std::move(result)));
}

void WebViewBackend::processRemoteEditDownloadProgress(
    NativeProcess *process, const RemoteEditDownload &download)
{
    // The download worker reports progress lines ("P\t<done>\t<total>\t...")
    // on stdout after every block.  Reading them continuously is mandatory:
    // a large file fills the pipe buffer and the worker blocks on flush
    // forever unless the parent drains stdout.
    std::string &buffer = m_sftpTransferBuffers[process];
    buffer += process->takeStandardOutput();
    while (true) {
        const std::size_t newline = buffer.find('\n');
        if (newline == std::string::npos)
            break;
        const std::string rawLine = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        const auto progress = parseSftpTransferProgressLine(rawLine);
        if (!progress)
            continue;
        NativeJsonDom::Object payload;
        payload.values.emplace(
            "index", static_cast<double>(download.profileIndex));
        payload.values.emplace("remotePath", utf8Text(download.remotePath));
        payload.values.emplace(
            "name", utf8Text(fileNameText(download.remotePath)));
        payload.values.emplace("state", "downloading");
        payload.values.emplace(
            "done", static_cast<double>(progress->done));
        payload.values.emplace(
            "total", static_cast<double>(progress->total));
        sendNativeEvent("sftp.remote-edit", "", payload);
    }
}

void WebViewBackend::finishRemoteEditDownload(NativeProcess *process, int exitCode)
{
    const auto request = m_remoteEditDownloads.find(process);
    if (request == m_remoteEditDownloads.end())
        return;
    const RemoteEditDownload download = request->second;
    m_remoteEditDownloads.erase(request);
    m_sftpTransferBuffers.erase(process);
    m_sftpProcesses.erase(process);
    std::string errors = takeMapped(m_sftpStandardErrors, process);
    errors += process->takeStandardError();
    process->takeStandardOutput();
    delete process;

    NativeJsonDom::Object payload;
    payload.values.emplace("index", static_cast<double>(download.profileIndex));
    payload.values.emplace("remotePath", utf8Text(download.remotePath));
    payload.values.emplace("name", utf8Text(fileNameText(download.remotePath)));
    if (exitCode != 0 || !std::filesystem::is_regular_file(nativePath(download.localPath))) {
        payload.values.emplace("state", "error");
        payload.values.emplace("message", utf8Text(errors.empty()
            ? NativeString("下载远程编辑副本失败") : utf8Text(errors).trimmed()));
        sendNativeEvent("sftp.remote-edit", "", payload);
        return;
    }

    std::error_code infoError;
    RemoteEdit edit;
    edit.localPath = download.localPath;
    edit.remotePath = download.remotePath;
    edit.profileIndex = download.profileIndex;
    edit.conflict = download.conflict;
    edit.size = std::filesystem::file_size(nativePath(edit.localPath), infoError);
    if (!infoError)
        edit.modified = std::filesystem::last_write_time(nativePath(edit.localPath), infoError);
    m_remoteEdits[download.editKey] = std::move(edit);
    if (!openLocalFileWithWindows(download.localPath, download.chooseApplication,
                                  download.applicationPath)) {
        payload.values.emplace("state", "error");
        payload.values.emplace("message", "无法使用 Windows 程序打开临时副本");
        sendNativeEvent("sftp.remote-edit", "", payload);
        return;
    }
    payload.values.emplace("state", "opened");
    payload.values.emplace("localPath", utf8Text(download.localPath));
    sendNativeEvent("sftp.remote-edit", "", payload);
}

void WebViewBackend::startRemoteEditUpload(const NativeString &editKey)
{
    const auto editIt = m_remoteEdits.find(editKey);
    if (editIt == m_remoteEdits.end() || editIt->second.uploadInFlight)
        return;
    RemoteEdit &edit = editIt->second;
    const std::vector<ServerRecord> records = readServerRecords();
    if (edit.profileIndex < 0 || edit.profileIndex >= static_cast<int>(records.size()))
        return;
    const ServerRecord &record = records.at(static_cast<std::size_t>(edit.profileIndex));
    const NativeString address = nativeText(record.address);
    const NativeString password = nativeText(record.password).isEmpty()
        ? readStoredPassword(address) : nativeText(record.password);
    const NativeString keyPath = nativeText(record.keyPath);
    if (record.connectionType != "ssh" || (password.isEmpty() && keyPath.isEmpty()))
        return;
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword = nativeText(record.proxyPassword).isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : nativeText(record.proxyPassword);
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    if (edit.conflict == "prompt") {
        edit.changed = false;
        NativeJsonDom::Object payload;
        payload.values.emplace("index", static_cast<double>(edit.profileIndex));
        payload.values.emplace("remotePath", utf8Text(edit.remotePath));
        payload.values.emplace("name", utf8Text(fileNameText(edit.remotePath)));
        payload.values.emplace("state", "conflict_prompt");
        sendNativeEvent("sftp.remote-edit", "", payload);
        return;
    }
    const NativeString workerPath = applicationDirectory()
        + NativeString("/MasterTermSftpWorker.exe");
    auto *process = new NativeProcess();
    if (!process->start(toWide(workerPath), toWideArguments({
            NativeString("--upload"), address, nativeText(record.port),
            edit.localPath, edit.remotePath, keyPath}),
            sftpEnvironment(password, proxyJump, proxyPassword, proxyKeyPath,
                            edit.conflict.isEmpty() ? NativeString("overwrite") : edit.conflict))) {
        delete process;
        return;
    }
    edit.uploadInFlight = true;
    edit.changed = false;
    m_sftpProcesses.insert(process);
    m_remoteEditUploads[process] = editKey;
    NativeJsonDom::Object payload;
    payload.values.emplace("index", static_cast<double>(edit.profileIndex));
    payload.values.emplace("remotePath", utf8Text(edit.remotePath));
    payload.values.emplace("name", utf8Text(fileNameText(edit.remotePath)));
    payload.values.emplace("state", "syncing");
    sendNativeEvent("sftp.remote-edit", "", payload);
}

void WebViewBackend::finishRemoteEditUpload(NativeProcess *process, int exitCode)
{
    const auto upload = m_remoteEditUploads.find(process);
    if (upload == m_remoteEditUploads.end())
        return;
    const NativeString editKey = upload->second;
    m_remoteEditUploads.erase(upload);
    m_sftpProcesses.erase(process);
    std::string errors = takeMapped(m_sftpStandardErrors, process);
    errors += process->takeStandardError();
    process->takeStandardOutput();
    delete process;
    const auto edit = m_remoteEdits.find(editKey);
    if (edit == m_remoteEdits.end())
        return;
    edit->second.uploadInFlight = false;
    std::error_code infoError;
    const std::filesystem::path localPath = nativePath(edit->second.localPath);
    if (std::filesystem::is_regular_file(localPath, infoError)) {
        edit->second.size = std::filesystem::file_size(localPath, infoError);
        edit->second.modified = std::filesystem::last_write_time(localPath, infoError);
    }
    edit->second.changed = false;
    NativeJsonDom::Object payload;
    payload.values.emplace("index", static_cast<double>(edit->second.profileIndex));
    payload.values.emplace("remotePath", utf8Text(edit->second.remotePath));
    payload.values.emplace("name", utf8Text(fileNameText(edit->second.remotePath)));
    if (exitCode == 0) {
        payload.values.emplace("state", "synced");
    } else {
        payload.values.emplace("state", "error");
        payload.values.emplace("message", utf8Text(errors.empty()
            ? NativeString("自动同步远程文件失败") : utf8Text(errors).trimmed()));
    }
    sendNativeEvent("sftp.remote-edit", "", payload);
}

void WebViewBackend::pollRemoteEdits()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<NativeString> ready;
    for (auto &item : m_remoteEdits) {
        RemoteEdit &edit = item.second;
        std::error_code error;
        const std::filesystem::path path = nativePath(edit.localPath);
        if (!std::filesystem::is_regular_file(path, error))
            continue;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error) continue;
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) continue;
        if (size != edit.size || modified != edit.modified) {
            edit.size = size;
            edit.modified = modified;
            edit.changed = true;
            edit.changedAt = now;
            continue;
        }
        if (edit.changed && !edit.uploadInFlight
            && now - edit.changedAt >= std::chrono::seconds(1))
            ready.push_back(item.first);
    }
    for (const NativeString &editKey : ready)
        startRemoteEditUpload(editKey);
}

bool WebViewBackend::retryRemoteEdit(const NativeJsonDom::Object &params, NativeString &error)
{
    const int profileIndex = NativeJsonDom::integerValue(params, "index", -1);
    const NativeString remotePath = nativeText(
        NativeJsonDom::stringValue(params, "remotePath")).trimmed();
    const NativeString key = remoteEditKey(profileIndex, remotePath);
    const auto edit = m_remoteEdits.find(key);
    if (profileIndex < 0 || remotePath.isEmpty() || edit == m_remoteEdits.end()) {
        error = NativeString("找不到可重试的远程编辑副本");
        return false;
    }
    if (edit->second.uploadInFlight) {
        error = NativeString("该编辑副本正在同步");
        return false;
    }
    const NativeString conflict = nativeText(
        NativeJsonDom::stringValue(params, "conflict")).trimmed();
    if (!conflict.isEmpty()) {
        edit->second.conflict = conflict;
    }
    edit->second.changed = true;
    startRemoteEditUpload(key);
    if (!edit->second.uploadInFlight && edit->second.conflict != "prompt") {
        error = NativeString("无法启动远程编辑同步");
        return false;
    }
    return true;
}

bool WebViewBackend::stopRemoteEdit(const NativeJsonDom::Object &params, NativeString &error)
{
    const int profileIndex = NativeJsonDom::integerValue(params, "index", -1);
    const NativeString remotePath = nativeText(
        NativeJsonDom::stringValue(params, "remotePath")).trimmed();
    const NativeString key = remoteEditKey(profileIndex, remotePath);
    const auto edit = m_remoteEdits.find(key);
    if (profileIndex < 0 || remotePath.isEmpty() || edit == m_remoteEdits.end()) {
        error = NativeString("找不到远程编辑副本");
        return false;
    }
    for (auto upload = m_remoteEditUploads.begin(); upload != m_remoteEditUploads.end();) {
        if (upload->second != key) {
            ++upload;
            continue;
        }
        NativeProcess *process = upload->first;
        upload = m_remoteEditUploads.erase(upload);
        m_sftpProcesses.erase(process);
        m_sftpStandardOutputs.erase(process);
        m_sftpStandardErrors.erase(process);
        process->terminate();
        delete process;
    }
    m_remoteEdits.erase(edit);
    return true;
}

bool WebViewBackend::openRemoteEditDirectory(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    const int profileIndex = NativeJsonDom::integerValue(params, "index", -1);
    const NativeString remotePath = nativeText(
        NativeJsonDom::stringValue(params, "remotePath")).trimmed();
    const auto edit = m_remoteEdits.find(remoteEditKey(profileIndex, remotePath));
    if (profileIndex < 0 || remotePath.isEmpty() || edit == m_remoteEdits.end()) {
        error = NativeString("找不到远程编辑副本");
        return false;
    }
#ifdef _WIN32
    const std::filesystem::path directory = nativePath(edit->second.localPath).parent_path();
    const HINSTANCE launched = ShellExecuteW(nullptr, L"open", directory.c_str(),
                                              nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) > 32)
        return true;
    error = NativeString("无法打开本机副本目录");
    return false;
#else
    (void)edit;
    error = NativeString("当前平台不支持打开本机副本目录");
    return false;
#endif
}

void WebViewBackend::failSftpList(NativeProcess *process, const NativeString &requestId,
                                  const NativeString &message)
{
    if (m_sftpProcesses.erase(process) == 0)
        return;
    m_sftpListRequests.erase(process);
    m_sftpStandardOutputs.erase(process);
    m_sftpStandardErrors.erase(process);
    delete process;
    sendError(requestId, message);
}

void WebViewBackend::startSftpOperation(
    const NativeString &requestId, const NativeJsonDom::Object &params)
{
    const NativeString operation = nativeText(
        NativeJsonDom::stringValue(params, "operation")).toLower();
    const std::map<NativeString, NativeString> commands{
        {NativeString("mkdir"), NativeString("--mkdir")},
        {NativeString("mkdir-p"), NativeString("--mkdir-p")},
        {NativeString("touch"), NativeString("--touch")},
        {NativeString("rename"), NativeString("--rename")},
        {NativeString("copy"), NativeString("--copy")},
        {NativeString("chmod"), NativeString("--chmod")},
        {NativeString("remove"), NativeString("--remove")}
    };
    if (commands.find(operation) == commands.end()) {
        sendError(requestId, NativeString("不支持的 SFTP 文件操作"));
        return;
    }

    const int profileIndex =
        NativeJsonDom::integerValue(params, "index", -1);
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0
        || profileIndex >= static_cast<int>(records.size())) {
        sendError(requestId, NativeString("服务器配置不存在"));
        return;
    }
    const ServerRecord &record =
        records.at(static_cast<std::size_t>(profileIndex));
    if (record.connectionType != "ssh") {
        sendError(requestId, NativeString("SFTP 仅适用于 SSH 服务器"));
        return;
    }

    NativeString path = cleanRemotePath(nativeText(
        NativeJsonDom::stringValue(params, "path")).trimmed());
    NativeString targetPath =
        cleanRemotePath(nativeText(NativeJsonDom::stringValue(
            params, "targetPath")).trimmed());
    if (path.isEmpty() || path == NativeString(".") || !path.startsWith(static_cast<char>('/'))) {
        sendError(requestId, NativeString("远程路径无效"));
        return;
    }
    if ((operation == NativeString("remove") || operation == NativeString("rename")
         || operation == NativeString("copy") || operation == NativeString("chmod"))
        && path == NativeString("/")) {
        sendError(requestId, NativeString("禁止修改远程根目录"));
        return;
    }
    if ((operation == NativeString("rename") || operation == NativeString("copy"))
        && (targetPath.isEmpty() || targetPath == NativeString(".")
            || !targetPath.startsWith(static_cast<char>('/')) || targetPath == NativeString("/"))) {
        sendError(requestId, NativeString("目标路径无效"));
        return;
    }
    if (operation == NativeString("chmod")) {
        bool modeOk = false;
        const unsigned int mode = targetPath.toUInt(&modeOk, 8);
        if (!modeOk || mode > 07777) {
            sendError(requestId, NativeString("权限格式无效，请输入 0000 到 7777 的八进制权限"));
            return;
        }
    }
    if (operation == NativeString("copy") && targetPath == path) {
        sendError(requestId, NativeString("复制目标不能与源路径相同"));
        return;
    }

    const NativeString address = nativeText(record.address);
    const NativeString port = nativeText(record.port);
    const NativeString configuredPassword = nativeText(record.password);
    const NativeString password = configuredPassword.isEmpty()
        ? readStoredPassword(address) : configuredPassword;
    const NativeString keyPath = nativeText(record.keyPath);
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString configuredProxyPassword = nativeText(record.proxyPassword);
    const NativeString proxyPassword = configuredProxyPassword.isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : configuredProxyPassword;
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    if (password.isEmpty() && keyPath.isEmpty()) {
        sendError(requestId, NativeString("未找到 SFTP 凭据，请先连接一次该 SSH 服务器"));
        return;
    }

    const NativeString workerPath =
        applicationDirectory() + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath))) {
        sendError(requestId, NativeString("找不到 MasterTermSftpWorker.exe"));
        return;
    }

    std::vector<NativeString> arguments{commands.at(operation), address, port, path};
    if (operation == NativeString("rename") || operation == NativeString("copy")
        || operation == NativeString("chmod"))
        arguments.push_back(targetPath);
    arguments.push_back(keyPath);
    auto *process = new NativeProcess();
    if (!process->start(
            toWide(workerPath), toWideArguments(arguments),
            sftpEnvironment(
                password, proxyJump, proxyPassword, proxyKeyPath))) {
        const NativeString message = NativeString("无法启动 SFTP 工作进程：")
            + nativeProcessError(process);
        delete process;
        sendError(requestId, message);
        return;
    }
    m_sftpProcesses.insert(process);
    m_sftpOperationRequests[process] =
        {requestId, operation, path, targetPath};
}

void WebViewBackend::finishSftpOperation(NativeProcess *process, const NativeString &requestId,
                                         const NativeString &operation, const NativeString &path,
                                         const NativeString &targetPath, int exitCode)
{
    if (m_sftpProcesses.erase(process) == 0)
        return;
    m_sftpOperationRequests.erase(process);
    m_sftpStandardOutputs.erase(process);
    std::string standardErrorBytes = takeMapped(m_sftpStandardErrors, process);
    standardErrorBytes += process->takeStandardError();
    const NativeString standardError = utf8Text(standardErrorBytes).trimmed();
    const NativeString processError = process->errorMessage().empty()
        ? NativeString() : NativeString::fromStdWString(process->errorMessage());
    delete process;
    if (exitCode != 0) {
        const NativeString message = !standardError.isEmpty()
            ? standardError.section(static_cast<char>('\n'), -1)
            : (processError.isEmpty() ? NativeString("SFTP 文件操作失败") : processError);
        sendError(requestId, message);
        return;
    }
    NativeJsonDom::Object result;
    result.values.emplace("operation", utf8Text(operation));
    result.values.emplace("path", utf8Text(path));
    result.values.emplace("targetPath", utf8Text(targetPath));
    sendNativeResult(requestId, NativeJsonDom::Value(result));
}

void WebViewBackend::startSftpTransfer(
    const NativeString &requestId, const NativeJsonDom::Object &params)
{
    const NativeString operation = nativeText(
        NativeJsonDom::stringValue(params, "operation")).toLower();
    const bool upload = operation == NativeString("upload")
        || operation == NativeString("upload-tree")
        || operation == NativeString("upload-stream");
    const bool directoryTransfer = operation == NativeString("upload-tree")
        || operation == NativeString("download-dir");
    const bool streamUpload = operation == NativeString("upload-stream");
    if (!upload && operation != NativeString("download")
        && operation != NativeString("download-dir")) {
        sendError(requestId, NativeString("不支持的 SFTP 传输类型"));
        return;
    }

    const int profileIndex =
        NativeJsonDom::integerValue(params, "index", -1);
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0
        || profileIndex >= static_cast<int>(records.size())) {
        sendError(requestId, NativeString("服务器配置不存在"));
        return;
    }
    const ServerRecord &record =
        records.at(static_cast<std::size_t>(profileIndex));
    if (record.connectionType != "ssh") {
        sendError(requestId, NativeString("SFTP 仅适用于 SSH 服务器"));
        return;
    }

    const NativeString address = nativeText(record.address);
    const NativeString port = nativeText(record.port);
    const NativeString configuredPassword = nativeText(record.password);
    const NativeString password = configuredPassword.isEmpty()
        ? readStoredPassword(address) : configuredPassword;
    const NativeString keyPath = nativeText(record.keyPath);
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString configuredProxyPassword = nativeText(record.proxyPassword);
    const NativeString proxyPassword = configuredProxyPassword.isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : configuredProxyPassword;
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    if (password.isEmpty() && keyPath.isEmpty()) {
        sendError(requestId, NativeString("未找到 SFTP 凭据，请先连接一次该 SSH 服务器"));
        return;
    }

    NativeString remotePath = nativeText(
        NativeJsonDom::stringValue(params, "remotePath")).trimmed();
    if (remotePath.isEmpty()) {
        sendError(requestId, NativeString("远程路径不能为空"));
        return;
    }
    if (!remotePath.startsWith(static_cast<char>('/')))
        remotePath.prepend(static_cast<char>('/'));
    remotePath = cleanRemotePath(remotePath);

    NativeString localPath = nativeText(
        NativeJsonDom::stringValue(params, "localPath")).trimmed();
    if (streamUpload) {
        NativeString remoteName = fileNameText(
            nativeText(NativeJsonDom::stringValue(
                params, "remoteName"))).trimmed();
        if (remoteName.isEmpty() || remoteName == NativeString(".")
            || remoteName == NativeString("..")) {
            sendError(requestId, NativeString("远程文件名称无效"));
            return;
        }
        remotePath = remoteChildPath(remotePath, remoteName);
    } else if (upload) {
        if (localPath.isEmpty() && directoryTransfer)
            localPath = chooseWindowsPath(
                NativeString("选择要上传的文件夹"), NativeString(), true, false);
        else if (localPath.isEmpty())
            localPath = chooseWindowsPath(
                NativeString("选择要上传的文件"), NativeString(), false, false);
        if (localPath.isEmpty()) {
            NativeJsonDom::Object result;
            result.values.emplace("cancelled", true);
            sendNativeResult(requestId, NativeJsonDom::Value(result));
            return;
        }
        const std::filesystem::path localFsPath = nativePath(localPath);
        if (!std::filesystem::exists(localFsPath)
            || (directoryTransfer ? !std::filesystem::is_directory(localFsPath)
                                   : !std::filesystem::is_regular_file(localFsPath))) {
            sendError(requestId, directoryTransfer
                ? NativeString("本地文件夹不存在或不可读")
                : NativeString("本地文件不存在或不可读"));
            return;
        }
        if (!directoryTransfer) {
            NativeString remoteName = fileNameText(
                nativeText(NativeJsonDom::stringValue(
                    params, "remoteName"))).trimmed();
            if (remoteName.isEmpty())
                remoteName = pathText(localFsPath.filename());
            if (remoteName.isEmpty() || remoteName == NativeString(".")
                || remoteName == NativeString("..")) {
                sendError(requestId, NativeString("远程文件名称无效"));
                return;
            }
            // The WebView sends the current remote directory. The worker
            // expects the complete destination file path for a single file.
            remotePath = remoteChildPath(remotePath, remoteName);
        }
    } else {
        const NativeString defaultName = fileNameText(remotePath);
        if (localPath.isEmpty() && directoryTransfer)
            localPath = chooseWindowsPath(
                NativeString("选择文件夹保存位置"), NativeString(), true, false);
        else if (localPath.isEmpty())
            localPath = chooseWindowsPath(
                NativeString("保存远程文件"), defaultName, false, true);
        if (localPath.isEmpty()) {
            NativeJsonDom::Object result;
            result.values.emplace("cancelled", true);
            sendNativeResult(requestId, NativeJsonDom::Value(result));
            return;
        }
    }

    const NativeString workerPath = applicationDirectory()
        + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath))) {
        sendError(requestId, NativeString("找不到 MasterTermSftpWorker.exe"));
        return;
    }

    auto *process = new NativeProcess();
    const NativeString transferId = NativeString("sftp-%1").arg(m_nextTransferNumber++);
    const NativeString fileName = upload && !directoryTransfer
        ? fileNameText(remotePath)
        : fileNameText(upload ? localPath : remotePath);
    std::vector<NativeString> arguments;
    if (streamUpload) {
        arguments = {NativeString("--upload-stream"), address, port,
                     remotePath, keyPath,
                     NativeString::number(std::max<std::int64_t>(
                         0, static_cast<std::int64_t>(
                             NativeJsonDom::numberValue(params, "size"))))};
    } else if (operation == NativeString("upload-tree")) {
        arguments = {NativeString("--upload-tree"), address, port,
                     remotePath, keyPath, localPath};
    } else {
        const NativeString command = operation == NativeString("download-dir")
            ? NativeString("--download-dir")
            : (upload ? NativeString("--upload") : NativeString("--download"));
        arguments = {command, address, port,
                     upload ? localPath : remotePath,
                     upload ? remotePath : localPath, keyPath};
    }
    const std::int64_t resumeOffset = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(
            NativeJsonDom::numberValue(params, "resumeOffset", 0)));
    if (!streamUpload && !directoryTransfer && resumeOffset > 0)
        arguments.push_back(NativeString::number(resumeOffset));
    auto environment = sftpEnvironment(
        password, proxyJump, proxyPassword, proxyKeyPath,
        nativeText(NativeJsonDom::stringValue(
            params, "conflict", "overwrite")));
    if (NativeJsonDom::booleanValue(params, "syncMode", false))
        environment[L"MASTERSSH_SYNC_MODE"] = L"1";
    if (NativeJsonDom::booleanValue(params, "resumeDirectory", false))
        environment[L"MASTERSSH_RESUME_TREE"] = L"1";
    if (NativeJsonDom::booleanValue(params, "verifyChecksum", false))
        environment[L"MASTERSSH_VERIFY_CHECKSUM"] = L"1";
    if (!process->start(
            toWide(workerPath), toWideArguments(arguments),
            environment, true)) {
        const NativeString message = NativeString("无法启动 SFTP 传输进程：")
            + nativeProcessError(process);
        delete process;
        sendError(requestId, message);
        return;
    }
    m_sftpProcesses.insert(process);
    m_sftpTransferIds[process] = transferId;
    if (NativeJsonDom::booleanValue(params, "temporary") || streamUpload)
        m_sftpTransferNames[process] = fileName;
    m_sftpTransfers[transferId] = process;
    if (streamUpload) {
        m_sftpStreamTransfers.insert(process);
        m_sftpStreamTotals[process] = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(
                NativeJsonDom::numberValue(params, "size")));
        m_sftpStreamQueued[process] = 0;
    }
    if (NativeJsonDom::booleanValue(params, "temporary"))
        m_sftpTemporaryFiles[process] = localPath;
    NativeJsonDom::Object result;
    result.values.emplace("transferId", utf8Text(transferId));
    result.values.emplace("operation", utf8Text(operation));
    result.values.emplace("name", utf8Text(fileName));
    result.values.emplace("localPath", utf8Text(localPath));
    result.values.emplace("remotePath", utf8Text(remotePath));
    sendNativeResult(requestId, NativeJsonDom::Value(result));
}

void WebViewBackend::processSftpTransferOutput(NativeProcess *process)
{
    const auto transferIt = m_sftpTransferIds.find(process);
    if (transferIt == m_sftpTransferIds.end())
        return;
    const NativeString transferId = transferIt->second;
    std::string &buffer = m_sftpTransferBuffers[process];
    buffer += process->takeStandardOutput();
    while (true) {
        const std::size_t newline = buffer.find('\n');
        if (newline == std::string::npos)
            break;
        const std::string rawLine = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        const auto progress = parseSftpTransferProgressLine(rawLine);
        if (!progress)
            continue;
        NativeString progressName = decodeBase64Text(progress->encodedName);
        const auto nameIt = m_sftpTransferNames.find(process);
        const NativeString displayName = nameIt == m_sftpTransferNames.end()
            ? NativeString() : nameIt->second;
        if (!displayName.isEmpty())
            progressName = displayName;
        NativeJsonDom::Object payload;
        payload.values.emplace("transferId", utf8Text(transferId));
        payload.values.emplace("state", "progress");
        payload.values.emplace(
            "done",
            static_cast<double>(progress->done));
        payload.values.emplace(
            "total",
            static_cast<double>(progress->total));
        payload.values.emplace("name", utf8Text(progressName));
        if (progress->hasFileProgress) {
            payload.values.emplace(
                "completedFiles",
                static_cast<double>(progress->completedFiles));
            payload.values.emplace(
                "totalFiles",
                static_cast<double>(progress->totalFiles));
        }
        if (progress->hasFileBytes) {
            payload.values.emplace(
                "fileDone",
                static_cast<double>(progress->fileDone));
            payload.values.emplace(
                "fileTotal",
                static_cast<double>(progress->fileTotal));
        }
        sendNativeEvent("sftp.transfer", "", payload);
        if (progress->total > 0 && progress->done >= progress->total)
            m_sftpTransferRemoteComplete.insert(process);
    }
}

bool WebViewBackend::detachSftpTransfer(NativeProcess *process, NativeString &transferId,
                                        NativeString &temporaryPath)
{
    const auto transferIt = m_sftpTransferIds.find(process);
    if (transferIt == m_sftpTransferIds.end())
        return false;

    transferId = transferIt->second;
    m_sftpTransferIds.erase(transferIt);
    m_sftpTransferNames.erase(process);
    m_sftpStreamTotals.erase(process);
    m_sftpStreamQueued.erase(process);
    m_sftpTransferRemoteComplete.erase(process);
    m_sftpTransferBuffers.erase(process);
    m_sftpStandardOutputs.erase(process);
    m_sftpStandardErrors.erase(process);
    m_sftpTransfers.erase(transferId);
    m_sftpStreamTransfers.erase(process);
    m_sftpProcesses.erase(process);
    temporaryPath = takeMapped(m_sftpTemporaryFiles, process);
    return true;
}

void WebViewBackend::finishSftpTransfer(NativeProcess *process, int exitCode)
{
    processSftpTransferOutput(process);
    std::string errorBytes = takeMapped(m_sftpStandardErrors, process);
    errorBytes += process->takeStandardError();
    NativeString transferId;
    NativeString temporaryPath;
    if (!detachSftpTransfer(process, transferId, temporaryPath))
        return;
    const NativeString error = utf8Text(errorBytes).trimmed();
    delete process;
    if (!temporaryPath.isEmpty())
        std::filesystem::remove(nativePath(temporaryPath));
    NativeJsonDom::Object payload;
    payload.values.emplace("transferId", utf8Text(transferId));
    if (exitCode == 0) {
        payload.values.emplace("state", "completed");
    } else {
        payload.values.emplace("state", "error");
        payload.values.emplace(
            "message",
            utf8Text(error.isEmpty()
                ? NativeString("SFTP 传输失败") : error));
    }
    sendNativeEvent("sftp.transfer", "", payload);
}

void WebViewBackend::failSftpTransfer(NativeProcess *process, const NativeString &message)
{
    NativeString transferId;
    NativeString temporaryPath;
    if (!detachSftpTransfer(process, transferId, temporaryPath))
        return;
    process->terminate();
    delete process;
    if (!temporaryPath.isEmpty())
        std::filesystem::remove(nativePath(temporaryPath));
    NativeJsonDom::Object payload;
    payload.values.emplace("transferId", utf8Text(transferId));
    payload.values.emplace("state", "error");
    payload.values.emplace("message", utf8Text(message));
    sendNativeEvent("sftp.transfer", "", payload);
}

bool WebViewBackend::cancelSftpTransfer(const NativeString &transferId)
{
    const auto transferIt = m_sftpTransfers.find(transferId);
    if (transferIt == m_sftpTransfers.end())
        return false;
    NativeProcess *process = transferIt->second;
    NativeString detachedTransferId;
    NativeString temporaryPath;
    if (!detachSftpTransfer(process, detachedTransferId, temporaryPath))
        return false;
    process->terminate();
    delete process;
    if (!temporaryPath.isEmpty())
        std::filesystem::remove(nativePath(temporaryPath));
    NativeJsonDom::Object payload;
    payload.values.emplace(
        "transferId", utf8Text(detachedTransferId));
    payload.values.emplace("state", "cancelled");
    sendNativeEvent("sftp.transfer", "", payload);
    return true;
}

bool WebViewBackend::setSftpTransferPaused(
    const NativeString &transferId, bool paused)
{
    const auto transferIt = m_sftpTransfers.find(transferId);
    if (transferIt == m_sftpTransfers.end())
        return false;
    NativeProcess *process = transferIt->second;
    const bool streamUpload =
        m_sftpStreamTransfers.find(process) != m_sftpStreamTransfers.end();
    // Stream uploads carry their data over the same stdin pipe; the worker
    // blocks waiting for the next chunk, so the frontend halting its chunk
    // feed is the pause mechanism itself.
    if (!streamUpload && !process->write(
            paused ? "pause\n" : "resume\n"))
        return false;
    NativeJsonDom::Object payload;
    payload.values.emplace("transferId", utf8Text(transferId));
    payload.values.emplace("state", paused ? "paused" : "active");
    sendNativeEvent("sftp.transfer", "", payload);
    return true;
}

bool WebViewBackend::writeSftpStreamChunk(const NativeJsonDom::Object &params,
                                          std::int64_t &bufferedBytes, NativeString &error)
{
    const NativeString transferId = nativeText(
        NativeJsonDom::stringValue(params, "transferId"));
    const auto transferIt = m_sftpTransfers.find(transferId);
    NativeProcess *process = transferIt == m_sftpTransfers.end()
        ? nullptr : transferIt->second;
    if (!process || m_sftpStreamTransfers.find(process) == m_sftpStreamTransfers.end()) {
        error = NativeString("流式上传任务不存在");
        return false;
    }
    const std::string bytes = NativeBase64::decode(
        NativeJsonDom::stringValue(params, "data"));
    if (bytes.empty() && !NativeJsonDom::stringValue(params, "data").empty()) {
        error = NativeString("上传数据块无效");
        return false;
    }
    if (!process->write(bytes)) {
        error = NativeString("无法写入上传数据流");
        return false;
    }
    bufferedBytes = static_cast<std::int64_t>(process->bufferedInputBytes());
    const auto total = m_sftpStreamTotals.find(process);
    const auto queued = m_sftpStreamQueued.find(process);
    if (total != m_sftpStreamTotals.end() && queued != m_sftpStreamQueued.end()) {
        queued->second = std::min(total->second, queued->second
            + static_cast<std::int64_t>(bytes.size()));
        const auto transferName = m_sftpTransferNames.find(process);
        NativeJsonDom::Object payload;
        payload.values.emplace("transferId", utf8Text(transferId));
        payload.values.emplace("state", "progress");
        payload.values.emplace("done", static_cast<double>(queued->second));
        payload.values.emplace("total", static_cast<double>(total->second));
        payload.values.emplace("name", utf8Text(transferName == m_sftpTransferNames.end()
            ? NativeString() : transferName->second));
        payload.values.emplace("queued", true);
        sendNativeEvent("sftp.transfer", "", payload);
    }
    return true;
}

bool WebViewBackend::finishSftpStream(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const NativeString transferId = nativeText(
        NativeJsonDom::stringValue(params, "transferId"));
    const auto transferIt = m_sftpTransfers.find(transferId);
    NativeProcess *process = transferIt == m_sftpTransfers.end()
        ? nullptr : transferIt->second;
    if (!process || m_sftpStreamTransfers.find(process) == m_sftpStreamTransfers.end()) {
        error = NativeString("流式上传任务不存在");
        return false;
    }
    process->closeInput();
    return true;
}

bool WebViewBackend::querySftpStream(const NativeJsonDom::Object &params,
                                     std::int64_t &bufferedBytes, NativeString &error) const
{
    const NativeString transferId = nativeText(
        NativeJsonDom::stringValue(params, "transferId"));
    const auto transferIt = m_sftpTransfers.find(transferId);
    NativeProcess *process = transferIt == m_sftpTransfers.end()
        ? nullptr : transferIt->second;
    if (!process || m_sftpStreamTransfers.find(process) == m_sftpStreamTransfers.end()) {
        error = NativeString("流式上传任务不存在");
        return false;
    }
    bufferedBytes = static_cast<std::int64_t>(process->bufferedInputBytes());
    return true;
}

void WebViewBackend::startRemoteMonitor(const NativeString &sessionId, int profileIndex)
{
    if (sessionId.isEmpty() || !sshSession(sessionId)
        || m_remoteMonitors.find(sessionId) != m_remoteMonitors.end())
        return;
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0 || profileIndex >= static_cast<int>(records.size()))
        return;
    const ServerRecord &record = records.at(static_cast<std::size_t>(profileIndex));
    if (nativeText(record.connectionType) != NativeString("ssh"))
        return;
    const NativeString address = nativeText(record.address);
    const NativeString port = nativeText(record.port);
    const NativeString password = nativeText(record.password).isEmpty()
        ? readStoredPassword(address) : nativeText(record.password);
    const NativeString keyPath = nativeText(record.keyPath);
    if (address.isEmpty() || port.isEmpty() || (password.isEmpty() && keyPath.isEmpty()))
        return;
    const NativeString workerPath = applicationDirectory()
        + NativeString("/MasterTermSftpWorker.exe");
    if (!std::filesystem::exists(nativePath(workerPath)))
        return;
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword = nativeText(record.proxyPassword).isEmpty()
        ? readStoredCredential(NativeString("ProxyJump"), proxyJump)
        : nativeText(record.proxyPassword);
    const NativeString proxyKeyPath = nativeText(record.proxyKeyPath);
    auto process = std::make_unique<NativeProcess>();
    if (!process->start(toWide(workerPath),
            toWideArguments({NativeString("--monitor"), address, port,
                             NativeString("/"), keyPath}),
            sftpEnvironment(password, proxyJump, proxyPassword, proxyKeyPath))) {
        return;
    }
    NativeProcess *rawProcess = process.get();
    try {
        m_sftpProcesses.insert(rawProcess);
        m_remoteMonitorRequests.emplace(
            rawProcess, RemoteMonitorRequest{sessionId, profileIndex});
        m_remoteMonitors.emplace(sessionId, rawProcess);
        process.release();
    } catch (const std::exception &error) {
        m_remoteMonitors.erase(sessionId);
        m_remoteMonitorRequests.erase(rawProcess);
        m_sftpProcesses.erase(rawProcess);
        DiagnosticLog::write("remote-monitor-start-failed", error.what());
    } catch (...) {
        m_remoteMonitors.erase(sessionId);
        m_remoteMonitorRequests.erase(rawProcess);
        m_sftpProcesses.erase(rawProcess);
        DiagnosticLog::write(
            "remote-monitor-start-failed", "unknown exception");
    }
}

void WebViewBackend::stopRemoteMonitor(const NativeString &sessionId)
{
    const auto monitor = m_remoteMonitors.find(sessionId);
    if (monitor == m_remoteMonitors.end())
        return;
    NativeProcess *process = monitor->second;
    m_remoteMonitors.erase(monitor);
    m_remoteMonitorRequests.erase(process);
    m_remoteMonitorBuffers.erase(process);
    m_sftpStandardOutputs.erase(process);
    m_sftpStandardErrors.erase(process);
    m_sftpProcesses.erase(process);
    delete process;
}

void WebViewBackend::processRemoteMonitorOutput(NativeProcess *process)
{
    const auto request = m_remoteMonitorRequests.find(process);
    if (request == m_remoteMonitorRequests.end())
        return;
    std::string &buffer = m_remoteMonitorBuffers[process];
    buffer += process->takeStandardOutput();
    while (true) {
        const std::size_t separator = buffer.find("__MASTERTERM_SAMPLE_END__");
        if (separator == std::string::npos)
            return;
        const std::string sample = buffer.substr(0, separator);
        buffer.erase(0, separator + std::string("__MASTERTERM_SAMPLE_END__").size());
        if (!buffer.empty() && buffer.front() == '\r') buffer.erase(0, 1);
        if (!buffer.empty() && buffer.front() == '\n') buffer.erase(0, 1);
        if (!sshSession(request->second.sessionId))
            continue;
        const NativeMetrics::Sample metrics = NativeMetrics::parseSample(sample);
        if (!metrics.usable())
            continue;
        NativeJsonDom::Object payload;
        payload.values.emplace("state", "available");
        payload.values.emplace("cpuTotal", static_cast<double>(metrics.cpuTotal));
        payload.values.emplace("cpuIdle", static_cast<double>(metrics.cpuIdle));
        payload.values.emplace("memoryTotal", static_cast<double>(metrics.memoryTotal));
        payload.values.emplace("memoryAvailable", static_cast<double>(metrics.memoryAvailable));
        payload.values.emplace("netRx", static_cast<double>(metrics.netRx));
        payload.values.emplace("netTx", static_cast<double>(metrics.netTx));
        payload.values.emplace("uptime", static_cast<double>(metrics.uptime));
        payload.values.emplace("diskMount", metrics.diskMount);
        payload.values.emplace("diskTotal", static_cast<double>(metrics.diskTotal));
        payload.values.emplace("diskUsed", static_cast<double>(metrics.diskUsed));
        payload.values.emplace("diskAvailable", static_cast<double>(metrics.diskAvailable));
        NativeJsonDom::Array diskPartitions;
        for (const NativeMetrics::DiskPartition &partition : metrics.partitions) {
            NativeJsonDom::Object object;
            object.values.emplace("mount", partition.mount);
            object.values.emplace("total", static_cast<double>(partition.total));
            object.values.emplace("used", static_cast<double>(partition.used));
            object.values.emplace("available", static_cast<double>(partition.available));
            diskPartitions.values.emplace_back(std::move(object));
        }
        payload.values.emplace(
            "diskPartitions", NativeJsonDom::Value(std::move(diskPartitions)));
        sendNativeEvent("session.metrics", utf8Text(request->second.sessionId), payload);
        startRemoteLatencyProbe(request->second.sessionId, request->second.profileIndex);
    }
}

void WebViewBackend::finishRemoteMonitor(NativeProcess *process, int exitCode)
{
    const auto request = m_remoteMonitorRequests.find(process);
    if (request == m_remoteMonitorRequests.end())
        return;
    const NativeString sessionId = request->second.sessionId;
    const NativeString error = utf8Text(takeMapped(m_sftpStandardErrors, process)).trimmed();
    m_remoteMonitorRequests.erase(request);
    m_remoteMonitorBuffers.erase(process);
    m_sftpStandardOutputs.erase(process);
    m_sftpProcesses.erase(process);
    const auto monitor = m_remoteMonitors.find(sessionId);
    if (monitor != m_remoteMonitors.end() && monitor->second == process)
        m_remoteMonitors.erase(monitor);
    delete process;
    if (!sshSession(sessionId) || exitCode == ERROR_CANCELLED)
        return;
    NativeJsonDom::Object payload;
    payload.values.emplace("state", "unavailable");
    if (!error.isEmpty()) payload.values.emplace("message", utf8Text(error));
    sendNativeEvent("session.metrics", utf8Text(sessionId), payload);
}

void WebViewBackend::startRemoteLatencyProbe(const NativeString &sessionId, int profileIndex)
{
    const NativeString unavailableSessionId = sessionId;
    const auto reportUnavailable = [this, unavailableSessionId] {
        NativeJsonDom::Object payload;
        payload.values.emplace("latency", -1.0);
        sendNativeEvent(
            "session.latency", utf8Text(unavailableSessionId), payload);
    };
    if (!sshSession(sessionId)
        || m_remoteLatencyProbes.find(sessionId) != m_remoteLatencyProbes.end())
        return;
    const auto now = std::chrono::steady_clock::now();
    const auto lastProbe = m_remoteLatencyProbeTimes.find(sessionId);
    if (lastProbe != m_remoteLatencyProbeTimes.end()
        && now - lastProbe->second < std::chrono::minutes(1))
        return;
    // Throttle failed probes as well; a blocked ICMP route must not make the
    // header update every monitor cycle.
    m_remoteLatencyProbeTimes[sessionId] = now;
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0 || profileIndex >= static_cast<int>(records.size()))
        return;
    const ServerRecord &record = records.at(static_cast<std::size_t>(profileIndex));
    if (nativeText(record.connectionType) != NativeString("ssh"))
        return;
    NativeString host = nativeText(record.address);
    const int at = host.lastIndexOf('@');
    if (at >= 0) host = host.mid(at + 1);

    bool isProxyJump = false;
    NativeString probeHost = host;
    if (!record.proxyJump.empty()) {
        NativeString jumpSpec = nativeText(record.proxyJump).trimmed();
        const int jumpAt = jumpSpec.lastIndexOf('@');
        NativeString jumpHostPort = jumpAt >= 0 ? jumpSpec.mid(jumpAt + 1) : jumpSpec;
        jumpHostPort = jumpHostPort.trimmed();
        if (jumpHostPort.startsWith('[') && jumpHostPort.contains(']')) {
            const int closeBracket = jumpHostPort.indexOf(']');
            probeHost = jumpHostPort.mid(1, closeBracket - 1).trimmed();
            isProxyJump = !probeHost.isEmpty();
        } else {
            const int jumpColon = jumpHostPort.lastIndexOf(':');
            probeHost = (jumpColon > 0 ? jumpHostPort.left(jumpColon) : jumpHostPort).trimmed();
            isProxyJump = !probeHost.isEmpty();
        }
    }
    if (probeHost.isEmpty()) {
        reportUnavailable();
        return;
    }
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT directoryLength = GetSystemDirectoryW(
        systemDirectory, MAX_PATH);
    const std::wstring pingPath = directoryLength > 0
        && directoryLength < MAX_PATH
        ? std::wstring(systemDirectory, directoryLength) + L"\\ping.exe"
        : std::wstring(L"ping.exe");
    auto process = std::make_unique<NativeProcess>();
    if (!process->start(pingPath,
            {L"-n", L"1", L"-w", L"1000", probeHost.toStdWString()}, {})) {
        reportUnavailable();
        return;
    }
    NativeProcess *rawProcess = process.get();
    try {
        m_sftpProcesses.insert(rawProcess);
        m_remoteLatencyRequests.emplace(
            rawProcess, RemoteLatencyRequest{sessionId, isProxyJump, probeHost});
        m_remoteLatencyProbes.emplace(sessionId, rawProcess);
        process.release();
    } catch (const std::exception &error) {
        m_remoteLatencyProbes.erase(sessionId);
        m_remoteLatencyRequests.erase(rawProcess);
        m_sftpProcesses.erase(rawProcess);
        DiagnosticLog::write(
            "latency-probe-start-failed", error.what());
        reportUnavailable();
    } catch (...) {
        m_remoteLatencyProbes.erase(sessionId);
        m_remoteLatencyRequests.erase(rawProcess);
        m_sftpProcesses.erase(rawProcess);
        DiagnosticLog::write(
            "latency-probe-start-failed", "unknown exception");
        reportUnavailable();
    }
}

void WebViewBackend::stopRemoteLatencyProbe(const NativeString &sessionId)
{
    m_remoteLatencyProbeTimes.erase(sessionId);
    const auto latency = m_remoteLatencyProbes.find(sessionId);
    if (latency == m_remoteLatencyProbes.end())
        return;
    NativeProcess *process = latency->second;
    m_remoteLatencyProbes.erase(latency);
    m_remoteLatencyRequests.erase(process);
    m_sftpStandardOutputs.erase(process);
    m_sftpStandardErrors.erase(process);
    m_sftpProcesses.erase(process);
    delete process;
}

void WebViewBackend::finishRemoteLatencyProbe(NativeProcess *process, int exitCode)
{
    const auto request = m_remoteLatencyRequests.find(process);
    if (request == m_remoteLatencyRequests.end())
        return;
    const NativeString sessionId = request->second.sessionId;
    const bool isProxyJump = request->second.isProxyJump;
    const NativeString probeHost = request->second.probeHost;
    std::string output = takeMapped(m_sftpStandardOutputs, process);
    output += process->takeStandardOutput();
    m_sftpStandardErrors.erase(process);
    m_remoteLatencyRequests.erase(request);
    m_sftpProcesses.erase(process);
    const auto latency = m_remoteLatencyProbes.find(sessionId);
    if (latency != m_remoteLatencyProbes.end() && latency->second == process)
        m_remoteLatencyProbes.erase(latency);
    delete process;
    if (!sshSession(sessionId)) return;
    double milliseconds = -1;
    if (exitCode == 0) {
        // ping.exe is localized.  Rather than looking for the English word
        // "time", locate the numeric value directly before the universal
        // "ms" suffix (works for both "time=12ms" and "时间<1ms").
        std::size_t marker = output.find("ms");
        if (marker != std::string::npos) {
            std::size_t end = marker;
            while (end > 0 && std::isspace(static_cast<unsigned char>(output[end - 1])))
                --end;
            std::size_t begin = end;
            while (begin > 0 && (std::isdigit(static_cast<unsigned char>(output[begin - 1]))
                                 || output[begin - 1] == '.'))
                --begin;
            if (begin < end) {
                std::size_t comparison = begin;
                while (comparison > 0
                       && std::isspace(static_cast<unsigned char>(output[comparison - 1])))
                    --comparison;
                milliseconds = comparison > 0 && output[comparison - 1] == '<'
                    ? 1.0 : std::strtod(output.c_str() + begin, nullptr);
            }
        }
    }
    NativeJsonDom::Object payload;
    payload.values.emplace("latency", milliseconds);
    if (isProxyJump) {
        payload.values.emplace("isProxyJump", true);
        payload.values.emplace("proxyHost", utf8Text(probeHost));
    }
    sendNativeEvent("session.latency", utf8Text(sessionId), payload);
}

SshSession *WebViewBackend::sshSession(const NativeString &sessionId) const
{
    const auto it = m_sshSessions.find(sessionId);
    return it == m_sshSessions.end() ? nullptr : it->second;
}

SerialSession *WebViewBackend::serialSession(const NativeString &sessionId) const
{
    const auto it = m_serialSessions.find(sessionId);
    return it == m_serialSessions.end() ? nullptr : it->second;
}

LocalShellSession *WebViewBackend::localSession(
    const NativeString &sessionId) const
{
    const auto it = m_localSessions.find(sessionId);
    return it == m_localSessions.end() ? nullptr : it->second;
}

bool WebViewBackend::removeSession(const NativeString &sessionId)
{
    stopRemoteMonitor(sessionId);
    stopRemoteLatencyProbe(sessionId);
    const std::string sessionIdText = sessionId.toStdString();
    std::string sessionKind;
    if (m_sshSessions.find(sessionId) != m_sshSessions.end())
        sessionKind = "ssh";
    else if (m_serialSessions.find(sessionId) != m_serialSessions.end())
        sessionKind = "serial";
    else if (m_localSessions.find(sessionId) != m_localSessions.end())
        sessionKind = "local";
    const auto tunnels = m_tunnels.find(sessionIdText);
    if (tunnels != m_tunnels.end()) {
        for (const auto &tunnel : tunnels->second)
            tunnel->stop();
        m_tunnels.erase(tunnels);
    }
    m_sessionProfiles.erase(sessionId);
    closeSessionLog(sessionId);
    if (!sessionKind.empty())
        DiagnosticLog::write(
            "session-remove",
            sessionKind + " " + sessionIdText);
    const auto sshIt = m_sshSessions.find(sessionId);
    if (sshIt != m_sshSessions.end()) {
        SshSession *ssh = sshIt->second;
        m_sshSessions.erase(sshIt);
        delete ssh;
        return true;
    }
    const auto serialIt = m_serialSessions.find(sessionId);
    if (serialIt != m_serialSessions.end()) {
        SerialSession *serial = serialIt->second;
        m_serialSessions.erase(serialIt);
        delete serial;
        return true;
    }
    const auto localIt = m_localSessions.find(sessionId);
    if (localIt != m_localSessions.end()) {
        LocalShellSession *local = localIt->second;
        m_localSessions.erase(localIt);
        delete local;
        return true;
    }
    return false;
}

NativeJsonDom::Object WebViewBackend::tunnelToJson(
    const SshTunnel &tunnel, const std::string &sessionId) const
{
    NativeJsonDom::Object object;
    object.values.emplace("tunnelId", tunnel.id());
    object.values.emplace("mode", tunnel.mode() == SshTunnel::Mode::Local
        ? "local" : "remote");
    const int reportedPort = tunnel.boundPort() > 0
        ? tunnel.boundPort() : tunnel.listenPort();
    object.values.emplace("listenPort", static_cast<double>(reportedPort));
    object.values.emplace("targetHost", tunnel.targetHost());
    object.values.emplace(
        "targetPort", static_cast<double>(tunnel.targetPort()));
    const char *state = "starting";
    switch (tunnel.state()) {
    case SshTunnel::State::Listening: state = "listening"; break;
    case SshTunnel::State::Failed: state = "failed"; break;
    case SshTunnel::State::Stopped: state = "stopped"; break;
    default: break;
    }
    object.values.emplace("state", state);
    if (!tunnel.errorString().empty())
        object.values.emplace("error", tunnel.errorString());
    const auto automatic = m_automaticTunnelIds.find(sessionId);
    if (automatic != m_automaticTunnelIds.end()
        && automatic->second.count(tunnel.id()) != 0)
        object.values.emplace("automatic", true);
    return object;
}

SshTunnel *WebViewBackend::findTunnel(
    const std::string &sessionId, const std::string &tunnelId) const
{
    const auto found = m_tunnels.find(sessionId);
    if (found == m_tunnels.end())
        return nullptr;
    for (const auto &tunnel : found->second) {
        if (tunnel->id() == tunnelId)
            return tunnel.get();
    }
    return nullptr;
}

NativeJsonDom::Value WebViewBackend::createTunnel(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const std::string sessionId = jsonText(params, "sessionId");
    if (sessionId.empty()) {
        error = NativeString("缺少会话 ID");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    SshSession *session = sshSession(NativeString(sessionId));
    if (!session || !session->isRunning()) {
        error = NativeString("SSH 会话未连接，无法创建端口转发");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    const std::string mode = jsonText(params, "mode");
    const bool local = mode == "local";
    if (!local && mode != "remote") {
        error = NativeString("转发模式无效");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    const int listenPort = jsonInteger(params, "listenPort", -1);
    const int targetPort = jsonInteger(params, "targetPort", -1);
    if (listenPort < 0 || listenPort > 65535
        || targetPort < 0 || targetPort > 65535) {
        error = NativeString("端口必须在 0-65535 之间");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    const std::string targetHost = jsonText(params, "targetHost");
    if (targetHost.empty()) {
        error = NativeString("缺少目标主机");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    const std::string listenHost = jsonText(
        params, "listenHost", local ? "127.0.0.1" : "localhost");
    auto tunnel = std::make_unique<SshTunnel>(
        local ? SshTunnel::Mode::Local : SshTunnel::Mode::Remote,
        listenHost, listenPort, targetHost, targetPort);
    const std::string tunnelId = tunnel->id();
    tunnel->start(session->sessionHandle(),
        [this, sessionId, tunnelId] {
            SshTunnel *current = findTunnel(sessionId, tunnelId);
            if (!current)
                return;
            sendNativeEvent(
                "tunnel.state", sessionId, tunnelToJson(*current, sessionId));
        });
    if (tunnel->state() == SshTunnel::State::Failed) {
        error = NativeString("无法创建端口转发：")
            + utf8Text(tunnel->errorString());
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    const NativeJsonDom::Object result = tunnelToJson(*tunnel, sessionId);
    if (jsonInteger(params, "automatic", 0) != 0)
        m_automaticTunnelIds[sessionId].insert(tunnelId);
    m_tunnels[sessionId].push_back(std::move(tunnel));
    if (jsonInteger(params, "saveConfig", 0) != 0) {
        const int profileIndex = jsonInteger(params, "profileIndex", -1);
        if (profileIndex >= 0) {
            NativeJsonDom::Object configParams;
            configParams.values.emplace(
                "profileIndex", static_cast<double>(profileIndex));
            configParams.values.emplace("mode", mode);
            configParams.values.emplace("listenHost", listenHost);
            configParams.values.emplace(
                "listenPort", static_cast<double>(listenPort));
            configParams.values.emplace("targetHost", targetHost);
            configParams.values.emplace(
                "targetPort", static_cast<double>(targetPort));
            NativeString configError;
            saveTunnelConfig(configParams, configError);
        }
    }
    return NativeJsonDom::Value(result);
}

NativeJsonDom::Value WebViewBackend::listTunnels(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    static_cast<void>(error);
    NativeJsonDom::Array result;
    const std::string sessionId = jsonText(params, "sessionId");
    const auto appendTunnel = [this, &result](
        const SshTunnel &tunnel, const std::string &session) {
        NativeJsonDom::Object object = tunnelToJson(tunnel, session);
        object.values.emplace("sessionId", session);
        result.values.emplace_back(std::move(object));
    };
    if (!sessionId.empty()) {
        const auto found = m_tunnels.find(sessionId);
        if (found != m_tunnels.end()) {
            for (const auto &tunnel : found->second)
                appendTunnel(*tunnel, sessionId);
        }
        return NativeJsonDom::Value(result);
    }
    for (const auto &entry : m_tunnels) {
        for (const auto &tunnel : entry.second)
            appendTunnel(*tunnel, entry.first);
    }
    return NativeJsonDom::Value(result);
}

bool WebViewBackend::stopTunnel(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const std::string sessionId = jsonText(params, "sessionId");
    const std::string tunnelId = jsonText(params, "tunnelId");
    const auto found = m_tunnels.find(sessionId);
    if (found == m_tunnels.end()) {
        error = NativeString("端口转发不存在");
        return false;
    }
    for (auto it = found->second.begin(); it != found->second.end(); ++it) {
        if ((*it)->id() == tunnelId) {
            (*it)->stop();
            found->second.erase(it);
            const auto automatic = m_automaticTunnelIds.find(sessionId);
            if (automatic != m_automaticTunnelIds.end())
                automatic->second.erase(tunnelId);
            return true;
        }
    }
    error = NativeString("端口转发不存在");
    return false;
}

bool WebViewBackend::saveTunnelConfig(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const int profileIndex = jsonInteger(params, "profileIndex", -1);
    std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0
        || profileIndex >= static_cast<int>(records.size())) {
        error = NativeString("连接不存在，无法保存转发配置");
        return false;
    }
    const std::string mode = jsonText(params, "mode", "local");
    if (mode != "local" && mode != "remote") {
        error = NativeString("转发模式无效");
        return false;
    }
    const int listenPort = jsonInteger(params, "listenPort", -1);
    const int targetPort = jsonInteger(params, "targetPort", -1);
    if (listenPort < 0 || listenPort > 65535
        || targetPort < 0 || targetPort > 65535) {
        error = NativeString("端口必须在 0-65535 之间");
        return false;
    }
    const std::string targetHost = jsonText(params, "targetHost");
    if (targetHost.empty()) {
        error = NativeString("缺少目标主机");
        return false;
    }
    ServerRecord::TunnelConfig config;
    config.mode = mode;
    config.listenHost = jsonText(
        params, "listenHost", mode == "local" ? "127.0.0.1" : "localhost");
    config.listenPort = listenPort;
    config.targetHost = targetHost;
    config.targetPort = targetPort;
    std::vector<ServerRecord::TunnelConfig> &tunnels =
        records[static_cast<std::size_t>(profileIndex)].tunnels;
    const auto sameConfig = [&config](const ServerRecord::TunnelConfig &item) {
        return item.mode == config.mode
            && item.listenHost == config.listenHost
            && item.listenPort == config.listenPort
            && item.targetHost == config.targetHost
            && item.targetPort == config.targetPort;
    };
    if (std::find_if(tunnels.begin(), tunnels.end(), sameConfig)
        == tunnels.end())
        tunnels.push_back(config);
    writeServerRecords(records);
    return true;
}

bool WebViewBackend::removeTunnelConfig(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const int profileIndex = jsonInteger(params, "profileIndex", -1);
    std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0
        || profileIndex >= static_cast<int>(records.size())) {
        error = NativeString("连接不存在");
        return false;
    }
    const std::string mode = jsonText(params, "mode");
    const std::string listenHost = jsonText(params, "listenHost");
    const int listenPort = jsonInteger(params, "listenPort", -1);
    const std::string targetHost = jsonText(params, "targetHost");
    const int targetPort = jsonInteger(params, "targetPort", -1);
    std::vector<ServerRecord::TunnelConfig> &tunnels =
        records[static_cast<std::size_t>(profileIndex)].tunnels;
    const auto matches = [&](const ServerRecord::TunnelConfig &item) {
        return item.mode == mode && item.listenHost == listenHost
            && item.listenPort == listenPort
            && item.targetHost == targetHost
            && item.targetPort == targetPort;
    };
    const auto before = tunnels.size();
    tunnels.erase(
        std::remove_if(tunnels.begin(), tunnels.end(), matches),
        tunnels.end());
    if (tunnels.size() == before) {
        error = NativeString("转发配置不存在");
        return false;
    }
    writeServerRecords(records);
    return true;
}

void WebViewBackend::startConfiguredTunnels(
    int profileIndex, const std::string &sessionId)
{
    const std::vector<ServerRecord> records = readServerRecords();
    if (profileIndex < 0
        || profileIndex >= static_cast<int>(records.size()))
        return;
    for (const ServerRecord::TunnelConfig &config :
         records[static_cast<std::size_t>(profileIndex)].tunnels) {
        NativeJsonDom::Object params;
        params.values.emplace("sessionId", sessionId);
        params.values.emplace("mode", config.mode);
        params.values.emplace("listenHost", config.listenHost);
        params.values.emplace(
            "listenPort", static_cast<double>(config.listenPort));
        params.values.emplace("targetHost", config.targetHost);
        params.values.emplace(
            "targetPort", static_cast<double>(config.targetPort));
        params.values.emplace("automatic", 1.0);
        NativeString tunnelError;
        createTunnel(params, tunnelError);
    }
}

std::string WebViewBackend::closeBehavior() const
{
    bool available = false;
    const NativeJsonDom::Object config = readNativeConfig(&available);
    const std::string behavior =
        NativeJsonDom::stringValue(config, "closeBehavior");
    return (behavior == "tray" || behavior == "exit") ? behavior : "ask";
}

bool WebViewBackend::requestCloseConfirmation()
{
    if (!m_sendHandler)
        return false;
    NativeJsonDom::Object payload;
    sendNativeEvent("app.close-request", "", payload);
    return true;
}

bool WebViewBackend::setCloseBehavior(const std::string &behavior)
{
    if (behavior != "ask" && behavior != "tray" && behavior != "exit")
        return false;
    bool available = false;
    NativeJsonDom::Object config = readNativeConfig(&available);
    config.values["closeBehavior"] = behavior;
    return writeNativeConfig(config);
}

void WebViewBackend::notifyRdpState(
    const std::string &sessionId, const std::string &state)
{
    NativeJsonDom::Object payload;
    payload.values.emplace("state", state);
    sendNativeEvent("rdp.state", sessionId, payload);
}

void WebViewBackend::notifyRdpQuality(
    const std::string &sessionId, const RdpQualitySnapshot &snapshot)
{
    NativeJsonDom::Object payload;
    payload.values.emplace("colorDepth", static_cast<double>(snapshot.colorDepth));
    payload.values.emplace("displayWidth", static_cast<double>(snapshot.displayWidth));
    payload.values.emplace("displayHeight", static_cast<double>(snapshot.displayHeight));
    payload.values.emplace("smartSizing", snapshot.smartSizing);
    payload.values.emplace("performanceFlags",
                           static_cast<double>(snapshot.performanceFlags));
    payload.values.emplace("networkConnectionType",
                           static_cast<double>(snapshot.networkConnectionType));
    payload.values.emplace("bandwidthDetection", snapshot.bandwidthDetection);
    payload.values.emplace("clientProtocolSpec",
                           static_cast<double>(snapshot.clientProtocolSpec));
    payload.values.emplace("fullFrameRefreshCount",
                           static_cast<double>(snapshot.fullFrameRefreshCount));
    payload.values.emplace("lastHresult",
                           static_cast<double>(snapshot.lastHresult));
    payload.values.emplace("lastHresultSucceeded",
                           snapshot.lastHresultSucceeded);
    payload.values.emplace("lastOperation", wideToUtf8(snapshot.lastOperation));
    payload.values.emplace("avcStatus", wideToUtf8(snapshot.avcStatus));
    sendNativeEvent("rdp.quality", sessionId, payload);
}

void WebViewBackend::notifyRdpFullscreen(
    const std::string &sessionId, bool enabled)
{
    NativeJsonDom::Object payload;
    payload.values.emplace("enabled", enabled);
    sendNativeEvent("rdp.fullscreen", sessionId, payload);
}

void WebViewBackend::notifyRdpFullscreenBar(
    const std::string &sessionId, bool visible)
{
    NativeJsonDom::Object payload;
    payload.values.emplace("visible", visible);
    sendNativeEvent("rdp.fullscreenBar", sessionId, payload);
}

void WebViewBackend::notifyRdpFullscreenNativeBar(
    const std::string &sessionId, bool visible, bool pinned)
{
    NativeJsonDom::Object payload;
    payload.values.emplace("visible", visible);
    payload.values.emplace("pinned", pinned);
    sendNativeEvent("rdp.fullscreenNativeBar", sessionId, payload);
}

void WebViewBackend::notifyRdpContextAction(
    const std::string &sessionId, const std::string &action)
{
    NativeJsonDom::Object payload;
    payload.values.emplace("action", action);
    sendNativeEvent("rdp.contextAction", sessionId, payload);
}

void WebViewBackend::startUpdateCheck()
{
    const std::function<void(const std::string &)> handler = m_sendHandler;
    if (!handler)
        return;
    // Detached worker thread: it only captures a copy of the send handler,
    // never "this", so it is safe even if the backend is destroyed first.
    std::thread([handler] {
        const std::string body = httpGetUtf8(
            L"gitee.com",
            L"/api/v5/repos/bigmasterwang/masterterm/releases/latest");
        std::string jsonError;
        NativeJsonDom::Value root;
        std::string latestTag;
        std::string releaseUrl;
        std::string releaseBody;
        const std::string jsonBody = stripUtf8Bom(body);
        if (NativeJsonDom::parse(jsonBody, root, &jsonError)
            && root.isObject()) {
            const NativeJsonDom::Object &object = root.object();
            const auto tag = object.values.find("tag_name");
            if (tag != object.values.end() && tag->second.isString())
                latestTag = tag->second.string();
            const auto url = object.values.find("html_url");
            if (url != object.values.end() && url->second.isString())
                releaseUrl = url->second.string();
            const auto releaseBodyValue = object.values.find("body");
            if (releaseBodyValue != object.values.end()
                && releaseBodyValue->second.isString()) {
                releaseBody = safeTruncateUtf8(releaseBodyValue->second.string(), 4000);
            }
        }
        const std::string currentVersion = MASTERTERM_VERSION;
        const bool hasUpdate =
            !parseVersionParts(latestTag).empty()
            && parseVersionParts(latestTag) > parseVersionParts(currentVersion);
        NativeJsonDom::Object payload;
        payload.values.emplace("currentVersion", currentVersion);
        payload.values.emplace("latestVersion", latestTag);
        payload.values.emplace("url", releaseUrl);
        payload.values.emplace("body", releaseBody);
        payload.values.emplace("hasUpdate", hasUpdate);
        NativeJsonDom::Object message;
        message.values.emplace("event", "update.info");
        message.values.emplace("sessionId", std::string());
        message.values.emplace("payload", payload);
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(message))));
    }).detach();
}

void WebViewBackend::startSelfUpdateCheck(
    const NativeString &requestId, const NativeJsonDom::Object &params)
{
    const std::wstring url = nativeText(NativeJsonDom::stringValue(
        params, "url")).trimmed().toStdWString();
    if (url.empty()) {
        // No custom update server configured: fall back to the Gitee
        // releases check and still resolve the request.
        startUpdateCheck();
        sendNativeResult(requestId, NativeJsonDom::Value(true));
        return;
    }
    std::wstring host;
    unsigned short port = 0;
    bool secure = false;
    std::wstring path;
    if (!parseCloudEndpoint(url, host, port, secure, path)) {
        sendError(requestId, NativeString("更新源地址无效"));
        return;
    }
    const std::function<void(const std::string &)> handler = m_sendHandler;
    if (!handler)
        return;
    std::thread([handler, requestId, host, port, secure, path] {
        const HttpResult response = httpRequestUtf8(
            host, port, secure, L"GET", path, std::wstring(), std::string());
        NativeJsonDom::Object payload;
        payload.values.emplace(
            "currentVersion", std::string(MASTERTERM_VERSION));
        payload.values["hasUpdate"] = false;
        payload.values.emplace("status", static_cast<double>(response.status));
        if (response.status == 200) {
            std::string jsonError;
            NativeJsonDom::Value root;
            const std::string jsonBody = stripUtf8Bom(response.body);
            if (NativeJsonDom::parse(jsonBody, root, &jsonError)
                && root.isObject()) {
                const NativeJsonDom::Object &object = root.object();
                const auto version = object.values.find("version");
                const auto urlValue = object.values.find("url");
                const auto sha = object.values.find("sha256");
                const auto notes = object.values.find("notes");
                std::string latestVersion = version != object.values.end()
                        && version->second.isString()
                    ? version->second.string() : std::string();
                if (!parseVersionParts(latestVersion).empty()
                    && parseVersionParts(latestVersion)
                        > parseVersionParts(MASTERTERM_VERSION)) {
                    payload.values.emplace("latestVersion", latestVersion);
                    if (urlValue != object.values.end()
                        && urlValue->second.isString())
                        payload.values.emplace(
                            "url", urlValue->second.string());
                    if (sha != object.values.end()
                        && sha->second.isString())
                        payload.values.emplace(
                            "sha256", sha->second.string());
                    if (notes != object.values.end()
                        && notes->second.isString()) {
                        std::string body = safeTruncateUtf8(notes->second.string(), 4000);
                        payload.values.emplace("body", body);
                    }
                    payload.values["hasUpdate"] = true;
                } else {
                    payload.values.emplace("latestVersion", latestVersion);
                }
            } else {
                std::string snippet = jsonBody.substr(0, 160);
                for (char &character : snippet) {
                    if (character == '\r' || character == '\n'
                        || character == '\t')
                        character = ' ';
                }
                payload.values.emplace("debugBody", snippet);
            }
        }
        NativeJsonDom::Object message;
        message.values.emplace("event", "update.info");
        message.values.emplace("sessionId", std::string());
        message.values.emplace("payload", payload);
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(message))));
        // Resolve the original update.check request so the caller's promise
        // (e.g. the "立即检查更新" button) does not hang.
        NativeJsonDom::Object responseMessage;
        responseMessage.values.emplace("id", utf8Text(requestId));
        responseMessage.values.emplace("ok", true);
        responseMessage.values.emplace(
            "result", NativeJsonDom::Value(std::move(payload)));
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(responseMessage))));
    }).detach();
}

void WebViewBackend::startUpdateDownload(
    const NativeString &requestId, const NativeJsonDom::Object &params)
{
    const std::wstring url = nativeText(NativeJsonDom::stringValue(
        params, "url")).trimmed().toStdWString();
    const std::string expectedSha256 = NativeJsonDom::stringValue(
        params, "sha256");
    std::wstring host;
    unsigned short port = 0;
    bool secure = false;
    std::wstring path;
    if (!parseCloudEndpoint(url, host, port, secure, path)) {
        sendError(requestId, NativeString("下载地址无效"));
        return;
    }
    const std::function<void(const std::string &)> handler = m_sendHandler;
    if (!handler)
        return;
    std::thread([handler, requestId, host, port, secure, path, expectedSha256] {
        const auto failWith = [handler, requestId](const std::string &message) {
            NativeJsonDom::Object response;
            response.values.emplace("id", utf8Text(requestId));
            response.values.emplace("ok", false);
            response.values.emplace("error", message);
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(response))));
        };
        // Transient network issues (proxy cut-off, flaky link) can truncate
        // the artifact.  Retry a few times and report sizes so a persistent
        // failure is diagnosable.
        constexpr int maximumAttempts = 3;
        for (int attempt = 1; attempt <= maximumAttempts; ++attempt) {
            const HINTERNET session = WinHttpOpen(
                L"MasterTerm", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                nullptr, nullptr, 0);
            if (!session) {
                failWith("无法连接下载服务器");
                return;
            }
            WinHttpSetTimeouts(session, 10000, 10000, 15000, 300000);
            const HINTERNET connection = WinHttpConnect(
                session, host.c_str(), port, 0);
            if (!connection) {
                WinHttpCloseHandle(session);
                failWith("无法连接下载服务器");
                return;
            }
            const HINTERNET request = WinHttpOpenRequest(
                connection, L"GET", path.c_str(), nullptr, nullptr, nullptr,
                secure ? WINHTTP_FLAG_SECURE : 0);
            if (!request) {
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                failWith("无法发起下载请求");
                return;
            }
            const bool sent = WinHttpSendRequest(
                request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                nullptr, 0, 0, 0)
                && WinHttpReceiveResponse(request, nullptr);
            if (!sent) {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                failWith("下载请求失败");
                return;
            }
            std::int64_t totalBytes = 0;
            wchar_t lengthBuffer[32]{};
            DWORD lengthSize = sizeof(lengthBuffer);
            if (WinHttpQueryHeaders(
                    request, WINHTTP_QUERY_CONTENT_LENGTH,
                    WINHTTP_HEADER_NAME_BY_INDEX, lengthBuffer, &lengthSize,
                    WINHTTP_NO_HEADER_INDEX)) {
                totalBytes = std::wcstoll(lengthBuffer, nullptr, 10);
            }
            std::string downloaded;
            downloaded.reserve(totalBytes > 0
                ? static_cast<std::size_t>(totalBytes) : 4 * 1024 * 1024);
            DWORD available = 0;
            bool readError = false;
            while (WinHttpQueryDataAvailable(request, &available)
                   && available > 0) {
                std::vector<char> buffer(available);
                DWORD read = 0;
                if (!WinHttpReadData(
                        request, buffer.data(), available, &read)) {
                    readError = true;
                    break;
                }
                if (read > 0) {
                    downloaded.append(buffer.data(), read);
                    NativeJsonDom::Object payload;
                    payload.values.emplace(
                        "transferId", utf8Text(requestId));
                    payload.values.emplace(
                        "done", static_cast<double>(downloaded.size()));
                    payload.values.emplace(
                        "total", static_cast<double>(totalBytes));
                    NativeJsonDom::Object message;
                    message.values.emplace("event", "update.downloadProgress");
                    message.values.emplace("sessionId", std::string());
                    message.values.emplace("payload", payload);
                    handler(NativeJsonDom::stringify(
                        NativeJsonDom::Value(std::move(message))));
                }
            }
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            const std::int64_t actualSize =
                static_cast<std::int64_t>(downloaded.size());
            const bool sizeMatches = totalBytes <= 0
                || actualSize == totalBytes;
            const std::string actual = expectedSha256.empty()
                ? std::string()
                : NativeHash::sha256Hex(downloaded);
            const bool hashMatches = expectedSha256.empty()
                || actual == expectedSha256;
            if (!readError && !downloaded.empty()
                && sizeMatches && hashMatches) {
                const auto now = std::chrono::system_clock::now();
                const std::time_t time =
                    std::chrono::system_clock::to_time_t(now);
                std::tm local{};
                localtime_s(&local, &time);
                wchar_t stamp[32]{};
                wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
                const std::filesystem::path zipPath =
                    std::filesystem::temp_directory_path()
                    / (std::wstring(L"masterterm-update-") + stamp + L".zip");
                std::ofstream output(
                    zipPath, std::ios::binary | std::ios::trunc);
                if (!output) {
                    failWith("无法创建更新包临时文件");
                    return;
                }
                output.write(downloaded.data(),
                             static_cast<std::streamsize>(downloaded.size()));
                output.close();
                NativeJsonDom::Object result;
                result.values.emplace("path", utf8Text(pathText(zipPath)));
                result.values.emplace(
                    "size", static_cast<double>(actualSize));
                NativeJsonDom::Object message;
                message.values.emplace("id", utf8Text(requestId));
                message.values.emplace("ok", true);
                message.values.emplace("result", result);
                handler(NativeJsonDom::stringify(
                    NativeJsonDom::Value(std::move(message))));
                return;
            }
            // Retry on the next iteration; report the final failure below.
            if (attempt == maximumAttempts) {
                std::string reason;
                if (readError || downloaded.empty())
                    reason = "下载不完整（已接收 "
                        + std::to_string(actualSize) + " 字节"
                        + (totalBytes > 0
                            ? " / 期望 " + std::to_string(totalBytes) + " 字节"
                            : std::string()) + "）";
                else if (!sizeMatches)
                    reason = "下载大小不一致（已接收 "
                        + std::to_string(actualSize) + " 字节 / 期望 "
                        + std::to_string(totalBytes) + " 字节）";
                else
                    reason = "更新包校验失败：SHA-256 不一致";
                failWith(reason + "（已重试 " + std::to_string(attempt)
                         + " 次）");
                return;
            }
        }
    }).detach();
}

bool WebViewBackend::installUpdate(
    const NativeString &zipPath, NativeString &error) const
{
    const std::filesystem::path zip = nativePath(zipPath);
    std::error_code checkError;
    if (!std::filesystem::exists(zip, checkError)) {
        error = NativeString("更新包不存在：") + pathText(zip);
        return false;
    }
    const std::filesystem::path applicationDir =
        nativePath(applicationDirectory());
    const std::filesystem::path scriptPath =
        std::filesystem::temp_directory_path() / L"masterterm-update.ps1";
    // Single-quoted PowerShell strings: escape embedded quotes by doubling.
    std::wstring zipText = zip.wstring();
    std::wstring dirText = applicationDir.wstring();
    std::wstring exeText = (applicationDir / L"MasterTerm.exe").wstring();
    const auto escape = [](std::wstring value) {
        std::wstring result;
        result.reserve(value.size() + 4);
        result.push_back(L'\'');
        for (wchar_t character : value) {
            if (character == L'\'')
                result += L"''";
            else
                result.push_back(character);
        }
        result.push_back(L'\'');
        return result;
    };
    std::wstring script;
    script += L"$log = Join-Path $env:TEMP 'masterterm-update.log'\r\n";
    script += L"'--- update start ' + (Get-Date) | Out-File $log\r\n";
    script += L"$zip = " + escape(zipText) + L"\r\n";
    script += L"$dir = " + escape(dirText) + L"\r\n";
    script += L"$exe = " + escape(exeText) + L"\r\n";
    script += L"$stage = Join-Path $env:TEMP ('masterterm-update-stage-' + [guid]::NewGuid().ToString('N'))\r\n";
    script += L"'zip=' + $zip | Out-File $log -Append\r\n";
    script += L"'dir=' + $dir | Out-File $log -Append\r\n";
    script += L"$deadline = (Get-Date).AddMinutes(5)\r\n";
    script += L"while (Get-Process -Name MasterTerm -ErrorAction SilentlyContinue) {\r\n";
    script += L"    if ((Get-Date) -gt $deadline) { 'timeout waiting for MasterTerm exit' | Out-File $log -Append; exit 1 }\r\n";
    script += L"    Start-Sleep -Seconds 1\r\n";
    script += L"}\r\n";
    script += L"'MasterTerm exited' | Out-File $log -Append\r\n";
    script += L"$before = (Get-Item $exe -ErrorAction SilentlyContinue).Length\r\n";
    script += L"try {\r\n";
    script += L"    New-Item -ItemType Directory -Path $stage -Force | Out-Null\r\n";
    script += L"    Expand-Archive -LiteralPath $zip -DestinationPath $stage -Force -ErrorAction Stop 2>>$log\r\n";
    script += L"    if (-not (Test-Path (Join-Path $stage 'MasterTerm.exe'))) { throw 'package MasterTerm.exe missing' }\r\n";
    script += L"    if (-not (Test-Path (Join-Path $stage 'web\\index.html'))) { throw 'package web/index.html missing' }\r\n";
    script += L"    if (-not (Test-Path (Join-Path $stage 'web\\app.js'))) { throw 'package web/app.js missing' }\r\n";
    script += L"    Get-Process -Name MasterTermSftpWorker -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue\r\n";
    script += L"    Copy-Item -Path (Join-Path $stage '*') -Destination $dir -Recurse -Force -ErrorAction Stop 2>>$log\r\n";
    script += L"} catch {\r\n";
    script += L"    'extract exception: ' + $_.Exception.Message | Out-File $log -Append\r\n";
    script += L"    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    script += L"    exit 3\r\n";
    script += L"}\r\n";
    script += L"'validated staged install copied' | Out-File $log -Append\r\n";
    script += L"$after = (Get-Item $exe -ErrorAction SilentlyContinue).Length\r\n";
    script += L"'exe before=' + $before + ' after=' + $after | Out-File $log -Append\r\n";
    script += L"if (-not (Test-Path $exe)) { 'exe missing after extract' | Out-File $log -Append; exit 2 }\r\n";
    script += L"if (-not (Test-Path (Join-Path $dir 'web\\index.html'))) { 'web/index.html missing after extract' | Out-File $log -Append; exit 4 }\r\n";
    script += L"if ($before -eq $after) { 'exe unchanged - extract may have failed' | Out-File $log -Append }\r\n";
    script += L"Remove-Item $zip -Force -ErrorAction SilentlyContinue\r\n";
    script += L"Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    script += L"Start-Process -FilePath $exe -WorkingDirectory $dir\r\n";
    script += L"'update finished' | Out-File $log -Append\r\n";
    script += L"Remove-Item $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue\r\n";
    std::ofstream output(scriptPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = NativeString("无法创建更新脚本");
        return false;
    }
    const std::string utf8 = wideToUtf8(script);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    output.close();
    std::wstring commandLine = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \""
        + scriptPath.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommandLine = commandLine;
    if (!CreateProcessW(
            nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = NativeString("无法启动更新脚本");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

NativeJsonDom::Value WebViewBackend::createServerProfile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    ServerRecord record;
    if (!applyProfileParams(record, params, error))
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    std::vector<ServerRecord> records = readServerRecords();
    records.push_back(record);
    writeServerRecords(records);

    const NativeString address = nativeText(record.address);
    const NativeString password =
        nativeText(NativeJsonDom::stringValue(params, "password"));
    if (!password.isEmpty())
        saveStoredPassword(address, password);
    const NativeString keyPassphrase =
        nativeText(NativeJsonDom::stringValue(params, "keyPassphrase"));
    if (!keyPassphrase.isEmpty())
        saveStoredCredential(NativeString("KeyPassphrase"), address, keyPassphrase);
    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword =
        nativeText(NativeJsonDom::stringValue(params, "proxyPassword"));
    if (!proxyJump.isEmpty() && !proxyPassword.isEmpty())
        saveStoredCredential(NativeString("ProxyJump"), proxyJump, proxyPassword);

    const NativeJsonDom::Array profiles = serverProfiles();
    return profiles.values.empty()
        ? NativeJsonDom::Value(NativeJsonDom::Object{})
        : profiles.values.back();
}

NativeJsonDom::Value WebViewBackend::updateServerProfile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const int index = NativeJsonDom::integerValue(params, "index", -1);
    std::vector<ServerRecord> records = readServerRecords();
    if (index < 0 || index >= static_cast<int>(records.size())) {
        error = NativeString("服务器配置不存在");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    ServerRecord record = records.at(static_cast<std::size_t>(index));
    const NativeString oldAddress = nativeText(record.address);
    const NativeString oldProxyJump = nativeText(record.proxyJump);
    if (!applyProfileParams(record, params, error))
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    records[index] = record;
    writeServerRecords(records);

    const NativeString address = nativeText(record.address);
    const NativeString password =
        nativeText(NativeJsonDom::stringValue(params, "password"));
    if (!password.isEmpty()) {
        saveStoredPassword(address, password);
        if (oldAddress != address)
            deleteStoredCredential(NativeString("SSH"), oldAddress);
    } else if (oldAddress != address) {
        const NativeString existingPassword = readStoredPassword(oldAddress);
        if (!existingPassword.isEmpty()) {
            saveStoredPassword(address, existingPassword);
            deleteStoredCredential(NativeString("SSH"), oldAddress);
        }
    }

    const NativeString keyPassphrase =
        nativeText(NativeJsonDom::stringValue(params, "keyPassphrase"));
    if (!keyPassphrase.isEmpty()) {
        saveStoredCredential(
            NativeString("KeyPassphrase"), address, keyPassphrase);
        if (oldAddress != address)
            deleteStoredCredential(NativeString("KeyPassphrase"), oldAddress);
    } else if (oldAddress != address) {
        const NativeString existingPassphrase = readStoredCredential(
            NativeString("KeyPassphrase"), oldAddress);
        if (!existingPassphrase.isEmpty()) {
            saveStoredCredential(
                NativeString("KeyPassphrase"), address, existingPassphrase);
            deleteStoredCredential(NativeString("KeyPassphrase"), oldAddress);
        }
    }

    const NativeString proxyJump = nativeText(record.proxyJump);
    const NativeString proxyPassword =
        nativeText(NativeJsonDom::stringValue(params, "proxyPassword"));
    if (!proxyJump.isEmpty() && !proxyPassword.isEmpty()) {
        saveStoredCredential(NativeString("ProxyJump"), proxyJump, proxyPassword);
        if (oldProxyJump != proxyJump && !oldProxyJump.isEmpty())
            deleteStoredCredential(NativeString("ProxyJump"), oldProxyJump);
    }

    return serverProfiles().values.at(static_cast<std::size_t>(index));
}

NativeJsonDom::Value WebViewBackend::copyServerProfile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const int index = NativeJsonDom::integerValue(params, "index", -1);
    std::vector<ServerRecord> records = readServerRecords();
    if (index < 0 || index >= static_cast<int>(records.size())) {
        error = NativeString("服务器配置不存在");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    ServerRecord copy = records.at(static_cast<std::size_t>(index));
    copy.name += " 副本";
    records.push_back(copy);
    writeServerRecords(records);
    const NativeJsonDom::Array profiles = serverProfiles();
    return profiles.values.back();
}

bool WebViewBackend::reorderServerProfile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const int sourceIndex = NativeJsonDom::integerValue(params, "index", -1);
    const int targetIndex = NativeJsonDom::integerValue(
        params, "targetIndex", -1);
    const bool before = NativeJsonDom::booleanValue(params, "before", true);
    std::vector<ServerRecord> records = readServerRecords();
    const int size = static_cast<int>(records.size());
    if (sourceIndex < 0 || sourceIndex >= size
        || targetIndex < 0 || targetIndex >= size) {
        error = NativeString("连接排序目标不存在");
        return false;
    }
    if (sourceIndex == targetIndex)
        return true;

    ServerRecord moved = std::move(records.at(
        static_cast<std::size_t>(sourceIndex)));
    if (NativeJsonDom::contains(params, "workspace")) {
        NativeString workspace = nativeText(NativeJsonDom::stringValue(
            params, "workspace", moved.workspace)).trimmed();
        if (workspace.isEmpty())
            workspace = NativeString("未分配");
        const std::string workspaceText = utf8Text(workspace);
        const std::vector<std::string> workspaces = readWorkspaceNames();
        if (std::find(workspaces.begin(), workspaces.end(), workspaceText)
            == workspaces.end()) {
            error = NativeString("工作区不存在，请先创建工作区");
            return false;
        }
        moved.workspace = workspaceText;
    }
    records.erase(records.begin() + sourceIndex);

    int insertionIndex = targetIndex;
    if (sourceIndex < targetIndex)
        --insertionIndex;
    if (!before)
        ++insertionIndex;
    insertionIndex = std::clamp(
        insertionIndex, 0, static_cast<int>(records.size()));
    const std::string movedWorkspace = moved.workspace;
    records.insert(
        records.begin() + insertionIndex, std::move(moved));
    writeServerRecords(records);
    DiagnosticLog::write(
        "server-reorder",
        "from=" + std::to_string(sourceIndex)
            + " target=" + std::to_string(targetIndex)
            + " before=" + (before ? "true" : "false")
            + " workspace=" + movedWorkspace);
    return true;
}

bool WebViewBackend::deleteServerProfile(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const int index = NativeJsonDom::integerValue(params, "index", -1);
    std::vector<ServerRecord> records = readServerRecords();
    if (index < 0 || index >= static_cast<int>(records.size())) {
        error = NativeString("服务器配置不存在");
        return false;
    }
    // A session owns its connection settings after it has been created.
    // Deleting a saved profile must therefore only affect future connections;
    // existing SSH and serial sessions remain usable until the user closes them.
    const ServerRecord removed =
        records.at(static_cast<std::size_t>(index));
    records.erase(records.begin() + index);
    writeServerRecords(records);

    const NativeString address = nativeText(removed.address);
    const NativeString proxyJump = nativeText(removed.proxyJump);
    bool addressStillUsed = false;
    bool proxyStillUsed = false;
    for (const ServerRecord &record : records) {
        addressStillUsed = addressStillUsed
            || record.address == removed.address;
        proxyStillUsed = proxyStillUsed
            || record.proxyJump == removed.proxyJump;
    }
    if (!address.isEmpty() && !addressStillUsed) {
        deleteStoredCredential(NativeString("SSH"), address);
        deleteStoredCredential(NativeString("KeyPassphrase"), address);
    }
    if (!proxyJump.isEmpty() && !proxyStillUsed)
        deleteStoredCredential(NativeString("ProxyJump"), proxyJump);
    return true;
}

NativeJsonDom::Array WebViewBackend::workspaceNames() const
{
    NativeJsonDom::Array result;
    for (const std::string &name : readWorkspaceNames())
        result.values.emplace_back(name);
    return result;
}

bool WebViewBackend::createWorkspace(
    const NativeJsonDom::Object &params, std::string &name,
    NativeString &error)
{
    name = utf8Text(nativeText(
        NativeJsonDom::stringValue(params, "name")).trimmed());
    if (name.empty()) {
        error = NativeString("工作区名称不能为空");
        return false;
    }
    if (name == "全部连接" || name == "未分配") {
        error = NativeString("该名称为系统保留名称");
        return false;
    }
    std::vector<std::string> names = readWorkspaceNames();
    if (std::find(names.begin(), names.end(), name) != names.end()) {
        error = NativeString("工作区已存在");
        return false;
    }
    names.push_back(name);
    writeWorkspaceNames(names);
    return true;
}

bool WebViewBackend::renameWorkspace(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const std::string oldName = utf8Text(nativeText(
        NativeJsonDom::stringValue(params, "name")).trimmed());
    const std::string newName = utf8Text(nativeText(
        NativeJsonDom::stringValue(params, "newName")).trimmed());
    std::vector<std::string> names = readWorkspaceNames();
    if (oldName.empty() || oldName == "未分配"
        || std::find(names.begin(), names.end(), oldName) == names.end()) {
        error = NativeString("工作区不存在或不可重命名");
        return false;
    }
    if (newName.empty() || newName == "全部连接"
        || newName == "未分配"
        || std::find(names.begin(), names.end(), newName) != names.end()) {
        error = NativeString("新工作区名称无效或已存在");
        return false;
    }
    *std::find(names.begin(), names.end(), oldName) = newName;
    std::vector<ServerRecord> records = readServerRecords();
    for (ServerRecord &record : records) {
        if (record.workspace == oldName)
            record.workspace = newName;
    }
    writeServerRecords(records);
    writeWorkspaceNames(names);
    return true;
}

bool WebViewBackend::deleteWorkspace(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const std::string name = utf8Text(nativeText(
        NativeJsonDom::stringValue(params, "name")).trimmed());
    std::vector<std::string> names = readWorkspaceNames();
    if (name.empty() || name == "未分配"
        || std::find(names.begin(), names.end(), name) == names.end()) {
        error = NativeString("工作区不存在或不可删除");
        return false;
    }
    std::vector<ServerRecord> records = readServerRecords();
    for (ServerRecord &record : records) {
        if (record.workspace == name)
            record.workspace = "未分配";
    }
    writeServerRecords(records);
    names.erase(
        std::remove(names.begin(), names.end(), name), names.end());
    writeWorkspaceNames(names);
    return true;
}

void WebViewBackend::sendNativeEvent(
    const std::string &event, const std::string &sessionId,
    const NativeJsonDom::Object &payload)
{
    if (!m_sendHandler)
        return;
    NativeJsonDom::Object message;
    message.values.emplace("event", event);
    message.values.emplace("sessionId", sessionId);
    message.values.emplace("payload", payload);
    m_sendHandler(
        NativeJsonDom::stringify(NativeJsonDom::Value(std::move(message))));
}

void WebViewBackend::openSessionLog(
    const NativeString &sessionId, const std::string &name)
{
    if (!m_sessionLoggingEnabled)
        return;
    collectSensitiveValues();
    const std::string key = utf8Text(sessionId);
    if (m_sessionLogs.find(key) != m_sessionLogs.end())
        return;
    std::error_code directoryError;
    const std::filesystem::path directory = sessionLogDirectory();
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError)
        return;
    std::string safeName;
    safeName.reserve(name.size());
    for (const char character : name) {
        if (std::isalnum(static_cast<unsigned char>(character))
            || character == '-' || character == '_' || character == '.')
            safeName.push_back(character);
        else
            safeName.push_back('_');
    }
    if (safeName.empty())
        safeName = "session";
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    wchar_t stamp[32]{};
    wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
    std::filesystem::path filePath = directory
        / (std::wstring(stamp) + L"-" + utf8ToWide(safeName) + L".log");
    std::ofstream file(filePath, std::ios::binary | std::ios::app);
    if (file)
        m_sessionLogs[key] = std::move(file);
}

void WebViewBackend::closeSessionLog(const NativeString &sessionId)
{
    const auto found = m_sessionLogs.find(utf8Text(sessionId));
    if (found == m_sessionLogs.end())
        return;
    found->second.flush();
    found->second.close();
    m_sessionLogs.erase(found);
}

void WebViewBackend::collectSensitiveValues()
{
    // Passwords, key passphrases and proxy passwords must never appear in
    // session logs.  Rebuild the redaction list whenever a session log opens
    // so later credential edits are also masked.
    m_sensitiveValues.clear();
    const auto appendIfMeaningful = [this](const NativeString &value) {
        const std::string text = utf8Text(value);
        if (text.size() >= 4)
            m_sensitiveValues.push_back(text);
    };
    for (const ServerRecord &record : readServerRecords()) {
        appendIfMeaningful(nativeText(record.password));
        appendIfMeaningful(nativeText(record.keyPassphrase));
        appendIfMeaningful(nativeText(record.proxyPassword));
        appendIfMeaningful(readStoredPassword(nativeText(record.address)));
        appendIfMeaningful(readStoredCredential(
            NativeString("KeyPassphrase"), nativeText(record.address)));
        appendIfMeaningful(readStoredCredential(
            NativeString("ProxyJump"), nativeText(record.proxyJump)));
    }
}

std::string WebViewBackend::redactSensitiveText(const std::string &text) const
{
    if (m_sensitiveValues.empty())
        return text;
    std::string result = text;
    for (const std::string &secret : m_sensitiveValues) {
        if (secret.empty() || secret.size() < 4)
            continue;
        for (std::size_t position = 0;
             (position = result.find(secret, position)) != std::string::npos;) {
            result.replace(position, secret.size(), "***");
            position += 3;
        }
    }
    return result;
}

void WebViewBackend::emitSessionOutput(
    const NativeString &sessionId, const std::string &output)
{
    const auto found = m_sessionLogs.find(utf8Text(sessionId));
    if (found != m_sessionLogs.end()) {
        const std::string redacted = redactSensitiveText(output);
        found->second.write(redacted.data(), static_cast<std::streamsize>(redacted.size()));
        found->second.flush();
    }
    NativeJsonDom::Object payload;
    payload.values.emplace("data", NativeBase64::encode(output));
    sendNativeEvent("session.output", utf8Text(sessionId), payload);
}

void WebViewBackend::startCloudAuthRequest(
    const NativeString &requestId, const NativeJsonDom::Object &params,
    bool registerMode)
{
    const std::wstring url = nativeText(NativeJsonDom::stringValue(
        params, "url")).trimmed().toStdWString();
    const std::wstring username = nativeText(NativeJsonDom::stringValue(
        params, "username")).trimmed().toStdWString();
    const std::wstring password = nativeText(NativeJsonDom::stringValue(
        params, "password")).toStdWString();
    const std::function<void(const std::string &)> handler = m_sendHandler;
    if (!handler)
        return;
    // Detached worker thread: captures only the send handler and value
    // copies, never "this", so it is safe even if the backend is destroyed.
    std::thread([url, username, password, requestId, registerMode, handler] {
        std::wstring host;
        unsigned short port = 0;
        bool secure = false;
        std::wstring pathPrefix;
        if (!parseCloudEndpoint(url, host, port, secure, pathPrefix)) {
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value([&] {
                    NativeJsonDom::Object response;
                    response.values.emplace("id", utf8Text(requestId));
                    response.values.emplace("ok", false);
                    response.values.emplace(
                        "error", std::string("同步服务器地址无效，示例：https://example.com"));
                    return response;
                }())));
            return;
        }
        NativeJsonDom::Object body;
        body.values.emplace("username", utf8Text(NativeString::fromStdWString(username)));
        body.values.emplace("password", utf8Text(NativeString::fromStdWString(password)));
        const HttpResult response = httpRequestUtf8(
            host, port, secure, L"POST",
            cloudApiPath(pathPrefix, registerMode ? L"api/register" : L"api/login"),
            std::wstring(), NativeJsonDom::stringify(NativeJsonDom::Value(std::move(body))));
        std::string jsonError;
        NativeJsonDom::Value root;
        if (response.status == 200
            && NativeJsonDom::parse(response.body, root, &jsonError)
            && root.isObject()) {
            NativeJsonDom::Object message;
            message.values.emplace("id", utf8Text(requestId));
            message.values.emplace("ok", true);
            message.values.emplace("result", std::move(root));
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(message))));
            return;
        }
        std::string message;
        if (NativeJsonDom::parse(response.body, root, &jsonError)
            && root.isObject()) {
            const auto found = root.object().values.find("error");
            if (found != root.object().values.end() && found->second.isString())
                message = found->second.string();
        }
        if (response.status == 0)
            message = "无法连接同步服务器";
        else if (message.empty())
            message = "同步服务器返回错误（HTTP "
                + std::to_string(response.status) + "）";
        NativeJsonDom::Object responseMessage;
        responseMessage.values.emplace("id", utf8Text(requestId));
        responseMessage.values.emplace("ok", false);
        responseMessage.values.emplace("error", message);
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(responseMessage))));
    }).detach();
}

void WebViewBackend::startCloudSyncRequest(
    const NativeString &requestId, const NativeJsonDom::Object &params,
    bool push)
{
    const std::wstring url = nativeText(NativeJsonDom::stringValue(
        params, "url")).trimmed().toStdWString();
    const std::wstring token = nativeText(NativeJsonDom::stringValue(
        params, "token")).trimmed().toStdWString();
    const std::string payloadJson = NativeJsonDom::stringValue(
        params, "payload");
    const std::function<void(const std::string &)> handler = m_sendHandler;
    if (!handler)
        return;
    if (push && payloadJson.size() > 5 * 1024 * 1024) {
        NativeJsonDom::Object response;
        response.values.emplace("id", utf8Text(requestId));
        response.values.emplace("ok", false);
        response.values.emplace(
            "error", std::string("同步数据超过 5MB 上限，已拒绝上传"));
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(response))));
        return;
    }
    std::thread([url, token, payloadJson, requestId, push, handler] {
        std::wstring host;
        unsigned short port = 0;
        bool secure = false;
        std::wstring pathPrefix;
        if (!parseCloudEndpoint(url, host, port, secure, pathPrefix)) {
            NativeJsonDom::Object response;
            response.values.emplace("id", utf8Text(requestId));
            response.values.emplace("ok", false);
            response.values.emplace(
                "error", std::string("同步服务器地址无效，示例：https://example.com"));
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(response))));
            return;
        }
        const std::wstring headers = push
            ? L"Authorization: Bearer " + token + L"\r\nContent-Type: application/json\r\n"
            : L"Authorization: Bearer " + token + L"\r\n";
        std::string body;
        if (push) {
            std::string payloadError;
            NativeJsonDom::Value payload;
            NativeJsonDom::Object wrapper;
            if (NativeJsonDom::parse(payloadJson, payload, &payloadError))
                wrapper.values.emplace("data", std::move(payload));
            else
                wrapper.values.emplace("data", NativeJsonDom::Value(nullptr));
            body = NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(wrapper)));
        }
        const HttpResult response = httpRequestUtf8(
            host, port, secure, push ? L"PUT" : L"GET",
            cloudApiPath(pathPrefix, L"api/sync"), headers, body);
        std::string jsonError;
        NativeJsonDom::Value root;
        if (response.status == 200
            && NativeJsonDom::parse(response.body, root, &jsonError)
            && root.isObject()) {
            NativeJsonDom::Object message;
            message.values.emplace("id", utf8Text(requestId));
            message.values.emplace("ok", true);
            message.values.emplace("result", std::move(root));
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(message))));
            return;
        }
        std::string message;
        if (NativeJsonDom::parse(response.body, root, &jsonError)
            && root.isObject()) {
            const auto found = root.object().values.find("error");
            if (found != root.object().values.end() && found->second.isString())
                message = found->second.string();
        }
        if (response.status == 0)
            message = "无法连接同步服务器";
        else if (message.empty())
            message = "同步服务器返回错误（HTTP "
                + std::to_string(response.status) + "）";
        NativeJsonDom::Object responseMessage;
        responseMessage.values.emplace("id", utf8Text(requestId));
        responseMessage.values.emplace("ok", false);
        responseMessage.values.emplace("error", message);
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(responseMessage))));
    }).detach();
}

void WebViewBackend::startCloudHistoryRequest(
    const NativeString &requestId, const NativeJsonDom::Object &params,
    const NativeString &operation)
{
    // 云端历史快照：list 用 GET；restore、rename、delete 用 POST。
    const std::wstring url = nativeText(NativeJsonDom::stringValue(
        params, "url")).trimmed().toStdWString();
    const std::wstring token = nativeText(NativeJsonDom::stringValue(
        params, "token")).trimmed().toStdWString();
    // Accept numeric strings too for compatibility with older WebView
    // snapshots and imported cloud data.
    const int seq = NativeJsonDom::convertedIntegerValue(params, "seq", 0);
    const std::string operationUtf8 = operation.toUtf8();
    const std::wstring name = nativeText(NativeJsonDom::stringValue(
        params, "name")).trimmed().toStdWString();
    const std::function<void(const std::string &)> handler = m_sendHandler;
    if (!handler)
        return;
    std::thread([url, token, seq, name, operationUtf8, requestId, handler] {
        std::wstring host;
        unsigned short port = 0;
        bool secure = false;
        std::wstring pathPrefix;
        if (!parseCloudEndpoint(url, host, port, secure, pathPrefix)) {
            NativeJsonDom::Object response;
            response.values.emplace("id", utf8Text(requestId));
            response.values.emplace("ok", false);
            response.values.emplace(
                "error", std::string("同步服务器地址无效，示例：https://example.com"));
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(response))));
            return;
        }
        const bool list = operationUtf8 == "list";
        std::string body;
        if (!list) {
            NativeJsonDom::Object wrapper;
            wrapper.values.emplace("seq", static_cast<double>(seq));
            if (operationUtf8 == "rename") {
                const std::wstring trimmedName = name.substr(0, 64);
                wrapper.values.emplace("name",
                    NativeString::fromStdWString(trimmedName).toUtf8());
            }
            body = NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(wrapper)));
        }
        const std::wstring headers =
            L"Authorization: Bearer " + token
            + (list ? L"\r\n" : L"\r\nContent-Type: application/json\r\n");
        const wchar_t *endpoint =
            operationUtf8 == "restore" ? L"api/sync/restore"
            : operationUtf8 == "rename" ? L"api/sync/history/rename"
            : operationUtf8 == "delete" ? L"api/sync/history/delete"
            : L"api/sync/history";
        const HttpResult response = httpRequestUtf8(
            host, port, secure, list ? L"GET" : L"POST",
            cloudApiPath(pathPrefix, endpoint),
            headers, body);
        std::string jsonError;
        NativeJsonDom::Value root;
        if (response.status == 200
            && NativeJsonDom::parse(response.body, root, &jsonError)
            && root.isObject()) {
            NativeJsonDom::Object message;
            message.values.emplace("id", utf8Text(requestId));
            message.values.emplace("ok", true);
            message.values.emplace("result", std::move(root));
            handler(NativeJsonDom::stringify(
                NativeJsonDom::Value(std::move(message))));
            return;
        }
        std::string message;
        if (NativeJsonDom::parse(response.body, root, &jsonError)
            && root.isObject()) {
            const auto found = root.object().values.find("error");
            if (found != root.object().values.end() && found->second.isString())
                message = found->second.string();
        }
        if (response.status == 0)
            message = "无法连接同步服务器";
        else if (message.empty())
            message = "同步服务器返回错误（HTTP "
                + std::to_string(response.status) + "）";
        NativeJsonDom::Object responseMessage;
        responseMessage.values.emplace("id", utf8Text(requestId));
        responseMessage.values.emplace("ok", false);
        responseMessage.values.emplace("error", message);
        handler(NativeJsonDom::stringify(
            NativeJsonDom::Value(std::move(responseMessage))));
    }).detach();
}

NativeJsonDom::Array WebViewBackend::exportCloudServers() const{
    NativeJsonDom::Array result;
    const std::vector<ServerRecord> records = readServerRecords();
    for (const ServerRecord &record : records) {
        NativeJsonDom::Object profile;
        profile.values.emplace("connectionType", record.connectionType);
        profile.values.emplace("name", record.name);
        profile.values.emplace("address", record.address);
        profile.values.emplace("port", record.port);
        profile.values.emplace("tag", record.tag);
        profile.values.emplace("tagMode", record.tagMode);
        profile.values.emplace("workspace", record.workspace);
        profile.values.emplace("color", record.color);
        profile.values.emplace("iconKey", record.iconKey);
        profile.values.emplace("keyPath", record.keyPath);
        profile.values.emplace("keyPassphrase", record.keyPassphrase);
        profile.values.emplace("proxyJump", record.proxyJump);
        profile.values.emplace("proxyKeyPath", record.proxyKeyPath);
        profile.values.emplace(
            "serialDataBits", static_cast<double>(record.serialDataBits));
        profile.values.emplace("serialParity", record.serialParity);
        profile.values.emplace("serialStopBits", record.serialStopBits);
        profile.values.emplace("serialFlowControl", record.serialFlowControl);
        if (record.connectionType == "rdp")
            profile.values.emplace(
                "rdpOptions",
                NativeJsonDom::Value(rdpOptionsJson(record.rdpOptions)));
        // Credentials: configured values first, then Windows Credential
        // Manager, so a synced profile carries everything needed on a new
        // machine.  Every secret is DPAPI-encrypted before it leaves the
        // machine (T2-2); the sync server only ever stores ciphertext and
        // a profile imported on another device simply falls back to asking
        // for credentials.
        const NativeString address = nativeText(record.address);
        std::string cryptoError;
        const auto protectSecret = [&cryptoError](const NativeString &value) {
            if (value.isEmpty())
                return std::string();
            std::string error;
            std::string encrypted = NativeCrypto::protectBase64(
                value.toStdString(), error);
            if (encrypted.empty())
                cryptoError = error;
            return NativeCrypto::EncryptedPrefix + encrypted;
        };
        profile.values.emplace(
            "password", protectSecret(record.password.empty()
                ? readStoredPassword(address)
                : nativeText(record.password)));
        profile.values.emplace(
            "keyPassphrase", protectSecret(record.keyPassphrase.empty()
                ? readStoredCredential(NativeString("KeyPassphrase"), address)
                : nativeText(record.keyPassphrase)));
        profile.values.emplace(
            "proxyPassword", protectSecret(record.proxyPassword.empty()
                ? readStoredCredential(NativeString("ProxyJump"), nativeText(record.proxyJump))
                : nativeText(record.proxyPassword)));
        // Private key files travel along as base64 so a synced profile works
        // on a fresh machine without re-picking keys.  The key material is
        // encrypted with the other secrets; a different device cannot
        // decrypt it and skips the field during import.
        const auto readKeyFileBase64 = [](const std::string &path) {
            std::error_code error;
            const std::filesystem::path file = std::filesystem::u8path(path);
            if (path.empty() || !std::filesystem::exists(file, error))
                return std::string();
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return std::string();
            std::string content((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
            return NativeBase64::encode(content);
        };
        profile.values.emplace(
            "keyData", protectSecret(nativeText(readKeyFileBase64(record.keyPath))));
        profile.values.emplace(
            "proxyKeyData", protectSecret(nativeText(readKeyFileBase64(record.proxyKeyPath))));
        if (!cryptoError.empty())
            DiagnosticLog::write(
                "cloud-export-crypto-error", cryptoError);
        NativeJsonDom::Array tunnels;
        for (const ServerRecord::TunnelConfig &config : record.tunnels) {
            NativeJsonDom::Object tunnel;
            tunnel.values.emplace("mode", config.mode);
            tunnel.values.emplace("listenHost", config.listenHost);
            tunnel.values.emplace(
                "listenPort", static_cast<double>(config.listenPort));
            tunnel.values.emplace("targetHost", config.targetHost);
            tunnel.values.emplace(
                "targetPort", static_cast<double>(config.targetPort));
            tunnels.values.emplace_back(std::move(tunnel));
        }
        profile.values.emplace(
            "tunnels", NativeJsonDom::Value(std::move(tunnels)));
        result.values.emplace_back(std::move(profile));
    }
    return result;
}

NativeJsonDom::Value WebViewBackend::importCloudServers(
    const NativeJsonDom::Object &params, NativeString &error)
{
    const auto servers = params.values.find("servers");
    if (servers == params.values.end() || !servers->second.isArray()) {
        error = NativeString("同步数据缺少连接列表");
        return NativeJsonDom::Value(NativeJsonDom::Object{});
    }
    std::vector<ServerRecord> records = readServerRecords();
    std::vector<std::string> workspaces = readWorkspaceNames();
    int undecryptableSecrets = 0;
    // T2-5：工作区同步。按云端顺序插入缺失工作区（未分配固定在最前），
    // 本机独有工作区排在后面；下载后清理"不在云端且没有任何连接"的
    // 本机空工作区，保证两台设备列表一致。
    std::vector<std::string> cloudWorkspaceOrder;
    const auto addWorkspace = [&cloudWorkspaceOrder](const std::string &name) {
        if (name.empty())
            return;
        if (std::find(cloudWorkspaceOrder.begin(), cloudWorkspaceOrder.end(),
                      name) == cloudWorkspaceOrder.end()) {
            cloudWorkspaceOrder.push_back(name);
        }
    };
    const auto importedWorkspaces = params.values.find("workspaces");
    if (importedWorkspaces != params.values.end()
        && importedWorkspaces->second.isArray()) {
        for (const NativeJsonDom::Value &value :
             importedWorkspaces->second.array().values) {
            if (value.isString())
                addWorkspace(value.string());
        }
    }
    // Match by connection type + address so updates land on the same profile
    // and brand-new entries are appended; nothing is deleted.
    auto findRecord = [&records](const NativeJsonDom::Object &server) {
        const std::string type = NativeJsonDom::stringValue(
            server, "connectionType", "ssh");
        const std::string address = NativeJsonDom::stringValue(server, "address");
        for (ServerRecord &record : records) {
            if (record.connectionType == type && record.address == address)
                return &record;
        }
        return static_cast<ServerRecord *>(nullptr);
    };
    for (const NativeJsonDom::Value &value : servers->second.array().values) {
        if (!value.isObject())
            continue;
        const NativeJsonDom::Object &server = value.object();
        addWorkspace(NativeJsonDom::stringValue(server, "workspace"));
        ServerRecord *target = findRecord(server);
        ServerRecord merged;
        const bool isNew = target == nullptr;
        if (!isNew)
            merged = *target;
        else
            merged.connectionType = NativeJsonDom::stringValue(
                server, "connectionType", "ssh");
        const auto apply = [&server, &merged](const char *key, std::string ServerRecord::*field) {
            if (NativeJsonDom::contains(server, key))
                merged.*field = NativeJsonDom::stringValue(server, key);
        };
        apply("name", &ServerRecord::name);
        apply("address", &ServerRecord::address);
        apply("port", &ServerRecord::port);
        apply("tag", &ServerRecord::tag);
        apply("workspace", &ServerRecord::workspace);
        apply("color", &ServerRecord::color);
        apply("iconKey", &ServerRecord::iconKey);
        apply("keyPath", &ServerRecord::keyPath);
        apply("keyPassphrase", &ServerRecord::keyPassphrase);
        apply("proxyJump", &ServerRecord::proxyJump);
        apply("proxyKeyPath", &ServerRecord::proxyKeyPath);
        apply("serialParity", &ServerRecord::serialParity);
        apply("serialStopBits", &ServerRecord::serialStopBits);
        apply("serialFlowControl", &ServerRecord::serialFlowControl);
        if (NativeJsonDom::contains(server, "serialDataBits"))
            merged.serialDataBits = std::clamp(
                NativeJsonDom::integerValue(server, "serialDataBits", 8), 5, 8);
        if (NativeJsonDom::contains(server, "rdpOptions"))
            readRdpOptions(server, merged.rdpOptions);
        if (NativeJsonDom::contains(server, "tunnels")
            && server.values.at("tunnels").isArray()) {
            merged.tunnels.clear();
            for (const NativeJsonDom::Value &tunnelValue :
                 server.values.at("tunnels").array().values) {
                if (!tunnelValue.isObject())
                    continue;
                const NativeJsonDom::Object &tunnel = tunnelValue.object();
                ServerRecord::TunnelConfig config;
                config.mode = NativeJsonDom::stringValue(tunnel, "mode", "local");
                config.listenHost = NativeJsonDom::stringValue(tunnel, "listenHost");
                config.listenPort = NativeJsonDom::integerValue(tunnel, "listenPort", 0);
                config.targetHost = NativeJsonDom::stringValue(tunnel, "targetHost");
                config.targetPort = NativeJsonDom::integerValue(tunnel, "targetPort", 0);
                if (!config.targetHost.empty() && config.targetPort > 0
                    && config.listenPort >= 0 && config.listenPort <= 65535
                    && config.targetPort <= 65535)
                    merged.tunnels.push_back(config);
            }
        }
        // Credentials go to the Windows Credential Manager, never into the
        // config file, mirroring the local profile editor.  Fields with the
        // "enc:" prefix are DPAPI ciphertext (T2-2); a blob encrypted on a
        // different device cannot be decrypted and is skipped (counted for
        // the UI hint) instead of failing the whole import.  Legacy
        // plaintext values are still accepted (T2-1 backward compatibility).
        const NativeString address = nativeText(merged.address);
        const auto secretOrEmpty = [&undecryptableSecrets](
            const std::string &value) {
            if (value.empty())
                return std::string();
            if (!NativeCrypto::isEncrypted(value))
                return value;
            std::string error;
            const std::string plain = NativeCrypto::unprotectBase64(
                value.substr(std::string(NativeCrypto::EncryptedPrefix).size()),
                error);
            if (plain.empty()) {
                ++undecryptableSecrets;
                return std::string();
            }
            return plain;
        };
        const std::string password = secretOrEmpty(
            NativeJsonDom::stringValue(server, "password"));
        if (!password.empty())
            saveStoredPassword(address, nativeText(password));
        const std::string keyPassphrase = secretOrEmpty(
            NativeJsonDom::stringValue(server, "keyPassphrase"));
        if (!keyPassphrase.empty())
            saveStoredCredential(
                NativeString("KeyPassphrase"), address, nativeText(keyPassphrase));
        const NativeString proxyJump = nativeText(merged.proxyJump);
        const std::string proxyPassword = secretOrEmpty(
            NativeJsonDom::stringValue(server, "proxyPassword"));
        if (!proxyJump.isEmpty() && !proxyPassword.empty())
            saveStoredCredential(
                NativeString("ProxyJump"), proxyJump, nativeText(proxyPassword));
        // Private key files: decrypt the synced payload into the local data
        // directory and point the profile at the written file.  An
        // undecryptable blob (other device) clears the synced path so the
        // UI asks the user to pick a local key again.
        const auto importKeyData = [](const std::string &data,
                                      const std::string &fallbackName,
                                      NativeString &keyError) {
            if (data.empty())
                return std::string();
            const std::string decoded = NativeBase64::decode(data);
            if (decoded.empty()) {
                keyError = NativeString("私钥数据无效");
                return std::string();
            }
            const std::filesystem::path keyDir =
                NativeDataDir::resolveDataRoot() / L"keys";
            std::error_code directoryError;
            std::filesystem::create_directories(keyDir, directoryError);
            if (directoryError) {
                keyError = NativeString("无法创建私钥目录");
                return std::string();
            }
            std::string baseName = fallbackName;
            const std::size_t slash = baseName.find_last_of("/\\");
            if (slash != std::string::npos)
                baseName = baseName.substr(slash + 1);
            std::string cleanName;
            for (char c : baseName) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
                    cleanName.push_back(c);
                }
            }
            if (cleanName.empty() || cleanName == "."
                || cleanName == ".." || cleanName.size() > 64)
                cleanName = "synced-key.pem";
            const std::string digest = NativeHash::sha256Hex(decoded);
            const std::filesystem::path target =
                keyDir / (digest.substr(0, 16) + "-" + cleanName);
            std::error_code existsError;
            if (!std::filesystem::exists(target, existsError)) {
                std::ofstream output(target, std::ios::binary | std::ios::trunc);
                if (!output) {
                    keyError = NativeString("无法写入私钥文件");
                    return std::string();
                }
                output.write(decoded.data(),
                             static_cast<std::streamsize>(decoded.size()));
                output.close();
            }
            return pathText(target).toStdString();
        };
        NativeString keyImportError;
        std::string syncedKey = secretOrEmpty(
            NativeJsonDom::stringValue(server, "keyData"));
        const std::string importedKeyPath = importKeyData(
            syncedKey, NativeJsonDom::stringValue(server, "keyPath"),
            keyImportError);
        if (!keyImportError.isEmpty()) {
            error = keyImportError;
            DiagnosticLog::write(
                "cloud-import-error", utf8Text(keyImportError));
            return NativeJsonDom::Value(NativeJsonDom::Object{});
        }
        if (!importedKeyPath.empty())
            merged.keyPath = importedKeyPath;
        else if (!NativeJsonDom::stringValue(server, "keyData").empty())
            // The synced blob existed but could not be decrypted on this
            // device; do not keep pointing at the source machine's path.
            merged.keyPath.clear();
        std::string syncedProxyKey = secretOrEmpty(
            NativeJsonDom::stringValue(server, "proxyKeyData"));
        const std::string importedProxyKeyPath = importKeyData(
            syncedProxyKey, NativeJsonDom::stringValue(server, "proxyKeyPath"),
            keyImportError);
        if (!keyImportError.isEmpty()) {
            error = keyImportError;
            return NativeJsonDom::Value(NativeJsonDom::Object{});
        }
        if (!importedProxyKeyPath.empty())
            merged.proxyKeyPath = importedProxyKeyPath;
        else if (!NativeJsonDom::stringValue(server, "proxyKeyData").empty())
            merged.proxyKeyPath.clear();
        if (isNew)
            records.push_back(merged);
        else
            *target = merged;
    }
    writeServerRecords(records);
    // 工作区顺序：未分配 -> 云端顺序 -> 本机独有；清理无连接且不在云端的空工作区。
    std::vector<std::string> orderedWorkspaces;
    for (const std::string &name : workspaces) {
        if (name == "未分配" || std::find(cloudWorkspaceOrder.begin(),
                cloudWorkspaceOrder.end(), name) != cloudWorkspaceOrder.end())
            orderedWorkspaces.push_back(name);
    }
    for (const std::string &name : cloudWorkspaceOrder) {
        if (std::find(orderedWorkspaces.begin(), orderedWorkspaces.end(), name)
            == orderedWorkspaces.end())
            orderedWorkspaces.push_back(name);
    }
    for (const std::string &name : workspaces) {
        if (name == "未分配"
            || std::find(orderedWorkspaces.begin(), orderedWorkspaces.end(),
                         name) != orderedWorkspaces.end())
            continue;
        orderedWorkspaces.push_back(name);
    }
    std::map<std::string, int> usage;
    for (const ServerRecord &record : records)
        ++usage[record.workspace];
    std::vector<std::string> finalWorkspaces;
    for (const std::string &name : orderedWorkspaces) {
        const bool cloudOwned = std::find(cloudWorkspaceOrder.begin(),
            cloudWorkspaceOrder.end(), name) != cloudWorkspaceOrder.end();
        if (name == "未分配" || cloudOwned || usage[name] > 0)
            finalWorkspaces.push_back(name);
    }
    writeWorkspaceNames(finalWorkspaces);
    DiagnosticLog::write(
        "cloud-import-ok",
        "servers=" + std::to_string(records.size())
            + " undecryptable=" + std::to_string(undecryptableSecrets));
    NativeJsonDom::Object result;
    result.values.emplace(
        "profiles", NativeJsonDom::Value(serverProfiles()));
    result.values.emplace(
        "undecryptableSecrets", static_cast<double>(undecryptableSecrets));
    return NativeJsonDom::Value(std::move(result));
}

bool WebViewBackend::replaceLocalCommandHistory(
    const NativeJsonDom::Object &params, NativeString &error) const
{
    const auto commands = params.values.find("commands");
    if (commands == params.values.end() || !commands->second.isArray()) {
        error = NativeString("同步数据缺少历史命令列表");
        return false;
    }
    std::vector<std::string> lines;
    for (const NativeJsonDom::Value &value : commands->second.array().values) {
        if (!value.isString())
            continue;
        std::string line = value.string();
        while (!line.empty()
               && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    constexpr std::size_t maximumStoredCommands = 2000;
    if (lines.size() > maximumStoredCommands) {
        lines.erase(lines.begin(), lines.end()
            - static_cast<std::ptrdiff_t>(maximumStoredCommands));
    }
    const std::filesystem::path path = localCommandHistoryFilePath();
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = NativeString("无法创建本地历史命令目录：")
            + NativeString::fromStdString(directoryError.message());
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = NativeString("无法写入本地历史命令文件：") + pathText(path);
        return false;
    }
    for (const std::string &line : lines)
        output << line << '\n';
    return true;
}

void WebViewBackend::sendNativeResult(
    const NativeString &requestId, const NativeJsonDom::Value &result)
{
    if (!m_sendHandler)
        return;
    NativeJsonDom::Object response;
    response.values.emplace("id", utf8Text(requestId));
    response.values.emplace("ok", true);
    response.values.emplace("result", result);
    m_sendHandler(
        NativeJsonDom::stringify(NativeJsonDom::Value(std::move(response))));
}

void WebViewBackend::sendError(const NativeString &requestId, const NativeString &message)
{
    if (!m_sendHandler)
        return;
    NativeJsonDom::Object response;
    response.values.emplace("id", utf8Text(requestId));
    response.values.emplace("ok", false);
    response.values.emplace("error", utf8Text(message));
    m_sendHandler(
        NativeJsonDom::stringify(NativeJsonDom::Value(std::move(response))));
}

NativeJsonDom::Object WebViewBackend::appInfo() const
{
    NativeJsonDom::Object result;
    result.values.emplace("name", std::string("MasterTerm"));
    result.values.emplace("version", std::string(MASTERTERM_VERSION));
    result.values.emplace("transport", std::string("webview2-json"));
    return result;
}

NativeJsonDom::Array WebViewBackend::serverProfiles() const
{
    NativeJsonDom::Array result;
    const std::vector<ServerRecord> records = readServerRecords();
    for (int index = 0; index < static_cast<int>(records.size()); ++index) {
        const ServerRecord &record =
            records.at(static_cast<std::size_t>(index));
        // Credentials intentionally stay in the C++ process and are never serialized.
        // Only their lengths are exposed so the editor can show a truthful
        // masked placeholder without ever receiving the secret itself.
        const NativeString address = nativeText(record.address);
        const NativeString proxyJump = nativeText(record.proxyJump);
        const NativeString storedPasswordValue =
            nativeText(record.password).isEmpty()
                ? readStoredPassword(address)
                : nativeText(record.password);
        const NativeString storedKeyPassphraseValue =
            nativeText(record.keyPassphrase).isEmpty()
                ? readStoredCredential(
                    NativeString("KeyPassphrase"), address)
                : nativeText(record.keyPassphrase);
        const NativeString storedProxyPasswordValue =
            nativeText(record.proxyPassword).isEmpty() && !proxyJump.isEmpty()
                ? readStoredCredential(
                    NativeString("ProxyJump"), proxyJump)
                : nativeText(record.proxyPassword);
        NativeJsonDom::Object profile;
        profile.values.emplace("index", static_cast<double>(index));
        profile.values.emplace("connectionType", record.connectionType);
        profile.values.emplace("name", record.name);
        profile.values.emplace("address", record.address);
        profile.values.emplace("port", record.port);
        profile.values.emplace("tag", record.tag);
        profile.values.emplace("workspace", record.workspace);
        profile.values.emplace("color", record.color);
        profile.values.emplace("icon", record.iconKey);
        profile.values.emplace("keyPath", record.keyPath);
        profile.values.emplace("proxyJump", record.proxyJump);
        profile.values.emplace("proxyKeyPath", record.proxyKeyPath);
        profile.values.emplace(
            "passwordLength",
            static_cast<double>(storedPasswordValue.size()));
        profile.values.emplace(
            "keyPassphraseLength",
            static_cast<double>(storedKeyPassphraseValue.size()));
        profile.values.emplace(
            "proxyPasswordLength",
            static_cast<double>(storedProxyPasswordValue.size()));
        profile.values.emplace(
            "serialDataBits", static_cast<double>(record.serialDataBits));
        profile.values.emplace("serialParity", record.serialParity);
        profile.values.emplace("serialStopBits", record.serialStopBits);
        profile.values.emplace(
            "serialFlowControl", record.serialFlowControl);
        if (record.connectionType == "rdp")
            profile.values.emplace(
                "rdpOptions",
                NativeJsonDom::Value(rdpOptionsJson(record.rdpOptions)));
        NativeJsonDom::Array tunnels;
        for (const ServerRecord::TunnelConfig &config : record.tunnels) {
            NativeJsonDom::Object tunnel;
            tunnel.values.emplace("mode", config.mode);
            tunnel.values.emplace("listenHost", config.listenHost);
            tunnel.values.emplace(
                "listenPort", static_cast<double>(config.listenPort));
            tunnel.values.emplace("targetHost", config.targetHost);
            tunnel.values.emplace(
                "targetPort", static_cast<double>(config.targetPort));
            tunnels.values.emplace_back(std::move(tunnel));
        }
        profile.values.emplace(
            "tunnels", NativeJsonDom::Value(std::move(tunnels)));
        result.values.emplace_back(std::move(profile));
    }
    return result;
}
