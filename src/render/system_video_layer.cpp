#include "render/system_video_layer.hpp"

#include <cstdlib>
#include <string>

#include "util/log.hpp"

#if defined(__APPLE__) || defined(DS_TYPECHECK_APPLE)
#include <TargetConditionals.h>
#endif

#if (defined(__APPLE__) || defined(DS_TYPECHECK_APPLE)) && TARGET_OS_IPHONE
#define DS_HAVE_SYSTEM_VIDEO_LAYER 1
#else
#define DS_HAVE_SYSTEM_VIDEO_LAYER 0
#endif

#if DS_HAVE_SYSTEM_VIDEO_LAYER
// apple/system_video_layer.m. Everything is on the main thread, where SDL runs
// the app on these platforms.
extern "C" {
void* ds_video_view_for_window(SDL_Window* window);
void* ds_video_layer_create(void* view);
void ds_video_layer_destroy(void* layer);
// 0 shown; 1 not shown this time but worth trying again; -1 failed, with the
// reason written to `reason` (at most `reason_size` bytes, terminated).
int ds_video_layer_show(void* layer, void* picture, int is_new, float x, float y, float width,
                        float height, int quarter_turns, int smooth, float output_width,
                        float output_height, char* reason, int reason_size);
void ds_video_layer_hide(void* layer);
}
#endif

namespace ds {

#if DS_HAVE_SYSTEM_VIDEO_LAYER
namespace {

// Failures before the layer is given up on for the rest of the run. One can be
// the app going to the background; several in a row is the layer not working.
constexpr unsigned kFailuresBeforeGivingUp = 3;

bool turned_off_by_environment() {
    const char* setting = std::getenv("DUALSCREEN_VIDEO_LAYER");
    return setting != nullptr && setting[0] == '0';
}

}  // namespace
#endif

SystemVideoLayers::SystemVideoLayers() = default;

SystemVideoLayers::~SystemVideoLayers() {
#if DS_HAVE_SYSTEM_VIDEO_LAYER
    for (Layer* layer : {&top_, &bottom_}) {
        if (layer->handle != nullptr) {
            ds_video_layer_destroy(layer->handle);
            layer->handle = nullptr;
        }
    }
#endif
}

bool SystemVideoLayers::available() { return DS_HAVE_SYSTEM_VIDEO_LAYER != 0; }

bool SystemVideoLayers::attach(SDL_Window* window) {
#if DS_HAVE_SYSTEM_VIDEO_LAYER
    if (turned_off_by_environment()) {
        log_info("video: drawing streamed video with OpenGL (DUALSCREEN_VIDEO_LAYER=0)");
        return false;
    }
    view_ = ds_video_view_for_window(window);
    if (view_ == nullptr) {
        log_warn("video: the window has no view to lay video over; drawing it with OpenGL");
        return false;
    }
    return true;
#else
    (void)window;
    return false;
#endif
}

bool SystemVideoLayers::usable() const { return view_ != nullptr && !given_up_; }

void SystemVideoLayers::begin_frame() {
    top_.shown_this_frame = false;
    bottom_.shown_this_frame = false;
}

void SystemVideoLayers::show(ScreenId screen, const std::shared_ptr<void>& picture,
                             uint64_t serial, const PlacedScreen& placed, bool smooth,
                             float output_width, float output_height) {
#if DS_HAVE_SYSTEM_VIDEO_LAYER
    if (!usable() || picture == nullptr) {
        return;
    }
    Layer& layer = layer_for(screen);
    if (layer.handle == nullptr) {
        layer.handle = ds_video_layer_create(view_);
        if (layer.handle == nullptr) {
            given_up_ = true;
            log_warn("video: could not make a layer to show video in; drawing it with OpenGL");
            return;
        }
    }
    int turns = 0;
    if (placed.rotation == ScreenRotation::Clockwise) turns = 1;
    if (placed.rotation == ScreenRotation::CounterClockwise) turns = -1;
    // A layer that was hidden is still holding whatever it last showed, which
    // GL has since drawn past: it is given the current picture again.
    const bool is_new = !layer.visible || serial != layer.serial;
    char reason[256] = {0};
    const int result = ds_video_layer_show(layer.handle, picture.get(), is_new ? 1 : 0,
                                           placed.rect.x, placed.rect.y, placed.rect.w,
                                           placed.rect.h, turns, smooth ? 1 : 0, output_width,
                                           output_height, reason, static_cast<int>(sizeof(reason)));
    if (result < 0) {
        hide(layer);
        ++failures_;
        log_warn(std::string("video: the system video layer failed (") + reason + ")" +
                 (failures_ >= kFailuresBeforeGivingUp
                      ? "; drawing video with OpenGL from now on"
                      : "; drawing with OpenGL until it recovers"));
        if (failures_ >= kFailuresBeforeGivingUp) {
            given_up_ = true;
        }
        return;
    }
    layer.shown_this_frame = true;
    layer.visible = true;
    if (is_new) {
        layer.serial = serial;
    }
    if (result == 0 && !said_working_) {
        said_working_ = true;
        log_info("video: showing streamed video through the system's own video layer, not GL");
    }
#else
    (void)screen;
    (void)picture;
    (void)serial;
    (void)placed;
    (void)smooth;
    (void)output_width;
    (void)output_height;
#endif
}

void SystemVideoLayers::end_frame() {
    for (Layer* layer : {&top_, &bottom_}) {
        if (!layer->shown_this_frame) {
            hide(*layer);
        }
    }
}

void SystemVideoLayers::hide(Layer& layer) {
    if (!layer.visible) {
        return;
    }
    layer.visible = false;
#if DS_HAVE_SYSTEM_VIDEO_LAYER
    if (layer.handle != nullptr) {
        ds_video_layer_hide(layer.handle);
    }
#endif
}

}  // namespace ds
