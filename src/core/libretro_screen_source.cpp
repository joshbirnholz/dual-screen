#include "core/libretro_screen_source.hpp"

#include <algorithm>
#include <cstring>

#include "config/paths.hpp"
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

// Citra's equivalent. Its "Default Top-Bottom Screen" stacks the 400x240 upper
// screen above the 320x240 touch screen, the narrower one centred beneath the
// wider, in a buffer 400 wide and 480 tall at 1x.
constexpr const char* kOpt3dsLayout = "citra_layout_option";
constexpr const char* kOpt3dsLayoutValue = "Default Top-Bottom Screen";
// "Top" means the top screen is the prominent one, which is Citra's way of
// saying "not swapped" -- it has no disabled/enabled here.
constexpr const char* kOpt3dsSwapScreens = "citra_swap_screen";
constexpr const char* kOpt3dsSwapScreensValue = "Top";
// Citra ignores the libretro pointer entirely unless this is on: with it off,
// touches reach the core and are dropped, and the touch screen does nothing.
constexpr const char* kOpt3dsTouch = "citra_touch_touchscreen";
// Its own on-screen pointer, which would sit on top of the one we draw.
constexpr const char* kOpt3dsRenderTouch = "citra_render_touchscreen";

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
    warnings_.clear();

    system_ = config.system;
    const SystemProfile& profile = system_profile(system_);

    if (config.core_path.empty()) {
        if (error != nullptr) {
            *error = "No " + profile.core_description + " libretro core was found. Install " +
                     profile.core_filenames.front() + ", or set \"core_path\" in settings.json.";
        }
        return false;
    }
    if (!file_exists(config.core_path)) {
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
        case SystemId::Ds:
            core_->force_option(kOptNumberOfLayouts, "1");   // no layout cycling
            core_->force_option(kOptScreenLayout1, "top-bottom");
            core_->force_option(kOptScreenGap, "0");         // no dead rows between screens
            core_->force_option(kOptRenderMode, "software"); // composited from a CPU framebuffer
            core_->force_option(kOptTouchMode, "touch");     // touch arrives as a libretro pointer
            core_->force_option(kOptShowCursor, "disabled"); // we draw our own UI
            break;
        case SystemId::ThreeDs:
            core_->force_option(kOpt3dsLayout, kOpt3dsLayoutValue);
            core_->force_option(kOpt3dsSwapScreens, kOpt3dsSwapScreensValue);
            core_->force_option(kOpt3dsTouch, "enabled");        // touch reaches the core
            core_->force_option(kOpt3dsRenderTouch, "disabled"); // we draw our own pointer
            break;
    }

    if (!core_->load(config.core_path, error)) {
        return false;
    }
    core_description_ = core_->core_name() + " " + core_->core_version();

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
    core_->run_frame();

    for (const std::string& message : core_->core_messages()) {
        warnings_.push_back(message);
    }
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

bool LibretroCoreScreenSource::blit_screen(const CoreVideoFrame& frame,
                                           Texture& destination,
                                           int source_x,
                                           int source_y,
                                           int width,
                                           int height) {
    destination.allocate_rgba(width, height);
    if (!destination.valid()) return false;

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
    gl::BlitFramebuffer(source_x, y0, source_x + width, y1, 0, 0, width, height,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    return true;
}

void LibretroCoreScreenSource::upload_screens() {
    const CoreVideoFrame& frame = core_->video_frame();
    if (!frame.valid() || !split_.valid || frame.serial == uploaded_serial_) {
        return;
    }

    if (frame.source == FrameSource::Texture) {
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

bool LibretroCoreScreenSource::wants_shutdown() const {
    return core_ && core_->wants_shutdown();
}

double LibretroCoreScreenSource::frames_per_second() const {
    return core_ ? core_->av_info().fps : 60.0;
}

double LibretroCoreScreenSource::audio_sample_rate() const {
    return core_ ? core_->av_info().sample_rate : 48000.0;
}

}  // namespace ds
