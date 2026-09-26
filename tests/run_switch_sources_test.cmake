# Whether the Switch Makefile compiles every source the companion needs.
#
# The companion is one program built three ways: by CMake for a desktop, by
# CMake for iOS, tvOS and Android, and by switch/Makefile for a Switch. The
# first two read one list, cmake/companion_sources.cmake. The Makefile keeps its
# own, because devkitPro's rules are Make -- and a second list is a list that
# falls behind. It has, twice: once when the Switch build was new, and again
# when src/util/build_stamp.cpp joined the companion and the Makefile never
# heard, so a Switch build compiled every file and then failed to link.
#
# Nothing here can run that link, since nothing here has devkitPro. It can
# compare the two lists, which is the whole of what went wrong both times.
#
# Invoked by CTest with -DSOURCES_CMAKE and -DSWITCH_MAKEFILE.

# For if(IN_LIST), which a -P script does not get without asking.
cmake_minimum_required(VERSION 3.16)

file(READ "${SOURCES_CMAKE}" cmake_text)
string(REGEX MATCH "set\\(DUALSCREEN_COMPANION_SOURCES([^)]*)\\)" _ "${cmake_text}")
string(REGEX MATCHALL "[A-Za-z0-9_]+\\.cpp" cmake_files "${CMAKE_MATCH_1}")
list(REMOVE_DUPLICATES cmake_files)
if(NOT cmake_files)
    message(FATAL_ERROR "found no sources in ${SOURCES_CMAKE}; has the list moved?")
endif()

file(READ "${SWITCH_MAKEFILE}" make_text)
string(REGEX MATCH "CPPFILES[ \t]*:=(.*)\\$\\(EMBEDDED_SOURCES\\)" _ "${make_text}")
string(REGEX MATCHALL "[A-Za-z0-9_]+\\.cpp" make_files "${CMAKE_MATCH_1}")
if(NOT make_files)
    message(FATAL_ERROR "found no CPPFILES in ${SWITCH_MAKEFILE}; has the list moved?")
endif()

set(missing "")
foreach(source IN LISTS cmake_files)
    if(NOT source IN_LIST make_files)
        list(APPEND missing "${source}")
    endif()
endforeach()
if(missing)
    list(JOIN missing ", " missing_text)
    message(FATAL_ERROR
        "the companion compiles ${missing_text} everywhere else, but switch/Makefile "
        "does not list it, so a Switch build will fail to link. Add it to CPPFILES.")
endif()

list(LENGTH cmake_files count)
message(STATUS "switch/Makefile lists all ${count} of the companion's sources")

# And a Switch build with a DS core linked in, which is the desktop app as well
# as the companion: everything the desktop app's library is built from has to
# be in one list or the other, or that build fails to link in the same way.
if(DESKTOP_CMAKE)
    file(READ "${DESKTOP_CMAKE}" desktop_text)
    string(REGEX MATCH "add_library\\(dualscreen_core STATIC([^)]*)\\)" _ "${desktop_text}")
    string(REGEX MATCHALL "[A-Za-z0-9_]+\\.cpp" desktop_files "${CMAKE_MATCH_1}")
    if(NOT desktop_files)
        message(FATAL_ERROR "found no dualscreen_core sources in ${DESKTOP_CMAKE}; has it moved?")
    endif()
    string(REGEX MATCH "LOCAL_PLAY_CPPFILES[ \t]*:=([^\n]*(\\\\\n[^\n]*)*)" _ "${make_text}")
    string(REGEX MATCHALL "[A-Za-z0-9_]+\\.cpp" local_play_files "${CMAKE_MATCH_1}")
    if(NOT local_play_files)
        message(FATAL_ERROR "found no LOCAL_PLAY_CPPFILES in ${SWITCH_MAKEFILE}; has it moved?")
    endif()
    set(missing "")
    foreach(source IN LISTS desktop_files)
        if(NOT source IN_LIST make_files AND NOT source IN_LIST local_play_files)
            list(APPEND missing "${source}")
        endif()
    endforeach()
    if(missing)
        list(JOIN missing ", " missing_text)
        message(FATAL_ERROR
            "the desktop app is built from ${missing_text}, but switch/Makefile lists it in "
            "neither CPPFILES nor LOCAL_PLAY_CPPFILES, so a Switch build with a DS core will "
            "fail to link. Add it to LOCAL_PLAY_CPPFILES.")
    endif()
    list(LENGTH desktop_files desktop_count)
    message(STATUS "and every one of the desktop app's ${desktop_count} sources, for local play")
endif()
