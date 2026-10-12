// Data Management, driven the way a player drives it -- presses in, words
// out -- against a second device answering on loopback: what the page says
// about two saves, what each dialog asks, and what happens when it is
// answered, including the resume point's warning and the restart a game in
// play needs. Nothing here draws, so none of it needs a GL context.

#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>

#include "config/paths.hpp"
#include "core/save_store.hpp"
#include "net/save_exchange.hpp"
#include "ui/data_management_screen.hpp"
#include "ui/page_frame.hpp"
#include "ui/ui_theme.hpp"

using namespace ds;

namespace {

// Below 32768, out of the range the system hands out for outgoing connections
// -- see test_save_exchange.cpp.
constexpr uint16_t kQueryPort = 29943;
constexpr uint16_t kExchangePort = 29944;
constexpr const char* kGameId = "AMCE-01-0-1a2b";

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-datamanagement-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

void put_u16(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value & 0xFF);
    bytes[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

std::vector<uint8_t> ds_rom() {
    std::vector<uint8_t> bytes(0x5000, 0);
    std::memcpy(bytes.data() + 0x00C, "AMCE", 4);
    std::memcpy(bytes.data() + 0x010, "01", 2);
    put_u16(bytes, 0x068, 0x4000);
    put_u16(bytes, 0x15E, 0x1a2b);
    const std::u16string title = u"Mario Kart DS\nNintendo";
    for (std::size_t index = 0; index < title.size(); ++index) {
        put_u16(bytes, 0x4000 + 0x240 + 256 + index * 2, title[index]);
    }
    return bytes;
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    return write_file_atomically(path, bytes.data(), bytes.size(), nullptr);
}

struct Device {
    std::string saves;
    std::string rom;
    SaveStore store;

    Device(const std::string& root, const char* name)
        : saves(path_join(root, std::string(name) + "-saves")),
          rom(path_join(root, std::string(name) + "-Mario Kart DS.nds")),
          store(saves, name) {
        ensure_directory(saves);
        write_bytes(rom, ds_rom());
    }

    std::string save_path() const { return path_join(saves, path_stem(rom) + ".srm"); }
    std::string resume_path() const { return path_join(saves, path_stem(rom) + ".resume"); }

    void play(uint8_t fill) {
        store.note_game_started(rom);
        write_bytes(save_path(), std::vector<uint8_t>(8192, fill));
        store.note_game_stopped(rom);
    }
};

DataManagementResult press(DataManagementScreen& screen, UiAction action) {
    return screen.update({action}, {}, 1280.0f, 800.0f);
}

// Passes with nothing pressed until `done`, as frames would, for up to a few
// seconds.
DataManagementResult settle(DataManagementScreen& screen, const std::function<bool()>& done) {
    DataManagementResult last;
    for (int pass = 0; pass < 600; ++pass) {
        last = screen.update({}, {}, 1280.0f, 800.0f);
        if (last.restart_with || done()) return last;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return last;
}

// Walks the game page's cursor to one of its four rows.
void focus_row(DataManagementScreen& screen, int row) {
    for (int step = 0; step < 8 && screen.action_focus() > row; ++step) press(screen, UiAction::Up);
    for (int step = 0; step < 8 && screen.action_focus() < row; ++step) press(screen, UiAction::Down);
}

bool has_line(const std::vector<std::string>& lines, const std::string& wanted) {
    for (const std::string& line : lines) {
        if (line == wanted) return true;
    }
    return false;
}

}  // namespace

// A list longer than the screen keeps the row under the cursor on screen as
// the d-pad walks it, all the way down and back.
void test_the_list_follows_the_cursor(const std::string& sandbox) {
    const std::string saves = path_join(sandbox, "many-saves");
    ensure_directory(saves);
    SaveStore store(saves, "many");
    for (int game = 0; game < 25; ++game) {
        std::vector<uint8_t> rom = ds_rom();
        const std::string code = std::string("A") + static_cast<char>('A' + game) + "CE";
        std::memcpy(rom.data() + 0x00C, code.data(), 4);
        const std::string path = path_join(sandbox, "game" + std::to_string(game) + ".nds");
        write_bytes(path, rom);
        store.note_game_started(path);
        write_bytes(path_join(saves, path_stem(path) + ".srm"), std::vector<uint8_t>(64, 1));
        store.note_game_stopped(path);
    }
    DataManagementScreen screen(store, "many-instance");
    screen.open();
    CHECK(screen.saves().size() >= 20u);
    const PageFrame frame = page_frame(1280.0f, 800.0f);
    const float row_h = menu_two_line_row_height(frame.line);
    const float view = frame.content_bottom - frame.content_top;
    auto visible = [&]() {
        const float top = static_cast<float>(screen.list_focus()) * row_h - screen.list_scroll();
        return top >= -0.5f && top + row_h <= view + 0.5f;
    };
    for (std::size_t step = 0; step + 1 < screen.saves().size(); ++step) {
        press(screen, UiAction::Down);
        CHECK_MSG(visible(), "the row under the cursor went off screen at row " +
                                 std::to_string(screen.list_focus()));
    }
    for (std::size_t step = 0; step + 1 < screen.saves().size(); ++step) {
        press(screen, UiAction::Up);
        CHECK_MSG(visible(), "the row under the cursor went off screen going up, at row " +
                                 std::to_string(screen.list_focus()));
    }
}

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_the_list_follows_the_cursor(sandbox);
    Device here(sandbox, "Switch Lite");
    Device deck(sandbox, "steamdeck");

    // The Deck has the first version; the Switch Lite has played on since.
    here.play(1);
    {
        SaveInfo info;
        std::vector<uint8_t> bytes;
        CHECK(here.store.read_for_sending(kGameId, info, bytes, nullptr));
        CHECK(deck.store.accept(info, bytes, "Switch Lite", nullptr));
    }
    here.play(2);

    net::SaveExchangeService deck_service(
        deck.store, net::SaveExchangeService::Config{kQueryPort, kExchangePort,
                                                     net::kDeviceKindSteamDeck, {kSaveSystemDs}});
    std::string error;
    CHECK_MSG(deck_service.start(&error), error);

    net::SaveSearch search;
    search.query_port = kQueryPort;
    search.addresses = {"127.0.0.1"};
    search.timeout_ms = 400;
    DataManagementScreen screen(here.store, "switch-lite-instance");
    screen.set_search_for_test(search);
    const auto idle = [&] { return !screen.searching(); };

    // The list, then the game's page.
    screen.open();
    CHECK(screen.page() == DataManagementScreen::Page::List);
    CHECK_EQ(screen.saves().size(), static_cast<std::size_t>(1));
    settle(screen, idle);
    press(screen, UiAction::Confirm);
    CHECK(screen.page() == DataManagementScreen::Page::Game);
    settle(screen, [&] { return idle() && !screen.comparison_text().empty(); });
    CHECK_EQ(screen.devices().size(), static_cast<std::size_t>(1));
    CHECK_STR_EQ(screen.comparison_text(), "Newer on Switch Lite");
    CHECK(!screen.comparison_is_conflict());
    std::vector<std::string> labels = screen.action_labels();
    CHECK_STR_EQ(labels[0], "Sync with steamdeck");
    CHECK_STR_EQ(labels[1], "Get Save from steamdeck");
    CHECK_STR_EQ(labels[2], "Send Save to steamdeck");
    CHECK(screen.actions_enabled()[0] && screen.actions_enabled()[1] && screen.actions_enabled()[2]);
    // The cursor starts on Sync.
    CHECK_EQ(screen.action_focus(), 0);

    // Get: asked first, with the warning that this one is further along.
    focus_row(screen, 1);
    press(screen, UiAction::Confirm);
    CHECK(screen.overlay() == DataManagementScreen::Overlay::Confirm);
    CHECK_STR_EQ(screen.dialog_title(), "Replace Switch Lite's save with steamdeck's?");
    CHECK(has_line(screen.dialog_lines(), "Switch Lite's save has progress steamdeck's doesn't."));
    CHECK(has_line(screen.dialog_lines(),
                   "Switch Lite's current save will be moved to Earlier Saves."));
    // Cancelled, nothing happens.
    press(screen, UiAction::Cancel);
    CHECK(screen.overlay() == DataManagementScreen::Overlay::None);

    // With a resume point, asked about that too, before anything moves.
    CHECK(write_bytes(here.resume_path(), std::vector<uint8_t>(16, 7)));
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Confirm);
    CHECK(screen.overlay() == DataManagementScreen::Overlay::ResumeConfirm);
    CHECK(has_line(screen.dialog_lines(),
                   "Replacing this save file will delete the resume point. Are you sure you want "
                   "to replace it?"));
    CHECK(file_exists(here.resume_path()));
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return screen.overlay() == DataManagementScreen::Overlay::Notice; });
    CHECK_STR_EQ(screen.notice(), "Got steamdeck's save for Mario Kart DS.");
    CHECK(!file_exists(here.resume_path()));
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return idle() && screen.comparison_text() == "Up to date"; });
    CHECK_STR_EQ(screen.comparison_text(), "Up to date");
    CHECK_EQ(here.store.earlier(kGameId).size(), static_cast<std::size_t>(1));

    // Send, after playing on here.
    here.play(3);
    press(screen, UiAction::Cancel);
    CHECK(screen.page() == DataManagementScreen::Page::List);
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return idle() && screen.comparison_text() == "Newer on Switch Lite"; });
    focus_row(screen, 2);
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.dialog_title(), "Replace steamdeck's save with Switch Lite's?");
    CHECK(has_line(screen.dialog_lines(), "steamdeck's current save will be moved to Earlier Saves."));
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return screen.overlay() == DataManagementScreen::Overlay::Notice; });
    CHECK_STR_EQ(screen.notice(), "Sent Mario Kart DS to steamdeck.");
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return idle() && screen.comparison_text() == "Up to date"; });
    CHECK_STR_EQ(screen.comparison_text(), "Up to date");

    // Both play on: a conflict, in red.
    here.play(4);
    deck.play(5);
    press(screen, UiAction::MenuReset);
    settle(screen, [&] { return idle() && screen.comparison_is_conflict(); });
    CHECK_STR_EQ(screen.comparison_text(), "Save file conflict");
    CHECK(screen.comparison_is_conflict());

    // While the Deck plays it, neither way is offered.
    deck.store.note_game_started(deck.rom);
    press(screen, UiAction::MenuReset);
    settle(screen, [&] { return idle() && screen.comparison_text() == "Currently playing on steamdeck"; });
    CHECK_STR_EQ(screen.comparison_text(), "Currently playing on steamdeck");
    CHECK(!screen.actions_enabled()[0] && !screen.actions_enabled()[1] && !screen.actions_enabled()[2]);
    // And the cursor where the player left it, rather than moved to Earlier
    // Saves for them because nothing else can be done -- after searching
    // again, too.
    focus_row(screen, 0);
    press(screen, UiAction::MenuReset);
    settle(screen, idle);
    CHECK_EQ(screen.action_focus(), 0);
    deck.store.note_game_stopped(deck.rom);

    // Delete: into Earlier Saves.
    press(screen, UiAction::MenuReset);
    settle(screen, idle);
    const std::size_t earlier_before = here.store.earlier(kGameId).size();
    focus_row(screen, 4);
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.dialog_title(), "Delete Switch Lite's save for Mario Kart DS?");
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.notice(), "Deleted Switch Lite's save for Mario Kart DS.");
    CHECK(!file_exists(here.save_path()));
    CHECK_EQ(here.store.earlier(kGameId).size(), earlier_before + 1);
    press(screen, UiAction::Confirm);
    CHECK(!screen.actions_enabled()[4]);
    CHECK_STR_EQ(screen.comparison_text(), "Only on steamdeck");

    // Earlier Saves, and putting the deleted one back.
    focus_row(screen, 3);
    press(screen, UiAction::Confirm);
    CHECK(screen.page() == DataManagementScreen::Page::Earlier);
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.dialog_title(), "Restore this earlier save?");
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.notice(), "Restored an earlier save for Mario Kart DS.");
    CHECK(file_exists(here.save_path()));
    press(screen, UiAction::Confirm);
    CHECK(screen.page() == DataManagementScreen::Page::Game);

    // During the game: straight to its page, and getting a save restarts it.
    screen.close();
    here.store.note_game_started(here.rom);
    screen.open(kGameId);
    CHECK(screen.page() == DataManagementScreen::Page::Game);
    settle(screen, [&] { return idle() && !screen.comparison_text().empty(); });
    focus_row(screen, 1);
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.dialog_title(), "Restart Mario Kart DS with steamdeck's save?");
    CHECK(has_line(screen.dialog_lines(),
                   "The game will restart from steamdeck's save. Anything since your last in-game "
                   "save on Switch Lite will be lost. The save itself will be moved to Earlier Saves."));
    press(screen, UiAction::Confirm);
    const DataManagementResult restart = settle(screen, [] { return false; });
    CHECK(static_cast<bool>(restart.restart_with));
    CHECK_STR_EQ(restart.restart_notice, "Restarted Mario Kart DS with steamdeck's save.");
    if (restart.restart_with) {
        // What the app does: the game closes, then the save changes.
        here.store.note_game_stopped(here.rom);
        CHECK(restart.restart_with(&error));
        CHECK(std::vector<uint8_t>(8192, 5) == [&] {
            std::vector<uint8_t> bytes;
            read_file(here.save_path(), bytes, nullptr);
            return bytes;
        }());
    }

    // Back, from a game's own page, leaves the screen.
    here.store.note_game_started(here.rom);
    screen.open(kGameId);
    CHECK(press(screen, UiAction::Cancel).close_requested);
    CHECK(!screen.is_open());
    here.store.note_game_stopped(here.rom);
    settle(screen, idle);

    const auto notice_up = [&] { return screen.overlay() == DataManagementScreen::Overlay::Notice; };
    const auto read_save = [](const std::string& path) {
        std::vector<uint8_t> bytes;
        read_file(path, bytes, nullptr);
        return bytes;
    };

    // Sync: the newer save goes where it is not, with no question asked.
    here.play(6);
    screen.open();
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return idle() && screen.comparison_text() == "Newer on Switch Lite"; });
    focus_row(screen, 0);
    press(screen, UiAction::Confirm);
    CHECK(screen.overlay() == DataManagementScreen::Overlay::Busy);
    settle(screen, notice_up);
    CHECK_STR_EQ(screen.notice(), "Sent Mario Kart DS to steamdeck.");
    CHECK(read_save(deck.save_path()) == std::vector<uint8_t>(8192, 6));
    press(screen, UiAction::Confirm);
    settle(screen, [&] { return idle() && screen.comparison_text() == "Up to date"; });
    // And when there is nothing to do, says so.
    focus_row(screen, 0);
    press(screen, UiAction::Confirm);
    CHECK_STR_EQ(screen.notice(), "Mario Kart DS is already up to date on Switch Lite and steamdeck.");
    press(screen, UiAction::Confirm);

    // A conflict asks which to keep; this device's is the second choice.
    here.play(7);
    deck.play(8);
    press(screen, UiAction::MenuReset);
    settle(screen, [&] { return idle() && screen.comparison_is_conflict(); });
    focus_row(screen, 0);
    press(screen, UiAction::Confirm);
    CHECK(screen.overlay() == DataManagementScreen::Overlay::Choice);
    CHECK_STR_EQ(screen.choice_title(), "Which save do you want to keep?");
    press(screen, UiAction::Down);
    CHECK_EQ(screen.choice_focus(), 1);
    press(screen, UiAction::Confirm);
    settle(screen, notice_up);
    CHECK_STR_EQ(screen.notice(), "Sent Mario Kart DS to steamdeck.");
    CHECK(read_save(deck.save_path()) == std::vector<uint8_t>(8192, 7));
    press(screen, UiAction::Confirm);

    // Sync All, from the list with X: this device's newer save goes over,
    // and a game only the Deck has comes here.
    here.play(9);
    {
        SaveInfo quest;
        quest.stem = "Dragon Quest IX";
        quest.game_id = "YDTE-01-0-0000";
        quest.title = "Dragon Quest IX";
        SaveVersion version;
        version.id = "quest-1";
        version.hash = "unused";
        version.made_on = "steamdeck";
        version.made_at = 1000;
        quest.history.push_back(version);
        CHECK(deck.store.accept(quest, std::vector<uint8_t>(512, 3), "PC", nullptr));
    }
    press(screen, UiAction::Cancel);
    CHECK(screen.page() == DataManagementScreen::Page::List);
    press(screen, UiAction::MenuQuit);
    CHECK(screen.syncing_all());
    settle(screen, notice_up);
    CHECK_STR_EQ(screen.notice(), "Synced 2 saves with steamdeck.");
    CHECK(read_save(deck.save_path()) == std::vector<uint8_t>(8192, 9));
    SaveInfo quest_here;
    CHECK(here.store.find("YDTE-01-0-0000", quest_here) && quest_here.has_save);
    CHECK_EQ(screen.saves().size(), static_cast<std::size_t>(2));
    press(screen, UiAction::Confirm);

    // A conflict during Sync All is asked about, naming the game; skipped here.
    here.play(10);
    deck.play(11);
    press(screen, UiAction::MenuQuit);
    settle(screen, [&] { return screen.overlay() == DataManagementScreen::Overlay::Choice; });
    CHECK_STR_EQ(screen.choice_title(), "Mario Kart DS: which save do you want to keep?");
    press(screen, UiAction::Cancel);
    settle(screen, notice_up);
    CHECK_STR_EQ(screen.notice(), "Nothing was synced with steamdeck. Skipped 1.");
    press(screen, UiAction::Confirm);
    CHECK(!screen.syncing_all());

    // A save that arrives from another device is shown at once.
    uint64_t arrivals = 0;
    screen.close();
    screen.set_change_source([&] { return arrivals; });
    screen.open();
    const std::size_t before = screen.saves().size();
    {
        SaveInfo arrived;
        arrived.stem = "Arrived";
        arrived.game_id = "ZZZE-01-0-0000";
        arrived.title = "Arrived";
        SaveVersion version;
        version.id = "arrived-1";
        version.hash = "unused";
        arrived.history.push_back(version);
        CHECK(here.store.accept(arrived, std::vector<uint8_t>(64, 1), "steamdeck", nullptr));
    }
    screen.update({}, {}, 1280.0f, 800.0f);
    CHECK_EQ(screen.saves().size(), before);
    ++arrivals;
    screen.update({}, {}, 1280.0f, 800.0f);
    CHECK_EQ(screen.saves().size(), before + 1);
    screen.set_change_source(nullptr);

    // Nobody there: only this device, and nothing to get or send.
    deck_service.stop();
    screen.open();
    press(screen, UiAction::Confirm);
    settle(screen, idle);
    CHECK(screen.devices().empty());
    CHECK(screen.comparison_text().empty());
    labels = screen.action_labels();
    CHECK_STR_EQ(labels[0], "Sync");
    CHECK_STR_EQ(labels[1], "Get Save");
    CHECK_STR_EQ(labels[2], "Send Save");
    CHECK(!screen.actions_enabled()[0] && !screen.actions_enabled()[1] && !screen.actions_enabled()[2]);

    CHECK_STR_EQ(DataManagementScreen::describe_count(1), "1 save");
    CHECK_STR_EQ(DataManagementScreen::describe_count(5), "5 saves");
    return test::finish("data management");
}
