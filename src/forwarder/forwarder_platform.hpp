#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "forwarder/steamgriddb.hpp"

namespace ds {

// A forwarder: a small title installed on the Switch's HOME menu that starts
// Dualscreen straight into one game -- what Sphaira and others make for any
// .nro and its arguments, made here for a ROM from the file picker.
struct ForwarderRequest {
    std::string rom_path;
    // What the HOME menu says: the game's name and who made it.
    std::string title;
    std::string publisher;
    // A 256x256 JPEG (see forwarder_icon.hpp).
    std::vector<uint8_t> icon;
    // Whether the console's own capture button works in it.
    bool screenshots = true;
    bool video_capture = true;
};

// The longest a title and a publisher can be, in bytes: what a title's
// control data has room for.
constexpr std::size_t kForwarderTitleMax = 0x200 - 1;
constexpr std::size_t kForwarderPublisherMax = 0x100 - 1;

// What only the device can do: type with its keyboard, reach the network,
// and install the title. The Switch's is in companion/source; nowhere else
// makes forwarders, and ForwarderScreen is only ever opened where there is
// one of these.
class ForwarderPlatform {
public:
    virtual ~ForwarderPlatform() = default;

    // Asks for a line of text, starting from `initial`. False when the player
    // backed out of the keyboard.
    virtual bool edit_text(const std::string& header, const std::string& guide,
                           const std::string& initial, std::size_t max_length,
                           std::string& out) = 0;
    // Called off the main thread.
    virtual HttpResponse http_get(const std::string& url, const std::string& bearer) = 0;
    // Builds the forwarder and installs it. Called off the main thread.
    virtual bool install(const ForwarderRequest& request, std::string* error) = 0;
};

}  // namespace ds
