#include "ui/graphic_packs_prompt.hpp"

#include <algorithm>
#include <cmath>

#include "ui/ui_theme.hpp"

namespace ds {
namespace {

std::string percent(float progress) {
    return std::to_string(static_cast<int>(std::floor(std::clamp(progress, 0.0f, 1.0f) * 100.0f))) + "%";
}

}  // namespace

void GraphicPacksPrompt::choose(bool game_running) {
    if (downloader_ == nullptr) return;
    const update::GraphicPacksDownloader::Status status = downloader_->status();
    if (status.phase == Phase::Downloading || status.phase == Phase::Installing) {
        // Going already: show how far it has got.
        install_round_ = status.round;
        popup_ = Popup::Progress;
        cursor_.reset(1, 0);
        return;
    }
    if (game_running) {
        show_notice("The graphics packs will be ready to download when the game is closed.",
                    {"Cemu reads them as a game starts, so they are left alone while one is being played."});
        return;
    }
    if (downloader_->busy()) {
        // Checking: its answer is this press's.
        waiting_round_ = status.round;
        return;
    }
    downloader_->check();
    waiting_round_ = downloader_->status().round;
}

std::string GraphicPacksPrompt::row_label() const {
    const std::string installed = downloader_ != nullptr ? downloader_->status().installed : std::string();
    return installed.empty() ? "Download Graphics Packs" : "Update Graphics Packs";
}

std::string GraphicPacksPrompt::row_value() const {
    if (downloader_ == nullptr) return std::string();
    const update::GraphicPacksDownloader::Status status = downloader_->status();
    // A download or install in progress isn't shown here: its popup, over
    // the row, shows it.
    if (status.phase == Phase::Checking) return "Checking...";
    if (status.phase == Phase::Checked && !status.installed.empty() && status.newer()) {
        return update::graphic_packs_version(status.latest.name) + " available";
    }
    if (status.installed.empty()) return "Not downloaded";
    return update::graphic_packs_version(status.installed);
}

void GraphicPacksPrompt::show_notice(std::string message, std::vector<std::string> lines) {
    message_ = std::move(message);
    lines_ = std::move(lines);
    popup_ = Popup::Notice;
    cursor_.reset(1, 0);
}

void GraphicPacksPrompt::start_install() {
    fresh_ = downloader_->status().installed.empty();
    downloader_->install();
    status_ = downloader_->status();
    install_round_ = status_.round;
    popup_ = Popup::Progress;
    cursor_.reset(1, 0);
}

void GraphicPacksPrompt::poll() {
    if (downloader_ == nullptr) return;
    status_ = downloader_->status();

    // The row's check, answered.
    if (waiting_round_ != 0 && status_.round == waiting_round_ &&
        (status_.phase == Phase::Checked || status_.phase == Phase::Failed)) {
        waiting_round_ = 0;
        if (status_.phase == Phase::Failed) {
            show_notice("Dualscreen could not reach the graphics packs.", {status_.error});
        } else if (status_.installed.empty()) {
            // None here yet: the row said Download, so that is what it does.
            start_install();
        } else if (status_.newer()) {
            message_ = "Graphics packs " + update::graphic_packs_version(status_.latest.name) + " are available.";
            lines_ = {"They will replace " + update::graphic_packs_version(status_.installed) + "."};
            popup_ = Popup::Offer;
            cursor_.reset(2, 0);
        } else {
            show_notice("The graphics packs are up to date.",
                        {"These are " + update::graphic_packs_version(status_.installed) + "."});
        }
    } else if (waiting_round_ != 0 && status_.round != waiting_round_) {
        waiting_round_ = 0;
    }

    // The install's outcome.
    if (popup_ == Popup::Progress && status_.round == install_round_) {
        if (status_.phase == Phase::Installed) {
            show_notice(fresh_ ? "The graphics packs are downloaded." : "The graphics packs are up to date now.",
                        {"Turn them on for a game in Graphics Packs, in its quick menu."});
        } else if (status_.phase == Phase::Failed) {
            show_notice("The graphics packs were not installed.", {status_.error});
        } else if (status_.phase == Phase::Checked) {
            // Cancelled.
            popup_ = Popup::None;
        }
    }
}

std::vector<PopupButton> GraphicPacksPrompt::buttons() const {
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

PopupText GraphicPacksPrompt::words() const {
    if (popup_ != Popup::Progress) return PopupText{message_, lines_, 0};
    PopupText text;
    switch (status_.phase) {
        case Phase::Installing:
        case Phase::Installed:
            text.message = "Installing the graphics packs...";
            text.lines = {" "};
            break;
        default:
            text.message = "Downloading the graphics packs...";
            text.lines = {status_.progress >= 0.0f ? percent(status_.progress) : std::string(" ")};
            break;
    }
    return text;
}

PopupGeometry GraphicPacksPrompt::geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    float height = popup_text_height(words(), width, output_width, output_height);
    // Room for the bar a download draws under its words.
    if (popup_ == Popup::Progress) height += TextRenderer::line_height(popup_scale(output_width, output_height)) * 1.2f;
    return popup_geometry(output_width, output_height, height, buttons());
}

void GraphicPacksPrompt::update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                                float output_width, float output_height) {
    if (popup_ == Popup::None || downloader_ == nullptr) return;
    const std::vector<PopupButton> labels = buttons();
    if (cursor_.count() != static_cast<int>(labels.size())) cursor_.reset(static_cast<int>(labels.size()), 0);
    const int pressed = cursor_.update(actions, pointers, geometry(output_width, output_height));
    if (pressed == PopupCursor::kNone) return;
    switch (popup_) {
        case Popup::Offer:
            if (pressed == 0) {
                start_install();
            } else {
                popup_ = Popup::None;
            }
            break;
        case Popup::Progress:
            // Its one button, or B: stop the download. It closes once the
            // downloader has.
            if (status_.phase == Phase::Downloading) downloader_->cancel();
            break;
        case Popup::Notice: popup_ = Popup::None; break;
        case Popup::None: break;
    }
}

void GraphicPacksPrompt::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                float output_width, float output_height) {
    (void)glyphs;
    if (popup_ == Popup::None) return;
    const PopupGeometry box = geometry(output_width, output_height);
    const std::vector<PopupButton> labels = buttons();
    draw_popup(quads, text, output_width, output_height, box, labels, cursor_.focus(),
               cursor_.shown() && !labels.empty());
    const float bottom = draw_popup_text(quads, text, words(), box.body.x, box.body.y, box.body.w, output_width, output_height);
    if (popup_ == Popup::Progress) {
        // How far the download has got, as a bar, full while installing.
        const UiTheme& theme = ui_theme();
        const float line = TextRenderer::line_height(popup_scale(output_width, output_height));
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

void GraphicPacksPrompt::describe_for_sound(UiSoundView& view) const {
    if (popup_ == Popup::None) return;
    mix_sound_state(view.focus, static_cast<uint64_t>(cursor_.focus() + 1) * 7919u);
    mix_sound_state(view.rest, static_cast<uint64_t>(popup_) + 211u);
    mix_sound_state(view.rest, message_);
    view.on_cancel = popup_ == Popup::Progress || (popup_ == Popup::Offer && cursor_.focus() == 1);
}

}  // namespace ds
