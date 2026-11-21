/* s23_gorgon.c -- the Namco GORgON board ("System 22.5": Rapid River, Final Furlong) on the System 23 host. Linked ONLY by those games
 * (their CMakeLists put it before S23_HOST_TAIL: its state is saved), so every other game's build and save states stay as they were.
 * Values from MAME's namcos23.cpp (gorgon_state). GORgON carries System 23's chips at other addresses; s23_mem.c asks fold() for
 * System 23's address of the same chip, so every chip model serves both boards:
 *   GORgON                    System 23                 chip
 *   0x06108000-0x061087FF  -> 0x06A08000                 C404 registers
 *   0x06110000-0x0613FFFF  -> 0x06A10000                 C404 palette RAM
 *   0x06400000-0x0641FFFF  -> 0x06800000                 C361 text (character RAM + tile map)
 *   0x06420000-0x0642000F  -> 0x06820000                 C361 registers
 *   0x0C000000-0x0C00FFFF  -> 0x06000000                 backup RAM
 *   0x0F000000-0x0F000003  -> 0x0E800000                 sub-CPU mailbox
 *   0x0F200000-0x0F203FFF  -> 0x06200000                 C422 RAM
 *   0x0F300000-0x0F30000F  -> 0x06400000                 C422 registers
 * and its own (answered here, before the fold):
 *   0x06000000-3 W         (MAME: "nvram_w", TODO there) ignored, reads 0
 *   0x06080000-0x0608000F  CZ attribute registers (word 4: per bank write-disable bit 0 / reverse bit 1; word 5: the bank read back)
 *   0x06080200-0x060803FF  CZ RAM: 4 banks x 256 words, the depth-cue compare table
 *   0x06300000 W           C404 sprite attribute index, 0x06300002 W its data (4 words a sprite, 0x280 sprites)
 *   C435 0x0100005C W      PIO mode: bit 0 = the next DMAs carry SPRITE data (s23_c435.c calls pio_mode / sprite_w) */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "s23_game.h"
#include "s23_frame.h"

static uint16_t spritedata[0x10000];              /* MAME m_c435.spritedata: [0] control, [2]/[4] first/last, [6]/[10]/[12] offsets, 0x4000+ 16 words a sprite */
static uint16_t spr_attr[0x280][4];               /* MAME m_c404.sprites: z (24 bit, d0:d1), colour d2, cz + fade d3 */
static uint32_t spr_attr_idx;
static uint32_t pio_mode, spr_pos, spr_target;
static uint16_t czattr[8];
static uint16_t czram[4][0x100];

static uint32_t fold(uint32_t p)
{
    static const struct { uint32_t lo, hi, to; } m[] = {
        { 0x06108000u, 0x061087FFu, 0x06A08000u }, { 0x06110000u, 0x0613FFFFu, 0x06A10000u },
        { 0x06400000u, 0x0641FFFFu, 0x06800000u }, { 0x06420000u, 0x0642000Fu, 0x06820000u },
        { 0x0C000000u, 0x0C00FFFFu, 0x06000000u }, { 0x0F000000u, 0x0F000003u, 0x0E800000u },
        { 0x0F200000u, 0x0F203FFFu, 0x06200000u }, { 0x0F300000u, 0x0F30000Fu, 0x06400000u } };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (p >= m[i].lo && p <= m[i].hi) return p - m[i].lo + m[i].to;
    return p;
}

static int g_read(uint32_t p, int size, uint32_t *v)
{
    if (p >= 0x06000000u && p <= 0x06000003u) { *v = 0; return 1; }
    if (p >= 0x06080000u && p <= 0x0608000Fu) { *v = czattr[(p >> 1) & 7]; if (size == 4) *v = (*v << 16) | czattr[((p >> 1) + 1) & 7]; return 1; }
    if (p >= 0x06080200u && p <= 0x060803FFu) { *v = czram[czattr[5] & 3][((p - 0x06080200u) >> 1) & 0xFF]; return 1; }
    if (p >= 0x06300000u && p <= 0x06300003u) { *v = 0; return 1; }
    return 0;
}

static void cz_w(uint32_t i, uint16_t d)
{
    for (int b = 0; b < 4; b++) if (!(czattr[4] >> (b * 4) & 1)) czram[b][i & 0xFF] = d;   /* bit 0 of each bank's nibble = write DISABLE */
}
static void spr_attr_w(uint16_t d) { spr_attr[(spr_attr_idx >> 2) % 0x280][spr_attr_idx & 3] = d; spr_attr_idx++; }

static int g_write(uint32_t p, int size, uint32_t v)
{
    if (p >= 0x06000000u && p <= 0x06000003u) return 1;
    if (p >= 0x06080000u && p <= 0x0608000Fu) {
        const uint32_t i = (p >> 1) & 7;
        if (size == 4) { czattr[i] = (uint16_t)(v >> 16); czattr[(i + 1) & 7] = (uint16_t)v; } else czattr[i] = (uint16_t)v;
        return 1;
    }
    if (p >= 0x06080200u && p <= 0x060803FFu) {
        const uint32_t i = (p - 0x06080200u) >> 1;
        if (size == 4) { cz_w(i, (uint16_t)(v >> 16)); cz_w(i + 1, (uint16_t)v); } else cz_w(i, (uint16_t)v);
        return 1;
    }
    if (p >= 0x06300000u && p <= 0x06300003u) {               /* a 32-bit write = index then data (MAME's 16-bit handlers, both halves) */
        if (size == 4) { spr_attr_idx = (v >> 16) >> 1; spr_attr_w((uint16_t)v); }
        else if (p & 2) spr_attr_w((uint16_t)v);
        else spr_attr_idx = (v & 0xFFFF) >> 1;
        return 1;
    }
    return 0;
}

static void pio_mode_w(uint32_t v) { pio_mode = v; spr_pos = 0; }
static int sprite_mode(void) { return pio_mode & 1; }
static void bufpos0(void) { spr_pos = 0; }
/* MAME c435_sprite_w: words 0-1 ignored, 2-3 = the target word index (the last one counts), then data from there */
static void sprite_w(uint16_t w)
{
    if (spr_pos >= 4) spritedata[(uint16_t)(spr_target + spr_pos - 4)] = w;
    else if (spr_pos >= 2) spr_target = w;
    spr_pos++;
}

/* this frame's sprites placed on the screen, for the renderer's snapshot (MAME gorgon_state::render_run; the caller checks C404 layer bit 1) */
static int sprites(s23_sprite *out, int max)
{
    const uint16_t *sd = spritedata;
    /* bit 0 = the list is being rewritten: show the last finished one. MAME draws nothing then, which strobes the timer at 15 Hz when the
     * game runs at 30 (session 8: two frames on, two off, the whole second race); a strobing timer is not what the arcade shows.
     * ponytail: held for 3 frames at most -- after start the bit stays set ~400 frames and the title must go (session 11), as MAME */
    static s23_sprite last[0x280]; static int nlast, held;
    if (sd[0] & 1) { if (held++ >= 3) return 0; const int k = nlast < max ? nlast : max; memcpy(out, last, sizeof *out * (size_t)k); return k; }
    held = 0;
    const int16_t dx = (int16_t)((int16_t)sd[6] + (int16_t)sd[10] + 0x50), dy = (int16_t)((int16_t)sd[12] + ((sd[0] & 4) ? 0x2A : 0x15));
    int count = (uint16_t)(sd[4] - sd[2]) + 1, n = 0;
    if (count > 0x280) count = 0x280;                 /* ponytail: MAME reads past 0x280 into nothing useful; the attribute RAM holds 0x280 */
    { static long rf = -2; extern uint32_t rr_frame; if (rf == -2) { const char *e = getenv("S23_SPRRAW"); rf = e ? atol(e) : -1; }   /* debug: $S23_SPRRAW=frame: raw words */
      if (rf >= 0 && (long)rr_frame == rf) { fprintf(stderr, "[SRAW] hdr"); for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", sd[k]); fprintf(stderr, "\n");
          for (int i = 0; i < count; i++) { fprintf(stderr, "[SRAW] %2d", i); for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", sd[0x4000 + i * 16 + k]); fprintf(stderr, "\n"); } } }
    for (int i = 0; i < count && n < max; i++) {
        const uint16_t *d = &sd[0x4000 + i * 16];
        s23_sprite *s = &out[n];
        /* the position is 11 bits, wrapping after the screen offset; bits 11-15 are flags (the high-score table: x 0x30E8 = 104 on screen;
         * the attract's Parental Advisory / second title: x 0x880.. = 0.. -- 12 bits put them 2048 px off screen, 13 bits the high scores): Rapid River's timer sits at 16,16 with 0x8000 / 0x4000 / 0xC000 / 0x9000 / 0x2000
         * on either word for most of a race (MAME takes all 16 bits: the timer off screen), the name entry's letters at x | 0x4000 */
        s->x = (int16_t)((((d[0] - dx) & 0x7FF) ^ 0x400) - 0x400); s->y = (int16_t)((((d[2] - dy) & 0x7FF) ^ 0x400) - 0x400); s->xs = d[4] & 0xFF; s->ys = d[6] & 0xFF;   /* bit 8 is a flag, not size (Rapid River route-select picture: 0x120 = 32 + 0x100); MAME takes all 16 bits */
        s->rows = d[10] & 7 ? d[10] & 7 : 8; s->cols = (d[10] >> 4) & 7 ? (d[10] >> 4) & 7 : 8;
        s->alpha = d[14] >> 8; s->code = d[12];
        s->color = spr_attr[i][2] & 0xFF; s->fade = spr_attr[i][3] >> 15; s->cz = spr_attr[i][3] & 0xFF; { static long k = 0; if (!k) { const char *e = getenv("S23_SPRZ_SCALE"); k = e && atol(e) > 0 ? atol(e) : 4; }   /* the depth part (low 21 bits) in polygon depth units */
          const uint64_t zz = (uint64_t)((uint32_t)spr_attr[i][0] << 16 | spr_attr[i][1]) % 0x200000 * (uint64_t)k; s->z = zz > 0x1FFFFF ? 0x1FFFFF : (uint32_t)zz; }
        /* ponytail: the top 3 bits (7 on nearly every sprite, 6 on the polygons) are not compared: as MAME's band they put the timer, the spray, the
         * end route lines and "YOUR RANKING" behind every polygon. x4 keeps the sun (0x1637A) behind the helicopter (0x29524) -- $S23_SPRZ_SCALE */
        if (d[10] & 0x200) s->x -= s->xs * s->cols - 1;   /* right justify */
        if (d[10] & 0x100) s->y -= s->ys * s->rows - 1;   /* bottom justify */
        if (d[10] & 8) { s->y += s->ys * s->rows - 1; s->ys = -s->ys; }      /* y flip */
        if (d[10] & 0x80) { s->x += s->xs * s->cols - 1; s->xs = -s->xs; }   /* x flip */
        if (s->xs && s->ys) n++;
    }
    memcpy(last, out, sizeof *out * (size_t)n); nlast = n;
    return n;
}

int s23_gorgon_sprhdr(int i) { return spritedata[i]; }   /* debug ($S23_SPRLOG) */

/* THE DEPTH FOG, as Super 22's (engine/fog_hw.c fog_quad; GORgON is "System 22.5" and its CZ registers match): word 6 picks the bank
 * for cz type 0 (ponytail: every polygon taken as type 0 -- Super 22 measured 0 on all of Prop Cycle's; read the type per polygon if a
 * scene needs it), word 4 bit 2 of the bank's nibble switches it on, bit 1 reverses it, word <bank>'s low byte (sign from bit 15) is
 * the level offset. MAME builds the tables but draws no fog. */
static int fog(uint16_t pcz[256], int *rev, int *delta)
{
    const int bank = czattr[6] & 3;
    memcpy(pcz, czram[bank], 0x100 * sizeof *pcz);
    *rev = (czattr[4] >> (bank * 4) & 2) ? 0xFF : 0;
    const int d = czattr[bank]; *delta = (d & 0x8000) ? (d | 0xFF00) - 0x10000 : (d & 0xFF);
    { static int lg = -1; if (lg < 0) lg = getenv("S23_FOGLOG") != NULL;   /* debug: the registers when they change */
      static uint16_t last[8]; if (lg && memcmp(last, czattr, sizeof last)) { memcpy(last, czattr, sizeof last); extern uint32_t rr_frame;
          fprintf(stderr, "[FOG] f%u czattr %04X %04X %04X %04X %04X %04X %04X %04X; bank %d table %04X %04X .. %04X %04X\n", rr_frame, czattr[0], czattr[1], czattr[2], czattr[3],
                  czattr[4], czattr[5], czattr[6], czattr[7], bank, pcz[0], pcz[1], pcz[254], pcz[255]); } }
    return (czattr[4] >> (bank * 4) & 4) != 0;
}
const s23_gorgon_ops s23_gorgon = { fold, g_read, g_write, pio_mode_w, sprite_mode, sprite_w, bufpos0, sprites, fog };
