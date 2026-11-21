/*
 * am_game.c -- Armadillo Racing (Namco 1997, Super System 22, MAME `adillor`, World AM2 Ver.A) as a table for the shared Super 22 host
 * (engine/ss22_*.c). Everything here is Armadillo's: its ROM chips, its program image's layout, the sound board's wiring, the cabinet's
 * controls and the scripted play a test run feeds them. The program is gen/am_lifted_NN.c (tools/namco22/lift.py); its master DSP program
 * (word for word Dirt Dash's) and its sound program are translated at build time (gen/am_c25.c, gen/am_snd_driver.c).
 *
 * THE TRACKBALL: on the board two pulse trains into the sound MCU's Timer A2/A3 inputs (event-counter mode, engine/snd/m377_periph.c
 * m37710_timer_event), made from the ball's two counters by MAME's adillor_state rule (engine/ss22_snd.c); the player's mouse -- a USB
 * trackball is a mouse -- the left stick and the arrow keys move the counters (engine/ss22_input.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "am_lifted.h"

bool am_c25_exec(c71_t *d, int pc);          /* gen/am_c25.c */

/* tools/setup_roms.py's REQUIRED (World, AM2 Ver.A); the unpacker checks name and size. The DSP BIOS c71.bin is built into the engine. */
static const eng_rom_t k_roms[] = {
    {"am2vera.rom1", 0x100000}, {"am2vera.rom2", 0x100000}, {"am2vera.rom3", 0x100000}, {"am2vera.rom4", 0x100000},
    {"am1data.8k", 0x80000},
    {"am1scg0.12f", 0x200000}, {"am1scg1.10f", 0x200000},
    {"am1cg0.8d", 0x200000}, {"am1cg1.10d", 0x200000}, {"am1cg2.12d", 0x200000}, {"am1cg3.13d", 0x200000},
    {"am1cg4.14d", 0x200000}, {"am1cg5.16d", 0x200000}, {"am1cg6.18d", 0x200000}, {"am1cg7.19d", 0x200000},
    {"am1ccrl.3d", 0x200000}, {"am1ccrh.1d", 0x80000},
    {"am1ptrl0.18k", 0x80000}, {"am1ptrl1.16k", 0x80000}, {"am1ptrl2.15k", 0x80000}, {"am1ptrl3.14k", 0x80000},
    {"am1ptrm0.18j", 0x80000}, {"am1ptrm1.16j", 0x80000}, {"am1ptrm2.15j", 0x80000}, {"am1ptrm3.14j", 0x80000},
    {"am1ptru0.18f", 0x80000}, {"am1ptru1.16f", 0x80000}, {"am1ptru2.15f", 0x80000}, {"am1ptru3.14f", 0x80000},
    {"am1wavea.2l", 0x400000}, {"am1waveb.1l", 0x400000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

/* MAME ROM_LOAD32_BYTE: am2vera.rom1 -> byte 3 of every long, .rom2 -> 2, .rom3 -> 1, .rom4 -> 0 (four 1 MB chips) */
static bool am_load_program(const char *dir)
{
    static const char *const lane[4] = { "am2vera.rom4", "am2vera.rom3", "am2vera.rom2", "am2vera.rom1" };      /* byte 0 .. 3 */
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

/* no default EEPROM image in the set: MAME starts from a blank 2864 (all 0xFF) and the game formats it */
static bool am_load_eeprom(const char *dir)
{
    (void)dir;
    const char *e = getenv("AM_EEPROM_FILL");
    memset(g_ss22.eeprom, e ? (int)strtol(e, NULL, 0) : 0xFF, SS22_EEPROM_SIZE);
    return true;
}

/* keycus: Armadillo Racing is in none of namcos22_keycus_r's cases, so every read is random (a unit that is never asked: 0xFFFF) */
static const ss22_board_cfg am_board = { "AM", 0xFFFF, 0, am_load_program, am_load_eeprom, NULL };

/* the cabinet (MAME INPUT_PORTS_START( adillor )): coin bit 0, service bit 2, the Test switch bit 3 (PORT_SERVICE: a toggle), START bit 8 */
#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_START   0x0100
static const ss22_action actions[] = {
    { "Insert coin",  "coin",    SDL_SCANCODE_5,      SDL_SCANCODE_6,       IN_COIN,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start",        "start",   SDL_SCANCODE_1,      SDL_SCANCODE_RETURN,  IN_START,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A), SDL_BUTTON_LMASK },
    { "Service",      "service", SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN, IN_SERVICE, 0, 0 },
    { "Test",         "test",    SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN, IN_TEST,    0, 0 },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x001, 0x3FF, 0x1FF, 12,
    { 0, 0 }, 0,
    { "Enter, then press the new key (Esc cancels)", "Trackball / mouse: roll to run and steer (captured while playing, Esc releases it); click = Start. Touchpad: two-finger scroll. Arrows roll too", "Pad: left stick / D-pad roll, Start or A start, Back coin" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
    false,                                  /* no steering motor */
    .trackball = true,                      /* the mouse (a USB trackball), the left stick and the arrows roll the ball (engine/ss22_input.c) */
};

static const ss22_press_name presses[] = { {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST} };

/* AM_AUTOPLAY=test: the operator menu (test mode) walked at random, as Dirt Dash's DD_AUTOPLAY=test -- tools/mame/cov_trace_test.lua feeds MAME the
 * same walk. The cabinet's Test switch on from frame 300 (off for 120 frames every 4000, restarting the menu), then every 12..31 frames a new action
 * held: the ball rolled up / down (CHOOSE) or left / right, START (ENTER), service, coin. AM_TEST_SEED picks the walk. */
static void autoplay_test(long n, uint16_t *p)
{
    static unsigned long long seed; static int init, left; static uint16_t held; static int tx, ty;
    if (!init) { const char *e = getenv("AM_TEST_SEED"); seed = e ? strtoull(e, NULL, 0) : 12345; init = 1; }
    const long t = n - 300;
    if (t >= 0 && !(t >= 4000 && t % 4000 < 120)) *p |= IN_TEST;
    if (n > 400) {
        if (left > 0) left--;
        else {
            #define RND(k) (seed = (seed * 1103515245ull + 12345ull) % 2147483648ull, (unsigned)((seed / 65536) % (k)))
            const unsigned r = RND(100);
            held = 0; tx = ty = 0;
            if      (r < 22) ty = 40;
            else if (r < 44) ty = -40;
            else if (r < 54) tx = 40;
            else if (r < 64) tx = -40;
            else if (r < 84) held = IN_START;
            else if (r < 90) held = IN_SERVICE;
            else if (r < 94) held = IN_COIN;
            left = 12 + (int)RND(20);
            #undef RND
        }
        *p |= held;
        if (tx || ty) ss22_snd_trackball(tx, ty);
    }
}

/* tools/mame/cov_trace_play.lua, frame for frame: five coins from frame 300 (a game costs four; MAME drops the first at boot), Start from 600, and from
 * frame 600 the ball's counters advanced as the script advances MAME's OPT ports (Y +28 a frame, +4 every third 300-frame block; X +-14, flipping every
 * 70 frames). The ports are what both sides count from; whether MAME's set_value is taken before or after the Y port's PORT_REVERSE is not checked. */
static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)wheel; (void)pedal1; (void)pedal2;
    static int test = -1;
    if (test < 0) { const char *e = getenv("AM_AUTOPLAY"); test = e && !strcmp(e, "test"); }
    if (test) { autoplay_test(n, p); return; }
    const long m = n % 4800;
    if (m >= 300 && m < 600 && (m - 300) % 60 < 8) *p |= IN_COIN;
    if (n >= 600 && n % 90 < 6) *p |= IN_START;
    static int full = -1; if (full < 0) full = getenv("AM_ROLL_FULL") != NULL;         /* AM_ROLL_FULL=1: the ball rolled forward at full speed (a test) */
    if (full && n > 600) { const char *e = getenv("AM_ROLL_FULL"); ss22_snd_trackball(e[0] == 'x' ? atoi(e + 1) : 0, e[0] == 'x' ? 0 : atoi(e)); return; }   /* AM_ROLL_FULL=<dy> or x<dx> */
    if (n > 600) ss22_snd_trackball(((n / 70) % 2 == 0) ? -14 : 14, ((n / 300) % 3 == 2) ? 4 : 28);
}

static const eng_hud_mark hud_marks_none[] = { { 0, 0, 0 } };

/* our fork, THE CABINET LINK. GAME OPTIONS > PCB ID (RED / BLUE / YELLOW / GREEN: four cabinets) is EEPROM byte 7. The game reads its
 * settings as 16 records of 32 bytes (record r = EEPROM r*16.. and 0x1000 + r*16.., copied to 0xE01A00 by FUN_00009554), record 0's
 * byte 7 becoming the live PCB ID at 0xE00180 (FUN_00009430); the whole 512-byte copy is checked by a 16-bit sum of its bytes
 * 0..0x1FD, kept in its last word (FUN_000096C0) = EEPROM 0x10FE (high) / 0x10FF (low). Link play writes byte 7, then the sum.
 * The ROM set has no EEPROM image: a blank one (all 0xFF) is set up by the game itself on its first start, which would undo the ID, so
 * the settings it writes then (record 0 below, everything else 0, as read back from a first start) go in first. */
static const uint8_t am_first_start[16] = { 0xB0, 0x53, 0x55, 0x01, 0x03, 0x03, 0x00, 0x00, 0x28, 0x28, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00 };
static void am_link_id_written(void)
{
    bool blank = true;
    for (int r = 0; r < 16 && blank; r++) for (int k = 0; k < 16; k++)
        if (g_ss22.eeprom[r * 16 + k] != 0xFF && !(r == 0 && k == 7)) { blank = false; break; }   /* (byte 7: the ID just written) */
    if (blank) {
        const uint8_t id = g_ss22.eeprom[7] & 3;
        for (int r = 0; r < 16; r++) { memset(&g_ss22.eeprom[r * 16], 0, 16); memset(&g_ss22.eeprom[0x1000 + r * 16], 0, 16); }
        memcpy(g_ss22.eeprom, am_first_start, 16);
        g_ss22.eeprom[7] = id;
    }
    unsigned sum = 0; const unsigned old_hi = g_ss22.eeprom[0x10FE], old_lo = g_ss22.eeprom[0x10FF];
    for (int r = 0; r < 16; r++)
        for (int k = 0; k < 32; k++) {
            if (r == 15 && k >= 30) break;                     /* the sum's own word */
            sum += k < 16 ? g_ss22.eeprom[r * 16 + k] : g_ss22.eeprom[0x1000 + r * 16 + k - 16];
        }
    g_ss22.eeprom[0x10FE] = (uint8_t)(sum >> 8); g_ss22.eeprom[0x10FF] = (uint8_t)sum;
    fprintf(stderr, "[LINK] Armadillo Racing: PCB ID byte %u, settings sum %04X (was %02X%02X)\n", g_ss22.eeprom[7], sum & 0xFFFF, old_hi, old_lo);
}
static int am_link_game_id(void) { return (int)(rr_read(0xE00180u, 2) & 3); }
/* The dust of the armadillo rolling and bumping, by the first tile of each sprite (found by listing every sprite of whole races, 3600 frames of autoplay, against the
 * sprite ROM's pictures): the puffs at the start and while running (3556..3584, 3484..3547: the same cloud in 3x3-tile and 4-tile sizes) and the small burst shapes the
 * ROM keeps after them. Before 3484 are the HUD and the banners (Ready / 3 / 2 / GOOD LUCK, the heart, FREE PLAY, the place letters). The Display menu's "Effects"
 * option smooths exactly these. (The red / white impact star is not a sprite of this layer.) */
static const int fx_tiles[][2] = { { 3484, 4095 } };

static const ss22_game game = {
    .name = "Armadillo Racing", .tag = "AM", .lname = "am", .logname = "adillor.log", .zip = "adillor.zip",
    .out_gain = 2.4,                         /* not measured for this game yet (Dirt Dash 2.4, Tokyo Wars 1.4) */
    .board = &am_board,
    .dsp = { { {"am1ptrl0.18k", "am1ptrl1.16k", "am1ptrl2.15k", "am1ptrl3.14k"},
               {"am1ptrm0.18j", "am1ptrm1.16j", "am1ptrm2.15j", "am1ptrm3.14j"},
               {"am1ptru0.18f", "am1ptru1.16f", "am1ptru2.15f", "am1ptru3.14f"} }, 4, am_c25_exec,     /* three planes of four chips (Tokyo Wars' shape) */
               .spin_pc = 0x45BB, .spin_op = 0x2000 },   /* Dirt Dash's upload poll: the master program is the same, word for word */
    .snd = { "am1data.8k", { "am1wavea.2l", "am1waveb.1l" }, { 0, 0x800000 }, 0x1000000, 0xFFFF, { 1, 2 }, { true, true },
             .trackball = true },   /* the ball: Timer A2 (Y) / A3 (X) pulses, MAME's adillor rule (engine/ss22_snd.c) */   /* MAME: both ROM_LOAD16_WORD_SWAP, a at 0, b at 0x800000 */
    .video = { { "am1cg0.8d", "am1cg1.10d", "am1cg2.12d", "am1cg3.13d", "am1cg4.14d", "am1cg5.16d", "am1cg6.18d", "am1cg7.19d" },
               "am1ccrl.3d", "am1ccrh.1d", { "am1scg0.12f", "am1scg1.10f" }, 2, 0x1000000, 0xFF,
               false, false,                /* MAME: a 16 MB sprite region, ROMREGION_ERASEFF, the two chips at 0 and 0x200000 */
               .hud = { hud_marks_none, 0, 0x10, -1 },
               .fx = fx_tiles, .n_fx = 1 },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_4000,
    .polls_per_frame = 66800,               /* MAME: 40,064,130 main-CPU instructions in its first 600 frames (tools/mame/regtrace_run.sh) */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .link_id_byte = 7, .link_game_id = am_link_game_id, .link_id_written = am_link_id_written,   /* PCB ID, see am_link_id_written */
};

int main(int argc, char **argv) { return ss22_main(argc, argv, &game); }
