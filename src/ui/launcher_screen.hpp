#pragma once

#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "net/discovery.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// What the player picked on the opening screen.
enum class LauncherChoice {
    None,
    LoadGame,
    ConnectAsTopScreen,
    ConnectAsBottomScreen,
    Quit,
};

struct LauncherResult {
    LauncherChoice choice = LauncherChoice::None;
};

// The opening screen when no ROM was named: play a game here, or join one
// already running on another device and be one of its screens.
class LauncherScreen {
public:
    void open();
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    LauncherResult update(const std::vector<UiAction>& actions,
                          const std::vector<PointerEvent>& pointers,
                          float output_width,
                          float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                float output_width,
                float output_height,
                const MenuButtonLabels& labels,
                int hosts_found);

private:
    struct Row {
        LauncherChoice choice;
        const char* title;
        const char* detail;
    };

    std::vector<PixelRect> compute_rows(float output_width, float output_height, float& out_scale)
        const;

    bool open_ = false;
    int focus_ = 0;
};

// Picks which host to join, from whatever discovery has found.
struct HostPickerResult {
    bool cancelled = false;
    bool chosen = false;
    ds::net::DiscoveredHost host;
};

class HostPickerScreen {
public:
    void open(ScreenId wanted_screen);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    ScreenId wanted_screen() const { return wanted_screen_; }

    HostPickerResult update(const std::vector<ds::net::DiscoveredHost>& hosts,
                            const std::vector<UiAction>& actions,
                            const std::vector<PointerEvent>& pointers,
                            float output_width,
                            float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                float output_width,
                float output_height,
                const std::vector<ds::net::DiscoveredHost>& hosts,
                const MenuButtonLabels& labels,
                const std::string& status);

private:
    std::vector<PixelRect> compute_rows(std::size_t count,
                                        float output_width,
                                        float output_height,
                                        float& out_scale) const;

    bool open_ = false;
    int focus_ = 0;
    ScreenId wanted_screen_ = ScreenId::Bottom;
};

}  // namespace ds
