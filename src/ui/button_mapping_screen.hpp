#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/system.hpp"
#include "input/button_mapping.hpp"
#include "input/input_manager.hpp"
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

namespace ds {

struct ButtonMappingScreenResult {
    bool close_requested = false;
    // The menu button: the quick menu goes too.
    bool close_everything = false;
    // A button given another job, or every one put back: the whole mapping
    // as it now is, for the caller to take on and keep with the game.
    std::optional<ButtonMapping> changed;
};

// A DS or 3DS game's Change Controls, from Controls in its quick menu: each of
// the pad's buttons down the left as its icon, with what it does, and on the
// right the console, the button the highlighted one presses picked out on
// it. A picks another job for it from one list beside its row -- the
// console's buttons, then the rest -- Y puts a console button or Touch Point
// on turbo or takes it off, X puts every button back to the standard mapping,
// after asking, and B goes back to the menu. Touch Point asks where on the
// touch screen it touches, on the paused game's own touch screen.
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
    }
    bool is_open() const { return open_; }
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }
    // What the page shows: the game's mapping, the console it is played on --
    // which decides what Standard is and what can be chosen -- and the
    // game's name for the heading. Every frame: the mapping can change under
    // the page, from the other device.
    void set_mapping(const ButtonMapping& mapping, SystemId system, const PadConsole& console,
                     const std::string& game_title);
    // The game's touch screen as it was paused, for placing a Touch Point
    // on. None draws an empty screen.
    void set_touch_screen(const Texture* touch_screen) { touch_screen_ = touch_screen; }

    ButtonMappingScreenResult update(const std::vector<UiAction>& actions,
                                     const std::vector<PointerEvent>& pointers, float output_width,
                                     float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                float output_width, float output_height);
    void describe_for_sound(UiSoundView& view) const;

    // The row the cursor is on: an index into kPadControlRows.
    int focus() const { return focus_; }
    bool choosing() const { return choice_.is_open(); }
    int choice_focus() const { return choice_.focus(); }
    bool confirming() const { return confirming_; }
    int confirm_focus() const { return confirm_cursor_.focus(); }
    // What a row says it does: "L Button", say, or "L Button (turbo)".
    std::string action_label(PadControl control) const;
    // A Touch Point being placed, and where so far.
    bool placing() const { return placing_; }
    const TouchSpot& spot() const { return spot_; }
    // The spot put where the other device's finger has it, while placing.
    void set_spot(const TouchSpot& spot) {
        if (placing_) spot_ = TouchSpot{std::clamp(spot.u, 0.0f, 1.0f), std::clamp(spot.v, 0.0f, 1.0f)};
    }
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
    // the OK under it kPlacingOkEntry.
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
        // Where the console is drawn, and the words under it.
        PixelRect diagram;
        float caption_y = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - (view_bottom - view_top)); }
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void scroll_focus_into_view(const Geometry& geometry);
    // The picker up beside the focused row, the cursor on what it does now.
    void open_choice(float output_width, float output_height);
    void choose(int index, ButtonMappingScreenResult& result);
    void ask_revert();
    void answer_revert(int button, ButtonMappingScreenResult& result);
    // Turbo's picker, beside the same row, after Turbo was chosen.
    // Y: the focused button made to press its console button over and over
    // while it is held, or back to pressing it once.
    void toggle_turbo(ButtonMappingScreenResult& result);
    bool can_toggle_turbo() const;
    bool is_turbo_button(PadAction action) const;
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
    PadControl focused_control() const { return kPadControlRows[static_cast<std::size_t>(focus_)]; }
    // What the console is called on the page: "DS", "3DS".
    std::string console_name() const;
    // The words under the console, for what the focused button does: after
    // the button itself, drawn as its icon in the player's own style.
    std::string caption() const;
    void draw_console(QuadRenderer& quads, const PixelRect& box, PadControl control) const;

    bool open_ = false;
    bool over_game_ = false;
    ButtonMapping mapping_;
    SystemId system_ = SystemId::Ds;
    PadConsole console_;
    std::string game_title_;
    int focus_ = 0;
    float scroll_ = 0.0f;
    ListDrag drag_;
    ScrollEase scroll_ease_;
    // The picker, and the action each of its lines is.
    ChoicePopup choice_;
    std::vector<PadAction> choice_actions_;
    // Touch Point's page.
    bool placing_ = false;
    TouchSpot spot_;
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
