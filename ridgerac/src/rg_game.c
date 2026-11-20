/* Ridge Racer (MAME `ridgerac`, World RR2 Ver.B, 1994-01-17) on the shared System 22 runtime (../raverace/src) -- the same board as
 * Ridge Racer 2; what this game says about itself: which ROM chip is where, and what MAME's namcos22.cpp gives it (init_ridgerac:
 * NAMCOS22_RIDGE_RACER). The 68020 program is OUR translation (tools/import.sh: Ghidra + ../tools/gen/lift68k.py -> gen/rr_lifted_*.c);
 * the master DSP and the sound MCU are interpreted (../victlap/src/vl_interp.c). */
#include "rr_game.h"
#include "rr_lifted.h"

/* A player's first run: every chip taken out of MAME's zips by name and size (engine/romzip.c), the list as
 * ../acedriver/tools/setup_roms.py's GAMES["ridgerac"] plus the sound BIOS. */
static const char *const rg_zips[] = { "ridgerac.zip", "namcoc74.zip" };      /* (the DSP BIOS, c71.bin, is built into the engine) */
static const eng_rom_t rg_roms[] = {
    { "rr2_prgllb.4d", 0x80000, NULL, NULL },
    { "rr2_prglmb.2d", 0x80000, NULL, NULL },
    { "rr2_prgumb.8d", 0x80000, NULL, NULL },
    { "rr2_prguub.6d", 0x80000, NULL, NULL },
    { "rr1data.6r", 0x80000, NULL, NULL },
    { "rr1cg0.bin", 0x200000, NULL, NULL },
    { "rr1cg1.bin", 0x200000, NULL, NULL },
    { "rr1cg2.bin", 0x200000, NULL, NULL },
    { "rr1cg3.bin", 0x200000, NULL, NULL },
    { "rr1ccrl.bin", 0x200000, NULL, NULL },
    { "rr1ccrh.bin", 0x80000, NULL, NULL },
    { "rr1potl0.5b", 0x80000, NULL, NULL },
    { "rr1potl1.4b", 0x80000, NULL, NULL },
    { "rr1potm0.5c", 0x80000, NULL, NULL },
    { "rr1potm1.4c", 0x80000, NULL, NULL },
    { "rr1potu0.5d", 0x80000, NULL, NULL },
    { "rr1potu1.4d", 0x80000, NULL, NULL },
    { "rr1wav0.10r", 0x100000, NULL, NULL },
    { "rr1wav1.10p", 0x100000, NULL, NULL },
    { "rr1wav2.10n", 0x100000, NULL, NULL },
    { "rr1wav3.10l", 0x100000, NULL, NULL },
    { "rr1gam.2d", 0x100, NULL, NULL },
    { "rr1gam.3d", 0x100, NULL, NULL },
    { "rr1gam.4d", 0x100, NULL, NULL },
    { "c74.bin", 0x4000, NULL, NULL },        /* the sound MCU BIOS: namcoc74.zip */
};

static const rr_game_t rg = {
    .name = "ridgerac", .title = "Ridge Racer", .cfg_file = "rg_controls.cfg", .nv_file = "rg_eeprom.nv", .entry = L_9C74,
    .prg = { "rr2_prguub.6d", "rr2_prgumb.8d", "rr2_prglmb.2d", "rr2_prgllb.4d" },  /* ROM_LOAD32_BYTE offsets 0, 1, 2, 3 */
    .cg = { NULL, NULL, NULL, NULL, "rr1cg0.bin", "rr1cg1.bin", "rr1cg2.bin", "rr1cg3.bin" },   /* the upper 8 MB, as Ridge Racer 2 */
    .ccrl = "rr1ccrl.bin", .ccrh = "rr1ccrh.bin", .tex_fixup = 1,           /* MAME init_tables: Ridge Racer 1/2 */
    .pot = { { "rr1potl0.5b", "rr1potl1.4b" }, { "rr1potm0.5c", "rr1potm1.4c" }, { "rr1potu0.5d", "rr1potu1.4d" } },
    .pot_chips = 2,
    .snd_data = "rr1data.6r",
    .wav = { { "rr1wav0.10r", 0x000000 }, { "rr1wav2.10n", 0x100000 }, { "rr1wav1.10p", 0x200000 }, { "rr1wav3.10l", 0x300000 } },
    .gamma = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" },
    .eeprom = NULL,                                   /* no default image in the set: the game initialises a blank EEPROM */
    .keycus_off = -1,                                 /* namcos22_keycus_r: no case for NAMCOS22_RIDGE_RACER */
    .steer_add = 0x160, .gas_add = 884, .brake_add = 809,   /* handle_driving_io: Ridge Racer 1/2 */
    .inputs_idle = 0xFEFF,                            /* INPUT_PORTS_START(ridgera): cabinet 0x0100 = 0 Standard, the rest active-low 1 */
    .steer_min = 0x280, .steer_max = 0xD80,           /* ADC.0 PORT_MINMAX(0x280, 0xd80) */
    .gas_max = 0x610, .brake_max = 0x610,             /* ADC.1 / ADC.2 (0, 0x610) */
    .gas_delta = 160, .brake_delta = 160,             /* PORT_KEYDELTA(160) */
    .spin_pc = 0, .spin_op = 0,
    .zips = rg_zips, .nzips = 2, .roms = rg_roms, .nroms = (int)(sizeof rg_roms / sizeof rg_roms[0]),
    .log_file = "ridgeracer.log",
};
const rr_game_t *g_rr_game = &rg;

/* gen/ is built with LIFT_LAST_PC: the instruction last started, so a crash message can say where the program was */
uint8_t g_lift_cov[0x100000];                /* one byte per even address of the 2 MB program: this instruction has run */
uint32_t g_lift_pc;
extern const uint32_t *g_rr_last_pc;
__attribute__((constructor)) static void rg_last_pc(void) { g_rr_last_pc = &g_lift_pc; }
