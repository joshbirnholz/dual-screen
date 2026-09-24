// Save point files: what goes in comes back out, a damaged file is refused
// rather than handed to a core, and the picture is both screens side by side.
// The menu that makes and loads them is tested in test_quick_settings.

#include "test_support.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <stdlib.h>

#include "config/paths.hpp"
#include "core/save_points.hpp"
#include "core/screen_source.hpp"

using namespace ds;

namespace {

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-savepoints-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

// Two flat screens, a red top and a blue bottom, handed back in whichever
// byte order the test asks for.
class FlatScreens : public ScreenSource {
public:
    FlatScreens(int top_width, int bottom_width, int height, ChannelOrder order)
        : top_(static_cast<std::size_t>(top_width) * height),
          bottom_(static_cast<std::size_t>(bottom_width) * height),
          top_width_(top_width),
          bottom_width_(bottom_width),
          height_(height),
          order_(order) {
        const uint32_t red = order == ChannelOrder::Rgba ? 0xFF0000FFu : 0x00FF0000u;
        const uint32_t blue = order == ChannelOrder::Rgba ? 0xFFFF0000u : 0x000000FFu;
        std::fill(top_.begin(), top_.end(), red);
        std::fill(bottom_.begin(), bottom_.end(), blue);
    }

    bool advance_frame(std::string*) override { return true; }
    const ScreenTextures& screens() override { return textures_; }
    SystemScreens screens_geometry() const override { return SystemScreens{}; }
    bool read_screen(ScreenId screen, ScreenReadback& out, int, int) override {
        const bool top = screen == ScreenId::Top;
        out.pixels = top ? top_.data() : bottom_.data();
        out.width = top ? top_width_ : bottom_width_;
        out.height = height_;
        out.stride_pixels = out.width;
        out.channel_order = order_;
        return true;
    }

private:
    ScreenTextures textures_;
    std::vector<uint32_t> top_;
    std::vector<uint32_t> bottom_;
    int top_width_;
    int bottom_width_;
    int height_;
    ChannelOrder order_;
};

std::vector<uint8_t> sample_state() {
    std::vector<uint8_t> state(200000);
    for (std::size_t index = 0; index < state.size(); ++index) {
        state[index] = static_cast<uint8_t>((index * 7 + index / 300) & 0xFF);
    }
    return state;
}

void test_paths() {
    CHECK_STR_EQ(save_point_path("/saves", "/games/Mario Kart DS.nds", 0),
                 std::string("/saves/Mario Kart DS.savestate1"));
    CHECK_STR_EQ(save_point_path("/saves", "/games/Mario Kart DS.nds", 3),
                 std::string("/saves/Mario Kart DS.savestate4"));
}

// Slots written under their first name, .savepoint1, are picked up under
// the new one -- and a slot that already has a new file keeps it.
void test_old_names_are_adopted(const std::string& sandbox) {
    const std::string rom = "/games/Old Game.nds";
    const std::string old_first = path_join(sandbox, "Old Game.savepoint1");
    const std::string old_second = path_join(sandbox, "Old Game.savepoint2");
    std::string error;
    CHECK(write_save_point(old_first, std::vector<uint8_t>(64, 1), SavePointThumbnail{}, 10,
                           &error));
    CHECK(write_save_point(old_second, std::vector<uint8_t>(64, 2), SavePointThumbnail{}, 20,
                           &error));
    CHECK(write_save_point(save_point_path(sandbox, rom, 1), std::vector<uint8_t>(64, 3),
                           SavePointThumbnail{}, 30, &error));

    adopt_old_save_point_names(sandbox, rom);
    SavePointInfo info;
    CHECK(read_save_point_info(save_point_path(sandbox, rom, 0), info));
    CHECK_EQ(info.saved_at, static_cast<int64_t>(10));
    CHECK(!file_exists(old_first));
    CHECK(read_save_point_info(save_point_path(sandbox, rom, 1), info));
    CHECK_EQ(info.saved_at, static_cast<int64_t>(30));
}

void test_round_trip(const std::string& sandbox) {
    const std::string path = path_join(sandbox, "game.savepoint1");
    SavePointInfo info;
    CHECK(!read_save_point_info(path, info));
    CHECK(!info.exists);

    SavePointThumbnail thumbnail;
    thumbnail.width = 3;
    thumbnail.height = 2;
    thumbnail.pixels = {0x00112233u, 0x00445566u, 0x00778899u,
                        0x00AABBCCu, 0x00DDEEFFu, 0x00010203u};
    const std::vector<uint8_t> state = sample_state();
    std::string error;
    CHECK(write_save_point(path, state, thumbnail, 1749120960, &error));
    CHECK_STR_EQ(error, std::string());

    CHECK(read_save_point_info(path, info));
    CHECK(info.exists);
    CHECK_EQ(info.saved_at, static_cast<int64_t>(1749120960));
    CHECK_EQ(info.thumbnail.width, 3);
    CHECK_EQ(info.thumbnail.height, 2);
    CHECK(info.thumbnail.pixels == thumbnail.pixels);

    std::vector<uint8_t> loaded;
    CHECK(read_save_point_state(path, loaded, &error));
    CHECK(loaded == state);

    // Overwritten in place: the new one, whole.
    std::vector<uint8_t> second(5000, 0x5A);
    CHECK(write_save_point(path, second, SavePointThumbnail{}, 1749121000, &error));
    CHECK(read_save_point_info(path, info));
    CHECK_EQ(info.saved_at, static_cast<int64_t>(1749121000));
    CHECK(!info.thumbnail.valid());
    CHECK(read_save_point_state(path, loaded, &error));
    CHECK(loaded == second);

    // Nothing is left behind beside it.
    CHECK(!file_exists(path + ".tmp"));
}

void test_damage_refused(const std::string& sandbox) {
    const std::string path = path_join(sandbox, "damaged.savepoint1");
    std::string error;
    CHECK(write_save_point(path, sample_state(), SavePointThumbnail{}, 1, &error));

    // Cut short halfway through the state.
    std::vector<char> whole;
    {
        std::ifstream in(path, std::ios::binary);
        whole.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(whole.data(), static_cast<std::streamsize>(whole.size() / 2));
    }
    std::vector<uint8_t> loaded;
    CHECK(!read_save_point_state(path, loaded, &error));
    CHECK(!error.empty());
    CHECK(loaded.empty());

    // Not a save point at all.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "this is a cartridge save, not a save point";
    }
    SavePointInfo info;
    CHECK(!read_save_point_info(path, info));
    CHECK(!read_save_point_state(path, loaded, &error));

    // Nothing to save is refused before anything is written.
    const std::string empty_path = path_join(sandbox, "empty.savepoint1");
    CHECK(!write_save_point(empty_path, {}, SavePointThumbnail{}, 1, &error));
    CHECK(!file_exists(empty_path));
}

void test_thumbnail(ChannelOrder order) {
    // A 3DS's pair: a wider top than bottom, both 240 tall.
    FlatScreens screens(400, 320, 240, order);
    const SavePointThumbnail thumbnail = capture_save_point_thumbnail(screens, 96);
    CHECK(thumbnail.valid());
    // One above the other: 96 + a 4-pixel seam + 96, as wide as the wider.
    CHECK_EQ(thumbnail.height, 96 + 4 + 96);
    CHECK_EQ(thumbnail.width, 160);
    CHECK_EQ(thumbnail.pixels[10], 0x00FF0000u);
    // The seam.
    CHECK_EQ(thumbnail.pixels[static_cast<std::size_t>(98) * thumbnail.width + 80], 0u);
    // The bottom screen, 128 wide, centred under the top: 16 either side.
    CHECK_EQ(thumbnail.pixels[static_cast<std::size_t>(150) * thumbnail.width + 80], 0x000000FFu);
    CHECK_EQ(thumbnail.pixels[static_cast<std::size_t>(150) * thumbnail.width + 8], 0u);
    CHECK_EQ(thumbnail.pixels[static_cast<std::size_t>(150) * thumbnail.width + 20], 0x000000FFu);
}

void test_time() {
    // In UTC, so the clock reads the same wherever the test runs: 1749120960
    // is 5 June 2025, 10:56, and 1749082200 is 00:10 the same morning.
    setenv("TZ", "UTC", 1);
    tzset();
    TimeStyle american;
    american.order = TimeStyle::Order::MonthDayYear;
    american.twelve_hour = true;
    CHECK_STR_EQ(format_save_time(1749120960, american), std::string("06/05/2025 - 10:56 AM"));
    // Past noon, and the hour after midnight, which a 12-hour clock calls 12.
    CHECK_STR_EQ(format_save_time(1749120960 + 6 * 3600, american),
                 std::string("06/05/2025 - 4:56 PM"));
    CHECK_STR_EQ(format_save_time(1749082200, american), std::string("06/05/2025 - 12:10 AM"));

    TimeStyle german;
    german.order = TimeStyle::Order::DayMonthYear;
    german.separator = '.';
    CHECK_STR_EQ(format_save_time(1749120960, german), std::string("05.06.2025 - 10:56"));

    TimeStyle japanese;
    japanese.order = TimeStyle::Order::YearMonthDay;
    CHECK_STR_EQ(format_save_time(1749120960, japanese), std::string("2025/06/05 - 10:56"));

    // What a device that only says where it is gets.
    const TimeStyle us = time_style_for_region("en", "US");
    CHECK(us.order == TimeStyle::Order::MonthDayYear && us.twelve_hour);
    const TimeStyle uk = time_style_for_region("en", "GB");
    CHECK(uk.order == TimeStyle::Order::DayMonthYear && !uk.twelve_hour && uk.separator == '/');
    const TimeStyle de = time_style_for_region("de", "DE");
    CHECK(de.order == TimeStyle::Order::DayMonthYear && !de.twelve_hour && de.separator == '.');
    const TimeStyle jp = time_style_for_region("ja", "");
    CHECK(jp.order == TimeStyle::Order::YearMonthDay && !jp.twelve_hour);
    const TimeStyle quebec = time_style_for_region("fr", "CA");
    CHECK(!quebec.twelve_hour);
    const TimeStyle au = time_style_for_region("en", "au");
    CHECK(au.order == TimeStyle::Order::DayMonthYear && au.twelve_hour);
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_paths();
    test_round_trip(sandbox);
    test_old_names_are_adopted(sandbox);
    test_damage_refused(sandbox);
    test_thumbnail(ChannelOrder::XrgbBytes);
    test_thumbnail(ChannelOrder::Rgba);
    test_time();
    return test::finish("save points");
}
