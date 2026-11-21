/*
 * s21_lanlink.c -- our fork: Cyber Sled on the CABINET LAN, the same system as the other linked games (engine/link_host.c: link.cfg,
 * the Network page, the cabinets gathering at start-up, the link lights, the safety net, the test windows on one PC).
 *
 * The game's own side stays the author's (src/link/s21_link.c): the C139 model and the LOCKSTEP exchange, in which the packets a
 * cabinet sends in frame f are delivered to the other in frame f + 1, so both machines stay deterministic. This file is only the
 * transport for that exchange, over engine/link_net.c instead of the author's UDP socket (which is not in the Windows build):
 *   one link frame = 0x5321, count, count x { frame (low, high word), length in bytes, the bytes two to a word }
 * carrying this cabinet's last 4 frames each time, so a lost datagram is covered by the next one; while a cabinet waits for the
 * other, the newest is sent again every 20 ms (both waiting on each other unblock). Two cabinets: POSITION LEFT (cabinet 1) and
 * RIGHT (cabinet 2), the game's own setting (GAME OPTIONS > POSITION), seeded from the cabinet number on a cabinet's first run.
 */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "s21_link.h"
#include "s21_board.h"
#include "link_host.h"
#include "link_net.h"

#define MAGIC 0x5321u
#define KEEP  4
typedef struct { uint32_t f; uint16_t n; uint8_t d[S21_LINK_PAYLOAD_MAX]; int ok; } slot_t;
static slot_t tx[KEEP], rx[64];
static int ntx;
static uint32_t last_send, n_resend, n_too_big;

static void flush(void)
{
    uint16_t w[LNK_MAX_WORDS];
    int k = 2; w[0] = (uint16_t)MAGIC; w[1] = 0;
    for (int i = 0; i < KEEP && i < ntx; i++) {
        const slot_t *s = &tx[(ntx - 1 - i) & (KEEP - 1)];
        if (!s->ok) break;
        const int need = 3 + (s->n + 1) / 2;
        if (k + need > LNK_MAX_WORDS) { if (!w[1]) n_too_big++; break; }   /* the newest always goes, unless it alone is too long */
        w[k++] = (uint16_t)s->f; w[k++] = (uint16_t)(s->f >> 16); w[k++] = s->n;
        for (int j = 0; j < s->n; j += 2) w[k++] = (uint16_t)(s->d[j] << 8 | (j + 1 < s->n ? s->d[j + 1] : 0));
        w[1]++;
    }
    if (w[1]) lnk_send(w, k);
    last_send = SDL_GetTicks();
}
static void t_send(void *c, uint32_t f, const uint8_t *p, size_t n)
{
    (void)c;
    slot_t *s = &tx[ntx & (KEEP - 1)];
    s->f = f; s->n = (uint16_t)(n > S21_LINK_PAYLOAD_MAX ? S21_LINK_PAYLOAD_MAX : n); memcpy(s->d, p, s->n); s->ok = 1;
    ntx++;
    flush();
}
static void drain(void)
{
    static uint16_t w[LNK_MAX_WORDS];
    int n;
    while ((n = lnk_recv(w, LNK_MAX_WORDS)) > 0) {
        if (n < 2 || w[0] != MAGIC) continue;
        int k = 2;
        for (int i = 0; i < w[1]; i++) {
            if (k + 3 > n) break;
            const uint32_t f = w[k] | (uint32_t)w[k + 1] << 16; const uint16_t len = w[k + 2]; k += 3;
            const int nw = (len + 1) / 2;
            if (len > S21_LINK_PAYLOAD_MAX || k + nw > n) break;
            slot_t *s = &rx[f & 63];
            if (!s->ok || s->f != f) {
                s->f = f; s->n = len; s->ok = 1;
                for (int j = 0; j < len; j++) s->d[j] = (uint8_t)(j & 1 ? w[k + j / 2] : w[k + j / 2] >> 8);
            }
            k += nw;
        }
    }
}
static int t_recv(void *c, uint32_t f, uint8_t *p, size_t cap)
{
    (void)c;
    drain();
    const slot_t *s = &rx[f & 63];
    if (!s->ok || s->f != f || s->n > cap) return -1;
    memcpy(p, s->d, s->n);
    return s->n;
}
static void t_poll(void *c, int ms)
{
    (void)c;
    drain();
    if (ms > 0) {
        SDL_Delay(ms > 2 ? 2 : 1);
        if (ntx && SDL_GetTicks() - last_send > 20) { flush(); n_resend++; }
    }
}

/* ---- the link host's view of the game ---- */
static void no_irq(void) {}                               /* (the C139 here is the author's model, driven by s21_c139_slice) */
static void report(char *b, int n)
{
    snprintf(b, (size_t)n, "game: %s, ring %d, position %s, resent %u%s", s21_link_state() == S21_LINK_ON ? "lockstep" : "unplugged",
             s21_link_ring_size(), (g_s21.shared[0x228 / 2] & 1) ? "RIGHT" : "LEFT", n_resend, n_too_big ? ", a frame too long to send" : "");
}
static bool good(void) { return s21_link_state() == S21_LINK_ON && s21_link_ring_size() > 0; }   /* the game hears the ring */
static int  game_id(void) { return g_s21.shared[0x228 / 2] & 1; }  /* the slot the game uses (0x900228, from POSITION) */
static void (*restart_hook)(void);
static void before_restart(void) { if (restart_hook) restart_hook(); }

bool s21_lanlink_open(bool first_run, void (*save_and_close)(void))
{
    static link_host_game lg;
    lg = (link_host_game){ "CS", no_irq, no_irq, report, good, game_id, NULL, before_restart, 0 };
    lg.max_cabinets = 2;                                  /* POSITION LEFT / RIGHT */
    restart_hook = save_and_close;
    if (!link_host_open(&lg)) return false;
    if (!link_host_on()) return true;
    const bool seed = first_run || link_host_take_id_reset();   /* a cabinet's first run (or a new number on the Network page): its
                                                                 * POSITION from the number; otherwise the test menu's is kept */
    static const s21_link_transport t = { NULL, t_send, t_recv, t_poll, NULL };
    if (!s21_link_attach(&t, seed ? link_host_id() : -1, 1)) { fprintf(stderr, "[LINK] Cyber Sled: another link is already attached (CS_LINK)\n"); return false; }
    fprintf(stderr, "[LINK] Cyber Sled: lockstep over the cabinet LAN, cabinet %d%s\n", link_host_id() + 1,
            seed ? (link_host_id() ? ": POSITION set to RIGHT" : ": POSITION set to LEFT") : " (POSITION from the test menu)");
    return true;
}

/* after the start-up wait: a cabinet that is alone in its session (the other one is off, or was already playing -- it is not let in)
 * plays on its own, as a cabinet with its link cable unplugged; a lockstep link would wait for frames that never come */
void s21_lanlink_started(void)
{
    if (s21_link_state() != S21_LINK_ON) return;                 /* not ours (no --link / link.cfg), or already unplugged */
    if (!link_host_on()) { fprintf(stderr, "[LINK] Cyber Sled: the link was skipped: playing alone\n"); s21_link_detach(); return; }
    const uint32_t others = lnk_members() & ~(1u << link_host_id());
    if (lnk_state() != LNK_LINKED || !others) {
        fprintf(stderr, "[LINK] Cyber Sled: no other cabinet in this session: playing alone (the link as an unplugged cable)\n");
        s21_link_detach();
    }
}
