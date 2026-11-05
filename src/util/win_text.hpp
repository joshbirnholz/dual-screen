#pragma once

// UTF-8 <-> UTF-16 for Windows's wide-character API. Everything in this
// codebase is UTF-8; Windows's own text is UTF-16, so the two meet here.
// Header-only, so the builds that never see Windows need no new source file.

#if defined(_WIN32)

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ds {

inline std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    }
    return wide;
}

inline std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(),
                              length, nullptr, nullptr);
    }
    return text;
}

inline std::string narrow(const wchar_t* wide) { return wide != nullptr ? narrow(std::wstring(wide)) : std::string(); }

// What GetLastError()'s code means, in words, in UTF-8.
inline std::string windows_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text = length > 0 ? narrow(std::wstring(buffer, length)) : std::string();
    if (buffer != nullptr) ::LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) text.pop_back();
    return text.empty() ? "error " + std::to_string(code) : text;
}

}  // namespace ds

#endif  // _WIN32
