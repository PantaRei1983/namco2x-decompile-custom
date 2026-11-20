/*
 * aj_game.c -- Aqua Jet (Namco 1996, Super System 22, MAME `aquajet`, World AJ2 Ver.B) as a table for the shared Super 22 host
 * (engine/ss22_run.c ...), made the same way as Alpine Racer 2 (alpinr2/) and Alpine Surfer (alpines/).
 *
 * The program is gen/aj_lifted_NN.c, made by tools/gen/lift68k.py from Ghidra's p-code (tools/import.sh, tools/regen.sh). The master
 * DSP and the sound MCU are interpreted (src/aj_interp.c, src/aj_snd_interp.c).
 *
 * MAME (namco/namcos22.cpp): machine `cybrcycc` (namcos22s + the "tank" speaker on C352 output 2); keycus: no fixed answer
 * (NAMCOS22_AQUA_JET has no case: every read is random); S22-BIOS 1.41; INPUTS: coin 0x01, service 0x04, test 0x08, start 0x10.
 * All three A-D channels are PORT_REVERSE: ADC 0 = the handlebar (0x000..0x3F8, centre 0x1FC), ADC 1 = the throttle (0x0E0..0x1FC:
 * released reads 0x1FC, full 0x0E0), ADC 2 = the rider's lean, a centred stick (0x000..0x3F8, centre 0x1FC).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "aj_lifted.h"
#include "lift_rt.h"

bool aj_c25_exec(c71_t *d, int pc);          /* src/aj_interp.c */

static const eng_rom_t k_roms[] = {
    {"aquajet_defaults.nv", 0x2000},
    {"aj1ccrh.1d", 0x80000}, {"aj1ccrl.3d", 0x200000},
    {"aj1cg0.8d", 0x200000}, {"aj1cg1.10d", 0x200000}, {"aj1cg2.12d", 0x200000}, {"aj1cg3.13d", 0x200000},
    {"aj1cg4.14d", 0x200000}, {"aj1cg5.16d", 0x200000}, {"aj1cg6.18d", 0x200000}, {"aj1cg7.19d", 0x200000},
    {"aj1data.8k", 0x80000},
    {"aj1ptrl0.18k", 0x80000}, {"aj1ptrl1.16k", 0x80000}, {"aj1ptrl2.15k", 0x80000}, {"aj1ptrl3.14k", 0x80000},
    {"aj1ptrm0.18j", 0x80000}, {"aj1ptrm1.16j", 0x80000}, {"aj1ptrm2.15j", 0x80000}, {"aj1ptrm3.14j", 0x80000},
    {"aj1ptru0.18f", 0x80000}, {"aj1ptru1.16f", 0x80000}, {"aj1ptru2.15f", 0x80000}, {"aj1ptru3.14f", 0x80000},
    {"aj1scg0.12f", 0x200000}, {"aj1scg1.10f", 0x200000}, {"aj1scg2.8f", 0x200000},
    {"aj1wavea.2l", 0x400000}, {"aj1waveb.1l", 0x400000},
    /* older MAME sets name the program chips aj2ver-b.N (the same dumps, same CRCs) */
    {"aj2verb.1", 0x100000, NULL, "aj2ver-b.1"}, {"aj2verb.2", 0x100000, NULL, "aj2ver-b.2"},
    {"aj2verb.3", 0x100000, NULL, "aj2ver-b.3"}, {"aj2verb.4", 0x100000, NULL, "aj2ver-b.4"},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

static bool aj_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: aj2verb.4/.3/.2/.1 -> byte 0/1/2/3 of every long */
    static const char *lane[4] = { "aj2verb.4", "aj2verb.3", "aj2verb.2", "aj2verb.1" };
    for (int l = 0; l < 4; l++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, lane[l]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < SS22_ROM_SIZE / 4; i++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            g_ss22.rom[i * 4 + (uint32_t)l] = (uint8_t)c;
        }
        fclose(f);
    }
    return true;
}

static bool aj_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/aquajet_defaults.nv", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[AJ] no %s -- EEPROM blank\n", p); return false; }
    size_t n = fread(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f);
    fclose(f);
    return n == SS22_EEPROM_SIZE;
}

static const ss22_board_cfg aj_board = { "AJ", 0xFFFF, 0, aj_load_program, aj_load_eeprom, NULL, NULL };   /* keycus: random on every unit */

#define IN_COIN     0x0001
#define IN_SERVICE  0x0004
#define IN_TEST     0x0008
#define IN_START    0x0010
#define IN_LEANBACK 0x0020                   /* not a cabinet input: our "lean back" key, taken off INPUTS by aj_inputs and put on ADC 2 */
static const ss22_action actions[] = {
    { "Insert coin",     "coin",        SDL_SCANCODE_5,      SDL_SCANCODE_6,        IN_COIN,     0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start / view",    "start",       SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, IN_START,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Service",         "service",     SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN,  IN_SERVICE,  0,                   0 },
    { "Test",            "test",        SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN,  IN_TEST,     0,                   0 },
    { "Steer left",      "steer_left",  SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,        0,           SS22_AX_WHEEL_LEFT,  0 },
    { "Steer right",     "steer_right", SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,        0,           SS22_AX_WHEEL_RIGHT, 0 },
    { "Throttle",        "throttle",    SDL_SCANCODE_UP,     SDL_SCANCODE_W,        0,           SS22_AX_PEDAL1,      0 },
    { "Lean forward",    "lean_fwd",    SDL_SCANCODE_Z,      SDL_SCANCODE_UNKNOWN,  0,           SS22_AX_PEDAL2,      0 },
    { "Lean back",       "lean_back",   SDL_SCANCODE_X,      SDL_SCANCODE_DOWN,     IN_LEANBACK, 0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x004, 0x3FC, 0x1F8, 40,               /* the handlebar's swing (centre 0x200 here; aj_adc moves it onto MAME's 0x000..0x3F8) */
    { 0x11C, 0x1FC }, 40,                  /* throttle travel 0x1FC - 0x0E0; lean forward to the stick's end */
    { "Enter, then press the new key (Esc cancels)", "Down / X: lean back", "Pad: stick steers, right trigger = throttle, left trigger = lean forward, B = lean back" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
};

static const ss22_press_name presses[] = { {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST} };

static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)pedal2;
    /* AJ_AUTO picks the test player: 0 rides (coin, start, full throttle, a weave; default), 4 never coins up (the attract loop),
     * 6 goes into the test menu and steps through it */
    static int v = -1; if (v < 0) { const char *e = getenv("AJ_AUTO"); v = e ? atoi(e) : 0; }
    *wheel = 0x200;
    if (v == 4) return;
    if (v == 6) {
        if (n > 300 && n < 9000) *p |= IN_TEST;
        if (n % 480 >= 300 && n % 480 < 306) *p |= IN_START;
        if (n % 1200 >= 700 && n % 1200 < 706) *p |= IN_SERVICE;
        return;
    }
    if (n % 600 >= 300 && n % 600 < 306) *p |= IN_COIN;
    if (n % 300 >= 150 && n % 300 < 156 && n > 400) *p |= IN_START;
    if (n > 700) *pedal1 = 0x11C;
    *wheel = ((n / 90) % 2 == 0) ? 0x180 : 0x280;
}

uint8_t g_lift_cov[0x200000];                /* gen/: which instructions have run (LIFT_LAST_PC) */
uint32_t g_lift_pc;                          /* gen/: the instruction last started (LIFT_LAST_PC) */
static void aj_frame(long n)
{
    { static int on = -1; if (on < 0) on = getenv("AJ_PCLOG") != NULL;      /* AJ_PCLOG=1: where the 68K is at each frame's end */
      if (on && n % 20 == 0) fprintf(stderr, "[AJ] f%ld pc %06X  D0 %08X A0 %08X SP %08X\n", n, (unsigned)g_lift_pc, (unsigned)RG4(0x00), (unsigned)RG4(0x20), (unsigned)RG4(0x3C)); }
    { static long c0 = -2, c1; if (c0 == -2) { c0 = -1; const char *e = getenv("AJ_COV"); if (e) sscanf(e, "%ld:%ld", &c0, &c1); }   /* AJ_COV=from:to -> aj_cov.txt */
      if (c0 >= 0 && n == c0) memset(g_lift_cov, 0, sizeof g_lift_cov);
      if (c0 >= 0 && n == c1) { FILE *f = fopen("aj_cov.txt", "w"); if (f) { for (unsigned a = 0; a < sizeof g_lift_cov; a++) if (g_lift_cov[a]) fprintf(f, "%X\n", a << 1); fclose(f); } } }
}

static bool lean_back;
static uint16_t aj_inputs(uint16_t in)       /* INPUTS is active low: the lean-back key is ours, not the cabinet's */
{
    lean_back = !(in & IN_LEANBACK);
    return (uint16_t)(in | IN_LEANBACK);
}
/* the controls onto MAME's reversed channels: the handlebar 0x3FC - wheel (centre 0x1FC); the throttle 0x1FC released .. 0x0E0 full;
 * the lean centre 0x1FC, forward towards 0x000 (the reversed stick's "up"), back towards 0x3F8. AJ_LEAN=reverse swaps forward and back. */
static void aj_adc(uint16_t *adc)
{
    static int rev = -1; if (rev < 0) { const char *e = getenv("AJ_LEAN"); rev = e && !strcmp(e, "reverse"); }
    int w = 0x3FC - (int)adc[0]; if (w < 0) w = 0; if (w > 0x3F8) w = 0x3F8;
    adc[0] = (uint16_t)w;
    int t = 0x1FC - (int)adc[1]; if (t < 0x0E0) t = 0x0E0;
    adc[1] = (uint16_t)t;
    int lean = (int)adc[2] - (lean_back ? 0x1FC : 0);
    if (rev) lean = -lean;
    int l = 0x1FC - lean; if (l < 0) l = 0; if (l > 0x3F8) l = 0x3F8;
    adc[2] = (uint16_t)l;
}

/* The water of the 2D sprite layer, by the tile each sprite draws (found by listing every sprite of an autoplay race against the sprite ROM's pictures):
 * 1..800 the spray thrown over the screen's edges while riding (eight frames, each four 5x5-tile pieces); 1346..3585 the wall of water when the course
 * runs under the waterfall. 801.. are the TIME digits, the banners and the course pictures; from 3586 the ranking and the staff roll's letters. The
 * Display menu's "Effects" option smooths exactly these. */
static const int fx_tiles[][2] = { { 1, 800 }, { 1346, 3585 } };

static const ss22_game game = {
    .name = "Aqua Jet", .tag = "AJ", .lname = "aj", .logname = "aquajet.log", .zip = "aquajet.zip",
    .board = &aj_board,
    .dsp = { { {"aj1ptrl0.18k", "aj1ptrl1.16k", "aj1ptrl2.15k", "aj1ptrl3.14k"},
               {"aj1ptrm0.18j", "aj1ptrm1.16j", "aj1ptrm2.15j", "aj1ptrm3.14j"},
               {"aj1ptru0.18f", "aj1ptru1.16f", "aj1ptru2.15f", "aj1ptru3.14f"} }, 4, aj_c25_exec },
    /* MAME's c352 region: aj1wavea.2l at 0, aj1waveb.1l at 0x800000; machine cybrcycc: add_route(2, "tank", 1.0) */
    .snd = { "aj1data.8k", { "aj1wavea.2l", "aj1waveb.1l" }, { 0, 0x800000 }, 0x1000000, 0xFFFF, { 1, 2 },
             .aux = { [0] = { "Tank speaker", 1.0f } } },
    .video = { { "aj1cg0.8d", "aj1cg1.10d", "aj1cg2.12d", "aj1cg3.13d", "aj1cg4.14d", "aj1cg5.16d", "aj1cg6.18d", "aj1cg7.19d" },
               "aj1ccrl.3d", "aj1ccrh.1d", { "aj1scg0.12f", "aj1scg1.10f", "aj1scg2.8f" }, 3, 0x1000000, 0xFF,
               .fx = fx_tiles, .n_fx = 2 },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_800,
    .polls_per_frame = 66772,           /* Tokyo Wars' figure (same CPU, same clock): not measured for this game */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x11C, 0 },
    .link_id_byte = -1,
    .frame = aj_frame,
};

int main(int argc, char **argv) { g_ss22_inputs_hook = aj_inputs; g_ss22_adc_hook = aj_adc; return ss22_main(argc, argv, &game); }
