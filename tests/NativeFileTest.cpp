#include "NativeFile.h"
#include "NativeConfigFile.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

int main()
{
    const auto suffix = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (std::wstring(L"MasterTerm-文件-") + suffix);
    const std::filesystem::path target = directory / L"配置.json";
    const std::filesystem::path temporary = directory / L"配置.json.tmp";
    std::filesystem::create_directories(directory);
    {
        std::ofstream output(target, std::ios::binary);
        output << "old";
    }
    {
        std::ofstream output(temporary, std::ios::binary);
        output << "new";
    }
    assert(NativeFile::replaceAtomically(temporary, target));
    std::ifstream input(target, std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>(input), {}) == "new");
    input.close();
    assert(!std::filesystem::exists(temporary));
    {
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output << "preserved";
    }
    assert(!NativeFile::replaceAtomically(directory / L"missing.tmp", target));
    std::ifstream preserved(target, std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>(preserved), {}) == "preserved");
    preserved.close();

    std::mutex temporaryMutex;
    std::vector<std::filesystem::path> temporaryPaths;
    std::vector<std::thread> writers;
    for (int index = 0; index < 8; ++index) {
        writers.emplace_back([&] {
            const std::filesystem::path candidate = NativeFile::uniqueTemporarySibling(target);
            std::lock_guard<std::mutex> lock(temporaryMutex);
            temporaryPaths.push_back(candidate);
        });
    }
    for (std::thread &writer : writers) writer.join();
    std::set<std::filesystem::path> uniquePaths(
        temporaryPaths.begin(), temporaryPaths.end());
    assert(uniquePaths.size() == temporaryPaths.size());
    for (const std::filesystem::path &candidate : uniquePaths) {
        assert(candidate.parent_path() == target.parent_path());
        assert(candidate.extension() == L".tmp");
    }

    const std::filesystem::path configPath = directory / L"MasterSSH.json";
    bool available = true;
    assert(NativeConfigFile::readObject(configPath, &available).values.empty());
    assert(!available);
    NativeJsonDom::Object config;
    config.values.emplace("name", "配置测试");
    config.values.emplace("enabled", true);
    assert(NativeConfigFile::writeObject(configPath, config));
    const NativeJsonDom::Object restored = NativeConfigFile::readObject(configPath, &available);
    assert(available);
    assert(NativeJsonDom::stringValue(restored, "name") == "配置测试");
    {
        std::ofstream corrupted(configPath, std::ios::binary | std::ios::trunc);
        corrupted << "{invalid";
    }
    assert(NativeConfigFile::readObject(configPath, &available).values.empty());
    assert(available);
    assert(!NativeConfigFile::writeObject(directory, config));

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    assert(!error);
    return 0;
}
