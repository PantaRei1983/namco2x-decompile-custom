/* ff_game.c -- FINAL FURLONG's table for the System 23 host (../s23/include/s23_game.h): MAME's finfurl set (FF2 Ver. A) on the Namco GORgON
 * board ("System 22.5": ../s23/src/s23_gorgon.c), ASCA-1 I/O board -- the same hardware as Rapid River (../rapidrvr/src/rr_game.c), one
 * model ROM pair instead of four. Values from MAME's namcos23.cpp (gorgon_state::finfurl, INPUT_PORTS_START(finfurl), ROM_START(finfurl)). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s23_game.h"

static const eng_rom_t chips[] = {
    { "ff2vera.ic2", 0x200000, NULL, NULL, 0xE10F9DFA }, { "ff2vera.ic1", 0x200000, NULL, NULL, 0x5A90FFBF },
    { "ff2vera.ic3", 0x80000, NULL, NULL, 0xAB681078 }, { "asc1_io-a.ic13", 0x40000, NULL, NULL, 0x77CDF69A },
    { "ff2mtah.3j", 0x400000, NULL, NULL, 0x161003CD }, { "ff2mtal.1j", 0x400000, NULL, NULL, 0xED1A5BF2 },
    { "ff2cgll.8b", 0x400000, NULL, NULL, 0x8E6C34EB }, { "ff2cglm.7b", 0x400000, NULL, NULL, 0x406F321B },
    { "ff2cgum.6b", 0x400000, NULL, NULL, 0xB808BE59 }, { "ff2cguu.5b", 0x400000, NULL, NULL, 0x595DEEE4 },
    { "ff2sprll.12t", 0x400000, NULL, NULL, 0x1B305A13 }, { "ff2sprlm.11p", 0x400000, NULL, NULL, 0x421A8FBF },
    { "ff2sprum.10p", 0x400000, NULL, NULL, 0xCB53C03E }, { "ff2spruu.9p", 0x400000, NULL, NULL, 0xC134B0DE },
    { "ff2ccrl.11a", 0x200000, NULL, NULL, 0xF1F9E77C }, { "ff2ccrh.11b", 0x200000, NULL, NULL, 0x71228C61 },
    { "ff2pt0h.9l", 0x400000, NULL, NULL, 0x344CE7A5 }, { "ff2pt0l.9j", 0x400000, NULL, NULL, 0x7EEDA441 },
    { "ff2wavel.2s", 0x800000, NULL, NULL, 0x6235C605 }, { "ff2waveh.3s", 0x800000, NULL, NULL, 0x2A59492A },
};
static const char *const zips[] = { "finfurl.zip", "namco_asca1.zip" };

int s23_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../s23/src/s23_rom.c */

/* the boot image (4 MB at 0x1FC00000): ff2vera.ic2 and .ic1 byte-interleaved, .ic2 first: the NMON monitor and, at 0x20000, the game's ELF
 * (one RWX segment at 0x80000000: tools/make_prog.py prints it) */
static int rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && s23_rom_read(dir, "ff2vera.ic1", a, 0x200000) && s23_rom_read(dir, "ff2vera.ic2", b, 0x200000);
    if (ok) for (uint32_t k = 0; k < 0x200000; k++) { boot[2 * k] = b[k]; boot[2 * k + 1] = a[k]; }
    free(a); free(b);
    return ok;
}
/* the sub-CPU's (H8/3002) program: ff2vera.ic3, word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !s23_rom_read(dir, "ff2vera.ic3", dst, 0x80000)) return 0;
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

/* THE HORSE on the ASCA-1's A/D (MAME: gorgon_state keeps namcos23_state::configure_jvs, the ports as they are; adc_r = the 16-bit value's
 * top 10 bits):
 *   0 SWING: the horse rocking fore and aft (MAME AD_STICK_Y, 0x0000-0xFFFF, centre 0x8000). The game measures the swing's size and rhythm
 *     ("use wide swings to speed up"; test menu SWING TEST, "ENTER SW: CLEAR SWING MAX VAL").
 *   1 HANDLE: the reins, left / right (AD_STICK_X, 0x5000-0xB000 reversed, centre 0x8000).
 *   2-7 unused: MAME's dummy ports read 0x8nFF.
 * Buttons (asca_switches, GORgON): whip L = p1.0 (IN_TRIGGER), whip R = p1.15 (FF_IN_WHIP_R), Enter = p1.1, select up / down = p1.5 / p1.4. */
extern uint32_t rr_frame;
extern uint16_t (*s23_asca_analog)(int ch);
extern uint16_t s23_analog_in[8];                 /* [0] swing, [1] handle: MAME's 16-bit values, set by the controls (ff_horse_frame) or $S23_ANALOG */
static uint32_t horse_frame = ~0u;
static uint16_t ff_analog(int ch)
{
    if (rr_frame != horse_frame) { horse_frame = rr_frame; void ff_horse_frame(void); ff_horse_frame(); }   /* once a frame, so a replay repeats it */
    const uint16_t v = ch < 2 ? s23_analog_in[ch] : (uint16_t)(0x80FF | ch << 8);
    return (uint16_t)((v & 0xFFC0) >> 6);
}
__attribute__((constructor)) static void ff_controls_init(void) { s23_analog_in[0] = s23_analog_in[1] = 0x8000; s23_asca_analog = ff_analog; }

/* THE HORSE ON KEYS AND A PAD. The game wants PACING (measured 2026-11-19: one steady fast pace all race = "out of energy! you ran too
 * fast in the first half", 7th; one steady slow pace = "you let the others run at a slow pace", 8th), so Up is a rhythm key: each press
 * starts one full swing (a sine wave fore and aft, nearly full travel) lasting the time since the previous press -- tap faster to gallop
 * faster; holding Up keeps swinging at the last pace (FF_STRIDE frames after a pause). Left / Right pull the reins; Z / X (pad X / B, the
 * triggers) the left / right whip. Nothing held: the horse comes to rest at the end of its swing.
 * Pad: the left stick rocks the horse directly (forward / back) and steers (left / right); A works as Up. */
#include <math.h>
#include <SDL2/SDL.h>
#include "ss22_input.h"
extern uint16_t s23_inputs;
#define FF_IN_RIDE    0x0200
#define FF_IN_LEFT    0x0800
#define FF_IN_RIGHT   0x1000
#define FF_IN_WHIP_R  0x2000                       /* s23_h8.c asca_switches: GORgON p1.15 */
#define FF_SWING_SPAN 0x7000                       /* about 0x8000: nearly the full travel */
#define FF_HANDLE_SPAN 0x3000                      /* MAME's 0x5000-0xB000 about 0x8000 */
#define FF_STRIDE     24                           /* frames a full swing when Up is just held (2.5 a second) */
#define FF_TAP_MIN    8                            /* taps closer than this (7.5 a second) or further apart than FF_TAP_MAX: the held pace */
#define FF_TAP_MAX    60
static SDL_GameController *pad;
static int pad_stick, pad_rock;
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
static const ss22_action ff_actions[] = {
    { "Insert coin",      "coin",     SDL_SCANCODE_5,      SDL_SCANCODE_6,       0x0001, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Ride (swing)",     "ride",     SDL_SCANCODE_UP,     SDL_SCANCODE_W,       FF_IN_RIDE,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Reins left",       "left",     SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,       FF_IN_LEFT,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Reins right",      "right",    SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,       FF_IN_RIGHT,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
    { "Whip left",        "whip_l",   SDL_SCANCODE_Z,      SDL_SCANCODE_1,       0x0010,       0, SS22_PAD(SDL_CONTROLLER_BUTTON_X) },
    { "Whip right",       "whip_r",   SDL_SCANCODE_X,      SDL_SCANCODE_RETURN,  FF_IN_WHIP_R, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
    { "Enter",            "enter",    SDL_SCANCODE_C,      SDL_SCANCODE_UNKNOWN, 0x0100,       0, SS22_PAD(SDL_CONTROLLER_BUTTON_START) },
    { "Service",          "service",  SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN, 0x0004, 0, 0 },
    { "Test",             "test",     SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN, 0x0008, 0, 0 },
    { "Operator up",      "op_up",    SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_UNKNOWN, 0x0040, 0, 0 },
    { "Operator down",    "op_down",  SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_UNKNOWN, 0x0080, 0, 0 },
    { "Operator enter",   "op_enter", SDL_SCANCODE_HOME,     SDL_SCANCODE_UNKNOWN, 0x0100, 0, 0 },
};
static void ff_send_inputs(uint16_t pressed, unsigned wheel, unsigned p1, unsigned p2)
{
    (void)wheel; (void)p1; (void)p2;
    static int scripted = -1;
    if (scripted < 0) scripted = getenv("S23_PRESS") != NULL || getenv("S23_REPLAY") != NULL;
    if (pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) pressed |= 0x0010;
    if (pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) pressed |= FF_IN_WHIP_R;
    if (!scripted) s23_inputs = pressed;
    pad_stick = pad_axis(SDL_CONTROLLER_AXIS_LEFTX);
    pad_rock = pad_axis(SDL_CONTROLLER_AXIS_LEFTY);
}
/* once a frame (ff_analog): the swing and reins from s23_inputs -- the keys, or a replayed session's recording ($S23_REPLAY) */
void ff_horse_frame(void)
{
    static int analog_scripted = -1; if (analog_scripted < 0) analog_scripted = getenv("S23_ANALOG") != NULL;
    if (analog_scripted) return;                   /* tests: $S23_ANALOG owns the horse */
    static int stride, stride2; static unsigned at2;   /* (tests: $FF_STRIDE = "frames" or "frames@frame:frames" -- a second held pace from a frame on) */
    if (!stride) { const char *e = getenv("FF_STRIDE"); stride = e && atoi(e) >= 4 ? atoi(e) : FF_STRIDE;
                   if (e && sscanf(e, "%*d@%u:%d", &at2, &stride2) == 2 && stride2 < 4) stride2 = 0; }
    if (stride2 && rr_frame >= at2) { stride = stride2; stride2 = 0; }
    const uint16_t in = s23_inputs;
    const int ride = (in & FF_IN_RIDE) != 0;
    static double t; static int running, was, period; static uint32_t last_press;   /* t: where the swing is in its stride, 0..1 */
    if (ride && !was) {                            /* a press: a new swing, as long as the gap since the last press */
        const uint32_t gap = rr_frame - last_press;
        period = last_press && gap >= FF_TAP_MIN && gap <= FF_TAP_MAX ? (int)gap : stride;
        last_press = rr_frame; t = 0; running = 1;
    }
    was = ride;
    if (running) { t += 1.0 / period; if (t >= 1) { t -= 1; if (!ride) running = 0, t = 0; } }
    if (ride && !running) running = 1;
    const int left = (in & FF_IN_LEFT) != 0, right = (in & FF_IN_RIGHT) != 0;
    const double rein = left != right ? (left ? -1 : 1) : pad_stick / 32767.0;
    const double swing = pad_rock && !running ? pad_rock / 32767.0 : sin(2 * M_PI * t);   /* the pad's stick rocks the horse itself */
    s23_analog_in[0] = (uint16_t)(0x8000 + (int)(swing * FF_SWING_SPAN));
    s23_analog_in[1] = (uint16_t)(0x8000 - (int)(rein * FF_HANDLE_SPAN));   /* PORT_REVERSE: left reads high. ponytail: check in SWING TEST */
}
/* WIDESCREEN: the attract's scrolling "FINAL FURLONG" tile background is a grid of flat 214 x 160 direct quads just covering the 4:3 screen
 * (4 columns, x -169..687 as it scrolls); the side strips showed black past them. The grid again one grid-width each side fills them.
 * ponytail: tiles found by their 214-pixel width; a scene with other 214-wide direct quads would be copied too */
#include "../../s23/src/s23_c435.h"
static void ff_list_hook(void *lv)
{
    s23_render_list *l = (s23_render_list *)lv;
    int s23_gl_wide(void); if (s23_gl_wide() <= 0) return;
    int lo = 0x7FFF, hi = -0x8000, tiles[64], n = 0;
    for (int i = 0; i < l->count && n < 64; i++) {
        const s23_render_entry *e = &l->e[i]; if (e->type != S23_RE_DIRECT) continue;
        int x0 = 0x7FFF, x1 = -0x8000;
        for (int j = 0; j < 4; j++) { const int x = (int16_t)e->d[4 + j * 6 + 2]; if (x < x0) x0 = x; if (x > x1) x1 = x; }
        if (x1 - x0 != 214) continue;
        tiles[n++] = i; if (x0 < lo) lo = x0; if (x1 > hi) hi = x1;
    }
    const int span = hi - lo;
    for (int s = -1; s <= 1 && n; s += 2) for (int k = 0; k < n && l->count < S23_RENDER_MAX; k++) {
        s23_render_entry *c = &l->e[l->count++]; *c = l->e[tiles[k]];
        for (int j = 0; j < 4; j++) c->d[4 + j * 6 + 2] = (uint16_t)((int16_t)c->d[4 + j * 6 + 2] + s * span);
    }
}
static const ss22_input_game ff_input = {
    .actions = ff_actions, .n = (int)(sizeof ff_actions / sizeof *ff_actions),
    .wheel_min = 0x001, .wheel_max = 0x3FF, .wheel_key_span = 0x1FF, .wheel_step = 12,
    .notes = { "Enter, then press the new key (Esc cancels)",
               "Tap Up in rhythm to ride (faster taps, faster gallop; hold = keep the pace); Left / Right reins; Z / X whips; 5 coin",
               "Pad: rock the left stick forward / back to ride, left / right to steer; X / B or the triggers whip" },
    .send = ff_send_inputs, .test_bit = 0x0008, .service_bit = 0x0004,
};

#include "eng/eng_ui.h"
extern const eng_ui_page ff_link_page;            /* src/ff_link.c */
static void ff_menu_pages(void) { eng_ui_add_page(&ff_link_page); }
extern const s23_link_ops ff_link_ops;

const s23_game_t s23_game = {
    .title = "Final Furlong", .tag = "finfurl", .window_title = "Final Furlong", .mame_set = "finfurl",
    .zip_help = "MAME's finfurl.zip (Final Furlong, FF2 Ver. A) and namco_asca1.zip",
    .zips = zips, .nzips = 2, .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .gorgon = &s23_gorgon, .io_asca = 1, .input = &ff_input,
    .znear = 0.01f,                               /* as Rapid River (MAME's 0.0001 read far-off texture right by the camera there) */
    .spr_alpha = 1,                               /* the race's course-map panel is see-through (alpha 0x80), not solid white */
    .c422_mame = 1,                               /* alone: the link chip as MAME has it (our TC2 model kept its "send done" interrupt up: a storm) */
    .link = &ff_link_ops, .menu_pages = ff_menu_pages,   /* linked: up to 4 cabinets on the C422 ring (src/ff_link.c), its Network page */
    .spr_fe_over = 1,                             /* course select: the race name over the see-through band */
    .spr_first_top = 1,                          /* the message board over everything, the horses on the course map */
    .list_hook = ff_list_hook,                    /* widescreen: the attract's tile background across the whole width */
    .no_hud_shift = 1,                            /* widescreen: the rider's arms stay on the hands (the HUD is sprites) */
    .ap_wrap = 1,                              /* the island in the attract fly-over: on the sea, not under it */
    .cz_fog = 1,                                /* the depth fog from the GORgON's CZ RAM (src/s23_gorgon.c fog) */
    .clock = 135475200u,                          /* BUSCLOCK * 4 (GORgON; System 23 is * 5) */
    .px_num = 250, .px_den = 1323,                /* 25.6 MHz / 135.4752 MHz */
    .sub_div = 8,                                 /* 16.9344 MHz */
    .io_num = 16, .io_den = 147,                  /* 14.7456 MHz */
    .c352_div = 1536,                             /* 88200 Hz */
    .ip = { [S23_SRC_VBLANK] = S23_IP(0), [S23_SRC_C361] = S23_IP(1), [S23_SRC_SUB] = S23_IP(1), [S23_SRC_C435] = S23_IP(2),
            [S23_SRC_C422] = S23_IP(3), [S23_SRC_TIMER] = S23_IP(5) },
    .boot_size = 0x400000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x008E,
    .depth_default = 1,                           /* Display > Depth "exact": a tail and a hind leg overlap by distance (the board sorts whole polygons: the leg cut the tail off) */
    .no_direct_buf = 1,                           /* (GORgON has no 0x0CC00000 direct buffer) */
    .data_rom = { { "ff2mtah.3j", "ff2mtal.1j" }, { NULL, NULL } },
    .npt = 1, .pt_rom = { { "ff2pt0h.9l", "ff2pt0l.9j" } },
    .ccrl = "ff2ccrl.11a", .ccrh = "ff2ccrh.11b", .cg = { "ff2cgll.8b", "ff2cglm.7b", "ff2cgum.6b", "ff2cguu.5b" },
    .spr = { "ff2sprll.12t", "ff2sprlm.11p", "ff2sprum.10p", "ff2spruu.9p" },
    .cg_chip = 0x400000,                          /* 4 MB texture tile chips (MAME region "textile" 0x1000000) */
    .wave = { "ff2wavel.2s", "ff2waveh.3s" },
};
