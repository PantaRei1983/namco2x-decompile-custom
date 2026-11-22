/*
 * af_game.c -- Alpine Surfer (Namco 1996, Super System 22, MAME `alpines`, World AF2 Ver.A) as a table for the shared Super 22 host
 * (engine/ss22_run.c ...), made the same way as Alpine Racer 2 (alpinr2/).
 *
 * The program is gen/af_lifted_NN.c, made by tools/gen/lift68k.py from Ghidra's p-code (tools/import.sh, tools/regen.sh). The master
 * DSP and the sound MCU are interpreted (src/af_interp.c, src/af_snd_interp.c).
 *
 * MAME (namco/namcos22.cpp): machine `alpines` = `alpine` (namcos22s + the step-lock motor) + a ROM BANK: 0x200000-0x3FFFFF shows one
 * of three 2 MB banks of the 8 MB program region, chosen by a write to 0x300000 (alpines_state::rombank_w: data & 3, at most 2). The
 * code is all in the fixed first 2 MB; the banks hold data. keycus: unit 1 answers 0x01A9 (NAMCOS22_ALPINE_SURFER); inputs `alpines`
 * = `alpiner` with the steps' swing (ADC 0) REVERSED; S22-BIOS 1.41; no default EEPROM in the set (the game sets a blank one up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "af_lifted.h"
#include "lift_rt.h"

bool af_c25_exec(c71_t *d, int pc);          /* src/af_interp.c */

static const eng_rom_t k_roms[] = {
    {"af1ccrh.1d", 0x80000}, {"af1ccrl.3d", 0x200000},
    {"af1cg0.8d", 0x200000}, {"af1cg1.10d", 0x200000}, {"af1cg2.12d", 0x200000}, {"af1cg3.13d", 0x200000}, {"af1cg4.14d", 0x200000},
    {"af1data.8k", 0x80000},
    {"af1ptrl0.18k", 0x80000}, {"af1ptrl1.16k", 0x80000},
    {"af1ptrm0.18j", 0x80000}, {"af1ptrm1.16j", 0x80000},
    {"af1ptru0.18f", 0x80000}, {"af1ptru1.16f", 0x80000},
    {"af1scg0b.12f", 0x200000},
    {"af1wavea.2l", 0x400000},
    {"af2ver-a_ll.ic2", 0x200000}, {"af2ver-a_lm.ic3", 0x200000}, {"af2ver-a_um.ic4", 0x200000}, {"af2ver-a_uu.ic5", 0x200000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

/* the whole 8 MB program region; g_ss22.rom holds the fixed 2 MB and the bank in the window */
#define AF_REGION 0x800000u
static uint8_t af_region[AF_REGION];
static int af_bank;
static void af_show_bank(int b)
{
    af_bank = b;
    memcpy(g_ss22.rom + 0x200000, af_region + 0x200000 + (uint32_t)b * 0x200000, 0x200000);
}

static bool af_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: af2ver-a_uu.ic5 / _um.ic4 / _lm.ic3 / _ll.ic2 -> byte 0/1/2/3 of every long */
    static const char *lane[4] = { "af2ver-a_uu.ic5", "af2ver-a_um.ic4", "af2ver-a_lm.ic3", "af2ver-a_ll.ic2" };
    for (int l = 0; l < 4; l++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, lane[l]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < AF_REGION / 4; i++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            af_region[i * 4 + (uint32_t)l] = (uint8_t)c;
        }
        fclose(f);
    }
    memcpy(g_ss22.rom, af_region, 0x200000);
    af_show_bank(0);
    return true;
}

/* the bank register (MAME map(0x300000, 0x300003).w(rombank_w)): the long's low 2 bits pick the bank, clamped to 0..2 */
static void af_rom_write(uint32_t a, int size, uint32_t v)
{
    if (a >= 0x300000 && a < 0x300004) {
        const uint32_t off = a - 0x300000;
        const uint32_t lw = size == 4 ? v : size == 2 ? v << (8 * (2 - off)) : v << (8 * (3 - off));
        int b = (int)(lw & 3); if (b > 2) b = 2;
        if (b != af_bank) af_show_bank(b);
        return;
    }
    static int warned; if (!warned++) fprintf(stderr, "[AF] ROM write %d bytes at %06X (ignored)\n", size, (unsigned)a);
}

static bool af_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/alpines_defaults.nv", dir);     /* not in MAME's set: the game sets a blank EEPROM up itself */
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f);
    fclose(f);
    return n == SS22_EEPROM_SIZE;
}

static const ss22_board_cfg af_board = { "AF", 1, 0x01A9, af_load_program, af_load_eeprom, NULL, af_rom_write };   /* keycus: unit 1 answers 0x01A9 */

#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_START   0x0010
#define IN_LEFT    0x0020
#define IN_RIGHT   0x0040
static const ss22_action actions[] = {
    { "Insert coin",     "coin",        SDL_SCANCODE_5,      SDL_SCANCODE_6,        IN_COIN,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Decision / view", "start",       SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, IN_START,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Service",         "service",     SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN,  IN_SERVICE, 0,                   0 },
    { "Test",            "test",        SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN,  IN_TEST,    0,                   0 },
    { "Select left",     "select_left", SDL_SCANCODE_Z,      SDL_SCANCODE_UNKNOWN,  IN_LEFT,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Select right",    "select_right",SDL_SCANCODE_X,      SDL_SCANCODE_UNKNOWN,  IN_RIGHT,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
    { "Swing left",      "swing_left",  SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,        0,          SS22_AX_WHEEL_LEFT,  0 },
    { "Swing right",     "swing_right", SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,        0,          SS22_AX_WHEEL_RIGHT, 0 },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x080, 0x380, 0x180, 16,
    { 0x100, 0x100 }, 16,
    { "Enter, then press the new key (Esc cancels)", "", "Pad: stick swings the board" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
};

static const ss22_press_name presses[] = { {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST}, {"left", IN_LEFT}, {"right", IN_RIGHT} };

static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)pedal1; (void)pedal2;
    /* AF_AUTO picks the test player: 0 wiggles (default), 1 rides straight, 2 / 3 press Select right / left before each Decision,
     * 4 never coins up (the attract loop), 5 wiggles gently, 6 goes into the test menu and steps through it */
    static int v = -1; if (v < 0) { const char *e = getenv("AF_AUTO"); v = e ? atoi(e) : 0; }
    *wheel = 0x200;
    if (v == 4) return;
    if (v == 6) {
        if (n > 300 && n < 9000) *p |= IN_TEST;
        if (n % 240 >= 100 && n % 240 < 106) *p |= IN_RIGHT;
        if (n % 480 >= 300 && n % 480 < 306) *p |= IN_START;
        if (n % 1200 >= 700 && n % 1200 < 706) *p |= IN_SERVICE;
        return;
    }
    if (n % 600 >= 300 && n % 600 < 306) *p |= IN_COIN;
    if (n % 300 >= 150 && n % 300 < 156 && n > 400) *p |= IN_START;
    if (v == 2 && n % 300 >= 100 && n % 300 < 106) *p |= IN_RIGHT;
    if (v == 3 && n % 300 >= 100 && n % 300 < 106) *p |= IN_LEFT;
    if (v == 1) return;
    if (v == 5) *wheel = ((n / 45) % 2 == 0) ? 0x1C0 : 0x240;
    else        *wheel = ((n / 90) % 2 == 0) ? 0x100 : 0x300;
}

/* THE STEP-LOCK MOTOR (MAME alpine_state::alpine_mcu_port4_w / alpine_motor_r), as Alpine Racer 2 */
static int motor_status = 2, motor_next, motor_timer;
uint8_t g_lift_cov[0x200000];                /* gen/: which instructions have run (LIFT_LAST_PC) */
uint32_t g_lift_pc;                          /* gen/: the instruction last started (LIFT_LAST_PC) */
static void af_frame(long n)
{
    { static int on = -1; if (on < 0) on = getenv("AF_PCLOG") != NULL;      /* AF_PCLOG=1: where the 68K is at each frame's end */
      if (on && n % 20 == 0) fprintf(stderr, "[AF] f%ld pc %06X  D0 %08X A0 %08X SP %08X  bank %d\n", n, (unsigned)g_lift_pc, (unsigned)RG4(0x00), (unsigned)RG4(0x20), (unsigned)RG4(0x3C), af_bank); }
    { static long c0 = -2, c1; if (c0 == -2) { c0 = -1; const char *e = getenv("AF_COV"); if (e) sscanf(e, "%ld:%ld", &c0, &c1); }   /* AF_COV=from:to -> af_cov.txt */
      if (c0 >= 0 && n == c0) memset(g_lift_cov, 0, sizeof g_lift_cov);
      if (c0 >= 0 && n == c1) { FILE *f = fopen("af_cov.txt", "w"); if (f) { for (unsigned a = 0; a < sizeof g_lift_cov; a++) if (g_lift_cov[a]) fprintf(f, "%X\n", a << 1); fclose(f); } } }
    const unsigned out = ss22_snd_outputs();
    if (motor_timer > 0 && --motor_timer == 0) motor_status = motor_next;
    if ((out & 6) == 6) { if (motor_status == 2) { motor_status = 0; motor_next = 1; motor_timer = 30; } }
    else if (out & 4)   { if (motor_status == 1) { motor_status = 0; motor_next = 2; motor_timer = 30; } }
}
static uint16_t af_inputs(uint16_t in)
{
    return (uint16_t)((in & ~0x0180u) | (motor_status & 1 ? 0x0080u : 0) | (motor_status & 2 ? 0x0100u : 0));
}

/* the board's SWING (ADC 0) is reversed on this cabinet (MAME PORT_REVERSE); its EDGE (ADC 1) follows the player's swing as in Alpine
 * Racer. The steps read centred while the motor holds them. AF_EDGE=off holds the edge at centre, AF_EDGE=reverse leans it the other way. */
static void af_adc(uint16_t *adc)
{
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("AF_EDGE"); mode = !e ? 1 : !strcmp(e, "off") ? 0 : !strcmp(e, "reverse") ? 2 : 1; }
    uint16_t s = adc[0];
    if (motor_status != 1) s = 0x200;
    adc[0] = (uint16_t)(0x400 - s);
    adc[1] = mode == 0 ? 0x200 : mode == 2 ? (uint16_t)(0x400 - s) : s;
}

/* The 2D effects of the sprite layer (the Display menu's "Effects" option smooths them): 1..221 are the snow the board throws up -- the spray at
 * the tail, the bursts on a turn or a landing, the big white-out when you hit the snow wall (from 222 the ROM holds the menus' words). 65535 is
 * how a linked spray sprite whose first cell is empty (link-table word 0xFFFF) comes out: in this game only that spray does. Found by logging
 * every sprite of an autoplay run against the sprite ROM's pictures. */
static const int fx_tiles[][2] = { { 1, 221 }, { 65535, 65535 } };

static const ss22_game game = {
    .name = "Alpine Surfer", .tag = "AF", .lname = "af", .logname = "alpines.log", .zip = "alpines.zip",
    .board = &af_board,
    .dsp = { { {"af1ptrl0.18k", "af1ptrl1.16k"}, {"af1ptrm0.18j", "af1ptrm1.16j"}, {"af1ptru0.18f", "af1ptru1.16f"} }, 2, af_c25_exec },
    /* MAME's c352 region: af1wavea.2l at 0. INPUTS idle: everything released, the motor "locked" */
    .snd = { "af1data.8k", { "af1wavea.2l", NULL }, { 0, 0 }, 0x1000000, 0xFFFF, { 1, 3 } },
    .video = { { "af1cg0.8d", "af1cg1.10d", "af1cg2.12d", "af1cg3.13d", "af1cg4.14d", NULL, NULL, NULL },
               "af1ccrl.3d", "af1ccrh.1d", { "af1scg0b.12f" }, 1, 0x1000000, 0xFF,
               .fx = fx_tiles, .n_fx = 2 },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_1000,
    .polls_per_frame = 66772,           /* Tokyo Wars' figure (same CPU, same clock): not measured for this game */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x100, 0x100 },
    .link_id_byte = -1,
    .frame = af_frame,
};

extern int g_eng_tex_smooth_default;           /* engine/tex_bake.c */
int main(int argc, char **argv) {
    g_eng_tex_smooth_default = 1;               /* 3D texture filter Bilinear by default, as Alpine Racer 2 (Display page; texture_filter in the cfg) */
    g_ss22_inputs_hook = af_inputs; g_ss22_adc_hook = af_adc; return ss22_main(argc, argv, &game); }
