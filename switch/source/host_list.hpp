#pragma once

#include <string>
#include <vector>

#include "net/discovery.hpp"

namespace ds::sw {

// The host picker's model: which of the hosts currently being heard from is
// focused, and which screen this device is asking to be.
//
// Kept apart from the drawing because the part worth getting right is what
// happens as the list churns. Hosts appear and go quiet on their own schedule,
// with no regard for where the focus is sitting, so a focus that is only an
// index into that list quietly ends up on a different machine than the one the
// player was looking at.
class HostList {
public:
    void set_wanted_screen(ScreenId screen) { wanted_screen_ = screen; }
    ScreenId wanted_screen() const { return wanted_screen_; }

    // Takes the browser's current list, keeping the focus on the same host
    // wherever it moved to. A host that has gone quiet leaves the focus at the
    // position it held, which is the nearest thing to "where you were".
    void update(const std::vector<net::DiscoveredHost>& hosts);

    void move_focus(int delta);

    const std::vector<net::DiscoveredHost>& hosts() const { return hosts_; }
    bool empty() const { return hosts_.empty(); }
    int focus() const { return focus_; }
    // The focused host, or null while nothing is being heard from.
    const net::DiscoveredHost* focused() const;
    // Whether the focused host still has the screen this device is asking for.
    // The other one may be taken by somebody else's device already.
    bool focused_is_joinable() const;
    // The first host with that screen free, for the command line's --connect,
    // which joins without anybody choosing.
    const net::DiscoveredHost* first_joinable() const;

private:
    std::vector<net::DiscoveredHost> hosts_;
    // The identity the focus belongs to, rather than the row it was on.
    std::string focus_address_;
    int focus_ = 0;
    ScreenId wanted_screen_ = ScreenId::Bottom;
};

}  // namespace ds::sw
