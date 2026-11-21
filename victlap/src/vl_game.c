/* Ace Driver: Victory Lap (MAME `victlap`, World ADV2 Ver.B, 96/05/21) on the shared System 22 runtime (../raverace/src) -- the
 * same board as Ace Driver; what this game says about itself: which ROM chip is where, and what MAME's namcos22.cpp gives it.
 * The 68020 program is OUR translation (tools/import.sh: Ghidra + ../tools/gen/lift68k.py -> gen/rr_lifted_*.c); the master DSP and
 * the sound MCU are interpreted (src/vl_interp.c). */
#include "rr_game.h"
#include "rr_lifted.h"

/* A player's first run: every chip taken out of MAME's zips by name and size (engine/romzip.c), the list as
 * ../acedriver/tools/setup_roms.py's GAMES["victlap"] plus the sound BIOS. */
static const char *const vl_zips[] = { "victlap.zip", "namcoc74.zip" };       /* (the DSP BIOS, c71.bin, is built into the engine) */
static const eng_rom_t vl_roms[] = {
    { "adv2_prgllb.4d", 0x80000, NULL, NULL },
    { "adv2_prglmb.2d", 0x80000, NULL, NULL },
    { "adv2_prgumb.8d", 0x80000, NULL, NULL },
    { "adv2_prguub.6d", 0x80000, NULL, NULL },
    { "adv1data.6r", 0x80000, NULL, NULL },
    { "adv1cg0.2a", 0x200000, NULL, NULL },
    { "adv1cg1.1c", 0x200000, NULL, NULL },
    { "adv1cg2.2d", 0x200000, NULL, NULL },
    { "adv1cg3.1e", 0x200000, NULL, NULL },
    { "adv1cg4.2f", 0x200000, NULL, NULL },
    { "adv1cg5.1j", 0x200000, NULL, NULL },
    { "adv1cg6.2k", 0x200000, NULL, NULL },
    { "adv1cg7.1n", 0x200000, NULL, NULL },
    { "adv1ccrl.5a", 0x200000, NULL, NULL },
    { "adv1ccrh.5c", 0x80000, NULL, NULL },
    { "adv1pot.l0", 0x80000, NULL, NULL },
    { "adv1pot.l1", 0x80000, NULL, NULL },
    { "adv1pot.l2", 0x80000, NULL, NULL },
    { "adv1pot.m0", 0x80000, NULL, NULL },
    { "adv1pot.m1", 0x80000, NULL, NULL },
    { "adv1pot.m2", 0x80000, NULL, NULL },
    { "adv1pot.u0", 0x80000, NULL, NULL },
    { "adv1pot.u1", 0x80000, NULL, NULL },
    { "adv1pot.u2", 0x80000, NULL, NULL },
    { "adv1wav0.10r", 0x100000, NULL, NULL },
    { "adv1wav1.10p", 0x100000, NULL, NULL },
    { "adv1wav2.10n", 0x100000, NULL, NULL },
    { "adv1wav3.10l", 0x100000, NULL, NULL },
    { "rr1gam.2d", 0x100, NULL, NULL },
    { "rr1gam.3d", 0x100, NULL, NULL },
    { "rr1gam.4d", 0x100, NULL, NULL },
    { "c74.bin", 0x4000, NULL, NULL },        /* the sound MCU BIOS: namcoc74.zip */
};

/* what the game writes on a blank EEPROM's first start (measured 2026-11-16: a window run from no vl_eeprom.nv), for the link to put
 * the cabinet number in before the game boots -- the same scheme as Ace Driver: block 0x000 valid, settings block 0x100 (magic 0x0156),
 * 8-bit ~sum of bytes 0..30 in byte 31 */
static const struct rr_ee_init vl_ee_init[2] = {
    { 0x000, { [31] = 0xFF } },
    { 0x100, { 0x01, 0x56, 0x10, 0x00, 0x03, 0xE0, 0x0B, 0xC0, 0x28, 0x28, 0x20, 0x22, 0x00, 0x13, 0x13, 0x13,
               0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x08 } },
};

static const rr_game_t vl = {
    .name = "victlap", .title = "Ace Driver: Victory Lap", .cfg_file = "vl_controls.cfg", .nv_file = "vl_eeprom.nv", .entry = L_47C,
    .prg = { "adv2_prguub.6d", "adv2_prgumb.8d", "adv2_prglmb.2d", "adv2_prgllb.4d" },      /* ROM_LOAD32_BYTE offsets 0, 1, 2, 3 */
    .cg = { "adv1cg0.2a", "adv1cg1.1c", "adv1cg2.2d", "adv1cg3.1e", "adv1cg4.2f", "adv1cg5.1j", "adv1cg6.2k", "adv1cg7.1n" },
    .ccrl = "adv1ccrl.5a", .ccrh = "adv1ccrh.5c", .tex_fixup = 0,           /* all 16 MB of texture: no Ace Driver fix-up */
    .pot = { { "adv1pot.l0", "adv1pot.l1", "adv1pot.l2" }, { "adv1pot.m0", "adv1pot.m1", "adv1pot.m2" },
             { "adv1pot.u0", "adv1pot.u1", "adv1pot.u2" } },
    .pot_chips = 3,
    .snd_data = "adv1data.6r",
    .wav = { { "adv1wav0.10r", 0x000000 }, { "adv1wav2.10n", 0x100000 }, { "adv1wav1.10p", 0x200000 }, { "adv1wav3.10l", 0x300000 } },
    .gamma = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" },
    .eeprom = NULL,                                   /* no default image in the set: the game initialises a blank EEPROM */
    .keycus_off = 2, .keycus_val = 0x0188,            /* namcos22_keycus_r: NAMCOS22_VICTORY_LAP, offset 2 */
    .steer_add = 2048, .gas_add = 992, .brake_add = 3008,   /* handle_driving_io: Ace Driver / Victory Lap */
    .inputs_idle = 0xFFFF,                            /* INPUT_PORTS_START(victlap) = acedrive's */
    .steer_min = 0x200, .steer_max = 0xE00,           /* ADC.0 PORT_MINMAX(0x200, 0xe00) */
    .gas_max = 0x480, .brake_max = 0x240,             /* ADC.1 (0, 0x480), ADC.2 (0, 0x240) */
    .gas_delta = 120, .brake_delta = 80,              /* PORT_KEYDELTA(120) / (80) */
    .spin_pc = 0, .spin_op = 0,
    .zips = vl_zips, .nzips = 2, .roms = vl_roms, .nroms = (int)(sizeof vl_roms / sizeof vl_roms[0]),
    .log_file = "victlap.log",
    /* LINK PLAY (our fork): the cabinet number is A6-0x73FC = 0x10000C04 as in Ace Driver (0x2C64C copies it from the settings'
     * 0x1000C14A), but packed: the settings parse (0x2C3E0, from the EEPROM block 0x100 copy at A6+0x4030) takes byte 0x0C bits 7..5 */
    .link_cab_addr = 0x10000C04, .link_ee_block = { 0x100, -1 }, .link_ee_byte = 0x0C, .link_ee_shift = 5, .link_ee_sum8 = 1,
    .link_ee_init = vl_ee_init, .nlink_ee_init = 2,
    .link_max_cabinets = 4,                           /* the game numbers 8, but 8 linked windows were all over the place in the user's test (2026-11-16): 4 for now */
    .ram_code_at = 0x10010000, .ram_code_rom = 0x50000, .ram_code_size = 0x8000,   /* its program 0x50000.. copied to work RAM and run there (measured: 0x10010000..0x10017DF0 = ROM) */
};
const rr_game_t *g_rr_game = &vl;

/* gen/ is built with LIFT_LAST_PC: the instruction last started, so a crash message can say where the program was */
uint8_t g_lift_cov[0x100000];                /* one byte per even address of the 2 MB program: this instruction has run */
uint32_t g_lift_pc;
extern const uint32_t *g_rr_last_pc;
__attribute__((constructor)) static void vl_last_pc(void) { g_rr_last_pc = &g_lift_pc; }
/* TEXTURE PAGES 8192 (not the engine's 4096): all of Victory Lap's 16 MB of texture then fits one page, so a run of quads is never
 * split by a page change (a race: 274588 page breaks -> 0 over 2600 frames; draw 4.7 -> 3.9 ms a frame in a window on Iris Xe, and
 * the start line of a 4-window linked test mostly back under 16.7 ms). Pictures identical but for ~10 texel-edge pixels.
 * ENG_ATLAS_DIM set by the user still wins. */
#include <stdlib.h>
__attribute__((constructor)) static void vl_atlas(void) { if (!getenv("ENG_ATLAS_DIM")) setenv("ENG_ATLAS_DIM", "8192", 0); }
