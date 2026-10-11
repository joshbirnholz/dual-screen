#pragma once

#include <optional>

#include <cstdint>

// Opaque here on purpose: libretro.h stays out of this header.
struct retro_hw_render_callback;
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/hw_render_target.hpp"
#include "core/system.hpp"

namespace ds {

// What a build with its core linked in, rather than loaded from a file, gives
// as that core's path -- see builtin_core_system(). Shown as it is, so the
// Settings screen says where the core is: in the program.
inline constexpr const char* kBuiltInCorePath = "built in";

// The console the linked-in core plays, on a build that has one; nullopt
// everywhere a core is a file loaded at run time. A Switch cannot load code,
// so it carries one core, melonDS, and plays DS games and nothing else.
std::optional<SystemId> builtin_core_system();

struct DsInputState;
enum class DsButton;

struct CoreAvInfo {
    unsigned base_width = 0;
    unsigned base_height = 0;
    unsigned max_width = 0;
    unsigned max_height = 0;
    float aspect_ratio = 0.0f;
    double fps = 60.0;
    double sample_rate = 48000.0;
};

// The most recent frame the core produced, in XRGB8888, owned by LibretroCore.
// Where a frame's picture actually is. A core that renders on the CPU hands
// over pixels; one that renders on the GPU leaves the picture in the
// framebuffer the frontend gave it and only says how much of it is used.
enum class FrameSource { None, Pixels, Texture };

struct CoreVideoFrame {
    FrameSource source = FrameSource::None;
    // Set when source == Pixels.
    const uint32_t* pixels = nullptr;
    // Set when source == Texture: the colour texture of the core's framebuffer,
    // with the picture in its lower-left corner when `bottom_left_origin`.
    GLuint texture = 0;
    // The framebuffer that texture is attached to, which is what a copy out of
    // it reads from.
    GLuint framebuffer = 0;
    unsigned texture_width = 0;   // the whole texture, which is >= width
    unsigned texture_height = 0;
    bool bottom_left_origin = false;
    // The part of either that holds this frame.
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride_pixels = 0;
    uint64_t serial = 0;  // increments whenever a new frame arrives
    bool valid() const {
        if (width == 0 || height == 0) return false;
        if (source == FrameSource::Pixels) return pixels != nullptr;
        if (source == FrameSource::Texture) return texture != 0;
        return false;
    }
};

// One declared core option, as reported by the core itself. We keep these so
// the app can verify at runtime that the options it forces actually exist on
// the core that got loaded, rather than assuming a particular core version.
struct CoreOptionInfo {
    std::string key;
    std::string description;
    std::string default_value;
    std::vector<std::string> allowed_values;
};

// Hosts a libretro core loaded from a shared object. Implements the frontend
// half of the libretro C ABI ourselves — RetroArch is not required, and is not
// involved.
//
// Only one LibretroCore may be loaded at a time: the libretro callback surface
// is plain C function pointers with no user-data parameter, so the active
// instance is reached through a file-static pointer.
class LibretroCore {
public:
    using AudioCallback = std::function<void(const int16_t* interleaved_stereo, std::size_t frames)>;

    LibretroCore();
    ~LibretroCore();

    LibretroCore(const LibretroCore&) = delete;
    LibretroCore& operator=(const LibretroCore&) = delete;

    // --- setup, before load() ---
    void set_system_directory(const std::string& directory);
    void set_save_directory(const std::string& directory);
    void set_audio_callback(AudioCallback callback);
    // Whether the core wants the controller's motor running -- a DS game with
    // a Rumble Pak in its GBA slot, chiefly. Called on change, from inside
    // run_frame(); never for a frame run ahead of the one that counts.
    using RumbleCallback = std::function<void(bool active)>;
    void set_rumble_callback(RumbleCallback callback) { rumble_callback_ = std::move(callback); }
    // The state read by the core's input_state callback each frame.
    void set_input_state(const DsInputState* state);
    // Where the touch screen sits inside the core's own output framebuffer and
    // how large that screen is in its own pixels, under whatever internal
    // layout the core is pinned to. libretro pointer coordinates span the whole
    // framebuffer, so this is what lets a touch expressed in screen pixels be
    // converted back into one. The rect and the size travel together because a
    // touch needs both: getting one from one system and one from another would
    // silently misplace every press.
    struct PointerSource {
        // Normalized position of the touch screen inside the framebuffer.
        float x = 0.0f;
        float y = 0.5f;
        float width = 1.0f;
        float height = 0.5f;
        // The touch screen's own resolution, which touch points are given in.
        int screen_width = 256;
        int screen_height = 192;
    };
    void set_pointer_source(const PointerSource& source);
    // Forwards one physical key event to the core's own on-screen keyboard,
    // if it has registered one -- a no-op otherwise. `retro_keycode` and
    // `modifiers` are RETROK_*/RETROKMOD_* from libretro.h, kept as plain
    // integers here so this header does not need it.
    void send_key_event(bool down, unsigned retro_keycode, uint32_t character, uint16_t modifiers);

    // Whether the loaded core renders on the GPU, which it declares while the
    // game is being opened. Only meaningful after load_game().
    bool uses_hw_render() const { return hw_render_.valid(); }
    // Whether a core that does not declare an option at all is worth
    // reporting. A system can be run by more than one core -- 3DS games by
    // Citra or by Azahar -- and the two do not always have the same options,
    // so "this core has no such option" is a real problem for some settings
    // and simply not applicable for others.
    enum class OptionRequirement {
        Required,   // every core we support has this one
        IfPresent,  // only some do, and the ones that do not need no setting
    };

    // Forces a core option to a value. Applied on top of the core's declared
    // defaults, and validated against the core's own option definitions once
    // those arrive (a mismatch is reported through option_warnings()).
    void force_option(const std::string& key, const std::string& value);

    // The same, given several spellings of the same setting: the first one the
    // loaded core actually offers is the one used, and only a core offering
    // none of them is reported.
    //
    // This exists because the value a setting is spelled with is the core's
    // business, not the setting's. Citra's screen layouts are written out in
    // words ("Default Top-Bottom Screen") and Azahar's are identifiers
    // ("default"); Citra's resolution scale is "3x" and Azahar's is "3". Both
    // run 3DS games and either may be the one installed, so the frontend asks
    // for the setting and lets the core say which name it goes by.
    void force_option(const std::string& key, std::vector<std::string> values,
                      OptionRequirement requirement = OptionRequirement::Required);

    // --- lifecycle ---
    bool load(const std::string& core_path, std::string* error);
    bool load_game(const std::string& rom_path, std::string* error);
    void run_frame();
    // Restarts the running game from power-on, the way a console's own reset
    // button would -- a no-op with nothing loaded.
    void reset();
    void unload_game();
    void unload();

    // The cartridge's save RAM, valid only once a game is loaded. In libretro
    // this buffer is the frontend's responsibility: the core never touches the
    // filesystem for it, so if we do not read and write it, saves do not exist.
    void* save_ram_data() const;
    std::size_t save_ram_size() const;

    // The whole running machine, as the core itself writes it out -- a save
    // state. Optional in libretro: a core that does not export these cannot
    // have one, and one that reports a size of 0 has none to give right now.
    // All of these run on the thread that runs frames, which is the only
    // thread a core is promised to be called on.
    //
    // has_save_states() is only whether the core has the functions, and is
    // cheap enough to ask every frame. state_size() is not: some cores --
    // Citra's among them -- find their size by writing a whole state out, so
    // it is asked only when one is actually being made.
    bool has_save_states() const;
    std::size_t state_size() const;
    bool save_state(std::vector<uint8_t>& out) const;
    bool load_state(const std::vector<uint8_t>& state);

    // Run-ahead: how many frames of the game's own input lag to hide.
    //
    // A game reads the pad in one frame and shows the result in a later one
    // -- two or three frames later is ordinary on a DS -- and a stream adds
    // its own delay on top. Each frame, the frame that counts is run and its
    // sound kept; the machine is saved; it is run on this many frames more
    // with the same input held, silently, and the last of those is the
    // picture shown; then it is put back as it was. What is on screen is
    // where the game will be a few frames from now if nothing changes, which
    // is exactly the delay the game itself would otherwise add. RetroArch's
    // single-instance run-ahead, the same way round.
    //
    // It costs a whole frame of emulation per frame hidden, plus a save and a
    // load each frame, and it only works on a core that can save states --
    // and quickly. Zero, the default, runs frames exactly as before. A core
    // that fails to save or load a state turns it back off and says so.
    void set_run_ahead(unsigned frames);
    unsigned run_ahead() const { return run_ahead_; }

    bool is_loaded() const { return handle_ != nullptr; }
    bool game_loaded() const { return game_loaded_; }
    bool wants_shutdown() const { return wants_shutdown_; }

    // --- state ---
    const CoreAvInfo& av_info() const { return av_info_; }
    const CoreVideoFrame& video_frame() const { return video_frame_; }
    const std::string& core_name() const { return core_name_; }
    const std::string& core_version() const { return core_version_; }
    const std::string& valid_extensions() const { return valid_extensions_; }
    bool needs_fullpath() const { return needs_fullpath_; }
    // Messages the core asked the frontend to display.
    const std::vector<std::string>& core_messages() const { return core_messages_; }
    void clear_core_messages() { core_messages_.clear(); }
    // Problems found while applying forced options (unknown key, value the core
    // does not offer). Empty means every forced option was accepted as written.
    const std::vector<std::string>& option_warnings() const { return option_warnings_; }
    const std::map<std::string, CoreOptionInfo>& declared_options() const { return declared_options_; }
    std::string option_value(const std::string& key) const;
    // The pad button this core blows into the microphone with, by libretro id:
    // whichever one its input descriptors call "Microphone", or L3 when it
    // names none. See mic_button_id_.
    unsigned microphone_button() const { return mic_button_id_; }

private:
    // libretro frontend callbacks (static thunks -> active instance)
    static bool environment_thunk(unsigned command, void* data);
    static void video_refresh_thunk(const void* data, unsigned width, unsigned height, std::size_t pitch);
    static void audio_sample_thunk(int16_t left, int16_t right);
    static std::size_t audio_sample_batch_thunk(const int16_t* data, std::size_t frames);
    static void input_poll_thunk();
    static int16_t input_state_thunk(unsigned port, unsigned device, unsigned index, unsigned id);

    bool on_environment(unsigned command, void* data);
    void on_video_refresh(const void* data, unsigned width, unsigned height, std::size_t pitch);
    int16_t on_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    // retro_id_for(), except for the microphone -- see mic_button_id_.
    unsigned id_for(DsButton button) const;
    int16_t pointer_axis(bool horizontal) const;

    // One retro_run, with this frontend's GL handover around it. run_frame()
    // is this, or several of it for run-ahead.
    void run_once();

    void register_option(const CoreOptionInfo& info);
    void apply_forced_options();
    void refresh_av_info();
    template <typename T>
    bool resolve_symbol(const char* name, T& out, std::string* error);

    void* handle_ = nullptr;
    bool game_loaded_ = false;
    bool wants_shutdown_ = false;
    bool options_registered_ = false;

    std::string system_directory_;
    std::string save_directory_;
    std::string core_name_;
    std::string core_version_;
    std::string valid_extensions_;
    bool needs_fullpath_ = false;
    bool supports_input_bitmasks_ = false;
    bool variables_dirty_ = true;

    // --- run-ahead ---
    unsigned run_ahead_ = 0;
    // While set, what the core draws or plays is not taken: the frame that
    // counts is not the one shown, and only it is heard.
    bool hide_video_ = false;
    bool hide_audio_ = false;
    // The machine as it stood after the frame that counts, put back once the
    // frames ahead of it have been drawn. Sized once: asking some cores their
    // size means writing a whole state out.
    std::vector<uint8_t> run_ahead_state_;
    // What run-ahead is costing, said once it has run for a few seconds.
    double run_ahead_spent_ms_ = 0.0;
    unsigned run_ahead_frames_timed_ = 0;

    CoreAvInfo av_info_;
    CoreVideoFrame video_frame_;
    std::vector<uint32_t> video_buffer_;
    uint64_t video_serial_ = 0;

    std::map<std::string, CoreOptionInfo> declared_options_;
    std::map<std::string, std::string> option_values_;
    // Where this app's microphone button (DsButton::LeftStick, on a console
    // with one) reaches the core. melonDS DS moved it: up to 1.2 the
    // microphone is L2 and L3 closes the DS's lid -- a press that blanked
    // both screens and paused the game until it was pressed again -- and
    // from 1.3 the microphone is L3. Both say which in their input
    // descriptors, so the core's own word is taken over any version number.
    unsigned mic_button_id_ = kJoypadL3;
    static constexpr unsigned kJoypadL3 = 14;  // RETRO_DEVICE_ID_JOYPAD_L3
    // A setting we insist on, and the spellings it may go by.
    struct ForcedOption {
        std::vector<std::string> values;  // in order of preference
        OptionRequirement requirement = OptionRequirement::Required;
    };
    std::map<std::string, ForcedOption> forced_options_;
    std::vector<std::string> option_warnings_;
    // What has already been written to the log, so a revalidation is quiet.
    std::vector<std::string> logged_option_warnings_;
    std::vector<std::string> core_messages_;
    // The last one written to the log. A core is entitled to re-send the same
    // message every frame -- melonDS restates its layout that often -- and
    // logging each one buries everything else at sixty lines a second.
    std::string last_logged_core_message_;

    // --- hardware rendering ---
    // Set when the core asked for a GPU context. The callback is the core's
    // own struct, kept because context_reset/context_destroy live in it.
    bool hw_render_requested_ = false;
    struct retro_hw_render_callback* hw_callback_ = nullptr;
    HwRenderTarget hw_render_;

public:
    // For the thunks in the .cpp, which cannot be members without dragging
    // libretro's calling convention into this header.
    GLuint hw_framebuffer_name() const { return hw_render_.framebuffer(); }
    // The sensor interface, for the same reason: libretro's own enum is in its
    // signature. `action` is a RETRO_SENSOR_* value and `id` a sensor id, kept
    // as plain unsigned here the way the keyboard modifiers above are.
    bool on_set_sensor_state(unsigned port, unsigned action, unsigned rate);
    float on_sensor_input(unsigned port, unsigned id) const;
    // The rumble interface, likewise: `effect` is a RETRO_RUMBLE_* value.
    bool on_set_rumble_state(unsigned port, unsigned effect, uint16_t strength);

private:
    bool hw_context_reset_done_ = false;
    // What the core last said it presented, so a change is reported once.
    // True until the core declares a real output size, so the guess made
    // before that can be replaced rather than grown from.
    bool hw_size_is_placeholder_ = true;
    bool hw_stain_done_ = false;
    // Raised when a core presents something larger than it declared.
    unsigned minimum_hw_width_ = 0;
    unsigned minimum_hw_height_ = 0;
    unsigned reported_frame_width_ = 0;
    unsigned reported_frame_height_ = 0;
    bool accept_hw_render(struct retro_hw_render_callback* callback);
    bool ensure_hw_target();
    void reset_hw_context();
    void destroy_hw_context();

    AudioCallback audio_callback_;
    RumbleCallback rumble_callback_;
    // Each motor's strength as the core last set it, strong then weak.
    uint16_t rumble_strength_[2] = {0, 0};
    bool rumbling_ = false;
    const DsInputState* input_state_ = nullptr;
    // Whether the core has asked for either sensor. A core that never asks
    // gets nothing, and one that asks and then stops should stop being given
    // readings rather than quietly keep receiving them -- which on a handheld
    // companion is a gyroscope left running for a game that gave up on it.
    bool wants_accelerometer_ = false;
    bool wants_gyroscope_ = false;
    PointerSource pointer_source_;
    // The core's own keyboard-event handler, if it registered one -- same
    // signature as libretro's retro_keyboard_event_t, spelled out locally so
    // this header does not need to include libretro.h for it.
    void (*keyboard_callback_)(bool down, unsigned keycode, uint32_t character,
                               uint16_t key_modifiers) = nullptr;

    // Resolved core entry points.
    struct Api;
    Api* api_ = nullptr;

    // Data the core keeps a pointer to must outlive the call that handed it over.
    std::vector<char> rom_data_;
};

}  // namespace ds
