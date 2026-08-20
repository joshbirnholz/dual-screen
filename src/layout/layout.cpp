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

PixelRect placement_pixel_rect(const ScreenPlacement& placement,
                               float output_width,
                               float output_height,
                               int source_width,
                               int source_height) {
    PixelRect rect;
    rect.x = placement.x * output_width;
    rect.y = placement.y * output_height;
    rect.w = placement.w * output_width;
    rect.h = placement.h * output_height;

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

Layout resolve_for_device(const Layout& layout, ScreenId local_screen) {
    if (!layout.per_device) {
        return layout;
    }
    Layout resolved = layout;
    resolved.placements.clear();

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

    for (const ScreenPlacement& placement : layout.placements) {
        if (placement.screen != ScreenId::Bottom) {
            continue;
        }
        const PixelRect rect = placement_pixel_rect(placement, output_width, output_height);
        if (rect.w <= 0.0f || rect.h <= 0.0f || !rect.contains(point_x, point_y)) {
            continue;
        }
        const float u = (point_x - rect.x) / rect.w;
        const float v = (point_y - rect.y) / rect.h;
        out_point.x = std::clamp(static_cast<int>(u * kDsScreenWidth), 0, kDsScreenWidth - 1);
        out_point.y = std::clamp(static_cast<int>(v * kDsScreenHeight), 0, kDsScreenHeight - 1);
        return true;
    }
    return false;
}

}  // namespace ds
