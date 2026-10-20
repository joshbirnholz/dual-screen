#include "ui/forwarder_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/notice_popup.hpp"
#include "ui/page_frame.hpp"
#include "ui/ui_theme.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// Pictures fetched at a time: enough to choose from, few enough that the
// wait for them is short.
constexpr std::size_t kBatch = 8;
constexpr int kGridColumns = 5;
// The games a choice offers, which the popup has room for.
constexpr std::size_t kGamesOffered = 5;

const char* row_label(ForwarderScreen::Row row) {
    switch (row) {
        case ForwarderScreen::Row::Publisher: return "Publisher";
        case ForwarderScreen::Row::Screenshots: return "Screenshots";
        case ForwarderScreen::Row::VideoCapture: return "Video Capture";
        case ForwarderScreen::Row::Create: return "Create Forwarder";
        case ForwarderScreen::Row::Count: break;
    }
    return "";
}

std::string quoted(const std::string& text) { return "\xE2\x80\x9C" + text + "\xE2\x80\x9D"; }

// The ring round whatever the cursor is on that is not a row of a list: the
// title box, the icon, a picture in the grid.
void draw_ring(QuadRenderer& quads, const PixelRect& rect, float scale) {
    const float ring = std::max(3.0f, std::round(scale * 1.4f));
    const float out = std::round(ring * 0.6f);
    quads.draw_outline(PixelRect{rect.x - out, rect.y - out, rect.w + out * 2.0f, rect.h + out * 2.0f},
                       ui_theme().accent, ring);
}

Color panel_fill() { return Color{0.09f, 0.10f, 0.14f, 1.0f}; }
Color box_fill() { return Color{0.06f, 0.07f, 0.10f, 1.0f}; }

void download(ForwarderPlatform& platform, const std::vector<std::string>& urls,
              std::vector<std::vector<uint8_t>>& icons, std::vector<RgbaImage>& previews) {
    for (const std::string& url : urls) {
        const HttpResponse response = platform.http_get(url, std::string());
        if (!response.reached || response.status < 200 || response.status >= 300) {
            log_info("forwarder: a SteamGridDB picture did not download (" +
                     (response.reached ? "status " + std::to_string(response.status) : response.error) +
                     ")");
            continue;
        }
        std::vector<uint8_t> icon = forwarder_icon_from_image(response.body);
        RgbaImage preview;
        if (icon.empty() || !decode_image(icon, preview)) continue;
        icons.push_back(std::move(icon));
        previews.push_back(std::move(preview));
    }
}

}  // namespace

ForwarderScreen::ForwarderScreen(ForwarderPlatform& platform) : platform_(platform) {}

ForwarderScreen::~ForwarderScreen() {
    if (worker_.joinable()) worker_.join();
}

void ForwarderScreen::open(const std::string& rom_path, const DsRomInfo& info,
                           const std::string& api_key) {
    if (worker_.joinable()) worker_.join();
    job_.reset();
    open_ = true;
    page_ = Page::Editor;
    overlay_ = Overlay::None;
    zone_ = Zone::Icon;
    list_focus_ = 0;
    api_key_ = api_key;
    request_ = ForwarderRequest();
    request_.rom_path = rom_path;
    // The name the ROM gives itself, and the publisher its banner names; the
    // file's own name for a ROM with no banner.
    request_.title = info.title;
    if (request_.title.empty()) {
        const std::size_t slash = rom_path.find_last_of("/\\");
        request_.title = slash == std::string::npos ? rom_path : rom_path.substr(slash + 1);
        const std::size_t dot = request_.title.find_last_of('.');
        if (dot != std::string::npos && dot > 0) request_.title.erase(dot);
    }
    request_.publisher = info.publisher;
    rom_icon_ = forwarder_icon_from_ds_banner(info.icon);
    if (!rom_icon_.empty()) {
        set_icon(rom_icon_, "ROM Icon");
    } else {
        set_icon(forwarder_icon_blank(), "None yet");
    }
    match_name_.clear();
    match_id_ = 0;
    search_term_.clear();
    games_.clear();
    urls_.clear();
    next_url_ = 0;
    found_.clear();
    grid_focus_ = 0;
    grid_scroll_ = 0.0f;
}

void ForwarderScreen::close() {
    open_ = false;
    found_.clear();
    icon_texture_.destroy();
    icon_texture_dirty_ = true;
}

void ForwarderScreen::set_icon(std::vector<uint8_t> jpeg, const std::string& source) {
    request_.icon = std::move(jpeg);
    icon_source_ = source;
    icon_texture_dirty_ = true;
}

void ForwarderScreen::show_notice(const std::string& message, bool closes) {
    notice_ = message;
    notice_closes_ = closes;
    overlay_ = Overlay::Notice;
}

// --- geometry ---------------------------------------------------------------

ForwarderScreen::Geometry ForwarderScreen::editor_geometry(float output_width,
                                                          float output_height) const {
    const PageFrame frame = page_frame(output_width, output_height);
    const float line = frame.line;
    const PixelRect content = frame.content();
    Geometry geometry;
    const float left_w = std::round(content.w * 0.3f);
    geometry.left = PixelRect{content.x, content.y, left_w, content.h};
    const float pad = std::round(line * 0.8f);
    geometry.title_box = PixelRect{geometry.left.x + pad, geometry.left.y + pad,
                                   geometry.left.w - pad * 2.0f, std::round(line * 3.0f)};
    // The icon as big as the panel allows, with room under it for what it
    // says about it.
    const float words = line * 4.6f;
    const float room_h = geometry.left.y + geometry.left.h - pad - words -
                         (geometry.title_box.y + geometry.title_box.h + pad);
    const float side = std::floor(std::max(line * 4.0f, std::min(geometry.left.w - pad * 3.0f, room_h)));
    geometry.icon_box = PixelRect{std::round(geometry.left.x + (geometry.left.w - side) * 0.5f),
                                  geometry.title_box.y + geometry.title_box.h + pad, side, side};
    geometry.icon_label_y = geometry.icon_box.y + geometry.icon_box.h + line * 0.5f;

    const float list_x = geometry.left.x + geometry.left.w + std::round(line * 1.5f);
    geometry.list = PixelRect{list_x, content.y, content.x + content.w - list_x, content.h};
    const float row_h = menu_row_height(line);
    const float gap = menu_row_gap(line);
    for (int index = 0; index < static_cast<int>(Row::Count); ++index) {
        geometry.rows.push_back(PixelRect{geometry.list.x, geometry.list.y + index * (row_h + gap),
                                          geometry.list.w, row_h});
    }
    return geometry;
}

ForwarderScreen::GridGeometry ForwarderScreen::grid_geometry(float output_width,
                                                            float output_height) const {
    const PageFrame frame = page_frame(output_width, output_height);
    GridGeometry grid;
    grid.view = frame.content();
    grid.columns = kGridColumns;
    grid.gap = std::round(frame.line * 0.8f);
    const float usable = grid.view.w - scroll_bar_width(frame.scale) - frame.line * 0.6f;
    grid.cell = std::floor((usable - grid.gap * (grid.columns - 1)) / grid.columns);
    const int rows = (grid_count() + grid.columns - 1) / grid.columns;
    grid.content_height = rows > 0 ? rows * (grid.cell + grid.gap) - grid.gap + grid.gap : 0.0f;
    return grid;
}

PixelRect ForwarderScreen::grid_cell(const GridGeometry& grid, int index) const {
    const int column = index % grid.columns;
    const int row = index / grid.columns;
    return PixelRect{grid.view.x + column * (grid.cell + grid.gap),
                     grid.view.y + grid.gap * 0.5f + row * (grid.cell + grid.gap) - grid_scroll_,
                     grid.cell, grid.cell};
}

int ForwarderScreen::grid_count() const {
    return static_cast<int>(found_.size()) + (has_more() ? 1 : 0);
}

std::vector<PopupButton> ForwarderScreen::source_buttons() const {
    return {{"ROM Icon", ""}, {"SteamGridDB", ""}};
}

std::vector<PopupButton> ForwarderScreen::game_buttons() const {
    std::vector<PopupButton> buttons;
    for (std::size_t index = 0; index < std::min(kGamesOffered, games_.size()); ++index) {
        const SteamGridGame& game = games_[index];
        std::string detail = game.year > 0 ? std::to_string(game.year) : std::string();
        if (game.id == match_id_ && page_ == Page::Icons) detail += detail.empty() ? "Showing" : ", showing";
        buttons.push_back({game.name, detail});
    }
    buttons.push_back({"Search for Another Name", ""});
    return buttons;
}

PopupGeometry ForwarderScreen::game_popup(float output_width, float output_height) const {
    const PopupText words{game_choice_text_, {}, 0};
    return popup_geometry(output_width, output_height,
                          popup_text_height(words, popup_body_width(output_width, output_height),
                                            output_height),
                          game_buttons());
}

PopupGeometry ForwarderScreen::source_popup(float output_width, float output_height) const {
    const PopupText words{"Where should the icon come from?", {}, 0};
    return popup_geometry(output_width, output_height,
                          popup_text_height(words, popup_body_width(output_width, output_height),
                                            output_height),
                          source_buttons());
}

PopupGeometry ForwarderScreen::busy_popup(float output_width, float output_height) const {
    const PopupText words{busy_, {}, 0};
    return popup_geometry(output_width, output_height,
                          popup_text_height(words, popup_body_width(output_width, output_height),
                                            output_height),
                          {});
}

// --- jobs -------------------------------------------------------------------

void ForwarderScreen::start_job(std::unique_ptr<Job> job, const std::string& busy) {
    if (worker_.joinable()) worker_.join();
    job_ = std::move(job);
    busy_ = busy;
    overlay_ = Overlay::Busy;
    Job* running = job_.get();
    ForwarderPlatform* platform = &platform_;
    worker_ = std::thread([running, platform] {
        switch (running->kind) {
            case Job::Kind::Search:
            case Job::Kind::Pictures: {
                const HttpGet get = [platform](const std::string& url, const std::string& bearer) {
                    return platform->http_get(url, bearer);
                };
                running->search = running->kind == Job::Kind::Pictures
                                      ? steamgrid_pictures(get, running->api_key, running->game)
                                      : steamgrid_search(get, running->api_key, running->query);
                if (running->search.error == SteamGridError::None) {
                    const std::size_t count = std::min(kBatch, running->search.urls.size());
                    running->urls.assign(running->search.urls.begin(),
                                         running->search.urls.begin() + static_cast<long>(count));
                    download(*platform, running->urls, running->icons, running->previews);
                }
                break;
            }
            case Job::Kind::More:
                download(*platform, running->urls, running->icons, running->previews);
                break;
            case Job::Kind::Install:
                running->ok = platform->install(running->request, &running->error);
                break;
        }
        running->done.store(true);
    });
}

void ForwarderScreen::finish_job(ForwarderScreenResult& result) {
    if (!job_ || !job_->done.load()) return;
    if (worker_.joinable()) worker_.join();
    std::unique_ptr<Job> job = std::move(job_);
    overlay_ = Overlay::None;
    const auto keep = [this, &job] {
        for (std::size_t index = 0; index < job->icons.size(); ++index) {
            auto found = std::make_unique<Found>();
            found->jpeg = std::move(job->icons[index]);
            found->preview = std::move(job->previews[index]);
            found_.push_back(std::move(found));
        }
        next_url_ += job->urls.size();
    };
    switch (job->kind) {
        case Job::Kind::Search:
        case Job::Kind::Pictures: {
            const SteamGridSearch& search = job->search;
            if (job->kind == Job::Kind::Search) {
                search_term_ = search.term;
                games_ = search.games;
            }
            if (search.error == SteamGridError::BadKey) {
                // Asked for again next time rather than tried again.
                api_key_.clear();
                result.api_key_changed = true;
                result.api_key.clear();
            }
            if (search.error == SteamGridError::BadKey || search.error == SteamGridError::Network) {
                show_notice(describe(search.error));
                return;
            }
            if (search.error == SteamGridError::NotFound) {
                if (games_.empty()) {
                    choose_game("SteamGridDB knows no game called " + quoted(job->query) +
                                ". It might know it by another name.");
                } else if (job->kind == Job::Kind::Pictures) {
                    choose_game("SteamGridDB has no pictures of " + quoted(search.match.name) +
                                ". Is it one of these?");
                } else {
                    choose_game("SteamGridDB has no pictures of the games it matched to " +
                                quoted(search_term_) + ". Is it one of these?");
                }
                return;
            }
            std::vector<std::unique_ptr<Found>> previous = std::move(found_);
            const std::size_t previous_next = next_url_;
            found_.clear();
            next_url_ = 0;
            keep();
            if (found_.empty()) {
                found_ = std::move(previous);
                next_url_ = previous_next;
                show_notice("None of SteamGridDB's pictures of " + quoted(search.match.name) +
                            " could be downloaded.");
                return;
            }
            match_name_ = search.match.name;
            match_id_ = search.match.id;
            urls_ = search.urls;
            next_url_ = job->urls.size();
            log_info("forwarder: SteamGridDB matched " + match_name_ + ", " + std::to_string(urls_.size()) +
                     " pictures");
            page_ = Page::Icons;
            grid_focus_ = 0;
            grid_scroll_ = 0.0f;
            grid_drag_.stop();
            return;
        }
        case Job::Kind::More: {
            const std::size_t before = found_.size();
            keep();
            if (found_.size() == before) show_notice("No more pictures could be downloaded.");
            return;
        }
        case Job::Kind::Install:
            if (job->ok) {
                log_info("forwarder: installed " + job->request.title + " for " + job->request.rom_path);
                show_notice("Installed. " + job->request.title + " is on the HOME menu.", true);
            } else {
                log_warn("forwarder: not installed: " + job->error);
                show_notice("The forwarder could not be installed: " + job->error);
            }
            return;
    }
}

// --- actions ----------------------------------------------------------------

void ForwarderScreen::edit_title() {
    std::string typed;
    if (platform_.edit_text("App Title", "The name the HOME menu will show", request_.title,
                            kForwarderTitleMax, typed) &&
        !typed.empty()) {
        request_.title = typed;
    }
}

void ForwarderScreen::choose_icon_source() {
    overlay_ = Overlay::IconSource;
    popup_cursor_.reset(2, 1);
}

bool ForwarderScreen::ensure_api_key(ForwarderScreenResult& result) {
    if (!api_key_.empty()) return true;
    std::string key;
    if (!platform_.edit_text("SteamGridDB API Key", "Your API key, from steamgriddb.com: Preferences, API", "",
                             256, key) ||
        key.empty()) {
        show_notice("Searching SteamGridDB takes an API key, which anyone can have for free "
                    "from steamgriddb.com.");
        return false;
    }
    api_key_ = key;
    result.api_key_changed = true;
    result.api_key = key;
    return true;
}

void ForwarderScreen::search_steamgriddb(ForwarderScreenResult& result, const std::string& term) {
    if (!ensure_api_key(result)) return;
    auto job = std::make_unique<Job>();
    job->kind = Job::Kind::Search;
    job->api_key = api_key_;
    job->query = term;
    start_job(std::move(job), "Searching SteamGridDB for " + quoted(term) + "...");
}

void ForwarderScreen::search_another_name(ForwarderScreenResult& result) {
    std::string typed;
    const std::string start = search_term_.empty() ? request_.title : search_term_;
    if (!platform_.edit_text("Search SteamGridDB", "The game's name, as SteamGridDB might know it", start, 128,
                             typed) ||
        typed.empty()) {
        return;
    }
    search_steamgriddb(result, typed);
}

void ForwarderScreen::show_pictures_of(const SteamGridGame& game) {
    auto job = std::make_unique<Job>();
    job->kind = Job::Kind::Pictures;
    job->api_key = api_key_;
    job->game = game;
    job->query = game.name;
    start_job(std::move(job), "Looking for pictures of " + quoted(game.name) + "...");
}

void ForwarderScreen::choose_game(const std::string& message) {
    game_choice_text_ = message;
    overlay_ = Overlay::GameChoice;
    popup_cursor_.reset(static_cast<int>(game_buttons().size()), 0);
}

void ForwarderScreen::load_more() {
    if (!has_more()) return;
    auto job = std::make_unique<Job>();
    job->kind = Job::Kind::More;
    const std::size_t end = std::min(urls_.size(), next_url_ + kBatch);
    job->urls.assign(urls_.begin() + static_cast<long>(next_url_), urls_.begin() + static_cast<long>(end));
    start_job(std::move(job), "Loading more pictures...");
}

void ForwarderScreen::create() {
    if (request_.title.empty()) {
        show_notice("The forwarder needs a title.");
        return;
    }
    if (request_.icon.empty()) request_.icon = forwarder_icon_blank();
    auto job = std::make_unique<Job>();
    job->kind = Job::Kind::Install;
    job->request = request_;
    start_job(std::move(job), "Installing " + quoted(request_.title) + "...");
}

void ForwarderScreen::activate(ForwarderScreenResult& result) {
    (void)result;
    switch (zone_) {
        case Zone::Title: edit_title(); return;
        case Zone::Icon: choose_icon_source(); return;
        case Zone::List: break;
    }
    switch (static_cast<Row>(list_focus_)) {
        case Row::Publisher: {
            std::string typed;
            if (platform_.edit_text("Publisher", "Who made the game", request_.publisher,
                                    kForwarderPublisherMax, typed)) {
                request_.publisher = typed;
            }
            break;
        }
        case Row::Screenshots: request_.screenshots = !request_.screenshots; break;
        case Row::VideoCapture: request_.video_capture = !request_.video_capture; break;
        case Row::Create: create(); break;
        case Row::Count: break;
    }
}

// --- update -----------------------------------------------------------------

ForwarderScreenResult ForwarderScreen::update(const std::vector<UiAction>& actions,
                                              const std::vector<PointerEvent>& pointers,
                                              float output_width, float output_height) {
    ForwarderScreenResult result;
    if (!open_) return result;
    finish_job(result);

    if (overlay_ == Overlay::Busy) return result;
    if (overlay_ == Overlay::Notice) {
        if (notice_popup_answered(actions, pointers, output_width, output_height, notice_)) {
            overlay_ = Overlay::None;
            if (notice_closes_) {
                result.close_requested = true;
                close();
            }
        }
        return result;
    }
    const std::vector<int> taps = take_taps(pointers, output_width, output_height);
    if (overlay_ == Overlay::IconSource) {
        const int answer = answer_popup(actions, taps, source_popup(output_width, output_height));
        if (answer == PopupCursor::kCancelled) {
            overlay_ = Overlay::None;
        } else if (answer == 0) {
            overlay_ = Overlay::None;
            if (rom_icon_.empty()) {
                show_notice("This ROM has no icon of its own.");
            } else {
                set_icon(rom_icon_, "ROM Icon");
            }
        } else if (answer == 1) {
            overlay_ = Overlay::None;
            search_steamgriddb(result, request_.title);
        }
        return result;
    }
    if (overlay_ == Overlay::GameChoice) {
        const int offered = static_cast<int>(game_buttons().size()) - 1;
        const int answer = answer_popup(actions, taps, game_popup(output_width, output_height));
        if (answer == PopupCursor::kCancelled) {
            overlay_ = Overlay::None;
        } else if (answer >= 0 && answer < offered) {
            overlay_ = Overlay::None;
            show_pictures_of(games_[static_cast<std::size_t>(answer)]);
        } else if (answer == offered) {
            overlay_ = Overlay::None;
            search_another_name(result);
        }
        return result;
    }

    if (page_ == Page::Icons) {
        update_icons(actions, taps, output_width, output_height);
    } else {
        update_editor(actions, taps, result);
    }
    return result;
}

int ForwarderScreen::answer_popup(const std::vector<UiAction>& actions, const std::vector<int>& taps,
                                  const PopupGeometry& popup) {
    int answer = popup_cursor_.update(actions, {}, popup);
    for (const int tap : taps) {
        if (answer != PopupCursor::kNone) break;
        if (tap == kTapOffPopup) answer = PopupCursor::kCancelled;
        if (tap >= kPopupEntryBase && tap < kGuideEntryBase) answer = popup_cursor_.tap(tap - kPopupEntryBase);
    }
    return answer;
}

bool ForwarderScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                             float output_height) {
    if (!open_ || page_ != Page::Icons || overlay_ != Overlay::None) return false;
    const GridGeometry grid = grid_geometry(output_width, output_height);
    grid_drag_.update(pointers, grid.view, grid_scroll_, std::max(0.0f, grid.content_height - grid.view.h));
    return grid_drag_.release_was_drag();
}

int ForwarderScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (!open_) return -1;
    if (asking()) {
        const int button = popup_button_at(asking_popup(output_width, output_height), x, y);
        return button >= 0 ? kPopupEntryBase + button : -1;
    }
    if (overlay_ != Overlay::None) return -1;
    const ButtonGlyph on_guide = guide_button_at(guide_hits_, x, y);
    if (on_guide != ButtonGlyph::Count) return kGuideEntryBase + static_cast<int>(on_guide);
    if (page_ == Page::Icons) {
        const GridGeometry grid = grid_geometry(output_width, output_height);
        if (!grid.view.contains(x, y)) return -1;
        const int count = grid_count();
        for (int index = 0; index < count; ++index) {
            if (grid_cell(grid, index).contains(x, y)) return index;
        }
        return -1;
    }
    const Geometry geometry = editor_geometry(output_width, output_height);
    if (geometry.title_box.contains(x, y)) return kTitleEntry;
    if (geometry.icon_box.contains(x, y)) return kIconEntry;
    for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
        if (geometry.rows[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
}

std::vector<float> ForwarderScreen::scroll_state(float output_width, float output_height) const {
    const GridGeometry grid = grid_geometry(output_width, output_height);
    const float step = grid.cell + grid.gap;
    return {step > 0.0f ? grid_scroll_ / step : 0.0f};
}

void ForwarderScreen::set_scroll_state(const std::vector<float>& state, float output_width,
                                       float output_height) {
    const GridGeometry grid = grid_geometry(output_width, output_height);
    grid_scroll_ = std::clamp(scroll_at(state, 0) * (grid.cell + grid.gap), 0.0f,
                              std::max(0.0f, grid.content_height - grid.view.h));
    grid_drag_.stop();
}

bool ForwarderScreen::off_popup(float x, float y, float output_width, float output_height) const {
    return open_ && asking() && !asking_popup(output_width, output_height).box.contains(x, y);
}

void ForwarderScreen::update_editor(const std::vector<UiAction>& input_actions,
                                    const std::vector<int>& taps, ForwarderScreenResult& result) {
    std::vector<UiAction> actions = input_actions;
    const int rows = static_cast<int>(Row::Count);
    for (const int tap : taps) {
        if (tap >= kGuideEntryBase) {
            const ButtonGlyph on_guide = static_cast<ButtonGlyph>(tap - kGuideEntryBase);
            if (on_guide == ButtonGlyph::A) actions.push_back(UiAction::Confirm);
            if (on_guide == ButtonGlyph::B) actions.push_back(UiAction::Cancel);
            continue;
        }
        // A first tap puts the cursor on a thing; a second on it chooses it.
        const auto tap_zone = [&](Zone zone) {
            if (zone_ == zone) {
                actions.push_back(UiAction::Confirm);
            } else {
                zone_ = zone;
            }
        };
        if (tap == kTitleEntry) {
            tap_zone(Zone::Title);
        } else if (tap == kIconEntry) {
            tap_zone(Zone::Icon);
        } else if (tap >= 0 && tap < rows) {
            if (zone_ == Zone::List && list_focus_ == tap) {
                actions.push_back(UiAction::Confirm);
            } else {
                zone_ = Zone::List;
                list_focus_ = tap;
            }
        }
    }

    for (const UiAction action : actions) {
        if (overlay_ != Overlay::None || result.close_requested) break;
        switch (action) {
            case UiAction::Up:
                if (zone_ == Zone::Icon) zone_ = Zone::Title;
                if (zone_ == Zone::List) list_focus_ = std::max(0, list_focus_ - 1);
                break;
            case UiAction::Down:
                if (zone_ == Zone::Title) {
                    zone_ = Zone::Icon;
                } else if (zone_ == Zone::List) {
                    list_focus_ = std::min(rows - 1, list_focus_ + 1);
                }
                break;
            case UiAction::Left:
                if (zone_ == Zone::List) zone_ = Zone::Icon;
                break;
            case UiAction::Right:
                if (zone_ != Zone::List) zone_ = Zone::List;
                break;
            case UiAction::Confirm: activate(result); break;
            case UiAction::Cancel:
                result.close_requested = true;
                close();
                break;
            default: break;
        }
    }
}

void ForwarderScreen::scroll_grid_to_focus(float output_width, float output_height) {
    const GridGeometry grid = grid_geometry(output_width, output_height);
    const float top = (grid_focus_ / grid.columns) * (grid.cell + grid.gap);
    const float bottom = top + grid.cell + grid.gap;
    if (top < grid_scroll_) grid_scroll_ = top;
    if (bottom > grid_scroll_ + grid.view.h) grid_scroll_ = bottom - grid.view.h;
    grid_scroll_ = std::clamp(grid_scroll_, 0.0f, std::max(0.0f, grid.content_height - grid.view.h));
}

void ForwarderScreen::update_icons(const std::vector<UiAction>& input_actions,
                                   const std::vector<int>& taps, float output_width,
                                   float output_height) {
    std::vector<UiAction> actions = input_actions;
    const GridGeometry grid = grid_geometry(output_width, output_height);
    const int count = grid_count();
    const auto choose = [this](int index) {
        if (index < static_cast<int>(found_.size())) {
            set_icon(found_[static_cast<std::size_t>(index)]->jpeg, "SteamGridDB");
            page_ = Page::Editor;
            zone_ = Zone::Icon;
        } else {
            load_more();
        }
    };
    for (const int tap : taps) {
        if (tap >= kGuideEntryBase) {
            const ButtonGlyph on_guide = static_cast<ButtonGlyph>(tap - kGuideEntryBase);
            if (on_guide == ButtonGlyph::A) actions.push_back(UiAction::Confirm);
            if (on_guide == ButtonGlyph::B) actions.push_back(UiAction::Cancel);
            if (on_guide == ButtonGlyph::Y) actions.push_back(UiAction::MenuReset);
            continue;
        }
        if (tap >= 0 && tap < count && tap_chooses(grid_focus_, tap)) choose(tap);
    }
    grid_drag_.settle();
    bool moved = false;
    for (const UiAction action : actions) {
        if (page_ != Page::Icons || overlay_ != Overlay::None) break;
        switch (action) {
            case UiAction::Left: grid_focus_ = std::max(0, grid_focus_ - 1); moved = true; break;
            case UiAction::Right: grid_focus_ = std::min(count - 1, grid_focus_ + 1); moved = true; break;
            case UiAction::Up:
                if (grid_focus_ - grid.columns >= 0) grid_focus_ -= grid.columns;
                moved = true;
                break;
            case UiAction::Down:
                if (grid_focus_ + grid.columns < count) {
                    grid_focus_ += grid.columns;
                } else if (grid_focus_ / grid.columns < (count - 1) / grid.columns) {
                    grid_focus_ = count - 1;
                }
                moved = true;
                break;
            case UiAction::Confirm: choose(grid_focus_); break;
            case UiAction::Cancel: page_ = Page::Editor; break;
            case UiAction::MenuReset: choose_game("Which game is it?"); break;
            default: break;
        }
    }
    if (moved) {
        grid_drag_.stop();
        scroll_grid_to_focus(output_width, output_height);
    }
}

// --- render -----------------------------------------------------------------

void ForwarderScreen::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                             float output_width, float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = frame.scale;
    const float line = frame.line;
    draw_page_background(quads, output_width, output_height, false);

    if (page_ == Page::Icons) {
        draw_page_header(quads, text, frame, "Choose an Icon", match_name_);
        const GridGeometry grid = grid_geometry(output_width, output_height);
        quads.set_clip(grid.view);
        const int count = grid_count();
        for (int index = 0; index < count; ++index) {
            const PixelRect cell = grid_cell(grid, index);
            if (cell.y + cell.h < grid.view.y || cell.y > grid.view.y + grid.view.h) continue;
            quads.draw_rounded_rect(cell, line * 0.3f, panel_fill());
            if (index < static_cast<int>(found_.size())) {
                Found& found = *found_[static_cast<std::size_t>(index)];
                if (!found.texture.valid() && !found.preview.empty()) {
                    found.texture.upload_rgba8888(reinterpret_cast<const uint32_t*>(found.preview.pixels.data()),
                                                  found.preview.width, found.preview.height,
                                                  found.preview.width);
                    found.texture.set_linear_filtering(true);
                }
                const float inset = std::round(line * 0.35f);
                if (found.texture.valid()) {
                    quads.draw_image(PixelRect{cell.x + inset, cell.y + inset, cell.w - inset * 2.0f,
                                               cell.h - inset * 2.0f},
                                     found.texture);
                }
            } else {
                const float mid = cell.y + cell.h * 0.5f;
                text.draw(quads, "+", cell.x + cell.w * 0.5f, mid - line * 2.2f, scale * 2.0f, theme.text,
                          TextAlign::Center);
                text.draw(quads, "Load More", cell.x + cell.w * 0.5f, mid + line * 0.2f, scale, theme.text,
                          TextAlign::Center);
                text.draw(quads, std::to_string(urls_.size() - next_url_) + " remaining",
                          cell.x + cell.w * 0.5f, mid + line * 1.5f, scale * 0.8f, theme.text_dim,
                          TextAlign::Center);
            }
            if (index == grid_focus_ && overlay_ == Overlay::None) draw_ring(quads, cell, scale);
        }
        quads.clear_clip();
        if (grid.content_height > grid.view.h + 0.5f) {
            draw_scroll_bar(quads, grid.view, grid_scroll_ / grid.content_height,
                            grid.view.h / grid.content_height, line, scale);
        }
    } else {
        draw_page_header(quads, text, frame, "Forwarder Creation");
        const Geometry geometry = editor_geometry(output_width, output_height);
        quads.draw_rounded_rect(geometry.left, line * 0.4f, panel_fill());

        // The title, in a box of its own at the top of the panel.
        const bool title_focused = zone_ == Zone::Title && overlay_ == Overlay::None;
        quads.draw_rounded_rect(geometry.title_box, line * 0.3f, box_fill());
        const float box_pad = std::round(line * 0.7f);
        const float label_scale = scale * 0.75f;
        const float block = TextRenderer::line_height(label_scale) + line * 0.2f + line;
        const float label_y = geometry.title_box.y + (geometry.title_box.h - block) * 0.5f;
        text.draw(quads, "App Title", geometry.title_box.x + box_pad, label_y, label_scale, theme.text_dim);
        text.draw(quads,
                  TextRenderer::elide_right(request_.title, geometry.title_box.w - box_pad * 2.0f, scale),
                  geometry.title_box.x + box_pad, label_y + TextRenderer::line_height(label_scale) + line * 0.2f,
                  scale, title_focused ? theme.accent : theme.text);
        if (title_focused) draw_ring(quads, geometry.title_box, scale);

        // The icon, as the HOME menu will show it.
        if (icon_texture_dirty_) {
            icon_texture_dirty_ = false;
            RgbaImage preview;
            if (decode_image(request_.icon, preview)) {
                icon_texture_.upload_rgba8888(reinterpret_cast<const uint32_t*>(preview.pixels.data()),
                                              preview.width, preview.height, preview.width);
                icon_texture_.set_linear_filtering(true);
            } else {
                icon_texture_.destroy();
            }
        }
        if (icon_texture_.valid()) {
            quads.draw_image(geometry.icon_box, icon_texture_);
        } else {
            quads.draw_solid(geometry.icon_box, box_fill());
        }
        const bool icon_focused = zone_ == Zone::Icon && overlay_ == Overlay::None;
        if (icon_focused) draw_ring(quads, geometry.icon_box, scale);
        const float centre = geometry.left.x + geometry.left.w * 0.5f;
        float pen_y = geometry.icon_label_y;
        text.draw(quads, "Icon", centre, pen_y, scale * 1.05f, icon_focused ? theme.accent : theme.text,
                  TextAlign::Center);
        pen_y += line * 1.3f;
        text.draw(quads, icon_source_, centre, pen_y, scale * 0.85f, theme.text_dim, TextAlign::Center);
        pen_y += line * 1.3f;
        text.draw_wrapped(quads, "Press A or tap the icon to change it", centre, pen_y,
                          geometry.left.w - line * 1.6f, scale * 0.75f, theme.text_dim, TextAlign::Center);

        // The rows.
        for (int index = 0; index < static_cast<int>(Row::Count); ++index) {
            const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
            const bool focused = zone_ == Zone::List && list_focus_ == index && overlay_ == Overlay::None;
            draw_menu_row(quads, row, focused, scale, index == static_cast<int>(Row::Count) - 1,
                          zone_ == Zone::List && list_focus_ + 1 == index && overlay_ == Overlay::None);
            const Row which = static_cast<Row>(index);
            const float pad = menu_row_pad(line);
            if (which == Row::Create) {
                text.draw(quads, row_label(which), row.x + row.w * 0.5f, page_row_text_y(row, scale), scale,
                          menu_row_text(focused), TextAlign::Center);
                continue;
            }
            text.draw(quads, row_label(which), row.x + pad, page_row_text_y(row, scale), scale,
                      menu_row_text(focused));
            std::string value;
            switch (which) {
                case Row::Publisher: value = request_.publisher.empty() ? "None" : request_.publisher; break;
                case Row::Screenshots: value = request_.screenshots ? "On" : "Off"; break;
                case Row::VideoCapture: value = request_.video_capture ? "On" : "Off"; break;
                default: break;
            }
            const float label_w = TextRenderer::measure_width(row_label(which), scale);
            text.draw(quads, TextRenderer::elide_right(value, row.w - label_w - pad * 3.0f, scale * 0.9f),
                      row.x + row.w - pad, page_row_text_y(row, scale * 0.9f), scale * 0.9f,
                      theme.text_dim, TextAlign::Right);
        }
    }

    guide_hits_.clear();
    std::vector<GuidePrompt> prompts;
    if (page_ == Page::Icons) prompts.push_back({ButtonGlyph::Y, "Other Game"});
    prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    prompts.push_back({ButtonGlyph::A, "Select", PromptRole::ActsOnSelection});
    draw_page_guide(quads, text, glyphs, frame, prompts, &guide_hits_);

    switch (overlay_) {
        case Overlay::IconSource: {
            const PopupGeometry popup = source_popup(output_width, output_height);
            draw_popup(quads, text, output_width, output_height, popup, source_buttons(),
                       popup_cursor_.focus(), popup_cursor_.shown());
            draw_popup_text(quads, text, PopupText{"Where should the icon come from?", {}, 0}, popup.body.x,
                            popup.body.y, popup.body.w, output_height);
            break;
        }
        case Overlay::GameChoice: {
            const PopupGeometry popup = game_popup(output_width, output_height);
            draw_popup(quads, text, output_width, output_height, popup, game_buttons(), popup_cursor_.focus(),
                       popup_cursor_.shown());
            draw_popup_text(quads, text, PopupText{game_choice_text_, {}, 0}, popup.body.x, popup.body.y,
                            popup.body.w, output_height);
            break;
        }
        case Overlay::Busy: {
            const PopupGeometry popup = busy_popup(output_width, output_height);
            draw_popup(quads, text, output_width, output_height, popup, {}, 0, false);
            draw_popup_text(quads, text, PopupText{busy_, {}, 0}, popup.body.x, popup.body.y, popup.body.w,
                            output_height);
            break;
        }
        case Overlay::Notice:
            render_notice_popup(quads, text, glyphs, output_width, output_height, notice_);
            break;
        case Overlay::None: break;
    }
}

void ForwarderScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(zone_));
    mix_sound_state(view.focus, static_cast<uint64_t>(list_focus_));
    mix_sound_state(view.focus, static_cast<uint64_t>(grid_focus_));
    if (overlay_ != Overlay::None) mix_sound_state(view.focus, static_cast<uint64_t>(popup_cursor_.focus()));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(page_));
    mix_sound_state(view.rest, static_cast<uint64_t>(overlay_));
    mix_sound_state(view.rest, icon_source_);
    mix_sound_state(view.rest, notice_);
    mix_sound_state(view.rest, busy_.empty() ? 0u : 1u);
}

}  // namespace ds
