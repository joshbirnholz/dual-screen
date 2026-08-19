#include "layout/layout_registry.hpp"

#include "util/log.hpp"

namespace ds {
namespace {

ScreenPlacement make_placement(float x, float y, float w, float h, ScreenId screen) {
    ScreenPlacement placement;
    placement.x = x;
    placement.y = y;
    placement.w = w;
    placement.h = h;
    placement.screen = screen;
    placement.fit = ScreenFit::Contain;
    return placement;
}

}  // namespace

const char* LayoutRegistry::fallback_id() { return "vertical"; }

LayoutRegistry::LayoutRegistry() {
    layouts_.push_back(Layout{
        "vertical",
        "Vertical",
        {
            make_placement(0.0f, 0.0f, 1.0f, 0.5f, ScreenId::Top),
            make_placement(0.0f, 0.5f, 1.0f, 0.5f, ScreenId::Bottom),
        },
        true,
    });

    layouts_.push_back(Layout{
        "vertical_swapped",
        "Vertical, swapped",
        {
            make_placement(0.0f, 0.0f, 1.0f, 0.5f, ScreenId::Bottom),
            make_placement(0.0f, 0.5f, 1.0f, 0.5f, ScreenId::Top),
        },
        true,
    });

    layouts_.push_back(Layout{
        "horizontal",
        "Horizontal",
        {
            make_placement(0.0f, 0.0f, 0.5f, 1.0f, ScreenId::Top),
            make_placement(0.5f, 0.0f, 0.5f, 1.0f, ScreenId::Bottom),
        },
        true,
    });

    layouts_.push_back(Layout{
        "horizontal_swapped",
        "Horizontal, swapped",
        {
            make_placement(0.0f, 0.0f, 0.5f, 1.0f, ScreenId::Bottom),
            make_placement(0.5f, 0.0f, 0.5f, 1.0f, ScreenId::Top),
        },
        true,
    });

    // The single-screen layouts double as the base case the future networked
    // companion mode reuses: each device renders one screen full-window.
    layouts_.push_back(Layout{
        "top_only",
        "Top screen only",
        {
            make_placement(0.0f, 0.0f, 1.0f, 1.0f, ScreenId::Top),
        },
        false,
    });

    layouts_.push_back(Layout{
        "bottom_only",
        "Bottom screen only",
        {
            make_placement(0.0f, 0.0f, 1.0f, 1.0f, ScreenId::Bottom),
        },
        true,
    });
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
    if (!id.empty()) {
        log_warn("unknown layout id '" + id + "', falling back to " + fallback_id());
    }
    return fallback();
}

}  // namespace ds
