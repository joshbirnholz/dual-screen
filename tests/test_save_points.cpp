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
                 std::string("/saves/Mario Kart DS.savepoint1"));
    CHECK_STR_EQ(save_point_path("/saves", "/games/Mario Kart DS.nds", 3),
                 std::string("/saves/Mario Kart DS.savepoint4"));
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
    CHECK_EQ(thumbnail.height, 96);
    // 160 + a 4-pixel seam + 128.
    CHECK_EQ(thumbnail.width, 160 + 4 + 128);
    CHECK_EQ(thumbnail.pixels[10], 0x00FF0000u);
    CHECK_EQ(thumbnail.pixels[162], 0u);
    CHECK_EQ(thumbnail.pixels[static_cast<std::size_t>(95) * thumbnail.width + 200], 0x000000FFu);
}

void test_time() {
    // Whatever the zone, the shape is the NSO one: "06/05/2025 - 10:56".
    const std::string text = format_save_time(1749120960);
    CHECK_EQ(text.size(), static_cast<std::size_t>(18));
    CHECK_STR_EQ(text.substr(2, 1), std::string("/"));
    CHECK_STR_EQ(text.substr(10, 3), std::string(" - "));
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_paths();
    test_round_trip(sandbox);
    test_damage_refused(sandbox);
    test_thumbnail(ChannelOrder::XrgbBytes);
    test_thumbnail(ChannelOrder::Rgba);
    test_time();
    return test::finish("save points");
}
