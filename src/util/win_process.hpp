#pragma once

// Starting other programs on Windows: what fork() and execvp() do elsewhere.
//
// Windows passes a program one command line rather than a list of arguments,
// and the program splits it again by the C runtime's rules. So the list this
// codebase keeps is quoted here by those same rules -- the way Python's
// subprocess.list2cmdline() and Rust's std::process do it -- and nothing goes
// through a shell, so nothing in a filename can become a command.
//
// Header-only, so the builds that never see Windows need no new source file.

#if defined(_WIN32)

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "util/win_text.hpp"

namespace ds {

// One argument as the C runtime's command-line splitter will read it back.
inline std::wstring quote_windows_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring quoted = L"\"";
    for (std::size_t index = 0;; ++index) {
        std::size_t backslashes = 0;
        while (index < argument.size() && argument[index] == L'\\') {
            ++index;
            ++backslashes;
        }
        if (index == argument.size()) {
            // Doubled before the closing quote, so they stay backslashes.
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(argument[index]);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

inline std::wstring windows_command_line(const std::string& program, const std::vector<std::string>& arguments) {
    std::wstring line = quote_windows_argument(widen(program));
    for (const std::string& argument : arguments) {
        line += L' ';
        line += quote_windows_argument(widen(argument));
    }
    return line;
}

// A file opened to hand to a child as its output, or NUL when there is none.
inline HANDLE inheritable_output(const std::string& path) {
    SECURITY_ATTRIBUTES inherit {};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    const std::wstring name = path.empty() ? std::wstring(L"NUL") : widen(path);
    return ::CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

struct WindowsLaunch {
    // Where its output and errors go; empty means nowhere. Only used with
    // `redirect`: otherwise the child shares this program's own.
    bool redirect = false;
    std::string out_path;
    std::string err_path;
    // No console window of its own: curl and tar are console programs, and
    // Dualscreen is not, so each would otherwise flash one up.
    bool hidden = true;
    // Not tied to this program at all: it outlives it, and has no console
    // or job of this one's. The Remote Start service starting a game, say.
    bool detached = false;
    // The folder it starts in; empty for this program's own.
    std::string working_directory;
};

// Starts `program` with `arguments`. On success the process handle is the
// caller's to close; the thread handle is closed here.
inline bool start_windows_process(const std::string& program, const std::vector<std::string>& arguments,
                                  const WindowsLaunch& launch, HANDLE& out_process, DWORD* out_id,
                                  std::string* error) {
    out_process = nullptr;
    std::wstring line = windows_command_line(program, arguments);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    HANDLE out = INVALID_HANDLE_VALUE;
    HANDLE err = INVALID_HANDLE_VALUE;
    HANDLE in = INVALID_HANDLE_VALUE;
    if (launch.redirect) {
        out = inheritable_output(launch.out_path);
        err = inheritable_output(launch.err_path);
        SECURITY_ATTRIBUTES inherit {};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;
        in = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0,
                           nullptr);
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = out;
        startup.hStdError = err;
        startup.hStdInput = in;
    }
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (launch.hidden) flags |= CREATE_NO_WINDOW;
    if (launch.detached) flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    const std::wstring directory = widen(generic_to_native_path(launch.working_directory));
    PROCESS_INFORMATION info {};
    BOOL started = ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, launch.redirect ? TRUE : FALSE, flags,
                                    nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &info);
    if (!started && launch.detached && ::GetLastError() == ERROR_ACCESS_DENIED) {
        // A job that does not allow breaking away; started inside it instead.
        flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
        started = ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, launch.redirect ? TRUE : FALSE, flags,
                                   nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &info);
    }
    const DWORD failure = started ? 0 : ::GetLastError();
    for (HANDLE handle : {out, err, in}) {
        if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    }
    if (!started) {
        if (error != nullptr) *error = windows_error_text(failure);
        return false;
    }
    ::CloseHandle(info.hThread);
    out_process = info.hProcess;
    if (out_id != nullptr) *out_id = info.dwProcessId;
    return true;
}

// The full path of this program's own .exe, with forward slashes.
inline std::string windows_self_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::string();
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::string path = narrow(buffer);
    for (char& character : path) {
        if (character == '\\') character = '/';
    }
    return path;
}

}  // namespace ds

#endif  // _WIN32
