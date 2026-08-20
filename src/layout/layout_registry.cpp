#include "layout/layout_registry.hpp"

#include <utility>
#include <vector>

#include "util/log.hpp"

namespace ds {
namespace {

// The Standard layout draws a picture of a DS, and its screens have to land on
// that picture's screens exactly. These come from the artwork itself
// (assets/ds_bezel.png, 615 x 630) and its companion overlay, where the screens
// are 256 x 192 -- one image pixel per DS pixel -- at (179, 60) and (179, 337).
constexpr float kShellImageWidth = 615.0f;
constexpr float kShellImageHeight = 630.0f;
constexpr float kShellAspect = kShellImageWidth / kShellImageHeight;
constexpr float kShellScreenX = 179.0f / kShellImageWidth;
constexpr float kShellScreenWidth = 256.0f / kShellImageWidth;
constexpr float kShellScreenHeight = 192.0f / kShellImageHeight;
constexpr float kShellTopScreenY = 60.0f / kShellImageHeight;
constexpr float kShellBottomScreenY = 337.0f / kShellImageHeight;

// Room to breathe around the console, so it does not sit hard against the edges
// of the window.
constexpr float kShellMargin = 0.03f;

// Everything below is measured off the Wii U Virtual Console's own output, on
// the 378 x 212 pixel picture it draws inside the TV frame in its Screen
// Settings shots. Numbers rather than eyeballing, because "a bit smaller, over
// to the right" is exactly the kind of thing that ends up subtly wrong.
//
// Focus layouts: a 227 x 170 screen beside a 76 x 57 one, 12 px apart, the pair
// inset 32 px from each side of a 378-wide picture.
constexpr float kFocusSmallScale = 76.0f / 227.0f;   // 0.335
constexpr float kFocusGap = 12.0f / 227.0f;          // 0.053 of the large screen
constexpr float kFocusMargin = 32.0f / 378.0f;       // 0.085

// Vertical: two 126 x 94 screens 18 px apart, 3 px clear of a 212-tall picture.
constexpr float kVerticalGap = 18.0f / 94.0f;        // 0.19 of a screen's height
constexpr float kVerticalMargin = 3.0f / 212.0f;     // 0.014

// Side-by-Side: two 154 x 204 screens 32 px apart, 4 px clear top and bottom.
// The gap is measured against the same edge of the screen either way -- the one
// the turn puts along the axis -- so the two layouts space alike.
constexpr float kSideBySideGap = 32.0f / 154.0f;     // 0.208
constexpr float kSideBySideMargin = 4.0f / 212.0f;   // 0.019

ScreenItem make_item(ScreenId screen,
                     float scale = 1.0f,
                     CrossAlign align = CrossAlign::Center,
                     ScreenRotation rotation = ScreenRotation::None) {
    ScreenItem item;
    item.screen = screen;
    item.scale = scale;
    item.align = align;
    item.rotation = rotation;
    return item;
}

LayoutComposition make_composition(LayoutAxis axis,
                                   std::vector<ScreenItem> items,
                                   float gap,
                                   float margin) {
    LayoutComposition composition;
    composition.used = true;
    composition.axis = axis;
    composition.items = std::move(items);
    composition.gap = gap;
    composition.margin = margin;
    return composition;
}

ScreenPlacement make_placement(float x,
                               float y,
                               float w,
                               float h,
                               ScreenId screen,
                               ScreenRotation rotation = ScreenRotation::None) {
    ScreenPlacement placement;
    placement.x = x;
    placement.y = y;
    placement.w = w;
    placement.h = h;
    placement.screen = screen;
    placement.fit = ScreenFit::Contain;
    placement.rotation = rotation;
    return placement;
}

}  // namespace

const char* LayoutRegistry::fallback_id() { return "standard"; }

std::string LayoutRegistry::migrated_id(const std::string& id) {
    // The layout set was replaced wholesale with the Wii U's. These are the old
    // ids, mapped to whichever new layout is closest, so a game that already
    // remembered one does not silently jump back to the default.
    if (id == "vertical_swapped") return "vertical";
    if (id == "horizontal" || id == "horizontal_swapped") return "side_by_side";
    if (id == "split_devices") return "upper_on_tv";
    if (id == "top_only") return "upper_focus";
    if (id == "bottom_only") return "touch_focus";
    return id;
}

LayoutRegistry::LayoutRegistry() {
    // The order, the names and the arrangements are the Wii U Virtual Console's
    // Screen Settings, which is the thing this app is trying to be. Layouts with
    // a left-handed variant there are one layout here plus a handedness toggle,
    // rather than two entries that differ only in which corner a screen is in.

    // 0. Both screens inside a drawing of the console itself.
    {
        Layout layout;
        layout.id = "standard";
        layout.display_name = "Standard Nintendo DS";
        layout.frame_aspect = kShellAspect;
        layout.frame_margin = kShellMargin;
        // On a display no bigger than a Deck's there is not enough room to scale
        // the console up without softening it, so it is drawn at its own size
        // and the screens come out exact. Larger displays scale it as usual.
        layout.native_frame.width = kShellImageWidth;
        layout.native_frame.height = kShellImageHeight;
        layout.native_frame.max_output_width = 1280.0f;
        layout.native_frame.max_output_height = 800.0f;
        layout.bezel = true;
        // Stretch rather than contain: these rects are already the screens'
        // exact shape, and letterboxing inside them would leave a hairline of
        // the artwork's black showing along an edge.
        layout.placements = {
            make_placement(kShellScreenX, kShellTopScreenY, kShellScreenWidth, kShellScreenHeight,
                           ScreenId::Top),
            make_placement(kShellScreenX, kShellBottomScreenY, kShellScreenWidth,
                           kShellScreenHeight, ScreenId::Bottom),
        };
        for (ScreenPlacement& placement : layout.placements) {
            placement.fit = ScreenFit::Stretch;
        }
        layouts_.push_back(layout);
    }

    // 1. The upper screen as large as it goes, the touch screen small beside it.
    // The small one sits at the bottom, where the touch screen is on a DS and
    // where a thumb can reach it; the two are bottom-aligned as a result.
    {
        Layout layout;
        layout.id = "upper_focus";
        layout.display_name = "Upper Screen Focus";
        layout.composition = make_composition(
            LayoutAxis::Horizontal,
            {
                make_item(ScreenId::Top),
                make_item(ScreenId::Bottom, kFocusSmallScale, CrossAlign::End),
            },
            kFocusGap, kFocusMargin);
        layouts_.push_back(layout);
    }

    // 2. The other way round: the touch screen large, the upper one small. That
    // one sits at the top, again where it belongs on a DS, so this pair is
    // top-aligned where the last was bottom-aligned.
    {
        Layout layout;
        layout.id = "touch_focus";
        layout.display_name = "Touch Screen Focus";
        layout.composition = make_composition(
            LayoutAxis::Horizontal,
            {
                make_item(ScreenId::Top, kFocusSmallScale, CrossAlign::Start),
                make_item(ScreenId::Bottom),
            },
            kFocusGap, kFocusMargin);
        layouts_.push_back(layout);
    }

    // 3. The DS's own arrangement without the console around it, so both screens
    // are as large as stacking them allows.
    {
        Layout layout;
        layout.id = "vertical";
        layout.display_name = "Vertical Screens";
        layout.composition = make_composition(
            LayoutAxis::Vertical,
            {make_item(ScreenId::Top), make_item(ScreenId::Bottom)},
            kVerticalGap, kVerticalMargin);
        layouts_.push_back(layout);
    }

    // 4. The console held sideways, which turns both screens a quarter turn.
    // Right-handed turns them anticlockwise and leaves the touch screen on the
    // right; the left-handed mirror falls out of resolve_for_handedness().
    {
        Layout layout;
        layout.id = "side_by_side";
        layout.display_name = "Side-by-Side Screens";
        layout.composition = make_composition(
            LayoutAxis::Horizontal,
            {
                make_item(ScreenId::Top, 1.0f, CrossAlign::Center,
                          ScreenRotation::CounterClockwise),
                make_item(ScreenId::Bottom, 1.0f, CrossAlign::Center,
                          ScreenRotation::CounterClockwise),
            },
            kSideBySideGap, kSideBySideMargin);
        layouts_.push_back(layout);
    }

    // 5. One screen on each device. With nobody on the other end it falls back
    // to showing both stacked, since half a game is not useful.
    {
        Layout layout;
        layout.id = "upper_on_tv";
        layout.display_name = "Upper Screen on TV";
        layout.per_device = true;
        layout.composition = make_composition(
            LayoutAxis::Vertical,
            {make_item(ScreenId::Top), make_item(ScreenId::Bottom)},
            kVerticalGap, kVerticalMargin);
        layouts_.push_back(layout);
    }
}

const Layout* LayoutRegistry::find(const std::string& id) const {
    for (const Layout& layout : layouts_) {
        if (layout.id == id) {
            return &layout;
        }
    }
    return nullptr;
}

int LayoutRegistry::index_of(const std::string& id) const {
    for (std::size_t i = 0; i < layouts_.size(); ++i) {
        if (layouts_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const Layout& LayoutRegistry::at(std::size_t index) const {
    if (index >= layouts_.size()) {
        return fallback();
    }
    return layouts_[index];
}

const Layout& LayoutRegistry::fallback() const {
    const Layout* layout = find(fallback_id());
    return layout != nullptr ? *layout : layouts_.front();
}

const Layout& LayoutRegistry::resolve(const std::string& id) const {
    const Layout* layout = find(id);
    if (layout != nullptr) {
        return *layout;
    }
    const std::string migrated = migrated_id(id);
    if (migrated != id) {
        layout = find(migrated);
        if (layout != nullptr) {
            return *layout;
        }
    }
    if (!id.empty()) {
        log_warn("unknown layout id '" + id + "', falling back to " + fallback_id());
    }
    return fallback();
}

}  // namespace ds
