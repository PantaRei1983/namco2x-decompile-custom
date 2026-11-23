/* s23_subs.c -- ENGLISH SUBTITLES for a game whose captions are PICTURES in the text layer (Angler King, 2026-10-25).
 *
 * Angler King writes no caption as text: it copies ready-drawn glyph images (Japanese, outlined) into the C361's character RAM from
 * tile 0x084 on and lays them out in the tile map -- the tile NUMBERS are just the next free ones, the PIXELS say which caption it is.
 * So every frame the tile map's caption tiles (code >= 0x084) are grouped into blocks (4-connected), and each block is identified by
 * an FNV-1a hash of its tiles' pixels (and their place and flips in the block, not their palette, which blinks). The table
 * (s23_game.subtitles, e.g. aking/aking_subtitles.txt: "HASH  English text", '|' = a new line, '#' = a comment, "-" = a block that only
 * continues another one's sentence) turns a hash into a line. IN THE GAME (the default, "subtitles = 2"): the English is drawn in place of
 * the Japanese, in the game's own colours (s23_subs_patch, below). IN A BAR ("subtitles = 1", or no font): everything recognised in a frame goes in ONE dark bar -- above the game's
 * caption box when one is in the lower part of the screen, else at the bottom -- over the picture (the menu's layer hook: the picture itself is not touched, and a
 * screenshot / headless shot is the game's own picture).
 *
 *   $S23_SUBLOG=<dir>   every caption block that stays for 10 frames and is not in the table: <dir>/<hash>.pgm (its glyphs, 16 px a
 *                       tile) and a "[SUB] new" line -- how the table is filled.
 *   $S23_SUBDBG=<frame> that frame's caption tiles over the whole map with the scroll, and the patched layer to subdbg.bin
 *   $S23_SUBLAY=1       each in-the-game line's layout (size, width allowed, pens) as it is worked out
 *   $S23_SUBFONT=<ttf>  the font for the English in the game
 * Menu: a "Subtitles" page (English: In the game / In a bar / Off, saved as subtitles = 2 / 1 / 0 in the game's cfg).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "s23_game.h"
#include "eng_ui.h"
#include "eng_nk.h"

uint8_t *s23_vram_ptr(uint32_t);
int eng_cfg_int(const char *, int);
void eng_cfg_set_int(const char *, int);

#define MAXSUB 512
#define MAXBLK 8
static struct { uint32_t hash; char *text; } tab[MAXSUB];
static int ntab, loaded, mode = 2;                      /* "subtitles" in the cfg: 0 off, 1 the bar, 2 in the game */

typedef struct { uint32_t hash; int r0, c0, r1, c1; } blk;
static blk cur[MAXBLK], prev[MAXBLK]; static int ncur, nprev;
static int stable;                                       /* frames the block set has been the same */
static SDL_mutex *lock;
static blk shown[MAXBLK]; static const char *shown_text[MAXBLK]; static int nshown;   /* what the layer draws (under lock) */

static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *e = s + strlen(s); while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0; return s; }
static void load(void)
{
    loaded = 1;
    if (!s23_game.subtitles) return;
    FILE *f = fopen(s23_game.subtitles, "r");
    if (!f) { char *b = SDL_GetBasePath(); if (b) { char p[1024]; snprintf(p, sizeof p, "%s%s", b, s23_game.subtitles); f = fopen(p, "r"); SDL_free(b); } }
    if (!f) { fprintf(stderr, "[SUB] no %s: no subtitles\n", s23_game.subtitles); return; }
    char line[1024];
    while (fgets(line, sizeof line, f) && ntab < MAXSUB) {
        char *s = trim(line), *end;
        if (!*s || *s == '#') continue;
        const unsigned long h = strtoul(s, &end, 16);
        if (end == s) continue;
        char *t = trim(end);
        if (!*t) continue;
        for (char *q = t; *q; q++) if (*q == '|') *q = '\n';
        tab[ntab].hash = (uint32_t)h; tab[ntab].text = strdup(t); ntab++;
    }
    fclose(f);
    fprintf(stderr, "[SUB] %d subtitles from %s\n", ntab, s23_game.subtitles);
}
static const char *find(uint32_t h) { for (int i = 0; i < ntab; i++) if (tab[i].hash == h) return tab[i].text; return NULL; }

static inline uint16_t tm_word(const uint8_t *tm, int r, int c) { const int o = (r * 64 + c) * 2; return (uint16_t)(tm[o] << 8 | tm[o + 1]); }
static inline int is_cap(uint16_t w) { return (w & 0x3FF) >= 0x084; }

/* the caption blocks in a text layer (character RAM cg, tile map tm): 4-connected caption tiles, each with its FNV-1a pixel hash */
static int find_blocks(const uint8_t *cg, const uint8_t *tm, blk *out, int max)
{
    static uint8_t seen[30][40];
    memset(seen, 0, sizeof seen);
    int n = 0;
    for (int r = 0; r < 30; r++) for (int c = 0; c < 40; c++) {
        if (seen[r][c] || !is_cap(tm_word(tm, r, c))) continue;
        int st[1200][2], sp = 0; blk b = { 0, r, c, r, c };
        st[sp][0] = r; st[sp][1] = c; sp++; seen[r][c] = 1;
        while (sp) {                                      /* the block's extent */
            sp--; const int y = st[sp][0], x = st[sp][1];
            if (y < b.r0) b.r0 = y; if (y > b.r1) b.r1 = y; if (x < b.c0) b.c0 = x; if (x > b.c1) b.c1 = x;
            static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; k++) { const int ny = y + d[k][0], nx = x + d[k][1];
                if (ny >= 0 && ny < 30 && nx >= 0 && nx < 40 && !seen[ny][nx] && is_cap(tm_word(tm, ny, nx))) { seen[ny][nx] = 1; st[sp][0] = ny; st[sp][1] = nx; sp++; } }
        }
        uint32_t h = 2166136261u;                        /* FNV-1a over the block in raster order: place, flips, the 128 pixel bytes */
        for (int y = b.r0; y <= b.r1; y++) for (int x = b.c0; x <= b.c1; x++) {
            const uint16_t t = tm_word(tm, y, x);
            if (!is_cap(t)) continue;
            const uint8_t hdr[3] = { (uint8_t)(y - b.r0), (uint8_t)(x - b.c0), (uint8_t)(t >> 10 & 3) };
            for (int k = 0; k < 3; k++) { h ^= hdr[k]; h *= 16777619u; }
            const uint8_t *px = cg + (size_t)(t & 0x3FF) * 128;
            for (int k = 0; k < 128; k++) { h ^= px[k]; h *= 16777619u; }
        }
        b.hash = h;
        if (n < max) out[n++] = b;
    }
    return n;
}

/* ---------------------------------------------------------------- IN THE GAME (mode 2): the English drawn in place of the Japanese
 * The game's captions are lines of outlined-looking glyphs on a box of one pen (the "box pen", e.g. 12) covering each line's tiles,
 * one glyph pen a line (white, red). For a recognised block, in the FRAME SNAPSHOT's copy of the text layer (src/s23_render.c; the game's
 * own memory is never touched, so the next frame's detection still sees the Japanese): each English line ('|') gets its own box in the
 * box pen and its letters in the glyph pen of the Japanese line it replaces (the same palette, so the game's blinking stays), as wide as
 * the English needs (out to the screen edge, then a smaller size), on character tiles of its own: tiles the tile map does not show
 * this frame (the game's are shared between cells). The letters: a bold system font (Segoe UI Bold, Arial Bold; DejaVu / Liberation on
 * Linux; $S23_SUBFONT), baked by Nuklear's stb_truetype at even pixel sizes, one pen, no smoothing (the game's Japanese has none).
 * No font found: the bar (mode 1) instead. */
#define NSIZE 28                                         /* baked sizes: 10, 12, .. 64 px (the results' fish name is 64 px of Japanese) */
static struct nk_font_atlas fatlas;
static struct nk_font *fsize[NSIZE];
static uint8_t *fimg; static int fimg_w, font_state;     /* font_state 0 not tried, 1 ready, -1 none */
static int font_load(void)
{
    if (font_state) return font_state > 0;
    font_state = -1;
    char win[600] = ""; const char *wd = getenv("WINDIR");
    const char *paths[8] = { getenv("S23_SUBFONT"), NULL, NULL,
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                             "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", NULL };
    static char p1[600], p2[600];
    if (wd) { snprintf(p1, sizeof p1, "%s\\Fonts\\segoeuib.ttf", wd); snprintf(p2, sizeof p2, "%s\\Fonts\\arialbd.ttf", wd); paths[1] = p1; paths[2] = p2; }
    (void)win;
    const char *use = NULL;
    for (int i = 0; i < 7 && !use; i++) if (paths[i] && *paths[i]) { FILE *f = fopen(paths[i], "rb"); if (f) { fclose(f); use = paths[i]; } }
    if (!use) { fprintf(stderr, "[SUB] no bold font found: the English goes in a bar instead\n"); return 0; }
    nk_font_atlas_init_default(&fatlas);
    nk_font_atlas_begin(&fatlas);
    for (int i = 0; i < NSIZE; i++) {
        struct nk_font_config cfg = nk_font_config((float)(10 + 2 * i));
        cfg.oversample_h = 1; cfg.oversample_v = 1; cfg.pixel_snap = nk_true; cfg.coord_type = NK_COORD_PIXEL;
        cfg.range = nk_font_default_glyph_ranges();
        fsize[i] = nk_font_atlas_add_from_file(&fatlas, use, (float)(10 + 2 * i), &cfg);
        if (!fsize[i]) { fprintf(stderr, "[SUB] cannot read the font %s: the English goes in a bar instead\n", use); return 0; }
    }
    int w = 0, h = 0;
    const void *img = nk_font_atlas_bake(&fatlas, &w, &h, NK_FONT_ATLAS_ALPHA8);
    if (!img) return 0;
    fimg = malloc((size_t)w * (size_t)h); if (!fimg) return 0;
    memcpy(fimg, img, (size_t)w * (size_t)h); fimg_w = w;
    nk_font_atlas_end(&fatlas, nk_handle_id(0), NULL);
    font_state = 1;
    fprintf(stderr, "[SUB] English drawn in the game with %s\n", use);
    return 1;
}
static int text_w(const struct nk_font *f, const char *s, int n)
{
    float w = 0;
    for (int i = 0; i < n; i++) { const struct nk_font_glyph *g = nk_font_find_glyph(f, (nk_rune)(uint8_t)s[i]); if (g) w += g->xadvance; }
    return (int)(w + 0.99f);
}

static int line_h(const struct nk_font *f) { return (int)(f->info.ascent - f->info.descent + 0.99f); }
/* the canvas: one block's English, in pixels over tile rows r0.. and columns 0..39 (pen 0xF = nothing) */
#define CV_W 640
static uint8_t cv[480][CV_W];
static void put_text(const struct nk_font *f, const char *s, int n, int x, int y, uint8_t pen)
{
    float pen_x = (float)x;
    for (int i = 0; i < n; i++) {
        const struct nk_font_glyph *g = nk_font_find_glyph(f, (nk_rune)(uint8_t)s[i]);
        if (!g) continue;
        const int gx = (int)(pen_x + g->x0), gy = y + (int)g->y0, gw = (int)(g->u1 - g->u0), gh = (int)(g->v1 - g->v0);
        for (int v = 0; v < gh; v++) for (int u = 0; u < gw; u++) {
            const int X = gx + u, Y = gy + v;
            if (X < 0 || X >= CV_W || Y < 0 || Y >= 480) continue;
            if (fimg[(size_t)((int)g->v0 + v) * (size_t)fimg_w + (size_t)((int)g->u0 + u)] >= 110) cv[Y][X] = pen;
        }
        pen_x += g->xadvance;
    }
}
static inline int cg_pen(const uint8_t *cg, uint16_t t, int x, int y)   /* pixel (x, y) of the cell whose tile entry is t */
{
    const int ty = (t & 0x800) ? 15 - y : y, tx = (t & 0x400) ? 15 - x : x;
    const uint8_t *row = cg + (size_t)(t & 0x3FF) * 128 + ty * 8;
    return (row[tx >> 1] >> ((tx & 1) ? 0 : 4)) & 0xF;
}
static int cell_ok(const uint8_t *cg, const uint8_t *tm, const blk *b, int r, int c)
{
    const uint16_t t = tm_word(tm, r, c);
    if (is_cap(t) && r >= b->r0 && r <= b->r1 && c >= b->c0 && c <= b->c1) return 1;
    const uint8_t *px = cg + (size_t)(t & 0x3FF) * 128;   /* else: an empty cell, or a plain one-pen tile (the caption box's filler, 0x080) */
    for (int k = 0; k < 128; k++) if (px[k] != px[0] || (px[0] >> 4) != (px[0] & 0xF)) return 0;
    return 1;
}
/* ONE CAPTION'S ENGLISH, worked out once (cached by its hash): the cells to show, relative to the block's top-left tile, each with its
 * 16x16 pixels and the cell whose palette it takes every frame (the game blinks some captions through the palette) */
typedef struct { int8_t dr, dc, pr, pc; uint8_t px[128]; } en_cell;
typedef struct { uint32_t hash; int n; en_cell *cell; } en_lay;
#define NLAY 160
static en_lay lay[NLAY]; static int nlay;
static uint8_t P[480][640];                              /* the block's pixels (pen 0xF outside its caption cells) */
static int ingame_layout(const uint8_t *cg, const uint8_t *tm, const blk *b, const char *text, en_lay *out)
{
    const int R0 = b->r0, C0 = b->c0, nr = b->r1 - b->r0 + 1, nc = b->c1 - b->c0 + 1, H = nr * 16, W = nc * 16;
    int hist[16] = { 0 };
    for (int r = 0; r < nr; r++) for (int c = 0; c < nc; c++) { const uint16_t t = tm_word(tm, R0 + r, C0 + c);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { const int p = is_cap(t) ? cg_pen(cg, t, x, y) : 0xF; P[r * 16 + y][c * 16 + x] = (uint8_t)p; if (is_cap(t)) hist[p]++; } }
    int tot = 0; for (int p = 0; p < 16; p++) tot += hist[p];   /* (only the caption's own cells: an L-shaped block's empty corner is not see-through) */
    int box = 0; for (int p = 1; p < 15; p++) if (hist[p] > hist[box]) box = p;
    const char *ls[16]; int ll[16], ne = 0;
    if (text[0] != '-' || text[1]) for (const char *s = text; *s && ne < 16; ) { const char *e = strchr(s, '\n'); ls[ne] = s; ll[ne] = e ? (int)(e - s) : (int)strlen(s); ne++; s = e ? e + 1 : s + strlen(s); }
    memset(cv, 0xF, sizeof cv);
    uint8_t cpr[30][40], cpc[30][40];                    /* each canvas cell's palette source (block-relative) */
    for (int r = 0; r < 30; r++) for (int c = 0; c < 40; c++) { cpr[r][c] = (uint8_t)(r < nr ? r : 0); cpc[r][c] = (uint8_t)(c < nc ? c : 0); }
    int letters_only = 0;
    if (hist[0xF] * 100 >= tot * 3) {
        /* (a shape or bare letters: ONE palette for all its English -- the block's commonest; its own cells' would colour pieces of the
         * English as the Japanese words were, e.g. the ranking title's red "marlin") */
        int pc[16] = { 0 }, best = 0, br = 0, bc = 0;
        for (int r = 0; r < nr; r++) for (int c = 0; c < nc; c++) { const uint16_t t = tm_word(tm, R0 + r, C0 + c); if (is_cap(t)) pc[t >> 12]++; }
        for (int p = 1; p < 16; p++) if (pc[p] > pc[best]) best = p;
        for (int r = 0; r < nr; r++) for (int c = 0; c < nc; c++) { const uint16_t t = tm_word(tm, R0 + r, C0 + c); if (is_cap(t) && (t >> 12) == best) { br = r; bc = c; r = nr; break; } }
        for (int r = 0; r < 30; r++) for (int c = 0; c < 40; c++) { cpr[r][c] = (uint8_t)br; cpc[r][c] = (uint8_t)bc; }
        /* A SHAPE (a speech bubble, a tag): keep it; rub out the letters inside it -- the pixels other than its fill well inside the
         * shape (5 px from any see-through pixel: the soft edge stays) -- and write the English there, centred, in the letters' pen */
        int x0 = W, y0 = H, x1 = -1, y1 = -1, ph[16] = { 0 }, inner = 0;
        for (int y = 5; y < H - 5; y++) for (int x = 5; x < W - 5; x++) {
            const int p = P[y][x]; if (p == 0xF) continue;
            int in = 1; for (int dy = -5; dy <= 5 && in; dy++) for (int dx = -5; dx <= 5 && in; dx++) in = P[y + dy][x + dx] != 0xF;
            if (!in) continue;
            inner++;
            if (p == box) continue;
            ph[p]++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
        /* a shape is mostly solid inside (a bubble: ~2/3 of it 5 px from its edge); big outlined letters are mostly near an edge */
        if (getenv("S23_SUBLAY")) fprintf(stderr, "[SUBLAY] %08X rows %d-%d cols %d-%d see-through %d of %d, inner %d of %d (%d%%): %s\n", b->hash, b->r0, b->r1, b->c0, b->c1, hist[0xF], tot, inner,
                                          tot - hist[0xF], (tot - hist[0xF]) ? inner * 100 / (tot - hist[0xF]) : 0, x1 < 0 || inner * 100 < (tot - hist[0xF]) * 35 ? "letters" : "shape");
        if (x1 < 0 || inner * 100 < (tot - hist[0xF]) * 35) letters_only = 1; else {
        int pen = 0; for (int p = 1; p < 15; p++) if (ph[p] > ph[pen]) pen = p;
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (P[y][x] != 0xF) P[y][x] = (uint8_t)box;
        for (int y = 0; y < H; y++) memcpy(cv[y], P[y], (size_t)W);
        const int th = y1 - y0 + 1, tw = x1 - x0 + 1;
        int s = NSIZE - 1;
        for (; s > 0; s--) { int fit = line_h(fsize[s]) * ne <= th + 2; for (int i = 0; i < ne && fit; i++) fit = text_w(fsize[s], ls[i], ll[i]) <= tw; if (fit) break; }
        const struct nk_font *f = fsize[s]; const int lh = line_h(f), top = y0 + (th - lh * ne) / 2;
        for (int i = 0; i < ne; i++) put_text(f, ls[i], ll[i], x0 + (tw - text_w(f, ls[i], ll[i])) / 2, top + i * lh, (uint8_t)pen);
        }
    } else {
        /* LINES ON BOXES (the usual caption): each Japanese line -- a run of tile rows with the same column range -- is a box in the box
         * pen with its letters in one glyph pen; each English line gets such a box, as wide as it needs (into empty cells only) */
        struct { int r0, r1, c0, c1, pen; } jl[30]; int nj = 0;
        for (int r = 0; r < nr; r++) {
            int c0 = 99, c1 = -1, ph[16] = { 0 };
            for (int c = 0; c < nc; c++) { if (!is_cap(tm_word(tm, R0 + r, C0 + c))) continue; if (c < c0) c0 = c; if (c > c1) c1 = c;
                for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) ph[P[r * 16 + y][c * 16 + x]]++; }
            if (c1 < 0) continue;
            int pen = -1; for (int p = 0; p < 15; p++) if (p != box && ph[p] && (pen < 0 || ph[p] > ph[pen])) pen = p;
            if (nj && jl[nj - 1].r1 == r - 1 && jl[nj - 1].c0 == c0 && jl[nj - 1].c1 == c1) { jl[nj - 1].r1 = r; if (jl[nj - 1].pen < 0) jl[nj - 1].pen = pen; continue; }
            if (nj < 30) { jl[nj].r0 = jl[nj].r1 = r; jl[nj].c0 = c0; jl[nj].c1 = c1; jl[nj].pen = pen; nj++; }
        }
        if (!nj) return 0;
        for (int i = 0; i < ne; i++) {
            int y0, y1, j = 0;
            if (ne == nj) { j = i; y0 = jl[i].r0 * 16; y1 = (jl[i].r1 + 1) * 16; }
            else { y0 = i * H / ne; y1 = (i + 1) * H / ne; for (int k = 0; k < nj; k++) if (jl[k].r0 * 16 <= y0) j = k; }
            int cmax = 39 - C0;                          /* to the right while the cells are this block's or empty (never over other text) */
            for (int r = y0 / 16; r <= (y1 - 1) / 16 && R0 + r < 30; r++) { int c = jl[j].c0; while (C0 + c + 1 < 40 && cell_ok(cg, tm, b, R0 + r, C0 + c + 1)) c++; if (c < cmax) cmax = c;
                if (getenv("S23_SUBLAY")) fprintf(stderr, "[SUBLAY] row %d stops at col %d: entry %04X (block rows %d-%d cols %d-%d)\n", R0 + r, C0 + c + 1, tm_word(tm, R0 + r, C0 + c + 1), b->r0, b->r1, b->c0, b->c1); }
            const int x0 = jl[j].c0 * 16, xmax = (cmax + 1) * 16 - 4, hgt = y1 - y0;
            int s = NSIZE - 1;                           /* the largest whose real line (ascent to descent) fills the box, then fits its width */
            while (s > 0 && (line_h(fsize[s]) > hgt - 2 || x0 + 8 + text_w(fsize[s], ls[i], ll[i]) > xmax)) s--;
            const struct nk_font *f = fsize[s];
            int bx1 = x0 + 8 + text_w(f, ls[i], ll[i]) + 6; bx1 = (bx1 + 15) & ~15; if (bx1 > (cmax + 1) * 16) bx1 = (cmax + 1) * 16;
            for (int y = y0; y < y1; y++) memset(&cv[y][x0], box, (size_t)(bx1 - x0));
            if (getenv("S23_SUBLAY")) fprintf(stderr, "[SUBLAY] %08X line %d of %d (jp %d): y %d-%d x0 %d xmax %d size %d line_h %d w %d pen %d box %d\n",
                                              b->hash, i, ne, nj, y0, y1, x0, xmax, 10 + 2 * s, line_h(f), text_w(f, ls[i], ll[i]), jl[j].pen, box);
            put_text(f, ls[i], ll[i], x0 + 6, y0 + (hgt - line_h(f)) / 2, (uint8_t)(jl[j].pen < 0 ? box ^ 1 : jl[j].pen));
            for (int r = y0 / 16; r <= (y1 - 1) / 16 && r < 30; r++) for (int c = x0 / 16; c < bx1 / 16 && c < 40; c++) { cpr[r][c] = (uint8_t)jl[j].r0; cpc[r][c] = (uint8_t)jl[j].c0; }
        }
    }
    if (letters_only) {
        /* OUTLINED LETTERS ON NOTHING (the attract's promo lines, the ranking title): the outline pen = the commonest next to the
         * see-through, the letters' pen the commonest other; the English in the letters' pen with a 2-pixel outline, left-aligned,
         * as big as the block's height allows, wider into empty cells if it needs */
        int oh[16] = { 0 }, gh[16] = { 0 };
        for (int y = 1; y < H - 1; y++) for (int x = 1; x < W - 1; x++) { const int p = P[y][x]; if (p == 0xF) continue;
            if (P[y - 1][x] == 0xF || P[y + 1][x] == 0xF || P[y][x - 1] == 0xF || P[y][x + 1] == 0xF) oh[p]++; else gh[p]++; }
        int op = 0; for (int p = 1; p < 15; p++) if (oh[p] > oh[op]) op = p;
        int gp = op ? 0 : 1; for (int p = 0; p < 15; p++) if (p != op && gh[p] > gh[gp]) gp = p;
        /* ONE PEN (the continue question's white, the stage intros' red: the glyphs' own shading is all one pen): the English in that pen with
         * no outline -- the next-commonest pen is a stray (or pen 0, unused), and drawing the letters in it washed them out */
        const int one_pen = gh[gp] * 4 < gh[op];
        if (one_pen) gp = op;
        if (getenv("S23_SUBLAY")) { fprintf(stderr, "[SUBLAY] %08X letters: outline pen %d, letter pen %d; edge pens", b->hash, op, gp);
            for (int p = 0; p < 16; p++) if (oh[p]) fprintf(stderr, " %d:%d", p, oh[p]); fprintf(stderr, "; inside pens");
            for (int p = 0; p < 16; p++) if (gh[p]) fprintf(stderr, " %d:%d", p, gh[p]); fprintf(stderr, "\n"); }
        memset(cv, 0xF, sizeof cv);
        for (int i = 0; i < ne; i++) {
            const int y0 = i * H / ne, y1 = (i + 1) * H / ne, hgt = y1 - y0;
            int cmax = 39 - C0;
            for (int r = y0 / 16; r <= (y1 - 1) / 16 && R0 + r < 30; r++) { int c = 0; while (C0 + c + 1 < 40 && cell_ok(cg, tm, b, R0 + r, C0 + c + 1)) c++; if (c < cmax) cmax = c; }
            const int xmax = (cmax + 1) * 16 - 3;
            int s = NSIZE - 1;
            while (s > 0 && (line_h(fsize[s]) > hgt - 4 || 3 + text_w(fsize[s], ls[i], ll[i]) > xmax)) s--;
            if (getenv("S23_SUBLAY")) fprintf(stderr, "[SUBLAY] %08X letters line %d: height %d, width allowed %d, size index %d\n", b->hash, i, hgt, xmax, s);
            put_text(fsize[s], ls[i], ll[i], 3, y0 + (hgt - line_h(fsize[s])) / 2, (uint8_t)gp);
        }
        for (int y = 0; y < 480 && !one_pen; y++) for (int x = 0; x < CV_W; x++) {
            if (cv[y][x] != 0xF) continue;
            int near = 0;
            for (int dy = -2; dy <= 2 && !near; dy++) for (int dx = -2; dx <= 2 && !near; dx++) {
                const int yy = y + dy, xx = x + dx;
                near = yy >= 0 && yy < 480 && xx >= 0 && xx < CV_W && cv[yy][xx] == gp && (dx * dx + dy * dy) <= 5; }
            if (near) cv[y][x] = 0xE0 | (uint8_t)op;         /* (marked: not a letter for the next pixels' test) */
        }
        for (int y = 0; y < 480; y++) for (int x = 0; x < CV_W; x++) if (cv[y][x] & 0xE0) cv[y][x] &= 0xF;
    }
    int n = 0; en_cell *cells = malloc(sizeof *cells * 30 * 40); if (!cells) return 0;
    for (int r = 0; r + R0 < 30; r++) for (int c = 0; c + C0 < 40; c++) {
        int any = 0;
        for (int y = 0; y < 16 && !any; y++) for (int x = 0; x < 16 && !any; x++) any = cv[r * 16 + y][c * 16 + x] != 0xF;
        if (!any) continue;
        en_cell *e = &cells[n++]; e->dr = (int8_t)r; e->dc = (int8_t)c; e->pr = (int8_t)cpr[r][c]; e->pc = (int8_t)cpc[r][c];
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x += 2) e->px[y * 8 + x / 2] = (uint8_t)(cv[r * 16 + y][c * 16 + x] << 4 | cv[r * 16 + y][c * 16 + x + 1]);
    }
    out->hash = b->hash; out->n = n; out->cell = cells;
    return 1;
}
/* this frame: the block's cells blank, the English cells on spare tiles, each with the palette its source cell has now */
static void ingame_apply(uint8_t *cg, uint8_t *tm, const blk *b, const en_lay *L, uint16_t *spare, int *nspare)
{
    if (L->n + 1 > *nspare) return;
    uint8_t pal[30][40] = { { 0 } };
    for (int r = b->r0; r <= b->r1; r++) for (int c = b->c0; c <= b->c1; c++) pal[r - b->r0][c - b->c0] = (uint8_t)(tm_word(tm, r, c) >> 12);
    const uint16_t blank = spare[--*nspare]; memset(cg + (size_t)blank * 128, 0xFF, 128);
    for (int r = b->r0; r <= b->r1; r++) for (int c = b->c0; c <= b->c1; c++) { const int o = (r * 64 + c) * 2; const uint16_t t = tm_word(tm, r, c);
        if (is_cap(t)) { const uint16_t w = (uint16_t)((t & 0xF000) | blank); tm[o] = (uint8_t)(w >> 8); tm[o + 1] = (uint8_t)w; } }
    for (int i = 0; i < L->n; i++) {
        const en_cell *e = &L->cell[i]; const int r = b->r0 + e->dr, c = b->c0 + e->dc;
        const uint16_t code = spare[--*nspare];
        memcpy(cg + (size_t)code * 128, e->px, 128);
        const uint16_t w = (uint16_t)(pal[e->pr][e->pc] << 12 | code); const int o = (r * 64 + c) * 2;
        tm[o] = (uint8_t)(w >> 8); tm[o + 1] = (uint8_t)w;
    }
}
/* the frame snapshot's text layer (src/s23_render.c, right after it is copied): the recognised captions in English. An entry starting
 * with '!' is a picture's label (the lure appeal meter): in the game it keeps its Japanese */
void s23_subs_patch(uint8_t *text, const uint16_t *lx, uint16_t ys)
{
    { static long dbg = -2; static unsigned long fn; fn++; if (dbg == -2) { const char *e = getenv("S23_SUBDBG"); dbg = e ? atol(e) : -1; }
      if (dbg > 0 && (long)fn == dbg) {                  /* $S23_SUBDBG=frame: the caption tiles over the whole map, and the scroll */
        const uint8_t *tm = text + 0x1E000;
        fprintf(stderr, "[SUBDBG] frame %lu yscroll %u\n", fn, ys);
        for (int r = 0; r < 64; r++) { int c0 = -1, c1 = -1; for (int c = 0; c < 64; c++) if (is_cap(tm_word(tm, r, c))) { if (c0 < 0) c0 = c; c1 = c; }
            if (c0 >= 0) fprintf(stderr, "[SUBDBG] row %2d cols %2d-%2d  xscroll %u\n", r, c0, c1, lx[(r * 16 + 8) & 0x3FF]); } } }
    if (!s23_game.subtitles || !loaded || mode != 2 || !font_load()) return;
    uint8_t *cg = text, *tm = text + 0x1E000;
    blk bl[MAXBLK]; const int n = find_blocks(cg, tm, bl, MAXBLK);
    int any = 0; for (int i = 0; i < n && !any; i++) { const char *t = find(bl[i].hash); any = t && t[0] != '!'; }
    if (!any) goto done;
    static uint8_t used[1024]; memset(used, 0, sizeof used);
    for (int i = 0; i < 64 * 64; i++) used[(tm[2 * i] << 8 | tm[2 * i + 1]) & 0x3FF] = 1;
    uint16_t spare[960]; int nspare = 0;
    for (int c = 959; c >= 0x084; c--) if (!used[c]) spare[nspare++] = (uint16_t)c;   /* (codes 960+ would read the tile map) */
    for (int i = 0; i < n; i++) {
        const char *t = find(bl[i].hash); if (!t || t[0] == '!') continue;
        int k = 0; while (k < nlay && lay[k].hash != bl[i].hash) k++;
        if (k == nlay) { if (nlay == NLAY || !ingame_layout(cg, tm, &bl[i], t, &lay[nlay])) continue; nlay++; }
        ingame_apply(cg, tm, &bl[i], &lay[k], spare, &nspare);
    }
done:
    { static long dbg = -2; static unsigned long fn; fn++; if (dbg == -2) { const char *e = getenv("S23_SUBDBG"); dbg = e ? atol(e) : -1; }
      if (dbg > 0 && (long)fn >= dbg) { fprintf(stderr, "[SUBDBG] patched: %d blocks, %d known\n", n, any); FILE *f = fopen("subdbg.bin", "wb"); if (f) { fwrite(text, 1, 0x20000, f); fclose(f); } dbg = -1; } }   /* (the patched layer) */
}


static void dump_pgm(const char *dir, const blk *b, const uint8_t *tm, const uint8_t *cg)
{
    char p[512]; snprintf(p, sizeof p, "%s/%08X.pgm", dir, b->hash);
    FILE *f = fopen(p, "wb"); if (!f) return;
    const int w = (b->c1 - b->c0 + 1) * 16, h = (b->r1 - b->r0 + 1) * 16;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint16_t t = tm_word(tm, b->r0 + y / 16, b->c0 + x / 16);
        uint8_t v = 0;
        if (is_cap(t)) {
            const int ty = (t & 0x800) ? 15 - y % 16 : y % 16, tx = (t & 0x400) ? 15 - x % 16 : x % 16;
            const uint8_t *row = cg + (size_t)(t & 0x3FF) * 128 + ty * 8;   /* 16 pixels, 4 bits each, big-endian nibbles */
            const int pen = (row[tx >> 1] >> ((tx & 1) ? 0 : 4)) & 0xF;
            v = pen == 0xF ? 0 : (uint8_t)(255 - pen * 12);
        }
        fputc(v, f);
    }
    fclose(f);
}

/* every game frame (src/s23_main.c s23_on_vblank): find the caption blocks */
void s23_subs_frame(uint32_t frame)
{
    if (!s23_game.subtitles) return;
    if (!loaded) { load(); mode = eng_cfg_int("subtitles", 2); if (mode < 0 || mode > 2) mode = 2; if (!lock) lock = SDL_CreateMutex();
                   const char *e = getenv("S23_SUBMODE"); if (e) mode = atoi(e); }   /* $S23_SUBMODE=0/1/2: tests (a headless run reads no cfg) */
    static const char *logdir; static int ld = -1;
    if (ld < 0) { logdir = getenv("S23_SUBLOG"); ld = 1; }
    const uint8_t *cg = s23_vram_ptr(0x06800000u), *tm = s23_vram_ptr(0x0681E000u);
    ncur = find_blocks(cg, tm, cur, MAXBLK);
    int same = ncur == nprev;
    for (int i = 0; same && i < ncur; i++) same = cur[i].hash == prev[i].hash;
    stable = same ? stable + 1 : 0;
    memcpy(prev, cur, sizeof cur); nprev = ncur;
    if (stable == 10 && logdir) {                       /* a caption that stays: log it if the table does not have it */
        static uint32_t logged[1024]; static int nlog;
        for (int i = 0; i < ncur; i++) {
            int had = find(cur[i].hash) != NULL;
            for (int k = 0; k < nlog && !had; k++) had = logged[k] == cur[i].hash;
            if (had) continue;
            if (nlog < 1024) logged[nlog++] = cur[i].hash;
            dump_pgm(logdir, &cur[i], tm, cg);
            fprintf(stderr, "[SUB] new %08X frame %u rows %d-%d cols %d-%d\n", cur[i].hash, frame, cur[i].r0, cur[i].r1, cur[i].c0, cur[i].c1);
        }
    }
    if (lock) {
        SDL_LockMutex(lock);
        nshown = 0;
        if (stable >= 2) for (int i = 0; i < ncur; i++) { const char *t = find(cur[i].hash); if (t) { shown[nshown] = cur[i]; shown_text[nshown] = t; nshown++; } }
        SDL_UnlockMutex(lock);
    }
}

/* ---------------------------------------------------------------- drawing: a layer of the menu (over the picture, menu closed) */
bool s23_pic_rect_win(float *x, float *y, float *w, float *h);   /* src/s23_window.c: the game's 4:3 area in window units */
const struct nk_user_font *eng_ui_font_big(float h);
static void layer_draw(struct nk_context *ctx, int ww, int wh, bool menu_open)
{
    (void)ww; (void)wh;
    if (menu_open || !lock || !(mode == 1 || (mode == 2 && font_state < 0))) return;   /* the bar: chosen, or no font for in the game */
    float px, py, pw, ph;
    if (!s23_pic_rect_win(&px, &py, &pw, &ph)) return;
    const struct nk_user_font *big = eng_ui_font_big(ph / 30.0f * 1.3f);    /* about 1.3 of the game's text rows (its captions are two rows a line) */
    if (!big) return;
    /* ONE bar for everything recognised this frame (a promo line can be two blocks shown together; the tutorial's title and its
     * caption are up at once), top-to-bottom; a "-" entry continues another block and adds nothing */
    char lines[16][256]; int n = 0, low = 99;
    SDL_LockMutex(lock);
    int ord[MAXBLK]; for (int i = 0; i < nshown; i++) ord[i] = i;
    for (int i = 1; i < nshown; i++) for (int k = i; k > 0 && shown[ord[k]].r0 < shown[ord[k - 1]].r0; k--) { const int t = ord[k]; ord[k] = ord[k - 1]; ord[k - 1] = t; }
    for (int j = 0; j < nshown; j++) {
        const blk *b = &shown[ord[j]]; const char *t = shown_text[ord[j]];
        if (b->r1 >= 18 && b->r0 < low) low = b->r0;      /* a caption box in the lower part: the bar goes above it */
        if (!strcmp(t, "-")) continue;
        if (*t == '!') t++;
        for (const char *s = t; *s && n < 16; ) {
            const char *e = strchr(s, '\n'); const size_t l = e ? (size_t)(e - s) : strlen(s);
            snprintf(lines[n++], sizeof lines[0], "%.*s", (int)(l < 255 ? l : 255), s);
            s = e ? e + 1 : s + l;
        }
    }
    SDL_UnlockMutex(lock);
    if (!n) return;
    const float lh = big->height + 4, tile = ph / 30.0f;
    float tw = 0;
    for (int k = 0; k < n; k++) { const float w = big->width(big->userdata, big->height, lines[k], (int)strlen(lines[k])); if (w > tw) tw = w; }
    float bw = tw + 32, bh = n * lh + 14;
    if (bw > pw - 8) bw = pw - 8;
    const float bx = px + (pw - bw) / 2;
    float by = low < 99 ? py + (low - 1) * tile - bh - 4 : py + ph - bh - 6;
    if (by < py + 2) by = py + 2;
    struct nk_style_item keep = ctx->style.window.fixed_background;
    const struct nk_vec2 pad = ctx->style.window.padding;
    ctx->style.window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 200));
    ctx->style.window.padding = nk_vec2(8, 6);
    nk_style_push_font(ctx, big);
    if (nk_begin(ctx, "subtitles", nk_rect(bx, by, bw, bh), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
        for (int k = 0; k < n; k++) { nk_layout_row_dynamic(ctx, lh, 1); nk_label_colored(ctx, lines[k], NK_TEXT_CENTERED, nk_rgb(255, 245, 200)); }
    }
    nk_end(ctx);
    nk_style_pop_font(ctx);
    ctx->style.window.fixed_background = keep; ctx->style.window.padding = pad;
}
static const eng_ui_layer sub_layer = { layer_draw, NULL, NULL };

/* ---------------------------------------------------------------- the menu page */
static int p_nrows(void) { return 1; }
static void p_text(int row, char *l, size_t ln, char *v, size_t vn) { (void)row; static const char *const nm[3] = { "Off", "In a bar", "In the game" }; snprintf(l, ln, "English"); snprintf(v, vn, "%s", nm[mode]); }
static void p_change(int row, int dir) { (void)row; mode = (mode + (dir < 0 ? 2 : 1)) % 3; eng_cfg_set_int("subtitles", mode); }
static void p_notes(void (*line)(const char *fmt, ...)) { line("%d captions translated (%s).", ntab, s23_game.subtitles ? s23_game.subtitles : "-"); line("In the game: the English drawn in place of the Japanese."); line("In a bar: an English line over the Japanese."); }
static const eng_ui_page sub_page = { "Subtitles", 360, 160, 0, p_nrows, NULL, NULL, p_text, p_change, p_notes };

void s23_subs_ui_init(void)                             /* src/s23_window.c, after eng_ui_init */
{
    if (!s23_game.subtitles) return;
    if (!loaded) { load(); mode = eng_cfg_int("subtitles", 2); if (mode < 0 || mode > 2) mode = 2; if (!lock) lock = SDL_CreateMutex();
                   const char *e = getenv("S23_SUBMODE"); if (e) mode = atoi(e); }   /* $S23_SUBMODE=0/1/2: tests (a headless run reads no cfg) */
    eng_ui_add_page(&sub_page);
    eng_ui_add_layer(&sub_layer);
    font_load();                                         /* now, not at the first caption (baking the sizes took a few frames) */
}
