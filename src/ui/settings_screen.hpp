#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "config/settings.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// One BIOS/firmware file the core may want, and whether it is actually there.
struct BiosFileStatus {
    std::string filename;
    bool required = false;
    bool present = false;
    std::string note;
};

// Checks the melonDS DS BIOS/firmware files in `system_dir`. Nothing
// Nintendo-owned ships with this app — these have to be the player's own dumps,
// so the app's job is to say clearly which ones it can and cannot see.
std::vector<BiosFileStatus> check_bios_files(const std::string& system_dir);

struct SettingsScreenResult {
    bool close_requested = false;
    bool settings_changed = false;
};

// Shows where the app is looking for the core and the BIOS files, and lets the
// few settings that can be changed without a keyboard be changed with a pad.
// Paths themselves are edited in settings.json; this screen cycles between
// directories that actually exist, which is what a controller can do usefully.
class SettingsScreen {
public:
    SettingsScreen(const LayoutRegistry& registry, const AppPaths& paths)
        : registry_(&registry), paths_(paths) {}

    void open(const Settings& settings, const std::string& resolved_core_path);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    const Settings& settings() const { return settings_; }

    SettingsScreenResult update(const std::vector<UiAction>& actions,
                                const std::vector<PointerEvent>& pointers,
                                float output_width,
                                float output_height);

    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height);

private:
    enum class Row { SystemDir, CorePath, DefaultLayout, StreamAudio, Vsync, Back, Count };
    static constexpr int kRowCount = static_cast<int>(Row::Count);

    struct Geometry {
        std::vector<PixelRect> rows;
        PixelRect panel;
        PixelRect bios_panel;
        float scale = 1.0f;
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    void adjust(int direction, SettingsScreenResult& result);
    void refresh_bios();

    const LayoutRegistry* registry_;
    AppPaths paths_;
    Settings settings_;
    std::string resolved_core_path_;
    std::vector<std::string> system_dir_candidates_;
    std::vector<BiosFileStatus> bios_;
    int focus_ = 0;
    bool open_ = false;
};

}  // namespace ds
