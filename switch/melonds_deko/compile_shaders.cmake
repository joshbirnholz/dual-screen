# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles the shaders melonDS's deko3d renderers load, the way RSDuck's own
# Switch build does, into a directory that becomes the app's romfs.
#
#   cmake -DMELONDS=<melonDS tree> -DOUT=<romfs>/shaders -DWORK=<scratch dir>
#         -DUAM=<uam> -P compile_shaders.cmake
#
# The list is read from that tree's src/frontend/switch/CMakeLists.txt rather
# than copied here, so it is always the list the renderers were written with:
# compile_shader(stage path) compiles <path>.glsl as it is, and
# compile_shader_instance(stage path name "A;B") first makes a copy with A and B
# switched on -- configure_file() turning each `#cmakedefine A` into
# `#define A` -- and compiles that as <name>.

foreach(required MELONDS OUT WORK UAM)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "compile_shaders.cmake needs -D${required}=...")
    endif()
endforeach()

set(frontend "${MELONDS}/src/frontend/switch")
set(list_file "${frontend}/CMakeLists.txt")
if(NOT EXISTS "${list_file}")
    message(FATAL_ERROR "no ${list_file}: is ${MELONDS} RSDuck's switch-new melonDS?")
endif()
if(NOT EXISTS "${UAM}")
    message(FATAL_ERROR "no shader compiler at ${UAM}: install it with (dkp-)pacman -S uam")
endif()

file(MAKE_DIRECTORY "${OUT}" "${WORK}")
file(READ "${list_file}" text)
# A CMake list is a string joined with semicolons, so the "A;B" a call names
# its defines with would split the call in two once the calls are a list.
# Carried as | until each call is taken apart.
string(REPLACE ";" "|" text "${text}")

function(compile stage source name)
    set(output "${OUT}/${name}.dksh")
    if(EXISTS "${output}" AND NOT "${source}" IS_NEWER_THAN "${output}")
        return()
    endif()
    execute_process(COMMAND "${UAM}" "--stage=${stage}" --out "${output}" "${source}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "uam could not compile ${name} from ${source}:\n${out}${err}")
    endif()
    message(STATUS "shader ${name}")
endfunction()

# The defines are set inside the function, so each instance starts with none.
function(compile_instance stage path name defines)
    foreach(define IN LISTS defines)
        set(${define} 1)
    endforeach()
    set(generated "${WORK}/${name}.glsl")
    configure_file("${frontend}/${path}.glsl" "${generated}")
    compile("${stage}" "${generated}" "${name}")
endfunction()

set(count 0)
string(REGEX MATCHALL "compile_shader_instance\\([^)]*\\)" instances "${text}")
foreach(call IN LISTS instances)
    if(NOT call MATCHES "compile_shader_instance\\(([a-z]+)[ \t]+([^ \t]+)[ \t]+([^ \t]+)[ \t]+\"([^\"]*)\"\\)")
        message(FATAL_ERROR "cannot read ${call}")
    endif()
    string(REPLACE "|" ";" defines "${CMAKE_MATCH_4}")
    compile_instance("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}" "${defines}")
    math(EXPR count "${count} + 1")
endforeach()

# compile_shader( itself, which the instances above do not match: their name
# goes on past "compile_shader" before the parenthesis.
string(REGEX MATCHALL "compile_shader\\([a-z]+[ \t]+[^ \t)]+\\)" plain "${text}")
foreach(call IN LISTS plain)
    string(REGEX MATCH "compile_shader\\(([a-z]+)[ \t]+([^ \t)]+)\\)" _ "${call}")
    get_filename_component(name "${CMAKE_MATCH_2}" NAME_WE)
    compile("${CMAKE_MATCH_1}" "${frontend}/${CMAKE_MATCH_2}.glsl" "${name}")
    math(EXPR count "${count} + 1")
endforeach()

if(count LESS 40)
    message(FATAL_ERROR "found only ${count} shaders in ${list_file}; has the list moved?")
endif()
message(STATUS "${count} shaders in ${OUT}")
