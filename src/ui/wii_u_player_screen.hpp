#pragma once

#include <optional>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "input/pad_routing.hpp"
#include "input/wii_u_mapping.hpp"
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
    // Remove Controller: the player gone, and every one after it a place up.
    bool removed = false;
    // Add Controller's page, added: the new player, as type_chosen.
    bool added = false;
    // Edit Button Mapping: its page, for what the game has this one as.
    bool edit_mapping = false;
    // Profile Switching changed: its button, or a profile in or out.
    std::optional<ProfileSwitching> switching_changed;
};

// One of a Wii U game's players, or its GamePad, from Controls in its quick
// menu: what the game has the player as -- Controller Type, Cemu's -- and
// Edit Button Mapping, which opens what each of its buttons presses, for the
// controller the game has it as -- see ButtonMappingScreen -- and for a
// player, Remove Controller. Which controller is which is Change Order's, in
// Controls. B goes back to the menu.
//
// Profile Switching, after Edit Button Mapping, is Off, Hold - or Hold + --
// see ProfileSwitching -- and while it is on, under Switch Between, each
// profile there is for the controller with a switch: which it moves through.
//
// Add Controller opens the page of the player after the last, yet to be: its
// type is chosen first, and Add makes it one -- so a player is never there
// with nothing said about it.
//
// Everything on it is the host's, which sees every controller: the page shows
// what it is handed and says what was chosen. With two devices it is one
// page on both.
class WiiUPlayerScreen : public MenuPage {
public:
    void open(PadSlot slot);
    // A new player's page.
    void open_adding(PadSlot slot);
    bool adding() const { return adding_; }
    // On a new player's page, what Add will make it: an index into Cemu's
    // types, -1 for none.
    int pending_type() const { return pending_type_; }
    // Lets go of its icons before the GL context they live in does.
    void release_textures() {
        icons_.destroy();
        icons_tried_ = false;
    }
    void close() {
        open_ = false;
        choice_.close();
    }
    bool is_open() const { return open_; }
    PadSlot slot() const { return slot_; }
    void set_over_game(bool over_game) { over_game_ = over_game; }
    // What the page shows, every frame: every controller as Controls lists
    // them, and for a player the types Cemu has for it, in Cemu's words, with
    // the one it is now -- none where Cemu has no type for the player. A new
    // player's is the one it starts as.
    void set_controllers(const std::vector<ControllerRowView>& controllers, std::vector<std::string> types,
                         int type_current);
    // And Profile Switching as the game has it for the player, every profile
    // there is for its controller, and the one its mapping is now -- empty
    // for none. Every frame.
    void set_profile_switching(ProfileSwitching switching, std::vector<std::string> profiles,
                               std::string mapping_profile);
    const ProfileSwitching& switching() const { return switching_; }
    const std::vector<std::string>& profile_names() const { return profile_names_; }
    const std::string& mapping_profile() const { return mapping_profile_; }

    WiiUPlayerScreenResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                                  float output_width, float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                float output_height);
    void describe_for_sound(UiSoundView& view) const;

    // The rows: Controller Type, for a player Cemu has a type for, then
    // Edit Button Mapping, Profile Switching and while it is on a row for
    // each profile, then for a player Remove Controller. A new player's has
    // Add instead of all after the first.
    enum class Row { Type, Remove, Add, Mapping, Switching, Profile };
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
        float content_height = 0.0f;
        // Where Switch Between is written, over the profiles.
        float heading_y = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - list.h); }
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void open_choice(float output_width, float output_height);
    void choose(int index, WiiUPlayerScreenResult& result);
    void handle_tap(int entry, WiiUPlayerScreenResult& result, float output_width, float output_height);
    // A on the row in focus: its popup, Remove or Add.
    void act(WiiUPlayerScreenResult& result, float output_width, float output_height);
    bool removable() const;
    Row focused_row() const;
    // The profile a row is, by its index among the profiles; -1 for none.
    int profile_of_row(int row) const;
    void scroll_focus_into_view(const Geometry& geometry);

    bool open_ = false;
    bool over_game_ = false;
    bool adding_ = false;
    int pending_type_ = -1;
    PadSlot slot_ = PadSlot::gamepad();
    std::vector<ControllerRowView> controllers_;
    std::vector<std::string> types_;
    int type_current_ = -1;
    int focus_ = 0;
    ChoicePopup choice_;
    // What each of the popup's lines is: an index into Cemu's types.
    std::vector<int> choice_types_;
    ProfileSwitching switching_;
    std::vector<std::string> profile_names_;
    std::string mapping_profile_;
    float scroll_ = 0.0f;
    ListDrag drag_;
    ScrollEase scroll_ease_;
    // A button's width for its height, as the page last drew them, for the
    // popup's.
    float glyph_aspect_ = 1.0f;
    Texture icons_;
    bool icons_tried_ = false;
    std::vector<GuideHit> guide_hits_;
};

// The page as a message for the other device.
net::WiiUPlayerViewMessage wii_u_player_view_of(const WiiUPlayerScreen& screen);

}  // namespace ds
