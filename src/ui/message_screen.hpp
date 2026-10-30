#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

enum class MessageKind { Error, Info };

// A full-screen readable message. Every failure path that would otherwise
// abort — no core, no ROM, no BIOS, a core that will not start — lands here
// instead, so the app never drops the player back to the Steam library with
// nothing to go on.
class MessageScreen {
public:
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;
    // With `choices`, it is an alert instead -- the body and details in a
    // popup, the choices as its buttons -- for a failure the app can do
    // something about: an emulator not here yet, which it can download.
    // B, or the last choice, puts it away as Continue would.
    void show(MessageKind kind,
              const std::string& title,
              const std::string& body,
              const std::vector<std::string>& details = {},
              const std::vector<std::string>& choices = {});
    void clear();
    bool is_visible() const { return visible_; }
    // Over a paused game, which shows through blurred; otherwise on the app's
    // own background. See draw_page_background().
    void set_over_game(bool over_game) { over_game_ = over_game; }

    const std::string& title() const { return title_; }

    // Returns true when the player dismissed the message, or answered it:
    // chosen() says which.
    //
    // A message cannot be dismissed for a moment after it appears. Launchers --
    // Steam Input especially -- deliver a press as the app starts, and without
    // this a fatal error screen is dismissed before it has been drawn once,
    // which from the outside looks exactly like the app quitting on startup for
    // no reason.
    bool update(const std::vector<UiAction>& actions,
                const std::vector<PointerEvent>& pointers,
                double delta_seconds,
                float output_width,
                float output_height);
    // The choice the last update() was answered with, or -1 for none: put
    // away with B, or a message with no choices.
    int chosen() const { return chosen_; }

    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height);

private:
    bool visible_ = false;
    bool over_game_ = false;
    MessageKind kind_ = MessageKind::Info;
    std::string title_;
    std::string body_;
    std::vector<std::string> details_;
    std::vector<std::string> choices_;
    int chosen_ = -1;
    PopupCursor cursor_;
    PopupGeometry alert_geometry(float output_width, float output_height) const;
    std::vector<PopupButton> buttons() const;
    double seconds_visible_ = 0.0;
    std::vector<GuideHit> guide_hits_;
};

// How long a message stays un-dismissable after appearing.
constexpr double kMessageSettleSeconds = 0.75;

}  // namespace ds
