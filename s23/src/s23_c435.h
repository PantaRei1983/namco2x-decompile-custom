/* s23_c435.h -- the C435 command processor's render list (src/s23_c435.c), what the 3D renderer draws */
#ifndef S23_C435_H
#define S23_C435_H
#include <stdint.h>

enum { S23_RE_MODEL = 1, S23_RE_IMMEDIATE = 2, S23_RE_DIRECT = 3 };
typedef struct { uint16_t type; int n; uint32_t h, pal, zbias, i[4], u[4], v[4], x[4], y[4], z[4]; } s23_immediate;
typedef struct {
    int type;
    uint16_t absolute_priority, model_blend_factor, light_power, light_ambient;
    int16_t tx, ty, vp_size_x, vp_size_y, vp_offset_x, vp_offset_y;
    float vp_fov;
    uint8_t clip43;                        /* widescreen: a flat screen-plane piece kept inside the 4:3 picture (set in the list snapshot) */
    uint8_t seg;                           /* which stretch of the list: the C435-interrupt models (model 0) divide it */
    /* a model */
    uint16_t model, model2; float scaling; int transpose, mirror_x;
    int16_t m[9]; int32_t v[3], light_vector[3];
    /* an immediate polygon */
    s23_immediate imm;
    /* a direct (screen-space) quad: 28 words */
    uint16_t d[28];
} s23_render_entry;
#define S23_RENDER_MAX 10000
/* a game's own solid rectangles over the picture (screen pixels, x1 / y1 exclusive), painted last before the gamma -- s23_game.list_hook */
typedef struct { int16_t x0, y0, x1, y1; uint32_t rgb; } s23_rect;
#define S23_RECTS 16
/* and one optional 128 x 128 PICTURE (0x00RRGGBB) stretched over x0..x1, y0..y1 after them (Gunmen Wars: the webcam inside the camera frame) */
/* and up to S23_MASKS coloured ALPHA MASKS (8-bit coverage, row-major, the game's own static data) at x, y -- captions (Gunmen Wars: the NamCam text) */
typedef struct { int16_t x, y, w, h; uint32_t rgb; const uint8_t *a; } s23_mask;
#define S23_MASKS 4
/* and up to 2 TEXT-LAYER SHIFTS (widescreen HUD, the GPU picture): text pixels in x0..x1, y0..y1 (the 640 x 480 layer) are drawn dx board pixels
 * over -- a game's HUD counters slide out with their 3D panels (Gunmen Wars' list hook) */
typedef struct { int count; int nrect; s23_rect rect[S23_RECTS]; int nmask; s23_mask mask[S23_MASKS]; int ntshift; int16_t tshift[2][5]; int ntstretch; int16_t tstretch[2][4]; uint8_t pillar_fade; int img_on; int16_t img_x0, img_y0, img_x1, img_y1; uint32_t img[128 * 128];
                 s23_render_entry e[S23_RENDER_MAX]; } s23_render_list;
extern s23_render_list s23_render[2];
extern int s23_render_cur;
extern int s23_c435_seg;
extern float s23_c435_fov_max; extern int16_t s23_c435_fov_vx, s23_c435_fov_vy; extern int s23_c435_fov_plain;              /* the largest projection scale of the clip sets sent while this list filled */                   /* the current stretch (s23_render_entry.seg); 0 at each new list */                 /* the list the C435 fills; the renderer draws the other one, then swaps */
float s23_f24(uint32_t v);
#endif
