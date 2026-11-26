/*
 * rr_main.c -- host for the lifted Rave Racer program.
 *
 * The 68020 program is gen/rr_lifted.c: entry_reset (L_4000) never returns,
 * it runs the game's own main loop. Time advances in rr_tick(), which the
 * lifted code polls on every call and backward branch (RR_POLL). Every
 * RR_POLLS_PER_FRAME polls is one video frame: the vblank devices run, the
 * vblank IRQ is raised, and any pending IRQ above the SR mask is delivered by
 * calling its autovector handler (vector table at VBR = 0).
 *
 * Ghidra's rte p-code is a bare return that restores neither SR nor SP, so an
 * interrupt pushes NO frame: the host saves SR, raises the mask to the
 * interrupt's level, calls the handler, and restores SR afterwards.
 *
 *   rr <rom_dir> [--frames N] [--dump DIR]    headless run
 */
#include <SDL.h>
#include "eng_args.h"
#include "rr_romzip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rr_mem.h"
#include "rr_hw.h"
#include "rr_dsp.h"
#include "rr_video.h"
#include "rr_gl.h"
#include <GL/gl.h>
#include "rr_scene.h"
#include "rr_lift_rt.h"
#include "lift_cpu.h"
#include "rr_game.h"
#include "rr_ui.h"
#include "rr_lifted.h"
#include "rr_input.h"
#include "rr_sound.h"
#include "rr_link.h"
#include "rr_net.h"

bool rr_host_open(int scale);
bool rr_host_frame(void);
bool rr_host_paused(void);
void rr_host_close(void);
void rr_host_message(const char *const *lines, int n);
int rr_host_joytest(void);
static int windowed;
/* THE RENDERER. 1 = the shared engine's OpenGL pipeline (../engine, src/rr_gl.c):
 * the game's renderer, always, in the window and headless. 0 = src/rr_video.c,
 * the software MAME port -- a test ORACLE, compiled only into the dev binaries
 * (RR_ORACLE: rr_oracle, rr_trace, rr_sndoracle), where it is the headless
 * default and --gl selects the engine. */
int g_rr_gl = 1;
static int gl_w = 640, gl_h = 480;             /* headless picture size (RR_RENDER_SIZE) */
static bool gl_ok;                             /* a GL context and the video ROMs: there is a picture */

/* --perf: per-frame emulation time (68K + DSP + render, host sleep excluded) and
 * render time, reported as percentiles at exit. Quote these, never one run's fps. */
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tex_bake.h"
#include "link_host.h"
#include "rr_ui.h"
#include "win_gpu.h"          /* Windows: run on the discrete GPU of a two-GPU laptop (Optimus / PowerXpress) */
static FILE *perflog;                         /* RR_PERFLOG=<file>: one line per frame -- see the perf block */
static double perflog_texels;
static int perf_on;
static double *perf_frame, *perf_video;
static uint32_t perf_n, perf_cap;
static double t_frame_start;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void perf_report(void)
{
    if (!perf_on || !perf_n) return;
    const char *nm[2] = { "frame", "video" };
    double *arr[2] = { perf_frame, perf_video };
    for (int k = 0; k < 2; k++) {
        double *v = malloc(perf_n * sizeof *v), sum = 0; int over = 0;
        memcpy(v, arr[k], perf_n * sizeof *v);
        for (uint32_t i = 0; i < perf_n; i++) { sum += v[i]; if (v[i] > 16.667) over++; }
        qsort(v, perf_n, sizeof *v, cmpd);
        fprintf(stderr, "[PERF] %-5s n=%u mean %.2f p50 %.2f p90 %.2f p99 %.2f max %.2f ms  over16.67: %d\n", nm[k], perf_n,
                sum / perf_n, v[perf_n / 2], v[perf_n * 9 / 10], v[perf_n * 99 / 100], v[perf_n - 1], over);
        free(v);
    }
}
static const char *rec_path, *rep_path;
static int freeplay = -1;
static int32_t test_coin = -1, test_gas = -1;   /* --coin F[,F2...]: pulse coin 1 at each frame (Ace Driver takes two a credit); --gas F: hold gas from F */
static int32_t test_coins[8]; static int n_test_coins;
static struct { int32_t at; int32_t value; } test_steer[8]; static int n_steer;
static struct { int32_t at, idx, value; } test_adc[32]; static int n_adc;   /* --adc F:I:V (repeatable): twin-stick ADC.I at V (0x47..0xB7, centre 0x7F) from frame F */   /* --steer F:V (repeatable): the wheel at V (0..0xFFF, centre 0x800) from frame F */

#define REG_SP   RR_REG_SP
#define REG_SR   RR_REG_SR

static int32_t  polls_per_frame = 83000;   /* 68K instructions per frame (MAME: 4.99M in 60) */
static uint32_t max_frames = 600;
#define frame rr_frame              /* the shared runtime names the frame in its traps */
static const char *dump_dir;
/* EXTRA CAMERAS (rr_game_t.cams; Ridge Racer 2): g_cfg_camera 0 = the game's own view, 1..ncams = an extra one. Its words are written
 * at every frame while it is chosen and allowed (the race); when it ends (the original chosen again, or the race over) cam_restore puts
 * the original's words back once. The choice is kept in the game's cfg ("camera") and shown as a hint when it changes. */
int g_cfg_camera;
void rr_cam_cycle(void)
{
    const rr_game_t *g = g_rr_game;
    if (!g->ncams) return;
    g_cfg_camera = (g_cfg_camera + 1) % (g->ncams + 1);
    char v[16]; snprintf(v, sizeof v, "%d", g_cfg_camera);
    rr_input_set_option(g->cfg_file, "camera", v);
    char h[96]; snprintf(h, sizeof h, "Camera: %s", g_cfg_camera ? g->cams[g_cfg_camera - 1].name : "original (cockpit)");
    rr_ui_set_hint(h, 120);
    fprintf(stderr, "[CAM] %s\n", h);
}
static void rr_cam_frame(void)
{
    const rr_game_t *g = g_rr_game;
    if (!g->ncams) return;
    static int applied; static uint32_t saved[8];
    { static int env = -1; if (env < 0) { const char *e = getenv("RR_CAMERA"); env = e ? atoi(e) : 0; if (e) g_cfg_camera = env; } }   /* RR_CAMERA=n: a run's camera (tests; headless reads no cfg) */
    int want = g_cfg_camera >= 0 && g_cfg_camera <= g->ncams ? g_cfg_camera : 0;
    if (want && g->cam_allowed && !g->cam_allowed()) want = 0;
    if (want && !applied) for (int i = 0; i < g->ncam_restore && i < 8; i++) saved[i] = rr_read(g->cam_restore[i].addr, 4);
    if (!want && applied) for (int i = 0; i < g->ncam_restore && i < 8; i++)
        rr_write(g->cam_restore[i].addr, 4, g->cam_restore[i].val == RR_CAM_SAVED ? saved[i] : g->cam_restore[i].val);
    applied = want;
    if (want) { const rr_cam_t *c = &g->cams[want - 1]; for (int i = 0; i < c->n; i++) rr_write(c->w[i].addr, 4, c->w[i].val); }
}
static const char *shot_dir;          /* --shots DIR EVERY: a PPM every EVERY frames */
static uint32_t shot_every;

void rd_budget_out(void);   /* src/rd: unwind a checker probe (budget ran out, or it trapped) */

/* The 68K host runtime -- register file, shadow return stack, traps, interrupt entry, the
 * instruction-trace hook -- is engine/lift_cpu.c, shared with every lifted game. */
#define get_sr rr_get_sr
#define set_sr rr_set_sr

static void dump_state(void)
{
    if (!dump_dir) return;
    char p[1024];
    snprintf(p, sizeof p, "%s/wram_f%u.bin", dump_dir, frame);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(g_rr.wram, 1, RR_WRAM_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/text_f%u.bin", dump_dir, frame);      /* the text tilemap: what marks a screen type (the HUD) */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.text, 1, RR_TEXT_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/pal_f%u.bin", dump_dir, frame);       /* palette RAM 0x90028000 (planar R/G/B) */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.pal, 1, RR_PAL_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/mixer_f%u.bin", dump_dir, frame);     /* mixer 0x90020000 */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.mixer, 1, RR_MIXER_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/eeprom_f%u.bin", dump_dir, frame);    /* our fork: the EEPROM 0x58000000 (finding a game's settings blocks) */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.eeprom, 1, RR_EEPROM_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/shared_f%u.bin", dump_dir, frame);    /* 68K byte order */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.shared, 1, RR_SHARED_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/poly_f%u.bin", dump_dir, frame);
    f = fopen(p, "wb");
    if (f) {
        for (uint32_t i = 0; i < RR_POLY_WORDS; i++) {
            uint32_t w = g_rr.poly[i] | 0xFF000000u;           /* what the 68K reads */
            uint8_t b[4] = { w >> 24, w >> 16, w >> 8, w };
            fwrite(b, 1, 4, f);
        }
        fclose(f);
    }
}

/* Time advances in SLICES: each slice the 68K spends polls_per_frame/SLICES
 * polls and the master DSP its share of a frame (40 MHz C71 = 10 MIPS =
 * ~166,667 instructions per 60 Hz frame); every SLICES slices is a frame. */
#define SLICES 16
#define DSP_STEPS_PER_FRAME 166667L
static uint32_t slice, serial_acc;
/* VTOTAL 525, VBSTART 480 (namcos22.cpp): vblank is the first 45/525 of a frame
 * after the screen update, ~1.37 of our 16 slices. The master answers the
 * vblank INT0 inside that window, and MAME's pdp_begin then sets pdp_frame one
 * AHEAD so the next screen update keeps the list (render_frame_active). */
bool g_rr_in_vblank;
static int vblank_slices;

/* --sndsweep HOLD[:PASS[:SLOT_LO:SLOT_HI[:CMD_MAX[:FROM]]]]: the sound driver's coverage harness (engine/ss22_run.c's, for this board). Rave Racer's
 * mailbox is the same as the Super 22 games' but at shared 0x1000: command words 0x1000 + slot*2, parameter words 0x1100..0x117E. PASS 0 writes
 * every command 0x4000|c and 0xC000|c into every slot in turn; PASS 1-4 fill the parameters with 0x0000/0x4000/0x8000/0xFFFF first; PASS >= 5
 * writes random commands and parameters (xorshift seeded by PASS). Run on rr_sndoracle with SND_COV=<file> (tools/grind_snd.sh). */
static struct { int on, hold, pass, slot_lo, slot_hi, cmd_max, from; } sweep;
static void sweep_arg(const char *a)
{
    sweep.on = 1; sweep.hold = 6; sweep.pass = 0; sweep.slot_lo = 0; sweep.slot_hi = 31; sweep.cmd_max = 0x7F; sweep.from = 1000;
    sscanf(a, "%d:%d:%d:%d:%i:%d", &sweep.hold, &sweep.pass, &sweep.slot_lo, &sweep.slot_hi, &sweep.cmd_max, &sweep.from);
    if (sweep.hold < 1) sweep.hold = 1;
}
#define SWEEP_BASE 0x1000
static void sweep_put16(uint32_t off, uint16_t v) { g_rr.shared[SWEEP_BASE + off] = (uint8_t)(v >> 8); g_rr.shared[SWEEP_BASE + off + 1] = (uint8_t)v; }
static uint32_t sweep_rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return (uint32_t)(*s >> 16); }
static void sweep_tick(void)
{
    static const uint16_t par[5] = { 0, 0x0000, 0x4000, 0x8000, 0xFFFF };
    static long prev_slot = -1;
    if (!sweep.on || frame < (uint32_t)sweep.from || (frame - (uint32_t)sweep.from) % (uint32_t)sweep.hold) return;
    if (sweep.pass >= 5) {
        static uint64_t rs;
        if (!rs) rs = 0x9E3779B97F4A7C15ull * (uint64_t)sweep.pass;
        for (int i = 0; i < 4; i++) {
            uint32_t sl = (uint32_t)sweep.slot_lo + sweep_rnd(&rs) % (uint32_t)(sweep.slot_hi - sweep.slot_lo + 1);
            uint32_t on = sweep_rnd(&rs), fl = sweep_rnd(&rs), c = sweep_rnd(&rs) % (uint32_t)(sweep.cmd_max + 1);
            sweep_put16(sl * 2, (on & 3) ? (uint16_t)(((fl & 1) ? 0xC000u : 0x4000u) | c) : 0);
        }
        for (uint32_t o = 0x100; o < 0x180; o += 2) { uint32_t v = sweep_rnd(&rs); if (v & 1) sweep_put16(o, (uint16_t)(v >> 1)); }
        return;
    }
    const long per_slot = 2L * (sweep.cmd_max + 1), nslots = sweep.slot_hi - sweep.slot_lo + 1;
    const long n = (long)((frame - (uint32_t)sweep.from) / (uint32_t)sweep.hold);
    if (prev_slot >= 0) sweep_put16((uint32_t)prev_slot * 2, 0);
    if (n >= nslots * per_slot) { if (n == nslots * per_slot) fprintf(stderr, "[SNDSWEEP] done at frame %u\n", frame); prev_slot = -1; return; }
    const long slot = sweep.slot_lo + n / per_slot, k = n % per_slot;
    if (sweep.pass > 0) for (uint32_t o = 0x100; o < 0x180; o += 2) sweep_put16(o, par[sweep.pass]);
    sweep_put16((uint32_t)slot * 2, (uint16_t)(((k & 1) ? 0xC000u : 0x4000u) | (unsigned)(k >> 1)));
    prev_slot = slot;
}

/* ---- the Debug page: developer screens nothing in the program reaches (rr_game.h rr_debug_t) ----
 * At a frame boundary, once the vblank interrupt is delivered and while the program is in its MAIN context (the coroutine task
 * stack empty -- inside the mode task the slot would be overwritten by its own next yield), the screen's entry is written into the
 * mode task's slot, exactly as the program's own setter does, and the Test switch goes ON. The main loop then resumes the screen;
 * the screen leaves through the program's own exit when the Test switch goes OFF. RR_DEBUG=<i>@<frame>: headless. */
static int debug_pending = -1;
static uint32_t debug_test_at;               /* the frame the Test switch went on for this launch (0: not yet) */
static uint32_t debug_slot_was;              /* the mode task's slot when the Test switch went on */
void rr_debug_launch(int i)
{
    if (!g_rr_game->debug || i < 0 || i >= g_rr_game->ndebug) return;
    debug_pending = i; debug_test_at = 0;
    fprintf(stderr, "[DEBUG] launching \"%s\" (0x%X)\n", g_rr_game->debug[i].name, g_rr_game->debug[i].entry);
}
/* Two steps, because the program reacts to the Test switch itself: switching it on makes the main loop install the operator
 * test mode into the very slot the screen goes in. So: the Test switch on first; the moment the slot changes (the test mode
 * installed, not yet run -- it would leave its menu on the text layer) the screen replaces it. After 120 frames without a
 * change the screen goes in anyway. */
static void debug_boundary(void)
{
    { static const char *e; static int init;           /* RR_DEBUG=i@frame[,i@frame...]: launch Debug-page entries headlessly */
      if (!init) { init = 1; e = getenv("RR_DEBUG"); }
      for (const char *p = e; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
          int idx, at; if (sscanf(p, "%d@%d", &idx, &at) == 2 && frame == (uint32_t)at) rr_debug_launch(idx);
      } }
    if (debug_pending < 0 || rr_in_irq) return;
    if (g_rr_game->debug_dsw) {                         /* a menu behind a DIP switch (rr_game.h debug_dsw) */
        const rr_debug_t *d = &g_rr_game->debug[debug_pending];
        if (d->entry == DEBUG_DSW_LEAVE) {
            g_hw.dsw |= g_rr_game->debug_dsw; debug_pending = -1;
            fprintf(stderr, "[DEBUG] DIP switch off at frame %u: back to the game\n", frame); return;
        }
        if (!debug_test_at) {
            g_hw.dsw &= ~g_rr_game->debug_dsw;
            if (windowed) rr_host_set_test(false);      /* the developer menu runs only with the Test switch OFF */
            else g_hw.inputs |= 0x0400;
            debug_test_at = frame;
            fprintf(stderr, "[DEBUG] DIP switch on at frame %u\n", frame);
            if (!d->entry) debug_pending = -1;
            return;
        }
        if (rr_read(g_rr_game->debug_mode_addr, 2) != g_rr_game->debug_mode ||
            rr_read(g_rr_game->debug_state_addr, 2) != g_rr_game->debug_menu_state) {
            if (frame > debug_test_at + 600) { debug_pending = -1; fprintf(stderr, "[DEBUG] the menu never came up\n"); }
            return;
        }
        rr_write(g_rr_game->debug_cursor_addr, 2, (d->entry - 1) / 2);
        rr_write(g_rr_game->debug_state_addr, 2, d->entry);
        fprintf(stderr, "[DEBUG] \"%s\" selected (state %u) at frame %u\n", d->name, d->entry, frame);
        debug_pending = -1;
        return;
    }
    if (!debug_test_at) {
        g_hw.inputs &= (uint16_t)~0x0400;               /* the Test switch ON (headless; a window's host keeps it on) */
        if (windowed) rr_host_set_test(true);
        debug_test_at = frame;
        debug_slot_was = rr_read(g_rr_game->task_slot, 4);
        return;
    }
    if (rr_read(g_rr_game->task_sp, 4) != g_rr_game->task_sp_empty) return;     /* not the main context: try next frame */
    if (rr_read(g_rr_game->task_slot, 4) == debug_slot_was && frame < debug_test_at + 120) return;
    const rr_debug_t *d = &g_rr_game->debug[debug_pending];
    debug_pending = -1;
    rr_write(g_rr_game->task_slot, 4, d->entry);
    fprintf(stderr, "[DEBUG] \"%s\" installed in the mode task at frame %u\n", d->name, frame);
}

void rr_tick(void)
{
    rd_budget_out();
    rr_budget = polls_per_frame / SLICES;
    if (rr_in_irq) { rr_budget = 1000; return; }      /* finish the handler first */
    g_rr_in_vblank = vblank_slices > 0;
    if (vblank_slices > 0) {                       /* split the vblank slice at VBEND */
        long vb = DSP_STEPS_PER_FRAME * 45 / 525 - (long)(2 - vblank_slices) * (DSP_STEPS_PER_FRAME / SLICES);
        long full = DSP_STEPS_PER_FRAME / SLICES;
        if (vb > full) vb = full;
        if (vb < 0) vb = 0;
        rr_dsp_run(vb);
        g_rr_in_vblank = false;
        rr_dsp_run(full - vb);
        vblank_slices--;
    } else
        rr_dsp_run(DSP_STEPS_PER_FRAME / SLICES);
    rr_sound_slice();                              /* the sound program's share of this slice */
    rr_audio_slice();                              /* and the mixer's samples */
    link_host_slice();                             /* and the link chip */
    serial_acc += 100;                             /* 100 Hz serial pulse */
    if (serial_acc >= 60 * SLICES) { serial_acc -= 60 * SLICES; rr_dsp_serial(); }
    if (++slice % SLICES) return;
    frame++;
    link_host_frame(frame, !windowed);
    { extern uint32_t rr_idle_addr, rr_idle_reads; static int on = -1;   /* RR_IDLE=<addr>: the vblank-flag reads of each frame (how much spare time) */
      if (on < 0) { const char *e = getenv("RR_IDLE"); on = e != NULL; if (e) rr_idle_addr = (uint32_t)strtoul(e, NULL, 0); }
      if (on) { fprintf(stderr, "[IDLE] f%u %u\n", frame, rr_idle_reads); rr_idle_reads = 0; } }
    { static int np = -1; static uint32_t pa, pf;            /* RR_PEEK=<addr>:<frame> (test): a 16-bit word and the wall clock at that frame
                                                             * (Ace Driver's race timer is 0x10001952: linked cabinets in step or not) */
      if (np < 0) { const char *e = getenv("RR_PEEK"); np = e && sscanf(e, "%x:%u", &pa, &pf) == 2; }
      if (np && frame == pf) fprintf(stderr, "[PEEK] f%u %08X = %u at %.2f ms\n", frame, pa, rr_read(pa, 2), now_ms()); }
    sweep_tick();
    double tv0 = perf_on ? now_ms() : 0;
    if (g_rr_gl && gl_ok) {                        /* screen update at the end of the frame */
        rr_gl_prepare(rr_dsp_slave_active());
        /* headless: draw every frame (RR_GL_DRAWALL=1) so --perf counts the GL
         * cost too; glFinish so it is the GPU's time, not just the submission */
        static int drawall = -1;
        static long drawfrom = -1;   /* RR_GL_DRAWFROM=F (with RR_GL_DRAWALL=1): draw only from frame F on -- a slow software GL reaches a late scene sooner */
        if (drawall < 0) { const char *e = getenv("RR_GL_DRAWALL"); drawall = e && *e == '1'; e = getenv("RR_GL_DRAWFROM"); drawfrom = e ? atol(e) : 0; }
        if (drawall && !windowed && (long)frame >= drawfrom) { rr_gl_draw(gl_w, gl_h); glFinish(); }
    }
#ifdef RR_ORACLE
    else if (!g_rr_gl) rr_video_frame(rr_dsp_slave_active());
#endif
    if (perf_on) {
        double t1 = now_ms();
        if (perf_n == perf_cap) { perf_cap = perf_cap ? perf_cap * 2 : 4096;
            perf_frame = realloc(perf_frame, perf_cap * sizeof *perf_frame); perf_video = realloc(perf_video, perf_cap * sizeof *perf_video); }
        if (t_frame_start > 0 && frame > 60) { perf_frame[perf_n] = t1 - t_frame_start; perf_video[perf_n] = t1 - tv0; perf_n++; }
        { extern void tex_caplog_frame(int); tex_caplog_frame((int)frame); }
        if (perflog && t_frame_start > 0) {         /* frame total_ms video_ms quads hits misses reallocs texels-this-frame placeholders refined tier-fallbacks (the last three cumulative) */
            fprintf(perflog, "%u %.3f %.3f %d %d %d %d %.0f %d %d %d\n", frame, t1 - t_frame_start, t1 - tv0, rr_gl_quads(), tex_frame_hits, tex_frame_misses,
                    tex_reallocs, g_bake_texels - perflog_texels, tex_placeholders, tex_refined, tex_tier_fallbacks);
            perflog_texels = g_bake_texels;
        }
    }
    for (int c = 0; c < n_test_coins; c++) {       /* scripted inputs for headless captures */
        if (frame == (uint32_t)test_coins[c]) g_hw.inputs &= (uint16_t)~0x1000;
        if (frame == (uint32_t)test_coins[c] + 6) g_hw.inputs |= 0x1000;
    }
    if (test_gas >= 0 && frame >= (uint32_t)test_gas) g_hw.gas = (uint16_t)g_rr_game->gas_max;
    { static int nv = -1; static uint32_t vf[16];          /* RR_TEST_VIEW=f1,f2,...: press VIEW CHANGE (active low 0x0040) for 30 frames at each */
      if (nv < 0) { nv = 0; const char *e = getenv("RR_TEST_VIEW"); while (e && *e && nv < 16) { char *q; const unsigned long v = strtoul(e, &q, 0); if (q == e) break; vf[nv++] = (uint32_t)v; e = q; if (*e == ',') e++; else break; } }
      for (int i = 0; i < nv; i++) { if (frame == vf[i]) g_hw.inputs &= (uint16_t)~0x0040; if (frame == vf[i] + 30) g_hw.inputs |= 0x0040; } }
    { static int np = -1; static uint32_t pf[16], pt[16], pa[16], pv[16]; static int ps[16];   /* RR_POKE=from[-to]:addr:size:value,...: a test switch, written at every frame in [from, to] */
      if (np < 0) { np = 0; const char *e = getenv("RR_POKE");
          while (e && *e && np < 16) { char *q; pf[np] = (uint32_t)strtoul(e, &q, 0); pt[np] = *q == '-' ? (uint32_t)strtoul(q + 1, &q, 0) : 0xFFFFFFFFu;
              unsigned a, v; int s, n = 0; if (sscanf(q, ":%x:%d:%x%n", &a, &s, &v, &n) != 3 || !n) break;
              pa[np] = a; ps[np] = s; pv[np] = v; np++; e = q + n; if (*e == ',') e++; else break; } }
      for (int i = 0; i < np; i++) if (frame >= pf[i] && frame <= pt[i]) rr_write(pa[i], ps[i], pv[i]); }
    rr_cam_frame();
    { static long every = -2; static uint32_t la[8]; static int nla;   /* RR_LOGWORDS=every:addr,addr,...: 32-bit words, every N frames (tests) */
      if (every == -2) { every = -1; const char *e = getenv("RR_LOGWORDS");
          if (e) { char *q; every = strtol(e, &q, 0); while (*q == ':' || *q == ',') { if (nla < 8) la[nla++] = (uint32_t)strtoul(q + 1, &q, 16); else break; } } }
      if (every > 0 && frame % (uint32_t)every == 0) { fprintf(stderr, "[WORDS] f%u", frame); for (int i = 0; i < nla; i++) fprintf(stderr, " %08X=%08X", la[i], rr_read(la[i], 4)); fprintf(stderr, "\n"); } }
    for (int i = 0; i < n_steer; i++) if (frame >= (uint32_t)test_steer[i].at) g_hw.steer = (uint16_t)test_steer[i].value;
    { static FILE *sf; static int init; static long nf = -1; static char ln[256];   /* RR_INSCRIPT=<file>: headless inputs per frame, lines
      * "<frame> steer=V gas=V brake=V test=0|1 in=MASK:BITS ...", each applied at its frame and kept (test-menu and select-screen checks) */
      if (!init) { init = 1; const char *e = getenv("RR_INSCRIPT"); if (e) sf = fopen(e, "r"); }
      while (sf) {
          if (nf < 0) { if (!fgets(ln, sizeof ln, sf)) { fclose(sf); sf = NULL; break; } if (ln[0] == '#' || sscanf(ln, "%ld", &nf) != 1) { nf = -1; continue; } }
          if ((uint32_t)nf > frame) break;
          for (char *t = strtok(ln, " \t\n"); t; t = strtok(NULL, " \t\n")) {
              unsigned v, m;
              if (sscanf(t, "steer=%i", &v) == 1) g_hw.steer = (uint16_t)v;
              else if (sscanf(t, "gas=%i", &v) == 1) g_hw.gas = (uint16_t)v;
              else if (sscanf(t, "brake=%i", &v) == 1) g_hw.brake = (uint16_t)v;
              else if (sscanf(t, "test=%u", &v) == 1) { if (v) g_hw.inputs &= (uint16_t)~0x0400; else g_hw.inputs |= 0x0400; }
              else if (sscanf(t, "in=%x:%x", &m, &v) == 2) g_hw.inputs = (uint16_t)((g_hw.inputs & ~m) | (v & m));
          }
          nf = -1;
      } }
    for (int i = 0; i < n_adc; i++) if (frame >= (uint32_t)test_adc[i].at) g_hw.adc[test_adc[i].idx & 3] = (uint8_t)test_adc[i].value;
    { static long pf = -2; if (pf == -2) { const char *e = getenv("RR_TEST_PLAY"); pf = e ? atol(e) : -1; }   /* RR_TEST_PLAY=F (twin-stick games, headless
      * tests): from frame F the gun pulses and both sticks turn left / centre / right in turn, so a match runs (with --coin);
      * RR_TEST_FIREONLY=1 keeps the sticks centred */
      if (pf >= 0 && frame >= (uint32_t)pf) {
          if (frame % 20 < 6) g_hw.inputs &= (uint16_t)~0x0001; else g_hw.inputs |= 0x0001;
          const int t = getenv("RR_TEST_FIREONLY") ? 0 : (int)((frame / 90) % 4);
          g_hw.adc[0] = (uint8_t)(t == 1 ? 0x47 : t == 3 ? 0xB7 : 0x7F); g_hw.adc[1] = (uint8_t)(t == 1 ? 0xB7 : t == 3 ? 0x47 : 0x7F);
          g_hw.adc[2] = 0x7F; g_hw.adc[3] = 0x7F; } }
    if (windowed) {
        if (frame % 120 == 0) rr_hw_eeprom_save();     /* the test menu's settings and the records, once the game has changed them */
        do {
            if (!rr_host_frame()) { perf_report(); rr_hw_eeprom_save(); rr_audio_close(); rr_host_close(); fprintf(stderr, "[RR] window closed at frame %u\n", frame); exit(0); }
            /* the Online menu lives in this loop: the built-in host and the lobby (HELLO/WELCOME, ROSTER, READY,
             * START) must keep running while it is open, or "Host a LAN game" never gets its own HELLO.
             * In a race session too, as a keepalive (pings, and the built-in host keeps relaying for the
             * others): without it the server dropped a player whose menu was open for 5 s */
            if (rr_host_paused()) rr_net_poll_paused();
        } while (rr_host_paused());
        for (int c = 0; c < n_test_coins; c++)         /* the scripted --coin / --gas in a window too (the window's own inputs replace them above) */
            if (frame >= (uint32_t)test_coins[c] && frame < (uint32_t)test_coins[c] + 6) g_hw.inputs &= (uint16_t)~0x1000;
        if (test_gas >= 0 && frame >= (uint32_t)test_gas) g_hw.gas = (uint16_t)g_rr_game->gas_max;
    }
    if (!windowed) {                               /* RR_PACE=1: a headless run at the real 59.9 Hz (network tests: jitter in ms means frames) */
        static int pace = -1; static uint32_t t0;
        if (pace < 0) { const char *e = getenv("RR_PACE"); pace = e && *e == '1'; t0 = SDL_GetTicks(); }
        if (pace) {
            const uint32_t t = SDL_GetTicks();
            uint32_t due = t0 + (uint32_t)((double)frame * 1000.0 / 59.9);
            if ((int32_t)(t - due) > 50) { t0 = t - (uint32_t)((double)frame * 1000.0 / 59.9); due = t; }   /* fell behind (a stall): carry on at 60 Hz, no catch-up burst */
            if ((int32_t)(due - t) > 0) SDL_Delay(due - t);
        }
    }
    if (!link_host_on()) rr_net_apply_inputs();    /* online (the author's Online menu): the automatic gas that starts every machine together */
    rr_input_frame(frame);                         /* replay overrides, recorder logs */
    if (perf_on) t_frame_start = now_ms();
    if (windowed && shot_dir && shot_every && frame % shot_every == 0) rr_host_screenshot(shot_dir);
    else if (shot_dir && shot_every && frame % shot_every == 0 && (!getenv("RR_GL_DRAWFROM") || (long)frame >= atol(getenv("RR_GL_DRAWFROM")))) {
        char p[1024]; snprintf(p, sizeof p, "%s/f%05u.ppm", shot_dir, frame);
        if (g_rr_gl && gl_ok && !windowed) { rr_gl_draw(gl_w, gl_h); rr_gl_write_ppm(p, gl_w, gl_h); }
#ifdef RR_ORACLE
        else if (!g_rr_gl) rr_video_write_ppm(p);
#endif
    }
    vblank_slices = 2;
    rr_dsp_vblank();
    if (!rr_env_active()) rr_hw_vblank();      /* (a trace oracle refreshes at MAME's own vblank interrupt: rr_env.c) */
    if (!link_host_on()) {               /* the author's Online play (our link, link.cfg / --link, has the chip instead: engine/lan139.c) */
        rr_net_poll();                   /* online play: peer FRAMEs into the link queue before the poll injects one */
        rr_link_poll();                  /* a received link packet's SCI IRQ lands at this frame edge */
    }
    if (!rr_env_active()) {                  /* a trace oracle lands the interrupts itself */
        rr_deliver_irqs(rr_hw_irq_level);
        /* online: the other pending rivals' packets, each through the game's own SCI handler (offline nothing is pending) */
        for (int k = 0; k < 7 && !link_host_on(); k++) {
            if (g_hw.irq_state & ~(1u << 2)) break;      /* only while the SCI is all that is pending: never re-run another, unacknowledged handler */
            if (!rr_link_inject_next()) break;
            rr_deliver_irqs(rr_hw_irq_level);
        }
    }
    /* a Debug-menu launch leaves the lifted call stack here (never returns if one is pending): AFTER the vblank interrupt,
     * whose handler rewinds the program's cooperative task list (Ace Driver: the pointer at A6-0x7FD0) -- launched before it,
     * the screen's first yield (0x4C6E) read past the end of the list and jumped to 0 */
    debug_boundary();
    if (g_rr_game->ffb_synth) {                         /* RR_FFBLOG=<file>: the synthesised wheel force per frame (headless never drives a wheel) */
        static FILE *fl; static int init;
        if (!init) { init = 1; const char *e = getenv("RR_FFBLOG"); if (e) fl = fopen(e, "w"); }
        if (fl) fprintf(fl, "%u %.3f %u\n", frame, g_rr_game->ffb_synth(), g_hw.steer);
    }
    if (dump_dir) {             /* RR_DUMP_EVERY=n (default 60), RR_DUMP_FROM=f: dump cadence */
        static unsigned every, from; static int init;
        if (!init) { const char *e = getenv("RR_DUMP_EVERY"), *f = getenv("RR_DUMP_FROM");
                     every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 60; from = f ? (unsigned)atoi(f) : 0; init = 1; }
        if ((frame >= from && frame % every == 0) || frame == max_frames) dump_state();
    }
    if (frame % 60 == 0)
        fprintf(stderr, "[RR] frame %u  traps %u  unmapped %u  romwrites %u  irqs %u/%u/%u/%u/%u/%u/%u  en %02X pc_sr %04X\n",
                frame, rr_n_traps, g_rr.n_unmapped, g_rr.n_romwrite, rr_n_irq[1], rr_n_irq[2], rr_n_irq[3], rr_n_irq[4], rr_n_irq[5], rr_n_irq[6], rr_n_irq[7],
                g_hw.irq_enabled, (unsigned)get_sr());
    if (frame % 60 == 0) { char b[256]; rr_dsp_debug(b, sizeof b); fprintf(stderr, "     %s\n", b); }
    if (frame >= max_frames) {
        dump_state();
        perf_report();
        rr_input_record_stop();
        rr_audio_close();
        fprintf(stderr, "[RR] stop at frame %u, %u traps\n", frame, rr_n_traps);
        exit(rr_n_traps ? 3 : 0);
    }
}

#ifdef _WIN32
void rr_win_startup(const char *logname);          /* src/rr_win.c */
#endif

/* LINK PLAY (engine/link_host.h): the chip's IRQ is syscon line 2; the game's own view of its link lives at A6 = 0x10008000
 * (docs/LINK_NOTES.md): +0x11A0 its cabinet number, +0x1104 who it hears (bit per cabinet; its own bit = its frame came back
 * within 8 frames), +0x1102 how many others it hears, +0x1108 frames since its own frame came back */
static void link_raise(void) { rr_hw_sci_irq(); }
static void link_deliver(void) { rr_deliver_irqs(rr_hw_irq_level); }
static bool link_good(void) { return rr_read(0x10009104u, 2) >> (rr_read(0x100091A0u, 2) & 15) & 1; }
static int  link_game_id(void) { return (int)(rr_read(0x100091A0u, 2) & 7); }
static void link_report(char *buf, int n)
{
    snprintf(buf, (size_t)n, "game: cabinet %u, hears %02X (%u in the ring), link %s, frames since own came back %u",
             rr_read(0x100091A0u, 2) + 1, rr_read(0x10009104u, 2), rr_read(0x10009102u, 2), link_good() ? "GOOD" : "down", rr_read(0x10009108u, 2));
}
/* another game on this board (rr_game_t link_*: Ace Driver): its own cabinet number; its link status is not decoded yet, so the
 * safety net goes by the network alone (good = NULL) */
static int  link_game_id_own(void) { return (int)(rr_read(g_rr_game->link_cab_addr, 2) & 7); }
static void link_report_own(char *buf, int n) { snprintf(buf, (size_t)n, "game: cabinet %u", rr_read(g_rr_game->link_cab_addr, 2) + 1); }
static void link_before_restart(void) { rr_hw_eeprom_save(); rr_audio_close(); rr_host_close(); }   /* the safety net's restart */
static link_host_game link_game = { "RR", link_raise, link_deliver, link_report, link_good, link_game_id, rr_ui_set_hint, link_before_restart };

/* our fork: a jump the lifted program has no instruction for. Into the game's copy of its program in work RAM (rr_game_t ram_code_*):
 * the same code in the program, when the copy still holds the program's bytes there. Then the SUPPLEMENT (rr_game_t supp_jump), the
 * code the shipped lifted program lacks. */
void rr_jump(uint32_t t, uint32_t at);
extern bool (*rr_trap_hook)(uint32_t at, uint32_t target, const char *what);
static bool unknown_jump(uint32_t at, uint32_t t, const char *what)
{
    const rr_game_t *g = g_rr_game;
    if (strncmp(what, "jump", 4)) return false;
    if (g->ram_code_size && t >= g->ram_code_at && t - g->ram_code_at + 16 <= g->ram_code_size) {
        const uint32_t r = g->ram_code_rom + (t - g->ram_code_at);
        bool same = true;
        for (uint32_t i = 0; i < 16; i += 2) if (rr_read(t + i, 2) != rr_read(r + i, 2)) same = false;   /* changed since the copy: not ours */
        if (same) { if (g->supp_jump && g->supp_jump(r, at)) return true; rr_jump(r, at); return true; }
    }
    return g->supp_jump && g->supp_jump(t, at);
}

/* THE LAST WORD (our fork, engine/lift_cpu.c rr_fatal_hook): the lifted program reached code it does not have and must stop -- or the
 * program itself crashed (a signal). Keep what is needed to find it -- this session's recording (it replays the crash) and the log, as
 * captures/crash_<date>_<time>.rec / .log, since the next start overwrites both -- save the EEPROM, and in a window say so instead of
 * just vanishing. */
const uint32_t *g_rr_last_pc;                  /* a game built with LIFT_LAST_PC points this at g_lift_pc (Victory Lap): the crash message names it */
static char fatal_log[64], fatal_title[64];
static int fatal_windowed;
static void copy_into(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"); if (!in) return;
    FILE *out = fopen(dst, "wb"); if (!out) { fclose(in); return; }
    char b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
    fclose(in); fclose(out);
}
static void on_fatal(const char *why)
{
    fflush(stderr);
    rr_hw_eeprom_save();
    char stamp[32]; const time_t t = time(NULL); strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&t));
    char r[80], l[80], kept[200] = "";
    snprintf(r, sizeof r, "captures/crash_%s.rec", stamp); snprintf(l, sizeof l, "captures/crash_%s.log", stamp);
    mkdir("captures", 0777);
    if (rr_input_record_copy(r)) {
        fprintf(stderr, "[RR] keeping the crash: %s\n", r); fflush(stderr);
        snprintf(kept, sizeof kept, "%s and %s", r, l);
    }
    if (fatal_log[0]) copy_into(fatal_log, l);
    if (!fatal_windowed) return;
    char msg[1024];
    snprintf(msg, sizeof msg, "%s stopped.\n\n%s\n\nPlease report it with %s -- they replay this session up to the crash.",
             fatal_title, why, kept[0] ? kept : (fatal_log[0] ? fatal_log : "the log file beside the program"));
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, fatal_title, msg, NULL);
}
static void on_signal(int sig)
{
    char why[160];
    if (g_rr_last_pc) snprintf(why, sizeof why, "The program crashed (signal %d) at frame %u, near instruction 0x%06X.", sig, rr_frame, *g_rr_last_pc);
    else snprintf(why, sizeof why, "The program crashed (signal %d) at frame %u.", sig, rr_frame);
    fprintf(stderr, "[RR] %s\n", why);
    signal(sig, SIG_DFL);
    if (rr_fatal_hook) { void (*h)(const char *) = rr_fatal_hook; rr_fatal_hook = NULL; h(why); }
    _exit(6);
}

int main(int argc, char **argv)
{
    eng_args_strip(&argc, argv);    /* --set KEY=VALUE: a setting for this run (engine/eng_args.h, GitHub #85) */
    int frames_given = 0;
    rr_trap_hook = unknown_jump;
    const int cab = link_host_prescan(argc, argv);    /* a linked copy: its own log (raveracer_cabN.log, acedriver_cabN.log) */
    char logname[64], title[64];
    {   /* the game's own names (rr_game_t title / log_file; Rave Racer's when it has none) */
        const char *lf = g_rr_game->log_file ? g_rr_game->log_file : "raveracer.log", *dot = strrchr(lf, '.');
        const int ln = dot ? (int)(dot - lf) : (int)strlen(lf);
        const char *t = g_rr_game->title ? g_rr_game->title : "Rave Racer";
        if (cab > 0) { snprintf(logname, sizeof logname, "%.*s_cab%d.log", ln, lf, cab); snprintf(title, sizeof title, "%s - Cabinet %d", t, cab); }
        else { snprintf(logname, sizeof logname, "%s", lf); snprintf(title, sizeof title, "%s", t); }
    }
    if (g_rr_game->link_cab_addr) {                   /* not Rave Racer: its own cabinet number, its own name on the network */
        link_game.tag = !strcmp(g_rr_game->name, "acedrive") ? "AD" : g_rr_game->name;
        link_game.report = link_report_own; link_game.good = NULL; link_game.game_id = link_game_id_own;
        link_game.max_cabinets = g_rr_game->link_max_cabinets;
    }
#ifdef _WIN32
    rr_win_startup(logname);        /* the program's folder, the log, DPI */
#endif
    const char *rom_dir = "extracted";
#ifdef RR_ORACLE
    int use_gl = -1;                            /* -1: GL in a window, the oracle headless */
#else
    int use_gl = 1;                             /* the game: always the engine */
#endif
    if (argc == 1) windowed = -1;               /* started with no arguments (a double-click): play */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) { max_frames = (uint32_t)atoi(argv[++i]); frames_given = 1; }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--ppf") && i + 1 < argc) polls_per_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shot_dir = argv[++i]; shot_every = (uint32_t)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--render-dump")) i += 3;
        else if (!strcmp(argv[i], "--fullscreen")) { if (!windowed) windowed = -1; setenv("RR_FULLSCREEN", "1", 1); }
        else if (!strcmp(argv[i], "--window")) {
            windowed = -1;                                      /* -1: the saved window size */
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9' && !argv[i + 1][1]) windowed = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--perf")) perf_on = 1;
        else if (!strcmp(argv[i], "--sndsweep") && i + 1 < argc) sweep_arg(argv[++i]);
        else if (!strcmp(argv[i], "--perflog") && i + 1 < argc) { perflog = fopen(argv[++i], "w"); perf_on = perflog != NULL; }
        else if (!strcmp(argv[i], "--gl")) use_gl = 1;
#ifdef RR_ORACLE
        else if (!strcmp(argv[i], "--sw")) use_gl = 0;
#endif
        else if (!strcmp(argv[i], "--freeplay")) freeplay = 1;
        else if (!strcmp(argv[i], "--coins")) freeplay = 0;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec_path = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) rep_path = argv[++i];
        else if (!strcmp(argv[i], "--joytest")) return rr_host_joytest();
        else if (!strcmp(argv[i], "--write-controls")) return rr_input_write(g_rr_game->cfg_file) ? 0 : 1;
        else if (!strcmp(argv[i], "--coin") && i + 1 < argc) {
            for (char *q = argv[++i]; *q && n_test_coins < 8; ) { char *e; long v = strtol(q, &e, 0); if (e == q) break; test_coins[n_test_coins++] = (int32_t)v; q = *e == ',' ? e + 1 : e; }
            test_coin = n_test_coins ? test_coins[0] : -1;
        }
        else if (!strcmp(argv[i], "--gas") && i + 1 < argc) test_gas = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--adc") && i + 1 < argc && n_adc < 32) {
            int f, k, v; if (sscanf(argv[++i], "%d:%d:%i", &f, &k, &v) == 3) { test_adc[n_adc].at = f; test_adc[n_adc].idx = k; test_adc[n_adc].value = v; n_adc++; }
        }
        else if (!strcmp(argv[i], "--steer") && i + 1 < argc && n_steer < 8) {
            int f, v; if (sscanf(argv[++i], "%d:%i", &f, &v) == 2) { test_steer[n_steer].at = f; test_steer[n_steer].value = v; n_steer++; }
        }
        else if (link_host_arg(argc, argv, &i)) {}
        else rom_dir = argv[i];
    }
#ifdef RR_ORACLE
    { const char *e = getenv("RR_RENDER");
      if (e && !strcmp(e, "sw")) use_gl = 0;
      if (e && !strcmp(e, "gl")) use_gl = 1; }
#endif
    for (int i = 1; i + 3 < argc; i++)          /* --render-dump DIR FRAME OUT.ppm: renderer gate, no CPU */
        if (!strcmp(argv[i], "--render-dump")) {
            if (!rr_dsp_init(rom_dir)) return 2;
#ifdef RR_ORACLE
            if (use_gl != 1) return rr_video_init(rom_dir) && rr_video_render_dump(argv[i + 1], atoi(argv[i + 2]), argv[i + 3]) ? 0 : 1;
#endif
            /* the captured state through the engine's pipeline */
            if (!rr_scene_load_capture(argv[i + 1], atoi(argv[i + 2])) || !rr_gl_init(rom_dir) || !rr_gl_open_headless(640, 480)) return 2;
            rr_scene_force_walk();
            rr_gl_prepare(true);
            rr_gl_draw(640, 480);
            fprintf(stderr, "[GL] render-dump: %d quads\n", rr_gl_quads());
            return rr_gl_write_ppm(argv[i + 3], 640, 480) ? 0 : 1;
        }
    static char last_rec[64];                     /* a played session always keeps its latest recording (and the one before): a bug a player
                                                   * meets can be replayed exactly -- <nv prefix>_last.rec, e.g. rr_last.rec (GitHub #62) */
    const bool rec_on = windowed && !rec_path && !rep_path && link_host_record_sessions();   /* our fork: link.cfg record=off turns it off */
    if (windowed && !rec_path && !rep_path && !rec_on) fprintf(stderr, "[RR] session recording off (link.cfg record=off)\n");
    if (rec_on) {
        const char *u = strchr(g_rr_game->nv_file, '_'); const int k = u ? (int)(u - g_rr_game->nv_file) : 2;
        char pre[24]; if (cab > 0) snprintf(pre, sizeof pre, "%.*s_cab%d", k, g_rr_game->nv_file, cab); else snprintf(pre, sizeof pre, "%.*s", k, g_rr_game->nv_file);
        char prev[64]; snprintf(last_rec, sizeof last_rec, "%s_last.rec", pre); snprintf(prev, sizeof prev, "%s_prev.rec", pre);   /* our fork: a linked copy its own (rr_cab2_last.rec) */
        remove(prev); rename(last_rec, prev); rec_path = last_rec;
    }
    if (rep_path && !rr_input_replay_open(rep_path)) return 2;
    if (rec_path && !rr_input_record_start(rec_path)) return 2;
    snprintf(fatal_log, sizeof fatal_log, "%s", logname); snprintf(fatal_title, sizeof fatal_title, "%s", title); fatal_windowed = windowed != 0;
    rr_fatal_hook = on_fatal;                         /* our fork: keep a crash's recording + log, and say so (on_fatal) */
    signal(SIGSEGV, on_signal); signal(SIGILL, on_signal); signal(SIGFPE, on_signal); signal(SIGABRT, on_signal);
    g_rr_gl = use_gl < 0 ? (windowed != 0) : use_gl;
    if (windowed) {
        if (!rr_host_open(windowed > 0 ? windowed : 0)) return 2;
        if (!frames_given) max_frames = 0xFFFFFFFFu;    /* a window plays until it is closed, unless --frames says */
    } else if (g_rr_gl) {
        const char *e = getenv("RR_RENDER_SIZE"); int w, h;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 48) { gl_w = w; gl_h = h; }
        /* no GL here: the game still runs (sound, traces, dumps), without a picture */
        gl_ok = rr_gl_open_headless(gl_w, gl_h);
        if (!gl_ok && shot_dir) { fprintf(stderr, "[RR] --shots needs OpenGL\n"); return 2; }
    }
    /* First run: take the ROMs out of MAME's raverace.zip + namcoc74.zip if the ROM
     * folder is incomplete (src/rr_romzip.c) -- how the Windows build is set up;
     * chips unzipped loose into roms/ work too. */
    if (g_rr_game->autosetup && rr_romzip_missing(rom_dir) && !strcmp(rom_dir, "extracted") && !rr_romzip_missing("roms"))
        rom_dir = "roms";
    if (g_rr_game->roms) {                       /* a game with a chip table (Ace Driver): the engine's table-driven unpacker */
        if (eng_romzip_missing(rom_dir, g_rr_game->roms, g_rr_game->nroms) && !strcmp(rom_dir, "extracted") &&
            !eng_romzip_missing("roms", g_rr_game->roms, g_rr_game->nroms))
            rom_dir = "roms";
        if (eng_romzip_missing(rom_dir, g_rr_game->roms, g_rr_game->nroms)) {
            char err[512], *base = SDL_GetBasePath();
            if (!eng_romzip_autosetup(rom_dir, base, g_rr_game->zips, g_rr_game->nzips, g_rr_game->roms, g_rr_game->nroms, err, sizeof err)) {
                char zl[256] = ""; for (int z = 0; z < g_rr_game->nzips; z++) { strncat(zl, z ? (z + 1 == g_rr_game->nzips ? " and " : ", ") : "", sizeof zl - strlen(zl) - 1); strncat(zl, g_rr_game->zips[z], sizeof zl - strlen(zl) - 1); }
                fprintf(stderr, "%s needs its ROMs (%s): %s\n", g_rr_game->title, zl, err);
                if (windowed) {
                    char msg[1024];
                    snprintf(msg, sizeof msg, "%s needs its ROMs.\n\nPut %s (the MAME ROM sets) in the \"roms\" folder next to this program, "
                             "then start it again.\n\n(%s)", g_rr_game->title, zl, err);
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g_rr_game->title, msg, NULL);
                }
                SDL_free(base);
                return 2;
            }
            SDL_free(base);
        }
    }
    if (g_rr_game->autosetup && rr_romzip_missing(rom_dir)) {
        char err[512], *base = SDL_GetBasePath();
        if (!rr_romzip_autosetup(rom_dir, base, err, sizeof err)) {
            fprintf(stderr, "Rave Racer needs its ROMs: %s\n", err);
            if (windowed) {
                char msg[1024];
                snprintf(msg, sizeof msg,
                         "Rave Racer needs its ROMs.\n\n"
                         "Put raverace.zip and namcoc74.zip (the MAME ROM sets) in the \"roms\" "
                         "folder next to this program, then start it again.\n\n(%s)", err);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rave Racer", msg, NULL);
            }
            SDL_free(base);
            return 2;
        }
        SDL_free(base);
    }
    if (!rr_load_program(rom_dir)) return 2;
    rr_audio_init(rom_dir);
    rr_sound_init(rom_dir);
    rr_hw_init(rom_dir);
#ifdef RR_TRACE
    rr_env_init();                         /* RR_ENV: the trace oracle's environment (dev builds) */
#endif
    /* the test menu's settings and the records survive a restart (a window or a linked copy; headless test runs and replays start from
     * the set's defaults / the recording's own EEPROM, and never save) */
    char nvname[64];
    if (cab > 0) { const char *dot = strrchr(g_rr_game->nv_file, '.');   /* (never shared between games: rr_eeprom_cab1.nv, ad_eeprom_cab1.nv) */
                   snprintf(nvname, sizeof nvname, "%.*s_cab%d.nv", dot ? (int)(dot - g_rr_game->nv_file) : (int)strlen(g_rr_game->nv_file), g_rr_game->nv_file, cab); }
    else snprintf(nvname, sizeof nvname, "%s", g_rr_game->nv_file);
    const bool nv_loaded = windowed && !rep_path ? rr_hw_eeprom_persist(nvname) : false;
    if (cab > 0) {
        if (!link_host_open(&link_game)) {
            if (windowed) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g_rr_game->title, "The link could not start.\n\nUsually another copy of the game is still running as the same cabinet (it may be hidden, e.g. the automatic check): close it, or restart the PC, and try again.\n\nDetails are in the log file beside the program.", NULL);
            return 2;
        }
        rr_hw_link_on(link_host_id(), link_host_take_id_reset() || !nv_loaded);   /* the ID: first run, or a new number on the Network page */
    }
    if (freeplay >= 0) rr_hw_set_freeplay(freeplay);
    else if (windowed && g_cfg_freeplay >= 0) {        /* the saved menu choice, windowed runs only */
        rr_hw_set_freeplay(g_cfg_freeplay);
        fprintf(stderr, "[RR] %s (rr_controls.cfg)\n", g_cfg_freeplay ? "free play" : "coins required");
    }
    if (windowed && g_cfg_car >= 0 && g_rr_game->car_set && g_cfg_car < g_rr_game->ncars) {   /* the saved Car choice, windowed runs only */
        g_rr_game->car_set(g_cfg_car);
        fprintf(stderr, "[RR] car %d %s (controls cfg)\n", g_cfg_car + 1, g_rr_game->cars[g_cfg_car]);
    }
    if (windowed) rr_hw_set_steering_motor(g_cfg_ffb_strength > 0);   /* the game only drives the motor with its own option ON */
    if (rep_path) rr_input_replay_eeprom(g_rr.eeprom);   /* a replay: the session's own EEPROM */
    rr_input_record_eeprom(g_rr.eeprom);                  /* a recording: the EEPROM this session starts from */
    { const char *e = getenv("RR_LINK_CABINET");              /* temporary: the lobby/config will drive this */
      if (e) rr_hw_set_link_cabinet(atoi(e)); }
    if (g_cfg_net_name[0]) rr_net_set_name(g_cfg_net_name);        /* the saved lobby name (windowed: from rr_controls.cfg) */
    if (windowed && g_cfg_net_server[0]) rr_net_preset_server(g_cfg_net_server);  /* remembered, but connecting stays a menu action */
    { const char *srv = getenv("RR_NET_SERVER");            /* headless/test bootstrap: connect at boot (env wins over cfg) */
      if (srv && *srv) {
          const char *nm = getenv("RR_NET_NAME");
          if (nm && *nm) rr_net_set_name(nm);
          if (rr_net_set_server(srv)) rr_net_connect();
      }
      const char *hst = getenv("RR_NET_HOST");                /* headless/test: host a LAN game (the built-in server) */
      if (hst && *hst == '1') {
          const char *nm = getenv("RR_NET_NAME"); if (nm && *nm) rr_net_set_name(nm);
          rr_net_host_start();
      }
      const char *dsc = getenv("RR_NET_DISCOVER");            /* headless/test: search the LAN and join the first host */
      if (dsc && *dsc == '1') {
          const char *nm = getenv("RR_NET_NAME"); if (nm && *nm) rr_net_set_name(nm);
          rr_net_discover_autojoin(1); rr_net_discover();
      } }
#ifdef RR_ORACLE
    if (!g_rr_gl && !rr_video_init(rom_dir)) fprintf(stderr, "[RR] video ROMs missing\n");
#endif
    if (g_rr_gl) {
        if (!rr_gl_init(rom_dir)) { fprintf(stderr, "[RR] video ROMs missing\n"); return 2; }
        if (windowed) gl_ok = true;             /* the window's context */
    }
    memset(R, 0, sizeof R);
    RS4(REG_SP, rr_read(0, 4));                 /* reset SP from vector 0 */
    set_sr(0x2700);                             /* supervisor, IPL 7 */
    rr_budget = polls_per_frame / SLICES;
    fprintf(stderr, "[RR] reset: SP=%08X PC=%08X\n", (uint32_t)RG4(REG_SP), rr_read(4, 4));
    rr_call_push(0xFFFFFFFEu);                  /* bottom of the shadow stack */
    { extern void rd_init(void); rd_init(); }   /* readable-C replacements (src/rd) */
    if (g_link_mute) rr_audio_set_volume(0);
    link_host_gather(title, windowed ? SDL_GL_GetCurrentWindow() : NULL, windowed ? rr_ui_set_hint : NULL, windowed ? rr_host_message : NULL);   /* the cabinets start together, as boards switched on together */
    g_rr_game->entry();                         /* entry_reset (this game's reset PC): never returns */
    fprintf(stderr, "[RR] entry_reset returned?!\n");
    return 1;
}
