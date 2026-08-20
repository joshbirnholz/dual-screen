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

Layout resolve_for_handedness(const Layout& layout, Handedness handedness) {
    if (handedness == Handedness::Right) {
        return layout;
    }
    Layout mirrored = layout;
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

PixelRect layout_frame(const Layout& layout, const PixelRect& window) {
    if (layout.frame_aspect <= 0.0f || window.w <= 0.0f || window.h <= 0.0f) {
        return window;
    }
    PixelRect frame = window;
    const float window_aspect = window.w / window.h;
    if (window_aspect > layout.frame_aspect) {
        frame.w = window.h * layout.frame_aspect;
        frame.x = window.x + (window.w - frame.w) * 0.5f;
    } else {
        frame.h = window.w / layout.frame_aspect;
        frame.y = window.y + (window.h - frame.h) * 0.5f;
    }
    return frame;
}

PixelRect layout_frame(const Layout& layout, float output_width, float output_height) {
    return layout_frame(layout, PixelRect{0.0f, 0.0f, output_width, output_height});
}

PixelRect placement_pixel_rect(const ScreenPlacement& placement,
                               const PixelRect& frame,
                               int source_width,
                               int source_height) {
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
                               int source_width,
                               int source_height) {
    return placement_pixel_rect(placement, PixelRect{0.0f, 0.0f, output_width, output_height},
                                source_width, source_height);
}

Layout resolve_for_device(const Layout& layout, ScreenId local_screen) {
    if (!layout.per_device) {
        return layout;
    }
    Layout resolved = layout;
    resolved.placements.clear();
    // One screen filling a whole device is not the DS's shell, and nothing is
    // beside anything, so neither the frame nor the bezel carries over.
    resolved.frame_aspect = 0.0f;
    resolved.bezel = false;

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
                            DsTouchPoint& out_point) {
    if (!layout.accepts_touch_on_bottom || output_width <= 0.0f || output_height <= 0.0f) {
        return false;
    }

    const PixelRect frame = layout_frame(layout, output_width, output_height);
    for (const ScreenPlacement& placement : layout.placements) {
        if (placement.screen != ScreenId::Bottom) {
            continue;
        }
        const PixelRect rect = placement_pixel_rect(placement, frame);
        if (rect.w <= 0.0f || rect.h <= 0.0f || !rect.contains(point_x, point_y)) {
            continue;
        }
        float u = (point_x - rect.x) / rect.w;
        float v = (point_y - rect.y) / rect.h;
        // Undo whatever turn the screen was drawn with, so a touch on a sideways
        // screen lands where the finger is pointing rather than 90 degrees off.
        switch (placement.rotation) {
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
        out_point.x = std::clamp(static_cast<int>(u * kDsScreenWidth), 0, kDsScreenWidth - 1);
        out_point.y = std::clamp(static_cast<int>(v * kDsScreenHeight), 0, kDsScreenHeight - 1);
        return true;
    }
    return false;
}

}  // namespace ds
