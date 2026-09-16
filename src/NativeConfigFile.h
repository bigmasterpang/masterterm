#pragma once

#include "NativeFile.h"
#include "NativeJsonDom.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace NativeConfigFile {

inline NativeJsonDom::Object readObject(const std::filesystem::path &path,
                                        bool *available = nullptr)
{
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(path, error);
    if (available) *available = exists && !error;
    if (!exists || error) return {};
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    NativeJsonDom::Value root;
    if (!NativeJsonDom::parse(contents, root) || !root.isObject()) return {};
    return root.object();
}

inline bool writeObject(const std::filesystem::path &path,
                        const NativeJsonDom::Object &config)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const std::filesystem::path temporary = NativeFile::uniqueTemporarySibling(path);
    const std::string bytes = NativeJsonDom::stringify(NativeJsonDom::Value(config));
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    if (NativeFile::replaceAtomically(temporary, path)) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

} // namespace NativeConfigFile
