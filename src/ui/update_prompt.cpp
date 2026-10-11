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
}

void UpdatePrompt::choose() {
    if (updater_ == nullptr) return;
    const update::Status status = updater_->status();
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
    updater_->check(true);
    waiting_round_ = updater_->status().round;
    waiting_asked_ = true;
}

void UpdatePrompt::show_offer(const update::Status& status) {
    const bool one = status.offers.size() == 1;
    message_ = one ? status.offers.front().component.name + " " + describe_build(status.offers.front().release.build) +
                         " is available."
                   : std::string("Updates are available.");
    lines_.clear();
    bool replaces_dualscreen = false;
    for (const update::Offer& offer : status.offers) {
        const std::string newest = offer.component.name + " " + describe_build(offer.release.build);
        if (offer.component.installed == update::kNotInstalled) {
            lines_.push_back(newest + " will be downloaded" +
                             (offer.component.id == Component::Cemu ? " to play Wii U games." : "."));
        } else {
            lines_.push_back(newest + " will replace " +
                             (offer.component.installed > 0 ? describe_build(offer.component.installed)
                                                            : std::string("this local build")) +
                             ".");
        }
        if (offer.component.id == Component::Dualscreen) replaces_dualscreen = true;
    }
    if (replaces_dualscreen) {
        lines_.push_back(updater_ != nullptr && updater_->restarts_after_install()
                             ? "Dualscreen will restart when it is done."
                             : "Android will ask to replace Dualscreen with it.");
    }
    offer_key_ = offer_key(status);
    offer_from_start_ = false;
    popup_ = Popup::Offer;
    cursor_.reset(2, 0);
}

void UpdatePrompt::show_notice(std::string message, std::vector<std::string> lines) {
    message_ = std::move(message);
    lines_ = std::move(lines);
    popup_ = Popup::Notice;
    cursor_.reset(1, 0);
}

UpdatePromptResult UpdatePrompt::poll(bool may_offer) {
    UpdatePromptResult result;
    if (updater_ == nullptr) return result;
    status_ = updater_->status();

    // A check's answer.
    const bool answered = status_.phase == Phase::Checked || status_.phase == Phase::Failed;
    if (waiting_round_ != 0 && status_.round == waiting_round_ && answered) {
        if (waiting_asked_) {
            waiting_round_ = 0;
            if (status_.phase == Phase::Failed) {
                show_notice("Dualscreen could not check for updates.", {status_.error});
            } else if (!status_.offers.empty()) {
                show_offer(status_);
            } else {
                int installed = update::kLocalBuild;
                for (const update::ComponentSpec& component : updater_->components()) {
                    if (component.id == Component::Dualscreen) installed = component.installed;
                }
                show_notice("Dualscreen is up to date.", {"This is " + describe_build(installed) + "."});
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
                show_notice(installing_cemu_ ? "Cemu is installed." : "The update is installed.",
                            {installing_cemu_ ? "Wii U games will be played with it from now on." : ""});
                if (lines_.front().empty()) lines_.clear();
            }
            if (installing_cemu_) result.cemu_installed = true;
            installing_cemu_ = false;
        } else if (status_.phase == Phase::Failed) {
            installing_cemu_ = false;
            show_notice("The update was not installed.", {status_.error});
        } else if (status_.phase == Phase::Checked) {
            // Cancelled.
            installing_cemu_ = false;
            popup_ = Popup::None;
        }
    }
    return result;
}

std::vector<PopupButton> UpdatePrompt::buttons() const {
    switch (popup_) {
        case Popup::Offer: return {{"Update", ""}, {"Not Now", ""}};
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
        case Popup::Offer:
            if (pressed == 0) {
                installing_cemu_ = false;
                for (const update::Offer& offer : updater_->status().offers) {
                    if (offer.component.id == Component::Cemu) installing_cemu_ = true;
                }
                updater_->install();
                status_ = updater_->status();
                install_round_ = status_.round;
                popup_ = Popup::Progress;
                cursor_.reset(1, 0);
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
    view.on_cancel = popup_ == Popup::Progress || (popup_ == Popup::Offer && cursor_.focus() == 1);
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
