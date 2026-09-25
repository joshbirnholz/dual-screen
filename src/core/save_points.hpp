#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ds {

class ScreenSource;

// Save points -- "save states" to the player, which is what the menu calls
// them: the whole running console written down at one moment, to be gone
// back to later -- what Nintendo Switch Online calls a suspend point.
// Nothing to do with the game's own save, which lives in the cartridge's
// .srm beside these and carries on being written the way it always has.
//
// A handful of numbered slots per game, each one file beside the game's
// save: the core's state, when it was made, and a small picture of both
// screens so the player can tell one from another without loading it.

inline constexpr int kSavePointSlots = 4;

// Both screens one above the other, shrunk: 0x00RRGGBB, tightly packed.
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
// "<saves>/<game>.savestate1" for the first.
std::string save_point_path(const std::string& saves_dir, const std::string& rom_path, int slot);

// Where the resume point for the game at `rom_path` lives:
// "<saves>/<game>.resume". A save point like the numbered ones, and read and
// written with the same calls, but never shown in the slot list: it is kept
// up to date on its own while the game plays, for "Resume from Save State" --
// see App::Impl::write_resume_point().
std::string resume_point_path(const std::string& saves_dir, const std::string& rom_path);

// Renames a game's slots from the name they were first written under,
// "<game>.savepoint1", to the one they have now, wherever a slot has only
// the old one. Harmless to call every time.
void adopt_old_save_point_names(const std::string& saves_dir, const std::string& rom_path);

// Writes a save point in one go, through a temporary file, so a slot is
// either the old one or the new one and never half of each.
bool write_save_point(const std::string& path,
                      const std::vector<uint8_t>& state,
                      const SavePointThumbnail& thumbnail,
                      int64_t saved_at,
                      std::string* error);

// Writes save points on a thread of its own, so the game only pays for
// taking the state -- a few milliseconds -- and not for compressing it and
// putting it on disk, which for a resume point written every half minute
// while the game runs would be a stutter every half minute.
//
// One write at a time. A write asked for while another is still waiting to
// start replaces it: only the newest state of a game is worth having.
class BackgroundSavePointWriter {
public:
    BackgroundSavePointWriter();
    // Finishes whatever is queued first: a resume point asked for on the way
    // out is the one that matters most.
    ~BackgroundSavePointWriter();
    BackgroundSavePointWriter(const BackgroundSavePointWriter&) = delete;
    BackgroundSavePointWriter& operator=(const BackgroundSavePointWriter&) = delete;

    void write(std::string path, std::vector<uint8_t> state, int64_t saved_at);
    // Waits until nothing is queued or being written.
    void finish();
    // Forgets a write not yet started, and waits out one that has.
    void cancel();
    // Writes that failed since the last ask, with the reason for the last.
    unsigned take_failures(std::string* last_error);

private:
    struct Job {
        std::string path;
        std::vector<uint8_t> state;
        int64_t saved_at = 0;
    };
    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    bool has_job_ = false;
    Job job_;
    bool busy_ = false;
    bool stopping_ = false;
    unsigned failures_ = 0;
    std::string last_error_;
    std::thread thread_;
};

// The slot's picture and time. False, with `out.exists` false, for an empty
// slot or one that cannot be read.
bool read_save_point_info(const std::string& path, SavePointInfo& out);

// The state itself, to hand the core.
bool read_save_point_state(const std::string& path, std::vector<uint8_t>& out,
                           std::string* error);

// Both of `source`'s screens as they are now, one above the other, each
// `height` pixels tall. Invalid when there is nothing to read.
SavePointThumbnail capture_save_point_thumbnail(ScreenSource& source, int height = 96);

// How this device writes a date and a time: which way round the day, month
// and year go, what sits between them, and whether the clock is 12-hour.
struct TimeStyle {
    enum class Order { MonthDayYear, DayMonthYear, YearMonthDay };
    Order order = Order::MonthDayYear;
    char separator = '/';
    bool twelve_hour = false;
};

// Asked of the device once: on Apple's systems its region and its own 24-Hour
// Time switch; on Linux -- SteamOS included -- the locale's own date and time
// formats; on a Switch its system language; elsewhere the language SDL says
// is preferred. Each device asks for itself, so a companion shows a save's
// time the way that device writes times, whatever the host does.
TimeStyle device_time_style();

// What a region writes, from a language and a country code ("en", "US"),
// either of which may be empty. For a device that only says where it is.
TimeStyle time_style_for_region(const std::string& language, const std::string& country);

// "06/05/2025 - 10:56 AM", "05.06.2025 - 10:56", in local time.
std::string format_save_time(int64_t saved_at, const TimeStyle& style);
std::string format_save_time(int64_t saved_at);

}  // namespace ds
