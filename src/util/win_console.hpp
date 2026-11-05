#pragma once

// A Windows program built with no console of its own (a game, started from
// Steam or Explorer) still writes its log and --help to the terminal it was
// started from, when there is one. Does nothing anywhere else, and nothing
// when output already goes somewhere (a file, a pipe).

#if defined(_WIN32)

#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#undef interface

namespace ds {

inline void attach_parent_console() {
    const auto redirected = [](DWORD which) {
        const HANDLE handle = ::GetStdHandle(which);
        return handle != nullptr && handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    const bool out_set = redirected(STD_OUTPUT_HANDLE);
    const bool err_set = redirected(STD_ERROR_HANDLE);
    if (out_set && err_set) return;
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return;
    if (!out_set) std::freopen("CONOUT$", "w", stdout);
    if (!err_set) std::freopen("CONOUT$", "w", stderr);
}

}  // namespace ds

#else

namespace ds {
inline void attach_parent_console() {}
}  // namespace ds

#endif
