#pragma once

// Unified data-directory resolution shared by MasterTerm.exe, the SFTP worker
// and the CLI so all three processes agree on where the configuration,
// known_hosts and logs live.
//
// Resolution order:
//   1. MASTERSSH_DATA_DIR environment variable (custom data directory).
//   2. portable.ini next to the executable (portable mode): the executable
//      directory itself becomes the data root.
//   3. %APPDATA%\MasterSSH (default).

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace NativeDataDir {

inline std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (length > 0 && length < sizeof(buffer) / sizeof(buffer[0])) {
        std::filesystem::path executable(buffer);
        return executable.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

inline std::filesystem::path resolveDataRoot()
{
#ifdef _WIN32
    wchar_t buffer[32768]{};
    const DWORD customLength = GetEnvironmentVariableW(
        L"MASTERSSH_DATA_DIR", buffer,
        static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (customLength > 0 && customLength < sizeof(buffer) / sizeof(buffer[0]))
        return std::filesystem::path(buffer);
    std::error_code error;
    const std::filesystem::path portableMarker =
        executableDirectory() / L"portable.ini";
    if (std::filesystem::exists(portableMarker, error))
        return executableDirectory();
    const DWORD appDataLength = GetEnvironmentVariableW(
        L"APPDATA", buffer,
        static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (appDataLength > 0 && appDataLength < sizeof(buffer) / sizeof(buffer[0]))
        return std::filesystem::path(buffer) / L"MasterSSH";
    const DWORD profileLength = GetEnvironmentVariableW(
        L"USERPROFILE", buffer,
        static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (profileLength > 0 && profileLength < sizeof(buffer) / sizeof(buffer[0]))
        return std::filesystem::path(buffer) / L"MasterSSH";
#endif
    return std::filesystem::current_path() / L"MasterSSH";
}

inline std::filesystem::path configFile()
{
    return resolveDataRoot() / L"MasterSSH.json";
}

inline std::filesystem::path knownHostsFile()
{
    return resolveDataRoot() / L"known_hosts";
}

inline std::filesystem::path sessionLogDirectory()
{
    return resolveDataRoot() / L"logs";
}

inline std::filesystem::path localCommandHistoryFile()
{
    return resolveDataRoot() / L"local-history.txt";
}

} // namespace NativeDataDir
