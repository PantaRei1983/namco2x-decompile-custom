/*
 * s23_c435.c -- the C435, System 23's 3D command processor, our own code (a fixed-function chip: behaviour as MAME's namcos23.cpp describes
 * it, c435_pio_w and what it dispatches). The main CPU feeds it 16-bit words -- one at a time through 0x02000000 (the PIO port), or by DMA from
 * its RAM (0x0100001C address, 0x01000020 size in bytes, 0x01000024 bit 0 = go) -- and it keeps:
 *   256 matrices (3x3, 2.14) and 256 vectors (s24); id 0x100 and up = a scratch zero one, vector 0x8000 = the light vector
 *   commands  (word 0 = header; the length is header & 0xF, or & 0xFF with bit 14)
 *     0x0xx_   matrix x matrix (.0), matrix x vector (.1), the same with an immediate operand (.2, .3), matrix set (.4), vector set (.5)
 *     0x4x__   state: 4.1 absolute priority, 4.2 / 4.3 scroll x / y, 4.4 scaling, 4.5 model blend factor, 4.7 / 4.8 light power / ambient,
 *              4.F a STATE SET: 0x0001 the C435 interrupt (Cause IP4, acknowledged at 0x0200000E), 0x0046 the viewport offset, 0x00C8 one
 *              line of the clip data (whose third line gives the viewport size), 0x0000 / 0x0002 / 0x000A an immediate polygon
 *     0x8___   RENDER a model: point-ROM model id, its matrix and vector -> one entry of the render list (a header of 0x8000-family
 *              with model 0 raises the C435 interrupt instead)
 *     0xC___   flush: bit 13 raises / clears the C451 interrupt (Evolution 2)
 * DIRECT quads (already in screen space: x, y about the centre, u, v, a 1/z mantissa / exponent, shade per corner) -- 28 words, through the CTL
 * (0x0D00000C, one at a time; a header with bit 15 set is dropped) or the direct buffer (0x0CC00002 opens it, 0x0CC00000 takes a dummy word
 * then the 28) -- MAME's ctl_direct_poly_w / direct_buf_w
 * The render list is double-buffered: the frame's renderer (s23_render.c) draws the one filled last frame and swaps, as MAME's render_run.
 * Also here: 0x01000028 R busy = 1 (never busy), 0x01000060 W / 0x0C800010 W restart the word buffer, 0x0C800016 W a state word (MAME
 * c435_state_pio_w: STATE SET without the 4.F header).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lift_rt.h"
#include "s23_host.h"
#include "s23_c435.h"
#include "s23_game.h"

uint32_t rr_read(uint32_t a, int size);
void s23_irq_set(int src, int on);

static struct {
    uint16_t buffer[256]; int pos;
    uint32_t address, size;
    uint16_t state_buf[256]; int state_pos;
} c435;
static int16_t matrices[256][9];
static int32_t vectors[256][3], light_vector[3], spv[3];
static int16_t spm[9];
static uint16_t absolute_priority, model_blend_factor, light_power, light_ambient, scaling;
static uint32_t vp_set_at, null_at;             /* window_memory: (frame + 1) * 256 + stretch of the last viewport state sent before any null model, of the last null model */
static int32_t zoff; static uint16_t vp_prio;     /* state type 9's depth (words 15-16, s23_game.zoff_state); the viewport state's word 3 (s23_game.vp_front0) */
static int16_t tx, ty;
static int16_t vp_offset_x, vp_offset_y, vp_size_x, vp_size_y;
static float clip_data[24]; static int clip_line;
static uint16_t direct_buf[28]; static int direct_pos, direct_nonempty, direct_open;
s23_render_list s23_render[2];
int s23_render_cur;
unsigned long s23_c435_unknown;

static int32_t s24(uint32_t v) { return (v & 0x800000) ? (int32_t)(v | 0xFF000000u) : (int32_t)(v & 0xFFFFFF); }
/* the C435's float: 8 bits of exponent, a 16-bit two's-complement mantissa; m * 2^(e - 46) */
float s23_f24(uint32_t v)
{
    if (!(v & 0xFFFF)) return 0;
    uint32_t r = (v & 0x8000) ? 0x80000000u : 0;
    uint16_t m = (uint16_t)(r ? -v : v);
    uint8_t e = (uint8_t)((v >> 16) + 0x60);
    while (!(m & 0x8000)) { m <<= 1; e--; }
    r |= (uint32_t)e << 23 | (uint32_t)(m & 0x7FFF) << 8;
    float f; memcpy(&f, &r, 4); return f;
}
static int32_t *getv(uint16_t id) { if (id == 0x8000) return light_vector; if (id >= 0x100) { memset(spv, 0, sizeof spv); return spv; } return vectors[id]; }
static int16_t *getm(uint16_t id) { if (id >= 0x100) { memset(spm, 0, sizeof spm); return spm; } return matrices[id]; }
static void transpose(int16_t *m) { int16_t t[9]; memcpy(t, m, sizeof t); for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 3 + c] = t[c * 3 + r]; }

int s23_c435_seg;
float s23_c435_fov_max; int16_t s23_c435_fov_vx, s23_c435_fov_vy; int s23_c435_fov_plain;                     /* the narrowest view (largest projection scale) among this list's clip sets */
static s23_render_entry *new_entry(int type)
{
    s23_render_list *l = &s23_render[s23_render_cur];
    if (l->count >= S23_RENDER_MAX) return NULL;
    s23_render_entry *e = &l->e[l->count++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->absolute_priority = absolute_priority;
    e->light_power = light_power; e->light_ambient = light_ambient;
    e->vp_size_x = vp_size_x; e->vp_size_y = vp_size_y; e->vp_offset_x = vp_offset_x; e->vp_offset_y = vp_offset_y;
    e->vp_fov = clip_data[23];
    e->seg = (uint8_t)(s23_c435_seg < 255 ? s23_c435_seg : 255);
    e->zoff = zoff; e->vp_prio = vp_prio;
    return e;
}

static void mat_mat(void)                         /* 0.0: t = m1 * m2 (m1 identity with bit 8, m2 transposed with bit 10) */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 4) return;
    int16_t m1[9], m2[9];
    memcpy(m2, getm(b[2]), sizeof m2);
    if (b[0] & 0x400) transpose(m2);
    if (b[0] & 0x100) { memset(m1, 0, sizeof m1); m1[0] = m1[4] = m1[8] = 0x4000; } else memcpy(m1, getm(b[4]), sizeof m1);
    int16_t *t = getm(b[1]);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        t[r * 3 + c] = (int16_t)((m1[r * 3] * m2[c * 3] + m1[r * 3 + 1] * m2[c * 3 + 1] + m1[r * 3 + 2] * m2[c * 3 + 2]) >> 14);
}
static void mv(int32_t *t, const int16_t *m, const int32_t *v, const uint16_t *b)
{
    int32_t r[3];
    for (int k = 0; k < 3; k++) r[k] = (int32_t)((m[k * 3] * (int64_t)v[0] + m[k * 3 + 1] * (int64_t)v[1] + m[k * 3 + 2] * (int64_t)v[2]) >> 14);
    if (b[3] != 0xFFFF) {
        const int32_t *vt = getv(b[3]);
        for (int k = 0; k < 3; k++) r[k] = (b[0] & 0x1000) ? r[k] - vt[k] : r[k] + vt[k];
    }
    memcpy(t, r, sizeof r);
}
static void mat_vec(void)                         /* 0.1: t = m * v (+/- vt) */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 4) return;
    int16_t m[9]; int32_t v[3];
    memcpy(m, getm(b[2]), sizeof m); memcpy(v, getv(b[4]), sizeof v);
    if (b[0] & 0x400) transpose(m);
    mv(getv(b[1]), m, v, b);
}
static void mat_mat_immed(void)                   /* 0.2: t = immediate * m2 */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 12) return;
    int16_t m1[9], m2[9];
    for (int i = 0; i < 9; i++) m1[i] = (int16_t)b[4 + i];
    /* HEADER BIT 8 = the second matrix is the IDENTITY, as on 0.0 (mat_mat): Gunmen Wars sets its characters' bone matrices with 0x012C
     * (t = the immediate matrix; b[2] names a matrix it never set). Not in MAME (which multiplies by b[2] and draws the players flat:
     * their bodies were missing in attract and play). */
    if (b[0] & 0x100) { { static int said; if (!said++ && getenv("S23_MATLOG")) fprintf(stderr, "[C435] 0.2 with the identity bit (header %04X)\n", b[0]); }
                        memset(m2, 0, sizeof m2); m2[0] = m2[4] = m2[8] = 0x4000;
                        /* and the immediate is then TRANSPOSED (judged on Gunmen Wars' soldiers: as sent, the limbs were jumbled; transposed they
                         * stand and run). $S23_BONE_T=0 (TEST) keeps it as sent */
                        { static int tr = -1; if (tr < 0) { const char *e5 = getenv("S23_BONE_T"); tr = !(e5 && *e5 == '0'); } if (tr) transpose(m1); } } else {
    memcpy(m2, getm(b[2]), sizeof m2);
    { static int ta = -1; if (ta < 0) ta = getenv("S23_IMM_T") != NULL; if (ta) transpose(m1); } }   /* TEST: transpose every 0.2 immediate */
    if (b[0] & 0x400) transpose(m2);
    int16_t *t = getm(b[1]);
    /* GUNMEN WARS (s23_game.imm_right; $S23_IMM_X=1 forces it): without bit 8, t = m2 * transpose(immediate) -- the immediate on the
     * RIGHT, transposed, as the bone matrices (bit 8) already are. Judged on the footage: the shadows (flatten x shear immediates) now lie
     * flat and start at the feet (were slanted smears beside the fighters), the camera pitch is right (horizon and your fighter sit where
     * the videos show them); yaw-only cameras come out the same either way */
    { static int x = -1; if (x < 0) x = s23_game.imm_right || getenv("S23_IMM_X") != NULL;
      if (x && !(b[0] & 0x100)) { int16_t r2[9];
          for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
              r2[r * 3 + c] = (int16_t)((m2[r * 3] * m1[c * 3] + m2[r * 3 + 1] * m1[c * 3 + 1] + m2[r * 3 + 2] * m1[c * 3 + 2]) >> 14);
          memcpy(t, r2, sizeof r2); return; } }
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        t[r * 3 + c] = (int16_t)((m1[r * 3] * m2[c] + m1[r * 3 + 1] * m2[3 + c] + m1[r * 3 + 2] * m2[6 + c]) >> 14);
}
static void mat_vec_immed(void)                   /* 0.3: t = m * immediate (+/- vt) */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 9) return;
    int16_t m[9]; int32_t v[3];
    memcpy(m, getm(b[2]), sizeof m);
    for (int k = 0; k < 3; k++) v[k] = (int32_t)((uint32_t)b[4 + 2 * k] << 16 | b[5 + 2 * k]);
    if (b[0] & 0x400) transpose(m);
    mv(getv(b[1]), m, v, b);
}
static void mat_set(void) { const uint16_t *b = c435.buffer; if ((b[0] & 0xF) != 10) return; int16_t *t = getm(b[1]); for (int i = 0; i < 9; i++) t[i] = (int16_t)b[i + 2]; }
static void vec_set(void) { const uint16_t *b = c435.buffer; if ((b[0] & 0xF) != 7) return; int32_t *t = getv(b[1]); for (int i = 0; i < 3; i++) t[i] = s24((uint32_t)b[2 * i + 2] << 16 | b[2 * i + 3]); }

static uint32_t dma_at;                           /* debug: the DMA word being fed */
static int state_size(uint16_t type)
{
    switch (type & 0xFF) {
    case 0x00: return 43; case 0x01: return 1; case 0x02: return 53; case 0x09: return 19; case 0x0A: return 47;
    case 0x42: return 41; case 0x46: return 13; case 0xC0: return 33; case 0xC6: return 13; case 0xC8: return 17;
    default: return -1;
    }
}
static uint32_t L(const uint16_t *p, int i) { return (uint32_t)p[i] << 16 | p[i + 1]; }
/* an IMMEDIATE polygon (state 0x0000 / 0x0002 / 0x000A): the words' layouts per MAME's c435_state_set */
static void immediate(uint16_t type, const uint16_t *p, uint16_t header)
{
    { static int lg = -1; static unsigned n; if (lg < 0) lg = getenv("S23_IMMLOG") != NULL;   /* debug: immediate polygons (type, header, first words) */
      if (lg && n++ < 400) fprintf(stderr, "[IMM] f%u type %04X hdr %04X w %04X %04X %04X %04X %04X %04X\n", rr_frame, type, header, p[0], p[1], p[2], p[3], p[4], p[5]); }
    s23_render_entry *e = new_entry(S23_RE_IMMEDIATE);
    if (!e) return;
    s23_immediate *q = &e->imm;
    q->type = p[0]; q->h = L(p, 1); q->pal = L(p, 3);
    int k = 5, n = 4, with_i = 1;
    if (type == 0x0000) { q->zbias = L(p, 5); k = 7; n = header == 0x4F38 ? 4 : 3; }
    else if (type == 0x0002) { q->zbias = 0; k = 5; }
    else { q->zbias = L(p, 5); k = 7; with_i = 0; }
    if (with_i) for (int i = 0; i < n; i++, k += 2) q->i[i] = L(p, k);
    for (int i = 0; i < n; i++) { q->u[i] = L(p, k); q->v[i] = L(p, k + 2); k += 4; }
    for (int i = 0; i < n; i++) { q->x[i] = L(p, k); q->y[i] = L(p, k + 2); q->z[i] = L(p, k + 4); k += 6; }
    q->n = n;
}
static void state_set(uint16_t type, const uint16_t *p, uint16_t header)
{
    switch (type) {
    case 0x0000: case 0x0002: case 0x000A: immediate(type, p, header); break;
    case 0x0001: s23_irq_set(S23_SRC_C435, p[0] & 1); break;
    case 0x0009: zoff = (int32_t)(L(p, 15) & 0x1FFFFF); break;   /* (MAME: unknown) a word, six 1.0s, 0, a depth, 0x1FFFFF. Panic Park sends it after
                                                   * each "model 0" stretch break: 0 for the title's island picture, 0x3A628 for its PANIC PARK letters
                                                   * (behind the island by depth, over it on the arcade) -- how it is used: s23_game.zoff_state */
    case 0x0046: {
        const uint32_t d = L(p, 7);
        vp_offset_x = (int16_t)((int16_t)((d & 0xFFF) << 4) >> 4);
        vp_offset_y = (int16_t)-((int16_t)(((d >> 12) & 0xFFF) << 4) >> 4);
        { const uint32_t here = (rr_frame + 1) * 256u + (uint32_t)(s23_c435_seg & 255); if (null_at != here) vp_set_at = here; }
        vp_prio = p[3];                                   /* (MAME: unused) word 3: 0x27 normally, 0x20 for Panic Park's plate (s23_game.vp_front0) */
        { static long a = -2, b; if (a == -2) { S23_SS_ENV(a); S23_SS_ENV(b); const char *e = getenv("S23_VPLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &b); }   /* debug: viewport offsets in frames a..b */
          if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= b) fprintf(stderr, "[VP] f%u offset %d %d size %d %d\n", rr_frame, vp_offset_x, vp_offset_y, vp_size_x, vp_size_y); }
        break; }
    case 0x00C8:
        for (int i = 0; i < 8; i++) clip_data[clip_line * 8 + i] = s23_f24(L(p, 2 * i + 1));
        clip_line = (clip_line + 1) % 3;
        { static int lg = -1; if (lg < 0) { S23_SS_ENV(lg); lg = getenv("S23_CLIPLOG") != NULL; }   /* $S23_CLIPLOG: every complete clip-data set (3 lines of 8 floats) */
          extern uint32_t rr_frame;
          if (lg && clip_line == 0) { static float prevc[24]; if (memcmp(prevc, clip_data, sizeof prevc)) { memcpy(prevc, clip_data, sizeof prevc);
              fprintf(stderr, "[CLIP] f%u", rr_frame); for (int i = 0; i < 24; i++) fprintf(stderr, "%s%g", i % 8 ? " " : " | ", clip_data[i]); fprintf(stderr, "\n"); } } }
        if (clip_line == 0 && clip_data[10] != 0.f) {
            const float a10 = clip_data[10] < 0 ? -clip_data[10] : clip_data[10], a23 = clip_data[23] < 0 ? -clip_data[23] : clip_data[23];
            const float a2 = clip_data[2] < 0 ? -clip_data[2] : clip_data[2];
            /* MAME's formula takes the clip planes as unit-normal (|c0| = |c9| = 1, all games' full-screen sets). Panic Park's RULE monitor
             * sends a window's planes unnormalised -- left (208, -40, -146.2), top (0, 429, -161.7), fov 276 -- and MAME's |c10| * fov
             * overflowed to a 44617-pixel viewport (its little screen drawn nowhere). The edge at the centre line is -c / normal: dividing by
             * the normal is the same for unit planes and gives this window's 194 x 104 */
            const float a0 = clip_data[0] < 0 ? -clip_data[0] : clip_data[0], a9 = clip_data[9] < 0 ? -clip_data[9] : clip_data[9];
            const float ny = a9 > 0.f ? a10 / a9 : a10, nx = a0 > 0.f ? a2 / a0 : a2;
            vp_size_y = (int16_t)(ny * a23 + 0.5f);
            vp_size_x = (int16_t)(nx * a23 + 0.5f);
        } else if (clip_line == 0 && (clip_data[9] > 1.5f || clip_data[9] < -1.5f)) {
            /* a CLOSED window: unnormalised planes with no depth term (Panic Park's RULE monitor shutting: top y <= 0, bottom y >= 0, fov 1) --
             * nothing of it shows. MAME keeps the last viewport, and the monitor's chick animation filled the screen over the roulette */
            vp_size_x = vp_size_y = 0;
        }
        /* the frame's MODEL view (s23_game.model_fov_max): the largest projection scale among its clip sets, leaving out the plain 90-degree
         * one (scale 320) whenever there is another -- with its viewport size: Gunmen Wars' match end shrinks that set (scale and size
         * together, 554 -> 277, 320x240 -> 160x120) to shrink the picture into a box; the max alone then fell back to 320 */
        if (clip_line == 0) { const float f = clip_data[23]; const int plain = f > 319.5f && f < 320.5f;
            if (!plain && f > s23_c435_fov_max) { s23_c435_fov_max = f; s23_c435_fov_vx = vp_size_x; s23_c435_fov_vy = vp_size_y; }
            if (plain && s23_c435_fov_max == 0.f) s23_c435_fov_plain = 1; }
        break;
    default: break;
    }
}
static void render_model(void)                    /* 8: a model of the point ROM */
{
    const uint16_t *b = c435.buffer;
    const int size = b[0] & 0xF;
    if (b[1] == 0) { s23_irq_set(S23_SRC_C435, 1); s23_c435_seg++; return; }
    s23_render_entry *e = new_entry(S23_RE_MODEL);
    if (!e) return;
    e->model = b[1];
    e->model2 = size == 4 ? b[2] : 0;
    { static long why = -2; static int nw; if (why == -2) { S23_SS_ENV(why); S23_SS_ENV(nw); const char *e4 = getenv("S23_WHYMODEL"); why = e4 ? strtol(e4, NULL, 16) : -1; }   /* debug */
      static long wf = -2; if (wf == -2) { const char *ef = getenv("S23_WHYFROM"); wf = ef ? atol(ef) : 0; }
      if (why == b[1] && (long)rr_frame >= wf && nw++ < 6) { fprintf(stderr, "[WHYC] f%u hdr %04X size %d words:", rr_frame, b[0], size); for (int k = 0; k <= size; k++) fprintf(stderr, " %04X", b[k]); { const int16_t *mm = getm(b[size - 1]); const int32_t *vv = getv(b[size]); fprintf(stderr, " | m %d %d %d %d %d %d %d %d %d v %d %d %d", mm[0], mm[1], mm[2], mm[3], mm[4], mm[5], mm[6], mm[7], mm[8], vv[0], vv[1], vv[2]); } fprintf(stderr, "\n"); } }
    e->scaling = (b[0] & 0x80) ? scaling / 16384.0f : 1.0f;
    e->transpose = (b[0] >> 6) & 1;
    e->mirror_x = (b[0] >> 13) & 1;
    e->model_blend_factor = model_blend_factor;
    e->tx = (b[0] & 0x200) ? tx : 0; e->ty = (b[0] & 0x200) ? ty : 0;
    memcpy(e->m, getm(b[size - 1]), sizeof e->m);
    memcpy(e->v, getv(b[size]), sizeof e->v);
    memcpy(e->light_vector, light_vector, sizeof e->light_vector);
    if (s23_game.null_model && e->model == s23_game.null_model) null_at = (rr_frame + 1) * 256u + (uint32_t)(s23_c435_seg & 255);
    if (s23_game.window_memory && !(s23_game.null_model && e->model == s23_game.null_model)) {   /* (s23_game.h window_memory) */
        static struct { uint32_t last, real, real_last; int16_t sx, sy, ox, oy; float fov; uint16_t prio; } wm[0x1000];
        const int full = e->vp_size_x == 320 && e->vp_size_y == 240 && !e->vp_offset_x && !e->vp_offset_y;
        typeof(&wm[0]) w = &wm[e->model & 0xFFF];
        static uint32_t sf; static int ss = -1; static int16_t sx, sy, ox, oy; static float sfov; static uint16_t sprio;   /* the window this stretch of the list last used */
        const int same = ss == s23_c435_seg && sf == rr_frame + 1;
        const int kept = w->last && rr_frame + 1 - w->last <= 2 && w->real != rr_frame + 1 && rr_frame + 1 - w->real_last <= 180;   /* (not when this
                                                   * frame drew it through a window already; not 3 s after the game last sent that window) */
        const int real = !full;
        const int told = vp_set_at == (rr_frame + 1) * 256u + (uint32_t)(s23_c435_seg & 255);   /* the game set the viewport in this stretch, before its
                                                   * null model: full screen meant (the INTERACTIVE CONTROLS demo's blue player, out of the window) */
        if (full && s23_c435_seg >= 1 && !told && (kept || same)) {
            if (kept) { e->vp_size_x = w->sx; e->vp_size_y = w->sy; e->vp_offset_x = w->ox; e->vp_offset_y = w->oy; e->vp_fov = w->fov; e->vp_prio = w->prio; }
            else { e->vp_size_x = sx; e->vp_size_y = sy; e->vp_offset_x = ox; e->vp_offset_y = oy; e->vp_fov = sfov; e->vp_prio = sprio; }   /* (the NEEDED count's digits, new in a remembered stretch) */
        }
        if (e->vp_size_x != 320 || e->vp_size_y != 240 || e->vp_offset_x || e->vp_offset_y) {
            if (real) w->real = w->real_last = rr_frame + 1;
            w->last = rr_frame + 1; w->sx = e->vp_size_x; w->sy = e->vp_size_y; w->ox = e->vp_offset_x; w->oy = e->vp_offset_y; w->fov = e->vp_fov; w->prio = e->vp_prio;
            sf = rr_frame + 1; ss = s23_c435_seg; sx = e->vp_size_x; sy = e->vp_size_y; ox = e->vp_offset_x; oy = e->vp_offset_y; sfov = e->vp_fov; sprio = e->vp_prio;
        }
    }
}

void s23_c435_pio(uint16_t w)
{
    c435.buffer[c435.pos++ & 255] = w;
    const uint16_t h = c435.buffer[0];
    const int psize = (h & 0x4000) ? (h & 0xFF) : (h & 0xF);
    if (c435.pos < psize + 1) return;
    int known = 1;
    switch (h & 0xC000) {
    case 0x0000:
        { static int lg = -1; static uint32_t seen[256]; if (lg < 0) lg = getenv("S23_MATLOG") != NULL;   /* debug: matrix ops whose length is not the one handled */
          static const int want[6] = { 4, 4, 12, 9, 10, 7 }; const int op = (h >> 4) & 0xF;
          if (lg && op < 6 && (h & 0xF) != want[op] && !seen[h & 0xFF]++) fprintf(stderr, "[MATLEN] op %d hdr %04X len %d (handled: %d) words %04X %04X %04X\n", op, h, h & 0xF, want[op], c435.buffer[1], c435.buffer[2], c435.buffer[3]); }
        { static int lg2 = -1; static uint32_t tg[16][0x10]; static unsigned long n2; if (lg2 < 0) lg2 = getenv("S23_MATIDS") != NULL;   /* debug: matrix-op target ids */
          if (lg2) { const int op = (h >> 4) & 0xF; const uint16_t t = c435.buffer[1]; tg[op][t >> 12]++;
            if ((op == 1 || op == 3) && (h & 0xBB00) && rr_frame > 6000 && n2++ < 16) { const int16_t *ms = getm(c435.buffer[2]);
                fprintf(stderr, "[MATID] f%u op %d hdr %04X target %04X src %04X %04X w4.. %04X %04X %04X %04X | src m %d %d %d %d\n", rr_frame, op, h, t, c435.buffer[2], c435.buffer[3],
                        c435.buffer[4], c435.buffer[5], c435.buffer[8], c435.buffer[12], ms[0], ms[1], ms[4], ms[8]); } } }
        { static long a = -2, b; if (a == -2) { S23_SS_ENV(a); S23_SS_ENV(b); const char *e = getenv("S23_STLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &b); }   /* debug: matrix ops too */
          if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= b) { fprintf(stderr, "[ST] f%u X @%08X %04X", rr_frame, dma_at, h); for (int i = 1; i <= (h & 0xF) && i < 16; i++) fprintf(stderr, " %04X", c435.buffer[i]); fprintf(stderr, "\n"); } }
        switch (h & 0xF0) {
        case 0x00: mat_mat(); break; case 0x10: mat_vec(); break; case 0x20: mat_mat_immed(); break; case 0x30: mat_vec_immed(); break;
        case 0x40: mat_set(); break; case 0x50: vec_set(); break; default: known = 0;
        }
        break;
    case 0x4000:
        { static long a = -2, b; if (a == -2) { S23_SS_ENV(a); S23_SS_ENV(b); const char *e = getenv("S23_STLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &b); }   /* debug: every state command in frames a..b */
          if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= b) { fprintf(stderr, "[ST] f%u %04X", rr_frame, h); for (int i = 1; i <= (h & 0xFF) && i < 64; i++) fprintf(stderr, " %04X", c435.buffer[i]); fprintf(stderr, "\n"); } }
        switch (h & 0x3F00) {
        case 0x0000: case 0x0600:                  /* MAME: unknown state-set types 4.0 / 4.6 */
            { static int lg = -1; if (lg < 0) lg = getenv("S23_UNKLOG") != NULL;   /* $S23_UNKLOG: every 4.0 / 4.6 write (TEST ONLY, GitHub #54) */
              if (lg) fprintf(stderr, "[UNK4] f%u %04X %04X\n", rr_frame, h, c435.buffer[1]); }
            break;
        case 0x0100: absolute_priority = c435.buffer[1]; break;
        case 0x0200: tx = (int16_t)c435.buffer[1]; break;
        case 0x0300: ty = (int16_t)c435.buffer[1]; break;
        case 0x0400: if ((h & 0xFF) == 1) scaling = c435.buffer[1]; break;
        case 0x0500: model_blend_factor = c435.buffer[1]; break;
        case 0x0700: light_power = c435.buffer[1]; break;
        case 0x0800: light_ambient = c435.buffer[1]; break;
        case 0x0F00:
            if ((h & 0xFF) == 0) break;
            if (state_size(c435.buffer[1]) != (h & 0xFF) - 1 && h != 0x4F38) break;
            state_set(c435.buffer[1], c435.buffer + 2, h);
            break;
        default: known = 0;
        }
        break;
    case 0x8000: { static long a = -2, b; if (a == -2) { S23_SS_ENV(a); S23_SS_ENV(b); const char *e = getenv("S23_STLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &b); } if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= b) fprintf(stderr, "[ST] f%u M %04X h %04X %04X %04X %04X %04X\n", rr_frame, c435.buffer[1], h, c435.buffer[2], c435.buffer[3], c435.buffer[4], c435.buffer[5]); } render_model(); break;
    case 0xC000:                                  /* FLUSH (MAME c435_flush): with a zero length, header bit 13 raises the C451 interrupt, clear lowers it.
                                                   * Wired on Evolution 2 (IRQ3, status 0x100000B0 bit 0); on System 23 the game table maps it to no pin */
        if ((h & 0xF) == 0) s23_irq_set(S23_SRC_C451, (h >> 13) & 1);
        { static int lg = -1; if (lg < 0) lg = getenv("S23_FLUSHLOG") != NULL;   /* $S23_FLUSHLOG: every C435 flush, with the list size so far */
          if (lg) fprintf(stderr, "[FLUSH] frame %u h %04X list %d\n", rr_frame, h, s23_render[s23_render_cur].count); }
        break;
    }
    if (!known) s23_c435_unknown++;
    c435.pos = 0;
}
/* MAME c435_state_pio_w: a state entry written word by word through 0x0C800016 (its type first) */
static void state_pio(uint16_t w)
{
    { static long a = -2, b; if (a == -2) { S23_SS_ENV(a); S23_SS_ENV(b); const char *e = getenv("S23_STLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &b); }   /* debug */
      if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= b) fprintf(stderr, "[SP] f%u %u %04X pc %08X\n", rr_frame, c435.state_pos, w, rr_pc); }
    c435.state_buf[c435.state_pos++ & 255] = w;
    const int n = state_size(c435.state_buf[0]);
    if (n < 0 || c435.state_pos < n + 1) { if (n < 0) c435.state_pos = 0; return; }
    state_set(c435.state_buf[0], c435.state_buf + 1, 0);
    c435.state_pos = 0;
}

static void direct_entry(void)
{
    if (s23_game.direct_off_bit && (direct_buf[2] & 0x8000)) return;   /* (s23_game.h) */
    s23_render_entry *e = new_entry(S23_RE_DIRECT);
    if (!e) return;
    memcpy(e->d, direct_buf, sizeof e->d);
    e->vp_size_x = 320; e->vp_size_y = 240; e->vp_offset_x = 0; e->vp_offset_y = 0; e->vp_fov = 320.f;
}
/* the command DMA from main RAM (MAME c435_dma): the length is the game's; a garbage one must not hang the frame -- main RAM is 16 MB */
static void dma(void)
{
    { static long a = -2, b; if (a == -2) { S23_SS_ENV(a); S23_SS_ENV(b); const char *e = getenv("S23_STLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &b); }   /* debug */
      if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= b) fprintf(stderr, "[DMA] f%u adr %08X size %X pc %08X\n", rr_frame, c435.address, c435.size, rr_pc); }
    const int spr = s23_game.gorgon && s23_game.gorgon->sprite_mode();   /* GORgON: PIO mode bit 0 = this DMA is sprite data (MAME c435_dma) */
    for (uint32_t pos = 0; pos < c435.size && pos < 0x1000000u; pos += 2) {
        dma_at = (c435.address & 0x1FFFFFFFu) + pos;
        const uint16_t w = (uint16_t)rr_read(dma_at, 2);
        if (spr) s23_game.gorgon->sprite_w(w); else s23_c435_pio(w);
    }
    dma_at = 0;
}
/* EVOLUTION 2's C450 (MAME crszone_state::c450_dma_*): the same DMA into the C435, started by writing the SIZE, and it interrupts when done
 * (the C450 source; 0x1040001C reads it, the IRQ status register 0x100000B4 acknowledges it). Raw 29-bit addresses: src/s23_mem.c calls
 * this before folding the Evolution 2 map onto System 23's. */
int s23_c450_write(uint32_t p, uint32_t v)
{
    if (p == 0x10400000u) { c435.address = v; return 1; }
    if (p == 0x1040000Cu) { c435.size = v; dma(); s23_irq_set(S23_SRC_C450, 1); return 1; }
    return 0;
}

int s23_c435_write(uint32_t p, int size, uint32_t v)
{
    switch (p) {
    case 0x0D00000Cu:                                    /* MAME ctl_direct_poly_w */
        { static int lg = -1; if (lg < 0) { S23_SS_ENV(lg); lg = getenv("S23_DIRECTLOG") != NULL; } if (lg && (v & 0xFFFF)) fprintf(stderr, "[DIRECT] frame %u CTL %04X\n", rr_frame, (unsigned)v); }
        /* BOUNDED: the buffer path below leaves direct_pos at up to 28, and a CTL word then wrote direct_buf[28] -- past the end, onto
         * direct_pos itself, after which the writes ran wild through memory (Gunmen Wars uses both paths: it crashed in a match,
         * inside the GL driver, a few frames later) */
        if (direct_pos < 0 || direct_pos >= 28) { direct_pos = 0; direct_nonempty = 0; }
        direct_buf[direct_pos++] = (uint16_t)v;
        if (v & 0xFFFF) direct_nonempty = 1;
        if (direct_pos >= 28) {
            if (direct_nonempty && !(direct_buf[0] & 0x8000)) direct_entry();
            direct_nonempty = 0; direct_pos = 0;
        }
        return 1;
    case 0x0CC00002u: if (s23_game.no_direct_buf) return 1; direct_open = (v & 0xFFFF) != 0; direct_pos = 0; return 1;
    case 0x0CC00000u:
        if (s23_game.no_direct_buf) return 1;
        { static int lg = -1; if (lg < 0) { S23_SS_ENV(lg); lg = getenv("S23_DIRECTLOG") != NULL; } if (lg) fprintf(stderr, "[DIRECT] frame %u BUF %04X open %d\n", rr_frame, (unsigned)v, direct_open); }
        if (!direct_open) return 1;
        if (direct_pos <= 0 || direct_pos > 28) { direct_pos = 1; direct_nonempty = 0; return 1; }   /* (bounded, as above: the dummy word first) */
        direct_buf[direct_pos - 1] = (uint16_t)v; direct_pos++; direct_nonempty = 1;
        if (direct_pos >= 29) { direct_entry(); direct_nonempty = 0; direct_pos = 0; }
        return 1;
    case 0x0100001Cu: c435.address = v; return 1;
    case 0x01000020u: c435.size = v; return 1;
    case 0x01000024u: if (v & 1) dma(); return 1;
    case 0x01000060u: c435.pos = 0; if (s23_game.gorgon) s23_game.gorgon->bufpos0(); return 1;
    case 0x0100005Cu:
        if (s23_game.gorgon) { s23_game.gorgon->pio_mode(v); c435.pos = 0; return 1; }   /* MAME gorgon c435_pio_mode_w */
        if (!s23_game.super23) return 0; s23_irq_set(S23_SRC_C361, 0); return 1;   /* MAME namcoss23_state::c435_c361_ack_w */
    case 0x02000000u: if (size == 2) { s23_c435_pio((uint16_t)v); return 1; } return 0;
    case 0x0C800010u: c435.state_pos = 0; return 1;
    case 0x0C800016u: state_pio((uint16_t)v); return 1;
    }
    return 0;
}
int s23_c435_owns(uint32_t p) { return (p >= 0x0100001Cu && p <= 0x0100002Bu) || p == 0x01000060u || ((s23_game.super23 || s23_game.gorgon) && p == 0x0100005Cu) || p == 0x0C800010u || p == 0x0C800016u; }
float s23_c435_fov_now(void) { return clip_data[23]; }   /* debug: the clip set now (S23_FOVLAST) */
