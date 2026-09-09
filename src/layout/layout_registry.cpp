#include "layout/layout_registry.hpp"

#include <utility>
#include <vector>

#include "core/system.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// Everything below is measured off the Wii U Virtual Console's own output, on
// the 378 x 212 pixel picture it draws inside the TV frame in its Screen
// Settings shots. Numbers rather than eyeballing, because "a bit smaller, over
// to the right" is exactly the kind of thing that ends up subtly wrong.
//
// Focus layouts: a 227 x 170 screen beside a 110 x 82 one, 12 px apart. The
// small screen was re-measured off a fresh Tetris DS reference shot -- the
// previous 76 x 57 read the earlier screenshot too small. The margin was
// re-measured too: the original 32 px read as a wide gutter around the pair,
// several times what the other layouts leave clear (kVerticalMargin,
// kSideBySideMargin below), and left the group looking small and adrift in
// the window rather than filling it with just a little air at the edges.
constexpr float kFocusSmallScale = 110.0f / 227.0f;  // 0.485
constexpr float kFocusGap = 12.0f / 227.0f;          // 0.053 of the large screen
constexpr float kFocusMargin = 8.0f / 378.0f;        // 0.021

// Vertical: two 126 x 94 screens 18 px apart, 3 px clear of a 212-tall picture.
constexpr float kVerticalGap = 18.0f / 94.0f;        // 0.19 of a screen's height
constexpr float kVerticalMargin = 3.0f / 212.0f;     // 0.014

// Side-by-Side: two 154 x 204 screens 32 px apart, 4 px clear top and bottom.
// The gap is measured against the same edge of the screen either way -- the one
// the turn puts along the axis -- so the two layouts space alike.
constexpr float kSideBySideGap = 32.0f / 154.0f;     // 0.208
constexpr float kSideBySideMargin = 4.0f / 212.0f;   // 0.019

// And none at all for a layout that is one screen and nothing else.
//
// Every margin above is air between things: between two screens, or between a
// group of them and the edge of a picture that is meant to read as an object
// sitting in a window. A single screen filling a device is not an object in a
// window -- it is the picture, and the device is a frame around it. Clear
// space there is not composition, it is a smaller game.
//
// This was a real complaint rather than a tidy-up. These two layouts are what
// a Wii U's screens are shown through, and kFocusMargin was copied into them
// when they were written. On a display of 1280x800 or less the margin is
// dropped anyway (see is_small_display), so a handheld companion never saw
// it and a docked one did: the same Switch showing the same screen padded on
// all four sides in the dock and not out of it.
constexpr float kAloneMargin = 0.0f;

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

    // 0. Both screens arranged the way the console itself arranges them, inside
    // a picture of it where there is one. The shape, the screen positions and
    // whether there is any artwork all come from the system being played, so
    // this entry says only that it is that layout.
    {
        Layout layout;
        layout.id = "standard";
        layout.display_name = "Standard";
        layout.uses_console_frame = true;
        // A Wii U's two screens are not one picture the way a DS's or a
        // 3DS's are, and it no longer has both available to compose here at
        // all -- see tv_only/gamepad_only below and docs/cemu-integration.md.
        layout.systems = {SystemId::Ds, SystemId::ThreeDs};
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
        // A player's choice like Vertical Screens' below -- closing it just
        // brings the small screen flush against the large one, which is a
        // real arrangement rather than a broken one. See
        // LayoutComposition::gap_toggleable.
        layout.composition.gap_toggleable = true;
        // L3 flips between the two focus layouts -- see Layout::swap_layout_id.
        layout.swap_layout_id = "touch_focus";
        // See "standard" above: a Wii U never has both screens to compose.
        layout.systems = {SystemId::Ds, SystemId::ThreeDs};
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
        layout.composition.gap_toggleable = true;
        layout.swap_layout_id = "upper_focus";
        // See "standard" above: a Wii U never has both screens to compose.
        layout.systems = {SystemId::Ds, SystemId::ThreeDs};
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
        // A player's choice rather than part of the arrangement: two screens
        // either side of nothing has no real equivalent on a Wii U, so there
        // is no "correct" answer to defer to. Side-by-Side below is the one
        // layout that keeps its gap fixed -- it stands in for the DS's own
        // hinge, so removing it would misrepresent the console rather than
        // just tighten the picture.
        layout.composition.gap_toggleable = true;
        // Stacking a DS's two screens without the hinge between them. A Wii U
        // has no hinge to be without -- its two screens are not one picture --
        // so this is not offered there. See Layout::systems.
        layout.systems = {SystemId::Ds, SystemId::ThreeDs};
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
        // The console held sideways, hinge and all -- the same reason Vertical
        // Screens above is not offered for a Wii U applies here too.
        layout.systems = {SystemId::Ds, SystemId::ThreeDs};
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
        // A Wii U's own per-device split is Cemu keeping one screen to itself
        // and this app only ever holding the other -- see tv_only/gamepad_only
        // below -- rather than this app resolving a two-screen layout down to
        // one side of it.
        layout.systems = {SystemId::Ds, SystemId::ThreeDs};
        layouts_.push_back(layout);
    }

    // 6. The television alone, filling whatever is showing it -- one device or
    // two. Only a Wii U has a screen that is ever the whole of the game rather
    // than half of one, so only a Wii U offers this.
    {
        Layout layout;
        layout.id = "tv_only";
        layout.display_name = "TV Only";
        layout.systems = {SystemId::WiiU};
        layout.composition =
            make_composition(LayoutAxis::Horizontal, {make_item(ScreenId::Top)}, 0.0f, kAloneMargin);
        // Nothing is drawn below to touch.
        layout.accepts_touch_on_bottom = false;
        layouts_.push_back(layout);
    }

    // 7. The GamePad alone, the same way -- the other half of the same idea.
    {
        Layout layout;
        layout.id = "gamepad_only";
        layout.display_name = "GamePad Only";
        layout.systems = {SystemId::WiiU};
        layout.composition = make_composition(LayoutAxis::Horizontal, {make_item(ScreenId::Bottom)},
                                              0.0f, kAloneMargin);
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
