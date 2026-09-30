// A forwarder's icon and the SteamGridDB search that can find one: the
// pictures come out as the 256x256 JPEG a HOME menu shows, and a search asks
// the right questions and makes sense of the answers -- with an HTTP stand-in,
// since the real one is the Switch's.

#include "test_support.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include "core/ds_rom_info.hpp"
#include "forwarder/forwarder_icon.hpp"
#include "forwarder/forwarder_platform.hpp"
#include "forwarder/steamgriddb.hpp"
#include "ui/forwarder_screen.hpp"

using namespace ds;

namespace {

std::vector<uint8_t> bytes_of(const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A DS banner icon: every pixel colour 1, colour 1 pure red.
std::vector<uint8_t> red_ds_icon() {
    std::vector<uint8_t> icon(512 + 32, 0x11);
    std::memset(icon.data() + 512, 0, 32);
    icon[512 + 2] = 0x1F;  // colour 1, BGR555 little-endian: red
    icon[512 + 3] = 0x00;
    return icon;
}

bool is_icon(const std::vector<uint8_t>& jpeg, RgbaImage& decoded) {
    return jpeg.size() > 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8 && decode_image(jpeg, decoded) &&
           decoded.width == kForwarderIconSize && decoded.height == kForwarderIconSize;
}

void test_the_ds_banner_becomes_a_menu_icon() {
    RgbaImage decoded;
    CHECK(is_icon(forwarder_icon_from_ds_banner(red_ds_icon()), decoded));
    const uint8_t* middle = decoded.pixels.data() + (128 * kForwarderIconSize + 128) * 4;
    CHECK_MSG(middle[0] > 200 && middle[1] < 60 && middle[2] < 60, "the icon lost its colour");
    CHECK(forwarder_icon_from_ds_banner({}).empty());
    CHECK(is_icon(forwarder_icon_blank(), decoded));
}

void test_a_downloaded_picture_is_squared_and_scaled() {
    // A wide picture, as a JPEG: the icon is its middle, at 256x256.
    RgbaImage wide;
    wide.width = 600;
    wide.height = 300;
    wide.pixels.assign(600 * 300 * 4, 255);
    for (int y = 0; y < 300; ++y) {
        for (int x = 150; x < 450; ++x) {
            uint8_t* pixel = wide.pixels.data() + (y * 600 + x) * 4;
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 255;
        }
    }
    RgbaImage decoded;
    CHECK(is_icon(forwarder_icon_from_image(encode_jpeg(wide)), decoded));
    // Blue to its corners: the white either side was cut away.
    for (const int at : {0, 128 * kForwarderIconSize + 128, kForwarderIconSize * kForwarderIconSize - 1}) {
        const uint8_t* pixel = decoded.pixels.data() + at * 4;
        CHECK_MSG(pixel[2] > 200 && pixel[0] < 60, "the icon is not the middle of the picture");
    }
    CHECK(forwarder_icon_from_image(bytes_of("not a picture")).empty());
    CHECK(forwarder_icon_from_image({}).empty());
}

void test_the_search_asks_and_understands() {
    std::vector<std::string> asked;
    std::vector<std::string> keys;
    const std::map<std::string, std::string> answers = {
        {"https://www.steamgriddb.com/api/v2/search/autocomplete/Mario%20Kart%20DS",
         R"({"success":true,"data":[{"id":1234,"name":"Mario Kart DS"},{"id":9,"name":"Other"}]})"},
        {"https://www.steamgriddb.com/api/v2/grids/game/1234?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/jpeg",
         R"({"data":[{"url":"https://cdn/a.png"},{"url":"https://cdn/b.jpg"}]})"},
        {"https://www.steamgriddb.com/api/v2/icons/game/1234?types=static&mimes=image/png,image/jpeg",
         R"({"data":[{"url":"https://cdn/b.jpg"},{"url":"https://cdn/c.png"}]})"},
    };
    const HttpGet get = [&](const std::string& url, const std::string& bearer) {
        asked.push_back(url);
        keys.push_back(bearer);
        HttpResponse response;
        response.reached = true;
        const auto it = answers.find(url);
        response.status = it == answers.end() ? 404 : 200;
        if (it != answers.end()) response.body = bytes_of(it->second);
        return response;
    };
    const SteamGridSearch search = steamgrid_search(get, "key123", "Mario Kart DS");
    CHECK(search.error == SteamGridError::None);
    CHECK_STR_EQ(search.match_name, "Mario Kart DS");
    CHECK_EQ(search.urls.size(), static_cast<std::size_t>(3));
    if (search.urls.size() == 3) {
        CHECK_STR_EQ(search.urls[0], "https://cdn/a.png");
        CHECK_STR_EQ(search.urls[2], "https://cdn/c.png");
    }
    CHECK_EQ(asked.size(), static_cast<std::size_t>(3));
    for (const std::string& key : keys) CHECK_STR_EQ(key, "key123");

    // A key the site turns down, a game it does not know, and no network.
    const HttpGet refuses = [](const std::string&, const std::string&) {
        HttpResponse response;
        response.reached = true;
        response.status = 401;
        return response;
    };
    CHECK(steamgrid_search(refuses, "bad", "Mario Kart DS").error == SteamGridError::BadKey);
    const HttpGet empty = [](const std::string&, const std::string&) {
        HttpResponse response;
        response.reached = true;
        response.status = 200;
        response.body = bytes_of(R"({"success":true,"data":[]})");
        return response;
    };
    CHECK(steamgrid_search(empty, "key", "Nothing").error == SteamGridError::NotFound);
    const HttpGet offline = [](const std::string&, const std::string&) { return HttpResponse(); };
    CHECK(steamgrid_search(offline, "key", "Mario Kart DS").error == SteamGridError::Network);
    CHECK(!describe(SteamGridError::BadKey).empty());

    CHECK_STR_EQ(url_escape("Pokémon & Co/1"), "Pok%C3%A9mon%20%26%20Co%2F1");
}

// The Switch's part, stood in for: a keyboard that types what it is told to,
// a SteamGridDB with one game and three pictures, and an install that keeps
// what it was asked to install.
class FakePlatform : public ForwarderPlatform {
public:
    std::vector<std::string> typed;
    std::vector<std::string> headers;
    std::vector<ForwarderRequest> installed;
    std::vector<std::string> keys_used;

    bool edit_text(const std::string& header, const std::string&, const std::string&, std::size_t,
                   std::string& out) override {
        headers.push_back(header);
        if (typed.empty()) return false;
        out = typed.front();
        typed.erase(typed.begin());
        return true;
    }
    HttpResponse http_get(const std::string& url, const std::string& bearer) override {
        HttpResponse response;
        response.reached = true;
        response.status = 200;
        if (url.find("/search/autocomplete/") != std::string::npos) {
            keys_used.push_back(bearer);
            response.body = bytes_of(R"({"data":[{"id":7,"name":"Mario Kart DS"}]})");
        } else if (url.find("/grids/game/7") != std::string::npos) {
            response.body = bytes_of(R"({"data":[{"url":"https://cdn/1.jpg"},{"url":"https://cdn/2.jpg"}]})");
        } else if (url.find("/icons/game/7") != std::string::npos) {
            response.body = bytes_of(R"({"data":[{"url":"https://cdn/3.jpg"}]})");
        } else if (url.rfind("https://cdn/", 0) == 0) {
            RgbaImage picture;
            picture.width = 300;
            picture.height = 300;
            picture.pixels.assign(300 * 300 * 4, 255);
            for (std::size_t i = 0; i < picture.pixels.size(); i += 4) picture.pixels[i] = 0;  // cyan
            response.body = encode_jpeg(picture);
        } else {
            response.status = 404;
        }
        return response;
    }
    bool install(const ForwarderRequest& request, std::string*) override {
        installed.push_back(request);
        return true;
    }
};

constexpr float kW = 1280.0f;
constexpr float kH = 720.0f;

ForwarderScreenResult press(ForwarderScreen& screen, UiAction action) {
    return screen.update({action}, {}, kW, kH);
}

// Until the job running off the main thread has come back.
ForwarderScreenResult settle(ForwarderScreen& screen) {
    ForwarderScreenResult last;
    for (int tries = 0; tries < 1000 && (screen.busy() || screen.overlay() == ForwarderScreen::Overlay::Busy); ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        last = screen.update({}, {}, kW, kH);
    }
    return last;
}

void test_a_forwarder_is_made_from_a_rom() {
    FakePlatform platform;
    ForwarderScreen screen(platform);
    DsRomInfo info;
    info.game_id = "AMCE-01-0-1a2b";
    info.title = "Mario Kart DS";
    info.publisher = "Nintendo";
    info.icon = red_ds_icon();
    screen.open("sdmc:/roms/nds/Mario Kart DS.nds", info, "");

    // The ROM's own name, publisher and icon, to start with.
    CHECK_STR_EQ(screen.request().title, "Mario Kart DS");
    CHECK_STR_EQ(screen.request().publisher, "Nintendo");
    CHECK_STR_EQ(screen.icon_source(), "ROM Icon");
    CHECK(!screen.request().icon.empty());
    CHECK(screen.zone() == ForwarderScreen::Zone::Icon);

    // The icon: from SteamGridDB, which asks for the API key first.
    press(screen, UiAction::Confirm);
    CHECK(screen.overlay() == ForwarderScreen::Overlay::IconSource);
    platform.typed = {"my-key"};
    const ForwarderScreenResult asked = press(screen, UiAction::Confirm);
    CHECK(asked.api_key_changed);
    CHECK_STR_EQ(asked.api_key, "my-key");
    settle(screen);
    CHECK(screen.page() == ForwarderScreen::Page::Icons);
    CHECK_EQ(screen.icon_count(), static_cast<std::size_t>(3));
    CHECK_EQ(platform.keys_used.size(), static_cast<std::size_t>(1));
    if (!platform.keys_used.empty()) CHECK_STR_EQ(platform.keys_used[0], "my-key");
    press(screen, UiAction::Right);
    press(screen, UiAction::Confirm);
    CHECK(screen.page() == ForwarderScreen::Page::Editor);
    CHECK_STR_EQ(screen.icon_source(), "SteamGridDB");
    RgbaImage chosen;
    CHECK(decode_image(screen.request().icon, chosen));
    CHECK_EQ(chosen.width, kForwarderIconSize);

    // The title, typed over.
    press(screen, UiAction::Up);
    CHECK(screen.zone() == ForwarderScreen::Zone::Title);
    platform.typed = {"Mario Kart"};
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.request().title, "Mario Kart");

    // Screenshots off, and Create.
    press(screen, UiAction::Right);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    CHECK(!screen.request().screenshots);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.list_focus(), static_cast<int>(ForwarderScreen::Row::Create));
    press(screen, UiAction::Confirm);
    settle(screen);
    CHECK_EQ(platform.installed.size(), static_cast<std::size_t>(1));
    if (!platform.installed.empty()) {
        const ForwarderRequest& made = platform.installed[0];
        CHECK_STR_EQ(made.rom_path, "sdmc:/roms/nds/Mario Kart DS.nds");
        CHECK_STR_EQ(made.title, "Mario Kart");
        CHECK_STR_EQ(made.publisher, "Nintendo");
        CHECK(!made.screenshots);
        CHECK(made.video_capture);
        CHECK(made.icon == screen.request().icon);
    }
    CHECK(screen.overlay() == ForwarderScreen::Overlay::Notice);
    const ForwarderScreenResult done = press(screen, UiAction::Confirm);
    CHECK(done.close_requested);
    CHECK(!screen.is_open());
}

void test_no_banner_and_no_key() {
    FakePlatform platform;
    ForwarderScreen screen(platform);
    DsRomInfo info;
    screen.open("sdmc:/roms/nds/Homebrew Thing.nds", info, "");
    // The file's name, and a plain icon until another is chosen.
    CHECK_STR_EQ(screen.request().title, "Homebrew Thing");
    CHECK(!screen.request().icon.empty());
    // SteamGridDB with the keyboard backed out of: said why, nothing searched.
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Confirm);
    CHECK(screen.overlay() == ForwarderScreen::Overlay::Notice);
    CHECK(platform.keys_used.empty());
    press(screen, UiAction::Confirm);
    // And B leaves.
    CHECK(press(screen, UiAction::Cancel).close_requested);
}

}  // namespace

int main() {
    test_a_forwarder_is_made_from_a_rom();
    test_no_banner_and_no_key();
    test_the_ds_banner_becomes_a_menu_icon();
    test_a_downloaded_picture_is_squared_and_scaled();
    test_the_search_asks_and_understands();
    return test::finish("forwarder");
}
