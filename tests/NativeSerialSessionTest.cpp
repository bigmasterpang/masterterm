#include "SerialSession.h"

#include <cassert>
#include <string>

int main()
{
    SerialSession session;
    int errorDispatchCount = 0;
    std::string errorMessage;
    session.setErrorHandler([&](const std::string &message) {
        ++errorDispatchCount;
        errorMessage = message;
    });

    // Worker events must only cross into backend state through poll().
    assert(!session.start(L"  ", 115200, 8, "none", "1", "none"));
    assert(errorDispatchCount == 0);
    // Starting again discards stale events from the previous attempt.
    assert(!session.start(L"", 9600, 8, "none", "1", "none"));
    session.poll();
    assert(errorDispatchCount == 1);
    assert(!errorMessage.empty());
    assert(!session.isConnected());
    assert(!session.isConnecting());
    assert(session.write(nullptr, 0) == 0);

    // Port discovery is intentionally safe even when no COM device exists.
    (void)SerialSession::availablePorts();
    session.stop();
    return 0;
}
