# Game icons

The official HOME Menu icon of every 3DS and Wii U game, as Nintendo's own
icon servers (`idbe-ctr` / `idbe-wup.cdn.nintendo.net`) serve them to the
consoles, so Discord can show the game being played in Dualscreen.

- `3ds/<TITLE ID>.png`, `wiiu/<TITLE ID>.png`: 256x256, title IDs as 16
  uppercase hex digits, e.g. `wiiu/000500001010EC00.png`.
- `index.json`: the English name of each, for finding one by hand.

Filled once by `tools/build_icons.py` (log in `tools/fill-log.txt`); no new
3DS or Wii U games are being made, so nothing adds to it. This branch shares
no history with the app's code.
