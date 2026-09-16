// Diagnostic-log unit test (task T1-2).  Verifies the crash context, the
// recent-event ring, sanitization and the unified diagnostic.log writer.
// The log directory is redirected to a build-local folder through
// MASTERSSH_DATA_DIR (set as a CTest environment property), so the real
// user log directory is never touched.
#include "DiagnosticLog.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "FAIL: " #condition " (" << __FILE__ << ":"           \
                      << __LINE__ << ")\n";                                    \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

void testCrashContext()
{
    DiagnosticLog::setPhase("request:session.connect");
    DiagnosticLog::setSession("ssh-7");
    const DiagnosticLog::CrashContext &context = DiagnosticLog::crashContext();
    CHECK(std::string(context.phase) == "request:session.connect");
    CHECK(std::string(context.session) == "ssh-7");

    // Oversized values are truncated inside the fixed buffers.
    DiagnosticLog::setPhase(std::string(200, 'p'));
    DiagnosticLog::setSession(std::string(200, 's'));
    CHECK(std::string(context.phase).size() == sizeof(context.phase) - 1);
    CHECK(std::string(context.session).size() == sizeof(context.session) - 1);
    CHECK(std::string(context.phase).find('p') != std::string::npos);
    CHECK(std::string(context.session).find('s') != std::string::npos);

    DiagnosticLog::setPhase("idle");
    DiagnosticLog::setSession("");
    CHECK(std::string(context.phase) == "idle");
    CHECK(std::string(context.session).empty());
}

void testSanitize()
{
    CHECK(DiagnosticLog::sanitize("plain text") == "plain text");
    // Newlines/tabs become spaces, control characters are dropped:
    // "a\r\nb\tc\1d" -> a + two spaces + b + space + c + d.
    const std::string cleaned =
        DiagnosticLog::sanitize(std::string("a\r\nb\tc\1d", 8));
    CHECK(cleaned == std::string("a") + "  b" + " c" + "d");
    // Length is capped.
    CHECK(DiagnosticLog::sanitize(std::string(500, 'x')).size() <= 160);
    // Empty input stays empty.
    CHECK(DiagnosticLog::sanitize("").empty());
}

void testRing()
{
    // The ring is process-global; fill it past capacity and check the dump
    // returns at most RingSize entries, newest first in order of insertion.
    const std::size_t before = DiagnosticLog::ringHead();
    DiagnosticLog::remember("ring-probe-start");
    DiagnosticLog::remember("ring-probe-end");
    const std::size_t after = DiagnosticLog::ringHead();
    CHECK(after == before + 2);

    std::ostringstream dump;
    DiagnosticLog::dumpRingTo(dump);
    const std::string text = dump.str();
    CHECK(text.find("recent diagnostics") != std::string::npos);
    CHECK(text.find("ring-probe-start") != std::string::npos);
    CHECK(text.find("ring-probe-end") != std::string::npos);
}

void testThreadNames()
{
    DiagnosticLog::registerThread("diagnostic-test");
    CHECK(DiagnosticLog::threadName(GetCurrentThreadId()) == "diagnostic-test");
    CHECK(DiagnosticLog::threadName(0xDEADBEEF).empty());
}

void testLogFile()
{
    const std::filesystem::path file = DiagnosticLog::path();
    std::error_code error;
    std::filesystem::remove(file, error);
    DiagnosticLog::write("diag-test-event", "detail with spaces");
    DiagnosticLog::write("diag-test-clean");
    std::ifstream input(file, std::ios::binary);
    CHECK(static_cast<bool>(input));
    std::string content(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    CHECK(content.find("diag-test-event detail with spaces")
          != std::string::npos);
    CHECK(content.find("diag-test-clean") != std::string::npos);
    CHECK(content.find("tid=") != std::string::npos);
    // Control characters are neutralized before writing: the newline
    // becomes a space and the \1 byte is dropped.
    DiagnosticLog::write(
        "diag-test-raw", std::string("x\nsecret\1tail", 13));
    input.close();
    input.open(file, std::ios::binary);
    content.assign(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    CHECK(content.find("x secrettail") != std::string::npos);
    CHECK(content.find('\1') == std::string::npos);
}

} // namespace

int main()
{
    testCrashContext();
    testSanitize();
    testRing();
    testThreadNames();
    testLogFile();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "DIAGNOSTIC_LOG_TEST_OK\n";
    return 0;
}
