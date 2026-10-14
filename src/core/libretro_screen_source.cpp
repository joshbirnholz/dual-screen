#include "core/libretro_screen_source.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config/melonds_tuning.hpp"
#include "config/paths.hpp"
#include "core/gl_call_trace.hpp"
#include "render/gl_loader.hpp"
#include "util/log.hpp"

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

namespace ds {
namespace {

#ifdef __SWITCH__
// gpu_alone=1 in /switch/dualscreen/melonds.ini (Settings > melonDS > App
// drawing finished first): the GPU finishes drawing this app's own screens
// before each frame of the game is run, so the emulator's GPU work never
// shares the GPU with the app's. For finding out whether the two running side
// by side is what spoils a DS game's graphics on the Switch, where melonDS's
// own app -- which draws almost nothing of its own -- shows the same game
// cleanly. Costs the time the app's drawing takes, every frame.
bool gpu_alone_asked() {
    const bool asked = read_melonds_tuning().gpu_alone;
    if (asked) log_info("the GPU finishes this app's drawing before each frame of the game");
    return asked;
}
#endif

// melonDS DS core option keys and values, as declared by the core itself. We do
// not trust these to exist: LibretroCore validates every forced option against
// the definitions the loaded core actually reports, and anything it cannot
// honour surfaces through warnings() instead of silently changing the picture.
constexpr const char* kOptNumberOfLayouts = "melonds_number_of_screen_layouts";
constexpr const char* kOptScreenLayout1 = "melonds_screen_layout1";
constexpr const char* kOptScreenGap = "melonds_screen_gap";
constexpr const char* kOptRenderMode = "melonds_render_mode";
constexpr const char* kOptTouchMode = "melonds_touch_mode";
constexpr const char* kOptShowCursor = "melonds_show_cursor";
// libretro's own melonDS, the core melonDS DS grew out of, and the one a
// Switch build links in (melonDS DS is not built for the Switch). Most of its
// options have different names: one layout rather than a numbered list, the
// renderer as a switch rather than a mode, and no cursor option at all -- it
// draws none in Touch mode. Neither core has the other's, so each of these is
// set only where it exists; a layout neither core took shows up anyway, loudly,
// as a framebuffer compute_top_bottom_split() cannot divide.
constexpr const char* kOptLegacyScreenLayout = "melonds_screen_layout";
constexpr const char* kOptLegacyOpenGl = "melonds_opengl_renderer";
// The software renderer drawing on a thread of its own. Both cores have it,
// and on a Switch, whose cores are a third the speed of a Deck's, it is the
// difference between a game that keeps up and one that does not.
constexpr const char* kOptThreadedRenderer = "melonds_threaded_renderer";
// The DS's firmware settings melonDS DS overrides: its owner's name -- asked
// of the frontend with "guess_username" -- and favorite color, 0 to 15.
constexpr const char* kOptFirmwareUsername = "melonds_firmware_username";
constexpr const char* kOptFirmwareFavoriteColor = "melonds_firmware_favorite_color";
constexpr const char* kOptFirmwareBirthMonth = "melonds_firmware_birth_month";
constexpr const char* kOptFirmwareBirthDay = "melonds_firmware_birth_day";
// The microphone. melonDS can synthesise the blow a DS game is listening for,
// which is the only way in here: the libretro microphone interface this app
// does not implement is exactly what the core reports missing at start-up
// ("Microphone interface not available; substituting silence instead"), and
// even with it the sound would have to be captured on whichever device the
// player is holding and sent across. A generated blow needs none of that, and
// games detect it more reliably than they detect a person.
//
// Spellings confirmed against melonDS DS 1.3.1, which declares:
//   melonds_mic_input        silence, blow, noise, microphone
//   melonds_mic_input_active hold, toggle, always
//   melonds_show_mic_state   enabled, disabled
// Candidates rather than single values because a core version that spells one
// differently should pick its own spelling rather than be told; force_option()
// takes the first the core declares and warns with the real list if none fit.
constexpr const char* kOptMicInput = "melonds_mic_input";
constexpr const char* kOptMicActive = "melonds_mic_input_active";
constexpr const char* kOptShowMicState = "melonds_show_mic_state";

// The 3DS equivalent, which two cores answer to: citra_libretro and Azahar,
// the fork that succeeded it. They share the option keys -- Azahar kept
// Citra's "citra_" prefix -- but not always the values, so each setting below
// lists every spelling it goes by and LibretroCore picks whichever the loaded
// core declares. Getting this wrong is not fatal, but it is loud: the core
// falls back to its own default layout and the split lands on nothing.
//
// The stacked layout: the 400x240 upper screen above the 320x240 touch screen,
// the narrower one centred beneath the wider, in a buffer 400 wide and 480
// tall at 1x. Citra writes it out in words; Azahar uses an identifier.
constexpr const char* kOpt3dsLayout = "citra_layout_option";
const std::vector<std::string> kOpt3dsLayoutValues = {"Default Top-Bottom Screen", "default"};
// "Top" means the top screen is the prominent one, which is these cores' way
// of saying "not swapped" -- neither has a disabled/enabled here.
constexpr const char* kOpt3dsSwapScreens = "citra_swap_screen";
constexpr const char* kOpt3dsSwapScreensValue = "Top";
// citra_libretro ignores the libretro pointer entirely unless this is on: with
// it off, touches reach the core and are dropped, and the touch screen does
// nothing. Azahar has no such option and always accepts the pointer, which is
// why this is only applied where it exists.
constexpr const char* kOpt3dsTouch = "citra_touch_touchscreen";
// Its own on-screen pointer, which would sit on top of the one we draw.
// Azahar does not draw one and has no option for it either.
constexpr const char* kOpt3dsRenderTouch = "citra_render_touchscreen";
// How many pixels the core renders per 3DS pixel. Citra spells these "2x",
// "3x" and so on with the first written out in full; Azahar uses the bare
// number.
constexpr const char* kOpt3dsResolution = "citra_resolution_factor";
// What the right stick does. Both cores default it to driving the C-Stick
// *and* a touchscreen pointer, which on Azahar draws a small cross on the
// touch screen and moves it with the stick. There is no way to press it from
// here and nothing that needs it -- this app has its own touch control, and
// the right stick is the C-Stick -- so it is a distraction drawn over the
// game. Pinned to the C-Stick alone, which leaves the stick doing exactly
// what it did and takes the cross away.
constexpr const char* kOpt3dsAnalogFunction = "citra_analog_function";
const std::vector<std::string> kOpt3dsAnalogFunctionValues = {"c_stick", "C-Stick"};

std::vector<std::string> citra_resolution_values(int scale) {
    return {scale <= 1 ? std::string("1x (Native)") : std::to_string(scale) + "x",
            std::to_string(scale)};
}

std::string dimensions(int width, int height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

}  // namespace

ScreenSplit compute_top_bottom_split(SystemId system,
                                     int frame_width,
                                     int frame_height,
                                     std::string* reason) {
    ScreenSplit split;
    if (frame_width <= 0 || frame_height <= 0) {
        if (reason != nullptr) *reason = "the core produced an empty frame";
        return split;
    }

    // What the pinned layout should have produced, at 1x. The core may be
    // rendering at a higher internal resolution, so the check is on the shape
    // rather than the size, and everything below scales with it.
    const SystemScreens& screens = system_profile(system).screens;
    const int unit_width = std::max(screens.top.width, screens.bottom.width);
    const int unit_height = screens.top.height + screens.bottom.height;

    // frame / unit must be the same ratio in both directions, and a whole
    // number of source pixels must land on each screen.
    if (frame_width * unit_height != frame_height * unit_width) {
        if (reason != nullptr) {
            *reason = "expected a framebuffer shaped like two " + system_profile(system).name +
                      " screens stacked (" + dimensions(unit_width, unit_height) +
                      ", or a whole multiple of it) but the core produced " +
                      dimensions(frame_width, frame_height);
        }
        return split;
    }
    if (frame_width % unit_width != 0) {
        if (reason != nullptr) {
            *reason = "the core produced " + dimensions(frame_width, frame_height) +
                      ", which is not a whole multiple of " + dimensions(unit_width, unit_height);
        }
        return split;
    }

    const int scale = frame_width / unit_width;
    split.frame_width = frame_width;
    split.frame_height = frame_height;
    split.top_width = screens.top.width * scale;
    split.top_height = screens.top.height * scale;
    split.bottom_width = screens.bottom.width * scale;
    split.bottom_height = screens.bottom.height * scale;
    // Each screen centred across the buffer's width, which is what the pinned
    // layouts do with the narrower of the two.
    split.top_x = (frame_width - split.top_width) / 2;
    split.top_y = 0;
    split.bottom_x = (frame_width - split.bottom_width) / 2;
    split.bottom_y = split.top_height;
    split.valid = true;
    return split;
}

LibretroCoreScreenSource::LibretroCoreScreenSource() : core_(new LibretroCore()) {}

LibretroCoreScreenSource::~LibretroCoreScreenSource() { close(); }

void LibretroCoreScreenSource::set_audio_callback(AudioCallback callback) {
    core_->set_audio_callback(std::move(callback));
}

bool LibretroCoreScreenSource::open(const LibretroSourceConfig& config, std::string* error) {
#ifdef __SWITCH__
    // Each game, so a change in Settings is taken up by the next one.
    gpu_alone_ = gpu_alone_asked();
#endif
    warnings_.clear();

    system_ = config.system;
    const SystemProfile& profile = system_profile(system_);
    render_scale_ = std::clamp(config.render_scale, 1, profile.max_render_scale);
    rumble_pak_ = config.rumble_pak;

    if (config.core_path.empty()) {
        if (error != nullptr) {
            *error = builtin_core_system()
                         ? "This device cannot play " + profile.name + " games."
                         : "No " + profile.core_description + " libretro core was found. Install " +
                               profile.core_filenames.front() +
                               ", or set \"core_path\" in settings.json.";
        }
        return false;
    }
    if (config.core_path != kBuiltInCorePath && !file_exists(config.core_path)) {
        if (error != nullptr) {
            *error = "The core file does not exist: " + config.core_path;
        }
        return false;
    }
    if (!file_exists(config.rom_path)) {
        if (error != nullptr) {
            *error = "The ROM file does not exist: " + config.rom_path;
        }
        return false;
    }

    core_->set_system_directory(config.system_directory);
    core_->set_username(system_ == SystemId::Ds ? config.nickname : std::string());
    core_->set_save_directory(config.save_directory);
    core_->set_input_state(input_sink_.state_pointer());

    // Pin the core to exactly one internal layout, chosen so the two screens
    // land as clean, axis-aligned pieces of a single buffer. Everything the
    // player sees is composited by us from those pieces, so this value is an
    // implementation detail they never interact with.
    //
    // Options a core does not have are ignored, and a core that refuses one
    // says so through warnings() rather than quietly changing the picture --
    // which is why both systems' options can simply be set here.
    switch (system_) {
        // Nothing to force on a console this class never runs: its frames come
        // from a program of its own. Reaching here at all would be a mistake
        // made much earlier, and there is no option that would fix it.
        case SystemId::WiiU:
            break;
        case SystemId::Ds:
        {
            using Requirement = LibretroCore::OptionRequirement;
            // melonDS DS's spellings:
            core_->force_option(kOptNumberOfLayouts, {"1"}, Requirement::IfPresent);  // no cycling
            core_->force_option(kOptScreenLayout1, {"top-bottom"}, Requirement::IfPresent);
            // composited from a CPU framebuffer
            core_->force_option(kOptRenderMode, {"software"}, Requirement::IfPresent);
            core_->force_option(kOptShowCursor, {"disabled"}, Requirement::IfPresent);  // ours
            // and the older core's:
            core_->force_option(kOptLegacyScreenLayout, {"Top/Bottom"}, Requirement::IfPresent);
            core_->force_option(kOptLegacyOpenGl, {"disabled"}, Requirement::IfPresent);
            // Both have these.
            core_->force_option(kOptScreenGap, "0");  // no dead rows between screens
            // touch arrives as a libretro pointer
            core_->force_option(kOptTouchMode, std::vector<std::string>{"touch", "Touch"});
            core_->force_option(kOptThreadedRenderer, {"enabled"}, Requirement::IfPresent);
            // The owner's name, which melonDS DS asks the frontend for when
            // told to guess it -- see LibretroCore::set_username() -- their
            // favorite color and their birthday.
            if (!config.nickname.empty()) {
                core_->force_option(kOptFirmwareUsername, {"guess_username"}, Requirement::IfPresent);
            }
            if (config.favorite_color >= 0 && config.favorite_color <= 15) {
                core_->force_option(kOptFirmwareFavoriteColor, {std::to_string(config.favorite_color)},
                                    Requirement::IfPresent);
            }
            if (config.birth_month >= 1 && config.birth_month <= 12 && config.birth_day >= 1 &&
                config.birth_day <= 31) {
                core_->force_option(kOptFirmwareBirthMonth, {std::to_string(config.birth_month)},
                                    Requirement::IfPresent);
                core_->force_option(kOptFirmwareBirthDay, {std::to_string(config.birth_day)},
                                    Requirement::IfPresent);
            }
        }
            // Blow rather than silence, and only while the button is held --
            // "always" would have the console blown into for the whole
            // session, which several games read as an input of its own.
            core_->force_option(kOptMicInput,
                                std::vector<std::string>{"blow", "Blow Noise", "blow noise"});
            // The older core has no such option: its blow lasts exactly as long as
            // the button is held, which is what this asks for anyway.
            core_->force_option(kOptMicActive, std::vector<std::string>{"hold", "Hold"},
                                LibretroCore::OptionRequirement::IfPresent);
            // And show it, because a microphone is the one input with no
            // visible effect when it is working and none when it is not: a
            // game that ignores a blow looks exactly like a button that is not
            // reaching the core.
            core_->force_option(kOptShowMicState, std::vector<std::string>{"enabled"},
                                LibretroCore::OptionRequirement::IfPresent);
            break;
        case SystemId::ThreeDs:
            core_->force_option(kOpt3dsLayout, kOpt3dsLayoutValues);
            core_->force_option(kOpt3dsSwapScreens, kOpt3dsSwapScreensValue);
            // Both only exist on citra_libretro; see above.
            core_->force_option(kOpt3dsTouch, {"enabled"},
                                LibretroCore::OptionRequirement::IfPresent);
            core_->force_option(kOpt3dsRenderTouch, {"disabled"},
                                LibretroCore::OptionRequirement::IfPresent);
            core_->force_option(kOpt3dsAnalogFunction, kOpt3dsAnalogFunctionValues);
            break;
    }
    // After the layout, because it multiplies whatever that layout produces.
    apply_render_scale();

    if (!core_->load(config.core_path, error)) {
        return false;
    }
    core_description_ = core_->core_name() + " " + core_->core_version();
    // What the core declares is known now, and nothing has been read from it
    // yet: the game loads below.
    find_core_options();
    apply_render_scale();
    if (offers_rumble_pak()) {
        set_rumble_pak(rumble_pak_);
    }

    for (const std::string& warning : core_->option_warnings()) {
        warnings_.push_back(warning);
    }

    if (!core_->load_game(config.rom_path, error)) {
        core_->unload();
        return false;
    }

    // The core's declared geometry should already describe the pinned layout;
    // the authoritative check happens on the first real frame.
    std::string reason;
    split_ = compute_top_bottom_split(system_, static_cast<int>(core_->av_info().base_width),
                                      static_cast<int>(core_->av_info().base_height), &reason);
    if (split_.valid) {
        apply_pointer_rect();
        log_split();
    }

    save_path_ = config.save_path;
    load_save_ram();

    if (profile.max_render_scale > 1) {
        log_info("render scale " + std::to_string(render_scale_) + "x");
    }
    log_info("core geometry " + std::to_string(core_->av_info().base_width) + "x" +
             std::to_string(core_->av_info().base_height) + " at " +
             std::to_string(core_->av_info().fps) + " fps, " +
             std::to_string(core_->av_info().sample_rate) + " Hz audio");
    return true;
}

void LibretroCoreScreenSource::close() {
    // Before the core goes away, not after: unloading frees the buffer.
    flush_save_ram(true);
    if (screen_framebuffer_ != 0 && gl::DeleteFramebuffers != nullptr) {
        gl::DeleteFramebuffers(1, &screen_framebuffer_);
        screen_framebuffer_ = 0;
    }
    if (readback_framebuffer_ != 0 && gl::DeleteFramebuffers != nullptr) {
        gl::DeleteFramebuffers(1, &readback_framebuffer_);
        readback_framebuffer_ = 0;
    }
    for (Readback& readback : readbacks_) {
        readback.texture.destroy();
        readback.pixels.clear();
        readback.filled = false;
    }
    if (core_) {
        core_->unload();
    }
    save_shadow_.clear();
    save_path_.clear();
    top_texture_.destroy();
    bottom_texture_.destroy();
    screens_ = ScreenTextures();
    uploaded_serial_ = 0;
    split_ = ScreenSplit();
    split_reported_ = false;
    blit_unavailable_reported_ = false;
}

bool LibretroCoreScreenSource::advance_frame(std::string* error) {
    if (!core_ || !core_->game_loaded()) {
        if (error != nullptr) *error = "no game is loaded";
        return false;
    }
#ifdef __SWITCH__
    if (gpu_alone_ && gl::Finish != nullptr) gl::Finish();
#endif
    core_->run_frame();
    if (core_->video_frame().valid() && core_->video_frame().serial != last_presented_serial_) {
        last_presented_serial_ = core_->video_frame().serial;
        quiet_frames_ = 0;
    } else {
        ++quiet_frames_;
    }
    // Checked here so the frontend's own check, at the top of the next frame's
    // drawing, is not blamed for something the core did. An emulator leaving an
    // error behind is its own business; the same error attributed to us would
    // send the next person hunting in the wrong file.
    gl::log_error_once("the core left an OpenGL error behind");
    probe_window_framebuffer();

    // What a core puts up on screen for a moment -- melonDS DS says "Layout
    // 1/1" as it starts -- is written to the log as it arrives (see
    // LibretroCore) and is not a warning. Kept out of warnings_, which is
    // for settings the core refused: counted as one, a notice from a frame
    // run while the game was still starting -- which resuming from a save
    // state does -- put up "did not accept every setting" over a game that
    // had started perfectly well.
    core_->clear_core_messages();

    refresh_split_from_frame();
    if (!split_.valid && core_->video_frame().valid()) {
        if (error != nullptr) {
            std::string reason;
            compute_top_bottom_split(system_, static_cast<int>(core_->video_frame().width),
                                     static_cast<int>(core_->video_frame().height), &reason);
            *error = "The core is not rendering in the screen layout this app pins it to: " + reason +
                     ". Check that the loaded core accepts the stacked screen layout this app "
                     "pins it to.";
        }
        return false;
    }
    return true;
}

void LibretroCoreScreenSource::load_save_ram() {
    save_shadow_.clear();
    if (save_path_.empty()) {
        return;
    }

    // Only valid after the cart has loaded, which is why this runs here and not
    // before load_game.
    void* data = core_->save_ram_data();
    const std::size_t size = core_->save_ram_size();
    if (data == nullptr || size == 0) {
        log_info("this game has no cartridge save");
        return;
    }

    std::vector<uint8_t> stored;
    std::string error;
    if (read_file(save_path_, stored, &error)) {
        const std::size_t copied = std::min(stored.size(), size);
        std::memcpy(data, stored.data(), copied);
        if (stored.size() != size) {
            // Keep going: a short file is a save from a smaller cart image, and
            // refusing to load it would lose the player's progress outright.
            log_warn("save file is " + std::to_string(stored.size()) + " bytes but this game wants " +
                     std::to_string(size) + "; loaded what fits");
        }
        log_info("loaded " + std::to_string(copied) + " bytes of save data from " + save_path_);
    } else {
        log_info("no existing save at " + save_path_ + "; starting fresh");
    }

    // The shadow is what is on disk now, so the first flush only writes if the
    // game actually changed something.
    save_shadow_.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + size);
}

bool LibretroCoreScreenSource::flush_save_ram(bool force) {
    if (save_path_.empty() || save_shadow_.empty() || !core_ || !core_->game_loaded()) {
        return false;
    }
    const void* data = core_->save_ram_data();
    const std::size_t size = core_->save_ram_size();
    if (data == nullptr || size == 0) {
        return false;
    }
    // The buffer can be reallocated by the core (a DSi swap, say); treat a size
    // change as "everything changed" rather than comparing across it.
    const bool resized = (size != save_shadow_.size());
    if (!force && !resized && std::memcmp(data, save_shadow_.data(), size) == 0) {
        return false;
    }

    std::string error;
    if (!write_file_atomically(save_path_, data, size, &error)) {
        log_error("could not write the save file: " + error);
        return false;
    }
    save_shadow_.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + size);
    log_debug("wrote " + std::to_string(size) + " bytes of save data to " + save_path_);
    return true;
}

void LibretroCoreScreenSource::refresh_split_from_frame() {
    const CoreVideoFrame& frame = core_->video_frame();
    if (!frame.valid()) {
        return;
    }
    if (split_.valid && split_.frame_width == static_cast<int>(frame.width) &&
        split_.frame_height == static_cast<int>(frame.height)) {
        return;  // unchanged
    }

    std::string reason;
    const ScreenSplit split = compute_top_bottom_split(system_, static_cast<int>(frame.width),
                                                       static_cast<int>(frame.height), &reason);
    split_ = split;
    if (!split.valid) {
        if (!split_reported_) {
            log_error("cannot split the core framebuffer: " + reason);
            split_reported_ = true;
        }
        return;
    }

    split_reported_ = false;
    apply_pointer_rect();
    log_split();
}

void LibretroCoreScreenSource::log_split() {
    log_info("splitting " + dimensions(split_.frame_width, split_.frame_height) + " into a " +
             dimensions(split_.top_width, split_.top_height) + " upper screen and a " +
             dimensions(split_.bottom_width, split_.bottom_height) + " touch screen");
}

void LibretroCoreScreenSource::set_render_scale(int scale) {
    const int wanted = std::clamp(scale, 1, system_profile(system_).max_render_scale);
    if (wanted == render_scale_) {
        return;
    }
    render_scale_ = wanted;
    apply_render_scale();
    // Nothing else to do here. The core picks the option up on its next frame,
    // announces the framebuffer it now wants, and refresh_split_from_frame()
    // re-derives the split from the frame that comes back -- which is the same
    // path a core that resizes itself for any other reason takes.
    log_info("render scale " + std::to_string(render_scale_) + "x");
}

bool LibretroCoreScreenSource::supports_save_states() const {
    return core_ && core_->has_save_states();
}

bool LibretroCoreScreenSource::save_state(std::vector<uint8_t>& out, std::string* error) {
    if (!supports_save_states()) {
        if (error != nullptr) *error = "This emulator cannot make save states.";
        return false;
    }
    if (!core_->save_state(out)) {
        if (error != nullptr) *error = "The emulator could not save the game's state.";
        return false;
    }
    return true;
}

bool LibretroCoreScreenSource::load_state(const std::vector<uint8_t>& state, std::string* error) {
    if (!supports_save_states()) {
        if (error != nullptr) *error = "This emulator cannot load save states.";
        return false;
    }
    if (!core_->load_state(state)) {
        if (error != nullptr) {
            *error = "The emulator could not load it. It may be from a different version of the "
                     "emulator.";
        }
        return false;
    }
    log_info("save state loaded (" + std::to_string(state.size()) + " bytes)");
    return true;
}

void LibretroCoreScreenSource::reset() {
    if (!core_) {
        return;
    }
    core_->reset();
    log_info("game reset");
}

void LibretroCoreScreenSource::apply_render_scale() {
    if (!core_) {
        return;
    }
    switch (system_) {
        // See above: not this class's console.
        case SystemId::WiiU:
            break;
        case SystemId::Ds:
            // melonDS is pinned to its software renderer, which draws one pixel
            // per DS pixel and has nothing to multiply.
            break;
        case SystemId::ThreeDs:
            core_->force_option(kOpt3dsResolution, citra_resolution_values(render_scale_));
            break;
    }
}

void LibretroCoreScreenSource::find_core_options() {
    rumble_key_.clear();
    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    // Every one, once, so that a log says exactly what this core can be told
    // -- which keys, which values -- rather than leaving it to be guessed.
    for (const auto& [key, info] : core_->declared_options()) {
        std::string values;
        for (const std::string& value : info.allowed_values) {
            if (!values.empty()) values += ", ";
            values += value;
        }
        log_debug("core option " + key + " = " + info.default_value + " of: " + values);
    }
    if (system_ != SystemId::Ds) {
        return;
    }
    for (const auto& [key, info] : core_->declared_options()) {
        // A slot device that can be a Rumble Pak.
        if (rumble_key_.empty()) {
            for (const std::string& value : info.allowed_values) {
                if (lower(value).find("rumble") != std::string::npos) {
                    rumble_key_ = key;
                    rumble_on_value_ = value;
                    break;
                }
            }
            if (!rumble_key_.empty()) {
                // Out is whatever the core starts with, unless that is the
                // Rumble Pak itself -- then anything that is not one.
                rumble_off_value_ = info.default_value;
                if (rumble_off_value_ == rumble_on_value_ || rumble_off_value_.empty()) {
                    for (const std::string& value : info.allowed_values) {
                        if (lower(value).find("rumble") == std::string::npos) {
                            rumble_off_value_ = value;
                            break;
                        }
                    }
                }
            }
        }
    }
    log_info(std::string("this core offers ") +
             (rumble_key_.empty() ? "no Rumble Pak"
                                  : "a Rumble Pak through " + rumble_key_ + " = " +
                                        rumble_on_value_));
}

void LibretroCoreScreenSource::set_rumble_pak(bool inserted) {
    rumble_pak_ = inserted;
    if (rumble_key_.empty()) {
        return;
    }
    core_->force_option(rumble_key_, inserted ? rumble_on_value_ : rumble_off_value_);
    log_info(std::string("Rumble Pak ") + (inserted ? "in" : "out"));
}

void LibretroCoreScreenSource::apply_pointer_rect() {
    // Where the touch screen sits inside the whole framebuffer, plus the size
    // of the screen itself, which together let a touch in the screen's own
    // coordinates become a libretro pointer.
    LibretroCore::PointerSource source;
    source.x = static_cast<float>(split_.bottom_x) / static_cast<float>(split_.frame_width);
    source.y = static_cast<float>(split_.bottom_y) / static_cast<float>(split_.frame_height);
    source.width = static_cast<float>(split_.bottom_width) / static_cast<float>(split_.frame_width);
    source.height =
        static_cast<float>(split_.bottom_height) / static_cast<float>(split_.frame_height);
    const ScreenSize touch = system_profile(system_).screens.bottom;
    source.screen_width = touch.width;
    source.screen_height = touch.height;
    core_->set_pointer_source(source);
}

void LibretroCoreScreenSource::probe_window_framebuffer() {
    // Looks at the window right after the core ran, once. A core told to draw
    // into framebuffer 0 paints the window instead of its own target, and the
    // frontend's clear erases it a moment later -- from the outside that is
    // indistinguishable from a core that never rendered, so it is worth being
    // able to tell the two apart.
    if (window_probe_done_ || gl::ReadPixels == nullptr) return;
    window_probe_done_ = true;

    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    std::string report;
    for (int y : {40, 200, 400}) {
        uint8_t rgba[4] = {0, 0, 0, 0};
        gl::ReadPixels(200, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (!report.empty()) report += ", ";
        report += "y=" + std::to_string(y) + " rgb(" + std::to_string(rgba[0]) + "," +
                  std::to_string(rgba[1]) + "," + std::to_string(rgba[2]) + ")";
    }
    log_info("the window right after the core ran: " + report);
    gl::take_error();
}

void LibretroCoreScreenSource::probe_core_framebuffer(const CoreVideoFrame& frame) {
    // Reads a column down the core's framebuffer and reports where the picture
    // actually is. "The crop ran and the screen is still black" has only two
    // explanations -- we are reading the wrong rows, or there is nothing to
    // read -- and one look down the buffer separates them.
    if (gl::ReadPixels == nullptr || gl::GenFramebuffers == nullptr) return;

    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, frame.framebuffer);
    const int x = static_cast<int>(frame.width) / 2;
    const int rows[] = {
        0,
        static_cast<int>(frame.height) / 4,
        static_cast<int>(frame.height) / 2,
        static_cast<int>(frame.height) - 1,
        static_cast<int>(frame.texture_height) / 2,
        static_cast<int>(frame.texture_height) - 1,
    };
    std::string report;
    bool all_black = true;
    for (const int y : rows) {
        if (y < 0 || y >= static_cast<int>(frame.texture_height)) continue;
        uint8_t rgba[4] = {0, 0, 0, 0};
        gl::ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0) all_black = false;
        if (!report.empty()) report += ", ";
        report += "y=" + std::to_string(y) + " rgb(" + std::to_string(rgba[0]) + "," +
                  std::to_string(rgba[1]) + "," + std::to_string(rgba[2]) + ")";
    }
    int brightest = 0;
    for (int y = 0; y < static_cast<int>(frame.height); y += 32) {
        for (int px = 0; px < static_cast<int>(frame.width); px += 32) {
            uint8_t rgba[4] = {0, 0, 0, 0};
            gl::ReadPixels(px, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            const int sum = rgba[0] + rgba[1] + rgba[2];
            if (sum > brightest) brightest = sum;
        }
    }
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    log_info("down the core's framebuffer at x=" + std::to_string(x) + ": " + report +
             " (brightest sampled pixel " + std::to_string(brightest) + ")");
    gl::log_error_once("reading back the core's framebuffer");

    // The symptom, recognised in the one place it can be: the core said it
    // presented a picture, and every pixel sampled out of what it presented is
    // black. Arm the call trace, so the next few frames say which of the core's
    // OpenGL calls the driver is rejecting. There is no use asking for that up
    // front -- by the time anyone knows to ask, the core has long since
    // resolved its functions and cannot be given wrapped ones.
    if (all_black) {
        gl_trace::arm("the core presented a frame with nothing in it");
    }
}

bool LibretroCoreScreenSource::blit_screen(const CoreVideoFrame& frame,
                                           Texture& destination,
                                           int source_x,
                                           int source_y,
                                           int width,
                                           int height,
                                           int destination_width,
                                           int destination_height) {
    if (destination_width <= 0) destination_width = width;
    if (destination_height <= 0) destination_height = height;
    destination.allocate_rgba(destination_width, destination_height);
    if (!destination.valid()) return false;

    // A blit is clipped by the scissor rect and filtered by the colour mask
    // just as a draw is, and the core we are copying from left both set to suit
    // itself. This runs before the frontend's own frame begins, so it cannot
    // rely on begin_frame having tidied up yet.
    gl::reset_render_state();

    if (screen_framebuffer_ == 0) {
        gl::GenFramebuffers(1, &screen_framebuffer_);
    }
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, frame.framebuffer);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, screen_framebuffer_);
    gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             destination.id(), 0);

    // The core's picture sits in the bottom-left of a texture that may be
    // bigger than the frame, and the split is expressed with a top-left origin.
    // Flipping the source rows in the blit is what turns one into the other,
    // and costs nothing extra.
    int y0 = source_y;
    int y1 = source_y + height;
    if (frame.bottom_left_origin) {
        y0 = static_cast<int>(frame.height) - source_y;
        y1 = static_cast<int>(frame.height) - (source_y + height);
    }
    // Linear only when the two sizes differ, and they only differ when the
    // core is rendering above the console's resolution: point-sampling that
    // reduction would throw away exactly the detail it was rendered for.
    const bool scaled = destination_width != width || destination_height != height;
    gl::BlitFramebuffer(source_x, y0, source_x + width, y1, 0, 0, destination_width,
                        destination_height, GL_COLOR_BUFFER_BIT,
                        scaled ? GL_LINEAR : GL_NEAREST);
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    return true;
}

bool LibretroCoreScreenSource::read_screen(ScreenId screen,
                                           ScreenReadback& out,
                                           int target_width,
                                           int target_height) {
    out = ScreenReadback();
    if (!core_ || !split_.valid) {
        return false;
    }
    const CoreVideoFrame& frame = core_->video_frame();
    if (!frame.valid()) {
        return false;
    }

    const bool is_top = (screen == ScreenId::Top);
    const int source_x = is_top ? split_.top_x : split_.bottom_x;
    const int source_y = is_top ? split_.top_y : split_.bottom_y;
    const int width = is_top ? split_.top_width : split_.bottom_width;
    const int height = is_top ? split_.top_height : split_.bottom_height;
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (frame.source == FrameSource::Pixels) {
        // Already on the CPU, at exactly the console's own resolution: a
        // software-rendered core has nothing above native to offer, so the
        // target size hint has nothing to act on. Hand back a view into the
        // core's own framebuffer rather than copying it.
        out.pixels = frame.pixels + static_cast<std::size_t>(source_y) * frame.stride_pixels +
                     source_x;
        out.width = width;
        out.height = height;
        out.stride_pixels = static_cast<int>(frame.stride_pixels);
        return true;
    }

    // Bounded to what is actually useful: never sharper than what was
    // rendered (there is nothing more to send), and never softer than the
    // console's own resolution (below that a picture stops being legible, and
    // it is the floor every other part of this app already assumes). `width`
    // and `height` here are the crop out of the core's framebuffer, i.e. the
    // resolution this screen was actually rendered at.
    const ScreenSize native = system_profile(system_).screens.size_of(screen);
    int want_width = target_width > 0 ? target_width : native.width;
    int want_height = target_height > 0 ? target_height : native.height;
    want_width = std::clamp(want_width, native.width, width);
    want_height = std::clamp(want_height, native.height, height);
    return read_screen_from_gpu(screen, frame, out, want_width, want_height);
}

bool LibretroCoreScreenSource::read_screen_from_gpu(ScreenId screen,
                                                    const CoreVideoFrame& frame,
                                                    ScreenReadback& out,
                                                    int target_width,
                                                    int target_height) {
    if (gl::ReadPixels == nullptr || gl::GenFramebuffers == nullptr || !gl::has_blit()) {
        if (!readback_failure_reported_) {
            readback_failure_reported_ = true;
            log_warn("this driver cannot copy a GPU-rendered screen back for the other device");
        }
        return false;
    }

    const bool is_top = (screen == ScreenId::Top);
    Readback& readback = readbacks_[is_top ? 0 : 1];
    if (readback.filled && readback.serial == frame.serial && readback.width == target_width &&
        readback.height == target_height) {
        out.pixels = readback.pixels.data();
        out.width = readback.width;
        out.height = readback.height;
        out.stride_pixels = readback.width;
        out.channel_order = ChannelOrder::Rgba;
        return true;  // already read this frame at this size, for the other caller
    }

    // Worth knowing when a stream looks softer or costlier than expected: the
    // one line that ties "what was rendered" to "what is actually going out".
    // Only when the size actually changes, so a companion drawing steadily
    // does not put one of these in the log every frame.
    if (readback.width != target_width || readback.height != target_height) {
        log_info("streaming the " + std::string(to_string(screen)) + " screen at " +
                 std::to_string(target_width) + "x" + std::to_string(target_height));
    }

    const int source_x = is_top ? split_.top_x : split_.bottom_x;
    const int source_y = is_top ? split_.top_y : split_.bottom_y;
    const int width = is_top ? split_.top_width : split_.bottom_width;
    const int height = is_top ? split_.top_height : split_.bottom_height;
    if (!blit_screen(frame, readback.texture, source_x, source_y, width, height, target_width,
                     target_height)) {
        return false;
    }

    if (readback_framebuffer_ == 0) {
        gl::GenFramebuffers(1, &readback_framebuffer_);
    }
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, readback_framebuffer_);
    gl::FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             readback.texture.id(), 0);

    const std::size_t count = static_cast<std::size_t>(target_width) * target_height;
    readback.pixels.resize(count);
    // GL_RGBA rather than a BGRA read: every driver takes this one. What comes
    // back is R,G,B,A in memory rather than the XRGB8888 everything else in
    // this app uses -- it used to be swizzled into that order right here, a
    // second full-frame CPU pass on top of the RGB565 one encode_screen_pixels
    // already has to do. Tagging it Rgba instead lets that encoder read the
    // source order directly, so the swizzle is gone rather than merely cheap.
    gl::ReadPixels(0, 0, target_width, target_height, GL_RGBA, GL_UNSIGNED_BYTE,
                   readback.pixels.data());
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // The rows need no flipping. glReadPixels starts at the framebuffer's
    // bottom-left, which for these textures is the screen's top row: the crop
    // that filled this texture already turned the core's bottom-left origin
    // into the top-row-first order every texture here is uploaded in.

    readback.width = target_width;
    readback.height = target_height;
    readback.serial = frame.serial;
    readback.filled = true;
    gl::take_error();

    out.pixels = readback.pixels.data();
    out.width = readback.width;
    out.height = readback.height;
    out.stride_pixels = readback.width;
    out.channel_order = ChannelOrder::Rgba;
    return true;
}

void LibretroCoreScreenSource::upload_screens() {
    const CoreVideoFrame& frame = core_->video_frame();
    if (!frame.valid() || !split_.valid || frame.serial == uploaded_serial_) {
        // A picture that never arrives looks exactly like one that arrives and
        // is drawn wrong, so say which it is rather than leaving a black screen
        // to be interpreted.
        if (!first_gpu_crop_reported_ && !silence_reported_ && quiet_frames_ > 120) {
            silence_reported_ = true;
            log_warn(std::string("120 frames in and nothing has been drawn: ") +
                     (!frame.valid() ? "the core has not presented a picture"
                                     : (!split_.valid ? "the frame could not be split"
                                                      : "the same frame keeps arriving")));
        }
        return;
    }

    if (frame.source == FrameSource::Texture) {
        gl::log_error_once("OpenGL was already unhappy before the screens were cropped");
        // Nothing to upload: the picture is already on the GPU. The two screens
        // are cropped straight out of it into the textures the compositor draws.
        if (!gl::has_blit()) {
            if (!blit_unavailable_reported_) {
                log_error("this OpenGL driver cannot copy between framebuffers, "
                          "which is how a GPU-rendered picture is split into two screens");
                blit_unavailable_reported_ = true;
            }
            return;
        }
        const bool ok =
            blit_screen(frame, top_texture_, split_.top_x, split_.top_y, split_.top_width,
                        split_.top_height) &&
            blit_screen(frame, bottom_texture_, split_.bottom_x, split_.bottom_y,
                        split_.bottom_width, split_.bottom_height);
        if (ok) {
            uploaded_serial_ = frame.serial;
            gl::log_error_once("cropping the screens out of the core's framebuffer failed");
            if (!first_gpu_crop_reported_) {
                first_gpu_crop_reported_ = true;
                probe_core_framebuffer(frame);
                log_info("cropped the first GPU frame: a " +
                         dimensions(split_.top_width, split_.top_height) + " upper screen at " +
                         std::to_string(split_.top_x) + "," + std::to_string(split_.top_y) +
                         " and a " + dimensions(split_.bottom_width, split_.bottom_height) +
                         " touch screen at " + std::to_string(split_.bottom_x) + "," +
                         std::to_string(split_.bottom_y));
            }
        }
        return;
    }

    const uint32_t* base = frame.pixels;
    const int stride = static_cast<int>(frame.stride_pixels);

    const uint32_t* top = base + static_cast<std::size_t>(split_.top_y) * stride + split_.top_x;
    top_texture_.upload_xrgb8888(top, split_.top_width, split_.top_height, stride);

    const uint32_t* bottom = base + static_cast<std::size_t>(split_.bottom_y) * stride + split_.bottom_x;
    bottom_texture_.upload_xrgb8888(bottom, split_.bottom_width, split_.bottom_height, stride);

    uploaded_serial_ = frame.serial;
}

const ScreenTextures& LibretroCoreScreenSource::screens() {
    upload_screens();
    screens_.top = top_texture_.valid() ? &top_texture_ : nullptr;
    screens_.bottom = bottom_texture_.valid() ? &bottom_texture_ : nullptr;
    return screens_;
}

SystemScreens LibretroCoreScreenSource::screens_geometry() const {
    // The console's own screen sizes rather than the framebuffer's: a core
    // rendering at 4x still shows a 3DS's 400x240 and 320x240 screens, and every
    // layout, touch mapping and preview reasons in those terms.
    return system_profile(system_).screens;
}

uint64_t LibretroCoreScreenSource::frame_serial() const {
    return core_ ? core_->video_frame().serial : 0;
}

bool LibretroCoreScreenSource::wants_shutdown() const {
    return core_ && core_->wants_shutdown();
}

void LibretroCoreScreenSource::send_key_event(bool down, unsigned retro_keycode,
                                              uint32_t character, uint16_t modifiers) {
    if (core_) {
        core_->send_key_event(down, retro_keycode, character, modifiers);
    }
}

double LibretroCoreScreenSource::frames_per_second() const {
    return core_ ? core_->av_info().fps : 60.0;
}

double LibretroCoreScreenSource::audio_sample_rate() const {
    return core_ ? core_->av_info().sample_rate : 48000.0;
}

}  // namespace ds
