#include "host_list.hpp"

#include <algorithm>

namespace ds::sw {

void HostList::update(const std::vector<net::DiscoveredHost>& hosts) {
    hosts_ = hosts;
    if (hosts_.empty()) {
        focus_ = 0;
        focus_address_.clear();
        return;
    }

    if (!focus_address_.empty()) {
        for (std::size_t index = 0; index < hosts_.size(); ++index) {
            if (hosts_[index].address == focus_address_) {
                focus_ = static_cast<int>(index);
                return;
            }
        }
    }
    // Either nothing was focused yet, or the host that was has gone quiet. The
    // position it held is the closest thing left to where the player was.
    focus_ = std::clamp(focus_, 0, static_cast<int>(hosts_.size()) - 1);
    focus_address_ = hosts_[static_cast<std::size_t>(focus_)].address;
}

void HostList::move_focus(int delta) {
    if (hosts_.empty()) {
        return;
    }
    const int count = static_cast<int>(hosts_.size());
    focus_ = ((focus_ + delta) % count + count) % count;
    focus_address_ = hosts_[static_cast<std::size_t>(focus_)].address;
}

const net::DiscoveredHost* HostList::focused() const {
    if (hosts_.empty() || focus_ < 0 || focus_ >= static_cast<int>(hosts_.size())) {
        return nullptr;
    }
    return &hosts_[static_cast<std::size_t>(focus_)];
}

bool HostList::focused_is_joinable() const {
    const net::DiscoveredHost* host = focused();
    return host != nullptr && host->offers(wanted_screen_);
}

const net::DiscoveredHost* HostList::first_joinable() const {
    for (const net::DiscoveredHost& host : hosts_) {
        if (host.offers(wanted_screen_)) {
            return &host;
        }
    }
    return nullptr;
}

}  // namespace ds::sw
