#pragma once

#include <string>
#include <vector>

#include "layout/layout.hpp"

namespace ds {

// The set of layouts the player can pick from. Adding a layout is meant to be
// adding data here — nothing downstream special-cases a particular layout id.
class LayoutRegistry {
public:
    LayoutRegistry();

    const std::vector<Layout>& layouts() const { return layouts_; }
    std::size_t size() const { return layouts_.size(); }

    // Returns nullptr when no layout has this id.
    const Layout* find(const std::string& id) const;
    // Returns the index of the layout with this id, or -1.
    int index_of(const std::string& id) const;
    const Layout& at(std::size_t index) const;

    // The layout used when a game has no saved choice and settings name none.
    const Layout& fallback() const;
    static const char* fallback_id();
    // Old ids from before the layout set was replaced with the Wii U's, mapped
    // to whichever new layout is closest. Returns the id unchanged when it is
    // not one of them.
    static std::string migrated_id(const std::string& id);

    // Resolves an id to a layout, falling back (with a log line) if unknown.
    const Layout& resolve(const std::string& id) const;

private:
    std::vector<Layout> layouts_;
};

// Where L3 goes from a layout -- the one it pairs with -- and whether the
// stylus hand goes with it. It does wherever both have a hand, so flipping
// between the two focus layouts never also flips which side the stylus is on:
// the player's hand has not changed because the layout did.
struct LayoutSwap {
    std::string layout_id;  // empty where the layout pairs with nothing
    bool keeps_hand = false;
};
LayoutSwap layout_swap_from(const LayoutRegistry& registry, const Layout& from);

}  // namespace ds
