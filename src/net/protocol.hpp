#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/ds_input.hpp"
#include "layout/layout.hpp"

namespace ds::net {

// Everything on the wire is little-endian, which every target this runs on is
// natively. Messages are length-prefixed so a reader never has to guess where
// one ends.
//
//   [uint32 payload_length][uint16 type][payload ...]
//
// The version is checked during the handshake and a mismatch is refused with a
// readable reason rather than left to produce garbage. Bump it whenever the
// meaning of an existing field changes.
constexpr uint32_t kProtocolMagic = 0x48535344u;  // "DSSH" little-endian
constexpr uint16_t kProtocolVersion = 1;

// Ports are fixed rather than negotiated: this is a LAN between two of the
// player's own devices, and a fixed port is one less thing to configure.
constexpr uint16_t kDiscoveryPort = 47820;
constexpr uint16_t kSessionPort = 47821;

constexpr uint32_t kMaxMessageBytes = 4u * 1024u * 1024u;

enum class MessageType : uint16_t {
    // companion -> host
    Hello = 1,
    Input = 2,
    Bye = 3,
    // host -> companion
    Welcome = 10,
    Reject = 11,
    SessionState = 12,
    Frame = 13,
    // either way
    Ping = 20,
};

// How a screen's pixels are encoded in a Frame message.
enum class FrameEncoding : uint8_t {
    // 16-bit 5-6-5, deflated. DS output is flat-shaded enough that this gets a
    // 256x192 screen down to a few kilobytes, which is what makes 60fps over
    // wifi realistic without pulling in a video encoder.
    Rgb565Deflate = 1,
};

struct HelloMessage {
    uint16_t protocol_version = kProtocolVersion;
    std::string device_name;
    // Which screen this device wants to show when the split layout is active.
    ScreenId wanted_screen = ScreenId::Bottom;
};

struct WelcomeMessage {
    uint16_t protocol_version = kProtocolVersion;
    std::string game_title;
    std::string rom_sha1;
    ScreenId granted_screen = ScreenId::Bottom;
    uint16_t screen_width = kDsScreenWidth;
    uint16_t screen_height = kDsScreenHeight;
};

struct RejectMessage {
    std::string reason;
};

// Sent whenever the host's layout changes, so the companion knows whether it is
// showing one screen or both.
struct SessionStateMessage {
    std::string layout_id;
    ScreenId companion_screen = ScreenId::Bottom;
    bool paused = false;
};

struct FrameMessage {
    ScreenId screen = ScreenId::Top;
    uint64_t serial = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    FrameEncoding encoding = FrameEncoding::Rgb565Deflate;
    std::vector<uint8_t> payload;
};

// Broadcast over UDP by a host that has a game running, so companions can find
// it without anyone typing an IP address. Sent as a bare datagram rather than a
// framed stream message: a datagram already delimits itself.
struct BeaconMessage {
    uint16_t protocol_version = kProtocolVersion;
    std::string device_name;
    std::string game_title;
    std::string rom_sha1;
    uint16_t session_port = kSessionPort;
    // Which screens are still unclaimed, so the picker can grey out the rest.
    bool top_available = true;
    bool bottom_available = true;
};

struct InputMessage {
    uint32_t buttons = 0;
    bool touching = false;
    int16_t touch_x = 0;
    int16_t touch_y = 0;
};

// --- encoding ---------------------------------------------------------------
//
// Each encode_* appends a complete framed message to `out`. Each decode_* reads
// a payload that has already had its header stripped, and returns false if the
// bytes do not describe a valid message rather than trusting the sender.

void encode_hello(const HelloMessage& message, std::vector<uint8_t>& out);
void encode_welcome(const WelcomeMessage& message, std::vector<uint8_t>& out);
void encode_reject(const RejectMessage& message, std::vector<uint8_t>& out);
void encode_session_state(const SessionStateMessage& message, std::vector<uint8_t>& out);
void encode_frame(const FrameMessage& message, std::vector<uint8_t>& out);
void encode_input(const InputMessage& message, std::vector<uint8_t>& out);
void encode_bye(std::vector<uint8_t>& out);
// The beacon is not framed -- it replaces the buffer with one datagram.
void encode_beacon(const BeaconMessage& message, std::vector<uint8_t>& out);
bool decode_beacon(const uint8_t* data, std::size_t size, BeaconMessage& out);
void encode_ping(std::vector<uint8_t>& out);

bool decode_hello(const uint8_t* data, std::size_t size, HelloMessage& out);
bool decode_welcome(const uint8_t* data, std::size_t size, WelcomeMessage& out);
bool decode_reject(const uint8_t* data, std::size_t size, RejectMessage& out);
bool decode_session_state(const uint8_t* data, std::size_t size, SessionStateMessage& out);
bool decode_frame(const uint8_t* data, std::size_t size, FrameMessage& out);
bool decode_input(const uint8_t* data, std::size_t size, InputMessage& out);

// Pulls one complete message off the front of `buffer`, erasing it. Returns
// false when a whole message is not there yet. Sets `fatal` if the stream is
// unusable (an absurd length), which the caller must treat as a disconnect.
bool take_message(std::vector<uint8_t>& buffer,
                  MessageType& out_type,
                  std::vector<uint8_t>& out_payload,
                  bool& fatal);

// --- pixel conversion -------------------------------------------------------

// XRGB8888 -> deflated RGB565. `stride_pixels` is the source's row stride.
bool encode_screen_pixels(const uint32_t* pixels,
                          int width,
                          int height,
                          int stride_pixels,
                          std::vector<uint8_t>& out);

// The inverse, producing XRGB8888 the renderer can upload unchanged.
bool decode_screen_pixels(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint32_t>& out);

}  // namespace ds::net
