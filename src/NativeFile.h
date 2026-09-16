#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace NativeFile {

inline std::filesystem::path uniqueTemporarySibling(
    const std::filesystem::path &target)
{
    static std::atomic_uint64_t sequence{0};
#ifdef _WIN32
    const std::wstring processId = std::to_wstring(GetCurrentProcessId());
#else
    const std::wstring processId = L"native";
#endif
    const std::wstring name = target.filename().wstring() + L"."
        + processId + L"." + std::to_wstring(++sequence) + L".tmp";
    return target.parent_path() / name;
}

// Replaces target only after a complete temporary file has been written.
// In particular, do not delete a valid configuration file as a retry step.
inline bool replaceAtomically(const std::filesystem::path &temporary,
                              const std::filesystem::path &target)
{
#ifdef _WIN32
    const std::wstring temporaryPath = temporary.wstring();
    const std::wstring targetPath = target.wstring();
    const DWORD attributes = GetFileAttributesW(targetPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES
        && ReplaceFileW(targetPath.c_str(), temporaryPath.c_str(), nullptr,
                        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return true;
    }
    return MoveFileExW(temporaryPath.c_str(), targetPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

} // namespace NativeFile
