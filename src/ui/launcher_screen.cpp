#include "ui/launcher_screen.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/page_frame.hpp"

namespace ds {
namespace {

// A game's icon with its corners rounded off, as the Switch's are: each pixel
// out past a corner's curve made as see-through as the curve leaves it,
// measured over a grid of points inside it so the edge comes out smooth even
// on a 32-pixel icon drawn many times its size. Only the alpha changes, so
// the filtering that smooths it draws no dark fringe.
std::vector<uint8_t> with_round_corners(const std::vector<uint8_t>& rgba, int width, int height) {
    std::vector<uint8_t> out = rgba;
    const float radius = std::min(width, height) * 0.16f;
    constexpr int kSamples = 4;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float px = x + (sx + 0.5f) / kSamples;
                    const float py = y + (sy + 0.5f) / kSamples;
                    const float dx = std::max({radius - px, 0.0f, px - (width - radius)});
                    const float dy = std::max({radius - py, 0.0f, py - (height - radius)});
                    if (dx * dx + dy * dy <= radius * radius) ++inside;
                }
            }
            if (inside == kSamples * kSamples) continue;
            uint8_t& alpha = out[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
            alpha = static_cast<uint8_t>(alpha * inside / (kSamples * kSamples));
        }
    }
    return out;
}

const LauncherScreen* unused_tag = nullptr;

const char* title_for(LauncherChoice choice) {
    switch (choice) {
        case LauncherChoice::LoadGame: return "Load a Game";
        case LauncherChoice::ConnectAsTopScreen: return "Connect as Top Screen";
        case LauncherChoice::ConnectAsBottomScreen: return "Connect as Bottom Screen";
        case LauncherChoice::Settings: return "Settings";
        case LauncherChoice::Quit: return "Quit";
        case LauncherChoice::StartRecent:
        case LauncherChoice::None: break;
    }
    return "";
}

}  // namespace

std::vector<PixelRect> LauncherScreen::compute_rows(float output_width,
                                                    float output_height,
                                                    float& out_scale) const {
    const PageFrame frame = page_frame(output_width, output_height);
    out_scale = frame.scale;
    const float row_height = menu_row_height(frame.line);
    const float gap = menu_row_gap(frame.line);

    std::vector<PixelRect> rows;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        rows.push_back(PixelRect{frame.panel.x,
                                 frame.content_top + static_cast<float>(index) * (row_height + gap),
                                 frame.panel.w, row_height});
    }
    return rows;
}

void LauncherScreen::open(bool runs_games) {
    open_ = true;
    focus_ = 0;
    confirming_ = -1;
    runs_games_ = runs_games;
    rows_.clear();
    if (runs_games || offers_load_game_) {
        rows_.push_back(LauncherChoice::LoadGame);
    }
    rows_.push_back(LauncherChoice::ConnectAsTopScreen);
    rows_.push_back(LauncherChoice::ConnectAsBottomScreen);
    // Anything that plays games has Settings, and Data Management is in
    // there, as its first row.
    if (runs_games || offers_load_game_) {
        rows_.push_back(LauncherChoice::Settings);
    }
    if (offers_quit_) {
        rows_.push_back(LauncherChoice::Quit);
    }
}

void LauncherScreen::set_recent(std::vector<LauncherTile> tiles) {
    // The game being asked about stays the one asked about, wherever it
    // moved to; gone, the alert goes too.
    const std::string asked = confirming_ >= 0 && confirming_ < static_cast<int>(tiles_.size())
                                  ? tiles_[static_cast<std::size_t>(confirming_)].key
                                  : std::string();
    tiles_ = std::move(tiles);
    confirming_ = -1;
    for (std::size_t index = 0; index < tiles_.size() && !asked.empty(); ++index) {
        if (tiles_[index].key == asked) confirming_ = static_cast<int>(index);
    }
    // Textures for games no longer shown go.
    for (auto texture = tile_textures_.begin(); texture != tile_textures_.end();) {
        const bool kept = std::any_of(tiles_.begin(), tiles_.end(),
                                      [&](const LauncherTile& tile) { return tile.key == texture->first; });
        texture = kept ? std::next(texture) : tile_textures_.erase(texture);
    }
    const int rows = static_cast<int>(rows_.size());
    if (focus_ >= rows && focus_ - rows >= static_cast<int>(tiles_.size())) {
        focus_ = tiles_.empty() ? rows - 1 : rows + static_cast<int>(tiles_.size()) - 1;
    }
}

std::vector<PixelRect> LauncherScreen::compute_tiles(float output_width, float output_height, int& columns) const {
    std::vector<PixelRect> tiles;
    columns = 1;
    if (tiles_.empty()) return tiles;
    const PageFrame frame = page_frame(output_width, output_height);
    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(output_width, output_height, scale);
    const float below = rows.empty() ? frame.content_top : rows.back().y + rows.back().h;
    // One row along the left of the page, centred in what is left under the
    // menu -- as much room above it as below.
    const float gap = std::round(frame.line * 1.6f);
    const float margin = std::round(frame.line * 1.4f);
    const float room = frame.content_bottom - below;
    const float tile = std::round(std::min(frame.line * 6.0f, room - margin * 2.0f));
    if (tile < frame.line * 2.5f) return tiles;
    columns = std::max(1, static_cast<int>((frame.panel.w + gap) / (tile + gap)));
    const int shown = std::min(static_cast<int>(tiles_.size()), columns);
    const float left = std::round(frame.panel.x);
    const float top = std::round(below + (room - tile) * 0.5f);
    for (int index = 0; index < shown; ++index) {
        tiles.push_back(PixelRect{left + index * (tile + gap), top, tile, tile});
    }
    return tiles;
}

int LauncherScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (!open_) return -1;
    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(output_width, output_height, scale);
    for (std::size_t index = 0; index < rows.size() && index < rows_.size(); ++index) {
        if (rows[index].contains(x, y)) return static_cast<int>(index);
    }
    int columns = 1;
    const std::vector<PixelRect> tiles = compute_tiles(output_width, output_height, columns);
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        if (tiles[index].contains(x, y)) return static_cast<int>(rows_.size() + index);
    }
    return -1;
}

LauncherResult LauncherScreen::update(const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers,
                                      float output_width,
                                      float output_height) {
    LauncherResult result;
    if (!open_) {
        return result;
    }

    const int count = static_cast<int>(rows_.size());
    if (count == 0) {
        return result;
    }
    // Asking whether to start the game chosen along the foot: it takes every
    // press until answered.
    if (confirming_ >= 0) {
        bool off = false;
        for (const int tap : take_taps(pointers, output_width, output_height)) off = off || tap == kTapOffPopup;
        const int answer = confirm_cursor_.update(actions, pointers, confirm_geometry(output_width, output_height));
        if (answer == 1) {
            result.choice = LauncherChoice::StartRecent;
            result.recent = tiles_[static_cast<std::size_t>(confirming_)].key;
            open_ = false;
        }
        if (answer != PopupCursor::kNone || off) confirming_ = -1;
        return result;
    }
    int columns = 1;
    tiles_shown_ = static_cast<int>(compute_tiles(output_width, output_height, columns).size());
    tile_columns_ = columns;
    if (focus_ >= count + tiles_shown_) focus_ = count + tiles_shown_ - 1;
    // Which of the whole, rows then tiles, a choice is.
    const auto choose = [&](int index) {
        if (index < count) {
            result.choice = rows_[static_cast<std::size_t>(index)];
        } else if (index - count < tiles_shown_) {
            // Asked first: it starts a game on another computer, perhaps
            // waking it.
            confirming_ = index - count;
            confirm_cursor_.reset(2, 1);
        }
    };

    for (const UiAction action : actions) {
        const int tile = focus_ - count;
        switch (action) {
            case UiAction::Up:
                if (tile >= columns) {
                    focus_ -= columns;
                } else if (tile >= 0) {
                    focus_ = count - 1;  // off the top row of games, onto the menu
                } else {
                    focus_ = std::max(0, focus_ - 1);
                }
                break;
            case UiAction::Down:
                if (tile < 0) {
                    // Off the end of the menu onto the first game.
                    if (focus_ + 1 < count || tiles_shown_ > 0) focus_ = std::min(count - 1 + tiles_shown_, focus_ + 1);
                } else if (tile + columns < tiles_shown_) {
                    focus_ += columns;
                } else if (tile / columns + 1 < (tiles_shown_ + columns - 1) / columns) {
                    focus_ = count + tiles_shown_ - 1;  // onto the end of a shorter last row
                }
                break;
            case UiAction::Left:
                if (tile > 0 && tile % columns > 0) --focus_;
                break;
            case UiAction::Right:
                if (tile >= 0 && tile % columns < columns - 1 && tile + 1 < tiles_shown_) ++focus_;
                break;
            case UiAction::Confirm:
                choose(focus_);
                break;
            // A menu's face buttons. Nothing here answers them, and a bare
            // fallthrough into Quit would make pressing X leave the screen.
            case UiAction::MenuQuit:
            case UiAction::MenuReset: break;
            case UiAction::MenuSettings:
            case UiAction::MenuDisconnect:
                // Plus. Only where the platform's own convention says so; see
                // set_plus_quits().
                if (plus_quits_ && offers_quit_) {
                    result.choice = LauncherChoice::Quit;
                }
                break;
            case UiAction::Quit:
                // F10, and whatever else means leave. Ignored where leaving is
                // not this app's to do: a keyboard attached to an iPad is not
                // a reason to be able to quit an iPad app from inside it.
                if (offers_quit_) {
                    result.choice = LauncherChoice::Quit;
                }
                break;
            default:
                break;
        }
    }

    for (const int index : take_taps(pointers, output_width, output_height)) {
        // A tap on a row chooses it: the finger held on it showed which it
        // was, and one that slid off took it back.
        if (index < count + tiles_shown_ && tap_chooses(focus_, index)) choose(index);
    }

    if (result.choice != LauncherChoice::None) {
        open_ = false;
    }
    return result;
}

std::string LauncherScreen::confirm_message() const {
    if (confirming_ < 0 || confirming_ >= static_cast<int>(tiles_.size())) return std::string();
    const LauncherTile& tile = tiles_[static_cast<std::size_t>(confirming_)];
    return "Start " + tile.title + " on " + tile.computer + "?";
}

PopupGeometry LauncherScreen::confirm_geometry(float output_width, float output_height) const {
    static const std::vector<PopupButton> buttons{{"Cancel", ""}, {"Start", ""}};
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height,
                          popup_text_height(PopupText{confirm_message(), {}, 0}, width, output_height), buttons);
}

bool LauncherScreen::off_popup(float x, float y, float output_width, float output_height) const {
    return confirming_ >= 0 && !confirm_geometry(output_width, output_height).box.contains(x, y);
}

std::string host_row_detail(const ds::net::DiscoveredHost& host, bool available) {
    // One on another build says so on the right of its row instead.
    return host.device_name + (available || host.needs_update() ? "" : "   this screen is already taken");
}

std::vector<GuidePrompt> LauncherScreen::guide_prompts() const {
    std::vector<GuidePrompt> guide;
    // Leftmost, the way Disconnect leads the quick menu's row: the one that
    // ends something goes first, and everything else keeps its place beside it.
    if (plus_quits_) {
        guide.push_back({ButtonGlyph::Plus, "Quit"});
    }
    // The row itself is the button on a touch device, and a bigger one.
    guide.push_back({ButtonGlyph::A, "Select", PromptRole::ActsOnSelection});
    return guide;
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
    const PageFrame frame = page_frame(output_width, output_height);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    draw_page_header(quads, text, frame, "Dualscreen");

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const PixelRect& row = rows[index];
        // Behind the alert, nothing is ringed.
        const bool focused = static_cast<int>(index) == focus_ && confirming_ < 0;
        draw_menu_row(quads, row, focused, scale, index + 1 == rows.size(),
                      static_cast<int>(index) == focus_ + 1 && confirming_ < 0);

        const float pad = menu_row_pad(line);
        // Centred on the row, since there is nothing under it any more: the
        // names say what they do without a sentence explaining them.
        text.draw(quads, title_for(rows_[index]), row.x + pad, page_row_text_y(row, scale), scale,
                  menu_row_text(focused));
    }

    // The games recently played on a computer this device can start them
    // on, along the foot: icons alone, as on a GamePad's Quick Start.
    int columns = 1;
    const std::vector<PixelRect> tiles = compute_tiles(output_width, output_height, columns);
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const PixelRect& tile = tiles[index];
        const LauncherTile& game = tiles_[index];
        const float radius = std::round(tile.w * 0.1f);
        quads.draw_rounded_rect(tile, radius, Color{0.17f, 0.19f, 0.24f, 1.0f});
        Texture& texture = tile_textures_[game.key];
        if (!texture.valid() && game.icon_width > 0 && game.icon_height > 0 &&
            game.icon_rgba.size() ==
                static_cast<std::size_t>(game.icon_width) * static_cast<std::size_t>(game.icon_height) * 4u) {
            const std::vector<uint8_t> rounded = with_round_corners(game.icon_rgba, game.icon_width, game.icon_height);
            texture.upload_rgba8888(reinterpret_cast<const uint32_t*>(rounded.data()), game.icon_width,
                                    game.icon_height, game.icon_width);
            texture.set_linear_filtering(true);
        }
        const float inset = std::round(tile.w * 0.09f);
        if (texture.valid()) {
            quads.draw_image(PixelRect{tile.x + inset, tile.y + inset, tile.w - inset * 2.0f, tile.h - inset * 2.0f},
                             texture, Color{1.0f, 1.0f, 1.0f, 1.0f});
        } else {
            text.draw(quads, game.system, tile.x + tile.w * 0.5f, std::round(tile.y + (tile.h - line) * 0.5f), scale,
                      theme.text_dim, TextAlign::Center);
        }
        const bool held = pointer_holding(tile);
        if (held) {
            quads.draw_rounded_rect(tile, radius, menu_highlight_fill());
        } else if (confirming_ < 0 && cursor_drawn(focus_ == static_cast<int>(rows_.size() + index))) {
            const float ring = std::max(3.0f, std::round(scale * 1.6f));
            const float out = std::round(ring * 1.2f);
            quads.draw_rounded_ring(
                bumped_cursor_ring(PixelRect{tile.x - out, tile.y - out, tile.w + out * 2.0f, tile.h + out * 2.0f},
                                   scale),
                radius + out, ring, menu_ring_left(), menu_ring_right());
        }
    }

    // Live feedback that the search is working, so "connect" is not a leap of
    // faith before anything has been found.
    const std::string found =
        hosts_found > 0
            ? std::to_string(hosts_found) + (hosts_found == 1 ? " device" : " devices") +
                  " running a game found nearby"
            : std::string("Looking for devices running a game...");
    draw_page_status(quads, text, glyphs, frame, found, hosts_found > 0 ? theme.success : theme.text_dim);
    draw_page_guide(quads, text, glyphs, frame, guide_prompts(), &guide_hits_);

    if (confirming_ >= 0) {
        static const std::vector<PopupButton> buttons{{"Cancel", ""}, {"Start", ""}};
        const PopupGeometry popup = confirm_geometry(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, buttons, confirm_cursor_.focus(),
                   confirm_cursor_.shown());
        draw_popup_text(quads, text, PopupText{confirm_message(), {}, 0}, popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

// --- HostPickerScreen -------------------------------------------------------

void HostPickerScreen::open(ScreenId wanted_screen) {
    open_ = true;
    focus_ = 0;
    wanted_screen_ = wanted_screen;
    needs_update_.reset();
}

std::vector<PopupButton> HostPickerScreen::alert_buttons() const {
    // Only this device's own update can be started from here: the other
    // cannot be reached until both are on the same build.
    if (needs_update_ && !needs_update_->is_older() && can_update_) {
        return {{"Not Now", ""}, {"Check for Updates", ""}};
    }
    return {{"OK", ""}};
}

std::string HostPickerScreen::alert_message() const {
    if (!needs_update_) return std::string();
    const std::string& name = needs_update_->device_name;
    if (needs_update_->is_older()) {
        return name + " is on an older build of Dualscreen than this device. It will need to be updated before "
                      "this device can connect: on " + name + ", choose Check for Updates in Settings.";
    }
    return name + " is on a newer build of Dualscreen than this device. This device will need to be updated "
                  "before it can connect.";
}

PopupGeometry HostPickerScreen::alert_geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height,
                          popup_text_height(PopupText{alert_message(), {}, 0}, width, output_height),
                          alert_buttons());
}

bool HostPickerScreen::off_popup(float x, float y, float output_width, float output_height) const {
    return needs_update_ && !alert_geometry(output_width, output_height).box.contains(x, y);
}

std::vector<PixelRect> HostPickerScreen::compute_rows(std::size_t count,
                                                      float output_width,
                                                      float output_height,
                                                      float& out_scale) const {
    const PageFrame frame = page_frame(output_width, output_height);
    out_scale = frame.scale;
    const float row_height = menu_two_line_row_height(frame.line);

    std::vector<PixelRect> rows;
    for (std::size_t index = 0; index < count; ++index) {
        rows.push_back(PixelRect{frame.panel.x,
                                 frame.content_top + index * (row_height + menu_row_gap(frame.line)),
                                 frame.panel.w, row_height});
    }
    return rows;
}

int HostPickerScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (!open_) return -1;
    float scale = 1.0f;
    const std::vector<PixelRect> rows = compute_rows(host_count_, output_width, output_height, scale);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
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
    host_count_ = hosts.size();

    // Saying which device needs an update: it takes every press until
    // answered.
    if (needs_update_) {
        bool off = false;
        for (const int tap : take_taps(pointers, output_width, output_height)) off = off || tap == kTapOffPopup;
        const int answer = alert_cursor_.update(actions, pointers, alert_geometry(output_width, output_height));
        if (answer == 1 && alert_buttons().size() == 2) result.check_for_updates = true;
        if (answer != PopupCursor::kNone || off) needs_update_.reset();
        return result;
    }

    const auto choose = [&](int index) {
        const ds::net::DiscoveredHost& host = hosts[static_cast<std::size_t>(index)];
        if (host.needs_update()) {
            needs_update_ = host;
            const std::size_t buttons = alert_buttons().size();
            alert_cursor_.reset(static_cast<int>(buttons), static_cast<int>(buttons) - 1);
            return;
        }
        result.chosen = true;
        result.host = host;
    };

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (count > 0) focus_ = std::max(0, focus_ - 1);
                break;
            case UiAction::Down:
                if (count > 0) focus_ = std::min(count - 1, focus_ + 1);
                break;
            case UiAction::Confirm:
                if (focus_ >= 0 && focus_ < count) choose(focus_);
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

    for (const int index : take_taps(pointers, output_width, output_height)) {
        if (index < count && tap_chooses(focus_, index)) choose(index);
    }

    if (result.chosen || result.cancelled) {
        open_ = false;
    }
    return result;
}

const Texture* HostPickerScreen::icon_for(const ds::net::DiscoveredHost& host) {
    if (host.icon_id == 0 || host.icon_width <= 0 || host.icon_height <= 0 ||
        host.icon_rgba.size() !=
            static_cast<std::size_t>(host.icon_width) * static_cast<std::size_t>(host.icon_height) * 4u) {
        return nullptr;
    }
    HostIcon& icon = icons_[host.address + ":" + std::to_string(host.session_port)];
    if (icon.icon_id != host.icon_id) {
        icon.icon_id = host.icon_id;
        icon.texture = Texture();
        icon.texture.upload_rgba8888(reinterpret_cast<const uint32_t*>(host.icon_rgba.data()),
                                     host.icon_width, host.icon_height, host.icon_width);
        // Drawn at about twice its size: smoothed, as the quick menu's is.
        icon.texture.set_linear_filtering(true);
    }
    return icon.texture.valid() ? &icon.texture : nullptr;
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
    const std::vector<PixelRect> rows =
        compute_rows(hosts.size(), output_width, output_height, scale);
    const float line = TextRenderer::line_height(scale);
    const PageFrame frame = page_frame(output_width, output_height);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    const std::string heading = std::string("Connect as the ") +
                                (wanted_screen_ == ScreenId::Top ? "top" : "bottom") + " screen";
    draw_page_header(quads, text, frame, heading);
    draw_page_status(quads, text, glyphs, frame, status, theme.text_dim);

    if (hosts.empty()) {
        text.draw_wrapped(quads,
                          "No devices found yet. Make sure a game is running on the other device, "
                          "and that both are on the same network.",
                          frame.panel.x, frame.content_top, frame.panel.w, scale, theme.text_dim);
    }

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const PixelRect& row = rows[index];
        const ds::net::DiscoveredHost& host = hosts[index];
        // Behind the alert nothing is ringed.
        const bool focused = (static_cast<int>(index) == focus_) && !needs_update_;
        const bool available = host.offers(wanted_screen_);

        draw_menu_row(quads, row, focused, scale, index + 1 == rows.size(), static_cast<int>(index) == focus_ + 1);

        const float pad = menu_row_pad(line);
        // Laid out as Data Management's rows are: the game's icon, then the
        // game's name and under it, smaller, the device's. Until the icon
        // comes in -- or from a host that does not send one -- a blank tile
        // holds its place, so the names never shift across when it lands.
        const float icon_size = line * 2.0f;
        const PixelRect icon_rect{row.x + pad, row.y + (row.h - icon_size) * 0.5f, icon_size, icon_size};
        if (const Texture* icon = icon_for(host)) {
            quads.draw_image(icon_rect, *icon, Color{1.0f, 1.0f, 1.0f, available ? 1.0f : 0.5f});
        } else {
            quads.draw_rounded_rect(icon_rect, icon_rect.w * 0.12f, Color{theme.text_dim.r, theme.text_dim.g, theme.text_dim.b, theme.text_dim.a * 0.35f});
        }
        const float text_x = row.x + pad * 1.8f + icon_size;
        const float sub_scale = scale * 0.75f;
        const float step = line * 1.3f;
        const float top = row.y + (row.h - step - TextRenderer::line_height(sub_scale)) * 0.5f;
        text.draw(quads, host.game_title.empty() ? "(no game)" : host.game_title, text_x, top,
                  scale, available ? menu_row_text(focused) : theme.text_dim);
        text.draw(quads, host_row_detail(host, available), text_x, top + step, sub_scale,
                  theme.text_dim);
        if (host.needs_update()) {
            text.draw(quads, "Needs Update", row.x + row.w - pad, std::round(row.y + (row.h - line) * 0.5f), scale,
                      theme.text_dim, TextAlign::Right);
        }
    }

    const bool on_host = focus_ >= 0 && focus_ < static_cast<int>(hosts.size());
    const char* act = !on_host                                                 ? "Choose"
                      : hosts[static_cast<std::size_t>(focus_)].needs_update() ? "Select"
                                                                                : "Connect";
    draw_page_guide(quads, text, glyphs, frame,
                    {{ButtonGlyph::A, act, PromptRole::ActsOnSelection},
                     {ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                    &guide_hits_);

    if (needs_update_) {
        const std::vector<PopupButton> buttons = alert_buttons();
        const PopupGeometry popup = alert_geometry(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, buttons, alert_cursor_.focus(),
                   alert_cursor_.shown());
        draw_popup_text(quads, text, PopupText{alert_message(), {}, 0}, popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

void LauncherScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, rows_.size());
    mix_sound_state(view.rest, static_cast<uint64_t>(confirming_ + 1));
    if (confirming_ >= 0) mix_sound_state(view.focus, static_cast<uint64_t>(confirm_cursor_.focus() + 1000));
}

void HostPickerScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, needs_update_ ? 1u : 0u);
    if (needs_update_) mix_sound_state(view.focus, static_cast<uint64_t>(alert_cursor_.focus() + 1000));
}

}  // namespace ds
