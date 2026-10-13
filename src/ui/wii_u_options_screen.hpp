#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/external_frame_link.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/sound_view.hpp"

namespace ds {

struct WiiUOptionsScreenResult {
    bool close_requested = false;
    // An option set to its `second`th value: for Cemu, which answers with the
    // whole list again -- see set_options().
    std::optional<std::pair<std::string, int>> change;
};

// Cemu's own settings worth changing mid-game -- how it scales its picture,
// its performance overlay -- from the Wii U quick menu's Options, laid out as
// Settings is: a row each, its value at the right, Left and Right stepping it.
// Cemu describes them itself (see ExternalOption), so this page shows whatever
// it offers; it only asks for changes, and shows each as made rather than
// waiting to hear it back.
class WiiUOptionsScreen {
public:
    // Up, and waiting for Cemu's list.
    void open();
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Cemu's list, as it is now: the cursor stays on the option it was on.
    void set_options(std::vector<ExternalOption> options);
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }

    WiiUOptionsScreenResult update(const std::vector<UiAction>& actions,
                                   const std::vector<PointerEvent>& pointers, float output_width,
                                   float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                float output_width, float output_height);
    void describe_for_sound(UiSoundView& view) const;

    int focus() const { return focus_; }
    const std::vector<ExternalOption>& options() const { return options_; }
    // Whether an option changed is one that waits for the game to start
    // again -- which the page then says.
    bool restart_pending() const { return restart_pending_; }

private:
    std::vector<PixelRect> rows(float output_width, float output_height) const;
    // Steps the focused option `direction` values, round from one end to the
    // other, as Settings' values go.
    void adjust(int direction, WiiUOptionsScreenResult& result);

    bool open_ = false;
    bool waiting_ = true;
    bool over_game_ = false;
    bool restart_pending_ = false;
    std::vector<ExternalOption> options_;
    int focus_ = 0;
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds
