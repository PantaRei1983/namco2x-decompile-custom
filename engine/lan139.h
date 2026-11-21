/*
 * lan139.h -- the Namco C139 serial link controller ("SCI"), at the level of whole frames. See docs/LINK_NOTES.md for what the
 * registers do and how Tokyo Wars' driver uses them.
 *
 * The chip owns a 16 KB buffer the 68K also sees (big-endian 16-bit words, 9-bit characters): the first 0x1000 words hold what the
 * game sends, the second 0x1000 words are a ring the chip writes received frames into. A send (register 5 = word count, register 7 =
 * where the words are) goes out as ONE network packet to the next cabinet (engine/link_net.c); a packet received is written into the
 * ring at register 6, and the game is interrupted. Frames are handed to the game one at a time: the next only after the game has
 * acknowledged the last (status written), or after a few slices if it never does, so a burst of frames is never merged.
 *
 * Off (no --link) the chip answers exactly as before (MAME's stub): status 0x0004, sends go nowhere, no interrupt.
 */
#ifndef ENG_C139_H
#define ENG_C139_H
#include <stdint.h>
#include <stdbool.h>

void     lan139_init(uint8_t *buf);                   /* the 16 KB buffer the 68K maps; the link is on when link_net is open */
bool     lan139_on(void);
bool     lan139_eof_writes(void);                     /* register 2 bit 1: the CPU's writes into the buffer carry the 9th bit, "last word of a
                                                     * frame" (Rave Racer writes 3 there before its checksum word; Tokyo Wars writes the bit
                                                     * itself and leaves register 2 at 0) */
uint16_t lan139_reg_r(unsigned unit);                 /* unit 0..7 = the 16-bit registers at +0x0 .. +0xE */
void     lan139_reg_w(unsigned unit, uint16_t data, uint16_t mask);
bool     lan139_tick(bool new_slice);                 /* finish a send, take in a frame; true = interrupt wanted. Called a few times a
                                                     * scheduler slice (new_slice on the first) so a frame is forwarded within the slice */
bool     lan139_irq(void);                            /* the interrupt condition now */
/* At most N received frames handed to the game per video frame, the rest kept for the next (0 = no limit, the default). The real
 * line brings each cabinet a steady few a frame; when a PC stalls and the network then delivers a burst, a game whose driver copies
 * every frame received since the last one in its main loop (Cyber Cycles: a 16-message queue) would otherwise get them all at once. */
void     lan139_set_per_frame(int n);
void     lan139_frame(void);                          /* every video frame */

typedef struct { uint32_t tx, rx, rx_dropped, rx_unacked, tx_restart; uint32_t q_sum, q_n, q_max; } lan139_stats;   /* q_*: the queue waiting for the game, sampled per video frame (link report)  */
const lan139_stats *lan139_get_stats(void);
#endif
