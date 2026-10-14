#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/popup.hpp"

namespace ds {

struct TextEntryResult {
    // OK, or Enter: what was typed is the answer -- see TextEntryPopup::text().
    bool done = false;
    // Cancel, B or Escape: nothing changes.
    bool cancelled = false;
};

// A few words typed in -- a name -- in the popup every question is asked in:
// what it is for, the field with what has been typed so far, and Cancel and
// OK. Typed on a keyboard, or Steam's on-screen one; the input layer hands
// over what was typed each frame (see InputManager::typed_text()).
class TextEntryPopup {
public:
    void open(std::string title, std::string initial, std::size_t max_characters);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    const std::string& text() const { return text_; }
    // What the field holds, changed from elsewhere -- typed where the game is.
    void set_text(std::string text);

    TextEntryResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                           const std::string& typed, int backspaces, float output_width,
                           float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height);

private:
    PopupText words() const;
    PopupGeometry geometry(float output_width, float output_height) const;
    float field_height(float output_height) const;

    bool open_ = false;
    std::string title_;
    std::string text_;
    std::size_t max_characters_ = 0;
    PopupCursor cursor_;
};

// How many characters a UTF-8 string has, and it short of its last one.
std::size_t utf8_length(const std::string& text);
std::string utf8_without_last(const std::string& text);

}  // namespace ds
