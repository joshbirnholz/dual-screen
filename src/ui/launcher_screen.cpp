#include "ui/launcher_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/prompt.hpp"

namespace ds {
namespace {

const LauncherScreen* unused_tag = nullptr;

constexpr int kLauncherRowCount = 4;

const char* const kTitles[kLauncherRowCount] = {
    "Load a game",
    "Connect as top screen",
    "Connect as bottom screen",
    "Quit",
};

const char* const kDetails[kLauncherRowCount] = {
    "Play here, on this device",
    "Show the top screen of a game running on another device",
    "Show the bottom screen, and use this device's touchscreen",
    "",
};

LauncherChoice choice_for_row(int row) {
    switch (row) {
        case 0: return LauncherChoice::LoadGame;
        case 1: return LauncherChoice::ConnectAsTopScreen;
        case 2: return LauncherChoice::ConnectAsBottomScreen;
        default: return LauncherChoice::Quit;
    }
}

}  // namespace

std::vector<PixelRect> LauncherScreen::compute_rows(float output_width,
                                                    float output_height,
                                                    float& out_scale) const {
    out_scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(out_scale);
    const float margin = std::round(output_width * 0.12f);
    const float width = output_width - margin * 2.0f;
    const float row_height = line * 3.4f;
    const float top = std::round(output_height * 0.30f);

    std::vector<PixelRect> rows;
    for (int index = 0; index < kLauncherRowCount; ++index) {
        rows.push_back(PixelRect{margin, top + index * (row_height + line * 0.5f), width,
                                 row_height});
    }
    return rows;
}

void LauncherScreen::open() {
    open_ = true;
    focus_ = 0;
}

LauncherResult LauncherScreen::update(const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers,
                                      float output_width,
                                      float output_height) {
    LauncherResult result;
    if (!open_) {
        return result;
    }

    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(output_width, output_height, scale);

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                focus_ = (focus_ + kLauncherRowCount - 1) % kLauncherRowCount;
                break;
            case UiAction::Down:
                focus_ = (focus_ + 1) % kLauncherRowCount;
                break;
            case UiAction::Confirm:
                result.choice = choice_for_row(focus_);
                break;
            case UiAction::Quit:
                result.choice = LauncherChoice::Quit;
                break;
            default:
                break;
        }
    }

    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (rows[index].contains(pointer.x, pointer.y)) {
                focus_ = static_cast<int>(index);
                result.choice = choice_for_row(focus_);
            }
        }
    }

    if (result.choice != LauncherChoice::None) {
        open_ = false;
    }
    return result;
}

void LauncherScreen::render(QuadRenderer& quads,
                            TextRenderer& text,
                            float output_width,
                            float output_height,
                            const MenuButtonLabels& labels,
                            int hosts_found) {
    if (!open_) {
        return;
    }
    (void)unused_tag;

    const UiTheme& theme = ui_theme();
    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(output_width, output_height, scale);
    const float line = TextRenderer::line_height(scale);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    const float margin = rows.empty() ? 0.0f : rows.front().x;
    text.draw(quads, "dualscreen", margin, std::round(output_height * 0.14f), scale * 2.0f,
              theme.text);
    text.draw(quads, "Play a DS game here, or be a screen for one running on another device",
              margin, std::round(output_height * 0.14f) + line * 2.4f, scale * 0.95f,
              theme.text_dim);

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const PixelRect& row = rows[index];
        const bool focused = (static_cast<int>(index) == focus_);
        quads.draw_solid(row, focused ? theme.panel_focused : theme.panel);
        if (focused) {
            quads.draw_outline(row, theme.accent, std::max(1.0f, scale * 0.5f));
        }

        const float pad = line * 0.7f;
        const bool is_quit = (choice_for_row(static_cast<int>(index)) == LauncherChoice::Quit);
        text.draw(quads, kTitles[index], row.x + pad, row.y + pad * 0.8f, scale * 1.2f,
                  is_quit ? theme.danger : theme.text);
        if (kDetails[index][0] != '\0') {
            text.draw(quads, kDetails[index], row.x + pad, row.y + pad * 0.8f + line * 1.5f,
                      scale * 0.9f, theme.text_dim);
        }
    }

    // Live feedback that the search is working, so "connect" is not a leap of
    // faith before anything has been found.
    const std::string found =
        hosts_found > 0
            ? std::to_string(hosts_found) + (hosts_found == 1 ? " device" : " devices") +
                  " running a game found nearby"
            : std::string("Looking for devices running a game...");
    text.draw(quads, found, margin, rows.back().y + rows.back().h + line * 1.2f, scale * 0.9f,
              hosts_found > 0 ? theme.success : theme.text_dim);

    text.draw(quads, format_prompts({{labels.confirm, "Select"}, {"Up/Down", "Move"}}),
              output_width * 0.5f, output_height - line * 2.5f, scale * 0.9f, theme.text_dim,
              TextAlign::Center);
}

// --- HostPickerScreen -------------------------------------------------------

void HostPickerScreen::open(ScreenId wanted_screen) {
    open_ = true;
    focus_ = 0;
    wanted_screen_ = wanted_screen;
}

std::vector<PixelRect> HostPickerScreen::compute_rows(std::size_t count,
                                                      float output_width,
                                                      float output_height,
                                                      float& out_scale) const {
    out_scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(out_scale);
    const float margin = std::round(output_width * 0.10f);
    const float width = output_width - margin * 2.0f;
    const float row_height = line * 3.0f;
    const float top = std::round(output_height * 0.28f);

    std::vector<PixelRect> rows;
    for (std::size_t index = 0; index < count; ++index) {
        rows.push_back(
            PixelRect{margin, top + index * (row_height + line * 0.4f), width, row_height});
    }
    return rows;
}

HostPickerResult HostPickerScreen::update(const std::vector<ds::net::DiscoveredHost>& hosts,
                                          const std::vector<UiAction>& actions,
                                          const std::vector<PointerEvent>& pointers,
                                          float output_width,
                                          float output_height) {
    HostPickerResult result;
    if (!open_) {
        return result;
    }

    const int count = static_cast<int>(hosts.size());
    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(hosts.size(), output_width, output_height, scale);

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (count > 0) focus_ = (focus_ + count - 1) % count;
                break;
            case UiAction::Down:
                if (count > 0) focus_ = (focus_ + 1) % count;
                break;
            case UiAction::Confirm:
                if (focus_ >= 0 && focus_ < count) {
                    result.chosen = true;
                    result.host = hosts[static_cast<std::size_t>(focus_)];
                }
                break;
            case UiAction::Cancel:
                result.cancelled = true;
                break;
            case UiAction::Quit:
                result.cancelled = true;
                break;
            default:
                break;
        }
    }
    focus_ = std::max(0, std::min(focus_, std::max(0, count - 1)));

    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (rows[index].contains(pointer.x, pointer.y)) {
                focus_ = static_cast<int>(index);
                result.chosen = true;
                result.host = hosts[index];
            }
        }
    }

    if (result.chosen || result.cancelled) {
        open_ = false;
    }
    return result;
}

void HostPickerScreen::render(QuadRenderer& quads,
                              TextRenderer& text,
                              float output_width,
                              float output_height,
                              const std::vector<ds::net::DiscoveredHost>& hosts,
                              const MenuButtonLabels& labels,
                              const std::string& status) {
    if (!open_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(hosts.size(), output_width, output_height, scale);
    const float line = TextRenderer::line_height(scale);
    const float margin = std::round(output_width * 0.10f);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    const std::string heading = std::string("Connect as the ") +
                                (wanted_screen_ == ScreenId::Top ? "top" : "bottom") + " screen";
    text.draw(quads, heading, margin, std::round(output_height * 0.13f), scale * 1.7f, theme.text);
    if (!status.empty()) {
        text.draw(quads, status, margin, std::round(output_height * 0.13f) + line * 2.2f,
                  scale * 0.95f, theme.text_dim);
    }

    if (hosts.empty()) {
        text.draw(quads, "No devices found yet. Make sure a game is running on the other device,",
                  margin, std::round(output_height * 0.34f), scale, theme.text_dim);
        text.draw(quads, "and that both are on the same network.", margin,
                  std::round(output_height * 0.34f) + line * 1.4f, scale, theme.text_dim);
    }

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const PixelRect& row = rows[index];
        const ds::net::DiscoveredHost& host = hosts[index];
        const bool focused = (static_cast<int>(index) == focus_);
        const bool available = host.offers(wanted_screen_);

        quads.draw_solid(row, focused ? theme.panel_focused : theme.panel);
        if (focused) {
            quads.draw_outline(row, theme.accent, std::max(1.0f, scale * 0.5f));
        }

        const float pad = line * 0.6f;
        text.draw(quads, host.game_title.empty() ? "(no game)" : host.game_title, row.x + pad,
                  row.y + pad * 0.7f, scale * 1.1f, available ? theme.text : theme.text_dim);
        const std::string detail =
            host.device_name + "   " + host.address +
            (available ? "" : "   this screen is already taken");
        text.draw(quads, detail, row.x + pad, row.y + pad * 0.7f + line * 1.4f, scale * 0.85f,
                  available ? theme.text_dim : theme.warning);
    }

    text.draw(quads,
              format_prompts({{labels.confirm, "Connect"}, {labels.cancel, "Back"}}),
              output_width * 0.5f, output_height - line * 2.5f, scale * 0.9f, theme.text_dim,
              TextAlign::Center);
}

}  // namespace ds
