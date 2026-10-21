#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/save_points.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/button_glyphs.hpp"
#include "render/compositor.hpp"
#include "render/texture.hpp"
#include "render/text_renderer.hpp"
#include "ui/choice_popup.hpp"
#include "ui/menu_page.hpp"
#include "ui/control_guide.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

struct LayoutPickerResult {
    bool close_requested = false;
    bool selection_changed = false;  // the layout changed; worth persisting
    // Swap Screens on a Wii U's list: the other screen, for this game.
    bool screens_swap_toggled = false;

    bool quit_requested = false;
    // Ends the two-device session. Offered on both devices, since either end
    // should be able to stop it.
    bool disconnect_requested = false;
    // The filtering toggle was used; the caller applies and persists it.
    bool filtering_toggled = false;
    // The sound target was cycled; same deal. DS/3DS only -- a Wii U's TV and
    // GamePad audio are not a player choice at all, see
    // LayoutPickerState::audio_target.
    bool audio_target_changed = false;
    // The pointer mode was cycled. Unlike the two above it belongs to this
    // device alone -- the other one has its own pointer, or none.
    bool cursor_mode_changed = false;
    // Which hand holds the stylus was toggled, mirroring the handed layouts.
    bool handedness_changed = false;
    // Separate Screens' swap button was pressed: the caller swaps which
    // screen this device shows -- see Layout::swaps_screens.
    bool screens_swapped = false;
    // The render scale was cycled. Like the pointer, it belongs to this device
    // alone -- it is a setting on the core, and only one device runs one.
    bool render_scale_changed = false;
    // Reset was chosen. Unlike the settings above there is no new value to
    // adopt -- the caller just restarts the game.
    bool reset_requested = false;
    // The gap toggle was used; the caller applies and persists it, the same
    // as filtering.
    bool screen_gap_toggled = false;
    // The right stick's mapping was cycled; same deal again.
    bool right_stick_mode_changed = false;
    // The Rumble Pak was put in or taken out. The game's, like the scale.
    bool rumble_pak_toggled = false;
    // The LCD filter was turned on or off. The game's too.
    bool lcd_filter_toggled = false;
    // A 3DS game's custom textures were turned on or off. The game's too.
    bool custom_textures_toggled = false;
    // Resume from Save State was turned on or off. The game's too.
    bool resume_toggled = false;
    // The settings screen was asked for. Not a value to adopt: the caller puts
    // that screen up, and it belongs to the device running the game however
    // this was pressed.
    bool settings_requested = false;
    // The same for the Controllers screen -- a Wii U's, which is the one
    // console whose controllers are each somebody's.
    bool controllers_requested = false;
    // And for a DS or 3DS game's Button Mapping, which is the game's: the
    // device running it puts the page up, on both devices.
    bool button_mapping_requested = false;
    // And for the Wii U game's graphics packs, which are Cemu's, on the
    // machine running it.
    bool graphic_packs_requested = false;
    // One of Cemu's own options in a Wii U's Options, stepped on: its index
    // in LayoutPickerState::producer_options. -1 for none.
    int producer_option = -1;
    // How many values on the option changed above goes: what was chosen in
    // its popup, counted from what it was in the order each one steps. One
    // for a switch flipped.
    int option_steps = 1;
    // An amiibo in the Scan amiibo list chosen, by its index in
    // LayoutPickerState::amiibo, or the row after them that looks elsewhere.
    int amiibo_chosen = -1;
    bool amiibo_folder_requested = false;
    // And for this game's page in Data Management.
    bool save_sync_requested = false;
    // A save point slot to save into or to load, already confirmed where
    // that throws something away: the caller does it, and hands the slots
    // back with set_save_slots(). -1 for none. Loading is also the end of
    // the menu -- the game picks up from there -- which is the caller's to
    // do, since the menu does not know whether it worked.
    int save_slot = -1;
    int load_slot = -1;
};

// Everything the menu needs to know about the state it is opening onto. A
// struct rather than a row of positional flags, because there are now enough of
// them that a call site would be unreadable.
struct LayoutPickerState {
    std::string layout_id;
    // Two devices are connected: adds Disconnect, and a sound target worth
    // choosing.
    bool session_active = false;
    bool bilinear_filtering = false;
    // A DS or a 3DS's single audio stream, the only one where which device it
    // plays through is a real choice -- a Wii U's own TV and GamePad audio
    // are not offered here at all: TV audio always stays on the machine
    // running Cemu, and GamePad audio always goes to whichever device is
    // connected, the same rule ShouldCapture already applies to that
    // screen's video. See app.cpp's External source audio callback.
    AudioTarget audio_target = AudioTarget::Both;
    CursorMode cursor_mode = CursorMode::Shown;
    // Whether this device has a pointer to show or hide at all.
    //
    // False on a companion client running on hardware with no mouse or
    // trackpad -- the Switch one, and whatever comes after it. It is the one
    // entry on this menu that belongs to the device rather than to the host,
    // which is exactly why it has nothing to say there, and an entry that
    // cannot do anything is worse than an absent one. Same rule as the render
    // scale and the right stick: offered where there is something to choose.
    bool cursor_choosable = true;
    // A Wii U's Swap Screens: alone, the other screen from the setting's for
    // this game (screens_swapped says whether it is, shown beside it); with a
    // second device, the two trading screens. Offered on both devices alike,
    // so that their lists line up row for row.
    bool screens_swappable = false;
    bool screens_swapped = false;
    bool screens_swap_value_shown = false;
    // Cemu can have the game's graphics packs changed from here -- see
    // GraphicPacksScreen. The host's to say, and told to the other device,
    // so that their lists line up row for row.
    bool graphic_packs_available = false;
    // Cemu's own options, which a Wii U's Options lists after its own: each
    // one's name and the value it has. The host's, read off Cemu, and told to
    // the other device so the two lists line up.
    std::vector<std::pair<std::string, std::string>> producer_options;
    // Each of those's choices, in Cemu's order, for its popup. Told to the
    // other device with them.
    std::vector<std::vector<std::string>> producer_choices;
    // Where in producer_options the rows that belong in Controls -- each
    // player's controller type -- begin, or -1 for none: those are listed
    // there, under Controller Order, rather than in Game Options.
    int producer_controls_from = -1;
    // Scan amiibo, where Cemu can: the amiibo found in `amiibo_folder`, by
    // name, listed on the right to be chosen from directly. The host's too.
    bool amiibo_available = false;
    // The game is looking for an amiibo as the menu comes up from it: the
    // menu opens on Scan amiibo, at its first amiibo, unless it was closed
    // there -- then where it was in it.
    bool amiibo_wanted = false;
    std::vector<std::string> amiibo;
    std::string amiibo_folder;
    // Which layouts are flipped for a left hand, by id: each is chosen on its
    // own, with the stylus button under the layouts, and each preview is
    // drawn the way round its own layout is.
    std::vector<std::string> left_handed_layouts;
    // How many pixels the core renders per console pixel, and the largest this
    // system offers. A maximum of 1 means there is nothing to choose and the
    // button does not appear.
    int render_scale = 1;
    int max_render_scale = 1;
    // Whether the current layout shows the gap between its screens. Only
    // meaningful -- and only offered -- while a layout that has one is
    // selected, which the picker reads off the registry for itself.
    bool screen_gap = true;
    // What the right stick is read as, whether that is a question at all, and
    // the answers the console has: a 3DS every one, a DS a stylus or nothing.
    // See RightStickMode and right_stick_modes().
    RightStickMode right_stick_mode = RightStickMode::CStick;
    bool right_stick_choosable = false;
    std::vector<RightStickMode> right_stick_modes;
    // A DS or 3DS game's Change Button Mapping, at the top of Controls.
    bool button_mapping_available = false;
    // A Rumble Pak in a DS's GBA slot, and whether that is on offer at all:
    // only where the core running the game can put one there. Nothing says
    // which DS games use one, so it is offered for every game there.
    bool rumble_pak = false;
    bool rumble_choosable = false;
    // A DS game's screens drawn with an LCD's pixel grid over them, and
    // whether that is on offer: a DS game's, kept for it.
    bool lcd_filter = false;
    bool lcd_filter_choosable = false;
    // A 3DS game drawn with a texture pack, and whether that is on offer:
    // only where the core running the game can load one.
    bool custom_textures = false;
    bool custom_textures_choosable = false;
    // Whether Reset makes sense to offer. False for a Wii U session: Cemu owns
    // restarting the game, and nothing here can do it for it. See
    // SystemProfile::offers_reset.
    bool reset_available = true;
    // Whether Screen Smoothing makes sense to offer. False for a Wii U
    // session: RightStick (R3) is already claimed by the game there --
    // game_uses_every_control means input_manager never turns it into
    // ToggleFilter in the first place -- so showing the prompt would advertise
    // a control that silently does nothing. See SystemProfile::offers_smoothing.
    bool smoothing_available = true;
    // Shown in place of the screen a Wii U host would otherwise show while
    // Cemu's own window plays the game, rather than opened over it: nothing is
    // paused, so there is nothing to resume and Back does nothing -- going
    // back to the game is going back to Cemu's window. `subtitle` goes under
    // the game's title.
    bool embedded = false;
    std::string subtitle;
    // Whether the core running the game can make save points at all -- see
    // LibretroCore::has_save_states(). Never for a Wii U, whose emulator is not
    // ours to ask.
    bool save_points_available = false;
    // Whether the game has a save that Data Management can sync: a DS game
    // played on the device holding the menu. Adds Save Sync.
    bool save_sync_available = false;
    // Whether the game starts where it was left, from a save state kept up to
    // date while it plays, and whether that is on offer at all: only where
    // save states are. See GameConfig::resume_from_save_state.
    bool resume_from_save_state = false;
    bool resume_choosable = false;
};

// What has been asked for and not yet been allowed to happen. Each of these
// throws away something the player cannot get back -- unsaved progress, or a
// save point -- so each is asked about first. Travels over the wire, so the
// numbers are part of the protocol.
enum class MenuConfirm : uint8_t {
    None = 0,
    Reset = 1,
    Quit = 2,
    LoadSavePoint = 3,
    OverwriteSavePoint = 4,
    // 5 was a question about a DS render scale restarting the game; a DS no
    // longer has a render scale. Left unused so the numbers on the wire keep
    // their meaning.
    // Not a question: something to read and put away with OK -- that a save
    // state was saved, or why one could not be saved or loaded. The words are
    // the save status; see set_save_status().
    Notice = 6,
    // Not a question either: an option's choices, beside its row -- see
    // ChoicePopup. Its cursor travels as the popup's focus.
    Choice = 7,
};

// The list down the left of a DS or 3DS quick menu, the way Nintendo Switch
// Online's own menu is laid out: each entry either shows its choices on the
// right, or does something. Travels over the wire inside a focus -- see
// LayoutPicker::focus_of() -- so the numbers are part of the protocol, and are
// the same whichever entries a given session happens to have.
enum class MenuSection : uint8_t {
    Layout = 0,
    CreateSavePoint = 1,
    LoadSavePoint = 2,
    Options = 3,
    Disconnect = 4,
    Reset = 5,
    Quit = 6,
    // First in the list, but numbered last: it came last, and the numbers
    // travel.
    Continue = 7,
    // This game's page in Data Management. Past the last number a focus
    // byte has room for, so it travels as a number of its own -- see
    // LayoutPicker::kSaveSyncFocus.
    SaveSync = 8,
    // A Wii U's: pages of the device running Cemu, and the amiibo it can
    // scan. Past a focus byte too, so they travel in the wider focus -- see
    // LayoutPicker::kWideFocusBase.
    ControllerOrder = 9,
    GraphicPacks = 10,
    Amiibo = 11,
};

// The quick menu. On a DS or a 3DS, two columns: the sections down the left
// -- Continue, Screen Layout, Create and Load Save Point, Options, then
// Disconnect, Reset Game and Quit -- and whichever of them is highlighted shows its choices on
// the right: the layouts as pictures of the game in each, the save point
// slots, or the options as a list. A Wii U's is a plain list; see
// rebuild_footer().
//
// There is one of these per session, not one per device. Whichever machine
// runs the game holds it; the other draws the same menu from what it is told
// and sends its presses back, so the player can work the menu from either pad
// while looking at either screen.
class LayoutPicker : public MenuPage {
public:
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;
    explicit LayoutPicker(const LayoutRegistry& registry);

    void open(const LayoutPickerState& state);
    // The row of buttons this device is offering. Rebuilt when a companion
    // arrives or leaves, since Disconnect and the sound target come and go
    // with it and a menu whose entries differed would put the two cursors on
    // different things.
    void set_session_active(bool active);
    // Which screen this device shows in a layout that gives each device one
    // -- Separate Screens -- so its tile shows that screen alone. Each
    // device's own, rather than shared menu state.
    void set_preview_screen(ScreenId screen) { preview_screen_ = screen; }
    // Whether a confirmation is up, which the app needs to know: while one is,
    // the menu is not what the player is answering.
    bool awaiting_confirmation() const { return confirm_ != MenuConfirm::None; }

    // The whole of what the other device has to be told to draw the same menu,
    // and what it hands back when it is the one being told.
    //
    // On a Wii U, the row. On a DS or a 3DS, one number for the section and
    // the thing in it: see focus_of().
    int focus() const;
    // A DS or 3DS focus: `section` on the left, or its `item` on the right.
    // Sections are 32 apart, so a focus fits the byte it travels in.
    static constexpr int kSectionStride = 32;
    // Save Sync's focus: the last number Continue's stride holds, which no
    // item of Continue's -- it has none -- will ever need.
    static constexpr int kSaveSyncFocus = 255;
    // The sections past those, and their things -- an amiibo list can be
    // long -- from here on, 1024 apart: these need the focus's wider form,
    // which a peer that predates them never sees.
    static constexpr int kWideFocusBase = 256;
    static constexpr int kWideStride = 1024;
    static int focus_of(MenuSection section, int item = -1) {
        if (section == MenuSection::SaveSync) return kSaveSyncFocus;
        const int section_number = static_cast<int>(section);
        if (section_number >= static_cast<int>(MenuSection::ControllerOrder)) {
            return kWideFocusBase +
                   (section_number - static_cast<int>(MenuSection::ControllerOrder)) * kWideStride +
                   (item >= 0 ? item + 1 : 0);
        }
        return section_number * kSectionStride + (item >= 0 ? item + 1 : 0);
    }
    MenuSection section() const { return section_; }
    bool on_content() const { return on_content_; }
    // How many things a section lists on the right: its rows, slots or tiles.
    int items_in(MenuSection section) const { return content_count(section); }
    // What is in each save point slot, and one line about the last save.
    // Read off the disk by the device running the game, and handed on to the
    // other one, which has only what it is told.
    void set_save_slots(std::vector<SavePointInfo> slots);
    // Lets go of the save slots' pictures before the GL context they live in
    // does, and draws them again the next time they are shown. Needed when
    // the window is rebuilt under an open menu -- a DS game moving to or from
    // its OpenGL renderer, from the menu's own Render Scale.
    void release_textures() {
        for (std::size_t slot = 0; slot < slot_pictures_.size(); ++slot) {
            slot_pictures_[slot].destroy();
            slot_pictures_dirty_[slot] = true;
        }
        ui_icons_.destroy();
        ui_icons_tried_ = false;
    }
    void set_save_status(std::string status, bool failed);
    // Puts the save status up as a popup with an OK to dismiss it -- see
    // MenuConfirm::Notice.
    void show_notice() {
        if (!save_status_.empty()) ask(MenuConfirm::Notice);
    }
    MenuConfirm confirmation() const { return confirm_; }
    // Which of the popup's buttons the cursor is on, plus one, or 0 when it
    // is not showing: what the other device is told, to draw the same.
    int confirm_focus_wire() const {
        if (confirm_ == MenuConfirm::Choice) return choice_.focus() + 1;
        return confirming() && confirm_cursor_.shown() ? confirm_cursor_.focus() + 1 : 0;
    }
    // A focus that stands for one of the popup's buttons rather than for
    // anything in the menu: see entry_at(). In the part of Continue's range
    // no item of Continue's -- it has none -- will ever use.
    static constexpr int kPopupFocusBase = 240;

    // Everything the menu shows, applied in one go, for the device that is not
    // holding it. Called every frame from what the host broadcast: one copy of
    // the state rather than a copy on each device kept in step by a scattering
    // of setters, which is the only way the two can be relied on to agree.
    void show(const LayoutPickerState& state, int focus, MenuConfirm confirm,
              int confirm_focus_wire = 0);
    // Which entry a tap landed on, or -1 for none. Resolved on the device the
    // finger touched, since only that one knows how its own menu is laid out.
    int entry_at(float x, float y, float output_width, float output_height) const override;
    // MenuPage: a list on the right scrolls under a finger, the mouse and the
    // wheel, and an option's popup does while it is up; a tap off that popup
    // puts it away.
    bool scroll(const std::vector<PointerEvent>& pointers, float output_width,
                float output_height) override;
    bool off_popup(float x, float y, float output_width, float output_height) const override;
    // The list on the right in rows, then a dropdown's, while one is up.
    std::vector<float> scroll_state(float output_width, float output_height) const override;
    void set_scroll_state(const std::vector<float>& state, float output_width, float output_height) override;
    // Whether a tap at a point lands off an option's popup altogether --
    // which puts it away. False when none is up.
    bool off_choice_popup(float x, float y, float output_width, float output_height) const {
        return confirm_ == MenuConfirm::Choice && !choice_.contains(x, y, output_width, output_height);
    }
    // The button a tap on the row of control prompts stands for, or Count for
    // a tap that missed them. Answered from where they were last drawn, since
    // the row is laid out from the glyph artwork's own proportions.
    ButtonGlyph guide_button_at(float x, float y) const;
    // Puts the cursor somewhere without acting on it, which is what arrives
    // from the other device's finger.
    void set_focus(int focus);
    // Kept in step while the menu is open: R3 can change filtering from under
    // us, and on a companion both of these are owned by the host and arrive
    // whenever it says so.
    void set_filtering(bool bilinear) { state_.bilinear_filtering = bilinear; }
    void set_audio_target(AudioTarget target) { state_.audio_target = target; }
    void set_cursor_mode(CursorMode mode) { state_.cursor_mode = mode; }
    void set_screens_swapped(bool swapped) { state_.screens_swapped = swapped; }
    // What it was opened with, as kept up since: for telling the other device.
    const LayoutPickerState& state() const { return state_; }
    // Cemu's options, and the amiibo in the folder, as they are now -- see
    // LayoutPickerState. Either can change while the menu is up: an option
    // just stepped, Cemu's list arriving, a folder chosen.
    void set_producer_options(std::vector<std::pair<std::string, std::string>> options,
                              std::vector<std::vector<std::string>> choices = {}, int controls_from = -1) {
        state_.producer_options = std::move(options);
        state_.producer_choices = std::move(choices);
        state_.producer_controls_from = controls_from;
        rebuild_footer();
    }
    void set_amiibo(std::vector<std::string> amiibo, std::string folder) {
        state_.amiibo = std::move(amiibo);
        state_.amiibo_folder = std::move(folder);
        rebuild_footer();
    }
    // Which of a Wii U's screens the device this menu is drawn on is showing,
    // drawn at the bottom left of its list -- a GamePad or a television.
    // Each device's own, so set by each, every frame, rather than shared.
    void set_device_screen(std::optional<ScreenId> screen) { device_screen_ = screen; }
    // The game's icon, drawn left of its title in the header; null for none.
    // The caller's, and kept alive by it while the menu is drawn.
    void set_game_icon(const Texture* icon) { game_icon_ = icon; }
    void set_left_handed_layouts(std::vector<std::string> layouts) {
        state_.left_handed_layouts = std::move(layouts);
    }
    void set_render_scale(int scale) { state_.render_scale = scale; }
    void set_screen_gap(bool include_gap) { state_.screen_gap = include_gap; }
    void set_right_stick_mode(RightStickMode mode) { state_.right_stick_mode = mode; }
    // Whether an Up or Down is a held direction repeating rather than a
    // press: the list of sections goes round from one end to the other on a
    // press, and stops at the end while held. Everything is a press without
    // one.
    void set_repeat_source(std::function<bool(UiAction)> repeating) { repeating_ = std::move(repeating); }
    void set_rumble_pak(bool inserted) { state_.rumble_pak = inserted; }
    void set_lcd_filter(bool on) { state_.lcd_filter = on; }
    void set_custom_textures(bool on) { state_.custom_textures = on; }
    void set_resume_from_save_state(bool on) { state_.resume_from_save_state = on; }
    // The console artwork, drawn inside the Standard layout's tile.
    void set_bezel_art(const Texture* art) { bezel_art_ = art; }
    void set_smoothing(bool smooth) { smoothing_ = smooth; }
    // The display the previews are judged against, when that is not the size
    // being drawn at -- see Compositor::set_display_size(). A tile has to show
    // the margin the real render would have, which is decided by the panel.
    // `fill` travels with the size because it answers the same question --
    // how much of the display the screens should take -- and a tile that
    // disagreed with the render behind it would be showing the wrong thing.
    void set_display_size(float width, float height, bool fill = false) {
        display_width_ = width;
        display_height_ = height;
        fill_display_ = fill;
    }
    // The console being played, which decides the screens' shapes and what the
    // Standard tile draws, which layouts fit at all, and which one leads the
    // menu -- see rebuild_tiles().
    void set_screens(const SystemScreens& screens, SystemId system = SystemId::Ds);
    // Which hand `layout_id` is drawn for.
    Handedness handedness_of(const std::string& layout_id) const;
    // The host can change the layout under a companion's open menu.
    void set_selected_layout(const std::string& layout_id);
    bool filtering() const { return state_.bilinear_filtering; }
    AudioTarget audio_target() const { return state_.audio_target; }
    bool is_open() const { return open_; }
    bool embedded() const { return open_ && state_.embedded; }
    void close() {
        open_ = false;
        lingering_ = false;
    }
    // Closed -- it answers nothing more -- but still drawn as it was, while
    // the quick menu goes away over the game. Until close() or open().
    void linger() { lingering_ = true; }
    bool is_drawn() const { return open_ || lingering_; }

    LayoutPickerResult update(const std::vector<UiAction>& actions,
                              const std::vector<PointerEvent>& pointers,
                              float output_width,
                              float output_height);

    // `screens` are the frames the game is paused on, drawn into the previews so
    // a tile shows the actual game in that arrangement. Null falls back to the
    // coloured blocks.
    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                const std::string& game_title,
                const ScreenTextures* textures = nullptr);

    const std::string& selected_layout_id() const { return selected_layout_id_; }

    // The layout a tile actually draws: `layout` with its own hand and the
    // current gap choice applied, the same as the game itself would
    // be drawn with. Exposed so a preview -- and a test -- do not have to
    // reimplement the combination; render() uses this for every tile, which is
    // what makes toggling Gap or Hands update the previews immediately.
    Layout resolved_layout_for_preview(const Layout& layout) const;
    // Width over height of the display the menu's previews stand for: the
    // panel set by set_display_size(), or else what the menu is drawn at.
    float preview_aspect(float output_width, float output_height) const;

    // Draws a layout at a glance: the game's own screens where there are frames
    // to draw, two coloured rectangles where there are not. `display_width`/
    // `display_height` are the real output size the menu itself is drawn at --
    // not `box`, which is only this one tile -- so a preview shows the margin
    // (or lack of it) the real layout would actually have.
    static void draw_preview(QuadRenderer& quads,
                             const Layout& layout,
                             const PixelRect& box,
                             float display_width,
                             float display_height,
                             const SystemScreens& geometry,
                             const ScreenTextures* textures,
                             const Texture* bezel_art,
                             bool smoothing);

private:
    // The rows of a quick menu's Options section.
    enum class Option {
        RenderScale,
        Sound,
        Cursor,
        RightStick,
        Rumble,
        ScreenGap,
        Smoothing,
        LcdFilter,
        CustomTextures,
        Resume,
        // A Wii U's: this game's screen, alone, or the two devices trading.
        SwapScreens,
        // A Wii U's graphics packs, a page of their own -- see
        // LayoutPickerState::graphic_packs_available.
        GraphicPacks,
        // One of Cemu's own -- see LayoutPickerState::producer_options.
        Producer,
        // A Wii U's Controller Order screen, a page of its own, at the top of
        // Controls.
        ControllerOrder,
        // A DS or 3DS game's Button Mapping, a page of its own, at the top
        // of its Controls.
        ButtonMapping,
    };
    struct OptionRow {
        Option option = Option::Cursor;
        // Which of Cemu's, for a Producer row.
        int index = -1;
    };

    // The two columns of a DS or 3DS menu. `items` are the right-hand side's
    // things for the section showing there, in its own order.
    struct MenuGeometry {
        PixelRect panel;
        float scale = 1.0f;
        std::vector<PixelRect> sections;
        float divider_x = 0.0f;
        float content_top = 0.0f;
        float content_bottom = 0.0f;
        PixelRect content;
        std::vector<PixelRect> items;
        int columns = 1;
        // Where the stylus button is drawn, usable or not. Among `items`
        // only while the layout in use has a hand to choose.
        PixelRect hand_button;
    };
    MenuGeometry compute_menu_geometry(float output_width, float output_height) const;
    // The popup a question is asked in: what it says, its buttons -- Cancel
    // and the thing asked about, or a notice's one OK -- and where they go.
    // Only meaningful while one is up.
    PopupText confirm_text() const;
    std::vector<PopupButton> confirm_buttons() const;
    PopupGeometry confirm_popup(float output_width, float output_height) const;
    // Puts a question up, the cursor on its answer.
    void ask(MenuConfirm confirm);
    bool confirming() const { return confirm_ != MenuConfirm::None; }
    int tile_count() const { return static_cast<int>(tiles_.size()); }
    ScreenId preview_screen_ = ScreenId::Top;
    // The layout behind a tile, and where a layout's tile is. Everything that
    // counts or numbers tiles goes through these rather than through the
    // registry, because not every layout is offered on every console.
    const Layout& tile_layout(int index) const;
    int tile_index_of(const std::string& layout_id) const;
    // How many of the right-hand side's rows fit, and keeping the cursor's
    // among them -- see content_scroll_.
    int content_rows_fitting(const MenuGeometry& geometry) const;
    // How far apart a list's rows are, and how far it can scroll, in rows.
    float content_row_step(const MenuGeometry& geometry) const;
    float content_max_scroll(const MenuGeometry& geometry) const;
    void keep_item_in_view(float output_width, float output_height);
    // A focus or entry -- see focus_of() -- as the section and the item in it
    // (-1 for the section itself). False for one that is not on the menu.
    bool decode_focus(int focus, MenuSection& section, int& item) const;
    // A tap on an entry, from this device or the other -- see take_taps().
    void handle_tap(int entry, LayoutPickerResult& result);
    // Whether the right-hand side is a list -- Game Options, amiibo -- that
    // can run past the bottom and scroll.
    bool content_is_list() const;

    bool has_section(MenuSection section) const;
    // How many things a section shows on the right; 0 for one that does
    // something instead.
    int content_count(MenuSection section) const;
    // The layout grid's width, from the number of layouts alone, so that both
    // devices agree on what Down means however different their screens are.
    int layout_columns() const;
    // How many across a section's things are laid out: the layout grid's
    // width, two for the save slots' square, one for a list.
    int content_columns(MenuSection section) const;
    // How many of a section's things are in its grid, which Left, Right, Up
    // and Down move around. The stylus button under the layouts is not.
    int grid_count(MenuSection section) const;
    // Whether the layout in use has a hand to choose -- see
    // layout_has_handedness(). Hidden, and passed over, where not.
    bool hand_button_usable() const;
    // Or, in the same place, Separate Screens' button that swaps the screens.
    bool swap_button_usable() const;
    // Whichever of the two the layout in use has, if either.
    bool corner_button_usable() const { return hand_button_usable() || swap_button_usable(); }
    bool slot_filled(int slot) const;
    void activate_section(LayoutPickerResult& result);
    void activate_item(LayoutPickerResult& result);
    void handle_menu_action(UiAction action, LayoutPickerResult& result);
    std::string option_name(const OptionRow& row) const;
    std::string option_value(const OptionRow& row) const;
    void render_sections(QuadRenderer& quads, TextRenderer& text, const MenuGeometry& geometry);
    void render_layouts(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                        const MenuGeometry& geometry, float output_width, float output_height,
                        const ScreenTextures* textures);
    void render_slots(QuadRenderer& quads, TextRenderer& text, const MenuGeometry& geometry);
    void render_options(QuadRenderer& quads, TextRenderer& text, const MenuGeometry& geometry);
    void render_amiibo(QuadRenderer& quads, TextRenderer& text, const MenuGeometry& geometry);

    // What the menu has depends on the session and the console -- a
    // companion, a render scale, a stick -- so it is built per-open rather
    // than fixed: the Wii U list, or the DS and 3DS sections and options.
    void rebuild_footer();
    // Which tiles this console offers, and in what order -- see set_screens().
    void rebuild_tiles();

    const LayoutRegistry* registry_;
    // Which of the registry's layouts this console can actually show, as
    // indices into it. Decided from the system alone, so a host and a companion
    // arrive at the same list and tile number three means the same layout on
    // both. See layout_fits_system().
    std::vector<std::size_t> tiles_;
    bool open_ = false;
    bool lingering_ = false;
    std::optional<ScreenId> device_screen_;
    const Texture* game_icon_ = nullptr;
    Texture ui_icons_;
    bool ui_icons_tried_ = false;
    // The cursor: which section, and whether it is over on that section's
    // things, and on which.
    std::vector<MenuSection> sections_;
    std::vector<OptionRow> options_;
    // Controls: a Wii U's Controller Order, then each player's controller;
    // a DS or 3DS game's Button Mapping and Right Stick.
    std::vector<OptionRow> controls_;
    // The rows of a list section: Controls', or Options'; and of whichever
    // is up.
    const std::vector<OptionRow>& rows_of(MenuSection section) const {
        return section == MenuSection::ControllerOrder ? controls_ : options_;
    }
    const std::vector<OptionRow>& rows() const { return rows_of(section_); }
    // How far a list on the right is scrolled, in rows, and the finger or
    // wheel moving it -- the cursor brings it with it only when it moves.
    float content_scroll_ = 0.0f;
    ListDrag content_drag_;
    ScrollEase content_ease_;
    MenuSection section_ = MenuSection::Layout;
    std::function<bool(UiAction)> repeating_;
    // Whether this menu has been opened before in this run, so the next open
    // goes back to the section it was closed on -- see open().
    bool opened_before_ = false;
    bool on_content_ = false;
    int item_ = 0;
    std::vector<SavePointInfo> save_slots_;
    std::array<Texture, kSavePointSlots> slot_pictures_;
    std::array<bool, kSavePointSlots> slot_pictures_dirty_{};
    std::string save_status_;
    bool save_status_failed_ = false;
    std::string selected_layout_id_;
    LayoutPickerState state_;
    const Texture* bezel_art_ = nullptr;
    bool smoothing_ = true;
    MenuConfirm confirm_ = MenuConfirm::None;
    PopupCursor confirm_cursor_;
    // Where the menu was closed, when the menu opened since has not had that
    // section -- see open().
    struct HeldPlace {
        MenuSection section = MenuSection::Continue;
        bool inside = false;
        int item = 0;
    };
    std::optional<HeldPlace> held_place_;
    // An option's choices, while MenuConfirm::Choice is up, beside its row.
    ChoicePopup choice_;
    // The window the menu was last laid out for, which the popup is placed
    // in when it is opened from a press.
    float last_width_ = 0.0f;
    float last_height_ = 0.0f;
    // The options row's choices, in the order it steps, and which is in
    // effect; empty for a switch or a way into a page.
    std::vector<std::string> option_choices(const OptionRow& row, int& current) const;
    // Drawn as a switch: On or Off, and nothing else to choose.
    bool is_switch(const OptionRow& row) const;
    // The popup up for the option the cursor is on.
    void open_choice();
    // Where the control prompts landed when they were last drawn, so a tap can
    // find them. Laid out from the glyph artwork, which only drawing knows.
    std::vector<GuideHit> guide_hits_;
    float display_width_ = 0.0f;
    float display_height_ = 0.0f;
    bool fill_display_ = false;
    SystemScreens screens_;
    SystemId system_ = SystemId::Ds;
};

}  // namespace ds
