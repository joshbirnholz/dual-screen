#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "input/raw_pad_state.hpp"
#include "layout/screen_id.hpp"
#include "net/transport.hpp"
#include "net/web_tls.hpp"

namespace ds::net {

// Web Play: a Wii U game watched and played from a web browser on another
// device, with nothing installed there.
//
// This computer serves one page over HTTPS. A browser that opens it gives a
// name, gets the television's picture and sound over a WebSocket -- H.264 it
// decodes itself with WebCodecs, and plain PCM -- and sends back the gamepads
// plugged into it (or the touch controls drawn on a phone), which this app
// then has as controllers of its own: in Controls and Change Order like any
// other, and handed to Cemu as players.
//
// How the browser got here is none of this file's business: the same LAN, a
// forwarded port, a VPN. It is a TCP connection to one port either way.
//
// This class is the protocol and nothing else -- no threads, no encoder, no
// app. Polled with pump(); see WebPlayHost for the thread that drives it.

// The WebSocket handshake's answer to a client's Sec-WebSocket-Key.
std::string websocket_accept_key(const std::string& client_key);

// One frame, server to client: never masked, never fragmented.
std::vector<uint8_t> websocket_frame(uint8_t opcode, const uint8_t* data, std::size_t size);

// A browser's frames, as they arrive: masked, maybe fragmented, maybe several
// in one read and maybe one across several.
class WebSocketReader {
public:
    struct Message {
        uint8_t opcode = 0;  // 1 text, 2 binary, 8 close, 9 ping, 10 pong
        std::vector<uint8_t> data;
    };
    // Takes what arrived; false if it was not WebSocket at all.
    bool feed(const uint8_t* data, std::size_t size, std::vector<Message>& out);

private:
    std::vector<uint8_t> buffer_;
    uint8_t fragment_opcode_ = 0;
    std::vector<uint8_t> fragments_;
};

// Messages on the WebSocket, by their first byte. Text messages are JSON
// with a "type"; binary ones are these.
enum class WebMessage : uint8_t {
    // Server to browser.
    Video = 1,  // screen, flags (1 keyframe), width u16, height u16, then Annex B H.264
    Audio = 2,  // screen, rate u32, channels u8, then s16 interleaved
    Menu = 3,   // format (0 JPEG, 1 PNG), width u16, height u16, then the picture
    // Browser to server.
    Pads = 16,  // count, then per pad: index, buttons u32 (SDL bits), 6 axes s16 (SDL order)
    Touch = 17,  // down u8, x u16, y u16: where on the picture, 0 to 65535 across and down
};

// What a browser holding the GamePad asked to see: the GamePad's screen, the
// TV's, or both. Everyone else sees the TV.
enum class WebView : uint8_t { Own, Tv, Both };

// What a browser's gamepads look like from here: one RawPadState each, the
// same as a pad plugged into this computer.
struct WebPad {
    // Which browser, for as long as it stays connected, and which of its
    // gamepads (the page's own number for it; the touch controls are 15).
    uint32_t viewer = 0;
    uint8_t index = 0;
    // The browser's name for it: "Xbox Wireless Controller", "Touch Controls".
    std::string name;
    RawPadState raw;
    // A unique id for this pad across every browser -- see web_pad_id().
    uint32_t id = 0;
};

// The id a browser's pad goes by among this computer's pads: never an SDL
// instance id (those count up from 0), never the touch controller's.
inline constexpr uint32_t kWebPadIdBase = 0x7E000000u;
inline uint32_t web_pad_id(uint32_t viewer, uint8_t index) {
    return kWebPadIdBase + ((viewer & 0xFFFFu) << 4) + (index & 0x0Fu);
}
inline bool is_web_pad_id(uint32_t id) { return id >= kWebPadIdBase && id < kWebPadIdBase + 0x100000u; }

// One browser, as the app sees it.
struct WebViewer {
    uint32_t id = 0;
    // What they typed when they opened the page.
    std::string name;
    std::string address;
    // Which screen is theirs: the GamePad's while their controller is it.
    ScreenId screen = ScreenId::Top;
    // And what of it they asked to see.
    WebView view = WebView::Own;
    // A finger (or the mouse) on that picture, from 0 to 65535 across and
    // down it -- the GamePad's touch screen, to a browser holding it.
    bool touching = false;
    uint16_t touch_x = 0;
    uint16_t touch_y = 0;
};

class WebPlayServer {
public:
    struct Config {
        uint16_t port = 0;
        // The certificate to answer with. Empty serves plain HTTP -- for the
        // tests, which have no browser to insist.
        TlsIdentity identity;
        // The page itself. Empty serves the one built into the app.
        std::string page;
        // Shown on the page before joining: "Steam Deck", the game's name.
        std::string device_name;
        std::string game_title;
    };

    WebPlayServer();
    ~WebPlayServer();
    WebPlayServer(const WebPlayServer&) = delete;
    WebPlayServer& operator=(const WebPlayServer&) = delete;

    bool start(const Config& config, std::string* error);
    void stop();
    bool running() const { return listener_.listening(); }
    uint16_t port() const { return listener_.port(); }

    // Accepts, reads and writes whatever is waiting, without waiting.
    void pump();
    // Every socket this has open, for a caller that wants to sleep until one
    // of them has something.
    std::vector<int> descriptors() const;
    bool wants_to_write() const;

    // The browsers that have joined, and their pads.
    std::vector<WebViewer> viewers() const;
    std::vector<WebPad> pads() const;
    // Bumped whenever someone joins or leaves, or a pad comes or goes.
    uint64_t roster_serial() const { return roster_serial_; }

    // Which screen a browser is sent; the television until told otherwise.
    void set_viewer_screen(uint32_t viewer, ScreenId screen);
    // Whether anyone is watching this screen.
    bool screen_watched(ScreenId screen) const;
    // Whether someone is waiting on a keyframe of it -- just joined, just
    // switched to it, or fell behind and had frames dropped. Cleared by asking.
    bool take_keyframe_request(ScreenId screen);

    // A frame of `screen`, to every browser watching it that can take it. A
    // browser still waiting for a keyframe is skipped until one comes; one
    // too far behind has this one dropped, and is waiting for a keyframe again.
    void send_video(ScreenId screen, const uint8_t* data, std::size_t size, bool keyframe, int width, int height);
    void send_audio(ScreenId screen, const int16_t* samples, std::size_t frames, uint32_t rate, uint8_t channels);
    // A JSON text message, to one browser or (viewer 0) all of them.
    void send_text(uint32_t viewer, const std::string& json);
    // The quick menu: a picture of it (a whole Menu message, see WebMessage)
    // while it is open, to every browser and to each one that joins; or that
    // it has closed. Open with no picture keeps the last one.
    void send_menu(bool open, std::vector<uint8_t> message);
    bool menu_open() const { return menu_open_; }
    // Vibration, for a browser's pad.
    void send_rumble(uint32_t pad_id, uint16_t low, uint16_t high, uint32_t duration_ms);

    // What is still queued to one browser, for tests.
    std::size_t pending_bytes(uint32_t viewer) const;

private:
    struct Viewer;
    void pump_viewer(Viewer& viewer);
    void handle_http(Viewer& viewer);
    void handle_message(Viewer& viewer, const WebSocketReader::Message& message);
    void send_frame(Viewer& viewer, uint8_t opcode, const uint8_t* data, std::size_t size);
    void want_keyframes(Viewer& viewer);
    void send_menu_to(Viewer& viewer);

    Config config_;
    TlsContext tls_;
    bool tls_on_ = false;
    TcpListener listener_;
    std::vector<std::unique_ptr<Viewer>> viewers_;
    uint32_t next_viewer_id_ = 1;
    uint64_t roster_serial_ = 0;
    bool keyframe_wanted_[2] = {false, false};
    bool menu_open_ = false;
    std::vector<uint8_t> menu_image_;
};

}  // namespace ds::net
