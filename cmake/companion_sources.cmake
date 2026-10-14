# The sources the companion clients are built from -- desktop, Nintendo Switch
# and Android.
#
# One list, read by the root CMakeLists.txt and by android/app/CMakeLists.txt,
# because the whole point of the companion is that it is the app's own code
# rather than a port of it. A file added to the client and forgotten in one of
# the builds is how the two ends would quietly start behaving differently.
#
# switch/Makefile carries its own copy because make cannot read this, which is
# why the desktop build compiles the same list on every build: a file missing
# from it fails there first.
#
# Sets DUALSCREEN_COMPANION_SOURCES, relative to DUALSCREEN_ROOT.
set(DUALSCREEN_COMPANION_SOURCES
    companion/source/main.cpp
    src/audio/audio_output.cpp
    src/audio/ui_sounds.cpp
    src/config/paths.cpp
    src/video/color_convert.cpp
    src/video/h264_bitstream.cpp
    src/video/videotoolbox_decoder.cpp
    src/video/mediacodec_decoder.cpp
    src/video/video_decoder.cpp
    src/video/video_encoder.cpp
    src/core/save_points.cpp
    src/core/system.cpp
    src/input/controller_mapping.cpp
    src/input/cursor_visibility.cpp
    src/input/ds_input.cpp
    src/input/dsu_client.cpp
    src/input/input_manager.cpp
    src/input/motion.cpp
    src/input/switch_motion.cpp
    src/input/motion_compare.cpp
    src/input/pad_routing.cpp
    src/layout/layout.cpp
    src/layout/layout_registry.cpp
    src/net/companion_session.cpp
    src/net/discovery.cpp
    src/net/protocol.cpp
    src/net/transport.cpp
    src/render/bezel_art.cpp
    src/render/button_glyphs.cpp
    src/render/compositor.cpp
    src/render/font.cpp
    src/render/frame_delay.cpp
    src/render/gl_loader.cpp
    src/render/gl_window.cpp
    src/render/menu_backdrop.cpp
    src/render/menu_overlay.cpp
    src/render/png_image.cpp
    src/render/quad_renderer.cpp
    src/render/system_video_layer.cpp
    src/render/text_renderer.cpp
    src/render/texture.cpp
    src/ui/connecting_screen.cpp
    src/ui/control_guide.cpp
    src/ui/controllers_screen.cpp
    src/ui/graphic_packs_screen.cpp
    src/ui/launcher_screen.cpp
    src/ui/layout_picker.cpp
    src/ui/choice_popup.cpp
    src/ui/text_entry.cpp
    src/ui/menu_state.cpp
    src/ui/notice_popup.cpp
    src/util/battery.cpp
    src/util/build_stamp.cpp
    src/util/log.cpp
)
list(TRANSFORM DUALSCREEN_COMPANION_SOURCES PREPEND "${DUALSCREEN_ROOT}/")

# And the desktop app's sources beyond the companion's, for a companion that
# also plays DS games itself -- an Android build, where a DS core rides in
# the APK. The same files switch/Makefile lists as LOCAL_PLAY_CPPFILES, less
# the Switch's forwarder installer, which only a Switch can use.
#
# Sets DUALSCREEN_LOCAL_PLAY_SOURCES, relative to DUALSCREEN_ROOT.
set(DUALSCREEN_LOCAL_PLAY_SOURCES
    src/app.cpp
    companion/source/local_play.cpp
    src/config/game_config.cpp
    src/config/settings.cpp
    src/config/melonds_tuning.cpp
    src/config/sha1.cpp
    src/core/ds_rom_info.cpp
    src/core/game_title.cpp
    src/core/save_store.cpp
    src/core/save_folder.cpp
    src/core/console_title_info.cpp
    src/core/gl_call_trace.cpp
    src/core/hw_render_target.cpp
    src/core/external_frame_link.cpp
    src/core/external_screen_source.cpp
    src/core/libretro_core.cpp
    src/core/libretro_screen_source.cpp
    src/core/core_input_sink.cpp
    src/input/retro_keymap.cpp
    src/net/host_session.cpp
    src/net/save_exchange.cpp
    src/ui/game_browser.cpp
    src/ui/message_screen.cpp
    src/ui/settings_screen.cpp
    src/ui/data_management_screen.cpp
    src/forwarder/forwarder_icon.cpp
    src/forwarder/steamgriddb.cpp
    src/ui/forwarder_screen.cpp
    src/util/child_process.cpp
    src/update/updater.cpp
    src/update/desktop_updater.cpp
    src/update/zip_extract.cpp
    src/ui/update_prompt.cpp
    companion/source/android_updater.cpp
)
list(TRANSFORM DUALSCREEN_LOCAL_PLAY_SOURCES PREPEND "${DUALSCREEN_ROOT}/")
