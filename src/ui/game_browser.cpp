#include "ui/game_browser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ui/prompt.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// A press has to move further than this before it counts as a scroll rather
// than a tap, so a slightly shaky finger still selects what it landed on.
constexpr float kDragThresholdPixels = 12.0f;

}  // namespace

bool is_supported_rom(const std::string& filename) {
    // Whatever any system claims, so the browser lists a 3DS game beside a DS
    // one and picking either simply works. Archives are not claimed by
    // anything: loading one would need a libretro VFS this frontend does not
    // provide.
    return is_supported_rom_for_any_system(filename);
}

std::vector<std::string> default_browse_dirs() {
    const char* home_env = std::getenv("HOME");
    const std::string home = (home_env != nullptr && home_env[0] != '\0') ? home_env : "/home";

    std::vector<std::string> candidates;
    const std::string preferred[] = {
        // Where the common Deck setups actually put things.
        path_join(home, "Emulation/roms/nds"),
        path_join(home, "Emulation/roms"),
        path_join(home, "retrodeck/roms/nds"),
        path_join(home, "retrodeck/roms"),
        path_join(home, "roms/nds"),
        path_join(home, "roms"),
        path_join(home, "ROMs"),
        path_join(home, "Games"),
        "/run/media/deck",  // the SD card slot on a Steam Deck
        "/run/media",
        path_join(home, "Downloads"),
    };
    for (const std::string& candidate : preferred) {
        if (directory_exists(candidate)) {
            candidates.push_back(candidate);
        }
    }
    candidates.push_back(home);
    return candidates;
}

void GameBrowser::open(const std::string& directory) {
    open_ = true;
    std::string start = expand_user_path(directory);
    if (start.empty() || !directory_exists(start)) {
        const std::vector<std::string> defaults = default_browse_dirs();
        start = defaults.empty() ? std::string("/") : defaults.front();
    }
    navigate_to(start);
}

void GameBrowser::navigate_to(const std::string& directory) {
    std::vector<DirectoryEntry> entries;
    std::string error;
    if (!list_directory(directory, entries, &error)) {
        // Stay where we are rather than stranding the player in a dead end.
        error_ = error;
        log_warn(error);
        if (!directory_.empty()) {
            return;
        }
        // Nothing to fall back to on the very first listing.
        directory_ = directory;
        rows_.clear();
        focus_ = 0;
        scroll_ = 0;
        return;
    }

    error_.clear();
    directory_ = directory;
    rows_.clear();
    focus_ = 0;
    scroll_ = 0;

    if (directory != "/") {
        Row up;
        up.label = "..";
        up.path = path_dirname(directory);
        up.is_directory = true;
        up.is_parent = true;
        rows_.push_back(up);
    }

    for (const DirectoryEntry& entry : entries) {
        if (!entry.is_directory && !is_supported_rom(entry.name)) {
            continue;
        }
        Row row;
        row.label = entry.name;
        row.path = entry.path;
        row.is_directory = entry.is_directory;
        rows_.push_back(row);
    }
}

GameBrowser::Geometry GameBrowser::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(geometry.scale);

    const float margin = std::round(output_width * 0.06f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.06f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.12f)};

    const float header = line * 4.0f;
    const float footer = line * 3.0f;
    geometry.list = PixelRect{geometry.panel.x, geometry.panel.y + header, geometry.panel.w,
                              geometry.panel.h - header - footer};
    geometry.row_height = line * 1.8f;
    geometry.visible_rows =
        std::max(1, static_cast<int>(geometry.list.h / geometry.row_height));

    const float button_width = std::min(geometry.panel.w * 0.3f,
                                        TextRenderer::measure_width("  Quit  ", geometry.scale) * 2.0f);
    const float button_y = geometry.panel.y + geometry.panel.h - footer + line * 0.4f;
    const float button_height = line * 2.0f;
    geometry.quit_button = PixelRect{geometry.panel.x + geometry.panel.w - button_width, button_y,
                                     button_width, button_height};
    // Beside Quit, because leaving the browser and leaving the app are the two
    // ways out and they belong together.
    const float back_width = std::min(
        geometry.panel.w * 0.35f,
        TextRenderer::measure_width("  Main menu  ", geometry.scale) * 1.4f);
    geometry.back_button =
        PixelRect{geometry.quit_button.x - back_width - line * 0.6f, button_y, back_width,
                  button_height};
    return geometry;
}

void GameBrowser::clamp_scroll(int visible_rows) {
    const int maximum = std::max(0, static_cast<int>(rows_.size()) - visible_rows);
    scroll_ = std::clamp(scroll_, 0, maximum);
}

void GameBrowser::scroll_focus_into_view(int visible_rows) {
    if (focus_ < scroll_) {
        scroll_ = focus_;
    } else if (focus_ >= scroll_ + visible_rows) {
        scroll_ = focus_ - visible_rows + 1;
    }
    clamp_scroll(visible_rows);
}

void GameBrowser::activate(int index, GameBrowserResult& result) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return;
    }
    const Row row = rows_[static_cast<std::size_t>(index)];
    if (row.is_directory) {
        navigate_to(row.path);
        result.directory_changed = true;
        return;
    }
    result.game_chosen = true;
    result.chosen_path = row.path;
}

GameBrowserResult GameBrowser::update(const InputManager& input,
                                      const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers,
                                      float output_width,
                                      float output_height) {
    GameBrowserResult result;
    if (!open_) {
        return result;
    }

    const Geometry geometry = compute_geometry(output_width, output_height);
    const int visible = geometry.visible_rows;
    const int count = static_cast<int>(rows_.size());

    // At most one navigation per update. activate() and the Cancel handler
    // both replace rows_ underneath this loop, so a second one in the same
    // frame would act on a directory the player never saw -- entering a folder
    // and immediately leaving it, which looks like the button doing nothing.
    bool navigated = false;

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (count > 0) focus_ = (focus_ + count - 1) % count;
                break;
            case UiAction::Down:
                if (count > 0) focus_ = (focus_ + 1) % count;
                break;
            case UiAction::PageUp:
            case UiAction::Left:
                focus_ = std::max(0, focus_ - visible);
                break;
            case UiAction::PageDown:
            case UiAction::Right:
                focus_ = std::min(count - 1, focus_ + visible);
                break;
            case UiAction::Confirm:
                if (!navigated) {
                    activate(focus_, result);
                    navigated = true;
                }
                break;
            case UiAction::Cancel:
                // Up a level, which is what Back means in a file browser -- and
                // out of the browser entirely once there is no level left.
                if (!navigated) {
                    if (!rows_.empty() && rows_.front().is_parent) {
                        navigate_to(rows_.front().path);
                        result.directory_changed = true;
                    } else {
                        result.back_requested = true;
                    }
                    navigated = true;
                }
                break;
            case UiAction::ToggleMenu:
                break;  // there is no game to pause yet
            case UiAction::ToggleFilter:
                break;  // nothing is being drawn from a game here
            case UiAction::Quit:
                result.quit_requested = true;
                break;
        }
    }
    focus_ = std::max(0, std::min(focus_, std::max(0, count - 1)));
    scroll_focus_into_view(visible);

    // Touch: press starts a potential drag, movement past the threshold turns
    // it into a scroll, and a release that never became a drag is a tap.
    for (const PointerEvent& pointer : pointers) {
        if (pointer.pressed) {
            dragging_ = geometry.list.contains(pointer.x, pointer.y);
            drag_moved_ = false;
            drag_start_y_ = pointer.y;
            drag_start_scroll_ = scroll_;
        } else if (pointer.released) {
            if (geometry.quit_button.contains(pointer.x, pointer.y)) {
                result.quit_requested = true;
            } else if (geometry.back_button.contains(pointer.x, pointer.y)) {
                result.back_requested = true;
            } else if (!navigated && dragging_ && !drag_moved_ &&
                       geometry.list.contains(pointer.x, pointer.y)) {
                const int row = scroll_ + static_cast<int>((pointer.y - geometry.list.y) /
                                                           geometry.row_height);
                if (row >= 0 && row < count) {
                    focus_ = row;
                    activate(row, result);
                    navigated = true;
                }
            }
            dragging_ = false;
            drag_moved_ = false;
        }
    }

    if (dragging_ && input.pointer_active() && geometry.row_height > 0.0f) {
        const float travel = drag_start_y_ - input.pointer_y();
        if (std::fabs(travel) > kDragThresholdPixels) {
            drag_moved_ = true;
        }
        if (drag_moved_) {
            scroll_ = drag_start_scroll_ + static_cast<int>(travel / geometry.row_height);
            clamp_scroll(visible);
        }
    }

    if (result.game_chosen || result.quit_requested) {
        open_ = false;
    }
    return result;
}

void GameBrowser::render(QuadRenderer& quads,
                         TextRenderer& text,
                         float output_width,
                         float output_height,
                         const MenuButtonLabels& labels) {
    if (!open_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    text.draw(quads, "Choose a game", geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);
    text.draw(quads,
              TextRenderer::elide_left(directory_, geometry.panel.w, scale * 0.95f),
              geometry.panel.x, geometry.panel.y + line * 2.1f, scale * 0.95f, theme.text_dim);

    quads.draw_solid(geometry.list, Color{0.03f, 0.04f, 0.06f, 1.0f});

    if (!error_.empty()) {
        text.draw_wrapped(quads, error_, geometry.list.x + line * 0.5f,
                          geometry.list.y + line * 0.5f, geometry.list.w - line, scale, theme.danger);
    } else if (rows_.empty()) {
        text.draw(quads, "No DS games or folders here", geometry.list.x + geometry.list.w * 0.5f,
                  geometry.list.y + geometry.list.h * 0.5f, scale, theme.text_dim, TextAlign::Center);
    }

    const int count = static_cast<int>(rows_.size());
    const int last = std::min(count, scroll_ + geometry.visible_rows);
    for (int index = scroll_; index < last; ++index) {
        const Row& row = rows_[static_cast<std::size_t>(index)];
        const PixelRect rect{geometry.list.x,
                             geometry.list.y + (index - scroll_) * geometry.row_height,
                             geometry.list.w, geometry.row_height};
        const bool focused = (index == focus_);
        if (focused) {
            quads.draw_solid(rect, theme.panel_focused);
            quads.draw_outline(rect, theme.accent, std::max(1.0f, scale * 0.5f));
        }

        const float pad = line * 0.5f;
        const float baseline = rect.y + (rect.h - line) * 0.5f;
        // Folders get a trailing slash and the accent colour, so the two kinds
        // of row are separable at a glance on a small screen.
        const std::string label =
            (row.is_directory && !row.is_parent) ? (row.label + "/") : row.label;
        const Color color = row.is_parent ? theme.text_dim
                                          : (row.is_directory ? theme.accent : theme.text);
        text.draw(quads, TextRenderer::elide_left(label, geometry.list.w - pad * 3.0f, scale),
                  rect.x + pad, baseline, scale, color);
    }

    // Scrollbar, drawn only when the list actually overflows.
    if (count > geometry.visible_rows) {
        const float track_width = std::max(3.0f, scale * 2.0f);
        const PixelRect track{geometry.list.x + geometry.list.w - track_width, geometry.list.y,
                              track_width, geometry.list.h};
        quads.draw_solid(track, Color{0.10f, 0.12f, 0.16f, 1.0f});
        const float fraction = static_cast<float>(geometry.visible_rows) / count;
        const float thumb_height = std::max(line, geometry.list.h * fraction);
        const float travel = geometry.list.h - thumb_height;
        const float offset = static_cast<float>(scroll_) / (count - geometry.visible_rows);
        quads.draw_solid(PixelRect{track.x, track.y + travel * offset, track.w, thumb_height},
                         theme.accent);
    }

    quads.draw_solid(geometry.back_button, theme.panel);
    text.draw(quads, "Main menu", geometry.back_button.x + geometry.back_button.w * 0.5f,
              geometry.back_button.y + (geometry.back_button.h - line) * 0.5f, scale, theme.text,
              TextAlign::Center);

    quads.draw_solid(geometry.quit_button, theme.panel);
    text.draw(quads, "Quit", geometry.quit_button.x + geometry.quit_button.w * 0.5f,
              geometry.quit_button.y + (geometry.quit_button.h - line) * 0.5f, scale, theme.danger,
              TextAlign::Center);

    // Kept clear of the Quit button rather than running underneath it. The
    // letters come from what this pad prints, so the hint names the button the
    // player is actually looking at.
    const float hint_width = geometry.back_button.x - geometry.panel.x - line;
    // Short enough to fit beside two buttons. Cancel goes up a folder while
    // there is one to go up to and out of the browser once there is not, which
    // "Back" covers either way.
    const std::string hint = format_prompts(
        {{labels.confirm, "Open"}, {labels.cancel, "Back"}, {"L/R", "Page"}});
    text.draw(quads, TextRenderer::elide_left(hint, hint_width, scale * 0.9f), geometry.panel.x,
              geometry.quit_button.y + (geometry.quit_button.h - line) * 0.5f, scale * 0.9f,
              theme.text_dim);
}

}  // namespace ds
