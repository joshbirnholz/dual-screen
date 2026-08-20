#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/screen_source.hpp"
#include "input/ds_input.hpp"
#include "net/discovery.hpp"
#include "net/protocol.hpp"
#include "net/transport.hpp"

namespace ds::net {

// The companion half: finds hosts, connects to one, and presents the screens it
// receives as a ScreenSource -- the same seam the libretro core plugs into, so
// the compositor and layout system never learn where the pixels came from.
class CompanionSession final : public ScreenSource {
public:
    enum class Status {
        Idle,
        Searching,
        Connecting,
        Connected,
        Failed,
    };

    CompanionSession();
    ~CompanionSession() override;

    // Starts browsing for hosts. Safe to call repeatedly.
    bool begin_search(const std::string& device_name, ScreenId wanted_screen, std::string* error);
    // Connects to one of the hosts found by the search.
    // By value: callers pass an entry out of hosts(), and a session that ever
    // clears that list while connecting would leave the argument dangling.
    void connect_to(DiscoveredHost host);
    void disconnect();

    void poll(uint64_t now_ms);

    Status status() const { return status_; }
    const std::string& status_detail() const { return status_detail_; }
    const std::vector<DiscoveredHost>& hosts() const { return browser_.hosts(); }
    const std::string& game_title() const { return game_title_; }
    // Which console the host is playing, so this device draws it the same way.
    SystemId system() const { return system_; }
    ScreenId granted_screen() const { return granted_screen_; }
    // The session as the host sees it. This device follows all of it rather
    // than deciding anything locally, which is what keeps the two in step.
    const std::string& host_layout_id() const { return host_layout_id_; }
    bool host_paused() const { return host_paused_; }
    bool host_filtering() const { return host_filtering_; }
    AudioTarget host_audio_target() const { return host_audio_target_; }
    Handedness host_handedness() const { return host_handedness_; }

    // Sends this device's input to the host. Only what changed goes out.
    void send_input(const DsInputState& state);

    // Asks the host to change something. The host owns every one of these and
    // broadcasts the result back, so nothing is assumed to have taken effect
    // here -- which is exactly why the two devices cannot end up disagreeing.
    void request_layout(const std::string& layout_id);
    void request_filtering(bool bilinear);
    void request_audio_target(AudioTarget target);
    void request_handedness(Handedness handedness);

    // Tells the host whether a menu is up on this device, so it pauses too.
    // Sent whenever it changes, and repeated with every other request.
    void set_menu_open(bool open);

    // Called with audio received from the host, ready to play. Set before
    // connecting.
    using AudioCallback =
        std::function<void(const int16_t* interleaved_stereo, std::size_t frames, uint32_t rate)>;
    void set_audio_callback(AudioCallback callback) { audio_callback_ = std::move(callback); }
    // The rate the host said it produces audio at, known from the handshake.
    uint32_t audio_sample_rate() const { return audio_sample_rate_; }

    // Whether a screen has ever arrived, and the serial of the last one. The
    // split layout only sends one screen, so "did the other one arrive?" is a
    // real question worth being able to answer.
    bool has_screen(ScreenId screen) const;
    uint64_t received_serial(ScreenId screen) const;
    // Top-left pixel of the last frame for a screen, for tests that have no GL
    // context and so cannot look at the texture.
    uint32_t sample_pixel(ScreenId screen) const;

    // --- ScreenSource ---
    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    SystemScreens screens_geometry() const override;
    bool wants_shutdown() const override { return false; }

private:
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
    void send_request(RequestMessage request);
    void fail(const std::string& reason);

    HostBrowser browser_;
    TcpStream stream_;
    std::vector<uint8_t> inbox_;

    Status status_ = Status::Idle;
    std::string status_detail_;
    std::string device_name_;
    ScreenId wanted_screen_ = ScreenId::Bottom;
    ScreenId granted_screen_ = ScreenId::Bottom;
    std::string game_title_;
    SystemId system_ = SystemId::Ds;
    std::string host_layout_id_;
    bool host_paused_ = false;
    bool host_filtering_ = false;
    AudioTarget host_audio_target_ = AudioTarget::Both;
    Handedness host_handedness_ = Handedness::Right;
    bool menu_open_ = false;
    bool said_hello_ = false;

    // Latest pixels per screen, uploaded lazily so a frame that arrives twice
    // between draws is only sent to the GPU once.
    struct ScreenBuffer {
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;
        uint64_t uploaded_serial = 0;
        Texture texture;
    };
    ScreenBuffer top_;
    ScreenBuffer bottom_;
    ScreenTextures textures_;

    DsInputState last_sent_input_;
    bool ever_sent_input_ = false;
    AudioCallback audio_callback_;
    uint32_t audio_sample_rate_ = 0;
};

const char* to_string(CompanionSession::Status status);

}  // namespace ds::net
