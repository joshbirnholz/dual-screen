#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/core_input_sink.hpp"
#include "core/libretro_core.hpp"
#include "core/screen_source.hpp"
#include "core/system.hpp"
#include "layout/layout.hpp"

namespace ds {

// Where the two screens sit inside the core's single output framebuffer, in
// framebuffer pixels. Derived from the frame's own dimensions rather than
// hardcoded, so it stays correct if the core is ever upscaled -- and the two
// screens are sized separately, because on a 3DS they are different shapes.
struct ScreenSplit {
    int frame_width = 0;
    int frame_height = 0;
    int top_x = 0, top_y = 0, top_width = 0, top_height = 0;
    int bottom_x = 0, bottom_y = 0, bottom_width = 0, bottom_height = 0;
    bool valid = false;
};

// Computes the split for a core framebuffer holding one system's two screens
// stacked with no gap. Both cores are pinned to that arrangement; what differs
// is the shape. Returns an invalid split, and an explanation, if the geometry is
// not what the pinned layout should have produced.
ScreenSplit compute_top_bottom_split(SystemId system,
                                     int frame_width,
                                     int frame_height,
                                     std::string* reason);

struct LibretroSourceConfig {
    SystemId system = SystemId::Ds;
    std::string core_path;
    std::string rom_path;
    std::string system_directory;
    std::string save_directory;
    // Where this game's cartridge save lives. Empty disables saving entirely.
    std::string save_path;
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

    // Writes the cartridge save out if it has changed since the last write.
    // `force` writes regardless, for shutdown. Returns true if a write happened.
    bool flush_save_ram(bool force);
    // True once a save file is being tracked for the loaded game. Games with no
    // cartridge save (and a core showing an error screen) report false.
    bool has_save_ram() const { return !save_shadow_.empty(); }

    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    SystemScreens screens_geometry() const override;
    bool wants_shutdown() const override;

    double frames_per_second() const;
    double audio_sample_rate() const;
    const ScreenSplit& split() const { return split_; }
    // Non-fatal complaints worth showing the player (e.g. a core option this
    // build expected but the loaded core does not have).
    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::string& core_description() const { return core_description_; }
    // The core's raw framebuffer, before the split. Only used by tests, which
    // need to inspect what the core actually drew.
    const CoreVideoFrame& core_frame_for_test() const { return core_->video_frame(); }

private:
    void refresh_split_from_frame();
    void upload_screens();
    // Tells the core where the touch screen sits in its own framebuffer.
    void apply_pointer_rect();
    // Crops one screen out of a GPU-rendered frame into `destination`.
    bool blit_screen(const CoreVideoFrame& frame,
                     Texture& destination,
                     int source_x,
                     int source_y,
                     int width,
                     int height);
    // Reused for every crop: the destination texture is swapped in each time
    // rather than a framebuffer being created per screen per frame.
    GLuint screen_framebuffer_ = 0;
    void probe_core_framebuffer(const CoreVideoFrame& frame);
    void probe_window_framebuffer();
    bool window_probe_done_ = false;
    bool blit_unavailable_reported_ = false;
    bool first_gpu_crop_reported_ = false;
    bool silence_reported_ = false;
    int quiet_frames_ = 0;
    // Says what the framebuffer was cut into, which is the first thing worth
    // knowing when a core turns out not to be arranged the way we asked.
    void log_split();
    void load_save_ram();

    std::unique_ptr<LibretroCore> core_;
    CoreInputSink input_sink_;
    SystemId system_ = SystemId::Ds;
    ScreenSplit split_;
    Texture top_texture_;
    Texture bottom_texture_;
    ScreenTextures screens_;
    uint64_t uploaded_serial_ = 0;
    bool split_reported_ = false;
    std::vector<std::string> warnings_;
    std::string core_description_;

    // Cartridge save. `save_shadow_` is what is currently on disk, so a write
    // only happens when the game has actually changed something.
    std::string save_path_;
    std::vector<uint8_t> save_shadow_;
};

}  // namespace ds
