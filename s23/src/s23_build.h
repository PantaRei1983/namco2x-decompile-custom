/* s23_build.h -- ONE FRAME, BUILT: what both picture back ends draw.
 *   The geometry stage (src/s23_render.c, on the worker thread) turns a frame snapshot (src/s23_frame.h) into screen-space polygons in the
 *   hardware's draw order (z-sort key, far to near) plus the C361 text layer as a pen map. Then either
 *     - the GPU back end (src/s23_gl.c): one draw call over all polygons in that order, texturing / shading / fade / alpha in a shader, the
 *       text layer and the polygon-over-text priority in two more passes -- at any internal resolution; or
 *     - the software back end (src/s23_render.c): the same at 640 x 480 on the CPU -- the fallback, and the oracle the GPU is gated against.
 */
#ifndef S23_BUILD_H
#define S23_BUILD_H
#include <stdint.h>
#include "s23_frame.h"

typedef struct { float x, y, p[4]; } s23_pvert;       /* screen x / y (projected, before the viewport offset); p[0] = 1/z, p[1..3] = u, v, shade times 1/z */
#define S23_POLY_MAXV 20                    /* a polygon's corners after near-plane clipping (point ROM: up to 15) */
typedef struct {
    s23_pvert pv[S23_POLY_MAXV]; int n;   /* up to 15 corners from the point ROM, + 1 per clipped edge pair */
    uint32_t zkey, index;
    int stencil, cmode, tbase, color_base, blend, prioverchar, alpha_enabled;
    int vp_size_x, vp_size_y, vp_offset_x, vp_offset_y;
    int clip43;                                        /* widescreen: clip to the 4:3 picture (s23_render_entry.clip43) */
    int boost;                                         /* shade may go to 2x (s23_game.imm_adjust: Gunmen Wars' namco logo) */
    int role;                                          /* s23_game.depth_role */
    int entry, model;                                  /* the render-list entry it came from and its model (-1 = immediate / direct) */
    int fog;                                           /* takes the depth fog (bit 15 of its colour word clear, as Super System 22's C-fog gate) */
} s23_poly;
/* the C404's per-frame values every polygon uses */
typedef struct { int pr, pg, pb, alpha_color, alpha_pen, alpha, fr, fg, fb, ffactor, fflags, layers; uint32_t bg;
                 int fog, cr, cg, cb; } s23_fx;           /* fog: the depth fog is on this frame (fogtab non-zero somewhere); cr/cg/cb its colour (C404 5-7) */

#define S23_MAX_POLYS 30000
typedef struct {
    s23_frame_state fs;                                /* the snapshot */
    s23_fx fx;
    s23_poly polys[S23_MAX_POLYS]; int npoly;
    uint32_t order[S23_MAX_POLYS];                     /* indices into polys[], in draw order */
    uint16_t mix[640 * 480];                           /* the text layer: pen | 0x8000 where opaque, 0 where transparent */
    uint8_t fogtab[0x2000];                            /* depth (z * 64, i.e. zsort >> 8) -> fog level 0..255: the PCZ half expanded as Super 22's
                                                        * recalc_czram (engine/fog_hw.c recalc_bank) */
    uint16_t band[480];                                /* widescreen: a text row opaque all the way across (a letterbox band): its commonest
                                                        * pen | 0x8000 -- the side strips continue it; 0 = not a band */
} s23_built;

/* the newest built frame (the window's renderer reads it on the main thread); NULL before the first */
const s23_built *s23_built_latest(void);
const s23_built *s23_built_latest_wait(void);          /* waits for the frame that just ended */
const s23_built *s23_built_mid(int k);                 /* motion smoothing: in-between picture k (of s23_built_mid_count(): 1 at 1/2, or 2 at 1/3 and 2/3)
                                                        * of the step to s23_built_latest(); NULL = none */
int  s23_built_mid_count(void);
void s23_smooth_rate(int pictures_per_s);            /* the window's picture rate: 150+ asks for two in-between frames */
int  s23_smooth_on(void);                              /* Display > Motion smoothing */
void s23_smooth_set(int on);

/* the GPU back end (src/s23_gl.c) */
int  s23_gl_init(void);                                /* after the window's GL context exists: 0 = not available (the software picture is used) */
int  s23_gl_ok(void);
void s23_gl_set_scale(int s);                          /* the internal resolution: 640 * s x 480 * s */
unsigned s23_gl_draw(const s23_built *b);              /* draws b; returns the GL texture holding the picture */
int  s23_gl_scale(void);
void s23_gl_set_wide(int e);                           /* widescreen: the picture is (640 + 2e) x 480 board pixels, the 3D wider (Hor+), the text in the 4:3 centre */
int  s23_gl_wide(void);
int  s23_gl_read_full(uint8_t *rgb, int cap);           /* the whole target, RGB, top row first: returns its width */
void s23_gl_read(uint32_t *rgba);                      /* the last picture at 640 x 480 (0x00RRGGBB, top row first) -- screenshots, the gate */
/* the software back end, on a built frame (the gate) */
void s23_soft_raster(const s23_built *b, uint32_t *rgba);
#endif
