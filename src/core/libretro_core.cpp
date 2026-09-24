#include <cctype>
#include "core/libretro_core.hpp"

#include "core/gl_call_trace.hpp"

#include <SDL.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "input/ds_input.hpp"
#include "libretro/libretro.h"
#include "render/gl_loader.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// The libretro callback surface is plain C function pointers with no user-data
// parameter, so the active core instance has to be reachable statically.
LibretroCore* g_active_core = nullptr;

// libretro hands these two straight to the core, so they have to carry
// libretro's own types -- which is why they are here rather than on the class:
// libretro_core.hpp deliberately does not include libretro.h.
bool set_sensor_state_thunk(unsigned port, enum retro_sensor_action action, unsigned rate) {
    if (g_active_core == nullptr) return false;
    return g_active_core->on_set_sensor_state(port, static_cast<unsigned>(action), rate);
}

float sensor_input_thunk(unsigned port, unsigned id) {
    if (g_active_core == nullptr) return 0.0f;
    return g_active_core->on_sensor_input(port, id);
}

// Handed to a GPU-rendering core so it knows where to draw. libretro calls
// these with its own calling convention, so they are free functions here
// rather than members.
uintptr_t RETRO_CALLCONV hw_get_current_framebuffer_thunk() {
    const GLuint name = (g_active_core == nullptr) ? 0 : g_active_core->hw_framebuffer_name();
    // Once. Zero here means the core is being told to draw into the window,
    // where the frontend's own clear wipes it out before anyone sees it -- which
    // looks exactly like a core that never rendered.
    static bool reported = false;
    if (!reported) {
        reported = true;
        log_info("the core asked where to draw and was given framebuffer " +
                 std::to_string(name));
    }
    return static_cast<uintptr_t>(name);
}

retro_proc_address_t RETRO_CALLCONV hw_get_proc_address_thunk(const char* symbol) {
    if (symbol == nullptr) return nullptr;
    // The same resolver the frontend's own renderer uses, so the core and the
    // frontend are talking to one driver rather than two.
    // A traced wrapper when tracing is on, so a call the driver rejects names
    // itself instead of surfacing as "an error happened somewhere this frame".
    if (void* traced = gl_trace::wrapper_for(symbol)) {
        return reinterpret_cast<retro_proc_address_t>(traced);
    }
    void* address = SDL_GL_GetProcAddress(symbol);
    if (address == nullptr) {
        // A core that silently loses an entry point renders nothing and says
        // nothing. Capped, because a loader asking for every name in the
        // specification will miss plenty it never intended to use.
        static int reported = 0;
        if (reported < 12) {
            ++reported;
            log_debug(std::string("the core asked for a GL entry point this context does not "
                                  "have: ") + symbol);
        }
    }
    return reinterpret_cast<retro_proc_address_t>(address);
}

std::vector<std::string> split_values(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, separator)) {
        if (!current.empty()) {
            parts.push_back(current);
        }
    }
    return parts;
}

LogLevel translate_log_level(int level) {
    switch (level) {
        case RETRO_LOG_DEBUG: return LogLevel::Debug;
        case RETRO_LOG_INFO:  return LogLevel::Info;
        case RETRO_LOG_WARN:  return LogLevel::Warn;
        default:              return LogLevel::Error;
    }
}

// Matches retro_log_printf_t exactly; cores keep this pointer for their lifetime.
void core_log_printf(enum retro_log_level level, const char* format, ...) {
    if (format == nullptr) return;
    char buffer[2048];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written <= 0) return;

    std::string message(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    if (message.empty()) return;
    log_message(translate_log_level(static_cast<int>(level)), "core: " + message);
}

unsigned retro_id_for(DsButton button) {
    switch (button) {
        // libretro's face-button names follow the SNES layout: JOYPAD_B is the
        // south button, JOYPAD_A the east one. That matches how every other
        // DS core is mapped, so a player's existing muscle memory carries over.
        case DsButton::A:      return RETRO_DEVICE_ID_JOYPAD_A;
        case DsButton::B:      return RETRO_DEVICE_ID_JOYPAD_B;
        case DsButton::X:      return RETRO_DEVICE_ID_JOYPAD_X;
        case DsButton::Y:      return RETRO_DEVICE_ID_JOYPAD_Y;
        case DsButton::L:      return RETRO_DEVICE_ID_JOYPAD_L;
        case DsButton::R:      return RETRO_DEVICE_ID_JOYPAD_R;
        case DsButton::Start:  return RETRO_DEVICE_ID_JOYPAD_START;
        case DsButton::Select: return RETRO_DEVICE_ID_JOYPAD_SELECT;
        case DsButton::Up:     return RETRO_DEVICE_ID_JOYPAD_UP;
        case DsButton::Down:   return RETRO_DEVICE_ID_JOYPAD_DOWN;
        case DsButton::Left:   return RETRO_DEVICE_ID_JOYPAD_LEFT;
        case DsButton::Right:  return RETRO_DEVICE_ID_JOYPAD_RIGHT;
        // No console played by a core has these; a Wii U does, and it is not
        // played by one. Mapped anyway rather than left to fall through to B,
        // which is what an unhandled case here would silently become.
        case DsButton::Zl:     return RETRO_DEVICE_ID_JOYPAD_L2;
        case DsButton::Zr:     return RETRO_DEVICE_ID_JOYPAD_R2;
        case DsButton::LeftStick:  return RETRO_DEVICE_ID_JOYPAD_L3;
        case DsButton::RightStick: return RETRO_DEVICE_ID_JOYPAD_R3;
        case DsButton::Count:  break;
    }
    return RETRO_DEVICE_ID_JOYPAD_B;
}

}  // namespace

// Entry points resolved out of the core's shared object.
struct LibretroCore::Api {
    void (*retro_init)() = nullptr;
    void (*retro_deinit)() = nullptr;
    unsigned (*retro_api_version)() = nullptr;
    void (*retro_get_system_info)(struct retro_system_info*) = nullptr;
    void (*retro_get_system_av_info)(struct retro_system_av_info*) = nullptr;
    void (*retro_set_environment)(retro_environment_t) = nullptr;
    void (*retro_set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (*retro_set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*retro_set_input_poll)(retro_input_poll_t) = nullptr;
    void (*retro_set_input_state)(retro_input_state_t) = nullptr;
    void (*retro_set_controller_port_device)(unsigned, unsigned) = nullptr;
    void (*retro_reset)() = nullptr;
    void (*retro_run)() = nullptr;
    bool (*retro_load_game)(const struct retro_game_info*) = nullptr;
    void (*retro_unload_game)() = nullptr;
    void* (*retro_get_memory_data)(unsigned) = nullptr;
    std::size_t (*retro_get_memory_size)(unsigned) = nullptr;
    // Optional: save states. See LibretroCore::state_size().
    std::size_t (*retro_serialize_size)() = nullptr;
    bool (*retro_serialize)(void*, std::size_t) = nullptr;
    bool (*retro_unserialize)(const void*, std::size_t) = nullptr;
};

LibretroCore::LibretroCore() : api_(new Api()) {}

LibretroCore::~LibretroCore() {
    unload();
    delete api_;
    api_ = nullptr;
}

void LibretroCore::set_system_directory(const std::string& directory) { system_directory_ = directory; }
void LibretroCore::set_save_directory(const std::string& directory) { save_directory_ = directory; }
void LibretroCore::set_audio_callback(AudioCallback callback) { audio_callback_ = std::move(callback); }
void LibretroCore::set_input_state(const DsInputState* state) { input_state_ = state; }

void LibretroCore::set_pointer_source(const PointerSource& source) { pointer_source_ = source; }

void LibretroCore::send_key_event(bool down, unsigned retro_keycode, uint32_t character,
                                  uint16_t modifiers) {
    // A no-op until a core actually registers one -- most never do, since
    // most never need text entry.
    if (keyboard_callback_ != nullptr) {
        keyboard_callback_(down, retro_keycode, character, modifiers);
    }
}

int16_t LibretroCore::pointer_axis(bool horizontal) const {
    // Sample the centre of the addressed touch-screen pixel, then place it
    // inside the core's framebuffer and scale to libretro's [-0x7fff, 0x7fff]
    // range. The divisor is this system's touch screen, not the DS's: a 3DS
    // touch screen is 320x240, so dividing by 256x192 would overshoot by 25%.
    const float within_screen =
        horizontal ? (static_cast<float>(input_state_->touch_x) + 0.5f) /
                         static_cast<float>(pointer_source_.screen_width)
                   : (static_cast<float>(input_state_->touch_y) + 0.5f) /
                         static_cast<float>(pointer_source_.screen_height);
    const float normalized = horizontal ? pointer_source_.x + within_screen * pointer_source_.width
                                        : pointer_source_.y + within_screen * pointer_source_.height;
    const float scaled = (normalized * 2.0f - 1.0f) * 32767.0f;
    return static_cast<int16_t>(std::clamp(scaled, -32767.0f, 32767.0f));
}

void LibretroCore::force_option(const std::string& key, const std::string& value) {
    force_option(key, std::vector<std::string>{value});
}

void LibretroCore::force_option(const std::string& key, std::vector<std::string> values,
                                OptionRequirement requirement) {
    if (values.empty()) {
        return;
    }
    ForcedOption& forced = forced_options_[key];
    forced.values = std::move(values);
    forced.requirement = requirement;
    option_values_[key] = forced.values.front();
    variables_dirty_ = true;
    if (options_registered_) {
        apply_forced_options();
    }
}

std::string LibretroCore::option_value(const std::string& key) const {
    const auto it = option_values_.find(key);
    return it == option_values_.end() ? std::string() : it->second;
}

template <typename T>
bool LibretroCore::resolve_symbol(const char* name, T& out, std::string* error) {
    void* symbol = dlsym(handle_, name);
    if (symbol == nullptr) {
        if (error != nullptr) {
            *error = std::string("core is missing the libretro entry point '") + name + "'";
        }
        return false;
    }
    out = reinterpret_cast<T>(symbol);
    return true;
}

bool LibretroCore::load(const std::string& core_path, std::string* error) {
    if (handle_ != nullptr) {
        if (error != nullptr) *error = "a core is already loaded";
        return false;
    }
    if (g_active_core != nullptr) {
        if (error != nullptr) *error = "another libretro core instance is already active";
        return false;
    }

    // RTLD_LOCAL keeps the core's symbols out of the global namespace; cores
    // routinely bundle their own copies of zlib and friends.
    handle_ = dlopen(core_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char* reason = dlerror();
        if (error != nullptr) {
            *error = "could not load core '" + core_path + "': " + (reason ? reason : "unknown error");
        }
        return false;
    }

    const bool resolved =
        resolve_symbol("retro_api_version", api_->retro_api_version, error) &&
        resolve_symbol("retro_init", api_->retro_init, error) &&
        resolve_symbol("retro_deinit", api_->retro_deinit, error) &&
        resolve_symbol("retro_get_system_info", api_->retro_get_system_info, error) &&
        resolve_symbol("retro_get_system_av_info", api_->retro_get_system_av_info, error) &&
        resolve_symbol("retro_set_environment", api_->retro_set_environment, error) &&
        resolve_symbol("retro_set_video_refresh", api_->retro_set_video_refresh, error) &&
        resolve_symbol("retro_set_audio_sample", api_->retro_set_audio_sample, error) &&
        resolve_symbol("retro_set_audio_sample_batch", api_->retro_set_audio_sample_batch, error) &&
        resolve_symbol("retro_set_input_poll", api_->retro_set_input_poll, error) &&
        resolve_symbol("retro_set_input_state", api_->retro_set_input_state, error) &&
        resolve_symbol("retro_set_controller_port_device", api_->retro_set_controller_port_device, error) &&
        resolve_symbol("retro_reset", api_->retro_reset, error) &&
        resolve_symbol("retro_run", api_->retro_run, error) &&
        resolve_symbol("retro_load_game", api_->retro_load_game, error) &&
        resolve_symbol("retro_unload_game", api_->retro_unload_game, error) &&
        resolve_symbol("retro_get_memory_data", api_->retro_get_memory_data, error) &&
        resolve_symbol("retro_get_memory_size", api_->retro_get_memory_size, error);
    if (!resolved) {
        dlclose(handle_);
        handle_ = nullptr;
        return false;
    }
    // Save states are a feature some cores have, not a requirement of loading
    // one: looked up without failing, and a core without them simply offers
    // no save points.
    api_->retro_serialize_size =
        reinterpret_cast<std::size_t (*)()>(dlsym(handle_, "retro_serialize_size"));
    api_->retro_serialize =
        reinterpret_cast<bool (*)(void*, std::size_t)>(dlsym(handle_, "retro_serialize"));
    api_->retro_unserialize =
        reinterpret_cast<bool (*)(const void*, std::size_t)>(dlsym(handle_, "retro_unserialize"));

    const unsigned api_version = api_->retro_api_version();
    if (api_version != RETRO_API_VERSION) {
        if (error != nullptr) {
            *error = "core targets libretro API version " + std::to_string(api_version) +
                     ", this build speaks version " + std::to_string(RETRO_API_VERSION);
        }
        dlclose(handle_);
        handle_ = nullptr;
        return false;
    }

    g_active_core = this;

    // retro_set_environment must come first: cores declare their options from
    // inside it, and we need those before retro_init runs.
    api_->retro_set_environment(&LibretroCore::environment_thunk);
    api_->retro_set_video_refresh(&LibretroCore::video_refresh_thunk);
    api_->retro_set_audio_sample(&LibretroCore::audio_sample_thunk);
    api_->retro_set_audio_sample_batch(&LibretroCore::audio_sample_batch_thunk);
    api_->retro_set_input_poll(&LibretroCore::input_poll_thunk);
    api_->retro_set_input_state(&LibretroCore::input_state_thunk);

    struct retro_system_info system_info {};
    api_->retro_get_system_info(&system_info);
    core_name_ = system_info.library_name != nullptr ? system_info.library_name : "";
    core_version_ = system_info.library_version != nullptr ? system_info.library_version : "";
    valid_extensions_ = system_info.valid_extensions != nullptr ? system_info.valid_extensions : "";
    needs_fullpath_ = system_info.need_fullpath;

    api_->retro_init();
    apply_forced_options();

    log_info("loaded core " + core_name_ + " " + core_version_ + " from " + core_path);
    return true;
}

bool LibretroCore::load_game(const std::string& rom_path, std::string* error) {
    if (handle_ == nullptr) {
        if (error != nullptr) *error = "no core loaded";
        return false;
    }
    if (game_loaded_) {
        unload_game();
    }

    struct retro_game_info info {};
    info.path = rom_path.c_str();
    info.meta = nullptr;

    if (!needs_fullpath_) {
        std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            if (error != nullptr) *error = "could not open ROM: " + rom_path;
            return false;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            if (error != nullptr) *error = "ROM file is empty: " + rom_path;
            return false;
        }
        file.seekg(0, std::ios::beg);
        rom_data_.resize(static_cast<std::size_t>(size));
        if (!file.read(rom_data_.data(), size)) {
            if (error != nullptr) *error = "could not read ROM: " + rom_path;
            rom_data_.clear();
            return false;
        }
        info.data = rom_data_.data();
        info.size = rom_data_.size();
    }

    if (!api_->retro_load_game(&info)) {
        rom_data_.clear();
        if (error != nullptr) {
            *error = "the core refused to load this ROM";
            if (!core_messages_.empty()) {
                *error += ": " + core_messages_.back();
            }
        }
        return false;
    }

    game_loaded_ = true;
    api_->retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    refresh_av_info();

    if (hw_render_requested_) {
        // Now that av_info is in, the framebuffer can be sized to what the core
        // will really draw rather than the placeholder it was given during
        // load. Only then is the core told its context is ready.
        if (!ensure_hw_target()) {
            if (error != nullptr) {
                *error = "could not give the core a framebuffer to render into";
            }
            api_->retro_unload_game();
            game_loaded_ = false;
            rom_data_.clear();
            return false;
        }
        reset_hw_context();
    }
    return true;
}

void* LibretroCore::save_ram_data() const {
    if (handle_ == nullptr || !game_loaded_ || api_->retro_get_memory_data == nullptr) {
        return nullptr;
    }
    return api_->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
}

std::size_t LibretroCore::save_ram_size() const {
    if (handle_ == nullptr || !game_loaded_ || api_->retro_get_memory_size == nullptr) {
        return 0;
    }
    return api_->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
}

bool LibretroCore::has_save_states() const {
    return handle_ != nullptr && game_loaded_ && api_->retro_serialize_size != nullptr &&
           api_->retro_serialize != nullptr && api_->retro_unserialize != nullptr;
}

std::size_t LibretroCore::state_size() const {
    return has_save_states() ? api_->retro_serialize_size() : 0;
}

bool LibretroCore::save_state(std::vector<uint8_t>& out) const {
    const std::size_t size = state_size();
    if (size == 0) {
        return false;
    }
    out.assign(size, 0);
    return api_->retro_serialize(out.data(), out.size());
}

bool LibretroCore::load_state(const std::vector<uint8_t>& state) {
    if (!has_save_states() || state.empty()) {
        return false;
    }
    return api_->retro_unserialize(state.data(), state.size());
}

void LibretroCore::refresh_av_info() {
    struct retro_system_av_info info {};
    api_->retro_get_system_av_info(&info);
    av_info_.base_width = info.geometry.base_width;
    av_info_.base_height = info.geometry.base_height;
    av_info_.max_width = info.geometry.max_width;
    av_info_.max_height = info.geometry.max_height;
    av_info_.aspect_ratio = info.geometry.aspect_ratio;
    av_info_.fps = info.timing.fps > 0.0 ? info.timing.fps : 60.0;
    av_info_.sample_rate = info.timing.sample_rate > 0.0 ? info.timing.sample_rate : 48000.0;
}

// --- hardware rendering ----------------------------------------------------

bool LibretroCore::accept_hw_render(struct retro_hw_render_callback* callback) {
    if (callback == nullptr) return false;

    // Only OpenGL, and only a context this window can actually service. Saying
    // no here is the honest answer -- the core reports it as a failure to load
    // the game, which is far better than drawing nothing and calling it success.
    const char* refusal = nullptr;
    switch (callback->context_type) {
        case RETRO_HW_CONTEXT_OPENGL:
        case RETRO_HW_CONTEXT_OPENGL_CORE:
            if (gl::is_gles()) {
                refusal = "this window has an OpenGL ES context and the core wants desktop OpenGL";
            } else if (!gl::at_least(static_cast<int>(callback->version_major),
                                     static_cast<int>(callback->version_minor))) {
                refusal = "this driver's OpenGL is older than the core asked for";
            }
            break;
        case RETRO_HW_CONTEXT_OPENGLES2:
        case RETRO_HW_CONTEXT_OPENGLES3:
        case RETRO_HW_CONTEXT_OPENGLES_VERSION:
            if (!gl::is_gles()) {
                refusal = "this window has a desktop OpenGL context and the core wants OpenGL ES";
            }
            break;
        default:
            refusal = "this frontend only renders cores through OpenGL";
            break;
    }
    if (refusal != nullptr) {
        const std::string message =
            std::string("declining the core's request for a hardware context: ") + refusal;
        log_error(message);
        option_warnings_.push_back(message);
        return false;
    }

    hw_callback_ = callback;
    hw_render_requested_ = true;
    // Silently, and before the core resolves anything: the wrappers cannot be
    // put in place later, and whether they will be needed is not known yet.
    gl_trace::install();
    hw_context_reset_done_ = false;
    callback->get_current_framebuffer = &hw_get_current_framebuffer_thunk;
    callback->get_proc_address = &hw_get_proc_address_thunk;

    log_info("the core renders on the GPU; giving it a framebuffer to draw into");
    // The framebuffer has to exist before context_reset, because the core sets
    // up its own GL state there and may bind ours immediately after.
    return ensure_hw_target();
}

bool LibretroCore::ensure_hw_target() {
    if (!hw_render_requested_) return false;

    // Sized to what the core says it is drawing, not to the largest it could
    // ever draw. Citra declares a maximum ten times its base -- room for a
    // resolution multiplier it is not using -- so taking the maximum leaves the
    // picture in one corner of a buffer a hundred times its area, and every
    // read out of it then depends on knowing which corner. Matching the base
    // removes that question, and reclaims the 76MB the maximum would cost.
    //
    // Growing is still handled: the core announces a size change through
    // SET_GEOMETRY or SET_SYSTEM_AV_INFO before it draws at the new size, and
    // run_frame checks again in case a core ever draws larger unannounced.
    unsigned width = av_info_.base_width != 0 ? av_info_.base_width : av_info_.max_width;
    unsigned height = av_info_.base_height != 0 ? av_info_.base_height : av_info_.max_height;
    width = width > minimum_hw_width_ ? width : minimum_hw_width_;
    height = height > minimum_hw_height_ ? height : minimum_hw_height_;
    // Whether this is the real size or a guess made before the core declared
    // one. A guess must not become a floor the real size cannot shrink below.
    const bool from_declared_geometry = (width != 0 && height != 0);
    if (!from_declared_geometry) {
        // Nothing declared yet. A modest default keeps the framebuffer valid
        // until the real size arrives and replaces it.
        width = 1024;
        height = 1024;
    }
    const bool exact = from_declared_geometry && hw_size_is_placeholder_;

    const GLuint before = hw_render_.framebuffer();
    std::string error;
    if (!hw_render_.create(width, height, hw_callback_ != nullptr && hw_callback_->depth,
                           hw_callback_ != nullptr && hw_callback_->stencil, exact, &error)) {
        log_error(error);
        option_warnings_.push_back(error);
        return false;
    }
    if (from_declared_geometry) {
        hw_size_is_placeholder_ = false;
    }
    if (hw_render_.framebuffer() != before) {
        // Growing throws the old framebuffer away. Any frame still pointing at
        // it describes a texture that no longer exists, so it is dropped rather
        // than left to be read from.
        video_frame_ = CoreVideoFrame();
    }
    return true;
}

void LibretroCore::reset_hw_context() {
    if (!hw_render_requested_ || hw_callback_ == nullptr) return;
    if (hw_callback_->context_reset == nullptr) return;
    if (hw_context_reset_done_) return;
    // The core builds its GL resources in here, so it must run with our context
    // current and its own framebuffer already allocated.
    hw_callback_->context_reset();
    hw_context_reset_done_ = true;
}

void LibretroCore::destroy_hw_context() {
    if (hw_render_requested_ && hw_callback_ != nullptr && hw_context_reset_done_ &&
        hw_callback_->context_destroy != nullptr) {
        hw_callback_->context_destroy();
    }
    hw_context_reset_done_ = false;
    hw_render_.destroy();
    hw_callback_ = nullptr;
    hw_render_requested_ = false;
}

void LibretroCore::set_run_ahead(unsigned frames) {
    frames = std::min(frames, 3u);
    if (frames == run_ahead_) {
        return;
    }
    run_ahead_ = frames;
    run_ahead_state_.clear();
    run_ahead_spent_ms_ = 0.0;
    run_ahead_frames_timed_ = 0;
    log_info(frames == 0 ? std::string("run-ahead is off")
                         : "running " + std::to_string(frames) + " frame" +
                               (frames == 1 ? "" : "s") + " ahead to hide the game's own lag");
}

void LibretroCore::run_frame() {
    if (handle_ == nullptr || !game_loaded_) {
        return;
    }
    if (run_ahead_ == 0 || !has_save_states()) {
        run_once();
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    // The frame that counts: heard, not seen.
    hide_video_ = true;
    run_once();
    if (run_ahead_state_.empty()) {
        run_ahead_state_.resize(state_size());
    }
    if (run_ahead_state_.empty() ||
        !api_->retro_serialize(run_ahead_state_.data(), run_ahead_state_.size())) {
        // This frame goes unseen -- its picture was not taken -- and the next
        // is run the ordinary way.
        hide_video_ = false;
        run_ahead_ = 0;
        run_ahead_state_.clear();
        log_warn("this core could not save its state to run ahead; run-ahead is off");
        return;
    }
    // The frames ahead of it: seen, the last of them, and never heard.
    hide_audio_ = true;
    for (unsigned frame = 0; frame < run_ahead_; ++frame) {
        hide_video_ = frame + 1 < run_ahead_;
        run_once();
    }
    hide_video_ = false;
    hide_audio_ = false;
    if (!api_->retro_unserialize(run_ahead_state_.data(), run_ahead_state_.size())) {
        // The game has jumped those frames ahead for real, which is a moment's
        // skip and nothing worse.
        run_ahead_ = 0;
        run_ahead_state_.clear();
        log_warn("this core could not load back the state it ran ahead from; run-ahead is off");
        return;
    }
    run_ahead_spent_ms_ += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    if (++run_ahead_frames_timed_ == 300) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "run-ahead is costing %.1fms a frame for %u frame%s ahead, of %.1fms "
                      "the game has for each",
                      run_ahead_spent_ms_ / run_ahead_frames_timed_, run_ahead_,
                      run_ahead_ == 1 ? "" : "s", 1000.0 / av_info_.fps);
        log_info(text);
    }
}

void LibretroCore::run_once() {
    if (hw_render_requested_) {
        // Catches a core that drew larger than it had announced: the frame that
        // overflowed is already gone, but the next one fits.
        ensure_hw_target();
        // Everything the core is about to do belongs to the core. Draining here
        // means the check after retro_run reports only what the core caused.
        gl::take_error();

        // The context goes back to OpenGL's defaults before the core touches
        // it. A core tracks GL state in a shadow object that starts at those
        // defaults and only issues the calls where its belief differs -- so
        // anything this app leaves switched on is invisible to the core, and it
        // will not switch it off. This app's own alpha blending, left enabled,
        // silently swallows everything a core draws that has no alpha of its
        // own, which is exactly what a 3DS screen is.
        gl::reset_state_for_core();

        // On the first frame only, stain the buffer a colour nothing else uses.
        // Afterwards it reads back as one of exactly two things, and they mean
        // opposite things: still stained means the core never wrote to the
        // buffer it was given, and black means the core cleared it and then
        // drew nothing. One needs the handover fixed, the other the core's
        // rendering, and there is otherwise no way to tell them apart.
        gl_trace::begin_frame();
        if (!hw_stain_done_) {
            hw_stain_done_ = true;
            gl::BindFramebuffer(GL_FRAMEBUFFER, hw_render_.framebuffer());
            gl::ClearColor(1.0f, 0.0f, 1.0f, 1.0f);  // magenta
            gl::Clear(GL_COLOR_BUFFER_BIT);
            log_info("stained the core's framebuffer magenta before its first frame");
        }
        // The core draws into its own framebuffer, not the window. Binding it
        // here rather than trusting the core to means a core that leaves the
        // binding somewhere else cannot scribble over the window.
        gl::BindFramebuffer(GL_FRAMEBUFFER, hw_render_.framebuffer());
        api_->retro_run();
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    api_->retro_run();
}

void LibretroCore::reset() {
    if (handle_ == nullptr || !game_loaded_) {
        return;
    }
    api_->retro_reset();
}

void LibretroCore::unload_game() {
    if (handle_ != nullptr && game_loaded_) {
        api_->retro_unload_game();
    }
    // After retro_unload_game, so the core still has its context while it is
    // shutting the game down.
    destroy_hw_context();
    game_loaded_ = false;
    rom_data_.clear();
    run_ahead_state_.clear();
    video_frame_ = CoreVideoFrame();
    video_buffer_.clear();
}

void LibretroCore::unload() {
    unload_game();
    if (handle_ != nullptr) {
        if (api_->retro_deinit != nullptr) {
            api_->retro_deinit();
        }
        dlclose(handle_);
        handle_ = nullptr;
    }
    if (g_active_core == this) {
        g_active_core = nullptr;
    }
}

// --- core options -----------------------------------------------------------

void LibretroCore::register_option(const CoreOptionInfo& info) {
    if (info.key.empty()) {
        return;
    }
    declared_options_[info.key] = info;
    // The core's default only applies if we have not already been told otherwise.
    if (option_values_.find(info.key) == option_values_.end()) {
        option_values_[info.key] = info.default_value;
    }
    options_registered_ = true;
    // What this core offers around the microphone, and exactly how it spells
    // it. melonDS can synthesise the blow a DS game is listening for, which is
    // the whole feature -- but the option key and the value differ between
    // core versions, and a forced option that guesses wrong is silently
    // ignored unless it is Required. Saying it here means one session's log
    // settles it instead of a guess.
    for (const auto& entry : declared_options_) {
        if (entry.first.find("mic") == std::string::npos) {
            continue;
        }
        std::string values;
        for (const std::string& value : entry.second.allowed_values) {
            if (!values.empty()) values += ", ";
            values += value;
        }
        log_info("core option '" + entry.first + "' offers: " +
                 (values.empty() ? std::string("anything") : values));
    }
}

void LibretroCore::apply_forced_options() {
    const auto join = [](const std::vector<std::string>& values, const char* separator,
                         bool quoted) {
        std::string joined;
        for (const std::string& value : values) {
            if (!joined.empty()) joined += separator;
            joined += quoted ? "'" + value + "'" : value;
        }
        return joined;
    };

    option_warnings_.clear();
    for (const auto& entry : forced_options_) {
        const std::string& key = entry.first;
        const ForcedOption& forced = entry.second;
        // Until the core has said what it offers there is nothing to choose
        // between, so the first spelling stands in.
        option_values_[key] = forced.values.front();

        if (!options_registered_) {
            continue;  // nothing to validate against yet
        }
        const auto declared = declared_options_.find(key);
        if (declared == declared_options_.end()) {
            if (forced.requirement == OptionRequirement::Required) {
                option_warnings_.push_back("this core does not have the option '" + key + "'");
            }
            continue;
        }
        const std::vector<std::string>& allowed = declared->second.allowed_values;
        if (allowed.empty()) {
            continue;  // free-form; whatever we asked for is what it gets
        }
        const auto chosen = std::find_first_of(forced.values.begin(), forced.values.end(),
                                               allowed.begin(), allowed.end());
        if (chosen != forced.values.end()) {
            option_values_[key] = *chosen;
            continue;
        }
        option_warnings_.push_back("core option '" + key + "' does not accept " +
                                   join(forced.values, " or ", true) + " (it offers: " +
                                   join(allowed, ", ", false) + ")");
    }
    variables_dirty_ = true;
    // A core can declare its options more than once (a v1 table after a v2 one,
    // say), and each declaration revalidates. Logging only what changed keeps
    // one real problem from appearing several times in the log.
    if (option_warnings_ != logged_option_warnings_) {
        for (const std::string& warning : option_warnings_) {
            log_warn(warning);
        }
        logged_option_warnings_ = option_warnings_;
    }
}

// --- environment ------------------------------------------------------------

bool LibretroCore::on_environment(unsigned command, void* data) {
    switch (command) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            *static_cast<bool*>(data) = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_OVERSCAN: {
            *static_cast<bool*>(data) = false;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto format = *static_cast<const enum retro_pixel_format*>(data);
            // The compositor's texture upload path assumes 32-bit XRGB8888.
            // melonDS DS uses it natively; anything else we decline so the core
            // falls back rather than silently producing wrong colours.
            if (format == RETRO_PIXEL_FORMAT_XRGB8888) {
                return true;
            }
            log_warn("core requested an unsupported pixel format (" +
                     std::to_string(static_cast<int>(format)) + "); only XRGB8888 is supported");
            return false;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            *static_cast<const char**>(data) =
                system_directory_.empty() ? nullptr : system_directory_.c_str();
            return !system_directory_.empty();
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            *static_cast<const char**>(data) =
                save_directory_.empty() ? nullptr : save_directory_.c_str();
            return !save_directory_.empty();
        }
        // GET_CONTENT_DIRECTORY is the deprecated alias of this same value.
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: {
            *static_cast<const char**>(data) =
                system_directory_.empty() ? nullptr : system_directory_.c_str();
            return !system_directory_.empty();
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            static_cast<struct retro_log_callback*>(data)->log = &core_log_printf;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* variable = static_cast<struct retro_variable*>(data);
            if (variable == nullptr || variable->key == nullptr) {
                return false;
            }
            const auto it = option_values_.find(variable->key);
            if (it == option_values_.end() || it->second.empty()) {
                variable->value = nullptr;
                return false;
            }
            // The string has to outlive this call; option_values_ owns it and
            // is only rewritten through force_option().
            variable->value = it->second.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            *static_cast<bool*>(data) = variables_dirty_;
            variables_dirty_ = false;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLE: {
            const auto* variable = static_cast<const struct retro_variable*>(data);
            if (variable == nullptr || variable->key == nullptr) {
                return true;  // a null pointer here is just a capability probe
            }
            if (variable->value != nullptr) {
                option_values_[variable->key] = variable->value;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const auto* variables = static_cast<const struct retro_variable*>(data);
            for (; variables != nullptr && variables->key != nullptr; ++variables) {
                CoreOptionInfo info;
                info.key = variables->key;
                // Legacy format: "Human readable description; first|second|third"
                const std::string blob = variables->value != nullptr ? variables->value : "";
                const std::size_t separator = blob.find(';');
                if (separator == std::string::npos) {
                    info.description = blob;
                } else {
                    info.description = blob.substr(0, separator);
                    std::string values = blob.substr(separator + 1);
                    const std::size_t first = values.find_first_not_of(" \t");
                    if (first != std::string::npos) {
                        values = values.substr(first);
                    }
                    info.allowed_values = split_values(values, '|');
                    if (!info.allowed_values.empty()) {
                        info.default_value = info.allowed_values.front();
                    }
                }
                register_option(info);
            }
            apply_forced_options();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: {
            *static_cast<unsigned*>(data) = 2;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            const auto* definitions = static_cast<const struct retro_core_option_definition*>(data);
            for (; definitions != nullptr && definitions->key != nullptr; ++definitions) {
                CoreOptionInfo info;
                info.key = definitions->key;
                info.description = definitions->desc != nullptr ? definitions->desc : "";
                info.default_value = definitions->default_value != nullptr ? definitions->default_value : "";
                for (const auto& value : definitions->values) {
                    if (value.value == nullptr) break;
                    info.allowed_values.push_back(value.value);
                }
                register_option(info);
            }
            apply_forced_options();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            const auto* intl = static_cast<const struct retro_core_options_intl*>(data);
            if (intl == nullptr) return false;
            // We only speak English; the "us" table is always the complete one.
            return on_environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS,
                                  const_cast<void*>(static_cast<const void*>(intl->us)));
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
            const auto* options = static_cast<const struct retro_core_options_v2*>(data);
            if (options == nullptr) return false;
            const struct retro_core_option_v2_definition* definitions = options->definitions;
            for (; definitions != nullptr && definitions->key != nullptr; ++definitions) {
                CoreOptionInfo info;
                info.key = definitions->key;
                info.description = definitions->desc != nullptr ? definitions->desc : "";
                info.default_value = definitions->default_value != nullptr ? definitions->default_value : "";
                for (const auto& value : definitions->values) {
                    if (value.value == nullptr) break;
                    info.allowed_values.push_back(value.value);
                }
                register_option(info);
            }
            apply_forced_options();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const auto* intl = static_cast<const struct retro_core_options_v2_intl*>(data);
            if (intl == nullptr) return false;
            return on_environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
                                  const_cast<void*>(static_cast<const void*>(intl->us)));
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
            // We do not surface the core's own options UI, so visibility hints
            // are accepted and ignored.
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
            const auto* info = static_cast<const struct retro_system_av_info*>(data);
            if (info == nullptr) return false;
            av_info_.base_width = info->geometry.base_width;
            av_info_.base_height = info->geometry.base_height;
            av_info_.max_width = info->geometry.max_width;
            av_info_.max_height = info->geometry.max_height;
            av_info_.aspect_ratio = info->geometry.aspect_ratio;
            if (info->timing.fps > 0.0) av_info_.fps = info->timing.fps;
            if (info->timing.sample_rate > 0.0) av_info_.sample_rate = info->timing.sample_rate;
            // A GPU-rendering core resizes its output when its resolution
            // multiplier changes, and then draws that much on the very next
            // frame -- so the framebuffer has to grow now, not later.
            if (hw_render_requested_) ensure_hw_target();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const auto* geometry = static_cast<const struct retro_game_geometry*>(data);
            if (geometry == nullptr) return false;
            av_info_.base_width = geometry->base_width;
            av_info_.base_height = geometry->base_height;
            if (geometry->max_width > 0) av_info_.max_width = geometry->max_width;
            if (geometry->max_height > 0) av_info_.max_height = geometry->max_height;
            av_info_.aspect_ratio = geometry->aspect_ratio;
            if (hw_render_requested_) ensure_hw_target();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
            supports_input_bitmasks_ = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS: {
            *static_cast<unsigned*>(data) = 1;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LANGUAGE: {
            *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
            *static_cast<int*>(data) = 0x3;  // audio + video both enabled
            return true;
        }
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING: {
            *static_cast<bool*>(data) = false;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE: {
            *static_cast<float*>(data) = 60.0f;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_JIT_CAPABLE: {
            *static_cast<bool*>(data) = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
            // Answer with what this window can actually service. A core that
            // asks is deciding which API to request next, so saying "none" to a
            // core with no software renderer just makes it fail later.
            *static_cast<unsigned*>(data) =
                gl::is_gles() ? RETRO_HW_CONTEXT_OPENGLES2
                              : (gl::at_least(3, 3) ? RETRO_HW_CONTEXT_OPENGL_CORE
                                                    : RETRO_HW_CONTEXT_OPENGL);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            return accept_hw_render(static_cast<struct retro_hw_render_callback*>(data));
        }
        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT: {
            // "Give me a context of my own rather than yours." This frontend
            // runs the core on the same context it draws with, which is what
            // the shared-context contract allows; accepting it is what lets the
            // core go on to ask for the context itself.
            return true;
        }
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
            // What a core's own on-screen keyboard (a 3DS's swkbd, chiefly)
            // actually reads from. Without this registered, the core has no
            // way to receive what is typed and falls back to a default the
            // moment it opens -- keys are forwarded to it from the SDL event
            // loop in send_key_event() below.
            const auto* callback = static_cast<const struct retro_keyboard_callback*>(data);
            keyboard_callback_ = callback != nullptr ? callback->callback : nullptr;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE: {
            // How a 3DS core is handed the gyroscope and accelerometer of
            // whatever device is actually being tilted -- which for a session
            // played across two devices is the companion holding the bottom
            // screen, not the machine running the core. See
            // DsInputState::motion, and app.cpp where a companion's reading
            // wins over the host's own.
            auto* sensors = static_cast<struct retro_sensor_interface*>(data);
            if (sensors == nullptr) {
                return false;
            }
            sensors->set_sensor_state = &set_sensor_state_thunk;
            sensors->get_sensor_input = &sensor_input_thunk;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const auto* message = static_cast<const struct retro_message*>(data);
            if (message != nullptr && message->msg != nullptr) {
                core_messages_.emplace_back(message->msg);
                if (core_messages_.back() != last_logged_core_message_) {
                    last_logged_core_message_ = core_messages_.back();
                    log_info("core message: " + last_logged_core_message_);
                }
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: {
            *static_cast<unsigned*>(data) = 1;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            const auto* message = static_cast<const struct retro_message_ext*>(data);
            if (message != nullptr && message->msg != nullptr) {
                core_messages_.emplace_back(message->msg);
                if (core_messages_.back() != last_logged_core_message_) {
                    last_logged_core_message_ = core_messages_.back();
                    log_message(translate_log_level(message->level),
                                "core message: " + last_logged_core_message_);
                }
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SHUTDOWN: {
            wants_shutdown_ = true;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
            // Read rather than merely accepted, because this is the only place
            // a core says what it does with a button beyond the obvious ones.
            // melonDS puts the DS microphone on a pad button and names it
            // here; without this the name is in RetroArch's binding screen and
            // nowhere else, and which button it is has to be guessed.
            const auto* descriptor = static_cast<const struct retro_input_descriptor*>(data);
            for (; descriptor != nullptr && descriptor->description != nullptr; ++descriptor) {
                const std::string description = descriptor->description;
                std::string lowered;
                lowered.reserve(description.size());
                for (const char character : description) {
                    lowered.push_back(static_cast<char>(
                        std::tolower(static_cast<unsigned char>(character))));
                }
                const bool interesting = lowered.find("mic") != std::string::npos ||
                                         lowered.find("blow") != std::string::npos;
                const std::string line =
                    "core input: port " + std::to_string(descriptor->port) + " id " +
                    std::to_string(descriptor->id) + " is \"" + description + "\"";
                if (interesting) {
                    log_info(line);
                } else {
                    log_debug(line);
                }
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
            // Accepted but not acted on in V1.
            return true;
        }
        default:
            break;
    }

    // Anything we do not implement is declined, which every well-behaved core
    // treats as "the frontend cannot do that" and works around.
    log_debug("unhandled libretro environment command " + std::to_string(command));
    return false;
}

// --- video / audio / input --------------------------------------------------

void LibretroCore::on_video_refresh(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (hide_video_) {
        // A frame run to get somewhere, not to be looked at -- see
        // set_run_ahead(). A GPU core has drawn it into the framebuffer
        // regardless, and the frame that is shown draws over it.
        return;
    }
    if (data == nullptr) {
        // Frame duplication: keep showing the previous frame.
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }

    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        // Nothing to copy: the picture is already in the texture we handed the
        // core. All this frame carries is how much of it is used.
        if (!hw_render_.valid()) return;
        if (width > hw_render_.width() || height > hw_render_.height()) {
            // Undeclared and too big: remember it so the next frame has room,
            // and say so, because this frame is being cropped short.
            minimum_hw_width_ = width;
            minimum_hw_height_ = height;
            log_warn("the core drew a " + std::to_string(width) + "x" + std::to_string(height) +
                     " picture into a smaller framebuffer than it asked for; growing it");
            return;
        }
        video_frame_.source = FrameSource::Texture;
        video_frame_.pixels = nullptr;
        video_frame_.texture = hw_render_.texture();
        video_frame_.framebuffer = hw_render_.framebuffer();
        video_frame_.texture_width = hw_render_.width();
        video_frame_.texture_height = hw_render_.height();
        video_frame_.bottom_left_origin =
            hw_callback_ != nullptr && hw_callback_->bottom_left_origin;
        video_frame_.width = width;
        video_frame_.height = height;
        video_frame_.stride_pixels = 0;
        video_frame_.serial = ++video_serial_;
        // Once, and again whenever the size changes. Without this there is no
        // way to tell a core that is not presenting from one presenting
        // something the frontend then mishandles.
        if (width != reported_frame_width_ || height != reported_frame_height_) {
            reported_frame_width_ = width;
            reported_frame_height_ = height;
            log_info("the core presented a " + std::to_string(width) + "x" +
                     std::to_string(height) + " picture on the GPU, in a " +
                     std::to_string(hw_render_.width()) + "x" +
                     std::to_string(hw_render_.height()) + " framebuffer, origin " +
                     (video_frame_.bottom_left_origin ? "bottom-left" : "top-left"));
        }
        return;
    }

    const std::size_t needed = static_cast<std::size_t>(width) * height;
    if (video_buffer_.size() < needed) {
        video_buffer_.resize(needed);
    }

    const auto* source = static_cast<const uint8_t*>(data);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(uint32_t);
    for (unsigned row = 0; row < height; ++row) {
        std::memcpy(video_buffer_.data() + static_cast<std::size_t>(row) * width,
                    source + static_cast<std::size_t>(row) * pitch,
                    row_bytes);
    }

    video_frame_.source = FrameSource::Pixels;
    video_frame_.texture = 0;
    video_frame_.framebuffer = 0;
    video_frame_.pixels = video_buffer_.data();
    video_frame_.width = width;
    video_frame_.height = height;
    video_frame_.stride_pixels = width;
    video_frame_.serial = ++video_serial_;
}

int16_t LibretroCore::on_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port != 0 || input_state_ == nullptr) {
        return 0;
    }

    if (device == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t mask = 0;
            for (unsigned button = 0; button < static_cast<unsigned>(DsButton::Count); ++button) {
                if (input_state_->pressed(static_cast<DsButton>(button))) {
                    mask |= static_cast<int16_t>(1 << retro_id_for(static_cast<DsButton>(button)));
                }
            }
            return mask;
        }
        for (unsigned button = 0; button < static_cast<unsigned>(DsButton::Count); ++button) {
            const DsButton ds_button = static_cast<DsButton>(button);
            if (retro_id_for(ds_button) == id) {
                return input_state_->pressed(ds_button) ? 1 : 0;
            }
        }
        return 0;
    }

    if (device == RETRO_DEVICE_ANALOG) {
        // The left stick is the circle pad; the right one is Citra's C-Stick,
        // which stands in for a Circle Pad Pro or New 3DS -- games built for
        // neither simply never read it.
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return input_state_->circle_x;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return input_state_->circle_y;
        } else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return input_state_->c_stick_x;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return input_state_->c_stick_y;
        }
        return 0;
    }

    if (device == RETRO_DEVICE_POINTER) {
        if (id == RETRO_DEVICE_ID_POINTER_COUNT) {
            return input_state_->touching ? 1 : 0;
        }
        if (index != 0) {
            return 0;
        }
        if (id == RETRO_DEVICE_ID_POINTER_PRESSED) {
            return input_state_->touching ? 1 : 0;
        }
        if (id == RETRO_DEVICE_ID_POINTER_X || id == RETRO_DEVICE_ID_POINTER_Y) {
            if (!input_state_->touching) {
                return 0;
            }
            // Pointer coordinates are expressed across the core's whole output
            // framebuffer, so the DS touch point has to be placed back where
            // the core's own internal layout puts the bottom screen. The
            // ScreenSource that owns that layout hands us the offsets.
            return pointer_axis(id == RETRO_DEVICE_ID_POINTER_X);
        }
        return 0;
    }

    return 0;
}

bool LibretroCore::on_set_sensor_state(unsigned port, unsigned action, unsigned rate) {
    // One pad, one set of sensors: there is nothing on any other port to turn
    // on. `rate` is a hint about how often the core would like to sample, and
    // there is nothing here that can honour it -- readings arrive when a frame
    // of input does -- so it is accepted and ignored rather than refused.
    (void)rate;
    if (port != 0) {
        return false;
    }
    switch (action) {
        case RETRO_SENSOR_ACCELEROMETER_ENABLE: wants_accelerometer_ = true; return true;
        case RETRO_SENSOR_ACCELEROMETER_DISABLE: wants_accelerometer_ = false; return true;
        case RETRO_SENSOR_GYROSCOPE_ENABLE: wants_gyroscope_ = true; return true;
        case RETRO_SENSOR_GYROSCOPE_DISABLE: wants_gyroscope_ = false; return true;
        default:
            // Illuminance, and anything added later. Refused rather than
            // silently accepted, so a core can tell it will get nothing.
            return false;
    }
}

float LibretroCore::on_sensor_input(unsigned port, unsigned id) const {
    if (port != 0 || input_state_ == nullptr || !input_state_->motion.present) {
        return 0.0f;
    }
    const DsMotionState& motion = input_state_->motion;
    // An earlier version of this said the two conventions already agreed and
    // passed the axes straight through, changing only the unit. They do not
    // agree, and a 3DS game steered the wrong way for it.
    //
    // libretro's sensor interface never says what its axes mean, so what they
    // mean in practice is whatever the core reading them assumes -- and here
    // that core is Azahar, which feeds these values directly into the 3DS's
    // own motion device with no conversion of its own. So the frame to hand
    // over is the 3DS's, not SDL's.
    //
    // Which is a claim worth sourcing rather than deriving. Azahar reads the
    // same 3DS motion device from SDL as well, and that path is the one people
    // actually use, so it is the definition of the conversion: in its
    // SDLState::HandleGameControllerEvent it stores acceleration as
    // (x, -y, z) in g and rotation as (-x, y, -z), converting radians to
    // degrees on the way. That is what the two lists below reproduce -- the
    // same axis order, acceleration's second component negated and rotation's
    // first and third, which is the acceleration transform's own negative and
    // the same relationship Cemu's two readers have (see parse_dsu_motion).
    //
    // The units are what libretro does document: g for acceleration, radians
    // a second for rotation. DsMotionState carries metres per second squared
    // and radians a second, so only the first needs converting.
    constexpr float kGravity = 9.80665f;
    switch (id) {
        case RETRO_SENSOR_ACCELEROMETER_X:
            return wants_accelerometer_ ? motion.accel_x / kGravity : 0.0f;
        case RETRO_SENSOR_ACCELEROMETER_Y:
            return wants_accelerometer_ ? -motion.accel_y / kGravity : 0.0f;
        case RETRO_SENSOR_ACCELEROMETER_Z:
            return wants_accelerometer_ ? motion.accel_z / kGravity : 0.0f;
        case RETRO_SENSOR_GYROSCOPE_X: return wants_gyroscope_ ? -motion.gyro_x : 0.0f;
        case RETRO_SENSOR_GYROSCOPE_Y: return wants_gyroscope_ ? motion.gyro_y : 0.0f;
        case RETRO_SENSOR_GYROSCOPE_Z: return wants_gyroscope_ ? -motion.gyro_z : 0.0f;
        default: return 0.0f;
    }
}

// --- static thunks ----------------------------------------------------------

bool LibretroCore::environment_thunk(unsigned command, void* data) {
    if (g_active_core == nullptr) return false;
    return g_active_core->on_environment(command, data);
}

void LibretroCore::video_refresh_thunk(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (g_active_core == nullptr) return;
    g_active_core->on_video_refresh(data, width, height, pitch);
}

void LibretroCore::audio_sample_thunk(int16_t left, int16_t right) {
    if (g_active_core == nullptr || !g_active_core->audio_callback_ ||
        g_active_core->hide_audio_) {
        return;
    }
    const int16_t frame[2] = {left, right};
    g_active_core->audio_callback_(frame, 1);
}

std::size_t LibretroCore::audio_sample_batch_thunk(const int16_t* data, std::size_t frames) {
    if (g_active_core != nullptr && g_active_core->audio_callback_ && data != nullptr &&
        !g_active_core->hide_audio_) {
        g_active_core->audio_callback_(data, frames);
    }
    return frames;
}

void LibretroCore::input_poll_thunk() {
    // Input is polled by the app's own event loop before retro_run(); the
    // latched state is already current by the time the core asks for it.
}

int16_t LibretroCore::input_state_thunk(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (g_active_core == nullptr) return 0;
    return g_active_core->on_input_state(port, device, index, id);
}

}  // namespace ds
