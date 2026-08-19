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

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

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

add_library(dualscreen_core STATIC
    src/app.cpp
    src/audio/audio_output.cpp
    src/config/game_config.cpp
    src/config/paths.cpp
    src/config/settings.cpp
    src/config/sha1.cpp
    src/core/libretro_core.cpp
    src/core/libretro_screen_source.cpp
    src/core/core_input_sink.cpp
    src/input/ds_input.cpp
    src/input/input_manager.cpp
    src/layout/layout.cpp
    src/layout/layout_registry.cpp
    src/render/compositor.cpp
    src/render/font.cpp
    src/render/gl_loader.cpp
    src/render/quad_renderer.cpp
    src/render/text_renderer.cpp
    src/render/texture.cpp
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
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

if(DUALSCREEN_WERROR)
    target_compile_options(dualscreen_core PRIVATE -Wall -Wextra -Werror)
else()
    target_compile_options(dualscreen_core PRIVATE -Wall -Wextra)
endif()

add_executable(dualscreen src/main.cpp)
target_link_libraries(dualscreen PRIVATE dualscreen_core)

install(TARGETS dualscreen RUNTIME DESTINATION bin)

if(DUALSCREEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
