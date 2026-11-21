# Namco C139 link (SCI) — research notes

Working notes for LAN link play. Everything here is inferred from the games' own
driver code (the 68K listing in the `/* ... */` comments of `gen/*_lifted.c`);
MAME's C139 is a stub (status reads `0x0004`, nothing is sent) so there is no
reference implementation to compare against.

## The chip, as the 68K sees it

| | Super 22 (Tokyo Wars, Dirt Dash) | System 22 (Rave Racer) |
|---|---|---|
| buffer, 16 KB | `0x410000` | `0x20010000` |
| send area (own packet) | `0x410000` (word offset 0) | `0x20010000` |
| receive ring, 0x1000 words | `0x412000` (word offset `0x1000`) | `0x20012000` |
| registers, 8 x 16-bit | `0x420000`–`0x42000E` | `0x20020000`–`0x2002000E` |
| interrupt | syscon line 2 (`0x700002`), TW sets level 4, vector `0x932A0` | syscon SCI line |

Buffer words are 9-bit characters: bits 0-7 data, bit 8 marks the last word of a
frame (the sender sets it; the checksum word starts at `0x100`).

Registers (word offset → meaning, from Tokyo Wars' use):

| reg | addr | meaning (inferred) |
|---|---|---|
| 0 | `+0x0` | status. bit 1 = a frame was received. Written 0 by the handler = acknowledge. Interrupt with bit 1 clear = "transmitter idle". |
| 1 | `+0x2` | control. Values seen `0xF`, `0xD`, `0xB`, `0x9`. bit 1 = mask the receive interrupt, bit 2 = mask the transmit-idle interrupt (level-triggered: the driver masks it whenever its send queue is empty). |
| 2 | `+0x4` | bit 1 = the CPU's writes into the buffer carry the 9th bit (end of frame). Rave Racer writes 1, then 3 just before its checksum word; Tokyo Wars leaves it 0 and writes the bit itself. |
| 3 | `+0x6` | written 1 then 0 around starting a send when more sends are queued (strobe; no visible effect needed so far) |
| 4 | `+0x8` | `0xFF` at init |
| 5 | `+0xA` | send word count. Write `0x46` (70) starts a send; reads non-zero while the transmitter is busy. |
| 6 | `+0xC` | receive write pointer, in words, `0x1000`-based, wraps in `0x1000..0x1FFF`. Init `0x1000`. |
| 7 | `+0xE` | send source, in words from the buffer base: 0 = own packet, `0x1000+n` = a received frame in the ring (forwarding). |

## Tokyo Wars' protocol (driver at `0x92F82`–`0x93820`)

- Cabinet number = EEPROM byte 9, bits 0-1 (read at `0x92F82` with `movep.l` from `0x460010`): 0..3.
- Called every frame from `0x4A02` / `0x11232`: `0x9362E` (who answered, ring size, link health) then `0x93072` (send own packet).
- Frame, 70 words: `0x81+id`, hop, `0xA2`, `0x73+id`, 64 data bytes, hop (copy), checksum (`0x100` + byte sum).
- **Ring, store-and-forward.** On receive (handler `0x932A0`): find the end marker scanning back from reg 6, check `0xA2`,
  both hop copies equal, hop+1 ≤ 4, id bytes agree, id < 16; increment both hop copies (and checksum by 2) in the ring
  buffer, copy the data to the sender's double buffer, then forward the frame by pointing reg 7 at it. A frame that
  comes back to its sender stops there; its hop count = ring size.
- Link health: 8 frames without the own frame returning → link bad. The set of cabinets heard must be stable for 8 frames.
- Each cabinet sends its own state (authoritative), so there is no lockstep and no "desync" in the lockstep sense:
  a late or lost frame only means the others see slightly old data for that frame.

## Rave Racer's protocol (driver at `0x26B12`–`0x27670`)

- Cabinet number = operator settings group 3 byte 0 (EEPROM `0x2C0`, copied to WRAM `0x10001060`), 0..7.
- Link state at A6 = `0x10008000`: `+0x11A0` own number, `+0x1104` who is heard (own bit = own frame back within 8 frames),
  `+0x1102` cabinets in the ring, `+0x1108` frames since the own frame came back.
- Frame, 40 words: sender/slot in word 0 (low 4 bits), 38 data bytes, the length `0x28`, checksum (byte sum) with bit 8
  set through register 2. Store-and-forward ring like Tokyo Wars; send queue drained from the transmit-idle interrupt.
- Interrupt handler `0x26BBE` reads status: bit 0 error (counted), bit 1 frame received, bit 2 transmitter idle.
- The attract screen offers LINK / SOLO when another cabinet is heard.

## Emulation approach

Packet-level, not bit-level: when the game starts a send, the model takes the 70 words and delivers them to the next
cabinet in the ring (UDP over the LAN, or 127.0.0.1 for several instances on one PC). A receiving model writes one frame
at a time into its ring at reg 6, sets status bit 1 and raises the interrupt; the next queued frame is delivered only
after the game acknowledges. The ring order skips cabinets that are switched off, so a dead PC does not break the others.

Start-up follows the boards' rule (they look for each other only at power-on): each cabinet waits up to 60 s for the others,
then the set starts together with a fixed member list; a cabinet started later is not let in (engine/link_net.h).

## Timing: what breaks the link on PCs (found 2026-09-29, 3-device test)

The games want every cabinet to pass the ring's frames on within about 8 frames (~0.13 s), all the time: the real boards on their
cable always did. On PCs anything that holds one cabinet (or its network) still for longer drops the game's link for a moment, and
afterwards the frames pile up at the next cabinet (thousands of queue-drops); Tokyo Wars then ends the battle ("BATTLE OVER").
Seen on a 3-PC Wi-Fi set: in WINDOWS an Intel UHD 630 PC froze 0.1-0.2 s in busy scenes -> hundreds of queue-drops and battles ended;
the same three in FULL SCREEN (exclusive 640x480) played whole matches. Reproduced here with `--link-test 0:0:0:0:300:120` (Wi-Fi
bursts: 120 ms of every 300 ms held back): ~120 game link losses in 2.5 min with a 32-frame queue, ~70 with 58.
A slow-but-steady PC (TW_SLOWDRAW=20:12, ~55 fps in a window) did NOT break it (0-1 drops): the ring paces itself.
Tried and removed: a "catch-up" (run frames undrawn when behind the board's clock) -- it turned slowness into bursts and made it worse
(4 link losses and 154 drops against 0 and 0). Kept: the chip's queue at 58 frames (the real ring buffer's size).
Advice: full screen, cabled network; on Wi-Fi a "best performance" power plan.

## Alpine Racer 2's protocol (driver at `0x60FC`–`0x63BE`, 2 cabinets)

- Super 22 chip addresses (as Tokyo Wars). Init `0x60FC`; the receive interrupt is level 4 (vector `0x60DC` -> `0x616C`), enabled
  each frame by the send routine (syscon `0x700002` = 4); only the receive interrupt is used (control `0xFFFD`).
- **Point to point, no forwarding.** Each frame `0x6270` writes its packet into a double buffer at word 0 or `0x200`:
  `[n+2][0xA5][n bytes][0x100 | n+1]`, and when register 5 reads 0 sends `n+2` words from the buffer word after the length
  (register 7 = offset + 1). The receive handler scans back from register 6 (masked `0xFFF`) to the end word, checks `0xA5`
  and keeps up to 8 frames; the frame routine copies them to 250-byte slots at `0xE17E90`.
- The game's packet (`0x2EA54`, `0x66` bytes, a 16-bit sum) carries `0x17300000 + counter << 4 + ID` at `+0x10`; the receiver
  (`0x2EDB4`) rejects its own echo and a cabinet with the same ID. The ID = operator setting `+0x32` (RAM `0xE186EA`, EEPROM
  byte `0x32`). Link state word `0xE1867E`, shown in the test menu: 4 LINK ON 2, 1 LINK OFF, 7 LINK ON 1, 9 LINK NG.
- With the ID set (alpinr2 `link_id_byte = 0x32`) the existing chip model links it unchanged: skier select says THIS IS
  HEAD-TO-HEAD RACE, the race shows positions out of 2 and a RIVAL marker.
