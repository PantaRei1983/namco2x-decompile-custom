# Gunmen Wars cabinet link

Status: **LAN link working (2 cabinets: a full match; 3 cabinets: team select and play; all on one PC).**

- Each cabinet sees the other as a human player:
  - at team select;
  - in play;
  - with the same scores on both screens and a win on one, a loss on the other.
- The test menu (GAME OPTIONS) shows `LINK ON 2`.
- An online transport can be added without touching the emulation (see *Layers*).

## How the real link works

- The cabinets are linked by **IEEE 1394 (FireWire)**. Each cabinet's GMEN board has:
  - an SH-2 CPU, running a program the main CPU uploads at boot;
  - a Fujifilm **MD8412B** link-layer chip.
- Our port runs that SH-2 program in an interpreter. The MD8412B is our own model.
  Both are in `src/gw_gmen.c`.
- The main CPU never touches the bus. Its per-cabinet data and the "frame ready" word go through
  the RAM it shares with the SH-2 (main `0x0E70xxxx`). The SH-2 moves that data over the bus.

## Layers

| Layer | Where | What it does |
|---|---|---|
| Chip | `src/gw_gmen.c` | Each packet it sends goes to `gw_link_send()`. Bus packets come back through `gw_fw_deliver()`, which keeps what is addressed to this node, as the real chip does. |
| Bus | `src/gw_link.c` | The session (who is on the bus, formed at power-on), game-time stamps, lockstep, and the delivery queue. |
| Cable | `src/gw_link.c`, a `gw_link_transport` | Moves datagrams between cabinets. Here: UDP on the local network. |

The cable interface:

```c
typedef struct {
    const char *name;
    int  (*open)(const gw_link_cfg *c, char *why, size_t wn);   /* 1 = ready */
    void (*send)(int cab, const uint8_t *b, int n);             /* to one cabinet; -1 = to all the others */
    int  (*recv)(uint8_t *b, int max);                          /* the next datagram, 0 = none */
    void (*wait)(int ms);                                       /* until something arrives, at most ms */
    void (*close)(void);
} gw_link_transport;
```

- **An online transport** (a relay server, as in the author's online link for the other games)
  implements these five calls.
  - Its datagrams are opaque.
  - Byte 4 of each is the sending cabinet's number.
- **Lockstep:**
  - Every packet carries its sender's game time.
  - It is delivered at the receiver's game time = sent + LAG.
  - LAG is the Link page's **Network** setting (`link_net` in `gunwars.cfg`): `lan` 0.1 frame (the default),
    `wifi` 1, `slow` 3, `internet` 6 frames. Every hello carries it (header byte 6) and while the cabinets find
    each other each one takes the largest, so all play alike. `GW_LINK_LAG=<frames>` / `GW_LINK_NET=0-3` for tests.
  - Tested: two copies, one on `internet` and one on `lan`: both adopted 6 frames, a full match, 0 packets late.
  - A datagram that fills up and goes early promises only the stamp of the packet that did not fit (`flush_to`);
    promising the board's whole run made packets late with the longer delays.

## What the game needs (read off the SH-2 program and the main program)

1. **PCB ID** (test menu, GAME OPTIONS) **must be different on every cabinet**, or they do not
   link.
   - It is the main program's setting `0x790` in the backup RAM, at physical `0x06000F20`:
     - the high byte in the low byte of the halfword at `+0`;
     - the low byte in the low byte of the halfword at `+2`.
   - While linked, the port sets it to the cabinet number - 1.
2. **Self-ID:**
   - After a bus reset the chip delivers **one** group: `E0`, each node's self-ID packet followed
     by its inverse, then one status word `1`.
   - The board's parser (`0x040165FC`) clears its node table at every `E0`. So a group per node
     leaves only the last node.
   - PHY register 0 holds this node's PHY number (bits 2-7) and the root bit (bit 1). The root is
     the highest node.
3. **Bus setup:**
   - The root sends four quadlet writes, the other node answers, and both reach state 0
     (`0x0405CD18`).
   - The root then broadcasts a token every bus cycle (isochronous, type 1). It says who may send.
4. **Receive FIFOs:**
   - **Async**, `0x44`:
     - Receive format: q0 = destination << 16 | tl/rt/tcode; q1 = source << 16 | offset high.
     - Then the rest of the packet, then a status word.
     - Empty = register `0x50` bit 2. Interrupt bit 20.
   - **Isochronous**, `0x4C` (also on the real chip):
     - Empty = `0x50` bit 6. Interrupt bit 22.
   - A block write's data is read out with **SH-2 DMA channel 0**, which is now modelled
     (auto-request).
5. **Transmit:**
   - Game data travels as async **block writes** (tcode 1) broadcast to `FFFF`. They are put in
     the transmit FIFO by DMA.
   - The FIFO's empty flag (`0x50` bit 0) must come back once it has been sent.
6. **IRQ6 once a frame:**
   - The board's IRQ6 handler (`0x04011920`) is its per-frame link step. It swaps buffers and sets
     the "frame ready" word at shared `+0xC040`.
   - The main program waits for that word every frame (`0x80093D10`); without it, LINK shows NG.
   - Its source on the real board is not known, presumably the main board's frame. We raise it
     once a frame while linked.
7. **Timing:** the SH-2 must catch up with the main CPU every slice while linked. Otherwise it
   runs only when the main CPU touches it, which can be 24 frames apart at power-on.

## NamCam photos and the menu

- **Photos:**
  - The photo page (texture page `0x1F000`) has a 128 x 128 square per cabinet, by PCB ID, in a row: cabinet n at
    u = 128 n (measured with 3 linked: 0, 128, 256). The engine's photo texture is 1024 x 256 (`S23_PHOTO_W`).
  - The real boards pass the pictures over the bus. Here each cabinet sends its own over the cable as 28 `I`
    datagrams (16-bit colour), again every 2 s.
  - Received photos go into the sender's square: `gw_link_photo()` and `photo_in()` in `src/gw_link.c`,
    `gw_photo_slot()` in `src/gw_game.c`.
  - Tested: team select and the in-play side lists show both photos.
- **Menu:** the Esc menu has a **Link** page (`gw_link_page`, `src/gw_link.c`): on/off, this cabinet, cabinets,
  find the others (network / this PC), port, and the current state. It writes the `link` lines of `gunwars.cfg`;
  changes take effect at the next start.

## Settings and tests

- **Settings:** the `link` lines of `gunwars.cfg`, the game's own settings file. The engine rewrites only its own lines,
  so these stay. They are added on the first run, with the link off:
  - `link = on | off`
  - `link_cabinet` (1, 2, ...): every PC a different one; the game's PCB ID follows it
  - `link_cabinets`
  - `link_address`: `auto`, a list of the other PCs, or `loop` for copies on one PC (each in its own folder)
  - `link_port`
  - `link_wait`
  - `link_lights`
  - `link_net`: `lan`, `wifi`, `slow` or `internet`
- **Tests:** `GW_LINK=<cabinet>:<cabinets>:<first port>` runs copies on one PC. Each copy needs its
  own folder. For example `GW_LINK=1:2:23150` and `GW_LINK=2:2:23150`.
- **Two headless copies:** `build/linktest/run2.sh <instructions>`, from `build/linktest`
  (`cab1/`, `cab2/`).

| Debug switch | What it does |
|---|---|
| `GW_FWLOG=1` | Bus resets, self-ID, PHY writes, async tx/rx, an isochronous sample, DMA |
| `GW_LINKLOG=1` | Each datagram |
| `GW_LINKSTAT=1` | Once a second: the main program's link state (LINK OK, slot, the ready word) |
| `GW_SH2WATCH=a,b,..` | Value changes of SH-2 longwords |
| `GW_SH2WWATCH=a,..` | Writes to SH-2 longwords, with the writer's PC |
| `GW_SH2PC=a,b,..` | How often SH-2 addresses run |
| `GW_SHWATCH=lo-hi` | Writes to the shared RAM (main M / SH-2 S) |
| `GW_SHWATCH_FROM=frame` | Start of the `GW_SHWATCH` log |
| `GW_CMDLOG`, `GW_RDLOG` | Main writes to the command word, main reads of `C000-C0FF` |
| `GW_IRQ6VB=0` | No IRQ6 after boot with one cabinet (MAME's behaviour; the default is once a frame, which makes the game draw its own NamCam screen) |
| `GW_FWNODES=n` | One cabinet: the fake node count |

## Still to do

- **Two real PCs:**
  - `address=auto` broadcasts to find the others.
  - Windows' firewall asks once.
- **Four or more cabinets:** the code allows up to 8; 3 work, 4 is experimental (cabinet 3 stalls at team select).
- **Launcher:** "2 players on this PC (2 linked windows)" (each copy in its own `try-N` folder, `S23_CABINET_DIR`; windows
  tiled with `S23_WINPOS=N/M`; copy 2 muted, `S23_MUTE`; one game controller each, `S23_PAD=n`).
- **A cabinet that drops out mid-game:** the others carry on after 5 s; the game's own handling
  has not been checked.
