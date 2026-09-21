# Compiles the companion the way a device's GL does: OpenGL ES headers only.
#
# iOS, tvOS and the Switch have no desktop GL. Everything core in desktop GL
# and absent from ES -- GL_RED, GL_RG, GL_CONTEXT_PROFILE_MASK,
# GL_CONTEXT_CORE_PROFILE_BIT -- is a compile error there and nowhere here,
# because every build in this repository has desktop GL. Five of them reached
# a person's Xcode, and the compiler gave up partway through rather than list
# them, so they arrived in instalments.
#
# tests/gles_stub/SDL_opengl.h puts the ES2 headers behind the name the
# sources already include, which makes the ordinary host compiler produce the
# device's answer. Same trick as tests/switch_stub, same reason: the build
# that breaks is the one nothing here can run.
#
# What this does not prove: that any of it draws. ES 2.0 has no glBlitFramebuffer
# and no vertex arrays, and the renderer asks at runtime -- see has_blit(). Only
# a device shows whether those answers are right. This is about the compiler.
#
# Invoked by CTest with -DCOMPILER, -DSOURCES, -DSTUB_DIR, -DSRC_DIR,
# -DEXTRA_INCLUDES.

set(SOURCE_LIST ${SOURCES})
set(INCLUDE_FLAGS "-I${STUB_DIR}" "-I${SRC_DIR}")
foreach(dir IN LISTS EXTRA_INCLUDES)
    if(dir)
        list(APPEND INCLUDE_FLAGS "-I${dir}")
    endif()
endforeach()

# Clang for preference, for the same reason as run_no_codec_typecheck.cmake:
# the builds this stands in for are Xcode's and devkitPro's, and both are
# clang. Preferred rather than required, so a GCC-only machine still gets the
# compiles-at-all half.
find_program(GLES_CLANG NAMES clang++)
if(GLES_CLANG)
    set(COMPILER "${GLES_CLANG}")
endif()

foreach(source IN LISTS SOURCE_LIST)
    execute_process(
        COMMAND "${COMPILER}" -fsyntax-only -std=c++17 -Wall -Wextra -Werror
                ${INCLUDE_FLAGS} "${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${source} does not build cleanly against OpenGL ES headers, which "
            "is what iOS, tvOS and the Switch compile against:\n${errors}${output}")
    endif()
endforeach()

message(STATUS "the renderer compiles against OpenGL ES headers, the way a device builds it")
