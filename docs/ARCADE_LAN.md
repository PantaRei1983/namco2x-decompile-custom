# Arcade LAN: linking real cabinets

**This fork exists for one purpose: restoring real Namco System 22 arcade cabinets, with a PC in each, linked over a
wired LAN the way the original boards were linked by their cable.**

For playing at home or over the internet, use the original project,
[spacestate1/namco22-decompile](https://github.com/spacestate1/namco22-decompile). All the work of rebuilding the games is
the original author's; this fork only adds the cabinet link described here, and follows their releases (currently 0.9.2).

| Game | Arcade LAN | Cabinets |
|---|---|---|
| Tokyo Wars | yes | up to 4 |
| Rave Racer | yes | tested with 4 |
| Time Crisis II | yes (C422, the twin cabinet) | 2 |
| Cyber Cycles | yes (this fork's own port) | up to 4, tested with 4 |

## What it does

- **Links at power-on, like the boards.** Each cabinet looks for the others when its game starts, and only then. When all
  are there (or the start-up wait runs out) they begin together. A cabinet that starts late, or restarts after a crash,
  is left out and plays on its own until the whole set is restarted -- the same rule as the real hardware.
- **No menus to play.** Coins and Start work as on the original machines. The games' own test-menu settings (cabinet ID,
  team colour, car type ...) are kept per cabinet.
- **LAN only.** Nothing here uses the internet.

## Setting up

1. Put the same build on every cabinet PC, on the same wired network (a switch; a fixed address for each PC is best).
2. Start the game on every PC. On each, press `Esc`, open **Network**, choose **Find cabinets**, then **Use the N found**.
   That fills in the address list -- the same on every PC -- with *This cabinet* on *Auto*. (Or type the addresses by hand.)
3. Close the game on every PC and start them all again, full screen.

The settings are in `link.cfg` beside the program (also `record=on|off`: keep the last session's inputs, `<game>_last.rec`).

## The Network page

| Row | |
|---|---|
| Link play | On / Off |
| This cabinet | Auto (its place in the address list) or a number |
| Cabinets in set, Cabinet 1..N | how many, and each one's address |
| Start-up wait | how long to wait for a missing cabinet |
| Link lights | small status lights, one per cabinet, in a corner of the screen |
| If link is lost | Restart this cabinet, or Carry on |
| Frame timing | New (the author's current timer) or Classic (0.4.0's) |
| Restart all cabinets | restarts every cabinet of the set at once (Enter twice) |
| Find cabinets | asks the network which PCs are running the game |

While the cabinets look for each other the screen shows which are found, how quick the network is to each one, and the
countdown; **S** skips the link for that run. The page, the waiting screen and the log say so when a cabinet cannot be
heard, when two are set up differently, or when they are running different builds.

## What it needs

- **A cable.** The games expect every cabinet to answer within about a tenth of a second, all the time. Wi-Fi reconnects
  and power saving break that; a wired switch does not.
- **Full screen** with three or more cabinets: in a window a PC with modest graphics can freeze for a moment, which is
  enough to drop the link.
- **The same build on every cabinet**, and Windows Firewall allowing it on the network the cabinets are on.

## Arcade mode

The menus show only the Arcade LAN. The original author's own online play (a lobby: host or find a game, Ready, Start;
made for home and internet play) is still in the program, hidden: `arcade_mode=off` in `link.cfg` brings back the
**Online** page and a **Link method** row (*Arcade LAN* / *Modern multiplayer*). Only one of the two is used at a time.

## Bonus: several players on one PC

Not what the fork is for, but it comes free with the link: `--party` starts a copy as one part of a split screen (the
parts cover the whole screen; `--party-taskbar` stops above the task bar), and
`--pad K` gives it the K-th game controller only (`--pad 0`: none, the keyboard's copy), read even while another copy's
window has the focus. Two or four linked copies then make a 2- or 4-player game on one PC:

```
TokyoWars roms --window 1 --link 2 --link-cabinets 2 --party --pad 1 --mute
TokyoWars roms --window 1 --link 1 --link-cabinets 2 --party --pad 0
```

Copies on one PC share its graphics chip, so in a party they keep in step (a small piece of shared memory holds each
copy's frame count; a copy more than a frame ahead waits): a stutter holds every picture for a moment instead of dropping
a player from the link. Real cabinets never wait for each other.

## For developers

- `engine/lan139.c` -- the C139 link chip at the level of whole frames; `engine/link_net.c` -- the cabinets' UDP transport
  (ports 22220 + cabinet, discovery on 22219); `engine/link_host.c` -- start-up, the Network page, `link.cfg`, the safety
  net. Notes on the games' link protocols: `docs/LINK_NOTES.md`.
- The author's own link (`engine/c139.c`, `engine/ss22_link.c`, `engine/net.c`) is kept as it is; their `--link` option is
  `--netlink` here, because `--link N` is the Arcade LAN's.
- Tests: `tokyowar/tools/link_tests.sh`, `raverace/tools/link_tests.sh` (four copies on one PC over 127.0.0.1).
