#include <setjmp.h>
/*
 * s23_main.c -- run Time Crisis 2's lifted program from a MAME state snapshot (tools/mame/snapshot.lua): the first milestone of the host.
 *
 *   tc2 SNAPSHOT_PREFIX [INSTRUCTIONS]      e.g.  build/tc2 work/snap_f1500 5000000
 *   tc2 reset [INSTRUCTIONS]                from POWER-ON: the boot monitor at the reset vector, no snapshot
 *
 * Loads boot.bin (tools/make_prog.py), SNAPSHOT.ram (16 MB) and SNAPSHOT.regs (MAME's register names, include/s23_regmap.h), enters the lifted
 * code at the snapshot's PC and runs INSTRUCTIONS instructions (default 3,000,000 ~ one frame). The trace build (-DRR_TRACE) writes every
 * executed PC to $S23_TRACE (default s23_trace.txt), with "IRQ cause" / "ERET" markers, to compare with MAME's own trace from the same point.
 * Devices: src/s23_ctl.c, s23_chips.c, s23_screen.c (ours); the rest replayed from MAME ($S23_ENV). $S23_IOLOG logs every device access.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "lift_rt.h"
#include "s23_lifted.h"
#include "s23_host.h"
#include "s23_game.h"
#include "s23_regmap.h"
#include <SDL2/SDL.h>

extern uint8_t s23_ram[0x1000000], s23_boot[0x800000];
void s23_io_flush(void);

static uint64_t ins_total, ins_limit = 3000000;

static int load(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[S23] cannot open %s\n", path); return 0; }
    size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[S23] %s: %zu of %zu bytes\n", path, got, n); return 0; }
    return 1;
}

static uint32_t load_regs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[S23] cannot open %s\n", path); exit(1); }
    char name[64]; char hex[64]; uint32_t pc = 0; int n = 0;
    while (fscanf(f, "%63s %63s", name, hex) == 2) {
        const uint64_t v = strtoull(hex, NULL, 16);
        if (!strcmp(name, "PC")) { pc = (uint32_t)v; continue; }
        for (size_t i = 0; i < sizeof s23_regmap / sizeof s23_regmap[0]; i++)
            if (!strcmp(s23_regmap[i].name, name)) {
                if (s23_regmap[i].size == 8) RS8(s23_regmap[i].off, v); else RS4(s23_regmap[i].off, v);
                n++; break;
            }
    }
    fclose(f);
    fprintf(stderr, "[S23] %d registers from %s, PC %08X\n", n, path, pc);
    return pc;
}

#ifdef RR_TRACE
static FILE *trace_f;
static unsigned trace_from;                   /* $S23_TRACE_FROM: the first frame traced */
void rr_trace_ins(uint32_t pc) { if (trace_f && rr_frame >= trace_from) fprintf(trace_f, "%08X\n", pc); }
void s23_trace_mark(const char *what, uint32_t v) { if (trace_f) fprintf(trace_f, "%s %08X\n", what, v); }
#else
void s23_trace_mark(const char *what, uint32_t v) { (void)what; (void)v; }
#endif

void s23_on_exit(void)
{
    { void s23_out_close(void); s23_out_close(); }                  /* "mame_stop = 1" to the output listeners (src/s23_out.c) */
#ifdef RR_TRACE
    if (trace_f) fclose(trace_f);
#endif
    { void s23_backup_save(int); s23_backup_save(1); }
    s23_io_flush();
    { void s23_h8_report(void); s23_h8_report(); }
    { void s23_c422_report(void); s23_c422_report(); }
    { void s23_prof_dump(void); s23_prof_dump(); }                  /* $S23_PROFILE (src/s23_prof.c) */
    { extern unsigned long s23_render_frames, s23_render_models, s23_render_immediate, s23_c435_unknown;
      extern unsigned long s23_render_polys; fprintf(stderr, "[3D] %lu frames: %lu model entries, %lu immediate polygons, %lu polygons drawn; %lu unknown C435 commands\n", s23_render_frames, s23_render_models, s23_render_immediate, s23_render_polys, s23_c435_unknown); }
    extern unsigned long s23_env_hits, s23_env_miss;
    extern uint32_t rr_n_fpe;
    fprintf(stderr, "[S23] %llu instructions, %u frames, %llu interrupts, %u traps, %u FP exceptions (FCSR %08X); replay: %lu device reads replayed, %lu not in the recording\n",
            (unsigned long long)ins_total, rr_frame, (unsigned long long)rr_n_irq, rr_n_traps, rr_n_fpe, (unsigned)RG4(0x1210), s23_env_hits, s23_env_miss);
}
void s23_on_trap(void) { if (getenv("S23_STOP_ON_TRAP")) { s23_on_exit(); exit(3); } }

/* a frame is complete (the start of vertical blank): S23_SHOT=path:frame writes the text layer as a PPM there */
void s23_text_render(uint32_t *rgba);
/* $S23_PRESS = "bits@frame+frames,..." (headless runs): cabinet inputs held for a stretch -- the bits of src/s23_input.c (1 coin, 4 service,
 * 8 test, 0x10 trigger, 0x20 pedal, 0x40 / 0x80 operator up / down, 0x100 operator enter) */
#ifdef _WIN32
#include <malloc.h>
#endif
static void scripted_inputs(uint32_t frame)
{
#ifdef _WIN32
    { static int hc = -1; if (hc < 0) hc = getenv("S23_HEAPCHK") != NULL;   /* debug: the C heap checked every frame -- the first frame it is damaged */
      if (hc) { const int r = _heapchk(); static int said; if (r != _HEAPOK && !said++) { fprintf(stderr, "[HEAP] damaged (%d) at frame %u\n", r, frame); fflush(stderr); } } }
#endif
    static const char *spec; static int init;
    if (!init) { S23_SS_ENV(init); S23_SS_ENV(spec); init = 1; spec = getenv("S23_PRESS"); }
    if (!spec) return;
    extern uint16_t s23_inputs;
    uint16_t held = 0;
    for (const char *p = spec; *p; ) {
        unsigned bits = 0, at = 0, len = 1; int n = 0;
        if (sscanf(p, "%x@%u+%u%n", &bits, &at, &len, &n) < 2) break;
        if (frame >= at && frame < at + len) held |= (uint16_t)bits;
        p += n ? n : 1; while (*p && *p != ',') p++; if (*p == ',') p++;
    }
    s23_inputs = held;
    /* $S23_AIM = "x:y@frame,..." (fractions of the screen, 0..1): the gun aimed there from that frame on -- picks a ZONE SELECT entry headlessly */
    static const char *aim; static int ainit;
    if (!ainit) { ainit = 1; aim = getenv("S23_AIM"); }
    for (const char *p = aim; p && *p; ) {
        float x, y; unsigned at; int n = 0;
        if (sscanf(p, "%f:%f@%u%n", &x, &y, &at, &n) < 3) break;
        if (frame >= at) { extern uint16_t g_ss22_gun_x, g_ss22_gun_y;
            g_ss22_gun_x = (uint16_t)(s23_game.gun_x0 + x * s23_game.gun_xw); g_ss22_gun_y = (uint16_t)(s23_game.gun_y0 + y * s23_game.gun_yh); }
        p += n ? n : 1; while (*p && *p != ',') p++; if (*p == ',') p++;
    }
    /* the ASCA-3 board's inputs (Angler King's rod / arm / reel, src/s23_h8.c): $S23_ANALOG = "ch:value@frame,..." (value hex, MAME's 16-bit
     * JVS value, held from that frame on); $S23_REEL = "counts@frame+frames,..." (the reel wound by that many counts a frame: s23_reel_wind) */
    static const char *an, *rl; static int rinit;
    if (!rinit) { rinit = 1; an = getenv("S23_ANALOG"); rl = getenv("S23_REEL"); }
    extern uint16_t s23_analog_in[8];
    for (const char *p = an; p && *p; ) {
        unsigned ch, v, at; int n = 0;
        if (sscanf(p, "%u:%x@%u%n", &ch, &v, &at, &n) < 3) break;
        if (frame == at && ch < 8) s23_analog_in[ch] = (uint16_t)v;
        p += n ? n : 1; while (*p && *p != ',') p++; if (*p == ',') p++;
    }
    for (const char *p = rl; p && *p; ) {
        int c = 0; unsigned at = 0, len = 1; int n = 0;
        if (sscanf(p, "%d@%u+%u%n", &c, &at, &len, &n) < 2) break;
        if (frame >= at && frame < at + len) { void s23_reel_wind(int); s23_reel_wind(c); }
        p += n ? n : 1; while (*p && *p != ',') p++; if (*p == ',') p++;
    }
}

/* THE GPU GATE: $S23_GPU_GATE=dir:every -- a hidden GL context in a headless run; every N frames the same built frame is drawn by the GPU
 * back end (src/s23_gl.c) and by the software one (the oracle), both written as PPMs, and the difference reported */
#include <SDL2/SDL.h>
#include "s23_build.h"
#include "eng/eng_args.h"
#include "eng/win_gpu.h"   /* Windows: run on the discrete GPU of a two-GPU laptop */
static void ppm(const char *path, const uint32_t *px)
{
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < 640 * 480; i++) { const uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}
static void gpu_gate(uint32_t frame)
{
    static int init; static char dir[512]; static unsigned every;
    if (!init) {
        init = 1; const char *e = getenv("S23_GPU_GATE"); if (!e) return;
        snprintf(dir, sizeof dir, "%s", e); char *c = strrchr(dir, ':'); if (!c) return; *c = 0; every = (unsigned)atoi(c + 1);
        SDL_Init(SDL_INIT_VIDEO);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_Window *w = SDL_CreateWindow("tc2 gate", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!w || !SDL_GL_CreateContext(w) || !s23_gl_init()) { fprintf(stderr, "[GATE] no GPU renderer: %s\n", SDL_GetError()); every = 0; }
    }
    static long from = -1; if (from < 0) { const char *f = getenv("S23_GATE_FROM"); from = f ? atol(f) : 0; }   /* first frame to draw */
    if (!every || frame % every || (long)frame < from) return;
    const s23_built *b = s23_built_latest_wait(); if (!b) return;
    static uint32_t s[640 * 480], g[640 * 480];
    s23_soft_raster(b, s);
    s23_gl_draw(b); s23_gl_read(g);
    long sum = 0, big = 0, diff = 0; int mx = 0;
    for (int i = 0; i < 640 * 480; i++) {
        int d = 0;
        for (int sh = 0; sh <= 16; sh += 8) { const int x = abs((int)(s[i] >> sh & 0xFF) - (int)(g[i] >> sh & 0xFF)); sum += x; if (x > d) d = x; }
        if (d) diff++; if (d > 16) big++; if (d > mx) mx = d;
    }
    fprintf(stderr, "[GATE] frame %u: %d polygons; mean %.3f/255, %ld px differ, %ld px > 16 (%.3f%%), max %d\n", frame, b->npoly,
            sum / (640.0 * 480 * 3), diff, big, 100.0 * big / (640 * 480), mx);
    char p[700];
    snprintf(p, sizeof p, "%s/soft_%05u.ppm", dir, frame); ppm(p, s);
    snprintf(p, sizeof p, "%s/gpu_%05u.ppm", dir, frame); ppm(p, g);
    if (getenv("S23_GATE_WIDEPOLYS")) {
        int st = 0, bl = 0, pv[8] = { 0 };
        for (int i = 0; i < b->npoly; i++) { st += b->polys[i].stencil; bl += b->polys[i].blend; pv[b->polys[i].prioverchar & 7]++; }
        fprintf(stderr, "[GATE]  %d stencil, %d blend, prioverchar 0:%d 2:%d 7:%d, %d entries\n", st, bl, pv[0], pv[2], pv[7], b->fs.list.count);
    }
    if (getenv("S23_GATE_WIDEPOLYS"))                          /* polygons as wide as the 4:3 screen: what they are */
        for (int i = 0; i < b->npoly; i++) {
            const s23_poly *q = &b->polys[b->order[i]];
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int k = 0; k < q->n; k++) { const float x = q->pv[k].x + (320 - q->vp_size_x) + q->vp_offset_x, y = q->pv[k].y + (240 - q->vp_size_y) - q->vp_offset_y;
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            if ((q->stencil || x1 - x0 > (getenv("S23_GATE_WIDEMIN") ? atof(getenv("S23_GATE_WIDEMIN")) : 600) && y0 > -50 && y1 < 530)) fprintf(stderr, "[GATE]  poly %d: n %d x %.1f..%.1f y %.1f..%.1f pal %04X cmode %d tbase %05X zkey %06X vp %d,%d %d,%d st %d uv0 %.1f,%.1f uv2 %.1f,%.1f\n", i, q->n, x0, x1, y0, y1,
                                       q->color_base, q->cmode, q->tbase, q->zkey, q->vp_size_x, q->vp_size_y, q->vp_offset_x, q->vp_offset_y, q->stencil, q->pv[0].p[1]/q->pv[0].p[0], q->pv[0].p[2]/q->pv[0].p[0], q->pv[2].p[1]/q->pv[2].p[0], q->pv[2].p[2]/q->pv[2].p[0]);
        }
    if (getenv("S23_GATE_FOG")) {                              /* the fog inputs the game set: C404 fog colour and the C412 PCZ table */
        uint16_t *s23_c412_pcz(void); const uint16_t *z = s23_c412_pcz(); int nz = 0;
        for (int i = 0; i < 0x200; i++) nz += z[i] != 0;
        fprintf(stderr, "[GATE]  fog RGB %02X %02X %02X; PCZ %d/512 non-zero:", b->fs.c404[11], b->fs.c404[13], b->fs.c404[15], nz);
        for (int i = 0; i < 0x200; i += 32) fprintf(stderr, " %04X", z[i]);
        fprintf(stderr, "\n");
        { FILE *f = fopen("work/pcz.txt", "w"); if (f) { for (int i = 0; i < 0x200; i++) fprintf(f, "%d %d\n", i, z[i]); fclose(f); } }   /* the whole table, for study */
    }
    if (getenv("S23_GATE_TEXTMASK")) {                         /* where the text layer is opaque: white */
        for (int i = 0; i < 640 * 480; i++) g[i] = (b->mix[i] & 0x8000) ? (b->fs.pens[b->mix[i] & 0x7FFF] | 0x404040) : 0x000040;
        snprintf(p, sizeof p, "%s/text_%05u.ppm", dir, frame); ppm(p, g);
    }
    if (s23_gl_scale() > 1 || s23_gl_wide()) {                                   /* and the GPU picture at its full internal resolution */
        static uint8_t full[(640 + 800) * 4 * 1920 * 3]; const int fh = 480 * s23_gl_scale();
        const int w = s23_gl_read_full(full, (int)sizeof full);
        snprintf(p, sizeof p, "%s/gpufull_%05u.ppm", dir, frame);
        FILE *f = w ? fopen(p, "wb") : NULL;
        if (f) { fprintf(f, "P6\n%d %d\n255\n", w, fh); fwrite(full, 1, (size_t)w * fh * 3, f); fclose(f); }
        const s23_built *m = s23_built_mid_count() > 0 ? s23_built_mid(0) : NULL;   /* $S23_SMOOTH=1: the GPU's in-between picture too */
        if (m) { s23_gl_draw(m); const int w2 = s23_gl_read_full(full, (int)sizeof full);
            snprintf(p, sizeof p, "%s/gpumid_%05u.ppm", dir, frame); FILE *f2 = w2 ? fopen(p, "wb") : NULL;
            if (f2) { fprintf(f2, "P6\n%d %d\n255\n", w2, fh); fwrite(full, 1, (size_t)w2 * fh * 3, f2); fclose(f2); } }
    }
}
/* THE SESSION RECORDER: a window session writes every change of the cabinet inputs (the buttons, the gun's x / y / off-screen) with its
 * frame number to tc2_last.rec (the previous session is kept as tc2_prev.rec); $S23_REPLAY=<file> plays one back in a headless run --
 * the same inputs at the same frames, so a bug a player reached (a sub-CPU trap in stage 2...) is reproduced exactly. Lines:
 *   F <frame> <inputs hex> <gun x> <gun y> <off>       (the state from the end of that frame's vertical blank on) */
extern uint16_t s23_inputs, g_ss22_gun_x, g_ss22_gun_y;
extern _Bool g_ss22_gun_off;
static void session_rec(uint32_t frame)
{
    static FILE *rec, *rep; static int init; static char line[128]; static int have;
    if (!init) {
        init = 1;
        const char *r = getenv("S23_REPLAY");
        if (r) { rep = fopen(r, "r"); fprintf(stderr, rep ? "[REC] replaying %s\n" : "[REC] cannot read %s\n", r); }
        else { int s23_window_active(void), s23_c422_record(void);
               if (s23_window_active() && !s23_c422_record()) fprintf(stderr, "[REC] session recording off (tc2_link.cfg record=off)\n");
               else if (s23_window_active()) {               /* <tag>_last.rec / <tag>_prev.rec: s23_..., crszone_... (the game table's tag) */
                   char last[64], prev[64]; snprintf(last, sizeof last, "%s_last.rec", s23_game.tag); snprintf(prev, sizeof prev, "%s_prev.rec", s23_game.tag);
                   remove(prev); rename(last, prev); rec = fopen(last, "w");   /* (Windows rename does not replace: the last session was lost) */
                   if (rec) { fprintf(rec, "# TC2 session: F frame inputs gunx guny off\n"); fprintf(stderr, "[REC] recording the inputs to %s\n", last); } } }
    }
    if (rep) {
        for (;;) {
            if (!have) { if (!fgets(line, sizeof line, rep)) { fclose(rep); rep = NULL; fprintf(stderr, "[REC] replay ends at frame %u\n", frame); return; } have = 1; }
            if (line[0] != 'F') { have = 0; continue; }
            unsigned f, in, gx, gy, off;
            if (sscanf(line, "F %u %x %u %u %u", &f, &in, &gx, &gy, &off) != 5) { have = 0; continue; }
            if (f > frame) return;
            s23_inputs = (uint16_t)in; g_ss22_gun_x = (uint16_t)gx; g_ss22_gun_y = (uint16_t)gy; g_ss22_gun_off = off != 0;
            have = 0;
        }
    }
    if (rec) {
        static uint16_t pin = 0xFFFF, px, py; static int poff = -1;
        if (s23_inputs != pin || g_ss22_gun_x != px || g_ss22_gun_y != py || (int)g_ss22_gun_off != poff) {
            fprintf(rec, "F %u %04X %u %u %d\n", frame, s23_inputs, g_ss22_gun_x, g_ss22_gun_y, (int)g_ss22_gun_off);
            fflush(rec);                                   /* a crash or ^C keeps everything up to here */
            pin = s23_inputs; px = g_ss22_gun_x; py = g_ss22_gun_y; poff = g_ss22_gun_off;
        }
    }
}
void s23_on_vblank(uint32_t frame)
{
    scripted_inputs(frame);
    { void s23_subs_frame(uint32_t); s23_subs_frame(frame); }   /* English subtitles: which caption is up (src/s23_subs.c) */
    { static int on = -1; static uint64_t t; if (on < 0) { on = getenv("S23_FRAMETIME") != NULL; t = SDL_GetPerformanceCounter(); }   /* $S23_FRAMETIME: ms per 30 frames */
      if (on && frame % 30 == 0) { const uint64_t n = SDL_GetPerformanceCounter(); fprintf(stderr, "[TIME] f%u %.0f ms / 30 frames\n", frame, (double)(n - t) * 1000.0 / (double)SDL_GetPerformanceFrequency()); t = n; } }
    { static int rt = -1; static uint64_t t0; static double hz;    /* $S23_REALTIME=1 (tests, our fork): a headless run held to the board's 59.906 Hz, as a cabinet;
                                                                    * =1.002: 0.2% fast (a PC whose clock runs fast) */
      if (rt < 0) { const char *e = getenv("S23_REALTIME"); rt = e != NULL; hz = 59.906 * (e && atof(e) > 0.5 ? atof(e) : 1.0); t0 = SDL_GetPerformanceCounter(); }
      if (rt) { const uint64_t f = SDL_GetPerformanceFrequency(), due = t0 + (uint64_t)((double)frame * (double)f / hz);
                for (uint64_t now; (now = SDL_GetPerformanceCounter()) < due; ) if (due - now > f / 500) SDL_Delay(1); } }
    { static int rf = -2, rt, re; static char rd[512];        /* $S23_RAMDUMP=dir:from-to:every (tests, our fork): the first 4 MB of RAM, r<frame>.bin */
      if (rf == -2) { S23_SS_ENV(rf); rf = -1; const char *e = getenv("S23_RAMDUMP"); if (e) { snprintf(rd, sizeof rd, "%s", e); char *c = strrchr(rd, ':'); char *c2 = c ? (*c = 0, strrchr(rd, ':')) : NULL;
                      if (c && c2) { *c2 = 0; re = atoi(c + 1); if (sscanf(c2 + 1, "%d-%d", &rf, &rt) != 2) rf = -1; } } }
      if (rf >= 0 && (int)frame >= rf && (int)frame <= rt && re > 0 && ((int)frame - rf) % re == 0) {
          char p[600]; snprintf(p, sizeof p, "%s/r%05u.bin", rd, frame); FILE *f = fopen(p, "wb"); if (f) { fwrite(s23_ram, 1, 0x400000, f); fclose(f); } } }
    { static int tf = -2; static const char *tp; if (tf == -2) { const char *e = getenv("S23_TEXTDUMP"); tf = -1; if (e) { tf = atoi(e); tp = strchr(e, ':'); if (tp) tp++; } }
      if (tf >= 0 && (int)frame == tf && tp) {   /* $S23_TEXTDUMP=frame:file -- the C361 character RAM + tile map (0x06800000-0x0681FFFF), big-endian */
          uint8_t *s23_vram_ptr(uint32_t); FILE *f = fopen(tp, "wb"); if (f) { fwrite(s23_vram_ptr(0x06800000u), 1, 0x20000, f); fclose(f); } } }
    static int pclog = -1; if (pclog < 0) pclog = getenv("S23_PCLOG") != NULL;
    if (pclog) fprintf(stderr, "[PC] frame %u pc %08X status %08X cause %08X\n", frame, rr_pc, (uint32_t)RG8(S23_R_STATUS), (uint32_t)RG8(S23_R_CAUSE));
    { void s23_render_frame_end(void); s23_render_frame_end(); }   /* the picture: background, text, the 3D render list (src/s23_render.c) */
    { void s23_out_poll(uint16_t); uint8_t s23_h8_outputs(void); s23_out_poll(s23_h8_outputs()); }   /* the gun recoil out (src/s23_out.c) */
    { void s23_window_frame(void); s23_window_frame(); }
    if (rr_frame % 300 == 0) { void s23_backup_save(int); s23_backup_save(0); }   /* the backup RAM to its file when it changed */
    session_rec(frame);                                     /* after the window set this frame's inputs: record them, or replay */
    gpu_gate(frame);
    static const char *shot; static unsigned long at; static int init;
    if (!init) { init = 1; const char *e = getenv("S23_SHOT"); if (e) { static char path[512]; snprintf(path, sizeof path, "%s", e);
                 char *c = strrchr(path, ':'); if (c) { *c = 0; at = strtoul(c + 1, NULL, 0); shot = path; } } }
    { static const char *dir; static unsigned every; static int i2;          /* $S23_SHOTS=dir:every -- a PPM every N frames */
      if (!i2) { S23_SS_ENV(i2); S23_SS_ENV(dir); S23_SS_ENV(every); i2 = 1; const char *e = getenv("S23_SHOTS"); if (e) { static char d[512]; snprintf(d, sizeof d, "%s", e); char *c = strrchr(d, ':'); if (c) { *c = 0; every = (unsigned)atoi(c + 1); dir = d; } } }
      static const char *ldir; static char lst[4096]; static int i3;       /* $S23_SHOTLIST=dir:f1,f2,a-b,... -- pictures at chosen frames */
      if (!i3) { S23_SS_ENV(i3); S23_SS_ENV(ldir); S23_SS_ENV(lst); i3 = 1; const char *e = getenv("S23_SHOTLIST"); if (e) { static char d[512]; snprintf(d, sizeof d, "%s", e); char *c = strchr(d, ':'); if (c) { *c = 0; ldir = d; snprintf(lst, sizeof lst, ",%s,", c + 1); } } }
      int listed = 0;
      if (ldir) { for (const char *q = lst; *q; ) { unsigned lo = 0, hi = 0; int n = 0; if (sscanf(q, ",%u-%u%n", &lo, &hi, &n) == 2 || (sscanf(q, ",%u%n", &lo, &n) == 1 && (hi = lo, 1))) { if (frame >= lo && frame <= hi) listed = 1; q += n; } else q++; } }
      if (listed && !dir) dir = ldir;
      if (dir && ((every && frame % every == 0) || listed)) {
          static uint32_t px2[640 * 480]; { void s23_frame_rgba_wait(uint32_t *); s23_frame_rgba_wait(px2); }
          char p[700]; snprintf(p, sizeof p, "%s/f%05u.ppm", dir, frame);
          FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "P6\n640 480\n255\n");
              for (int i = 0; i < 640 * 480; i++) { const uint8_t c[3] = { (uint8_t)(px2[i] >> 16), (uint8_t)(px2[i] >> 8), (uint8_t)px2[i] }; fwrite(c, 1, 3, f); } fclose(f); } } }
    if (!shot || frame != at) return;
    static uint32_t px[640 * 480];
    { void s23_frame_rgba_wait(uint32_t *); s23_frame_rgba_wait(px); }
    FILE *f = fopen(shot, "wb"); if (!f) return;
    fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < 640 * 480; i++) { const uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
    fprintf(stderr, "[S23] frame %u -> %s\n", frame, shot);
}

/* time passed: the screen (vblank, the C361 raster line -- src/s23_screen.c) and the instruction limit */
void s23_screen_advance(uint32_t n);
uint32_t s23_screen_until_event(void);
void s23_host_step(uint32_t n)
{
    ins_total += n;
    s23_screen_advance(n);
    { void s23_h8_run(uint64_t); uint64_t s23_cycles_now(void); s23_h8_run(s23_cycles_now()); }   /* the two H8s catch up (src/s23_h8.c) */
    { void s23_c422_tick(void); s23_c422_tick(); }                  /* the twin-cabinet link (src/s23_c422.c, our fork) */
    if (ins_total >= ins_limit) { s23_on_exit(); exit(0); }
}
uint32_t s23_until_event(void) { return s23_screen_until_event(); }

/* $S23_CABINET_DIR=<name> (our fork: two cabinets on one PC, the link's "try both" launcher): this copy keeps its log, settings, backup
 * RAM and tc2_link.cfg in that sub-folder of the program's folder, and reads the ROMs from the program's extracted/ */
static const char *s23_xdir = "extracted";
/* a second copy of the game ($S23_INSTANCE=2: the author's local two-player online start, 0.9.3) keeps "_2" on its file names;
 * our fork's twin-cabinet test runs each side in its own folder instead ($S23_CABINET_DIR), so this is "" there */
const char *s23_instance(void)
{
    static char suf[16]; static int init;
    if (!init) { init = 1; const char *e = getenv("S23_INSTANCE"); const int n = e ? atoi(e) : 0; if (n >= 2 && n <= 99) snprintf(suf, sizeof suf, "_%d", n); }
    return suf;
}
/* GitHub #85: the command line a launcher needs, taken out of argv before the positional parsing below.
 *   --set KEY=VALUE        any line of the settings file, for this run only (eng_args.h): --set widescreen=1 --set fullscreen=1
 *   --host                 host a LAN game at start-up (a twin-cabinet game: Time Crisis II, Crisis Zone)
 *   --join                 find a LAN game and join it at start-up
 *   --server ADDR          connect to an online server at start-up
 *   --name NAME            the player's name online
 *   --instance N           a second (third...) copy on this computer: its own cfg, backup RAM and log (s23_N.cfg)
 *   --pos X,Y[,SCALE]      the window at X,Y on the desktop (and its size), e.g. two copies side by side
 * They map onto the $S23_NET_* / $S23_INSTANCE / $S23_WINDOW variables the runtime already reads. */
static void cli_args(int *argc, char **argv)
{
    eng_args_strip(argc, argv);
    int o = 1;
    for (int i = 1; i < *argc; i++) {
        const char *a = argv[i], *v = i + 1 < *argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host")) eng_args_setenv("S23_NET_HOST", "1");
        else if (!strcmp(a, "--join")) eng_args_setenv("S23_NET_DISCOVER", "1");
        else if (!strcmp(a, "--server") && v) { eng_args_setenv("S23_NET_SERVER", v); i++; }
        else if (!strcmp(a, "--name") && v) { eng_args_setenv("S23_NET_NAME", v); i++; }
        else if (!strcmp(a, "--instance") && v) { eng_args_setenv("S23_INSTANCE", v); i++; }
        else if (!strcmp(a, "--pos") && v) { eng_args_setenv("S23_WINDOW", v); i++; }
        else argv[o++] = argv[i];
    }
    argv[o] = NULL; *argc = o;
    static char *fs_argv[] = { NULL, "--window", "--fullscreen", NULL };      /* "tc2 --fullscreen": play, fullscreen (not a headless run) */
    if (*argc == 2 && !strcmp(argv[1], "--fullscreen")) { fs_argv[0] = argv[0]; memcpy(argv, fs_argv, sizeof fs_argv); *argc = 3; }
}
/* THE OLD SWITCH NAMES: the host's environment switches were S23_* until it moved out of tc2/ into s23/ (2026-11-01); the launcher's Time
 * Crisis 2 link entries and players' .bat files still set S23_SIDE, S23_LINK, ... -- each S23_X is copied to S23_X unless that is set */
#ifndef _WIN32
extern char **environ;
#endif
static void old_switch_names(void)
{
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "TC2_", 4)) continue;
        const char *eq = strchr(*e, '='); if (!eq || eq - *e > 120) continue;
        char name[128]; snprintf(name, sizeof name, "S23_%.*s", (int)(eq - *e - 4), *e + 4);
        if (getenv(name)) continue;
#ifdef _WIN32
        _putenv_s(name, eq + 1);
#else
        setenv(name, eq + 1, 0);
#endif
    }
}
int main(int argc, char **argv)
{
    { void s23_prof_start(void); s23_prof_start(); }          /* $S23_PROFILE=1: where the time goes (src/s23_prof.c) */
    old_switch_names();
    cli_args(&argc, argv);                                   /* --set, --fullscreen ... (the author's, GitHub #85) */
#ifdef _WIN32
    /* Windows: a double-click or a shortcut starts us anywhere -- work in the program's own folder (roms/, extracted/, <tag>.cfg, the
     * backup RAM), and with no console the log goes to <tag>.log beside the .exe (tc2.log, crszone.log, gunwars.log: what the crash
     * message points at) */
    { wchar_t wp[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, wp, MAX_PATH);
      if (n > 0 && n < MAX_PATH) { wchar_t *sl = wcsrchr(wp, L'\\'); if (sl) { *sl = 0; _wchdir(wp); } } }
    { const char *cd = getenv("S23_CABINET_DIR"); if (cd && *cd) { _mkdir(cd); if (_chdir(cd) == 0) s23_xdir = "../extracted"; } }   /* our fork, below */
    { char lp[64]; snprintf(lp, sizeof lp, "%s.log", s23_game.tag); freopen(lp, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0); }
#endif
    /* no arguments (a double-click, the player's launcher): the game in a window from power-on */
    static char *win_argv[] = { NULL, "--window", NULL };
    if (argc < 2) { win_argv[0] = argv[0]; argv = win_argv; argc = 2; }
    if (argc > 2 && strcmp(argv[1], "--window")) ins_limit = strtoull(argv[2], NULL, 0);
    char p[1024];
    /* the ROM folder: from the player's timecrs2.zip on the first run (src/s23_rom.c), then the boot image made from it */
    {
        char exe_dir[1024] = ".";
        { char *bp = SDL_GetBasePath();                  /* the program's own folder, on every system (was /proc/self/exe) */
          if (bp) { snprintf(exe_dir, sizeof exe_dir, "%s", bp); size_t k = strlen(exe_dir); while (k > 1 && (exe_dir[k - 1] == '/' || exe_dir[k - 1] == '\\')) exe_dir[--k] = 0; SDL_free(bp); } }
        int s23_rom_setup(const char *, const char *), s23_rom_boot(const char *, uint8_t *);
        if (!s23_rom_setup(s23_xdir, exe_dir) || !s23_rom_boot(s23_xdir, s23_boot)) {
            extern char s23_rom_error[];                  /* a window run (a double-click): say it on screen, there may be no console */
            if (!strcmp(argv[1], "--window")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, s23_game.title,
                s23_rom_error[0] ? s23_rom_error : "The game could not read its ROM files in extracted/ (see the log).", NULL);
            return 1;
        }
    }
    { int s23_load_data(const char *); if (!s23_load_data(s23_xdir)) return 1; }
    { void s23_h8_init(const char *); s23_h8_init(s23_xdir); }
    { void s23_render_init(const char *); s23_render_init(s23_xdir); }
    if (getenv("S23_NO_RASTER")) { void s23_render_soft(int); s23_render_soft(0); }   /* profiling: the CPU work of a GPU run (no software fill) */
    /* POWER-ON ("reset" instead of a snapshot): the R4650's reset state as MAME's mips3 device_reset gives it -- PC at the reset vector
     * 0xBFC00000 (KSEG1, the boot ROM), Status = BEV | ERL, Compare 0xFFFFFFFF, Count 0, PRId 0x2200 (R4650), the FPU's FIR the same; main
     * RAM zero (MAME's RAM share starts zeroed). The boot monitor then copies the game's ELF to RAM and enters _start (frame 12 on MAME). */
    /* --window [scale] [--fullscreen]: in a window from power-on, at the board's speed (src/s23_window.c); no instruction limit */
    if (!strcmp(argv[1], "--window")) {
        int scale = 0, fs = 0;
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--fullscreen")) fs = 1; else scale = atoi(argv[i]); }
        int s23_window_open(int, int);
        if (!s23_window_open(scale, fs)) return 1;
        argv[1] = "reset"; ins_limit = UINT64_MAX;
    }
    S23_SS_ENV(ins_limit);                         /* save states: the LOADING run's instruction limit, not the saving run's */
    const int power_on = !strcmp(argv[1], "reset");
    uint32_t pc;
    if (power_on) {
        memset(s23_ram, 0, sizeof s23_ram);
        pc = 0xBFC00000u;
        RS8(S23_R_STATUS, 0x00400004u);                /* BEV | ERL */
        RS8(S23_R_COMPARE, 0xFFFFFFFFu);
        RS8(S23_R_COUNT, 0);
        RS8(0x2078, 0x2200);                          /* PRId */
        RS4(0x1200, 0x2200);                          /* FPU FIR */
        fprintf(stderr, "[S23] power-on: PC BFC00000, Status 00400004\n");
        { void s23_c422_gate(int (*)(const char *, const char *)); int s23_window_wait_pump(const char *, const char *), s23_window_active(void);
          s23_c422_gate(s23_window_active() ? s23_window_wait_pump : NULL); }    /* the twin-cabinet link: wait for the other one (our fork) */
        { void s23_backup_load(int); int s23_window_active(void); s23_backup_load(s23_window_active()); }
    } else {
        snprintf(p, sizeof p, "%s.ram", argv[1]); if (!load(p, s23_ram, sizeof s23_ram)) return 1;
        snprintf(p, sizeof p, "%s.regs", argv[1]);
        pc = load_regs(p);
        int s23_vram_load(const char *);
        snprintf(p, sizeof p, "%s.vram", argv[1]);
        if (!s23_vram_load(p)) fprintf(stderr, "[S23] no %s: the video RAMs start empty (an older snapshot: re-take it with tools/mame/snapshot.lua)\n", p);
        int s23_chips_load(const char *);
        snprintf(p, sizeof p, "%s.chips", argv[1]);
        if (!s23_chips_load(p)) fprintf(stderr, "[S23] no %s: the modelled chips start at reset (an older snapshot: re-take it)\n", p);
    }
    /* CP0 Config: MAME's debugger does not expose it, so it is MAME's own computation (mips3com.cpp compute_config_register for this board:
     * 0x00026030 | DC 8 KB (1 << 6) | IC 8 KB (1 << 9) | big-endian 0x8000, clock divider 2 -> 0) = 0x0002E270 -- 32-byte I and D lines,
     * as the RC4650 datasheet gives. The game's only Config read (0x80000470, the cache flush at 0x800004B0) uses bit 4 (DB) alone; the
     * earlier 0x8010 agreed on that bit (measured: the loop steps 32 bytes on MAME, 6 / 52 / 27 iterations against 12 / 102 / 53 with 16) */
    RS8(0x2080, 0x0002E270);
#ifdef RR_TRACE
    { const char *t = getenv("S23_TRACE"); trace_f = fopen(t ? t : "s23_trace.txt", "w");
      if ((t = getenv("S23_TRACE_FROM"))) trace_from = (unsigned)atoi(t); }
#endif
    rr_budget = S23_SLICE;
    rr_pc = pc;
    /* the snapshot is taken at the end of a frame (MAME's frame_done): vertical blank is asserted then, and MAME's own trace from that
     * point starts in the exception vector -- so the program resumes the way the CPU does, by taking it first */
    { void s23_screen_reset(void), s23_screen_at_vblank(void); if (power_on) s23_screen_reset(); else s23_screen_at_vblank(); }
    if (!power_on) {
        s23_irq_set(S23_SRC_VBLANK, 1);
        s23_poll_irq();
    }
    { /* save states (src/s23_savestate.c): a loaded state comes back here, on a fresh C stack, at its interrupt vector */
      extern jmp_buf s23_ss_jb; extern int s23_ss_jb_set; extern uint32_t s23_ss_resume_pc; volatile uint32_t start = pc;
      if (setjmp(s23_ss_jb)) start = s23_ss_resume_pc;
      s23_ss_jb_set = 1;
      for (;;) { rr_jump(start, start); int rr_ss_base_unwound(uint32_t *); uint32_t nx; if (!rr_ss_base_unwound(&nx)) break; start = nx; } }
    fprintf(stderr, "[S23] the lifted program returned to the host at %08X\n", rr_pc);
    s23_on_exit();
    return 0;
}
