# Parses the one file that decides whether the iOS app launches at all.
#
# apple/ios_scene_lifecycle.m is Objective-C, it is compiled only for iOS and
# tvOS, and before this existed nothing on a Linux machine had ever looked at
# it. A mistyped selector or a missing include would have been found by a
# person, in Xcode, at the end of a build -- which is how every iOS problem in
# this project has been found so far.
#
# tests/uikit_stub holds the slice of UIKit and SDL the file names. Same trick
# as tests/switch_stub and tests/gles_stub, same reason: the build that breaks
# is the one nothing here can run.
#
# -fobjc-runtime=macosx because Apple's runtime is what it will be compiled
# against, and the GNU runtime clang defaults to on Linux rejects instance
# variables declared in an @implementation -- which is a difference between
# runtimes, not a mistake in the file.
#
# What this does not prove: that a scene ever connects, that SDL's window ends
# up on it, or that the app draws. Those need a simulator. This proves the
# file agrees with the shape of the API it calls.
#
# Invoked by CTest with -DCOMPILER, -DSOURCE, -DSTUB_DIR.

find_program(UIKIT_CLANG NAMES clang)
if(UIKIT_CLANG)
    set(COMPILER "${UIKIT_CLANG}")
endif()

execute_process(
    COMMAND "${COMPILER}" -fsyntax-only -x objective-c -fobjc-runtime=macosx
            -Wall -Wextra -Werror
            "-I${STUB_DIR}" "${SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "${SOURCE} does not parse, and it is what makes the iOS app "
        "launch:\n${errors}${output}")
endif()

message(STATUS "the UIKit scene life cycle file parses")
