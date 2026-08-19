#pragma once

#include <initializer_list>
#include <string>

namespace ds {

// House style for on-screen control prompts: "<input>: <action>", never
// "<input> does <action>".
//
//   good:  "A: Open"          "L/R: Page"        "Two fingers: Menu"
//   bad:   "A opens"          "L/R pages"        "two fingers open the menu"
//
// Build every prompt through here so the style cannot drift.
struct ControlPrompt {
    const char* input;
    const char* action;
};

inline std::string format_prompt(const ControlPrompt& prompt) {
    return std::string(prompt.input) + ": " + prompt.action;
}

// Joins several prompts into one line with consistent spacing.
inline std::string format_prompts(std::initializer_list<ControlPrompt> prompts) {
    std::string line;
    for (const ControlPrompt& prompt : prompts) {
        if (!line.empty()) {
            line += "   ";
        }
        line += format_prompt(prompt);
    }
    return line;
}

}  // namespace ds
