#pragma once

#include <string>
#include <vector>

namespace ds {

// Native dimensions of one DS screen. Touch coordinates are always expressed
// in this space, whatever the core's internal framebuffer resolution is.
constexpr int kDsScreenWidth = 256;
constexpr int kDsScreenHeight = 192;

enum class ScreenId { Top, Bottom };

const char* to_string(ScreenId screen);

// How a screen fills the normalized rect it is placed in.
enum class ScreenFit {
    Contain,  // preserve the screen's 4:3 aspect, letterboxed inside the rect
    Stretch,  // fill the rect exactly, distorting if it does not match
};

struct ScreenPlacement {
    // Normalized [0,1] rect within the output window this screen is drawn into.
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
    // Which logical screen ("top" or "bottom") this placement shows.
    ScreenId screen = ScreenId::Top;
    ScreenFit fit = ScreenFit::Contain;
};

struct Layout {
    std::string id;            // stable identifier, used in config files
    std::string display_name;  // shown in the picker UI
    std::vector<ScreenPlacement> placements;
    bool accepts_touch_on_bottom = true;  // whether this layout exposes a
                                          // touchable bottom-screen region

    // When true, this layout puts one screen on each device rather than both in
    // one window: each device fills itself with the screen it owns. It only
    // means anything with two devices connected, and `placements` is then
    // resolved per device by resolve_for_device().
    bool per_device = false;
};

// The placements this device should actually draw. For an ordinary layout that
// is the layout itself; for a per-device one it is a single full-window
// placement of `local_screen`. Called once per frame, on both devices, so each
// draws its own half of the same choice.
Layout resolve_for_device(const Layout& layout, ScreenId local_screen);

// A rect in output pixels.
struct PixelRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Where a placement actually lands, in pixels, for a given output size. This is
// the single source of truth shared by the compositor and by touch mapping — if
// they ever disagreed, touches would land in the wrong place.
PixelRect placement_pixel_rect(const ScreenPlacement& placement,
                               float output_width,
                               float output_height,
                               int source_width = kDsScreenWidth,
                               int source_height = kDsScreenHeight);

struct DsTouchPoint {
    int x = 0;
    int y = 0;
};

// Maps a point in output-window pixels back into DS touchscreen space, using
// the inverse of the bottom screen's placement. Returns false when the point is
// not over a touchable bottom screen. This is derived generically from the
// layout data, so a new layout is new data, not new code.
bool map_output_point_to_ds(const Layout& layout,
                            float output_width,
                            float output_height,
                            float point_x,
                            float point_y,
                            DsTouchPoint& out_point);

// True if this layout shows the bottom screen at all (and so can accept touch).
bool layout_has_touchable_bottom(const Layout& layout);

}  // namespace ds
