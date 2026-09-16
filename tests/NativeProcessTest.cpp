#include "NativeProcess.h"

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring executablePath()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

int runChild()
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    char buffer[64 * 1024];
    while (std::cin) {
        std::cin.read(buffer, sizeof(buffer));
        const std::streamsize count = std::cin.gcount();
        if (count > 0)
            std::cout.write(buffer, count);
    }
    std::cout.flush();
    return 0;
}

bool runRound(const std::wstring &path, int round)
{
    NativeProcess process;
    if (!process.start(path, {L"--child"}, {}, true)) {
        std::wcerr << L"start failed: " << process.errorMessage() << L'\n';
        return false;
    }

    std::string expected;
    for (int chunkIndex = 0; chunkIndex < 128; ++chunkIndex) {
        std::string chunk(8192, static_cast<char>(
            'A' + (round + chunkIndex) % 26));
        expected += chunk;
        if (!process.write(chunk)) {
            std::cerr << "write rejected\n";
            return false;
        }
    }
    process.closeInput();

    std::string actual;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    DWORD exitCode = STILL_ACTIVE;
    while (std::chrono::steady_clock::now() < deadline) {
        actual += process.takeStandardOutput();
        if (process.isFinished(exitCode))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    actual += process.takeStandardOutput();
    if (exitCode != 0 || actual != expected) {
        std::cerr << "round " << round << " failed: exit=" << exitCode
                  << " expected=" << expected.size()
                  << " actual=" << actual.size() << '\n';
        return false;
    }
    return true;
}

bool runCancellationRound(const std::wstring &path)
{
    NativeProcess process;
    if (!process.start(path, {L"--child"}, {}, true))
        return false;
    const std::string chunk(64 * 1024, 'X');
    for (int index = 0; index < 64; ++index) {
        if (!process.write(chunk))
            return false;
    }
    process.terminate();
    return true;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"--child")
        return runChild();

    const std::wstring path = executablePath();
    if (path.empty())
        return 1;
    for (int round = 0; round < 32; ++round) {
        if (!runRound(path, round))
            return 2;
    }
    for (int round = 0; round < 16; ++round) {
        if (!runCancellationRound(path))
            return 3;
    }
    return 0;
}
