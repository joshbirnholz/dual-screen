#include "layout/layout_registry.hpp"

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

// The small screen in the two "focus" layouts, as a fraction of the frame. The
// big one takes most of the width and the small one tucks into the far corner,
// which is the arrangement the Wii U uses.
constexpr float kFocusLargeWidth = 0.62f;
constexpr float kFocusSmallWidth = 0.22f;

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

    // 1. The upper screen as large as it goes, the touch screen tucked into the
    // corner under the stylus hand.
    {
        Layout layout;
        layout.id = "upper_focus";
        layout.display_name = "Upper Screen Focus";
        layout.placements = {
            make_placement(0.02f, 0.05f, kFocusLargeWidth, 0.9f, ScreenId::Top),
            make_placement(1.0f - kFocusSmallWidth - 0.03f, 0.62f, kFocusSmallWidth, 0.33f,
                           ScreenId::Bottom),
        };
        layouts_.push_back(layout);
    }

    // 2. The other way round: the touch screen large, the upper one small and
    // out of the way in the far corner.
    {
        Layout layout;
        layout.id = "touch_focus";
        layout.display_name = "Touch Screen Focus";
        layout.placements = {
            make_placement(0.03f, 0.05f, kFocusSmallWidth, 0.33f, ScreenId::Top),
            make_placement(1.0f - kFocusLargeWidth - 0.02f, 0.05f, kFocusLargeWidth, 0.9f,
                           ScreenId::Bottom),
        };
        layouts_.push_back(layout);
    }

    // 3. The DS's own arrangement without the console around it, so both screens
    // are as large as stacking them allows.
    {
        Layout layout;
        layout.id = "vertical";
        layout.display_name = "Vertical Screens";
        layout.placements = {
            make_placement(0.0f, 0.0f, 1.0f, 0.5f, ScreenId::Top),
            make_placement(0.0f, 0.5f, 1.0f, 0.5f, ScreenId::Bottom),
        };
        layouts_.push_back(layout);
    }

    // 4. The console held sideways, which turns both screens a quarter turn.
    // Right-handed turns them anticlockwise and leaves the touch screen on the
    // right; the left-handed mirror falls out of resolve_for_handedness().
    {
        Layout layout;
        layout.id = "side_by_side";
        layout.display_name = "Side-by-Side Screens";
        layout.placements = {
            make_placement(0.02f, 0.02f, 0.47f, 0.96f, ScreenId::Top,
                           ScreenRotation::CounterClockwise),
            make_placement(0.51f, 0.02f, 0.47f, 0.96f, ScreenId::Bottom,
                           ScreenRotation::CounterClockwise),
        };
        layouts_.push_back(layout);
    }

    // 5. One screen on each device. With nobody on the other end it falls back
    // to showing both, since half a game is not useful.
    {
        Layout layout;
        layout.id = "upper_on_tv";
        layout.display_name = "Upper Screen on TV";
        layout.per_device = true;
        layout.placements = {
            make_placement(0.0f, 0.0f, 1.0f, 0.5f, ScreenId::Top),
            make_placement(0.0f, 0.5f, 1.0f, 0.5f, ScreenId::Bottom),
        };
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
