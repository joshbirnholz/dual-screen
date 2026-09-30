#include "ui/data_management_screen.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "core/ds_rom_info.hpp"
#include "core/save_points.hpp"
#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"
#include "ui/notice_popup.hpp"
#include "ui/ui_theme.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// The cells of assets/ui_icons.png, in order.
enum class UiIcon { Get, Send, History, Delete, SteamDeck, Switch, Pc, Sync, Count };

UiIcon device_icon(const std::string& kind) {
    if (kind == net::kDeviceKindSteamDeck) return UiIcon::SteamDeck;
    // No open icon of a Switch Lite exists, so a Lite is drawn as a Switch.
    if (kind == net::kDeviceKindSwitch || kind == net::kDeviceKindSwitchLite) return UiIcon::Switch;
    return UiIcon::Pc;
}

std::tm local_time(int64_t when) {
    const std::time_t time = static_cast<std::time_t>(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return local;
}

// "Today, 4:12 PM", "Yesterday, 21:40", and the save points' own form for
// anything older.
std::string describe_time(int64_t when) {
    if (when <= 0) return "Unknown date";
    const TimeStyle style = device_time_style();
    const std::tm then = local_time(when);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::tm today = local_time(now);
    const std::tm yesterday = local_time(now - 24 * 60 * 60);
    const auto same_day = [](const std::tm& a, const std::tm& b) {
        return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
    };
    const char* day = same_day(then, today) ? "Today" : same_day(then, yesterday) ? "Yesterday" : nullptr;
    if (day == nullptr) {
        // "09/24/2026, 11:50 PM": a comma between the two, as a list reads.
        std::string text = format_save_time(when, style);
        const std::size_t dash = text.find(" - ");
        if (dash != std::string::npos) text.replace(dash, 3, ", ");
        return text;
    }
    char clock[32];
    if (style.twelve_hour) {
        const int hour = then.tm_hour % 12 == 0 ? 12 : then.tm_hour % 12;
        std::snprintf(clock, sizeof clock, "%d:%02d %s", hour, then.tm_min,
                      then.tm_hour < 12 ? "AM" : "PM");
    } else {
        std::snprintf(clock, sizeof clock, "%02d:%02d", then.tm_hour, then.tm_min);
    }
    return std::string(day) + ", " + clock;
}

// Shortened from the end to fit, for a title too long for its row.
std::string elide_right(const std::string& text, float max_width, float scale) {
    if (TextRenderer::measure_width(text, scale) <= max_width) return text;
    std::string cut = text;
    while (!cut.empty() && TextRenderer::measure_width(cut + "...", scale) > max_width) {
        cut.pop_back();
        // Never half a character.
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0xC0) cut.pop_back();
    }
    while (!cut.empty() && cut.back() == ' ') cut.pop_back();
    return cut + "...";
}

Color with_alpha(Color color, float alpha) {
    color.a = alpha;
    return color;
}

struct Frame {
    float scale = 1.0f;
    float line = 0.0f;
    PixelRect panel;
    float header_bottom = 0.0f;
    float content_top = 0.0f;
    float content_bottom = 0.0f;
    float guide_y = 0.0f;
};

Frame frame_for(float output_width, float output_height) {
    Frame frame;
    frame.scale = UiTheme::base_scale(output_height);
    frame.line = TextRenderer::line_height(frame.scale);
    const float margin_x = std::round(output_width * 0.06f);
    const float margin_y = std::round(output_height * 0.06f);
    frame.panel = PixelRect{margin_x, margin_y, output_width - margin_x * 2.0f,
                            output_height - margin_y * 2.0f};
    frame.header_bottom = frame.panel.y + frame.line * 2.6f;
    frame.content_top = frame.header_bottom + frame.line * 0.6f;
    const float guide_height = control_guide_height(frame.scale * 0.9f);
    frame.guide_y = frame.panel.y + frame.panel.h - guide_height;
    frame.content_bottom = frame.guide_y - frame.line * 0.9f;
    return frame;
}

// The part of a row inside the list, for a finger: a row half scrolled out of
// sight can only be touched where it shows.
PixelRect visible_part(const PixelRect& row, const PixelRect& list) {
    const float top = std::max(row.y, list.y);
    const float bottom = std::min(row.y + row.h, list.y + list.h);
    return PixelRect{row.x, top, row.w, std::max(0.0f, bottom - top)};
}

}  // namespace

DataManagementScreen::DataManagementScreen(SaveStore& store, std::string own_instance)
    : store_(store), own_instance_(std::move(own_instance)) {
    search_.own_instance = own_instance_;
}

DataManagementScreen::~DataManagementScreen() {
    if (worker_.joinable()) worker_.join();
}

std::string DataManagementScreen::describe_count(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " save" : " saves");
}

void DataManagementScreen::open(const std::string& game_in_play) {
    open_ = true;
    game_in_play_ = game_in_play;
    own_name_ = store_.device_name();
    own_kind_ = net::this_device_kind();
    overlay_ = Overlay::None;
    page_ = Page::List;
    list_focus_ = 0;
    list_scroll_ = 0.0f;
    sync_all_ = false;
    plan_queued_ = false;
    seen_changes_ = change_source_ ? change_source_() : 0;
    reload_saves();
    // A game's page looks for devices as it opens; the list waits to be
    // asked, by Sync All.
    if (in_game()) {
        open_game(game_in_play_);
    }
    log_info(in_game() ? "Data Management is up, on the page for the game being played"
                       : "Data Management is up");
}

void DataManagementScreen::close() { open_ = false; }

void DataManagementScreen::reload_saves() {
    saves_.clear();
    for (SaveInfo& info : store_.list()) {
        // A save whose game is not known cannot be matched with another
        // device's; it is listed once its ROM has been played or found.
        if (!info.game_id.empty()) saves_.push_back(std::move(info));
    }
    list_focus_ = std::min(list_focus_, std::max(0, static_cast<int>(saves_.size()) - 1));
}

void DataManagementScreen::open_game(const std::string& game_id) {
    SaveInfo info;
    if (!store_.find(game_id, info)) {
        info = SaveInfo();
        info.game_id = game_id;
    }
    const bool same_game = game_.game_id == game_id;
    game_ = info;
    page_ = Page::Game;
    if (!same_game) {
        action_focus_ = 0;
        action_focus_chosen_ = false;
    }
    if (status_game_id_ != game_id) {
        remote_.clear();
        status_game_id_.clear();
    }
    // Who is around is looked at afresh each time a page opens, so a device
    // switched on since is there without being asked for.
    start_search();
    settle_action_focus();
}

void DataManagementScreen::refresh_game() {
    SaveInfo fresh;
    if (store_.find(game_.game_id, fresh)) game_ = fresh;
    settle_action_focus();
}

void DataManagementScreen::check_for_changes() {
    if (!change_source_) return;
    const uint64_t now = change_source_();
    if (now == seen_changes_) return;
    seen_changes_ = now;
    // Another device has just sent a save here: whatever is on screen is
    // read again, and the other devices asked again about this game.
    log_info("Data Management: a save arrived from another device; looking again");
    reload_saves();
    if (page_ == Page::Game) {
        refresh_game();
        start_status();
    } else if (page_ == Page::Earlier) {
        refresh_game();
        earlier_ = store_.earlier(game_.game_id);
        earlier_focus_ = std::min(earlier_focus_, std::max(0, static_cast<int>(earlier_.size()) - 1));
    }
}

void DataManagementScreen::settle_action_focus() {
    // Until the player moves it, the cursor is on the first thing that can be
    // done -- which changes as the other device answers.
    // While the network is still being asked, where it will most likely
    // end up: on Get Save, which does nothing until an answer comes. Moved
    // to the first thing that can be done only once there is an answer, so
    // a press made early never lands on something else.
    if (job_ && !action_focus_chosen_) {
        action_focus_ = 0;
        return;
    }
    const std::vector<bool> enabled = actions_enabled();
    if (action_focus_chosen_) {
        if (enabled[static_cast<std::size_t>(action_focus_)]) return;
    }
    for (std::size_t index = 0; index < enabled.size(); ++index) {
        if (enabled[index]) {
            action_focus_ = static_cast<int>(index);
            return;
        }
    }
}

void DataManagementScreen::start_job(std::unique_ptr<Job> job) {
    if (worker_.joinable()) worker_.join();
    job_ = std::move(job);
    Job* running = job_.get();
    SaveStore* store = &store_;
    const net::SaveSearch search = search_;
    worker_ = std::thread([running, store, search] {
        const auto statuses = [running] {
            for (const net::SaveDevice& device : running->devices) {
                RemoteState state;
                std::string error;
                state.answered = net::ask_save_status(device, kSaveSystemDs, running->game_id,
                                                      state.status, &error);
                if (!state.answered) {
                    log_info("Data Management: " + device.name + " did not answer: " + error);
                }
                running->remote[device.instance] = state;
            }
        };
        switch (running->kind) {
            case Job::Kind::Search: {
                for (net::SaveDevice& device : net::search_save_devices(search)) {
                    if (device.keeps(kSaveSystemDs)) running->devices.push_back(device);
                }
                if (!running->game_id.empty()) statuses();
                running->ok = true;
                break;
            }
            case Job::Kind::Status:
                statuses();
                running->ok = true;
                break;
            case Job::Kind::Plan:
                for (net::SaveDevice& device : net::search_save_devices(search)) {
                    if (!device.keeps(kSaveSystemDs)) continue;
                    std::vector<net::RemoteSave> saves;
                    std::string error;
                    if (net::list_remote_saves(device, saves, &error)) {
                        running->lists.emplace_back(device, std::move(saves));
                    } else {
                        log_info("Data Management: " + device.name + " did not say what it has: " + error);
                    }
                }
                running->ok = true;
                break;
            case Job::Kind::Fetch:
                running->ok = net::fetch_save(running->device, running->game_id, running->info,
                                              running->bytes, &running->error);
                break;
            case Job::Kind::Send: {
                SaveInfo info;
                std::vector<uint8_t> bytes;
                running->ok = store->read_for_sending(running->game_id, info, bytes, &running->error) &&
                              net::send_save(running->device, info, bytes, store->device_name(),
                                             &running->error);
                break;
            }
        }
        running->done = true;
    });
}

void DataManagementScreen::start_search() {
    if (job_) {
        search_queued_ = true;
        return;
    }
    remote_.clear();
    status_game_id_.clear();
    std::unique_ptr<Job> job(new Job());
    job->kind = Job::Kind::Search;
    if (page_ != Page::List) job->game_id = game_.game_id;
    start_job(std::move(job));
}

void DataManagementScreen::start_status() {
    if (page_ == Page::List || game_.game_id.empty()) return;
    if (job_) {
        status_queued_ = true;
        return;
    }
    std::unique_ptr<Job> job(new Job());
    job->kind = Job::Kind::Status;
    job->game_id = game_.game_id;
    job->devices = devices_;
    start_job(std::move(job));
}

const net::SaveDevice* DataManagementScreen::device() const {
    if (page_ == Page::List || status_game_id_ != game_.game_id) return nullptr;
    if (device_index_ >= devices_.size()) return nullptr;
    return &devices_[device_index_];
}

const DataManagementScreen::RemoteState* DataManagementScreen::remote() const {
    const net::SaveDevice* shown = device();
    if (shown == nullptr) return nullptr;
    const auto found = remote_.find(shown->instance);
    return found != remote_.end() && found->second.answered ? &found->second : nullptr;
}

SaveComparison DataManagementScreen::comparison() const {
    const RemoteState* there = remote();
    if (there == nullptr) return SaveComparison::Neither;
    return compare_saves(version_state(game_), there->status.versions);
}

std::string DataManagementScreen::comparison_text() const {
    const net::SaveDevice* shown = device();
    const RemoteState* there = remote();
    if (shown == nullptr || there == nullptr) return std::string();
    if (there->status.playing_now) return "Currently playing on " + shown->name;
    switch (comparison()) {
        case SaveComparison::UpToDate: return "Up to date";
        case SaveComparison::NewerHere: return "Newer on " + own_name_;
        case SaveComparison::NewerThere: return "Newer on " + shown->name;
        case SaveComparison::Conflict: return "Save file conflict";
        case SaveComparison::OnlyHere: return "Only on " + own_name_;
        case SaveComparison::OnlyThere: return "Only on " + shown->name;
        case SaveComparison::Neither: break;
    }
    return std::string();
}

bool DataManagementScreen::comparison_is_conflict() const {
    const RemoteState* there = remote();
    return there != nullptr && !there->status.playing_now &&
           comparison() == SaveComparison::Conflict;
}

std::vector<std::string> DataManagementScreen::action_labels() const {
    const net::SaveDevice* shown = remote() != nullptr ? device() : nullptr;
    return {shown != nullptr ? "Sync with " + shown->name : std::string("Sync"),
            shown != nullptr ? "Get Save from " + shown->name : std::string("Get Save"),
            shown != nullptr ? "Send Save to " + shown->name : std::string("Send Save"),
            "Earlier Saves", "Delete Save Data"};
}

std::vector<bool> DataManagementScreen::actions_enabled() const {
    const RemoteState* there = remote();
    const bool reachable = there != nullptr && !there->status.playing_now && !job_;
    return {reachable && (there->status.versions.has_save || game_.has_save),
            reachable && there->status.versions.has_save, reachable && game_.has_save, true,
            game_.has_save};
}

void DataManagementScreen::show_notice(const std::string& message) {
    notice_ = message;
    overlay_ = Overlay::Notice;
    log_info("Data Management: " + message);
}

void DataManagementScreen::show_busy(const std::string& message) {
    busy_ = message;
    overlay_ = Overlay::Busy;
}

void DataManagementScreen::finish_job(DataManagementResult& result) {
    if (!job_ || !job_->done) return;
    if (worker_.joinable()) worker_.join();
    std::unique_ptr<Job> job = std::move(job_);
    const std::string title = game_.display_title();
    switch (job->kind) {
        case Job::Kind::Search:
        case Job::Kind::Status: {
            // This device's side is read again with the other's, so the two
            // are compared as they are now.
            if (!game_.game_id.empty()) {
                SaveInfo fresh;
                if (store_.find(game_.game_id, fresh)) game_ = fresh;
            }
            const std::string kept =
                device_index_ < devices_.size() ? devices_[device_index_].instance : std::string();
            if (job->kind == Job::Kind::Search) {
                devices_ = job->devices;
                searched_ = true;
            }
            if (!job->game_id.empty()) {
                remote_ = job->remote;
                status_game_id_ = job->game_id;
                // A device that did not answer about this game is not there.
                devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                              [&](const net::SaveDevice& found) {
                                                  const auto state = remote_.find(found.instance);
                                                  return state == remote_.end() ||
                                                         !state->second.answered;
                                              }),
                               devices_.end());
            }
            device_index_ = 0;
            for (std::size_t index = 0; index < devices_.size(); ++index) {
                if (devices_[index].instance == kept) device_index_ = index;
            }
            if (page_ != Page::List && status_game_id_ != game_.game_id) status_queued_ = true;
            break;
        }
        case Job::Kind::Plan: {
            reload_saves();
            sync_items_.clear();
            sync_device_names_.clear();
            for (auto& [device, saves] : job->lists) {
                sync_device_names_.push_back(device.name);
                // Every game either side has a save for: this device's, and
                // the other's -- including a game this one has never had.
                std::vector<std::string> seen;
                for (const net::RemoteSave& save : saves) {
                    SyncItem item;
                    item.device = device;
                    item.game_id = save.game_id;
                    item.title = save.title;
                    item.remote = save.status;
                    for (const SaveInfo& here : saves_) {
                        if (here.game_id == save.game_id) item.title = here.display_title();
                    }
                    sync_items_.push_back(item);
                    seen.push_back(save.game_id);
                }
                for (const SaveInfo& here : saves_) {
                    if (!here.has_save ||
                        std::find(seen.begin(), seen.end(), here.game_id) != seen.end()) {
                        continue;
                    }
                    SyncItem item;
                    item.device = device;
                    item.game_id = here.game_id;
                    item.title = here.display_title();
                    sync_items_.push_back(item);
                }
            }
            if (job->lists.empty()) {
                sync_all_ = false;
                show_notice("No other devices were found.");
                break;
            }
            sync_index_ = 0;
            overlay_ = Overlay::None;
            advance_sync_all();
            break;
        }
        case Job::Kind::Fetch: {
            if (sync_all_) {
                std::string error;
                if (job->ok && store_.accept(job->info, job->bytes, job->device.name, &error)) {
                    ++sync_done_;
                } else {
                    ++sync_failed_;
                    log_warn("Sync All: could not get " + job->device.name + "'s save for " +
                             job->game_id + ": " + (job->ok ? error : job->error));
                }
                ++sync_index_;
                advance_sync_all();
                break;
            }
            if (!job->ok) {
                show_notice("Could not get " + job->device.name + "'s save: " + job->error);
                break;
            }
            if (this_game_in_play()) {
                SaveStore* store = &store_;
                auto info = std::make_shared<SaveInfo>(job->info);
                auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(job->bytes));
                const std::string from = job->device.name;
                result.restart_with = [store, info, bytes, from](std::string* error) {
                    return store->accept(*info, *bytes, from, error);
                };
                result.restart_notice = "Restarted " + title + " with " + from + "'s save.";
                overlay_ = Overlay::None;
                break;
            }
            std::string error;
            if (store_.accept(job->info, job->bytes, job->device.name, &error)) {
                show_notice("Got " + job->device.name + "'s save for " + title + ".");
            } else {
                show_notice("Could not replace the save: " + error);
            }
            reload_saves();
            refresh_game();
            status_queued_ = true;
            break;
        }
        case Job::Kind::Send:
            if (sync_all_) {
                if (job->ok) {
                    ++sync_done_;
                } else {
                    ++sync_failed_;
                    log_warn("Sync All: could not send " + job->game_id + " to " + job->device.name +
                             ": " + job->error);
                }
                ++sync_index_;
                advance_sync_all();
                break;
            }
            if (job->ok) {
                show_notice("Sent " + title + " to " + job->device.name + ".");
            } else {
                show_notice("Could not send " + title + " to " + job->device.name + ": " + job->error);
            }
            status_queued_ = true;
            break;
    }
    if (page_ == Page::Game) settle_action_focus();
    if (plan_queued_ && !job_) {
        plan_queued_ = false;
        sync_all_ = false;
        start_sync_all();
        return;
    }
    if (search_queued_) {
        search_queued_ = false;
        status_queued_ = false;
        start_search();
    } else if (status_queued_) {
        status_queued_ = false;
        start_status();
    }
}

void DataManagementScreen::activate(Action action, DataManagementResult& result) {
    const std::vector<bool> enabled = actions_enabled();
    if (!enabled[static_cast<std::size_t>(action)]) return;
    switch (action) {
        case Action::Sync: start_sync(result); break;
        case Action::Get: ask(Pending::Get); break;
        case Action::Send: ask(Pending::Send); break;
        case Action::Delete: ask(Pending::Delete); break;
        case Action::Earlier:
            earlier_ = store_.earlier(game_.game_id);
            earlier_focus_ = 0;
            earlier_scroll_ = 0.0f;
            page_ = Page::Earlier;
            break;
        case Action::Count: break;
    }
}

void DataManagementScreen::ask(Pending pending) {
    pending_ = pending;
    dialog_lines_.clear();
    dialog_plain_lines_ = 0;
    dialog_from_name_.clear();
    dialog_from_date_.clear();
    dialog_to_name_.clear();
    dialog_to_date_.clear();
    const std::string title = game_.display_title();
    const std::string here_date = game_.has_save ? describe_time(game_.saved_at) : "No save";
    const net::SaveDevice* shown = device();
    const RemoteState* there = remote();
    const std::string other = shown != nullptr ? shown->name : std::string();
    const std::string there_date =
        there != nullptr && there->status.versions.has_save ? describe_time(there->status.saved_at)
                                                            : "No save";
    const SaveComparison standing = comparison();
    const bool playing = this_game_in_play();
    const std::string restart_tail = " Anything since your last in-game save on " + own_name_ +
                                     " will be lost. The save itself will be moved to Earlier Saves.";
    switch (pending) {
        case Pending::Get:
            dialog_from_name_ = other;
            dialog_from_date_ = there_date;
            dialog_to_name_ = own_name_;
            dialog_to_date_ = here_date;
            if (playing) {
                dialog_title_ = "Restart " + title + " with " + other + "'s save?";
                dialog_lines_.push_back("The game will restart from " + other + "'s save." + restart_tail);
                dialog_accept_ = "Restart";
                break;
            }
            dialog_title_ = game_.has_save ? "Replace " + own_name_ + "'s save with " + other + "'s?"
                                           : "Get " + other + "'s save?";
            if (standing == SaveComparison::Conflict || standing == SaveComparison::NewerHere) {
                dialog_lines_.push_back(own_name_ + "'s save has progress " + other + "'s doesn't.");
                dialog_plain_lines_ = 1;
            }
            if (game_.has_save) {
                dialog_lines_.push_back(own_name_ + "'s current save will be moved to Earlier Saves.");
            }
            dialog_accept_ = game_.has_save ? "Replace" : "Get";
            break;
        case Pending::Send: {
            const bool replaces = there != nullptr && there->status.versions.has_save;
            dialog_from_name_ = own_name_;
            dialog_from_date_ = here_date;
            dialog_to_name_ = other;
            dialog_to_date_ = there_date;
            dialog_title_ = replaces ? "Replace " + other + "'s save with " + own_name_ + "'s?"
                                     : "Send " + own_name_ + "'s save to " + other + "?";
            if (standing == SaveComparison::Conflict || standing == SaveComparison::NewerThere) {
                dialog_lines_.push_back(other + "'s save has progress " + own_name_ + "'s doesn't.");
                dialog_plain_lines_ = 1;
            }
            if (replaces) {
                dialog_lines_.push_back(other + "'s current save will be moved to Earlier Saves.");
            }
            dialog_accept_ = replaces ? "Replace" : "Send";
            break;
        }
        case Pending::Delete:
            if (playing) {
                dialog_title_ = "Restart " + title + " without a save?";
                dialog_lines_.push_back("The game will restart with no save." + restart_tail);
                dialog_accept_ = "Restart";
            } else {
                dialog_title_ = "Delete " + own_name_ + "'s save for " + title + "?";
                dialog_lines_.push_back("It will be moved to Earlier Saves.");
                dialog_accept_ = "Delete";
            }
            break;
        case Pending::Restore: {
            const auto chosen = std::find_if(earlier_.begin(), earlier_.end(), [&](const EarlierSave& save) {
                return save.id == pending_earlier_;
            });
            if (chosen != earlier_.end()) {
                dialog_from_name_ = "Saved on " + (chosen->made_on.empty() ? own_name_ : chosen->made_on);
                dialog_from_date_ = describe_time(chosen->saved_at);
                dialog_to_name_ = own_name_;
                dialog_to_date_ = here_date;
            }
            if (playing) {
                dialog_title_ = "Restart " + title + " with this earlier save?";
                dialog_lines_.push_back("The game will restart from the earlier save." + restart_tail);
                dialog_accept_ = "Restart";
            } else {
                dialog_title_ = "Restore this earlier save?";
                if (game_.has_save) {
                    dialog_lines_.push_back(own_name_ + "'s current save will be moved to Earlier Saves.");
                }
                dialog_accept_ = "Restore";
            }
            break;
        }
        case Pending::None: return;
    }
    overlay_ = Overlay::Confirm;
}

void DataManagementScreen::confirmed(DataManagementResult& result) {
    // A resume point holds the save as it was when it was made, and would
    // put it back: replacing the save means losing it, and that is asked
    // about first.
    bool resume_point = false;
    std::string whose;
    if (pending_ == Pending::Send) {
        const RemoteState* there = remote();
        resume_point = there != nullptr && there->status.has_resume_point;
        whose = device() != nullptr ? device()->name : std::string();
    } else {
        resume_point = store_.has_resume_point(game_.game_id);
    }
    if (!resume_point) {
        carry_out(result);
        return;
    }
    dialog_from_name_.clear();
    dialog_lines_.clear();
    dialog_plain_lines_ = 1;
    if (pending_ == Pending::Send) {
        dialog_title_ = "Replace " + whose + "'s save file?";
        dialog_lines_.push_back("Replacing " + whose +
                                "'s save file will delete its resume point. Are you sure you want "
                                "to replace it?");
        dialog_accept_ = "Replace";
    } else if (pending_ == Pending::Delete) {
        dialog_title_ = "Delete this save file?";
        dialog_lines_.push_back(
            "Deleting this save file will delete the resume point. Are you sure you want to "
            "delete it?");
        dialog_accept_ = "Delete";
    } else if (pending_ == Pending::Restore) {
        dialog_title_ = "Restore this save file?";
        dialog_lines_.push_back(
            "Restoring this save file will delete the resume point. Are you sure you want to "
            "restore it?");
        dialog_accept_ = "Restore";
    } else {
        dialog_title_ = "Replace this save file?";
        dialog_lines_.push_back(
            "Replacing this save file will delete the resume point. Are you sure you want to "
            "replace it?");
        dialog_accept_ = "Replace";
    }
    overlay_ = Overlay::ResumeConfirm;
}

void DataManagementScreen::carry_out(DataManagementResult& result) {
    const std::string title = game_.display_title();
    const std::string game_id = game_.game_id;
    overlay_ = Overlay::None;
    switch (pending_) {
        case Pending::Get: {
            const net::SaveDevice* shown = device();
            if (shown == nullptr || job_) break;
            start_transfer(Job::Kind::Fetch, *shown, game_id, "Getting " + shown->name + "'s save...");
            break;
        }
        case Pending::Send: {
            const net::SaveDevice* shown = device();
            if (shown == nullptr || job_) break;
            start_transfer(Job::Kind::Send, *shown, game_id,
                           "Sending " + title + " to " + shown->name + "...");
            break;
        }
        case Pending::Delete: {
            if (this_game_in_play()) {
                SaveStore* store = &store_;
                result.restart_with = [store, game_id](std::string* error) {
                    return store->remove(game_id, error);
                };
                result.restart_notice = "Restarted " + title + " without a save.";
                break;
            }
            std::string error;
            if (store_.remove(game_id, &error)) {
                show_notice("Deleted " + own_name_ + "'s save for " + title + ".");
            } else {
                show_notice("Could not delete the save: " + error);
            }
            reload_saves();
            refresh_game();
            break;
        }
        case Pending::Restore: {
            const std::string earlier_id = pending_earlier_;
            if (this_game_in_play()) {
                SaveStore* store = &store_;
                result.restart_with = [store, game_id, earlier_id](std::string* error) {
                    return store->restore(game_id, earlier_id, error);
                };
                result.restart_notice = "Restarted " + title + " with an earlier save.";
                break;
            }
            std::string error;
            if (store_.restore(game_id, earlier_id, &error)) {
                show_notice("Restored an earlier save for " + title + ".");
            } else {
                show_notice("Could not restore the save: " + error);
            }
            reload_saves();
            refresh_game();
            page_ = Page::Game;
            status_queued_ = true;
            if (!job_) {
                status_queued_ = false;
                start_status();
            }
            break;
        }
        case Pending::None: break;
    }
    pending_ = Pending::None;
    sync_mode_ = false;
}

void DataManagementScreen::start_transfer(Job::Kind kind, const net::SaveDevice& device,
                                          const std::string& game_id, const std::string& busy) {
    std::unique_ptr<Job> job(new Job());
    job->kind = kind;
    job->device = device;
    job->game_id = game_id;
    show_busy(busy);
    start_job(std::move(job));
}

DataManagementScreen::SyncWay DataManagementScreen::sync_way(const SaveVersionState& here,
                                                             const net::RemoteSaveStatus& there) {
    if (there.playing_now) return SyncWay::Busy;
    switch (compare_saves(here, there.versions)) {
        case SaveComparison::UpToDate:
        case SaveComparison::Neither: return SyncWay::Nothing;
        case SaveComparison::NewerHere:
        case SaveComparison::OnlyHere: return SyncWay::Send;
        case SaveComparison::NewerThere:
        case SaveComparison::OnlyThere: return SyncWay::Get;
        case SaveComparison::Conflict: return SyncWay::Conflict;
    }
    return SyncWay::Nothing;
}

void DataManagementScreen::start_sync(DataManagementResult& result) {
    const net::SaveDevice* shown = device();
    const RemoteState* there = remote();
    if (shown == nullptr || there == nullptr) return;
    const std::string title = game_.display_title();
    switch (sync_way(version_state(game_), there->status)) {
        case SyncWay::Nothing:
            show_notice(title + " is already up to date on " + own_name_ + " and " + shown->name + ".");
            return;
        case SyncWay::Busy: return;
        case SyncWay::Send:
            // The newer one goes where it is not, without a second question:
            // that is what Sync is for. The one it replaces is kept there.
            pending_ = Pending::Send;
            sync_mode_ = true;
            confirmed(result);
            return;
        case SyncWay::Get:
            if (this_game_in_play()) {
                // Which restarts the game, so that is still asked about.
                ask(Pending::Get);
                return;
            }
            pending_ = Pending::Get;
            sync_mode_ = true;
            confirmed(result);
            return;
        case SyncWay::Conflict:
            show_choice("Which save do you want to keep?", shown->name,
                        there->status.versions.has_save ? describe_time(there->status.saved_at) : "No save",
                        game_.has_save ? describe_time(game_.saved_at) : "No save");
            return;
    }
}

void DataManagementScreen::show_choice(const std::string& title, const std::string& there_name,
                                       const std::string& there_date, const std::string& here_date) {
    choice_title_ = title;
    choice_line_ = "Each save has progress the other doesn't. The one you don't keep will be moved to "
                   "Earlier Saves on its device.";
    choice_names_[0] = there_name + "'s save";
    choice_dates_[0] = there_date;
    choice_names_[1] = own_name_ + "'s save";
    choice_dates_[1] = here_date;
    choice_focus_ = 0;
    overlay_ = Overlay::Choice;
}

void DataManagementScreen::chosen(DataManagementResult& result) {
    // The first choice is the other device's save, which comes here; the
    // second is this one's, which goes there.
    const bool get = choice_focus_ == 0;
    overlay_ = Overlay::None;
    if (sync_all_) {
        begin_sync_transfer(get ? SyncWay::Get : SyncWay::Send);
        return;
    }
    if (get && this_game_in_play()) {
        ask(Pending::Get);
        return;
    }
    pending_ = get ? Pending::Get : Pending::Send;
    sync_mode_ = true;
    confirmed(result);
}

void DataManagementScreen::start_sync_all() {
    if (sync_all_) return;
    sync_all_ = true;
    sync_done_ = sync_skipped_ = sync_failed_ = 0;
    sync_items_.clear();
    if (job_) {
        // Once whatever is being asked now has its answer.
        plan_queued_ = true;
        show_busy("Looking for devices...");
        return;
    }
    std::unique_ptr<Job> job(new Job());
    job->kind = Job::Kind::Plan;
    show_busy("Looking for devices...");
    start_job(std::move(job));
    log_info("Sync All: looking for devices and what they have");
}

void DataManagementScreen::advance_sync_all() {
    while (sync_all_ && sync_index_ < sync_items_.size()) {
        const SyncItem& item = sync_items_[sync_index_];
        SaveInfo here;
        const SaveVersionState versions = store_.find(item.game_id, here) ? version_state(here)
                                                                          : SaveVersionState();
        switch (sync_way(versions, item.remote)) {
            case SyncWay::Nothing:
                ++sync_index_;
                continue;
            case SyncWay::Busy:
                ++sync_skipped_;
                ++sync_index_;
                continue;
            case SyncWay::Conflict:
                // Asked one at a time, naming the game.
                show_choice(item.title + ": which save do you want to keep?", item.device.name,
                            item.remote.versions.has_save ? describe_time(item.remote.saved_at) : "No save",
                            here.has_save ? describe_time(here.saved_at) : "No save");
                return;
            case SyncWay::Get:
            case SyncWay::Send:
                begin_sync_transfer(sync_way(versions, item.remote));
                return;
        }
    }
    if (sync_all_) finish_sync_all();
}

void DataManagementScreen::begin_sync_transfer(SyncWay way) {
    const SyncItem& item = sync_items_[sync_index_];
    // The resume point's question, the same as anywhere else a save is
    // replaced -- once per game that has one.
    const bool resume_point = way == SyncWay::Get ? store_.has_resume_point(item.game_id)
                                                  : item.remote.has_resume_point;
    if (!resume_point) {
        start_sync_job(way);
        return;
    }
    sync_pending_way_ = way;
    dialog_from_name_.clear();
    dialog_lines_.clear();
    dialog_plain_lines_ = 1;
    if (way == SyncWay::Get) {
        dialog_title_ = item.title + ": replace this save file?";
        dialog_lines_.push_back(
            "Replacing this save file will delete the resume point. Are you sure you want to "
            "replace it?");
    } else {
        dialog_title_ = item.title + ": replace " + item.device.name + "'s save file?";
        dialog_lines_.push_back("Replacing " + item.device.name +
                                "'s save file will delete its resume point. Are you sure you want "
                                "to replace it?");
    }
    dialog_accept_ = "Replace";
    overlay_ = Overlay::ResumeConfirm;
}

void DataManagementScreen::start_sync_job(SyncWay way) {
    const SyncItem& item = sync_items_[sync_index_];
    start_transfer(way == SyncWay::Get ? Job::Kind::Fetch : Job::Kind::Send, item.device, item.game_id,
                   "Syncing " + item.title + " with " + item.device.name + "...");
}

void DataManagementScreen::skip_sync_item() {
    ++sync_skipped_;
    ++sync_index_;
    overlay_ = Overlay::None;
    advance_sync_all();
}

void DataManagementScreen::finish_sync_all() {
    sync_all_ = false;
    reload_saves();
    std::string with;
    if (sync_device_names_.size() == 1) with = " with " + sync_device_names_.front();
    std::string message;
    if (sync_done_ == 0 && sync_failed_ == 0) {
        message = sync_skipped_ == 0 ? "Every save is already up to date" + with + "."
                                     : "Nothing was synced" + with + ".";
    } else {
        message = "Synced " + std::to_string(sync_done_) + (sync_done_ == 1 ? " save" : " saves") + with + ".";
    }
    if (sync_skipped_ > 0) message += " Skipped " + std::to_string(sync_skipped_) + ".";
    if (sync_failed_ > 0) {
        message += " " + std::to_string(sync_failed_) + (sync_failed_ == 1 ? " save" : " saves") +
                   " could not be synced.";
    }
    show_notice(message);
}

std::vector<GuidePrompt> DataManagementScreen::guide_prompts() const {
    switch (overlay_) {
        case Overlay::Confirm:
        case Overlay::ResumeConfirm:
            return {{ButtonGlyph::B, "Cancel", PromptRole::LeavesScreen},
                    {ButtonGlyph::A, dialog_accept_}};
        case Overlay::Choice:
            return {{ButtonGlyph::B, sync_all_ ? "Skip" : "Cancel", PromptRole::LeavesScreen},
                    {ButtonGlyph::A, "Keep", PromptRole::ActsOnSelection}};
        case Overlay::Busy:
        case Overlay::Notice:
            return {};
        case Overlay::None: break;
    }
    switch (page_) {
        case Page::List: {
            std::vector<GuidePrompt> prompts;
            prompts.push_back({ButtonGlyph::X, "Sync All"});
            if (!saves_.empty()) prompts.push_back({ButtonGlyph::A, "Open", PromptRole::ActsOnSelection});
            prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
            return prompts;
        }
        case Page::Game: {
            std::vector<GuidePrompt> prompts;
            if (devices_.size() > 1) {
                GuidePrompt change{ButtonGlyph::L, "Change Device"};
                change.pair = ButtonGlyph::R;
                prompts.push_back(change);
            }
            prompts.push_back({ButtonGlyph::Y, "Search for Devices"});
            prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
            prompts.push_back({ButtonGlyph::A, "OK", PromptRole::ActsOnSelection});
            return prompts;
        }
        case Page::Earlier: {
            std::vector<GuidePrompt> prompts;
            if (!earlier_.empty()) {
                prompts.push_back({ButtonGlyph::A, "Restore", PromptRole::ActsOnSelection});
            }
            prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
            return prompts;
        }
    }
    return {};
}

void DataManagementScreen::handle_guide_tap(ButtonGlyph glyph, std::vector<UiAction>& actions) const {
    switch (glyph) {
        case ButtonGlyph::A: actions.push_back(UiAction::Confirm); break;
        case ButtonGlyph::B: actions.push_back(UiAction::Cancel); break;
        case ButtonGlyph::Y: actions.push_back(UiAction::MenuReset); break;
        case ButtonGlyph::X: actions.push_back(UiAction::MenuQuit); break;
        case ButtonGlyph::L: actions.push_back(UiAction::PageDown); break;
        default: break;
    }
}

DataManagementResult DataManagementScreen::update(const std::vector<UiAction>& input_actions,
                                                  const std::vector<PointerEvent>& pointers,
                                                  float output_width,
                                                  float output_height) {
    DataManagementResult result;
    if (!open_) return result;
    finish_job(result);
    if (overlay_ == Overlay::None && !sync_all_) check_for_changes();
    if (result.restart_with) return result;

    if (overlay_ == Overlay::Notice) {
        if (notice_popup_answered(input_actions, pointers, output_width, output_height)) {
            overlay_ = Overlay::None;
        }
        return result;
    }
    if (overlay_ == Overlay::Busy) return result;

    std::vector<UiAction> actions = input_actions;
    if (overlay_ == Overlay::None) drag_list(pointers, output_width, output_height);
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        const ButtonGlyph on_guide = guide_button_at(guide_hits_, pointer.x, pointer.y);
        if (on_guide != ButtonGlyph::Count) {
            handle_guide_tap(on_guide, actions);
            continue;
        }
        // Everywhere here, a first tap highlights and a second, on the same
        // thing, chooses it.
        if (overlay_ == Overlay::Choice) {
            for (std::size_t index = 0; index < choice_rects_.size(); ++index) {
                if (!tapped(pointer, choice_rects_[index])) continue;
                if (tap_chooses(choice_focus_, static_cast<int>(index))) {
                    actions.push_back(UiAction::Confirm);
                }
            }
            continue;
        }
        if (overlay_ != Overlay::None || drag_.release_was_drag()) continue;
        for (std::size_t index = 0; index < row_rects_.size(); ++index) {
            if (!tapped(pointer, row_rects_[index])) continue;
            bool chosen = false;
            if (page_ == Page::List) chosen = tap_chooses(list_focus_, row_first_ + static_cast<int>(index));
            if (page_ == Page::Game) {
                chosen = tap_chooses(action_focus_, static_cast<int>(index));
                action_focus_chosen_ = true;
            }
            if (page_ == Page::Earlier) {
                chosen = tap_chooses(earlier_focus_, row_first_ + static_cast<int>(index));
            }
            if (chosen) actions.push_back(UiAction::Confirm);
        }
    }

    for (const UiAction action : actions) {
        if (result.close_requested || result.restart_with) break;
        if (overlay_ == Overlay::Choice) {
            if (action == UiAction::Up) choice_focus_ = 0;
            if (action == UiAction::Down) choice_focus_ = 1;
            if (action == UiAction::Confirm) chosen(result);
            if (action == UiAction::Cancel) {
                if (sync_all_) {
                    skip_sync_item();
                } else {
                    overlay_ = Overlay::None;
                }
            }
            continue;
        }
        if (overlay_ == Overlay::ResumeConfirm && sync_all_) {
            if (action == UiAction::Confirm) {
                overlay_ = Overlay::None;
                start_sync_job(sync_pending_way_);
            } else if (action == UiAction::Cancel) {
                skip_sync_item();
            }
            continue;
        }
        if (overlay_ == Overlay::Confirm || overlay_ == Overlay::ResumeConfirm) {
            if (action == UiAction::Confirm) {
                if (overlay_ == Overlay::Confirm) {
                    confirmed(result);
                } else {
                    carry_out(result);
                }
            } else if (action == UiAction::Cancel) {
                overlay_ = Overlay::None;
                pending_ = Pending::None;
                sync_mode_ = false;
            }
            continue;
        }
        if (overlay_ != Overlay::None) break;
        switch (page_) {
            case Page::List: {
                const int count = static_cast<int>(saves_.size());
                if (action == UiAction::Up && count > 0) list_focus_ = std::max(0, list_focus_ - 1);
                if (action == UiAction::Down && count > 0) list_focus_ = std::min(count - 1, list_focus_ + 1);
                if (action == UiAction::Confirm && count > 0) {
                    open_game(saves_[static_cast<std::size_t>(list_focus_)].game_id);
                }
                if (action == UiAction::Cancel) result.close_requested = true;
                if (action == UiAction::MenuQuit) start_sync_all();
                break;
            }
            case Page::Game: {
                const int count = static_cast<int>(Action::Count);
                if (action == UiAction::Up) action_focus_ = std::max(0, action_focus_ - 1);
                if (action == UiAction::Down) action_focus_ = std::min(count - 1, action_focus_ + 1);
                if (action == UiAction::Up || action == UiAction::Down) action_focus_chosen_ = true;
                if (action == UiAction::Confirm) activate(static_cast<Action>(action_focus_), result);
                if (action == UiAction::MenuReset) start_search();
                if ((action == UiAction::PageUp || action == UiAction::PageDown) && devices_.size() > 1) {
                    const std::size_t devices = devices_.size();
                    device_index_ = action == UiAction::PageUp ? (device_index_ + devices - 1) % devices
                                                               : (device_index_ + 1) % devices;
                }
                if (action == UiAction::Cancel) {
                    if (in_game()) {
                        result.close_requested = true;
                    } else {
                        reload_saves();
                        for (std::size_t index = 0; index < saves_.size(); ++index) {
                            if (saves_[index].game_id == game_.game_id) list_focus_ = static_cast<int>(index);
                        }
                        page_ = Page::List;
                    }
                }
                break;
            }
            case Page::Earlier: {
                const int count = static_cast<int>(earlier_.size());
                if (action == UiAction::Up && count > 0) earlier_focus_ = std::max(0, earlier_focus_ - 1);
                if (action == UiAction::Down && count > 0) earlier_focus_ = std::min(count - 1, earlier_focus_ + 1);
                if (action == UiAction::Confirm && count > 0) {
                    pending_earlier_ = earlier_[static_cast<std::size_t>(earlier_focus_)].id;
                    ask(Pending::Restore);
                }
                if (action == UiAction::Cancel) page_ = Page::Game;
                break;
            }
        }
    }
    drag_.settle();
    if (!input_actions.empty() && overlay_ == Overlay::None) follow_focus(output_width, output_height);
    if (result.close_requested) {
        open_ = false;
    }
    return result;
}

void DataManagementScreen::drag_list(const std::vector<PointerEvent>& pointers, float output_width,
                                     float output_height) {
    if (page_ == Page::Game) return;
    const Frame frame = frame_for(output_width, output_height);
    const float row_h = menu_two_line_row_height(frame.line);
    const PixelRect list{frame.panel.x, frame.content_top, frame.panel.w,
                         frame.content_bottom - frame.content_top};
    const int count = static_cast<int>(page_ == Page::List ? saves_.size() : earlier_.size());
    float& scroll = page_ == Page::List ? list_scroll_ : earlier_scroll_;
    drag_.update(pointers, list, scroll, std::max(0.0f, static_cast<float>(count) * row_h - list.h));
}

void DataManagementScreen::follow_focus(float output_width, float output_height) {
    // The cursor brings the list with it when it is moved -- and only then,
    // so a list a finger has moved stays where it was left.
    if (page_ == Page::Game) return;
    drag_.stop();
    const Frame frame = frame_for(output_width, output_height);
    const float row_h = menu_two_line_row_height(frame.line);
    const float view = frame.content_bottom - frame.content_top;
    const int count = static_cast<int>(page_ == Page::List ? saves_.size() : earlier_.size());
    float& scroll = page_ == Page::List ? list_scroll_ : earlier_scroll_;
    const int focus = page_ == Page::List ? list_focus_ : earlier_focus_;
    const float top = static_cast<float>(focus) * row_h;
    if (top < scroll) scroll = top;
    if (top + row_h > scroll + view) scroll = top + row_h - view;
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, static_cast<float>(count) * row_h - view));
}

bool DataManagementScreen::ensure_icons() {
    if (ui_icons_.valid()) return true;
    if (ui_icons_tried_) return false;
    ui_icons_tried_ = true;
    DecodedImage image;
    std::string error;
    if (!decode_png(assets::kUiIconsPng, assets::kUiIconsPng_size, image, &error)) {
        log_warn("Data Management's icons would not load: " + error);
        return false;
    }
    std::vector<uint8_t> coverage(image.pixels.size());
    for (std::size_t index = 0; index < image.pixels.size(); ++index) {
        coverage[index] = static_cast<uint8_t>(image.pixels[index] >> 24);
    }
    ui_icons_.upload_alpha8(coverage.data(), image.width, image.height);
    ui_icons_.set_linear_filtering(true);
    return ui_icons_.valid();
}

const Texture* DataManagementScreen::game_icon(const SaveInfo& info) {
    const auto found = game_icons_.find(info.game_id);
    if (found != game_icons_.end()) return found->second.valid() ? &found->second : nullptr;
    Texture& texture = game_icons_[info.game_id];
    std::vector<uint32_t> pixels = decode_ds_icon(info.icon);
    if (pixels.empty()) return nullptr;
    // 0xAARRGGBB to the bytes R, G, B, A.
    for (uint32_t& pixel : pixels) {
        pixel = (pixel & 0xFF00FF00u) | ((pixel >> 16) & 0xFFu) | ((pixel & 0xFFu) << 16);
    }
    texture.upload_rgba8888(pixels.data(), kDsIconSize, kDsIconSize, kDsIconSize);
    return texture.valid() ? &texture : nullptr;
}

void DataManagementScreen::render(QuadRenderer& quads,
                                  TextRenderer& text,
                                  const ButtonGlyphSheet& glyphs,
                                  float output_width,
                                  float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const Frame frame = frame_for(output_width, output_height);
    const float scale = frame.scale;
    const float line = frame.line;
    const bool icons = ensure_icons();
    const auto draw_icon = [&](UiIcon icon, const PixelRect& rect, const Color& color) {
        if (!icons) return;
        const float cells = static_cast<float>(static_cast<int>(UiIcon::Count));
        const float cell = static_cast<float>(static_cast<int>(icon));
        quads.draw_alpha_mask(rect, ui_icons_, color, cell / cells, 0.0f, (cell + 1.0f) / cells, 1.0f);
    };
    const auto draw_game_icon = [&](const SaveInfo& info, const PixelRect& rect) {
        if (const Texture* icon = game_icon(info)) {
            quads.draw_image(rect, *icon);
        } else {
            quads.draw_rounded_rect(rect, rect.w * 0.12f, with_alpha(theme.text_dim, 0.35f));
        }
    };
    const Color rule = with_alpha(theme.text_dim, 0.3f);

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.background);

    // The heading: the game's icon and title on its pages, the screen's name
    // on the list.
    const float title_scale = scale * 1.45f;
    const float title_y = frame.panel.y + (frame.header_bottom - frame.panel.y - TextRenderer::line_height(title_scale)) * 0.5f;
    float title_x = frame.panel.x;
    std::string heading = "Data Management";
    if (page_ != Page::List) {
        const float icon_size = line * 1.7f;
        draw_game_icon(game_, PixelRect{frame.panel.x, frame.panel.y + (frame.header_bottom - frame.panel.y - icon_size) * 0.5f,
                                        icon_size, icon_size});
        title_x += icon_size + line * 0.6f;
        heading = game_.display_title();
        if (page_ == Page::Earlier) heading += " \xC2\xB7 Earlier Saves";
    }
    float heading_room = frame.panel.x + frame.panel.w - title_x;
    if (this_game_in_play()) {
        // Said beside the title, the way the mockup has it: this is the
        // save the game has open.
        const float pill_scale = scale * 0.75f;
        const std::string pill = "Playing now";
        const float pill_w = TextRenderer::measure_width(pill, pill_scale) + line * 1.2f;
        const float pill_h = TextRenderer::line_height(pill_scale) * 1.5f;
        const PixelRect box{frame.panel.x + frame.panel.w - pill_w,
                            frame.panel.y + (frame.header_bottom - frame.panel.y - pill_h) * 0.5f, pill_w, pill_h};
        quads.draw_capsule(box, theme.accent);
        quads.draw_capsule(PixelRect{box.x + 1.5f, box.y + 1.5f, box.w - 3.0f, box.h - 3.0f}, theme.background);
        text.draw(quads, pill, box.x + box.w * 0.5f, box.y + (box.h - TextRenderer::line_height(pill_scale)) * 0.5f,
                  pill_scale, theme.accent, TextAlign::Center);
        heading_room -= pill_w + line;
    }
    text.draw(quads, elide_right(heading, heading_room, title_scale), title_x, title_y, title_scale, theme.text);
    quads.draw_solid(PixelRect{frame.panel.x, frame.header_bottom, frame.panel.w, 1.0f}, rule);
    quads.draw_solid(PixelRect{frame.panel.x, frame.content_bottom + line * 0.45f, frame.panel.w, 1.0f}, rule);

    row_rects_.clear();
    if (page_ == Page::List) {
        const float row_h = menu_two_line_row_height(line);
        const PixelRect list{frame.panel.x, frame.content_top, frame.panel.w,
                             frame.content_bottom - frame.content_top};
        const int count = static_cast<int>(saves_.size());
        const float content = static_cast<float>(count) * row_h;
        list_scroll_ = std::clamp(list_scroll_, 0.0f, std::max(0.0f, content - list.h));
        const bool scrolls = content > list.h + 0.5f;
        // Placed by the pixel, so the top and bottom rows can be part-way out
        // of the list: drawn clipped to it.
        const int first = std::max(0, static_cast<int>(std::floor(list_scroll_ / row_h)));
        const int last = std::min(count, static_cast<int>(std::ceil((list_scroll_ + list.h) / row_h)));
        row_first_ = first;
        quads.set_clip(list);
        const float row_w = list.w - (scrolls ? scroll_bar_width(scale) + line * 0.6f : 0.0f);
        if (saves_.empty()) {
            text.draw(quads, "There are no saves on " + own_name_ + " yet.", output_width * 0.5f,
                      (frame.content_top + frame.content_bottom) * 0.5f - line * 0.5f, scale, theme.text_dim,
                      TextAlign::Center);
        }
        for (int index = first; index < last; ++index) {
            const SaveInfo& info = saves_[static_cast<std::size_t>(index)];
            const PixelRect row{list.x, list.y + index * row_h - list_scroll_, row_w, row_h - menu_row_gap(line)};
            row_rects_.push_back(visible_part(row, list));
            const bool focused = index == list_focus_ && overlay_ == Overlay::None;
            draw_menu_row(quads, row, focused, scale);
            if (!focused) draw_menu_rule(quads, row);
            const Color& ink = menu_row_text(focused);
            const Color dim = focused ? with_alpha(theme.background, 0.7f) : theme.text_dim;
            const float icon_size = line * 2.0f;
            const float pad = menu_row_pad(line);
            draw_game_icon(info, PixelRect{row.x + pad, row.y + (row.h - icon_size) * 0.5f, icon_size, icon_size});
            const std::string when = info.has_save ? describe_time(info.saved_at) : "No save";
            const float date_scale = scale * 0.85f;
            const float date_w = TextRenderer::measure_width(when, date_scale);
            const float text_x = row.x + pad * 1.8f + icon_size;
            const float room = row.x + row.w - pad * 2.0f - date_w - text_x;
            const float sub_scale = scale * 0.75f;
            const float step = line * 1.3f;
            const float block = step + TextRenderer::line_height(sub_scale);
            const float top = row.y + (row.h - block) * 0.5f;
            text.draw(quads, elide_right(info.display_title(), room, scale), text_x, top, scale, ink);
            text.draw(quads, elide_right(info.publisher, room, sub_scale), text_x, top + step, sub_scale, dim);
            text.draw(quads, when, row.x + row.w - pad, row.y + (row.h - TextRenderer::line_height(date_scale)) * 0.5f,
                      date_scale, dim, TextAlign::Right);
        }
        quads.clear_clip();
        if (scrolls) draw_scroll_bar(quads, list, list_scroll_ / content, list.h / content, line, scale);
    } else if (page_ == Page::Game) {
        const float left_w = std::round(frame.panel.w * 0.55f);
        const float row_h = std::min(menu_row_height(line),
                                     (frame.content_bottom - frame.content_top) / static_cast<float>(Action::Count));
        const std::vector<std::string> labels = action_labels();
        const std::vector<bool> enabled = actions_enabled();
        const UiIcon action_icons[] = {UiIcon::Sync, UiIcon::Get, UiIcon::Send, UiIcon::History, UiIcon::Delete};
        for (int index = 0; index < static_cast<int>(Action::Count); ++index) {
            const PixelRect row{frame.panel.x, frame.content_top + index * row_h, left_w, row_h - menu_row_gap(line)};
            row_rects_.push_back(row);
            const bool focused = index == action_focus_ && overlay_ == Overlay::None;
            draw_menu_row(quads, row, focused, scale);
            if (!focused) draw_menu_rule(quads, row);
            Color ink = menu_row_text(focused);
            if (!enabled[static_cast<std::size_t>(index)]) ink = with_alpha(focused ? theme.background : theme.text_dim, 0.55f);
            const float icon_size = line * 1.35f;
            const float pad = menu_row_pad(line);
            draw_icon(action_icons[index], PixelRect{row.x + pad, row.y + (row.h - icon_size) * 0.5f, icon_size, icon_size}, ink);
            const float label_scale = scale * 1.05f;
            const float label_x = row.x + pad * 2.0f + icon_size;
            text.draw(quads, elide_right(labels[static_cast<std::size_t>(index)], row.x + row.w - pad - label_x, label_scale),
                      label_x, row.y + (row.h - TextRenderer::line_height(label_scale)) * 0.5f, label_scale, ink);
        }
        const float divider_x = frame.panel.x + left_w + line * 0.9f;
        quads.draw_solid(PixelRect{divider_x, frame.content_top, 1.0f, frame.content_bottom - frame.content_top}, rule);

        const float right_x = divider_x + line * 0.9f;
        const float right_w = frame.panel.x + frame.panel.w - right_x;
        const float centre = right_x + right_w * 0.5f;
        const float icon_size = std::min(line * 4.2f, right_w * 0.5f);
        // A device: its name, its picture, and when its save last changed.
        const auto draw_device = [&](const std::string& name, const std::string& kind, const std::string& date, float top) {
            text.draw(quads, elide_right(name, right_w, scale * 0.95f), centre, top, scale * 0.95f, theme.text, TextAlign::Center);
            // The icon fills the band across the middle of its square cell.
            const float band_top = top + line * 1.5f;
            draw_icon(device_icon(kind), PixelRect{centre - icon_size * 0.5f, band_top - icon_size * 0.25f, icon_size, icon_size},
                      theme.text);
            text.draw(quads, date, centre, band_top + icon_size * 0.5f + line * 0.35f, scale * 0.85f, theme.text, TextAlign::Center);
        };
        const float block_h = line * 1.5f + icon_size * 0.5f + line * 0.35f + line;
        const std::string here_date = game_.has_save ? describe_time(game_.saved_at) : "No save";
        const net::SaveDevice* shown = device();
        const RemoteState* there = remote();
        if (shown != nullptr && there != nullptr) {
            const float box_h = line * 2.0f;
            const PixelRect box{right_x, frame.content_bottom - box_h, right_w, box_h};
            const float room = box.y - frame.content_top;
            const float gap = std::max(line * 0.4f, (room - block_h * 2.0f) / 3.0f);
            const std::string there_date = there->status.versions.has_save ? describe_time(there->status.saved_at) : "No save";
            draw_device(shown->name, shown->kind, there_date, frame.content_top + gap);
            draw_device(own_name_, own_kind_, here_date, frame.content_top + gap * 2.0f + block_h);
            quads.draw_rounded_rect(box, line * 0.35f, theme.panel);
            const std::string words = comparison_text();
            if (!words.empty()) {
                text.draw(quads, elide_right(words, box.w - line, scale * 0.9f), box.x + box.w * 0.5f,
                          box.y + (box.h - TextRenderer::line_height(scale * 0.9f)) * 0.5f, scale * 0.9f,
                          comparison_is_conflict() ? theme.danger : theme.text, TextAlign::Center);
            }
        } else {
            // Nobody else found: only this device, in the middle.
            draw_device(own_name_, own_kind_, here_date,
                        (frame.content_top + frame.content_bottom) * 0.5f - block_h * 0.5f);
        }
        if (job_) {
            text.draw(quads, "Searching for devices...", centre, frame.content_top, scale * 0.75f, theme.text_dim,
                      TextAlign::Center);
        }
    } else {
        const float row_h = menu_two_line_row_height(line);
        const PixelRect list{frame.panel.x, frame.content_top, frame.panel.w,
                             frame.content_bottom - frame.content_top};
        if (earlier_.empty()) {
            text.draw(quads, "No earlier saves.", output_width * 0.5f,
                      (frame.content_top + frame.content_bottom) * 0.5f - line * 0.5f, scale, theme.text_dim, TextAlign::Center);
        }
        const int count = static_cast<int>(earlier_.size());
        const float content = static_cast<float>(count) * row_h;
        earlier_scroll_ = std::clamp(earlier_scroll_, 0.0f, std::max(0.0f, content - list.h));
        const bool scrolls = content > list.h + 0.5f;
        const int first = std::max(0, static_cast<int>(std::floor(earlier_scroll_ / row_h)));
        const int last = std::min(count, static_cast<int>(std::ceil((earlier_scroll_ + list.h) / row_h)));
        row_first_ = first;
        quads.set_clip(list);
        const float row_w = list.w - (scrolls ? scroll_bar_width(scale) + line * 0.6f : 0.0f);
        for (int index = first; index < last; ++index) {
            const EarlierSave& save = earlier_[static_cast<std::size_t>(index)];
            const PixelRect row{list.x, list.y + index * row_h - earlier_scroll_, row_w, row_h - menu_row_gap(line)};
            row_rects_.push_back(visible_part(row, list));
            const bool focused = index == earlier_focus_ && overlay_ == Overlay::None;
            draw_menu_row(quads, row, focused, scale);
            if (!focused) draw_menu_rule(quads, row);
            const Color& ink = menu_row_text(focused);
            const Color dim = focused ? with_alpha(theme.background, 0.7f) : theme.text_dim;
            const float pad = menu_row_pad(line);
            const float sub_scale = scale * 0.75f;
            const float step = line * 1.3f;
            const float top = row.y + (row.h - step - TextRenderer::line_height(sub_scale)) * 0.5f;
            text.draw(quads, describe_time(save.saved_at), row.x + pad, top, scale, ink);
            text.draw(quads, save.reason, row.x + pad, top + step, sub_scale, dim);
            text.draw(quads, "Saved on " + (save.made_on.empty() ? own_name_ : save.made_on), row.x + row.w - pad,
                      row.y + (row.h - TextRenderer::line_height(scale * 0.85f)) * 0.5f, scale * 0.85f, dim, TextAlign::Right);
        }
        quads.clear_clip();
        if (scrolls) draw_scroll_bar(quads, list, earlier_scroll_ / content, list.h / content, line, scale);
    }

    // The dialogs: the page stays where it is behind them, dimmed.
    if (overlay_ == Overlay::Confirm || overlay_ == Overlay::ResumeConfirm || overlay_ == Overlay::Busy) {
        quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, Color{0.0f, 0.0f, 0.0f, 0.62f});
        const float width = std::round(std::min(output_width * 0.62f, line * 30.0f));
        const float pad = line * 1.0f;
        const float inner = width - pad * 2.0f;
        const float title_scale_d = scale * 1.2f;
        const std::string& heading_d = overlay_ == Overlay::Busy ? busy_ : dialog_title_;
        const std::size_t title_lines = TextRenderer::wrap(heading_d, inner, title_scale_d).size();
        float height = pad * 2.0f + TextRenderer::wrapped_line_step(title_scale_d) * title_lines;
        const bool strip = overlay_ == Overlay::Confirm && !dialog_from_name_.empty();
        const float strip_h = line * 3.0f;
        if (strip) height += line * 0.8f + strip_h;
        const float body_scale = scale * 0.85f;
        if (overlay_ != Overlay::Busy) {
            for (const std::string& body : dialog_lines_) {
                height += line * 0.5f + TextRenderer::wrapped_line_step(body_scale) * TextRenderer::wrap(body, inner, body_scale).size();
            }
        }
        const PixelRect dialog{std::round((output_width - width) * 0.5f), std::round((output_height - height) * 0.45f), width, height};
        quads.draw_rounded_rect(dialog, line * 0.5f, theme.panel);
        float pen_y = dialog.y + pad;
        pen_y += text.draw_wrapped(quads, heading_d, dialog.x + pad, pen_y, inner, title_scale_d, theme.text);
        if (strip) {
            pen_y += line * 0.8f;
            const PixelRect band{dialog.x + pad, pen_y, inner, strip_h};
            quads.draw_rounded_rect(band, line * 0.35f, theme.background);
            const float left_c = band.x + band.w * 0.25f;
            const float right_c = band.x + band.w * 0.75f;
            const float date_step = line * 1.2f;
            const float name_y = band.y + (band.h - date_step - TextRenderer::line_height(scale * 0.75f)) * 0.5f;
            text.draw(quads, elide_right(dialog_from_name_, band.w * 0.4f, scale * 0.85f), left_c, name_y, scale * 0.85f, theme.text, TextAlign::Center);
            text.draw(quads, dialog_from_date_, left_c, name_y + date_step, scale * 0.75f, theme.text_dim, TextAlign::Center);
            text.draw(quads, elide_right(dialog_to_name_, band.w * 0.4f, scale * 0.85f), right_c, name_y, scale * 0.85f, theme.text, TextAlign::Center);
            text.draw(quads, dialog_to_date_, right_c, name_y + date_step, scale * 0.75f, theme.text_dim, TextAlign::Center);
            // An arrow from one to the other: a shaft and a head.
            const float mid_x = band.x + band.w * 0.5f;
            const float mid_y = band.y + band.h * 0.5f;
            const float shaft = line * 0.9f;
            const float thick = std::max(2.0f, std::round(scale * 0.6f));
            quads.draw_solid(PixelRect{mid_x - shaft * 0.5f, mid_y - thick * 0.5f, shaft, thick}, theme.accent);
            // The head, as thin slices narrowing to its point.
            const int slices = 8;
            const float head = line * 0.4f;
            for (int step = 0; step < slices; ++step) {
                const float size = line * 0.7f * (1.0f - static_cast<float>(step) / slices);
                quads.draw_solid(PixelRect{mid_x + shaft * 0.5f - head * 0.5f + step * head / slices, mid_y - size * 0.5f,
                                           head / slices + 0.5f, size}, theme.accent);
            }
            pen_y += strip_h;
        }
        if (overlay_ != Overlay::Busy) {
            for (std::size_t index = 0; index < dialog_lines_.size(); ++index) {
                pen_y += line * 0.5f;
                pen_y += text.draw_wrapped(quads, dialog_lines_[index], dialog.x + pad, pen_y, inner, body_scale,
                                           index < dialog_plain_lines_ ? theme.text : theme.text_dim);
            }
        }
    }

    choice_rects_.clear();
    if (overlay_ == Overlay::Choice) {
        // A conflict: which of the two saves both devices keep. The two are
        // rows to pick between, each with its date.
        quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, Color{0.0f, 0.0f, 0.0f, 0.62f});
        const float width = std::round(std::min(output_width * 0.62f, line * 30.0f));
        const float pad = line * 1.0f;
        const float inner = width - pad * 2.0f;
        const float title_scale_d = scale * 1.2f;
        const float body_scale = scale * 0.85f;
        const float option_h = menu_row_height(line);
        const float height = pad * 2.0f +
                             TextRenderer::wrapped_line_step(title_scale_d) *
                                 TextRenderer::wrap(choice_title_, inner, title_scale_d).size() +
                             line * 0.5f +
                             TextRenderer::wrapped_line_step(body_scale) *
                                 TextRenderer::wrap(choice_line_, inner, body_scale).size() +
                             line * 0.8f + option_h * 2.0f + menu_row_gap(line);
        const PixelRect dialog{std::round((output_width - width) * 0.5f), std::round((output_height - height) * 0.45f),
                               width, height};
        quads.draw_rounded_rect(dialog, line * 0.5f, theme.panel);
        float pen_y = dialog.y + pad;
        pen_y += text.draw_wrapped(quads, choice_title_, dialog.x + pad, pen_y, inner, title_scale_d, theme.text);
        pen_y += line * 0.5f;
        pen_y += text.draw_wrapped(quads, choice_line_, dialog.x + pad, pen_y, inner, body_scale, theme.text_dim);
        pen_y += line * 0.8f;
        for (int index = 0; index < 2; ++index) {
            const PixelRect option{dialog.x + pad, pen_y, inner, option_h};
            choice_rects_.push_back(option);
            const bool focused = index == choice_focus_;
            if (focused) {
                draw_menu_row(quads, option, true, scale);
            } else {
                quads.draw_rounded_rect(option, line * 0.35f, theme.background);
            }
            const float row_pad = menu_row_pad(line);
            const float baseline = option.y + (option.h - line) * 0.5f;
            text.draw(quads, elide_right(choice_names_[index], inner * 0.55f, scale), option.x + row_pad, baseline,
                      scale, menu_row_text(focused));
            text.draw(quads, choice_dates_[index], option.x + option.w - row_pad,
                      option.y + (option.h - TextRenderer::line_height(scale * 0.85f)) * 0.5f, scale * 0.85f,
                      focused ? with_alpha(theme.background, 0.7f) : theme.text_dim, TextAlign::Right);
            pen_y += option_h + menu_row_gap(line);
        }
    }

    guide_hits_.clear();
    if (overlay_ == Overlay::Notice) {
        render_notice_popup(quads, text, glyphs, output_width, output_height, notice_);
    } else {
        draw_control_guide(quads, text, glyphs, guide_prompts(), frame.panel.x + frame.panel.w, frame.guide_y,
                           scale * 0.9f, theme.text, TextAlign::Right, &guide_hits_);
    }
}

}  // namespace ds
