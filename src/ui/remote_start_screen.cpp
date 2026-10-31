#include "ui/remote_start_screen.hpp"

#include <cmath>
#include <thread>

#include "ui/page_frame.hpp"
#include "util/log.hpp"

namespace ds {

namespace {

const std::vector<PopupButton>& ok_button() {
    static const std::vector<PopupButton> buttons{{"OK", ""}};
    return buttons;
}

const std::vector<PopupButton>& close_buttons() {
    static const std::vector<PopupButton> buttons{{"Cancel", ""}, {"Close It", ""}};
    return buttons;
}

PopupGeometry alert_geometry(const std::string& message, const std::vector<PopupButton>& buttons,
                             float output_width, float output_height) {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height, popup_text_height(PopupText{message, {}, 0}, width, output_height),
                          buttons);
}

}  // namespace

RemoteStartScreen::~RemoteStartScreen() = default;

std::string RemoteStartScreen::system_label(const std::string& system) {
    if (system == "ds") return "DS";
    if (system == "3ds") return "3DS";
    if (system == "wiiu") return "Wii U";
    return system;
}

void RemoteStartScreen::open_and_start(const net::LaunchHost& host, std::string token, const LaunchableGame& game,
                                       int icon_width, int icon_height, std::vector<uint8_t> icon_rgba) {
    open_ = true;
    host_ = host;
    token_ = std::move(token);
    game_ = game;
    alert_.clear();
    icon_width_ = icon_width;
    icon_height_ = icon_height;
    icon_rgba_ = std::move(icon_rgba);
    icon_ = Texture();
    opened_at_ = std::chrono::steady_clock::now();
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
    asking_close_ = false;
    // Its thread finishes on its own; what it brings back goes nowhere.
    drop_job();
}

void RemoteStartScreen::connecting() { step_ = Step::Connecting; }

void RemoteStartScreen::awaiting() { step_ = Step::Awaiting; }

void RemoteStartScreen::fail(std::string message) {
    drop_job();
    alert(std::move(message));
}

std::string RemoteStartScreen::status() const {
    switch (step_) {
        case Step::Waking: return "Waking " + host_.name + "...";
        case Step::Starting: return "Asking " + host_.name + " to start it...";
        case Step::Closing: return "Closing " + running_title_ + " on " + host_.name + "...";
        case Step::Awaiting: return "Waiting for it to start on " + host_.name + "...";
        case Step::Connecting: return "Connecting...";
    }
    return std::string();
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
    step_ = Step::Waking;
    run(
        [host](const std::atomic<bool>& cancelled) {
            Outcome outcome;
            outcome.kind = Outcome::Kind::Woken;
            outcome.ok = net::wake_and_wait(host, 90000, cancelled, outcome.woken, &outcome.error);
            return outcome;
        },
        "Waking " + host_.name + "...");
}

void RemoteStartScreen::start(bool replace) {
    const net::LaunchHost host = host_;
    const std::string token = token_;
    const LaunchableGame game = game_;
    step_ = replace ? Step::Closing : Step::Starting;
    run(
        [host, token, game, replace](const std::atomic<bool>&) {
            Outcome outcome;
            outcome.kind = Outcome::Kind::Started;
            net::StartAnswer answer;
            outcome.ok = net::start_host_game(host, token, game.id, replace, answer, &outcome.error);
            outcome.unpaired = answer.unpaired;
            outcome.already = answer.already;
            outcome.running = answer.running;
            return outcome;
        },
        "Starting " + game_.title + " on " + host_.name + "...");
}

void RemoteStartScreen::alert(std::string message) {
    alert_ = std::move(message);
    asking_close_ = false;
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
            if (!outcome.ok && !outcome.running.empty()) {
                // Another game is up there: asked whether to close it.
                running_title_ = outcome.running;
                alert_ = outcome.running + " is running on " + host_.name + ". It will be closed, and " +
                         game_.title + " started in its place.";
                asking_close_ = true;
                alert_cursor_.reset(2, 1);
                return;
            }
            if (!outcome.ok) {
                alert(outcome.error);
                return;
            }
            // That very game is up there already: joined as it is.
            if (outcome.already) log_info(game_.title + " is running on " + host_.name + " already; joining it");
            result.answered = true;
            result.started = true;
            result.started_title = game_.title;
            step_ = Step::Awaiting;
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
        const std::vector<PopupButton>& buttons = asking_close_ ? close_buttons() : ok_button();
        const int answer =
            alert_cursor_.update(actions, pointers, alert_geometry(alert_, buttons, output_width, output_height));
        if (answer == PopupCursor::kNone) return result;
        if (asking_close_ && answer == 1) {
            alert_.clear();
            asking_close_ = false;
            start(true);
            return result;
        }
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
    const float scale = frame.scale;
    const float line = frame.line;

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    // The game's icon, its name and what is happening, with a spinner under
    // them, all in the middle of the page.
    const float icon_side = std::round(std::min(line * 8.0f, output_height * 0.3f));
    const float title_scale = scale * 1.3f;
    const float title_height = TextRenderer::line_height(title_scale);
    const float status_height = line;
    const float spinner = std::round(line * 1.6f);
    const float gap = std::round(line * 1.2f);
    const float block = icon_side + gap + title_height + line * 0.6f + status_height + gap + spinner;
    float y = std::round((output_height - block) * 0.5f);
    const float centre_x = std::round(output_width * 0.5f);

    const PixelRect tile{centre_x - icon_side * 0.5f, y, icon_side, icon_side};
    const float radius = std::round(icon_side * 0.1f);
    quads.draw_rounded_rect(tile, radius, Color{0.17f, 0.19f, 0.24f, 1.0f});
    if (!icon_.valid() && icon_width_ > 0 && icon_height_ > 0 &&
        icon_rgba_.size() == static_cast<std::size_t>(icon_width_) * static_cast<std::size_t>(icon_height_) * 4u) {
        icon_.upload_rgba8888(reinterpret_cast<const uint32_t*>(icon_rgba_.data()), icon_width_, icon_height_,
                              icon_width_);
        icon_.set_linear_filtering(true);
    }
    if (icon_.valid()) {
        const float inset = std::round(icon_side * 0.09f);
        quads.draw_image(PixelRect{tile.x + inset, tile.y + inset, tile.w - inset * 2.0f, tile.h - inset * 2.0f},
                         icon_, Color{1.0f, 1.0f, 1.0f, 1.0f});
    }
    y += icon_side + gap;
    const std::string title =
        TextRenderer::elide_right("Starting " + game_.title + "...", frame.panel.w, title_scale);
    text.draw(quads, title, centre_x, y, title_scale, theme.text, TextAlign::Center);
    y += title_height + line * 0.6f;
    if (alert_.empty()) {
        text.draw(quads, TextRenderer::elide_right(status(), frame.panel.w, scale), centre_x, y, scale,
                  theme.text_dim, TextAlign::Center);
        y += status_height + gap;
        // Eight dots round a circle, the brightest going round once a
        // second.
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - opened_at_).count();
        const float lead = seconds * 8.0f;
        const float dot = std::max(3.0f, std::round(spinner * 0.2f));
        const float round_radius = (spinner - dot) * 0.5f;
        for (int index = 0; index < 8; ++index) {
            const float angle = static_cast<float>(index) * 3.14159265f / 4.0f;
            float behind = std::fmod(lead - static_cast<float>(index), 8.0f);
            if (behind < 0.0f) behind += 8.0f;
            const float alpha = 0.2f + 0.8f * (1.0f - behind / 8.0f);
            const float x = centre_x + std::sin(angle) * round_radius - dot * 0.5f;
            const float dot_y = y + spinner * 0.5f - std::cos(angle) * round_radius - dot * 0.5f;
            quads.draw_rounded_rect(PixelRect{std::round(x), std::round(dot_y), dot, dot}, dot * 0.5f,
                                    Color{theme.text.r, theme.text.g, theme.text.b, theme.text.a * alpha});
        }
    }

    std::vector<GuidePrompt> prompts;
    prompts.push_back({ButtonGlyph::B, "Cancel", PromptRole::LeavesScreen});
    draw_page_guide(quads, text, glyphs, frame, prompts, &guide_hits_);

    if (!alert_.empty()) {
        const std::vector<PopupButton>& buttons = asking_close_ ? close_buttons() : ok_button();
        const PopupGeometry popup = alert_geometry(alert_, buttons, output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, buttons, alert_cursor_.focus(),
                   alert_cursor_.shown());
        draw_popup_text(quads, text, PopupText{alert_, {}, 0}, popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

void RemoteStartScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, alert_.empty() ? 0u : 1u);
    mix_sound_state(view.rest, static_cast<uint64_t>(step_));
    mix_sound_state(view.focus, static_cast<uint64_t>(alert_cursor_.focus() + 1));
}

}  // namespace ds
