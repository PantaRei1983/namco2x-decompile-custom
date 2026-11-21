/*
 * ac_game.c -- Air Combat 22 (Namco 1995, Super System 22, MAME `airco22b`, Japan ACS1 Ver.B) as a table for the shared Super 22 host
 * (engine/ss22_run.c ...), made the same way as Alpine Surfer (alpines/) and Alpine Racer 2 (alpinr2/).
 *
 * The program is gen/ac_lifted_NN.c, made by tools/gen/lift68k.py from Ghidra's p-code (tools/import.sh, tools/regen.sh). The master
 * DSP and the sound MCU are interpreted (src/ac_interp.c, src/ac_snd_interp.c).
 *
 * MAME (namco/namcos22.cpp): machine `airco22b` = namcos22s + a "bodysonic" speaker (C352 channel 2 to a subwoofer behind the seat);
 * init_airco22: NAMCOS22_AIR_COMBAT22, NO KEYCUS (every read answers 0); S22-BIOS 1.20. Inputs `airco22`: coin 0x01, service 0x04,
 * test 0x08, missile 0x10, gun trigger 0x20, start (also the view change) 0x40; ADC 0 / 1 the stick (X / Y, 0x100..0x300), ADC 2 the
 * throttle stick (springs back to centre, 0x100..0x300). No default EEPROM in the set (the game sets a blank one up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "ac_lifted.h"
#include "lift_rt.h"

bool ac_c25_exec(c71_t *d, int pc);          /* src/ac_interp.c */

static const eng_rom_t k_roms[] = {
    {"acs1ccrh.1d", 0x80000}, {"acs1ccrl.3d", 0x200000},
    {"acs1cg0.8d", 0x200000}, {"acs1cg1.10d", 0x200000}, {"acs1cg2.12d", 0x200000}, {"acs1cg3.13d", 0x200000},
    {"acs1cg4.14d", 0x200000}, {"acs1cg5.16d", 0x200000}, {"acs1cg6.18d", 0x200000}, {"acs1cg7.19d", 0x200000},
    {"acs1data.8k", 0x80000},
    {"acs1ptl0.18k", 0x80000}, {"acs1ptl1.16k", 0x80000}, {"acs1ptl2.15k", 0x80000}, {"acs1ptl3.14k", 0x80000},
    {"acs1ptm0.18j", 0x80000}, {"acs1ptm1.16j", 0x80000}, {"acs1ptm2.15j", 0x80000}, {"acs1ptm3.14j", 0x80000},
    {"acs1ptu0.18f", 0x80000}, {"acs1ptu1.16f", 0x80000}, {"acs1ptu2.15f", 0x80000}, {"acs1ptu3.14f", 0x80000},
    {"acs1scg0.12f", 0x200000}, {"acs1scg1.10f", 0x200000},
    {"acs1wav0.1", 0x400000}, {"acs1wav1.2", 0x400000},
    {"acs1verb.1", 0x100000}, {"acs1verb.2", 0x100000}, {"acs1verb.3", 0x100000}, {"acs1verb.4", 0x100000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

static bool ac_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: acs1verb.4 / .3 / .2 / .1 -> byte 0/1/2/3 of every long, 1 MB each */
    static const char *lane[4] = { "acs1verb.4", "acs1verb.3", "acs1verb.2", "acs1verb.1" };
    for (int l = 0; l < 4; l++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, lane[l]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < 0x100000; i++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            g_ss22.rom[i * 4 + (uint32_t)l] = (uint8_t)c;
        }
        fclose(f);
    }
    return true;
}

static bool ac_load_eeprom(const char *dir) { (void)dir; return false; }   /* not in MAME's set: the game sets a blank EEPROM up itself */

static const ss22_board_cfg ac_board = { "AC", 0, 0, ac_load_program, ac_load_eeprom, NULL, NULL, true };   /* no keycus: every read 0 */

#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_MISSILE 0x0010
#define IN_GUN     0x0020
#define IN_START   0x0040
/* Pitch and the throttle swing both ways, which the engine's key "pedals" (0..max) cannot: their rows press these spare
 * INPUTS bits, which ac_send turns into the stick's Y (ADC 1) and the throttle (ADC 2) and clears before the board sees them.
 * The throttle lever springs back to the middle (MAME); forward (A-D up) = afterburner, back = air brake. X is the wheel axis. */
#define IN_V_PITCH_UP   0x1000
#define IN_V_PITCH_DOWN 0x2000
#define IN_V_THR_FWD    0x4000
#define IN_V_THR_BACK   0x8000
#define IN_VIRTUAL      0xF000
static const ss22_action actions[] = {
    { "Insert coin",      "coin",      SDL_SCANCODE_5,      SDL_SCANCODE_6,        IN_COIN,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start / view",     "start",     SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, IN_START,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_Y) },
    { "Gun trigger",      "gun",       SDL_SCANCODE_SPACE,  SDL_SCANCODE_LCTRL,    IN_GUN,     0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Missile",          "missile",   SDL_SCANCODE_LALT,   SDL_SCANCODE_X,        IN_MISSILE, 0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
    { "Service",          "service",   SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN,  IN_SERVICE, 0,                   0 },
    { "Test",             "test",      SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN,  IN_TEST,    0,                   0 },
    { "Stick left",       "left",      SDL_SCANCODE_LEFT,   SDL_SCANCODE_UNKNOWN,  0,          SS22_AX_WHEEL_LEFT,  0 },
    { "Stick right",      "right",     SDL_SCANCODE_RIGHT,  SDL_SCANCODE_UNKNOWN,  0,          SS22_AX_WHEEL_RIGHT, 0 },
    { "Nose down (stick fwd)", "pitchdown", SDL_SCANCODE_UP, SDL_SCANCODE_UNKNOWN, IN_V_PITCH_DOWN, 0,              0 },
    { "Nose up (stick back)",      "pitchup",   SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN, IN_V_PITCH_UP, 0,              0 },
    { "Throttle down (brake)", "accel", SDL_SCANCODE_W, SDL_SCANCODE_LSHIFT, IN_V_THR_FWD,  0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },   /* the user: pad accelerate / slow down swapped (2026-11-19) */
    { "Throttle up (boost)",        "brake", SDL_SCANCODE_S, SDL_SCANCODE_Z,      IN_V_THR_BACK, 0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
};
static void ac_send(uint16_t p, unsigned wheel, unsigned pedal1, unsigned pedal2);
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x100, 0x300, 0x100, 12,
    { 0x100, 0x100 }, 16,
    { "Enter, then press the new key (Esc cancels)", "The throttle springs back to the middle, like the cabinet's lever",
      "Pad: left stick flies; right trigger / RB afterburner, left trigger / LB brake (or right stick); A gun, B missile" },
    ac_send,
    IN_TEST, IN_SERVICE,
};

static const ss22_press_name presses[] = { {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST},
                                           {"gun", IN_GUN}, {"missile", IN_MISSILE} };

static unsigned clamp_adc(int v) { return (unsigned)(v < 0x100 ? 0x100 : v > 0x300 ? 0x300 : v); }
static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    /* AC_AUTO picks the test player: 0 flies about and fires (default), 4 never coins up (the attract loop), 6 goes into the test menu.
     * The stick's Y and the throttle travel as the "pedals" (ADC 1, 2), so a recording keeps them */
    static int v = -1; if (v < 0) { const char *e = getenv("AC_AUTO"); v = e ? atoi(e) : 0; }
    int auto_pitch = 0, auto_throttle = 0;
    *wheel = 0x200; *pedal1 = *pedal2 = 0x200;
    if (v == 4) {   /* AC_STICK=F+LEN:X,Y;...: hold the stick (and X = throttle when written T) from frame F for LEN frames (scripts the test menu) */
        static const char *sc; static int got; if (!got) { got = 1; sc = getenv("AC_STICK"); }
        for (const char *q = sc; q && *q; ) { long f, len; int x, y, k = 0;
            if (sscanf(q, "%ld+%ld:%i,%i%n", &f, &len, &x, &y, &k) == 4 && n >= f && n < f + len) { *wheel = (unsigned)x; *pedal1 = (unsigned)y; }
            const char *nx = strchr(q, ';'); q = nx ? nx + 1 : NULL; }
        return;
    }
    if (v == 6) {
        if (n > 300 && n < 9000) *p |= IN_TEST;
        if (n % 480 >= 300 && n % 480 < 306) *p |= IN_START;
        if (n % 1200 >= 700 && n % 1200 < 706) *p |= IN_SERVICE;
        return;
    }
    if (n % 600 >= 300 && n % 600 < 306) *p |= IN_COIN;
    if (n % 300 >= 150 && n % 300 < 156 && n > 400) *p |= IN_START;
    if (n % 40 < 20) *p |= IN_GUN;
    if (n % 400 < 10) *p |= IN_MISSILE;
    *wheel = ((n / 120) % 2 == 0) ? 0x180 : 0x280;
    auto_pitch = ((n / 200) % 2 == 0) ? 0x60 : -0x60;
    auto_throttle = 0x80;
    *pedal1 = clamp_adc(0x200 + auto_pitch); *pedal2 = clamp_adc(0x200 + auto_throttle);
    { const char *e = getenv("AC_THROTTLE"); if (e) *pedal2 = (unsigned)strtol(e, NULL, 0); }   /* TEST: hold the throttle at this A-D value */
}

uint8_t g_lift_cov[0x200000];                /* gen/: which instructions have run (LIFT_LAST_PC) */
#ifdef RR_TRACE
void (*rr_read_probe)(uint32_t, int);          /* (the trace build links lift_env.c's weak hook: Windows needs a definition) */
#endif
uint32_t g_lift_pc;                          /* gen/: the instruction last started (LIFT_LAST_PC) */
static void ac_frame(long n)
{
    { static long pf = -2; static unsigned pa, pv; if (pf == -2) { pf = -1; const char *e = getenv("AC_POKE"); if (e) sscanf(e, "%ld:%x:%x", &pf, &pa, &pv); }
      if (pf >= 0 && n == pf) rr_write(pa, 4, pv); }   /* TEST AC_POKE=frame:addr:value -- a long into memory (e.g. a bad routine pointer: the crash path) */
    { static int on = -1; if (on < 0) on = getenv("AC_ADCLOG") != NULL;  /* AC_ADCLOG=1: the stick words the I/O chip leaves (0xA0BD0A..) and the game's centres */
      if (on && n % 10 == 0) fprintf(stderr, "[ADC] f%ld raw %04X %04X %04X %04X centre %04X %04X %04X\n", n,
          (unsigned)rr_read(0xA0BD0A, 2), (unsigned)rr_read(0xA0BD0C, 2), (unsigned)rr_read(0xA0BD0E, 2), (unsigned)rr_read(0xA0BD10, 2),
          (unsigned)rr_read(0xE08000 - 0x79EC, 2), (unsigned)rr_read(0xE08000 - 0x79EA, 2), (unsigned)rr_read(0xE08000 - 0x79E8, 2)); }
    { static int on = -1; if (on < 0) on = getenv("AC_PCLOG") != NULL;      /* AC_PCLOG=1: where the 68K is at each frame's end */
      if (on && n % 20 == 0) fprintf(stderr, "[AC] f%ld pc %06X  D0 %08X A0 %08X SP %08X\n", n, (unsigned)g_lift_pc, (unsigned)RG4(0x00), (unsigned)RG4(0x20), (unsigned)RG4(0x3C)); }
    { static long c0 = -2, c1; if (c0 == -2) { c0 = -1; const char *e = getenv("AC_COV"); if (e) sscanf(e, "%ld:%ld", &c0, &c1); }   /* AC_COV=from:to -> ac_cov.txt */
      if (c0 >= 0 && n == c0) memset(g_lift_cov, 0, sizeof g_lift_cov);
      if (c0 >= 0 && n == c1) { FILE *f = fopen("ac_cov.txt", "w"); if (f) { for (unsigned a = 0; a < sizeof g_lift_cov; a++) if (g_lift_cov[a]) fprintf(f, "%X\n", a << 1); fclose(f); } } }
}

/* THE STICK'S Y (ADC 1) AND THE THROTTLE (ADC 2): up / down arrows pitch, W / S throttle (it springs back to the middle, as the
 * cabinet's); a pad's left stick Y and right stick Y. X (ADC 0) is the engine's wheel axis: left / right arrows, the pad's left stick. */
static SDL_GameController *ac_pad;
static int ac_axis(SDL_GameControllerAxis ax)
{
    if (!ac_pad || !SDL_GameControllerGetAttached(ac_pad)) {
        ac_pad = NULL;
        for (int i = 0; i < SDL_NumJoysticks() && !ac_pad; i++) if (SDL_IsGameController(i)) ac_pad = SDL_GameControllerOpen(i);
        if (!ac_pad) return 0;
    }
    const int v = SDL_GameControllerGetAxis(ac_pad, ax);
    return v > -6000 && v < 6000 ? 0 : v;
}
/* the window's controls -> the board: Y and the throttle as the two "pedals" (ADC 1, 2: .snd adc_pedal), so the session recording
 * (and an F9 capture's replay) keeps them -- an A-D hook after the recording made replays drift */
static void ac_send(uint16_t p, unsigned wheel, unsigned pedal1, unsigned pedal2)
{
    (void)pedal1; (void)pedal2;
    int y = 0, t = 0;
    if (p & IN_V_PITCH_DOWN) y -= 0x100;
    if (p & IN_V_PITCH_UP) y += 0x100;
    if (p & IN_V_THR_FWD) t += 0x100;
    if (p & IN_V_THR_BACK) t -= 0x100;
    y += ac_axis(SDL_CONTROLLER_AXIS_LEFTY) / 128;
    t -= ac_axis(SDL_CONTROLLER_AXIS_RIGHTY) / 128;
    /* LT throttle up, RT down: as LB / RB (the user, 2026-11-19) -- unless the trigger is bound on the Controls page, where
     * the binding alone counts (the user's RT = up was cancelled by this reading RT as down) */
    if (!ss22_input_trigger_bound(false)) t += ac_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128;
    if (!ss22_input_trigger_bound(true)) t -= ac_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128;
    ss22_snd_inputs((uint16_t)(p & ~IN_VIRTUAL), wheel, clamp_adc(0x200 + y), clamp_adc(0x200 + t));
}

/* The 2D effects of the sprite layer, by the tile each sprite draws (listed from the attract's kills against the sprite ROM's pictures): 2106..2462
 * the explosion (a white flash that turns into an orange fireball), 2551..2911 the black smoke a falling plane trails, 3262..3390 the smoke puffs of a
 * hit; the rest of 2100..3583 is the same kind of art (more smoke and cloud frames). 2048..2099 are pictures, from 3584 the HUD's symbols and the map.
 * The Display menu's "Effects" option smooths exactly these -- and, as they are drawn close to their own size (where bilinear sampling changes
 * little), also softens them a little, like Dirt Dash's headlights (.soft). */
static const int fx_tiles[][2] = { { 2100, 3583 } };

static const ss22_game game = {
    .name = "Air Combat 22", .tag = "AC", .lname = "ac", .logname = "airco22.log", .zip = "airco22b.zip",
    .board = &ac_board,
    .dsp = { { {"acs1ptl0.18k", "acs1ptl1.16k", "acs1ptl2.15k", "acs1ptl3.14k"},
               {"acs1ptm0.18j", "acs1ptm1.16j", "acs1ptm2.15j", "acs1ptm3.14j"},
               {"acs1ptu0.18f", "acs1ptu1.16f", "acs1ptu2.15f", "acs1ptu3.14f"} }, 4, ac_c25_exec },
    /* MAME's c352 region: acs1wav0.1 at 0x400000, acs1wav1.2 at 0x800000. ADC 1 and 2 = the "pedals": the stick's Y and the throttle (ac_send, autoplay) */
    .snd = { "acs1data.8k", { "acs1wav0.1", "acs1wav1.2" }, { 0x400000, 0x800000 }, 0x1000000, 0xFFFF, { 1, 2 } },
    .video = { { "acs1cg0.8d", "acs1cg1.10d", "acs1cg2.12d", "acs1cg3.13d", "acs1cg4.14d", "acs1cg5.16d", "acs1cg6.18d", "acs1cg7.19d" },
               "acs1ccrl.3d", "acs1ccrh.1d", { "acs1scg0.12f", "acs1scg1.10f" }, 2, 0x1000000, 0xFF,
               false, false, { 0 }, true,    /* vics_linked: set 1 is set 2's header (engine/ss22_game.h) */
               .fx = fx_tiles, .n_fx = 1, .soft = fx_tiles, .n_soft = 1 },   /* drawn near 1:1: bilinear alone leaves the pixel edge, so softened too */  /* vics_linked: set 1 is set 2's header (engine/ss22_game.h) */                     /* the VICS sprite count is the REGISTER (0x40), not the list: MAME reads it from the list as for Dirt Dash, but Air Combat 22 rotates 4 lists and that word never changes (65 sprites always -- old sprites stuck on screen, the user's captures) */
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_5000,
    .polls_per_frame = 66772,           /* Tokyo Wars' figure (same CPU, same clock): not measured for this game */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x100, 0x100 },
    .link_id_byte = -1,
    .frame = ac_frame,
};

int main(int argc, char **argv) { return ss22_main(argc, argv, &game); }
