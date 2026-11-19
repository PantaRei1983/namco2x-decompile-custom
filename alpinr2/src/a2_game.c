/*
 * a2_game.c -- Alpine Racer 2 (Namco 1996, Super System 22, MAME `alpinr2a`, World ARS2 Ver.A) as a table for the shared Super 22 host
 * (engine/ss22_run.c ...), made the same way as Alpine Racer (alpiner/) and Cyber Cycles (cybrcycc/).
 *
 * The program is gen/a2_lifted_NN.c, made by tools/gen/lift68k.py from Ghidra's p-code (tools/import.sh, tools/regen.sh). The master
 * DSP and the sound MCU are interpreted (src/a2_interp.c, src/a2_snd_interp.c).
 *
 * MAME (namco/namcos22.cpp): machine `alpine` and inputs `alpiner`, as Alpine Racer: namcos22s + the step-lock motor; keycus: no fixed
 * answer (NAMCOS22_ALPINE_RACER_2 has no case: every read is random); INPUTS: coin 0x01, service 0x04, test 0x08, start (decision /
 * view) 0x10, L / R selection 0x20 / 0x40, bits 7 and 8 the step-lock motor's status (active high); ADC 0 = the steps' swing, ADC 1 =
 * their edge (0x080..0x380, centre 0x200).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "a2_lifted.h"
#include "lift_rt.h"

bool a2_c25_exec(c71_t *d, int pc);          /* src/a2_interp.c */

static const eng_rom_t k_roms[] = {
    {"alpiner2_defaults.nv", 0x2000},
    {"ars1ccrh.1d", 0x80000}, {"ars1ccrl.3d", 0x200000},
    {"ars1cg0.8d", 0x200000}, {"ars1cg1.10d", 0x200000}, {"ars1cg2.12d", 0x200000}, {"ars1cg3.13d", 0x200000},
    {"ars1cg4.14d", 0x200000}, {"ars1cg5.16d", 0x200000},
    {"ars2data.8k", 0x80000},
    {"ars1ptrl0.18k", 0x80000}, {"ars1ptrl1.16k", 0x80000}, {"ars1ptrl2.15k", 0x80000}, {"ars1ptrl3.14k", 0x80000},
    {"ars1ptrm0.18j", 0x80000}, {"ars1ptrm1.16j", 0x80000}, {"ars1ptrm2.15j", 0x80000}, {"ars1ptrm3.14j", 0x80000},
    {"ars1ptru0.18f", 0x80000}, {"ars1ptru1.16f", 0x80000}, {"ars1ptru2.15f", 0x80000}, {"ars1ptru3.14f", 0x80000},
    {"ars1scg0.12f", 0x200000},
    {"ars1wavea.2l", 0x400000}, {"ars2waveb.1l", 0x400000},
    {"ars2vera.2", 0x200000}, {"ars2vera.3", 0x200000}, {"ars2vera.4", 0x200000}, {"ars2vera.5", 0x200000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

static bool a2_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: ars2vera.5/.4/.3/.2 -> byte 0/1/2/3 of every long (the chips' second halves are empty) */
    static const char *lane[4] = { "ars2vera.5", "ars2vera.4", "ars2vera.3", "ars2vera.2" };
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

static bool a2_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/alpiner2_defaults.nv", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[A2] no %s -- EEPROM blank\n", p); return false; }
    size_t n = fread(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f);
    fclose(f);
    return n == SS22_EEPROM_SIZE;
}

static const ss22_board_cfg a2_board = { "A2", 0xFFFF, 0, a2_load_program, a2_load_eeprom, NULL };   /* keycus: random on every unit */

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
    { "Enter, then press the new key (Esc cancels)", "", "Pad: stick swings the steps" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
};

static const ss22_press_name presses[] = { {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST}, {"left", IN_LEFT}, {"right", IN_RIGHT} };

static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)pedal1; (void)pedal2;
    /* A2_AUTO picks the test player: 0 wiggles (default), 1 skis straight, 2 / 3 press Select right / left before each Decision,
     * 4 never coins up (the attract loop), 5 wiggles gently, 6 goes into the test menu and steps through it */
    static int v = -1; if (v < 0) { const char *e = getenv("A2_AUTO"); v = e ? atoi(e) : 0; }
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

/* THE STEP-LOCK MOTOR (MAME alpine_state::alpine_mcu_port4_w / alpine_motor_r), as Alpine Racer: the MCU's outputs 1 and 2 drive it:
 * both on = free the steps, output 2 alone = lock them; the move takes 0.5 s, and INPUTS bits 7 and 8 (active high) report "free" and
 * "locked". Status: 2 locked, 1 free, 0 moving. Without it the boot test stops at MOTOR ERROR. */
static int motor_status = 2, motor_next, motor_timer;
uint8_t g_lift_cov[0x200000];                /* gen/: which instructions have run (LIFT_LAST_PC) */
uint32_t g_lift_pc;                          /* gen/: the instruction last started (LIFT_LAST_PC) */
static void a2_frame(long n)
{
    { static int on = -1; if (on < 0) on = getenv("A2_PCLOG") != NULL;      /* A2_PCLOG=1: where the 68K is at each frame's end */
      if (on && n % 20 == 0) fprintf(stderr, "[A2] f%ld pc %06X  D0 %08X A0 %08X SP %08X\n", n, (unsigned)g_lift_pc, (unsigned)RG4(0x00), (unsigned)RG4(0x20), (unsigned)RG4(0x3C)); }
    { static long c0 = -2, c1; if (c0 == -2) { c0 = -1; const char *e = getenv("A2_COV"); if (e) sscanf(e, "%ld:%ld", &c0, &c1); }   /* A2_COV=from:to -> a2_cov.txt: every instruction run in those frames */
      if (c0 >= 0 && n == c0) memset(g_lift_cov, 0, sizeof g_lift_cov);
      if (c0 >= 0 && n == c1) { FILE *f = fopen("a2_cov.txt", "w"); if (f) { for (unsigned a = 0; a < sizeof g_lift_cov; a++) if (g_lift_cov[a]) fprintf(f, "%X\n", a << 1); fclose(f); } } }
    const unsigned out = ss22_snd_outputs();
    if (motor_timer > 0 && --motor_timer == 0) motor_status = motor_next;
    if ((out & 6) == 6) { if (motor_status == 2) { motor_status = 0; motor_next = 1; motor_timer = 30; } }
    else if (out & 4)   { if (motor_status == 1) { motor_status = 0; motor_next = 2; motor_timer = 30; } }
}
static uint16_t a2_inputs(uint16_t in)
{
    return (uint16_t)((in & ~0x0180u) | (motor_status & 1 ? 0x0080u : 0) | (motor_status & 2 ? 0x0100u : 0));
}

/* the steps' EDGE (ADC 1) follows the swing, as Alpine Racer (one stick or pair of keys drives both).
 * A2_EDGE=off holds the edge at centre, A2_EDGE=reverse leans it the other way. */
static void a2_adc(uint16_t *adc)
{
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("A2_EDGE"); mode = !e ? 1 : !strcmp(e, "off") ? 0 : !strcmp(e, "reverse") ? 2 : 1; }
    /* the steps cannot move while the motor holds them (status 2 locked, 0 moving): AR2 checks for that and stops at MOTOR ERROR2 */
    if (motor_status != 1) adc[0] = 0x200;
    adc[1] = mode == 0 ? 0x200 : mode == 2 ? (uint16_t)(0x400 - adc[0]) : adc[0];
}

/* THE CABINET LINK (2 cabinets; engine/lan139.c). The driver (0x60FC init, 0x616C the receive interrupt -- level 4 -- 0x6270 the
 * per-frame send) is point to point: each cabinet sends its own packet [length][0xA5][bytes][0x100 | count] and keeps the frames it
 * receives (up to 8) for the game; nothing is forwarded. The game's packet (0x2EA54, 0x66 bytes) carries 0x17300000 + a counter << 4
 * + its ID at +0x10; the receiver (0x2EDB4, state word 0xE1867E) rejects its own echo and one with the same ID (state 7). The ID is
 * the operator setting at +0x32 of the settings (RAM 0xE186B8, the EEPROM's layout): EEPROM byte 0x32. */
static uint16_t w16(uint32_t a) { return (uint16_t)rr_read(a, 2); }
static int  a2_link_game_id(void) { return (int)(rr_read(0xE186EA, 1) & 3); }
static bool a2_link_good(void)                   /* frames keep arriving: the game's "nothing received" count stays still */
{
    static uint16_t last; static uint32_t quiet;
    const uint16_t n = w16(0xE18668);
    if (n != last) { last = n; quiet++; } else quiet = 0;
    return quiet < 30;
}
static void a2_link_report(char *buf, int n)
{
    snprintf(buf, (size_t)n, "game: cabinet %u, state %u, no data %u, bad sum %u, no end %u, no A5 %u", (unsigned)(rr_read(0xE186EA, 1) & 3) + 1,
             w16(0xE1867E), w16(0xE18668), (unsigned)rr_read(0xE1866C, 4), (unsigned)rr_read(0xE18660, 4), (unsigned)rr_read(0xE18664, 4));
}

static const ss22_game game = {
    .name = "Alpine Racer 2", .tag = "A2", .lname = "a2", .logname = "alpinr2.log", .zip = "alpinr2a.zip",
    .more_zips = (const char *const[]){ "alpinr2b.zip", NULL },      /* a split MAME set keeps the shared chips in the parent (Ver.B) zip */
    .board = &a2_board,
    .dsp = { { {"ars1ptrl0.18k", "ars1ptrl1.16k", "ars1ptrl2.15k", "ars1ptrl3.14k"},
               {"ars1ptrm0.18j", "ars1ptrm1.16j", "ars1ptrm2.15j", "ars1ptrm3.14j"},
               {"ars1ptru0.18f", "ars1ptru1.16f", "ars1ptru2.15f", "ars1ptru3.14f"} }, 4, a2_c25_exec },
    /* MAME's c352 region: ars1wavea.2l at 0, ars2waveb.1l at 0x800000. INPUTS idle: everything released, the motor "locked" */
    .snd = { "ars2data.8k", { "ars1wavea.2l", "ars2waveb.1l" }, { 0, 0x800000 }, 0x1000000, 0xFFFF, { 1, 3 } },
    .video = { { "ars1cg0.8d", "ars1cg1.10d", "ars1cg2.12d", "ars1cg3.13d", "ars1cg4.14d", "ars1cg5.16d", NULL, NULL },
               "ars1ccrl.3d", "ars1ccrh.1d", { "ars1scg0.12f" }, 1, 0x1000000, 0xFF },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_8B4,
    .polls_per_frame = 66772,           /* Tokyo Wars' figure (same CPU, same clock): not measured for this game */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x100, 0x100 },
    .link_id_byte = 0x32,
    .link_game_id = a2_link_game_id, .link_report = a2_link_report, .link_good = a2_link_good,
    .frame = a2_frame,
};

extern int g_eng_tex_smooth_default;           /* engine/tex_bake.c */
int main(int argc, char **argv) {
    g_eng_tex_smooth_default = 1;               /* 3D texture filter Bilinear by default: the cabinet's snow is smooth (Display page; texture_filter in the cfg) */
    g_ss22_inputs_hook = a2_inputs; g_ss22_adc_hook = a2_adc; return ss22_main(argc, argv, &game); }
