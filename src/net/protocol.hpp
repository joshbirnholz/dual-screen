#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
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
constexpr uint16_t kProtocolVersion = 22;

// Ports are fixed rather than negotiated: this is a LAN between two of the
// player's own devices, and a fixed port is one less thing to configure.
constexpr uint16_t kDiscoveryPort = 47820;
constexpr uint16_t kSessionPort = 47821;
// The UDP media channel -- Frame, Audio, Input, FrameAck. Separate from the
// TCP session above rather than sharing its port: connection setup, session
// state, menu/settings requests, rumble and quit all stay on that reliable
// connection unchanged, since none of it is latency-critical and all of it
// already works. This port carries only the four message types that are.
constexpr uint16_t kMediaPort = 47822;

constexpr uint32_t kMaxMessageBytes = 4u * 1024u * 1024u;

enum class MessageType : uint16_t {
    // companion -> host
    Hello = 1,
    Input = 2,
    Bye = 3,
    Request = 4,
    // Which frame the companion has actually decoded. The only thing that
    // tells the host how far behind the picture really is: a sender can see
    // its own queues, but not the ones on the far side of the wire.
    FrameAck = 5,
    // host -> companion
    Welcome = 10,
    Reject = 11,
    SessionState = 12,
    Frame = 13,
    Audio = 14,
    // The GamePad's motor turning on or off. Sent only on Wii U, and only
    // when it changes -- see HostSession::publish_rumble().
    Rumble = 15,
    // either way
    Ping = 20,
};

// A press a companion sends into the host's menu. Numbered here rather than
// reusing UiAction's values, so reordering that enum cannot silently change
// what goes over the wire.
enum class MenuAction : uint8_t {
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
    Confirm = 5,
    Cancel = 6,
    Quit = 7,
    Reset = 8,
    Disconnect = 9,
    // Open the host's settings. It is the host's own screen rather than a
    // shared one -- see SessionStateMessage::settings_open -- but either pad
    // can ask for it and either pad can work it once it is up.
    Settings = 10,
};

// How a screen's pixels are encoded in a Frame message.
enum class FrameEncoding : uint8_t {
    // 16-bit 5-6-5, deflated. DS output is flat-shaded enough that this gets a
    // 256x192 screen down to a few kilobytes, which is what makes 60fps over
    // wifi realistic without pulling in a video encoder.
    Rgb565Deflate = 1,
    // The same, but only the 16x16 tiles that differ from the last frame this
    // recipient was sent, with a bitmap saying which. A whole frame costs the
    // same as Rgb565Deflate does -- the bytes are the same bytes in a
    // different order -- so this is never worse, and a screen where only a
    // health bar or a blinking marker moved costs a fraction of one.
    Rgb565DeflateTiles = 2,
    // H.264, one packet per frame, no B-frames. For a screen the two above
    // cannot carry: a Wii U GamePad measures at 43 Mbit/s through the tiles and
    // 2.5 through this. Only sent to a companion that said it can decode it --
    // see HelloMessage::accepts_video -- since a build without FFmpeg cannot.
    H264 = 3,
};

// The tile edge used by Rgb565DeflateTiles, as a power of two. Sixteen keeps a
// moving sprite from dirtying much more than it covers, and the bitmap for a
// DS screen is 24 bytes, which is nothing against the frame it saves.
constexpr int kFrameTileShift = 4;

struct HelloMessage {
    uint16_t protocol_version = kProtocolVersion;
    std::string device_name;
    // Which screen this device wants to show when the split layout is active.
    ScreenId wanted_screen = ScreenId::Bottom;
    // Whether this device can decode FrameEncoding::H264. A build without a
    // video decoder says no and is sent tiles, which is what every DS and 3DS
    // session uses anyway. Asked rather than assumed from the version: the same
    // protocol version is spoken by builds with and without one.
    bool accepts_video = false;
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
    // Generated fresh by the host for this session and echoed on every UDP
    // media datagram either side sends (see kMediaPort). UDP has no
    // connection state of its own to say which session a stray datagram
    // belongs to -- a late packet from a previous session, or anything else
    // that happens to land on this port -- so every one carries this and is
    // ignored if it does not match.
    uint64_t session_token = 0;
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
    // Counts up by one per packet actually sent on this stream (see
    // HostSession::publish_audio) -- never per audio frame or sample, so a
    // packet split off a bigger batch (kMaxAudioFramesPerPacket) still only
    // costs one. The far end has no other way to tell a genuinely missing
    // packet (audible as a click -- samples that should have played never
    // did) from ordinary jitter in when packets happen to arrive, since UDP
    // promises neither order nor delivery. See CompanionSession's own use of
    // this for what a gap does about it.
    uint32_t sequence = 0;
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
    // The companion is asking for the menu to be open or closed. It does not
    // open one of its own: there is one menu, the host holds it, and both
    // devices draw what the host says it looks like.
    bool menu_open = false;
    // A press the player made on this device while that menu was up. The host
    // applies it to the one menu and broadcasts what happened, so the two
    // screens cannot show different things.
    bool wants_menu_action = false;
    MenuAction menu_action = MenuAction::Confirm;
    // Where a finger landed, resolved to an entry here rather than there: the
    // two devices' menus are laid out for different-sized screens, so only
    // this one knows what its own tap was on.
    bool wants_menu_focus = false;
    uint8_t menu_focus = 0;
    // Restart the running game. One-shot rather than a value to agree on: it is
    // not echoed back in SessionState, since there is nothing to stay in sync
    // about once the moment has passed.
    bool wants_reset = false;
    // Shut the host down entirely, the same as choosing Quit there. One-shot,
    // like the reset above; the companion leaves the session on its own right
    // after asking, rather than waiting to be told to.
    bool wants_quit = false;
    // Whether a layout shows the gap between its screens, for whichever
    // layouts offer the choice -- see LayoutComposition::gap_toggleable.
    bool wants_screen_gap = false;
    bool screen_gap = true;
    // What the right stick is read as. Both devices' pads drive the game, so
    // both read their own stick the same way -- which means this is the host's
    // to decide like everything else here.
    bool wants_right_stick_mode = false;
    RightStickMode right_stick_mode = RightStickMode::CStick;
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
    bool screen_gap = true;
    RightStickMode right_stick_mode = RightStickMode::CStick;

    // The one menu, as the host currently has it. A companion draws from these
    // rather than running a menu of its own, which is what lets the player
    // watch either screen while working the same menu from either pad.
    bool menu_open = false;
    uint8_t menu_focus = 0;
    // 0 none, 1 reset, 2 quit -- see LayoutPicker::MenuConfirm.
    uint8_t menu_confirm = 0;
    // The host's settings screen, which unlike the menu is not mirrored: it is
    // about the machine running the game, and half of what it shows -- where
    // that machine's BIOS files and emulators are -- is not the companion's
    // business at all. The companion is told only that it is up, so it can say
    // where to look and keep sending presses.
    bool settings_open = false;
    // What decides which buttons the menu has. Sent so the companion builds
    // the identical row: a menu that agreed about everything except how many
    // entries it had would put the two devices' cursors in different places.
    uint8_t max_render_scale = 1;
    bool cursor_choosable = false;
    bool right_stick_choosable = false;
    // Whether this device's pointer is shown. Per-device in what it does, but
    // the menu that offers it is one menu, so the label it carries has to come
    // from the same place as everything else on it.
    bool cursor_shown = true;
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

// The Wii U GamePad's own motor, standing in for whichever controller the
// companion actually holds -- see ExternalControl::Rumble for where this
// starts. Just on/off: what a real Wii U GamePad's motor does with a finer
// pattern than that is not something a phone's or a Joy-Con's single motor
// could reproduce over a network link anyway.
struct RumbleMessage {
    bool active = false;
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
    // The right stick, standing in for a Circle Pad Pro or New 3DS's C-Stick.
    int16_t c_stick_x = 0;
    int16_t c_stick_y = 0;
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
// As encode_audio_payload() is to encode_audio() above: what decode_input()
// reads back, without the TCP connection's length/type framing -- what the
// UDP media channel appends after encode_media_header().
void encode_input_payload(const InputMessage& message, std::vector<uint8_t>& out);
void encode_audio(const AudioMessage& message, std::vector<uint8_t>& out);
// The same fields encode_audio() sends, but without the length/type framing
// that wraps them for the TCP connection -- exactly what decode_audio()
// reads back either way, since it was already payload-only. This is what the
// UDP media channel appends after encode_media_header(); encode_audio()
// above is defined in terms of it.
void encode_audio_payload(const AudioMessage& message, std::vector<uint8_t>& out);
void encode_rumble(const RumbleMessage& message, std::vector<uint8_t>& out);
void encode_request(const RequestMessage& message, std::vector<uint8_t>& out);
void encode_bye(std::vector<uint8_t>& out);
void encode_frame_ack(uint64_t serial, std::vector<uint8_t>& out);
// As encode_audio_payload() is to encode_audio() above.
void encode_frame_ack_payload(uint64_t serial, std::vector<uint8_t>& out);
bool decode_frame_ack(const uint8_t* data, std::size_t size, uint64_t& out_serial);
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
bool decode_rumble(const uint8_t* data, std::size_t size, RumbleMessage& out);
bool decode_request(const uint8_t* data, std::size_t size, RequestMessage& out);

// Pulls one complete message off the front of `buffer`, erasing it. Returns
// false when a whole message is not there yet. Sets `fatal` if the stream is
// unusable (an absurd length), which the caller must treat as a disconnect.
bool take_message(std::vector<uint8_t>& buffer,
                  MessageType& out_type,
                  std::vector<uint8_t>& out_payload,
                  bool& fatal);

// --- the UDP media channel ---------------------------------------------------
//
// Frame, Audio, Input and FrameAck ride here instead of the TCP connection
// above -- see kMediaPort. A datagram already delimits itself, so there is no
// length prefix; each one starts with a small header saying which session it
// belongs to and what it carries.

// [u64 session_token][u8 type][payload ...]. Appends to `out`, which the
// caller then fills with the encoded payload (encode_frame_fragment(),
// encode_input(), encode_audio(), or encode_frame_ack()) before handing the
// whole thing to UdpSocket::send_to().
void encode_media_header(uint64_t session_token, MessageType type, std::vector<uint8_t>& out);
// Reads the header off the front of one received datagram. `out_payload` and
// `out_payload_size` point into `data` itself -- no copy -- for whichever
// decode_* the caller picks based on `out_type`. False if the datagram is too
// short to even hold a header.
bool decode_media_header(const uint8_t* data, std::size_t size, uint64_t& out_token,
                         MessageType& out_type, const uint8_t*& out_payload,
                         std::size_t& out_payload_size);

// How large one UDP media datagram is allowed to be, media header and
// fragment header included. Comfortably under a LAN's usual 1500-byte MTU so
// IP itself never has to fragment -- losing one IP fragment loses the whole
// datagram either way, so fragmenting up here instead means a lost chunk
// costs one chunk, not everything already sent alongside it.
constexpr std::size_t kMaxMediaDatagramBytes = 1200;

// A Frame message's own payload can run well past one datagram (an H.264
// frame especially), so it travels as a short run of these instead of one
// encode_frame() -- everything encode_frame() would send except the payload
// itself, which arrives in pieces.
struct FrameFragmentHeader {
    uint64_t serial = 0;
    ScreenId screen = ScreenId::Top;
    FrameEncoding encoding = FrameEncoding::Rgb565Deflate;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fragment_index = 0;
    uint16_t fragment_count = 1;
};

// Appends one fragment (header plus its slice of the payload) to `out`. The
// caller slices a frame's payload into pieces and calls this once per piece
// -- see HostSession::publish_screens() for the loop that does it.
void encode_frame_fragment(const FrameFragmentHeader& header, const uint8_t* payload,
                           std::size_t payload_size, std::vector<uint8_t>& out);
// The inverse, again pointing `out_payload` into `data` rather than copying.
bool decode_frame_fragment(const uint8_t* data, std::size_t size, FrameFragmentHeader& out_header,
                           const uint8_t*& out_payload, std::size_t& out_payload_size);

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

// -> deflated RGB565, but only the tiles that differ from `previous`, which is
// the last frame this recipient was actually sent and which this updates to
// match. Empty (or the wrong size for these dimensions) sends every tile,
// which is how a recipient that has just connected, or one whose screen
// changed size, is given something whole to start from.
//
// It is the *sent* frame rather than the produced one on purpose: a frame
// skipped because the link was behind must still be carried into the next one
// that goes out, or the tiles it held would never arrive at all.
bool encode_screen_tiles(const uint32_t* pixels,
                         int width,
                         int height,
                         int stride_pixels,
                         std::vector<uint8_t>& previous,
                         std::vector<uint8_t>& out,
                         PixelChannelOrder channel_order = PixelChannelOrder::XrgbBytes);

// Inflates one whole-frame payload to RGB565, the form it travels in.
bool decode_screen_rgb565(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint8_t>& out);

// Applies one tiled payload onto `screen`, which holds the RGB565 frame this
// recipient last had. Resized and zeroed if it is not already that frame's
// size, so a first payload that is not a whole frame cannot read uninitialised
// pixels back out.
bool decode_screen_tiles(const std::vector<uint8_t>& payload,
                         int width,
                         int height,
                         std::vector<uint8_t>& screen);

// RGB565 -> XRGB8888, for a renderer that wants it that way. One that can
// upload 5-6-5 directly should: this is a whole extra pass over the frame and
// twice the bytes to hand the GPU.
void expand_rgb565(const std::vector<uint8_t>& raw, std::vector<uint32_t>& out);

// The inverse of encode_screen_pixels, producing XRGB8888 in one step.
bool decode_screen_pixels(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint32_t>& out);

}  // namespace ds::net
