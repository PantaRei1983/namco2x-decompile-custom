/* s23_frame.h -- what one frame's picture needs, snapshotted at the start of vertical blank so the picture can be composed on another thread
 * while the CPUs run on (src/s23_render.c fills it, src/s23_text.c and the polygon fill read it) */
#ifndef S23_FRAME_H
#define S23_FRAME_H
#include <stdint.h>
#include "s23_c435.h"
typedef struct s23_sprite {            /* one GORgON sprite, on the screen: rows x cols tiles of 32x32 pixels each drawn xs x ys (negative = flipped) */
    int16_t x, y, xs, ys; uint16_t code; uint8_t rows, cols, color, alpha, fade, cz;
    uint32_t z;                        /* the C404 depth key: its low 21 bits x $S23_SPRZ_SCALE (s23_gorgon.c), against the polygons' low 21 (larger = farther) */
} s23_sprite;
typedef struct {
    uint8_t  c404[0x40];               /* the C404's registers 0x00-0x1F (big-endian words) */
    uint8_t  gamma[3][256];            /* the C404's GAMMA RAM: R / G / B curves at 0x200 / 0x400 / 0x600 (256 words each, low bytes) --
                                        * the last stage of the picture; MAME never applies it, so its picture is darker than the board's */
    uint32_t pens[0x8000];             /* the palette as 0x00RRGGBB */
    uint8_t  text[0x20000];            /* the C361's character RAM (0x06800000) and tile map (0x0681E000) */
    uint16_t linexscroll[1024], yscroll;
    uint16_t sram[0x20000];            /* the C412 SRAM: the stencil tiles */
    s23_render_list list;              /* the render list to draw */
    uint16_t pcz[256];                 /* the C412 PCZ half in use (0x0C000000 bit 2): fog level i is reached at depth pcz[i] (0..0x1FFF) */
    int cz_on;                         /* C412 flags bit 0: the depth fog on */
    int cz_rev, cz_delta;              /* GORgON: level = (table index ^ cz_rev) + cz_delta (0, 0 elsewhere) */
    int nspr; s23_sprite spr[0x280];   /* GORgON: the sprites (s23_game.gorgon->sprites), 0 elsewhere */
} s23_frame_state;
void s23_sprites_draw(const s23_frame_state *fs, int i0, int i1, uint32_t *ovl, int ow, int ox, int bb[4]);   /* sprites i0..i1-1 into a cleared ow x 480 ARGB overlay (alpha 0 = clear),
                                                   * board x 0 at ox; bb = the overlay rectangle touched (x0, y0, x1, y1; empty: x0 >= x1) */
void s23_spr_cell(const s23_frame_state *fs, const s23_sprite *s, int row, int col, uint32_t *cell);   /* one tile of an effect sprite, 34 x 34 premultiplied with its neighbours' rim */
int s23_spr_is_fx(const s23_sprite *s);   /* the game names it an effect sprite (s23_game.spr_fx) */
int s23_fx_smooth(void);                  /* the "Effects" option on (eng_display fx_filter, or $S23_FX_FILTER=0|1 for a run) */
extern int s23_spr_fullpic, s23_spr_hud;   /* s23_spr_hud: a HUD sprite moved out this frame (the game's spr_wide) */   /* this frame's sprites cover the 4:3 screen (a title / map picture): widescreen shows it with bars */
void s23_text_layer(const s23_frame_state *fs, uint16_t *mix, uint8_t *pri);
void s23_text_mix(const s23_frame_state *fs, uint32_t *rgba, const uint16_t *mix, const uint8_t *pri, int prival);
#endif
