cmake_minimum_required(VERSION 3.16)
project(dualscreen LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(DUALSCREEN_BUILD_TESTS "Build unit tests and the mock libretro core" ON)
option(DUALSCREEN_WERROR "Treat warnings as errors" OFF)
# Statically links libstdc++ and libgcc. On by default because the normal way
# to get this onto a Deck is to build it in a container and copy the binary to
# a host with a different GCC, and the C++ ABI is the coupling most likely to
# break that. Leaves the binary needing only SDL2, libc and libm.
option(DUALSCREEN_PORTABLE "Statically link the C++ runtime" ON)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# The compressed video path, for a screen the tile codec cannot carry -- see
# docs/research-notes.md. Optional on purpose: everything DS and 3DS do works
# without it, and the Switch and Android toolchains do not have FFmpeg wired up
# yet, so requiring it here would break those builds before they can use it.
#
# A shipping build should have h264_vaapi and no libx264: FFmpeg is LGPL until
# it is built with GPL pieces, and that is the piece that would do it.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    # Asked afresh on every configure. pkg_check_modules keeps its answer in
    # the cache and does not ask again, so once Homebrew (or any package
    # manager) upgrades FFmpeg the cache still names the old version's
    # directories -- deleted by the upgrade -- and the build fails on paths
    # that no longer exist. pkg-config is quick; a stale answer is not.
    unset(__pkg_config_checked_FFMPEG CACHE)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale)
endif()
# zstd, for reading a Wii U game's name and icon out of Cemu's .wua archives
# in Data Management. Optional: without it those games are named from Cemu's
# game list, with no icon.
if(PkgConfig_FOUND)
    unset(__pkg_config_checked_ZSTD CACHE)
    pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
endif()
if(FFMPEG_FOUND)
    message(STATUS "video codec: libavcodec ${FFMPEG_libavcodec_VERSION}")
else()
    message(STATUS "video codec: not available, building without one")
endif()

# nlohmann::json — prefer a system/CMake package, fall back to the plain header,
# and only then reach for the network.
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
    # The copy in third_party is the one the Switch build uses, which has no
    # package to find it in; it is the same release as the download below.
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
              PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    if(NLOHMANN_JSON_INCLUDE_DIR)
        add_library(nlohmann_json INTERFACE)
        target_include_directories(nlohmann_json INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    else()
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

# The console artwork the Standard layout draws, turned into source files so it
# travels inside the binary. Regenerated whenever an image changes.
set(BEZEL_SOURCES "")
function(dualscreen_embed_asset image symbol out_variable)
    set(asset "${CMAKE_CURRENT_SOURCE_DIR}/assets/${image}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${symbol}.cpp")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${asset}
                -DOUTPUT=${generated}
                -DSYMBOL=${symbol}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_asset.cmake
        DEPENDS "${asset}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_asset.cmake"
        COMMENT "Embedding assets/${image}"
        VERBATIM)
    set(${out_variable} "${${out_variable}};${generated}" PARENT_SCOPE)
endfunction()

dualscreen_embed_asset(ds_bezel.png kDsBezelPng BEZEL_SOURCES)
dualscreen_embed_asset(three_ds_bezel.png kThreeDsBezelPng BEZEL_SOURCES)
dualscreen_embed_asset(inter.ttf kUiFont BEZEL_SOURCES)
dualscreen_embed_asset(stylus_hand.png kStylusHandPng BEZEL_SOURCES)
dualscreen_embed_asset(button_icons.png kButtonIconsPng BEZEL_SOURCES)
# Data Management's icons: only the app that plays games draws them.
dualscreen_embed_asset(ui_icons.png kUiIconsPng BEZEL_SOURCES)
# The menus' sounds: see assets/make_ui_sounds.py.
dualscreen_embed_asset(ui_sounds.bin kUiSounds BEZEL_SOURCES)

set(DUALSCREEN_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_stamp.cmake)
dualscreen_stamp_build("${CMAKE_CURRENT_SOURCE_DIR}/src/util/build_stamp.cpp")

add_library(dualscreen_core STATIC
    ${BEZEL_SOURCES}
    src/app.cpp
    src/audio/audio_output.cpp
    src/audio/ui_sounds.cpp
    src/video/video_decoder.cpp
    src/video/color_convert.cpp
    src/video/h264_bitstream.cpp
    src/video/videotoolbox_decoder.cpp
    src/video/mediacodec_decoder.cpp
    src/video/video_encoder.cpp
    src/config/game_config.cpp
    src/config/paths.cpp
    src/config/settings.cpp
    src/config/melonds_tuning.cpp
    src/config/sha1.cpp
    src/core/ds_rom_info.cpp
    src/forwarder/forwarder_icon.cpp
    src/forwarder/steamgriddb.cpp
    src/core/game_title.cpp
    src/core/gl_call_trace.cpp
    src/core/hw_render_target.cpp
    src/core/external_frame_link.cpp
    src/core/external_screen_source.cpp
    src/core/libretro_core.cpp
    src/core/libretro_screen_source.cpp
    src/core/save_points.cpp
    src/core/save_store.cpp
    src/core/save_folder.cpp
    src/core/console_title_info.cpp
    src/core/core_input_sink.cpp
    src/core/system.cpp
    src/input/controller_mapping.cpp
    src/input/cursor_visibility.cpp
    src/input/ds_input.cpp
    src/input/pad_routing.cpp
    src/input/dsu_client.cpp
    src/input/input_manager.cpp
    src/input/motion.cpp
    src/input/switch_motion.cpp
    src/input/motion_compare.cpp
    src/input/retro_keymap.cpp
    src/layout/layout.cpp
    src/net/companion_session.cpp
    src/net/discovery.cpp
    src/net/host_session.cpp
    src/net/protocol.cpp
    src/net/save_exchange.cpp
    src/net/transport.cpp
    src/layout/layout_registry.cpp
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
    src/ui/game_browser.cpp
    src/ui/launcher_screen.cpp
    src/ui/layout_picker.cpp
    src/ui/menu_state.cpp
    src/ui/notice_popup.cpp
    src/ui/message_screen.cpp
    src/ui/settings_screen.cpp
    src/ui/controllers_screen.cpp
    src/ui/graphic_packs_screen.cpp
    src/ui/choice_popup.cpp
    src/ui/text_entry.cpp
    src/ui/game_dialog.cpp
    src/ui/mirrored_game_dialog.cpp
    src/ui/data_management_screen.cpp
    src/ui/forwarder_screen.cpp
    src/util/battery.cpp
    src/util/build_stamp.cpp
    src/util/child_process.cpp
    src/update/updater.cpp
    src/update/desktop_updater.cpp
    src/update/zip_extract.cpp
    src/ui/update_prompt.cpp
    src/util/log.cpp
)

target_include_directories(dualscreen_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(dualscreen_core
    PUBLIC
        SDL2::SDL2
        OpenGL::GL
        nlohmann_json::nlohmann_json
        ZLIB::ZLIB
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

# A save state's time is written the way the Mac's own region and 24-Hour Time
# setting say -- see device_time_style().
if(APPLE)
    target_link_libraries(dualscreen_core PUBLIC "-framework CoreFoundation")
endif()

# PUBLIC so that anything linking the core -- the companion client included --
# compiles the same headers against the same answer about whether there is a
# codec at all.
if(FFMPEG_FOUND)
    target_link_libraries(dualscreen_core PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(dualscreen_core PUBLIC DS_HAVE_VIDEO_CODEC=1)
else()
    target_compile_definitions(dualscreen_core PUBLIC DS_HAVE_VIDEO_CODEC=0)
endif()
if(ZSTD_FOUND)
    message(STATUS "Wii U archives (.wua): zstd ${ZSTD_VERSION}")
    target_link_libraries(dualscreen_core PRIVATE PkgConfig::ZSTD)
    target_compile_definitions(dualscreen_core PRIVATE DUALSCREEN_HAVE_ZSTD=1)
else()
    message(STATUS "Wii U archives (.wua): no zstd, so named from Cemu's game list only")
endif()

if(DUALSCREEN_WERROR)
    target_compile_options(dualscreen_core PRIVATE -Wall -Wextra -Werror)
else()
    target_compile_options(dualscreen_core PRIVATE -Wall -Wextra)
endif()

add_executable(dualscreen src/main.cpp)
target_link_libraries(dualscreen PRIVATE dualscreen_core)

if(DUALSCREEN_PORTABLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(dualscreen PRIVATE -static-libstdc++ -static-libgcc)
endif()

# FFmpeg's own dev packages ship no static archives on Debian, unlike
# libstdc++ above, so PkgConfig::FFMPEG is necessarily a shared-library
# dependency -- one a container-built binary's host will not have, and a
# different problem from the glibc-floor one DUALSCREEN_PORTABLE exists for.
# $ORIGIN in the rpath is the other half of that: tools/build-steamos.sh
# copies the exact .so files this binary resolved to sit right next to it,
# and this is what makes the dynamic linker look there before giving up --
# see that script's bundle_ffmpeg_libs(). A path that turns out not to exist
# (there is nothing to copy when FFmpeg was not found at all) is simply
# skipped, not an error.
if(FFMPEG_FOUND)
    set_target_properties(dualscreen PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

install(TARGETS dualscreen RUNTIME DESTINATION bin)

# The companion clients: the same program for a desktop, a Nintendo Switch and
# an Android device. It runs no core and holds no ROM -- it joins a game
# already running elsewhere and becomes one of its screens -- and shares
# everything that describes the session with the app: the wire protocol,
# discovery, the layout arithmetic, the controller mapping, the compositor and
# the quick menu.
#
# switch/Makefile and android/app/CMakeLists.txt build the same sources for
# those devices; this target builds them for this machine, which is what lets
# the client be run against a real host and looked at (see
# tests/run_switch_client_test.sh). It is compiled on every build, so a file
# added to the client cannot quietly go missing from the ones that are not.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/companion_sources.cmake)

add_executable(dualscreen-companion
    ${BEZEL_SOURCES}
    ${DUALSCREEN_COMPANION_SOURCES}
)
target_include_directories(dualscreen-companion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/companion/source
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(dualscreen-companion PRIVATE
    SDL2::SDL2
    OpenGL::GL
    ZLIB::ZLIB
    Threads::Threads
)
if(APPLE)
    target_link_libraries(dualscreen-companion PRIVATE "-framework CoreFoundation")
endif()
# The same answer the core got, so both ends of a session agree about whether
# there is a codec before they try to negotiate one.
if(FFMPEG_FOUND)
    target_link_libraries(dualscreen-companion PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(dualscreen-companion PRIVATE DS_HAVE_VIDEO_CODEC=1)
else()
    target_compile_definitions(dualscreen-companion PRIVATE DS_HAVE_VIDEO_CODEC=0)
endif()

target_compile_options(dualscreen-companion PRIVATE -Wall -Wextra)
if(DUALSCREEN_PORTABLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(dualscreen-companion PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Same reasoning as dualscreen's own BUILD_RPATH above.
if(FFMPEG_FOUND)
    set_target_properties(dualscreen-companion PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

if(DUALSCREEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# A diagnostic tool, not part of the app: loads a real libretro core and reports
# what it asks the frontend for, without needing a ROM for it.
# What a screen costs on the wire, driven through the real encoder. Not a test:
# it answers a question about a console this app does not run yet.
add_executable(frame_budget tools/frame_budget.cpp src/net/protocol.cpp src/util/log.cpp)
target_include_directories(frame_budget PRIVATE ${CMAKE_SOURCE_DIR}/src ${SDL2_INCLUDE_DIRS})
target_link_libraries(frame_budget PRIVATE ZLIB::ZLIB ${SDL2_LIBRARIES})
target_compile_options(frame_budget PRIVATE -Wall -Wextra)

# Stands in for Cemu, so everything on this side of the frame link can be
# finished and held to before the emulator on the other side of it exists.
add_executable(fake_wii_u tools/fake_wii_u.cpp src/core/external_frame_link.cpp
               src/core/system.cpp src/layout/layout.cpp src/util/log.cpp)
target_include_directories(fake_wii_u PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(fake_wii_u PRIVATE -Wall -Wextra)

add_executable(probe_core tools/probe_core.cpp)
target_include_directories(probe_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(probe_core PRIVATE ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS})
target_include_directories(probe_core PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_options(probe_core PRIVATE -Wall -Wextra)
