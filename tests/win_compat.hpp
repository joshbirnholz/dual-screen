#pragma once

// The few POSIX calls the tests use that Windows does not have, for the tests
// built for Windows (and run under Wine on the Linux machine that built
// them). Forced into every test there by tests/CMakeLists.txt, so the tests
// themselves read as they always did.

#if defined(_WIN32)

#include <direct.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstdio>
#include <cstring>

#include <atomic>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#undef interface

// The tests point the app's folders at a sandbox by setting HOME and the XDG
// variables. On Windows the app reads USERPROFILE, APPDATA and LOCALAPPDATA
// instead, so each is set alongside the variable it stands in for.
inline const char* windows_twin_of(const char* name) {
    if (std::strcmp(name, "HOME") == 0) return "USERPROFILE";
    if (std::strcmp(name, "XDG_CONFIG_HOME") == 0) return "APPDATA";
    if (std::strcmp(name, "XDG_DATA_HOME") == 0) return "LOCALAPPDATA";
    return nullptr;
}

inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && ::getenv(name) != nullptr) return 0;
    if (const char* twin = windows_twin_of(name)) {
        ::SetEnvironmentVariableA(twin, value);
        ::_putenv_s(twin, value);
    }
    // putenv keeps the C library's copy of the environment, which is the one
    // getenv() reads, in step; SetEnvironmentVariable does the same for the
    // processes this one starts.
    ::SetEnvironmentVariableA(name, value);
    return ::_putenv_s(name, value);
}

inline int unsetenv(const char* name) {
    if (const char* twin = windows_twin_of(name)) {
        ::SetEnvironmentVariableA(twin, nullptr);
        ::_putenv_s(twin, "");
    }
    ::SetEnvironmentVariableA(name, nullptr);
    return ::_putenv_s(name, "");
}

// mkdtemp() as POSIX has it, in place in the caller's buffer. "/tmp/..." on
// Windows is \tmp on the current drive, which is made first if need be: a
// test's sandbox lands at C:\tmp\dualscreen-test-1a2b3c, and its paths keep
// the forward slashes every Windows API accepts.
inline char* mkdtemp(char* pattern) {
    static std::atomic<unsigned> counter {0};
    std::string folder = pattern;
    const std::size_t slash = folder.find_last_of('/');
    if (slash != std::string::npos && slash > 0) ::_mkdir(folder.substr(0, slash).c_str());
    char* xs = std::strstr(pattern, "XXXXXX");
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (xs != nullptr) {
            const unsigned unique = (static_cast<unsigned>(::GetCurrentProcessId()) << 12) ^
                                    (counter.fetch_add(1) * 2654435761u) ^ ::GetTickCount();
            char digits[8];
            std::snprintf(digits, sizeof(digits), "%06x", unique & 0xFFFFFFu);
            std::memcpy(xs, digits, 6);
        }
        if (::_mkdir(pattern) == 0) return pattern;
    }
    return nullptr;
}

#endif  // _WIN32
