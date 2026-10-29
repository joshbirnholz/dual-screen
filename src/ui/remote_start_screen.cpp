#include "ui/remote_start_screen.hpp"

#include <thread>

#include "ui/page_frame.hpp"
#include "util/log.hpp"

namespace ds {

namespace {

const std::vector<PopupButton>& ok_button() {
    static const std::vector<PopupButton> buttons{{"OK", ""}};
    return buttons;
}

PopupGeometry alert_geometry(const std::string& message, float output_width, float output_height) {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height, popup_text_height(PopupText{message, {}, 0}, width, output_height),
                          ok_button());
}

}  // namespace

RemoteStartScreen::~RemoteStartScreen() = default;

std::string RemoteStartScreen::system_label(const std::string& system) {
    if (system == "ds") return "DS";
    if (system == "3ds") return "3DS";
    if (system == "wiiu") return "Wii U";
    return system;
}

void RemoteStartScreen::open_and_start(const net::LaunchHost& host, std::string token, const LaunchableGame& game) {
    open_ = true;
    host_ = host;
    token_ = std::move(token);
    game_ = game;
    alert_.clear();
    drop_job();
    if (host_.asleep) {
        wake();
    } else {
        start();
    }
}

void RemoteStartScreen::close() {
    open_ = false;
    alert_.clear();
    // Its thread finishes on its own; what it brings back goes nowhere.
    drop_job();
}

void RemoteStartScreen::run(std::function<Outcome(const std::atomic<bool>& cancelled)> work, std::string waiting_for) {
    drop_job();
    waiting_for_ = std::move(waiting_for);
    auto job = std::make_shared<Job>();
    job_ = job;
    std::thread([job, work = std::move(work)] {
        job->outcome = work(job->cancelled);
        job->done = true;
    }).detach();
}

void RemoteStartScreen::drop_job() {
    if (job_) job_->cancelled = true;
    job_.reset();
}

void RemoteStartScreen::wake() {
    const net::LaunchHost host = host_;
    run(
        [host](const std::atomic<bool>& cancelled) {
            Outcome outcome;
            outcome.kind = Outcome::Kind::Woken;
            outcome.ok = net::wake_and_wait(host, 90000, cancelled, outcome.woken, &outcome.error);
            return outcome;
        },
        "Waking " + host_.name + "...");
}

void RemoteStartScreen::start() {
    const net::LaunchHost host = host_;
    const std::string token = token_;
    const LaunchableGame game = game_;
    run(
        [host, token, game](const std::atomic<bool>&) {
            Outcome outcome;
            outcome.kind = Outcome::Kind::Started;
            outcome.ok = net::start_host_game(host, token, game.id, outcome.unpaired, &outcome.error);
            return outcome;
        },
        "Starting " + game_.title + " on " + host_.name + "...");
}

void RemoteStartScreen::alert(std::string message) {
    alert_ = std::move(message);
    alert_cursor_.reset(1, 0);
}

void RemoteStartScreen::handle_outcome(const Outcome& outcome, RemoteStartResult& result) {
    // A token the computer no longer knows -- Remote Start turned off and on
    // again there -- is let go: joining a game there gives a new one.
    if (outcome.unpaired) {
        result.token_refused = true;
        alert(host_.name + " will not start games for this device any more. Join a game on " + host_.name +
              " from here once to allow it again.");
        return;
    }
    switch (outcome.kind) {
        case Outcome::Kind::Woken:
            if (!outcome.ok) {
                alert(outcome.error);
                return;
            }
            host_ = outcome.woken;
            result.answered = true;
            log_info(host_.name + " is awake");
            start();
            return;
        case Outcome::Kind::Started:
            if (!outcome.ok) {
                alert(outcome.error);
                return;
            }
            result.answered = true;
            result.started = true;
            result.started_title = game_.title;
            close();
            return;
    }
}

RemoteStartResult RemoteStartScreen::update(const std::vector<UiAction>& pressed,
                                            const std::vector<PointerEvent>& touched, float output_width,
                                            float output_height) {
    RemoteStartResult result;
    if (!open_) return result;
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    take_guide_taps(guide_hits_, actions, pointers);
    // Nothing on the page itself to tap; asked so the other device's taps
    // are let go of too.
    take_taps(pointers, output_width, output_height);

    if (job_ && job_->done) {
        const Outcome outcome = job_->outcome;
        job_.reset();
        handle_outcome(outcome, result);
        // What was pressed went to the page before it answered.
        return result;
    }

    if (!alert_.empty()) {
        const int answer =
            alert_cursor_.update(actions, pointers, alert_geometry(alert_, output_width, output_height));
        if (answer == PopupCursor::kNone) return result;
        result.back = true;
        close();
        return result;
    }
    // B leaves; nothing else means anything until it answers.
    for (const UiAction action : actions) {
        if (action == UiAction::Cancel) {
            result.back = true;
            close();
            return result;
        }
    }
    return result;
}

void RemoteStartScreen::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                               float output_width, float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const PageFrame frame = page_frame(output_width, output_height);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);
    draw_page_header(quads, text, frame, game_.title, host_.name);
    if (alert_.empty()) draw_page_status(quads, text, glyphs, frame, waiting_for_, theme.text_dim);

    std::vector<GuidePrompt> prompts;
    prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    draw_page_guide(quads, text, glyphs, frame, prompts, &guide_hits_);

    if (!alert_.empty()) {
        const PopupGeometry popup = alert_geometry(alert_, output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, ok_button(), alert_cursor_.focus(),
                   alert_cursor_.shown());
    }
}

void RemoteStartScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, alert_.empty() ? 0u : 1u);
    mix_sound_state(view.focus, static_cast<uint64_t>(alert_cursor_.focus() + 1));
}

}  // namespace ds
