#include "layout/layout_registry.hpp"

#include <utility>
#include <vector>

#include "util/log.hpp"

namespace ds {
namespace {

// Real Nintendo DS proportions, which is what makes the Standard layout read as
// a console rather than as a border: the body is 148.7 x 84.7 mm per half and
// the active area of a screen is 61.8 x 46.4 mm.
constexpr float kShellWidthPerScreen = 148.7f / 61.8f;   // 2.406
constexpr float kShellHalfHeightPerScreen = 84.7f / 46.4f;  // 1.825

// A screen is 4:3, so the whole open console is this much wider than it is tall.
constexpr float kShellAspect = (kShellWidthPerScreen * 4.0f / 3.0f) /
                               (kShellHalfHeightPerScreen * 2.0f);

// Where each screen sits inside that shell, as a fraction of the whole.
constexpr float kShellScreenWidth = 1.0f / kShellWidthPerScreen;             // 0.416
constexpr float kShellScreenHeight = 1.0f / (kShellHalfHeightPerScreen * 2.0f);  // 0.274

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
        layout.bezel = true;
        // The shell is the point of this one, and a blurred screen inside a
        // crisp plastic body looks like a mistake. The Wii U does not offer
        // smoothing here either.
        layout.forbids_filtering = true;
        layout.placements = {
            make_placement((1.0f - kShellScreenWidth) * 0.5f, 0.25f - kShellScreenHeight * 0.5f,
                           kShellScreenWidth, kShellScreenHeight, ScreenId::Top),
            make_placement((1.0f - kShellScreenWidth) * 0.5f, 0.75f - kShellScreenHeight * 0.5f,
                           kShellScreenWidth, kShellScreenHeight, ScreenId::Bottom),
        };
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
