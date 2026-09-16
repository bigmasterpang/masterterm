#pragma once

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <libssh2.h>

#include <mutex>

// C++ guarantees one shared initialization for a function-local static in an
// inline function. Both terminal SSH and background SFTP must use this exact
// entry point: concurrent independent libssh2_init() calls can corrupt state.
inline bool masterSshInitializeLibraries()
{
    static const bool initialized = [] {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
        return libssh2_init(0) == 0;
    }();
    return initialized;
}

// The packaged libssh2 build is used by both the GUI-thread terminal and the
// SFTP worker. Serializing calls avoids unsafe cross-thread access inside the
// Windows crypto/runtime dependencies used by that DLL.
inline std::recursive_mutex &masterSshLibraryMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}
