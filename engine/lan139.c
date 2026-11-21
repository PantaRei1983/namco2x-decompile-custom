/* lan139.c -- see lan139.h. */
#include <stdio.h>
#include <string.h>
#include "lan139.h"
#include "link_net.h"
#include <stdlib.h>
#include <time.h>
/* LINK_TRACE=1 (test): every frame sent, arrived and handed to the game, with the video frame.slice and the wall clock */
static int tr_on = -1; static unsigned tr_frame, tr_slice;
static void tr(const char *what, const uint16_t *w, int n)
{
    if (tr_on < 0) tr_on = getenv("LINK_TRACE") != NULL;
    if (!tr_on) return;
    uint32_t h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(stderr, "[LTR] %s %u.%02u %.2f n%d %04X %04X %08X\n", what, tr_frame, tr_slice, t.tv_sec * 1e3 + t.tv_nsec / 1e6, n, n > 0 ? w[0] : 0, n > 1 ? w[1] : 0, (unsigned)h);
}

#define RING_BASE 0x1000u                   /* the receive ring: words 0x1000..0x1FFF of the buffer */
#define RING_MASK 0x0FFFu
#define QMAX      58                        /* frames waiting for the game (the real ring buffer's 0x1000 words hold about 58 of 70);
                                             * beyond this the oldest go (a stalled PC catches up on fresh data) */
#define ACK_SLICES 3                        /* a frame the game has not acknowledged by then is overwritten by the next, as on the real chip */

enum { ST_RX = 0x0002 };                    /* status bit 1: a frame was received */
enum { CT_RX_MASK = 0x0002, CT_TX_MASK = 0x0004 };   /* control (register 1) */

static struct {
    uint8_t *buf;
    uint16_t reg[8];
    uint16_t status;
    bool     tx_busy;
    int      tx_len;
    uint16_t tx[LNK_MAX_WORDS];
    int      unacked;
    uint16_t q[QMAX][LNK_MAX_WORDS]; int qlen[QMAX]; int qhead, qn;
    int      per_frame, this_frame;         /* lan139_set_per_frame: frames handed over per video frame (0 = no limit) */
    lan139_stats st;
} C;

static uint16_t wr_(unsigned w) { w &= 0x1FFF; return (uint16_t)(C.buf[2 * w] << 8 | C.buf[2 * w + 1]); }
static void ww_(unsigned w, uint16_t v) { w &= 0x1FFF; C.buf[2 * w] = (uint8_t)(v >> 8); C.buf[2 * w + 1] = (uint8_t)v; }

void lan139_init(uint8_t *buf)
{
    memset(&C, 0, sizeof C);
    C.buf = buf;
    C.reg[6] = RING_BASE;
}

bool lan139_on(void) { return lnk_on(); }
const lan139_stats *lan139_get_stats(void) { return &C.st; }

bool lan139_irq(void)
{
    if (!lnk_on()) return false;
    const uint16_t ct = C.reg[1];
    return ((C.status & ST_RX) && !(ct & CT_RX_MASK)) || (!C.tx_busy && !(ct & CT_TX_MASK));
}

bool lan139_eof_writes(void) { return lnk_on() && (C.reg[2] & 2); }

uint16_t lan139_reg_r(unsigned unit)
{
    unit &= 7;
    if (!lnk_on()) return unit == 0 ? 0x0004 : 0;              /* MAME's namcos22_sci_r */
    switch (unit) {
    case 0: return (uint16_t)(0x0004 | C.status);
    case 5: return C.tx_busy ? C.reg[5] : 0;                   /* non-zero while the transmitter is busy */
    default: return C.reg[unit];
    }
}

/* a send: the words are taken now (the game has finished writing them); they leave at the next slice */
static void start_tx(void)
{
    unsigned n = C.reg[5] & 0x1FF, src = C.reg[7];
    if (!n) return;
    if (C.tx_busy) C.st.tx_restart++;
    if (n > LNK_MAX_WORDS) n = LNK_MAX_WORDS;
    for (unsigned i = 0; i < n; i++)
        C.tx[i] = (src & RING_BASE) ? wr_(RING_BASE | ((src + i) & RING_MASK)) : wr_(src + i);
    C.tx_len = (int)n;
    C.tx_busy = true;
}

void lan139_reg_w(unsigned unit, uint16_t data, uint16_t mask)
{
    unit &= 7;
    if (!lnk_on()) return;                                     /* MAME: writes do nothing */
    uint16_t v = (uint16_t)((C.reg[unit] & ~mask) | (data & mask));
    switch (unit) {
    case 0: C.status = 0; C.unacked = 0; break;               /* the handler's acknowledge */
    case 5: C.reg[5] = v; start_tx(); break;
    case 6: C.reg[6] = (uint16_t)(RING_BASE | (v & RING_MASK)); break;
    default: C.reg[unit] = v; break;
    }
}

static void deliver(void)
{
    int n = C.qlen[C.qhead]; const uint16_t *w = C.q[C.qhead]; tr("RX", w, n);
    C.qhead = (C.qhead + 1) % QMAX; C.qn--;
    unsigned p = C.reg[6] & RING_MASK;
    for (int i = 0; i < n; i++) ww_(RING_BASE | ((p + (unsigned)i) & RING_MASK), w[i]);
    C.reg[6] = (uint16_t)(RING_BASE | ((p + (unsigned)n) & RING_MASK));
    C.status |= ST_RX;
    C.unacked = 0;
    C.st.rx++;
}

bool lan139_tick(bool new_slice)
{
    if (!lnk_on()) return false;
    if (new_slice) tr_slice++;
    if (C.tx_busy) { tr("TX", C.tx, C.tx_len); lnk_send(C.tx, C.tx_len); C.tx_busy = false; C.st.tx++; }
    if (new_slice && (C.status & ST_RX) && ++C.unacked > ACK_SLICES) { C.status &= (uint16_t)~ST_RX; C.st.rx_unacked++; }
    for (;;) {                                                 /* everything the network has, into the queue */
        int slot = (C.qhead + C.qn) % QMAX;
        if (C.qn == QMAX) { C.qhead = (C.qhead + 1) % QMAX; C.qn--; C.st.rx_dropped++; slot = (C.qhead + C.qn) % QMAX; }
        int n = lnk_recv(C.q[slot], LNK_MAX_WORDS);
        if (n <= 0) break;
        C.qlen[slot] = n; C.qn++; tr("IN", C.q[slot], n);
    }
    if (!(C.status & ST_RX) && C.qn && (!C.per_frame || C.this_frame < C.per_frame)) { deliver(); C.this_frame++; }
    return lan139_irq();
}

void lan139_set_per_frame(int n) { C.per_frame = n > 0 ? n : 0; }
void lan139_frame(void)          { tr_frame++; tr_slice = 0; C.this_frame = 0; C.st.q_sum += (uint32_t)C.qn; C.st.q_n++; if ((uint32_t)C.qn > C.st.q_max) C.st.q_max = (uint32_t)C.qn; }
