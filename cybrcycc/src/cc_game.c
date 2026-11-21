/*
 * cc_game.c -- Cyber Cycles (Namco 1995, Super System 22, MAME `cybrcycc`, World CB2 Ver.C) as a table for the shared Super 22 host
 * (engine/ss22_run.c ...). A FEASIBILITY TEST, made the same way as the Alpine Racer one (branch alpiner-feasibility).
 *
 * The program is gen/cc_lifted_NN.c, made by tools/gen/lift68k.py from Ghidra's p-code (tools/gen/ghidra). The master DSP and the
 * sound MCU are interpreted (src/cc_interp.c, src/cc_snd_interp.c).
 *
 * MAME (namco/namcos22.cpp): machine `cybrcycc` = namcos22s + a "tank" speaker on C352 output 2; keycus: unit 0xF answers 0x0387;
 * INPUTS: coin 0x01, service 0x04, test 0x08, start (also the view change) 0x10; ADC 0 = the bike's bank (0x004..0x3FC, centre 0x200),
 * ADC 1 = the throttle grip (0..0x300), ADC 2 = the brake lever (0..0x100).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "cc_lifted.h"
#include "lift_rt.h"

bool cc_c25_exec(c71_t *d, int pc);          /* src/cc_interp.c */

static const eng_rom_t k_roms[] = {
    {"cybrcycc_defaults.nv", 0x2000},
    {"cb1ccrh.1d", 0x80000}, {"cb1ccrl.3d", 0x200000},
    {"cb1cg0.8d", 0x200000}, {"cb1cg1.10d", 0x200000}, {"cb1cg2.12d", 0x200000}, {"cb1cg3.13d", 0x200000},
    {"cb1cg4.14d", 0x200000}, {"cb1cg5.16d", 0x200000}, {"cb1cg6.18d", 0x200000},
    {"cb1datab.8k", 0x80000},
    {"cb1ptrl0.18k", 0x80000}, {"cb1ptrl1.16k", 0x80000}, {"cb1ptrl2.15k", 0x80000}, {"cb1ptrl3.14k", 0x80000},
    {"cb1ptrm0.18j", 0x80000}, {"cb1ptrm1.16j", 0x80000}, {"cb1ptrm2.15j", 0x80000}, {"cb1ptrm3.14j", 0x80000},
    {"cb1ptru0.18f", 0x80000}, {"cb1ptru1.16f", 0x80000}, {"cb1ptru2.15f", 0x80000}, {"cb1ptru3.14f", 0x80000},
    {"cb1scg0.12f", 0x200000},
    {"cb1wavea.2l", 0x400000}, {"cb1waveb.1l", 0x200000},
    {"cb2verc.1", 0x100000}, {"cb2verc.2", 0x100000}, {"cb2verc.3", 0x100000}, {"cb2verc.4", 0x100000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

static bool cc_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: cb2verc.4/.3/.2/.1 -> byte 0/1/2/3 of every long */
    static const char *lane[4] = { "cb2verc.4", "cb2verc.3", "cb2verc.2", "cb2verc.1" };
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

static bool cc_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/cybrcycc_defaults.nv", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[CC] no %s -- EEPROM blank\n", p); return false; }
    size_t n = fread(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f);
    fclose(f);
    return n == SS22_EEPROM_SIZE;
}

static const ss22_board_cfg cc_board = { "CC", 0xF, 0x0387, cc_load_program, cc_load_eeprom };      /* keycus: unit 0xF answers 0x0387 (NAMCOS22_CYBER_CYCLES) */

#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_START   0x0010
static const ss22_action actions[] = {
    { "Insert coin",     "coin",        SDL_SCANCODE_5,      SDL_SCANCODE_6,        IN_COIN,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start / view",    "start",       SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, IN_START,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A) | SS22_PAD(SDL_CONTROLLER_BUTTON_Y) },
    { "Service",         "service",     SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN,  IN_SERVICE, 0,                   0 },
    { "Test",            "test",        SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN,  IN_TEST,    0,                   0 },
    { "Bank left",       "bank_left",   SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,        0,          SS22_AX_WHEEL_LEFT,  0 },
    { "Bank right",      "bank_right",  SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,        0,          SS22_AX_WHEEL_RIGHT, 0 },
    { "Throttle",        "throttle",    SDL_SCANCODE_UP,     SDL_SCANCODE_W,        0,          SS22_AX_PEDAL1,      0 },
    { "Brake",           "brake",       SDL_SCANCODE_DOWN,   SDL_SCANCODE_S,        0,          SS22_AX_PEDAL2,      0 },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x004, 0x3FC, 0x1F8, 40,               /* a held bank key goes to full bank (MAME PORT_MINMAX 0x004..0x3FC): the real bike drifts at full lean */
    { 0x300, 0x100 }, 40,
    { "Enter, then press the new key (Esc cancels)", "Alternates: 6, keypad Enter, A D W S", "Pad: stick banks, triggers = throttle / brake" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
};

static const ss22_press_name presses[] = { {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST} };

static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)pedal2;
    /* CC_AUTO picks the test player: 0 rides (coin, start, full throttle, gentle weave; default), 4 never coins up (the attract loop),
     * 6 goes into the test menu and steps through it, 9 sits still once started (see below) */
    static int v = -1; if (v < 0) { const char *e = getenv("CC_AUTO"); v = e ? atoi(e) : 0; }
    *wheel = 0x200;
    if (v == 4) return;
    if (v == 8) {                            /* 8: the SWITCH TEST (test + service): bank each way, throttle, brake, then both to leave */
        if (n > 300) *p |= IN_TEST;
        if (n >= 600 && n < 606) *p |= IN_SERVICE;
        if (n >= 900 && n < 1100) *wheel = 0x004;
        if (n >= 1100 && n < 1300) *wheel = 0x3FC;
        if (n >= 1400 && n < 1600) *pedal1 = 0x300;
        if (n >= 1700 && n < 1900) *pedal2 = 0x100;
        if (n >= 2000 && n < 2100) { *pedal1 = 0x300; *pedal2 = 0x100; }
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
    if (v == 9) return;                      /* 9: coins up and starts, then sits still on the grid (no throttle) */
    if (n > 700) *pedal1 = 0x300;
    if (v == 7) {                            /* 7: hard riding -- full bank each way, brake pulses (matched by MAME's play_hard.lua) */
        *wheel = ((n / 120) % 2 == 0) ? 0x004 : 0x3FC;
        if (n > 700 && n % 200 < 30) { *pedal1 = 0; *pedal2 = 0x100; }
        return;
    }
    *wheel = ((n / 90) % 2 == 0) ? 0x180 : 0x280;
}

uint8_t g_lift_cov[0x200000];                /* gen/: which instructions have run (LIFT_LAST_PC) */
uint32_t g_lift_pc;                          /* gen/: the instruction last started (LIFT_LAST_PC) */
static void cc_frame(long n)
{
    { static int pl = -1; if (pl < 0) pl = getenv("CC_PEERLOG") != NULL;   /* CC_PEERLOG=1: the link driver's view of each cabinet, every frame */
      if (pl) {                                                              /* (0xE0A668 + cab * 0x100: +0 frames since its last message, */
          char b[160]; int k = 0;                                            /*  saturating at 0x20 = gone; +4 its game state) */
          for (uint32_t c = 0; c < 4; c++) k += snprintf(b + k, sizeof b - (size_t)k, "  %u: age %2u st %02X", c + 1,
                                                         (unsigned)rr_read(0xE0A668 + c * 0x100, 2), (unsigned)rr_read(0xE0A66C + c * 0x100, 1));
          fprintf(stderr, "[PEER] f%ld me %u st %02X fwdq %u%s\n", n, (unsigned)(rr_read(0xE08400, 2) & 3) + 1, (unsigned)rr_read(0xE0A65C, 2),
                  (unsigned)rr_read(0xE0A634, 2), b);   /* fwdq: messages queued for FUN_00000CB4 (16 = full) */
      } }
    { static int on = -1; if (on < 0) on = getenv("CC_PCLOG") != NULL;      /* CC_PCLOG=1: where the 68K is at each frame's end */
      if (on && n % 20 == 0) fprintf(stderr, "[CC] f%ld pc %06X  D0 %08X A0 %08X SP %08X\n", n, (unsigned)g_lift_pc, (unsigned)RG4(0x00), (unsigned)RG4(0x20), (unsigned)RG4(0x3C)); }
    { static long c0 = -2, c1; if (c0 == -2) { c0 = -1; const char *e = getenv("CC_COV"); if (e) sscanf(e, "%ld:%ld", &c0, &c1); }   /* CC_COV=from:to -> cc_cov.txt: every instruction run in those frames */
      if (c0 >= 0 && n == c0) memset(g_lift_cov, 0, sizeof g_lift_cov);
      if (c0 >= 0 && n == c1) { FILE *f = fopen("cc_cov.txt", "w"); if (f) { for (unsigned a = 0; a < sizeof g_lift_cov; a++) if (g_lift_cov[a]) fprintf(f, "%X\n", a << 1); fclose(f); } } }
}

/* THE CABINET LINK (engine/lan139.c, --link N / link.cfg). The game's link driver: FUN_00000C06 init, FUN_00000CB4 the per-frame send
 * (a message: 0x5A, the cabinet number + 0x10 per hop, the payload, its length with bit 8 = the last word), FUN_00000A5C the SCI
 * handler -- it checks a message, and a message from another cabinet that has made fewer than 5 hops is queued to go on round the
 * ring (store and forward, as Tokyo Wars). The cabinet number (0-3) is work RAM 0xE08400, set at boot from the operator setting at
 * 0xE0AFAE = EEPROM byte 0x26 bits 0-1 (the settings block 0xE0AF88 is the EEPROM, byte for byte; FUN_00006F04 sets it).
 * The driver's counters: 0xE0A628 messages received, 0xE0A62A sends finished, 0xE0A62C no end mark found, 0xE0A62E no 0x5A. */
static uint16_t w16(uint32_t a) { return (uint16_t)rr_read(a, 2); }
static void cc_link_cabinet(int id)
{
    g_ss22.eeprom[0x26] = (uint8_t)((g_ss22.eeprom[0x26] & ~3u) | (unsigned)(id & 3));
    g_ss22.wram[0xAFAE] = (uint8_t)((g_ss22.wram[0xAFAE] & ~3u) | (unsigned)(id & 3));   /* the settings copy, for a renumbering after boot */
}
static int  cc_link_game_id(void) { return (int)(w16(0xE08400) & 3); }
static bool cc_link_good(void)                /* messages are still arriving: the count moved within the last second */
{
    static uint16_t last; static uint32_t quiet;
    const uint16_t n = w16(0xE0A628);
    if (n != last) { last = n; quiet = 0; } else if (quiet < 1000) quiet++;
    return quiet < 60;
}
static void cc_link_report(char *buf, int n)
{
    snprintf(buf, (size_t)n, "game: cabinet %u, received %u, sent %u, no end %u, no 5A %u", (unsigned)(w16(0xE08400) & 3) + 1,
             w16(0xE0A628), w16(0xE0A62A), w16(0xE0A62C), w16(0xE0A62E));
}
static void cc_link_debug(uint32_t f)
{
    fprintf(stderr, "[CC] link f%u game: cab %u rx %u tx %u noend %u no5A %u fwdq %u pend %04X/%04X/%04X/%04X\n", f,
            w16(0xE08400), w16(0xE0A628), w16(0xE0A62A), w16(0xE0A62C), w16(0xE0A62E), w16(0xE0A634),
            w16(0xE0A606), w16(0xE0A60A), w16(0xE0A60E), w16(0xE0A612));
}

static const ss22_game game = {
    .name = "Cyber Cycles", .tag = "CC", .lname = "cc", .logname = "cybrcycc.log", .zip = "cybrcycc.zip",
    .board = &cc_board,
    .dsp = { { {"cb1ptrl0.18k", "cb1ptrl1.16k", "cb1ptrl2.15k", "cb1ptrl3.14k"},
               {"cb1ptrm0.18j", "cb1ptrm1.16j", "cb1ptrm2.15j", "cb1ptrm3.14j"},
               {"cb1ptru0.18f", "cb1ptru1.16f", "cb1ptru2.15f", "cb1ptru3.14f"} }, 4, cc_c25_exec },
    /* MAME's c352 region: cb1wavea.2l at 0, cb1waveb.1l at 0x800000; cybrcycc(): m_c352->add_route(2, "tank", 1.0) */
    .snd = { "cb1datab.8k", { "cb1wavea.2l", "cb1waveb.1l" }, { 0, 0x800000 }, 0x1000000, 0xFFFF, { 1, 2 },
             .aux = { [0] = { "Tank speaker", 1.0f } } },
    .video = { { "cb1cg0.8d", "cb1cg1.10d", "cb1cg2.12d", "cb1cg3.13d", "cb1cg4.14d", "cb1cg5.16d", "cb1cg6.18d", NULL },
               "cb1ccrl.3d", "cb1ccrh.1d", { "cb1scg0.12f" }, 1, 0x1000000, 0xFF },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_5E8,
    .polls_per_frame = 66772,           /* Tokyo Wars' figure (same CPU, same clock): not measured for this game */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x300, 0x100 },
    .link_id_byte = 0x26,               /* link play: the operator setting's cabinet number = EEPROM byte 0x26, bits 0-1 */
    .link_report = cc_link_report, .link_good = cc_link_good, .link_game_id = cc_link_game_id,
    .link_cabinet = cc_link_cabinet, .link_debug = cc_link_debug,
    .pdp_late_slices = 6,               /* the main loop does the link work (FUN_00000CB4) before it sets the master going (0x93A): with 4
                                         * cabinets that ends ~2 slices after the vblank, and MAME's rule would drop the frame's 3D */
    .pdp_hold_frames = 2,               /* a begin still too late (now and then, on a busy PC): that update shows the last 3D again */
    .link_rx_per_frame = 6,             /* 4 cabinets bring 4 a frame; a burst after a PC stalls (12-16 at once) made FUN_00000CB4 copy
                                         * for 5+ slices (a late 3D begin = the blink) and overrun its 16 x 0x200 copy area at 0xE08606 */
    .frame = cc_frame,
};

int main(int argc, char **argv) { return ss22_main(argc, argv, &game); }
