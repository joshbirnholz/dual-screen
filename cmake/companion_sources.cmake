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
    src/video/video_decoder.cpp
    src/video/video_encoder.cpp
    src/core/system.cpp
    src/input/controller_mapping.cpp
    src/input/cursor_visibility.cpp
    src/input/ds_input.cpp
    src/input/dsu_client.cpp
    src/input/input_manager.cpp
    src/input/motion.cpp
    src/input/switch_motion.cpp
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
    src/render/gl_loader.cpp
    src/render/gl_window.cpp
    src/render/menu_backdrop.cpp
    src/render/png_image.cpp
    src/render/quad_renderer.cpp
    src/render/text_renderer.cpp
    src/render/texture.cpp
    src/ui/connecting_screen.cpp
    src/ui/control_guide.cpp
    src/ui/launcher_screen.cpp
    src/ui/layout_picker.cpp
    src/util/log.cpp
)
list(TRANSFORM DUALSCREEN_COMPANION_SOURCES PREPEND "${DUALSCREEN_ROOT}/")
