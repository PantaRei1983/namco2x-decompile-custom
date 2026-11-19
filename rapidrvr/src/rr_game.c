/* rr_game.c -- RAPID RIVER's table for the System 23 host (../s23/include/s23_game.h): MAME's rapidrvr set (RD3 Ver. C) on the Namco
 * GORgON board ("System 22.5": ../s23/src/s23_gorgon.c), ASCA-1 I/O board. Values from MAME's namcos23.cpp (gorgon_state, rapidrvr_state,
 * ROM_START(rapidrvr)) and namcoio.cpp (namco_asca_1_device). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s23_game.h"

static const eng_rom_t chips[] = {
    { "rd3verc.ic2", 0x200000, NULL, NULL, 0xC15C0F30 }, { "rd3verc.ic1", 0x200000, NULL, NULL, 0x9D7F4411 },
    { "rd3verc.ic3", 0x80000, NULL, NULL, 0x6E26FBAF }, { "asc1_io-a.ic13", 0x40000, NULL, NULL, 0x77CDF69A },
    { "rd1mtah.3j", 0x400000, NULL, NULL, 0xD8FA0F3D }, { "rd1mtal.1j", 0x400000, NULL, NULL, 0x8F0EFA86 },
    { "rd1cgll.8b", 0x800000, NULL, NULL, 0xB58B92AC }, { "rd1cglm.7b", 0x800000, NULL, NULL, 0x447067FA },
    { "rd1cgum.6b", 0x800000, NULL, NULL, 0xC50DE2EF }, { "rd1cguu.5b", 0x800000, NULL, NULL, 0x611BAB41 },
    { "rd1sprll.12t", 0x400000, NULL, NULL, 0x8D450259 }, { "rd1sprlm.11p", 0x400000, NULL, NULL, 0x6C8DB3A5 },
    { "rd1sprum.10p", 0x400000, NULL, NULL, 0x8E08B2C6 }, { "rd1spruu.9p", 0x400000, NULL, NULL, 0xF20A9673 },
    { "rd1ccrl.11a", 0x200000, NULL, NULL, 0xB0EA2B32 }, { "rd1ccrh.11b", 0x200000, NULL, NULL, 0xFAFFFB86 },
    { "rd1pt0h.9l", 0x400000, NULL, NULL, 0x6F280EFF }, { "rd1pt0l.9j", 0x400000, NULL, NULL, 0x47B1C5A5 },
    { "rd1pt1h.10l", 0x400000, NULL, NULL, 0x37BD9BDF }, { "rd1pt1l.10j", 0x400000, NULL, NULL, 0x91131CB3 },
    { "rd1pt2h.11l", 0x400000, NULL, NULL, 0xFA601E83 }, { "rd1pt2l.11j", 0x400000, NULL, NULL, 0x3423FF9F },
    { "rd1pt3h.12l", 0x400000, NULL, NULL, 0xE82FF66A }, { "rd1pt3l.12j", 0x400000, NULL, NULL, 0x7216D63E },
    { "rd1wavel.2s", 0x800000, NULL, NULL, 0xBF52C08C }, { "rd1waveh.3s", 0x800000, NULL, NULL, 0xEF0136B5 },
};
static const char *const zips[] = { "rapidrvr.zip", "namco_asca1.zip" };

int s23_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../s23/src/s23_rom.c */

/* the boot image (4 MB at 0x1FC00000): rd3verc.ic2 and .ic1 byte-interleaved, .ic2 first: the NMON monitor ("for GORgON P2") and, at
 * 0x20000, the game's ELF (tools/make_prog.py prints the layout) */
static int rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && s23_rom_read(dir, "rd3verc.ic1", a, 0x200000) && s23_rom_read(dir, "rd3verc.ic2", b, 0x200000);
    if (ok) for (uint32_t k = 0; k < 0x200000; k++) { boot[2 * k] = b[k]; boot[2 * k + 1] = a[k]; }
    free(a); free(b);
    return ok;
}
/* the sub-CPU's (H8/3002) program: rd3verc.ic3, word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !s23_rom_read(dir, "rd3verc.ic3", dst, 0x80000)) return 0;
    for (uint32_t k = 0; k < 0x80000; k += 2) { const uint8_t t = dst[k]; dst[k] = dst[k + 1]; dst[k + 1] = t; }
    return 1;
}
/* the ASCA-1 I/O board's (H8/3334) program: the first n bytes of asc1_io-a.ic13 */
static int rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x40000);
    const int ok = all && n <= 0x40000 && s23_rom_read(dir, "asc1_io-a.ic13", all, 0x40000);
    if (ok) memcpy(dst, all, n);
    free(all);
    return ok;
}

/* THE CONTROLS on the ASCA-1's A/D (MAME rapidrvr_state::configure_jvs; namco_asca_1_device::adc_r = the 16-bit value's top 10 bits):
 *   0 YAW (the paddle column turned: steering), 1 PITCH (the paddle rotated: thrust -- full left to full right, over and over), both
 *   the port minus 0x8000 (centre = 0);
 *   2-5 THE SEAT's height sensors front-right, front-left, rear-right, rear-left: each rises 0x280 a 200 Hz tick (to 0x1100) while its
 *   air valve (I/O output bit 7-i = 0x6004 bit i) is open, else falls to 0x40; front-left and rear-right read negated. A cabinet whose
 *   seat does not move says so at power-on and plays on after START (MAME's notes); this keeps the seat "working". */
extern uint32_t rr_frame;
uint8_t s23_h8_outputs(void);
extern uint16_t (*s23_asca_analog)(int ch);
extern uint16_t s23_analog_in[8];                 /* [0] yaw, [1] pitch: MAME's 16-bit values (PORT_MINMAX 0x4C80-0xB340, centre 0x8000), set by
                                                   * the controls (rr_send) or $S23_ANALOG */
static int32_t seat[4] = { 0x40, 0x40, 0x40, 0x40 };
static uint32_t seat_frame;
static uint16_t rr_analog(int ch)
{
    if (rr_frame != seat_frame) {                 /* the 200 Hz ticks since the last frame (~3.34 a frame) */
        const uint32_t ticks = (rr_frame - seat_frame) * 10 / 3; seat_frame = rr_frame;
        void rr_paddle_frame(void); rr_paddle_frame();   /* the paddle from the held keys (below): once a frame, so a replay repeats it */
        const uint8_t out = s23_h8_outputs();
        for (int i = 0; i < 4; i++) {
            seat[i] += (out >> i & 1 ? 0x280 : -0x280) * (int32_t)(ticks > 20 ? 20 : ticks);
            if (seat[i] > 0x1100) seat[i] = 0x1100;
            if (seat[i] < 0x40) seat[i] = 0x40;
        }
    }
    uint16_t v = 0;
    switch (ch) {
    case 0: case 1: v = (uint16_t)(s23_analog_in[ch] - 0x8000); break;
    case 2: case 3: case 4: case 5: { const int i = ch - 2; v = (uint16_t)((i == 1 || i == 2) ? -seat[i] : seat[i]); break; }
    default: v = 0; break;
    }
    return (uint16_t)((v & 0xFFC0) >> 6);
}
__attribute__((constructor)) static void rr_controls_init(void) { s23_asca_analog = rr_analog; }

/* THE PADDLE ON KEYS AND A PAD (the service manual: channel 0 "HORIZONTAL" = the paddle pushed forwards / backwards, channel 1
 * "VERTICAL" = pushed up / down; "by rotating the paddle faster speed is accelerated. Keep the paddle inclined left or right to steer"):
 * a stroke is a CIRCLE on both pots. Measured headless 2026-11-17: clockwise (ch0 = cos, ch1 = +sin) paddles about twice as far as
 * cranking channel 1 alone; the other way round is slower (backwards); channel 0 alone does nothing. Up turns the circle once every
 * RR_STROKE frames; Left / Right incline the paddle (channel 1 held over) -- with Up the circle keeps turning, shifted to that side.
 * Nothing held: the paddle rests at centre. Pad: A / a trigger paddle, the left stick inclines (in proportion), LB / RB fully. */
#include <math.h>
#include <SDL2/SDL.h>
#include "ss22_input.h"
extern uint16_t s23_inputs;
#define RR_IN_FWD   0x0200
#define RR_IN_LEFT  0x0800
#define RR_IN_RIGHT 0x1000
#define RR_SPAN     0x3380                         /* MAME's PORT_MINMAX 0x4C80-0xB340 about 0x8000 */
#define RR_STROKE   12                             /* frames a full paddle circle: 5 a second. The Nile's "paddle upstream" statue
                                                    * (session 6 replayed): 24 stalls and times out; 16, 12, 8 all get through. */
static SDL_GameController *pad;
static int pad_stick;                              /* the pad's left stick X (steering), read with the keys */
static int pad_axis(SDL_GameControllerAxis ax)
{
    if (!pad || !SDL_GameControllerGetAttached(pad)) {
        pad = NULL;
        int s23_pad_choice(void); const int only = s23_pad_choice();
        for (int i = 0, gi = -1; only != -3 && i < SDL_NumJoysticks() && !pad; i++) if (SDL_IsGameController(i) && (++gi == only || only < 0)) pad = SDL_GameControllerOpen(i);
        if (!pad) return 0;
    }
    const int v = SDL_GameControllerGetAxis(pad, ax);
    return v > -6000 && v < 6000 ? 0 : v;
}
static const ss22_action rr_actions[] = {
    { "Insert coin",      "coin",     SDL_SCANCODE_5,      SDL_SCANCODE_6,       0x0001, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start",            "start",    SDL_SCANCODE_1,      SDL_SCANCODE_RETURN,  0x0010, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_START) },
    { "Paddle (thrust)",  "fwd",      SDL_SCANCODE_UP,     SDL_SCANCODE_W,       RR_IN_FWD,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Steer left",       "left",     SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,       RR_IN_LEFT,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Steer right",      "right",    SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,       RR_IN_RIGHT, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
    { "Service",          "service",  SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN, 0x0004, 0, 0 },
    { "Test",             "test",     SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN, 0x0008, 0, 0 },
    { "Operator up",      "op_up",    SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_UNKNOWN, 0x0040, 0, 0 },
    { "Operator down",    "op_down",  SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_UNKNOWN, 0x0080, 0, 0 },
    { "Operator enter",   "op_enter", SDL_SCANCODE_HOME,     SDL_SCANCODE_UNKNOWN, 0x0100, 0, 0 },
};
static void rr_send_inputs(uint16_t pressed, unsigned wheel, unsigned p1, unsigned p2)
{
    (void)wheel; (void)p1; (void)p2;
    static int scripted = -1;
    if (scripted < 0) scripted = getenv("S23_PRESS") != NULL || getenv("S23_REPLAY") != NULL;
    if (pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000 || pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) pressed |= RR_IN_FWD;
    if (!scripted) s23_inputs = pressed;
    pad_stick = pad_axis(SDL_CONTROLLER_AXIS_LEFTX);
}
/* once a frame (rr_analog): the paddle from s23_inputs -- the keys, or a replayed session's recording ($S23_REPLAY) */
void rr_paddle_frame(void)
{
    static int analog_scripted = -1; if (analog_scripted < 0) analog_scripted = getenv("S23_ANALOG") != NULL;
    if (analog_scripted) return;                   /* tests: $S23_ANALOG owns the paddle */
    const uint16_t pressed = s23_inputs;
    static int stroke, stroke2; static unsigned at2;   /* (tuning: $RR_STROKE = "frames" or "frames@frame:frames" -- a second rate from a frame on) */
    if (!stroke) { const char *e = getenv("RR_STROKE"); stroke = e && atoi(e) >= 4 ? atoi(e) : RR_STROKE;
                   if (e && sscanf(e, "%*d@%u:%d", &at2, &stroke2) == 2 && stroke2 < 4) stroke2 = 0; }
    if (stroke2 && rr_frame >= at2) { stroke = stroke2; stroke2 = 0; }
    static double t;                               /* where the paddle is round its circle, 0..1 */
    const int fwd = (pressed & RR_IN_FWD) != 0;
    if (fwd) { t += 1.0 / stroke; if (t >= 1) t -= 1; }
    const int left = (pressed & RR_IN_LEFT) != 0, right = (pressed & RR_IN_RIGHT) != 0;
    const double incline = left != right ? (left ? -1 : 1) : pad_stick / 32767.0;   /* low = left (measured) */
    double h = 0, v = incline;                     /* not paddling: centre fore/aft, inclined as held */
    if (fwd) {                                     /* paddling: the clockwise circle, squeezed to the inclined side */
        const double a = 2 * M_PI * t, k = 1 - 0.5 * fabs(incline);
        h = cos(a); v = sin(a) * k + incline * (1 - k);
    }
    s23_analog_in[0] = (uint16_t)(0x8000 + (int)(h * RR_SPAN));
    s23_analog_in[1] = (uint16_t)(0x8000 + (int)(v * RR_SPAN));
}
/* WIDESCREEN: a full-screen sprite picture (title, route map: src/s23_render.c s23_spr_fullpic) shows 4:3 with black bars (the user's
 * choice); in play the bottom text line (LAP TIME left, FREE PLAY right) moves out to the edges with the timer sprites */
#include "../../s23/src/s23_c435.h"
#include "../../s23/src/s23_frame.h"
/* the timer (top left, palette F2: session 11 frames 1280 / 2296) is the only HUD sprite; the route map's labels sit near the sides too */
/* the 2D effects (the S23_SPRLOG of a race): the spray off the raft and the rocks -- 4x4 tiles 0x0120-0x01DF, banks 6C/6D, translucent --
 * and the foam puffs -- 2x2 tiles 0x18AB-0x18CB. Not the HUD arrows (0x1700), the title logo or the text */
static const uint16_t rr_spr_fx[][2] = { { 0x0120, 0x01DF }, { 0x18AB, 0x18CB } };
static int rr_spr_wide(const s23_sprite *s) { return (s->color == 0xF2 && s->y < 64 && s->x < 160) ? -1 : 0; }
static int rr_hud_wide_ok(const void *l) { (void)l; return s23_spr_hud && !s23_spr_fullpic; }   /* full width only in play (timer up): the route map's flat labels sit on 3D lines that widen */
static int rr_wide_pillar(void) { return 1; }
static void rr_list_hook(void *lv)
{
    s23_render_list *l = (s23_render_list *)lv;
    int s23_gl_wide(void); extern int g_eng_hud_edges_on; const int ex = s23_gl_wide();
    if (ex <= 0 || !g_eng_hud_edges_on || s23_spr_fullpic || !s23_spr_hud) return;   /* only with the timer up: in play */
    static const int16_t sh[2][5] = { { 0, 446, 416, 474, 0 }, { 440, 446, 640, 474, 0 } };   /* LAP TIME (x 33 to ~400 with a long time: session 13 left its last digit), FREE PLAY (x 463-540). ponytail: a centred message there would split */
    memcpy(l->tshift, sh, sizeof sh); l->tshift[0][4] = (int16_t)-ex; l->tshift[1][4] = (int16_t)ex; l->ntshift = 2;
}
/* the polygons' z-bias word x4 (MAME: x1). The tunnel mouths' dark "teeth" (model 11150, bias 0xAF0 against the wall's 0x7D0-0x9C4) sort just in
 * front of the wall at x1 and show as black triangles the arcade does not have (the user, real-hardware footage); x4 puts them behind it.
 * Eight race frames of session 11 otherwise differ by a few hundred pixels at most. ponytail: every model; per model if a scene sorts wrong */
static int rr_zbias_scale(int model) { (void)model; return 4; }
/* the raft + crew (10837..10932 in session 16's attract) own their pixels; the wake's foam disc (9829 / 9830) runs under the raft and showed
 * as water inside it (session 16). ponytail: model ranges read off one frame -- widen if foam shows in the raft elsewhere */
static int rr_depth_role(int m) { return m >= 10800 && m <= 10999 ? 1 : m == 9829 || m == 9830 ? 2 : 0; }
static const ss22_input_game rr_input = {
    .actions = rr_actions, .n = (int)(sizeof rr_actions / sizeof *rr_actions),
    .wheel_min = 0x001, .wheel_max = 0x3FF, .wheel_key_span = 0x1FF, .wheel_step = 12,
    .notes = { "Enter, then press the new key (Esc cancels)",
               "Hold Up to paddle (thrust); Left / Right steer, with or without Up. 5 = coin, 1 = start",
               "Pad: A or a trigger paddles; the left stick steers, LB / RB steer full" },
    .send = rr_send_inputs, .test_bit = 0x0008, .service_bit = 0x0004,
};

const s23_game_t s23_game = {
    .title = "Rapid River", .tag = "rapidrvr", .window_title = "Rapid River", .mame_set = "rapidrvr",
    .zip_help = "MAME's rapidrvr.zip (Rapid River, RD3 Ver. C) and namco_asca1.zip",
    .zips = zips, .nzips = 2, .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .gorgon = &s23_gorgon, .io_asca = 1, .input = &rr_input,
    .depth_within = 2,                            /* exact depth always: the Nile gate's yellow wedges (within one model) and its inner door over the pyramid
                                                   * face, the foam inside the raft (between models) -- the user's arcade footage has none */
    .zbias_scale = rr_zbias_scale, .depth_role = rr_depth_role, .text_no_range = 1, .spr_wide = rr_spr_wide, .hud_wide_ok = rr_hud_wide_ok, .wide_pillar = rr_wide_pillar, .list_hook = rr_list_hook,
    .znear = 0.01f,                               /* session 8, leaving the Glacier Land tunnel: at MAME's 0.0001 the ceiling right by the camera read its
                                                   * texture from far off (an orange flash, 1-2 frames); 0.01 and 0.1 fix it, 1 cuts the tunnel away */
    .cz_fog = 1,                                 /* the depth fog from the GORgON's CZ RAM (src/s23_gorgon.c fog): the arcade's Nile footage is hazy */
    .clock = 135475200u,                          /* BUSCLOCK * 4 (GORgON; System 23 is * 5) */
    .px_num = 250, .px_den = 1323,                /* 25.6 MHz / 135.4752 MHz */
    .sub_div = 8,                                 /* 16.9344 MHz */
    .io_num = 16, .io_den = 147,                  /* 14.7456 MHz */
    .c352_div = 1536,                             /* 88200 Hz */
    /* MAME namcos23_state::machine_start (GORgON keeps it): IRQ0 vblank, IRQ1 C361 + sub-CPU, IRQ2 C435, IRQ3 C422, IRQ5 RS-232 */
    .ip = { [S23_SRC_VBLANK] = S23_IP(0), [S23_SRC_C361] = S23_IP(1), [S23_SRC_SUB] = S23_IP(1), [S23_SRC_C435] = S23_IP(2),
            [S23_SRC_C422] = S23_IP(3), [S23_SRC_TIMER] = S23_IP(5) },
    .boot_size = 0x400000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x008E, .depth_default = 0,
    .spr_fx = rr_spr_fx, .n_spr_fx = 2,
    .no_direct_buf = 1,                           /* (GORgON has no 0x0CC00000 direct buffer) */
    .data_rom = { { "rd1mtah.3j", "rd1mtal.1j" }, { NULL, NULL } },
    .npt = 4, .pt_rom = { { "rd1pt0h.9l", "rd1pt0l.9j" }, { "rd1pt1h.10l", "rd1pt1l.10j" }, { "rd1pt2h.11l", "rd1pt2l.11j" }, { "rd1pt3h.12l", "rd1pt3l.12j" } },
    .ccrl = "rd1ccrl.11a", .ccrh = "rd1ccrh.11b", .cg = { "rd1cgll.8b", "rd1cglm.7b", "rd1cgum.6b", "rd1cguu.5b" },
    .spr = { "rd1sprll.12t", "rd1sprlm.11p", "rd1sprum.10p", "rd1spruu.9p" },
    .wave = { "rd1wavel.2s", "rd1waveh.3s" },
};
