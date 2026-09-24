#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/save_points.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"

namespace ds {

// Which of the two things the screen is for.
enum class SavePointMode { Load, Create };

struct SavePointsScreenResult {
    bool close_requested = false;
    // A slot to load, already confirmed: the caller loads it and goes back
    // to the game. -1 for none.
    int load_slot = -1;
    // A slot to save into, already confirmed where it had something in it:
    // the caller saves, hands back the new slots with set_slots() and says
    // how it went with set_status(). -1 for none.
    int save_slot = -1;
};

// Save points, laid out the way Nintendo Switch Online's own suspend point
// menu is: the two things there are to do down the left, a line, and the
// slots down the right -- each a picture of the game where it was left and
// when. Loading one, or saving over one, is asked about first; saving into an
// empty slot is not, since nothing is lost.
//
// Belongs to the device running the game, like Settings: that is where the
// saves are, and the core that makes them.
class SavePointsScreen {
public:
    // Opens with the cursor already on the slots, since the quick menu entry
    // it was opened from has said which of the two it is for.
    void open(SavePointMode mode);
    void close();
    bool is_open() const { return open_; }

    // What is in each slot, read off the disk by the caller: on opening and
    // after each save.
    void set_slots(std::vector<SavePointInfo> slots);
    // One line under the slots: "Saved.", or why not.
    void set_status(std::string status, bool failed);

    SavePointsScreenResult update(const std::vector<UiAction>& actions,
                                  const std::vector<PointerEvent>& pointers,
                                  float output_width,
                                  float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height);

    SavePointMode mode() const { return mode_; }
    bool on_slots() const { return on_slots_; }
    int slot_focus() const { return slot_; }
    bool confirming() const { return confirm_ != Confirm::None; }

private:
    enum class Confirm { None, Load, Overwrite };

    struct Geometry {
        PixelRect panel;
        PixelRect modes[2];
        float divider_x = 0.0f;
        std::array<PixelRect, kSavePointSlots> slots{};
        PixelRect dialog;
        PixelRect accept;
        PixelRect dismiss;
        float scale = 1.0f;
    };
    Geometry compute_geometry(float output_width, float output_height) const;

    bool slot_filled(int slot) const;
    // Where the cursor goes when the slots are entered: the newest save to
    // load, or somewhere to save that loses the least.
    int natural_slot() const;
    // A on a slot: straight through, or a question first.
    void choose_slot(SavePointsScreenResult& result);

    bool open_ = false;
    SavePointMode mode_ = SavePointMode::Load;
    bool on_slots_ = true;
    int slot_ = 0;
    Confirm confirm_ = Confirm::None;
    std::vector<SavePointInfo> slots_;
    std::array<Texture, kSavePointSlots> pictures_;
    std::array<bool, kSavePointSlots> pictures_dirty_{};
    std::string status_;
    bool status_failed_ = false;
};

}  // namespace ds
