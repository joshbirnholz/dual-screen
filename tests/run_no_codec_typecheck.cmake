# Compiles the companion the way iOS and tvOS compile it: with no FFmpeg.
#
# DS_HAVE_VIDEO_CODEC=0 is not a hypothetical configuration. There is no
# Homebrew for a phone, so a device build goes without FFmpeg unless somebody
# cross-compiles one, and that is the configuration those platforms ship in --
# with VideoToolbox doing the decoding instead, which is what it was already
# doing underneath FFmpeg all along.
#
# Nothing here has ever built it. On Linux FFmpeg is always found, so the
# #else branch of video_decoder.cpp is dead code to every build in this
# repository and would rot without noticing: it is one half of a preprocessor
# fork, and the compiler never sees it.
#
# This proves the configuration compiles. It proves nothing about whether
# VideoToolbox works, which needs Apple hardware -- see apple/README.md.
#
# Warnings are errors here, and the compiler is clang when there is one, for
# the same reason the test exists: the only build that compiles this half is
# Xcode's, and Xcode is clang. A warning that only this configuration
# produces -- eight unused private fields, three colours with no readers --
# otherwise reaches a person looking at a device build, which is the slowest
# possible place to find out. Clang is preferred rather than required, so a
# machine with only GCC still gets the compiles-at-all half.
#
# Invoked by CTest with -DCOMPILER, -DSOURCES, -DSRC_DIR, -DEXTRA_INCLUDES.

# SOURCES arrives as a CMake list (semicolon-separated), so it is already one
# -- separate_arguments would re-join it into a single impossible filename.
set(SOURCE_LIST ${SOURCES})
set(INCLUDE_FLAGS "-I${SRC_DIR}")
foreach(dir IN LISTS EXTRA_INCLUDES)
    if(dir)
        list(APPEND INCLUDE_FLAGS "-I${dir}")
    endif()
endforeach()

find_program(NO_CODEC_CLANG NAMES clang++)
if(NO_CODEC_CLANG)
    set(COMPILER "${NO_CODEC_CLANG}")
    set(WHICH "clang, the way Xcode would")
else()
    set(WHICH "${COMPILER}, with no clang here to use instead")
endif()

foreach(source IN LISTS SOURCE_LIST)
    execute_process(
        COMMAND "${COMPILER}" -fsyntax-only -std=c++17 -Wall -Wextra -Werror
                -DDS_HAVE_VIDEO_CODEC=0
                ${INCLUDE_FLAGS} "${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${source} does not build cleanly without FFmpeg, which is how iOS "
            "and tvOS build it:\n${errors}${output}")
    endif()
endforeach()

message(STATUS
    "the companion compiles warning-free with no FFmpeg, the way a device "
    "builds it -- using ${WHICH}")
