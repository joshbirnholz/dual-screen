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
    state.right_stick_modes = right_stick_modes(system_profile(session.system()).has_analog_stick);
    // A Wii U's buttons are Cemu's, player by player.
    state.button_mapping_available = session.system() != SystemId::WiiU;
    state.rumble_pak = session.host_rumble_pak();
    state.rumble_choosable = session.host_rumble_choosable();
    state.lcd_filter = session.host_lcd_filter();
    state.lcd_filter_choosable = session.host_lcd_filter_choosable();
    state.custom_textures = session.host_custom_textures();
    state.custom_textures_choosable = session.host_custom_textures_choosable();
    state.save_points_available = session.host_save_points_available();
    state.save_sync_available = session.host_save_sync_available();
    state.resume_from_save_state = session.host_resume_from_save_state();
    state.resume_choosable = session.host_resume_choosable();
    state.cursor_mode = session.host_cursor_shown() ? CursorMode::Shown : CursorMode::Hidden;
    // Properties of the console, not of the session -- the session already
    // carries `system()`, so these need nothing from the host beyond that.
    state.reset_available = system_profile(session.system()).offers_reset;
    state.smoothing_available = system_profile(session.system()).offers_smoothing;
    state.screens_swappable = system_profile(session.system()).solo_screens == SoloScreens::One;
    state.graphic_packs_available = session.host_graphic_packs_available();
    state.amiibo_available = session.host_amiibo_available();
    state.producer_options = session.host_producer_options();
    state.producer_choices = session.host_producer_choices();
    state.producer_controls_from = session.host_producer_controls_from();
    state.wii_u_controllers = session.host_wii_u_controllers();
    state.amiibo = session.host_amiibo();
    state.amiibo_folder = session.host_amiibo_folder();
    return state;
}

void show_host_wii_u_player(WiiUPlayerScreen& screen, const net::CompanionSession& session,
                            float output_width, float output_height) {
    const net::WiiUPlayerViewMessage& view = session.host_wii_u_player_view();
    // Whose page it is, then what it shows, then where the host has its
    // cursor on it.
    if (!screen.is_open() || !(screen.slot() == view.slot)) screen.open(view.slot);
    std::vector<std::string> types;
    int current = -1;
    if (view.slot.kind == PadSlot::Kind::Player) {
        // Cemu's option for the player, as the host's menu lists it.
        const std::string label = "Player " + std::to_string(view.slot.player + 1) + " Controller";
        const auto& options = session.host_producer_options();
        const auto& choices = session.host_producer_choices();
        for (std::size_t index = 0; index < options.size() && index < choices.size(); ++index) {
            if (options[index].first != label) continue;
            types = choices[index];
            for (std::size_t at = 0; at < types.size(); ++at) {
                if (types[at] == options[index].second) current = static_cast<int>(at);
            }
        }
    }
    screen.set_controllers(session.host_wii_u_controllers(), std::move(types), current);
    screen.show_mirrored(view, output_width, output_height);
}

}  // namespace ds
