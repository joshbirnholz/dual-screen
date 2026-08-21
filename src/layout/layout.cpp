#include "layout/layout.hpp"

#include <algorithm>
#include <cmath>

namespace ds {

const char* to_string(ScreenId screen) {
    return screen == ScreenId::Top ? "top" : "bottom";
}

const char* to_string(AudioTarget target) {
    switch (target) {
        case AudioTarget::TopScreen: return "top screen only";
        case AudioTarget::BottomScreen: return "bottom screen only";
        case AudioTarget::Both: break;
    }
    return "both screens";
}

bool audio_reaches(AudioTarget target, ScreenId screen) {
    switch (target) {
        case AudioTarget::TopScreen: return screen == ScreenId::Top;
        case AudioTarget::BottomScreen: return screen == ScreenId::Bottom;
        case AudioTarget::Both: break;
    }
    return true;
}

AudioTarget next_audio_target(AudioTarget target) {
    switch (target) {
        case AudioTarget::Both: return AudioTarget::TopScreen;
        case AudioTarget::TopScreen: return AudioTarget::BottomScreen;
        case AudioTarget::BottomScreen: break;
    }
    return AudioTarget::Both;
}

const char* audio_target_key(AudioTarget target) {
    switch (target) {
        case AudioTarget::TopScreen: return "top";
        case AudioTarget::BottomScreen: return "bottom";
        case AudioTarget::Both: break;
    }
    return "both";
}

AudioTarget audio_target_from_key(const std::string& key) {
    if (key == "top") return AudioTarget::TopScreen;
    if (key == "bottom") return AudioTarget::BottomScreen;
    return AudioTarget::Both;
}

ScreenRotation mirror_rotation(ScreenRotation rotation) {
    switch (rotation) {
        case ScreenRotation::Clockwise: return ScreenRotation::CounterClockwise;
        case ScreenRotation::CounterClockwise: return ScreenRotation::Clockwise;
        case ScreenRotation::None: break;
    }
    return ScreenRotation::None;
}

bool rotation_is_quarter_turn(ScreenRotation rotation) {
    return rotation != ScreenRotation::None;
}

const char* to_string(Handedness handedness) {
    return handedness == Handedness::Left ? "left-handed" : "right-handed";
}

const char* handedness_key(Handedness handedness) {
    return handedness == Handedness::Left ? "left" : "right";
}

Handedness handedness_from_key(const std::string& key) {
    return key == "left" ? Handedness::Left : Handedness::Right;
}

Handedness other_hand(Handedness handedness) {
    return handedness == Handedness::Left ? Handedness::Right : Handedness::Left;
}

SystemScreens as_preview(const SystemScreens& screens) {
    SystemScreens preview = screens;
    // A tile is smaller than any threshold, so leaving this in would draw the
    // console at full size across the rest of the menu.
    preview.console.native_width = 0.0f;
    preview.console.native_height = 0.0f;
    return preview;
}

Layout resolve_for_handedness(const Layout& layout, Handedness handedness) {
    if (handedness == Handedness::Right) {
        return layout;
    }
    Layout mirrored = layout;
    // A composed layout mirrors by running its items the other way along the
    // axis. Stacked screens are unaffected, which is right: there is no left or
    // right to a column, and the Wii U's vertical layouts differ only in how the
    // GamePad is held.
    if (mirrored.composition.used) {
        if (mirrored.composition.axis == LayoutAxis::Horizontal) {
            std::reverse(mirrored.composition.items.begin(), mirrored.composition.items.end());
        }
        for (ScreenItem& item : mirrored.composition.items) {
            item.rotation = mirror_rotation(item.rotation);
        }
    }
    for (ScreenPlacement& placement : mirrored.placements) {
        // Mirrored about the frame's vertical centre line, so a screen tucked
        // into the right corner ends up in the left one. Centred placements come
        // back unchanged, which is why the layouts with no handed variant on the
        // Wii U need no special case here.
        placement.x = 1.0f - placement.x - placement.w;
        placement.rotation = mirror_rotation(placement.rotation);
    }
    return mirrored;
}

namespace {

// The size one screen occupies, in source pixels, given its own dimensions and
// its rotation. Each item measures against its own screen, so "a third the size"
// means a third of that screen rather than a third of some shared unit -- which
// is what keeps the arrangements right on a console whose screens differ.
void item_source_size(const ScreenItem& item,
                      const SystemScreens& screens,
                      float& out_width,
                      float& out_height) {
    const ScreenSize size = screens.size_of(item.screen);
    const bool turned = rotation_is_quarter_turn(item.rotation);
    out_width = static_cast<float>(turned ? size.height : size.width) * item.scale;
    out_height = static_cast<float>(turned ? size.width : size.height) * item.scale;
}

std::vector<PlacedScreen> place_composed(const LayoutComposition& composition,
                                         const PixelRect& frame,
                                         const SystemScreens& screens) {
    std::vector<PlacedScreen> placed;
    if (composition.items.empty() || frame.w <= 0.0f || frame.h <= 0.0f) {
        return placed;
    }

    const bool horizontal = composition.axis == LayoutAxis::Horizontal;

    // The group's natural size, in source pixels, before it is scaled to fit.
    float along = 0.0f;
    float across = 0.0f;
    float largest_along = 0.0f;
    for (const ScreenItem& item : composition.items) {
        float width = 0.0f;
        float height = 0.0f;
        item_source_size(item, screens, width, height);
        const float item_along = horizontal ? width : height;
        const float item_across = horizontal ? height : width;
        along += item_along;
        across = std::max(across, item_across);
        largest_along = std::max(largest_along, item_along);
    }
    const float gap = composition.gap * largest_along;
    along += gap * static_cast<float>(composition.items.size() - 1);
    if (along <= 0.0f || across <= 0.0f) {
        return placed;
    }

    // The largest scale that fits inside the frame once the margin is taken off.
    const float usable_width = frame.w * (1.0f - composition.margin * 2.0f);
    const float usable_height = frame.h * (1.0f - composition.margin * 2.0f);
    const float usable_along = horizontal ? usable_width : usable_height;
    const float usable_across = horizontal ? usable_height : usable_width;
    const float scale = std::min(usable_along / along, usable_across / across);
    if (!(scale > 0.0f)) {
        return placed;
    }

    // Centred in the frame itself rather than inside the margin, so a group that
    // came out smaller than its allowance still sits in the middle.
    const float group_along = along * scale;
    const float group_across = across * scale;
    float cursor = (horizontal ? frame.x : frame.y) +
                   ((horizontal ? frame.w : frame.h) - group_along) * 0.5f;
    const float across_origin = (horizontal ? frame.y : frame.x) +
                                ((horizontal ? frame.h : frame.w) - group_across) * 0.5f;

    for (const ScreenItem& item : composition.items) {
        float width = 0.0f;
        float height = 0.0f;
        item_source_size(item, screens, width, height);
        width *= scale;
        height *= scale;
        const float item_along = horizontal ? width : height;
        const float item_across = horizontal ? height : width;

        float across_offset = 0.0f;
        switch (item.align) {
            case CrossAlign::Start: break;
            case CrossAlign::Center: across_offset = (group_across - item_across) * 0.5f; break;
            case CrossAlign::End: across_offset = group_across - item_across; break;
        }

        PlacedScreen entry;
        entry.screen = item.screen;
        entry.rotation = item.rotation;
        entry.rect.x = horizontal ? cursor : across_origin + across_offset;
        entry.rect.y = horizontal ? across_origin + across_offset : cursor;
        entry.rect.w = width;
        entry.rect.h = height;
        placed.push_back(entry);

        cursor += item_along + gap * scale;
    }
    return placed;
}

}  // namespace

namespace {

// Notices a screen that came out at its own size and makes it exactly that,
// on whole pixels.
//
// Within a pixel counts. A layout fitted to a window lands on round numbers only
// by accident -- the Wii U's focus layouts put their small screen at 256.5 x
// 192.4 on a Deck, which is a DS screen in every sense that matters -- and
// insisting on equality would leave those being resampled for the sake of half a
// pixel nobody can see. Moving a screen a fraction of a pixel is invisible;
// the difference between a resampled picture and an exact one is not.
constexpr float kNativeTolerance = 1.0f;

void mark_native(PlacedScreen& placed, const SystemScreens& screens) {
    const ScreenSize size = screens.size_of(placed.screen);
    const bool turned = rotation_is_quarter_turn(placed.rotation);
    const float natural_width = static_cast<float>(turned ? size.height : size.width);
    const float natural_height = static_cast<float>(turned ? size.width : size.height);
    if (std::fabs(placed.rect.w - natural_width) > kNativeTolerance ||
        std::fabs(placed.rect.h - natural_height) > kNativeTolerance) {
        return;
    }
    placed.native = true;
    placed.rect.x = std::round(placed.rect.x);
    placed.rect.y = std::round(placed.rect.y);
    placed.rect.w = natural_width;
    placed.rect.h = natural_height;
}

}  // namespace

std::vector<PlacedScreen> place_screens(const Layout& layout,
                                        const PixelRect& window,
                                        const SystemScreens& screens) {
    const PixelRect frame = layout_frame(layout, window, screens);
    std::vector<PlacedScreen> placed;
    if (layout.composition.used) {
        placed = place_composed(layout.composition, frame, screens);
    } else if (layout.uses_console_frame && screens.console.exists()) {
        // The console's own arrangement, from whichever system is running.
        for (const ScreenPlacement* placement : {&screens.console.top, &screens.console.bottom}) {
            PlacedScreen entry;
            entry.screen = placement->screen;
            entry.rotation = placement->rotation;
            entry.rect = placement_pixel_rect(*placement, frame, screens.size_of(placement->screen));
            placed.push_back(entry);
        }
    } else {
        placed.reserve(layout.placements.size());
        for (const ScreenPlacement& placement : layout.placements) {
            PlacedScreen entry;
            entry.screen = placement.screen;
            entry.rotation = placement.rotation;
            entry.rect =
                placement_pixel_rect(placement, frame, screens.size_of(placement.screen));
            placed.push_back(entry);
        }
    }
    for (PlacedScreen& entry : placed) {
        mark_native(entry, screens);
    }
    return placed;
}

std::vector<PlacedScreen> place_screens(const Layout& layout,
                                        float output_width,
                                        float output_height,
                                        const SystemScreens& screens) {
    return place_screens(layout, PixelRect{0.0f, 0.0f, output_width, output_height}, screens);
}

PixelRect layout_frame(const Layout& layout,
                       const PixelRect& window,
                       const SystemScreens& screens) {
    const ConsoleFrame& console = screens.console;
    if (!layout.uses_console_frame || !console.exists() || window.w <= 0.0f || window.h <= 0.0f) {
        return window;
    }
    const bool native_fits = console.native_width > 0.0f && console.native_height > 0.0f &&
                             window.w <= console.native_max_output_width &&
                             window.h <= console.native_max_output_height;
    if (native_fits) {
        // Whole pixels, so the screens inside land on the pixel grid rather than
        // half a pixel off it. Bigger than the window is allowed: an exact
        // picture with its edges over the sides beats a soft one that fits.
        PixelRect frame;
        frame.w = console.native_width;
        frame.h = console.native_height;
        frame.x = std::round(window.x + (window.w - frame.w) * 0.5f);
        frame.y = std::round(window.y + (window.h - frame.h) * 0.5f);
        return frame;
    }
    PixelRect frame = window;
    if (console.margin > 0.0f) {
        frame.x += window.w * console.margin;
        frame.y += window.h * console.margin;
        frame.w -= window.w * console.margin * 2.0f;
        frame.h -= window.h * console.margin * 2.0f;
        if (frame.w <= 0.0f || frame.h <= 0.0f) {
            return window;
        }
    }
    const PixelRect available = frame;
    const float window_aspect = available.w / available.h;
    if (window_aspect > console.aspect) {
        frame.w = available.h * console.aspect;
        frame.x = available.x + (available.w - frame.w) * 0.5f;
    } else {
        frame.h = available.w / console.aspect;
        frame.y = available.y + (available.h - frame.h) * 0.5f;
    }
    return frame;
}

PixelRect layout_frame(const Layout& layout,
                       float output_width,
                       float output_height,
                       const SystemScreens& screens) {
    return layout_frame(layout, PixelRect{0.0f, 0.0f, output_width, output_height}, screens);
}

PixelRect placement_pixel_rect(const ScreenPlacement& placement,
                               const PixelRect& frame,
                               ScreenSize source) {
    int source_width = source.width;
    int source_height = source.height;
    PixelRect rect;
    rect.x = frame.x + placement.x * frame.w;
    rect.y = frame.y + placement.y * frame.h;
    rect.w = placement.w * frame.w;
    rect.h = placement.h * frame.h;

    // A quarter-turned screen is 3:4 where an upright one is 4:3, and the fit
    // has to letterbox against the shape actually drawn.
    if (rotation_is_quarter_turn(placement.rotation)) {
        std::swap(source_width, source_height);
    }

    if (placement.fit == ScreenFit::Stretch || source_width <= 0 || source_height <= 0 ||
        rect.w <= 0.0f || rect.h <= 0.0f) {
        return rect;
    }

    // Letterbox the source's aspect ratio inside the placement rect.
    const float source_aspect = static_cast<float>(source_width) / static_cast<float>(source_height);
    const float rect_aspect = rect.w / rect.h;
    if (rect_aspect > source_aspect) {
        const float fitted_width = rect.h * source_aspect;
        rect.x += (rect.w - fitted_width) * 0.5f;
        rect.w = fitted_width;
    } else {
        const float fitted_height = rect.w / source_aspect;
        rect.y += (rect.h - fitted_height) * 0.5f;
        rect.h = fitted_height;
    }
    return rect;
}

PixelRect placement_pixel_rect(const ScreenPlacement& placement,
                               float output_width,
                               float output_height,
                               ScreenSize source) {
    return placement_pixel_rect(placement, PixelRect{0.0f, 0.0f, output_width, output_height},
                                source);
}

Layout resolve_for_device(const Layout& layout, ScreenId local_screen) {
    if (!layout.per_device) {
        return layout;
    }
    Layout resolved = layout;
    resolved.placements.clear();
    // One screen filling a whole device is not the DS's shell, and nothing is
    // beside anything, so neither the frame, the bezel nor the arrangement
    // carries over.
    resolved.uses_console_frame = false;
    resolved.composition = LayoutComposition();

    ScreenPlacement placement;
    placement.x = 0.0f;
    placement.y = 0.0f;
    placement.w = 1.0f;
    placement.h = 1.0f;
    placement.screen = local_screen;
    placement.fit = ScreenFit::Contain;
    resolved.placements.push_back(placement);

    // Only the device holding the bottom screen can be touched, which falls out
    // of the generic touch mapping once the placements say so.
    resolved.accepts_touch_on_bottom = (local_screen == ScreenId::Bottom);
    return resolved;
}

bool layout_has_touchable_bottom(const Layout& layout) {
    if (!layout.accepts_touch_on_bottom) {
        return false;
    }
    if (layout.composition.used) {
        for (const ScreenItem& item : layout.composition.items) {
            if (item.screen == ScreenId::Bottom) {
                return true;
            }
        }
        return false;
    }
    for (const ScreenPlacement& placement : layout.placements) {
        if (placement.screen == ScreenId::Bottom) {
            return true;
        }
    }
    return false;
}

bool map_output_point_to_ds(const Layout& layout,
                            float output_width,
                            float output_height,
                            float point_x,
                            float point_y,
                            DsTouchPoint& out_point,
                            const SystemScreens& screens) {
    if (!layout.accepts_touch_on_bottom || output_width <= 0.0f || output_height <= 0.0f) {
        return false;
    }

    const ScreenSize touch = screens.bottom;
    const std::vector<PlacedScreen> placed_screens =
        place_screens(layout, output_width, output_height, screens);

    const PlacedScreen* touch_screen = nullptr;
    for (const PlacedScreen& placed : placed_screens) {
        if (placed.screen == ScreenId::Bottom && placed.rect.w > 0.0f && placed.rect.h > 0.0f) {
            touch_screen = &placed;
            break;
        }
    }

    if (touch_screen != nullptr) {
        // A tap that landed on some other screen -- the upper one, on every
        // layout that shows both -- is a look at that screen, not a miss of
        // this one, so it still registers no touch. Everywhere else in the
        // window is fair game: bezel plastic, the gap between two stacked
        // screens, the letterbox around a screen that does not fill its
        // device. A finger that lands there is clamped to the nearest edge of
        // the touch screen, which is where a stylus aimed at that edge would
        // actually have landed.
        for (const PlacedScreen& placed : placed_screens) {
            if (&placed == touch_screen) {
                continue;
            }
            if (placed.rect.w > 0.0f && placed.rect.h > 0.0f &&
                placed.rect.contains(point_x, point_y)) {
                touch_screen = nullptr;
                break;
            }
        }
    }

    if (touch_screen != nullptr) {
        const PixelRect& rect = touch_screen->rect;
        // Clamped here, before the rotation below, rather than on the final
        // touch-space coordinate: turning the screen is just a relabelling and
        // reflection of u and v, so clamping first and rotating after lands on
        // exactly the same point as the other order would -- and it is the
        // point outside the rect that can be arbitrarily far away, so this is
        // the step that has to bound it.
        float u = std::clamp((point_x - rect.x) / rect.w, 0.0f, 1.0f);
        float v = std::clamp((point_y - rect.y) / rect.h, 0.0f, 1.0f);
        // Undo whatever turn the screen was drawn with, so a touch on a sideways
        // screen lands where the finger is pointing rather than 90 degrees off.
        switch (touch_screen->rotation) {
            case ScreenRotation::Clockwise: {
                const float turned_u = v;
                const float turned_v = 1.0f - u;
                u = turned_u;
                v = turned_v;
                break;
            }
            case ScreenRotation::CounterClockwise: {
                const float turned_u = 1.0f - v;
                const float turned_v = u;
                u = turned_u;
                v = turned_v;
                break;
            }
            case ScreenRotation::None: break;
        }
        out_point.x = std::clamp(static_cast<int>(u * static_cast<float>(touch.width)), 0,
                                 touch.width - 1);
        out_point.y = std::clamp(static_cast<int>(v * static_cast<float>(touch.height)), 0,
                                 touch.height - 1);
        return true;
    }
    return false;
}

}  // namespace ds
