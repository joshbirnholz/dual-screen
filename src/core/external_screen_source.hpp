#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/external_frame_link.hpp"
#include "core/screen_source.hpp"
#include "core/system.hpp"
#include "input/ds_input.hpp"

namespace ds {

// A console played by another program, seen through the seam everything else in
// this app already looks through. See ExternalFrameLink for the handover
// itself, and SystemProfile::frame_origin for which consoles arrive this way.
//
// The differences from a core are all absences. There is no ROM to load, no
// save RAM to flush, no options to force and no render scale to set: whatever
// is playing the game owns all of that and is configured in its own right. What
// is left is two screens, some audio, and a way to ask for a reset -- which is
// the whole of what this seam was ever for.
class ExternalScreenSource final : public ScreenSource {
public:
    // Tagged by screen: a Wii U's TV and GamePad audio are independent
    // streams, each routed by whatever the player set that screen's audio
    // target to.
    using AudioCallback =
        std::function<void(ScreenId screen, const int16_t* interleaved_stereo, std::size_t frames)>;

    explicit ExternalScreenSource(SystemId system);
    ~ExternalScreenSource() override;

    // Starts listening. The game is not here yet and may never arrive; that is
    // not a failure, and running() says which it turned out to be.
    bool open(const std::string& socket_path, std::string* error);
    void close();
    // True once a game has connected and said what it is sending.
    bool running() const { return link_.connected(); }
    // Why the session ended, once it has. Empty while it has not.
    const std::string& ended_reason() const { return ended_reason_; }

    bool advance_frame(std::string* error) override;
    // Waits up to `timeout_ms` for the game to say something -- a new frame,
    // most often -- so the loop taking its frames runs at the game's pace.
    bool wait_for_frame(int timeout_ms) const { return link_.wait_for_message(timeout_ms); }
    // How many frames the game announced for a screen since the last ask, and
    // how many were overwritten before this app took them.
    ExternalFrameLink::FrameCounts take_frame_counts(ScreenId screen) {
        return link_.take_frame_counts(screen);
    }
    const ScreenTextures& screens() override;
    SystemScreens screens_geometry() const override;
    bool read_screen(ScreenId screen, ScreenReadback& out, int target_width = 0,
                     int target_height = 0) override;
    // The two screens arrive under one serial each frame but are published
    // separately, so either may be the one that has just landed.
    uint64_t frame_serial() const override { return std::max(top_.serial, bottom_.serial); }
    // One screen's own, for telling whether a new picture of it has arrived.
    uint64_t screen_serial(ScreenId screen) const {
        return screen == ScreenId::Top ? top_.serial : bottom_.serial;
    }
    bool wants_shutdown() const override { return ended_; }

    void set_audio_callback(AudioCallback callback) { audio_callback_ = std::move(callback); }
    double audio_sample_rate() const;
    double frames_per_second() const;

    // Called whenever the GamePad's own motor turns on or off, so a shell can
    // forward that to whatever is standing in for the GamePad -- see
    // ExternalFrameLink::take_rumble(). Set before the game connects, same as
    // the audio callback above.
    using RumbleCallback = std::function<void(bool active)>;
    void set_rumble_callback(RumbleCallback callback) { rumble_callback_ = std::move(callback); }
    // The same for each of the players besides the GamePad: which one (0 for
    // Player 1) and whether its motor is on. Every change is passed on, in
    // order, so two players' motors never cover for each other.
    using PlayerRumbleCallback = std::function<void(int player, bool active)>;
    void set_player_rumble_callback(PlayerRumbleCallback callback) {
        player_rumble_callback_ = std::move(callback);
    }

    // Only the program playing the game knows what restarting one means.
    void reset() { link_.request_reset(); }

    // This app's menu drawn over the game's own window, and the key that asks
    // for it there -- see ExternalFrameLink::publish_overlay().
    bool overlay_supported() const { return link_.overlay_supported(); }
    bool publish_overlay(const uint8_t* rgba, int width, int height, bool takes_input = true) {
        return link_.publish_overlay(rgba, width, height, takes_input);
    }
    // Whether the program animates the menu itself, and handing it the two
    // pictures it does that with -- see ExternalFrameLink.
    bool overlay_animation_supported() const { return link_.overlay_animation_supported(); }
    bool publish_overlay_animation(ExternalOverlayAnimation::Mode mode, const uint8_t* backdrop,
                                   const uint8_t* contents, int width, int height, bool takes_input) {
        return link_.publish_overlay_animation(mode, backdrop, contents, width, height, takes_input);
    }
    void hide_overlay() { link_.hide_overlay(); }
    bool take_menu_key() { return link_.take_menu_key(); }
    // The game's graphics packs, which Cemu keeps -- see
    // ExternalFrameLink::graphic_packs_supported().
    bool graphic_packs_supported() const { return link_.graphic_packs_supported(); }
    void request_graphic_packs() { link_.request_graphic_packs(); }
    bool take_graphic_packs(std::vector<ExternalGraphicPack>& out) {
        return link_.take_graphic_packs(out);
    }
    void change_graphic_pack(const ExternalGraphicPackChange& change) {
        link_.change_graphic_pack(change);
    }
    // Cemu's own options, and an amiibo scanned -- see
    // ExternalFrameLink::options_supported() and amiibo_supported().
    bool options_supported() const { return link_.options_supported(); }
    void request_options() { link_.request_options(); }
    bool take_options(std::vector<ExternalOption>& out) { return link_.take_options(out); }
    void change_option(const std::string& id, int choice) { link_.change_option(id, choice); }
    bool amiibo_supported() const { return link_.amiibo_supported(); }
    void scan_amiibo(const std::string& path) { link_.scan_amiibo(path); }
    bool take_amiibo_result(ExternalAmiiboResult& out) { return link_.take_amiibo_result(out); }
    bool amiibo_reading_supported() const { return link_.amiibo_reading_supported(); }
    std::optional<ExternalAmiiboReading> amiibo_reading() const { return link_.amiibo_reading(); }
    // The game's own dialogs, shown here -- see
    // ExternalFrameLink::game_dialogs_supported().
    bool game_dialogs_supported() const { return link_.game_dialogs_supported(); }
    void want_game_dialogs(bool wanted) { link_.want_game_dialogs(wanted); }
    bool take_game_dialog(ExternalGameDialog& out) { return link_.take_game_dialog(out); }
    void answer_game_dialog(const ExternalGameDialogAnswer& answer) { link_.answer_game_dialog(answer); }
    bool pause_supported() const { return link_.pause_supported(); }
    void request_pause(bool pause) { link_.request_pause(pause); }
    bool take_pause_state(ExternalPauseState& out) { return link_.take_pause_state(out); }
    bool take_menu_keyboard(ExternalMenuKey& key, bool& down) {
        return link_.take_menu_keyboard(key, down);
    }
    void set_pointer_shown(bool shown) { link_.set_pointer_shown(shown); }
    bool take_overlay_tap(float& press_x, float& press_y, float& x, float& y) {
        return link_.take_overlay_tap(press_x, press_y, x, y);
    }
    bool overlay_pointer_supported() const { return link_.overlay_pointer_supported(); }
    bool take_overlay_pointer(ExternalOverlayPointer& out) { return link_.take_overlay_pointer(out); }

    // Tells the far end which single screen, if any, this app actually wants
    // captured and sent right now, the largest it is any use at, and which
    // screen the far end should show in its own window -- see
    // ExternalFrameLink::request_screen(). Leaving all of it alone (the
    // defaults) costs the far end nothing at all: nothing is captured and it
    // shows whatever it was already showing.
    void request_screen(std::optional<ScreenId> screen, int max_width = 0, int max_height = 0,
                        std::optional<ScreenId> shown = std::nullopt) {
        link_.request_screen(screen, max_width, max_height, shown);
    }

    // The console's own pad -- the GamePad, on a Wii U -- on its way to
    // whatever is playing the game. Translated on the way out: what crosses the link is a
    // GamePad's controls rather than this app's own shape. See ExternalInput.
    void submit_input(const DsInputState& state);

    // One of the players besides the GamePad (0 for Player 1): the pads behind
    // it, merged, and whether any are -- see route_pads(). Sent every frame
    // like submit_input(); the link only puts changes on the wire.
    void submit_player_input(int player, bool fed, const DsInputState& state);
    // The same, its buttons `buttons` -- ExternalButton bits, already the
    // controller's own, as a Wii U game's mapping makes them -- rather than
    // the state's.
    void submit_input(const DsInputState& state, uint32_t buttons);
    void submit_player_input(int player, bool fed, const DsInputState& state, uint32_t buttons);

private:
    struct Screen {
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;
        uint64_t uploaded_serial = 0;
        Texture texture;
    };

    Screen& screen_for(ScreenId screen) { return screen == ScreenId::Top ? top_ : bottom_; }
    const Screen& screen_for(ScreenId screen) const {
        return screen == ScreenId::Top ? top_ : bottom_;
    }

    SystemId system_;
    ExternalFrameLink link_;
    ScreenTextures textures_;
    Screen top_;
    Screen bottom_;
    AudioCallback audio_callback_;
    std::vector<int16_t> audio_[2];
    RumbleCallback rumble_callback_;
    PlayerRumbleCallback player_rumble_callback_;
    bool ended_ = false;
    std::string ended_reason_;
};

}  // namespace ds
