# Roadmap

The ongoing list of work to do, in the order it is to be done. Take the top
item that is not done; when it ships, delete it from here (the commit history
keeps the record). New requests go in where they belong in the order, not at
the end by default. Items refer to each other by name, not number, so the
numbers can change freely.

## Next up

### Fixes, easiest first

1. **Graphics Packs from a connected device: dropdowns.** Scrolling an open
   option dropdown by touch on the connected device scrolls it on the host
   but not there, and lifting the finger puts it back where it was.

2. **Graphics Packs from a connected device: the left-hand list.** Scrolling
   it works but is slow and not smooth. (With this, the following device
   should glide to each position it is told rather than jumping, and keep the
   highlighted row in view when its screen fits fewer rows than the host's.)

3. **Quick menu animation over a Cemu game** is not as smooth as over a DS
   or 3DS game.

4. **Long names in Graphics Packs.** A pack's setting names, and the choices
   in its dropdowns, are cut off -- worst where both are long. Needs a
   solution (e.g. the highlighted row's text scrolling to show the rest, as
   the Switch does, and dropdowns wide enough for their longest choice).

### Controls

5. **"Controller Order" becomes "Controls"** in the Wii U quick menu. The
   Player 1-4 controller types move into it, with a button on it that opens
   the Controller Order screen. It is where remapping goes, so DS and 3DS
   games get the tab too.

6. **Per-game button remapping** -- the next major feature. **Show mockups
   first**: the Controls page in the quick menu, and any subscreen opened
   from it. A controller diagram, as Nintendo Classics has, each button
   opening a list of actions: the console's buttons, Quick Menu, Swap
   Screens, Swap Layout, Blow into Mic, Turbo, **Touch Screen Point** (placed
   on the paused game's touch screen by finger, mouse or the Touch Cursor,
   kept as a fraction of the screen), and **Touch** -- the Touch Cursor's
   tip, so it can be R2 rather than R3 (most 3DS games leave ZL/ZR alone, and
   L1/R1 are already L and R). Saved per game with a per-console default;
   "Revert to Default Mapping" on X; mirrored on both devices. For Cemu, each
   player's controller type and the assignment of that type's buttons.

### After remapping

7. **Starting a session from a connected device.** Not to be worked on
    until remapping is done. Half built on the branch
    `claude/remote-start-wip`: the Steam `shortcuts.vdf` reader and game
    list, the background service (`dualscreen --service`, a systemd user
    unit) with pairing codes and tokens, the device-side calls, and
    Wake-on-LAN packets, all tested. Still to do: the Settings page that
    turns the service on and shows a pairing code, and the connected
    device's screens to pick a PC and a game and join the session that
    starts; Wake-on-LAN with the PC's MAC remembered from earlier
    connections (needs WoL enabled on the PC; a Steam Deck cannot be woken
    this way).

8. **Microphone, on every platform.** Captured through SDL (Linux, Android,
    iOS) and the Switch's audio input; fed to melonDS DS and Azahar through
    libretro's microphone interface, and to Cemu as the GamePad's (needs the
    fork). A connected device can send its own. Game Options: **Microphone:
    This Device / *device name* / Blow Only / Off** -- Blow Only plays breath
    noise while a button is held.

## Low priority

9. **Screenshot on the Capture button.** Both screens as laid out, saved
    where Steam keeps screenshots on a Deck.

10. **Rewind and fast-forward for DS.** Hold to rewind, from the save state
    support already there.

11. **Game library.** A grid of games with art. Mostly covered by Steam
    shortcuts, and starting a session from a connected
    device gives it a list anyway.

12. **Hand a game off to another device.** "Continue on *device*": its resume
    point moved over and started there. Save states only load in the same
    core (a Deck and an Android tablet running the same melonDS DS can trade
    them; the Switch's own melonDS port cannot), so elsewhere it falls back to
    the in-game save, which Save Sync already moves.

## Not possible for now

- **The 3DS keyboard as a Dualscreen popup.** Azahar's libretro core answers a
  game's keyboard itself with its default stub (`RegisterDefaultApplets`) and
  never asks the frontend; it would take a fork of the core.
