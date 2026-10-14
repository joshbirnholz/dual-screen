#pragma once

#include <cstddef>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "ui/text_entry.hpp"

namespace ds {

// The system's own keyboard for typing a few words, where there is one to
// ask for: a Switch's, shown there and then. True with what was typed,
// false if it was put away -- or, anywhere else, if there is none, which is
// where a TextEntryPopup is drawn instead.
inline bool system_keyboard_available() {
#ifdef __SWITCH__
    return true;
#else
    return false;
#endif
}

inline bool edit_with_system_keyboard(const std::string& header, const std::string& initial,
                                      std::size_t max_characters, std::string& out) {
#ifdef __SWITCH__
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, header.c_str());
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(max_characters));
    std::vector<char> text(max_characters * 4 + 1, 0);
    const Result result = swkbdShow(&keyboard, text.data(), text.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result)) return false;
    out = text.data();
    while (utf8_length(out) > max_characters) out = utf8_without_last(out);
    return true;
#else
    (void)header;
    (void)initial;
    (void)max_characters;
    (void)out;
    return false;
#endif
}

}  // namespace ds
