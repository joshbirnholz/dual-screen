#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/button_mapping.hpp"
#include "input/controller_mapping.hpp"
#include "input/ds_input.hpp"
#include "input/pad_routing.hpp"
#include "input/wii_u_mapping.hpp"
#include "core/save_points.hpp"
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
constexpr uint16_t kProtocolVersion = 27;

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
    // "I lost a frame for this screen and cannot decode past it -- send a
    // picture that stands on its own." Rides the media channel like
    // FrameAck, and for the same reason: it is only worth anything if it
    // arrives now. See CompanionSession's reassembly and, on the other end,
    // HostSession::poll_media(). Its existence is what lets the encoder's
    // periodic keyframe interval be long: without a way to ask, the only
    // recovery from a lost frame is waiting for the next scheduled one, so
    // that interval had to stay short enough to be the recovery time -- and
    // every one of those keyframes is a burst tens of times the size of an
    // ordinary frame, which is exactly what the pacing queue then has to
    // spread out, and exactly the periodic latency spike that follows.
    KeyframeRequest = 6,
    // "These pieces of this frame never got here -- send them again." Rides
    // the media channel. The step before asking for a keyframe: a video
    // frame missing more pieces than its recovery pieces can put back is set
    // aside, the pieces are asked for, and the frame goes on to be shown when
    // they land, a round trip later rather than a keyframe later. A host from
    // before this ignores it, and the companion falls back to asking for a
    // keyframe. See CompanionSession's repair and HostSession::resend_pieces().
    RepairRequest = 7,
    // The answer to the game's dialog shown here -- see
    // GameDialogAnswerMessage. A host from before this ignores it.
    GameDialogAnswer = 8,
    // What this device's own updater found, asked by the host's
    // UpdateRequest -- see UpdateAnswerMessage. A host from before this
    // ignores it.
    UpdateAnswer = 9,
    // host -> companion
    Welcome = 10,
    Reject = 11,
    SessionState = 12,
    Frame = 13,
    Audio = 14,
    // The GamePad's motor turning on or off. Sent only on Wii U, and only
    // when it changes -- see HostSession::publish_rumble().
    Rumble = 15,
    // What is in each save point slot, for the quick menu's Create and Load
    // Save Point to show on a companion: the saves are on the host's disk.
    // Sent when they change. A companion from before save points ignores it.
    SaveSlots = 16,
    // "The input you numbered N first reached the game in the frame with
    // serial S." Rides the media channel. The whole of how long a tap takes to
    // come back as a picture is measured from it: the companion knows when it
    // sent N and when it shows S, and those are two times on one clock -- see
    // CompanionSession's input_to_picture figures. A companion from before
    // this ignores it.
    InputEcho = 17,
    // A menu sound the host just played, for the companion to play too: the
    // quick menu is mirrored there, so it sounds there as well. See
    // MenuSoundMessage.
    MenuSound = 18,
    // The host's Controller Order screen, as it stands, for a companion to
    // draw while it is up -- see ControllersViewMessage. Sent when it changes.
    // A companion from before this ignores it and says where to look instead.
    ControllersView = 19,
    // either way
    Ping = 20,
    // The host's Graphics Packs page, as it stands, for a companion to draw
    // while it is up -- see GraphicPacksViewMessage. Sent when it changes. A
    // companion from before this ignores it and says where to look instead.
    GraphicPacksView = 21,
    // A Wii U game's own dialog -- an error, its keyboard -- up over it, for
    // the companion to show too; see GameDialogMessage. Sent when it changes.
    // A companion from before this ignores it.
    GameDialog = 22,
    // The host's amiibo browser, as it stands, for a companion to draw while
    // it is up -- see AmiiboBrowserViewMessage. Sent when it changes.
    AmiiboBrowserView = 23,
    // either way: where the menu page both devices show is scrolled -- see
    // MenuScrollMessage. Ignored by a peer from before it.
    MenuScroll = 24,
    // host -> companion: where the stylus on the right stick is, for the
    // companion to draw over the touch screen too -- see StylusCursorMessage.
    // Sent when it changes. A companion from before this ignores it.
    StylusCursor = 25,
    // host -> companion: the host's Button Mapping page, as it stands, for a
    // companion to draw while it is up -- see ButtonMappingViewMessage. Sent
    // when it changes. A companion from before this ignores it.
    ButtonMappingView = 26,
    // either way: whether the cursor shows on the page both devices show,
    // and what a finger is held on there -- see MenuCursorMessage. Sent when
    // it changes. Ignored by a peer from before it.
    MenuCursor = 27,
    // host -> companion: the game was reset, so what the companion is showing
    // is from before it and is taken down until the restarted game sends a
    // picture of its own. Carries the newest serial sent before the reset, so
    // a frame from before it still on its way is not taken for the restarted
    // game's first. A companion from before this ignores it and goes on
    // showing the old picture until a new one arrives.
    ScreensCleared = 28,
    // host -> companion: check for an update of the companion's own, or
    // install the one it found -- see UpdateRequestMessage. Sent when the
    // player checks for updates on the host. A companion from before this
    // ignores it, and the host offers only its own.
    UpdateRequest = 29,
    // host -> companion: a Wii U player's page, for the companion to draw
    // while it is up -- see WiiUPlayerViewMessage. Sent when it changes, and
    // once more when it closes.
    WiiUPlayerView = 30,
    // host -> companion: this device may start games on the host's computer
    // from now on, with this token -- see RemoteStartGrantMessage. Sent on
    // joining, when Remote Start is on there. A companion from before this
    // ignores it.
    RemoteStartGrant = 31,
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
    // The menu's own button: closes the menu from wherever its cursor is,
    // where Cancel steps back out of a section first. A host that predates it
    // drops it, as it drops any value it does not know.
    Close = 11,
    // L and R in a menu: the Touch Analog Stick made smaller and larger, a
    // page of a list.
    PageUp = 12,
    PageDown = 13,
    // The - button: Button Mapping's profiles.
    Minus = 14,
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
    // Whether this device has a mouse pointer (see device_has_pointer()). The
    // host offers the Pointer setting when either device has one, and only
    // then. Missing from an older build's hello, which is taken as yes: that
    // is what every build assumed before it was asked.
    bool has_pointer = true;
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
    // The game's icon for the quick menu's header, decoded already: RGBA8,
    // icon_width x icon_height, so the other device needs nothing of any
    // console's own format to show it. Trailing and optional: empty from an
    // older host, and an older companion stops reading before it.
    uint16_t icon_width = 0;
    uint16_t icon_height = 0;
    std::vector<uint8_t> icon_rgba;
    // Which of the icons kept for Discord is this game's ("wiiu/<title ID>"
    // and so on -- see discord_game_icon_url()), so a companion's Discord
    // status is the host's. Trailing and optional, after the icon -- sent
    // as 0x0 when there is none, so this still has a place.
    std::string icon_key;
    // The game's name as its console lays it out over its lines ("THE LEGEND
    // OF ZELDA", "The Wind Waker HD"), so a companion's Discord status breaks
    // it where the host's does. Trailing and optional, after icon_key -- sent
    // empty when there is none, so this still has a place.
    std::vector<std::string> title_lines;
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
    // A quick menu focus -- see LayoutPicker::focus_of(). One byte on the
    // wire for a peer from before the wider ones, which gets 0 for those, and
    // the whole of it again at the end.
    uint16_t menu_focus = 0;
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
    // What clicking the left stick does -- see LeftStickPress. The host's to
    // decide like the right stick, for the same reason: the quick menu that
    // offers it is one menu, and its entries and what they say have to be the
    // same on both screens.
    bool wants_left_stick_press = false;
    LeftStickPress left_stick_press = LeftStickPress::Microphone;
    // A DS game's Rumble Pak, in or out -- the host's to decide, per game.
    bool wants_rumble_pak = false;
    bool rumble_pak = false;
    // A DS game's LCD filter on or off -- see SessionStateMessage::lcd_filter.
    // Last, and optional.
    bool wants_lcd_filter = false;
    bool lcd_filter = false;
    // Where a Touch Point being placed on Button Mapping is put, by a finger
    // on the companion's drawing of the page -- as it moves, not only where
    // it lifts. In 65536ths of the touch screen on the wire.
    bool wants_touch_spot = false;
    float touch_spot_u = 0.5f;
    float touch_spot_v = 0.5f;
    // Whether the game starts where it was left the next time it is played --
    // see SessionStateMessage::resume_from_save_state.
    bool wants_resume_from_save_state = false;
    bool resume_from_save_state = false;
    // Separate Screens' swap: the two devices trade screens. One-shot, like
    // the reset above -- the result is SessionState's companion_screen.
    bool wants_screen_swap = false;
    // A 3DS game's custom textures on or off -- see
    // SessionStateMessage::custom_textures. Last, and optional.
    bool wants_custom_textures = false;
    bool custom_textures = false;
    // A new mapping profile's name, as typed on the companion so far -- see
    // ButtonMappingViewMessage::naming. Last, and optional.
    bool wants_profile_name = false;
    std::string profile_name;
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
    // As RequestMessage::menu_focus: a byte, and the whole of it at the end.
    uint16_t menu_focus = 0;
    // 0 none, 1 reset, 2 quit, and so on -- see MenuConfirm.
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
    // Whether the menu offers what the left stick's click does -- only where
    // the console has a microphone for it to be -- and what the host has it
    // set to, which is what both devices' L3 does while they are connected.
    // Sent rather than worked out at each end, so the Options list is the
    // same length on both screens and a cursor means the same row on each.
    bool left_stick_choosable = false;
    LeftStickPress left_stick_press = LeftStickPress::Microphone;
    // Whether the menu offers a Rumble Pak -- a DS game on a core that can
    // put one in its GBA slot -- and whether one is in.
    bool rumble_choosable = false;
    bool rumble_pak = false;
    // Whether this device's pointer is shown. Per-device in what it does, but
    // the menu that offers it is one menu, so the label it carries has to come
    // from the same place as everything else on it.
    bool cursor_shown = true;
    // Whether the host has any use for how the companion is being held. True
    // only when the console being played reads motion at all -- see
    // SystemProfile::uses_motion. A companion leaves its sensors off until
    // this says otherwise, which on a phone is the difference between a
    // gyro running for a whole session and one that never starts.
    bool motion_wanted = false;
    // Whether the host's core can make save points, which adds Load and
    // Create Save Point to the menu. Last, and optional: a host from before
    // save points sends nothing, which reads as none.
    bool save_points_available = false;
    // Which layouts are flipped for a left hand, by id -- chosen layout by
    // layout, and wanted here for the menu's previews of each. `handedness`
    // above stays the hand of the layout in use, for a companion from before
    // this, which only ever draws that one. Last, and optional: from an older
    // host it is worked out from those two -- see decode_session_state().
    std::vector<std::string> left_handed_layouts;
    // Whether the menu offers Resume from Save State -- a game whose core can
    // make save states -- and whether it is on: the next time the game is
    // started, it starts from where it was when the app last closed.
    bool resume_choosable = false;
    bool resume_from_save_state = false;
    // The game has just been started that way and is waiting, paused, under
    // a popup that says so, for somebody to press OK. The companion shows
    // the same popup, and its OK is sent over as MenuAction::Confirm.
    bool resume_notice = false;
    // Whether the menu offers Save Sync: the host's game has a save its Data
    // Management can sync. Last, and optional: from an older host, none.
    bool save_sync_available = false;
    // Which button of the menu's popup the cursor is on, plus one; 0 when
    // the cursor is not showing, or there is no popup. Last, and optional.
    uint8_t menu_confirm_focus = 0;
    // The few words in the top right corner that say the menu can't be opened
    // right now -- a Wii U game that would not pause for it -- are up on the
    // host, and the companion shows them too, for as long. Last, and
    // optional.
    bool menu_unavailable_notice = false;
    // The host's Wii U menu has Graphics Packs, Options and Scan amiibo --
    // whichever Cemu offered -- so the companion's has the rows too, and the
    // two line up. One byte, a bit each; last, and optional.
    bool graphic_packs_available = false;
    bool amiibo_available = false;
    // A Wii U's Options: Cemu's own rows, each one's name and value, after
    // the menu's own -- see LayoutPickerState::producer_options. Last, and
    // optional, as are the two below.
    std::vector<std::pair<std::string, std::string>> producer_options;
    // Scan amiibo's list, by name -- see LayoutPickerState::amiibo.
    std::vector<std::string> amiibo;
    std::string amiibo_folder;
    // Whether the menu offers an LCD filter -- a DS game's -- and whether it
    // is on: the screens drawn with their pixel grid, on both devices. A
    // byte, a bit each; last, and optional.
    bool lcd_filter_choosable = false;
    bool lcd_filter = false;
    // Each of producer_options' choices, in order, so the other device draws
    // the same popup when one is opened. Optional.
    std::vector<std::vector<std::string>> producer_choices;
    // The host's favorite color -- an index into kDsFavoriteColors -- which
    // the other device's menus take as their accent while it is connected,
    // or -1 for none said. A byte, the index plus one. Last, and optional.
    int favorite_color = -1;
    // The few words in the host's top right corner -- "Scanning…", a setting
    // that changes on a reset -- for the other device to show in its own, or
    // empty. Last, and optional.
    std::string corner_notice;
    // Where among producer_options the rows of the menu's Controls begin --
    // each player's controller -- or -1 for none. Two bytes, it plus one;
    // last, and optional.
    int producer_controls_from = -1;
    // The game's Button Mapping, which the companion's pad follows as the
    // host's does -- one game, one mapping. A count, then each PadControl's
    // PadAction as a byte. Last, and optional: from an older host, standard.
    ButtonMapping button_mapping;
    // Whether the menu offers Custom Textures -- a 3DS game on a core that
    // can load a texture pack -- and whether they are on. A byte, a bit each;
    // last, and optional.
    bool custom_textures_choosable = false;
    bool custom_textures = false;
    // A Wii U game's Controls tab: every controller, as the host lines them
    // up. A count, then each row; last, and optional.
    std::vector<ControllerRowView> wii_u_controllers;
    // A Wii U game's corner alerts about a controller -- a mapping profile
    // it switched to -- shown under corner_notice, oldest first. A count,
    // then each; last, and optional.
    std::vector<ControllerNotice> controller_notices;
};

struct FrameMessage {
    ScreenId screen = ScreenId::Top;
    uint64_t serial = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    FrameEncoding encoding = FrameEncoding::Rgb565Deflate;
    // Whether this frame stands on its own -- see FrameFragmentHeader's own
    // field, which this fills in. Not part of the TCP encoding of a frame:
    // frames only ever travel over the media channel now.
    bool keyframe = false;
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

// The game's icon, for beside its title in the companion's picker. Sent on the
// discovery port beside the beacon rather than inside it: an icon is several
// kilobytes even shrunk and deflated, a beacon has to stay one small datagram,
// and a broadcast too big for one packet is the kind a Wi-Fi network drops.
// So it goes in pieces, each a datagram of its own under its own magic number,
// which a build from before this reads as a beacon it cannot decode and drops.
constexpr uint32_t kBeaconIconMagic = 0x49535344u;  // "DSSI" little-endian
// Shrunk to fit this square first: the picker draws it at about this size on
// a 1080p screen, and a Wii U game's 128 would only be four times the pieces.
constexpr int kBeaconIconMaxSide = 64;
// What one piece carries, well under a packet once the header and the
// network's own are added.
constexpr std::size_t kBeaconIconPieceBytes = 1200;

struct BeaconIconPiece {
    uint16_t protocol_version = kProtocolVersion;
    // Which of the host's sessions it belongs to, as its beacon names it.
    uint16_t session_port = kSessionPort;
    // Tells one icon from the next, so pieces of two never get mixed.
    uint32_t icon_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t index = 0;
    uint16_t count = 0;
    // The whole deflated icon's size, which every piece repeats.
    uint32_t total_bytes = 0;
    std::vector<uint8_t> data;
};

// The Wii U GamePad's own motor, standing in for whichever controller the
// companion actually holds -- see ExternalControl::Rumble for where this
// starts. Just on/off: what a real Wii U GamePad's motor does with a finer
// pattern than that is not something a phone's or a Joy-Con's single motor
// could reproduce over a network link anyway.
struct SaveSlotsMessage {
    // kSavePointSlots of them, empty ones included.
    std::vector<SavePointInfo> slots;
    // One line about the last save -- "Saved to save point 1." -- or why not.
    std::string status;
    bool status_failed = false;
};

// The host's Controller Order screen, mirrored on the companion the way the
// quick menu is: everything its rows are drawn from, as the host has it, so
// the two screens show the same line with the same row picked out. Presses
// on the companion already reach the host's screen (see settings_open); this
// is only what it looks like.
//
// The pads as the host knows them, `on_this_device` meaning the host -- and
// the two names their rows say they are plugged into, worked out there, so
// the companion draws the host's words rather than guessing at them.
struct ControllersViewPad {
    bool on_this_device = true;
    uint32_t id = 0;
    int32_t position = 0;
    std::string name;
    int32_t slot = -1;
};

// The host's Graphics Packs page as it stands, for a companion to draw the
// same: the packs Cemu listed, and where the cursor is among them. Presses on
// the companion already reach the host's page (see settings_open); this is
// only what it looks like.
// Which of the host's pages both devices are showing, for MenuScrollMessage:
// the scroll of one is never taken for another's.
enum class MirroredPage : uint8_t {
    None = 0,
    QuickMenu = 1,
    GraphicPacks = 2,
    Controllers = 3,
    AmiiboBrowser = 4,
    ButtonMapping = 5,
    WiiUPlayer = 6,
};

// Where the page both devices show is scrolled -- see MenuPage::scroll_state()
// -- from the host as it stands, and from the companion as a finger or the
// wheel moves it there. Each takes the other's, but for while it is being
// scrolled itself.
struct MenuScrollMessage {
    MirroredPage page = MirroredPage::None;
    std::vector<float> positions;
};

// The cursor on the page both devices show, as one device has it: whether
// its ring is drawn -- a finger going down on either device hides it, a
// press of either pad brings it back -- and the entry (see
// MenuPage::entry_at()) a finger is held on there, filled on both, or -1.
// Each takes the other's.
struct MenuCursorMessage {
    MirroredPage page = MirroredPage::None;
    bool shown = true;
    int32_t held = -1;
    bool operator==(const MenuCursorMessage& other) const {
        return page == other.page && shown == other.shown && held == other.held;
    }
    bool operator!=(const MenuCursorMessage& other) const { return !(*this == other); }
};

// The host's amiibo browser, for the companion to draw: the folder, what is
// in it, and where the cursor is. Taps there go over as the row they landed
// on, like any mirrored page's.
struct AmiiboBrowserViewEntry {
    std::string label;
    bool is_directory = false;
    bool is_parent = false;
};
struct AmiiboBrowserViewMessage {
    bool open = false;
    std::string directory;
    std::vector<AmiiboBrowserViewEntry> entries;
    int32_t focus = 0;
};

// A game's own dialog, as the host shows it -- see ExternalGameDialog, which
// it carries. The companion shows the same popup, and answers it as the host
// would; the keyboard is typed on the companion when `typed_here`, the
// device in the player's hands being the GamePad.
struct GameDialogMessage {
    uint8_t kind = 0;  // ExternalGameDialogKind: 0 none, 1 message, 2 keyboard
    uint32_t serial = 0;
    std::string title;
    // A message's words; a keyboard's field as it now stands.
    std::string text;
    std::vector<std::string> buttons;
    uint32_t max_length = 0;
    bool typed_here = false;
};

// The companion's answer: a message's button, or a keyboard's field -- as it
// is typed (`final` false), so the host's popup shows it, and then as
// accepted.
struct GameDialogAnswerMessage {
    uint32_t serial = 0;
    bool final = true;
    int32_t button = 0;
    std::string text;
};

// The host asking the companion's updater: to look for a newer build of
// itself, or to install what it found, showing how it goes on its own screen
// as its Settings' Updates row does.
enum class UpdateRequest : uint8_t {
    Check = 1,
    Install = 2,
};
struct UpdateRequestMessage {
    UpdateRequest request = UpdateRequest::Check;
};

// The companion's answer, sent as the check starts and again when it ends.
enum class UpdateAnswer : uint8_t {
    // This device has no updater, or it is busy installing already.
    Unavailable = 0,
    Checking = 1,
    UpToDate = 2,
    Available = 3,
    Failed = 4,
};
struct UpdateAnswerMessage {
    UpdateAnswer answer = UpdateAnswer::Unavailable;
    // Available: the build it found, and the one it has (0 for a local
    // build).
    int32_t newest_build = 0;
    int32_t installed_build = 0;
    // Whether installing restarts it, rather than handing it to the system's
    // installer, as Android's is.
    bool restarts = true;
};

// What a companion needs to start games on the host's computer later, from
// its own first screen, with nobody at the computer: who the computer is to
// net::LaunchService, the token that service will take from this device,
// and how to wake it. Handed over on joining a game there, which is all the
// pairing there is.
struct RemoteStartGrantMessage {
    std::string host_id;
    std::string name;
    std::string kind;
    uint16_t port = 0;
    std::string token;
    std::vector<std::string> macs;
};

struct GraphicPacksViewChoice {
    std::string category;
    std::vector<std::string> presets;
    int32_t active = -1;
};

struct GraphicPacksViewPack {
    std::string id;
    std::string path;
    std::string name;
    std::string description;
    bool enabled = false;
    bool restart_needed = false;
    std::vector<GraphicPacksViewChoice> choices;
};

struct GraphicPacksViewMessage {
    bool open = false;
    // Still asking Cemu: nothing to list yet.
    bool waiting = true;
    bool restart_pending = false;
    int32_t focus = 0;
    bool in_pack = false;
    int32_t item = 0;
    std::vector<GraphicPacksViewPack> packs;
    // A preset's choices up beside its row, and which the cursor is on, plus
    // one; 0 for none. Last, and optional.
    uint16_t choice_focus = 0;
};

struct ControllersViewMessage {
    bool open = false;
    int32_t focus = 0;
    bool holding = false;
    bool gamepad_here = true;
    std::string this_device;
    std::string other_device;
    std::vector<ControllersViewPad> pads;
    // PadOrder, field for field.
    bool order_chosen = false;
    bool order_gamepad_here = true;
    std::string order_gamepad;
    std::vector<std::string> order_players;
    std::vector<std::string> order_unused;
};

// Which of the menus' sounds -- see UiSound, whose order this is numbered apart
// from, as MenuAction is from UiAction.
enum class MenuSound : uint8_t {
    MoveCursor = 1,
    Confirm = 2,
    Cancel = 3,
    Error = 4,
    OpenQuickMenu = 5,
    // Once the quick menu closing, which is Cancel now.
    SaveState = 6,
    ToggleOn = 7,
    ToggleOff = 8,
};

// The stylus cursor, as the host has it -- see StylusCursor. `u` and `v` are
// fractions of the touch screen, 0 to 1; `opacity` 0 means it is not drawn.
struct StylusCursorMessage {
    float u = 0.5f;
    float v = 0.5f;
    float opacity = 0.0f;
    bool pressed = false;
};

// The host's Button Mapping page, for the companion to draw the same: the row
// the cursor is on, and the picker or the question up over it. The mapping
// it shows is the session state's -- see SessionStateMessage::button_mapping.
struct ButtonMappingViewMessage {
    bool open = false;
    uint8_t focus = 0;
    // The picker's cursor plus one, or 0 when it is not up.
    uint8_t choice_focus = 0;
    // Reset to Default's question's cursor plus one, or 0 when it is not up.
    uint8_t confirm_focus = 0;
    // A Touch Point being placed, and where it is so far.
    bool placing = false;
    float spot_u = 0.5f;
    float spot_v = 0.5f;
    // Or the Touch Analog Stick, its centre the spot above and this its size.
    bool placing_stick = false;
    float stick_radius = kDefaultStickRadius;
    // A Wii U controller's page instead: which, the words at the right of its
    // heading, and its mapping as it is -- a Wii U game's mappings are the
    // host's alone, so the page carries the one it shows.
    bool wii_u = false;
    uint8_t wii_u_controller = 0;
    std::string wii_u_detail;
    WiiUMapping wii_u_mapping;
    // Profiles: the page of them up in place of the mapping
    // (ButtonMappingScreen::ProfilePage, its cursor plus one in choice_focus),
    // and what they are -- the names, the one in use (-1 for none), the
    // words over them -- a new one's name being typed, with its popup's
    // cursor, and what the question up asks (ButtonMappingScreen::Asking)
    // and of which profile. The cursor on Save Profile... or Load
    // Profile... is focus ButtonMappingScreen::kButtonFocusBase plus 0 or 1.
    uint8_t profile_page = 0;
    std::vector<std::string> profile_names;
    int profile_current = -1;
    std::string profile_heading;
    bool naming = false;
    std::string name_text;
    uint8_t naming_focus = 0;
    // The name is typed on the companion: it was the companion's press or
    // tap that asked for it. The keyboard comes up on that device alone.
    bool naming_on_companion = false;
    uint8_t asking = 0;
    int asking_profile = -1;

    bool operator==(const ButtonMappingViewMessage& other) const {
        return open == other.open && focus == other.focus && choice_focus == other.choice_focus &&
               confirm_focus == other.confirm_focus &&
               placing == other.placing && spot_u == other.spot_u && spot_v == other.spot_v &&
               placing_stick == other.placing_stick && stick_radius == other.stick_radius &&
               wii_u == other.wii_u && wii_u_controller == other.wii_u_controller &&
               wii_u_detail == other.wii_u_detail && wii_u_mapping == other.wii_u_mapping &&
               profile_page == other.profile_page && profile_names == other.profile_names &&
               profile_current == other.profile_current && profile_heading == other.profile_heading &&
               naming == other.naming && name_text == other.name_text && naming_focus == other.naming_focus &&
               naming_on_companion == other.naming_on_companion &&
               asking == other.asking && asking_profile == other.asking_profile;
    }
};

// The host's page for one of a Wii U game's players, or its GamePad, for the
// companion to draw the same: whose it is, the row the cursor is on, the
// popup up beside it, and what Add Controller will make a new one. The controllers it
// shows are the session state's -- see SessionStateMessage::wii_u_controllers.
struct WiiUPlayerViewMessage {
    bool open = false;
    PadSlot slot = PadSlot::gamepad();
    uint8_t focus = 0;
    // The popup's cursor plus one, or 0 when none is up.
    uint8_t choice_focus = 0;
    // Once Detect Controller up, and how many seconds it had left: a page
    // that no longer has it says false, but the bytes stay where they were.
    bool detecting = false;
    uint8_t detect_seconds = 0;
    // Add Controller's new player, and what Add will make it: Cemu's type
    // plus one, 0 for none, and the controller by pad_identity().
    bool adding = false;
    uint8_t pending_type = 0;
    std::string pending_pad;
    // Profile Switching: its button, 0 Off, 1 -, 2 +; the profiles it moves
    // through; every profile there is for the controller; and the one the
    // mapping is now, empty for none. Last, and optional.
    uint8_t switch_button = 0;
    std::vector<std::string> switch_profiles;
    std::vector<std::string> profile_names;
    std::string mapping_profile;

    bool operator==(const WiiUPlayerViewMessage& other) const {
        return open == other.open && slot == other.slot && focus == other.focus &&
               choice_focus == other.choice_focus && detecting == other.detecting &&
               detect_seconds == other.detect_seconds && adding == other.adding &&
               pending_type == other.pending_type && pending_pad == other.pending_pad &&
               switch_button == other.switch_button && switch_profiles == other.switch_profiles &&
               profile_names == other.profile_names && mapping_profile == other.mapping_profile;
    }
};

struct MenuSoundMessage {
    MenuSound sound = MenuSound::MoveCursor;
};

struct RumbleMessage {
    bool active = false;
    // Which of the companion's pads, by the id it reported (see ReportedPad),
    // or kWholeDevice for all of them -- the GamePad's motor on whatever the
    // companion is, which is what every rumble meant before a companion
    // reported its pads one by one.
    static constexpr uint32_t kWholeDevice = 0xFFFFFFFFu;
    uint32_t pad = kWholeDevice;
    // Non-zero for a single buzz of this many milliseconds that ends on its
    // own -- the Controllers screen asking which pad is which -- rather than
    // a motor switched on until it is switched off. Ended by the device that
    // buzzes, so it is the same length however late the message arrived; an
    // "off" sent after it would wait behind whatever else the connection is
    // carrying, which with a screen streaming is a lot.
    uint16_t ping_ms = 0;
    // How hard, in percent: a Wii Remote's is turned down -- see
    // Settings::wii_remote_rumble. 100 from a host that does not say.
    uint8_t strength = 100;
};

// One pad plugged into a companion, reported on its own so the host can make
// it any controller the game has -- see input/pad_routing.hpp. `id` is the
// companion's own number for it, stable while it stays plugged in, which is
// what a rumble names it by; `held` is its buttons, both sticks and, when it
// has its own sensors, its motion.
//
// Its name travels with it, but only to be shown: a pad is known by its id,
// never by a name Steam may no longer have matched to the pad it describes.
struct ReportedPad {
    // The id an earlier build of this companion reported its keyboard under.
    // Keyboards are not reported any more -- on a Wii U the keys are Cemu's
    // to map -- and a host skips one from a companion that still does.
    static constexpr uint32_t kKeyboard = 0xFFFFFFFEu;
    uint32_t id = 0;
    DsInputState held;
    // What the companion's SDL calls it, cut to kMaxReportedPadName bytes.
    // Empty from a companion that does not say.
    std::string name;
    // Its player slot on the companion, from 0, or -1 -- see KnownPad::slot.
    int slot = -1;
    // Its buttons, heard by the game or not -- see KnownPad::pressing. 0
    // from a companion that does not say.
    uint32_t pressing = 0;
    // Its own buttons, unmapped -- see KnownPad::controls -- and whether the
    // companion said: one too old to is read from `held` instead.
    uint32_t controls = 0;
    bool has_controls = false;
};

// This device's pads as a companion reports them -- see InputMessage::pads.
template <typename Readings>
std::vector<ReportedPad> reported_pads(const Readings& readings) {
    std::vector<ReportedPad> pads;
    pads.reserve(readings.size());
    for (const auto& reading : readings) {
        ReportedPad pad;
        pad.id = static_cast<uint32_t>(reading.id);
        pad.held = reading.held;
        pad.name = reading.name;
        pad.slot = reading.slot;
        pad.pressing = reading.pressing;
        pad.controls = reading.controls;
        pad.has_controls = true;
        pads.push_back(pad);
    }
    return pads;
}

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
    // How the companion is being held and moved. Carried raw, in SDL's own
    // units and axes -- see DsMotionState, whose fields these are. Absent
    // unless the host asked for it (SessionStateMessage::motion_wanted), so a
    // session on a console that has no use for motion costs nothing here and
    // no battery on the device sending it.
    DsMotionState motion;
    // Every pad plugged into the companion, each on its own, in the
    // companion's own order. `reports_pads` false means a companion built
    // before this existed, which sends only the merged state above; the host
    // then treats that device as one pad.
    //
    // A tail after everything above, so a host built before this reads the
    // fields it knows and never sees the rest.
    bool reports_pads = false;
    std::vector<ReportedPad> pads;
    // Which input this is, counting up from the start of the session, so the
    // host can say which frame it first reached (see InputEcho) and can tell
    // a copy sent again from something new (see CompanionSession::send_input).
    // The last tail of all, and only after the pads' -- a message with no pads
    // tail has nowhere to put it that an older host would not misread.
    bool has_sequence = false;
    uint32_t sequence = 0;
};

// See MessageType::InputEcho.
struct InputEchoMessage {
    uint32_t sequence = 0;
    uint64_t serial = 0;
};
void encode_input_echo_payload(const InputEchoMessage& message, std::vector<uint8_t>& out);
bool decode_input_echo_payload(const uint8_t* data, std::size_t size, InputEchoMessage& out);

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
void encode_screens_cleared(uint64_t last_serial, std::vector<uint8_t>& out);
bool decode_screens_cleared(const uint8_t* data, std::size_t size, uint64_t& out_last_serial);
void encode_menu_sound(const MenuSoundMessage& message, std::vector<uint8_t>& out);
void encode_save_slots(const SaveSlotsMessage& message, std::vector<uint8_t>& out);
void encode_controllers_view(const ControllersViewMessage& message, std::vector<uint8_t>& out);
void encode_graphic_packs_view(const GraphicPacksViewMessage& message, std::vector<uint8_t>& out);
void encode_game_dialog(const GameDialogMessage& message, std::vector<uint8_t>& out);
void encode_menu_scroll(const MenuScrollMessage& message, std::vector<uint8_t>& out);
void encode_menu_cursor(const MenuCursorMessage& message, std::vector<uint8_t>& out);
void encode_stylus_cursor(const StylusCursorMessage& message, std::vector<uint8_t>& out);
void encode_button_mapping_view(const ButtonMappingViewMessage& message, std::vector<uint8_t>& out);
void encode_wii_u_player_view(const WiiUPlayerViewMessage& message, std::vector<uint8_t>& out);
void encode_amiibo_browser_view(const AmiiboBrowserViewMessage& message, std::vector<uint8_t>& out);
void encode_game_dialog_answer(const GameDialogAnswerMessage& message, std::vector<uint8_t>& out);
void encode_update_request(const UpdateRequestMessage& message, std::vector<uint8_t>& out);
void encode_remote_start_grant(const RemoteStartGrantMessage& message, std::vector<uint8_t>& out);
void encode_update_answer(const UpdateAnswerMessage& message, std::vector<uint8_t>& out);
void encode_request(const RequestMessage& message, std::vector<uint8_t>& out);
// `quit_too`, from the host: the player quit from the companion, so the
// companion closes its own app rather than going back to its main menu. One
// byte, and only when set -- a Bye from before this is empty, and reads as an
// ordinary end.
void encode_bye(std::vector<uint8_t>& out, bool quit_too = false);
bool bye_says_quit_too(const std::vector<uint8_t>& payload);
void encode_frame_ack(uint64_t serial, std::vector<uint8_t>& out);
// As encode_audio_payload() is to encode_audio() above.
void encode_frame_ack_payload(uint64_t serial, std::vector<uint8_t>& out);
bool decode_frame_ack(const uint8_t* data, std::size_t size, uint64_t& out_serial);

// How much later than its quickest the newest acked frame reached the
// companion, in milliseconds: the outward half of a round trip, which the host
// cannot measure on its own. Added after the serial; an ack from before this
// has none, and one that has not measured says kUnknownLinkDelay.
inline constexpr uint16_t kUnknownLinkDelay = 0xFFFF;
void encode_frame_ack_payload(uint64_t serial, uint16_t link_delay_ms, std::vector<uint8_t>& out);
// `out_link_delay_ms` is kUnknownLinkDelay when the ack does not say.
bool decode_frame_ack(const uint8_t* data, std::size_t size, uint64_t& out_serial,
                      uint16_t& out_link_delay_ms);
// The beacon is not framed -- it replaces the buffer with one datagram.
void encode_beacon(const BeaconMessage& message, std::vector<uint8_t>& out);
bool decode_beacon(const uint8_t* data, std::size_t size, BeaconMessage& out);
// The icon (RGBA8, width x height), shrunk and deflated and cut into the
// datagrams that carry it. Empty when there is no icon to send.
std::vector<std::vector<uint8_t>> encode_beacon_icon(int width,
                                                     int height,
                                                     const std::vector<uint8_t>& rgba,
                                                     uint16_t session_port);
bool decode_beacon_icon_piece(const uint8_t* data, std::size_t size, BeaconIconPiece& out);
// The pieces' bytes, put together, back into RGBA8. False unless they come to
// exactly width x height.
bool inflate_beacon_icon(const std::vector<uint8_t>& deflated,
                         int width,
                         int height,
                         std::vector<uint8_t>& rgba);
void encode_ping(std::vector<uint8_t>& out);

bool decode_hello(const uint8_t* data, std::size_t size, HelloMessage& out);
bool decode_welcome(const uint8_t* data, std::size_t size, WelcomeMessage& out);
bool decode_reject(const uint8_t* data, std::size_t size, RejectMessage& out);
bool decode_session_state(const uint8_t* data, std::size_t size, SessionStateMessage& out);
bool decode_frame(const uint8_t* data, std::size_t size, FrameMessage& out);
bool decode_input(const uint8_t* data, std::size_t size, InputMessage& out);
bool decode_audio(const uint8_t* data, std::size_t size, AudioMessage& out);
bool decode_rumble(const uint8_t* data, std::size_t size, RumbleMessage& out);
// False for a sound this end does not know.
bool decode_menu_sound(const uint8_t* data, std::size_t size, MenuSoundMessage& out);
bool decode_save_slots(const uint8_t* data, std::size_t size, SaveSlotsMessage& out);
bool decode_controllers_view(const uint8_t* data, std::size_t size, ControllersViewMessage& out);
bool decode_graphic_packs_view(const uint8_t* data, std::size_t size, GraphicPacksViewMessage& out);
bool decode_game_dialog(const uint8_t* data, std::size_t size, GameDialogMessage& out);
bool decode_menu_scroll(const uint8_t* data, std::size_t size, MenuScrollMessage& out);
bool decode_menu_cursor(const uint8_t* data, std::size_t size, MenuCursorMessage& out);
bool decode_stylus_cursor(const uint8_t* data, std::size_t size, StylusCursorMessage& out);
bool decode_button_mapping_view(const uint8_t* data, std::size_t size, ButtonMappingViewMessage& out);
bool decode_wii_u_player_view(const uint8_t* data, std::size_t size, WiiUPlayerViewMessage& out);
bool decode_amiibo_browser_view(const uint8_t* data, std::size_t size, AmiiboBrowserViewMessage& out);
bool decode_game_dialog_answer(const uint8_t* data, std::size_t size, GameDialogAnswerMessage& out);
bool decode_update_request(const uint8_t* data, std::size_t size, UpdateRequestMessage& out);
bool decode_remote_start_grant(const uint8_t* data, std::size_t size, RemoteStartGrantMessage& out);
bool decode_update_answer(const uint8_t* data, std::size_t size, UpdateAnswerMessage& out);

// The most pads one companion may report. Far more than anyone plugs into one
// device; a bound so a malformed message cannot ask for a million.
inline constexpr std::size_t kMaxReportedPads = 16;
// The longest name a reported pad carries; longer ones are cut.
inline constexpr std::size_t kMaxReportedPadName = 48;
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

// Writes FrameFragmentHeader::sent_at_ms into a media datagram that has
// already been built. The only thing ever written into one after the fact,
// and the reason is worth stating.
//
// The stamp has to be taken when the datagram really leaves, not when it was
// built: everything between those two moments is the sender's own pacing --
// see HostSession::drain_pending_media_sends() -- and the far end reads the
// spread of transit times as jitter on the link and buys buffer against it.
// Buffer cannot cover a delay the sender has already paid; the picture is
// late either way. Measured as link jitter it is paid for twice, once in the
// queue and again in the buffer.
//
// Returns false, changing nothing, for a datagram that is not a Frame
// fragment or is too short to hold one.
bool stamp_media_datagram_sent_at(uint8_t* datagram, std::size_t size, uint32_t sent_at_ms);
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
    // Whether this frame stands on its own -- an H.264 IDR, or any tile
    // frame, which is always whole. What tells the far end that the recovery
    // it asked for has actually arrived, so it can stop asking; a request
    // sent over UDP can be lost like anything else, so "stop when it lands"
    // is the only reliable way to end the asking. Always true for encodings
    // that have no notion of a reference frame at all.
    bool keyframe = false;
    // Which frame this is in the run of frames actually sent on this screen,
    // counting from the start of the session and wrapping.
    //
    // Not the serial, which counts frames the *emulator* made: the host sends
    // only some of those -- see publish_screens() -- so a gap in serials is
    // usually the host choosing to skip one and says nothing about the link.
    // This counts only what went out, so a gap in it means one thing and one
    // thing only: a frame left the host and never got here.
    //
    // Which is the only way whole-frame loss can be seen at all. Reassembly
    // notices a frame that arrived in pieces and never completed, but a frame
    // small enough to be one datagram -- which on a DS is nearly all of them,
    // at around a kilobyte each -- either arrives or vanishes without trace.
    // Those were being lost silently, leaving the decoder predicting from a
    // reference frame it never received: a picture that goes wrong and stays
    // wrong until the next keyframe comes around on its own.
    uint16_t sent_index = 0;
    // When the host sent this, on the host's own monotonic clock.
    //
    // The two machines' clocks have no relationship to each other and this
    // does not try to give them one. What it buys is the *variation* in how
    // long a frame took to arrive: the offset between the clocks is unknown
    // but constant, so it cancels the moment two transit times are compared,
    // and what is left is queueing on the link and nothing else.
    //
    // That distinction is the whole reason this exists. Unevenness measured
    // from arrivals alone cannot tell a link that stalled from an emulator
    // that rendered slowly, and buying buffer for the second does nothing but
    // add delay -- those frames were already late when they were made. Citra
    // on a Steam Deck swings between 18 and 58 frames a second, and that read
    // as 55 to 78ms of link jitter on a link whose round trip averaged 34ms
    // and was dropping almost nothing.
    //
    // Milliseconds, wrapping every 49 days, which outlasts any session and
    // would be harmless anyway: only differences are ever taken.
    uint32_t sent_at_ms = 0;
};

// FrameFragmentHeader on the wire: 8 (serial) + 1 (screen) + 1 (encoding)
// + 2*4 (width, height, fragment index, fragment count) + 1 (keyframe)
// + 4 (sent_at_ms) + 2 (sent_index). Named because the host sizes its datagram
// budget from it and the decoder skips past it, and those two disagreeing is a
// whole frame of garbage rather than an error.
constexpr std::size_t kFrameFragmentHeaderBytes = 25;

// How many frames went missing between the last sent_index seen on a screen
// and one that has just arrived. Zero for the next one in the run, and zero
// for anything that arrives out of order or twice -- a fragment of the frame
// already being put together, or one that overtook its neighbours.
//
// Wrapping is handled by taking the difference as a signed sixteen-bit value,
// so the counter passing 65535 reads as one step forward rather than as
// sixty-five thousand frames of loss.
unsigned frames_missing_between(uint16_t last_seen, uint16_t arrived);

// Recovery pieces for a video frame, so a lost datagram does not lose the
// frame. Over wifi datagrams go missing now and then, and a frame of eighty of
// them -- a 720p keyframe -- has about even odds of losing one at a loss rate
// of one in a hundred. Lost, the frame was gone: the picture held while a new
// keyframe was asked for, which could lose a piece just the same.
//
// Reed-Solomon, the way Moonlight and Sunshine do it: about a fifth again as
// many recovery pieces as the frame has pieces of its own, never fewer than
// two, and any of them stands in for any missing piece -- so a frame comes
// through whole however its losses fall, up to as many as it has recovery
// pieces. A frame of more than kMaxDataPerParityBlock pieces is split into
// blocks, each with its own. (The first version here was one XOR piece per
// eight, which put back one loss per group and nothing more.)
//
// Numbered straight on from the frame's own pieces.
//
// A recovery payload is the length of the frame's last piece (u16) -- the
// only length a lost piece cannot be told from its neighbours -- and then the
// piece itself.
constexpr std::size_t kParityPrefixBytes = 2;
constexpr std::size_t kParityPercent = 20;
constexpr std::size_t kMinParityPerBlock = 2;
constexpr std::size_t kMaxDataPerParityBlock = 200;
// Where a frame's recovery pieces start, and how many there are.
std::size_t frame_parity_first_index(std::size_t fragment_count);
std::size_t frame_parity_count(std::size_t fragment_count);
// The recovery payloads for `payload` cut into `fragment_size` pieces, in the
// order they are numbered.
std::vector<std::vector<uint8_t>> build_frame_parity(const std::vector<uint8_t>& payload,
                                                     std::size_t fragment_size);
// Puts back every missing piece the recovery pieces that arrived can:
// everything in a block missing no more pieces than it has recovery pieces
// here. `fragments`/`received` are the frame's own pieces; `parity`/
// `parity_received` are frame_parity_count() long. Returns how many came back.
unsigned recover_fragments(std::vector<std::vector<uint8_t>>& fragments,
                           std::vector<bool>& received,
                           const std::vector<std::vector<uint8_t>>& parity,
                           const std::vector<bool>& parity_received);

// The payload of a KeyframeRequest: which screen went wrong, and the serial
// the far end last managed to put together whole (zero if it never has). The
// serial is only used to tell a request that crossed with a keyframe already
// in flight from one that still needs answering.
struct KeyframeRequestMessage {
    ScreenId screen = ScreenId::Top;
    uint64_t last_good_serial = 0;
};

void encode_keyframe_request_payload(const KeyframeRequestMessage& request,
                                     std::vector<uint8_t>& out);

// Which pieces of which frame to send again: the frame by its screen and its
// sent_index -- which, unlike its serial, names one sending of it: a picture
// sent again after a stall goes out under the same serial, encoded afresh --
// and the indices of its own pieces (never its recovery pieces) that did not
// arrive. At most kMaxRepairPieces, which a single datagram holds with room
// to spare; a frame missing more than that is not worth repairing.
//
// Or, with `whole_frame`, every piece of the frame sent `sent_index`th on that
// screen: one lost outright, which the companion knows of only from the gap
// it left in the run of sent_index values, and so has no serial to name it by.
// A small video frame is one or two datagrams, and a burst of loss takes it
// whole.
struct RepairRequestMessage {
    ScreenId screen = ScreenId::Top;
    bool whole_frame = false;
    uint64_t serial = 0;
    uint16_t sent_index = 0;
    std::vector<uint16_t> pieces;
};
constexpr std::size_t kMaxRepairPieces = 256;
void encode_repair_request_payload(const RepairRequestMessage& request, std::vector<uint8_t>& out);
bool decode_repair_request_payload(const uint8_t* data, std::size_t size,
                                   RepairRequestMessage& out);
bool decode_keyframe_request_payload(const uint8_t* data, std::size_t size,
                                     KeyframeRequestMessage& out);

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
//
// `also_unlike` is every other state the recipient might be showing any tile
// of: frames already sent on top of `previous` whose arrival is not yet
// confirmed. A tile is sent if it differs from any of them too. Without that,
// a tile one of those frames changed and this one puts back matches
// `previous`, is left out -- and stays wrong on the far end for good, once
// that end confirms this frame and it becomes the next `previous` itself.
// Unchanged pictures of a moving game left exactly those tiles behind.
bool encode_screen_tiles(const uint32_t* pixels,
                         int width,
                         int height,
                         int stride_pixels,
                         std::vector<uint8_t>& previous,
                         std::vector<uint8_t>& out,
                         PixelChannelOrder channel_order = PixelChannelOrder::XrgbBytes,
                         const std::vector<const std::vector<uint8_t>*>& also_unlike = {});

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
