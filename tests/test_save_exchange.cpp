// Two devices' saves over a real socket: a search finds the other and not
// itself, and a save goes each way -- or does not, while the game is being
// played where it would land.
//
// Both devices live in this process, on loopback, on ports of their own so
// this can run beside the tests that stand up a real host.

#include "test_support.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <stdlib.h>

#include "config/paths.hpp"
#include "core/save_store.hpp"
#include "net/save_exchange.hpp"

using namespace ds;
using namespace ds::net;

namespace {

// Below 32768, where the system never hands out a port for an outgoing
// connection: a port in that range can be taken, at random, by another test's
// connection at the moment this one wants to listen on it.
constexpr uint16_t kDeckQueryPort = 29923;
constexpr uint16_t kDeckExchangePort = 29924;
constexpr uint16_t kSwitchQueryPort = 29933;
constexpr uint16_t kSwitchExchangePort = 29934;
constexpr const char* kGameId = "AMCE-01-0-1a2b";

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-saveexchange-XXXXXX";
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

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::vector<uint8_t> bytes;
    read_file(path, bytes, nullptr);
    return bytes;
}

// A device: its saves folder, its store, and the service answering for it.
struct Device {
    std::string saves;
    std::string rom;
    SaveStore store;
    SaveExchangeService service;

    Device(const std::string& root, const char* name, const char* kind, uint16_t query,
           uint16_t exchange)
        : saves(path_join(root, std::string(name) + "-saves")),
          rom(path_join(root, std::string(name) + "-Mario Kart DS.nds")),
          store(saves, name),
          service(store, SaveExchangeService::Config{query, exchange, kind, {kSaveSystemDs}}) {
        ensure_directory(saves);
        write_bytes(rom, ds_rom());
    }

    void play(uint8_t fill) {
        store.note_game_started(rom);
        write_bytes(path_join(saves, path_stem(rom) + ".srm"), std::vector<uint8_t>(8192, fill));
        store.note_game_stopped(rom);
    }
};

SaveDevice find_device(const std::vector<SaveDevice>& devices, const std::string& name) {
    for (const SaveDevice& device : devices) {
        if (device.name == name) return device;
    }
    return SaveDevice();
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    Device deck(sandbox, "steamdeck", kDeviceKindSteamDeck, kDeckQueryPort, kDeckExchangePort);
    Device lite(sandbox, "Switch Lite", kDeviceKindSwitchLite, kSwitchQueryPort, kSwitchExchangePort);
    std::string error;
    CHECK_MSG(deck.service.start(&error), error);
    CHECK_MSG(lite.service.start(&error), error);

    // The Switch looks, and finds the Deck by what it said of itself.
    SaveSearch search;
    search.query_port = kDeckQueryPort;
    search.own_instance = lite.service.instance();
    search.addresses = {"127.0.0.1"};
    search.timeout_ms = 700;
    std::vector<SaveDevice> found = search_save_devices(search);
    CHECK_EQ(found.size(), static_cast<std::size_t>(1));
    SaveDevice deck_device = find_device(found, "steamdeck");
    CHECK_STR_EQ(deck_device.kind, kDeviceKindSteamDeck);
    CHECK(deck_device.keeps("ds"));
    CHECK(!deck_device.keeps("3ds"));
    CHECK_EQ(deck_device.port, kDeckExchangePort);

    // And never itself.
    search.query_port = kSwitchQueryPort;
    CHECK(search_save_devices(search).empty());

    // The Deck has played, the Switch has not.
    deck.play(1);
    RemoteSaveStatus status;
    CHECK_MSG(ask_save_status(deck_device, "ds", kGameId, status, &error), error);
    CHECK(status.versions.has_save);
    CHECK(!status.playing_now);
    CHECK_STR_EQ(status.made_on, "steamdeck");
    CHECK_EQ(status.size, static_cast<uint64_t>(8192));
    SaveInfo mine;
    lite.store.find(kGameId, mine);
    CHECK(compare_saves(version_state(mine), status.versions) == SaveComparison::OnlyThere);

    // Get Save from steamdeck.
    SaveInfo fetched;
    std::vector<uint8_t> bytes;
    CHECK_MSG(fetch_save(deck_device, kGameId, fetched, bytes, &error), error);
    CHECK_STR_EQ(fetched.title, "Mario Kart DS");
    CHECK_EQ(bytes.size(), static_cast<std::size_t>(8192));
    CHECK_MSG(lite.store.accept(fetched, bytes, deck_device.name, &error), error);
    CHECK(lite.store.find(kGameId, mine));
    CHECK(compare_saves(version_state(mine), status.versions) == SaveComparison::UpToDate);

    // Played on the Switch, then sent back.
    lite.play(2);
    CHECK(lite.store.find(kGameId, mine));
    CHECK(ask_save_status(deck_device, "ds", kGameId, status, &error));
    CHECK(compare_saves(version_state(mine), status.versions) == SaveComparison::NewerHere);
    const uint64_t received_before = deck.service.saves_received();
    CHECK(lite.store.read_for_sending(kGameId, mine, bytes, &error));
    CHECK_MSG(send_save(deck_device, mine, bytes, "Switch Lite", &error), error);
    CHECK_EQ(deck.service.saves_received(), received_before + 1);
    CHECK(read_bytes(path_join(deck.saves, path_stem(deck.rom) + ".srm")) ==
          std::vector<uint8_t>(8192, 2));
    CHECK(ask_save_status(deck_device, "ds", kGameId, status, &error));
    CHECK(compare_saves(version_state(mine), status.versions) == SaveComparison::UpToDate);
    const std::vector<EarlierSave> earlier = deck.store.earlier(kGameId);
    CHECK_EQ(earlier.size(), static_cast<std::size_t>(1));
    if (!earlier.empty()) CHECK_STR_EQ(earlier[0].reason, "Replaced by Switch Lite's save");

    // While the Deck plays it, its save is neither read nor replaced.
    deck.store.note_game_started(deck.rom);
    CHECK(ask_save_status(deck_device, "ds", kGameId, status, &error));
    CHECK(status.playing_now);
    error.clear();
    CHECK(!send_save(deck_device, mine, bytes, "Switch Lite", &error));
    CHECK(!error.empty());
    CHECK(!fetch_save(deck_device, kGameId, fetched, bytes, &error));
    deck.store.note_game_stopped(deck.rom);

    // A game the Deck has never seen.
    CHECK(ask_save_status(deck_device, "ds", "ZZZZ-01-0-0000", status, &error));
    CHECK(!status.versions.has_save);
    CHECK(!fetch_save(deck_device, "ZZZZ-01-0-0000", fetched, bytes, &error));

    // Nobody there.
    SaveDevice nobody = deck_device;
    nobody.port = 29999;
    CHECK(!ask_save_status(nobody, "ds", kGameId, status, &error));

    deck.service.stop();
    lite.service.stop();
    return test::finish("save exchange");
}
