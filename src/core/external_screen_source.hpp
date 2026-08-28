#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "core/external_frame_link.hpp"
#include "core/screen_source.hpp"
#include "core/system.hpp"
#include "input/ds_input.hpp"

namespace ds {

// A console played by another program, seen through the seam everything else in
// this app already looks through. See ExternalFrameLink for the handover
// itself, and SystemProfile::frame_origin for which consoles arrive this way.
//
// The differences from a core are all absences. There is no ROM to load, no
// save RAM to flush, no options to force and no render scale to set: whatever
// is playing the game owns all of that and is configured in its own right. What
// is left is two screens, some audio, and a way to ask for a reset -- which is
// the whole of what this seam was ever for.
class ExternalScreenSource final : public ScreenSource {
public:
    // Tagged by screen: a Wii U's TV and GamePad audio are independent
    // streams, each routed by whatever the player set that screen's audio
    // target to.
    using AudioCallback =
        std::function<void(ScreenId screen, const int16_t* interleaved_stereo, std::size_t frames)>;

    explicit ExternalScreenSource(SystemId system);
    ~ExternalScreenSource() override;

    // Starts listening. The game is not here yet and may never arrive; that is
    // not a failure, and running() says which it turned out to be.
    bool open(const std::string& socket_path, std::string* error);
    void close();
    // True once a game has connected and said what it is sending.
    bool running() const { return link_.connected(); }
    // Why the session ended, once it has. Empty while it has not.
    const std::string& ended_reason() const { return ended_reason_; }

    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    SystemScreens screens_geometry() const override;
    bool read_screen(ScreenId screen, ScreenReadback& out, int target_width = 0,
                     int target_height = 0) override;
    // The two screens arrive under one serial each frame but are published
    // separately, so either may be the one that has just landed.
    uint64_t frame_serial() const override { return std::max(top_.serial, bottom_.serial); }
    bool wants_shutdown() const override { return ended_; }

    void set_audio_callback(AudioCallback callback) { audio_callback_ = std::move(callback); }
    double audio_sample_rate() const;
    double frames_per_second() const;

    // Only the program playing the game knows what restarting one means.
    void reset() { link_.request_reset(); }

    // Both players' pads, already merged, on their way to whatever is playing
    // the game. Translated on the way out: what crosses the link is a
    // GamePad's controls rather than this app's own shape. See ExternalInput.
    void submit_input(const DsInputState& state);

private:
    struct Screen {
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;
        uint64_t uploaded_serial = 0;
        Texture texture;
    };

    Screen& screen_for(ScreenId screen) { return screen == ScreenId::Top ? top_ : bottom_; }
    const Screen& screen_for(ScreenId screen) const {
        return screen == ScreenId::Top ? top_ : bottom_;
    }

    SystemId system_;
    ExternalFrameLink link_;
    ScreenTextures textures_;
    Screen top_;
    Screen bottom_;
    AudioCallback audio_callback_;
    std::vector<int16_t> audio_[2];
    bool ended_ = false;
    std::string ended_reason_;
};

}  // namespace ds
