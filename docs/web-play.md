# Web Play

Web Play lets other people watch and play a Wii U game from a web browser,
with nothing to install. Turn it on in **Settings > Wii U > Web Play**. While a
Wii U game is running, Dualscreen serves a page at

    https://<this computer's address>:8443

and the row in Settings shows the address it found. Anyone who opens it
types a name and joins. They see the TV and hear the game. Any controller
plugged into their device (or paired with it) becomes a controller of the
game. It shows up in Controls and Change Order like any other, under their
name. A phone with no controller gets touch controls.

## Passing the GamePad

Whoever's controller is the GamePad in Controls is shown the GamePad's screen
in their browser instead of the TV, and hears the GamePad's sound. A finger or
the mouse on that picture is the GamePad's touch screen. Everyone else sees
the TV. Passing the GamePad, with Change Order or a player's page, passes the
screen too: in Nintendo Land, whoever takes the GamePad sees it, and whoever
gives it up goes back to the TV.

Holding the GamePad, you hear both screens, and the page's menu (the button
in the corner) has **Show**: the **GamePad** screen, the **TV**, or **Both**,
with **Layout** **Side by Side** or **Stacked** (stacked on a phone held
upright until you choose). **TV Sound** and **GamePad Sound** mute either
one. Touching the GamePad's picture still touches the GamePad.

A phone holding the GamePad is the GamePad being tilted: its own motion
sensors are the GamePad's gyro (Safari asks once). A browser can't read a
controller's own gyro, so a controller plugged into a computer or phone has
none here.

Someone joining or leaving shows as an alert in the corner on this computer,
and everyone in a browser sees the corner alerts too: a player's profile
switched, a controller low on battery.

### Passing it in Controls

In the quick menu's **Controls**, **-** (Swap) picks up a row and **A** on
another trades the two controllers, nobody else moving. When Nintendo Land
asks who will play with the GamePad next, swap the GamePad's row with that
player's.

## The quick menu

When you open the quick menu, everyone in a browser sees it over the game,
so they can see the game is paused and follow along in Change Order: when
Controls is waiting for the next player, they press a button on their own
controller. They can watch it but can't move around in it. A page opened
from the menu, such as Settings or a player's page, isn't shown; they keep
seeing the main menu until you come back to it or close it.

Web Play works in Chrome, Edge, Safari 16.4+ (iOS too) and Firefox 130+.

## HTTPS and the certificate

A browser only lets a page read gamepads and decode video when the page came
over HTTPS, so Dualscreen always serves HTTPS. Opening `http://` by mistake
redirects to `https://`.

Dualscreen doesn't know or care how a browser reaches it. That could be the
same network, a forwarded port, or a VPN. It's a connection to one port in
every case. What changes is the certificate:

- **By default** Dualscreen makes its own certificate the first time and
  keeps it (`web-play-self-signed.crt` and `.key` in its settings folder,
  `~/.config/dualscreen` or `%APPDATA%\dualscreen`). Browsers don't know it,
  so each one shows a warning the first time. Choose to continue (in Chrome:
  Advanced, then Proceed). The log shows the certificate's fingerprint if
  you want to check it's this computer's.
- **With your own certificate** there's no warning. Put the certificate
  chain and its private key, both PEM, in the settings folder as
  `web-play.crt` and `web-play.key`. Dualscreen uses them the next time Web
  Play starts. Any certificate works: one from Let's Encrypt for a domain that
  points here, or one from your VPN.

The port is `web_play_port` in `settings.json` (8443 by default).

## Playing over the internet

Dualscreen has no internet features of its own. Use whatever tool you like to
put friends on the same network as this computer. Two common ways:

### A VPN such as Tailscale

Everyone joins the same tailnet, then opens this computer's tailnet address
on port 8443. To get a certificate browsers trust with no warning, either:

- run `tailscale cert <machine>.<tailnet>.ts.net` and save the two files it
  writes as `web-play.crt` and `web-play.key`, as above, or
- leave Dualscreen alone and put Tailscale's own HTTPS in front of it:

      tailscale serve --bg --https=443 https+insecure://localhost:8443

  Friends then open `https://<machine>.<tailnet>.ts.net/`. Tailscale talks to
  Dualscreen's own certificate (`https+insecure` accepts it) and shows the
  browser a real one.

### Port forwarding

Forward a TCP port on your router to port 8443 on this computer, and give
friends `https://<your public address>:<port>`. Anyone on the internet can
open the page and join, so only leave the port forwarded while you're
playing. A VPN is safer.

## Bandwidth

Each browser is sent its own copy of the stream, about 6 Mbit/s at 720p. Four
friends over the internet need about 24 Mbit/s of upload here. A browser on a
slow connection drops frames until the next keyframe rather than falling
behind.

## How it works

`src/net/web_play_server.{hpp,cpp}` is the protocol: HTTPS (mbedTLS,
`src/net/web_tls.*`), the page (`assets/web/index.html`, built into the app),
and one WebSocket per browser. Over it go H.264 (Annex B, decoded in the
browser with WebCodecs) and plain PCM out, and the browser's gamepads (in
SDL's button and axis order) in. The quick menu goes out as a picture of the
same overlay drawn over Cemu's window (JPEG when it covers the game, PNG when
the game shows through), sent only from its main page. `src/net/web_play_host.*`
runs it on a thread of its own with the encoders.

Cemu sends Dualscreen only the screens it asks for, and the TV is normally
shown in Cemu's own window instead. So Web Play asks Cemu to *mirror* the
screens browsers are watching: capture and send them while still showing and
playing them itself (`kExternalFeatureMirror`, the `mirrored` byte of
`RequestScreen` in `src/core/external_frame_link.hpp`).

Browsers' pads reach the game as pads of this computer's
(`InputManager::set_foreign_pads`). Their ids start at `kWebPadIdBase`. The
browser is told what each of its pads is ("Player 2") as Controls changes.
