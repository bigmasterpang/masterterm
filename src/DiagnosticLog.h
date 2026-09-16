#pragma once

// Unified sanitized diagnostic log for MasterTerm (task T1-2).
//
// Components:
//  - append-only `diagnostic.log` in the session log directory with
//    timestamp, thread id/name, event and detail for every lifecycle phase
//    (startup, sessions, SFTP workers, RDP, shutdown, watchdog, crash);
//  - a fixed-size crash context (current phase + active session) that the
//    unhandled-exception handler copies into crash.log so a crash can be
//    located without the full log;
//  - a small ring buffer of the most recent events, also dumped to
//    crash.log on an unhandled exception;
//  - thread-name registry so multi-threaded faults (serial, proxy relay,
//    shutdown watchdog, worker stdin writer) are identifiable.
//
// Events never carry credentials.  Only ids, states, phases and method
// names are allowed.  `tests/log-redaction-check.js` scans every *.log in
// the same directory and fails on any plaintext secret.

#include "NativeDataDir.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace DiagnosticLog {

// ---------------------------------------------------------------------------
// Crash context (fixed-size buffers; a torn read in the fault path is
// acceptable and never crashes the handler itself).
// ---------------------------------------------------------------------------

struct CrashContext
{
    char phase[96]{};
    char session[96]{};
    bool valid = false;
};

inline CrashContext &crashContext()
{
    static CrashContext context{};
    return context;
}

inline void setPhase(std::string_view phase)
{
    CrashContext &context = crashContext();
    std::snprintf(
        context.phase, sizeof(context.phase), "%.*s",
        static_cast<int>(std::min<std::size_t>(phase.size(), sizeof(context.phase) - 1)),
        phase.data());
    context.valid = true;
}

inline void setSession(std::string_view session)
{
    CrashContext &context = crashContext();
    std::snprintf(
        context.session, sizeof(context.session), "%.*s",
        static_cast<int>(std::min<std::size_t>(session.size(), sizeof(context.session) - 1)),
        session.data());
    context.valid = true;
}

// ---------------------------------------------------------------------------
// Thread names.
// ---------------------------------------------------------------------------

inline std::mutex &threadNamesMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<DWORD, std::string> &threadNames()
{
    static std::unordered_map<DWORD, std::string> names;
    return names;
}

inline void registerThread(const char *name)
{
    std::lock_guard<std::mutex> lock(threadNamesMutex());
    threadNames()[GetCurrentThreadId()] = name ? name : "";
}

inline std::string threadName(DWORD threadId)
{
    std::lock_guard<std::mutex> lock(threadNamesMutex());
    const auto found = threadNames().find(threadId);
    return found == threadNames().end() ? std::string() : found->second;
}

// ---------------------------------------------------------------------------
// Recent-event ring buffer (dumped into crash.log on unhandled exceptions).
// ---------------------------------------------------------------------------

constexpr std::size_t RingSize = 48;
constexpr std::size_t RingEntrySize = 224;

inline std::mutex &ringMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::array<std::array<char, RingEntrySize>, RingSize> &ring()
{
    static std::array<std::array<char, RingEntrySize>, RingSize> entries{};
    return entries;
}

inline std::size_t &ringHead()
{
    static std::size_t head = 0;
    return head;
}

inline void ringInsertUnlocked(std::string_view line)
{
    auto &entry = ring()[ringHead() % RingSize];
    std::snprintf(
        entry.data(), entry.size(), "%.*s",
        static_cast<int>(std::min<std::size_t>(line.size(), RingEntrySize - 1)),
        line.data());
    ++ringHead();
}

inline void remember(std::string_view line)
{
    std::lock_guard<std::mutex> lock(ringMutex());
    ringInsertUnlocked(line);
}

// ---------------------------------------------------------------------------
// Sanitization: strip control characters and cap the length.
// ---------------------------------------------------------------------------

inline std::string sanitize(std::string_view value, std::size_t maxLength = 160)
{
    std::string result;
    result.reserve(std::min(value.size(), maxLength));
    for (const char character : value) {
        const unsigned char code = static_cast<unsigned char>(character);
        if (code == '\r' || code == '\n' || code == '\t') {
            result.push_back(' ');
        } else if (code >= 0x20) {
            result.push_back(character);
        }
        if (result.size() >= maxLength)
            break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Timestamp and unified writer.
// ---------------------------------------------------------------------------

inline std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    local = *std::localtime(&time);
#endif
    char stamp[64]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::string result(stamp);
    result.push_back('.');
    char fraction[8]{};
    std::snprintf(fraction, sizeof(fraction), "%03lld",
                  static_cast<long long>(milliseconds));
    result.append(fraction);
    return result;
}

inline std::filesystem::path path()
{
    return NativeDataDir::sessionLogDirectory() / L"diagnostic.log";
}

inline std::mutex &fileMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline void write(std::string_view event, std::string_view detail = {})
{
    const std::string safeEvent = sanitize(event);
    const std::string safeDetail = sanitize(detail);
    const DWORD threadId = GetCurrentThreadId();
    const std::string name = threadName(threadId);
    std::string line = timestamp();
    line.push_back(' ');
    line.append("tid=").append(std::to_string(threadId));
    if (!name.empty()) {
        line.push_back('/');
        line.append(name);
    }
    line.push_back(' ');
    line.append(safeEvent);
    if (!safeDetail.empty()) {
        line.push_back(' ');
        line.append(safeDetail);
    }
    remember(line);
    try {
        std::lock_guard<std::mutex> lock(fileMutex());
        std::error_code directoryError;
        std::filesystem::create_directories(
            path().parent_path(), directoryError);
        if (directoryError)
            return;
        std::ofstream file(path(), std::ios::binary | std::ios::app);
        if (!file)
            return;
        file << line << '\n';
        file.flush();
    } catch (...) {
    }
}

// Called from the unhandled-exception handler.  Tries the locks; when a
// fault happened while holding one of them the entry is simply skipped
// instead of deadlocking the dying process.
inline void writeCrashLine(std::string_view event, std::string_view detail = {})
{
    const std::string safeEvent = sanitize(event);
    const std::string safeDetail = sanitize(detail);
    const DWORD threadId = GetCurrentThreadId();
    std::string line = timestamp();
    line.push_back(' ');
    line.append("tid=").append(std::to_string(threadId));
    line.push_back(' ');
    line.append(safeEvent);
    if (!safeDetail.empty()) {
        line.push_back(' ');
        line.append(safeDetail);
    }
    try {
        std::unique_lock<std::mutex> ringLock(ringMutex(), std::try_to_lock);
        if (ringLock.owns_lock())
            ringInsertUnlocked(line);
    } catch (...) {
    }
    try {
        std::unique_lock<std::mutex> lock(fileMutex(), std::try_to_lock);
        if (!lock.owns_lock())
            return;
        std::error_code directoryError;
        std::filesystem::create_directories(
            path().parent_path(), directoryError);
        if (directoryError)
            return;
        std::ofstream file(path(), std::ios::binary | std::ios::app);
        if (!file)
            return;
        file << line << '\n';
        file.flush();
    } catch (...) {
    }
}

// Copies the most recent events into the crash log; safe to call from the
// unhandled-exception handler.
inline void dumpRingTo(std::ostream &out)
{
    try {
        std::unique_lock<std::mutex> lock(ringMutex(), std::try_to_lock);
        if (!lock.owns_lock())
            return;
        out << "  recent diagnostics:\n";
        const std::size_t head = ringHead();
        const std::size_t start = head > RingSize ? head - RingSize : 0;
        for (std::size_t index = start; index < head; ++index) {
            const auto &entry = ring()[index % RingSize];
            out << "    " << entry.data() << '\n';
        }
    } catch (...) {
    }
}

} // namespace DiagnosticLog
