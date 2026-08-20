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

// Which device plays the game's sound, named by the screen that device is
// holding. With one device there is nothing to choose, so this only means
// anything while two are connected. DS and 3DS have a single soundtrack, so
// "Both" is two devices playing the same thing rather than two soundtracks.
enum class AudioTarget { TopScreen, BottomScreen, Both };

const char* to_string(AudioTarget target);
// Whether the device holding `screen` should be making sound.
bool audio_reaches(AudioTarget target, ScreenId screen);
AudioTarget next_audio_target(AudioTarget target);
// Round-trips through settings files; unknown text falls back to Both.
const char* audio_target_key(AudioTarget target);
AudioTarget audio_target_from_key(const std::string& key);

// How a screen fills the normalized rect it is placed in.
enum class ScreenFit {
    Contain,  // preserve the screen's 4:3 aspect, letterboxed inside the rect
    Stretch,  // fill the rect exactly, distorting if it does not match
};

// A quarter turn applied to a screen before it is drawn. Side-by-Side is the
// DS held sideways, which turns both screens; which way it turns is which hand
// holds the stylus.
enum class ScreenRotation { None, Clockwise, CounterClockwise };

ScreenRotation mirror_rotation(ScreenRotation rotation);
// True when the turn swaps the screen's width and height.
bool rotation_is_quarter_turn(ScreenRotation rotation);

struct ScreenPlacement {
    // Normalized [0,1] rect within the layout's frame this screen is drawn
    // into. The frame is the whole window unless the layout gives an aspect
    // ratio of its own -- see Layout::frame_aspect.
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
    // Which logical screen ("top" or "bottom") this placement shows.
    ScreenId screen = ScreenId::Top;
    ScreenFit fit = ScreenFit::Contain;
    ScreenRotation rotation = ScreenRotation::None;
};

// Which way a composed layout lays its screens out.
enum class LayoutAxis { Horizontal, Vertical };

// Where a screen sits across that axis when it is smaller than the tallest (or
// widest) thing beside it. The Wii U's two focus layouts differ by exactly this:
// the small touch screen sits at the bottom, the small upper screen at the top,
// each where it would be on a real DS.
enum class CrossAlign { Start, Center, End };

// One screen in a composed layout, sized relative to the largest in the group.
struct ScreenItem {
    ScreenId screen = ScreenId::Top;
    float scale = 1.0f;
    CrossAlign align = CrossAlign::Center;
    ScreenRotation rotation = ScreenRotation::None;
};

// How a layout arranges its screens, as a description rather than as rectangles.
//
// Rectangles cannot be written down ahead of time: a screen's shape depends on
// the window's, and normalized rects would stretch with it. More to the point,
// arrangements repeat -- "these screens in a row, this one a third the size,
// this much space between them" is the same idea whether the screens are a DS's
// two, a 3DS's differently-shaped two, or a Wii U's TV and GamePad. Describing
// the arrangement and solving for the rectangles means those are new data
// rather than new arithmetic.
struct LayoutComposition {
    bool used = false;
    LayoutAxis axis = LayoutAxis::Vertical;
    std::vector<ScreenItem> items;
    // Space between neighbours, as a fraction of the largest item's extent
    // along the axis. Measured from the Wii U, where it is a deliberate visual
    // gap rather than the hinge's real one.
    float gap = 0.0f;
    // Inset on every side, as a fraction of the frame. The group is fitted
    // inside what is left and then centred.
    float margin = 0.0f;
};

struct Layout {
    std::string id;            // stable identifier, used in config files
    std::string display_name;  // shown in the picker UI
    // Either a composition, or explicit placements for the layouts whose screens
    // are positioned by something other than a row -- Standard's are set by the
    // console shell around them.
    LayoutComposition composition;
    std::vector<ScreenPlacement> placements;
    bool accepts_touch_on_bottom = true;  // whether this layout exposes a
                                          // touchable bottom-screen region

    // When true, this layout puts one screen on each device rather than both in
    // one window: each device fills itself with the screen it owns. It only
    // means anything with two devices connected, and `placements` is then
    // resolved per device by resolve_for_device().
    bool per_device = false;

    // When non-zero, placements are relative to a box of this aspect ratio
    // (width / height) letterboxed inside the window, rather than to the window
    // itself. Standard uses it so the DS's shell keeps the proportions of a real
    // one whatever shape the window is.
    float frame_aspect = 0.0f;

    // Padding around that frame, as a fraction of the window, so a layout that
    // draws something with edges of its own does not press them against the
    // edges of the display.
    float frame_margin = 0.0f;

    // Draw the DS's own shell around the screens. Only Standard does: it is the
    // layout that says "this is a Nintendo DS" rather than "these are two
    // pictures".
    bool bezel = false;
};

// Which hand holds the stylus. Layouts that put one screen beside another are
// defined right-handed and mirrored for the other, exactly as the Wii U does.
enum class Handedness { Right, Left };

const char* to_string(Handedness handedness);
const char* handedness_key(Handedness handedness);
Handedness handedness_from_key(const std::string& key);
Handedness other_hand(Handedness handedness);

// The layout as it should be drawn for a given hand: right-handed as written,
// left-handed mirrored left to right, with any quarter turns reversed to match.
Layout resolve_for_handedness(const Layout& layout, Handedness handedness);

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

// A screen as it should actually be drawn: where it goes and which way up.
// Everything that cares -- the compositor, the preview tiles, touch mapping --
// reads this and nothing else, so they cannot disagree about where a screen is.
struct PlacedScreen {
    ScreenId screen = ScreenId::Top;
    PixelRect rect;
    ScreenRotation rotation = ScreenRotation::None;
};

std::vector<PlacedScreen> place_screens(const Layout& layout,
                                        const PixelRect& window,
                                        int source_width = kDsScreenWidth,
                                        int source_height = kDsScreenHeight);
std::vector<PlacedScreen> place_screens(const Layout& layout,
                                        float output_width,
                                        float output_height,
                                        int source_width = kDsScreenWidth,
                                        int source_height = kDsScreenHeight);

// The box a layout's placements are relative to: the whole window, unless the
// layout asked for an aspect ratio of its own, in which case the largest box of
// that shape that fits, centred.
PixelRect layout_frame(const Layout& layout, const PixelRect& window);
PixelRect layout_frame(const Layout& layout, float output_width, float output_height);

// Where a placement actually lands, in pixels, within a frame. This is the
// single source of truth shared by the compositor, the previews and touch
// mapping — if they ever disagreed, touches would land in the wrong place.
PixelRect placement_pixel_rect(const ScreenPlacement& placement,
                               const PixelRect& frame,
                               int source_width = kDsScreenWidth,
                               int source_height = kDsScreenHeight);
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
