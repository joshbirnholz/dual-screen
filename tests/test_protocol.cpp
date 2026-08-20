// The wire protocol is the one place in this project that reads bytes it did
// not write, so every decoder is exercised against truncated and malformed
// input as well as valid input.

#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <vector>

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
    CHECK_EQ(static_cast<int>(received_state.render_scale), 3);
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
    sent.menu_open = true;

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
    CHECK_EQ(received.menu_open, true);

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
    CHECK_EQ(received_quiet.menu_open, false);

    // An audio target outside the three that exist is refused rather than
    // becoming one of them by accident.
    std::vector<uint8_t> corrupt(payload);
    corrupt[corrupt.size() - 6] = 9;  // the audio target byte
    RequestMessage rejected;
    CHECK(!decode_request(corrupt.data(), corrupt.size(), rejected));
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
    test_corrupt_pixel_payload_is_rejected();
    return test::finish("protocol");
}
