# Roadmap

The ongoing list of work to do, in the order it is to be done. Take the top
item that is not done; when it ships, delete it from here (the commit history
keeps the record). New requests go in where they belong in the order, not at
the end by default. Items refer to each other by name, not number, so the
numbers can change freely.

## Next up

### Controls

- **Button mapping, the rest of it.** DS and 3DS games have Controls >
  Change Button Mapping now: each pad button given one of the console's
  buttons, Touch Point (placed on the paused game's touch screen), Activate
  Touch Cursor, Quick Menu, Swap Screens, Next Screen Layout, Blow into
  Mic or Disabled, kept with the game; Y toggles turbo on
  a button pressing one of the console's, X is Reset to Default, one page on
  both devices. Still to do, in this
  order, from the mockups (https://claude.ai/artifact/WjbNkq2jKemE92hbj6pcMk):
  - Right Stick on the mapping page itself: Touch Cursor or Touch Analog
    Stick, with Choose Spot.
  - Mapping profiles, saved and loaded from the page; loading one copies it
    into the game. The mockups open them with Y, which now toggles turbo:
    they need another button.
  - Wii U: the Controls tab as the mockups have it, then each player's
    controller type and the mapping of that type's buttons, motion and
    rumble. The player-type option doing nothing is a bug to look at first,
    once there is a log of it.

### After remapping

- **Starting a session from a connected device.** Not to be worked on
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

- The background service should also be able to make the device visible
  for save data syncing even when Dualscreen isn't open, making automatic
  save file syncing more useful.

- **Microphone, on every platform.** Captured through SDL (Linux, Android,
  iOS) and the Switch's audio input; fed to melonDS DS and Azahar through
  libretro's microphone interface, and to Cemu as the GamePad's (needs the
  fork). A connected device can send its own. Game Options: **Microphone:
  This Device / *device name* / Blow Only / Off** -- Blow Only plays breath
  noise while a button is held.

- The app should be able to make it as easy as possible to connect with
  other devices running Dualscreen, even if those devices aren't on the same
  network, or there is no Internet or Wifi connection at all. This could mean
  enabling a device to be used as a hotspot so that other devices can connect
  to it. Dualscreen should be able to facilitate both sides of this to make
  things as easy as possible. On Switch, one switch playing a DS game could
  connect to another Switch acting as a companion, and be able to connect
  using the Switch's native local wireless. Should look into what, if any,
  similar options are available on other platforms.

## Low priority

- Update the README.md. I'd like it written less as a technical document, and
  more as an introduction to Dualscreen, listing its features, and how to use
  it. Some of it is out of date, including the screenshots. The technical
  details can be saved in a separate document.

- **Screenshot, mappable to a button.** Both screens as laid out, saved
  where Steam keeps screenshots on a Deck.

- **Rewind and fast-forward for DS.** Hold to rewind, from the save state
  support already there.

- **Game library.** A grid of games with art. Mostly covered by Steam
  shortcuts, and starting a session from a connected
  device gives it a list anyway.

- **Hand a game off to another device.** "Continue on *device*": its resume
  point moved over and started there. Save states only load in the same
  core (a Deck and an Android tablet running the same melonDS DS can trade
  them; the Switch's own melonDS port cannot), so elsewhere it falls back to
  the in-game save, which Save Sync already moves.

## Not possible for now

- **The 3DS keyboard as a Dualscreen popup.** Azahar's libretro core answers a
  game's keyboard itself with its default stub (`RegisterDefaultApplets`) and
  never asks the frontend; it would take a fork of the core.
