# Whether every menu page is a MenuPage, and hears its taps through it.
#
# A page of menu answers a finger, the mouse and the wheel the same way on
# whichever device draws it only if it goes through MenuPage
# (src/ui/menu_page.hpp): scroll() for its lists, entry_at() for what a point
# is on, and take_taps() for the taps, its own and the other device's alike.
# A page written later that reads its pointers for itself would work under a
# finger here and do nothing for a tap made on the other device -- which is
# what Graphics Packs and Controller Order did until they were brought in.
#
# So here: every class in src/ui/*.hpp with an update() that takes pointers
# must derive from MenuPage, unless it is listed below with why not; and the
# .cpp of a header declaring MenuPages must ask take_taps() at least once for
# each of them.
#
# Invoked by CTest with -DUI_DIR.

cmake_minimum_required(VERSION 3.16)

# Not pages: each is part of one, which feeds it what a MenuPage heard.
set(not_pages
    # A dropdown, inside the page that opened it: the page's scroll() and
    # entry_at() hand on to its scroll() and choice_at().
    ChoicePopup
    # A popup's buttons, answered by the page holding it: see tap().
    PopupCursor
    # The drag and glide under a list, which a page's scroll() runs.
    ListDrag
    # Typing a name: the text field and keyboard of whatever page is open.
    TextEntryPopup
    # An update's offer, over Settings, answered as Settings is.
    UpdatePrompt
    # A message that any press or tap at all puts away: nothing to land on.
    MessageScreen
)

file(GLOB headers "${UI_DIR}/*.hpp")
set(failures "")
set(pages_checked 0)
foreach(header IN LISTS headers)
    file(STRINGS "${header}" lines)
    set(current_class "")
    set(current_is_page FALSE)
    set(in_update FALSE)
    set(signature "")
    set(menu_pages 0)
    foreach(line IN LISTS lines)
        if(line MATCHES "^class ([A-Za-z_0-9]+)([^;]*)$")
            set(current_class "${CMAKE_MATCH_1}")
            if(CMAKE_MATCH_2 MATCHES "public MenuPage")
                set(current_is_page TRUE)
                math(EXPR menu_pages "${menu_pages} + 1")
            else()
                set(current_is_page FALSE)
            endif()
            continue()
        endif()
        if(NOT in_update AND line MATCHES "[ \t]update\\(")
            set(in_update TRUE)
            set(signature "")
        endif()
        if(in_update)
            string(APPEND signature "${line}")
            if(line MATCHES "[;{]")
                set(in_update FALSE)
                if(signature MATCHES "PointerEvent" AND NOT current_class STREQUAL "")
                    if(NOT current_is_page AND NOT current_class IN_LIST not_pages)
                        string(APPEND failures
                            "  ${current_class} (${header}) takes pointers in update() but is not a MenuPage\n")
                    endif()
                endif()
            endif()
        endif()
    endforeach()

    if(menu_pages GREATER 0)
        string(REGEX REPLACE "\\.hpp$" ".cpp" source "${header}")
        if(NOT EXISTS "${source}")
            string(APPEND failures "  ${header} declares a MenuPage with no ${source} to hear its taps\n")
        else()
            file(READ "${source}" source_text)
            string(REGEX MATCHALL "take_taps\\(" asks "${source_text}")
            list(LENGTH asks ask_count)
            if(ask_count LESS menu_pages)
                string(APPEND failures
                    "  ${source} asks take_taps() ${ask_count} time(s) for ${menu_pages} MenuPage(s): "
                    "a page that never asks hears no tap from the other device\n")
            endif()
        endif()
        math(EXPR pages_checked "${pages_checked} + ${menu_pages}")
    endif()
endforeach()

if(pages_checked EQUAL 0)
    message(FATAL_ERROR "found no MenuPage in ${UI_DIR}; has it moved?")
endif()
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "menu pages that skip MenuPage:\n${failures}"
        "See src/ui/menu_page.hpp, or list a class in tests/lint_menu_pages.cmake with why it is not a page.")
endif()
message(STATUS "${pages_checked} menu pages, all MenuPages")
