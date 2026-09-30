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
enum class UiIcon { Get, Send, History, Delete, SteamDeck, Switch, Pc, Count };

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
    if (day == nullptr) return format_save_time(when, style);
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

// A row of one of these lists, in the menus' new look: under the cursor, a
// light rounded fill inside a ring of the accent colour; elsewhere nothing but
// a faint rule beneath it.
void draw_list_row(QuadRenderer& quads, const PixelRect& rect, bool focused, bool ruled,
                   float scale) {
    const UiTheme& theme = ui_theme();
    const float line = TextRenderer::line_height(scale);
    const float radius = line * 0.35f;
    if (focused) {
        const float ring = menu_border(scale);
        quads.draw_rounded_rect(rect, radius, theme.accent);
        quads.draw_rounded_rect(PixelRect{rect.x + ring, rect.y + ring, rect.w - ring * 2.0f,
                                          rect.h - ring * 2.0f},
                                std::max(0.0f, radius - ring), theme.text);
    } else if (ruled) {
        quads.draw_solid(PixelRect{rect.x, rect.y + rect.h - 1.0f, rect.w, 1.0f},
                         with_alpha(theme.text_dim, 0.25f));
    }
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
    list_scroll_ = 0;
    reload_saves();
    if (in_game()) {
        open_game(game_in_play_);
    }
    // Every device worth offering, looked for once as the screen opens. Y
    // looks again.
    start_search();
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
    settle_action_focus();
    if (status_game_id_ != game_id) {
        remote_.clear();
        status_game_id_.clear();
        if (searched_) start_status();
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
    return {shown != nullptr ? "Get Save from " + shown->name : std::string("Get Save"),
            shown != nullptr ? "Send Save to " + shown->name : std::string("Send Save"),
            "Earlier Saves", "Delete Save Data"};
}

std::vector<bool> DataManagementScreen::actions_enabled() const {
    const RemoteState* there = remote();
    const bool reachable = there != nullptr && !there->status.playing_now && !job_;
    return {reachable && there->status.versions.has_save, reachable && game_.has_save, true,
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
        case Job::Kind::Fetch: {
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
            open_game(game_.game_id);
            status_queued_ = true;
            break;
        }
        case Job::Kind::Send:
            if (job->ok) {
                show_notice("Sent " + title + " to " + job->device.name + ".");
            } else {
                show_notice("Could not send " + title + " to " + job->device.name + ": " + job->error);
            }
            status_queued_ = true;
            break;
    }
    if (page_ == Page::Game) settle_action_focus();
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
    (void)result;
    const std::vector<bool> enabled = actions_enabled();
    if (!enabled[static_cast<std::size_t>(action)]) return;
    switch (action) {
        case Action::Get: ask(Pending::Get); break;
        case Action::Send: ask(Pending::Send); break;
        case Action::Delete: ask(Pending::Delete); break;
        case Action::Earlier:
            earlier_ = store_.earlier(game_.game_id);
            earlier_focus_ = 0;
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
            std::unique_ptr<Job> job(new Job());
            job->kind = Job::Kind::Fetch;
            job->device = *shown;
            job->game_id = game_id;
            show_busy("Getting " + shown->name + "'s save...");
            start_job(std::move(job));
            break;
        }
        case Pending::Send: {
            const net::SaveDevice* shown = device();
            if (shown == nullptr || job_) break;
            std::unique_ptr<Job> job(new Job());
            job->kind = Job::Kind::Send;
            job->device = *shown;
            job->game_id = game_id;
            show_busy("Sending " + title + " to " + shown->name + "...");
            start_job(std::move(job));
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
            open_game(game_id);
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
            open_game(game_id);
            break;
        }
        case Pending::None: break;
    }
    pending_ = Pending::None;
}

std::vector<GuidePrompt> DataManagementScreen::guide_prompts() const {
    switch (overlay_) {
        case Overlay::Confirm:
        case Overlay::ResumeConfirm:
            return {{ButtonGlyph::B, "Cancel", PromptRole::LeavesScreen},
                    {ButtonGlyph::A, dialog_accept_}};
        case Overlay::Busy:
        case Overlay::Notice:
            return {};
        case Overlay::None: break;
    }
    switch (page_) {
        case Page::List: {
            std::vector<GuidePrompt> prompts;
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
    if (result.restart_with) return result;

    if (overlay_ == Overlay::Notice) {
        if (notice_popup_answered(input_actions, pointers, output_width, output_height)) {
            overlay_ = Overlay::None;
        }
        return result;
    }
    if (overlay_ == Overlay::Busy) return result;

    std::vector<UiAction> actions = input_actions;
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        const ButtonGlyph on_guide = guide_button_at(guide_hits_, pointer.x, pointer.y);
        if (on_guide != ButtonGlyph::Count) {
            handle_guide_tap(on_guide, actions);
            continue;
        }
        if (overlay_ != Overlay::None) continue;
        for (std::size_t index = 0; index < row_rects_.size(); ++index) {
            if (!tapped(pointer, row_rects_[index])) continue;
            if (page_ == Page::List) list_focus_ = list_scroll_ + static_cast<int>(index);
            if (page_ == Page::Game) action_focus_ = static_cast<int>(index);
            if (page_ == Page::Earlier) earlier_focus_ = static_cast<int>(index);
            actions.push_back(UiAction::Confirm);
        }
    }

    for (const UiAction action : actions) {
        if (result.close_requested || result.restart_with) break;
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
            }
            continue;
        }
        if (overlay_ != Overlay::None) break;
        switch (page_) {
            case Page::List: {
                const int count = static_cast<int>(saves_.size());
                if (action == UiAction::Up && count > 0) list_focus_ = (list_focus_ + count - 1) % count;
                if (action == UiAction::Down && count > 0) list_focus_ = (list_focus_ + 1) % count;
                if (action == UiAction::Confirm && count > 0) {
                    open_game(saves_[static_cast<std::size_t>(list_focus_)].game_id);
                }
                if (action == UiAction::Cancel) result.close_requested = true;
                if (action == UiAction::MenuReset) start_search();
                break;
            }
            case Page::Game: {
                const int count = static_cast<int>(Action::Count);
                if (action == UiAction::Up) action_focus_ = (action_focus_ + count - 1) % count;
                if (action == UiAction::Down) action_focus_ = (action_focus_ + 1) % count;
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
                if (action == UiAction::Up && count > 0) earlier_focus_ = (earlier_focus_ + count - 1) % count;
                if (action == UiAction::Down && count > 0) earlier_focus_ = (earlier_focus_ + 1) % count;
                if (action == UiAction::Confirm && count > 0) {
                    pending_earlier_ = earlier_[static_cast<std::size_t>(earlier_focus_)].id;
                    ask(Pending::Restore);
                }
                if (action == UiAction::Cancel) page_ = Page::Game;
                break;
            }
        }
    }
    if (result.close_requested) {
        open_ = false;
    }
    return result;
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
        const float row_h = line * 2.9f;
        const int visible = std::max(1, static_cast<int>((frame.content_bottom - frame.content_top) / row_h));
        if (list_focus_ < list_scroll_) list_scroll_ = list_focus_;
        if (list_focus_ >= list_scroll_ + visible) list_scroll_ = list_focus_ - visible + 1;
        if (saves_.empty()) {
            text.draw(quads, "There are no saves on " + own_name_ + " yet.", output_width * 0.5f,
                      (frame.content_top + frame.content_bottom) * 0.5f - line * 0.5f, scale, theme.text_dim,
                      TextAlign::Center);
        }
        for (int index = list_scroll_; index < static_cast<int>(saves_.size()) && index < list_scroll_ + visible; ++index) {
            const SaveInfo& info = saves_[static_cast<std::size_t>(index)];
            const PixelRect row{frame.panel.x, frame.content_top + (index - list_scroll_) * row_h, frame.panel.w, row_h - line * 0.15f};
            row_rects_.push_back(row);
            const bool focused = index == list_focus_;
            draw_list_row(quads, row, focused, true, scale);
            const Color& ink = menu_row_text(focused);
            const Color dim = focused ? with_alpha(theme.background, 0.7f) : theme.text_dim;
            const float icon_size = line * 1.6f;
            const float pad = line * 0.5f;
            draw_game_icon(info, PixelRect{row.x + pad, row.y + (row.h - icon_size) * 0.5f, icon_size, icon_size});
            const std::string when = info.has_save ? describe_time(info.saved_at) : "No save";
            const float date_scale = scale * 0.8f;
            const float date_w = TextRenderer::measure_width(when, date_scale);
            const float text_x = row.x + pad * 2.0f + icon_size;
            const float room = row.x + row.w - pad * 3.0f - date_w - text_x;
            const float sub_scale = scale * 0.72f;
            const float step = line * 1.25f;
            const float block = step + TextRenderer::line_height(sub_scale);
            const float top = row.y + (row.h - block) * 0.5f;
            text.draw(quads, elide_right(info.display_title(), room, scale), text_x, top, scale, ink);
            text.draw(quads, elide_right(info.publisher, room, sub_scale), text_x, top + step, sub_scale, dim);
            text.draw(quads, when, row.x + row.w - pad, row.y + (row.h - TextRenderer::line_height(date_scale)) * 0.5f,
                      date_scale, dim, TextAlign::Right);
        }
        // On the line the prompts' buttons sit on.
        text.draw(quads, "Saves on " + own_name_, frame.panel.x,
                  frame.guide_y + control_guide_height(scale * 0.9f) * glyphs.button_centre() -
                      TextRenderer::line_height(scale * 0.75f) * 0.5f,
                  scale * 0.75f, theme.text_dim);
    } else if (page_ == Page::Game) {
        const float left_w = std::round(frame.panel.w * 0.55f);
        const float row_h = line * 2.4f;
        const std::vector<std::string> labels = action_labels();
        const std::vector<bool> enabled = actions_enabled();
        const UiIcon action_icons[] = {UiIcon::Get, UiIcon::Send, UiIcon::History, UiIcon::Delete};
        for (int index = 0; index < static_cast<int>(Action::Count); ++index) {
            const PixelRect row{frame.panel.x, frame.content_top + index * row_h, left_w, row_h - line * 0.15f};
            row_rects_.push_back(row);
            const bool focused = index == action_focus_ && overlay_ == Overlay::None;
            draw_list_row(quads, row, focused, true, scale);
            Color ink = menu_row_text(focused);
            if (!enabled[static_cast<std::size_t>(index)]) ink = with_alpha(focused ? theme.background : theme.text_dim, 0.55f);
            const float icon_size = line * 1.35f;
            const float pad = line * 0.6f;
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
        const float row_h = line * 2.9f;
        if (earlier_.empty()) {
            text.draw(quads, "No earlier saves.", output_width * 0.5f,
                      (frame.content_top + frame.content_bottom) * 0.5f - line * 0.5f, scale, theme.text_dim, TextAlign::Center);
        }
        const int visible = std::max(1, static_cast<int>((frame.content_bottom - frame.content_top) / row_h));
        const int first = std::max(0, earlier_focus_ - visible + 1);
        for (int index = first; index < static_cast<int>(earlier_.size()) && index < first + visible; ++index) {
            const EarlierSave& save = earlier_[static_cast<std::size_t>(index)];
            const PixelRect row{frame.panel.x, frame.content_top + (index - first) * row_h, frame.panel.w, row_h - line * 0.15f};
            row_rects_.push_back(row);
            const bool focused = index == earlier_focus_ && overlay_ == Overlay::None;
            draw_list_row(quads, row, focused, true, scale);
            const Color& ink = menu_row_text(focused);
            const Color dim = focused ? with_alpha(theme.background, 0.7f) : theme.text_dim;
            const float pad = line * 0.5f;
            const float sub_scale = scale * 0.72f;
            const float step = line * 1.25f;
            const float top = row.y + (row.h - step - TextRenderer::line_height(sub_scale)) * 0.5f;
            text.draw(quads, describe_time(save.saved_at), row.x + pad, top, scale, ink);
            text.draw(quads, save.reason, row.x + pad, top + step, sub_scale, dim);
            text.draw(quads, "Saved on " + (save.made_on.empty() ? own_name_ : save.made_on), row.x + row.w - pad,
                      row.y + (row.h - TextRenderer::line_height(scale * 0.8f)) * 0.5f, scale * 0.8f, dim, TextAlign::Right);
        }
        if (first > 0) row_rects_.insert(row_rects_.begin(), static_cast<std::size_t>(first), PixelRect{});
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

    guide_hits_.clear();
    if (overlay_ == Overlay::Notice) {
        render_notice_popup(quads, text, glyphs, output_width, output_height, notice_);
    } else {
        draw_control_guide(quads, text, glyphs, guide_prompts(), frame.panel.x + frame.panel.w, frame.guide_y,
                           scale * 0.9f, theme.text, TextAlign::Right, &guide_hits_);
    }
}

}  // namespace ds
