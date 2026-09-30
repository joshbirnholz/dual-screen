#include "ui/game_browser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/page_frame.hpp"
#include "util/log.hpp"

namespace ds {
bool is_supported_rom(const std::string& filename) {
    // Whatever any system claims, so the browser lists a 3DS game beside a DS
    // one and picking either simply works. Archives are not claimed by
    // anything: loading one would need a libretro VFS this frontend does not
    // provide.
    return is_supported_rom_for_any_system(filename);
}

std::vector<std::string> default_browse_dirs() {
#if defined(__SWITCH__)
    // The SD card, and where Switch homebrew conventionally keeps ROMs --
    // RetroArch's own default among them.
    std::vector<std::string> on_the_card;
    for (const char* candidate : {"/roms/nds", "/roms/NDS", "/roms/ds", "/roms", "/ROMs"}) {
        if (directory_exists(candidate)) {
            on_the_card.push_back(candidate);
        }
    }
    on_the_card.push_back("/");
    return on_the_card;
#else
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
#endif
}

void GameBrowser::open(const std::string& directory) {
    open_ = true;
    // A remembered directory has to be absolute. A relative one could only
    // have come from a bug, and resolving it against wherever the app happens
    // to have been started from lands the player somewhere arbitrary -- so it
    // is discarded in favour of a sensible default rather than honoured.
    std::string start = expand_user_path(directory);
    if (!start.empty() && start.front() != '/') {
        log_warn("ignoring a remembered folder that is not an absolute path: " + start);
        start.clear();
    }
    if (start.empty() || !directory_exists(start)) {
        const std::vector<std::string> defaults = default_browse_dirs();
        start = defaults.empty() ? std::string("/") : defaults.front();
    }
    navigate_to(start);
}

// By value, not by reference. Callers hand this the path out of a row, and
// rebuilding the row list below destroys that string -- so a reference would
// dangle from there on, and the ".." written for the next level would point at
// freed memory. Taking a copy makes that impossible for every caller rather
// than asking each one to remember.
bool GameBrowser::navigate_to(std::string directory) {
    // Everything below assumes a path that can be walked upwards. A relative
    // one cannot: path_dirname maps "." and any bare name to "." forever, so a
    // browser that ever stored one could never climb out of it again.
    directory = absolute_path(directory);

    std::vector<DirectoryEntry> entries;
    std::string error;
    if (!list_directory(directory, entries, &error)) {
        // Stay where we are rather than stranding the player in a dead end.
        error_ = error;
        log_warn(error);
        if (!directory_.empty()) {
            return false;
        }
        // Nothing to fall back to on the very first listing.
        directory_ = directory;
        rows_.clear();
        focus_ = 0;
        scroll_ = 0.0f;
        drag_.stop();
        return true;
    }

    error_.clear();
    directory_ = directory;
    rows_.clear();
    focus_ = 0;
    scroll_ = 0.0f;
    drag_.stop();

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
    return true;
}

GameBrowser::Geometry GameBrowser::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    // The same frame as every other page: see ui/page_frame.hpp.
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    geometry.panel = frame.panel;
    geometry.list = frame.content();
    // A row and the gap under it: the rows are as tall as every other list's.
    geometry.row_height = menu_row_height(frame.line) + menu_row_gap(frame.line);
    geometry.visible_rows =
        std::max(1, static_cast<int>(geometry.list.h / geometry.row_height));
    return geometry;
}

float GameBrowser::max_scroll(const Geometry& geometry) const {
    return std::max(0.0f, static_cast<float>(rows_.size()) * geometry.row_height - geometry.list.h);
}

void GameBrowser::scroll_focus_into_view(const Geometry& geometry) {
    const float top = static_cast<float>(focus_) * geometry.row_height;
    const float bottom = top + geometry.row_height;
    if (top < scroll_) {
        scroll_ = top;
    } else if (bottom > scroll_ + geometry.list.h) {
        scroll_ = bottom - geometry.list.h;
    }
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll(geometry));
}

void GameBrowser::activate(int index, GameBrowserResult& result) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return;
    }
    const Row row = rows_[static_cast<std::size_t>(index)];
    if (row.is_directory) {
        result.directory_changed = navigate_to(row.path);
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
    // Where the finger is comes from live_pointer() now, the same for every
    // list; the input is kept for what callers hand in.
    (void)input;
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
                if (count > 0) focus_ = std::max(0, focus_ - 1);
                break;
            case UiAction::Down:
                if (count > 0) focus_ = std::min(count - 1, focus_ + 1);
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
                        result.directory_changed = navigate_to(rows_.front().path);
                    } else {
                        result.back_requested = true;
                    }
                    navigated = true;
                }
                break;
            case UiAction::ToggleMenu:
                break;  // there is no game to pause yet
            case UiAction::ToggleFilter:
            case UiAction::SwapLayout:
                break;  // nothing is being drawn from a game here
            // A menu's face buttons. Nothing here answers them, and a bare
            // fallthrough into Quit would make pressing X leave the screen.
            // X: a forwarder for the ROM under the cursor, where there is one.
            case UiAction::MenuQuit: {
                std::string path;
                if (!navigated && forwarder_target(path)) {
                    result.forwarder_requested = true;
                    result.chosen_path = path;
                    navigated = true;
                }
                break;
            }
            case UiAction::MenuReset:
            // Settings is reached from the main menu and from a game's own
            // menu; a file browser is neither.
            case UiAction::MenuSettings: break;
            // Plus: straight out to the main menu, from however deep in the
            // folders this is -- where B only goes up one.
            case UiAction::MenuDisconnect:
                if (!navigated) {
                    result.back_requested = true;
                    navigated = true;
                }
                break;
            // Escape also raises Cancel, which is what it means here.
            case UiAction::OpenMenu: break;
            // Leaving the app is the main menu's to offer, not this page's.
            // F10 still quits from anywhere, which the app sees for itself.
            case UiAction::Quit: break;
        }
    }
    focus_ = std::max(0, std::min(focus_, std::max(0, count - 1)));
    // The view follows the cursor when it moves -- and only then, or a list
    // dragged by a finger would be pulled straight back to it.
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(geometry);
    }

    // Touch: the list follows a finger dragged on it and glides on after
    // one flicked; a release that never became a drag is a tap.
    drag_.update(pointers, geometry.list, scroll_, max_scroll(geometry));
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        // A prompt tapped is that button pressed, where it came down on the
        // same one it left.
        const ButtonGlyph under = guide_button_at(guide_hits_, pointer.x, pointer.y);
        if (!navigated && under != ButtonGlyph::Count &&
            under == guide_button_at(guide_hits_, pointer.press_x, pointer.press_y)) {
            std::string path;
            if (under == ButtonGlyph::Plus) {
                result.back_requested = true;
            } else if (under == ButtonGlyph::X && forwarder_target(path)) {
                result.forwarder_requested = true;
                result.chosen_path = path;
            } else if (under == ButtonGlyph::A) {
                activate(focus_, result);
            } else if (under == ButtonGlyph::B) {
                if (!rows_.empty() && rows_.front().is_parent) {
                    result.directory_changed = navigate_to(rows_.front().path);
                } else {
                    result.back_requested = true;
                }
            }
            navigated = true;
        } else if (!navigated && !drag_.release_was_drag() &&
                   geometry.list.contains(pointer.x, pointer.y)) {
            const auto row_under = [&](float y) {
                return static_cast<int>(std::floor((y - geometry.list.y + scroll_) / geometry.row_height));
            };
            const int row = row_under(pointer.y);
            // The finger has to have come down on the same row it left on,
            // not merely somewhere in the list. A first tap highlights the
            // row; a second, on the same row, opens it.
            if (row >= 0 && row < count && row == row_under(pointer.press_y)) {
                if (tap_chooses(focus_, row)) activate(row, result);
                navigated = true;
            }
        }
    }
    drag_.settle();

    if (result.game_chosen) {
        open_ = false;
    }
    return result;
}

void GameBrowser::render(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         float output_width,
                         float output_height) {
    if (!open_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    // The folder, at the right of the heading, keeping its end.
    draw_page_header(quads, text, frame, "Choose a game", directory_, 0.0f, true);

    if (!error_.empty()) {
        text.draw_wrapped(quads, error_, geometry.list.x + line * 0.5f,
                          geometry.list.y + line * 0.5f, geometry.list.w - line, scale, theme.danger);
    } else if (rows_.empty()) {
        text.draw(quads, "No DS games or folders here", geometry.list.x + geometry.list.w * 0.5f,
                  geometry.list.y + geometry.list.h * 0.5f, scale, theme.text_dim, TextAlign::Center);
    }

    const int count = static_cast<int>(rows_.size());
    // Rows are placed by the pixel, so the one at the top can be part-way
    // off it: drawn clipped to the list.
    quads.set_clip(geometry.list);
    const int first = std::max(0, static_cast<int>(std::floor(scroll_ / geometry.row_height)));
    const int last = std::min(count, static_cast<int>(std::ceil((scroll_ + geometry.list.h) /
                                                                 geometry.row_height)));
    for (int index = first; index < last; ++index) {
        const Row& row = rows_[static_cast<std::size_t>(index)];
        // Short of the scroll bar, which runs beside the rows.
        const PixelRect rect{geometry.list.x,
                             geometry.list.y + index * geometry.row_height - scroll_,
                             geometry.list.w - scroll_bar_width(scale) - line * 0.4f,
                             geometry.row_height - menu_row_gap(line)};
        const bool focused = (index == focus_);
        draw_menu_row(quads, rect, focused, scale);

        const float pad = menu_row_pad(line);
        const float baseline = page_row_text_y(rect, scale);
        // Folders get a trailing slash and the accent colour, so the two kinds
        // of row are separable at a glance on a small screen.
        const std::string label =
            (row.is_directory && !row.is_parent) ? (row.label + "/") : row.label;
        const Color color = focused ? menu_row_text(true)
                                    : (row.is_parent ? theme.text_dim
                                                     : (row.is_directory ? theme.accent
                                                                         : theme.text));
        text.draw(quads, TextRenderer::elide_left(label, geometry.list.w - pad * 3.0f, scale),
                  rect.x + pad, baseline, scale, color);
    }

    // Scrollbar, drawn only when the list actually overflows.
    quads.clear_clip();
    const float content = static_cast<float>(count) * geometry.row_height;
    if (content > geometry.list.h + 0.5f) {
        draw_scroll_bar(quads, geometry.list, scroll_ / content, geometry.list.h / content, line,
                        scale);
    }

    // Right-aligned to the panel's edge, as every other screen's are. Main
    // Menu leads, the way the one that leaves leads the main menu's and the
    // quick menu's rows. Cancel goes up a folder while there is one to go up
    // to and out of the browser once there is not, which "Back" covers
    // either way.
    guide_hits_.clear();
    std::vector<GuidePrompt> prompts{{ButtonGlyph::Plus, "Main Menu"}};
    std::string target;
    if (forwarder_target(target)) prompts.push_back({ButtonGlyph::X, "Create Forwarder"});
    prompts.push_back({ButtonGlyph::A, "Open", PromptRole::ActsOnSelection});
    prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    draw_page_guide(quads, text, glyphs, frame, prompts, &guide_hits_);
}

bool GameBrowser::forwarder_target(std::string& path) const {
    if (!offers_forwarders_ || focus_ < 0 || focus_ >= static_cast<int>(rows_.size())) return false;
    const Row& row = rows_[static_cast<std::size_t>(focus_)];
    if (row.is_directory || row.is_parent) return false;
    path = row.path;
    return true;
}

}  // namespace ds
