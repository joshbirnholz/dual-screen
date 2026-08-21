// Exercises the libretro hosting layer against the mock core: option forcing,
// geometry, video capture, button mapping and touch-to-pointer conversion.
//
// Most of it needs no GL context. The 3DS core does: it renders on the GPU, the
// way Citra does, so that test builds a real context and skips where there is
// none.

#include "test_support.hpp"

#include <SDL.h>

#include "render/gl_loader.hpp"
#include "render/texture.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "config/paths.hpp"
#include "core/libretro_core.hpp"
#include "core/libretro_screen_source.hpp"
#include "input/ds_input.hpp"

using namespace ds;

namespace {

std::string mock_core_path() {
    const char* from_environment = std::getenv("DUALSCREEN_MOCK_CORE");
    return from_environment != nullptr ? std::string(from_environment) : std::string();
}

std::string make_fake_rom() {
    char pattern[] = "/tmp/dualscreen-rom-XXXXXX";
    const char* directory = mkdtemp(pattern);
    if (directory == nullptr) return std::string();
    const std::string path = path_join(directory, "fake.nds");
    std::ofstream file(path, std::ios::binary);
    file << "not a real rom, the mock core does not look";
    return path;
}

void test_core_loads_and_runs() {
    const std::string core_path = mock_core_path();
    CHECK_MSG(!core_path.empty(), "DUALSCREEN_MOCK_CORE is not set");
    if (core_path.empty()) return;

    LibretroCore core;
    DsInputState input;
    core.set_input_state(&input);

    // The same options the real source pins.
    core.force_option("melonds_number_of_screen_layouts", "1");
    core.force_option("melonds_screen_layout1", "top-bottom");
    core.force_option("melonds_screen_gap", "0");
    core.force_option("melonds_render_mode", "software");
    core.force_option("melonds_touch_mode", "touch");
    core.force_option("melonds_show_cursor", "disabled");
    // ...plus one that does not exist, to prove the validation actually fires.
    core.force_option("melonds_not_a_real_option", "whatever");

    std::string error;
    CHECK_MSG(core.load(core_path, &error), error);
    CHECK_STR_EQ(core.core_name(), "Mock DS");

    // Every real option was accepted; only the invented one is complained about.
    CHECK_EQ(core.option_warnings().size(), static_cast<std::size_t>(1));
    if (!core.option_warnings().empty()) {
        CHECK(core.option_warnings()[0].find("melonds_not_a_real_option") != std::string::npos);
    }
    CHECK_STR_EQ(core.option_value("melonds_screen_layout1"), "top-bottom");
    // An option we did not force keeps the core's own default.
    CHECK(core.declared_options().count("melonds_touch_mode") == 1);

    std::size_t audio_frames = 0;
    core.set_audio_callback([&audio_frames](const int16_t*, std::size_t frames) {
        audio_frames += frames;
    });

    const std::string rom = make_fake_rom();
    CHECK(!rom.empty());
    CHECK_MSG(core.load_game(rom, &error), error);

    CHECK_EQ(core.av_info().base_width, 256u);
    CHECK_EQ(core.av_info().base_height, 384u);
    CHECK(core.av_info().fps > 59.0 && core.av_info().fps < 60.0);

    // The circle pad has to reach the core. A 3DS is played with it, and a DS
    // core simply ignores it, so it is always sent.
    input.circle_x = -20000;
    input.circle_y = 15000;
    // Same for the right stick, standing in for a Circle Pad Pro or New
    // 3DS's C-Stick.
    input.c_stick_x = 9000;
    input.c_stick_y = -4000;
    core.run_frame();
    bool saw_circle = false;
    bool saw_c_stick = false;
    for (const std::string& message : core.core_messages()) {
        if (message == "circle -20000,15000") saw_circle = true;
        if (message == "c_stick 9000,-4000") saw_c_stick = true;
    }
    CHECK_MSG(saw_circle, "the circle pad never reached the core");
    CHECK_MSG(saw_c_stick, "the C-Stick never reached the core");

    const CoreVideoFrame& frame = core.video_frame();
    CHECK(frame.valid());
    CHECK_EQ(frame.width, 256u);
    CHECK_EQ(frame.height, 384u);
    CHECK(audio_frames > 0);

    // The mock paints a magenta band across the top screen if the frontend
    // failed to pin the layout option, so a plain pixel check catches that.
    bool magenta_band = false;
    for (unsigned x = 0; x < frame.width; ++x) {
        const uint32_t pixel = frame.pixels[70u * frame.stride_pixels + x] & 0x00ffffffu;
        if (pixel == 0x00ff00ffu) {
            magenta_band = true;
            break;
        }
    }
    CHECK_MSG(!magenta_band, "the core reported that its layout options were not honoured");

    // The two halves must not be identical, or the split would be untestable.
    bool halves_differ = false;
    for (unsigned x = 0; x < frame.width && !halves_differ; ++x) {
        const uint32_t top = frame.pixels[10u * frame.stride_pixels + x];
        const uint32_t bottom = frame.pixels[(192u + 10u) * frame.stride_pixels + x];
        halves_differ = (top != bottom);
    }
    CHECK(halves_differ);

    core.unload();
}

void test_screen_source_reports_a_valid_split() {
    const std::string core_path = mock_core_path();
    if (core_path.empty()) return;

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.core_path = core_path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";

    std::string error;
    CHECK_MSG(source.open(config, &error), error);
    CHECK(source.split().valid);
    CHECK_EQ(source.split().top_width, 256);
    CHECK_EQ(source.split().top_height, 192);
    CHECK_EQ(source.split().bottom_width, 256);
    CHECK_EQ(source.split().bottom_height, 192);
    CHECK_EQ(source.screens_geometry().top.width, 256);
    CHECK_EQ(source.screens_geometry().bottom.height, 192);
    CHECK(source.warnings().empty());

    CHECK_MSG(source.advance_frame(&error), error);
    source.close();
}

// A window and an OpenGL context, for the tests that need one. A 3DS core
// renders on the GPU, so without a real context there is nothing for it to draw
// into and it will not load a game at all -- which is exactly what the frontend
// should do, and exactly why this cannot be tested against a dummy driver.
// Reads one pixel back out of a texture, which is the only way to check what
// the GPU actually put there.
bool read_texture_pixel(const Texture& texture, int x, int y, uint8_t out_rgba[4]) {
    if (!texture.valid() || gl::GenFramebuffers == nullptr) return false;
    GLuint framebuffer = 0;
    gl::GenFramebuffers(1, &framebuffer);
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.id(), 0);
    const bool complete = gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        gl::ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::DeleteFramebuffers(1, &framebuffer);
    return complete;
}

class GlFixture {
public:
    bool start() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
        initialized_ = true;
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        window_ = SDL_CreateWindow("test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (window_ == nullptr) return false;
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) return false;
        SDL_GL_MakeCurrent(window_, context_);
        std::string error;
        return gl::load(false, &error) && gl::at_least(3, 3);
    }

    ~GlFixture() {
        if (context_ != nullptr) {
            gl::unload();
            SDL_GL_DeleteContext(context_);
        }
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        if (initialized_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    bool initialized_ = false;
};

// The same frontend against a core shaped like a 3DS: two screens of different
// widths, the narrower one centred under the wider, drawn on the GPU rather
// than handed over as pixels. Every step from the core option through the
// hardware-render handshake and the crop to the reported geometry has to follow
// the console rather than a remembered 256x192.
void test_screen_source_handles_a_3ds_core() {
    const char* path = std::getenv("DUALSCREEN_MOCK_CORE_3DS");
    if (path == nullptr || path[0] == '\0') return;

    GlFixture gl_fixture;
    if (!gl_fixture.start()) {
        // No GL here at all, so there is nothing this test can say. It runs
        // under Xvfb in CI; skipping is better than a failure that means
        // "this machine has no display".
        test::skip("no OpenGL 3.3 context available for the 3DS core");
        return;
    }

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.system = SystemId::ThreeDs;
    config.core_path = path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";

    std::string error;
    CHECK_MSG(source.open(config, &error), error);
    CHECK_MSG(source.split().valid, "the 3DS framebuffer did not split");
    CHECK_EQ(source.split().top_width, 400);
    CHECK_EQ(source.split().top_height, 240);
    CHECK_EQ(source.split().bottom_width, 320);
    CHECK_EQ(source.split().bottom_height, 240);
    CHECK_EQ(source.split().bottom_x, 40);
    CHECK_EQ(source.split().bottom_y, 240);
    CHECK_EQ(source.screens_geometry().top.width, 400);
    CHECK_EQ(source.screens_geometry().bottom.width, 320);
    // The core reports whether the layout option actually reached it, and it
    // paints a loud magenta band across the upper screen when it did not.
    CHECK_MSG(source.warnings().empty(), "the 3DS core refused an option we pin");

    CHECK_MSG(source.advance_frame(&error), error);

    // The frame is a texture, not pixels: the core drew it into the framebuffer
    // the frontend handed over. Anything still reaching for frame.pixels here
    // would be reading a null pointer.
    const CoreVideoFrame& frame = source.core_frame_for_test();
    CHECK_MSG(frame.source == FrameSource::Texture, "the 3DS core did not render on the GPU");
    CHECK_MSG(frame.valid(), "the GPU frame did not describe a usable texture");
    CHECK_MSG(frame.pixels == nullptr, "a GPU frame should carry no CPU pixels");
    CHECK_MSG(frame.bottom_left_origin, "the core's origin convention was not carried through");
    CHECK_EQ(static_cast<int>(frame.width), 400);
    CHECK_EQ(static_cast<int>(frame.height), 480);

    // And the crop really lands on the right part of it: the mock draws the
    // upper screen in blues and the touch screen in greens, so reading the two
    // screen textures back tells us the split and the vertical flip are right.
    const ScreenTextures& screens = source.screens();
    CHECK_MSG(screens.top != nullptr && screens.bottom != nullptr,
              "the GPU frame did not become two screen textures");
    if (screens.top != nullptr && screens.bottom != nullptr) {
        CHECK_EQ(screens.top->width(), 400);
        CHECK_EQ(screens.bottom->width(), 320);
        uint8_t top_rgba[4] = {0, 0, 0, 0};
        uint8_t bottom_rgba[4] = {0, 0, 0, 0};
        CHECK(read_texture_pixel(*screens.top, 200, 120, top_rgba));
        CHECK(read_texture_pixel(*screens.bottom, 160, 120, bottom_rgba));
        CHECK_MSG(top_rgba[2] > top_rgba[1], "the upper screen was not where it should be");
        CHECK_MSG(bottom_rgba[1] > bottom_rgba[2], "the touch screen was not where it should be");
    }
    source.close();
}

// Rendering at 2x changes how much the core draws and nothing else. The two
// screens have to come out of the larger buffer whole, in the same places, and
// the geometry the layouts are built from has to stay the console's own -- a
// render scale that moved the screens around would be a different feature.
void test_render_scale_only_changes_the_resolution() {
    const char* path = std::getenv("DUALSCREEN_MOCK_CORE_3DS");
    if (path == nullptr || path[0] == '\0') return;

    GlFixture gl_fixture;
    if (!gl_fixture.start()) {
        test::skip("no OpenGL 3.3 context available for the 3DS core");
        return;
    }

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.system = SystemId::ThreeDs;
    config.core_path = path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";
    config.render_scale = 2;

    std::string error;
    CHECK_MSG(source.open(config, &error), error);
    CHECK_MSG(source.warnings().empty(), "the core refused the resolution we asked for");
    CHECK_EQ(source.render_scale(), 2);
    CHECK_MSG(source.advance_frame(&error), error);

    const CoreVideoFrame& frame = source.core_frame_for_test();
    CHECK_EQ(static_cast<int>(frame.width), 800);
    CHECK_EQ(static_cast<int>(frame.height), 960);
    CHECK_EQ(source.split().top_width, 800);
    CHECK_EQ(source.split().top_height, 480);
    CHECK_EQ(source.split().bottom_width, 640);
    CHECK_EQ(source.split().bottom_y, 480);
    // Still centred, at the larger size: the narrower screen sits 80 pixels in
    // rather than 40.
    CHECK_EQ(source.split().bottom_x, 80);
    // The part every layout is built from, which must not have moved.
    CHECK_EQ(source.screens_geometry().top.width, 400);
    CHECK_EQ(source.screens_geometry().top.height, 240);
    CHECK_EQ(source.screens_geometry().bottom.width, 320);

    const ScreenTextures& screens = source.screens();
    CHECK_MSG(screens.top != nullptr && screens.bottom != nullptr,
              "the upscaled frame did not become two screen textures");
    if (screens.top != nullptr && screens.bottom != nullptr) {
        CHECK_EQ(screens.top->width(), 800);
        CHECK_EQ(screens.top->height(), 480);
        CHECK_EQ(screens.bottom->width(), 640);
        // The same two screens, still the right way up and not swapped: the
        // crop followed the scale instead of reading the old rectangles.
        uint8_t top_rgba[4] = {0, 0, 0, 0};
        uint8_t bottom_rgba[4] = {0, 0, 0, 0};
        CHECK(read_texture_pixel(*screens.top, 400, 240, top_rgba));
        CHECK(read_texture_pixel(*screens.bottom, 320, 240, bottom_rgba));
        CHECK_MSG(top_rgba[2] > top_rgba[1], "the upper screen was not where it should be at 2x");
        CHECK_MSG(bottom_rgba[1] > bottom_rgba[2], "the touch screen was not where it should be at 2x");
    }

    // Changed while the game runs, which is the only way the menu can change
    // it. The core picks the option up on its next frame and resizes itself.
    source.set_render_scale(1);
    CHECK_EQ(source.render_scale(), 1);
    CHECK_MSG(source.advance_frame(&error), error);
    CHECK_EQ(static_cast<int>(source.core_frame_for_test().width), 400);
    CHECK_EQ(source.split().top_width, 400);
    CHECK_EQ(source.split().bottom_x, 40);
    CHECK_EQ(source.screens_geometry().top.width, 400);

    // More than this system offers is clamped rather than sent to the core,
    // which would reject a value it never declared.
    source.set_render_scale(99);
    CHECK_EQ(source.render_scale(), system_profile(SystemId::ThreeDs).max_render_scale);
    CHECK_MSG(source.advance_frame(&error), error);
    CHECK_MSG(source.warnings().empty(), "a clamped render scale still reached the core");
    source.close();
}

// read_screen()'s target size is a hint, not an instruction: the picture it
// hands back is never sharper than what the core actually rendered, and never
// softer than the console's own resolution, whatever the caller asks for.
void test_read_screen_bounds_the_target_size() {
    const char* path = std::getenv("DUALSCREEN_MOCK_CORE_3DS");
    if (path == nullptr || path[0] == '\0') return;

    GlFixture gl_fixture;
    if (!gl_fixture.start()) {
        test::skip("no OpenGL 3.3 context available for the 3DS core");
        return;
    }

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.system = SystemId::ThreeDs;
    config.core_path = path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";
    config.render_scale = 2;  // rendered at 800x480 / 640x480

    std::string error;
    CHECK_MSG(source.open(config, &error), error);
    CHECK_MSG(source.advance_frame(&error), error);

    ScreenReadback top;

    // No hint at all -- the default every caller got before this existed --
    // is still the console's own resolution, not the rendered one. A caller
    // that does not know better should not accidentally start sending more.
    CHECK(source.read_screen(ScreenId::Top, top));
    CHECK_EQ(top.width, 400);
    CHECK_EQ(top.height, 240);

    // Between native and rendered: honoured exactly. This is the ordinary
    // case -- a companion whose window makes this screen some size in between.
    CHECK(source.read_screen(ScreenId::Top, top, 600, 360));
    CHECK_EQ(top.width, 600);
    CHECK_EQ(top.height, 360);

    // More than was ever rendered: there is nothing sharper to send, so this
    // clamps down to what 2x actually produced rather than upscaling into a
    // bigger, blurrier picture that costs more to send for no more detail.
    CHECK(source.read_screen(ScreenId::Top, top, 4000, 4000));
    CHECK_EQ(top.width, 800);
    CHECK_EQ(top.height, 480);

    // Smaller than the console's own resolution: clamped up rather than down,
    // since a picture duller than native stops being worth looking at, and
    // every other part of this app already treats native as the floor.
    CHECK(source.read_screen(ScreenId::Top, top, 10, 10));
    CHECK_EQ(top.width, 400);
    CHECK_EQ(top.height, 240);

    // And the picture itself is still correct at an arbitrary in-between size
    // -- not just the right dimensions but the right screen, right way up.
    CHECK(source.read_screen(ScreenId::Top, top, 600, 360));
    ScreenReadback bottom;
    CHECK(source.read_screen(ScreenId::Bottom, bottom, 480, 360));
    CHECK_EQ(bottom.width, 480);
    CHECK_EQ(bottom.height, 360);
    const uint32_t top_pixel = top.pixels[(top.height / 2) * top.stride_pixels + top.width / 2];
    const uint32_t bottom_pixel =
        bottom.pixels[(bottom.height / 2) * bottom.stride_pixels + bottom.width / 2];
    // A GPU readback comes back R,G,B,A off the card rather than XRGB8888, and
    // is tagged as such rather than paying to swizzle it -- see ChannelOrder.
    CHECK(top.channel_order == ChannelOrder::Rgba);
    CHECK(bottom.channel_order == ChannelOrder::Rgba);
    // Red in the low byte, green next, blue the one above that.
    CHECK_MSG(((top_pixel >> 16) & 0xff) > (top_pixel & 0xff) &&
                  ((top_pixel >> 16) & 0xff) > ((top_pixel >> 8) & 0xff),
              "the upper screen was not where it should be at an arbitrary size");
    CHECK_MSG(((bottom_pixel >> 8) & 0xff) > (bottom_pixel & 0xff) &&
                  ((bottom_pixel >> 8) & 0xff) > ((bottom_pixel >> 16) & 0xff),
              "the touch screen was not where it should be at an arbitrary size");

    source.close();
}

// melonDS is pinned to its software renderer, so there is no scale to set and
// asking for one changes nothing.
void test_render_scale_is_ignored_where_there_is_none() {
    const std::string core_path = mock_core_path();
    if (core_path.empty()) return;

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.core_path = core_path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";
    config.render_scale = 2;

    std::string error;
    CHECK_MSG(source.open(config, &error), error);
    CHECK_EQ(source.render_scale(), 1);
    CHECK_MSG(source.advance_frame(&error), error);
    CHECK_EQ(source.split().top_width, 256);

    // A software-rendered core has nothing above native to offer, so a target
    // size hint has nothing to act on: read_screen() hands back the same
    // view into the core's own framebuffer regardless of what is asked for.
    ScreenReadback top;
    CHECK(source.read_screen(ScreenId::Top, top, 4000, 4000));
    CHECK_EQ(top.width, 256);
    CHECK_EQ(top.height, 192);

    source.set_render_scale(3);
    CHECK_EQ(source.render_scale(), 1);
    source.close();
}

void test_screen_source_reports_missing_files_clearly() {
    LibretroCoreScreenSource source;
    std::string error;

    LibretroSourceConfig no_core;
    no_core.rom_path = "/tmp/whatever.nds";
    CHECK(!source.open(no_core, &error));
    CHECK(error.find("core") != std::string::npos);

    LibretroSourceConfig bad_core;
    bad_core.core_path = "/tmp/definitely-not-here.so";
    bad_core.rom_path = "/tmp/whatever.nds";
    CHECK(!source.open(bad_core, &error));
    CHECK(error.find("does not exist") != std::string::npos);

    const std::string core_path = mock_core_path();
    if (!core_path.empty()) {
        LibretroSourceConfig missing_rom;
        missing_rom.core_path = core_path;
        missing_rom.rom_path = "/tmp/definitely-not-here.nds";
        CHECK(!source.open(missing_rom, &error));
        CHECK(error.find("ROM") != std::string::npos);
    }
}

// Saves are the frontend's job in libretro: the core never touches the
// filesystem for cartridge SRAM. This covers the whole contract.
void test_save_ram_round_trip() {
    const std::string core_path = mock_core_path();
    if (core_path.empty()) return;

    const std::string rom = make_fake_rom();
    const std::string save_path = path_dirname(rom) + "/save.srm";
    std::string error;

    // First session: no save file exists, so the game starts fresh and the
    // frontend writes one out when the core dirties the buffer.
    {
        LibretroCoreScreenSource source;
        LibretroSourceConfig config;
        config.core_path = core_path;
        config.rom_path = rom;
        config.system_directory = "/tmp";
        config.save_directory = "/tmp";
        config.save_path = save_path;

        CHECK_MSG(source.open(config, &error), error);
        CHECK_MSG(source.has_save_ram(), "the core reported no save RAM");
        CHECK(!file_exists(save_path));

        // Nothing has changed yet, so there is nothing to write.
        CHECK(!source.flush_save_ram(false));

        CHECK_MSG(source.advance_frame(&error), error);
        CHECK_MSG(source.flush_save_ram(false), "a changed save buffer was not written");
        CHECK(file_exists(save_path));

        // A second flush with nothing changed in between must be a no-op.
        CHECK(!source.flush_save_ram(false));
        // ...but a forced one always writes.
        CHECK(source.flush_save_ram(true));

        source.close();
    }

    std::vector<uint8_t> written;
    CHECK(read_file(save_path, written, &error));
    CHECK_EQ(written.size(), static_cast<std::size_t>(1024));
    // The mock stamps this magic as it runs.
    CHECK(written.size() >= 4 && written[0] == 'D' && written[1] == 'S' && written[2] == 'S' &&
          written[3] == 'V');

    // Second session: the frontend must load that file into the core's buffer
    // *before* the first frame, which is what the mock checks for.
    {
        LibretroCoreScreenSource source;
        LibretroSourceConfig config;
        config.core_path = core_path;
        config.rom_path = rom;
        config.system_directory = "/tmp";
        config.save_directory = "/tmp";
        config.save_path = save_path;

        CHECK_MSG(source.open(config, &error), error);
        CHECK_MSG(source.advance_frame(&error), error);

        // The mock paints an orange band across the top screen when it found
        // the magic in its save RAM on the first frame.
        const CoreVideoFrame& frame = source.core_frame_for_test();
        bool found_marker = false;
        for (unsigned x = 0; x < frame.width && !found_marker; ++x) {
            const uint32_t pixel = frame.pixels[80u * frame.stride_pixels + x] & 0x00ffffffu;
            found_marker = (pixel == 0x00ff8c00u);
        }
        CHECK_MSG(found_marker, "the core did not see its save data on the first frame");
        source.close();
    }
}

void test_save_ram_is_skipped_without_a_path() {
    const std::string core_path = mock_core_path();
    if (core_path.empty()) return;

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.core_path = core_path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";
    // No save_path: saving is off, and nothing should blow up.
    std::string error;
    CHECK_MSG(source.open(config, &error), error);
    CHECK(!source.has_save_ram());
    CHECK(!source.flush_save_ram(true));
    source.close();
}

}  // namespace

int main() {
    test_core_loads_and_runs();
    test_screen_source_reports_a_valid_split();
    test_screen_source_handles_a_3ds_core();
    test_render_scale_only_changes_the_resolution();
    test_read_screen_bounds_the_target_size();
    test_render_scale_is_ignored_where_there_is_none();
    test_screen_source_reports_missing_files_clearly();
    test_save_ram_round_trip();
    test_save_ram_is_skipped_without_a_path();
    return test::finish("core-host");
}
