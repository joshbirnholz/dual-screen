#include "core/libretro_screen_source.hpp"

#include <algorithm>
#include <cstring>

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
// How many pixels Citra renders per 3DS pixel. Its values are spelled "2x",
// "3x" and so on, with the first one written out in full.
constexpr const char* kOpt3dsResolution = "citra_resolution_factor";

std::string citra_resolution_value(int scale) {
    return scale <= 1 ? std::string("1x (Native)") : std::to_string(scale) + "x";
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
    warnings_.clear();

    system_ = config.system;
    const SystemProfile& profile = system_profile(system_);
    render_scale_ = std::clamp(config.render_scale, 1, profile.max_render_scale);

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
    // After the layout, because it multiplies whatever that layout produces.
    apply_render_scale();

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
    core_->run_frame();
    // Checked here so the frontend's own check, at the top of the next frame's
    // drawing, is not blamed for something the core did. An emulator leaving an
    // error behind is its own business; the same error attributed to us would
    // send the next person hunting in the wrong file.
    gl::log_error_once("the core left an OpenGL error behind");
    probe_window_framebuffer();

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

void LibretroCoreScreenSource::apply_render_scale() {
    if (!core_) {
        return;
    }
    switch (system_) {
        case SystemId::Ds:
            // melonDS is pinned to its software renderer, which draws one pixel
            // per DS pixel and has nothing to multiply.
            break;
        case SystemId::ThreeDs:
            core_->force_option(kOpt3dsResolution, citra_resolution_value(render_scale_));
            break;
    }
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
    // GL_RGBA rather than a BGRA read: every driver takes this one, and the
    // swizzle below costs a fraction of what compressing the frame costs.
    gl::ReadPixels(0, 0, target_width, target_height, GL_RGBA, GL_UNSIGNED_BYTE,
                   readback.pixels.data());
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // What came back is R,G,B,A in memory; what the encoder reads is XRGB8888.
    //
    // The rows need no flipping. glReadPixels starts at the framebuffer's
    // bottom-left, which for these textures is the screen's top row: the crop
    // that filled this texture already turned the core's bottom-left origin
    // into the top-row-first order every texture here is uploaded in.
    for (uint32_t& pixel : readback.pixels) {
        pixel = ((pixel & 0x000000ffu) << 16) | (pixel & 0x0000ff00u) |
                ((pixel & 0x00ff0000u) >> 16);
    }

    readback.width = target_width;
    readback.height = target_height;
    readback.serial = frame.serial;
    readback.filled = true;
    gl::take_error();

    out.pixels = readback.pixels.data();
    out.width = readback.width;
    out.height = readback.height;
    out.stride_pixels = readback.width;
    return true;
}

void LibretroCoreScreenSource::upload_screens() {
    const CoreVideoFrame& frame = core_->video_frame();
    if (!frame.valid() || !split_.valid || frame.serial == uploaded_serial_) {
        // A picture that never arrives looks exactly like one that arrives and
        // is drawn wrong, so say which it is rather than leaving a black screen
        // to be interpreted.
        if (!first_gpu_crop_reported_ && !silence_reported_ && ++quiet_frames_ > 120) {
            silence_reported_ = true;
            log_warn(std::string("120 frames in and nothing has been drawn: ") +
                     (!frame.valid() ? "the core has not presented a picture"
                                     : (!split_.valid ? "the frame could not be split"
                                                      : "the same frame keeps arriving")));
        }
        return;
    }
    quiet_frames_ = 0;

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
