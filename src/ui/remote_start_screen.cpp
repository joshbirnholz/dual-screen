#include "ui/remote_start_screen.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#include "ui/page_frame.hpp"
#include "util/log.hpp"

namespace ds {

namespace {

constexpr std::size_t kCodeDigits = 6;

const std::vector<PopupButton>& ok_button() {
    static const std::vector<PopupButton> buttons{{"OK", ""}};
    return buttons;
}

PopupGeometry alert_geometry(const std::string& message, float output_width, float output_height) {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height, popup_text_height(PopupText{message, {}, 0}, width, output_height),
                          ok_button());
}

// The digits typed, and nothing else: a code read off a screen is often
// typed with a space in the middle.
std::string digits_of(const std::string& typed) {
    std::string digits;
    for (const char c : typed) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    return digits;
}

}  // namespace

RemoteStartScreen::~RemoteStartScreen() = default;

std::string RemoteStartScreen::system_label(const std::string& system) {
    if (system == "ds") return "DS";
    if (system == "3ds") return "3DS";
    if (system == "wiiu") return "Wii U";
    return system;
}

void RemoteStartScreen::open(const net::LaunchHost& host, std::string token, std::string device_name) {
    open_ = true;
    host_ = host;
    token_ = std::move(token);
    device_name_ = std::move(device_name);
    games_.clear();
    listed_ = false;
    focus_ = 0;
    scroll_ = 0.0f;
    drag_.stop();
    alert_.clear();
    code_entry_.close();
    editor_pending_ = false;
    job_.reset();
    if (token_.empty()) {
        ask_for_code();
    } else {
        list_games();
    }
}

void RemoteStartScreen::close() {
    open_ = false;
    code_entry_.close();
    editor_pending_ = false;
    // Its thread finishes on its own; what it brings back goes nowhere.
    job_.reset();
}

void RemoteStartScreen::run(std::function<Outcome()> work, std::string waiting_for) {
    phase_ = Phase::Waiting;
    waiting_for_ = std::move(waiting_for);
    auto job = std::make_shared<Job>();
    job_ = job;
    std::thread([job, work = std::move(work)] {
        job->outcome = work();
        job->done = true;
    }).detach();
}

void RemoteStartScreen::list_games() {
    const net::LaunchHost host = host_;
    const std::string token = token_;
    run(
        [host, token] {
            Outcome outcome;
            outcome.kind = Outcome::Kind::Listed;
            outcome.ok = net::list_host_games(host, token, outcome.games, outcome.unpaired, &outcome.error);
            return outcome;
        },
        "Asking " + host_.name + " which games it has...");
}

void RemoteStartScreen::ask_for_code() {
    phase_ = Phase::Code;
    if (text_editor_) {
        editor_pending_ = true;
    } else {
        code_entry_.open("Enter the code " + host_.name + " shows in Settings, under Pair a Device.", "",
                         kCodeDigits);
    }
}

void RemoteStartScreen::alert(std::string message, Phase after, bool leaves) {
    phase_ = Phase::Alert;
    alert_ = std::move(message);
    after_alert_ = after;
    alert_leaves_ = leaves;
    alert_cursor_.reset(1, 0);
}

void RemoteStartScreen::start_game(int index) {
    if (index < 0 || index >= static_cast<int>(games_.size())) return;
    const LaunchableGame game = games_[static_cast<std::size_t>(index)];
    const net::LaunchHost host = host_;
    const std::string token = token_;
    run(
        [host, token, game] {
            Outcome outcome;
            outcome.kind = Outcome::Kind::Started;
            outcome.title = game.title;
            outcome.ok = net::start_host_game(host, token, game.id, outcome.unpaired, &outcome.error);
            return outcome;
        },
        "Starting " + game.title + " on " + host_.name + "...");
}

void RemoteStartScreen::handle_outcome(const Outcome& outcome, RemoteStartResult& result) {
    // A token the computer no longer knows -- its paired devices forgotten --
    // is let go, and a code asked for in its place.
    if (outcome.unpaired) {
        token_.clear();
        result.token_changed = true;
        result.token.clear();
        ask_for_code();
        return;
    }
    switch (outcome.kind) {
        case Outcome::Kind::Paired:
            if (!outcome.ok) {
                alert(outcome.error, Phase::Code);
                return;
            }
            token_ = outcome.token;
            result.token_changed = true;
            result.token = token_;
            log_info("this device can start games on " + host_.name + " now");
            list_games();
            return;
        case Outcome::Kind::Listed:
            if (!outcome.ok) {
                alert(outcome.error, Phase::List, true);
                return;
            }
            games_ = outcome.games;
            listed_ = true;
            focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(games_.size()) - 1));
            phase_ = Phase::List;
            return;
        case Outcome::Kind::Started:
            if (!outcome.ok) {
                alert(outcome.error, Phase::List);
                return;
            }
            result.started = true;
            result.started_title = outcome.title;
            close();
            return;
    }
}

RemoteStartScreen::Geometry RemoteStartScreen::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    geometry.list = frame.content();
    geometry.row_height = menu_row_height(frame.line) + menu_row_gap(frame.line);
    return geometry;
}

float RemoteStartScreen::max_scroll(const Geometry& geometry) const {
    return std::max(0.0f, static_cast<float>(games_.size()) * geometry.row_height - geometry.list.h);
}

void RemoteStartScreen::scroll_focus_into_view(const Geometry& geometry) {
    const float top = static_cast<float>(focus_) * geometry.row_height;
    const float bottom = top + geometry.row_height;
    if (top < scroll_) {
        scroll_ = top;
    } else if (bottom > scroll_ + geometry.list.h) {
        scroll_ = bottom - geometry.list.h;
    }
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll(geometry));
}

bool RemoteStartScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width, float output_height) {
    if (!open_ || phase_ != Phase::List) return false;
    const Geometry geometry = compute_geometry(output_width, output_height);
    drag_.update(pointers, geometry.list, scroll_, max_scroll(geometry));
    return drag_.release_was_drag();
}

int RemoteStartScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (!open_ || phase_ != Phase::List) return -1;
    const Geometry geometry = compute_geometry(output_width, output_height);
    if (!geometry.list.contains(x, y)) return -1;
    const int row = static_cast<int>(std::floor((y - geometry.list.y + scroll_) / geometry.row_height));
    return row >= 0 && row < static_cast<int>(games_.size()) ? row : -1;
}

std::vector<float> RemoteStartScreen::scroll_state(float output_width, float output_height) const {
    return {scroll_in_lines(scroll_, compute_geometry(output_width, output_height).scale)};
}

void RemoteStartScreen::set_scroll_state(const std::vector<float>& state, float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 0), geometry.scale), 0.0f, max_scroll(geometry));
    drag_.stop();
}

RemoteStartResult RemoteStartScreen::update(const std::vector<UiAction>& pressed,
                                            const std::vector<PointerEvent>& touched, float output_width,
                                            float output_height) {
    RemoteStartResult result;
    if (!open_) return result;
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    take_guide_taps(guide_hits_, actions, pointers);
    const std::string typed = typed_;
    const int backspaces = backspaces_;
    typed_.clear();
    backspaces_ = 0;

    if (job_ && job_->done) {
        const Outcome outcome = job_->outcome;
        job_.reset();
        handle_outcome(outcome, result);
        // What was pressed went to the page before it answered.
        return result;
    }

    switch (phase_) {
        case Phase::Waiting:
            // B leaves; nothing else means anything until it answers.
            for (const UiAction action : actions) {
                if (action == UiAction::Cancel) {
                    result.back = true;
                    close();
                    return result;
                }
            }
            return result;

        case Phase::Alert: {
            const int answer =
                alert_cursor_.update(actions, pointers, alert_geometry(alert_, output_width, output_height));
            if (answer == PopupCursor::kNone) return result;
            alert_.clear();
            if (alert_leaves_) {
                result.back = true;
                close();
            } else if (after_alert_ == Phase::Code) {
                ask_for_code();
            } else {
                phase_ = Phase::List;
            }
            return result;
        }

        case Phase::Code: {
            std::string code;
            bool answered = false;
            if (editor_pending_) {
                editor_pending_ = false;
                std::string out;
                answered = text_editor_("Code from " + host_.name, "", kCodeDigits, out);
                code = out;
                if (!answered) code.clear();
                if (!answered) {
                    // Put away: back to the list when there is one, else out.
                    if (listed_) {
                        phase_ = Phase::List;
                    } else {
                        result.back = true;
                        close();
                    }
                    return result;
                }
            } else {
                const TextEntryResult entered =
                    code_entry_.update(actions, pointers, typed, backspaces, output_width, output_height);
                if (entered.cancelled) {
                    code_entry_.close();
                    if (listed_) {
                        phase_ = Phase::List;
                    } else {
                        result.back = true;
                        close();
                    }
                    return result;
                }
                if (!entered.done) return result;
                code = code_entry_.text();
                code_entry_.close();
            }
            code = digits_of(code);
            if (code.size() != kCodeDigits) {
                alert("A code is six numbers. Enter the one " + host_.name + " shows in Settings, under Pair a Device.",
                      Phase::Code);
                return result;
            }
            const net::LaunchHost host = host_;
            const std::string device = device_name_;
            run(
                [host, code, device] {
                    Outcome outcome;
                    outcome.kind = Outcome::Kind::Paired;
                    outcome.ok = net::pair_with_host(host, code, device, outcome.token, &outcome.error);
                    return outcome;
                },
                "Pairing with " + host_.name + "...");
            return result;
        }

        case Phase::List:
            break;
    }

    const Geometry geometry = compute_geometry(output_width, output_height);
    const int count = static_cast<int>(games_.size());
    const int visible = std::max(1, static_cast<int>(geometry.list.h / geometry.row_height));
    bool acted = false;
    for (const UiAction action : actions) {
        if (acted) break;
        switch (action) {
            case UiAction::Up:
                if (count > 0) focus_ = std::max(0, focus_ - 1);
                break;
            case UiAction::Down:
                if (count > 0) focus_ = std::min(count - 1, focus_ + 1);
                break;
            case UiAction::Left:
                focus_ = std::max(0, focus_ - visible);
                break;
            case UiAction::Right:
                focus_ = std::max(0, std::min(count - 1, focus_ + visible));
                break;
            case UiAction::Confirm:
                if (count > 0) {
                    start_game(focus_);
                    acted = true;
                }
                break;
            case UiAction::Cancel:
                result.back = true;
                close();
                return result;
            default:
                break;
        }
    }
    focus_ = std::clamp(focus_, 0, std::max(0, count - 1));
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(geometry);
    }
    for (const int entry : take_taps(pointers, output_width, output_height)) {
        if (acted) break;
        if (entry >= 0 && entry < count && tap_chooses(focus_, entry)) {
            focus_ = entry;
            start_game(entry);
            acted = true;
        }
    }
    drag_.settle();
    return result;
}

void RemoteStartScreen::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                               float output_width, float output_height) {
    if (!open_) return;
    const ShownScroll shown(scroll_, scroll_ease_, drag_.dragging());
    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);
    draw_page_header(quads, text, frame, "Start a Game", host_.name);

    if (phase_ == Phase::Waiting) {
        draw_page_status(quads, text, glyphs, frame, waiting_for_, theme.text_dim);
    }
    if (listed_ && games_.empty()) {
        text.draw_wrapped(quads,
                          "No games found on " + host_.name +
                              ". Add a game to Steam from Dualscreen there, or open one there once so "
                              "its folder is known.",
                          geometry.list.x, geometry.list.y, geometry.list.w, scale, theme.text_dim);
    }

    const int count = static_cast<int>(games_.size());
    quads.set_clip(menu_list_clip(geometry.list, scale));
    const int first = std::max(0, static_cast<int>(std::floor(scroll_ / geometry.row_height)));
    const int last =
        std::min(count, static_cast<int>(std::ceil((scroll_ + geometry.list.h) / geometry.row_height)));
    const bool cursor = phase_ == Phase::List;
    for (int index = first; index < last; ++index) {
        const LaunchableGame& game = games_[static_cast<std::size_t>(index)];
        const PixelRect rect{geometry.list.x, geometry.list.y + index * geometry.row_height - scroll_,
                             geometry.list.w - scroll_bar_width(scale) - line * 0.4f,
                             geometry.row_height - menu_row_gap(line)};
        const bool focused = cursor && index == focus_;
        draw_menu_row(quads, rect, focused, scale, index == count - 1, cursor && index == focus_ + 1);
        const float pad = menu_row_pad(line);
        const float baseline = page_row_text_y(rect, scale);
        const std::string system = system_label(game.system);
        const float system_width = TextRenderer::measure_width(system, scale);
        text.draw(quads, TextRenderer::elide_right(game.title, rect.w - pad * 3.0f - system_width, scale),
                  rect.x + pad, baseline, scale, menu_row_text(focused));
        text.draw(quads, system, rect.x + rect.w - pad, baseline, scale, theme.text_dim, TextAlign::Right);
    }
    quads.clear_clip();
    const float content = static_cast<float>(count) * geometry.row_height;
    if (content > geometry.list.h + 0.5f) {
        draw_scroll_bar(quads, geometry.list, scroll_ / content, geometry.list.h / content, line, scale);
    }

    std::vector<GuidePrompt> prompts;
    if (phase_ == Phase::List && count > 0) prompts.push_back({ButtonGlyph::A, "Start", PromptRole::ActsOnSelection});
    prompts.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    draw_page_guide(quads, text, glyphs, frame, prompts, &guide_hits_);

    if (phase_ == Phase::Code && code_entry_.is_open()) {
        code_entry_.render(quads, text, output_width, output_height);
    } else if (phase_ == Phase::Alert) {
        const PopupGeometry popup = alert_geometry(alert_, output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, ok_button(), alert_cursor_.focus(),
                   alert_cursor_.shown());
        draw_popup_text(quads, text, PopupText{alert_, {}, 0}, popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

void RemoteStartScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(phase_));
    mix_sound_state(view.rest, games_.size());
}

}  // namespace ds
