#include "ui/update_prompt.hpp"

#include <algorithm>
#include <cmath>

#include "config/paths.hpp"
#include "util/log.hpp"
#include "ui/ui_theme.hpp"

namespace ds {
namespace {

using update::Component;
using update::Phase;

// What an offer was, to remember it was declined: "Dualscreen 15, Cemu 4".
std::string offer_key(const update::Status& status) {
    std::string key;
    for (const update::Offer& offer : status.offers) {
        if (!key.empty()) key += ", ";
        key += offer.component.name + " " + std::to_string(offer.release.build);
    }
    return key;
}

std::string percent(float progress) {
    return std::to_string(static_cast<int>(std::floor(std::clamp(progress, 0.0f, 1.0f) * 100.0f))) + "%";
}

}  // namespace

std::string describe_build(int build) {
    return build > 0 ? "build " + std::to_string(build) : std::string("a local build");
}

void UpdatePrompt::check_at_start() {
    if (updater_ == nullptr || offered_at_start_) return;
    offered_at_start_ = true;
    updater_->check(false);
    waiting_round_ = updater_->status().round;
    waiting_asked_ = false;
    waiting_only_.reset();
}

void UpdatePrompt::set_peer_name(const std::string& name) {
    if (name == peer_name_) return;
    // Another device, or none: what the last one said is not this one's.
    peer_name_ = name;
    peer_state_ = PeerState::None;
}

void UpdatePrompt::peer_answered(const net::UpdateAnswerMessage& answer) {
    if (peer_state_ == PeerState::None) return;
    if (answer.answer == net::UpdateAnswer::Checking) {
        peer_state_ = PeerState::Checking;
    } else {
        peer_state_ = PeerState::Answered;
        peer_answer_ = answer;
    }
}

bool UpdatePrompt::waiting_on_peer() const {
    const auto waited = std::chrono::steady_clock::now() - peer_asked_at_;
    // Long enough for it to hear and say it is checking; and once it has,
    // for a check of its own over the same network as this one's.
    if (peer_state_ == PeerState::Asked) return waited < std::chrono::seconds(2);
    if (peer_state_ == PeerState::Checking) return waited < std::chrono::seconds(20);
    return false;
}

void UpdatePrompt::install_on_peer() {
    if (peer_ask_) peer_ask_(net::UpdateRequest::Install);
    log_info("updates: asking " + peer_name_ + " to install " + describe_build(offer_peer_answer_.newest_build));
}

void UpdatePrompt::host_requests(net::UpdateRequest request) {
    net::UpdateAnswerMessage answer;
    if (updater_ == nullptr) {
        host_answer_ = answer;
        return;
    }
    const update::Status status = updater_->status();
    if (request == net::UpdateRequest::Check) {
        if (status.phase == Phase::Downloading || status.phase == Phase::Installing) {
            // Installing one already: nothing more to offer.
            host_answer_ = answer;
            return;
        }
        updater_->check(true);
        host_round_ = updater_->status().round;
        answer.answer = net::UpdateAnswer::Checking;
        host_answer_ = answer;
        return;
    }
    log_info("updates: the host asked for the update here to be installed");
    if (status.phase == Phase::Checked && !status.offers.empty()) {
        // As Update on this device's own offer: how it goes is shown here.
        start_install();
    } else {
        // Found no longer, or downloading already: as Settings' Updates row.
        choose();
    }
}

void UpdatePrompt::choose(std::optional<update::Component> only) {
    if (updater_ == nullptr) return;
    const update::Status status = updater_->status();
    // The device connected is asked about its own as this one checks.
    const bool asks_peer = !only && peer_ask_ && !peer_name_.empty() &&
                           status.phase != Phase::Downloading && status.phase != Phase::Installing;
    if (asks_peer) {
        peer_ask_(net::UpdateRequest::Check);
        peer_state_ = PeerState::Asked;
        peer_asked_at_ = std::chrono::steady_clock::now();
    } else {
        peer_state_ = PeerState::None;
    }
    if (updater_->busy()) {
        // Downloading already: show how far it has got.
        if (status.phase == Phase::Downloading || status.phase == Phase::Installing) {
            install_round_ = status.round;
            popup_ = Popup::Progress;
            cursor_.reset(1, 0);
        } else {
            // Checking: its answer is this press's.
            waiting_round_ = status.round;
            waiting_asked_ = true;
        }
        return;
    }
    updater_->check(true, only);
    waiting_round_ = updater_->status().round;
    waiting_asked_ = true;
    waiting_only_ = only;
}

std::string UpdatePrompt::component_value(update::Component id) const {
    if (updater_ == nullptr) return std::string();
    std::string name;
    int installed = update::kNotInstalled;
    bool core = false;
    std::string built;
    for (const update::ComponentSpec& component : updater_->components()) {
        if (component.id == id) {
            name = component.name;
            installed = component.installed;
            core = component.source == update::Source::Buildbot;
            // "<date> <hash>": the date, which is what tells builds apart to
            // a person.
            built = component.installed_version.substr(0, component.installed_version.find(' '));
        }
    }
    const bool this_one = status_.current == name;
    switch (status_.phase) {
        case Phase::Checking:
            if (waiting_only_ == id) return "Checking...";
            break;
        case Phase::Downloading:
            if (this_one) {
                return status_.progress >= 0.0f ? "Downloading " + percent(status_.progress) : "Downloading...";
            }
            break;
        case Phase::Installing:
            if (this_one) return "Installing...";
            break;
        case Phase::Checked:
            for (const update::Offer& offer : status_.offers) {
                if (offer.component.id == id && installed != update::kNotInstalled) {
                    return core ? std::string("Update available")
                                : "Build " + std::to_string(offer.release.build) + " available";
                }
            }
            break;
        default: break;
    }
    if (installed == update::kNotInstalled) return "Not installed";
    if (core) return built.size() == 10 ? "Built " + built : std::string("Installed");
    return installed > 0 ? "Build " + std::to_string(installed) : std::string("Installed");
}

void UpdatePrompt::show_offer(const update::Status& status, bool own_failed) {
    offer_own_ = !own_failed && !status.offers.empty();
    const bool one = status.offers.size() == 1;
    message_ = !offer_own_ ? peer_name_ + " can be updated to " + describe_build(offer_peer_answer_.newest_build) + "."
               : one       ? status.offers.front().component.name + " " +
                           update::describe_release(status.offers.front().component, status.offers.front().release) +
                           " is available."
                           : std::string("Updates are available.");
    lines_.clear();
    if (!offer_own_) {
        lines_.push_back(own_failed ? "Dualscreen could not check for updates on this device."
                                    : "This device is up to date.");
    }
    bool replaces_dualscreen = false;
    for (const update::Offer& offer : status.offers) {
        const std::string newest =
            offer.component.name + " " + update::describe_release(offer.component, offer.release);
        const char* plays = offer.component.id == Component::Cemu          ? " to play Wii U games."
                            : offer.component.id == Component::DsCore      ? " to play DS games."
                            : offer.component.id == Component::ThreeDsCore ? " to play 3DS games."
                                                                          : ".";
        if (offer.component.installed == update::kNotInstalled) {
            lines_.push_back(newest + " will be downloaded" + plays);
        } else if (offer.component.source == update::Source::Buildbot) {
            lines_.push_back(newest + " will replace the one here.");
        } else {
            lines_.push_back(newest + " will replace " +
                             (offer.component.installed > 0 ? describe_build(offer.component.installed)
                                                            : std::string("this local build")) +
                             ".");
        }
        if (offer.component.id == Component::Dualscreen) replaces_dualscreen = true;
    }
    if (offer_own_ && replaces_dualscreen) {
        const bool restarts = updater_ != nullptr && updater_->restarts_after_install();
        lines_.push_back(!restarts         ? "Android will ask to replace Dualscreen with it."
                         : !game_playing_  ? "Dualscreen will restart when it is done."
                         : game_comes_back_ ? "Dualscreen will restart when it is done, and start this game again."
                                            : "The game will close, and Dualscreen will restart when it is done.");
    }
    if (offer_peer_) {
        // The device connected installs its own, as its Settings would.
        const net::UpdateAnswerMessage& peer = offer_peer_answer_;
        if (offer_own_) {
            lines_.push_back(peer_name_ + " can be updated to " + describe_build(peer.newest_build) + " too.");
        }
        lines_.push_back(peer.restarts ? peer_name_ + " will download it, and restart when it is done."
                                       : peer_name_ + " will download it, and ask to replace Dualscreen with it.");
    }
    offer_key_ = offer_key(status);
    offer_from_start_ = false;
    popup_ = Popup::Offer;
    cursor_.reset(static_cast<int>(buttons().size()), 0);
}

void UpdatePrompt::show_notice(std::string message, std::vector<std::string> lines) {
    message_ = std::move(message);
    lines_ = std::move(lines);
    popup_ = Popup::Notice;
    cursor_.reset(1, 0);
}

UpdatePromptResult UpdatePrompt::poll(bool may_offer) {
    UpdatePromptResult result;
    result.answer_host = host_answer_;
    host_answer_.reset();
    if (updater_ == nullptr) return result;
    status_ = updater_->status();

    // The check the host asked for, answered.
    if (host_round_ != 0 && status_.round == host_round_ &&
        (status_.phase == Phase::Checked || status_.phase == Phase::Failed)) {
        host_round_ = 0;
        net::UpdateAnswerMessage answer;
        answer.answer = status_.phase == Phase::Failed ? net::UpdateAnswer::Failed : net::UpdateAnswer::UpToDate;
        for (const update::Offer& offer : status_.offers) {
            if (offer.component.id != Component::Dualscreen) continue;
            answer.answer = net::UpdateAnswer::Available;
            answer.newest_build = offer.release.build;
            answer.installed_build = std::max(0, offer.component.installed);
            answer.restarts = updater_->restarts_after_install();
        }
        result.answer_host = answer;
    } else if (host_round_ != 0 && status_.round != host_round_) {
        host_round_ = 0;
    }

    // A check's answer.
    const bool answered = status_.phase == Phase::Checked || status_.phase == Phase::Failed;
    if (waiting_round_ != 0 && status_.round == waiting_round_ && answered) {
        if (waiting_asked_ && !waiting_only_ && waiting_on_peer()) {
            // The two answers are offered together.
        } else if (waiting_asked_) {
            waiting_round_ = 0;
            offer_peer_ = !waiting_only_ && peer_state_ == PeerState::Answered &&
                          peer_answer_.answer == net::UpdateAnswer::Available;
            if (offer_peer_) offer_peer_answer_ = peer_answer_;
            peer_state_ = PeerState::None;
            if (status_.phase == Phase::Failed && offer_peer_) {
                show_offer(status_, true);
            } else if (status_.phase == Phase::Failed) {
                show_notice(waiting_only_ ? "Dualscreen could not reach Cemu's builds."
                                          : "Dualscreen could not check for updates.",
                            {status_.error});
            } else if (!status_.offers.empty() || offer_peer_) {
                show_offer(status_);
            } else {
                const Component asked_about = waiting_only_.value_or(Component::Dualscreen);
                int installed = update::kLocalBuild;
                std::string name = "Dualscreen";
                bool core = false;
                for (const update::ComponentSpec& component : updater_->components()) {
                    if (component.id == asked_about) {
                        installed = component.installed;
                        name = component.name;
                        core = component.source == update::Source::Buildbot;
                    }
                }
                show_notice(name + " is up to date.",
                            {core ? std::string("It is the buildbot's latest.")
                                  : "This is " + describe_build(installed) + "."});
            }
        } else if (may_offer && popup_ == Popup::None) {
            // At start-up: said only when there is something to say, and
            // only somewhere it can be.
            waiting_round_ = 0;
            std::vector<uint8_t> declined;
            const bool was_declined =
                !declined_file_.empty() && read_file(declined_file_, declined, nullptr) &&
                std::string(declined.begin(), declined.end()) == offer_key(status_);
            if (status_.phase == Phase::Checked && !status_.offers.empty()) {
                if (was_declined) {
                    log_info("updates: " + offer_key(status_) + " was turned down before; not offering it again");
                } else {
                    offer_peer_ = false;
                    show_offer(status_);
                    offer_from_start_ = true;
                }
            }
        } else if (status_.phase == Phase::Failed || status_.offers.empty()) {
            waiting_round_ = 0;
        }
    } else if (waiting_round_ != 0 && status_.round != waiting_round_) {
        // Overtaken by another check or an install.
        waiting_round_ = 0;
    }

    // An install's outcome.
    if (popup_ == Popup::Progress && status_.round == install_round_) {
        if (status_.phase == Phase::Installed) {
            if (status_.restart) {
                // The popup stays up, saying so, while the app shuts down.
                if (!restart_reported_) {
                    restart_reported_ = true;
                    result.restart = true;
                }
            } else if (status_.handed_to_system) {
                popup_ = Popup::None;
            } else {
                // An emulator, new to this device or updated: said by name,
                // and what it plays.
                const bool one = installing_.size() == 1;
                const bool fresh = one && installing_.front().component.installed == update::kNotInstalled;
                const std::string name = one ? installing_.front().component.name : std::string();
                show_notice(one ? name + (fresh ? " is installed." : " is up to date now.")
                                : std::string("The updates are installed."),
                            {});
                if (one && fresh) {
                    const Component id = installing_.front().component.id;
                    lines_ = {std::string(id == Component::Cemu     ? "Wii U"
                                          : id == Component::DsCore ? "DS"
                                                                    : "3DS") +
                              " games will be played with it from now on."};
                }
            }
            for (const update::Offer& offer : installing_) {
                result.installed.push_back(offer.component.id);
                if (offer.component.id == Component::Cemu) result.cemu_installed = true;
            }
            installing_.clear();
        } else if (status_.phase == Phase::Failed) {
            installing_.clear();
            show_notice("The update was not installed.", {status_.error});
        } else if (status_.phase == Phase::Checked) {
            // Cancelled.
            installing_.clear();
            popup_ = Popup::None;
        }
    }
    return result;
}

bool UpdatePrompt::install_closes_game() const {
    if (!game_playing_ || updater_ == nullptr || !updater_->restarts_after_install()) return false;
    for (const update::Offer& offer : status_.offers) {
        if (offer.component.id == Component::Dualscreen) return true;
    }
    return false;
}

void UpdatePrompt::start_install() {
    installing_ = updater_->status().offers;
    updater_->install();
    status_ = updater_->status();
    install_round_ = status_.round;
    popup_ = Popup::Progress;
    cursor_.reset(1, 0);
}

std::vector<PopupButton> UpdatePrompt::buttons() const {
    switch (popup_) {
        case Popup::Offer: {
            // Download, when nothing it offers is here yet.
            bool all_new = !status_.offers.empty();
            for (const update::Offer& offer : status_.offers) {
                if (offer.component.installed != update::kNotInstalled) all_new = false;
            }
            if (!offer_own_) return {{"Update " + peer_name_, ""}, {"Not Now", ""}};
            if (offer_peer_) {
                // One above the other, each with the build it gets.
                std::string own;
                for (const update::Offer& offer : status_.offers) {
                    if (offer.component.id == Component::Dualscreen) own = "Build " + std::to_string(offer.release.build);
                }
                return {{all_new ? "Download" : "Update", own},
                        {"Update " + peer_name_ + " Too", "Build " + std::to_string(offer_peer_answer_.newest_build)},
                        {"Not Now", ""}};
            }
            return {{all_new ? "Download" : "Update", ""}, {"Not Now", ""}};
        }
        case Popup::CloseGame: return {{"Cancel", ""}, {"Update", ""}};
        case Popup::Progress:
            if (status_.phase == Phase::Downloading) return {{"Cancel", ""}};
            return {};
        case Popup::Notice: return {{"OK", ""}};
        case Popup::None: break;
    }
    return {};
}

PopupText UpdatePrompt::words() const {
    if (popup_ != Popup::Progress) return PopupText{message_, lines_, 0};
    PopupText text;
    switch (status_.phase) {
        case Phase::Installed:
            text.message = status_.restart ? "Dualscreen is restarting." : "Done.";
            text.lines = {" "};
            break;
        case Phase::Installing:
            text.message = "Installing " + status_.current + "...";
            text.lines = {" "};
            break;
        default:
            text.message = "Downloading " + status_.current + "...";
            text.lines = {status_.progress >= 0.0f ? percent(status_.progress) : std::string(" ")};
            break;
    }
    return text;
}

PopupGeometry UpdatePrompt::geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    float height = popup_text_height(words(), width, output_height);
    // Room for the bar a download draws under its words.
    if (popup_ == Popup::Progress) height += TextRenderer::line_height(popup_scale(output_height)) * 1.2f;
    return popup_geometry(output_width, output_height, height, buttons());
}

UpdatePromptResult UpdatePrompt::update(const std::vector<UiAction>& actions,
                                        const std::vector<PointerEvent>& pointers, float output_width,
                                        float output_height) {
    UpdatePromptResult result;
    if (popup_ == Popup::None || updater_ == nullptr) return result;
    const std::vector<PopupButton> labels = buttons();
    if (cursor_.count() != static_cast<int>(labels.size())) cursor_.reset(static_cast<int>(labels.size()), 0);
    const int pressed = cursor_.update(actions, pointers, geometry(output_width, output_height));
    if (pressed == PopupCursor::kNone) return result;
    switch (popup_) {
        case Popup::Offer: {
            // Which of the offer's buttons: this device's update, the
            // peer's as well, or the peer's alone; the last is Not Now.
            const bool not_now = pressed < 0 || pressed == static_cast<int>(labels.size()) - 1;
            peer_too_ = !not_now && offer_peer_ && (!offer_own_ || pressed == 1);
            if (!not_now && !offer_own_) {
                install_on_peer();
                show_notice(peer_name_ + " will update itself.",
                            {"How it is going will be shown on " + peer_name_ + "."});
            } else if (!not_now && install_closes_game()) {
                // Asked again, as Quit is: the restart closes the game.
                message_ = "Are you sure you want to update now?";
                lines_ = {game_comes_back_ ? "The game will close, and start again when the update is done."
                                           : "The game will close."};
                if (!game_progress_kept_) lines_.push_back("Unsaved progress will be lost.");
                popup_ = Popup::CloseGame;
                // On the answer, as Quit's is; B is the way back out.
                cursor_.reset(2, 1);
            } else if (!not_now) {
                if (peer_too_) install_on_peer();
                start_install();
            } else {
                // Not now: and, offered at start-up, not at the next start
                // either.
                if (offer_from_start_ && !declined_file_.empty()) {
                    ensure_directory(path_dirname(declined_file_));
                    std::string ignored;
                    write_file_atomically(declined_file_, offer_key_.data(), offer_key_.size(), &ignored);
                }
                popup_ = Popup::None;
            }
            break;
        }
        case Popup::CloseGame:
            if (pressed == 1) {
                if (peer_too_) install_on_peer();
                start_install();
            } else {
                popup_ = Popup::None;
            }
            break;
        case Popup::Progress:
            // Its one button, or B: stop the download. It closes once the
            // updater has.
            if (status_.phase == Phase::Downloading) updater_->cancel();
            break;
        case Popup::Notice:
            popup_ = Popup::None;
            break;
        case Popup::None: break;
    }
    return result;
}

void UpdatePrompt::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                          float output_width, float output_height) {
    (void)glyphs;
    if (popup_ == Popup::None) return;
    const PopupGeometry box = geometry(output_width, output_height);
    const std::vector<PopupButton> labels = buttons();
    draw_popup(quads, text, output_width, output_height, box, labels, cursor_.focus(),
               cursor_.shown() && !labels.empty());
    const float bottom =
        draw_popup_text(quads, text, words(), box.body.x, box.body.y, box.body.w, output_height);
    if (popup_ == Popup::Progress) {
        // How far the download has got, as a bar, full while installing.
        const UiTheme& theme = ui_theme();
        const float line = TextRenderer::line_height(popup_scale(output_height));
        const float bar_h = std::max(4.0f, std::round(line * 0.35f));
        const PixelRect track{box.body.x, std::round(bottom + line * 0.6f), box.body.w, bar_h};
        quads.draw_rounded_rect(track, bar_h * 0.5f, popup_rule());
        float filled = status_.phase == Phase::Downloading ? std::max(0.0f, status_.progress) : 1.0f;
        filled = std::clamp(filled, 0.0f, 1.0f);
        if (filled > 0.0f) {
            quads.draw_rounded_rect(PixelRect{track.x, track.y, std::max(bar_h, track.w * filled), bar_h},
                                    bar_h * 0.5f, theme.accent);
        }
    }
}

void UpdatePrompt::describe_for_sound(UiSoundView& view) const {
    if (popup_ == Popup::None) return;
    mix_sound_state(view.focus, static_cast<uint64_t>(cursor_.focus() + 1) * 7919u);
    mix_sound_state(view.rest, static_cast<uint64_t>(popup_) + 101u);
    mix_sound_state(view.rest, message_);
    view.on_cancel = popup_ == Popup::Progress ||
                     (popup_ == Popup::Offer && cursor_.focus() == static_cast<int>(buttons().size()) - 1) ||
                     (popup_ == Popup::CloseGame && cursor_.focus() == 0);
}

std::string UpdatePrompt::row_value() const {
    if (updater_ == nullptr) return std::string();
    switch (status_.phase) {
        case Phase::Checking: return "Checking...";
        case Phase::Downloading:
            return status_.progress >= 0.0f ? "Downloading " + percent(status_.progress) : "Downloading...";
        case Phase::Installing: return "Installing...";
        case Phase::Installed:
            if (status_.restart) return "Restarting...";
            break;
        case Phase::Checked:
            for (const update::Offer& offer : status_.offers) {
                if (offer.component.id == Component::Dualscreen) {
                    return "Build " + std::to_string(offer.release.build) + " available";
                }
            }
            break;
        case Phase::Idle:
        case Phase::Failed: break;
    }
    for (const update::ComponentSpec& component : updater_->components()) {
        if (component.id == Component::Dualscreen) {
            return component.installed > 0 ? "Build " + std::to_string(component.installed) : "Local build";
        }
    }
    return std::string();
}

}  // namespace ds
