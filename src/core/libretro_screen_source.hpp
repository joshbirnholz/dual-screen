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
    // Internal resolution multiplier the core renders at, clamped to what the
    // system offers. Set before the core loads so the first frame is already
    // the right size.
    int render_scale = 1;
    // A Rumble Pak in the DS's GBA slot, where the core offers one -- see
    // LibretroCoreScreenSource::offers_rumble_pak().
    bool rumble_pak = false;
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
    void set_rumble_callback(LibretroCore::RumbleCallback callback) {
        core_->set_rumble_callback(std::move(callback));
    }
    // Whether the loaded core can put a Rumble Pak in a DS's GBA slot, which
    // is how a DS game rumbles at all. Found by looking through the options
    // the core declares for a slot device that is one, rather than by name:
    // the name is the core's business, and a guess that turned out wrong
    // would offer a switch that does nothing.
    bool offers_rumble_pak() const { return !rumble_key_.empty(); }
    bool rumble_pak() const { return rumble_pak_; }
    // In or out. The core takes it straight away; a game that only looks for
    // one as it boots sees it after Reset Game.
    void set_rumble_pak(bool inserted);
    // Changes the core's internal resolution while it runs. The core answers
    // with a new geometry and the split follows it, so nothing else has to be
    // told. A system with no scale to choose ignores this.
    void set_render_scale(int scale);
    int render_scale() const { return render_scale_; }
    // Frames to run ahead of the game's own input lag -- see
    // LibretroCore::set_run_ahead(). Safe to change while the game runs.
    void set_run_ahead(unsigned frames) {
        if (core_) core_->set_run_ahead(frames);
    }
    // Restarts the running game from power-on, the way a console's own reset
    // button would. Save RAM is untouched -- this restarts execution, not the
    // cartridge's saved progress.
    void reset();
    // The sink whose latched state the core reads each frame.
    CoreInputSink& input_sink() { return input_sink_; }

    // Writes the cartridge save out if it has changed since the last write.
    // `force` writes regardless, for shutdown. Returns true if a write happened.
    bool flush_save_ram(bool force);
    // True once a save file is being tracked for the loaded game. Games with no
    // cartridge save (and a core showing an error screen) report false.
    bool has_save_ram() const { return !save_shadow_.empty(); }

    // Save states, where the core can make them -- see LibretroCore. The
    // errors are sentences for the player. supports_save_states() is cheap
    // enough to ask every frame.
    bool supports_save_states() const;
    bool save_state(std::vector<uint8_t>& out, std::string* error);
    bool load_state(const std::vector<uint8_t>& state, std::string* error);

    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    bool read_screen(ScreenId screen,
                     ScreenReadback& out,
                     int target_width = 0,
                     int target_height = 0) override;
    SystemScreens screens_geometry() const override;
    uint64_t frame_serial() const override;
    bool wants_shutdown() const override;
    void send_key_event(bool down, unsigned retro_keycode, uint32_t character,
                        uint16_t modifiers) override;

    double frames_per_second() const;
    double audio_sample_rate() const;
    const ScreenSplit& split() const { return split_; }
    // Non-fatal complaints worth showing the player (e.g. a core option this
    // build expected but the loaded core does not have). Only what loading
    // the core found: a core's own passing notices go to the log.
    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::string& core_description() const { return core_description_; }
    // The core's raw framebuffer, before the split. Only used by tests, which
    // need to inspect what the core actually drew.
    const CoreVideoFrame& core_frame_for_test() const { return core_->video_frame(); }
    // What the core has been told for one of its options.
    std::string core_option_for_test(const std::string& key) const {
        return core_->option_value(key);
    }

private:
    void refresh_split_from_frame();
    void upload_screens();
    // Tells the core where the touch screen sits in its own framebuffer.
    void apply_pointer_rect();
    // Pins the core's resolution option to render_scale_, in whichever core's
    // language this system speaks.
    void apply_render_scale();
    // Crops one screen out of a GPU-rendered frame into `destination`. A
    // destination size of zero means the source's own, which is the usual case;
    // a smaller one scales on the way, which is how a core rendering above the
    // console's resolution is read back at the console's resolution.
    bool blit_screen(const CoreVideoFrame& frame,
                     Texture& destination,
                     int source_x,
                     int source_y,
                     int width,
                     int height,
                     int destination_width = 0,
                     int destination_height = 0);
    // Copies one screen off the GPU into `readbacks_`, at `target_width` x
    // `target_height` (already clamped to [native, rendered] by the caller).
    // Only what is being sent somewhere is ever asked for.
    bool read_screen_from_gpu(ScreenId screen,
                              const CoreVideoFrame& frame,
                              ScreenReadback& out,
                              int target_width,
                              int target_height);
    // Reused for every crop: the destination texture is swapped in each time
    // rather than a framebuffer being created per screen per frame.
    GLuint screen_framebuffer_ = 0;
    // One staging texture and one CPU buffer per screen, for reading a
    // GPU-rendered screen back out. Kept alive between frames: a session sends
    // one of these sixty times a second and reallocating each time would be
    // pure waste. Cached by frame serial so two callers in one frame read the
    // card once.
    struct Readback {
        Texture texture;
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;
        bool filled = false;
    };
    Readback readbacks_[2];
    GLuint readback_framebuffer_ = 0;
    bool readback_failure_reported_ = false;
    void probe_core_framebuffer(const CoreVideoFrame& frame);
    void probe_window_framebuffer();
    bool window_probe_done_ = false;
    bool blit_unavailable_reported_ = false;
    bool first_gpu_crop_reported_ = false;
    bool silence_reported_ = false;
    // Frames the core has run since it last presented a new picture. Counted
    // where frames are run, not where screens are drawn: a game paused under
    // a menu is drawn a hundred times over without running once, and that is
    // not a core that has stopped presenting.
    int quiet_frames_ = 0;
    uint64_t last_presented_serial_ = 0;
    // Says what the framebuffer was cut into, which is the first thing worth
    // knowing when a core turns out not to be arranged the way we asked.
    void log_split();
    void load_save_ram();

    std::unique_ptr<LibretroCore> core_;
    CoreInputSink input_sink_;
    SystemId system_ = SystemId::Ds;
    int render_scale_ = 1;
    // What the loaded core calls the settings found by looking -- see
    // find_core_options(). Empty where the core has no such thing.
    std::string rumble_key_;
    std::string rumble_on_value_;
    std::string rumble_off_value_;
    bool rumble_pak_ = false;
    void find_core_options();
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
