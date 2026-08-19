#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

enum class MessageKind { Error, Info };

// A full-screen readable message. Every failure path that would otherwise
// abort — no core, no ROM, no BIOS, a core that will not start — lands here
// instead, so the app never drops the player back to the Steam library with
// nothing to go on.
class MessageScreen {
public:
    void show(MessageKind kind,
              const std::string& title,
              const std::string& body,
              const std::vector<std::string>& details = {});
    void clear();
    bool is_visible() const { return visible_; }

    const std::string& title() const { return title_; }

    // Returns true when the player dismissed the message.
    bool update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers);

    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height);

private:
    bool visible_ = false;
    MessageKind kind_ = MessageKind::Info;
    std::string title_;
    std::string body_;
    std::vector<std::string> details_;
};

}  // namespace ds
