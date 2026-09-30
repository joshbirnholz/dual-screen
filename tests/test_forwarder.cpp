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
    // A wide picture, as a JPEG: blue in the middle, white either side. All
    // of it makes the icon, across its middle, over a dimmed blur of itself.
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
    const auto at = [&](int x, int y) { return decoded.pixels.data() + (y * kForwarderIconSize + x) * 4; };
    CHECK_MSG(at(128, 128)[2] > 200 && at(128, 128)[0] < 60, "the middle of the picture is not in the middle");
    CHECK_MSG(at(6, 128)[0] > 200 && at(6, 128)[2] > 200, "the picture's white edge was cut away");
    CHECK_MSG(at(128, 8)[2] > 60 && at(128, 8)[2] < 190 && at(128, 8)[0] < 60,
              "above the picture is not a dimmed blur of it");

    // A nearly square one: its middle, to the corners.
    RgbaImage squarish;
    squarish.width = 300;
    squarish.height = 270;
    squarish.pixels.assign(300 * 270 * 4, 255);
    for (std::size_t i = 0; i < squarish.pixels.size(); i += 4) squarish.pixels[i] = 0;  // cyan
    CHECK(is_icon(forwarder_icon_from_image(encode_jpeg(squarish)), decoded));
    CHECK(at(0, 0)[0] < 40 && at(0, 0)[1] > 200);

    CHECK(forwarder_icon_from_image(bytes_of("not a picture")).empty());
    CHECK(forwarder_icon_from_image({}).empty());
}

// A Windows icon of one 2x2 32-bit picture, as SteamGridDB serves many
// icons: red on top, blue underneath.
std::vector<uint8_t> tiny_ico() {
    std::vector<uint8_t> ico = {0, 0, 1, 0, 1, 0,                     // ICONDIR: an icon, one picture
                                2, 2, 0, 0, 1, 0, 32, 0, 0, 0, 0, 0,  // 2x2, 1 plane, 32 bits, size...
                                22, 0, 0, 0};                         // ...at 22
    std::vector<uint8_t> dib(40, 0);
    dib[0] = 40;
    dib[4] = 2;   // width
    dib[8] = 4;   // height: the colours and the mask
    dib[12] = 1;  // planes
    dib[14] = 32;
    const uint8_t blue[4] = {255, 0, 0, 255};  // BGRA
    const uint8_t red[4] = {0, 0, 255, 255};
    for (int i = 0; i < 2; ++i) dib.insert(dib.end(), blue, blue + 4);  // bottom row first
    for (int i = 0; i < 2; ++i) dib.insert(dib.end(), red, red + 4);
    dib.insert(dib.end(), 8, 0);  // the mask, two rows of four bytes
    const uint32_t size = static_cast<uint32_t>(dib.size());
    std::memcpy(ico.data() + 14, &size, 4);
    ico.insert(ico.end(), dib.begin(), dib.end());
    return ico;
}

void test_an_ico_is_read() {
    RgbaImage image;
    CHECK(decode_image(tiny_ico(), image));
    CHECK_EQ(image.width, 2);
    CHECK_EQ(image.height, 2);
    if (image.pixels.size() == 16) {
        CHECK(image.pixels[0] == 255 && image.pixels[2] == 0);   // top left: red
        CHECK(image.pixels[8] == 0 && image.pixels[10] == 255);  // bottom left: blue
    }
    RgbaImage decoded;
    CHECK(is_icon(forwarder_icon_from_image(tiny_ico()), decoded));
}

HttpGet answering(const std::map<std::string, std::string>& answers, std::vector<std::string>* asked = nullptr,
                  std::vector<std::string>* keys = nullptr) {
    return [answers, asked, keys](const std::string& url, const std::string& bearer) {
        if (asked != nullptr) asked->push_back(url);
        if (keys != nullptr) keys->push_back(bearer);
        HttpResponse response;
        response.reached = true;
        const auto it = answers.find(url);
        response.status = it == answers.end() ? 404 : 200;
        if (it != answers.end()) response.body = bytes_of(it->second);
        return response;
    };
}

const std::string kApi = "https://www.steamgriddb.com/api/v2";
std::string square_grids(int id) {
    return kApi + "/grids/game/" + std::to_string(id) +
           "?dimensions=512x512,1024x1024&types=static&mimes=image/png,image/jpeg";
}
std::string icons(int id) {
    return kApi + "/icons/game/" + std::to_string(id) + "?types=static&mimes=image/png,image/vnd.microsoft.icon";
}
std::string other_grids(int id) {
    return kApi + "/grids/game/" + std::to_string(id) +
           "?dimensions=600x900,342x482,660x930,460x215,920x430&types=static&mimes=image/png,image/jpeg";
}

void test_the_search_asks_and_understands() {
    std::vector<std::string> asked;
    std::vector<std::string> keys;
    // The name escaped twice, as SteamGridDB's own plugin sends it; icons
    // asked for as PNG or ICO, which is what the site has.
    const HttpGet get = answering(
        {
            {kApi + "/search/autocomplete/Mario%2520Kart%2520DS",
             R"({"success":true,"data":[{"id":1234,"name":"Mario Kart DS","release_date":1100000000},{"id":9,"name":"Other"}]})"},
            {square_grids(1234), R"({"data":[{"url":"https://cdn/a.png"},{"url":"https://cdn/b.jpg"}]})"},
            {icons(1234), R"({"data":[{"url":"https://cdn/b.jpg"},{"url":"https://cdn/c.ico"}]})"},
            {other_grids(1234), R"({"data":[{"url":"https://cdn/d.jpg"}]})"},
        },
        &asked, &keys);
    const SteamGridSearch search = steamgrid_search(get, "key123", "Mario Kart DS");
    CHECK(search.error == SteamGridError::None);
    CHECK_STR_EQ(search.match.name, "Mario Kart DS");
    CHECK_EQ(search.match.year, 2004);
    CHECK_EQ(search.games.size(), static_cast<std::size_t>(2));
    CHECK_EQ(search.urls.size(), static_cast<std::size_t>(4));
    if (search.urls.size() == 4) {
        CHECK_STR_EQ(search.urls[0], "https://cdn/a.png");
        CHECK_STR_EQ(search.urls[2], "https://cdn/c.ico");
        CHECK_STR_EQ(search.urls[3], "https://cdn/d.jpg");
    }
    CHECK_EQ(asked.size(), static_cast<std::size_t>(4));
    for (const std::string& key : keys) CHECK_STR_EQ(key, "key123");

    // The first game matched has no pictures: the next one's are shown.
    const SteamGridSearch next = steamgrid_search(
        answering({
            {kApi + "/search/autocomplete/Mario%2520Kart%2520DS",
             R"({"data":[{"id":1234,"name":"Mario Kart DS"},{"id":9,"name":"Mario Kart DS Demo"}]})"},
            {icons(9), R"({"data":[{"url":"https://cdn/e.png"}]})"},
        }),
        "key", "Mario Kart DS");
    CHECK(next.error == SteamGridError::None);
    CHECK_EQ(next.match.id, static_cast<int64_t>(9));

    // A name it does not know, tidied and then made simpler until it does.
    std::vector<std::string> tried;
    const SteamGridSearch simpler = steamgrid_search(
        answering({{kApi + "/search/autocomplete/Mario%2520Kart", R"({"data":[{"id":5,"name":"Mario Kart"}]})"},
                   {square_grids(5), R"({"data":[{"url":"https://cdn/f.png"}]})"}},
                  &tried),
        "key", "Mario_Kart\xE2\x84\xA2 DS (USA) [!]");
    CHECK(simpler.error == SteamGridError::None);
    CHECK_STR_EQ(simpler.term, "Mario Kart");
    CHECK(!tried.empty() && tried[0] == kApi + "/search/autocomplete/Mario%2520Kart%2520DS");

    // Every game without pictures: NotFound, with the games to choose from.
    const SteamGridSearch bare = steamgrid_search(
        answering({{kApi + "/search/autocomplete/Zelda", R"({"data":[{"id":1,"name":"Zelda"},{"id":2,"name":"Zelda II"}]})"}}),
        "key", "Zelda");
    CHECK(bare.error == SteamGridError::NotFound);
    CHECK_EQ(bare.games.size(), static_cast<std::size_t>(2));

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
    const SteamGridSearch nothing = steamgrid_search(empty, "key", "Nothing");
    CHECK(nothing.error == SteamGridError::NotFound);
    CHECK(nothing.games.empty());
    const HttpGet offline = [](const std::string&, const std::string&) { return HttpResponse(); };
    CHECK(steamgrid_search(offline, "key", "Mario Kart DS").error == SteamGridError::Network);
    CHECK(!describe(SteamGridError::BadKey).empty());

    CHECK_STR_EQ(url_escape("Pokémon & Co/1"), "Pok%C3%A9mon%20%26%20Co%2F1");
}

void test_names_are_tidied_for_searching() {
    CHECK_STR_EQ(steamgrid_clean_title("Mario Kart\xE2\x84\xA2 DS (USA) (En,Fr)"), "Mario Kart DS");
    CHECK_STR_EQ(steamgrid_clean_title("  Pokemon_Platinum [!] "), "Pokemon Platinum");
    CHECK_STR_EQ(steamgrid_clean_title("(Demo)"), "(Demo)");
    const std::vector<std::string> luigi = steamgrid_search_terms("Mario & Luigi: Bowser's Inside Story");
    CHECK_EQ(luigi.size(), static_cast<std::size_t>(3));
    if (luigi.size() == 3) {
        CHECK_STR_EQ(luigi[0], "Mario & Luigi: Bowser's Inside Story");
        CHECK_STR_EQ(luigi[1], "Mario Luigi Bowsers Inside Story");
        CHECK_STR_EQ(luigi[2], "Mario & Luigi");
    }
    const std::vector<std::string> kart = steamgrid_search_terms("Mario Kart DS");
    CHECK_EQ(kart.size(), static_cast<std::size_t>(2));
    if (kart.size() == 2) CHECK_STR_EQ(kart[1], "Mario Kart");
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


// A SteamGridDB that knows two games by the name, the first with no pictures
// and the second with some, and "Brain Age" by another name only.
class PickyPlatform : public FakePlatform {
public:
    HttpResponse http_get(const std::string& url, const std::string& bearer) override {
        HttpResponse response;
        response.reached = true;
        response.status = 200;
        if (url.find("/search/autocomplete/Brain%2520Age%2520DS") != std::string::npos ||
            url.find("/search/autocomplete/Brain%2520Age") != std::string::npos) {
            response.body = bytes_of(R"({"data":[]})");
        } else if (url.find("/search/autocomplete/Brain%2520Training") != std::string::npos) {
            response.body = bytes_of(R"({"data":[{"id":7,"name":"Brain Training"},{"id":8,"name":"Brain Training 2"}]})");
        } else if (url.find("/grids/game/8") != std::string::npos || url.find("/icons/game/8") != std::string::npos) {
            response.body = bytes_of(R"({"data":[]})");
        } else {
            return FakePlatform::http_get(url, bearer);
        }
        return response;
    }
};

void test_another_game_and_another_name() {
    PickyPlatform platform;
    ForwarderScreen screen(platform);
    DsRomInfo info;
    info.title = "Brain Age DS";
    screen.open("sdmc:/roms/nds/Brain Age.nds", info, "key");

    // Nothing by the ROM's name: asked for another.
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Confirm);
    settle(screen);
    CHECK(screen.overlay() == ForwarderScreen::Overlay::GameChoice);
    CHECK(screen.games().empty());
    CHECK(screen.game_choice_text().find("knows no game called") != std::string::npos);
    platform.typed = {"Brain Training"};
    press(screen, UiAction::Confirm);  // Search for Another Name
    settle(screen);
    CHECK(screen.page() == ForwarderScreen::Page::Icons);
    CHECK_STR_EQ(screen.match_name(), "Brain Training");
    CHECK_EQ(screen.games().size(), static_cast<std::size_t>(2));

    // Y: the other game, which has no pictures -- said so, and offered again.
    press(screen, UiAction::MenuReset);
    CHECK(screen.overlay() == ForwarderScreen::Overlay::GameChoice);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    settle(screen);
    CHECK(screen.overlay() == ForwarderScreen::Overlay::GameChoice);
    CHECK(screen.game_choice_text().find("Brain Training 2") != std::string::npos);
    // B: back to the pictures there were.
    press(screen, UiAction::Cancel);
    CHECK(screen.overlay() == ForwarderScreen::Overlay::None);
    CHECK(screen.page() == ForwarderScreen::Page::Icons);
    CHECK_EQ(screen.icon_count(), static_cast<std::size_t>(3));
}

}  // namespace

int main() {
    test_a_forwarder_is_made_from_a_rom();
    test_no_banner_and_no_key();
    test_the_ds_banner_becomes_a_menu_icon();
    test_a_downloaded_picture_is_squared_and_scaled();
    test_the_search_asks_and_understands();
    test_names_are_tidied_for_searching();
    test_an_ico_is_read();
    test_another_game_and_another_name();
    return test::finish("forwarder");
}
