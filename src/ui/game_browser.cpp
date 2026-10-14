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

bool is_amiibo_file(const std::string& filename) {
    const std::string extension = path_extension_lower(filename);
    return extension == "bin" || extension == "nfc" || extension == "dat";
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
#elif defined(__ANDROID__)
    // Shared storage, where a file manager, a USB cable and RetroArch all put
    // ROMs. Reading it at all takes "access to all files" from Android 11 on,
    // which the app asks for before it gets here -- see
    // DualscreenActivity.java.
    const std::string storage = "/storage/emulated/0";
    std::vector<std::string> candidates;
    for (const char* candidate : {"roms/nds", "roms/NDS", "roms/ds", "ROMs/nds", "ROMs/NDS",
                                  "RetroArch/roms/nds", "Emulation/roms/nds", "roms", "ROMs",
                                  "Download"}) {
        const std::string path = path_join(storage, candidate);
        if (directory_exists(path)) {
            candidates.push_back(path);
        }
    }
    candidates.push_back(storage);
    return candidates;
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
        if (!entry.is_directory &&
            !(amiibo_ ? is_amiibo_file(entry.name) : is_supported_rom(entry.name))) {
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

bool GameBrowser::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                         float output_height) {
    if (!open_) return false;
    const Geometry geometry = compute_geometry(output_width, output_height);
    drag_.update(pointers, geometry.list, scroll_, max_scroll(geometry));
    return drag_.release_was_drag();
}

int GameBrowser::entry_at(float x, float y, float output_width, float output_height) const {
    if (!open_) return -1;
    // A prompt is the button it shows.
    const ButtonGlyph under = guide_button_at(guide_hits_, x, y);
    if (under != ButtonGlyph::Count) return kGuideEntryBase + static_cast<int>(under);
    const Geometry geometry = compute_geometry(output_width, output_height);
    if (!geometry.list.contains(x, y)) return -1;
    const int row = static_cast<int>(std::floor((y - geometry.list.y + scroll_) / geometry.row_height));
    return row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
}

std::vector<GameBrowserEntry> GameBrowser::entries() const {
    std::vector<GameBrowserEntry> out;
    out.reserve(rows_.size());
    for (const Row& row : rows_) out.push_back(GameBrowserEntry{row.label, row.is_directory, row.is_parent});
    return out;
}

void GameBrowser::show_mirrored(const std::string& directory, const std::vector<GameBrowserEntry>& entries,
                                int focus) {
    if (!open_ || directory != directory_) scroll_ = 0.0f;
    open_ = true;
    directory_ = directory;
    error_.clear();
    rows_.clear();
    for (const GameBrowserEntry& entry : entries) {
        Row row;
        row.label = entry.label;
        row.is_directory = entry.is_directory;
        row.is_parent = entry.is_parent;
        rows_.push_back(std::move(row));
    }
    focus_ = std::clamp(focus, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
}

std::vector<float> GameBrowser::scroll_state(float output_width, float output_height) const {
    return {scroll_in_lines(scroll_, compute_geometry(output_width, output_height).scale)};
}

void GameBrowser::set_scroll_state(const std::vector<float>& state, float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 0), geometry.scale), 0.0f, max_scroll(geometry));
    drag_.stop();
}

bool GameBrowser::handle_tap(int entry, GameBrowserResult& result) {
    if (entry >= kGuideEntryBase) {
        const ButtonGlyph under = static_cast<ButtonGlyph>(entry - kGuideEntryBase);
        std::string path;
        if (under == ButtonGlyph::Plus && !amiibo_) {
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
        return true;
    }
    if (entry < 0 || entry >= static_cast<int>(rows_.size())) return false;
    // A first tap highlights the row; a second, on the same row, opens it.
    if (tap_chooses(focus_, entry)) activate(entry, result);
    return true;
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
            // L and R jump nowhere: a shoulder is not a way through a list.
            case UiAction::PageUp:
            case UiAction::PageDown: break;
            case UiAction::Left:
                focus_ = std::max(0, focus_ - visible);
                break;
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
            case UiAction::MenuReset: break;
            // Plus: straight out to the main menu, from however deep in the
            // folders this is -- where B only goes up one. (Plus is the
            // quick menu's Settings; a file browser has no Settings.)
            case UiAction::MenuSettings:
            case UiAction::MenuDisconnect:
                // An amiibo's browser is a page of the quick menu, which B
                // leaves; there is no main menu behind it.
                if (!navigated && !amiibo_) {
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
    for (const int entry : take_taps(pointers, output_width, output_height)) {
        if (navigated) break;
        navigated = handle_tap(entry, result);
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

    if (amiibo_) {
        draw_page_background(quads, output_width, output_height, over_game_);
    } else {
        quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
        quads.clear(theme.background);
    }

    // The folder, at the right of the heading, keeping its end.
    draw_page_header(quads, text, frame, amiibo_ ? "Choose an amiibo" : "Choose a game", directory_,
                     0.0f, true);

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
    std::vector<GuidePrompt> prompts;
    if (!amiibo_) prompts.push_back({ButtonGlyph::Plus, "Main Menu"});
    std::string target;
    if (forwarder_target(target)) prompts.push_back({ButtonGlyph::X, "Create Forwarder"});
    // An amiibo is scanned rather than opened; a folder is still opened.
    const bool on_file = focus_ >= 0 && focus_ < static_cast<int>(rows_.size()) &&
                         !rows_[static_cast<std::size_t>(focus_)].is_directory;
    prompts.push_back({ButtonGlyph::A, amiibo_ && on_file ? "Scan" : "Open",
                       PromptRole::ActsOnSelection});
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

void GameBrowser::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, directory_);
    mix_sound_state(view.rest, rows_.size());
    view.error = view.error || !error_.empty();
}

}  // namespace ds
