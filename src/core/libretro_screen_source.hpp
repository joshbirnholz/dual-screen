#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/core_input_sink.hpp"
#include "core/libretro_core.hpp"
#include "core/screen_source.hpp"
#include "layout/layout.hpp"

namespace ds {

// Where the two screens sit inside the core's single output framebuffer, in
// framebuffer pixels. Derived from the frame's own dimensions rather than
// hardcoded, so it stays correct if the core is ever upscaled.
struct ScreenSplit {
    int frame_width = 0;
    int frame_height = 0;
    int screen_width = 0;
    int screen_height = 0;
    int top_x = 0, top_y = 0;
    int bottom_x = 0, bottom_y = 0;
    bool valid = false;
};

// Computes the split for a core framebuffer that is one DS screen wide and two
// tall (the core's "top-bottom" internal layout with no screen gap). Returns an
// invalid split, and an explanation, if the geometry is not that shape.
ScreenSplit compute_top_bottom_split(int frame_width, int frame_height, std::string* reason);

struct LibretroSourceConfig {
    std::string core_path;
    std::string rom_path;
    std::string system_directory;
    std::string save_directory;
};

// The V1 ScreenSource: hosts the melonDS DS libretro core, pins it to a single
// known internal screen layout, and hands out the two screens as separate
// textures every frame.
class LibretroCoreScreenSource final : public ScreenSource {
public:
    using AudioCallback = LibretroCore::AudioCallback;

    LibretroCoreScreenSource();
    ~LibretroCoreScreenSource() override;

    // Loads the core and the ROM. Returns false with a readable reason.
    bool open(const LibretroSourceConfig& config, std::string* error);
    void close();

    void set_audio_callback(AudioCallback callback);
    // The sink whose latched state the core reads each frame.
    CoreInputSink& input_sink() { return input_sink_; }

    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    ScreenSize native_screen_size() const override;
    bool wants_shutdown() const override;

    double frames_per_second() const;
    double audio_sample_rate() const;
    const ScreenSplit& split() const { return split_; }
    // Non-fatal complaints worth showing the player (e.g. a core option this
    // build expected but the loaded core does not have).
    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::string& core_description() const { return core_description_; }

private:
    void refresh_split_from_frame();
    void upload_screens();

    std::unique_ptr<LibretroCore> core_;
    CoreInputSink input_sink_;
    ScreenSplit split_;
    Texture top_texture_;
    Texture bottom_texture_;
    ScreenTextures screens_;
    uint64_t uploaded_serial_ = 0;
    bool split_reported_ = false;
    std::vector<std::string> warnings_;
    std::string core_description_;
};

}  // namespace ds
