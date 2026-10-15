# Roadmap

The ongoing list of work to do, in the order it is to be done. Take the top
item that is not done; when it ships, delete it from here (the commit history
keeps the record). New requests go in where they belong in the order, not at
the end by default. Items refer to each other by name, not number, so the
numbers can change freely.

## Next up

1. **Starting a session from a connected device.** A small background service
   on the PC, started at login, that a Switch or tablet finds as it finds a
   running host. Paired once with a code. It lists the PC's Dualscreen Steam
   shortcuts (read from Steam's `shortcuts.vdf`), falling back to the ROM
   folders, and launches through the shortcut so Steam Input applies; the
   device joins the session that starts. Wake-on-LAN from the device, with the
   PC's MAC remembered from earlier connections (needs WoL enabled on the PC;
   a Steam Deck cannot be woken this way).

2. **Microphone, on every platform.** Captured through SDL (Linux, Android,
   iOS) and the Switch's audio input; fed to melonDS DS and Azahar through
   libretro's microphone interface, and to Cemu as the GamePad's (needs the
   fork). A connected device can send its own. Game Options: **Microphone:
   This Device / *device name* / Blow Only / Off** -- Blow Only plays breath
   noise while a button is held.

3. **Per-game DS/3DS button remapping.** A controller diagram, as Nintendo
   Classics has, each button opening a list of actions: the console's buttons,
   Quick Menu, Swap Screens, Swap Layout, Blow into Mic (see Microphone), Turbo, and
   **Touch Screen Point** -- placed on the paused game's touch screen by
   finger, mouse or the right stick's Touch Cursor, kept as a fraction of the screen.
   Saved per game with a per-console default; "Revert to Default Mapping" on
   X; mirrored on both devices. The diagram is then reused for the Wii U players' controller types.

4. **Smoother scrolling on the following device.** The device that is not
   being scrolled glides to each position it is told rather than jumping, and
   keeps the highlighted row in view when its screen fits fewer rows than the
   host's. Small -- can move up whenever wanted.

## Low priority

5. **Screenshot on the Capture button.** Both screens as laid out, saved
   where Steam keeps screenshots on a Deck.

6. **Rewind and fast-forward for DS.** Hold to rewind, from the save state
   support already there.

7. **Game library.** A grid of games with art. Mostly covered by Steam
   shortcuts, and starting a session from a connected
   device gives it a list anyway.

8. **Hand a game off to another device.** "Continue on *device*": its resume
   point moved over and started there. Save states only load in the same
   core (a Deck and an Android tablet running the same melonDS DS can trade
   them; the Switch's own melonDS port cannot), so elsewhere it falls back to
   the in-game save, which Save Sync already moves.

## Not possible for now

- **The 3DS keyboard as a Dualscreen popup.** Azahar's libretro core answers a
  game's keyboard itself with its default stub (`RegisterDefaultApplets`) and
  never asks the frontend; it would take a fork of the core.
