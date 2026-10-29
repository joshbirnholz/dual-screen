# Roadmap

The ongoing list of work to do, in the order it is to be done. Take the top
item that is not done; when it ships, delete it from here (the commit history
keeps the record). New requests go in where they belong in the order, not at
the end by default. Items refer to each other by name, not number, so the
numbers can change freely.

## Next up

### After remapping

- **Waking a computer to start a game on.** Remote Start (Settings) lets a
  paired device start a game on a PC or Deck that is on; still to do is
  Wake-on-LAN for one that is asleep, with the PC's MAC remembered from its
  answer to an earlier search (the packet itself is built and tested in
  `net/launch_service`; needs WoL enabled on the PC; a Steam Deck cannot be
  woken this way).

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
