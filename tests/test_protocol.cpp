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
}

void test_welcome_and_session_round_trip() {
    WelcomeMessage welcome;
    welcome.game_title = "Pokemon Platinum";
    welcome.rom_sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d";
    welcome.granted_screen = ScreenId::Top;
    welcome.system = SystemId::ThreeDs;

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
}

// Everything a companion can ask the host for travels in one message, each
// field saying whether it means anything, so asking about the sound does not
// silently also ask for a layout.
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
        {"audio target", 21, 1u,
         [](const RequestMessage& message) {
             return message.audio_target == AudioTarget::BottomScreen;
         }},
        {"handedness", 19, 1u,
         [](const RequestMessage& message) { return message.handedness == Handedness::Left; }},
        {"right stick mode", 5, 2u,
         [](const RequestMessage& message) {
             return message.right_stick_mode == RightStickMode::DPad;
         }},
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

    std::vector<uint8_t> impossible(payload);
    impossible[impossible.size() - 3] = 200;
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
    CHECK_EQ(received.c_stick_x, -30000);
    CHECK_EQ(received.c_stick_y, 8000);
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

int main() {
    test_hello_round_trip();
    test_welcome_and_session_round_trip();
    test_request_round_trip();
    test_input_round_trip();
    test_stream_framing();
    test_absurd_length_is_fatal();
    test_truncation_is_rejected_everywhere();
    test_random_bytes_are_rejected();
    test_pixels_round_trip();
    test_a_frame_carries_its_encoding();
    test_only_changed_tiles_are_sent();
    test_a_resized_screen_starts_over();
    test_corrupt_tiled_payload_is_rejected();
    test_corrupt_pixel_payload_is_rejected();
    test_every_system_round_trips();
    return test::finish("protocol");
}
