# Compiles the Apple-only halves of the sources on a machine with no Apple SDK.
#
# videotoolbox_decoder.cpp and the texture cache in texture.cpp are only ever
# compiled by Xcode, and every mistake in them used to be found by a person
# building for their Apple TV -- one at a time, since the compiler stops at
# the first file it cannot build. tests/apple_stub declares the slice of
# CoreFoundation, CoreMedia, CoreVideo and VideoToolbox those halves name, the
# way the SDK does, and DS_TYPECHECK_APPLE turns the halves on without
# claiming to be __APPLE__ to every system header on this machine.
#
# Twice: as iOS/tvOS, where decoded pictures are drawn from where they are,
# and as macOS, where they are copied out.
#
# Invoked by CTest with -DCOMPILER, -DSOURCES, -DSTUB_DIR, -DGLES_STUB_DIR,
# -DSRC_DIR, -DEXTRA_INCLUDES.

set(SOURCE_LIST ${SOURCES})
set(INCLUDE_FLAGS "-I${STUB_DIR}" "-I${GLES_STUB_DIR}" "-I${SRC_DIR}")
foreach(dir IN LISTS EXTRA_INCLUDES)
    if(dir)
        list(APPEND INCLUDE_FLAGS "-I${dir}")
    endif()
endforeach()

# Clang for preference: it is what Xcode is.
find_program(APPLE_CLANG NAMES clang++)
if(APPLE_CLANG)
    set(COMPILER "${APPLE_CLANG}")
endif()

foreach(iphone IN ITEMS 1 0)
    foreach(source IN LISTS SOURCE_LIST)
        # -Wno-four-char-constants: '420f' is how CoreVideo spells a format.
        execute_process(
            COMMAND "${COMPILER}" -fsyntax-only -std=c++17 -Wall -Wextra -Werror
                    -Wno-multichar -Wno-four-char-constants
                    -DDS_TYPECHECK_APPLE=1 "-DTARGET_OS_IPHONE=${iphone}"
                    ${INCLUDE_FLAGS} "${source}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors)
        if(NOT result EQUAL 0)
            if(iphone)
                set(which "iOS and tvOS")
            else()
                set(which "macOS")
            endif()
            message(FATAL_ERROR
                "${source} does not build the way ${which} would build it:\n${errors}${output}")
        endif()
    endforeach()
endforeach()

message(STATUS "the Apple halves compile against the declarations in tests/apple_stub")
