#pragma once

#include <optional>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "input/pad_routing.hpp"
#include "net/protocol.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/choice_popup.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"

namespace ds {

struct WiiUPlayerScreenResult {
    bool close_requested = false;
    // The menu button: the quick menu goes too.
    bool close_everything = false;
    // Controller Type chosen: its index among Cemu's choices for the player.
    int type_chosen = -1;
    // A controller chosen to be this one, by pad_identity(), trading places
    // with the one here -- or, empty, none here at all.
    std::optional<std::string> controller_chosen;
    // Remove Player, or the GamePad's Remove Controller: the controller here
    // out of play.
    bool removed = false;
    // Detect Controller, Y, put up: the caller watches every controller for
    // a press until it is put away -- see detected().
    bool detect_started = false;
};

// One of a Wii U game's players, or its GamePad, from Controls in its quick
// menu: what the game has the player as -- Controller Type, Cemu's -- and
// which controller is the one -- Controller, any of them on either device, a
// controller another player has trading places -- and Remove Player, which
// takes the player's controller out of play. Detect Controller, Y, takes
// whichever controller a button is pressed on next, for ten seconds. B goes
// back to the menu.
//
// Everything on it is the host's, which sees every controller: the page shows
// what it is handed and says what was chosen. With two devices it is one
// page on both.
class WiiUPlayerScreen : public MenuPage {
public:
    static constexpr float kDetectSeconds = 10.0f;

    void open(PadSlot slot);
    // Lets go of its icons before the GL context they live in does.
    void release_textures() {
        icons_.destroy();
        icons_tried_ = false;
    }
    void close() {
        open_ = false;
        choice_.close();
        detecting_ = false;
    }
    bool is_open() const { return open_; }
    PadSlot slot() const { return slot_; }
    void set_over_game(bool over_game) { over_game_ = over_game; }
    // What the page shows, every frame: every controller as Controls lists
    // them, and for a player the types Cemu has for it, in Cemu's words, with
    // the one it is now -- none where Cemu has no type for the player.
    void set_controllers(const std::vector<ControllerRowView>& controllers, std::vector<std::string> types,
                         int type_current);

    WiiUPlayerScreenResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                                  float output_width, float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                float output_height);
    void describe_for_sound(UiSoundView& view) const;

    // Detect Controller: counted down by the caller's clock, and put away
    // when a controller was found or the time is up.
    bool detecting() const { return detecting_; }
    int detect_seconds() const;
    void tick(float seconds);
    void detected() { detecting_ = false; }

    // The rows: Controller Type, for a player Cemu has a type for, then
    // Controller, then Remove Player -- Remove Controller, on the GamePad --
    // where there is a controller to take out of play.
    enum class Row { Type, Controller, Remove };
    int focus() const { return focus_; }
    std::vector<Row> rows() const;
    bool choosing() const { return choice_.is_open(); }
    int choice_focus() const { return choice_.focus(); }
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }
    // The other device's page, drawn from what it said. Opens the page if it
    // is not.
    void show_mirrored(const net::WiiUPlayerViewMessage& view, float output_width, float output_height);

    // MenuPage: entries are a row by its index, a choice in the popup
    // kChoiceEntryBase plus its index.
    bool scroll(const std::vector<PointerEvent>& pointers, float output_width, float output_height) override;
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float x, float y, float output_width, float output_height) const override;
    std::vector<float> scroll_state(float output_width, float output_height) const override;
    void set_scroll_state(const std::vector<float>& state, float output_width, float output_height) override;
    static constexpr int kChoiceEntryBase = 0x4000;

    // What the page is called: "Player 2", "GamePad".
    std::string title() const;
    // The row for this page's slot among the controllers handed in, if any.
    const ControllerRowView* own_row() const;

private:
    struct Geometry {
        float scale = 1.0f;
        float line = 0.0f;
        std::vector<PixelRect> rows;
        PixelRect list;
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void open_choice(float output_width, float output_height);
    void choose(int index, WiiUPlayerScreenResult& result);
    void handle_tap(int entry, WiiUPlayerScreenResult& result, float output_width, float output_height);
    void remove(WiiUPlayerScreenResult& result);
    void start_detecting(WiiUPlayerScreenResult& result);
    // A on the row in focus: its popup, or Remove.
    void act(WiiUPlayerScreenResult& result, float output_width, float output_height);
    std::string remove_label() const;
    bool removable() const;
    PopupText detect_text() const;
    PopupGeometry detect_popup(float output_width, float output_height) const;
    Row focused_row() const;

    bool open_ = false;
    bool over_game_ = false;
    PadSlot slot_ = PadSlot::gamepad();
    std::vector<ControllerRowView> controllers_;
    std::vector<std::string> types_;
    int type_current_ = -1;
    int focus_ = 0;
    ChoicePopup choice_;
    // What each of the popup's lines is: an index into Cemu's types, or for
    // Controller a controller's identity -- empty for a device's heading.
    std::vector<int> choice_types_;
    std::vector<std::string> choice_pads_;
    bool detecting_ = false;
    float detect_left_ = 0.0f;
    Texture icons_;
    bool icons_tried_ = false;
    std::vector<GuideHit> guide_hits_;
};

// The page as a message for the other device.
net::WiiUPlayerViewMessage wii_u_player_view_of(const WiiUPlayerScreen& screen);

}  // namespace ds
