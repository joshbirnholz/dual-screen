#pragma once

#include <cstdint>

// Opaque here on purpose: libretro.h stays out of this header.
struct retro_hw_render_callback;
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/hw_render_target.hpp"

namespace ds {

struct DsInputState;

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

    // Whether the loaded core renders on the GPU, which it declares while the
    // game is being opened. Only meaningful after load_game().
    bool uses_hw_render() const { return hw_render_.valid(); }
    // Forces a core option to a value. Applied on top of the core's declared
    // defaults, and validated against the core's own option definitions once
    // those arrive (a mismatch is reported through option_warnings()).
    void force_option(const std::string& key, const std::string& value);

    // --- lifecycle ---
    bool load(const std::string& core_path, std::string* error);
    bool load_game(const std::string& rom_path, std::string* error);
    void run_frame();
    void unload_game();
    void unload();

    // The cartridge's save RAM, valid only once a game is loaded. In libretro
    // this buffer is the frontend's responsibility: the core never touches the
    // filesystem for it, so if we do not read and write it, saves do not exist.
    void* save_ram_data() const;
    std::size_t save_ram_size() const;

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
    int16_t pointer_axis(bool horizontal) const;

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

    CoreAvInfo av_info_;
    CoreVideoFrame video_frame_;
    std::vector<uint32_t> video_buffer_;
    uint64_t video_serial_ = 0;

    std::map<std::string, CoreOptionInfo> declared_options_;
    std::map<std::string, std::string> option_values_;
    std::map<std::string, std::string> forced_options_;
    std::vector<std::string> option_warnings_;
    // What has already been written to the log, so a revalidation is quiet.
    std::vector<std::string> logged_option_warnings_;
    std::vector<std::string> core_messages_;

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
    const DsInputState* input_state_ = nullptr;
    PointerSource pointer_source_;

    // Resolved core entry points.
    struct Api;
    Api* api_ = nullptr;

    // Data the core keeps a pointer to must outlive the call that handed it over.
    std::vector<char> rom_data_;
};

}  // namespace ds
