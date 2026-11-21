/*
 * The STORY INTRODUCTION of the attract mode -- ROM 0x0300AC..0x03164C, ported from the machine code.
 *
 * The attract program (the sequence table at 0x3510C) plays case 14 once per cycle: ROM 0x03157A sets the scene up and
 * moves to case 15, ROM 0x03164C, which runs one of nine phases a frame (the table at 0x39814) -- about 35 seconds of
 * Solitar, with the narrator's lines (sound 0x8F..0x98) and the captions (stage_event_text_display, the caption script
 * at 0x39092). The phase counter 0xE172E8 advances when the frame counter 0xE172EA reaches the phase's end in the
 * table at 0x39082 (270, 480, 720, 1020, 1260, 1500, 1800, 2070); phase 8 hands back to the attract program.
 *
 * Nothing of this was ported: cases 14/15 called attract_cinematic_tick (ROM 0xD97A, another scene), which ended at once,
 * so the attract skipped the story. A first-pass transpile of the phases sat unused in game_misc.c (countdown_sub0..6,
 * countdown_stage_town_overview, countdown_stage_flythrough...) with ROM tables read one byte at a time and host pointers
 * where the ROM passes addresses; this file replaces it, instruction by instruction from the disassembly. The index
 * scales were checked against Ghidra's listing: capstone prints `(d8,An,Xn*4)` without its `*4`.
 *
 * Locals the ROM passes by address (scene origins read through 0xE046F8 / 0xE046FC / 0xE04700 by the scenery
 * renderers) live in EFRAME() blocks below the machine's stack, like the ending's (include/ending_rd.h).
 */
#include "propcycl.h"
#include "ending_rd.h"
#include "ea68k.h"

extern intptr_t _W[];
#define W _W

#define g_fog_mode W[0xEB18]
#define g_fog_r    W[0xEB1A]
#define g_fog_g    W[0xEB1C]
#define g_fog_b    W[0xEB1E]

/* the shared code this calls */
int  math_lerp_int(int a, int b, int c, int d, int t);           /* 0x02ABFE: a + (b - a) * (t - c) / (d - c) */
uint32_t math_atan2(int32_t y, int32_t x);                       /* 0x00A870 */
void sync_post(void);                                            /* 0x02106A */
void set_background_color(uint8_t r, uint8_t g, uint8_t b);      /* 0x021AC2 */
void sound_reset_upper(void);                                    /* 0x00F8EC */
short *sound_play_or_defer();                                    /* 0x00F4E0 */
short *sound_play_p2();                                          /* 0x00F77C */
void dsp_cmd_emit_object_mode_8000(void);                        /* 0x026072 */
void dsp_cmd_emit_object_mode_8002(void);                        /* 0x0260B4 */
void dsp_viewport_setup(int vp, int mode);                       /* 0x025FE2 */
void ending_sky_draw_alt(void);                                  /* 0x02B172 */
void fog_set_from_table(int idx);                                /* 0x02AF50 */
void terrain_chunk_render(uint32_t course);                      /* 0x00ACD6 */
void terrain_chunk_visibility(uint32_t x, int y, uint32_t z, int ry);   /* 0x00AB46 */
void render_tower_complex(void);                                 /* 0x01679E */
void render_bridge_structure(void);                              /* 0x016900 */
int *render_flag_banner(int idx, uint32_t pos, int scale, uint32_t angle, int add, int *out);   /* 0x01629A */
intptr_t anim_load_list(intptr_t a1, uint32_t d1);               /* 0x0268CE */
void scene_interpolation_evaluate(int *chain, int t);            /* 0x0267AC */
void anim_update_and_render(intptr_t nb, int t);                 /* 0x026808 */
void player_model_render_only(void);                             /* 0x02689A */
void stage_objects_load_list(int idx);                           /* 0x032F30 */
void stage_static_objects_draw(void);                            /* 0x03303C */
void ending_player_reset(void);                                  /* 0x02B1B2 */
int  ending_frame_render(void);                                  /* 0x02B5C8 */
void ending_object_strip_draw_anim(ea_t list, int32_t frame, int32_t bias);   /* 0x02B502 */
void stage_event_text_display(int frame, int32_t args);          /* 0x033264 */
void attract_logo_init(void);                                    /* 0x032466 */
void attract_logo_run(void);                                     /* 0x0324A2 */
void attract_advance_sub_state(void);                            /* 0x00C25C */

/* 32-bit products and quotients the way muls.l / divs.l leave them (a zero divisor traps to a bare rte on the machine) */
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t div32(int32_t a, int32_t b) { return b ? (int32_t)((int64_t)a / b) : a; }
static inline int32_t trig(uint32_t addr, int32_t idx) { return vrd16s(addr + ((uint32_t)idx & 0xfffc)); }
static inline int32_t *cur(void) { return (int32_t *)W[0x0CA4]; }
static inline void setcur(int32_t *p) { W[0x0CA4] = (intptr_t)p; }
#define PUT(v) (p = dl_put(p, (int32_t)(v)))
int32_t *dl_put(int32_t *p, int32_t v);

#define CAM_X ((int32_t)W[0x0CDC])
#define CAM_Y ((int32_t)W[0x0CE0])
#define CAM_Z ((int32_t)W[0x0CE4])
#define CAM_RX ((int32_t)W[0x0CE8])
#define CAM_RY ((int32_t)W[0x0CEC])
#define NARRATE(id) sound_play_p2((int32_t)(0x00230000 | (id)), 0x14)   /* move.w #$14 ; move.w #id ; move.w #$23 */

/* the frame blocks the scene origins live in (include/ending_rd.h: 0, 1, 3, 4 are taken) */
#define FR_FLY  EFRAME(8)     /* ROM 0x030614's -$34(a6)..-$8(a6): three origins */
#define PA (FR_FLY + 0x00)
#define PB (FR_FLY + 0x10)
#define PC (FR_FLY + 0x20)

/* ---- ROM 0x0300AC: the camera block 0xE00CDC from six longs, and the far clip from a seventh ---------------------- */
static void story_camera_set(ea_t src, int with_far)
{
  int i;
  for (i = 0; i < 6; i++) W[0x0CDC + 4 * i] = ea_rd32(src + 4 * i);
  if (with_far) W16_SET(0x17360, (int16_t)ea_rd32(src + 24));
}

/* ---- ROM 0x0300DA: four balloons circling a point -------------------------------------------------------------------
 * pos: the centre (an origin record); tbl: four 7-long records (spin speed, radius, bob speed, bob height, height,
 * wobble speed, wobble size) at 0x39118 or 0x39188. Each balloon faces the camera; one of the four a frame also draws
 * its sparkle (0x391F8). */
static void story_balloon_ring(ea_t pos, ea_t tbl)
{
  int32_t *p;
  int32_t rx, ry, rz, s8, c4, d7, d6, t, x, y, z, k;
  int d2;
  dsp_cmd_emit_object_mode_8002();
  p = cur();
  rx = ea_rd32(pos)     - CAM_X;
  ry = ea_rd32(pos + 4) - CAM_Y;
  rz = ea_rd32(pos + 8) - CAM_Z;
  s8 = trig(0x20B004, -CAM_RX); c4 = trig(0x20B006, -CAM_RX);
  d7 = trig(0x20B004, -CAM_RY); d6 = trig(0x20B006, -CAM_RY);
  k = W16(0x172EA);
  for (d2 = 0; d2 < 4; d2++) {
    ea_t r = tbl + d2 * 0x1C;
    int32_t a = (d2 << 14) + mul32(k, ea_rd32(r));
    t = mul32(trig(0x20B004, mul32(k, ea_rd32(r + 0x14))), ea_rd32(r + 0x18)) >> 15;
    x = (mul32(trig(0x20B006, a), t + ea_rd32(r + 4)) >> 15) + rx;
    y = ry + ea_rd32(r + 0x10) + (mul32(trig(0x20B004, mul32(k, ea_rd32(r + 8))), ea_rd32(r + 0xC)) >> 15);
    z = rz - (mul32(trig(0x20B004, a), t + ea_rd32(r + 4)) >> 15);
    PUT((k & 0x2c) ? ((k >> 3) & 3) + 0x326 : 0x32a);
    PUT(x); PUT(y); PUT(z); PUT(s8); PUT(c4); PUT(d7); PUT(d6); PUT(0); PUT(0x7fff); PUT(0);
    if (((k >> 4) & 3) == (d2 & 3)) {
      PUT(vrd32s(0x391F8 + ((k >> 1) & 7) * 4));
      PUT(x); PUT(y); PUT(z); PUT(s8); PUT(c4); PUT(d7); PUT(d6); PUT(0); PUT(0x7fff); PUT(0);
    }
  }
  setcur(p);
}

/* ---- ROM 0x0302E2: the town's ferris wheel, eight gondolas -----------------------------------------------------------
 * y0 / y1: the gondolas' height at rest and after the swing; t: the frame. Returns the gondola that reaches the top
 * at t == 6 (the caller ignores it). The trig longs are written as the machine writes them; the master DSP keeps their
 * low halves (the convention of render_player_bike_model and render_tower_complex). */
static int story_ferris_wheel(int32_t y0, int32_t y1, int32_t t)
{
  int32_t *p, d3, d7, a0, d0;
  int d2, d4 = 0;
  dsp_cmd_emit_object_mode_8002();
  p = cur();
  PUT(0x8010); PUT(3); W[0x4704] = (int32_t)0xffff0000; PUT(0xffff0000); PUT(-1);
  for (d2 = 0; d2 < 8; d2++) {
    d3 = mul32((d2 >> 1) + (d2 & 1) * 4, -10) + t;
    if (d3 == 6) d4 = (d2 == 7) ? 1 : d2 + 1;
    d7 = (d3 < 6) ? y0 : div32(mul32(y1 - y0, d3 - 6), 0xae) + y0;
    PUT(0x8008); PUT(3); PUT(0); PUT(0); PUT(0); PUT(0xffffa600); PUT(1);
    if (d3 < 0x24) a0 = 0x4000;
    else {
      int16_t w = vrd16s(0x39218 + d2 * 2);
      d0 = math_lerp_int(0, 0x8000, 0x24, 0x168, d3);
      d0 = (int32_t)(int16_t)trig(0x20B006, d0) * w;           /* muls.w */
      d0 = div32(d0, -0x8000);
      a0 = w + d0 + 0x4000;
    }
    PUT(trig(0x20B004, a0)); PUT(trig(0x20B006, a0));        /* move.l -2(a4,d0) / 2(a1): the low halves */
    d0 = ((d2 << 16) >> 3) + 0x8000;
    PUT(trig(0x20B004, d0)); PUT(trig(0x20B006, d0));
    PUT(0); PUT(0x7fff); PUT(4); PUT(-1);
    PUT(0x800a); PUT(vrd16s(0x36414 + d2 * 2)); PUT(3);
    PUT(((trig(0x20B004, d0) << 12) >> 11) - CAM_X + 0x3c000);
    PUT(d7 - CAM_Y + 0x18600);
    PUT(((trig(0x20B006, d0) << 12) >> 11) - CAM_Z + 0x114000);
  }
  PUT(0x8010); PUT(-1);
  W[0x4704] = 0;
  setcur(p);
  return d4;
}

/* ---- ROM 0x0304BC: the clouds drifting past the cliff (records of 5 longs at 0x39228, -1 ends) ------------------- */
static void story_cloud_ring(int32_t t)
{
  int32_t *p, s8, d7, d6, d5;
  ea_t a3;
  dsp_cmd_emit_object_mode_8002();
  p = cur();
  s8 = trig(0x20B004, (int32_t)0xffffc000 - CAM_RX);    /* lea $c000.w: sign-extended */
  d7 = trig(0x20B006, (int32_t)0xffffc000 - CAM_RX);
  d6 = trig(0x20B004, -CAM_RY);
  d5 = trig(0x20B006, -CAM_RY);
  for (a3 = 0x39228; vrd32s((uint32_t)a3) >= 0; a3 += 0x14) {
    int32_t lo = vrd32s((uint32_t)a3), hi = vrd32s((uint32_t)a3 + 4), ang, d0;
    if (t < lo || t > hi) continue;
    PUT(0x8010); PUT(3); W[0x4704] = 0x7000; PUT(0x7000); PUT(2);
    PUT(vrd16s(0x39344 + ((a3 >> 2) & 3) * 2)); PUT(-1);
    PUT(0x38a);
    ang = vrd32s((uint32_t)a3 + 0x10);
    PUT(((trig(0x20B002, ang) * 0x1200) >> 11) - CAM_X + 0x3c000);
    d0 = math_lerp_int(vrd32s((uint32_t)a3 + 8), vrd32s((uint32_t)a3 + 0xC), lo, hi, t);
    PUT(d0 - CAM_Y + 0x18600);
    PUT(((trig(0x20B004, ang) * 0x1200) >> 11) - CAM_Z + 0x114000);
    PUT(s8); PUT(d7); PUT(d6); PUT(d5);
    d0 = ((int32_t)(a3 & 0xf) << 10) + 0x3000;
    PUT(d0); PUT(d0); PUT(0);
  }
  setcur(p);
}

/* ---- ROM 0x030614: the flight over the cliff town (phases 3 and 4) ---------------------------------------------------
 * The camera follows the path at 0x3934C toward the castle, the three islands (0x37A7A / 0x37A86 / 0x37A94) with the
 * tower, the bridge and their balloons, then the ring of eight flags. t: the phase-3 frame. */
static void story_flythrough(int32_t t)
{
  int32_t *p, v, d4, d0;
  int i;
  e_wr32(PA, 0); e_wr32(PA + 8, 0); e_wr32(PA + 12, 0x615);
  e_wr32(PA + 4, div32(mul32(vrd32s(0x39384) - vrd32s(0x39380), t), 0x21c) + vrd32s(0x39380));
  for (i = 0; i < 4; i++) { e_wr32(PC + 4 * i, e_rd32(PA + 4 * i)); e_wr32(PB + 4 * i, e_rd32(PA + 4 * i)); }
  if (t < 0x12c) {
    int32_t w = trig(0x20B004, div32(t << 14, 0x12c));
    e_wr32(PB + 8, e_rd32(PB + 8) + div32(w << 12, -0x8000) + 0x1000);
    e_wr32(PC,     e_rd32(PC)     + div32((int32_t)((uint32_t)w << 16), -0x8000) + 0x10000);
  }
  if (W16(0x172EC) == 0)
    for (i = 0; i < 7; i++) W[0x17294 + 4 * i] = vrd32s(0x3934C + 4 * i);
  W[0x17294] = vrd32s(0x3934C);
  W[0x1729C] = vrd32s(0x39354);
  W[0x172A4] = vrd32s(0x3935C);
  W[0x17298] = vrd32s(0x39368);
  d4 = (int32_t)math_atan2(vrd32s(0x39370) - (int32_t)W[0x17294], vrd32s(0x39378) - (int32_t)W[0x1729C]);
  {
    int32_t c = trig(0x20B006, d4 & 0xffff) >> 5;
    int32_t x = div32((vrd32s(0x39378) - (int32_t)W[0x1729C]) << 8, c);
    int32_t y = (e_rd32(PA + 4) - (int32_t)W[0x17298] + vrd32s(0x39374)) >> 2;
    W[0x172A0] = -(int32_t)math_atan2(y, x);
  }
  story_camera_set(EA_W(0x17294), 1);
  ending_sky_draw_alt();
  W[0x46F8] = PA; scene_objects_draw_list(0x37A7A, PA);
  W[0x46FC] = PB; scene_objects_draw_list(0x37A86, PB);
  W[0x4700] = PC; scene_objects_draw_list(0x37A94, PC);
  W[0x291C] = 0;
  W16_SET(0x4708, 0xffff);
  render_tower_complex();
  render_bridge_structure();
  for (i = 0; i < 3; i++) {
    W[0x17294 + 4 * i] = e_rd32(PA + 4 * i) + vrd32s(0x393D0 + 4 * i);
    e_wr32(PB + 4 * i, e_rd32(PB + 4 * i) + vrd32s(0x393DC + 4 * i));
    e_wr32(PC + 4 * i, e_rd32(PC + 4 * i) + vrd32s(0x393E8 + 4 * i));
  }
  story_balloon_ring(EA_W(0x17294), 0x39188);
  story_balloon_ring(PB, 0x39188);
  story_balloon_ring(PC, 0x39188);
  e_wr32(PA,     e_rd32(PA)     - CAM_X);
  e_wr32(PA + 4, e_rd32(PA + 4) - CAM_Y);
  e_wr32(PA + 8, e_rd32(PA + 8) - CAM_Z);
  dsp_cmd_emit_object_mode_8000();
  p = cur();
  if (e_rd32(PA + 4) > (int32_t)0xfffe6e00 && (int32_t)W[0x4704] != (int32_t)0xffff9000) {
    PUT(0x8010); PUT(3); W[0x4704] = (int32_t)0xffff9000; PUT(0xffff9000); PUT(-1);
  }
  PUT(0x1f4); PUT(e_rd32(PA)); PUT(e_rd32(PA + 4)); PUT(e_rd32(PA + 8));
  if ((int32_t)W[0x4704] != (int32_t)0xffff0000) {
    PUT(0x8010); PUT(3); W[0x4704] = (int32_t)0xffff0000; PUT(0xffff0000); PUT(-1);
  }
  W[0x169E8] = 0x8002;
  PUT(0x8002); PUT(0);
  if (t <= 0xf0) {
    v  = math_lerp_int(vrd32s(0x39394), vrd32s(0x39388), 0, 0xf0, t);
    d4 = math_lerp_int(vrd32s(0x39398), vrd32s(0x3938C), 0, 0xf0, t);
    d0 = math_lerp_int(vrd32s(0x3939C), vrd32s(0x39390), 0, 0xf0, t);
  } else {
    v = vrd32s(0x39388); d4 = vrd32s(0x3938C); d0 = vrd32s(0x39390);
  }
  e_wr32(PA + 4, e_rd32(PA + 4) - d0);
  if (t > 0xf0) {
    for (i = 0; i < 8; i++) p = (int32_t *)render_flag_banner(i, PA, v, (uint32_t)d4, 0, (int *)p);
  } else {
    for (i = 0; i < 8; i++) {
      int32_t add, ang;
      e_wr32(PA + 4, e_rd32(PA + 4) + math_lerp_int(vrd16s(0x393C0 + 2 * i), 0, 0, 0xf0, t));
      add = math_lerp_int(vrd16s(0x393B0 + 2 * i), 0, 0, 0xf0, t);
      ang = math_lerp_int(vrd16s(0x393A0 + 2 * i), 0, 0, 0xf0, t) + d4;
      p = (int32_t *)render_flag_banner(i, PA, v, (uint32_t)ang, add, (int *)p);
    }
  }
  PUT(0x8010); PUT(-1);
  W[0x4704] = 0;
  setcur(p);
}

/* ---- ROM 0x030A1E: the boy flying over the hills with the balloons (phases 5 and 6) ------------------------------
 * The camera eases along the path at 0x39700 -> 0x3971C; the rider's chain is loaded at 0xE0B704 on the first frame
 * and evaluated at t after; each balloon at 0x39738 (records of 4 longs, -1 ends) bobs until the rider comes within
 * reach, then pops through the burst models 0x2F8.. (state in 0xE17294[], -1 = gone). */
static void story_town(int32_t t)
{
  int32_t cam[7], *p, d0, d1;
  int d2;
  uint32_t d4;
  if (t < 0x1fe) {
    for (d2 = 0; d2 < 6; d2++) {
      d1 = trig(0x20B004, math_lerp_int(0, 0x4000, 0, 0x21c, t));
      d0 = vrd32s(0x3971C + 4 * d2) - vrd32s(0x39700 + 4 * d2);
      if (d0 < 0) d0 += 0x1f;
      d0 = (mul32(d0 >> 5, d1) >> 10) + vrd32s(0x39700 + 4 * d2);
      cam[d2] = d0;
    }
  } else {
    for (d2 = 0; d2 < 6; d2++) {
      d1 = trig(0x20B004, math_lerp_int(0, 0x4000, 0, 0x21c, 0x1fe));
      d0 = vrd32s(0x3971C + 4 * d2) - vrd32s(0x39700 + 4 * d2);
      if (d0 < 0) d0 += 7;
      d0 = (mul32(d0 >> 3, d1) >> 12) + vrd32s(0x39700 + 4 * d2);
      cam[d2] = math_lerp_int(d0, vrd32s(0x3971C + 4 * d2), 0x1fe, 0x21c, t);
    }
  }
  cam[6] = math_lerp_int(vrd32s(0x39718), vrd32s(0x39734), 0, 0x21c, t);
  story_camera_set((ea_t)cam, 1);
  if (t == 0) {
    W[0x0E0C] = 0;
    terrain_chunk_render(0);
  }
  terrain_chunk_visibility((uint32_t)W[0x0CDC], (int)W[0x0CE0], (uint32_t)W[0x0CE4], (int)W[0x0CEC]);
  ending_sky_draw_alt();
  dsp_cmd_emit_object_mode_8000();
  p = cur();
  PUT(vrd32s(0x390C8 + ((t >> 1) & 0xf) * 4) + 0x146);
  PUT(-CAM_X); PUT(0xa000 - CAM_Y); PUT(-CAM_Z);
  setcur(p);
  W[0x12BC] = 1;
  if (t == 0) {
    anim_load_list(0xB704, 0x3949C);
    ending_player_reset();
  } else if (t < 0x1ff) {
    scene_interpolation_evaluate((int *)(intptr_t)0xE0B704, (int16_t)t);
  }
  ending_frame_render();
  W[0x16668] = trig(0x20B004, -CAM_RY);
  W[0x1666C] = trig(0x20B006, -CAM_RY);
  ending_object_strip_draw_anim((ea_t)0x394A4, t, (int32_t)0xffffe000);
  dsp_cmd_emit_object_mode_8002();
  p = cur();
  PUT(0x8010); PUT(-1);
  W[0x4704] = 0;
  for (d2 = 0, d4 = 0x39738; vrd32s(d4) >= 0; d2++, d4 += 0x10) {
    intptr_t s = 0x17294 + 4 * d2;
    if (t == 0) W[s] = 0;
    if ((int32_t)W[s] >= 0) {
      PUT((int32_t)W[s] == 0 ? ((t >> 3) & 3) + vrd32s(d4 + 0xC) : (((int32_t)W[s] - 1) >> 1) + 0x2f8);
      PUT(vrd32s(d4) - CAM_X);
      d1 = div32(trig(0x20B004, t << 8) << 11, 0x8000);
      PUT(vrd32s(d4 + 4) - CAM_Y + d1);
      PUT(vrd32s(d4 + 8) - CAM_Z);
      PUT(0); PUT(0x7fff); PUT(W[0x16668]); PUT(W[0x1666C]); PUT(0); PUT(0x7fff); PUT(0);
    }
    if ((int32_t)W[s] > 0) {
      W[s] = (int32_t)W[s] + 1;
      if ((int32_t)W[s] > 0x10) W[s] = -1;
    }
    if ((int32_t)W[s] == 0) {
      int32_t dx = (vrd32s(d4)     - (int32_t)W[0xB728]) >> 7;
      int32_t dy = (vrd32s(d4 + 4) - (int32_t)W[0xB72C]) >> 7;
      int32_t dz = (vrd32s(d4 + 8) - (int32_t)W[0xB730]) >> 7;
      if (mul32(dx, dx) + mul32(dy, dy) + mul32(dz, dz) < 0x736) W[s] = 1;
    }
  }
  setcur(p);
}

/* =====================================================================================================================
 * THE PHASES (the table at 0x39814). Each keeps its own frame in 0xE172EC = 0xE172EA - its start.
 * ===================================================================================================================== */

/* ROM 0x030DA2, frames 0..269: the town at dawn from the ferris wheel, fading in */
static void story_phase0(void)
{
  int16_t k = W16(0x172EA);
  int32_t *p;
  W16_SET(0x172EC, k);
  if (k == 0) terrain_chunk_render(0);
  if (k == 0x30) NARRATE(0x8F);
  if (k <= 0x40) {
    W[0xEB16] = (int16_t)(0xff - k * 4);
    if ((int16_t)W[0xEB16] < 0) W[0xEB16] = 0;
  }
  story_camera_set((ea_t)0x397BC, 0);
  W16_SET(0x17360, math_lerp_int(0xc00, 0x800, 0, 0x10e, k));
  ending_sky_draw_alt();
  terrain_chunk_visibility((uint32_t)W[0x0CDC], (int)W[0x0CE0], (uint32_t)W[0x0CE4], (int)W[0x0CEC]);
  W[0x46F8] = 0x39108;
  scene_objects_draw_list((ea_t)0x37A7A, (ea_t)0x39108);
  story_ferris_wheel(vrd32s(0x3910C), vrd32s(0x3910C), 0);
  dsp_cmd_emit_object_mode_8000();
  p = cur();
  W[0x17294] = vrd32s(0x37AB4) - CAM_X;
  W[0x17298] = vrd32s(0x37AB8) - CAM_Y;
  W[0x1729C] = vrd32s(0x37ABC) - CAM_Z;
  PUT(vrd32s(0x390C8 + ((k >> 1) & 0xf) * 4) + 0x146);
  PUT(W[0x17294]); PUT(W[0x17298]); PUT(W[0x1729C]);
  PUT(0x8010); PUT(3); W[0x4704] = 0x3000; PUT(0x3000); PUT(-1);
  PUT(vrd32s(0x390C8 + ((k >> 1) & 0xf) * 4) + 0x164);
  PUT(W[0x17294]); PUT(W[0x17298]); PUT(W[0x1729C]);
  setcur(p);
}

/* ROM 0x030F28, frames 270..479: the PROP CYCLE logo */
static void story_phase1(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x10e);
  W16_SET(0x172EC, k);
  if (k == 0) {
    NARRATE(0x90);
    W16_SET(0x17360, 0x780);
    g_fog_mode = 1;
    attract_logo_init();
  } else {
    attract_logo_run();
  }
}

/* ROM 0x030F6E, frames 480..719: the balloons rising past the ferris wheel, the clouds, the flashes of light */
static void story_phase2(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x1e0);
  int32_t *p;
  int i;
  W16_SET(0x172EC, k);
  if (k == 0) {
    NARRATE(0x91);
    for (i = 0; i < 4; i++) {
      W[0x17294 + 4 * i] = vrd32s(0x39108 + 4 * i);
      W[0x172A4 + 4 * i] = vrd32s(0x39108 + 4 * i) + vrd32s(0x397F4 + 4 * i);
    }
    W16_SET(0x172FC, 0);
  }
  story_camera_set((ea_t)0x397D8, 1);
  W[0x17298] = math_lerp_int(vrd32s(0x3910C) + 0x5000, vrd32s(0x3910C) + 0xb000, 0, 0xf0, k);
  W[0x172A8] = (int32_t)W[0x17298] + vrd32s(0x397F8);
  if ((int16_t)W[0xEB16] > 0) {
    if (W16(0x172FC) != 0 && (int16_t)W[0xEB16] != 0xff) {
      fog_set_from_table(0);
      W16_SET(0x172FA, 0x40);
    } else {
      W[0xEB16] = (int16_t)((int16_t)W[0xEB16] - W16(0x172FA));
      if ((int16_t)W[0xEB16] < 0) W[0xEB16] = 0;
    }
  }
  for (i = 0; vrd16s(0x39804 + 2 * i) >= 0; i++)
    if (vrd16s(0x39804 + 2 * i) == k) {
      fog_set_from_table(7);
      W16_SET(0x172FA, 1);
    }
  ending_sky_draw_alt();
  terrain_chunk_visibility((uint32_t)W[0x0CDC], (int)W[0x0CE0], (uint32_t)W[0x0CE4], (int)W[0x0CEC]);
  W[0x46F8] = EA_W(0x17294);
  scene_objects_draw_list((ea_t)0x37A7A, EA_W(0x17294));
  story_balloon_ring(EA_W(0x172A4), 0x39118);
  story_ferris_wheel(vrd32s(0x3910C), vrd32s(0x3910C) + 0x8000, k + 0x3c);
  story_cloud_ring(k);
  dsp_cmd_emit_object_mode_8000();
  p = cur();
  if ((int32_t)W[0x4704] != (int32_t)0xffffa000) {
    PUT(0x8010); PUT(3); W[0x4704] = (int32_t)0xffffa000; PUT(0xffffa000); PUT(-1);
  }
  PUT(0x1f4);
  PUT((int32_t)W[0x17294] - CAM_X); PUT((int32_t)W[0x17298] - CAM_Y); PUT((int32_t)W[0x1729C] - CAM_Z);
  W[0x172B4] = vrd32s(0x37AB4) - CAM_X;
  W[0x172B8] = vrd32s(0x37AB8) - CAM_Y;
  W[0x172BC] = vrd32s(0x37ABC) - CAM_Z;
  PUT(vrd32s(0x390C8 + ((k >> 1) & 0xf) * 4) + 0x146);
  PUT(W[0x172B4]); PUT(W[0x172B8]); PUT(W[0x172BC]);
  setcur(p);
  if (k >= 2) {                                   /* 0311B8: the sky flickers -- the four CZ attribute words */
    int32_t d1, d2;
    d1 = div32(trig(0x20B004, (int32_t)k << 11) << 8, 0x8000) + 0x200;
    d2 = trig(0x20B004, mul32(k, d1));
    d1 = div32(mul32(trig(0x20B006, (int32_t)k << 12), 3) << 4, 0x8000) + 0x10;
    d2 = div32(mul32(d2, d1), 0x8000) + 0x10;
    if (d2 < 0) d2 = 0;
    mem_write16(0x810000, (uint16_t)d2);
    mem_write16(0x810002, (uint16_t)d2);
    mem_write16(0x810004, (uint16_t)d2);
    mem_write16(0x810006, (uint16_t)d2);
  }
}

/* ROM 0x031252, frames 720..1019: the flight over the cliff town begins */
static void story_phase3(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x2d0);
  W16_SET(0x172EC, k);
  if (k == 0) NARRATE(0x92);
  if (k == 2) {
    g_sys.videomix[0x07] = 0; g_sys.videomix[0x06] = 0; g_sys.videomix[0x05] = 0;   /* 0x824007..5 */
    mem_write16(0x810000, 0); mem_write16(0x810002, 0); mem_write16(0x810004, 0); mem_write16(0x810006, 0);
  }
  story_flythrough(k);
}

/* ROM 0x0312BC, frames 1020..1259: ...and fades to white */
static void story_phase4(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x3fc);
  W16_SET(0x172EC, k);
  if (k == 0) {
    NARRATE(0x93);
    g_fog_b = 0xff; g_fog_g = 0xff; g_fog_r = 0xff;
  }
  story_flythrough((int16_t)(W16(0x172EA) - 0x2d0));
  if (k >= 0xb4) W[0xEB16] = (int16_t)div32(k * 0xff - 0xb34c, 0x3c);
}

/* ROM 0x031330, frames 1260..1499: the boy and the balloons, out of the white */
static void story_phase5(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x4ec);
  W16_SET(0x172EC, k);
  if (k == 0) NARRATE(0x94);
  if (k == 3) { g_sys.videomix[0x07] = 0x80; g_sys.videomix[0x06] = 0x80; g_sys.videomix[0x05] = 0x80; }
  if (k <= 0x3c) W[0xEB16] = (int16_t)(div32(k * 0xff, -0x3c) + 0xff);
  else           W[0xEB16] = 0;
  story_town(k);
}

/* ROM 0x0313A8, frames 1500..1799: ...fading to black */
static void story_phase6(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x5dc);
  W16_SET(0x172EC, k);
  if (k == 0) {
    NARRATE(0x96);
    g_fog_b = 0; g_fog_g = 0; g_fog_r = 0;
  }
  story_town((int16_t)(W16(0x172EA) - 0x4ec));
  if (k >= 0x11d) W[0xEB16] = (int16_t)div32(k * 0xff - 0x11be3, 0xf);
}

/* ROM 0x03141A, frames 1800..2069: the rider on the Prop Cycle (animation set 10), then the fade */
static void story_phase7(void)
{
  int16_t k = (int16_t)(W16(0x172EA) - 0x708);
  W16_SET(0x172EC, k);
  if (k == 0) {
    NARRATE(0x97);
    stage_objects_load_list(0x0A);
    player_model_render_only();
  } else {
    int32_t *p = cur();
    W[0x169E8] = 0x8002;
    PUT(0x8002);
    PUT((int16_t)W16(0x169F0));
    setcur(p);
    dsp_viewport_setup((int16_t)W16(0x169F0), (int16_t)W16(0x169F2));
    /* 0x031498: `addq.w #1,$e0eb04 ; move.w (a0),-(a7)` -- the chain's frame counter, kept as the whole slot like the
     * intermission's (game_gameplay.c sub 27) */
    W[0xEB04] = (int16_t)(W[0xEB04] + 1);
    anim_update_and_render(0xAB04, (int16_t)W[0xEB04]);
  }
  W16_SET(0x17360, (int16_t)W[0xABA8]);
  stage_static_objects_draw();
  if (k <= 0xf) W[0xEB16] = (int16_t)(div32(k * 0xff, -0xf) + 0xff);
  else if (k < 0x96) W[0xEB16] = 0;
  else {
    g_fog_mode = 3;
    W[0xEB16] = (int16_t)div32(k * 0xff - 0x956a, 0x78);
    if ((int16_t)W[0xEB16] > 0xff) W[0xEB16] = 0xff;
  }
  if (k == 0x96) NARRATE(0x98);
}

/* ROM 0x031532, frame 2070: back to the attract program (or, outside it, hold) */
static void story_phase8(void)
{
  if ((int32_t)W[0x0CBC] == 1) {
    g_fog_b = 0; g_fog_g = 0; g_fog_r = 0;
    W[0xEB16] = 0xff;
    W[0x0CC4] = 2;
    attract_advance_sub_state();
  } else {
    W16_SET(0x172EA, -1);
    W16_SET(0x172E8, -1);
  }
}

static void (*const story_phases[9])(void) = {
  story_phase0, story_phase1, story_phase2, story_phase3, story_phase4,
  story_phase5, story_phase6, story_phase7, story_phase8,
};

/* ---- ROM 0x03164C: attract case 15, every frame ------------------------------------------------------------------ */
void attract_story_run(void)
{
  int16_t ph;
  W[0x4704] = 0;
  W[0x169E8] = 0;
  ph = W16(0x172E8);
  if (ph >= 0 && ph < 9) story_phases[ph]();
  dsp_w32(0x10038 + (uint32_t)W[0x0CA0] * 0x8000u, (int32_t)(int16_t)W16(0x17360));   /* the far clip of this buffer */
  if (W[0x4704] != 0) {
    int32_t *p = cur();
    PUT(0x8010); PUT(-1);
    setcur(p);
    W[0x4704] = 0;
  }
  stage_event_text_display((int16_t)W16(0x172EA), (0x19 << 16) | 7);
  W[0x0C8C] = (int32_t)W[0x0C8C] + 1;
  W16_SET(0x172EA, W16(0x172EA) + 1);
  ph = W16(0x172E8);
  if (W16(0x172EA) == vrd16s((uint32_t)(0x39082 + ph * 2))) {
    W16_SET(0x172E8, ph + 1);
    W[0x0CC4] = 15;
  }
}

/* ---- ROM 0x03157A: attract case 14 -- set the story up, then run its first frame ---------------------------------- */
void attract_story_init(void)
{
  sync_post();
  set_background_color(0, 0, 0);
  sound_reset_upper();
  sound_play_or_defer(0x0E);
  set_background_color(0, 0, 0);
  W_SET_HI16(0xEB20, 1);                                       /* move.w #1,$e0eb20 */
  g_sys.videomix[0x07] = 0xff; g_sys.videomix[0x06] = 0xff; g_sys.videomix[0x05] = 0xff;   /* 0x824007..5 */
  g_fog_b = 0; g_fog_g = 0; g_fog_r = 0;
  W[0xEB16] = 0xff;
  g_fog_mode = 3;
  W16_SET(0x4708, 0);
  W16_SET(0x17360, 0x780);
  W16_SET(0x169F0, 0);
  W16_SET(0x169F2, 0);
  W[0x17394] = 0x39092;                                        /* the caption script */
  W16_SET(0x17398, 0);
  W[0x12DC] = 0;
  W[0x12D4] = 0;
  W[0x0C8C] = 0;
  W16_SET(0x172EA, 0);
  W16_SET(0x172E8, 0);
  W[0x0CC0] = 3;
  W[0x0CC4] = 15;
  attract_story_run();
}
