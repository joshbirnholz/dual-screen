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
    void connect_to(const DiscoveredHost& host);
    void disconnect();

    void poll(uint64_t now_ms);

    Status status() const { return status_; }
    const std::string& status_detail() const { return status_detail_; }
    const std::vector<DiscoveredHost>& hosts() const { return browser_.hosts(); }
    const std::string& game_title() const { return game_title_; }
    ScreenId granted_screen() const { return granted_screen_; }
    // The layout the host is showing, so this device draws the same thing.
    const std::string& host_layout_id() const { return host_layout_id_; }
    bool host_paused() const { return host_paused_; }

    // Sends this device's input to the host. Only what changed goes out.
    void send_input(const DsInputState& state);

    // Asks the host to change layout. The host owns the choice and broadcasts
    // the result back, so nothing is assumed to have taken effect here.
    void request_layout(const std::string& layout_id);

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
    ScreenSize native_screen_size() const override;
    bool wants_shutdown() const override { return false; }

private:
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
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
    std::string host_layout_id_;
    bool host_paused_ = false;
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
