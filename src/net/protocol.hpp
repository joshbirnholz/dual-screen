#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/ds_input.hpp"
#include "core/system.hpp"
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
constexpr uint16_t kProtocolVersion = 7;

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
    Request = 4,
    // host -> companion
    Welcome = 10,
    Reject = 11,
    SessionState = 12,
    Frame = 13,
    Audio = 14,
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
    // Which console is being played. The companion looks the screen sizes and
    // the console's own arrangement up from this rather than being sent them:
    // both devices run the same build, so a name is enough and there is no way
    // for the two to end up with different numbers.
    SystemId system = SystemId::Ds;
    // So the companion can open its own audio output at the rate the host is
    // actually producing, rather than resampling something it guessed.
    uint32_t audio_sample_rate = 0;
};

// Which device an audio stream belongs to.
//
// DS and 3DS have one soundtrack, so both devices play the same thing and the
// stream is Shared. Wii U genuinely has separate audio per device -- the
// GamePad speaker is not a copy of the TV's -- so the field exists now and the
// per-device values are reserved rather than being bolted on later.
enum class AudioStreamId : uint8_t {
    Shared = 0,
    TopDevice = 1,
    BottomDevice = 2,
};

struct AudioMessage {
    AudioStreamId stream = AudioStreamId::Shared;
    uint32_t sample_rate = 0;
    // Interleaved stereo 16-bit, sent uncompressed: it is about 1 Mbit/s at DS
    // rates, which is comparable to the video and costs no latency to produce.
    std::vector<int16_t> samples;
};

// Sent by a companion to ask the host to change something, and to tell it when
// its own menu is open. The host owns every one of these choices, so they are
// requests rather than instructions: it applies them and broadcasts the result
// as SessionState, which is what keeps the two devices from ever disagreeing.
//
// Each field carries its own "I mean this one" flag, so a companion asking for
// a layout does not have to have an opinion about the sound at the same time.
struct RequestMessage {
    std::string layout_id;  // empty asks for no layout change
    bool wants_filtering = false;
    bool bilinear_filtering = false;
    bool wants_audio_target = false;
    AudioTarget audio_target = AudioTarget::Both;
    bool wants_handedness = false;
    Handedness handedness = Handedness::Right;
    // How many pixels the core renders per console pixel. The companion is not
    // running a core, but the menu is one menu wherever it is opened from, so
    // it asks the host the same way it asks for anything else.
    bool wants_render_scale = false;
    uint8_t render_scale = 1;
    // The companion's own window, in device pixels. Not a shared setting --
    // the host never echoes this back in SessionState, it just remembers it
    // privately and uses it to decide how big a picture that screen actually
    // needs to be. Sent once on connecting and again whenever the window
    // resizes; zero means not known yet, which falls back to the console's
    // own resolution.
    bool wants_display_size = false;
    uint16_t display_width = 0;
    uint16_t display_height = 0;
    // The companion has a menu up. The host pauses while either device does, so
    // both are looking at the same still frame.
    bool menu_open = false;
};

struct RejectMessage {
    std::string reason;
};

// The host's view of the session, sent whenever any of it changes. Everything
// both devices have to agree on lives here and nowhere else: the companion
// never decides any of it locally, which is why the two cannot drift apart.
struct SessionStateMessage {
    std::string layout_id;
    ScreenId companion_screen = ScreenId::Bottom;
    bool paused = false;
    bool bilinear_filtering = false;
    AudioTarget audio_target = AudioTarget::Both;
    Handedness handedness = Handedness::Right;
    uint8_t render_scale = 1;
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
    // The circle pad, which a 3DS game is played with. A companion device that
    // did not send these would be able to press buttons but not walk.
    int16_t circle_x = 0;
    int16_t circle_y = 0;
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
void encode_audio(const AudioMessage& message, std::vector<uint8_t>& out);
void encode_request(const RequestMessage& message, std::vector<uint8_t>& out);
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
bool decode_audio(const uint8_t* data, std::size_t size, AudioMessage& out);
bool decode_request(const uint8_t* data, std::size_t size, RequestMessage& out);

// Pulls one complete message off the front of `buffer`, erasing it. Returns
// false when a whole message is not there yet. Sets `fatal` if the stream is
// unusable (an absurd length), which the caller must treat as a disconnect.
bool take_message(std::vector<uint8_t>& buffer,
                  MessageType& out_type,
                  std::vector<uint8_t>& out_payload,
                  bool& fatal);

// --- pixel conversion -------------------------------------------------------

// Which byte in each source pixel holds which channel. A CPU-rendered core's
// framebuffer (and everything else in this app) is XRGB8888. A GPU-rendered
// core's screen, read back with glReadPixels(GL_RGBA), comes off the card as
// four bytes in R,G,B,A order and used to be swizzled into XRGB8888 to match
// -- a second full-frame CPU pass on top of the one below, for a console
// whose whole point is running above native resolution. Reading the source
// order directly here instead means that pass is gone.
enum class PixelChannelOrder { XrgbBytes, Rgba };

// -> deflated RGB565. `stride_pixels` is the source's row stride.
bool encode_screen_pixels(const uint32_t* pixels,
                          int width,
                          int height,
                          int stride_pixels,
                          std::vector<uint8_t>& out,
                          PixelChannelOrder channel_order = PixelChannelOrder::XrgbBytes);

// The inverse, producing XRGB8888 the renderer can upload unchanged.
bool decode_screen_pixels(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint32_t>& out);

}  // namespace ds::net
