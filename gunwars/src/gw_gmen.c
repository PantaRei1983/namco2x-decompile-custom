/* gw_gmen.c -- Gunmen Wars' GMEN board: its SH-2 (Hitachi SH7604) and what it sees, after MAME's namcoss23_gmen_state (namcos23.cpp).
 *
 * The main CPU writes the SH-2's program into the 64 KB RAM they share (main 0x0E700000 = SH-2 0x00000000, mirrored at 0x01000000) and
 * releases the SH-2 from reset by reading 0x0E400000; the SH-2 starts from the vectors at the bottom of that RAM (PC at +0, SP at +4).
 * A command the main CPU writes at +0x20 (high half below 0x0100) raises the SH-2's IRQ6; the SH-2 writing +0x20 with a zero high half
 * drops it. The program is uploaded, so it is INTERPRETED here (ours, from the SH-1/SH-2 programming manual), not translated.
 *
 * FIRST STAGE: the VPX-3220 (camera) and the MD8412B (FireWire link) answer as MAME's map does (line reads 0x7FFF0000, status
 * 0x04000000); the link chip's registers are plain storage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gw_link.h"

uint8_t gw_gmen_ram[0x10000 + 4];                     /* shared with the main CPU (src/gw_game.c) */
static uint8_t ram18[0x400000], ram40[0x400000];  /* 0x01800000 and 0x04000000 (SH-2 work SDRAM) */
static uint8_t onchip[0x200 + 4];                     /* 0xFFFFFE00..0xFFFFFFFF: the SH7604's own registers, kept as written */
static uint8_t cache_data[0x1000 + 4];                /* 0xC0000000: the cache's data array used as RAM */
/* ---- THE MD8412B (Fujifilm IEEE-1394 link layer: the cabinet link), our model of what MAME's md8412b_s23 device does. 32-bit
 * registers; the async receive FIFO (0x44) gets the self-ID and the answers; PHY registers through 0x20; a bus reset (PHY reg 1 bit 6)
 * reports a TWO-node bus as MAME does (this PHY 0 = root, PHY 1 its child); the 8 kHz cycle timer; the async transmit FIFO (0x40,
 * sent on the next cycle): write requests to the three Namco addresses get MAME's fixed answers, others an echo; interrupt status 0x54
 * (write 1 = clear) under mask 0x58 (1 = masked) -> the SH-2's IRQ 8. ONE CABINET: nothing really goes out.
 * LINKED (src/gw_link.c, LINK.md): a real bus instead. The self-ID lists the cabinets present (this one's PHY number in PHY register 0, the
 * highest the root); async packets go to the others in the receive format (q0 = destination, q1 = source, then the rest, a status word last)
 * and arrive in the async receive FIFO (0x44, interrupt bit 20); isochronous packets go to all and arrive in the isochronous receive FIFO
 * (0x4C, empty = 0x50 bit 6, interrupt bit 22) -- read off the board's program: its cycle handler 0x04015CF0 reads 0x4C, 0x04015E1C 0x44. */
static struct {
    uint32_t ctrl, node_id, async_bs, sync_bs, packet_ctrl, diag, phy_ctrl, at_rc, cycle, sync_len, sync_cfg[4], buf, intr, imask, tgo, bus_time, at_r;
    uint8_t phy[8];
    uint32_t arf[512]; int arf_r, arf_n;
    uint32_t atf[512]; int atf_n;
    uint32_t itf[512]; int itf_n, iso_going;   /* the isochronous transmit FIFO (0x48): sent on the next cycle, nobody listens (one cabinet) */
    uint32_t irf[512]; int irf_r, irf_n;       /* the isochronous receive FIFO (0x4C): linked only */
    int async_phase, cycle_started, self_id_done;
    uint64_t now;                              /* the game time (main CPU cycles) of the chip's present tick: the packets' stamp */
} fw;
static void fw_reset(void)
{
    memset(&fw, 0, sizeof fw);
    fw.ctrl = 0x00030001; fw.node_id = 0x0000FFFF; fw.async_bs = fw.sync_bs = 0x007F00FF; fw.packet_ctrl = 0x00001020; fw.at_rc = 1;
    fw.sync_len = 0x00040000; fw.buf = 0x55; fw.imask = 0xFFFFFFFF; fw.at_r = 0x00C80000;
    static const uint8_t phy0[7] = { 0x03, 0x3F, 0x43, 0xF4, 0xF0, 0xF0, 0x20 }; memcpy(fw.phy, phy0, 7);
}
static void arf_push(uint32_t w) { if (fw.arf_n < 512) { fw.arf[(fw.arf_r + fw.arf_n) & 511] = w; fw.arf_n++; } fw.buf &= ~4u; }
static uint32_t arf_pop(void) { if (!fw.arf_n) return 0; const uint32_t w = fw.arf[fw.arf_r]; fw.arf_r = (fw.arf_r + 1) & 511; if (!--fw.arf_n) fw.buf |= 4u; return w; }
static void irf_push(uint32_t w) { if (fw.irf_n < 512) { fw.irf[(fw.irf_r + fw.irf_n) & 511] = w; fw.irf_n++; } fw.buf &= ~0x40u; }
static uint32_t irf_pop(void) { if (!fw.irf_n) return 0; const uint32_t w = fw.irf[fw.irf_r]; fw.irf_r = (fw.irf_r + 1) & 511; if (!--fw.irf_n) fw.buf |= 0x40u; return w; }
static int fw_irq(void) { return (fw.intr & ~fw.imask) != 0; }
static int fwlog(void) { static int lg = -1; if (lg < 0) lg = getenv("GW_FWLOG") != NULL; return lg; }   /* $GW_FWLOG: the link traffic */
static void fw_self_id(void)
{
    { extern uint32_t rr_frame; if (fwlog()) fprintf(stderr, "[FW] f%u bus reset: self-ID\n", rr_frame); }
    fw.phy[0] = 0x03; fw.phy[3] = 0xFC; fw.phy[6] |= 0x10;
    const uint32_t gap = (uint32_t)(fw.phy[1] & 0x3F) << 16;
    fw.self_id_done = 1;
    const int nodes_linked = gw_link_nodes();
    if (nodes_linked > 0) {                                   /* LINKED: the cabinets present, a chain 0 - 1 - ... - n-1 (the root, highest) */
        const int me = gw_link_node(), n = nodes_linked;
        fw.phy[0] = (uint8_t)(me << 2 | (me == n - 1 ? 2 : 0) | 1);
        /* ONE self-ID group: E0, each node's packet and its inverse, one status word. The board's parser (0x040165FC) clears its node
         * table at each E0, so a group per node (MAME's way, kept for one cabinet below) leaves only the last node in it */
        arf_push(0xE0);
        for (int k = 0; k < n; k++) {                         /* port 0 towards the lower node (a child), port 1 towards the higher (the parent) */
            const uint32_t p0 = k == 0 ? (n > 1 ? 2u : 1u) : 3u, p1 = (k > 0 && k < n - 1) ? 2u : 1u;
            const uint32_t q = 0x80000000u | (uint32_t)k << 24 | 1u << 22 | gap | 1u << 14 | 1u << 11 | p0 << 6 | p1 << 4 | 1u << 2 | (k == me ? 2u : 0u);
            arf_push(q); arf_push(~q);
        }
        arf_push(1);
        if (fwlog()) fprintf(stderr, "[FW] self-ID: %d nodes, this one %d%s\n", n, me, me == n - 1 ? " (root)" : "");
        return;
    }
    uint32_t p = 0x80000000u | 1u << 22 | gap | 1u << 14 | 1u << 11 | 3u << 6 | 1u << 4 | 1u << 2 | 1u << 1;
    arf_push(0xE0); arf_push(p); arf_push(~p); arf_push(1);
    /* the other nodes: MAME reports one (PHY 1, our child); $GW_FWNODES=n (TEST) reports n nodes in all */
    static int nodes = -1; if (nodes < 0) { const char *e = getenv("GW_FWNODES"); nodes = e ? atoi(e) : 2; if (nodes < 1) nodes = 1; if (nodes > 8) nodes = 8; }
    for (int k = 1; k < nodes; k++) {
        p = 0x80000000u | (uint32_t)k << 24 | 1u << 22 | gap | 1u << 14 | 1u << 11 | 2u << 6 | 1u << 4 | 1u << 2;
        arf_push(0xE0); arf_push(p); arf_push(~p); arf_push(1);
    }
}
static void fw_async_tx_linked(void)
{
    const uint32_t w0 = fw.atf[0], w3 = fw.atf[3];
    const int tcode = (int)(w0 >> 4 & 15);
    int len = 4;                                              /* quadlets: a write request (0) has its datum in q3, a block write (1) its data after */
    if (tcode == 1) len = 4 + (int)(((w3 >> 16) + 3) / 4);
    else if (tcode == 4) len = 3;                             /* a quadlet read request */
    if (len > GW_PKT_MAXQ - 1) len = GW_PKT_MAXQ - 1;
    if (fw.atf_n < len) return;                               /* the rest of the packet is not written yet */
    uint32_t q[GW_PKT_MAXQ];
    const uint32_t dst = fw.atf[1] >> 16, src = 0xFFC0u | (uint32_t)gw_link_node();
    q[0] = dst << 16 | (w0 & 0xFFFFu); q[1] = src << 16 | (fw.atf[1] & 0xFFFFu);
    for (int i = 2; i < len; i++) q[i] = fw.atf[i];
    q[len] = (w0 & 0x00030000u) | 1;                          /* the status word: the speed, ack_complete */
    { extern uint32_t rr_frame; static unsigned n; if (fwlog() && n++ < 400) fprintf(stderr, "[FW] f%u async tx tcode %d to %04X, %d quadlets: %08X %08X %08X %08X\n", rr_frame, tcode, dst, len, q[0], q[1], len > 2 ? q[2] : 0, len > 3 ? q[3] : 0); }
    gw_link_send(GW_PKT_ASYNC, (int)(dst & 63), q, len + 1, fw.now);
    memmove(fw.atf, fw.atf + len, (size_t)(fw.atf_n - len) * 4); fw.atf_n -= len;
    fw.diag = (fw.diag & ~0xF00u) | 0x100u;                   /* acknowledged at once (the cable is not lossy enough to model the wait) */
    fw.intr |= 1u << 23;
}
static void fw_async_tx(void)
{
    if (gw_link_nodes() > 0) { fw_async_tx_linked(); return; }
    if (fw.atf_n < 4) return;
    const uint32_t w0 = fw.atf[0], w1 = fw.atf[1], w2 = fw.atf[2], w3 = fw.atf[3];
    { extern uint32_t rr_frame; static unsigned n; if (fwlog() && n++ < 400) fprintf(stderr, "[FW] f%u async tx %08X %08X %08X %08X (fifo %d more)\n", rr_frame, w0, w1, w2, w3, fw.atf_n - 4); }
    memmove(fw.atf, fw.atf + 4, (size_t)(fw.atf_n - 4) * 4); fw.atf_n -= 4;
    const uint32_t ack = (w0 & 0x00030000u) | 1;
    if (w0 == 0x0000AC00u && (w1 & 0xFFFF) == 0x0A00) { arf_push(0xFFC0AC00u); arf_push(0xFFC10A00u); arf_push(0); arf_push(0x01000100u); arf_push(ack); }
    else if (w0 == 0x0000BC00u && (w1 & 0xFFFF) == 0x0B00) {
        arf_push(0xFFC0BC00u); arf_push(0xFFFF0B00u); arf_push(0); arf_push(0x01010000u); arf_push(ack);
        arf_push(0xFFC1BC00u); arf_push(0xFFFF0B00u); arf_push(0); arf_push(0x00000100u); arf_push(ack);
    } else if (w0 == 0x00008C00u && (w1 & 0xFFFF) == 0x0800) { arf_push(0xFFC09C00u); arf_push(0xFFC10900u); arf_push(0); arf_push(0); arf_push(ack); }
    else { arf_push(0xFFC00000u | (w0 & 0xFCFF)); arf_push(0xFFC10000u | (w1 & 0xFFFF)); arf_push(w2); arf_push(w3); arf_push(ack); }
    fw.diag = (fw.diag & ~0xF00u) | 0x100u;
    fw.intr |= 1u << 20 | 1u << 23;
}
static void fw_cycle_tick(void)                   /* 16 kHz: two phases a 125 us cycle */
{
    fw.async_phase ^= 1;
    if (!fw.async_phase) {
        const uint32_t old = fw.cycle; fw.cycle += 0x1000;
        if ((fw.cycle ^ old) & 0xFE000000u) fw.intr |= 1u << 7;
        if ((fw.ctrl >> 17 & 1) && (fw.ctrl >> 4 & 1)) { fw.intr |= 1u << 6; fw.cycle_started = 1; }
        else if (fw.self_id_done && gw_link_nodes() > 0) fw.cycle_started = 1;   /* LINKED, not the root: the root's cycle starts reach us */
        if (fw.iso_going) {                                   /* one packet a cycle: header (length in bits 16-31) + payload */
            while (fw.itf_n > 0) {
                const int len = (int)(((fw.itf[0] >> 16) + 3) / 4 + 1);
                if (fw.itf_n < len) break;
                { extern uint32_t rr_frame; static unsigned n; static uint32_t lf; if (fwlog() && (n++ < 40 || rr_frame / 600 != lf)) { lf = rr_frame / 600;
                    fprintf(stderr, "[FW] f%u iso tx #%u len %d:", rr_frame, n, len); for (int i = 0; i < len && i < 10; i++) fprintf(stderr, " %08X", fw.itf[i]); fprintf(stderr, "\n"); } }
                if (gw_link_nodes() > 0) {                    /* LINKED: to every other cabinet, with the status word */
                    uint32_t q[GW_PKT_MAXQ]; const int m = len < GW_PKT_MAXQ - 1 ? len : GW_PKT_MAXQ - 1;
                    memcpy(q, fw.itf, (size_t)m * 4); q[m] = 1;
                    gw_link_send(GW_PKT_ISO, 0xFFFF, q, m + 1, fw.now);
                }
                memmove(fw.itf, fw.itf + len, (size_t)(fw.itf_n - len) * 4); fw.itf_n -= len;
                fw.intr |= 1u << 19;                          /* ITX_END */
                fw.diag &= ~(1u << 25); fw.iso_going = 0;
            }
            if (!fw.itf_n) { fw.tgo &= ~2u; fw.buf |= 0x10u; }
        }
    } else {
        if (fw.atf_n > 0) {
            if ((gw_link_nodes() > 0 || ((fw.atf[0] >> 4) & 15) == 0) && fw.atf_n >= 4)   /* linked: any packet (block writes: the game data) */ { if (!fw.cycle_started) { fw.intr |= 1u << 6; fw.cycle_started = 1; } fw_async_tx(); }
            fw.diag &= ~(1u << 24);
            if (!fw.atf_n) { fw.tgo &= ~1u; fw.buf |= 1u; }   /* drained: the async transmit FIFO reads empty again (0x50 bit 0) */
        }
        if (fw.cycle_started) fw.intr |= 1u << 5;
    }
}
static uint32_t fw_read(uint32_t o)
{
    switch (o) {
    case 0x00: return 0x00010002; case 0x04: return fw.ctrl; case 0x08: return fw.node_id; case 0x0C: return 0;
    case 0x10: return fw.async_bs; case 0x14: return fw.sync_bs; case 0x18: return fw.packet_ctrl; case 0x1C: return fw.diag;
    case 0x20: { const uint32_t v = fw.phy_ctrl; fw.phy_ctrl &= ~(1u << 14); return v; }
    case 0x24: return fw.at_rc; case 0x28: return fw.cycle; case 0x2C: return fw.sync_len;
    case 0x30: case 0x34: case 0x38: case 0x3C: return fw.sync_cfg[(o - 0x30) >> 2];
    case 0x44: return arf_pop();
    case 0x4C: return irf_pop();
    case 0x50: return fw.buf; case 0x54: return fw.intr; case 0x58: return fw.imask; case 0x5C: return fw.tgo;
    case 0x60: return fw.bus_time; case 0x68: return fw.at_r;
    }
    return 0;
}
/* a packet from the bus (src/gw_link.c), at its due time: kept if it is for this node (or for all), as the chip does */
void gw_fw_deliver(int kind, int src, const uint32_t *q, int n)
{
    extern uint32_t rr_frame;
    if (kind == GW_PKT_ISO) {
        for (int i = 0; i < n; i++) irf_push(q[i]);
        fw.intr |= 1u << 22;
        { static unsigned c; static uint32_t lf; if (fwlog() && (c++ < 40 || rr_frame / 600 != lf)) { lf = rr_frame / 600;
            fprintf(stderr, "[FW] f%u iso rx #%u from %d:", rr_frame, c, src); for (int i = 0; i < n && i < 8; i++) fprintf(stderr, " %08X", q[i]); fprintf(stderr, "\n"); } }
        return;
    }
    const uint32_t dst = q[0] >> 16;
    if ((dst & 63) != 63 && (int)(dst & 63) != gw_link_node()) return;   /* for another node */
    for (int i = 0; i < n; i++) arf_push(q[i]);
    fw.intr |= 1u << 20;
    { static unsigned c; if (fwlog() && c++ < 400) { fprintf(stderr, "[FW] f%u async rx from %d, %d quadlets:", rr_frame, src, n); for (int i = 0; i < n && i < 8; i++) fprintf(stderr, " %08X", q[i]); fprintf(stderr, "\n"); } }
}
#define CMB(r, m) (r) = ((r) & ~(m)) | (v & (m))
static void fw_write(uint32_t o, uint32_t v)
{
    switch (o) {
    case 0x04: CMB(fw.ctrl, 0x31770093u); break; case 0x08: CMB(fw.node_id, 0x8000FFFFu); break;
    case 0x10: CMB(fw.async_bs, 0x01FF01FFu); break; case 0x14: CMB(fw.sync_bs, 0x01FF01FFu); break; case 0x18: CMB(fw.packet_ctrl, 0x00001776u); break;
    case 0x20: {
        fw.phy_ctrl = (fw.phy_ctrl & ~0xFFFu) | (v & 0xFFFu);
        const int addr = (int)(fw.phy_ctrl >> 8 & 15);
        if (v >> 12 & 1) {
            { extern uint32_t rr_frame; if (fwlog()) fprintf(stderr, "[FW] f%u PHY write reg %d = %02X\n", rr_frame, addr, v & 0xFF); }
            if (addr == 1) { fw.phy[1] = (uint8_t)v; if (fw.phy[1] & 0x40) { fw.intr |= 1u << 14 | 1u << 13; if (fw.packet_ctrl >> 5 & 1) fw_self_id(); } }
            else if (addr == 6) fw.phy[6] = (uint8_t)(v & ~0x20u);
        } else if (v >> 13 & 1) {
            fw.phy_ctrl = (fw.phy_ctrl & ~0xFFu) | (addr < 7 ? fw.phy[addr] : 0) | 1u << 14;
            fw.intr |= 1u << 12;
        }
        break; }
    case 0x24: CMB(fw.at_rc, 0x1FFu); break; case 0x28: fw.cycle = v; break; case 0x2C: CMB(fw.sync_len, 0x0FFF0000u); break;
    case 0x30: case 0x34: case 0x38: case 0x3C: CMB(fw.sync_cfg[(o - 0x30) >> 2], 0xFF03FFF3u); break;
    case 0x40: if (fw.atf_n < 512) fw.atf[fw.atf_n++] = v; fw.buf &= ~1u; break;
    case 0x48: if (fw.itf_n < 512) fw.itf[fw.itf_n++] = v; fw.buf &= ~0x10u; break;
    case 0x50: CMB(fw.buf, 0x01FF7077u); break;
    case 0x54: fw.intr &= ~(v & 0x01FFFFF8u); break;
    case 0x58: CMB(fw.imask, 0x01FFFFF8u); break;
    case 0x5C: CMB(fw.tgo, 7u); if (!(fw.diag >> 24 & 1) && (fw.tgo & 1)) fw.diag |= 1u << 24;
        if (!(fw.diag >> 25 & 1) && (fw.tgo & 2) && !(fw.ctrl >> 20 & 7)) { fw.diag |= 1u << 25; fw.iso_going = 1; } break;
    case 0x60: CMB(fw.bus_time, 0xFFFFFF80u); break; case 0x68: CMB(fw.at_r, 0xFFFF0000u); break;
    }
}
static uint32_t vpxstate;
static int vpx_sdao = 1;
/* THE VPX-3220A's I2C side (MAME vpx_i2c_w: bit 0 = SDA, bit 1 = SCL; the read = the chip's SDA out). Ours: a plain I2C slave at
 * address 0x86 (write) / 0x87 (read) with a 256-byte register file; byte reads return its registers (the part ID, Micronas JEDEC 0xEC
 * and 0x4680 for the 3220A, at 0x00..0x02, as its data sheet gives -- unchecked). Acknowledges every byte addressed to it. */
static uint8_t vpx_reg[256] = { 0xEC, 0x46, 0x80 };
static struct { int scl, sda, bit, active, send, nbyte; uint8_t sh, sub; } i2c = { 1, 1 };
static void vpx_i2c_write(uint8_t v)
{
    const int sda = v & 1, scl = (v >> 1) & 1;
    if (i2c.scl && scl && i2c.sda != sda) {                                       /* SDA moving while SCL is high: START / STOP */
        if (!sda) { i2c.active = 1; i2c.bit = 0; i2c.nbyte = 0; i2c.send = 0; i2c.sh = 0; }
        else i2c.active = 0;
        vpx_sdao = 1;
    } else if (i2c.active && !i2c.scl && scl) {                                   /* SCL rising: the bit is sampled */
        if (i2c.bit < 8) { if (!i2c.send) i2c.sh = (uint8_t)(i2c.sh << 1 | sda); i2c.bit++; }
        else { if (i2c.send && sda) i2c.active = 0; i2c.bit = 0; }                 /* the 9th clock: a master NACK ends a read */
    } else if (i2c.active && i2c.scl && !scl) {                                   /* SCL falling: the slave sets SDA for the next bit */
        if (i2c.bit == 8) {
            { static int lg = -1, nl; if (lg < 0) lg = getenv("GW_I2CLOG") != NULL; if (lg && nl++ < 300) fprintf(stderr, "[I2C] byte %d %s %02X\n", i2c.nbyte, i2c.send ? "sent" : "got", i2c.sh); }
            if (!i2c.send) {                                                      /* a byte received: ack it (low) */
                if (i2c.nbyte == 0) { if ((i2c.sh & 0xFE) != 0x86) { i2c.active = 0; vpx_sdao = 1; goto out; } }
                else if (i2c.nbyte == 1) i2c.sub = i2c.sh;
                else vpx_reg[i2c.sub++] = i2c.sh;
                vpx_sdao = 0;
            } else vpx_sdao = 1;                                                  /* the master acks our byte */
            i2c.nbyte++;
        } else if (i2c.bit == 0) {                                                /* after the ack clock */
            if (i2c.nbyte == 1 && (i2c.sh & 1)) i2c.send = 1;                      /* the address said read */
            /* 0x26-0x29 = the VPX's FP (its internal processor) interface: FPRD, FPWR, FPDAT, FPSTA; FPSTA reads 0 = idle (no camera here) */
            if (i2c.send) { i2c.sh = i2c.sub == 0x29 ? 0x00 : vpx_reg[i2c.sub]; i2c.sub++; vpx_sdao = i2c.sh >> 7; } else { vpx_sdao = 1; i2c.sh = 0; }
        } else if (i2c.send) vpx_sdao = (i2c.sh >> (7 - i2c.bit)) & 1;
    }
out:
    i2c.scl = scl; i2c.sda = sda;
}

typedef struct { uint32_t r[16], pc, pr, gbr, vbr, mach, macl, sr; int running; uint64_t cycles; int irq_level; } sh2_t;
static sh2_t S;
#define T_BIT 0x001u
#define S_BIT 0x002u
#define Q_BIT 0x100u
#define M_BIT 0x200u

static uint32_t rd_be(const uint8_t *p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
static void wr_be(uint8_t *p, int n, uint32_t v) { for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }

/* ---- the SH-2's memory map (MAME namcoss23_gmen_state::sh2_map) ---- */
static int cmd_irq;
int gw_vpx_field;               /* the camera fields counted (odd / even in vpxstate) */
static int vref_irq, vpx_on;   /* the camera field IRQ4; vpx_on: the SH-2 main program runs (fields on) */
static uint32_t mem_r(uint32_t a, int n)
{
    { const uint32_t m = (a & 0x1FFFFFFFu) - 0x04000000u;              /* FAST PATH: the work SDRAM (most reads, instruction fetches) */
      if (m < 0x400000u && a < 0xFFFFFE00u && (a >> 29) != 6) {
          const uint8_t *q = ram40 + m;
          return n == 2 ? (uint32_t)q[0] << 8 | q[1] : n == 4 ? (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3] : q[0]; } }
    if (a >= 0xFFFFFE00u) return rd_be(onchip + (a & 0x1FF), n);
    if ((a >> 29) == 6) return rd_be(cache_data + (a & 0xFFF), n);
    a &= 0x1FFFFFFFu;                                                 /* cache-through / cached mirrors */
    if ((a & ~0x0100FFFFu) == 0) return rd_be(gw_gmen_ram + (a & 0xFFFF), n);
    if (a >= 0x01800000u && a < 0x01C00000u) return rd_be(ram18 + (a - 0x01800000u), n);
    if (a >= 0x04000000u && a < 0x04400000u) return rd_be(ram40 + (a - 0x04000000u), n);
    if (a >= 0x00C00000u && a < 0x00C00080u) { const uint32_t w = fw_read(a & 0x7C); const int sh = (int)(4 - n - (a & 3)) * 8;
        return n == 4 ? w : (w >> sh) & ((1u << (8 * n)) - 1); }
    uint32_t w = 0;
    if (a >= 0x01400000u && a < 0x01400400u) w = 0x7FFF0000u;           /* vpx_line_r */
    else if ((a & ~3u) == 0x02800000u) { extern int gw_vpx_field; w = (vpxstate & ~0x01000100u) | 0x01000000u | (uint32_t)(gw_vpx_field & 1) << 8; }   /* MAME (commented out): 0x01000000 | odd field << 8 */
    else if ((a & ~3u) == 0x03000000u) w = 0xFFu << 24;               /* GMENDSW: all off */
    else if ((a & ~3u) == 0x06000000u) w = (uint32_t)vpx_sdao << 24;  /* vpx i2c */
    else if ((a & ~3u) == 0x06200000u) w = 0x04000000u;
    else { static int n_; if (n_++ < 20) fprintf(stderr, "[GMEN] SH-2 read %08X (%d) pc %08X: unmapped\n", a, n, S.pc); return 0; }
    const int sh = (int)(4 - n - (a & 3)) * 8;
    return (w >> sh) & (n == 4 ? 0xFFFFFFFFu : (1u << (8 * n)) - 1);
}
static void divu(void);
static void dma_try(int ch);
static void mem_w(uint32_t a, int n, uint32_t v)
{
    if (a >= 0xFFFFFE00u) { wr_be(onchip + (a & 0x1FF), n, v); if ((a & 0x1FF) == 0x104 || (a & 0x1FF) == 0x114) divu();
        if ((a & 0x1FC) == 0x18C) dma_try(0); else if ((a & 0x1FC) == 0x19C) dma_try(1); else if ((a & 0x1FC) == 0x1B0) { dma_try(0); dma_try(1); }
        return; }
    if ((a >> 29) == 6) { wr_be(cache_data + (a & 0xFFF), n, v); return; }
    a &= 0x1FFFFFFFu;
    if ((a & ~0x0100FFFFu) == 0) {
        wr_be(gw_gmen_ram + (a & 0xFFFF), n, v);
        { void gw_shwatch(int who, uint32_t off, int n, uint32_t v); gw_shwatch(1, a & 0xFFFF, n, v); }
        if ((a & 0xFFFC) == 0x20 && (rd_be(gw_gmen_ram + 0x20, 2) == 0)) cmd_irq = 0;   /* the SH-2 answered: IRQ6 drops */
        return;
    }
    if (a >= 0x01800000u && a < 0x01C00000u) { wr_be(ram18 + (a - 0x01800000u), n, v); return; }
    if (a >= 0x04000000u && a < 0x04400000u) { wr_be(ram40 + (a - 0x04000000u), n, v);
        { static int nw = -1; static uint32_t wa[8];   /* $GW_SH2WWATCH=addr,...: each write to those longwords, with the writer's PC */
          if (nw < 0) { nw = 0; const char *e = getenv("GW_SH2WWATCH"); while (e && *e && nw < 8) { wa[nw++] = (uint32_t)strtoul(e, (char **)&e, 16) & 0x1FFFFFFCu; if (*e == ',') e++; else break; } }
          for (int i = 0; i < nw; i++) if ((a & ~3u) == wa[i]) { extern uint32_t rr_frame; fprintf(stderr, "[SH2WW] f%u %08X <- %08X (%d) pc %08X pr %08X\n", rr_frame, a, v, n, S.pc, S.pr); } }
        return; }
    if (a >= 0x00C00000u && a < 0x00C00080u) {
        if (n != 4) { const int sh = (int)(4 - n - (a & 3)) * 8; const uint32_t mk = (n == 2 ? 0xFFFFu : 0xFFu) << sh; v = (fw_read(a & 0x7C) & ~mk) | ((v << sh) & mk); }
        fw_write(a & 0x7C, v); return; }
    if ((a & ~3u) == 0x02800000u) { vpxstate = v; return; }
    if ((a & ~3u) == 0x06000000u) { vpx_i2c_write((uint8_t)(n == 4 ? v >> 24 : n == 2 ? v >> 8 : v)); return; }   /* (umask ff000000) */
    /* 0x06A00000: the GMEN board's INTERRUPT ACKNOWLEDGE (ours, from the SH-2 program: each handler writes a byte with its own bit clear --
     * level 6 0xFB, level 4 0xFD, level 2 0xFE; MAME ignores the write and clears the command only by a write to shared +0x20) */
    if ((a & ~3u) == 0x06A00000u) { const uint8_t b = (uint8_t)(n == 4 ? v >> 24 : n == 2 ? v >> 8 : v); if (!(b & 4)) cmd_irq = 0; if (!(b & 2)) vref_irq = 0; return; }
    if ((a & ~3u) == 0x06200000u || (a & ~3u) == 0x06600000u) return;
    if ((a & ~3u) == 0x03800000u) return;                             /* the board's LEDs 5-12 */
    if ((a & 0x1FFF0000u) == 0x1FFF0000u) return;                     /* SDRAM mode setting (the bus controller's write-only area) */
    static int n_; if (n_++ < 20) fprintf(stderr, "[GMEN] SH-2 write %08X (%d) = %08X pc %08X: unmapped\n", a, n, v, S.pc);
}
/* THE ON-CHIP DMA CONTROLLER (SH7604: SAR 0x180, DAR 0x184, TCR 0x188, CHCR 0x18C for channel 0, +0x10 for channel 1, DMAOR 0x1B0), the
 * auto-request transfers only, done at once: the board's program reads a received block write's data out of the link chip's FIFO with
 * channel 0 (0x04016020: source fixed at 0x20C00044, destination counting up, longwords) and waits for TE */
static void dma_try(int ch)
{
    uint8_t *r = onchip + 0x180 + ch * 0x10;
    const uint32_t chcr = rd_be(r + 12, 4), dmaor = rd_be(onchip + 0x1B0, 4);
    if (!(chcr & 1) || (chcr & 2) || !(dmaor & 1) || (dmaor & 6)) return;   /* DE, not TE yet; DME, no NMI / address error */
    if (!(chcr >> 9 & 1)) { static int said; if (!said++) fprintf(stderr, "[GMEN] DMA %d by external request (CHCR %08X): not modelled\n", ch, chcr); return; }
    uint32_t sar = rd_be(r, 4), dar = rd_be(r + 4, 4), tcr = rd_be(r + 8, 4) & 0xFFFFFFu;
    if (!tcr) tcr = 0x1000000u;
    const int ts = (int)(chcr >> 10 & 3), size = ts == 0 ? 1 : ts == 1 ? 2 : 4, sm = (int)(chcr >> 12 & 3), dm = (int)(chcr >> 14 & 3);
    uint32_t units = ts == 3 ? (tcr + 3) / 4 * 4 : tcr;     /* 16-byte units: four longwords each */
    if (units > 0x100000u) units = 0x100000u;
    for (uint32_t i = 0; i < units; i++) {
        mem_w(dar, size, mem_r(sar, size));
        if (sm == 1) sar += (uint32_t)size; else if (sm == 2) sar -= (uint32_t)size;
        if (dm == 1) dar += (uint32_t)size; else if (dm == 2) dar -= (uint32_t)size;
    }
    wr_be(r, 4, sar); wr_be(r + 4, 4, dar); wr_be(r + 8, 4, 0); wr_be(r + 12, 4, chcr | 2);
    if (chcr & 4) { static int said; if (!said++) fprintf(stderr, "[GMEN] DMA %d end interrupt asked (CHCR %08X): not modelled\n", ch, chcr); }
    { static int n_; if (fwlog() && n_++ < 50) fprintf(stderr, "[GMEN] DMA %d: %u x %d bytes (CHCR %08X)\n", ch, units, size, chcr); }
}
/* the on-chip divider (DIVU): DVSR 0x100, DVDNT 0x104 (32 / 32), DVDNTH 0x110 + DVDNTL 0x114 (64 / 32); results DVDNTL 0x114 (quotient)
 * and DVDNTH 0x110 (remainder), mirrored at 0x11C / 0x118 */
static void divu(void)
{
    const int32_t d = (int32_t)rd_be(onchip + 0x100, 4);
    const int64_t n = (int64_t)(int32_t)rd_be(onchip + 0x104, 4);
    if (!d) return;
    const int64_t q = n / d, r = n % d;
    wr_be(onchip + 0x114, 4, (uint32_t)q); wr_be(onchip + 0x110, 4, (uint32_t)r);
    wr_be(onchip + 0x11C, 4, (uint32_t)q); wr_be(onchip + 0x118, 4, (uint32_t)r);
}

#define RB(a) ((uint8_t)mem_r((a), 1))
#define RW(a) ((uint16_t)mem_r((a), 2))
#define RL(a) (mem_r((a), 4))
#define WB(a, v) mem_w((a), 1, (uint8_t)(v))
#define WW(a, v) mem_w((a), 2, (uint16_t)(v))
#define WL(a, v) mem_w((a), 4, (uint32_t)(v))

static void exec(uint16_t op);
static int in_delay;
static void delay_slot(uint32_t target)        /* a delayed branch: the next instruction runs, then the jump */
{
    const uint32_t slot = S.pc;
    in_delay = 1; S.pc = slot + 2; exec(RW(slot)); in_delay = 0;
    S.pc = target;
}
static void take_irq(int vector, int level)
{
    S.r[15] -= 4; WL(S.r[15], S.sr);
    S.r[15] -= 4; WL(S.r[15], S.pc);
    S.sr = (S.sr & ~0xF0u) | ((uint32_t)level << 4);
    S.pc = RL(S.vbr + (uint32_t)vector * 4);
}
static void illegal(uint16_t op)
{
    static int n_; if (n_++ < 10) fprintf(stderr, "[GMEN] SH-2 illegal instruction %04X at %08X\n", op, S.pc - 2);
    take_irq(4, (int)((S.sr >> 4) & 15));
}

static void exec(uint16_t op)
{
    uint32_t *R = S.r;
    const int n = (op >> 8) & 15, m = (op >> 4) & 15;
    const uint32_t i8 = op & 0xFF, d4 = op & 15;
    const int32_t s8 = (int8_t)(op & 0xFF), s12 = ((int32_t)(op & 0xFFF) << 20) >> 20;
    switch (op >> 12) {
    case 0x0:
        switch (op & 0xF) {
        case 0x2: switch (m) { case 0: R[n] = S.sr; return; case 1: R[n] = S.gbr; return; case 2: R[n] = S.vbr; return; } break;   /* stc */
        case 0x3: switch (m) { case 0: { const uint32_t t = S.pc + 2 + R[n]; S.pr = S.pc + 2; delay_slot(t); return; }        /* bsrf */
                               case 2: { const uint32_t t = S.pc + 2 + R[n]; delay_slot(t); return; } } break;                 /* braf */
        case 0x4: WB(R[n] + R[0], R[m]); return;
        case 0x5: WW(R[n] + R[0], R[m]); return;
        case 0x6: WL(R[n] + R[0], R[m]); return;
        case 0x7: S.macl = R[n] * R[m]; return;                                                         /* mul.l */
        case 0x8: switch (op) { case 0x0008: S.sr &= ~T_BIT; return; case 0x0018: S.sr |= T_BIT; return; case 0x0028: S.mach = S.macl = 0; return; } break;
        case 0x9: switch (m) { case 0: return;                                                          /* nop */
                               case 1: S.sr &= ~(M_BIT | Q_BIT | T_BIT); return;                        /* div0u */
                               case 2: R[n] = S.sr & T_BIT; return; } break;                            /* movt */
        case 0xA: switch (m) { case 0: R[n] = S.mach; return; case 1: R[n] = S.macl; return; case 2: R[n] = S.pr; return; } break;   /* sts */
        case 0xB: switch (op) {
                  case 0x000B: delay_slot(S.pr); return;                                                /* rts */
                  case 0x001B: S.pc -= 2; return;                                                       /* sleep: wait for an interrupt */
                  case 0x002B: { const uint32_t pc = RL(R[15]); R[15] += 4; S.sr = RL(R[15]) & 0x3F3; R[15] += 4; delay_slot(pc); return; } }   /* rte */
                  break;
        case 0xC: R[n] = (uint32_t)(int8_t)RB(R[m] + R[0]); return;
        case 0xD: R[n] = (uint32_t)(int16_t)RW(R[m] + R[0]); return;
        case 0xE: R[n] = RL(R[m] + R[0]); return;
        case 0xF: {                                                                                     /* mac.l @Rm+,@Rn+ */
            const int64_t a = (int32_t)RL(R[n]); R[n] += 4; const int64_t b = (int32_t)RL(R[m]); R[m] += 4;
            int64_t acc = (int64_t)((uint64_t)S.mach << 32 | S.macl) + a * b;
            if (S.sr & S_BIT) { if (acc > 0x00007FFFFFFFFFFFLL) acc = 0x00007FFFFFFFFFFFLL; if (acc < -0x0000800000000000LL) acc = -0x0000800000000000LL; }
            S.mach = (uint32_t)((uint64_t)acc >> 32); S.macl = (uint32_t)acc; return; }
        }
        break;
    case 0x1: WL(R[n] + d4 * 4, R[m]); return;
    case 0x2:
        switch (op & 0xF) {
        case 0x0: WB(R[n], R[m]); return;
        case 0x1: WW(R[n], R[m]); return;
        case 0x2: WL(R[n], R[m]); return;
        case 0x4: R[n] -= 1; WB(R[n], R[m]); return;
        case 0x5: R[n] -= 2; WW(R[n], R[m]); return;
        case 0x6: R[n] -= 4; WL(R[n], R[m]); return;
        case 0x7: { const uint32_t q = R[n] >> 31, mm = R[m] >> 31; S.sr = (S.sr & ~(Q_BIT | M_BIT | T_BIT)) | q << 8 | mm << 9 | (q ^ mm); return; }   /* div0s */
        case 0x8: S.sr = (S.sr & ~T_BIT) | ((R[n] & R[m]) == 0); return;                                /* tst */
        case 0x9: R[n] &= R[m]; return;
        case 0xA: R[n] ^= R[m]; return;
        case 0xB: R[n] |= R[m]; return;
        case 0xC: { const uint32_t t = R[n] ^ R[m]; S.sr = (S.sr & ~T_BIT) | (!(t & 0xFF000000u) || !(t & 0xFF0000u) || !(t & 0xFF00u) || !(t & 0xFFu)); return; }   /* cmp/str */
        case 0xD: R[n] = (R[m] << 16) | (R[n] >> 16); return;                                           /* xtrct */
        case 0xE: S.macl = (uint32_t)((uint16_t)R[n] * (uint16_t)R[m]); return;                         /* mulu.w */
        case 0xF: S.macl = (uint32_t)((int16_t)R[n] * (int16_t)R[m]); return;                           /* muls.w */
        }
        break;
    case 0x3:
        switch (op & 0xF) {
        case 0x0: S.sr = (S.sr & ~T_BIT) | (R[n] == R[m]); return;
        case 0x2: S.sr = (S.sr & ~T_BIT) | (R[n] >= R[m]); return;
        case 0x3: S.sr = (S.sr & ~T_BIT) | ((int32_t)R[n] >= (int32_t)R[m]); return;
        case 0x4: {                                                                                     /* div1 (the manual's algorithm) */
            const uint32_t old_q = (S.sr >> 8) & 1, mq = (S.sr >> 9) & 1; uint32_t q = (R[n] >> 31) & 1;
            R[n] = (R[n] << 1) | (S.sr & T_BIT);
            uint32_t tmp0 = R[n], tmp1;
            if (!old_q) {
                if (!mq) { R[n] -= R[m]; tmp1 = R[n] > tmp0; q = q ? (tmp1 == 0) : tmp1; }
                else     { R[n] += R[m]; tmp1 = R[n] < tmp0; q = q ? tmp1 : (tmp1 == 0); }
            } else {
                if (!mq) { R[n] += R[m]; tmp1 = R[n] < tmp0; q = q ? (tmp1 == 0) : tmp1; }
                else     { R[n] -= R[m]; tmp1 = R[n] > tmp0; q = q ? tmp1 : (tmp1 == 0); }
            }
            S.sr = (S.sr & ~(Q_BIT | T_BIT)) | q << 8 | (q == mq);
            return; }
        case 0x5: { const uint64_t p = (uint64_t)R[n] * R[m]; S.mach = (uint32_t)(p >> 32); S.macl = (uint32_t)p; return; }   /* dmulu.l */
        case 0x6: S.sr = (S.sr & ~T_BIT) | (R[n] > R[m]); return;
        case 0x7: S.sr = (S.sr & ~T_BIT) | ((int32_t)R[n] > (int32_t)R[m]); return;
        case 0x8: R[n] -= R[m]; return;
        case 0xA: { const uint32_t a = R[n], b = R[m], t = S.sr & T_BIT, r = a - b - t; S.sr = (S.sr & ~T_BIT) | (a < b || (a - b) < t); R[n] = r; return; }   /* subc */
        case 0xB: { const int32_t a = (int32_t)R[n], b = (int32_t)R[m]; const int32_t r = (int32_t)((uint32_t)a - (uint32_t)b); S.sr = (S.sr & ~T_BIT) | (((a ^ b) & (a ^ r)) >> 31 & 1); R[n] = (uint32_t)r; return; }   /* subv */
        case 0xC: R[n] += R[m]; return;
        case 0xD: { const int64_t p = (int64_t)(int32_t)R[n] * (int32_t)R[m]; S.mach = (uint32_t)((uint64_t)p >> 32); S.macl = (uint32_t)p; return; }   /* dmuls.l */
        case 0xE: { const uint32_t a = R[n], b = R[m], t = S.sr & T_BIT, s = a + b, r = s + t; S.sr = (S.sr & ~T_BIT) | (s < a || r < s); R[n] = r; return; }   /* addc */
        case 0xF: { const int32_t a = (int32_t)R[n], b = (int32_t)R[m]; const int32_t r = (int32_t)((uint32_t)a + (uint32_t)b); S.sr = (S.sr & ~T_BIT) | ((~(a ^ b) & (a ^ r)) >> 31 & 1); R[n] = (uint32_t)r; return; }   /* addv */
        }
        break;
    case 0x4:
        switch (op & 0xFF) {
        case 0x00: S.sr = (S.sr & ~T_BIT) | (R[n] >> 31); R[n] <<= 1; return;                          /* shll */
        case 0x01: S.sr = (S.sr & ~T_BIT) | (R[n] & 1); R[n] >>= 1; return;                            /* shlr */
        case 0x02: R[n] -= 4; WL(R[n], S.mach); return;
        case 0x03: R[n] -= 4; WL(R[n], S.sr); return;
        case 0x04: { const uint32_t t = R[n] >> 31; R[n] = (R[n] << 1) | t; S.sr = (S.sr & ~T_BIT) | t; return; }   /* rotl */
        case 0x05: { const uint32_t t = R[n] & 1; R[n] = (R[n] >> 1) | (t << 31); S.sr = (S.sr & ~T_BIT) | t; return; }   /* rotr */
        case 0x06: S.mach = RL(R[n]); R[n] += 4; return;
        case 0x07: S.sr = RL(R[n]) & 0x3F3; R[n] += 4; return;
        case 0x08: R[n] <<= 2; return;
        case 0x09: R[n] >>= 2; return;
        case 0x0A: S.mach = R[n]; return;
        case 0x0B: { const uint32_t t = R[n]; S.pr = S.pc + 2; delay_slot(t); return; }               /* jsr */
        case 0x0E: S.sr = R[n] & 0x3F3; return;
        case 0x10: R[n] -= 1; S.sr = (S.sr & ~T_BIT) | (R[n] == 0); return;                            /* dt */
        case 0x11: S.sr = (S.sr & ~T_BIT) | ((int32_t)R[n] >= 0); return;                              /* cmp/pz */
        case 0x12: R[n] -= 4; WL(R[n], S.macl); return;
        case 0x13: R[n] -= 4; WL(R[n], S.gbr); return;
        case 0x15: S.sr = (S.sr & ~T_BIT) | ((int32_t)R[n] > 0); return;                               /* cmp/pl */
        case 0x16: S.macl = RL(R[n]); R[n] += 4; return;
        case 0x17: S.gbr = RL(R[n]); R[n] += 4; return;
        case 0x18: R[n] <<= 8; return;
        case 0x19: R[n] >>= 8; return;
        case 0x1A: S.macl = R[n]; return;
        case 0x1B: { const uint8_t v = RB(R[n]); S.sr = (S.sr & ~T_BIT) | (v == 0); WB(R[n], v | 0x80); return; }   /* tas.b */
        case 0x1E: S.gbr = R[n]; return;
        case 0x20: S.sr = (S.sr & ~T_BIT) | (R[n] >> 31); R[n] <<= 1; return;                          /* shal */
        case 0x21: S.sr = (S.sr & ~T_BIT) | (R[n] & 1); R[n] = (uint32_t)((int32_t)R[n] >> 1); return; /* shar */
        case 0x22: R[n] -= 4; WL(R[n], S.pr); return;
        case 0x23: R[n] -= 4; WL(R[n], S.vbr); return;
        case 0x24: { const uint32_t t = R[n] >> 31; R[n] = (R[n] << 1) | (S.sr & T_BIT); S.sr = (S.sr & ~T_BIT) | t; return; }   /* rotcl */
        case 0x25: { const uint32_t t = R[n] & 1; R[n] = (R[n] >> 1) | ((S.sr & T_BIT) << 31); S.sr = (S.sr & ~T_BIT) | t; return; }   /* rotcr */
        case 0x26: S.pr = RL(R[n]); R[n] += 4; return;
        case 0x27: S.vbr = RL(R[n]); R[n] += 4; return;
        case 0x28: R[n] <<= 16; return;
        case 0x29: R[n] >>= 16; return;
        case 0x2A: S.pr = R[n]; return;
        case 0x2B: delay_slot(R[n]); return;                                                            /* jmp */
        case 0x2E: S.vbr = R[n]; return;
        }
        if ((op & 0xF) == 0xF) {                                                                        /* mac.w @Rm+,@Rn+ */
            const int32_t a = (int16_t)RW(R[n]); R[n] += 2; const int32_t b = (int16_t)RW(R[m]); R[m] += 2;
            if (S.sr & S_BIT) { int64_t acc = (int64_t)(int32_t)S.macl + (int64_t)a * b; if (acc > 0x7FFFFFFF) { acc = 0x7FFFFFFF; S.mach |= 1; } else if (acc < -0x80000000LL) { acc = -0x80000000LL; S.mach |= 1; } S.macl = (uint32_t)acc; }
            else { const int64_t acc = (int64_t)((uint64_t)S.mach << 32 | S.macl) + (int64_t)a * b; S.mach = (uint32_t)((uint64_t)acc >> 32); S.macl = (uint32_t)acc; }
            return;
        }
        break;
    case 0x5: R[n] = RL(R[m] + d4 * 4); return;
    case 0x6:
        switch (op & 0xF) {
        case 0x0: R[n] = (uint32_t)(int8_t)RB(R[m]); return;
        case 0x1: R[n] = (uint32_t)(int16_t)RW(R[m]); return;
        case 0x2: R[n] = RL(R[m]); return;
        case 0x3: R[n] = R[m]; return;
        case 0x4: { const uint32_t v = (uint32_t)(int8_t)RB(R[m]); if (n != m) R[m] += 1; R[n] = v; return; }
        case 0x5: { const uint32_t v = (uint32_t)(int16_t)RW(R[m]); if (n != m) R[m] += 2; R[n] = v; return; }
        case 0x6: { const uint32_t v = RL(R[m]); if (n != m) R[m] += 4; R[n] = v; return; }
        case 0x7: R[n] = ~R[m]; return;
        case 0x8: R[n] = (R[m] & 0xFFFF0000u) | ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF); return;   /* swap.b */
        case 0x9: R[n] = (R[m] << 16) | (R[m] >> 16); return;                                           /* swap.w */
        case 0xA: { const uint32_t t = S.sr & T_BIT, r = 0 - R[m] - t; S.sr = (S.sr & ~T_BIT) | (R[m] != 0 || t); R[n] = r; return; }   /* negc */
        case 0xB: R[n] = 0 - R[m]; return;
        case 0xC: R[n] = R[m] & 0xFF; return;
        case 0xD: R[n] = R[m] & 0xFFFF; return;
        case 0xE: R[n] = (uint32_t)(int8_t)R[m]; return;
        case 0xF: R[n] = (uint32_t)(int16_t)R[m]; return;
        }
        break;
    case 0x7: R[n] += (uint32_t)s8; return;
    case 0x8:
        switch (n) {
        case 0x0: WB(R[m] + d4, R[0]); return;
        case 0x1: WW(R[m] + d4 * 2, R[0]); return;
        case 0x4: R[0] = (uint32_t)(int8_t)RB(R[m] + d4); return;
        case 0x5: R[0] = (uint32_t)(int16_t)RW(R[m] + d4 * 2); return;
        case 0x8: S.sr = (S.sr & ~T_BIT) | (R[0] == (uint32_t)s8); return;
        case 0x9: if (S.sr & T_BIT) S.pc = S.pc + 2 + (uint32_t)(s8 * 2); return;                                      /* bt */
        case 0xB: if (!(S.sr & T_BIT)) S.pc = S.pc + 2 + (uint32_t)(s8 * 2); return;                                   /* bf */
        case 0xD: if (S.sr & T_BIT) delay_slot(S.pc + 2 + (uint32_t)(s8 * 2)); return;                                 /* bt/s */
        case 0xF: if (!(S.sr & T_BIT)) delay_slot(S.pc + 2 + (uint32_t)(s8 * 2)); return;                              /* bf/s */
        }
        break;
    case 0x9: R[n] = (uint32_t)(int16_t)RW(S.pc + 2 + i8 * 2); return;                                 /* mov.w @(disp,PC) */
    case 0xA: delay_slot(S.pc + 2 + (uint32_t)(s12 * 2)); return;                                      /* bra */
    case 0xB: { const uint32_t t = S.pc + 2 + (uint32_t)(s12 * 2); S.pr = S.pc + 2; delay_slot(t); return; }   /* bsr */
    case 0xC:
        switch (n) {
        case 0x0: WB(S.gbr + i8, R[0]); return;
        case 0x1: WW(S.gbr + i8 * 2, R[0]); return;
        case 0x2: WL(S.gbr + i8 * 4, R[0]); return;
        case 0x3: { S.r[15] -= 4; WL(S.r[15], S.sr); S.r[15] -= 4; WL(S.r[15], S.pc); S.pc = RL(S.vbr + i8 * 4); return; }   /* trapa */
        case 0x4: R[0] = (uint32_t)(int8_t)RB(S.gbr + i8); return;
        case 0x5: R[0] = (uint32_t)(int16_t)RW(S.gbr + i8 * 2); return;
        case 0x6: R[0] = RL(S.gbr + i8 * 4); return;
        case 0x7: R[0] = ((S.pc + 2) & ~3u) + i8 * 4; return;                                            /* mova */
        case 0x8: S.sr = (S.sr & ~T_BIT) | ((R[0] & i8) == 0); return;
        case 0x9: R[0] &= i8; return;
        case 0xA: R[0] ^= i8; return;
        case 0xB: R[0] |= i8; return;
        case 0xC: S.sr = (S.sr & ~T_BIT) | ((RB(S.gbr + R[0]) & i8) == 0); return;
        case 0xD: WB(S.gbr + R[0], RB(S.gbr + R[0]) & i8); return;
        case 0xE: WB(S.gbr + R[0], RB(S.gbr + R[0]) ^ i8); return;
        case 0xF: WB(S.gbr + R[0], RB(S.gbr + R[0]) | i8); return;
        }
        break;
    case 0xD: R[n] = RL(((S.pc + 2) & ~3u) + i8 * 4); return;                                          /* mov.l @(disp,PC) */
    case 0xE: R[n] = (uint32_t)s8; return;
    }
    illegal(op);
}

/* ---- running it ---- */
static uint64_t main_seen;                        /* main cycles already turned into SH-2 time */
uint64_t gw_gmen_time(void) { return main_seen; } /* (the link: how far the board has run) */
static uint64_t sh2_budget_frac;
void gw_gmen_reset_release(void)
{
    if (S.running) return;
    memset(&S, 0, sizeof S);
    S.pc = rd_be(gw_gmen_ram + 0, 4); S.r[15] = rd_be(gw_gmen_ram + 4, 4); S.sr = 0xF0;
    S.running = 1; fw_reset();
    fprintf(stderr, "[GMEN] SH-2 out of reset: PC %08X SP %08X\n", S.pc, S.r[15]);
}
void gw_gmen_command_written(void) { cmd_irq = 1; }
/* (vref_irq, vpx_on: declared with cmd_irq above)   /* vpx_on: the VPX-3220 programmed over I2C (not modelled yet: off) */
void gw_gmen_vblank(int on) { vref_irq = on; }   /* the VPX's vertical reference -> IRQ4 (one per field; tied to the screen here) */
/* run the SH-2 up to the main CPU's time now: the SH-2 at 28.6 MHz against the main 169.344 MHz -> 169 instructions per 1000 main cycles
 * (an instruction a cycle; the SH-2's real timing is not modelled) */
int gw_gmen_irq6vb(void) { static int t = -1; if (t < 0) { const char *e = getenv("GW_IRQ6VB"); t = !(e && *e == '0'); } return t; }
void gw_gmen_run(uint64_t main_now)
{
    if (!S.running) { main_seen = main_now; return; }
    if (main_now <= main_seen) return;
    uint64_t want = (main_now - main_seen) * 169 + sh2_budget_frac; main_seen = main_now;
    sh2_budget_frac = want % 1000; want /= 1000;
    static uint64_t next_field = 2822400;                 /* the camera's fields: one a frame (169344000 / 60) */
    /* the camera fields (IRQ4): once the SH-2 runs its MAIN program (VBR moved to 0x04010000, whose vector 66 is a real handler; the boot
     * loader at VBR 0x04000000 treats a field interrupt as an error) */
    if (S.vbr >= 0x04010000u) vpx_on = 1;
    if (main_now >= next_field) { vref_irq = vpx_on; while (next_field <= main_now) { next_field += 2822400; gw_vpx_field++; }
        /* IRQ6 ONCE A FRAME while linked: the board's IRQ6 handler (0x04011920) is its per-frame link step -- it swaps the send / receive
         * buffers and sets the "frame ready" word the main program waits for each frame (shared +0xC040; main 0x80093D10, else LINK NG).
         * Its source on the real board is not known (the main board's frame, presumably). ONE CABINET gets it too (2026-11-09): without it
         * the main program polls +0xC040 ~65,000 times every frame until its wait times out (~10% of solo time); $GW_IRQ6VB=0 = MAME's
         * behaviour (no IRQ6 after boot) */
        if ((gw_gmen_irq6vb() || gw_link_nodes() > 0) && vpx_on) cmd_irq = 1; }
    static uint64_t fw_next;                          /* the link chip's 16 kHz tick: 10584 main cycles */
    if (!fw_next) fw_next = main_now;
    while (fw_next <= main_now) { fw.now = fw_next; if (gw_link_nodes() > 0) gw_link_deliver(fw_next); fw_cycle_tick(); fw_next += 10584; }
    fw.now = main_now;
    while (want--) {
        const int imask = (int)((S.sr >> 4) & 15);
        int lvl = 0, vec = 0;
        if (fw_irq() && 8 > imask) { lvl = 8; vec = 64 + 4; }
        else if (cmd_irq && 6 > imask) { lvl = 6; vec = 64 + 3; }
        else if (vref_irq && 4 > imask) { lvl = 4; vec = 64 + 2; vref_irq = 0; }
        if (lvl) take_irq(vec, lvl);
        const uint32_t pc = S.pc;
        /* IDLE SKIP (the profile: the SH-2 was ~40% of the main thread). The board's main loop, 0x04010FB6-C0: call 0x040187EE (nothing
         * to do in bus states 0 / 1), test the word 0x04054264 an interrupt handler sets, round again. After one full pass every pass
         * leaves the same state, and no interrupt can arrive within this call (the chip's ticks for the span ran above): the rest of the
         * call's instructions are skipped when nothing is pending. Only for this program (its six words checked once). */
        { static int ok = -1, pass;
          if (pc == 0x04010FBAu) {
              if (ok < 0) { static const uint16_t w[6] = { 0x4D0B, 0x0009, 0xD31F, 0x6032, 0x2008, 0x89F9 }; ok = 1;
                            for (int k = 0; k < 6; k++) if (RW(0x04010FB6u + 2u * (uint32_t)k) != w[k]) ok = 0;
                            if (getenv("GW_NOIDLE")) ok = 0; }
              if (ok && pass && !lvl && S.r[13] == 0x040187EEu && RL(0x04054264u) == 0 && RL(0x0405CD18u) <= 1) { S.cycles += want + 1; want = 0; break; }
              pass = 1;
          } else if (pc == 0x04010FC2u) pass = 0; }
        { /* $GW_SH2PC=addr,addr,...: how often each runs, to the log every 300 frames */
          static int np = -1; static uint32_t pa[8]; static unsigned long pn[8]; static uint32_t lastf;
          if (np < 0) { np = 0; const char *e = getenv("GW_SH2PC"); while (e && *e && np < 8) { pa[np++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; else break; } }
          for (int i = 0; i < np; i++) if (pc == pa[i]) pn[i]++;
          extern uint32_t rr_frame; if (np && rr_frame / 300 != lastf) { lastf = rr_frame / 300; fprintf(stderr, "[SH2PC] f%u", rr_frame); for (int i = 0; i < np; i++) fprintf(stderr, " %08X:%lu", pa[i], pn[i]); fprintf(stderr, "\n"); } }
        { static uint32_t hist[16]; static int hn, said; hist[hn++ & 15] = pc;              /* the way into the error routine (debug) */
          if (pc == 0x04002324u && !said) { said = 1; fprintf(stderr, "[GMEN] SH-2 error routine, r4 %X; last PCs:", S.r[4]);
              for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", hist[(hn + k) & 15]); fprintf(stderr, " | ICR %04X IPRA %04X IPRB %04X VCRA %04X VCRB %04X VCRC %04X VCRD %04X VCRWDT %04X VCRDIV %08X", rd_be(onchip+0xE0,2), rd_be(onchip+0xE2,2), rd_be(onchip+0x60,2), rd_be(onchip+0x62,2), rd_be(onchip+0x64,2), rd_be(onchip+0x66,2), rd_be(onchip+0x68,2), rd_be(onchip+0xE4,2), rd_be(onchip+0x10C,4)); fprintf(stderr, "\n"); } }
        S.pc += 2;
        exec(RW(pc));
        S.cycles++;
        { static int sm = -1; if (sm < 0) sm = getenv("GW_SH2SAMPLE") != NULL;              /* $GW_SH2SAMPLE: the PC every 100003 instructions */
          if (sm && S.cycles % 100003 == 0) fprintf(stderr, "[SH2S] %llu %08X %03X\n", (unsigned long long)(main_now / 2822400), pc, S.sr); }
    }
    { /* $GW_SH2WATCH=addr,addr,...: each change of those SH-2 longwords (the link program's state: 0405CD18, ...) to the log */
      static int nw = -1; static uint32_t wa[8], wv[8];
      if (nw < 0) { nw = 0; const char *e = getenv("GW_SH2WATCH"); while (e && *e && nw < 8) { wa[nw] = (uint32_t)strtoul(e, (char **)&e, 16); wv[nw] = 0xDEADBEEFu; nw++; if (*e == ',') e++; else break; } }
      for (int i = 0; i < nw; i++) { const uint32_t v = mem_r(wa[i], 4);
          if (v != wv[i]) { extern uint32_t rr_frame; fprintf(stderr, "[SH2W] f%u %08X = %08X (was %08X) pc %08X\n", rr_frame, wa[i], v, wv[i], S.pc); wv[i] = v; } } }
    static int log = -1; if (log < 0) log = getenv("GW_SH2LOG") != NULL;   /* $GW_SH2LOG: the SH-2's PC once a field */
    static uint64_t last_field; if (log && next_field != last_field) { last_field = next_field;
        static long dump_at = -2; if (dump_at == -2) { const char *e = getenv("GW_SH2DUMP"); dump_at = e ? atol(e) : 1200; }
        if ((long)(main_now / 2822400) == dump_at) { FILE *f = fopen("sh2_ram40.bin", "wb"); if (f) { fwrite(ram40, 1, 0x80000, f); fclose(f); }
                                          f = fopen("sh2_shared.bin", "wb"); if (f) { fwrite(gw_gmen_ram, 1, 0x10000, f); fclose(f); } }
        fprintf(stderr, "[SH2] field %llu pc %08X sr %03X r0 %08X r1 %08X r12 %08X vbr %08X cmd %d%s", (unsigned long long)(main_now / 2822400), S.pc, S.sr, S.r[0], S.r[1], S.r[12], S.vbr, cmd_irq, "\n"); }
}

/* $GW_SHWATCH=lo-hi (hex offsets in the shared RAM): each write there that changes the value, by the main CPU (M) or the SH-2 (S) */
void gw_shwatch(int who, uint32_t off, int n, uint32_t v)
{
    static int on = -1; static uint32_t lo, hi; static uint32_t last[0x400]; static unsigned cnt;
    if (on < 0) { const char *e = getenv("GW_SHWATCH"); on = e && sscanf(e, "%x-%x", &lo, &hi) == 2; if (hi - lo >= 0x1000) hi = lo + 0xFFF; memset(last, 0xEE, sizeof last); }
    if (!on || off < lo || off > hi) return;
    const uint32_t k = ((off - lo) >> 2) & 0x3FF, w = rd_be(gw_gmen_ram + (off & ~3u), 4);
    extern uint32_t rr_frame; static long from = -1; if (from < 0) { const char *f = getenv("GW_SHWATCH_FROM"); from = f ? atol(f) : 0; }
    if (w == last[k] || cnt > 3000 || (long)rr_frame < from) return;
    last[k] = w; cnt++;
    fprintf(stderr, "[SHW] f%u %c %04X (%d) = %X  -> long %08X\n", rr_frame, who ? 'S' : 'M', off, n, v, w);
}
