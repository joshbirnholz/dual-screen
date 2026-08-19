#include "core/libretro_screen_source.hpp"

#include "config/paths.hpp"
#include "util/log.hpp"

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

}  // namespace

ScreenSplit compute_top_bottom_split(int frame_width, int frame_height, std::string* reason) {
    ScreenSplit split;
    if (frame_width <= 0 || frame_height <= 0) {
        if (reason != nullptr) *reason = "the core produced an empty frame";
        return split;
    }
    // One DS screen wide, two tall: height must be exactly 1.5x the width.
    if (frame_height % 2 != 0 || frame_height * 2 != frame_width * 3) {
        if (reason != nullptr) {
            *reason = "expected a framebuffer two DS screens tall (a 2:3 width:height ratio) but the "
                      "core produced " + std::to_string(frame_width) + "x" + std::to_string(frame_height);
        }
        return split;
    }

    split.frame_width = frame_width;
    split.frame_height = frame_height;
    split.screen_width = frame_width;
    split.screen_height = frame_height / 2;
    split.top_x = 0;
    split.top_y = 0;
    split.bottom_x = 0;
    split.bottom_y = split.screen_height;
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

    if (config.core_path.empty()) {
        if (error != nullptr) {
            *error = "No melonDS DS libretro core was found. Install melondsds_libretro.so, or set "
                     "\"core_path\" in settings.json.";
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
    // land as clean, axis-aligned halves of a single buffer. Everything the
    // player sees is composited by us from those halves, so this value is an
    // implementation detail they never interact with.
    core_->force_option(kOptNumberOfLayouts, "1");   // no layout cycling
    core_->force_option(kOptScreenLayout1, "top-bottom");
    core_->force_option(kOptScreenGap, "0");         // no dead rows between screens
    core_->force_option(kOptRenderMode, "software"); // V1 composites from a CPU framebuffer
    core_->force_option(kOptTouchMode, "touch");     // touch arrives as a libretro pointer
    core_->force_option(kOptShowCursor, "disabled"); // we draw our own UI

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
    split_ = compute_top_bottom_split(static_cast<int>(core_->av_info().base_width),
                                      static_cast<int>(core_->av_info().base_height),
                                      &reason);
    if (split_.valid) {
        core_->set_pointer_source_rect(
            static_cast<float>(split_.bottom_x) / static_cast<float>(split_.frame_width),
            static_cast<float>(split_.bottom_y) / static_cast<float>(split_.frame_height),
            static_cast<float>(split_.screen_width) / static_cast<float>(split_.frame_width),
            static_cast<float>(split_.screen_height) / static_cast<float>(split_.frame_height));
    }

    log_info("core geometry " + std::to_string(core_->av_info().base_width) + "x" +
             std::to_string(core_->av_info().base_height) + " at " +
             std::to_string(core_->av_info().fps) + " fps, " +
             std::to_string(core_->av_info().sample_rate) + " Hz audio");
    return true;
}

void LibretroCoreScreenSource::close() {
    if (core_) {
        core_->unload();
    }
    top_texture_.destroy();
    bottom_texture_.destroy();
    screens_ = ScreenTextures();
    uploaded_serial_ = 0;
    split_ = ScreenSplit();
    split_reported_ = false;
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
            compute_top_bottom_split(static_cast<int>(core_->video_frame().width),
                                     static_cast<int>(core_->video_frame().height), &reason);
            *error = "The core is not rendering in the screen layout this app pins it to: " + reason +
                     ". Check that the loaded core accepts the 'top-bottom' screen layout.";
        }
        return false;
    }
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
    const ScreenSplit split = compute_top_bottom_split(static_cast<int>(frame.width),
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
    core_->set_pointer_source_rect(
        static_cast<float>(split_.bottom_x) / static_cast<float>(split_.frame_width),
        static_cast<float>(split_.bottom_y) / static_cast<float>(split_.frame_height),
        static_cast<float>(split_.screen_width) / static_cast<float>(split_.frame_width),
        static_cast<float>(split_.screen_height) / static_cast<float>(split_.frame_height));
    log_info("splitting " + std::to_string(split_.frame_width) + "x" + std::to_string(split_.frame_height) +
             " into two " + std::to_string(split_.screen_width) + "x" +
             std::to_string(split_.screen_height) + " screens");
}

void LibretroCoreScreenSource::upload_screens() {
    const CoreVideoFrame& frame = core_->video_frame();
    if (!frame.valid() || !split_.valid || frame.serial == uploaded_serial_) {
        return;
    }

    const uint32_t* base = frame.pixels;
    const int stride = static_cast<int>(frame.stride_pixels);

    const uint32_t* top = base + static_cast<std::size_t>(split_.top_y) * stride + split_.top_x;
    top_texture_.upload_xrgb8888(top, split_.screen_width, split_.screen_height, stride);

    const uint32_t* bottom = base + static_cast<std::size_t>(split_.bottom_y) * stride + split_.bottom_x;
    bottom_texture_.upload_xrgb8888(bottom, split_.screen_width, split_.screen_height, stride);

    uploaded_serial_ = frame.serial;
}

const ScreenTextures& LibretroCoreScreenSource::screens() {
    upload_screens();
    screens_.top = top_texture_.valid() ? &top_texture_ : nullptr;
    screens_.bottom = bottom_texture_.valid() ? &bottom_texture_ : nullptr;
    return screens_;
}

ScreenSize LibretroCoreScreenSource::native_screen_size() const {
    if (split_.valid) {
        return ScreenSize{split_.screen_width, split_.screen_height};
    }
    return ScreenSize{kDsScreenWidth, kDsScreenHeight};
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
