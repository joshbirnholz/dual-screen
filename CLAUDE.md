# Working on Dualscreen

## The work list

`ROADMAP.md` is the ongoing, ordered list of work. Read it at the start of a
session. Work from the top unless asked otherwise. When an item ships, delete
it from the list in the same push; when something new is asked for, add it
where it belongs in the order; when an item is dropped, remove it. Keep the
list current -- it is how work carries across sessions.

## How changes are made

- Commit and push each fix or feature on its own.
- Run the tests touched while working; run the full suite (`cd build && ctest
  -j4`, about 6 minutes) before every push, and push only when it passes.
- The Cemu fork (joshbirnholz/cemu-dualscreen, branch
  `claude/dualscreen-frame-link`) shares `src/core/external_frame_link.{hpp,cpp}`
  with this repo as `src/dualscreen/core/`, and `src/audio/audio_stretch.{hpp,cpp}`
  as `src/dualscreen/audio/`; the two copies stay identical.

## How the app should feel

- Every menu works the same with a pad, touch, mouse and wheel, on whichever
  device it is drawn; every page is a `MenuPage` (see `src/ui/menu_page.hpp`
  and the `lint_menu_pages` test). With two devices, a page of the host's is
  one page on both: cursor, popups and scroll the same -- but for Settings and
  its pages, which are each device's own.
- Lists: a first tap highlights, a second chooses; a dropdown's choice is
  chosen by its first tap. Smooth scrolling, no wrap-around; held directions
  repeat every 75 ms; the cursor never moves unexpectedly.
- B goes back one level; the menu button closes everything.
- Title Case for menu labels (but "Save slot", as the Switch spells it).
- Values right-aligned; no yellow or orange text. Devices are named by their
  names. Dialogs speak in the future tense ("will change"). Info-level log
  lines are worded as a player would say them.
- Steam Input is the only source of truth for what a button does.
