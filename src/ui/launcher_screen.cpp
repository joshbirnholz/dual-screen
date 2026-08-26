#include "ui/launcher_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/control_guide.hpp"

namespace ds {
namespace {

const LauncherScreen* unused_tag = nullptr;

const char* title_for(LauncherChoice choice) {
    switch (choice) {
        case LauncherChoice::LoadGame: return "Load a game";
        case LauncherChoice::ConnectAsTopScreen: return "Connect as top screen";
        case LauncherChoice::ConnectAsBottomScreen: return "Connect as bottom screen";
        case LauncherChoice::Settings: return "Settings";
        case LauncherChoice::Quit: return "Quit";
        case LauncherChoice::None: break;
    }
    return "";
}

}  // namespace

std::vector<PixelRect> LauncherScreen::compute_rows(float output_width,
                                                    float output_height,
                                                    float& out_scale) const {
    out_scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(out_scale);
    const float margin = std::round(output_width * 0.12f);
    const float width = output_width - margin * 2.0f;
    const float row_height = line * 2.4f;
    const float top = std::round(output_height * 0.30f);

    std::vector<PixelRect> rows;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        rows.push_back(PixelRect{margin,
                                 top + static_cast<float>(index) * (row_height + line * 0.5f),
                                 width, row_height});
    }
    return rows;
}

void LauncherScreen::open(bool runs_games) {
    open_ = true;
    focus_ = 0;
    runs_games_ = runs_games;
    rows_.clear();
    if (runs_games) {
        rows_.push_back(LauncherChoice::LoadGame);
    }
    rows_.push_back(LauncherChoice::ConnectAsTopScreen);
    rows_.push_back(LauncherChoice::ConnectAsBottomScreen);
    if (runs_games) {
        rows_.push_back(LauncherChoice::Settings);
    }
    rows_.push_back(LauncherChoice::Quit);
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
    const int count = static_cast<int>(rows_.size());
    if (count == 0) {
        return result;
    }

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                focus_ = (focus_ + count - 1) % count;
                break;
            case UiAction::Down:
                focus_ = (focus_ + 1) % count;
                break;
            case UiAction::Confirm:
                result.choice = rows_[static_cast<std::size_t>(focus_)];
                break;
            // A menu's face buttons. Nothing here answers them, and a bare
            // fallthrough into Quit would make pressing X leave the screen.
            case UiAction::MenuQuit:
            case UiAction::MenuReset: break;
            case UiAction::MenuDisconnect:
                // Plus. Only where the platform's own convention says so; see
                // set_plus_quits().
                if (plus_quits_) {
                    result.choice = LauncherChoice::Quit;
                }
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
                result.choice = rows_[index];
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
                            const ButtonGlyphSheet& glyphs,
                            float output_width,
                            float output_height,
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
    text.draw(quads, "Dualscreen", margin, std::round(output_height * 0.14f), scale * 2.0f,
              theme.text);
    text.draw(quads,
              runs_games_
                  ? "Play a DS game here, or be a screen for one running on another device"
                  : "Be a screen for a game running on another device",
              margin, std::round(output_height * 0.14f) + line * 2.4f, scale * 0.95f,
              theme.text_dim);

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const PixelRect& row = rows[index];
        const bool focused = (static_cast<int>(index) == focus_);
        draw_menu_row(quads, row, focused, scale);

        const float pad = line * 0.7f;
        const bool is_quit = (rows_[index] == LauncherChoice::Quit);
        // Centred on the row, since there is nothing under it any more: the
        // names say what they do without a sentence explaining them.
        text.draw(quads, title_for(rows_[index]), row.x + pad,
                  row.y + (row.h - TextRenderer::line_height(scale * 1.2f)) * 0.5f, scale * 1.2f,
                  is_quit && !focused ? theme.danger : menu_row_text(focused));
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

    std::vector<GuidePrompt> guide{{ButtonGlyph::A, "Select"}};
    if (plus_quits_) {
        guide.push_back({ButtonGlyph::Plus, "Quit"});
    }
    draw_control_guide(quads, text, glyphs, guide, margin + rows[0].w,
                       output_height - line * 1.2f - control_guide_height(scale * 0.9f),
                       scale * 0.9f, theme.text, TextAlign::Right);
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
                              const ButtonGlyphSheet& glyphs,
                              float output_width,
                              float output_height,
                              const std::vector<ds::net::DiscoveredHost>& hosts,
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

        draw_menu_row(quads, row, focused, scale);

        const float pad = line * 0.6f;
        text.draw(quads, host.game_title.empty() ? "(no game)" : host.game_title, row.x + pad,
                  row.y + pad * 0.7f, scale * 1.1f,
                  available ? menu_row_text(focused) : theme.text_dim);
        const std::string detail =
            host.device_name + "   " + host.address +
            (available ? "" : "   this screen is already taken");
        text.draw(quads, detail, row.x + pad, row.y + pad * 0.7f + line * 1.4f, scale * 0.85f,
                  focused ? theme.panel : (available ? theme.text_dim : theme.warning));
    }

    draw_control_guide(quads, text, glyphs,
                       {{ButtonGlyph::A, "Connect"}, {ButtonGlyph::B, "Back"}},
                       rows.empty() ? output_width * 0.94f : rows[0].x + rows[0].w,
                       output_height - line * 1.2f - control_guide_height(scale * 0.9f),
                       scale * 0.9f, theme.text, TextAlign::Right);
}

}  // namespace ds
