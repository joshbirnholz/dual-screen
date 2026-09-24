#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

class ScreenSource;

// Save points: the whole running console written down at one moment, to be
// gone back to later -- what Nintendo Switch Online calls a suspend point.
// Nothing to do with the game's own save, which lives in the cartridge's
// .srm beside these and carries on being written the way it always has.
//
// A handful of numbered slots per game, each one file beside the game's
// save: the core's state, when it was made, and a small picture of both
// screens so the player can tell one from another without loading it.

inline constexpr int kSavePointSlots = 4;

// Both screens side by side, shrunk: 0x00RRGGBB, tightly packed.
struct SavePointThumbnail {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    bool valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

// What the slot list shows: everything but the state itself, which is by far
// the biggest part and only wanted when the slot is loaded.
struct SavePointInfo {
    bool exists = false;
    // Seconds since the epoch.
    int64_t saved_at = 0;
    SavePointThumbnail thumbnail;
};

// Where slot `slot` (from 0) for the game at `rom_path` lives:
// "<saves>/<game>.savepoint1" for the first.
std::string save_point_path(const std::string& saves_dir, const std::string& rom_path, int slot);

// Writes a save point in one go, through a temporary file, so a slot is
// either the old one or the new one and never half of each.
bool write_save_point(const std::string& path,
                      const std::vector<uint8_t>& state,
                      const SavePointThumbnail& thumbnail,
                      int64_t saved_at,
                      std::string* error);

// The slot's picture and time. False, with `out.exists` false, for an empty
// slot or one that cannot be read.
bool read_save_point_info(const std::string& path, SavePointInfo& out);

// The state itself, to hand the core.
bool read_save_point_state(const std::string& path, std::vector<uint8_t>& out,
                           std::string* error);

// Both of `source`'s screens as they are now, side by side and `height`
// pixels tall. Invalid when there is nothing to read.
SavePointThumbnail capture_save_point_thumbnail(ScreenSource& source, int height = 96);

// "06/05/2025 - 10:56", in local time.
std::string format_save_time(int64_t saved_at);

}  // namespace ds
