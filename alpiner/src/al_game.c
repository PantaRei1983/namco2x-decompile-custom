/*
 * al_game.c -- Alpine Racer (Namco 1994, Super System 22) as a table for the shared Super 22 host. Any of MAME's three sets: `alpinerd`
 * (World AR2 Ver.D, the parent), `alpinerc` (World AR2 Ver.C) or `alpinerjc` (Japan AR1 Ver.C) -- the program is translated from the
 * one you have (tools/setup_roms.py, tools/import.sh)
 * (engine/ss22_run.c ...). A FEASIBILITY TEST: can a game the original project does not have be brought up on its engine?
 *
 * The program is gen/al_lifted_NN.c, made by tools/gen/lift68k.py from Ghidra's p-code (tools/gen/ghidra). The master DSP and the
 * sound MCU are not translated here: src/al_interp.c runs them one instruction at a time through the engine's own instruction
 * routines (engine/c25/c25_sem.h, engine/snd/m377_sem.h).
 *
 * MAME (namco/namcos22.cpp): machine `alpine` = namcos22s + the step-lock motor; keycus: unit 1 answers 0x0187; INPUTS: coin 0x01,
 * service 0x04, test 0x08, start (decision / view) 0x10, L / R selection 0x20 / 0x40, bits 7 and 8 the step-lock motor's status
 * (active high); ADC 0 = the steps' swing, ADC 1 = their edge (0x080..0x380, centre 0x200).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "al_lifted.h"
#include "lift_rt.h"

bool al_c25_exec(c71_t *d, int pc);          /* src/al_interp.c */

static const eng_rom_t k_roms[] = {
    {"alpiner_defaults.nv", 0x2000},
    {"ar1ccrh.1d", 0x80000}, {"ar1ccrl.3d", 0x200000},
    {"ar1cg0.8d", 0x200000}, {"ar1cg1.10d", 0x200000}, {"ar1cg2.12d", 0x200000}, {"ar1cg3.13d", 0x200000},
    {"ar1cg4.14d", 0x200000}, {"ar1cg5.16d", 0x200000}, {"ar1cg6.18d", 0x200000}, {"ar1cg7.19d", 0x200000},
    {"ar1datab.8k", 0x80000},
    {"ar1ptrl0.18k", 0x80000}, {"ar1ptrl1.16k", 0x80000}, {"ar1ptrl2.15k", 0x80000}, {"ar1ptrl3.14k", 0x80000},
    {"ar1ptrm0.18j", 0x80000}, {"ar1ptrm1.16j", 0x80000}, {"ar1ptrm2.15j", 0x80000}, {"ar1ptrm3.14j", 0x80000},
    {"ar1ptru0.18f", 0x80000}, {"ar1ptru1.16f", 0x80000}, {"ar1ptru2.15f", 0x80000}, {"ar1ptru3.14f", 0x80000},
    {"ar1scg0.12f", 0x200000}, {"ar1scg1.10f", 0x200000},
    {"ar1wavea.2l", 0x200000},
    /* + the program chips of one set (al_pick_set): the game's own first-run unpack (engine/romzip.c) takes them from the zip too */
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))
static eng_rom_t g_roms[NROMS + 4];
static int g_nroms;

/* The set the build translated: extracted/SET, written by tools/setup_roms.py (beside the game, or one folder up when the .exe is
 * started from build/); alpinerd, MAME's parent, when there is none. Its program chips join the list the engine checks / unpacks. */
static void al_pick_set(void)
{
    static const eng_rom_t prog_d[] = { {"ar2verd.1", 0x100000}, {"ar2verd.2", 0x100000}, {"ar2verd.3", 0x100000}, {"ar2verd.4", 0x100000} };
    static const eng_rom_t prog_c[] = { {"ar2verc.1", 0x100000}, {"ar2verc.2", 0x100000}, {"ar2verc.3", 0x100000}, {"ar2verc.4", 0x100000} };
    static const eng_rom_t prog_jc[] = { {"ar1verc.1", 0x200000}, {"ar1verc.2", 0x200000} };
    char set[32] = "alpinerd";
    static const char *const where[] = { "extracted/SET", "../extracted/SET", "roms/SET" };
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(where[i], "r");
        if (!f) continue;
        if (fscanf(f, "%31s", set) != 1) strcpy(set, "alpinerd");
        fclose(f);
        break;
    }
    const eng_rom_t *p = prog_d; int np = 4;
    if (!strcmp(set, "alpinerc")) p = prog_c;
    else if (!strcmp(set, "alpinerjc")) { p = prog_jc; np = 2; }
    memcpy(g_roms, k_roms, sizeof k_roms);
    memcpy(g_roms + NROMS, p, (size_t)np * sizeof *p);
    g_nroms = NROMS + np;
}

static bool al_read(const char *dir, const char *name, uint8_t *buf, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    const size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}

/* The program, whichever set extracted/ holds (the build translated that one):
 *   alpinerd / alpinerc: ROM_LOAD32_BYTE ar2ver?.1 .. .4, chip .1 byte 3 of every long ... chip .4 byte 0
 *   alpinerjc: ROM_LOAD32_WORD_SWAP, ar1verc.2 the high word of every long, ar1verc.1 the low word, each word stored low byte first */
static bool al_load_program(const char *dir)
{
    static uint8_t chip[0x200000];
    static const char *const bytesets[2][4] = { { "ar2verd.1", "ar2verd.2", "ar2verd.3", "ar2verd.4" },
                                                { "ar2verc.1", "ar2verc.2", "ar2verc.3", "ar2verc.4" } };
    for (int s = 0; s < 2; s++) {
        int k;
        for (k = 0; k < 4; k++) {
            if (!al_read(dir, bytesets[s][k], chip, 0x100000)) break;
            for (uint32_t i = 0; i < 0x100000; i++) g_ss22.rom[i * 4 + (uint32_t)(3 - k)] = chip[i];
        }
        if (k == 4) { fprintf(stderr, "[AL] program: %s\n", s ? "alpinerc (AR2 Ver.C)" : "alpinerd (AR2 Ver.D)"); return true; }
        if (k) { fprintf(stderr, "[AL] %s/%s missing\n", dir, bytesets[s][k]); return false; }
    }
    static const char *half[2] = { "ar1verc.2", "ar1verc.1" };
    for (int h = 0; h < 2; h++) {
        if (!al_read(dir, half[h], chip, 0x200000)) {
            fprintf(stderr, "[AL] no program in %s: ar2verd.1-4, ar2verc.1-4 or ar1verc.1-2 (run tools/setup_roms.py)\n", dir);
            return false;
        }
        for (uint32_t i = 0; i < SS22_ROM_SIZE / 4; i++) {
            g_ss22.rom[i * 4 + (uint32_t)h * 2] = chip[i * 2 + 1];
            g_ss22.rom[i * 4 + (uint32_t)h * 2 + 1] = chip[i * 2];
        }
    }
    fprintf(stderr, "[AL] program: alpinerjc (AR1 Ver.C)\n");
    return true;
}

static bool al_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/alpiner_defaults.nv", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[AL] no %s -- EEPROM blank\n", p); return false; }
    size_t n = fread(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f);
    fclose(f);
    return n == SS22_EEPROM_SIZE;
}

static const ss22_board_cfg al_board = { "AL", 1, 0x0187, al_load_program, al_load_eeprom };      /* keycus: unit 1 answers 0x0187 (NAMCOS22_ALPINE_RACER) */

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
    /* AL_AUTO picks the test player: 0 wiggles (default), 1 skis straight, 2 / 3 press Select right / left before each Decision,
     * 4 never coins up (the attract loop), 5 wiggles gently, 6 goes into the test menu and steps through it */
    static int v = -1; if (v < 0) { const char *e = getenv("AL_AUTO"); v = e ? atoi(e) : 0; }
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

/* THE STEP-LOCK MOTOR (MAME alpine_state::alpine_mcu_port4_w / alpine_motor_r): the cabinet locks the ski steps until a game starts.
 * The MCU's outputs 1 and 2 drive it: both on = free the steps, output 2 alone = lock them; the move takes 0.5 s, and INPUTS bits 7
 * and 8 (active high) report "free" and "locked". Status: 2 locked, 1 free, 0 moving. Without it the boot test stops at MOTOR ERROR. */
static int motor_status = 2, motor_next, motor_timer;
uint8_t g_lift_cov[0x200000];                /* gen/: which instructions have run (LIFT_LAST_PC) */
uint32_t g_lift_pc;                          /* gen/: the instruction last started (LIFT_LAST_PC) */
static void al_frame(long n)
{
    { static int on = -1; if (on < 0) on = getenv("AL_PCLOG") != NULL;      /* AL_PCLOG=1: where the 68K is at each frame's end */
      if (on && n % 20 == 0) fprintf(stderr, "[AL] f%ld pc %06X  D0 %08X A0 %08X SP %08X\n", n, (unsigned)g_lift_pc, (unsigned)RG4(0x00), (unsigned)RG4(0x20), (unsigned)RG4(0x3C)); }
    { static long c0 = -2, c1; if (c0 == -2) { c0 = -1; const char *e = getenv("AL_COV"); if (e) sscanf(e, "%ld:%ld", &c0, &c1); }   /* AL_COV=from:to -> al_cov.txt: every instruction run in those frames */
      if (c0 >= 0 && n == c0) memset(g_lift_cov, 0, sizeof g_lift_cov);
      if (c0 >= 0 && n == c1) { FILE *f = fopen("al_cov.txt", "w"); if (f) { for (unsigned a = 0; a < sizeof g_lift_cov; a++) if (g_lift_cov[a]) fprintf(f, "%X\n", a << 1); fclose(f); } } }
    const unsigned out = ss22_snd_outputs();
    if (motor_timer > 0 && --motor_timer == 0) motor_status = motor_next;
    if ((out & 6) == 6) { if (motor_status == 2) { motor_status = 0; motor_next = 1; motor_timer = 30; } }
    else if (out & 4)   { if (motor_status == 1) { motor_status = 0; motor_next = 2; motor_timer = 30; } }
}
static uint16_t al_inputs(uint16_t in)
{
    return (uint16_t)((in & ~0x0180u) | (motor_status & 1 ? 0x0080u : 0) | (motor_status & 2 ? 0x0100u : 0));
}

/* the steps' EDGE (ADC 1) rests at centre; the engine's second axis is a pedal that rests at 0. On the cabinet a skier swings and
 * edges together, so one stick or pair of keys drives both: the edge follows the swing (same range, same centre).
 * AL_EDGE=off holds the edge at centre, AL_EDGE=reverse leans it the other way. */
static void al_adc(uint16_t *adc)
{
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("AL_EDGE"); mode = !e ? 1 : !strcmp(e, "off") ? 0 : !strcmp(e, "reverse") ? 2 : 1; }
    adc[1] = mode == 0 ? 0x200 : mode == 2 ? (uint16_t)(0x400 - adc[0]) : adc[0];
}

/* the reset vector, from whichever program the build translated (0x89FC in all three sets): rr_jump finds its function */
void rr_jump(uint32_t t, uint32_t at);
static void al_entry(void)
{
    const uint8_t *r = g_ss22.rom;
    rr_jump(((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | r[7], 0);
}

static ss22_game game = {
    .name = "Alpine Racer", .tag = "AL", .lname = "al", .logname = "alpiner.log", .zip = "alpinerd.zip",
    .board = &al_board,
    .dsp = { { {"ar1ptrl0.18k", "ar1ptrl1.16k", "ar1ptrl2.15k", "ar1ptrl3.14k"},
               {"ar1ptrm0.18j", "ar1ptrm1.16j", "ar1ptrm2.15j", "ar1ptrm3.14j"},
               {"ar1ptru0.18f", "ar1ptru1.16f", "ar1ptru2.15f", "ar1ptru3.14f"} }, 4, al_c25_exec },
    /* INPUTS idle: everything released (active low), and the step-lock motor's status = 2, "locked" (bit 7 clear, bit 8 set, active high) */
    .snd = { "ar1datab.8k", { "ar1wavea.2l", NULL }, { 0, 0 }, 0x400000, 0xFFFF, { 1, 3 } },
    .video = { { "ar1cg0.8d", "ar1cg1.10d", "ar1cg2.12d", "ar1cg3.13d", "ar1cg4.14d", "ar1cg5.16d", "ar1cg6.18d", "ar1cg7.19d" },
               "ar1ccrl.3d", "ar1ccrh.1d", { "ar1scg0.12f", "ar1scg1.10f" }, 2, 2 * 0x200000, 0xFF },
    .input = &input,
    .roms = g_roms,                     /* .n_roms: al_pick_set */
    .entry = al_entry,
    .polls_per_frame = 66772,           /* Tokyo Wars' figure (same CPU, same clock): not measured for this game */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x100, 0x100 },
    .link_id_byte = -1,
    .frame = al_frame,
};

int main(int argc, char **argv)
{
    al_pick_set();
    game.n_roms = g_nroms;
    game.zip = !strcmp(g_roms[NROMS].name, "ar2verc.1") ? "alpinerc.zip" : !strcmp(g_roms[NROMS].name, "ar1verc.1") ? "alpinerjc.zip" : "alpinerd.zip";
    g_ss22_inputs_hook = al_inputs; g_ss22_adc_hook = al_adc;
    return ss22_main(argc, argv, &game);
}
