// Exercises the libretro hosting layer against the mock core: option forcing,
// geometry, video capture, button mapping and touch-to-pointer conversion.
// No GL context is involved, so this runs anywhere.

#include "test_support.hpp"

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

    core.run_frame();
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

// The same frontend against a core shaped like a 3DS: two screens of different
// widths, the narrower one centred under the wider. Every step from the core
// option through the crop to the reported geometry has to follow the console
// rather than a remembered 256x192.
void test_screen_source_handles_a_3ds_core() {
    const char* path = std::getenv("DUALSCREEN_MOCK_CORE_3DS");
    if (path == nullptr || path[0] == '\0') return;

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

    // The crop really does take the touch screen from where the core put it:
    // the mock draws it in greens, and the upper screen in blues.
    const CoreVideoFrame& frame = source.core_frame_for_test();
    CHECK(frame.valid());
    if (frame.valid()) {
        const ScreenSplit& split = source.split();
        const uint32_t top_pixel =
            frame.pixels[(static_cast<std::size_t>(split.top_y) + 100) * frame.stride_pixels +
                         split.top_x + 100];
        const uint32_t bottom_pixel =
            frame.pixels[(static_cast<std::size_t>(split.bottom_y) + 100) * frame.stride_pixels +
                         split.bottom_x + 100];
        const auto blue = [](uint32_t p) { return p & 0xffu; };
        const auto green = [](uint32_t p) { return (p >> 8) & 0xffu; };
        CHECK_MSG(blue(top_pixel) > green(top_pixel), "the upper screen was not where it should be");
        CHECK_MSG(green(bottom_pixel) > blue(bottom_pixel),
                  "the touch screen was not where it should be");
    }
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
    test_screen_source_reports_missing_files_clearly();
    test_save_ram_round_trip();
    test_save_ram_is_skipped_without_a_path();
    return test::finish("core-host");
}
