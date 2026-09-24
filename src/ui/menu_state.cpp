#include "ui/menu_state.hpp"

#include "core/system.hpp"

namespace ds {

LayoutPickerState menu_state_from_host(const net::CompanionSession& session) {
    LayoutPickerState state;
    state.session_active = true;  // there is a host at the other end of this
    state.layout_id = session.host_layout_id();
    state.bilinear_filtering = session.host_filtering();
    state.audio_target = session.host_audio_target();
    state.left_handed_layouts = session.host_left_handed_layouts();
    state.render_scale = session.host_render_scale();
    state.screen_gap = session.host_screen_gap();
    state.right_stick_mode = session.host_right_stick_mode();
    state.max_render_scale = session.host_max_render_scale();
    state.cursor_choosable = session.host_cursor_choosable();
    state.right_stick_choosable = session.host_right_stick_choosable();
    state.left_stick_press = session.host_left_stick_press();
    state.left_stick_choosable = session.host_left_stick_choosable();
    state.save_points_available = session.host_save_points_available();
    state.cursor_mode = session.host_cursor_shown() ? CursorMode::Shown : CursorMode::Hidden;
    // Properties of the console, not of the session -- the session already
    // carries `system()`, so these need nothing from the host beyond that.
    state.reset_available = system_profile(session.system()).offers_reset;
    state.smoothing_available = system_profile(session.system()).offers_smoothing;
    return state;
}

}  // namespace ds
