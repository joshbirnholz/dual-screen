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
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale)
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
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
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
dualscreen_embed_asset(rodin.otf kRodinOtf BEZEL_SOURCES)
dualscreen_embed_asset(button_icons.png kButtonIconsPng BEZEL_SOURCES)

add_library(dualscreen_core STATIC
    ${BEZEL_SOURCES}
    src/app.cpp
    src/audio/audio_output.cpp
    src/video/video_decoder.cpp
    src/video/video_encoder.cpp
    src/config/game_config.cpp
    src/config/paths.cpp
    src/config/settings.cpp
    src/config/sha1.cpp
    src/core/gl_call_trace.cpp
    src/core/hw_render_target.cpp
    src/core/external_frame_link.cpp
    src/core/external_screen_source.cpp
    src/core/libretro_core.cpp
    src/core/libretro_screen_source.cpp
    src/core/core_input_sink.cpp
    src/core/system.cpp
    src/input/controller_mapping.cpp
    src/input/cursor_visibility.cpp
    src/input/ds_input.cpp
    src/input/input_manager.cpp
    src/input/retro_keymap.cpp
    src/layout/layout.cpp
    src/net/companion_session.cpp
    src/net/discovery.cpp
    src/net/host_session.cpp
    src/net/protocol.cpp
    src/net/transport.cpp
    src/layout/layout_registry.cpp
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
    src/ui/game_browser.cpp
    src/ui/launcher_screen.cpp
    src/ui/layout_picker.cpp
    src/ui/message_screen.cpp
    src/ui/settings_screen.cpp
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

# PUBLIC so that anything linking the core -- the companion client included --
# compiles the same headers against the same answer about whether there is a
# codec at all.
if(FFMPEG_FOUND)
    target_link_libraries(dualscreen_core PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(dualscreen_core PUBLIC DS_HAVE_VIDEO_CODEC=1)
else()
    target_compile_definitions(dualscreen_core PUBLIC DS_HAVE_VIDEO_CODEC=0)
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
set(DUALSCREEN_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
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
