/* pp_game.c -- PANIC PARK's table for the System 23 host (../s23/include/s23_game.h): MAME's panicprk set (World, PNP2 Ver. A) on Namco
 * System 23 (the board Time Crisis II runs on), with the ASCA-3A I/O board. Values from MAME's namcos23.cpp (namcos23_state::s23 /
 * panicprk, ROM_START(panicprk), INPUT_PORTS_START(panicprk)) and namcoio.cpp (namco_asca_3a_device = namco_asca_3_device). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s23_game.h"

static const eng_rom_t chips[] = {
    { "pnp2vera.ic2", 0x200000, NULL, NULL, 0xCD528597 }, { "pnp2vera.ic1", 0x200000, NULL, NULL, 0x80FEA853 },
    { "pnp1vera.ic3", 0x80000, NULL, NULL, 0xFE4BC6F4 },  { "asca-3a.ic14", 0x40000, NULL, NULL, 0x8E9266E5 },
    { "pnp1mtah.2j", 0x800000, NULL, NULL, 0x37ADDDDD }, { "pnp1mtal.2h", 0x800000, NULL, NULL, 0x6490FAAA },
    { "pnp1cgll.4m", 0x800000, NULL, NULL, 0xD03932CF }, { "pnp1cglm.5k", 0x800000, NULL, NULL, 0xABF4CCF2 },
    { "pnp1cgum.4j", 0x800000, NULL, NULL, 0x206217CA }, { "pnp1cguu.5f", 0x800000, NULL, NULL, 0xCD64F57F },
    { "pnp1ccrl.7f", 0x200000, NULL, NULL, 0xB7BC43C2 }, { "pnp1ccrh.7e", 0x200000, NULL, NULL, 0xCAAF1B73 },
    { "pnp1pt0h.7a", 0x400000, NULL, NULL, 0x43FC2246 }, { "pnp1pt0l.7c", 0x400000, NULL, NULL, 0x26AF5FA1 },
    { "pnp1pt1h.5a", 0x400000, NULL, NULL, 0x1FF470C0 }, { "pnp1pt1l.5c", 0x400000, NULL, NULL, 0x15C6F236 },
    { "pnp1wavel.2c", 0x800000, NULL, NULL, 0x35C6A9BD }, { "pnp1waveh.2a", 0x800000, NULL, NULL, 0x6FA1826A },
};
static const char *const zips[] = { "panicprk.zip", "namco_asca3a.zip" };   /* a split MAME set keeps the I/O board's program in its device zip */

int s23_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../s23/src/s23_rom.c */

/* the boot image (4 MB at 0x1FC00000): pnp2vera.ic2 and .ic1 byte-interleaved, .ic2 first: the NMON monitor ("NMON 1.0.5 P for SYSTEM23 P1")
 * and, at 0x20000, the game's ELF (tools/make_prog.py prints the layout) */
static int rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && s23_rom_read(dir, "pnp2vera.ic1", a, 0x200000) && s23_rom_read(dir, "pnp2vera.ic2", b, 0x200000);
    if (ok) for (uint32_t k = 0; k < 0x200000; k++) { boot[2 * k] = b[k]; boot[2 * k + 1] = a[k]; }
    free(a); free(b);
    return ok;
}
/* the sub-CPU's (H8/3002) program: pnp1vera.ic3, word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !s23_rom_read(dir, "pnp1vera.ic3", dst, 0x80000)) return 0;
    for (uint32_t k = 0; k < 0x80000; k += 2) { const uint8_t t = dst[k]; dst[k] = dst[k + 1]; dst[k + 1] = t; }
    return 1;
}
/* the ASCA-3A I/O board's (H8/3334) program: the first n bytes of asca-3a.ic14 */
static int rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x40000);
    const int ok = all && n <= 0x40000 && s23_rom_read(dir, "asca-3a.ic14", all, 0x40000);
    if (ok) memcpy(dst, all, n);
    free(all);
    return ok;
}

/* THE CONTROLS (MAME INPUT_PORTS_START(panicprk)): each player has a JOYSTICK the game reads left / right (an A/D channel each: player 1
 * channel 0, player 2 channel 1; 0x0000-0xFFFF, centre 0x8000; the A/D takes the top 10 bits) and a START button (player 1 = JVS button 2,
 * p1 bit 0; player 2 = button 3, p1 bit 15). The operator's Up / Down / Enter (bits 5, 4, 1) work the test menu. */
#include <math.h>
#include <SDL2/SDL.h>
#include "ss22_input.h"
extern uint16_t s23_inputs;
extern uint32_t rr_frame;
#define PP_IN_COIN     0x0001
#define PP_IN_SERVICE  0x0004
#define PP_IN_TEST     0x0008
#define PP_IN_START1   0x0010
#define PP_IN_START2   0x0020
#define PP_IN_OP_UP    0x0040
#define PP_IN_OP_DOWN  0x0080
#define PP_IN_OP_ENTER 0x0100
#define PP_IN_P1_LEFT  0x0200
#define PP_IN_P1_RIGHT 0x0400
#define PP_IN_P2_LEFT  0x0800
#define PP_IN_P2_RIGHT 0x1000
#define PP_IN_COIN2    0x2000

/* MAME namco_asca_1_device::in_r with Panic Park's JVS_PLAYER1 */
static uint8_t pp_asca_in(int n, int test_on)
{
    const uint16_t in = s23_inputs;
    switch (n) {
    case 0: return (uint8_t)~((!!(in & PP_IN_START2)) << 5 | (!!(in & PP_IN_START1)) << 4 | (!!(in & PP_IN_OP_ENTER)) << 3
                              | (!!(in & PP_IN_OP_DOWN)) << 2 | (!!(in & PP_IN_OP_UP)) << 1 | test_on);
    case 1: return 0xFF;
    case 2: return (uint8_t)~((!!(in & PP_IN_COIN)) << 3);
    default: return (uint8_t)~((!!(in & PP_IN_COIN2)) << 3 | 0x03);   /* both coin counters connected */
    }
}

/* the two levers (analogue, 10-bit A/D): a pad's stick as it is, from just outside a small dead zone (the whole travel, every point of it);
 * a key moves the lever steadily towards that side -- about a third of a second to the stop -- and it comes back to the centre at the
 * same pace when let go, so a tap gives a small step (it jumped from stop to stop in 4 frames: no positions in between) */
static SDL_GameController *pads[2];
static int pad_x[2];                               /* the sticks read with the keys: [0] player 1, [1] player 2 (-32768..32767, 0 = none) */
static uint16_t stick[2] = { 0x8000, 0x8000 };
static int pp_pad_axis(int i, SDL_GameControllerAxis ax)
{
    if (!pads[i] || !SDL_GameControllerGetAttached(pads[i])) {
        pads[i] = NULL;
        int s23_pad_choice(void); const int only = s23_pad_choice();
        if (only == -3) return 0;
        for (int j = 0, gi = -1; j < SDL_NumJoysticks(); j++) if (SDL_IsGameController(j) && ++gi == (only >= 0 ? only + i : i)) { pads[i] = SDL_GameControllerOpen(j); break; }
        if (!pads[i]) return 0;
    }
    const int v = SDL_GameControllerGetAxis(pads[i], ax);
    const int dz = 2500;                           /* the dead zone, then the rest of the travel spread over the whole range */
    if (v > -dz && v < dz) return 0;
    return v > 0 ? (int)((long)(v - dz) * 32767 / (32767 - dz)) : -(int)((long)(-v - dz) * 32767 / (32768 - dz));
}
static void pp_stick_frame(void)
{
    static int analog_scripted = -1; if (analog_scripted < 0) analog_scripted = getenv("S23_ANALOG") != NULL;
    if (analog_scripted) return;                   /* tests: $S23_ANALOG owns the sticks */
    const uint16_t in = s23_inputs;
    for (int p = 0; p < 2; p++) {
        const int l = (in & (p ? PP_IN_P2_LEFT : PP_IN_P1_LEFT)) != 0, r = (in & (p ? PP_IN_P2_RIGHT : PP_IN_P1_RIGHT)) != 0;
        int target;
        if (l != r) target = l ? 0x0000 : 0xFFFF;
        else if (pad_x[p]) target = 0x8000 + (int)((long)pad_x[p] * 0x7FFF / 32767);
        else target = 0x8000;
        if (target < 0) target = 0;
        if (target > 0xFFFF) target = 0xFFFF;
        if (pad_x[p] && l == r) { stick[p] = (uint16_t)target; continue; }   /* a stick: where it is */
        int v = stick[p];
        const int step = 0x0C00;
        v += target > v ? (target - v < step ? target - v : step) : -(v - target < step ? v - target : step);
        stick[p] = (uint16_t)v;
    }
}
extern uint16_t (*s23_asca_analog)(int ch);
extern uint16_t s23_analog_in[8];                 /* $S23_ANALOG sets [0] / [1] in tests */
static uint32_t stick_frame = ~0u;
static uint16_t pp_analog(int ch)
{
    if (rr_frame != stick_frame) { stick_frame = rr_frame; pp_stick_frame(); }   /* once a frame, so a replay repeats it */
    static int analog_scripted = -1; if (analog_scripted < 0) analog_scripted = getenv("S23_ANALOG") != NULL;
    uint16_t v = 0x8000;
    if (ch == 0 || ch == 1) v = analog_scripted ? s23_analog_in[ch] : stick[ch];
    return (uint16_t)((v & 0xFFC0) >> 6);
}
__attribute__((constructor)) static void pp_controls_init(void) { s23_asca_analog = pp_analog; }

static const ss22_action pp_actions[] = {
    { "Insert coin",      "coin",     SDL_SCANCODE_5,        SDL_SCANCODE_6,       PP_IN_COIN,     0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "P1 Start",         "start1",   SDL_SCANCODE_1,        SDL_SCANCODE_RETURN,  PP_IN_START1,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "P1 Stick left",    "p1_left",  SDL_SCANCODE_LEFT,     SDL_SCANCODE_UNKNOWN, PP_IN_P1_LEFT,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_DPAD_LEFT) },
    { "P1 Stick right",   "p1_right", SDL_SCANCODE_RIGHT,    SDL_SCANCODE_UNKNOWN, PP_IN_P1_RIGHT, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) },
    { "P2 Start",         "start2",   SDL_SCANCODE_2,        SDL_SCANCODE_UNKNOWN, PP_IN_START2,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
    { "P2 Stick left",    "p2_left",  SDL_SCANCODE_A,        SDL_SCANCODE_UNKNOWN, PP_IN_P2_LEFT,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_X) },
    { "P2 Stick right",   "p2_right", SDL_SCANCODE_D,        SDL_SCANCODE_UNKNOWN, PP_IN_P2_RIGHT, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_Y) },
    { "Service",          "service",  SDL_SCANCODE_9,        SDL_SCANCODE_UNKNOWN, PP_IN_SERVICE,  0, 0 },
    { "Test",             "test",     SDL_SCANCODE_F2,       SDL_SCANCODE_UNKNOWN, PP_IN_TEST,     0, 0 },
    { "Operator up",      "op_up",    SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_UP,      PP_IN_OP_UP,    0, 0 },
    { "Operator down",    "op_down",  SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_DOWN,    PP_IN_OP_DOWN,  0, 0 },
    { "Operator enter",   "op_enter", SDL_SCANCODE_HOME,     SDL_SCANCODE_UNKNOWN, PP_IN_OP_ENTER, 0, 0 },
};
static void pp_send_inputs(uint16_t pressed, unsigned wheel, unsigned p1, unsigned p2)
{
    (void)wheel; (void)p1; (void)p2;
    static int scripted = -1;
    if (scripted < 0) scripted = getenv("S23_PRESS") != NULL || getenv("S23_REPLAY") != NULL;
    pad_x[0] = pp_pad_axis(0, SDL_CONTROLLER_AXIS_LEFTX);
    /* player 2: a second pad's left stick, or the first pad's right stick */
    pad_x[1] = pads[1] || pp_pad_axis(1, SDL_CONTROLLER_AXIS_LEFTX) ? pp_pad_axis(1, SDL_CONTROLLER_AXIS_LEFTX) : pp_pad_axis(0, SDL_CONTROLLER_AXIS_RIGHTX);
    if (pads[1] && SDL_GameControllerGetButton(pads[1], SDL_CONTROLLER_BUTTON_A)) pressed |= PP_IN_START2;
    if (!scripted) s23_inputs = pressed;
}
static const ss22_input_game pp_input = {
    .actions = pp_actions, .n = (int)(sizeof pp_actions / sizeof *pp_actions),
    .wheel_min = 0x001, .wheel_max = 0x3FF, .wheel_key_span = 0x1FF, .wheel_step = 12,
    .notes = { "Enter, then press the new key (Esc cancels)",
               "Player 1: Left / Right = the joystick, 1 = START. Player 2: A / D, 2 = START. 5 = coin",
               "Pads: the left stick is the joystick, A = START (a second pad is player 2; one pad: right stick + B)" },
    .send = pp_send_inputs, .test_bit = PP_IN_TEST, .service_bit = PP_IN_SERVICE,
    .light_gun = false,                            /* no gun: the Display page leaves out the crosshair rows */
};

const s23_game_t s23_game = {
    .title = "Panic Park", .tag = "panicprk", .window_title = "Panic Park", .mame_set = "panicprk",
    .zip_help = "MAME's panicprk.zip (Panic Park, World PNP2 Ver. A) and namco_asca3a.zip",
    .zips = zips, .nzips = 2, .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .io_asca = 1, .asca_in = pp_asca_in, .input = &pp_input,
    .irq_prompt = 1,                              /* the C435's "model 0" interrupt taken at once, as on the board: its handler writes the next stretch's
                                                   * state before the main loop sends that stretch's models (it came a model or two late, every few frames:
                                                   * the background objects flickered in front of / behind each other) */
    .zoff_state = 6,                              /* state 9: a signed depth offset (title letters, Panic King scores, the practice's castle) (include/s23_game.h) */
    .vp_front0 = 1,                               /* the PANIC PARK plate over the monitor's NEEDED picture, as TeknoParrot shows it (include/s23_game.h) */
    .null_model = 0x6EF,
    .direct_v_shift = -1,                         /* the namco logo red, as the arcade's (include/s23_game.h) */
    .no_depth = 1,                                /* Display > Depth not offered: exact depth hid the title letters (include/s23_game.h) */
    .depth_within = 1,                            /* exact depth inside one model always: the INTERACTIVE CONTROLS close-up's red player showed his inside
                                                   * (the back of his head over the front) -- painter order within a model */
    .twin = 0, .evo2 = 0,
    .clock = 169344000u,                          /* BUSCLOCK * 5 */
    .px_num = 200, .px_den = 1323,                /* 25.6 MHz / 169.344 MHz */
    .sub_div = 10,                                /* 16.9344 MHz */
    .io_num = 64, .io_den = 735,                  /* 14.7456 MHz */
    .c352_div = 1920,                             /* 88200 Hz */
    /* MAME namcos23_state::machine_start: IRQ0 vblank, IRQ1 C361 + sub-CPU, IRQ2 C435, IRQ3 C422, IRQ5 RS-232 */
    .ip = { [S23_SRC_VBLANK] = S23_IP(0), [S23_SRC_C361] = S23_IP(1), [S23_SRC_SUB] = S23_IP(1), [S23_SRC_C435] = S23_IP(2),
            [S23_SRC_C422] = S23_IP(3), [S23_SRC_TIMER] = S23_IP(5) },
    .boot_size = 0x400000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x008E, .depth_default = 0,
    .data_rom = { { "pnp1mtah.2j", "pnp1mtal.2h" }, { NULL, NULL } },
    .npt = 2, .pt_rom = { { "pnp1pt0h.7a", "pnp1pt0l.7c" }, { "pnp1pt1h.5a", "pnp1pt1l.5c" } },
    .ccrl = "pnp1ccrl.7f", .ccrh = "pnp1ccrh.7e", .ccrl_half = 1, .cg = { "pnp1cgll.4m", "pnp1cglm.5k", "pnp1cgum.4j", "pnp1cguu.5f" },
    .wave = { "pnp1wavel.2c", "pnp1waveh.2a" },
};
