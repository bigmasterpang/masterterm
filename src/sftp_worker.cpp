#include "SshLibrary.h"
#include "SshProxyRelay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <libssh2_sftp.h>

#include "NativeString.h"
#include "NativeDataDir.h"

#include <functional>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string utf8Bytes(const NativeString &value)
{
    return value.toUtf8();
}

std::string base64Encode(const std::string &value)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((value.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < value.size(); index += 3) {
        const unsigned int first = static_cast<unsigned char>(value[index]);
        const unsigned int second = index + 1 < value.size()
            ? static_cast<unsigned char>(value[index + 1]) : 0;
        const unsigned int third = index + 2 < value.size()
            ? static_cast<unsigned char>(value[index + 2]) : 0;
        const unsigned int combined = (first << 16) | (second << 8) | third;
        encoded.push_back(alphabet[(combined >> 18) & 0x3f]);
        encoded.push_back(alphabet[(combined >> 12) & 0x3f]);
        encoded.push_back(index + 1 < value.size() ? alphabet[(combined >> 6) & 0x3f] : '=');
        encoded.push_back(index + 2 < value.size() ? alphabet[combined & 0x3f] : '=');
    }
    return encoded;
}

std::string base64Encode(const NativeString &value)
{
    return base64Encode(utf8Bytes(value));
}

class Sha256 final {
public:
    Sha256() { reset(); }

    void reset()
    {
        state_[0] = 0x6a09e667u; state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u; state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu; state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu; state_[7] = 0x5be0cd19u;
        total_ = 0;
        bufferLen_ = 0;
    }

    void update(const char *data, std::size_t size)
    {
        total_ += static_cast<std::uint64_t>(size);
        while (size > 0) {
            const std::size_t take = (std::min)(
                size, 64 - bufferLen_);
            std::memcpy(buffer_ + bufferLen_, data, take);
            bufferLen_ += take;
            data += take;
            size -= take;
            if (bufferLen_ == 64) {
                processBlock(buffer_);
                bufferLen_ = 0;
            }
        }
    }

    std::string hexDigest() const
    {
        Sha256 copy(*this);
        copy.finalize();
        static constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (std::uint32_t word : copy.state_) {
            for (int shift = 28; shift >= 0; shift -= 4)
                result.push_back(hex[(word >> shift) & 0x0f]);
        }
        return result;
    }private:
    static std::uint32_t rotateRight(
        std::uint32_t value, unsigned int bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    void processBlock(const std::uint8_t block[64])
    {
        static constexpr std::uint32_t k[] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::uint32_t w[64];
        for (int index = 0; index < 16; ++index)
            w[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24)
                | (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16)
                | (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8)
                | static_cast<std::uint32_t>(block[index * 4 + 3]);
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotateRight(w[index - 15], 7)
                ^ rotateRight(w[index - 15], 18) ^ (w[index - 15] >> 3);
            const std::uint32_t s1 = rotateRight(w[index - 2], 17)
                ^ rotateRight(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2];
        std::uint32_t d = state_[3], e = state_[4], f = state_[5];
        std::uint32_t g = state_[6], h = state_[7];
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotateRight(e, 6)
                ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + k[index] + w[index];
            const std::uint32_t s0 = rotateRight(a, 2)
                ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    void finalize()
    {
        const std::uint64_t messageLength = total_;
        const std::uint64_t bitLength = messageLength * 8;
        std::uint8_t pad[64] = {0x80};
        std::size_t padLength = bufferLen_ < 56 ? 56 - bufferLen_ : 120 - bufferLen_;
        update(reinterpret_cast<const char *>(pad), padLength);
        for (int index = 0; index < 8; ++index)
            buffer_[63 - index] = static_cast<std::uint8_t>(bitLength >> (index * 8));
        processBlock(buffer_);
        bufferLen_ = 0;
    }

    std::uint32_t state_[8];
    std::uint64_t total_ = 0;
    std::size_t bufferLen_ = 0;
    std::uint8_t buffer_[64];
};

class Utf8Output final {
public:
    explicit Utf8Output(std::ostream &stream) : stream_(stream) {}
    template <typename T>
    Utf8Output &operator<<(const T &value)
    {
        stream_ << value;
        return *this;
    }

    Utf8Output &operator<<(const NativeString &value)
    {
        stream_ << value.toUtf8();
        return *this;
    }

    void flush() { stream_.flush(); }

private:
    std::ostream &stream_;
};

NativeString isoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    std::ostringstream formatted;
    formatted << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
    return NativeString::fromStdString(formatted.str());
}

NativeString environmentValue(const char *name, const char *fallback = nullptr)
{
    const char *value = std::getenv(name);
    if (!value)
        return fallback ? NativeString::fromUtf8(fallback) : NativeString();
    return NativeString::fromUtf8(value);
}

NativeString executableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        const std::size_t separator = buffer.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
            buffer.resize(separator);
        return NativeString::fromWCharArray(buffer.c_str());
    }
#endif
    return NativeString(".");
}

NativeString appDataDirectory()
{
    return NativeString::fromStdWString(NativeDataDir::resolveDataRoot().wstring());
}

std::vector<NativeString> splitString(const NativeString &value, char separator, bool skipEmpty = false)
{
    std::vector<NativeString> parts;
    int start = 0;
    while (start <= value.size()) {
        const int separatorIndex = value.indexOf(separator, start);
        const int end = separatorIndex < 0 ? value.size() : separatorIndex;
        const NativeString part = value.mid(start, end - start);
        if (!skipEmpty || !part.isEmpty()) parts.push_back(part);
        if (separatorIndex < 0) break;
        start = separatorIndex + 1;
    }
    return parts;
}

class NativeFileInfo final {
public:
    explicit NativeFileInfo(const NativeString &path)
        : path_(path), nativePath_(path.toStdWString()) {}

    static bool exists(const NativeString &path)
    {
        std::error_code error;
        return std::filesystem::exists(std::filesystem::path(path.toStdWString()), error);
    }

    bool exists() const
    {
        return NativeFileInfo::exists(path_);
    }

    NativeString absoluteFilePath() const
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(nativePath_, error);
        return error ? path_ : NativeString::fromStdWString(absolute.wstring());
    }

    NativeString absolutePath() const
    {
        return NativeFileInfo(absoluteFilePath()).path();
    }

    NativeString path() const
    {
        const auto parent = nativePath_.parent_path();
        return parent.empty() ? NativeString(".") : NativeString::fromStdWString(parent.wstring());
    }

    NativeString fileName() const
    {
        return NativeString::fromStdWString(nativePath_.filename().wstring());
    }

    NativeString suffix() const
    {
        const auto extension = nativePath_.extension().wstring();
        return extension.empty() ? NativeString() : NativeString::fromStdWString(extension.substr(1));
    }

    NativeString completeBaseName() const
    {
        return NativeString::fromStdWString(nativePath_.stem().wstring());
    }

    std::int64_t size() const
    {
        std::error_code error;
        const auto value = std::filesystem::file_size(nativePath_, error);
        return error ? 0 : static_cast<std::int64_t>(value);
    }

    bool isDir() const
    {
        std::error_code error;
        return std::filesystem::is_directory(nativePath_, error);
    }

    bool isFile() const
    {
        std::error_code error;
        return std::filesystem::is_regular_file(nativePath_, error);
    }

private:
    NativeString path_;
    std::filesystem::path nativePath_;
};

class NativeDirectory final {
public:
    NativeDirectory() = default;
    explicit NativeDirectory(const NativeString &path) : path_(path) {}

    bool mkpath(const NativeString &path) const
    {
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path.toStdWString()), error);
        return !error && std::filesystem::is_directory(std::filesystem::path(path.toStdWString()), error);
    }

    NativeString filePath(const NativeString &child) const
    {
        const auto joined = std::filesystem::path(path_.toStdWString())
            / std::filesystem::path(child.toStdWString());
        return NativeString::fromStdWString(joined.wstring());
    }

    static NativeString cleanPath(const NativeString &path)
    {
        std::vector<NativeString> parts;
        for (const NativeString &part : splitString(path, '/', true)) {
            if (part == NativeString(".")) continue;
            if (part == NativeString("..")) {
                if (!parts.empty() && parts.back() != NativeString("..")) parts.pop_back();
                else if (!path.startsWith('/')) parts.push_back(part);
                continue;
            }
            parts.push_back(part);
        }
        NativeString result;
        for (const NativeString &part : parts) {
            if (!result.isEmpty()) result += '/';
            result += part;
        }
        if (path.startsWith('/')) return '/' + result;
        return result.isEmpty() ? NativeString(".") : result;
    }

private:
    NativeString path_;
};

namespace NativeIODevice {
constexpr int ReadOnly = 0x0001;
constexpr int WriteOnly = 0x0002;
constexpr int Append = 0x0004;
constexpr int Truncate = 0x0008;
constexpr int Text = 0x0010;
}

namespace NativeFileDevice {
constexpr int DontCloseHandle = 0x0001;
}

class NativeFile final {
public:
    NativeFile() = default;
    explicit NativeFile(const NativeString &path) : path_(path) {}
    NativeFile(const NativeFile &) = delete;
    NativeFile &operator=(const NativeFile &) = delete;

    ~NativeFile() { close(); }

    bool open(const NativeString &path, int mode)
    {
        close();
        path_ = path;
        error_.clear();
        const auto nativePath = std::filesystem::path(path.toStdWString());
        if (mode & NativeIODevice::WriteOnly) {
            std::ios::openmode flags = std::ios::binary;
            flags |= (mode & NativeIODevice::Append) ? std::ios::app : std::ios::trunc;
            output_.open(nativePath, flags);
            if (!output_.is_open()) {
                setOpenError();
                return false;
            }
            writable_ = true;
            return true;
        }
        input_.open(nativePath, std::ios::binary);
        if (!input_.is_open()) {
            setOpenError();
            return false;
        }
        readable_ = true;
        return true;
    }

    bool open(int mode) { return open(path_, mode); }

    bool open(FILE *stream, int mode, int)
    {
        close();
        error_.clear();
        if (!stream || !(mode & NativeIODevice::ReadOnly)) {
            error_ = NativeString("无效的文件输入流");
            return false;
        }
        borrowedInput_ = stream;
        readable_ = true;
        return true;
    }

    bool isOpen() const { return readable_ || writable_; }

    std::int64_t read(char *buffer, std::int64_t maxSize)
    {
        if (!readable_ || !buffer || maxSize <= 0) return 0;
        if (borrowedInput_) {
            const std::size_t count = std::fread(buffer, 1, static_cast<std::size_t>(maxSize), borrowedInput_);
            if (count == 0 && std::ferror(borrowedInput_)) {
                error_ = NativeString("读取文件失败");
                return -1;
            }
            return static_cast<std::int64_t>(count);
        }
        input_.read(buffer, static_cast<std::streamsize>(maxSize));
        const std::streamsize count = input_.gcount();
        if (count == 0 && input_.bad()) {
            setReadError();
            return -1;
        }
        return static_cast<std::int64_t>(count);
    }

    std::int64_t write(const char *buffer, std::int64_t size)
    {
        if (!writable_ || !buffer || size < 0) return -1;
        output_.write(buffer, static_cast<std::streamsize>(size));
        if (!output_) {
            error_ = NativeString("写入文件失败");
            return -1;
        }
        return size;
    }

std::int64_t write(const std::string &data) { return write(data.data(), data.size()); }

    bool seek(std::int64_t position)
    {
        if (readable_) {
            input_.clear();
            input_.seekg(static_cast<std::streamoff>(position), std::ios::beg);
            return input_.good();
        }
        if (writable_ && !(output_.flags() & std::ios::app)) {
            output_.clear();
            output_.seekp(static_cast<std::streamoff>(position), std::ios::beg);
            return output_.good();
        }
        return false;
    }

    std::int64_t size() const
    {
        if (path_.isEmpty()) return 0;
        std::error_code error;
        const auto value = std::filesystem::file_size(
            std::filesystem::path(path_.toStdWString()), error);
        return error ? 0 : static_cast<std::int64_t>(value);
    }

    NativeString errorString() const
    {
        return error_.isEmpty() ? NativeString("文件操作失败") : error_;
    }

    void close()
    {
        if (input_.is_open()) input_.close();
        if (output_.is_open()) output_.close();
        borrowedInput_ = nullptr;
        readable_ = false;
        writable_ = false;
    }

private:
    void setOpenError()
    {
        error_ = NativeString("无法打开文件：") + NativeString::fromStdWString(
            std::filesystem::path(path_.toStdWString()).wstring());
    }

    void setReadError() { error_ = NativeString("读取文件失败"); }

    NativeString path_;
    NativeString error_;
    std::ifstream input_;
    std::ofstream output_;
    FILE *borrowedInput_ = nullptr;
    bool readable_ = false;
    bool writable_ = false;
};

void diagnosticLog(const NativeString &message)
{
    const std::vector<NativeString> paths = {
        appDataDirectory() + "/MasterSSH-sftp-worker.log",
        executableDirectory() + "/MasterSSH-sftp-worker.log"
    };
    for (const NativeString &path : paths) {
        NativeDirectory().mkpath(NativeFileInfo(path).absolutePath());
        NativeFile file(path);
        if (!file.open(NativeIODevice::WriteOnly | NativeIODevice::Append | NativeIODevice::Text)) continue;
        const std::string line = utf8Bytes(isoTimestamp() + NativeString("  ") + message + '\n');
        file.write(line);
    }
}

NativeString sessionError(LIBSSH2_SESSION *session)
{
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(session, &message, &length, 0);
    return message && length > 0 ? NativeString::fromUtf8(message, length) : NativeString("未知 SSH 错误");
}

bool sha256LocalFile(NativeFile &file, std::string &hex, NativeString &error)
{
    file.seek(0);
    Sha256 hasher;
    std::vector<char> buffer(65536, '\0');
    while (true) {
        const std::int64_t count = file.read(buffer.data(), buffer.size());
        if (count < 0) {
            error = "读取本地文件失败：" + file.errorString();
            return false;
        }
        if (count == 0) break;
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
    }
    hex = hasher.hexDigest();
    return true;
}

bool sha256RemoteFile(LIBSSH2_SESSION *session, LIBSSH2_SFTP *sftp,
                      const NativeString &remotePath, std::string &hex,
                      NativeString &error)
{
    const std::string remoteBytes = utf8Bytes(remotePath);
    LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open_ex(
        sftp, remoteBytes.c_str(), remoteBytes.size(),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!remote) {
        error = "无法打开远程文件校验：" + sessionError(session);
        return false;
    }
    Sha256 hasher;
    std::vector<char> buffer(65536, '\0');
    while (true) {
        const ssize_t count = libssh2_sftp_read(
            remote, buffer.data(), static_cast<size_t>(buffer.size()));
        if (count < 0) {
            error = "读取远程文件失败：" + sessionError(session);
            libssh2_sftp_close(remote);
            return false;
        }
        if (count == 0) break;
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
    }
    libssh2_sftp_close(remote);
    hex = hasher.hexDigest();
    return true;
}

int knownHostType(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

NativeString knownHostsPath()
{
    return appDataDirectory() + "/known_hosts";
}

NativeString conflictPolicy()
{
    const NativeString value = environmentValue("MASTERSSH_SFTP_CONFLICT");
    return value == "skip" || value == "rename" ? value : NativeString("overwrite");
}

bool remotePathExists(LIBSSH2_SFTP *sftp, const NativeString &path)
{
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const std::string bytes = utf8Bytes(path);
    return libssh2_sftp_stat_ex(sftp, bytes.c_str(), bytes.size(), LIBSSH2_SFTP_STAT, &attributes) == 0;
}

NativeString uniqueRemotePath(LIBSSH2_SFTP *sftp, const NativeString &path)
{
    if (!remotePathExists(sftp, path)) return path;
    const int slash = path.lastIndexOf('/');
    const NativeString directory = slash >= 0 ? path.left(slash + 1) : NativeString();
    const NativeString name = slash >= 0 ? path.mid(slash + 1) : path;
    const int dot = name.lastIndexOf('.');
    const NativeString stem = dot > 0 ? name.left(dot) : name;
    const NativeString suffix = dot > 0 ? name.mid(dot) : NativeString();
    for (int index = 1; index < 10000; ++index) {
        const NativeString candidate = directory + NativeString("%1 (%2)%3").arg(stem).arg(index).arg(suffix);
        if (!remotePathExists(sftp, candidate)) return candidate;
    }
    return path;
}

bool verifyHost(LIBSSH2_SESSION *session, const NativeString &host, int port, NativeString *error)
{
    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(session);
    if (!hosts) { *error = "无法初始化 SSH 主机密钥校验"; return false; }
    const NativeString filePath = knownHostsPath();
    const std::string fileName = utf8Bytes(filePath);
    if (NativeFileInfo::exists(filePath))
        libssh2_knownhost_readfile(hosts, fileName.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t keyLength = 0;
    int keyType = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    const char *key = libssh2_session_hostkey(session, &keyLength, &keyType);
    const NativeString knownHost = port == 22 ? host : NativeString("[%1]:%2").arg(host).arg(port);
    const std::string hostName = utf8Bytes(knownHost);
    const int flags = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownHostType(keyType);
    libssh2_knownhost *matched = nullptr;
    const int check = key ? libssh2_knownhost_check(hosts, hostName.c_str(), key, keyLength, flags, &matched)
                          : LIBSSH2_KNOWNHOST_CHECK_FAILURE;
    bool valid = check == LIBSSH2_KNOWNHOST_CHECK_MATCH;
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        valid = libssh2_knownhost_addc(hosts, hostName.c_str(), nullptr, key, keyLength,
                                       nullptr, 0, flags, nullptr) == 0;
        if (valid) {
            NativeDirectory().mkpath(NativeFileInfo(filePath).absolutePath());
            valid = libssh2_knownhost_writefile(hosts, fileName.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0;
        }
    }
    libssh2_knownhost_free(hosts);
    if (!valid)
        *error = check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH ? "SSH 主机密钥与已保存记录不一致" : "无法校验或保存 SSH 主机密钥";
    return valid;
}

int fail(const NativeString &message)
{
    Utf8Output out(std::cerr);
    out << message << '\n';
    return 1;
}

NativeString remoteJoinPath(const NativeString &base, const NativeString &child)
{
    if (child.isEmpty()) return base;
    return base.endsWith('/') ? base + child : base + '/' + child;
}

bool ensureRemoteDirectory(LIBSSH2_SFTP *sftp, const NativeString &path, NativeString *error)
{
    const NativeString cleanPath = NativeDirectory::cleanPath(path);
    if (cleanPath.isEmpty() || cleanPath == "." || cleanPath == "/") return true;
    const bool absolute = cleanPath.startsWith('/');
    NativeString current = absolute ? NativeString("/") : NativeString();
    for (const NativeString &part : splitString(cleanPath, '/', true)) {
        current = current.isEmpty() || current == "/" ? current + part : current + '/' + part;
        const std::string currentBytes = utf8Bytes(current);
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const int statResult = libssh2_sftp_stat_ex(sftp, currentBytes.c_str(), currentBytes.size(),
                                                    LIBSSH2_SFTP_STAT, &attributes);
        if (statResult == 0) {
            const bool isDirectory = !(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                || ((attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR);
            if (!isDirectory) {
                *error = NativeString("远程路径不是文件夹：%1").arg(current);
                return false;
            }
            continue;
        }
        if (libssh2_sftp_mkdir_ex(sftp, currentBytes.c_str(), currentBytes.size(), 0755) != 0) {
            // A concurrent operation may have created it. Confirm before
            // reporting an error instead of rejecting a harmless race.
            LIBSSH2_SFTP_ATTRIBUTES created{};
            if (libssh2_sftp_stat_ex(sftp, currentBytes.c_str(), currentBytes.size(),
                                     LIBSSH2_SFTP_STAT, &created) != 0) {
                *error = NativeString("无法创建远程文件夹 %1").arg(current);
                return false;
            }
        }
    }
    return true;
}

bool removeRemotePath(LIBSSH2_SFTP *sftp, const NativeString &path, NativeString *error)
{
    const std::string pathBytes = utf8Bytes(path);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (libssh2_sftp_stat_ex(sftp, pathBytes.c_str(), pathBytes.size(),
                             LIBSSH2_SFTP_LSTAT, &attributes) != 0) {
        *error = NativeString("远程项目不存在：%1").arg(path);
        return false;
    }
    const bool directory = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        && ((attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR);
    if (!directory) {
        if (libssh2_sftp_unlink_ex(sftp, pathBytes.c_str(), pathBytes.size()) != 0) {
            *error = NativeString("无法删除远程文件：%1").arg(path);
            return false;
        }
        return true;
    }

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(sftp, pathBytes.c_str());
    if (!handle) {
        *error = NativeString("无法读取远程文件夹：%1").arg(path);
        return false;
    }
    char name[4096];
    char longName[4096];
    LIBSSH2_SFTP_ATTRIBUTES childAttributes{};
    while (error->isEmpty()) {
        const int count = libssh2_sftp_readdir_ex(
            handle, name, sizeof(name), longName, sizeof(longName), &childAttributes);
        if (count <= 0)
            break;
        const NativeString childName = NativeString::fromUtf8(name, count);
        if (childName == NativeString(".") || childName == NativeString(".."))
            continue;
        if (!removeRemotePath(sftp, remoteJoinPath(path, childName), error))
            break;
    }
    libssh2_sftp_closedir(handle);
    if (!error->isEmpty())
        return false;
    if (libssh2_sftp_rmdir_ex(sftp, pathBytes.c_str(), pathBytes.size()) != 0) {
        *error = NativeString("无法删除远程文件夹：%1").arg(path);
        return false;
    }
    return true;
}

bool copyRemotePath(LIBSSH2_SFTP *sftp, const NativeString &sourcePath,
                    const NativeString &targetPath, NativeString *error)
{
    const std::string sourceBytes = utf8Bytes(sourcePath);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (libssh2_sftp_stat_ex(sftp, sourceBytes.c_str(), sourceBytes.size(),
                             LIBSSH2_SFTP_LSTAT, &attributes) != 0) {
        *error = NativeString("远程项目不存在：%1").arg(sourcePath);
        return false;
    }
    if (remotePathExists(sftp, targetPath)) {
        *error = NativeString("目标项目已存在：%1").arg(targetPath);
        return false;
    }
    const bool directory = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        && ((attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR);
    if (!directory) {
        const std::string targetBytes = utf8Bytes(targetPath);
        LIBSSH2_SFTP_HANDLE *source = libssh2_sftp_open_ex(
            sftp, sourceBytes.c_str(), sourceBytes.size(),
            LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        if (!source) {
            *error = NativeString("无法打开远程源文件：%1").arg(sourcePath);
            return false;
        }
        const long mode = attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS
            ? static_cast<long>(attributes.permissions & 0777) : 0644;
        LIBSSH2_SFTP_HANDLE *target = libssh2_sftp_open_ex(
            sftp, targetBytes.c_str(), targetBytes.size(),
            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
            mode, LIBSSH2_SFTP_OPENFILE);
        if (!target) {
            libssh2_sftp_close(source);
            *error = NativeString("无法创建远程目标文件：%1").arg(targetPath);
            return false;
        }
        char buffer[65536];
        while (error->isEmpty()) {
            const ssize_t count = libssh2_sftp_read(source, buffer, sizeof(buffer));
            if (count < 0) {
                *error = NativeString("读取远程源文件失败：%1").arg(sourcePath);
                break;
            }
            if (count == 0)
                break;
            ssize_t written = 0;
            while (written < count) {
                const ssize_t result = libssh2_sftp_write(
                    target, buffer + written, static_cast<size_t>(count - written));
                if (result <= 0) {
                    *error = NativeString("写入远程目标文件失败：%1").arg(targetPath);
                    break;
                }
                written += result;
            }
        }
        libssh2_sftp_close(target);
        libssh2_sftp_close(source);
        return error->isEmpty();
    }

    const std::string targetBytes = utf8Bytes(targetPath);
    const long mode = attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS
        ? static_cast<long>(attributes.permissions & 0777) : 0755;
    if (libssh2_sftp_mkdir_ex(
            sftp, targetBytes.c_str(), targetBytes.size(), mode) != 0) {
        *error = NativeString("无法创建远程目标文件夹：%1").arg(targetPath);
        return false;
    }
    LIBSSH2_SFTP_HANDLE *directoryHandle =
        libssh2_sftp_opendir(sftp, sourceBytes.c_str());
    if (!directoryHandle) {
        *error = NativeString("无法读取远程源文件夹：%1").arg(sourcePath);
        return false;
    }
    char name[4096];
    char longName[4096];
    LIBSSH2_SFTP_ATTRIBUTES childAttributes{};
    while (error->isEmpty()) {
        const int count = libssh2_sftp_readdir_ex(
            directoryHandle, name, sizeof(name), longName, sizeof(longName),
            &childAttributes);
        if (count <= 0)
            break;
        const NativeString childName = NativeString::fromUtf8(name, count);
        if (childName == NativeString(".") || childName == NativeString(".."))
            continue;
        copyRemotePath(sftp, remoteJoinPath(sourcePath, childName),
                       remoteJoinPath(targetPath, childName), error);
    }
    libssh2_sftp_closedir(directoryHandle);
    return error->isEmpty();
}

struct UploadTreeFile {
    NativeString localPath;
    NativeString relativePath;
};

struct UploadTreePlan {
    std::vector<NativeString> directories;
    std::vector<UploadTreeFile> files;
};

bool collectUploadTree(const NativeString &sourcePath, UploadTreePlan *plan, NativeString *error)
{
    const NativeFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        *error = NativeString("本地路径不存在：%1").arg(sourcePath);
        return false;
    }
    if (sourceInfo.isFile()) {
        plan->files.push_back({sourceInfo.absoluteFilePath(), sourceInfo.fileName()});
        return true;
    }
    if (!sourceInfo.isDir()) {
        *error = NativeString("不支持的本地路径：%1").arg(sourcePath);
        return false;
    }
    const NativeString rootName = sourceInfo.fileName();
    if (rootName.isEmpty()) {
        *error = NativeString("无法确定文件夹名称：%1").arg(sourcePath);
        return false;
    }
    plan->directories.push_back(rootName);
    const std::filesystem::path rootPath(sourceInfo.absoluteFilePath().toStdWString());
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        rootPath, std::filesystem::directory_options::skip_permission_denied, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(iteratorError)) {
        if (iteratorError) {
            *error = NativeString("无法读取本地目录：%1").arg(NativeString::fromStdString(iteratorError.message()));
            return false;
        }
        const NativeString localPath = NativeString::fromStdWString(iterator->path().wstring());
        const NativeFileInfo info(localPath);
        const auto relative = std::filesystem::relative(iterator->path(), rootPath, iteratorError);
        if (iteratorError) {
            *error = NativeString("无法计算本地相对路径：%1").arg(NativeString::fromStdString(iteratorError.message()));
            return false;
        }
        const NativeString relativePath = rootName + '/' + NativeString::fromStdWString(relative.generic_wstring());
        if (info.isDir()) plan->directories.push_back(relativePath);
        else if (info.isFile()) plan->files.push_back({info.absoluteFilePath(), relativePath});
    }
    return true;
}

std::atomic<bool> g_transferPaused{false};
std::atomic<bool> g_transferControlClosed{false};

// The host drives a transfer pause through the worker stdin.  Only commands
// that do not consume stdin for data start this reader thread.
void transferControlReader()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "pause") g_transferPaused = true;
        else if (line == "resume") g_transferPaused = false;
    }
    g_transferControlClosed = true;
}

// Blocks while the host paused this transfer.  Returns false when the control
// pipe was closed while paused, which only happens after the host abandoned
// the worker; abort the transfer instead of spinning forever.
bool transferPausePoint()
{
    while (g_transferPaused.load()) {
        if (g_transferControlClosed.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool transferPausableCommand(const NativeString &command)
{
    return command == "--upload" || command == "--download"
        || command == "--download-dir" || command == "--upload-many"
        || command == "--upload-tree";
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    std::vector<NativeString> args;
    args.reserve(argc);
    for (int index = 0; index < argc; ++index) {
#ifdef _WIN32
        // WebViewBackend starts this worker with CreateProcessW.  Using the
        // narrow CRT argv path re-encodes non-ASCII file names through the
        // active ANSI code page and corrupts UTF-8 SFTP paths.
        args.push_back(NativeString::fromStdWString(argv[index]));
#else
        args.push_back(NativeString::fromLocal8Bit(argv[index]));
#endif
    }
    if (args.size() < 2 || (args.at(1) != "--list" && args.at(1) != "--read-history"
        && args.at(1) != "--monitor"
        && args.at(1) != "--download" && args.at(1) != "--download-dir"
        && args.at(1) != "--preview"
        && args.at(1) != "--upload" && args.at(1) != "--upload-stream"
        && args.at(1) != "--upload-many"
        && args.at(1) != "--upload-tree" && args.at(1) != "--mkdir"
        && args.at(1) != "--mkdir-p"
        && args.at(1) != "--touch" && args.at(1) != "--remove"
        && args.at(1) != "--rename" && args.at(1) != "--copy"
        && args.at(1) != "--chmod")
        || (args.at(1) == "--list" && args.size() != 6)
        || (args.at(1) == "--read-history" && args.size() != 6)
        || (args.at(1) == "--monitor" && args.size() != 6)
        || ((args.at(1) == "--mkdir" || args.at(1) == "--mkdir-p"
             || args.at(1) == "--touch"
             || args.at(1) == "--remove") && args.size() != 6)
        || ((args.at(1) == "--rename" || args.at(1) == "--copy"
             || args.at(1) == "--chmod") && args.size() != 7)
        || ((args.at(1) == "--download" || args.at(1) == "--upload")
             && args.size() != 7 && args.size() != 8)
        || (args.at(1) == "--download-dir" && args.size() != 7)
        || (args.at(1) == "--preview" && args.size() != 7)
        || (args.at(1) == "--upload-stream" && args.size() != 7)
        || ((args.at(1) == "--upload-many" || args.at(1) == "--upload-tree") && args.size() < 7))
        return fail("用法：MasterTermSftpWorker --list|--download|--upload|--preview|--mkdir|--touch|--remove|--rename|--copy|--chmod 用户名@主机 端口 源路径 目标路径/私钥路径");
    if (!masterSshInitializeLibraries())
        return fail("无法初始化 libssh2");

    const NativeString connection = args.at(2);
    const int at = connection.indexOf('@');
    const NativeString user = at > 0 ? connection.left(at) : NativeString();
    const NativeString host = at > 0 ? connection.mid(at + 1) : NativeString();
    const int port = nativeBound(1, args.at(3).toInt(), 65535);
    const NativeString command = args.at(1);
    diagnosticLog(NativeString("start command=%1 connection=%2 port=%3 path=%4")
                  .arg(command, connection, args.at(3), args.at(4)));
    const NativeString sourcePath = args.at(4);
    const NativeString privateKeyPath =
        (command == "--list" || command == "--read-history" || command == "--monitor"
         || command == "--mkdir" || command == "--mkdir-p"
         || command == "--touch" || command == "--remove")
        ? args.at(5)
        : ((command == "--upload-many" || command == "--upload-tree"
            || command == "--upload-stream") ? args.at(5) : args.at(6));
    const std::int64_t resumeOffset = [&args]() -> std::int64_t {
        if (args.size() < 8)
            return 0;
        bool ok = false;
        const long long value = args.at(7).toLongLong(&ok);
        return ok && value > 0 ? value : 0;
    }();
    const NativeString password = environmentValue("MASTERSSH_SFTP_PASSWORD");
    const NativeString proxyJump = environmentValue("MASTERSSH_PROXY_JUMP");
    const NativeString proxyPassword = environmentValue("MASTERSSH_PROXY_PASSWORD");
    const NativeString proxyKeyPath = environmentValue("MASTERSSH_PROXY_KEY_PATH");
    diagnosticLog(NativeString("proxy=%1 proxyPassword=%2 proxyKey=%3 targetKey=%4")
                  .arg(proxyJump.isEmpty() ? "none" : proxyJump,
                       proxyPassword.isEmpty() ? "no" : "yes",
                       proxyKeyPath.isEmpty() ? "no" : "yes",
                       privateKeyPath.isEmpty() ? "no" : "yes"));
    if (user.isEmpty() || host.isEmpty()) return fail("服务器地址必须是“用户名@主机”");
    if (transferPausableCommand(command))
        std::thread(transferControlReader).detach();

#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_SFTP *sftp = nullptr;
    LIBSSH2_SFTP_HANDLE *directory = nullptr;
    NativeString error;
    SshProxyRelay proxyRelay;
    NativeString proxyError;
    std::uint16_t proxyPort = 0;
    if (!proxyJump.trimmed().isEmpty()) {
        std::mutex proxyMutex;
        std::condition_variable proxyCondition;
        bool proxyReady = false;
        if (!proxyRelay.start(proxyJump.toStdString(), host.toStdString(), port,
                              proxyPassword.toStdString(), proxyKeyPath.toStdString(),
                              [&](std::uint16_t localPort,
                                  const std::string &message) {
                                  std::lock_guard<std::mutex> lock(proxyMutex);
                                  proxyPort = localPort;
                                  proxyError = NativeString::fromUtf8(
                                      message.data(),
                                      static_cast<int>(message.size()));
                                  proxyReady = true;
                                  proxyCondition.notify_all();
        })) {
            error = "无法启动内置 SFTP 跳板机转发器";
            diagnosticLog(error);
        } else {
            std::unique_lock<std::mutex> lock(proxyMutex);
            if (!proxyReady && proxyCondition.wait_for(
                    lock, std::chrono::seconds(15), [&] { return proxyReady; }) == false)
                error = "SFTP 跳板机连接超时";
            else if (!proxyReady) error = "SFTP 跳板机连接超时";
            else if (!proxyError.isEmpty()) error = proxyError;
            else if (proxyPort == 0) error = "SFTP 跳板机未建立目标通道";
            diagnosticLog(NativeString("proxy ready=%1 localPort=%2 error=%3")
                          .arg(proxyReady ? "yes" : "no").arg(proxyPort).arg(proxyError));
        }
    }
    const NativeString tcpHost = proxyJump.trimmed().isEmpty() ? host : NativeString("127.0.0.1");
    const int tcpPort = proxyJump.trimmed().isEmpty() ? port : proxyPort;
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *addresses = nullptr;
    const std::string hostBytes = utf8Bytes(tcpHost);
    const std::string service = std::to_string(tcpPort);
    if (error.isEmpty() && getaddrinfo(hostBytes.c_str(), service.c_str(), &hints, &addresses) != 0)
        error = "无法解析服务器地址";
    for (addrinfo *address = addresses; error.isEmpty() && address; address = address->ai_next) {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == INVALID_SOCKET) continue;
        if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
        closesocket(socket);
        socket = INVALID_SOCKET;
    }
    if (addresses) freeaddrinfo(addresses);
    if (error.isEmpty() && socket == INVALID_SOCKET) error = "无法建立 SFTP TCP 连接";
    diagnosticLog(NativeString("target tcp=%1:%2 socket=%3 error=%4")
                  .arg(tcpHost).arg(tcpPort).arg(socket == INVALID_SOCKET ? "invalid" : "connected").arg(error));
    if (error.isEmpty()) {
        session = libssh2_session_init();
        if (!session) error = "无法创建 libssh2 会话";
        else {
            libssh2_session_set_blocking(session, 1);
            if (libssh2_session_handshake(session, socket) != 0) error = sessionError(session);
        }
        diagnosticLog(NativeString("ssh handshake error=%1").arg(error));
    }
    if (error.isEmpty()) verifyHost(session, host, port, &error);
    diagnosticLog(NativeString("host key verify error=%1").arg(error));
    if (error.isEmpty()) {
        const std::string userBytes = utf8Bytes(user);
        const std::string passwordBytes = utf8Bytes(password);
        const std::string keyBytes = utf8Bytes(privateKeyPath);
        const int result = privateKeyPath.isEmpty()
            ? libssh2_userauth_password_ex(session, userBytes.c_str(), userBytes.size(), passwordBytes.c_str(), passwordBytes.size(), nullptr)
            : libssh2_userauth_publickey_fromfile_ex(session, userBytes.c_str(), userBytes.size(), nullptr, keyBytes.c_str(),
                                                      passwordBytes.empty() ? nullptr : passwordBytes.c_str());
        if (result != 0) error = "SFTP 身份验证失败：" + sessionError(session);
        diagnosticLog(NativeString("target auth result=%1 error=%2").arg(result).arg(error));
    }
    if (error.isEmpty() && command != "--read-history" && command != "--monitor"
        && !(sftp = libssh2_sftp_init(session))) {
        error = "无法初始化 SFTP：" + sessionError(session);
    }
    diagnosticLog(NativeString("sftp init=%1 error=%2").arg(sftp ? "ok" : "skipped", error));
    const NativeString policy = conflictPolicy();
    const bool verifyChecksum =
        environmentValue("MASTERSSH_VERIFY_CHECKSUM") == "1";
    const bool resumeTree =
        environmentValue("MASTERSSH_RESUME_TREE") == "1";
    const auto localRemoteChecksumMatches =
        [&](const NativeString &localPath, const NativeString &remotePath) {
            NativeFile local(localPath);
            std::string localHash;
            std::string remoteHash;
            NativeString hashError;
            if (!local.open(NativeIODevice::ReadOnly)) {
                error = "无法打开本地文件校验：" + local.errorString();
                return false;
            }
            if (!sha256LocalFile(local, localHash, hashError)
                || !sha256RemoteFile(session, sftp, remotePath,
                                     remoteHash, hashError)) {
                error = hashError;
                return false;
            }
            diagnosticLog(NativeString("checksum tree local=")
                          + NativeString(localHash.c_str())
                          + NativeString(" remote=")
                          + NativeString(remoteHash.c_str()));
            return localHash == remoteHash;
        };
    if (error.isEmpty() && command == "--monitor") {
        // Keep this separate from the interactive terminal channel.  The
        // monitor emits a compact sample every two seconds and must never
        // consume input or write escape sequences into the user's shell.
        // Keep the remote command deliberately simple.  Previous variants
        // nested awk/base64 shell scripts and some login shells accepted the
        // command but emitted no output.  The host parses these raw Linux
        // files locally, so this only depends on /proc, df and a POSIX shell.
        const std::string monitorCommand =
            "while :; do cat /proc/stat /proc/meminfo /proc/net/dev /proc/uptime; "
            "df -P -B1 -x tmpfs -x devtmpfs 2>/dev/null; "
            "echo __MASTERTERM_SAMPLE_END__; sleep 2; done";
        LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
        const int executeResult = channel
            ? libssh2_channel_exec(channel, monitorCommand.c_str()) : -1;
        diagnosticLog(NativeString("monitor channel=%1 execute=%2")
                      .arg(channel ? "ok" : "failed").arg(executeResult));
        if (!channel || executeResult != 0) {
            error = "无法启动服务器状态监控：" + sessionError(session);
        } else {
            char buffer[8192];
            while (true) {
                const ssize_t count = libssh2_channel_read(channel, buffer, sizeof(buffer));
                if (count > 0) {
                    std::cout.write(buffer, count);
                    std::cout.flush();
                    continue;
                }
                if (count == LIBSSH2_ERROR_EAGAIN) continue;
                break;
            }
        }
        if (channel) {
            libssh2_channel_close(channel);
            libssh2_channel_free(channel);
        }
        if (session) libssh2_session_free(session);
        if (socket != INVALID_SOCKET) closesocket(socket);
        return error.isEmpty() ? 0 : fail(error);
    }
    if (error.isEmpty() && command == "--read-history") {
        const int historyLimit = nativeMax(0, environmentValue("MASTERSSH_HISTORY_LIMIT", "500").toInt());
        const int historyDays = nativeMax(0, environmentValue("MASTERSSH_HISTORY_DAYS", "0").toInt());
        const bool readBash = environmentValue("MASTERSSH_HISTORY_BASH", "1") != "0";
        const bool readZsh = environmentValue("MASTERSSH_HISTORY_ZSH", "1") != "0";
        const bool deduplicate = environmentValue("MASTERSSH_HISTORY_DEDUP", "1") != "0";
        const std::int64_t currentSeconds = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const std::int64_t cutoff = historyDays > 0
            ? currentSeconds - static_cast<std::int64_t>(historyDays) * 86400
            : 0;
        std::vector<NativeString> historyCommands;
        std::ofstream historyOutputFile;
        const NativeString historyOutputPath = environmentValue("MASTERSSH_HISTORY_OUTPUT_FILE");
        if (!historyOutputPath.isEmpty()) {
            historyOutputFile.open(
                std::filesystem::path(historyOutputPath.toStdWString()),
                std::ios::binary | std::ios::trunc);
        }
        std::ostream &historyOutput = historyOutputFile.is_open()
            ? static_cast<std::ostream &>(historyOutputFile) : std::cout;
        Utf8Output out(historyOutput);
        std::string content;
        if (readBash || readZsh) {
            const auto shellQuote = [](const NativeString &path) {
                std::string quoted = "'";
                for (char character : utf8Bytes(path)) {
                    if (character == '\'') quoted += "'\"'\"'";
                    else quoted += character;
                }
                return quoted + "'";
            };
            std::vector<std::string> historyCommands;
            if (readBash) {
                historyCommands.push_back("cat -- " + shellQuote(
                    remoteJoinPath(sourcePath, ".bash_history")) + " 2>/dev/null");
            }
            if (readZsh) {
                // HISTFILE is frequently customized by .zshrc, so querying zsh
                // itself is more reliable than assuming ~/.zsh_history.
                historyCommands.push_back("zsh -ic 'history_file=${HISTFILE:-$HOME/.zsh_history}; "
                    "[ -r \"$history_file\" ] && cat -- \"$history_file\"' 2>/dev/null");
            }
            std::string commandLine;
            for (const std::string &historyCommand : historyCommands) {
                if (!commandLine.empty()) commandLine += "; ";
                commandLine += historyCommand;
            }
            LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
            if (!channel || libssh2_channel_exec(channel, commandLine.c_str()) != 0) {
                error = "无法读取远端历史：" + sessionError(session);
            } else {
                constexpr std::size_t maxHistoryBytes = 2 * 1024 * 1024;
                char buffer[8192];
                while (content.size() < maxHistoryBytes) {
                    const std::size_t remaining = maxHistoryBytes - content.size();
                    const ssize_t count = libssh2_channel_read(channel, buffer,
                        (std::min)(sizeof(buffer), remaining));
                    if (count <= 0) break;
                    content.append(buffer, static_cast<std::size_t>(count));
                }
            }
            if (channel) {
                libssh2_channel_close(channel);
                libssh2_channel_free(channel);
            }
            diagnosticLog(NativeString("history read bytes=%1").arg(
                static_cast<unsigned long long>(content.size())));
        }
        if (error.isEmpty()) {
            std::int64_t entryTimestamp = 0;
            for (const NativeString &line : splitString(
                     NativeString::fromUtf8(content.data(), static_cast<int>(content.size())), '\n')) {
                NativeString commandLine = line.trimmed();
                if (commandLine.isEmpty()) continue;
                if (commandLine.startsWith('#') && commandLine.mid(1).trimmed().toLongLong() > 0) {
                    entryTimestamp = commandLine.mid(1).trimmed().toLongLong();
                    continue;
                }
                if (commandLine.startsWith(": ")) {
                    const int separator = commandLine.indexOf(';');
                    if (separator >= 0) {
                        entryTimestamp = commandLine.mid(2, separator - 2).trimmed().toLongLong();
                        commandLine = commandLine.mid(separator + 1).trimmed();
                    }
                }
                if (commandLine.isEmpty()) continue;
                if (cutoff > 0 && entryTimestamp > 0 && entryTimestamp < cutoff) continue;
                historyCommands.push_back(commandLine);
            }
        }
        if (deduplicate) {
            std::set<NativeString> seen;
            std::vector<NativeString> unique;
            for (int index = historyCommands.size() - 1; index >= 0; --index) {
                const NativeString value = historyCommands.at(static_cast<std::size_t>(index));
                if (!seen.insert(value).second) continue;
                unique.insert(unique.begin(), value);
            }
            historyCommands = unique;
        }
        if (historyLimit > 0 && historyCommands.size() > historyLimit)
            historyCommands.erase(
                historyCommands.begin(),
                historyCommands.end() - historyLimit);
        diagnosticLog(NativeString("history parsed commands=%1").arg(
            static_cast<unsigned long long>(historyCommands.size())));
        out << "M\t" << content.size() << '\t' << historyCommands.size()
            << '\t' << base64Encode(remoteJoinPath(sourcePath, ".zsh_history")) << '\n';
        for (const NativeString &commandLine : std::as_const(historyCommands))
            out << "H\t" << base64Encode(commandLine) << '\n';
        out.flush();
        if (historyOutputFile.is_open()) historyOutputFile.close();
        // libssh2 can block while tearing down a channel after the complete
        // history has already been written to stdout. This worker is a
        // one-shot helper, so let process termination reclaim its resources.
        if (socket != INVALID_SOCKET) closesocket(socket);
        return error.isEmpty() ? 0 : fail(error);
    }
    NativeString listingPath = sourcePath;
    if (error.isEmpty() && command == "--list") {
        if (sourcePath == NativeString(".")) {
            char resolvedPath[4096] = {};
            const int resolvedLength = libssh2_sftp_realpath(
                sftp, ".", resolvedPath, sizeof(resolvedPath) - 1);
            if (resolvedLength > 0)
                listingPath = NativeString::fromUtf8(resolvedPath, resolvedLength);
        }
        const std::string pathBytes = utf8Bytes(listingPath);
        directory = libssh2_sftp_opendir(sftp, pathBytes.c_str());
        if (!directory) error = "无法读取目录：" + sessionError(session);
        diagnosticLog(NativeString("list path=%1 handle=%2 error=%3").arg(listingPath, directory ? "ok" : "failed", error));
    }
    if (error.isEmpty() && command == "--list") {
        auto readRemoteTextFile = [sftp](const char *path) {
            std::string content;
            LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, path, LIBSSH2_FXF_READ, 0);
            if (!handle) return content;
            char buffer[4096];
            while (true) {
                const ssize_t count = libssh2_sftp_read(handle, buffer, sizeof(buffer));
                if (count <= 0) break;
                content.append(buffer, static_cast<std::size_t>(count));
            }
            libssh2_sftp_close(handle);
            return content;
        };
        std::map<std::uint32_t, NativeString> users;
        std::map<std::uint32_t, NativeString> groups;
        const std::string passwdContent = readRemoteTextFile("/etc/passwd");
        const std::string groupContent = readRemoteTextFile("/etc/group");
        for (const auto &line : splitString(
                 NativeString::fromUtf8(passwdContent.data(), static_cast<int>(passwdContent.size())), '\n', true)) {
            const auto parts = splitString(line, ':');
            if (parts.size() > 2) users[parts.at(2).toUInt()] = parts.at(0);
        }
        for (const auto &line : splitString(
                 NativeString::fromUtf8(groupContent.data(), static_cast<int>(groupContent.size())), '\n', true)) {
            const auto parts = splitString(line, ':');
            if (parts.size() > 2) groups[parts.at(2).toUInt()] = parts.at(0);
        }
        const auto userName = [](const std::map<std::uint32_t, NativeString> &values, std::uint32_t id) {
            const auto found = values.find(id);
            return found == values.end() ? NativeString::number(id) : found->second;
        };
        Utf8Output out(std::cout);
        out << "R\t" << base64Encode(listingPath) << '\n';
        char name[4096];
        char longName[4096];
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        while (true) {
            const int count = libssh2_sftp_readdir_ex(directory, name, sizeof(name), longName, sizeof(longName), &attributes);
            if (count <= 0) break;
            const std::string rawName(name, static_cast<std::size_t>(count));
            if (rawName == "." || rawName == "..") continue;
            const int fileType = attributes.permissions & LIBSSH2_SFTP_S_IFMT;
            const char kind = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && fileType == LIBSSH2_SFTP_S_IFDIR ? 'd'
                            : ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && fileType == LIBSSH2_SFTP_S_IFLNK ? 'l' : 'f');
            const std::int64_t size = static_cast<std::int64_t>(attributes.filesize);
            const std::int64_t modified = (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? static_cast<std::int64_t>(attributes.mtime) : -1;
            const NativeString permissions = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                ? NativeString::number(attributes.permissions & 07777, 8) : "—";
            const NativeString owner = (attributes.flags & LIBSSH2_SFTP_ATTR_UIDGID)
                ? NativeString("%1:%2").arg(userName(users, attributes.uid), userName(groups, attributes.gid)) : "—";
            out << base64Encode(rawName) << '\t' << kind << '\t' << size << '\t' << modified
                << '\t' << permissions << '\t' << owner << '\n';
        }
        // The parent consumes directory records while the worker is still
        // alive. The prior stream wrapper flushed when it left scope; the native
        // output adapter must preserve that behavior explicitly.
        out.flush();
    }
    if (error.isEmpty() && command == "--mkdir") {
        const std::string pathBytes = utf8Bytes(sourcePath);
        if (remotePathExists(sftp, sourcePath))
            error = NativeString("远程项目已存在：%1").arg(sourcePath);
        else if (libssh2_sftp_mkdir_ex(sftp, pathBytes.c_str(), pathBytes.size(), 0755) != 0)
            error = NativeString("无法创建远程文件夹：%1").arg(sourcePath);
    }
    if (error.isEmpty() && command == "--mkdir-p")
        ensureRemoteDirectory(sftp, sourcePath, &error);
    if (error.isEmpty() && command == "--touch") {
        const std::string pathBytes = utf8Bytes(sourcePath);
        if (remotePathExists(sftp, sourcePath)) {
            error = NativeString("远程项目已存在：%1").arg(sourcePath);
        } else {
            LIBSSH2_SFTP_HANDLE *file = libssh2_sftp_open_ex(
                sftp, pathBytes.c_str(), pathBytes.size(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
                0644, LIBSSH2_SFTP_OPENFILE);
            if (!file)
                error = NativeString("无法创建远程文件：%1").arg(sourcePath);
            else
                libssh2_sftp_close(file);
        }
    }
    if (error.isEmpty() && command == "--rename") {
        const NativeString targetPath = args.at(5);
        if (remotePathExists(sftp, targetPath)) {
            error = NativeString("目标项目已存在：%1").arg(targetPath);
        } else {
            const std::string sourceBytes = utf8Bytes(sourcePath);
            const std::string targetBytes = utf8Bytes(targetPath);
            if (libssh2_sftp_rename_ex(
                    sftp, sourceBytes.c_str(), sourceBytes.size(),
                    targetBytes.c_str(), targetBytes.size(),
                    LIBSSH2_SFTP_RENAME_OVERWRITE) != 0)
                error = NativeString("无法重命名远程项目：%1").arg(sourcePath);
        }
    }
    if (error.isEmpty() && command == "--copy")
        copyRemotePath(sftp, sourcePath, args.at(5), &error);
    if (error.isEmpty() && command == "--chmod") {
        bool modeOk = false;
        const unsigned int mode = args.at(5).toUInt(&modeOk, 8);
        const std::string pathBytes = utf8Bytes(sourcePath);
        LIBSSH2_SFTP_ATTRIBUTES current{};
        if (!modeOk || mode > 07777) {
            error = NativeString("权限格式无效：%1").arg(args.at(5));
        } else if (libssh2_sftp_stat_ex(
                       sftp, pathBytes.c_str(), pathBytes.size(),
                       LIBSSH2_SFTP_LSTAT, &current) != 0) {
            error = NativeString("远程项目不存在：%1").arg(sourcePath);
        } else {
            LIBSSH2_SFTP_ATTRIBUTES changed{};
            changed.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
            changed.permissions = (current.permissions & LIBSSH2_SFTP_S_IFMT) | mode;
            if (libssh2_sftp_setstat(
                    sftp, pathBytes.c_str(), &changed) != 0)
                error = NativeString("无法修改远程权限：%1").arg(sourcePath);
        }
    }
    if (error.isEmpty() && command == "--remove")
        removeRemotePath(sftp, sourcePath, &error);
if (error.isEmpty() && command == "--download") {
        NativeString localPath = args.at(5);
        const std::string remoteBytes = utf8Bytes(sourcePath);
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        std::int64_t total = 0;
        if (libssh2_sftp_stat_ex(sftp, remoteBytes.c_str(), remoteBytes.size(), LIBSSH2_SFTP_STAT, &attributes) == 0
            && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)) total = static_cast<std::int64_t>(attributes.filesize);
        const NativeFileInfo localInfo(localPath);
        bool resume = false;
        if (resumeOffset > 0 && resumeOffset < total
            && NativeFileInfo::exists(localPath) && !localInfo.isDir()
            && localInfo.size() == resumeOffset) {
            resume = true;
        } else if (NativeFileInfo::exists(localPath)) {
            if (policy == "skip") return 0;
            if (policy == "rename") {
                const NativeString suffix = localInfo.isDir() ? NativeString() : ("." + localInfo.suffix());
                for (int index = 1; index < 10000; ++index) {
                    const NativeString candidate = NativeDirectory(localInfo.absolutePath()).filePath(
                        NativeString("%1 (%2)%3").arg(localInfo.completeBaseName()).arg(index).arg(suffix));
                    if (!NativeFileInfo::exists(candidate)) { localPath = candidate; break; }
                }
            }
        }
        LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open_ex(sftp, remoteBytes.c_str(), remoteBytes.size(),
            LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        if (!remote) error = "无法打开远程文件：" + sessionError(session);
        NativeFile local(localPath);
        if (error.isEmpty()) {
            NativeDirectory().mkpath(NativeFileInfo(localPath).absolutePath());
            if (resume) {
                if (!local.open(NativeIODevice::WriteOnly | NativeIODevice::Append))
                    error = "无法打开本地文件继续下载：" + local.errorString();
                else
                    libssh2_sftp_seek64(
                        remote, static_cast<libssh2_uint64_t>(resumeOffset));
            } else if (!local.open(NativeIODevice::WriteOnly | NativeIODevice::Truncate)) error = "无法创建本地文件：" + local.errorString();
        }
        std::int64_t done = resume ? resumeOffset : 0;
        char buffer[65536];
        while (error.isEmpty()) {
            if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
            const ssize_t count = libssh2_sftp_read(remote, buffer, sizeof(buffer));
            if (count == 0) break;
            if (count < 0) { error = "下载读取失败：" + sessionError(session); break; }
            if (local.write(buffer, count) != count) { error = "写入本地文件失败：" + local.errorString(); break; }
            done += count;
            Utf8Output out(std::cout); out << "P\t" << done << '\t' << total << '\t'
                << base64Encode(NativeFileInfo(sourcePath).fileName()) << "\t1\t1\t"
                << done << '\t' << total << '\n'; out.flush();
        }
        local.close();
        if (remote) libssh2_sftp_close(remote);
        if (error.isEmpty() && done != total)
            error = "校验失败：下载文件大小不一致";
        if (error.isEmpty() && verifyChecksum) {
            NativeFile verifyLocal(localPath);
            std::string localHash, remoteHash;
            NativeString hashError;
            if (!verifyLocal.open(NativeIODevice::ReadOnly)) {
                error = "无法打开本地文件校验：" + verifyLocal.errorString();
            } else if (!sha256LocalFile(verifyLocal, localHash, hashError)
                || !sha256RemoteFile(session, sftp, sourcePath, remoteHash, hashError)) {
                error = hashError;
            } else {
                diagnosticLog(NativeString("checksum download local=")
                    + NativeString(localHash.c_str()) + NativeString(" remote=")
                    + NativeString(remoteHash.c_str()));
                if (localHash != remoteHash)
                    error = "校验失败：SHA-256 不一致";
            }
        }
    }
    if (error.isEmpty() && command == "--preview") {
        const std::string remoteBytes = utf8Bytes(sourcePath);
        std::int64_t previewBytes = 524288;
        bool parseOk = false;
        const int requested = args.at(5).toInt(&parseOk);
        if (parseOk && requested > 0)
            previewBytes = std::min<std::int64_t>(requested, 1048576);
        LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open_ex(
            sftp, remoteBytes.c_str(), remoteBytes.size(),
            LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        if (!remote)
            error = "无法打开远程文件：" + sessionError(session);
        else {
            std::vector<char> buffer(static_cast<std::size_t>(previewBytes));
            const ssize_t got = libssh2_sftp_read(
                remote, buffer.data(), buffer.size());
            if (got < 0) {
                error = "预览读取失败：" + sessionError(session);
            } else {
                Utf8Output out(std::cout);
                out << "B\t"
                    << base64Encode(std::string(
                        buffer.data(), static_cast<std::size_t>(got)))
                    << '\n';
                out.flush();
            }
            libssh2_sftp_close(remote);
        }
    }
    if (error.isEmpty() && command == "--download-dir") {
        struct RemoteFile { NativeString remotePath; NativeString relativePath; std::int64_t size = 0; };
        std::vector<RemoteFile> files;
        std::int64_t totalBytes = 0;
        std::function<bool(const NativeString &, const NativeString &)> collect;
        collect = [&](const NativeString &remoteDirectory, const NativeString &relativeDirectory) {
            const std::string directoryBytes = utf8Bytes(remoteDirectory);
            LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(sftp, directoryBytes.c_str());
            if (!handle) { error = "无法读取目录：" + sessionError(session); return false; }
            char name[4096]; char longName[4096]; LIBSSH2_SFTP_ATTRIBUTES attributes{};
            while (error.isEmpty()) {
                const int count = libssh2_sftp_readdir_ex(handle, name, sizeof(name), longName, sizeof(longName), &attributes);
                if (count <= 0) break;
                const NativeString fileName = NativeString::fromUtf8(name, count);
                if (fileName == "." || fileName == "..") continue;
                const NativeString childRemote = remoteDirectory.endsWith('/') ? remoteDirectory + fileName : remoteDirectory + "/" + fileName;
                const NativeString childRelative = relativeDirectory.isEmpty() ? fileName : relativeDirectory + "/" + fileName;
                const bool isDirectory = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                    && ((attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR);
                if (isDirectory) collect(childRemote, childRelative);
                else {
                    const std::int64_t size = (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) ? static_cast<std::int64_t>(attributes.filesize) : 0;
                    files.push_back({childRemote, childRelative, size});
                    totalBytes += size;
                }
            }
            libssh2_sftp_closedir(handle);
            return error.isEmpty();
        };
        collect(sourcePath, {});
        const NativeString destinationBase = args.at(5);
        NativeString rootName = environmentValue("MASTERSSH_DOWNLOAD_NAME");
        if (rootName.isEmpty()) rootName = NativeFileInfo(sourcePath.endsWith('/') ? sourcePath.left(sourcePath.size() - 1) : sourcePath).fileName();
        NativeString localRoot = NativeDirectory(destinationBase).filePath(rootName.isEmpty() ? "download" : rootName);
        if (NativeFileInfo::exists(localRoot)) {
            if (policy == "skip") return 0;
            if (policy == "rename") {
                for (int index = 1; index < 10000; ++index) {
                    const NativeString candidate = NativeDirectory(destinationBase).filePath(NativeString("%1 (%2)").arg(rootName).arg(index));
                    if (!NativeFileInfo::exists(candidate)) { localRoot = candidate; break; }
                }
            }
        }
        if (error.isEmpty() && !NativeDirectory().mkpath(localRoot)) error = "无法创建本地下载目录";
        std::int64_t doneBytes = 0;
        std::int64_t completedFiles = 0;
        for (const RemoteFile &file : files) {
            if (!error.isEmpty()) break;
            if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
            const NativeString localFilePath = NativeDirectory(localRoot).filePath(file.relativePath);
            NativeDirectory().mkpath(NativeFileInfo(localFilePath).absolutePath());
            const NativeFileInfo localInfo(localFilePath);
            std::int64_t resumeOffset = 0;
            bool skipExisting = false;
            if (resumeTree && localInfo.exists() && !localInfo.isDir()) {
                const std::int64_t localSize = localInfo.size();
                if (localSize == file.size) {
                    skipExisting = !verifyChecksum
                        || localRemoteChecksumMatches(localFilePath,
                                                       file.remotePath);
                } else if (localSize > 0 && localSize < file.size) {
                    resumeOffset = localSize;
                }
            }
            if (!error.isEmpty()) break;
            if (skipExisting) {
                doneBytes += file.size;
                ++completedFiles;
                Utf8Output out(std::cout);
                out << "D\t" << doneBytes << '\t' << totalBytes << '\t'
                    << completedFiles << '\t' << files.size() << '\t'
                    << base64Encode(file.relativePath) << '\t'
                    << file.size << '\t' << file.size << '\n';
                out.flush();
                continue;
            }
            const std::string remoteBytes = utf8Bytes(file.remotePath);
            LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open_ex(sftp, remoteBytes.c_str(), remoteBytes.size(), LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
            if (!remote) { error = "无法打开远程文件：" + sessionError(session); break; }
            if (resumeOffset > 0)
                libssh2_sftp_seek64(remote,
                                    static_cast<libssh2_uint64_t>(resumeOffset));
            NativeFile local(localFilePath);
            if (!local.open(NativeIODevice::WriteOnly
                            | (resumeOffset > 0
                                   ? NativeIODevice::Append
                                   : NativeIODevice::Truncate))) {
                error = "无法创建本地文件：" + local.errorString();
                libssh2_sftp_close(remote);
                break;
            }
            char buffer[65536];
            std::int64_t fileDone = resumeOffset;
            doneBytes += resumeOffset;
            while (error.isEmpty()) {
                if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
                const ssize_t count = libssh2_sftp_read(remote, buffer, sizeof(buffer));
                if (count == 0) break;
                if (count < 0) { error = "下载读取失败：" + sessionError(session); break; }
                if (local.write(buffer, count) != count) { error = "写入本地文件失败：" + local.errorString(); break; }
                doneBytes += count;
                fileDone += count;
                Utf8Output out(std::cout); out << "D\t" << doneBytes << '\t' << totalBytes << '\t' << completedFiles << '\t' << files.size() << '\t' << base64Encode(file.relativePath) << '\t' << fileDone << '\t' << file.size << '\n'; out.flush();
            }
            local.close();
            libssh2_sftp_close(remote);
            ++completedFiles;
            Utf8Output out(std::cout); out << "D\t" << doneBytes << '\t' << totalBytes << '\t' << completedFiles << '\t' << files.size() << '\t' << base64Encode(file.relativePath) << '\t' << fileDone << '\t' << file.size << '\n'; out.flush();
            if (error.isEmpty() && fileDone != file.size)
                error = "校验失败：下载文件大小不一致（" + file.relativePath + "）";
            if (error.isEmpty() && verifyChecksum
                && !localRemoteChecksumMatches(localFilePath,
                                                file.remotePath))
                error = "校验失败：SHA-256 不一致（" + file.relativePath + "）";
        }
    }
if (error.isEmpty() && command == "--upload") {
        NativeString remotePath = args.at(5);
        NativeFile local(sourcePath);
        if (!local.open(NativeIODevice::ReadOnly)) error = "无法读取本地文件：" + local.errorString();
        const std::int64_t total = local.size();
        bool resume = false;
        if (resumeOffset > 0 && error.isEmpty() && resumeOffset < total) {
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            const std::string probe = utf8Bytes(remotePath);
            if (libssh2_sftp_stat_ex(sftp, probe.c_str(), probe.size(), LIBSSH2_SFTP_STAT, &attributes) == 0
                && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
                && static_cast<std::int64_t>(attributes.filesize) == resumeOffset) {
                resume = true;
            }
        }
        if (error.isEmpty() && !resume) {
            if (remotePathExists(sftp, remotePath)) {
                if (policy == "skip") { local.close(); return 0; }
                if (policy == "rename") remotePath = uniqueRemotePath(sftp, remotePath);
            }
        }
        const std::string remoteBytes = utf8Bytes(remotePath);
        LIBSSH2_SFTP_HANDLE *remote = nullptr;
        if (error.isEmpty()) {
            remote = libssh2_sftp_open_ex(sftp, remoteBytes.c_str(), remoteBytes.size(),
                resume ? (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_APPEND)
                       : (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC), 0644, LIBSSH2_SFTP_OPENFILE);
            if (!remote) error = "无法创建远程文件：" + sessionError(session);
        }
        std::int64_t done = 0;
        if (resume && error.isEmpty()) {
            if (!local.seek(resumeOffset))
                error = "无法定位本地续传位置：" + local.errorString();
            else done = resumeOffset;
        }
        std::vector<char> buffer(65536, '\0');
        while (error.isEmpty()) {
            if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
            const std::int64_t count = local.read(buffer.data(), buffer.size());
            if (count == 0) break;
            if (count < 0) { error = "读取本地文件失败：" + local.errorString(); break; }
            std::int64_t written = 0;
            while (written < count) {
                const ssize_t result = libssh2_sftp_write(remote, buffer.data() + written, static_cast<size_t>(count - written));
                if (result <= 0) { error = "上传写入失败：" + sessionError(session); break; }
                written += result;
            }
            done += written;
            Utf8Output out(std::cout); out << "P\t" << done << '\t' << total << '\t'
                << base64Encode(NativeFileInfo(sourcePath).fileName()) << "\t1\t1\t"
                << done << '\t' << total << '\n'; out.flush();
        }
        local.close();
        if (remote) libssh2_sftp_close(remote);
        if (error.isEmpty()) {
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            const std::string statBytes = utf8Bytes(remotePath);
            if (libssh2_sftp_stat_ex(sftp, statBytes.c_str(), statBytes.size(), LIBSSH2_SFTP_STAT, &attributes) != 0
                || !(attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
                || static_cast<std::int64_t>(attributes.filesize) != total)
                error = "校验失败：上传文件大小不一致";
        }
        if (error.isEmpty() && verifyChecksum) {
            NativeFile verifyLocal(sourcePath);
            std::string localHash, remoteHash;
            NativeString hashError;
            if (!verifyLocal.open(NativeIODevice::ReadOnly)) {
                error = "无法打开本地文件校验：" + verifyLocal.errorString();
            } else if (!sha256LocalFile(verifyLocal, localHash, hashError)
                || !sha256RemoteFile(session, sftp, remotePath, remoteHash, hashError)) {
                error = hashError;
            } else {
                diagnosticLog(NativeString("checksum upload local=")
                    + NativeString(localHash.c_str()) + NativeString(" remote=")
                    + NativeString(remoteHash.c_str()));
                if (localHash != remoteHash)
                    error = "校验失败：SHA-256 不一致";
            }
        }
    }
    if (error.isEmpty() && command == "--upload-stream") {
        NativeString remotePath = sourcePath;
        if (remotePathExists(sftp, remotePath)) {
            if (policy == "skip") return 0;
            if (policy == "rename") remotePath = uniqueRemotePath(sftp, remotePath);
        }
        if (!ensureRemoteDirectory(sftp, NativeFileInfo(remotePath).path(), &error)) {
            // Keep the error from ensureRemoteDirectory.
        }
        const std::string remoteBytes = utf8Bytes(remotePath);
        LIBSSH2_SFTP_HANDLE *remote = nullptr;
        if (error.isEmpty()) {
            remote = libssh2_sftp_open_ex(
                sftp, remoteBytes.c_str(), remoteBytes.size(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                0644, LIBSSH2_SFTP_OPENFILE);
            if (!remote)
                error = "无法创建远程文件：" + sessionError(session);
        }
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        NativeFile input;
        if (error.isEmpty() && !input.open(
                stdin, NativeIODevice::ReadOnly, NativeFileDevice::DontCloseHandle))
            error = "无法读取上传数据流";
        const std::int64_t total = nativeMax<std::int64_t>(0, args.at(6).toLongLong());
        std::int64_t done = 0;
        std::vector<char> buffer(256 * 1024, '\0');
        while (error.isEmpty() && done < total) {
            const std::int64_t count = input.read(
                buffer.data(), nativeMin<std::int64_t>(buffer.size(), total - done));
            if (count <= 0) {
                error = done == total
                    ? NativeString() : NativeString("上传数据流提前结束");
                break;
            }
            std::int64_t written = 0;
            while (written < count) {
                const ssize_t result = libssh2_sftp_write(
                    remote, buffer.data() + written,
                    static_cast<size_t>(count - written));
                if (result <= 0) {
                    error = "上传写入失败：" + sessionError(session);
                    break;
                }
                written += result;
            }
            done += written;
            Utf8Output out(std::cout);
            out << "P\t" << done << '\t' << total << '\t'
                << base64Encode(NativeFileInfo(remotePath).fileName()) << '\n';
            out.flush();
        }
        if (error.isEmpty() && done == total) {
            // Do not exit merely because the announced byte count was
            // reached. Waiting for EOF makes the frontend's explicit finish
            // request the single owner of stream completion and prevents the
            // worker-finished/finish-request race.
            char extraByte = 0;
            const std::int64_t extra = input.read(&extraByte, 1);
            if (extra > 0)
                error = NativeString("上传数据超过声明大小");
        }
        input.close();
        if (remote) libssh2_sftp_close(remote);
    }
    if (error.isEmpty() && command == "--upload-many") {
        const NativeString targetDirectory = sourcePath;
        std::int64_t totalBytes = 0;
        for (int index = 6; index < args.size(); ++index)
            totalBytes += NativeFileInfo(args.at(index)).size();
        std::int64_t doneBytes = 0;
        const int fileCount = args.size() - 6;
        for (int index = 6; index < args.size() && error.isEmpty(); ++index) {
            if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
            const NativeString localPath = args.at(index);
            NativeFile local(localPath);
            if (!local.open(NativeIODevice::ReadOnly)) { error = "无法读取本地文件：" + local.errorString(); break; }
            NativeString remotePath = (targetDirectory.endsWith('/') ? targetDirectory : targetDirectory + "/") + NativeFileInfo(localPath).fileName();
            if (remotePathExists(sftp, remotePath)) {
                if (policy == "skip") { local.close(); continue; }
                if (policy == "rename") remotePath = uniqueRemotePath(sftp, remotePath);
            }
            const std::string remoteBytes = utf8Bytes(remotePath);
            LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open_ex(sftp, remoteBytes.c_str(), remoteBytes.size(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644, LIBSSH2_SFTP_OPENFILE);
            if (!remote) { error = "无法创建远程文件：" + sessionError(session); local.close(); break; }
            const std::int64_t fileTotal = local.size();
            std::int64_t fileDone = 0;
            std::vector<char> buffer(65536, '\0');
            while (error.isEmpty()) {
                if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
                const std::int64_t count = local.read(buffer.data(), buffer.size());
                if (count == 0) break;
                if (count < 0) { error = "读取本地文件失败：" + local.errorString(); break; }
                std::int64_t written = 0;
                while (written < count) {
                    const ssize_t result = libssh2_sftp_write(remote, buffer.data() + written, static_cast<size_t>(count - written));
                    if (result <= 0) { error = "上传写入失败：" + sessionError(session); break; }
                    written += result;
                }
                doneBytes += written;
                fileDone += written;
                Utf8Output out(std::cout); out << "P\t" << doneBytes << '\t' << totalBytes << '\t'
                    << base64Encode(NativeFileInfo(localPath).fileName()) << '\t' << (index - 5) << '\t' << fileCount
                    << '\t' << fileDone << '\t' << fileTotal << '\n'; out.flush();
            }
            local.close();
            libssh2_sftp_close(remote);
            if (local.size() == 0) {
                Utf8Output out(std::cout); out << "P\t" << doneBytes << '\t' << totalBytes << '\t'
                    << base64Encode(NativeFileInfo(localPath).fileName()) << '\t' << (index - 5) << '\t' << fileCount
                    << '\t' << fileDone << '\t' << fileTotal << '\n'; out.flush();
            }
        }
    }
    if (error.isEmpty() && command == "--upload-tree") {
        const NativeString targetDirectory = sourcePath;
        UploadTreePlan plan;
        for (int index = 6; index < args.size() && error.isEmpty(); ++index)
            collectUploadTree(args.at(index), &plan, &error);
        if (error.isEmpty() && policy == "rename") {
            std::map<NativeString, NativeString> renamedRoots;
            for (int index = 6; index < args.size(); ++index) {
                const NativeString rootName = NativeFileInfo(args.at(index)).fileName();
                if (rootName.isEmpty() || renamedRoots.find(rootName) != renamedRoots.end())
                    continue;
                const NativeString originalRemoteRoot =
                    remoteJoinPath(targetDirectory, rootName);
                if (remotePathExists(sftp, originalRemoteRoot)) {
                    renamedRoots[rootName] = NativeFileInfo(uniqueRemotePath(
                        sftp, originalRemoteRoot)).fileName();
                }
            }
            const auto remapRoot = [&renamedRoots](const NativeString &relativePath) {
                const int separator = relativePath.indexOf('/');
                const NativeString root = separator < 0
                    ? relativePath : relativePath.left(separator);
                const auto found = renamedRoots.find(root);
                if (found == renamedRoots.end() || found->second.isEmpty())
                    return relativePath;
                const NativeString &renamed = found->second;
                return separator < 0
                    ? renamed : renamed + relativePath.mid(separator);
            };
            for (NativeString &directory : plan.directories)
                directory = remapRoot(directory);
            for (UploadTreeFile &file : plan.files)
                file.relativePath = remapRoot(file.relativePath);
        }
        // Sync mode: skip files that already exist remotely with the same
        // size, so the operation uploads only new or changed files.
        const bool syncMode = environmentValue("MASTERSSH_SYNC_MODE") == "1";
        if (syncMode && !resumeTree && error.isEmpty()) {
            for (auto it = plan.files.begin(); it != plan.files.end();) {
                const NativeString remotePath = remoteJoinPath(
                    targetDirectory, it->relativePath);
                LIBSSH2_SFTP_ATTRIBUTES attributes{};
                const std::string remoteBytes = utf8Bytes(remotePath);
                const bool exists = libssh2_sftp_stat_ex(
                    sftp, remoteBytes.c_str(), remoteBytes.size(),
                    LIBSSH2_SFTP_STAT, &attributes) == 0;
                const std::int64_t localSize =
                    NativeFileInfo(it->localPath).size();
                if (exists
                    && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
                    && static_cast<std::int64_t>(attributes.filesize)
                        == localSize) {
                    it = plan.files.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (error.isEmpty()) ensureRemoteDirectory(sftp, targetDirectory, &error);
        for (const NativeString &relativeDirectory : plan.directories) {
            if (!error.isEmpty()) break;
            ensureRemoteDirectory(sftp, remoteJoinPath(targetDirectory, relativeDirectory), &error);
        }
        std::int64_t totalBytes = 0;
        for (const UploadTreeFile &file : plan.files) totalBytes += NativeFileInfo(file.localPath).size();
        std::int64_t doneBytes = 0;
        const int fileCount = plan.files.size();
        if (fileCount > 0) {
            Utf8Output out(std::cout);
            out << "P\t0\t" << totalBytes << '\t'
                << base64Encode(plan.files.front().relativePath)
                << "\t1\t" << fileCount << '\n';
            out.flush();
        }
        for (int index = 0; index < plan.files.size() && error.isEmpty(); ++index) {
            if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
            const UploadTreeFile &file = plan.files.at(index);
            NativeFile local(file.localPath);
            if (!local.open(NativeIODevice::ReadOnly)) { error = "无法读取本地文件：" + local.errorString(); break; }
            NativeString remotePath = remoteJoinPath(targetDirectory, file.relativePath);
            if (remotePathExists(sftp, remotePath)) {
                if (policy == "skip") { local.close(); continue; }
                if (policy == "rename") remotePath = uniqueRemotePath(sftp, remotePath);
            }
            if (!ensureRemoteDirectory(sftp, NativeFileInfo(remotePath).path(), &error)) { local.close(); break; }
            const std::string remoteBytes = utf8Bytes(remotePath);
            const std::int64_t fileTotal = local.size();
            std::int64_t resumeOffset = 0;
            bool skipExisting = false;
            if (resumeTree && policy != "rename") {
                LIBSSH2_SFTP_ATTRIBUTES existing{};
                const bool exists = libssh2_sftp_stat_ex(
                    sftp, remoteBytes.c_str(), remoteBytes.size(),
                    LIBSSH2_SFTP_STAT, &existing) == 0;
                if (exists && (existing.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
                    const std::int64_t remoteSize =
                        static_cast<std::int64_t>(existing.filesize);
                    if (remoteSize == fileTotal) {
                        skipExisting = !verifyChecksum
                            || localRemoteChecksumMatches(file.localPath,
                                                           remotePath);
                    } else if (remoteSize > 0 && remoteSize < fileTotal) {
                        resumeOffset = remoteSize;
                    }
                }
            }
            if (!error.isEmpty()) { local.close(); break; }
            if (skipExisting) {
                doneBytes += fileTotal;
                Utf8Output out(std::cout);
                out << "P\t" << doneBytes << '\t' << totalBytes << '\t'
                    << base64Encode(file.relativePath) << "\t" << (index + 1)
                    << "\t" << fileCount << '\t' << fileTotal << '\t'
                    << fileTotal << '\n';
                out.flush();
                local.close();
                continue;
            }
            LIBSSH2_SFTP_HANDLE *remote = libssh2_sftp_open_ex(sftp, remoteBytes.c_str(), remoteBytes.size(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT
                    | (resumeOffset > 0 ? LIBSSH2_FXF_APPEND : LIBSSH2_FXF_TRUNC),
                0644, LIBSSH2_SFTP_OPENFILE);
            if (!remote) { error = "无法创建远程文件：" + sessionError(session); local.close(); break; }
            std::int64_t fileDone = resumeOffset;
            doneBytes += resumeOffset;
            if (resumeOffset > 0 && !local.seek(resumeOffset)) {
                error = "无法定位本地续传位置：" + local.errorString();
                libssh2_sftp_close(remote);
                local.close();
                break;
            }
            std::vector<char> buffer(65536, '\0');
            while (error.isEmpty()) {
                if (!transferPausePoint()) { error = "传输控制通道已关闭"; break; }
                const std::int64_t count = local.read(buffer.data(), buffer.size());
                if (count == 0) break;
                if (count < 0) { error = "读取本地文件失败：" + local.errorString(); break; }
                std::int64_t written = 0;
                while (written < count) {
                    const ssize_t result = libssh2_sftp_write(remote, buffer.data() + written, static_cast<size_t>(count - written));
                    if (result <= 0) { error = "上传写入失败：" + sessionError(session); break; }
                    written += result;
                }
                doneBytes += written;
                fileDone += written;
                Utf8Output out(std::cout); out << "P\t" << doneBytes << '\t' << totalBytes << '\t'
                    << base64Encode(file.relativePath) << '\t' << (index + 1) << '\t' << fileCount
                    << '\t' << fileDone << '\t' << fileTotal << '\n'; out.flush();
            }
            const bool emptyFile = local.size() == 0;
            local.close();
            libssh2_sftp_close(remote);
            if (emptyFile) {
                Utf8Output out(std::cout); out << "P\t" << doneBytes << '\t' << totalBytes << '\t'
                    << base64Encode(file.relativePath) << '\t' << (index + 1) << '\t' << fileCount
                    << '\t' << fileDone << '\t' << fileTotal << '\n'; out.flush();
            }
            if (error.isEmpty()) {
                LIBSSH2_SFTP_ATTRIBUTES uploaded{};
                if (libssh2_sftp_stat_ex(sftp, remoteBytes.c_str(),
                                         remoteBytes.size(), LIBSSH2_SFTP_STAT,
                                         &uploaded) != 0
                    || !(uploaded.flags & LIBSSH2_SFTP_ATTR_SIZE)
                    || static_cast<std::int64_t>(uploaded.filesize)
                        != fileTotal)
                    error = "校验失败：上传文件大小不一致（"
                        + file.relativePath + "）";
            }
            if (error.isEmpty() && verifyChecksum
                && !localRemoteChecksumMatches(file.localPath, remotePath))
                error = "校验失败：SHA-256 不一致（" + file.relativePath + "）";
        }
    }
    if (directory) libssh2_sftp_closedir(directory);
    if (sftp) libssh2_sftp_shutdown(sftp);
    if (session) libssh2_session_free(session);
    if (socket != INVALID_SOCKET) closesocket(socket);
    return error.isEmpty() ? 0 : fail(error);
#else
    return fail("当前 SFTP 工作进程仅支持 Windows");
#endif
}

