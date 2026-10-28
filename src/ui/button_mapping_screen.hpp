#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/system.hpp"
#include "input/button_mapping.hpp"
#include "input/input_manager.hpp"
#include "input/wii_u_mapping.hpp"
#include "net/protocol.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/texture.hpp"
#include "render/text_renderer.hpp"
#include "ui/choice_popup.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"
#include "ui/text_entry.hpp"

namespace ds {

struct ButtonMappingScreenResult {
    bool close_requested = false;
    // The menu button: the quick menu goes too.
    bool close_everything = false;
    // A button given another job, or every one put back: the whole mapping
    // as it now is, for the caller to take on and keep with the game.
    std::optional<ButtonMapping> changed;
    // Right Stick given another job, for the caller to take on.
    std::optional<RightStickMode> right_stick;
    // A Wii U controller's mapping changed: the whole of it as it now is.
    std::optional<WiiUMapping> wii_u_changed;
    // Mapping Profiles: one loaded, by its index in the list; the mapping as
    // it is saved as a new one, under this name; the mapping as it is saved
    // over one, or one deleted, by its index.
    int profile_load = -1;
    std::optional<std::string> profile_save_new;
    int profile_save_over = -1;
    int profile_delete = -1;
};

// A DS or 3DS game's Change Controls, from Controls in its quick menu: each of
// the pad's buttons down the left as its icon, with what it does, and on the
// right the console, the button the highlighted one presses picked out on
// it. A picks another job for it from one list beside its row -- the
// console's buttons, then the rest -- Y puts a console button or Touch Point
// on turbo or takes it off, X puts every button back to the standard mapping,
// after asking, and B goes back to the menu. Touch Point asks where on the
// touch screen it touches, on the paused game's own touch screen. Last of the
// rows is the right stick's: what it is, and for the Touch Analog Stick where
// it sits on the touch screen and how big it is, placed the same way.
//
// A Wii U game's Button Mapping, from a player's page or the GamePad's, is
// the same page for one of its controllers: the console's buttons are that
// controller's, drawn beside the list, and there is no right stick's row --
// a stick is a stick on a Wii U. A Wii Remote alone has Sideways Wii Remote
// there instead, a switch: on, the remote is drawn and its D-Pad pressed as
// held on its side.
//
// Profiles are mappings kept under a name for any game to load, from the
// two buttons at the top of the right of the page -- right from the list --
// each a page of its own: Save Profile, New Profile... and then the profiles
// there are for this page's kind of mapping -- a DS game's, a Wii Remote +
// Nunchuk's -- any of them saved over after asking; and Load Profile, the
// same profiles, the one the mapping is now marked, A loading one and X
// deleting one after asking.
//
// The mapping is the game's, held by the device running it: the page shows
// what it is handed and says what was changed. With two devices it is one
// page on both, cursor, picker and all.
class ButtonMappingScreen : public MenuPage {
public:
    void open();
    void close() {
        open_ = false;
        choice_.close();
        confirming_ = false;
        placing_ = false;
        page_ = ProfilePage::None;
        naming_.close();
    }
    bool is_open() const { return open_; }
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }
    // What the page shows: the game's mapping, and the console it is played
    // on, which decides what Standard is and what can be chosen. Every frame:
    // the mapping can change under the page, from the other device.
    void set_mapping(const ButtonMapping& mapping, SystemId system, const PadConsole& console);
    // Or a Wii U controller's, as `controller`, with `detail` -- "Player 2 ·
    // Wii Remote + Nunchuk" -- at the right of the heading.
    void set_wii_u_mapping(const WiiUMapping& mapping, WiiUController controller, std::string detail);
    bool wii_u() const { return wii_u_; }
    // Profiles: the names of those there are for this page's kind of
    // mapping, which of them the mapping is now (-1 for none), and the words
    // over them -- "DS Profiles". Every frame, from whoever keeps them.
    void set_profiles(std::vector<std::string> names, int current, std::string heading);
    // Save Profile's page or Load Profile's, in place of the mapping; and the
    // row the cursor is on there -- on Save Profile's, New Profile... is 0
    // and each profile one past its index.
    enum class ProfilePage : uint8_t { None, Load, Save };
    ProfilePage profile_page() const { return page_; }
    int page_focus() const { return page_focus_; }
    // The cursor on one of the buttons over the console instead of on the
    // list: 0 Save Profile..., 1 Load Profile..., -1 neither.
    int button_focus() const { return button_focus_; }
    // A new profile being named, and what has been typed: the keyboard is to
    // type into it -- see InputManager::set_text_entry() -- what it typed
    // coming in through type(), before update().
    bool naming() const { return naming_.is_open(); }
    const std::string& name_text() const { return naming_.text(); }
    int naming_focus() const { return naming_.focus(); }
    void type(const std::string& typed, int backspaces) {
        typed_ = typed;
        backspaces_ = backspaces;
    }
    // Whether the presses and taps update() is about to hear were made on
    // the other device -- the companion working the host's page. A name
    // asked for by them is typed there, on its keyboard, not on this one.
    void set_input_from_other_device(bool other) { input_from_other_device_ = other; }
    bool naming_elsewhere() const { return naming_.is_open() && naming_elsewhere_; }
    // The system's own keyboard, where there is one (see
    // edit_with_system_keyboard()): a name asked for here is typed on it
    // there and then, rather than into the popup.
    void set_text_editor(std::function<bool(const std::string& header, const std::string& initial,
                                            std::size_t max_characters, std::string& out)>
                             editor) {
        text_editor_ = std::move(editor);
    }
    // Typed on the other device.
    void set_name_text(const std::string& text) {
        if (naming_.is_open()) naming_.set_text(text);
    }
    // Typed here into the other device's popup, which is drawn as typed
    // rather than as that device last said for a moment, until it has caught
    // up. True if the name changed.
    bool type_mirrored(const std::string& typed, int backspaces);
    // What the question up over the page asks.
    enum class Asking : uint8_t { Reset, Delete, SaveOver };
    Asking asking() const { return asking_; }
    int asking_profile() const { return asking_profile_; }
    const std::vector<std::string>& profile_names() const { return profile_names_; }
    int profile_current() const { return profile_current_; }
    const std::string& profile_heading() const { return profile_heading_; }
    WiiUController wii_u_controller() const { return wii_u_controller_; }
    const WiiUMapping& wii_u_mapping() const { return wii_u_mapping_; }
    const std::string& detail() const { return detail_; }
    // The game's touch screen as it was paused, for placing a Touch Point
    // on. None draws an empty screen.
    void set_touch_screen(const Texture* touch_screen) { touch_screen_ = touch_screen; }

    ButtonMappingScreenResult update(const std::vector<UiAction>& actions,
                                     const std::vector<PointerEvent>& pointers, float output_width,
                                     float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                float output_width, float output_height);
    void describe_for_sound(UiSoundView& view) const;

    // The row the cursor is on: an index into kPadControlRows, or past its
    // end the right stick's, kRightStickRow.
    int focus() const { return focus_; }
    static constexpr int kRightStickRow = static_cast<int>(kPadControls);
    static constexpr int kRows = kRightStickRow + 1;
    // How many rows this page has: a Wii U controller's has no right stick's,
    // but a Wii Remote alone has Sideways Wii Remote's in its place.
    int row_count() const { return wii_u_ && !has_sideways_row() ? kRightStickRow : kRows; }
    bool has_sideways_row() const { return wii_u_ && wii_u_controller_ == WiiUController::Remote; }
    bool on_sideways() const { return has_sideways_row() && focus_ == kRightStickRow; }
    bool choosing() const { return choice_.is_open(); }
    int choice_focus() const { return choice_.focus(); }
    bool confirming() const { return confirming_; }
    int confirm_focus() const { return confirm_cursor_.focus(); }
    // What a row says it does: "L Button", say, or "L Button (turbo)".
    std::string action_label(PadControl control) const;
    // A Touch Point being placed, or the Touch Analog Stick, and where so
    // far; the stick's size, as a fraction of the touch screen's height.
    bool placing() const { return placing_; }
    bool placing_stick() const { return placing_ && placing_stick_; }
    const TouchSpot& spot() const { return spot_; }
    float stick_radius() const { return stick_radius_; }
    // The spot put where the other device's finger has it, while placing.
    void set_spot(const TouchSpot& spot) {
        if (placing_) spot_ = TouchSpot{std::clamp(spot.u, 0.0f, 1.0f), std::clamp(spot.v, 0.0f, 1.0f)};
    }
    // The pad moving the spot, as the Touch Cursor moves in a game: `push_x`
    // and `push_y` how far it is pushed past its deadzone, 0 to 1 each way
    // (see InputManager::menu_push()), a full push crossing the screen in
    // StylusCursor::kCrossingSeconds and a little one creeping. True if the
    // spot moved. A page drawn from the other device's keeps where it was
    // moved here for a moment, rather than going back to where the other
    // device last said, until the other device has caught up.
    bool steer(float push_x, float push_y, float seconds);
    // A finger down on the touch screen being placed on moves the spot with
    // it as it goes, not only once it lifts. True while one is.
    bool drag_spot(float output_width, float output_height);
    // Where the prompts at the foot of the page were drawn -- see
    // take_guide_taps() -- for a device drawing the other's page.
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }

    // The other device's page, drawn from what it said: its cursor, its
    // picker or question, and nothing changed here. Opens the page if it is
    // not.
    void show_mirrored(const net::ButtonMappingViewMessage& view, float output_width,
                       float output_height);

    // MenuPage: the list scrolls under a finger, the mouse and the wheel, and
    // the picker does while it is up. Entries: a row is its index, a
    // choice in the picker kChoiceEntryBase plus its index, a button of
    // Reset to Default's question kConfirmEntryBase plus its, a spot on
    // the touch screen being placed on kSpotEntryBase plus spot_entry(), and
    // the OK under it kPlacingOkEntry, Save Profile... and Load Profile...
    // kProfileButtonEntry plus 0 or 1, and a row of a profile page
    // kPageEntryBase plus its index.
    bool scroll(const std::vector<PointerEvent>& pointers, float output_width,
                float output_height) override;
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float x, float y, float output_width, float output_height) const override;
    // Anywhere on the touch screen being placed on is the same: the spot
    // goes where the finger came up.
    bool same_entry(int pressed_on, int released_on) const override {
        return pressed_on == released_on || (pressed_on >= kSpotEntryBase && released_on >= kSpotEntryBase);
    }
    std::vector<float> scroll_state(float output_width, float output_height) const override;
    void set_scroll_state(const std::vector<float>& state, float output_width, float output_height) override;
    static constexpr int kChoiceEntryBase = 0x4000;
    static constexpr int kConfirmEntryBase = 0x4800;
    static constexpr int kPlacingOkEntry = 0x5000;
    static constexpr int kSpotEntryBase = 0x10000;
    // A button of a new profile's name's popup.
    static constexpr int kNamingEntryBase = 0x5800;
    static constexpr int kProfileButtonEntry = 0x6000;
    static constexpr int kPageEntryBase = 0x6800;
    // How the cursor on a button is said in ButtonMappingViewMessage::focus.
    static constexpr int kButtonFocusBase = 200;
    // A spot as an entry, to 1/1024 of the screen each way, and back.
    static int spot_entry(const TouchSpot& spot);
    static TouchSpot entry_spot(int entry);

private:
    struct Geometry {
        float scale = 1.0f;
        float line = 0.0f;
        float view_top = 0.0f;
        float view_bottom = 0.0f;
        PixelRect list;
        std::vector<PixelRect> rows;
        float content_height = 0.0f;
        // Where the console is drawn, and the words under it; over it, Save
        // Profile... and Load Profile..., and the profile the mapping is.
        PixelRect diagram;
        float caption_y = 0.0f;
        PixelRect save_button;
        PixelRect load_button;
        float profile_y = 0.0f;
        float rule_y = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - (view_bottom - view_top)); }
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void scroll_focus_into_view(const Geometry& geometry);
    // The picker up beside the focused row, the cursor on what it does now.
    void open_choice(float output_width, float output_height);
    void choose(int index, ButtonMappingScreenResult& result);
    void ask_revert();
    void answer_revert(int button, ButtonMappingScreenResult& result);
    // A profile page: up in place of the mapping, the cursor on `focus`;
    // the profile a row of it is, or -1; its rows, and where they are; one
    // chosen, X pressed on one, and the name typed; and back to the mapping,
    // the cursor on the page's button.
    void open_page(ProfilePage page, int focus);
    int profile_of_row(int row) const;
    int page_rows() const;
    struct PageGeometry {
        float scale = 1.0f;
        float line = 0.0f;
        PixelRect list;
        std::vector<PixelRect> rows;
        float content_height = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - list.h); }
    };
    PageGeometry compute_page_geometry(float output_width, float output_height) const;
    void choose_on_page(int row, ButtonMappingScreenResult& result);
    void ask_about_profile(Asking asking);
    void back_from_page();
    void update_page(const std::vector<UiAction>& actions, ButtonMappingScreenResult& result);
    void render_page(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                     float output_height);
    void start_naming(ButtonMappingScreenResult& result);
    void finish_naming(const TextEntryResult& answer, ButtonMappingScreenResult& result);
    // Turbo's picker, beside the same row, after Turbo was chosen.
    // Y: the focused button made to press its console button over and over
    // while it is held, or back to pressing it once.
    void toggle_turbo(ButtonMappingScreenResult& result);
    bool can_toggle_turbo() const;
    bool is_turbo_button(PadAction action) const;
    bool is_wii_u_turbo_button(WiiUAction action) const;
    // The Wii U controller's halves of the above.
    void open_wii_u_choice(const PixelRect& row);
    void choose_wii_u(int index, ButtonMappingScreenResult& result);
    void toggle_wii_u_turbo(ButtonMappingScreenResult& result);
    std::string wii_u_caption() const;
    // Sideways Wii Remote turned on or off.
    void toggle_sideways(ButtonMappingScreenResult& result);
    // A on the focused row: its picker, or Sideways Wii Remote's switch.
    void activate(ButtonMappingScreenResult& result, float output_width, float output_height);
    // Touch Point's page, the spot where the button touches now.
    void start_placing();
    void finish_placing(ButtonMappingScreenResult& result);
    // Back a step from Turbo's picker or Touch Point's page: the first
    // picker again, on what was being chosen.
    void back_to_choice(PadAction on, float output_width, float output_height);
    void move_spot(float du, float dv);
    // Where the touch screen is drawn while a Touch Point is placed.
    PixelRect touch_rect(float output_width, float output_height) const;
    // The OK button under it, keeping the spot as A does.
    PixelRect placing_ok_rect(float output_width, float output_height) const;

    void render_placing(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                        float output_width, float output_height);
    PopupGeometry confirm_popup(float output_width, float output_height) const;
    PopupText confirm_text() const;
    std::vector<PopupButton> confirm_buttons() const;
    void handle_tap(int entry, ButtonMappingScreenResult& result, float output_width,
                    float output_height);
    bool on_right_stick() const { return !wii_u_ && focus_ == kRightStickRow; }
    // The control the cursor is on; meaningless on the right stick's row.
    PadControl focused_control() const {
        return kPadControlRows[static_cast<std::size_t>(std::min(focus_, kRightStickRow - 1))];
    }
    // The icon a row is drawn with.
    ButtonGlyph row_glyph(int row) const;
    // What the right stick's row says it is, and the picker's choices for it.
    std::vector<RightStickMode> stick_modes() const;
    void start_placing_stick();
    // What the console is called on the page: "DS", "3DS".
    std::string console_name() const;
    // The words under the console, for what the focused button does: after
    // the button itself, drawn as its icon in the player's own style.
    std::string caption() const;
    void draw_console(QuadRenderer& quads, const PixelRect& box, PadControl control) const;

    bool open_ = false;
    bool over_game_ = false;
    ButtonMapping mapping_;
    // A Wii U controller's page instead.
    bool wii_u_ = false;
    WiiUMapping wii_u_mapping_;
    WiiUController wii_u_controller_ = WiiUController::Pro;
    std::string detail_;
    std::vector<WiiUAction> choice_wii_u_;
    // Profiles, and the page of them up, its cursor and how far it is
    // scrolled; the cursor on a button over the console.
    std::vector<std::string> profile_names_;
    int profile_current_ = -1;
    std::string profile_heading_;
    ProfilePage page_ = ProfilePage::None;
    int page_focus_ = 0;
    float page_scroll_ = 0.0f;
    ListDrag page_drag_;
    int button_focus_ = -1;
    TextEntryPopup naming_;
    bool input_from_other_device_ = false;
    bool naming_elsewhere_ = false;
    std::function<bool(const std::string&, const std::string&, std::size_t, std::string&)> text_editor_;
    std::string typed_;
    int backspaces_ = 0;
    // How many more of the other device's views keep the name typed here.
    int own_name_views_ = 0;
    // The question's subject, and the profile it is about.
    Asking asking_ = Asking::Reset;
    int asking_profile_ = -1;
    SystemId system_ = SystemId::Ds;
    PadConsole console_;
    int focus_ = 0;
    float scroll_ = 0.0f;
    ListDrag drag_;
    ScrollEase scroll_ease_;
    // The picker, and the action each of its lines is -- or on the right
    // stick's row, the mode.
    ChoicePopup choice_;
    std::vector<PadAction> choice_actions_;
    std::vector<RightStickMode> choice_modes_;
    // Touch Point's page.
    bool placing_ = false;
    TouchSpot spot_;
    // Placing the Touch Analog Stick rather than a Touch Point, and its size.
    bool placing_stick_ = false;
    float stick_radius_ = kDefaultStickRadius;
    // How much longer the spot steer() moved stays this page's own.
    float own_spot_seconds_ = 0.0f;
    const Texture* touch_screen_ = nullptr;
    // Reset to Default's question, up over the page.
    bool confirming_ = false;
    PopupCursor confirm_cursor_;
    // The other device's cursor as last drawn here: the view follows it only
    // when it moves, and is otherwise where a finger here left it.
    int mirrored_focus_ = -1;
    float last_width_ = 1280.0f;
    float last_height_ = 800.0f;
    std::vector<GuideHit> guide_hits_;
};

// The page as a message for the other device.
net::ButtonMappingViewMessage button_mapping_view_of(const ButtonMappingScreen& screen);

}  // namespace ds
