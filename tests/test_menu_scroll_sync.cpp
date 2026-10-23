// One page on two devices, scrolled the same on both: whichever is scrolled,
// the other follows, at the same rows however different the screens -- and a
// finger is never pulled back by its own echo.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "test_support.hpp"
#include "ui/game_browser.hpp"
#include "ui/list_touch.hpp"
#include "ui/menu_cursor_sync.hpp"
#include "ui/menu_scroll_sync.hpp"

using namespace ds;

namespace {

constexpr float kHostWidth = 1280.0f;
constexpr float kHostHeight = 800.0f;
constexpr float kCompanionWidth = 800.0f;
constexpr float kCompanionHeight = 480.0f;

std::vector<GameBrowserEntry> amiibo(int count) {
    std::vector<GameBrowserEntry> entries;
    for (int index = 0; index < count; ++index) {
        entries.push_back(GameBrowserEntry{"Figure " + std::to_string(index) + ".bin", false, false});
    }
    return entries;
}

PointerEvent press(float y) {
    PointerEvent event;
    event.normalized = false;
    event.pressed = true;
    event.x = event.press_x = 300.0f;
    event.y = event.press_y = y;
    return event;
}

// A finger dragged up the companion's list by `pixels`, as passes of input.
void drag_up(GameBrowser& page, float from, float pixels) {
    set_live_pointer(true, 300.0f, from);
    MenuPage::taps_on(page, {press(from)}, kCompanionWidth, kCompanionHeight);
    for (int step = 1; step <= 4; ++step) {
        set_live_pointer(true, 300.0f, from - pixels * static_cast<float>(step) / 4.0f);
        MenuPage::taps_on(page, {}, kCompanionWidth, kCompanionHeight);
    }
    set_live_pointer(false, 300.0f, from - pixels);
}

// Passes of the sync a frame apart, from `start`, as long as it takes the
// list to glide to where it was told: `theirs` the first pass only, on the
// host, which hears each word once; every pass on the companion.
std::optional<net::MenuScrollMessage> follow(MenuScrollSync& sync, MenuPage& page, const net::MenuScrollMessage* theirs,
                                             bool every_pass, float width, float height,
                                             MenuScrollSync::Clock::time_point& now) {
    std::optional<net::MenuScrollMessage> said;
    for (int pass = 0; pass < 40; ++pass) {
        now += std::chrono::milliseconds(16);
        const auto told = sync.sync(&page, net::MirroredPage::AmiiboBrowser, (pass == 0 || every_pass) ? theirs : nullptr,
                                    width, height, now);
        if (told) said = told;
    }
    return said;
}

void test_the_host_scrolls_and_the_companion_follows() {
    GameBrowser host;
    GameBrowser companion;
    host.set_amiibo(true);
    companion.set_amiibo(true);
    host.show_mirrored("/amiibo", amiibo(60), 0);
    companion.show_mirrored("/amiibo", amiibo(60), 0);
    MenuScrollSync host_sync(true);
    MenuScrollSync companion_sync(false);

    host.set_scroll_state({20.0f}, kHostWidth, kHostHeight);
    const auto said = host_sync.sync(&host, net::MirroredPage::AmiiboBrowser, nullptr, kHostWidth, kHostHeight);
    CHECK_MSG(said.has_value(), "the host never said where it had scrolled");
    if (!said) return;
    auto now = MenuScrollSync::Clock::now();
    // Glided to, not jumped to: part of the way after one frame...
    CHECK(!companion_sync.sync(&companion, net::MirroredPage::AmiiboBrowser, &*said, kCompanionWidth,
                               kCompanionHeight, now));
    now += std::chrono::milliseconds(16);
    companion_sync.sync(&companion, net::MirroredPage::AmiiboBrowser, &*said, kCompanionWidth, kCompanionHeight, now);
    const float partway = companion.scroll_state(kCompanionWidth, kCompanionHeight).front();
    CHECK_MSG(partway > 0.0f && partway < 20.0f, "the companion jumped to " + std::to_string(partway));
    // ...and there soon after, saying nothing back.
    CHECK(!follow(companion_sync, companion, &*said, true, kCompanionWidth, kCompanionHeight, now));
    CHECK_MSG(companion.scroll_state(kCompanionWidth, kCompanionHeight) == said->positions,
              "the companion is not scrolled where the host is");
    CHECK(!companion_sync.following());
    // Nothing new: the host says nothing more.
    CHECK(!host_sync.sync(&host, net::MirroredPage::AmiiboBrowser, nullptr, kHostWidth, kHostHeight));
    // Another page's scroll is never taken for this one's.
    net::MenuScrollMessage other = *said;
    other.page = net::MirroredPage::GraphicPacks;
    other.positions = {3.0f};
    follow(companion_sync, companion, &other, true, kCompanionWidth, kCompanionHeight, now);
    CHECK(companion.scroll_state(kCompanionWidth, kCompanionHeight) == said->positions);
}

void test_the_companion_scrolls_and_the_host_follows() {
    GameBrowser host;
    GameBrowser companion;
    host.set_amiibo(true);
    companion.set_amiibo(true);
    host.show_mirrored("/amiibo", amiibo(60), 0);
    companion.show_mirrored("/amiibo", amiibo(60), 0);
    MenuScrollSync host_sync(true);
    MenuScrollSync companion_sync(false);
    const auto start = host_sync.sync(&host, net::MirroredPage::AmiiboBrowser, nullptr, kHostWidth, kHostHeight);
    CHECK(start.has_value());

    drag_up(companion, 400.0f, 150.0f);
    const float scrolled = companion.scroll_state(kCompanionWidth, kCompanionHeight).front();
    CHECK_MSG(scrolled > 0.0f, "the drag did not scroll the companion's list");
    // Scrolled here: the host's old position is not taken back...
    const auto said = companion_sync.sync(&companion, net::MirroredPage::AmiiboBrowser, &*start,
                                          kCompanionWidth, kCompanionHeight);
    CHECK_EQ(companion.scroll_state(kCompanionWidth, kCompanionHeight).front(), scrolled);
    // ...and the host is told, and takes it.
    CHECK_MSG(said.has_value(), "the companion never told the host where it scrolled");
    if (!said) return;
    auto now = MenuScrollSync::Clock::now();
    const auto echo = follow(host_sync, host, &*said, false, kHostWidth, kHostHeight, now);
    CHECK_EQ(host.scroll_state(kHostWidth, kHostHeight).front(), scrolled);
    // What it was told, it does not say back.
    CHECK_MSG(!echo.has_value(), "the host said back what it was told");
    // And the host's last word from before the drag does not pull the
    // companion back once the finger is off it.
    follow(companion_sync, companion, &*start, true, kCompanionWidth, kCompanionHeight, now);
    CHECK_EQ(companion.scroll_state(kCompanionWidth, kCompanionHeight).front(), scrolled);
}

}  // namespace

// A finger held on a row of the companion's drawing fills the same row on
// the host's, and a finger going down there hides the ring on both; a press
// of the host's pad brings it back on both.
void test_the_cursor_is_the_same_on_both() {
    GameBrowser host;
    GameBrowser companion;
    host.set_amiibo(true);
    companion.set_amiibo(true);
    host.show_mirrored("/amiibo", amiibo(20), 0);
    companion.show_mirrored("/amiibo", amiibo(20), 0);
    MenuCursorSync host_sync(true);
    MenuCursorSync companion_sync(false);
    const net::MirroredPage page = net::MirroredPage::AmiiboBrowser;

    set_menu_cursor_shown(true);
    const auto first = host_sync.sync(&host, page, nullptr, kHostWidth, kHostHeight);
    CHECK_MSG(first.has_value() && first->shown && first->held == -1, "the host did not say how it stands");
    CHECK(!companion_sync.sync(&companion, page, &*first, kCompanionWidth, kCompanionHeight).has_value());

    // A finger down on the companion's third row, held there.
    float x = 0.0f;
    float y = 0.0f;
    CHECK(MenuCursorSync::entry_point(companion, 2, kCompanionWidth, kCompanionHeight, x, y));
    set_live_pointer(true, x, y);
    set_menu_cursor_shown(false);
    const auto held = companion_sync.sync(&companion, page, nullptr, kCompanionWidth, kCompanionHeight);
    CHECK_MSG(held.has_value() && !held->shown && held->held == 2, "the companion did not say what it held");
    set_live_pointer(false, x, y);
    set_menu_cursor_shown(true);
    const auto start = std::chrono::steady_clock::now();
    const auto answer = host_sync.sync(&host, page, held ? &*held : nullptr, kHostWidth, kHostHeight);
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    CHECK_MSG(seconds < 0.05f, "finding the row held took too long");
    CHECK_MSG(!answer.has_value(), "the host said back what it was told");
    CHECK(!menu_cursor_shown());
    CHECK(remote_held().held);
    CHECK_EQ(host.entry_at(remote_held().x, remote_held().y, kHostWidth, kHostHeight), 2);

    // Let go; then the host's pad.
    const auto let_go = companion_sync.sync(&companion, page, nullptr, kCompanionWidth, kCompanionHeight);
    CHECK(let_go.has_value() && let_go->held == -1);
    host_sync.sync(&host, page, let_go ? &*let_go : nullptr, kHostWidth, kHostHeight);
    CHECK(!remote_held().held);
    set_menu_cursor_shown(true);
    const auto shown = host_sync.sync(&host, page, nullptr, kHostWidth, kHostHeight);
    CHECK(shown.has_value() && shown->shown);
    set_menu_cursor_shown(false);
    companion_sync.sync(&companion, page, shown ? &*shown : nullptr, kCompanionWidth, kCompanionHeight);
    CHECK(menu_cursor_shown());
}

// The host's word on its page can come before that page is up on the
// companion: it is kept for when it is.
void test_the_cursor_said_before_the_page_is_up_is_kept() {
    GameBrowser host;
    GameBrowser companion;
    host.set_amiibo(true);
    companion.set_amiibo(true);
    host.show_mirrored("/amiibo", amiibo(20), 0);
    companion.show_mirrored("/amiibo", amiibo(20), 0);
    MenuCursorSync host_sync(true);
    MenuCursorSync companion_sync(false);
    const net::MirroredPage page = net::MirroredPage::AmiiboBrowser;

    set_menu_cursor_shown(true);
    const auto said = host_sync.sync(&host, page, nullptr, kHostWidth, kHostHeight);
    CHECK(said.has_value() && said->shown);
    set_menu_cursor_shown(false);
    companion_sync.sync(nullptr, net::MirroredPage::None, said ? &*said : nullptr, kCompanionWidth, kCompanionHeight);
    companion_sync.sync(&companion, page, nullptr, kCompanionWidth, kCompanionHeight);
    CHECK_MSG(menu_cursor_shown(), "the host's cursor was lost while its page was coming up");
}

int main() {
    test_the_cursor_is_the_same_on_both();
    test_the_cursor_said_before_the_page_is_up_is_kept();
    test_the_host_scrolls_and_the_companion_follows();
    test_the_companion_scrolls_and_the_host_follows();
    return test::finish("menu-scroll-sync");
}
