// The wire protocol is the one place in this project that reads bytes it did
// not write, so every decoder is exercised against truncated and malformed
// input as well as valid input.

#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include "core/system.hpp"
#include "net/protocol.hpp"

using namespace ds;

namespace {
// What every session state ends with now, empty: the whole focus, then no
// Cemu options, no amiibo and no amiibo folder, then the LCD filter's byte,
// then no lists of Cemu's choices, then the favorite color's byte, then no
// corner notice.
constexpr std::size_t kWiiUTail = 2 + 2 + 2 + 2 + 1 + 2 + 1 + 2;
}  // namespace
using namespace ds::net;

namespace {

// Pulls the single message out of a freshly encoded buffer.
bool one_message(std::vector<uint8_t> buffer, MessageType& type, std::vector<uint8_t>& payload) {
    bool fatal = false;
    const bool got = take_message(buffer, type, payload, fatal);
    return got && !fatal && buffer.empty();
}

void test_hello_round_trip() {
    HelloMessage sent;
    sent.device_name = "Steam Deck";
    sent.wanted_screen = ScreenId::Bottom;

    std::vector<uint8_t> buffer;
    encode_hello(sent, buffer);

    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::Hello);

    HelloMessage received;
    CHECK(decode_hello(payload.data(), payload.size(), received));
    CHECK_STR_EQ(received.device_name, "Steam Deck");
    CHECK(received.wanted_screen == ScreenId::Bottom);
    CHECK_EQ(received.protocol_version, kProtocolVersion);
    CHECK(received.has_pointer);

    // A Switch says it has no pointer, which is what keeps the Pointer
    // setting off the menu of a Switch hosting another Switch.
    sent.has_pointer = false;
    buffer.clear();
    encode_hello(sent, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_hello(payload.data(), payload.size(), received));
    CHECK(!received.has_pointer);

    // A build from before the question was asked stops one byte short, and
    // still decodes -- as a device with a pointer, which is what was assumed.
    payload.pop_back();
    HelloMessage older;
    older.has_pointer = false;
    CHECK(decode_hello(payload.data(), payload.size(), older));
    CHECK(older.has_pointer);
    CHECK_STR_EQ(older.device_name, "Steam Deck");
}

void test_welcome_and_session_round_trip() {
    WelcomeMessage welcome;
    welcome.game_title = "Pokemon Platinum";
    welcome.rom_sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d";
    welcome.granted_screen = ScreenId::Top;
    welcome.system = SystemId::ThreeDs;
    welcome.session_token = 0x0123456789abcdefull;

    std::vector<uint8_t> buffer;
    encode_welcome(welcome, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::Welcome);

    WelcomeMessage received;
    CHECK(decode_welcome(payload.data(), payload.size(), received));
    CHECK_STR_EQ(received.game_title, welcome.game_title);
    CHECK_STR_EQ(received.rom_sha1, welcome.rom_sha1);
    CHECK(received.granted_screen == ScreenId::Top);
    CHECK(received.system == SystemId::ThreeDs);
    CHECK(received.session_token == welcome.session_token);
    CHECK(received.icon_rgba.empty());

    // With the game's icon on the end, decoded already.
    welcome.icon_width = 2;
    welcome.icon_height = 3;
    welcome.icon_rgba.assign(2 * 3 * 4, 0);
    for (std::size_t i = 0; i < welcome.icon_rgba.size(); ++i) {
        welcome.icon_rgba[i] = static_cast<uint8_t>(i * 7);
    }
    buffer.clear();
    encode_welcome(welcome, buffer);
    CHECK(one_message(buffer, type, payload));
    WelcomeMessage with_icon;
    CHECK(decode_welcome(payload.data(), payload.size(), with_icon));
    CHECK_EQ(static_cast<int>(with_icon.icon_width), 2);
    CHECK_EQ(static_cast<int>(with_icon.icon_height), 3);
    CHECK(with_icon.icon_rgba == welcome.icon_rgba);
    CHECK_STR_EQ(with_icon.game_title, welcome.game_title);
    welcome.icon_width = 0;
    welcome.icon_height = 0;
    welcome.icon_rgba.clear();

    SessionStateMessage state;
    state.layout_id = "upper_on_tv";
    state.companion_screen = ScreenId::Bottom;
    state.paused = true;
    // Deliberately not the defaults: a field that never made it across would
    // otherwise decode to the value that was sent and look like it worked.
    state.bilinear_filtering = true;
    state.audio_target = AudioTarget::TopScreen;
    state.handedness = Handedness::Left;
    state.render_scale = 3;
    state.screen_gap = false;
    state.right_stick_mode = RightStickMode::CirclePad;
    state.settings_open = true;

    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::SessionState);

    SessionStateMessage received_state;
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_STR_EQ(received_state.layout_id, "upper_on_tv");
    CHECK(received_state.companion_screen == ScreenId::Bottom);
    CHECK_EQ(received_state.paused, true);
    CHECK_EQ(received_state.bilinear_filtering, true);
    CHECK(received_state.audio_target == AudioTarget::TopScreen);
    CHECK(received_state.handedness == Handedness::Left);
    CHECK(received_state.right_stick_mode == RightStickMode::CirclePad);
    CHECK_EQ(static_cast<int>(received_state.render_scale), 3);
    CHECK_EQ(received_state.screen_gap, false);
    // The host's own settings screen: not mirrored, but the companion has to
    // know it is up to say where to look and to keep sending presses.
    CHECK_EQ(received_state.settings_open, true);
    CHECK_EQ(received_state.save_points_available, false);

    // Save points, which came later and go last: sent when there are some,
    // and absent -- from a host older than them -- read as none.
    state.save_points_available = true;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.save_points_available, true);
    // Not left-handed in the list, so no list and the hand below disagree:
    // from a host that sends one, the list is what counts.
    CHECK(received_state.left_handed_layouts.empty());

    // Which layouts are flipped, after that, and from a host from before the
    // list -- the message stopping at the save-state flag -- worked out from
    // the hand of the layout in use, which is all such a host says.
    state.left_handed_layouts = {"upper_focus", "side_by_side"};
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK(received_state.left_handed_layouts ==
          std::vector<std::string>({"upper_focus", "side_by_side"}));
    // (The left stick's, the Rumble Pak's, resuming's two, Save Sync's, the
    // popup cursor's, the menu-unavailable notice's and Graphics Packs' bytes
    // go after the list, so a host from before the list stops eight bytes
    // earlier still.)
    const std::size_t list_bytes = 1 + (2 + 11) + (2 + 12) + 8 + kWiiUTail;
    CHECK(decode_session_state(payload.data(), payload.size() - list_bytes, received_state));
    CHECK(received_state.left_handed_layouts == std::vector<std::string>({"upper_on_tv"}));
    CHECK(decode_session_state(payload.data(), payload.size() - list_bytes - 1, received_state));
    CHECK_EQ(received_state.save_points_available, false);
    CHECK_EQ(received_state.settings_open, true);

    // What the left stick's click does, last: whether the menu offers it at
    // all and which way the host has it, so both devices build the same
    // Options list. A host from before it offers no such entry.
    CHECK_EQ(received_state.left_stick_choosable, false);
    for (const LeftStickPress press : {LeftStickPress::Microphone, LeftStickPress::SwapLayout}) {
        state.left_stick_choosable = true;
        state.left_stick_press = press;
        buffer.clear();
        encode_session_state(state, buffer);
        CHECK(one_message(buffer, type, payload));
        CHECK(decode_session_state(payload.data(), payload.size(), received_state));
        CHECK_EQ(received_state.left_stick_choosable, true);
        CHECK(received_state.left_stick_press == press);
        CHECK(decode_session_state(payload.data(), payload.size() - 8 - kWiiUTail, received_state));
        CHECK_EQ(received_state.left_stick_choosable, false);
        CHECK(received_state.left_handed_layouts ==
              std::vector<std::string>({"upper_focus", "side_by_side"}));
    }

    // A DS game's Rumble Pak, after that: offered or not, and in or out. A
    // host from before it offers none.
    CHECK_EQ(received_state.rumble_choosable, false);
    for (const bool in : {false, true}) {
        state.rumble_choosable = true;
        state.rumble_pak = in;
        buffer.clear();
        encode_session_state(state, buffer);
        CHECK(one_message(buffer, type, payload));
        CHECK(decode_session_state(payload.data(), payload.size(), received_state));
        CHECK_EQ(received_state.rumble_choosable, true);
        CHECK_EQ(received_state.rumble_pak, in);
        // Without the bytes after it: the resume option, its popup, Save
        // Sync, the popup cursor, the menu-unavailable notice and Graphics
        // Packs.
        CHECK(decode_session_state(payload.data(), payload.size() - 7 - kWiiUTail, received_state));
        CHECK_EQ(received_state.rumble_choosable, false);
        CHECK_EQ(received_state.left_stick_choosable, true);
    }

    // Resume from Save State, last: offered or not, on or off, and whether
    // the game is waiting under the popup that says it resumed.
    for (const bool on : {false, true}) {
        for (const bool notice : {false, true}) {
            state.resume_choosable = true;
            state.resume_from_save_state = on;
            state.resume_notice = notice;
            buffer.clear();
            encode_session_state(state, buffer);
            CHECK(one_message(buffer, type, payload));
            CHECK(decode_session_state(payload.data(), payload.size(), received_state));
            CHECK_EQ(received_state.resume_choosable, true);
            CHECK_EQ(received_state.resume_from_save_state, on);
            CHECK_EQ(received_state.resume_notice, notice);
            CHECK_EQ(received_state.rumble_choosable, true);
        }
    }
    state.resume_choosable = false;
    state.resume_notice = false;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.resume_choosable, false);
    CHECK_EQ(received_state.resume_from_save_state, false);
    CHECK_EQ(received_state.resume_notice, false);

    // Save Sync, last of all, and none from a host from before it.
    CHECK_EQ(received_state.save_sync_available, false);
    state.save_sync_available = true;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.save_sync_available, true);
    CHECK(decode_session_state(payload.data(), payload.size() - 4 - kWiiUTail, received_state));
    CHECK_EQ(received_state.save_sync_available, false);

    // Where the cursor is on the menu's popup, last of all: none from a host
    // from before it.
    state.menu_confirm_focus = 2;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.menu_confirm_focus, static_cast<uint8_t>(2));
    CHECK(decode_session_state(payload.data(), payload.size() - 3 - kWiiUTail, received_state));
    CHECK_EQ(received_state.menu_confirm_focus, static_cast<uint8_t>(0));

    // The menu can't be opened right now, after that: not up, from a host
    // from before it.
    state.menu_unavailable_notice = true;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.menu_unavailable_notice, true);
    CHECK(decode_session_state(payload.data(), payload.size() - 2 - kWiiUTail, received_state));
    CHECK_EQ(received_state.menu_unavailable_notice, false);

    // Graphics Packs in the host's menu, after that: not there, from a host
    // from before them.
    state.graphic_packs_available = true;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.graphic_packs_available, true);
    CHECK(decode_session_state(payload.data(), payload.size() - 1 - kWiiUTail, received_state));
    CHECK_EQ(received_state.graphic_packs_available, false);

    // Scan amiibo shares its byte, a bit each.
    state.graphic_packs_available = false;
    state.amiibo_available = true;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK(!received_state.graphic_packs_available && received_state.amiibo_available);

    // Then the focus whole, past what its byte holds -- a Wii U menu's newer
    // sections -- which a host from before it sends as 0; Cemu's options for
    // the Options list; and the amiibo to list.
    state.menu_focus = 300;
    state.producer_options = {{"Upscaling", "Bicubic"}, {"CPU mode", "Auto"}};
    state.amiibo = {"Link", "Mario"};
    state.amiibo_folder = "/home/deck/amiibo";
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.menu_focus, static_cast<uint16_t>(300));
    CHECK(received_state.producer_options == state.producer_options);
    CHECK(received_state.amiibo == state.amiibo);
    CHECK(received_state.amiibo_folder == "/home/deck/amiibo");
    // A DS game's LCD filter, offered and on; from a host before it, neither.
    state.lcd_filter_choosable = true;
    state.lcd_filter = true;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK(received_state.lcd_filter_choosable && received_state.lcd_filter);
    CHECK(decode_session_state(payload.data(), payload.size() - 6, received_state));
    CHECK(!received_state.lcd_filter_choosable && !received_state.lcd_filter);
    state.lcd_filter_choosable = false;
    state.lcd_filter = false;

    // Each Cemu option's choices, for the popup the other device draws.
    state.producer_choices = {{"Bilinear", "Bicubic"}, {"Auto", "Multi-core", "Single-core"}};
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK(received_state.producer_choices == state.producer_choices);
    state.producer_choices.clear();
    // The host's favorite color, for the other device's menus; and none said
    // by a host from before it was sent.
    state.favorite_color = 7;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK_EQ(received_state.favorite_color, 7);
    // Before it the empty corner notice's length, two bytes.
    CHECK(decode_session_state(payload.data(), payload.size() - 3, received_state));
    CHECK_EQ(received_state.favorite_color, -1);
    state.favorite_color = -1;
    // The host's corner notice, shown on the other device too; and none from
    // a host from before it was sent.
    state.corner_notice = "Scanning\u2026";
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_session_state(payload.data(), payload.size(), received_state));
    CHECK(received_state.corner_notice == state.corner_notice);
    CHECK(decode_session_state(payload.data(), payload.size() - 2 - state.corner_notice.size(), received_state));
    CHECK(received_state.corner_notice.empty());
    state.corner_notice.clear();
    state.menu_focus = 40;
    buffer.clear();
    encode_session_state(state, buffer);
    CHECK(one_message(buffer, type, payload));
    const std::size_t tail =
        2 + 2 + (2 + 9 + 2 + 7) + (2 + 8 + 2 + 4) + 2 + (2 + 4) + (2 + 5) + (2 + 17) + 1 + 2;
    CHECK(decode_session_state(payload.data(), payload.size() - tail, received_state));
    CHECK_EQ(received_state.menu_focus, static_cast<uint16_t>(40));
    CHECK(received_state.producer_options.empty() && received_state.amiibo.empty());
}

// Everything a companion can ask the host for travels in one message, each
// field saying whether it means anything, so asking about the sound does not
// silently also ask for a layout.
// The save point slots a companion's menu shows: pictures, times and the last
// save's line all arrive as they were, and a slot's picture that does not
// unpack is only a slot without a picture.
void test_save_slots_round_trip() {
    SaveSlotsMessage sent;
    sent.slots.resize(4);
    sent.slots[0].exists = true;
    sent.slots[0].saved_at = 1749120960;
    sent.slots[0].thumbnail.width = 3;
    sent.slots[0].thumbnail.height = 2;
    sent.slots[0].thumbnail.pixels = {0x00112233u, 0x00445566u, 0x00778899u,
                                      0x00AABBCCu, 0x00DDEEFFu, 0x00010203u};
    sent.slots[2].exists = true;
    sent.slots[2].saved_at = 42;
    sent.status = "Save state saved to slot 1.";

    std::vector<uint8_t> buffer;
    encode_save_slots(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::SaveSlots);

    SaveSlotsMessage received;
    CHECK(decode_save_slots(payload.data(), payload.size(), received));
    CHECK_EQ(received.slots.size(), static_cast<std::size_t>(4));
    CHECK(received.slots[0].exists);
    CHECK_EQ(received.slots[0].saved_at, static_cast<int64_t>(1749120960));
    CHECK(received.slots[0].thumbnail.pixels == sent.slots[0].thumbnail.pixels);
    CHECK_EQ(received.slots[0].thumbnail.width, 3);
    CHECK(!received.slots[1].exists);
    CHECK(received.slots[2].exists);
    CHECK(!received.slots[2].thumbnail.valid());
    CHECK_STR_EQ(received.status, "Save state saved to slot 1.");
    CHECK(!received.status_failed);

    // Cut short: refused rather than half-read.
    CHECK(!decode_save_slots(payload.data(), payload.size() / 2, received));
}

void test_request_round_trip() {
    RequestMessage sent;
    sent.layout_id = "side_by_side";
    sent.wants_filtering = true;
    sent.bilinear_filtering = true;
    sent.wants_audio_target = true;
    sent.audio_target = AudioTarget::BottomScreen;
    sent.wants_handedness = true;
    sent.handedness = Handedness::Left;
    sent.wants_render_scale = true;
    sent.render_scale = 2;
    sent.wants_display_size = true;
    sent.display_width = 1920;
    sent.display_height = 1080;
    sent.menu_open = true;
    sent.wants_reset = true;
    sent.wants_quit = true;
    sent.wants_screen_gap = true;
    sent.screen_gap = false;
    sent.wants_right_stick_mode = true;
    sent.right_stick_mode = RightStickMode::DPad;
    sent.wants_left_stick_press = true;
    sent.left_stick_press = LeftStickPress::SwapLayout;
    sent.wants_rumble_pak = true;
    sent.rumble_pak = true;
    sent.wants_resume_from_save_state = true;
    sent.resume_from_save_state = true;
    sent.wants_screen_swap = true;
    sent.wants_lcd_filter = true;
    sent.lcd_filter = true;

    std::vector<uint8_t> buffer;
    encode_request(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::Request);

    RequestMessage received;
    CHECK(decode_request(payload.data(), payload.size(), received));
    CHECK_STR_EQ(received.layout_id, "side_by_side");
    CHECK_EQ(received.wants_filtering, true);
    CHECK_EQ(received.bilinear_filtering, true);
    CHECK_EQ(received.wants_audio_target, true);
    CHECK(received.audio_target == AudioTarget::BottomScreen);
    CHECK_EQ(received.wants_handedness, true);
    CHECK(received.handedness == Handedness::Left);
    CHECK_EQ(received.wants_render_scale, true);
    CHECK_EQ(static_cast<int>(received.render_scale), 2);
    CHECK_EQ(received.wants_display_size, true);
    CHECK_EQ(static_cast<int>(received.display_width), 1920);
    CHECK_EQ(static_cast<int>(received.display_height), 1080);
    CHECK_EQ(received.menu_open, true);
    CHECK_EQ(received.wants_reset, true);
    CHECK_EQ(received.wants_quit, true);
    CHECK_EQ(received.wants_screen_gap, true);
    CHECK_EQ(received.screen_gap, false);
    CHECK_EQ(received.wants_right_stick_mode, true);
    CHECK(received.right_stick_mode == RightStickMode::DPad);
    CHECK_EQ(received.wants_left_stick_press, true);
    CHECK(received.left_stick_press == LeftStickPress::SwapLayout);
    CHECK_EQ(received.wants_rumble_pak, true);
    CHECK_EQ(received.rumble_pak, true);
    CHECK_EQ(received.wants_resume_from_save_state, true);
    CHECK_EQ(received.resume_from_save_state, true);
    CHECK_EQ(received.wants_screen_swap, true);
    CHECK(received.wants_lcd_filter && received.lcd_filter);
    // From a companion from before the LCD filter: asks nothing about it.
    RequestMessage unfiltered;
    CHECK(decode_request(payload.data(), payload.size() - 1, unfiltered));
    CHECK(!unfiltered.wants_lcd_filter && unfiltered.wants_screen_swap);
    // From a companion from before Separate Screens' swap: asks for none.
    RequestMessage unswapped;
    // (Each counts past the whole menu focus near the end, two bytes, and
    // the LCD filter's byte after it.)
    CHECK(decode_request(payload.data(), payload.size() - 4, unswapped));
    CHECK_EQ(unswapped.wants_screen_swap, false);
    CHECK_EQ(unswapped.wants_resume_from_save_state, true);
    // Without that and the byte before it: nothing asked about resuming.
    RequestMessage shorter;
    CHECK(decode_request(payload.data(), payload.size() - 5, shorter));
    CHECK_EQ(shorter.wants_resume_from_save_state, false);
    CHECK_EQ(shorter.wants_rumble_pak, true);
    // From a companion from before the Rumble Pak: asks nothing about it.
    RequestMessage older;
    CHECK(decode_request(payload.data(), payload.size() - 6, older));
    CHECK_EQ(older.wants_rumble_pak, false);
    CHECK_EQ(older.wants_left_stick_press, true);
    // And from before the left stick: nothing about that either.
    CHECK(decode_request(payload.data(), payload.size() - 7, older));
    CHECK_EQ(older.wants_left_stick_press, false);
    CHECK_EQ(older.wants_right_stick_mode, true);

    // A request that means none of it round-trips as such rather than as an
    // accidental instruction.
    RequestMessage quiet;
    buffer.clear();
    encode_request(quiet, buffer);
    CHECK(one_message(buffer, type, payload));
    RequestMessage received_quiet;
    CHECK(decode_request(payload.data(), payload.size(), received_quiet));
    CHECK(received_quiet.layout_id.empty());
    CHECK_EQ(received_quiet.wants_filtering, false);
    CHECK_EQ(received_quiet.wants_audio_target, false);
    CHECK_EQ(received_quiet.wants_handedness, false);
    CHECK_EQ(received_quiet.wants_render_scale, false);
    CHECK_EQ(received_quiet.wants_display_size, false);
    CHECK_EQ(received_quiet.menu_open, false);
    CHECK_EQ(received_quiet.wants_reset, false);
    CHECK_EQ(received_quiet.wants_quit, false);
    CHECK_EQ(received_quiet.wants_screen_gap, false);
    CHECK_EQ(received_quiet.wants_right_stick_mode, false);
    CHECK_EQ(received_quiet.wants_left_stick_press, false);
    CHECK_EQ(received_quiet.wants_rumble_pak, false);
    CHECK_EQ(received_quiet.wants_menu_action, false);
    CHECK_EQ(received_quiet.wants_menu_focus, false);

    // Every field this message carries as a small integer is a value outside
    // the set it means, if the sender says so, and each one has to be refused
    // rather than becoming one of the real values by accident.
    //
    // Everything after the layout id is fixed-width, so the bytes are counted
    // back from the end -- forwards would have to walk past a string. An
    // offset that has drifted would still "pass" a rejection check by hitting
    // some other byte that happens to be refused too, so each one is first
    // identified positively: a legal value written at that offset has to come
    // out of the decoder as that field and nothing else.
    struct EnumByte {
        const char* name;
        std::size_t from_end;
        uint8_t legal;
        bool (*matches)(const RequestMessage&);
    };
    const EnumByte kEnumBytes[] = {
        {"audio target", 28, 1u,
         [](const RequestMessage& message) {
             return message.audio_target == AudioTarget::BottomScreen;
         }},
        {"handedness", 26, 1u,
         [](const RequestMessage& message) { return message.handedness == Handedness::Left; }},
        {"right stick mode", 12, 2u,
         [](const RequestMessage& message) {
             return message.right_stick_mode == RightStickMode::DPad;
         }},
        {"left stick click", 7, 2u,
         [](const RequestMessage& message) {
             return message.wants_left_stick_press &&
                    message.left_stick_press == LeftStickPress::SwapLayout;
         }},
        {"rumble pak", 6, 2u,
         [](const RequestMessage& message) {
             return message.wants_rumble_pak && message.rumble_pak;
         }},
        {"resume from save state", 5, 2u,
         [](const RequestMessage& message) {
             return message.wants_resume_from_save_state && message.resume_from_save_state;
         }},
        {"screen swap", 4, 1u, [](const RequestMessage& message) { return message.wants_screen_swap; }},
        {"LCD filter", 1, 1u,
         [](const RequestMessage& message) { return message.wants_lcd_filter && !message.lcd_filter; }},
    };

    for (const EnumByte& field : kEnumBytes) {
        std::vector<uint8_t> identified(payload);
        identified[identified.size() - field.from_end] = field.legal;
        RequestMessage decoded;
        CHECK_MSG(decode_request(identified.data(), identified.size(), decoded),
                  std::string("a legal ") + field.name + " byte was refused");
        CHECK_MSG(field.matches(decoded),
                  std::string("the byte ") + std::to_string(field.from_end) +
                      " from the end is not the " + field.name);

        std::vector<uint8_t> corrupt(payload);
        corrupt[corrupt.size() - field.from_end] = 9;
        RequestMessage rejected;
        CHECK_MSG(!decode_request(corrupt.data(), corrupt.size(), rejected),
                  std::string("an impossible ") + field.name + " was accepted");
    }

    // The press a companion sends into the host's menu is the same kind of
    // small integer, but it is only read when the sender says it means one:
    // a quiet request carries a byte there that no action stands for.
    RequestMessage pressed;
    pressed.wants_menu_action = true;
    pressed.menu_action = MenuAction::Settings;
    pressed.wants_menu_focus = true;
    pressed.menu_focus = 7;
    buffer.clear();
    encode_request(pressed, buffer);
    CHECK(one_message(buffer, type, payload));
    RequestMessage received_press;
    CHECK(decode_request(payload.data(), payload.size(), received_press));
    CHECK_EQ(received_press.wants_menu_action, true);
    CHECK(received_press.menu_action == MenuAction::Settings);
    CHECK_EQ(received_press.wants_menu_focus, true);
    CHECK_EQ(static_cast<int>(received_press.menu_focus), 7);
    // A focus past what its byte holds -- a Wii U menu's newer sections --
    // arrives whole; a host from before it would read 0 there.
    pressed.menu_focus = 1300;
    buffer.clear();
    encode_request(pressed, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_request(payload.data(), payload.size(), received_press));
    CHECK_EQ(static_cast<int>(received_press.menu_focus), 1300);
    pressed.menu_focus = 7;

    // The menu button's own action, which closes rather than steps back.
    pressed.menu_action = MenuAction::Close;
    buffer.clear();
    encode_request(pressed, buffer);
    CHECK(one_message(buffer, type, payload));
    RequestMessage received_close;
    CHECK(decode_request(payload.data(), payload.size(), received_close));
    CHECK(received_close.menu_action == MenuAction::Close);

    std::vector<uint8_t> impossible(payload);
    impossible[impossible.size() - 10] = 200;
    RequestMessage refused;
    CHECK_MSG(!decode_request(impossible.data(), impossible.size(), refused),
              "a press no button could have made was accepted");
}

void test_input_round_trip() {
    InputMessage sent;
    sent.buttons = 0b101010101010u;
    sent.touching = true;
    sent.touch_x = 200;
    sent.touch_y = 150;
    // Negative on purpose: the circle pad is signed and travels as unsigned
    // bytes, so a sign lost in the round trip would make a companion's stick
    // push the wrong way.
    sent.circle_x = -12345;
    sent.circle_y = 32000;
    // Same reasoning, and the same wire slot shape, as the circle pad above.
    sent.c_stick_x = -30000;
    sent.c_stick_y = 8000;
    // How the device is being held. Small, signed and fractional -- a gyro at
    // rest reads as a fraction of a radian per second -- so anything that
    // quantised these on the way through would land on zero and the far end
    // would see a pad that never moves.
    sent.motion.present = true;
    sent.motion.gyro_x = -0.0125f;
    sent.motion.gyro_y = 1.75f;
    sent.motion.gyro_z = 0.0f;
    sent.motion.accel_x = 0.03f;
    sent.motion.accel_y = -9.79f;
    sent.motion.accel_z = 0.42f;
    sent.motion.sampled_us = 4000000000u;  // past the halfway point of the wrap

    std::vector<uint8_t> buffer;
    encode_input(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::Input);

    InputMessage received;
    CHECK(decode_input(payload.data(), payload.size(), received));
    CHECK_EQ(received.buttons, sent.buttons);
    CHECK_EQ(received.touching, true);
    CHECK_EQ(received.touch_x, 200);
    CHECK_EQ(received.touch_y, 150);
    CHECK_EQ(received.circle_x, -12345);
    CHECK_EQ(received.circle_y, 32000);
    CHECK(received.motion == sent.motion);
    // Spelled out as well as compared, so a failure says which part went
    // wrong rather than only that something did.
    CHECK(received.motion.present);
    CHECK_EQ(received.motion.gyro_x, -0.0125f);
    CHECK_EQ(received.motion.accel_y, -9.79f);
    CHECK_EQ(received.motion.sampled_us, 4000000000u);

    // And a device with no sensors says so rather than reporting a pad that
    // is being held perfectly still -- which to a game is a real reading.
    InputMessage still;
    std::vector<uint8_t> without;
    encode_input(still, without);
    CHECK(one_message(without, type, payload));
    InputMessage received_still;
    CHECK(decode_input(payload.data(), payload.size(), received_still));
    CHECK(!received_still.motion.present);
    CHECK_EQ(received.c_stick_x, -30000);
    CHECK_EQ(received.c_stick_y, 8000);
}

void test_menu_sound_round_trip() {
    for (const MenuSound sound : {MenuSound::MoveCursor, MenuSound::Cancel, MenuSound::OpenQuickMenu, MenuSound::SaveState}) {
        MenuSoundMessage sent;
        sent.sound = sound;
        std::vector<uint8_t> buffer;
        encode_menu_sound(sent, buffer);
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        CHECK(one_message(buffer, type, payload));
        CHECK(type == MessageType::MenuSound);
        MenuSoundMessage received;
        CHECK(decode_menu_sound(payload.data(), payload.size(), received));
        CHECK(received.sound == sound);
    }
}

void test_rumble_round_trip() {
    for (const bool active : {true, false}) {
        RumbleMessage sent;
        sent.active = active;

        std::vector<uint8_t> buffer;
        encode_rumble(sent, buffer);
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        CHECK(one_message(buffer, type, payload));
        CHECK(type == MessageType::Rumble);

        RumbleMessage received;
        received.active = !active;  // start wrong, so a no-op decode cannot pass by accident
        CHECK(decode_rumble(payload.data(), payload.size(), received));
        CHECK_EQ(received.active, active);
    }
}

// A companion's pads, each on its own, ride the Input datagram as a tail --
// and neither end built before that existed is confused by the other.
void test_a_companions_pads_ride_the_input_as_a_tail() {
    InputMessage sent;
    sent.buttons = 3;
    sent.reports_pads = true;
    ReportedPad first;
    first.id = 7;
    first.held.buttons = 0x81u;
    first.held.circle_x = -32000;
    first.held.c_stick_y = 1234;
    first.name = "Steam Deck";
    first.slot = 1;
    ReportedPad second;
    second.id = 4000000000u;
    second.held.motion.present = true;
    second.held.motion.gyro_z = -0.5f;
    second.held.motion.accel_y = 9.81f;
    second.held.motion.sampled_us = 42;
    sent.pads = {first, second};

    std::vector<uint8_t> buffer;
    encode_input(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    InputMessage received;
    CHECK(decode_input(payload.data(), payload.size(), received));
    CHECK(received.reports_pads);
    CHECK_EQ(received.pads.size(), static_cast<std::size_t>(2));
    if (received.pads.size() == 2) {
        CHECK_EQ(received.pads[0].id, 7u);
        CHECK_EQ(received.pads[0].held.buttons, 0x81u);
        CHECK_EQ(received.pads[0].held.circle_x, -32000);
        CHECK_EQ(received.pads[0].held.c_stick_y, 1234);
        CHECK(!received.pads[0].held.motion.present);
        CHECK_EQ(received.pads[1].id, 4000000000u);
        CHECK(received.pads[1].held.motion == second.held.motion);
        CHECK(received.pads[0].name == "Steam Deck");
        CHECK(received.pads[1].name.empty());
        CHECK_EQ(received.pads[0].slot, 1);
        CHECK_EQ(received.pads[1].slot, -1);
    }
    CHECK_EQ(received.buttons, 3u);

    // A companion from before names travelled: the pads, and nothing after.
    // The names are a length byte each -- 1 + 10 and 1 here -- after them
    // all, and a slot byte each after those.
    InputMessage unslotted;
    CHECK(decode_input(payload.data(), payload.size() - 2, unslotted));
    CHECK(unslotted.pads.size() == 2 && unslotted.pads[0].name == "Steam Deck" &&
          unslotted.pads[0].slot == -1);
    InputMessage unnamed;
    CHECK(decode_input(payload.data(), payload.size() - 14, unnamed));
    CHECK(unnamed.reports_pads && unnamed.pads.size() == 2);
    CHECK(unnamed.pads.size() == 2 && unnamed.pads[0].name.empty() &&
          unnamed.pads[0].held.buttons == 0x81u);

    // A name longer than the wire allows is cut, not refused.
    InputMessage long_named;
    long_named.reports_pads = true;
    ReportedPad wordy;
    wordy.name = std::string(200, 'x');
    long_named.pads = {wordy};
    buffer.clear();
    encode_input(long_named, buffer);
    CHECK(one_message(buffer, type, payload));
    InputMessage received_long;
    CHECK(decode_input(payload.data(), payload.size(), received_long));
    CHECK(received_long.pads.size() == 1 &&
          received_long.pads[0].name.size() == kMaxReportedPadName);

    // A companion with no pads at all still says it reports them -- it simply
    // has none -- which is not the same as an older companion saying nothing.
    InputMessage none;
    none.reports_pads = true;
    buffer.clear();
    encode_input(none, buffer);
    CHECK(one_message(buffer, type, payload));
    InputMessage received_none;
    CHECK(decode_input(payload.data(), payload.size(), received_none));
    CHECK(received_none.reports_pads && received_none.pads.empty());

    // An older companion sends no tail, and is read as reporting nothing.
    InputMessage older;
    older.buttons = 9;
    buffer.clear();
    encode_input(older, buffer);
    CHECK(one_message(buffer, type, payload));
    InputMessage received_older;
    received_older.reports_pads = true;  // start wrong
    CHECK(decode_input(payload.data(), payload.size(), received_older));
    CHECK_MSG(!received_older.reports_pads,
              "a companion that never sent its pads was read as having none, which would take "
              "its buttons away from the GamePad");
    CHECK_EQ(received_older.buttons, 9u);

    // A tail cut short is a bad datagram, not a partial list of pads.
    buffer.clear();
    encode_input(sent, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(!decode_input(payload.data(), payload.size() - 3, received));
}

// Each input is numbered, after the pads, so the host can say which picture
// it first reached -- the only way a companion can measure a tap's whole trip
// -- and so a repeat of the same input, sent again in case the first was lost,
// is recognised as one.
void test_an_input_carries_its_number_and_the_host_echoes_it() {
    InputMessage sent;
    sent.touching = true;
    sent.reports_pads = true;
    sent.has_sequence = true;
    sent.sequence = 4000000123u;
    ReportedPad pad;
    pad.id = 3;
    pad.name = "Pro";
    pad.slot = 0;
    sent.pads = {pad};

    std::vector<uint8_t> buffer;
    encode_input(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    InputMessage received;
    CHECK(decode_input(payload.data(), payload.size(), received));
    CHECK(received.has_sequence);
    CHECK_EQ(received.sequence, 4000000123u);
    CHECK(received.touching);
    CHECK(received.pads.size() == 1 && received.pads[0].name == "Pro" &&
          received.pads[0].slot == 0);

    // A companion from before inputs were numbered is read as not numbering
    // them, whatever the message being decoded into said before.
    InputMessage unnumbered;
    CHECK(decode_input(payload.data(), payload.size() - 4, unnumbered));
    CHECK(!unnumbered.has_sequence);
    CHECK(unnumbered.pads.size() == 1 && unnumbered.pads[0].slot == 0);
    CHECK(decode_input(payload.data(), payload.size() - 4, received));
    CHECK_MSG(!received.has_sequence, "a number from an earlier input was left standing");

    // The echo: which input, and the first picture made with it.
    InputEchoMessage echo;
    echo.sequence = 77;
    echo.serial = 0x123456789ull;
    std::vector<uint8_t> echoed;
    encode_input_echo_payload(echo, echoed);
    InputEchoMessage back;
    CHECK(decode_input_echo_payload(echoed.data(), echoed.size(), back));
    CHECK_EQ(back.sequence, 77u);
    CHECK_EQ(back.serial, 0x123456789ull);
    CHECK(!decode_input_echo_payload(echoed.data(), echoed.size() - 1, back));
}

// Rumble can name a pad now; an older host names none, which means the whole
// device, as it always did.
void test_rumble_can_name_one_pad() {
    RumbleMessage sent;
    sent.active = true;
    sent.pad = 12;
    std::vector<uint8_t> buffer;
    encode_rumble(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    RumbleMessage received;
    CHECK(decode_rumble(payload.data(), payload.size(), received));
    CHECK(received.active);
    CHECK_EQ(received.pad, 12u);

    // What an older host sends: the one byte.
    const uint8_t older[] = {1};
    RumbleMessage received_older;
    received_older.pad = 3;
    CHECK(decode_rumble(older, sizeof(older), received_older));
    CHECK(received_older.active);
    CHECK_EQ(received_older.pad, RumbleMessage::kWholeDevice);
    CHECK_EQ(received_older.ping_ms, 0);

    // A ping carries its own length, so the pad that buzzes ends it itself;
    // an ordinary rumble carries none and stays on until told otherwise.
    CHECK_EQ(received.ping_ms, 0);
    RumbleMessage ping;
    ping.active = true;
    ping.pad = 5;
    ping.ping_ms = 180;
    buffer.clear();
    encode_rumble(ping, buffer);
    CHECK(one_message(buffer, type, payload));
    RumbleMessage received_ping;
    CHECK(decode_rumble(payload.data(), payload.size(), received_ping));
    CHECK(received_ping.active);
    CHECK_EQ(received_ping.pad, 5u);
    CHECK_EQ(received_ping.ping_ms, 180);
}

// A video frame's recovery pieces put back any missing pieces, however they
// fall, up to as many as the block has recovery pieces here -- the way
// Moonlight's Reed-Solomon does. More than that is more than it can do, and
// it says so by leaving the pieces missing rather than making them up.
void test_lost_pieces_come_back_from_parity() {
    constexpr std::size_t kPiece = 100;
    unsigned seed = 12345;
    const auto random = [&seed](std::size_t below) {
        seed = seed * 1103515245u + 12345u;
        return static_cast<std::size_t>((seed >> 8) % below);
    };
    for (const std::size_t size : {std::size_t{37}, std::size_t{100}, std::size_t{450},
                                   std::size_t{1650}, std::size_t{8000}, std::size_t{30050}}) {
        std::vector<uint8_t> payload(size);
        for (std::size_t index = 0; index < size; ++index) {
            payload[index] = static_cast<uint8_t>((index * 131 + 7 + index / 97) & 0xFFu);
        }
        const std::size_t count = (size + kPiece - 1) / kPiece;
        const std::vector<std::vector<uint8_t>> parity = build_frame_parity(payload, kPiece);
        CHECK_EQ(parity.size(), frame_parity_count(count));
        // At least two, and about a fifth as many again.
        CHECK(parity.size() >= 2);
        CHECK(parity.size() * 100 >= count * 20);
        // Numbered straight on from the frame's own.
        CHECK_EQ(frame_parity_first_index(count), count);
        const auto pieces = [&] {
            std::vector<std::vector<uint8_t>> out(count);
            for (std::size_t index = 0; index < count; ++index) {
                const std::size_t offset = index * kPiece;
                out[index].assign(payload.begin() + static_cast<long>(offset),
                                  payload.begin() + static_cast<long>(std::min(size, offset + kPiece)));
            }
            return out;
        };
        const std::vector<bool> all_parity(parity.size(), true);

        // Each single piece, the last (short) one included.
        for (std::size_t lost = 0; lost < count; ++lost) {
            std::vector<std::vector<uint8_t>> fragments = pieces();
            std::vector<bool> received(count, true);
            fragments[lost].clear();
            received[lost] = false;
            CHECK_EQ(recover_fragments(fragments, received, parity, all_parity), 1u);
            CHECK(fragments == pieces());
        }

        // Random losses, anywhere, as many as there are recovery pieces --
        // for a frame of one block. (A frame of several is checked below.)
        if (count <= kMaxDataPerParityBlock) {
            for (int trial = 0; trial < 50; ++trial) {
                std::vector<std::vector<uint8_t>> fragments = pieces();
                std::vector<bool> received(count, true);
                const std::size_t lose = std::min(parity.size(), count);
                std::size_t lost = 0;
                while (lost < lose) {
                    const std::size_t index = random(count);
                    if (!received[index]) continue;
                    received[index] = false;
                    fragments[index].assign(3, 0xEE);  // rubbish, to be replaced
                    ++lost;
                }
                CHECK_EQ(recover_fragments(fragments, received, parity, all_parity),
                         static_cast<unsigned>(lose));
                CHECK(fragments == pieces());
            }
            // Some recovery pieces lost as well: still fine while what is left
            // covers what is missing.
            if (parity.size() >= 2 && count >= 1) {
                std::vector<std::vector<uint8_t>> fragments = pieces();
                std::vector<bool> received(count, true);
                std::vector<bool> some_parity(parity.size(), true);
                some_parity[0] = false;
                received[count - 1] = false;
                CHECK_EQ(recover_fragments(fragments, received, parity, some_parity), 1u);
                CHECK(fragments == pieces());
            }
            // One more lost than there are recovery pieces: nothing put back,
            // and nothing invented.
            if (count > parity.size()) {
                std::vector<std::vector<uint8_t>> fragments = pieces();
                std::vector<bool> received(count, true);
                for (std::size_t index = 0; index <= parity.size(); ++index) {
                    received[index] = false;
                }
                CHECK_EQ(recover_fragments(fragments, received, parity, all_parity), 0u);
                for (std::size_t index = 0; index <= parity.size(); ++index) {
                    CHECK(!received[index]);
                }
            }
        } else {
            // Several blocks: a burst in each, each put back by its own.
            std::vector<std::vector<uint8_t>> fragments = pieces();
            std::vector<bool> received(count, true);
            received[0] = received[1] = received[2] = false;
            received[count - 1] = received[count - 2] = false;
            CHECK_EQ(recover_fragments(fragments, received, parity, all_parity), 5u);
            CHECK(fragments == pieces());
        }

        // Nor without the recovery pieces.
        {
            std::vector<std::vector<uint8_t>> fragments = pieces();
            std::vector<bool> received(count, true);
            received[0] = false;
            const std::vector<bool> none(parity.size(), false);
            CHECK_EQ(recover_fragments(fragments, received, parity, none), 0u);
        }
    }
}

// A request for missing pieces says which frame and which pieces, and a
// request claiming more than the cap is refused rather than trusted.
void test_a_repair_request_round_trips() {
    RepairRequestMessage request;
    request.screen = ScreenId::Bottom;
    request.serial = 123456789012ull;
    request.pieces = {0, 7, 8, 9, 81};
    std::vector<uint8_t> out;
    encode_repair_request_payload(request, out);
    RepairRequestMessage back;
    CHECK(decode_repair_request_payload(out.data(), out.size(), back));
    CHECK(back.screen == ScreenId::Bottom);
    CHECK_EQ(back.serial, request.serial);
    CHECK(back.pieces == request.pieces);
    CHECK(!back.whole_frame);
    // A frame lost outright, asked for by its place in the run.
    RepairRequestMessage whole;
    whole.screen = ScreenId::Top;
    whole.whole_frame = true;
    whole.sent_index = 65535;
    std::vector<uint8_t> whole_out;
    encode_repair_request_payload(whole, whole_out);
    RepairRequestMessage whole_back;
    CHECK(decode_repair_request_payload(whole_out.data(), whole_out.size(), whole_back));
    CHECK(whole_back.whole_frame);
    CHECK_EQ(whole_back.sent_index, 65535);
    CHECK(whole_back.pieces.empty());
    // Cut short: not a request.
    CHECK(!decode_repair_request_payload(out.data(), out.size() - 1, back));
    // A count past the cap, whatever follows it.
    std::vector<uint8_t> greedy;
    RepairRequestMessage none;
    encode_repair_request_payload(none, greedy);
    greedy[greedy.size() - 2] = 0xFF;
    greedy[greedy.size() - 1] = 0xFF;
    CHECK(!decode_repair_request_payload(greedy.data(), greedy.size(), back));
}

// A Bye can say the player quit from the companion, so it closes its own app
// too. An ordinary one says nothing -- and is still empty, so a companion from
// before this reads either the way it always did.
void test_a_bye_can_say_quit_too() {
    for (const bool quit_too : {false, true}) {
        std::vector<uint8_t> stream;
        encode_bye(stream, quit_too);
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        bool fatal = false;
        CHECK(take_message(stream, type, payload, fatal));
        CHECK(!fatal);
        CHECK(type == MessageType::Bye);
        CHECK_EQ(bye_says_quit_too(payload), quit_too);
        if (!quit_too) CHECK(payload.empty());
    }
}

// Several messages written back to back must come out one at a time, in order,
// and a partial tail must simply not be returned yet.
void test_stream_framing() {
    std::vector<uint8_t> stream;
    encode_ping(stream);
    InputMessage input;
    input.buttons = 7;
    encode_input(input, stream);
    encode_bye(stream);

    const std::size_t complete_size = stream.size();

    // Feed it a byte at a time; nothing may come out early or twice.
    std::vector<uint8_t> buffer;
    std::vector<MessageType> seen;
    for (std::size_t index = 0; index < complete_size; ++index) {
        buffer.push_back(stream[index]);
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        bool fatal = false;
        while (take_message(buffer, type, payload, fatal)) {
            CHECK(!fatal);
            seen.push_back(type);
        }
        CHECK(!fatal);
    }
    CHECK_EQ(seen.size(), static_cast<std::size_t>(3));
    if (seen.size() == 3) {
        CHECK(seen[0] == MessageType::Ping);
        CHECK(seen[1] == MessageType::Input);
        CHECK(seen[2] == MessageType::Bye);
    }
    CHECK(buffer.empty());
}

// A length field larger than the cap has to be fatal rather than a giant
// allocation: it means the stream is desynchronised or hostile.
void test_absurd_length_is_fatal() {
    std::vector<uint8_t> buffer = {0xff, 0xff, 0xff, 0xff, 0x01, 0x00};
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    bool fatal = false;
    CHECK(!take_message(buffer, type, payload, fatal));
    CHECK_MSG(fatal, "an absurd message length was not treated as fatal");
}

// Every decoder must reject a truncated payload at every possible cut, rather
// than reading past the end.
void test_truncation_is_rejected_everywhere() {
    HelloMessage hello;
    hello.device_name = "device";
    WelcomeMessage welcome;
    welcome.game_title = "game";
    welcome.rom_sha1 = "hash";
    SessionStateMessage state;
    state.layout_id = "vertical";
    InputMessage input;
    FrameMessage frame;
    frame.width = 4;
    frame.height = 4;
    frame.payload = {1, 2, 3, 4};

    struct Case {
        const char* name;
        std::vector<uint8_t> encoded;
    };
    std::vector<Case> cases;
    {
        std::vector<uint8_t> buffer;
        encode_hello(hello, buffer);
        cases.push_back({"hello", buffer});
    }
    {
        std::vector<uint8_t> buffer;
        encode_welcome(welcome, buffer);
        cases.push_back({"welcome", buffer});
    }
    {
        std::vector<uint8_t> buffer;
        encode_session_state(state, buffer);
        cases.push_back({"session", buffer});
    }
    {
        std::vector<uint8_t> buffer;
        encode_input(input, buffer);
        cases.push_back({"input", buffer});
    }
    {
        std::vector<uint8_t> buffer;
        encode_frame(frame, buffer);
        cases.push_back({"frame", buffer});
    }

    for (const Case& entry : cases) {
        // Strip the 6-byte header to get the payload, then cut it short.
        const std::vector<uint8_t> payload(entry.encoded.begin() + 6, entry.encoded.end());
        for (std::size_t length = 0; length < payload.size(); ++length) {
            HelloMessage a;
            WelcomeMessage b;
            SessionStateMessage c;
            InputMessage d;
            FrameMessage e;
            RejectMessage f;
            // Whichever decoder it is, a short read must fail, not crash.
            decode_hello(payload.data(), length, a);
            decode_welcome(payload.data(), length, b);
            decode_session_state(payload.data(), length, c);
            decode_input(payload.data(), length, d);
            decode_frame(payload.data(), length, e);
            decode_reject(payload.data(), length, f);
        }
        // The full payload still decodes with its own decoder.
        if (std::string(entry.name) == "frame") {
            FrameMessage decoded;
            CHECK(decode_frame(payload.data(), payload.size(), decoded));
            CHECK_EQ(decoded.width, 4);
            CHECK_EQ(decoded.payload.size(), static_cast<std::size_t>(4));
        }
    }
}

// Random bytes must never be accepted as a valid message.
void test_random_bytes_are_rejected() {
    std::mt19937 generator(1234);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int attempt = 0; attempt < 2000; ++attempt) {
        std::vector<uint8_t> noise(1 + (attempt % 40));
        for (uint8_t& value : noise) {
            value = static_cast<uint8_t>(byte(generator));
        }
        HelloMessage hello;
        // A hello carries a magic number, so noise must essentially never pass.
        if (decode_hello(noise.data(), noise.size(), hello)) {
            CHECK_MSG(false, "random bytes decoded as a valid hello");
        }
        FrameMessage frame;
        decode_frame(noise.data(), noise.size(), frame);
        InputMessage input;
        decode_input(noise.data(), noise.size(), input);
    }
}

void test_pixels_round_trip() {
    const int width = 256;
    const int height = 192;
    std::vector<uint32_t> source(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Values that survive 5-6-5 exactly, so the comparison is exact.
            const uint32_t r = static_cast<uint32_t>((x / 8) % 32) * 8;
            const uint32_t g = static_cast<uint32_t>((y / 4) % 64) * 4;
            const uint32_t b = static_cast<uint32_t>((x / 16) % 32) * 8;
            source[static_cast<std::size_t>(y) * width + x] =
                ((r | (r >> 5)) << 16) | ((g | (g >> 6)) << 8) | (b | (b >> 5));
        }
    }

    std::vector<uint8_t> encoded;
    CHECK(encode_screen_pixels(source.data(), width, height, width, encoded));
    CHECK(!encoded.empty());
    // Compression has to actually pay for itself, or streaming is pointless.
    CHECK_MSG(encoded.size() < source.size() * 4 / 8,
              "compressed frame was " + std::to_string(encoded.size()) + " bytes");

    std::vector<uint32_t> decoded;
    CHECK(decode_screen_pixels(encoded, width, height, decoded));
    CHECK_EQ(decoded.size(), source.size());
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        if ((decoded[index] & 0x00ffffffu) != (source[index] & 0x00ffffffu)) {
            CHECK_MSG(false, "pixel " + std::to_string(index) + " did not survive the round trip");
            break;
        }
    }

    // A strided source (the core's framebuffer is one screen of a taller
    // buffer) must encode the right rows.
    std::vector<uint32_t> tall(static_cast<std::size_t>(width) * height * 2, 0x00112233u);
    for (std::size_t index = 0; index < source.size(); ++index) {
        tall[source.size() + index] = source[index];
    }
    std::vector<uint8_t> from_stride;
    CHECK(encode_screen_pixels(tall.data() + source.size(), width, height, width, from_stride));
    CHECK(from_stride == encoded);
}

// Which encoding a frame was sent with has to survive the trip, because it is
// what tells the far end whether the payload is a whole screen or a patch onto
// the one it already has. Reading a patch as a whole frame produces garbage,
// so an encoding this build has no decoder for is refused outright.
void test_a_frame_carries_its_encoding() {
    for (const FrameEncoding encoding :
         {FrameEncoding::Rgb565Deflate, FrameEncoding::Rgb565DeflateTiles}) {
        FrameMessage frame;
        frame.screen = ScreenId::Bottom;
        frame.serial = 9;
        frame.width = 16;
        frame.height = 16;
        frame.encoding = encoding;
        frame.payload = {4, 1, 2, 3};

        std::vector<uint8_t> buffer;
        encode_frame(frame, buffer);
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        CHECK(one_message(buffer, type, payload));
        CHECK(type == MessageType::Frame);

        FrameMessage received;
        CHECK_MSG(decode_frame(payload.data(), payload.size(), received),
                  "a frame with encoding " +
                      std::to_string(static_cast<int>(encoding)) + " was refused");
        CHECK(received.encoding == encoding);
        CHECK(received.payload == frame.payload);
        CHECK_EQ(received.serial, static_cast<uint64_t>(9));

        // The encoding byte sits just before the payload length, which is the
        // last four bytes of the header ahead of the payload itself.
        std::vector<uint8_t> unknown(payload);
        unknown[payload.size() - frame.payload.size() - 5] = 0x7f;
        FrameMessage rejected;
        CHECK_MSG(!decode_frame(unknown.data(), unknown.size(), rejected),
                  "a frame in an encoding this build cannot read was accepted");
    }
}

// A DS screen full of tile art, with a small moving thing on it -- which is
// most of what a game actually puts on a screen from one frame to the next.
std::vector<uint32_t> screen_with_marker_at(int width, int height, int marker_x) {
    std::vector<uint32_t> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Built from 5- and 6-bit values expanded the way the decoder
            // expands them, so the comparison below is exact rather than
            // measuring how lossy 5-6-5 is.
            const uint32_t five = static_cast<uint32_t>(((x / 8) ^ (y / 8)) & 0x1f);
            const uint32_t six = static_cast<uint32_t>(((x / 8) + (y / 8)) & 0x3f);
            pixels[static_cast<std::size_t>(y) * width + x] =
                (((five << 3) | (five >> 2)) << 16) | (((six << 2) | (six >> 4)) << 8) |
                ((five << 3) | (five >> 2));
        }
    }
    for (int y = 90; y < 96; ++y) {
        for (int x = marker_x; x < marker_x + 6; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = 0x00ff0000u;
        }
    }
    return pixels;
}

// Only the tiles that moved go on the wire, and what the far end ends up
// holding is the frame that was sent -- not an approximation of it.
void test_only_changed_tiles_are_sent() {
    constexpr int width = 256;
    constexpr int height = 192;

    // Nothing to compare against: the first frame carries the whole screen,
    // and costs about what a whole frame costs.
    std::vector<uint8_t> previous;
    const std::vector<uint32_t> first = screen_with_marker_at(width, height, 100);
    std::vector<uint8_t> keyframe;
    CHECK(encode_screen_tiles(first.data(), width, height, width, previous, keyframe));

    std::vector<uint8_t> whole;
    CHECK(encode_screen_pixels(first.data(), width, height, width, whole));
    CHECK_MSG(keyframe.size() < whole.size() * 2,
              "a whole frame in tile order cost " + std::to_string(keyframe.size()) +
                  " against " + std::to_string(whole.size()) + " in row order");

    std::vector<uint8_t> received;
    CHECK(decode_screen_tiles(keyframe, width, height, received));
    CHECK(received == previous);

    // Now the marker moves and nothing else does. That is six pixels across
    // two tiles at most, and it has to cost about that rather than a frame.
    const std::vector<uint32_t> second = screen_with_marker_at(width, height, 108);
    std::vector<uint8_t> delta;
    CHECK(encode_screen_tiles(second.data(), width, height, width, previous, delta));
    CHECK_MSG(delta.size() * 20 < whole.size(),
              "a six-pixel change cost " + std::to_string(delta.size()) + " bytes against " +
                  std::to_string(whole.size()) + " for the whole frame");

    CHECK(decode_screen_tiles(delta, width, height, received));
    CHECK_MSG(received == previous, "the patched screen is not the frame that was sent");

    // And it really is the second frame, not the first with a hole in it.
    std::vector<uint32_t> expanded;
    expand_rgb565(received, expanded);
    CHECK(expanded == second);

    // A frame where nothing at all moved is a bitmap of zeroes and an empty
    // compressed body -- the cheapest thing this can send.
    std::vector<uint8_t> nothing;
    CHECK(encode_screen_tiles(second.data(), width, height, width, previous, nothing));
    CHECK_MSG(nothing.size() < 64, "an unchanged screen cost " + std::to_string(nothing.size()) +
                                       " bytes");
    CHECK(decode_screen_tiles(nothing, width, height, received));
    expand_rgb565(received, expanded);
    CHECK(expanded == second);
}

// A tile one unconfirmed frame changed and the next puts back still goes out.
// The host diffs against the last frame the companion confirmed, so the put-
// back tile matches that and would be left out -- and when the companion then
// confirms the later frame, the host takes it to hold that frame exactly,
// while the tile on its screen is still the one in between. Measured as tiles
// of fire and sprites left standing on a moving DS game's screen.
void test_a_tile_put_back_before_its_change_was_confirmed_still_goes_out() {
    constexpr int width = 256;
    constexpr int height = 192;
    const std::vector<uint32_t> at_rest = screen_with_marker_at(width, height, 100);
    const std::vector<uint32_t> moved = screen_with_marker_at(width, height, 200);

    // Confirmed: the marker at rest.
    std::vector<uint8_t> confirmed;
    std::vector<uint8_t> payload;
    CHECK(encode_screen_tiles(at_rest.data(), width, height, width, confirmed, payload));
    std::vector<uint8_t> far_end;
    CHECK(decode_screen_tiles(payload, width, height, far_end));

    // Sent, not yet confirmed: the marker moved.
    std::vector<uint8_t> in_flight = confirmed;
    CHECK(encode_screen_tiles(moved.data(), width, height, width, in_flight, payload));
    CHECK(decode_screen_tiles(payload, width, height, far_end));

    // And back again, diffed against what was confirmed. Against that alone,
    // nothing moved.
    std::vector<uint8_t> alone = confirmed;
    CHECK(encode_screen_tiles(at_rest.data(), width, height, width, alone, payload));
    std::vector<uint8_t> stale = far_end;
    CHECK(decode_screen_tiles(payload, width, height, stale));
    CHECK_MSG(stale != alone, "the case this guards no longer arises, so the test proves nothing");

    // Told what is still in flight, the tiles the move touched go out again,
    // and the far end holds exactly the frame the host will take it to hold.
    std::vector<uint8_t> told = confirmed;
    CHECK(encode_screen_tiles(at_rest.data(), width, height, width, told, payload,
                              PixelChannelOrder::XrgbBytes, {&in_flight}));
    CHECK(decode_screen_tiles(payload, width, height, far_end));
    CHECK_MSG(far_end == told, "a tile put back before its change was confirmed was left behind");
}

// A screen that changes size starts again from a whole frame rather than
// patching new tiles onto a buffer of the wrong shape.
void test_a_resized_screen_starts_over() {
    std::vector<uint8_t> previous;
    const std::vector<uint32_t> small = screen_with_marker_at(64, 64, 10);
    std::vector<uint8_t> payload;
    CHECK(encode_screen_tiles(small.data(), 64, 64, 64, previous, payload));
    CHECK_EQ(previous.size(), static_cast<std::size_t>(64 * 64 * 2));

    const std::vector<uint32_t> large = screen_with_marker_at(256, 192, 100);
    CHECK(encode_screen_tiles(large.data(), 256, 192, 256, previous, payload));
    CHECK_EQ(previous.size(), static_cast<std::size_t>(256 * 192 * 2));

    std::vector<uint8_t> received;
    CHECK(decode_screen_tiles(payload, 256, 192, received));
    std::vector<uint32_t> expanded;
    expand_rgb565(received, expanded);
    CHECK(expanded == large);
}

// The decoder reads a tile count out of the payload, so a payload claiming
// something absurd must be refused rather than believed.
void test_corrupt_tiled_payload_is_rejected() {
    constexpr int width = 256;
    constexpr int height = 192;
    std::vector<uint8_t> previous;
    const std::vector<uint32_t> source = screen_with_marker_at(width, height, 100);
    std::vector<uint8_t> payload;
    CHECK(encode_screen_tiles(source.data(), width, height, width, previous, payload));

    std::vector<uint8_t> screen;
    CHECK(decode_screen_tiles(payload, width, height, screen));

    // A tile size nothing produces, which would size the bitmap from a number
    // this end was told rather than one it worked out.
    for (const uint8_t shift : {uint8_t{0}, uint8_t{1}, uint8_t{9}, uint8_t{255}}) {
        std::vector<uint8_t> wild(payload);
        wild[0] = shift;
        std::vector<uint8_t> ignored;
        CHECK_MSG(!decode_screen_tiles(wild, width, height, ignored),
                  "a tile shift of " + std::to_string(shift) + " was accepted");
    }

    // Truncated anywhere: the bitmap, the compressed body, or the lot.
    for (const std::size_t keep : {std::size_t{0}, std::size_t{1}, std::size_t{8},
                                   payload.size() / 2, payload.size() - 1}) {
        std::vector<uint8_t> cut(payload.begin(), payload.begin() + static_cast<long>(keep));
        std::vector<uint8_t> ignored;
        // Not a crash and not a half-written screen: either it is refused, or
        // it happened to still be a whole valid payload.
        decode_screen_tiles(cut, width, height, ignored);
    }

    // Random bytes with a legal-looking shift in front.
    std::mt19937 rng(99);
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::vector<uint8_t> garbage(payload.size());
        for (uint8_t& byte : garbage) byte = static_cast<uint8_t>(rng() & 0xff);
        garbage[0] = 4;
        std::vector<uint8_t> ignored;
        decode_screen_tiles(garbage, width, height, ignored);
    }
}

void test_corrupt_pixel_payload_is_rejected() {
    std::vector<uint8_t> garbage(512, 0xab);
    std::vector<uint32_t> decoded;
    CHECK(!decode_screen_pixels(garbage, 256, 192, decoded));
    // A valid payload for the wrong dimensions must also fail rather than
    // producing a half-filled screen.
    std::vector<uint32_t> source(64 * 64, 0x00ff00ffu);
    std::vector<uint8_t> encoded;
    CHECK(encode_screen_pixels(source.data(), 64, 64, 64, encoded));
    CHECK(!decode_screen_pixels(encoded, 256, 192, decoded));
}

// The UDP media channel's own header, carrying which session a datagram
// belongs to and which of the four latency-critical message types it is --
// see kMediaPort. No length prefix (a datagram already delimits itself), so
// this is its own small round trip rather than going through take_message().
void test_media_header_round_trip() {
    std::vector<uint8_t> buffer;
    encode_media_header(0xfeedfacecafebeefull, MessageType::Audio, buffer);
    // Payload after the header, so a real caller's decode can find it too.
    buffer.push_back(0x42);

    uint64_t token = 0;
    MessageType type = MessageType::Ping;
    const uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    CHECK(decode_media_header(buffer.data(), buffer.size(), token, type, payload, payload_size));
    CHECK(token == 0xfeedfacecafebeefull);
    CHECK(type == MessageType::Audio);
    CHECK_EQ(static_cast<int>(payload_size), 1);
    CHECK_EQ(static_cast<int>(payload[0]), 0x42);
}

void test_media_header_rejects_truncation() {
    std::vector<uint8_t> buffer;
    encode_media_header(1, MessageType::Input, buffer);
    for (std::size_t length = 0; length < buffer.size(); ++length) {
        uint64_t token = 0;
        MessageType type = MessageType::Ping;
        const uint8_t* payload = nullptr;
        std::size_t payload_size = 0;
        CHECK(!decode_media_header(buffer.data(), length, token, type, payload, payload_size));
    }
}

// A Frame message's payload travels as a short run of these instead of one
// encode_frame() call, so an H.264 frame (or a large tile payload) too big
// for one UDP datagram can still cross as several -- see kMaxMediaDatagramBytes.
void test_frame_fragment_round_trip() {
    FrameFragmentHeader header;
    header.serial = 0x1122334455667788ull;
    header.screen = ScreenId::Bottom;
    header.encoding = FrameEncoding::H264;
    header.width = 854;
    header.height = 480;
    header.fragment_index = 2;
    header.fragment_count = 5;
    header.keyframe = true;
    header.sent_index = 40000;

    const std::vector<uint8_t> slice = {0xde, 0xad, 0xbe, 0xef, 0x00};
    std::vector<uint8_t> buffer;
    encode_frame_fragment(header, slice.data(), slice.size(), buffer);

    FrameFragmentHeader received;
    const uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    CHECK(decode_frame_fragment(buffer.data(), buffer.size(), received, payload, payload_size));
    CHECK(received.serial == header.serial);
    CHECK(received.screen == ScreenId::Bottom);
    CHECK(received.encoding == FrameEncoding::H264);
    CHECK_EQ(static_cast<int>(received.width), 854);
    CHECK_EQ(static_cast<int>(received.height), 480);
    CHECK_EQ(static_cast<int>(received.fragment_index), 2);
    CHECK_EQ(static_cast<int>(received.fragment_count), 5);
    // What tells the far end the recovery it asked for has landed, so it can
    // stop asking -- see MessageType::KeyframeRequest.
    CHECK(received.keyframe);
    // And what tells it a frame went missing on the way at all.
    CHECK_EQ(static_cast<int>(received.sent_index), 40000);
    CHECK_EQ(static_cast<int>(payload_size), static_cast<int>(slice.size()));
    CHECK(std::vector<uint8_t>(payload, payload + payload_size) == slice);
}

void test_frame_fragment_rejects_truncation() {
    FrameFragmentHeader header;
    header.serial = 7;
    const std::vector<uint8_t> slice = {1, 2, 3};
    std::vector<uint8_t> buffer;
    encode_frame_fragment(header, slice.data(), slice.size(), buffer);
    // Every length up to but not including the full header must fail --
    // truncated exactly at the header boundary (no payload) is legitimate
    // and must still succeed, since a fragment can carry zero payload bytes.
    const std::size_t header_size = buffer.size() - slice.size();
    for (std::size_t length = 0; length < header_size; ++length) {
        FrameFragmentHeader received;
        const uint8_t* payload = nullptr;
        std::size_t payload_size = 0;
        CHECK(!decode_frame_fragment(buffer.data(), length, received, payload, payload_size));
    }
}

// An ack says how late the frame it covers arrived, and one from before that
// did -- a bare serial -- still reads, as not saying.
void test_frame_ack_carries_how_late_the_frame_arrived() {
    std::vector<uint8_t> buffer;
    encode_frame_ack_payload(0x0102030405060708ull, 42, buffer);
    uint64_t serial = 0;
    uint16_t link_delay = 0;
    CHECK(decode_frame_ack(buffer.data(), buffer.size(), serial, link_delay));
    CHECK(serial == 0x0102030405060708ull);
    CHECK_EQ(link_delay, uint16_t{42});

    std::vector<uint8_t> older;
    encode_frame_ack_payload(77, older);
    CHECK(decode_frame_ack(older.data(), older.size(), serial, link_delay));
    CHECK(serial == 77u);
    CHECK_EQ(link_delay, kUnknownLinkDelay);
    // And the old reader still takes the new one.
    CHECK(decode_frame_ack(buffer.data(), buffer.size(), serial));
    CHECK(serial == 0x0102030405060708ull);

    for (std::size_t length = 0; length < 8; ++length) {
        CHECK(!decode_frame_ack(buffer.data(), length, serial, link_delay));
    }
}

void test_keyframe_request_round_trip() {
    KeyframeRequestMessage request;
    request.screen = ScreenId::Bottom;
    request.last_good_serial = 0x0102030405060708ull;

    std::vector<uint8_t> buffer;
    encode_keyframe_request_payload(request, buffer);

    KeyframeRequestMessage received;
    CHECK(decode_keyframe_request_payload(buffer.data(), buffer.size(), received));
    CHECK(received.screen == ScreenId::Bottom);
    CHECK(received.last_good_serial == request.last_good_serial);

    // Anything short of the whole thing is refused rather than half-read: this
    // arrives as a bare datagram from another machine.
    for (std::size_t length = 0; length < buffer.size(); ++length) {
        KeyframeRequestMessage truncated;
        CHECK(!decode_keyframe_request_payload(buffer.data(), length, truncated));
    }
}

}  // namespace

// Every console has to survive the wire and the config file, and adding one has
// to be something the build notices. The round trips below are driven from the
// list of systems rather than from a couple of names, so a console added without
// an answer here fails as soon as it exists rather than the first time somebody
// plays one across two devices.
void test_every_system_round_trips() {
    for (const SystemProfile& profile : system_profiles()) {
        SystemId back = SystemId::Ds;
        CHECK_MSG(system_from_key(system_key(profile.id), back),
                  std::string("the key '") + system_key(profile.id) + "' was not recognised back");
        CHECK_MSG(back == profile.id,
                  profile.name + " came back from its own key as a different console");

        WelcomeMessage welcome;
        welcome.system = profile.id;
        welcome.granted_screen = ScreenId::Bottom;
        std::vector<uint8_t> bytes;
        encode_welcome(welcome, bytes);
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        CHECK(one_message(bytes, type, payload));
        WelcomeMessage decoded;
        CHECK_MSG(decode_welcome(payload.data(), payload.size(), decoded),
                  profile.name + " could not be read back off the wire");
        CHECK_MSG(decoded.system == profile.id,
                  profile.name + " arrived at the other end as a different console, which is the "
                                 "wrong screen sizes for everything after it");

        // A console this app runs itself needs two distinct names for where its
        // core is; one it does not run needs neither. Both halves matter: the
        // first catches a console left at another's answer, the second catches
        // a core path invented for something that has no core.
        const bool runs_here = profile.frame_origin == FrameOrigin::LibretroCore;
        const std::string settings_key = core_path_settings_key(profile.id);
        const std::string environment = core_path_environment_variable(profile.id);
        if (runs_here) {
            CHECK_MSG(!settings_key.empty() && !environment.empty(),
                      profile.name + " runs in a core here but has nowhere to say where it is");
            CHECK_MSG(settings_key != environment,
                      profile.name + " names its core path the same way in both places, which "
                                     "means one of them was left at another console's answer");
        } else {
            CHECK_MSG(settings_key.empty() && environment.empty(),
                      profile.name + " is not run by this app, so it should have no core path to "
                                     "name -- and a player told to set one would be misled");
            CHECK_MSG(profile.core_filenames.empty(),
                      profile.name + " is not run by this app but names cores to look for");
        }
    }
}

// The stamp a fragment carries is written twice: once when the datagram is
// built, and again as it actually leaves. Everything between those two moments
// is the sender's own pacing queue, and the far end reads the spread of
// transits as jitter on the link and buys buffer against it -- buffer that
// cannot cover a delay the sender already paid.
//
// Which means four bytes are written into an already-encoded datagram by
// offset. If that offset and the encoder ever disagree, the clock lands in the
// middle of a picture, so the offset is checked against the encoder here
// rather than trusted.
void test_a_fragment_can_be_restamped_as_it_leaves() {
    FrameFragmentHeader header;
    header.serial = 99;
    header.screen = ScreenId::Top;
    header.encoding = FrameEncoding::H264;
    header.width = 256;
    header.height = 192;
    header.fragment_index = 0;
    header.fragment_count = 1;
    header.keyframe = false;
    // All four bytes different from the restamp below, so "exactly four
    // bytes moved" means what it says.
    header.sent_at_ms = 0x01020304u;
    header.sent_index = 7;

    const std::vector<uint8_t> slice = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> datagram;
    encode_media_header(1234567890123456789ull, MessageType::Frame, datagram);
    encode_frame_fragment(header, slice.data(), slice.size(), datagram);
    const std::vector<uint8_t> before = datagram;

    CHECK(stamp_media_datagram_sent_at(datagram.data(), datagram.size(), 0xa1b2c3d4u));

    // The whole datagram survives except the four bytes of the stamp: same
    // token, same type, same serial, same payload.
    uint64_t token = 0;
    MessageType type = MessageType::Ping;
    const uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    CHECK(decode_media_header(datagram.data(), datagram.size(), token, type, payload,
                              payload_size));
    CHECK(token == 1234567890123456789ull);
    CHECK(type == MessageType::Frame);

    FrameFragmentHeader received;
    const uint8_t* chunk = nullptr;
    std::size_t chunk_size = 0;
    CHECK(decode_frame_fragment(payload, payload_size, received, chunk, chunk_size));
    CHECK(received.sent_at_ms == 0xa1b2c3d4u);
    CHECK(received.serial == header.serial);
    CHECK_EQ(static_cast<int>(received.sent_index), 7);
    CHECK(received.encoding == FrameEncoding::H264);
    CHECK(!received.keyframe);
    CHECK(std::vector<uint8_t>(chunk, chunk + chunk_size) == slice);

    // Exactly four bytes moved, and they are the ones the encoder put the
    // stamp in -- which is what pins the offset to the encoder.
    std::size_t changed = 0;
    for (std::size_t i = 0; i < datagram.size(); ++i) {
        if (datagram[i] != before[i]) ++changed;
    }
    CHECK_EQ(changed, static_cast<std::size_t>(4));

    // Anything that is not a frame fragment is left alone rather than having
    // a clock written through it.
    std::vector<uint8_t> audio;
    encode_media_header(1u, MessageType::Audio, audio);
    audio.resize(audio.size() + 64, 0x5a);
    const std::vector<uint8_t> audio_before = audio;
    CHECK(!stamp_media_datagram_sent_at(audio.data(), audio.size(), 77u));
    CHECK(audio == audio_before);

    // And so is a datagram too short to hold a fragment header.
    std::vector<uint8_t> stub;
    encode_media_header(1u, MessageType::Frame, stub);
    CHECK(!stamp_media_datagram_sent_at(stub.data(), stub.size(), 77u));
}

// A frame small enough to fit one datagram -- nearly every DS frame, at about
// a kilobyte each -- either arrives whole or vanishes without trace, so
// reassembly can never notice it missing. The run of sent_index values is the
// only thing that can: the host counts only frames it really sent, so a gap is
// loss on the link and never the host deciding to skip one.
void test_a_gap_in_sent_frames_is_loss() {
    CHECK_EQ(frames_missing_between(7, 8), 0u);   // the next one, nothing lost
    CHECK_EQ(frames_missing_between(7, 9), 1u);   // one never arrived
    CHECK_EQ(frames_missing_between(7, 12), 4u);  // a burst of them

    // Another fragment of the frame already in hand, and one that arrived
    // twice. Neither is loss.
    CHECK_EQ(frames_missing_between(7, 7), 0u);
    // One that overtook its neighbours. The gap it left was already counted
    // when the newer frame went past, and counting it again here would have
    // the far end asking for keyframes it does not need.
    CHECK_EQ(frames_missing_between(9, 8), 0u);

    // The counter is sixteen bits and a session outlasts it -- at sixty
    // frames a second it comes round every eighteen minutes. Passing zero has
    // to read as one step forward, not as the whole range going missing.
    CHECK_EQ(frames_missing_between(65535, 0), 0u);
    // 65535 and 0 both went missing on the way from 65534 to 1.
    CHECK_EQ(frames_missing_between(65534, 1), 2u);
    CHECK_EQ(frames_missing_between(0, 65535), 0u);
}

// The host's Controller Order screen, for a companion to draw: every field
// it is drawn from comes back as it went, and a cut-short one is refused.
void test_controllers_view_round_trip() {
    ControllersViewMessage sent;
    sent.open = true;
    sent.focus = 3;
    sent.holding = true;
    sent.gamepad_here = false;
    sent.this_device = "SteamOS";
    sent.other_device = "G Cloud";
    sent.pads.push_back(ControllersViewPad{true, 12, 0, "Steam Deck", 1});
    sent.pads.push_back(ControllersViewPad{false, 3, 1, "", -1});
    sent.order_chosen = true;
    sent.order_gamepad_here = false;
    sent.order_gamepad = "companion#3";
    sent.order_players = {"this#12"};
    sent.order_unused = {};

    std::vector<uint8_t> buffer;
    encode_controllers_view(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::ControllersView);

    ControllersViewMessage received;
    CHECK(decode_controllers_view(payload.data(), payload.size(), received));
    CHECK(received.open);
    CHECK_EQ(received.focus, 3);
    CHECK(received.holding);
    CHECK(!received.gamepad_here);
    CHECK_STR_EQ(received.this_device, "SteamOS");
    CHECK_STR_EQ(received.other_device, "G Cloud");
    CHECK_EQ(received.pads.size(), static_cast<std::size_t>(2));
    CHECK(received.pads[0].on_this_device);
    CHECK_EQ(received.pads[0].id, 12u);
    CHECK_STR_EQ(received.pads[0].name, "Steam Deck");
    CHECK_EQ(received.pads[0].slot, 1);
    CHECK(!received.pads[1].on_this_device);
    CHECK_EQ(received.pads[1].position, 1);
    CHECK_EQ(received.pads[1].slot, -1);
    CHECK(received.order_chosen);
    CHECK(!received.order_gamepad_here);
    CHECK_STR_EQ(received.order_gamepad, "companion#3");
    CHECK(received.order_players == sent.order_players);
    CHECK(received.order_unused.empty());

    CHECK(!decode_controllers_view(payload.data(), payload.size() / 2, received));
}

void test_graphic_packs_view_round_trip() {
    GraphicPacksViewMessage sent;
    sent.open = true;
    sent.waiting = false;
    sent.restart_pending = true;
    sent.focus = 2;
    sent.in_pack = true;
    sent.item = 1;
    sent.choice_focus = 3;
    GraphicPacksViewPack pack;
    pack.id = "graphicPacks/Game_Resolution/rules.txt";
    pack.path = "Some Game/Graphics";
    pack.name = "Graphics";
    pack.description = "Renders the game at a higher resolution.";
    pack.enabled = true;
    pack.restart_needed = true;
    pack.choices.push_back(GraphicPacksViewChoice{"TV Resolution", {"720p", "1080p"}, 1});
    pack.choices.push_back(GraphicPacksViewChoice{"", {"Low", "High"}, -1});
    sent.packs.push_back(pack);
    sent.packs.push_back(GraphicPacksViewPack{"fps", "Some Game/Mods/FPS++", "FPS++", "", false, false, {}});

    std::vector<uint8_t> buffer;
    encode_graphic_packs_view(sent, buffer);
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::GraphicPacksView);

    GraphicPacksViewMessage received;
    CHECK(decode_graphic_packs_view(payload.data(), payload.size(), received));
    CHECK(received.open && !received.waiting && received.restart_pending && received.in_pack);
    CHECK_EQ(received.focus, 2);
    CHECK_EQ(received.item, 1);
    CHECK_EQ(received.choice_focus, 3);
    // From a host before the popup: none up.
    GraphicPacksViewMessage older;
    CHECK(decode_graphic_packs_view(payload.data(), payload.size() - 2, older));
    CHECK_EQ(older.choice_focus, 0);
    CHECK_EQ(received.packs.size(), static_cast<std::size_t>(2));
    if (received.packs.size() == 2) {
        const GraphicPacksViewPack& first = received.packs[0];
        CHECK_STR_EQ(first.id, pack.id);
        CHECK_STR_EQ(first.path, "Some Game/Graphics");
        CHECK_STR_EQ(first.description, pack.description);
        CHECK(first.enabled && first.restart_needed);
        CHECK_EQ(first.choices.size(), static_cast<std::size_t>(2));
        if (first.choices.size() == 2) {
            CHECK_STR_EQ(first.choices[0].category, "TV Resolution");
            CHECK(first.choices[0].presets == pack.choices[0].presets);
            CHECK_EQ(first.choices[0].active, 1);
            CHECK_EQ(first.choices[1].active, -1);
        }
        CHECK(!received.packs[1].enabled && received.packs[1].choices.empty());
    }

    CHECK(!decode_graphic_packs_view(payload.data(), payload.size() / 2, received));

    // Closed is a message too: how the other device learns to put it away.
    encode_graphic_packs_view(GraphicPacksViewMessage(), buffer);
    buffer.clear();
    encode_graphic_packs_view(GraphicPacksViewMessage(), buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_graphic_packs_view(payload.data(), payload.size(), received));
    CHECK(!received.open && received.packs.empty());
}

// A game's own dialog, to the companion, and its answer back.
void test_game_dialog_round_trip() {
    GameDialogMessage sent;
    sent.kind = 2;
    sent.serial = 9;
    sent.title = "Name your horse";
    sent.text = "Epona";
    sent.max_length = 10;
    sent.typed_here = true;
    std::vector<uint8_t> buffer;
    encode_game_dialog(sent, buffer);
    MessageType type{};
    std::vector<uint8_t> payload;
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::GameDialog);
    GameDialogMessage got;
    CHECK(decode_game_dialog(payload.data(), payload.size(), got));
    CHECK(got.kind == 2 && got.serial == 9u && got.title == sent.title && got.text == "Epona");
    CHECK(got.max_length == 10u && got.typed_here && got.buttons.empty());
    sent.kind = 1;
    sent.buttons = {"Retry", "Cancel"};
    sent.typed_here = false;
    buffer.clear();
    encode_game_dialog(sent, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_game_dialog(payload.data(), payload.size(), got));
    CHECK(got.buttons == sent.buttons && !got.typed_here);
    CHECK(!decode_game_dialog(payload.data(), 3, got));

    GameDialogAnswerMessage answer;
    answer.serial = 9;
    answer.final = false;
    answer.text = "Roa";
    buffer.clear();
    encode_game_dialog_answer(answer, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(type == MessageType::GameDialogAnswer);
    GameDialogAnswerMessage back;
    CHECK(decode_game_dialog_answer(payload.data(), payload.size(), back));
    CHECK(back.serial == 9u && !back.final && back.text == "Roa");
    answer.final = true;
    answer.button = 1;
    buffer.clear();
    encode_game_dialog_answer(answer, buffer);
    CHECK(one_message(buffer, type, payload));
    CHECK(decode_game_dialog_answer(payload.data(), payload.size(), back));
    CHECK(back.final && back.button == 1);
}

int main() {
    test_a_repair_request_round_trips();
    test_lost_pieces_come_back_from_parity();
    test_a_bye_can_say_quit_too();
    test_hello_round_trip();
    test_welcome_and_session_round_trip();
    test_request_round_trip();
    test_save_slots_round_trip();
    test_input_round_trip();
    test_rumble_round_trip();
    test_menu_sound_round_trip();
    test_a_companions_pads_ride_the_input_as_a_tail();
    test_an_input_carries_its_number_and_the_host_echoes_it();
    test_rumble_can_name_one_pad();
    test_stream_framing();
    test_absurd_length_is_fatal();
    test_truncation_is_rejected_everywhere();
    test_random_bytes_are_rejected();
    test_pixels_round_trip();
    test_a_frame_carries_its_encoding();
    test_only_changed_tiles_are_sent();
    test_a_tile_put_back_before_its_change_was_confirmed_still_goes_out();
    test_a_resized_screen_starts_over();
    test_corrupt_tiled_payload_is_rejected();
    test_corrupt_pixel_payload_is_rejected();
    test_every_system_round_trips();
    test_media_header_round_trip();
    test_media_header_rejects_truncation();
    test_frame_fragment_round_trip();
    test_frame_fragment_rejects_truncation();
    test_keyframe_request_round_trip();
    test_frame_ack_carries_how_late_the_frame_arrived();
    test_a_gap_in_sent_frames_is_loss();
    test_a_fragment_can_be_restamped_as_it_leaves();
    test_controllers_view_round_trip();
    test_graphic_packs_view_round_trip();
    test_game_dialog_round_trip();
    return test::finish("protocol");
}
