# The Apple bundles' property lists, checked from a machine that is not a Mac.
#
# A plist is the part of an Apple app that nothing compiles. A key missing, a
# placeholder left unexpanded, a stray character in a comment -- none of it is
# a build error. It is an app that launches and behaves oddly, on hardware the
# author may not have, over a file nobody reads twice.
#
# Three of these have already gone wrong here and would have gone unnoticed:
#
#   * "--" inside an XML comment. Not well formed, and a plist that will not
#     parse is one the system substitutes defaults for.
#   * A placeholder CMake never substitutes, which ends up in the bundle
#     verbatim -- a bundle identifier of "${MACOSX_BUNDLE_GUI_IDENTIFIER}"
#     installs and runs and is a different app from the one before it.
#   * NSLocalNetworkUsageDescription left out. Finding a machine on the network
#     *is* this app, and without the key iOS refuses the network silently: the
#     host still appears in the list, because receiving a broadcast needs no
#     permission, and then will not connect.
#
# Run as: cmake -DPLIST_DIR=<apple dir> -P run_apple_plist_test.cmake

# A script run with -P has no project, so the policies a project would set are
# not set here. IN_LIST is the one this needs.
cmake_policy(SET CMP0057 NEW)

if(NOT DEFINED PLIST_DIR)
    message(FATAL_ERROR "PLIST_DIR is required")
endif()
# Where the expanded copies go for xmllint to read. Named rather than defaulted
# to the working directory, which is how three of them ended up in the
# repository root the first time this was run by hand.
if(NOT DEFINED SCRATCH_DIR)
    set(SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/apple-plists")
endif()
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

# SEND_ERROR rather than FATAL_ERROR: every plist is checked and every problem
# reported, then the script exits non-zero at the end. Stopping at the first
# one means finding them one build at a time.
macro(fail message)
    message(SEND_ERROR "${message}")
endmacro()

# What the Apple CMakeLists actually substitutes. A placeholder outside this
# set is a typo, and a typo here survives into the bundle.
set(known
    MACOSX_BUNDLE_EXECUTABLE_NAME
    MACOSX_BUNDLE_BUNDLE_NAME
    MACOSX_BUNDLE_GUI_IDENTIFIER
    MACOSX_BUNDLE_ICON_FILE
    MACOSX_BUNDLE_SHORT_VERSION_STRING
    MACOSX_BUNDLE_BUNDLE_VERSION
    CMAKE_OSX_DEPLOYMENT_TARGET)

find_program(XMLLINT_TOOL xmllint)

foreach(entry "Info.plist.in;macos" "Info-iOS.plist.in;ios" "Info-tvOS.plist.in;tvos")
    list(GET entry 0 name)
    list(GET entry 1 platform)
    set(path "${PLIST_DIR}/${name}")
    if(NOT EXISTS "${path}")
        fail("${name}: not there at all")
        continue()
    endif()
    file(READ "${path}" text)

    # 1. Every placeholder is one the build actually sets.
    string(REGEX MATCHALL "\\$\\{[A-Za-z_][A-Za-z0-9_]*\\}" placeholders "${text}")
    foreach(placeholder ${placeholders})
        string(REGEX REPLACE "^\\$\\{(.*)\\}$" "\\1" bare "${placeholder}")
        if(NOT bare IN_LIST known)
            fail("${name}: ${placeholder} is not something the build substitutes")
        endif()
    endforeach()

    # 2. No "--" inside an XML comment. Expat rejects the file outright, and
    #    what a long explanatory comment is full of is dashes.
    # Walked by hand rather than matched: CMake's regex has no non-greedy
    # repetition, so one pattern for "a comment" swallows the whole file from
    # the first comment to the last.
    string(LENGTH "${text}" length)
    set(cursor 0)
    while(cursor LESS length)
        string(FIND "${text}" "<!--" open_at)
        if(open_at EQUAL -1)
            break()
        endif()
        math(EXPR body_at "${open_at} + 4")
        string(SUBSTRING "${text}" ${body_at} -1 rest)
        string(FIND "${rest}" "-->" close_at)
        if(close_at EQUAL -1)
            fail("${name}: a comment is never closed")
            break()
        endif()
        string(SUBSTRING "${rest}" 0 ${close_at} body)
        if(body MATCHES "--")
            fail("${name}: a comment contains '--', which is not well-formed XML")
        endif()
        math(EXPR after "${close_at} + 3")
        string(SUBSTRING "${rest}" ${after} -1 text)
        string(LENGTH "${text}" length)
        set(cursor 0)
    endwhile()

    # 3. The keys without which the app is broken rather than merely plainer.
    file(READ "${path}" text)
    foreach(key CFBundleIdentifier CFBundleExecutable CFBundleVersion
                NSLocalNetworkUsageDescription)
        if(NOT text MATCHES "<key>${key}</key>")
            fail("${name}: no ${key}")
        endif()
    endforeach()
    if(platform STREQUAL "ios")
        # Without one of these an iPhone runs the app in a compatibility mode
        # at the size of an iPhone 4, letterboxed. On an app whose whole job is
        # showing another machine's screen exactly, that is not cosmetic.
        if(NOT text MATCHES "<key>UILaunchScreen</key>" AND
           NOT text MATCHES "<key>UILaunchStoryboardName</key>")
            fail("${name}: no UILaunchScreen, so iOS will letterbox the app")
        endif()
        if(NOT text MATCHES "<key>UISupportedInterfaceOrientations</key>")
            fail("${name}: no UISupportedInterfaceOrientations")
        endif()
    endif()
    if(platform STREQUAL "macos" AND NOT text MATCHES "<key>NSHighResolutionCapable</key>")
        fail("${name}: no NSHighResolutionCapable, so a Retina Mac gets half the pixels")
    endif()

    # 4. And, where the tool happens to be installed, the real parser. Not
    #    required: the checks above are the ones that caught something, and a
    #    test that only runs on a machine with xmllint is a test that does not
    #    run on the Switch build machine.
    if(XMLLINT_TOOL)
        set(expanded "${text}")
        foreach(variable ${known})
            string(REPLACE "\${${variable}}" "placeholder" expanded "${expanded}")
        endforeach()
        set(scratch "${SCRATCH_DIR}/${name}.expanded.plist")
        file(WRITE "${scratch}" "${expanded}")
        execute_process(COMMAND "${XMLLINT_TOOL}" --noout "${scratch}"
                        RESULT_VARIABLE parsed ERROR_VARIABLE parse_error)
        if(NOT parsed EQUAL 0)
            fail("${name}: does not parse as XML: ${parse_error}")
        endif()
    endif()
endforeach()

if(XMLLINT_TOOL)
    message(STATUS "apple plists: checked, including a real XML parse")
else()
    message(STATUS "apple plists: checked (no xmllint here, so no XML parse)")
endif()
