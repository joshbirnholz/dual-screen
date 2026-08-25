#include "session_menu.hpp"

#include <algorithm>

namespace ds::sw {

void SessionMenu::open(const SessionMenuState& state) {
    open_ = true;
    focus_ = 0;
    set_state(state);
}

void SessionMenu::set_state(const SessionMenuState& state) {
    state_ = state;
    rebuild();
}

void SessionMenu::rebuild() {
    layout_rows_.clear();
    layout_rows_.push_back(Row::Layout);
    layout_rows_.push_back(Row::Smoothing);
    // Absent where there is nothing to choose: a console whose core draws at
    // one fixed resolution, and one with no stick of its own to have an
    // opinion about. Same rule as the desktop menu applies.
    if (state_.max_render_scale > 1) {
        layout_rows_.push_back(Row::RenderScale);
    }
    if (state_.right_stick_choosable) {
        layout_rows_.push_back(Row::RightStick);
    }
    layout_rows_.push_back(Row::Hands);
    if (state_.screen_gap_toggleable) {
        layout_rows_.push_back(Row::ScreenGap);
    }
    // Two devices are always connected while this menu can be open at all, so
    // which of them makes the sound is always a real question here.
    layout_rows_.push_back(Row::Sound);
    layout_rows_.push_back(Row::Reset);
    layout_rows_.push_back(Row::Resume);
    layout_rows_.push_back(Row::Disconnect);
    layout_rows_.push_back(Row::Quit);

    rows_.clear();
    rows_.reserve(layout_rows_.size());
    for (const Row row : layout_rows_) {
        rows_.push_back(label_for(row));
    }
    // A row the state just took away can have been the one in focus.
    focus_ = std::clamp(focus_, 0, static_cast<int>(layout_rows_.size()) - 1);
}

std::string SessionMenu::label_for(Row row) const {
    switch (row) {
        case Row::Layout:
            return "Layout: " + registry_->resolve(state_.layout_id).display_name;
        case Row::Smoothing:
            return state_.filtering ? "Smoothing: On" : "Smoothing: Off";
        case Row::RenderScale:
            return "Render: " + std::to_string(state_.render_scale) + "x";
        case Row::RightStick:
            return std::string("Right Stick: ") + to_string(state_.right_stick_mode);
        case Row::Hands:
            return state_.handedness == Handedness::Left ? "Left-Handed" : "Right-Handed";
        case Row::ScreenGap:
            return state_.screen_gap ? "Gap: On" : "Gap: Off";
        case Row::Sound:
            switch (state_.audio_target) {
                case AudioTarget::TopScreen: return "Sound: Top";
                case AudioTarget::BottomScreen: return "Sound: Bottom";
                case AudioTarget::Both: break;
            }
            return "Sound: Both";
        case Row::Reset: return "Reset Game";
        case Row::Resume: return "Resume";
        case Row::Disconnect: return "Disconnect";
        case Row::Quit: break;
    }
    return "Quit";
}

void SessionMenu::activate(int step, SessionMenuResult& result) {
    if (focus_ < 0 || focus_ >= static_cast<int>(layout_rows_.size())) {
        return;
    }
    // Confirm means "the next one", the same as pressing right.
    const int direction = (step == 0) ? 1 : step;

    switch (layout_rows_[static_cast<std::size_t>(focus_)]) {
        case Row::Layout: {
            const int count = static_cast<int>(registry_->size());
            int index = registry_->index_of(registry_->resolve(state_.layout_id).id);
            if (index < 0) {
                index = 0;
            }
            index = ((index + direction) % count + count) % count;
            result.wants_layout = true;
            result.layout_id = registry_->at(static_cast<std::size_t>(index)).id;
            break;
        }
        case Row::Smoothing:
            result.wants_filtering = true;
            result.filtering = !state_.filtering;
            break;
        case Row::RenderScale: {
            const int maximum = std::max(1, state_.max_render_scale);
            int scale = state_.render_scale + direction;
            if (scale > maximum) scale = 1;
            if (scale < 1) scale = maximum;
            result.wants_render_scale = true;
            result.render_scale = scale;
            break;
        }
        case Row::RightStick: {
            RightStickMode mode = state_.right_stick_mode;
            // next_right_stick_mode() only goes one way, which is all the
            // desktop's single button needs; going back is three of those.
            const int steps = (direction > 0) ? 1 : 3;
            for (int i = 0; i < steps; ++i) {
                mode = next_right_stick_mode(mode);
            }
            result.wants_right_stick_mode = true;
            result.right_stick_mode = mode;
            break;
        }
        case Row::Hands:
            result.wants_handedness = true;
            result.handedness = other_hand(state_.handedness);
            break;
        case Row::ScreenGap:
            result.wants_screen_gap = true;
            result.screen_gap = !state_.screen_gap;
            break;
        case Row::Sound: {
            AudioTarget target = state_.audio_target;
            const int steps = (direction > 0) ? 1 : 2;
            for (int i = 0; i < steps; ++i) {
                target = next_audio_target(target);
            }
            result.wants_audio_target = true;
            result.audio_target = target;
            break;
        }
        // The four that do something rather than change something. Only a
        // confirm counts: cycling left and right along a list should not
        // quit the game on the way past.
        case Row::Reset:
            if (step == 0) result.reset = true;
            break;
        case Row::Resume:
            if (step == 0) result.resume = true;
            break;
        case Row::Disconnect:
            if (step == 0) result.disconnect = true;
            break;
        case Row::Quit:
            if (step == 0) result.quit = true;
            break;
    }
}

SessionMenuResult SessionMenu::update(const std::vector<UiAction>& actions) {
    SessionMenuResult result;
    if (!open_ || layout_rows_.empty()) {
        return result;
    }
    const int count = static_cast<int>(layout_rows_.size());
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                focus_ = (focus_ + count - 1) % count;
                break;
            case UiAction::Down:
                focus_ = (focus_ + 1) % count;
                break;
            case UiAction::Left:
                activate(-1, result);
                break;
            case UiAction::Right:
                activate(1, result);
                break;
            case UiAction::Confirm:
                activate(0, result);
                break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu:
                result.resume = true;
                break;
            default:
                break;
        }
    }
    return result;
}

}  // namespace ds::sw
