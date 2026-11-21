/* gw_game.c -- GUNMEN WARS' table for the System 23 host (../s23/include/s23_game.h): MAME's gunwars set (Japan, GM1 Ver. B), Super System 23
 * with the GMEN board. Values from MAME's namcos23.cpp (namcoss23_gmen_state, ROM_START(gunwars)).
 *
 * SUPER SYSTEM 23 = System 23 (Time Crisis 2's board, same clocks and interrupt pins) plus one register: a write to 0x0100005C acknowledges
 * the C361 interrupt (MAME namcoss23_state::c435_c361_ack_w).
 * THE GMEN BOARD (camera overlay + the cabinet link): an SH-2 with its own RAM, a VPX-3220 video decoder (the "Namcam") and an MD8412B
 * FireWire link chip. The main CPU sees it at 0x0E400000 (read: release the SH-2 from reset), 0x0E600000 (status: MAME reads 1) and
 * 0x0E700000..0x0E70FFFF (RAM shared with the SH-2; a command written at +0x20 interrupts it). FIRST STAGE: MAME's main-CPU side only,
 * no SH-2 yet -- the RAM is plain RAM and nobody answers a command. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "s23_game.h"
#include "s23_host.h"

static const eng_rom_t chips[] = {
    { "gm1verb.ic2", 0x200000, NULL, NULL, 0x401F8264 }, { "gm1verb.ic1", 0x200000, NULL, NULL, 0xF9FD0F2B },
    { "gm1vera.ic3", 0x80000, NULL, NULL, 0x5582FDD4 },  { "asc5_io-a.ic14", 0x20000, NULL, NULL, 0x5964767F },
    { "gm1mtah.2j", 0x800000, NULL, NULL, 0x3CEA9094 }, { "gm1mtal.2h", 0x800000, NULL, NULL, 0xD531DFCD },
    { "gm1cgll.4m", 0x800000, NULL, NULL, 0x936C0079 }, { "gm1cglm.4k", 0x800000, NULL, NULL, 0xE2EE5493 },
    { "gm1cgum.4j", 0x800000, NULL, NULL, 0xA7728944 }, { "gm1cguu.5f", 0x800000, NULL, NULL, 0x26A74698 },
    { "gm1ccrl.7f", 0x400000, NULL, NULL, 0x2C54C182 }, { "gm1ccrh.7e", 0x200000, NULL, NULL, 0x8563EF01 },
    { "gm1pt0h.7a", 0x400000, NULL, NULL, 0x5EBD658C }, { "gm1pt0l.7c", 0x400000, NULL, NULL, 0x62E9BEDB },
    { "gm1pt1h.5a", 0x400000, NULL, NULL, 0x5F6CEBAB }, { "gm1pt1l.5c", 0x400000, NULL, NULL, 0xF44C149F },
    { "gm1wave.2c", 0x800000, NULL, NULL, 0x7D5C79A4 },
};
static const char *const zips[] = { "gunwars.zip", "namco_asca5.zip" };   /* a split MAME set keeps the I/O board's program in its device zip */

int s23_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../s23/src/s23_rom.c */

/* the boot image (4 MB at 0x1FC00000): gm1verb.ic2 and .ic1 byte-interleaved, .ic2 first (MAME ROM_LOAD16_BYTE), as Time Crisis 2's */
static int rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && s23_rom_read(dir, "gm1verb.ic2", a, 0x200000) && s23_rom_read(dir, "gm1verb.ic1", b, 0x200000);
    if (ok) for (uint32_t k = 0; k < 0x200000; k++) { boot[2 * k] = a[k]; boot[2 * k + 1] = b[k]; }
    free(a); free(b);
    return ok;
}
/* the sub-CPU's (H8/3002) program: gm1vera.ic3, word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !s23_rom_read(dir, "gm1vera.ic3", dst, 0x80000)) return 0;
    for (uint32_t k = 0; k < 0x80000; k += 2) { const uint8_t t = dst[k]; dst[k] = dst[k + 1]; dst[k + 1] = t; }
    return 1;
}
/* the I/O board's (ASCA-5, H8/3334) program: the first n bytes of asc5_io-a.ic14 */
static int rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x20000);
    const int ok = all && n <= 0x20000 && s23_rom_read(dir, "asc5_io-a.ic14", all, 0x20000);
    if (ok) memcpy(dst, all, n);
    free(all);
    return ok;
}

/* ---- the GMEN board, main-CPU side (MAME namcoss23_gmen_state::mips_map); the SH-2 behind it: src/gw_gmen.c ---- */
extern uint8_t gw_gmen_ram[0x10000 + 4];
void gw_gmen_reset_release(void), gw_gmen_command_written(void), gw_gmen_run(uint64_t main_now);
uint64_t s23_cycles_now(void);
static uint32_t be_rd(const uint8_t *p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
static void be_wr(uint8_t *p, int n, uint32_t v) { for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
int gw_snapshots;                                 /* no-camera snapshots reported (below) */
/* THE PHOTO the camera board hands over: shared +0x4000-0x7FFF, 8192 RGB555 pixels (big-endian; main 0x800951D4 reads them for its
 * colour histogram at each snapshot step, 0x80095934 writes back the background around the face). No camera: mid grey -- the
 * "greyed out" picture (the user's choice); the team select shows it in the player's photo slot. A loaded photo / webcam goes here later. */
/* THE PHOTO PAGE (texture page 0x1F000: the team select's photo slot is model 0x0A2E, u, v 0-127; the tags above the fighters too).
 * Its tile map in the ROM points at a filler tile everywhere and the photo palette 0x7E is never written: on the cabinet the camera board
 * supplies these texels itself (MAME shows nothing there). No camera: every photo greyed out (the user's choice) -- a loaded photo /
 * webcam picture goes into s23_photo_tex later */
#include "s23_host.h"
__attribute__((constructor)) static void gw_photo_init(void) { const int t = getenv("GW_PHOTO_TEST") != NULL;   /* TEST: red = u, green = v */
    for (int i = 0; i < S23_PHOTO_W * 256; i++) s23_photo_tex[i] = t ? (uint32_t)(i & 255) << 16 | (uint32_t)(i / S23_PHOTO_W) << 8 : 0x808080; s23_photo_gen++; }
static void gw_fill_photo(void) { for (int i = 0; i < 8192; i++) be_wr(gw_gmen_ram + 0x4000 + 2 * i, 2, 0x4210); }
static int gw_read(uint32_t p, int size, uint32_t *v)
{
    if (p >= 0x0E400000u && p < 0x0E800000u) gw_gmen_run(s23_cycles_now());          /* the SH-2 catches up with the main CPU first */
    /* THE CAMERA SCREEN'S WAIT (the user: "the sound and game slow down" on the NamCam screen): the main program polls shared +0xC0A4
     * for the camera board's frame -- measured 564,807 reads in one frame, each through this slow path (half again the frame's host time).
     * A read across to the GMEN board is a slow bus access on the real machine too: it is charged 40 cycles, so the same wait (emulated
     * time) takes ~1/40 of the reads (400 changed the game later on: timing) */
    if ((p & ~3u) == 0x0E70C0A4u) { extern int32_t rr_budget; rr_budget -= 40; }   /* (+0xC0A6, 2 bytes, in practice) */
    /* THE NAMCAM SNAPSHOT (team select: the screen flashes red / green / blue while the camera takes the player's picture). The main CPU
     * waits for the GMEN board's "snapshot taken" word at shared +0xC0A6 (0x80094EA4); our board has no camera, and its SH-2 program does
     * not report the snapshot without one -- so after 90 frames of waiting with no answer, it is reported taken (a blank picture). The
     * user's photo / webcam feature replaces this later. */
    if (p == 0x0E70C0A6u && size == 2) {
        extern uint32_t rr_frame;
        { extern int gw_cam_phase; if (!gw_cam_phase) gw_cam_phase = 1; }   /* the camera step has begun (the overlay: gw_list_hook) */
        static uint32_t first, last, pulsed;                 /* pulsed: the frame we set it -- a PULSE: back to 0 once the main CPU has seen it */
        if (pulsed && rr_frame > pulsed + 1) { be_wr(gw_gmen_ram + 0xC0A6, 2, 0); pulsed = 0; }
        if (be_rd(gw_gmen_ram + 0xC0A6, 2) != 0) first = 0;
        else if (!first || rr_frame > last + 2) first = rr_frame;
        else if (rr_frame - first > 90) { gw_fill_photo(); be_wr(gw_gmen_ram + 0xC0A6, 2, 1); first = 0; pulsed = rr_frame; { extern int gw_snapshots; gw_snapshots++; } fprintf(stderr, "[GMEN] no camera: snapshot reported taken (frame %u)\n", rr_frame); }
        last = rr_frame;
    }
    /* and the snapshot steps' acknowledge: the main CPU sets +0xC0AA to 1 for each step (0x80094E10) and waits for the board to clear it
     * (0x80094FAC); with no camera, cleared after 30 frames -- only once a no-camera snapshot was reported */
    if (p == 0x0E70C0AAu && size == 2) {
        extern uint32_t rr_frame;
        static uint32_t since; static int armed;
        { static int was; extern int gw_snapshots; if (gw_snapshots != was) { was = gw_snapshots; armed = 1; } }
        if (be_rd(gw_gmen_ram + 0xC0AA, 2) == 0) since = 0;
        else if (!since) since = rr_frame;
        else if (armed && rr_frame - since > 30) { gw_fill_photo(); be_wr(gw_gmen_ram + 0xC0AA, 2, 0); since = 0; fprintf(stderr, "[GMEN] no camera: snapshot step acknowledged (frame %u)\n", rr_frame); }
    }
    { static int on = -1; if (on < 0) on = getenv("GW_RDLOG") != NULL;   /* $GW_RDLOG: the main CPU's reads of the link words C000-C0FF, counted per frame */
      if (on && p >= 0x0E70C000u && p < 0x0E70C100u) { extern uint32_t rr_frame; static uint32_t lf, cnt[64]; if (rr_frame != lf) { if (lf % 300 == 0) { fprintf(stderr, "[RD] f%u:", lf); for (int i = 0; i < 64; i++) if (cnt[i]) fprintf(stderr, " C0%02X:%u", i * 4, cnt[i]); fputc(10, stderr); } memset(cnt, 0, sizeof cnt); lf = rr_frame; } cnt[(p >> 2) & 63]++; } }
    if (p >= 0x0E700000u && p + (uint32_t)size <= 0x0E710000u) { *v = be_rd(gw_gmen_ram + (p - 0x0E700000u), size); return 1; }
    if (p >= 0x0E400000u && p < 0x0E400004u) { gw_gmen_reset_release(); *v = 0; return 1; }   /* sh2_trigger_r: the SH-2 out of reset */
    if (p >= 0x0E600000u && p < 0x0E600004u) { *v = size == 4 ? 1 : 0; return 1; }          /* mips_sh2_unk_r: 0x00000001 */
    return 0;
}
static int gw_write(uint32_t p, int size, uint32_t v)
{
    if (p >= 0x0E700000u && p + (uint32_t)size <= 0x0E710000u) {
        gw_gmen_run(s23_cycles_now());
        be_wr(gw_gmen_ram + (p - 0x0E700000u), size, v);
        { void gw_shwatch(int who, uint32_t off, int n, uint32_t v); gw_shwatch(0, p - 0x0E700000u, size, v); }
        /* mips_to_sh2_shared_w: a command at +0x20 (the high half written, below 0x0100) interrupts the SH-2 (IRQ6) */
        { extern uint32_t rr_frame; static int n; if (getenv("GW_CMDLOG") && (p & ~3u) == 0x0E700020u && rr_frame > 1100 && n++ < 200) fprintf(stderr, "[CMD] f%u main writes +%X (%d) = %X\n", rr_frame, p - 0x0E700000u, size, v); }
        if (p == 0x0E700020u && (size == 4 ? (v >> 24) == 0 : size == 2 && (v >> 8) == 0)) gw_gmen_command_written();
        return 1;
    }
    if (p >= 0x0100005Cu && p < 0x01000060u) { s23_irq_set(S23_SRC_C361, 0); return 1; }      /* Super System 23: C361 acknowledge */
    return 0;
}

/* ---- THE CONTROLS on the ASCA-5's A/D (MAME gunwars ports: ANALOG_INPUT1 = slide left/right, 2 = slide forward/back, 3 = the gun's
 * yaw; 10 bits, centre 0x200). The GUN'S TURN is the engine's aim (the mouse, the arrow keys, a pad stick: g_ss22_gun_x over the range
 * gun_x0/gun_xw below); the SLIDE is W A S D or the pad's left stick. Trigger = the gun trigger (Space, left mouse, pad A); the start /
 * enter button = the pedal (Z, right mouse, pad B); select up / down = Page Up / Page Down (src: ../s23/src/s23_h8.c asca_switches). */
#include <SDL2/SDL.h>
extern uint16_t g_ss22_gun_x;
extern uint16_t (*s23_asca_analog)(int ch);
static SDL_GameController *pad;
static int pad_axis(SDL_GameControllerAxis ax)
{
    if (!pad || !SDL_GameControllerGetAttached(pad)) {
        pad = NULL;
        int s23_pad_choice(void); const int only = s23_pad_choice();   /* (one pad a window: $S23_PAD; "none" = the keyboard's copy) */
        for (int i = 0, gi = -1; only != -3 && i < SDL_NumJoysticks() && !pad; i++) if (SDL_IsGameController(i) && (++gi == only || only < 0)) pad = SDL_GameControllerOpen(i);
        if (!pad) return 0;
    }
    const int v = SDL_GameControllerGetAxis(pad, ax);
    return v > -6000 && v < 6000 ? 0 : v;                         /* a dead zone */
}
/* GUNMEN WARS' CONTROLS (the menu's Controls page: every row rebindable; the light-gun cabinet's trigger / foot pedal / reload list is
 * Time Crisis 2's). The ASCA reads Enter where the light-gun table has its pedal bit (src/s23_h8.c asca_switches), and the four moves are
 * our own bits that gw_analog turns into the slide inputs. */
#include "ss22_input.h"
extern uint16_t s23_inputs;
#define GW_IN_FWD   0x0200
#define GW_IN_BACK  0x0400
#define GW_IN_LEFT  0x0800
#define GW_IN_RIGHT 0x1000
static const ss22_action gw_actions[] = {
    { "Insert coin",     "coin",     SDL_SCANCODE_5,      SDL_SCANCODE_6,       0x0001, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Fire",            "trigger",  SDL_SCANCODE_SPACE,  SDL_SCANCODE_RETURN,  0x0010, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) | SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), SDL_BUTTON_LMASK },
    { "Start / Enter",   "enter",    SDL_SCANCODE_Z,      SDL_SCANCODE_X,       0x0020, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_B) | SS22_PAD(SDL_CONTROLLER_BUTTON_START), SDL_BUTTON_RMASK | SDL_BUTTON_MMASK },
    { "Move forward",    "forward",  SDL_SCANCODE_W,      SDL_SCANCODE_UNKNOWN, GW_IN_FWD,   0, 0 },
    { "Move back",       "back",     SDL_SCANCODE_S,      SDL_SCANCODE_UNKNOWN, GW_IN_BACK,  0, 0 },
    { "Move left",       "left",     SDL_SCANCODE_A,      SDL_SCANCODE_UNKNOWN, GW_IN_LEFT,  0, 0 },
    { "Move right",      "right",    SDL_SCANCODE_D,      SDL_SCANCODE_UNKNOWN, GW_IN_RIGHT, 0, 0 },
    { "Service",         "service",  SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN, 0x0004, 0, 0 },
    { "Test",            "test",     SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN, 0x0008, 0, 0 },
    { "Operator up",     "op_up",    SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_UNKNOWN, 0x0040, 0, 0 },
    { "Operator down",   "op_down",  SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_UNKNOWN, 0x0080, 0, 0 },
    { "Operator enter",  "op_enter", SDL_SCANCODE_HOME,     SDL_SCANCODE_UNKNOWN, 0x0100, 0, 0 },
};
static void gw_send_inputs(uint16_t pressed, unsigned wheel, unsigned p1, unsigned p2)
{
    (void)wheel; (void)p1; (void)p2;
    { static int scripted = -1; if (scripted < 0) scripted = getenv("S23_PRESS") != NULL; if (scripted) return; }   /* tests: the script owns the buttons */
    if (pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) pressed |= 0x0010;   /* the pad's TRIGGERS: right fires, left = start / enter */
    if (pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) pressed |= 0x0020;
    s23_inputs = pressed;
}
static const ss22_input_game gw_input = {
    .actions = gw_actions, .n = (int)(sizeof gw_actions / sizeof *gw_actions),
    .wheel_min = 0x001, .wheel_max = 0x3FF, .wheel_key_span = 0x1FF, .wheel_step = 12,
    .notes = { "Enter, then press the new key (Esc cancels)",
               "Mouse (Options > Controls): FPS = the mouse turns, left button fires, right button = enter; Cursor = push to a side to turn",
               "Pad: left stick moves, right stick turns, RT / A / RB fire, LT / B / Start enter, Back coin" },
    .send = gw_send_inputs, .test_bit = 0x0008, .service_bit = 0x0004,
    .light_gun = true, .aim_right_stick = true, .mounted_gun = true,   /* the mouse / RIGHT stick drive the aim cursor (g_ss22_gun_x); the left stick moves */
};
static int fps_layout = -1;
static int fps_sens = -1;
static int gw_fps_sens(void) { int eng_cfg_int(const char *, int); if (fps_sens < 0) { fps_sens = eng_cfg_int("fps_sensitivity", 4); if (fps_sens < 1) fps_sens = 1; if (fps_sens > 10) fps_sens = 10; } return fps_sens; }
static int gw_fps_layout(void) { int eng_cfg_int(const char *, int); if (fps_layout < 0) fps_layout = eng_cfg_int("fps_layout", 1) != 0; return fps_layout; }
/* the mouse captured (relative) while the game is played in FPS mode; free while the Esc menu is open or the window is not focused */
void gw_mouse_capture(void)
{
    _Bool eng_ui_is_open(void); SDL_Window *w = SDL_GetKeyboardFocus();
    int s23_pads_only(void);
    const int want = w && gw_fps_layout() && !eng_ui_is_open() && !s23_pads_only();   /* (a party's pad player never takes the mouse) */
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != want) SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
}
static uint16_t gw_analog(int ch)
{
    { static const char *hold; static int init; if (!init) { init = 1; hold = getenv("GW_HOLD"); }   /* TEST: "ch:value@frame+frames,..." (hex value) */
      if (hold) { extern uint32_t rr_frame; for (const char *q = hold; *q; ) { int c = -1, n = 0; unsigned val = 0, at = 0, len = 0;
          if (sscanf(q, "%d:%x@%u+%u%n", &c, &val, &at, &len, &n) < 4) break;
          if (c == ch && rr_frame >= at && rr_frame < at + len) return (uint16_t)(val & 0x3FF);
          q += n; if (*q == ',') q++; } } }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    int v = 0;
    switch (ch) {
    /* DIRECTIONS AND RANGE as the game's own GUN TEST page reads them (test menu > I/O TEST > GUN TEST): slide LEFT = a HIGHER value,
     * FORWARD = higher, gun yawing RIGHT = higher; 4 units of the game's scale per A/D step, saturating at +-2048 = +-0x200 -- a full
     * +-0x1FF push wraps to the other side, so every control stays within +-0x1F0. $GW_FB=reverse flips forward / back. */
    case 0: v = -pad_axis(SDL_CONTROLLER_AXIS_LEFTX) / 66; if (s23_inputs & GW_IN_LEFT) v += 0x1F0; if (s23_inputs & GW_IN_RIGHT) v -= 0x1F0; break;
    case 1: { static int rev = -1; if (rev < 0) { const char *e = getenv("GW_FB"); rev = e && !strcmp(e, "reverse"); }
              v = -pad_axis(SDL_CONTROLLER_AXIS_LEFTY) / 66; if (s23_inputs & GW_IN_FWD) v += 0x1F0; if (s23_inputs & GW_IN_BACK) v -= 0x1F0;
              if (rev) v = -v; break; }
    case 2: {
        /* THE FREE CURSOR (user's choice): the engine's crosshair is the cursor (the mouse / arrows / a stick over the picture, g_ss22_gun_x
         * 0..0x3FF across it); the gun is swung toward it so the game's lock-on square snaps onto whoever is nearest the cursor.
         * TURNING, measured (2026-11-01, held swivel values, the picture's sideways slide): the game does not turn below a swivel of +-0xD8,
         * then about 3 px a frame, ~6 at 0x100, ~11 at 0x180, ~25 at 0x1F0. So: the middle 60% of the width aims (0 .. 0xD0, just below
         * the turn), the outer 20% each side turns -- from the threshold, eased (t^1.7) so the inner part turns slowly and full speed is
         * only at the very edge (it was a 10% strip spanning the whole speed range: all or nothing). $GW_AIMZONE=<0.3..0.9> the aiming
         * part of the half-width; $GW_AIM=<hex> its swivel. Vertical: the cabinet has no pitch input -- the game aims up / down itself. */
        /* THE FPS LAYOUT (the user's choice, default; Options page "Controls"): the mouse's MOVEMENT turns, as in a shooter -- each frame's
         * sideways movement sets the swivel (past the turn threshold 0xD8, faster movement = faster turn) and it settles back to the
         * middle when the mouse stops; the mouse is captured while playing (not while the menu is open). The pad's right stick turns
         * the same way. The cursor layout (below) stays for those who prefer it. */
        if (gw_fps_layout()) {
            extern uint32_t rr_frame; static uint32_t lastf; static double sw;
            if (rr_frame != lastf) {
                lastf = rr_frame;
                int dx = 0, dy; SDL_GetRelativeMouseState(&dx, &dy); { int s23_pads_only(void); if (s23_pads_only()) dx = 0; }
                void gw_mouse_capture(void); gw_mouse_capture();
                const int rx = pad_axis(SDL_CONTROLLER_AXIS_RIGHTX);
                /* the mouse's speed, smoothed over a few frames (the user: "a bit twitchy"); a slow movement only nudges the aim (below the
                 * turn threshold), faster turns, gently; Sensitivity (Options page, 1-10) scales it */
                static double v; v = v * 0.75 + dx * 0.25;
                const double sens = gw_fps_sens() / 4.0, a = (v < 0 ? -v : v) * sens;
                double want = 0;
                if (a >= 0.4) want = a < 2.0 ? (a - 0.4) / 1.6 * 0xC0 : 0xD8 + ((a - 2.0) * 10.0 > 0x1F0 - 0xD8 ? 0x1F0 - 0xD8 : (a - 2.0) * 10.0);
                if (v < 0) want = -want;
                if (!want && rx) { const double t = (rx < 0 ? -rx : rx) / 32767.0; want = 0xC0 + t * t * (0x1F0 - 0xC0); if (rx < 0) want = -want; }
                sw = sw * 0.6 + want * 0.4;
                if (sw > -6 && sw < 6) sw = 0;
            }
            return (uint16_t)(0x200 + (int)sw);
        }
        static int aim = -1; static double zone = 0.6;
        if (aim < 0) { const char *e = getenv("GW_AIM"); aim = e ? (int)strtol(e, NULL, 16) : 0xD0; if (aim < 0x20) aim = 0x20; if (aim > 0x1F0) aim = 0x1F0;
                       e = getenv("GW_AIMZONE"); if (e) { zone = atof(e); if (zone < 0.3) zone = 0.3; if (zone > 0.9) zone = 0.9; } }
        const int gx = g_ss22_gun_x > 0x3FF ? 0x3FF : g_ss22_gun_x;
        const double x = (gx - 0x200) / 512.0, ax = x < 0 ? -x : x;                     /* -1 .. +1 across the picture */
        const double t = ax <= zone ? 0 : (ax - zone) / (1.0 - zone);
        const double off = ax <= zone ? ax / zone * aim : 0xD8 + pow(t > 1 ? 1 : t, 1.7) * (0x1F0 - 0xD8);
        return (uint16_t)(0x200 + (x < 0 ? -off : off)); }
    default: return 0x200;
    }
    if (v < -0x1F0) v = -0x1F0; if (v > 0x1F0) v = 0x1F0;
    return (uint16_t)(0x200 + v);
}
__attribute__((constructor)) static void gw_controls_init(void) { s23_asca_analog = gw_analog; }

/* THE BATTLEFIELD-SELECT PREVIEWS (2026-11-01, against the real machine: youtube MjUfpDo-pEc 436-454 s and the user's photos).
 * The real screen is a grey card with three windows, each showing its battlefield live. The game sends, between C435-interrupt models
 * (model 0, which split the list into stretches): the card (models 0x0C55..0x0CA7, laid out in screen pixels at depth 554), then the
 * shopping, waterfront and subway scenes, each a FULL-SCREEN view -- nothing in the command stream says where the windows are (no
 * viewport, clip or register change between them; checked every word), so how the board windows them is unknown. Here: the card gets
 * the 1:1 projection (fov 554.25, the game's own HUD setting), and the k-th scene after it is centred in window k, scaled so the window
 * shows y/z = +-0.375 (the select screen's footage: the shop sign is 0.72 the size of our full-screen one = window height / 240) and
 * clipped to the window. The "<battlefield> SELECTED" summary is the same card plus the 3D map (models 0x0080-0x009F) and one window.
 * $GW_PREVIEW=0 turns it off; $GW_PREVIEW_SCALE=<f> scales the game's own projection instead (tests). */
#include "../../s23/src/s23_c435.h"
/* THE CAMERA SCREEN ("POSE WITHIN THE FRAME", footage 460-462 s): on the real cabinet the GMEN board shows the live camera picture
 * inside an orange frame -- graphics the main board does not draw (no 3D, only the countdown in its text layer) and nobody emulates.
 * With no camera here: the orange frame with the video area greyed out (the user's choice), while the camera step shows no 3D.
 * gw_cam_phase: 1 = the main CPU began asking the GMEN board for the snapshot, 2 = its empty (camera) frames are showing; back to 0 at
 * the next screen with 3D (team select). $GW_CAMFRAME=0 turns it off. */
int gw_cam_phase;
/* THE NAMCAM SOURCE (the menu's NamCam page, saved as namcam_source): 0 Auto = photo.bmp if there is one, else the webcam, else grey;
 * 1 Webcam; 2 Photo file (photo.bmp beside the program); 3 Off (grey). The webcam (src/gw_cam.c) runs only while the camera screen shows:
 * its live picture fills the frame's box, and the picture at the FLASH (the camera step ends) becomes the player's photo */
#include "eng_cfg.h"
#include "eng_ui.h"
void gw_cam_start(void), gw_cam_stop(void); int gw_cam_frame(uint32_t *dst), gw_cam_failed(void);
static int namcam_src = -1;
static uint32_t photo_file[128 * 128]; static int have_file;   /* photo.bmp, 128 x 128 */
static uint32_t live[128 * 128]; static int have_live;          /* the webcam's latest */
static int src_now(void) { if (namcam_src < 0) namcam_src = eng_cfg_int("namcam_source", 0) & 3; return namcam_src; }
static int use_file(void) { const int s = src_now(); return have_file && (s == 0 || s == 2); }
static int use_cam(void) { const int s = src_now(); return s == 1 || (s == 0 && !have_file); }
/* the photo page has a 128 x 128 square per cabinet, by PCB ID, in a row: cabinet n at u = 128 n (measured with 3 linked: 0, 128, 256) */
void gw_photo_slot(int slot, const uint32_t *img)
{
    if (slot < 0 || slot >= S23_PHOTO_W / 128) return;
    const int u0 = slot * 128, v0 = 0;
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) s23_photo_tex[(v0 + y) * S23_PHOTO_W + u0 + x] = img ? img[y * 128 + x] : 0x808080;
    s23_photo_gen++;
}
static void slot_set(const uint32_t *img)                       /* this player's square; linked, the others get it over the cable */
{
    int gw_link_nodes(void), gw_link_node(void); void gw_link_photo(const uint32_t *);
    gw_photo_slot(gw_link_nodes() > 0 ? gw_link_node() : 0, img);
    gw_link_photo(img);
}
#include "gw_namcam_text.h"
static uint32_t cam_phase_at, cam_seen_at;                                   /* the frame the camera screen began */
static uint32_t cam_stop_at;                                    /* the frame to free the webcam (0 = none due) */
static void photo_apply(void);
static void gw_camera_overlay(s23_render_list *l)
{
    static int off = -1; if (off < 0) { const char *e = getenv("GW_CAMFRAME"); off = e && *e == '0'; }
    extern uint32_t rr_frame;
    /* the camera step has short stretches with 3D in it: the webcam stays on until 3 s after it last ended (no on / off flicker) */
    if (cam_stop_at && rr_frame >= cam_stop_at) { cam_stop_at = 0; gw_cam_stop(); }
    /* THE GAME'S OWN CAMERA SCREEN (2026-11-09; with the board's per-frame IRQ6, src/gw_gmen.c, the main program draws it itself, as the
     * arcade video: model 0x0CC8 = the orange frame + captions, then the video box = a photo-page immediate (pal FB00) -- the camera board
     * wrote its LIVE picture into the photo page). So: while 0x0CC8 is up, the webcam (or photo file) goes into this cabinet's photo square
     * each frame; when it goes (the white flash) the last picture is the photo, and the other cabinets get it. The old flow below (the
     * screen empty of 3D, our own frame drawn) runs only with $GW_IRQ6VB=0 */
    { int on = 0; for (int i = 0; i < l->count && !on; i++) on = l->e[i].type == S23_RE_MODEL && l->e[i].model == 0x0CC8;
      static int cam_on;
      int gw_link_nodes(void), gw_link_node(void); const int own = gw_link_nodes() > 0 ? gw_link_node() : 0;
      if (on) {
          if (!cam_on) { cam_on = 1; cam_stop_at = 0; gw_photo_slot(own, use_file() ? photo_file : NULL); if (use_cam()) gw_cam_start();
                         fprintf(stderr, "[CAM] f%u: camera screen\n", rr_frame); }
          if (use_cam() && gw_cam_frame(live)) gw_photo_slot(own, live);
          gw_cam_phase = 0; return;
      }
      if (cam_on) {
          cam_on = 0; cam_stop_at = rr_frame + 180;
          if (use_cam() && gw_cam_frame(live)) { slot_set(live); fprintf(stderr, "[CAM] f%u: photo taken\n", rr_frame); }
          else photo_apply();
          gw_cam_phase = 0; return;
      } }
    { int gw_gmen_irq6vb(void); if (gw_gmen_irq6vb()) { gw_cam_phase = 0; return; } }   /* (the old flow's "empty screen" test misfires here) */
    if (!gw_cam_phase) return;
    if (l->count) {                                             /* the flash: the camera step is over */
        if (gw_cam_phase == 2) {
            gw_cam_phase = 0; fprintf(stderr, "[CAM] f%u: 3D back (%d entries)\n", rr_frame, l->count);
            if (use_cam() && gw_cam_frame(live)) { slot_set(live); fprintf(stderr, "[CAM] f%u: photo taken\n", rr_frame); }
            cam_stop_at = rr_frame + 180;
        }
        return;
    }
    if (gw_cam_phase != 2) { fprintf(stderr, "[CAM] f%u: camera screen (no 3D)\n", rr_frame); gw_cam_phase = 2; cam_stop_at = 0; if (rr_frame - cam_seen_at > 600) cam_phase_at = rr_frame; if (use_cam()) gw_cam_start(); }   /* (a new camera screen, not its short breaks) */
    cam_seen_at = rr_frame;
    if (off) return;
    static const s23_rect r[5] = {
        { 149, 28, 492, 35, 0xE04818 }, { 149, 367, 492, 374, 0xE04818 },   /* the orange frame: top, bottom */
        { 149, 28, 156, 374, 0xE04818 }, { 485, 28, 492, 374, 0xE04818 },   /* left, right */
        { 189, 44, 459, 300, 0x808080 } };                                  /* the video area, greyed out */
    /* THE CAPTIONS (the real machine, the user's photos): first "顔を撮影します" (your face will be photographed) alone in the frame for
     * ~2 s, then the picture with "顔の位置を合わせてください" (line up your face) under it until the countdown flashes */
    const int first = rr_frame - cam_phase_at < 120;
    for (int i = 0; i < (first ? 4 : 5) && l->nrect < S23_RECTS; i++) l->rect[l->nrect++] = r[i];
    if (first) {
        l->mask[0] = (s23_mask){ (int16_t)(320 - gw_txt_face_w / 2), 120, (int16_t)gw_txt_face_w, (int16_t)gw_txt_face_h, 0xF8C028, gw_txt_face };
        l->mask[1] = (s23_mask){ (int16_t)(320 - gw_txt_shoot_w / 2), 196, (int16_t)gw_txt_shoot_w, (int16_t)gw_txt_shoot_h, 0xF0F0F0, gw_txt_shoot };
        l->nmask = 2;
        return;
    }
    l->mask[0] = (s23_mask){ (int16_t)(320 - gw_txt_align_w / 2), 318, (int16_t)gw_txt_align_w, (int16_t)gw_txt_align_h, 0xF8B820, gw_txt_align };
    l->nmask = 1;
    /* the picture in the box (square, centred in it): the webcam live, or the photo file */
    const uint32_t *pic = NULL;
    if (use_cam() && gw_cam_frame(live)) pic = live; else if (use_file()) pic = photo_file;
    if (pic) { l->img_on = 1; l->img_x0 = 324 - 128; l->img_x1 = 324 + 128; l->img_y0 = 44; l->img_y1 = 300; memcpy(l->img, pic, sizeof l->img); }
}
/* photo.bmp beside the program (read once, at the first frame: the program is in its own folder by then), centre-cropped to a square */
int gw_image_load(const char *path, uint32_t *dst);              /* src/gw_cam.c: Windows' image decoders */
static char photo_name[32];                                       /* the file found */
static void gw_photo_load(void)
{
    /* (a linked copy on one PC runs in its own folder, $S23_CABINET_DIR: the photo beside the .exe, one up, too) */
    static const char *names[] = { "photo.jpg", "photo.jpeg", "photo.png", "photo.bmp", "photo.avif", "photo.webp", "photo.heic", "photo.gif",
                                   "../photo.jpg", "../photo.jpeg", "../photo.png", "../photo.bmp", "../photo.avif", "../photo.webp", "../photo.heic", "../photo.gif" };
    const unsigned nn = getenv("S23_CABINET_DIR") ? sizeof names / sizeof *names : 8;
    for (unsigned k = 0; k < nn; k++) {
        FILE *t = fopen(names[k], "rb"); if (!t) continue; fclose(t);
        if (gw_image_load(names[k], photo_file)) { have_file = 1; snprintf(photo_name, sizeof photo_name, "%s", names[k]); fprintf(stderr, "[GMEN] %s loaded as the player's photo\n", names[k]); return; }
        SDL_Surface *s = SDL_LoadBMP(names[k]);                    /* (no Windows decoder: a plain BMP still works) */
        if (s) { SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB888, 0); SDL_FreeSurface(s);
            if (c) { const int side = c->w < c->h ? c->w : c->h, ox = (c->w - side) / 2, oy = (c->h - side) / 2;
                for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++)
                    photo_file[y * 128 + x] = ((const uint32_t *)((const uint8_t *)c->pixels + (size_t)(oy + y * side / 128) * (size_t)c->pitch))[ox + x * side / 128] & 0xFFFFFF;
                SDL_FreeSurface(c); have_file = 1; snprintf(photo_name, sizeof photo_name, "%s", names[k]); fprintf(stderr, "[GMEN] %s loaded as the player's photo\n", names[k]); return; } }
        fprintf(stderr, "[GMEN] %s: not a picture this PC can read (its contents decide, not its name)\n", names[k]);
        snprintf(photo_name, sizeof photo_name, "%s (unreadable)", names[k]);
    }
}
static void photo_apply(void) { slot_set(use_file() ? photo_file : NULL); }   /* the slot before / without a webcam picture */
/* SKIP THE BOOT NOTICE (the user's option, saved as skip_notice, default off): the "THIS GAME IS FOR USE EXCLUSIVELY IN JAPAN" screen
 * counts 25 s on a frame counter at 0x80333A7A (it ends at 1500 = 25 x 60; found by RAM diffs, tested by setting it). Once, while the
 * notice is up, the counter is moved to its last half second. */
static int skip_notice = -1;
static void gw_notice_skip(void)
{
    if (skip_notice < 0) skip_notice = eng_cfg_int("skip_notice", 0) != 0;
    static int done; extern uint32_t rr_frame; extern uint8_t s23_ram[];
    if (!skip_notice || done || rr_frame > 4000) return;
    const uint32_t o = 0x333A7A; const unsigned v = (unsigned)s23_ram[o] << 8 | s23_ram[o + 1];
    if (v >= 30 && v < 1400) { s23_ram[o] = 1470 >> 8; s23_ram[o + 1] = 1470 & 0xFF; done = 1; fprintf(stderr, "[GMEN] boot notice skipped\n"); }
}
/* ---- the menu's Options page: the NamCam source, the boot notice ---- */
static int fps_cross = -1;                                        /* Options "Crosshair (FPS)": 0 hidden (default), 1 shown */
static int gw_fps_cross(void) { if (fps_cross < 0) fps_cross = eng_cfg_int("fps_crosshair", 0) != 0; return fps_cross; }
static int gw_hide_crosshair(void) { return gw_fps_layout() && !gw_fps_cross(); }
/* THE NAMCO LOGO (attract): a ball rolls out and unrolls into the letters, which turn RED-ORANGE the moment they lie flat (arcade footage:
 * video 1 at 159.6-160.0 s, the Japanese set's video at 3:23). The ball / letters are 3-corner immediates (type 0x33C0, h 0x12E3, palette
 * 0x0C) reflecting a dusk-sky panorama; ours read it as given and the letters stayed grey-blue (the sky above the sunset). The hardware
 * rule is not known (palette, C404, text layer, texture chips: all constant). Matched to the footage: read 0x90 rows lower (into the
 * sunset glow) at double brightness -> the arcade's bright ball and red-orange letters. Those polygons only. $GW_LOGOFIX=0 off */
static void gw_imm_adjust(const void *iv, float *v_add, int *boost, int *uv_pal)
{
    const s23_immediate *q = iv;
    static int off = -1, flat_v = 0, ball_v = 0x90;
    if (off < 0) { const char *e = getenv("GW_LOGOFIX"); off = e && *e == '0';
        if ((e = getenv("GW_LOGOV"))) sscanf(e, "%x,%x", &ball_v, &flat_v); }   /* (tuning: ball,flat row offsets, hex) */
    if (off || (q->type & 0xFFFF) != 0x33C0 || (q->h & 0xFFFF) != 0x12E3 || (q->pal & 0x7F00) != 0x0C00) return;
    int flat = 1; for (int i = 0; i < 3; i++) flat &= (q->u[i] & 0xFFFF) == 0x0A00 && (q->v[i] & 0xFFFF) == 0x7100;
    if (flat) { uv_pal[0] = 0x370; uv_pal[1] = 0x1E30; uv_pal[2] = 0x7A; return; }   /* flat letters: the logo's own red, as its (R) mark (pal FA00) */
    *v_add = (float)ball_v; *boost = 1;
}
static int wide_pillar = -1;                                      /* Options "Widescreen outside matches": 1 = 4:3 with black bars (default) */
static int gw_wide_pillar(void) { if (wide_pillar < 0) wide_pillar = eng_cfg_int("wide_pillar", 1) != 0; return wide_pillar; }
static int nc_rows(void) { return 6; }
static void nc_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    static const char *names[4] = { "Auto", "Webcam", "Photo file", "Off" };
    if (r == 0) { snprintf(l, ln, "NamCam photo"); snprintf(v, vn, "%s", names[src_now()]); return; }
    if (r == 5) { snprintf(l, ln, "Widescreen outside matches"); snprintf(v, vn, "%s", gw_wide_pillar() ? "4:3 with black bars (the demo's sets end at the 4:3 edges)" : "full width"); return; }
    if (r == 4) { snprintf(l, ln, "Crosshair (FPS)"); snprintf(v, vn, "%s", gw_fps_cross() ? "shown" : "hidden (the mouse turns; nothing to point at)"); return; }
    if (r == 3) { snprintf(l, ln, "Mouse sensitivity"); snprintf(v, vn, "%d  (FPS layout; 1 slow - 10 fast)", gw_fps_sens()); return; }
    if (r == 2) { snprintf(l, ln, "Controls"); snprintf(v, vn, "%s", gw_fps_layout() ? "FPS (the mouse turns)" : "Cursor (push to a side to turn)"); return; }
    if (skip_notice < 0) skip_notice = eng_cfg_int("skip_notice", 0) != 0;
    snprintf(l, ln, "Boot notice"); snprintf(v, vn, "%s", skip_notice ? "skip (straight to the title)" : "show (25 s, as the arcade)");
}
static void nc_change(int r, int dir)
{
    if (r == 0) { namcam_src = (src_now() + (dir < 0 ? 3 : 1)) & 3; eng_cfg_set_int("namcam_source", namcam_src); photo_apply(); return; }
    if (r == 5) { wide_pillar = !gw_wide_pillar(); eng_cfg_set_int("wide_pillar", wide_pillar); return; }
    if (r == 4) { fps_cross = !gw_fps_cross(); eng_cfg_set_int("fps_crosshair", fps_cross); return; }
    if (r == 3) { fps_sens = gw_fps_sens() + (dir < 0 ? -1 : 1); if (fps_sens < 1) fps_sens = 1; if (fps_sens > 10) fps_sens = 10; eng_cfg_set_int("fps_sensitivity", fps_sens); return; }
    if (r == 2) { fps_layout = !gw_fps_layout(); eng_cfg_set_int("fps_layout", fps_layout); gw_mouse_capture(); return; }
    skip_notice = !skip_notice; eng_cfg_set_int("skip_notice", skip_notice);
}
static void nc_notes(void (*line)(const char *, ...))
{
    line("NamCam: your picture, taken before team select.");
    line("Auto = photo.jpg/.png/.bmp beside the .exe, else webcam.");
    line("Photo file: %s   Webcam: %s", photo_name[0] ? photo_name : "none", gw_cam_failed() ? "none found" : "ready");
    line("Boot notice: takes effect at the next start.");
}
static const eng_ui_page namcam_page = { "Options", 540, 150, 26, nc_rows, NULL, NULL, nc_text, nc_change, nc_notes };
extern const eng_ui_page gw_link_page;   /* src/gw_link.c */
static void gw_menu_pages(void) { eng_ui_add_page(&namcam_page); eng_ui_add_page(&gw_link_page); }
/* the widescreen HUD shift only while the in-game HUD is up (its panel models 3155 / 3156): the select / team / result screens are also
 * drawn in the screen plane, and pushing their panels apart split them past the 4:3 picture (the user, 2026-11-08) */
/* the blue suit-up's cheek plate (s23_game.h zbias_scale) */
static int gw_zbias_scale(int model) { return model == 0x0AD7 ? 4 : 1; }

static int gw_hud_wide_ok(const void *lv)
{
    const s23_render_list *hl = (const s23_render_list *)lv;
    for (int i = 0; i < hl->count; i++) if (hl->e[i].type == S23_RE_MODEL && (hl->e[i].model == 3155 || hl->e[i].model == 3156)) return 1;
    return 0;
}

static void gw_list_hook(void *lv)
{
    { static int once; if (!once) { once = 1; gw_photo_load(); photo_apply(); } }
    gw_notice_skip();
    { void gw_test_pokes(void); gw_test_pokes(); }   /* TEST switches kept outside the saved state (src/gw_link.c) */
    { static int hl = -1; if (hl < 0) hl = getenv("GW_HUDLOG") != NULL;   /* $GW_HUDLOG: each screen-plane model id / x (first time seen) */
      if (hl) { const s23_render_list *l = lv; static uint8_t seen[0x4000][2]; extern uint32_t rr_frame;
        for (int i = 0; i < l->count; i++) { const s23_render_entry *e = &l->e[i];
            if (e->type != S23_RE_MODEL || e->vp_fov <= 0.f || e->v[2] < e->vp_fov - 8.f || e->v[2] > e->vp_fov + 8.f || e->model >= 0x4000) continue;
            const int side = e->v[0] < 0 ? 0 : 1; if (seen[e->model][side]) continue; seen[e->model][side] = 1;
            fprintf(stderr, "[HUD] f%u model %04X x %d y %d\n", rr_frame, e->model, (int)e->v[0], (int)e->v[1]); } } }
    { /* THE WIDESCREEN HUD (engine: screen-plane models slide to the edges, src/s23_render.c): the counters' diamond and digits are TEXT
       * (the C361 layer) over the 3D panels (models 0x0E43 / 0x0E44 at x -+232): while those panels are up, their bands of text move too */
      s23_render_list *hl = (s23_render_list *)lv; extern int g_eng_hud_edges_on; int s23_gl_wide(void); const int ex = s23_gl_wide();
      if (g_eng_hud_edges_on && ex > 0) {
          int panels = 0;
          for (int i = 0; i < hl->count; i++) if (hl->e[i].type == S23_RE_MODEL && (hl->e[i].model == 3155 || hl->e[i].model == 3156)) panels = 1;
          if (panels) { static const int16_t r[2][5] = { { 0, 16, 200, 72, 0 }, { 440, 16, 640, 72, 0 } };
                        memcpy(hl->tshift, r, sizeof r); hl->tshift[0][4] = (int16_t)-ex; hl->tshift[1][4] = (int16_t)ex; hl->ntshift = 2; } } }
    /* and in the test menu (OTHERS page, the ROM versions): "S23" after ROM1's build date, in the board's own text -- the C361 tile map
     * (0x0681E000, 64 x 64 big-endian words, the code = ASCII) holds "ROM1 VER." at row 4, the date at row 5 ending "PM" at columns 32-33 */
    { uint8_t *s23_vram_ptr(uint32_t); uint8_t *tm = s23_vram_ptr(0x0681E000u);
      static const char lab[] = "ROM1 VER.";
      int ok = 1; for (int i = 0; i < 9 && ok; i++) ok = (tm[2 * (4 * 64 + 3 + i) + 1] == (uint8_t)lab[i]) && ((tm[2 * (4 * 64 + 3 + i)] & 0x03) == 0);
      if (ok && tm[2 * (5 * 64 + 32) + 1] == 'P' && tm[2 * (5 * 64 + 33) + 1] == 'M') {
          const uint8_t hi = tm[2 * (5 * 64 + 32)] & 0xFC;                  /* the date's palette / flips */
          static const char tag[] = "S23";
          for (int i = 0; i < 3; i++) { tm[2 * (5 * 64 + 35 + i)] = hi; tm[2 * (5 * 64 + 35 + i) + 1] = (uint8_t)tag[i]; } } }
    /* the build's mark (the user's choice): two white PAC-MEN -- the board's own power-on test Pac-Man -- chomping either side of the boot
     * NOTICE's countdown (x 306-330, y 402-413) while it runs (counter 0x80333A7A, see gw_notice_skip) */
    { extern uint32_t rr_frame; extern uint8_t s23_ram[]; s23_render_list *l = (s23_render_list *)lv;
      const unsigned v = (unsigned)s23_ram[0x333A7A] << 8 | s23_ram[0x333A7B];
      if (rr_frame < 4000 && v > 0 && v < 1500 && l->nmask + 2 <= S23_MASKS) {
          static const int seq[4] = { 0, 1, 2, 1 };                  /* wide, half, closed, half */
          const int k = seq[(v / 15) & 3];                          /* one chomp a countdown second (the counter: 60 a second) */
          static const unsigned char *const r[3] = { gw_pac0, gw_pac1, gw_pac2 }, *const m[3] = { gw_pacm0, gw_pacm1, gw_pacm2 };
          l->mask[l->nmask++] = (s23_mask){ 285, 401, 13, 13, 0xFFFFFF, r[k] };
          l->mask[l->nmask++] = (s23_mask){ 338, 401, 13, 13, 0xFFFFFF, m[k] }; } }
    { static unsigned long pa = 1, pv, pf; if (pa == 1) { const char *e = getenv("GW_POKE"); pa = 0; if (e) sscanf(e, "%lx:%lu@%lu", &pa, &pv, &pf); }   /* TEST: addr:value(16-bit)@frame */
      extern uint32_t rr_frame; extern uint8_t s23_ram[]; if (pa && rr_frame == pf) { const uint32_t o = (uint32_t)pa & 0x3FFFFF; s23_ram[o] = (uint8_t)(pv >> 8); s23_ram[o + 1] = (uint8_t)pv; } }
    { static long at = -2; if (at == -2) { const char *e = getenv("GW_PHOTODUMP"); at = e ? atol(e) : -1; }   /* debug: the shared RAM at frame n */
      extern uint32_t rr_frame; if (at >= 0 && (long)rr_frame == at) { FILE *f = fopen("gw_photo.bin", "wb"); if (f) { fwrite(gw_gmen_ram, 1, 0x10000, f); fclose(f); }
          uint8_t *s23_vram_ptr(uint32_t); const uint8_t *pal = s23_vram_ptr(0x06A10000u);   /* + palette 0x7E (R, G, B planes) */
          f = fopen("gw_pal7e.txt", "w"); if (f) { for (int n = 0x7E00; n < 0x7F00; n++) { const uint32_t o = 2 * n + 1; fprintf(f, "%02X%02X%02X%c", pal[o], pal[0x10000 + o], pal[0x20000 + o], (n & 15) == 15 ? '\n' : ' '); } fclose(f); } } }
    s23_render_list *l = (s23_render_list *)lv;
    gw_camera_overlay(l);
    static int off = -1, scale_set; static float scale = 1.f;
    if (off < 0) { const char *e = getenv("GW_PREVIEW"); off = e && *e == '0'; e = getenv("GW_PREVIEW_SCALE"); if (e) { scale = (float)atof(e); scale_set = 1; } }
    if (off) return;
    int card = -1, map = 0;
    for (int i = 0; i < l->count; i++)
        if (l->e[i].type == S23_RE_MODEL && (l->e[i].model == 0x0CA6 || l->e[i].model == 0x0CA7)) { card = l->e[i].seg; break; }
    if (card < 0) return;
    for (int i = 0; i < l->count; i++)                /* the 3D map with the card = the "<battlefield> SELECTED" summary */
        if (l->e[i].type == S23_RE_MODEL && l->e[i].seg == card && l->e[i].model >= 0x0080 && l->e[i].model < 0x00A0) { map = 1; break; }
    /* the windows in screen pixels: the select screen's three, the summary's one (its left half, orange bar to the grid line) */
    static const short sel[3][4] = { { 34, 162, 225, 335 }, { 225, 162, 415, 335 }, { 415, 162, 605, 335 } }, sum[1][4] = { { 32, 118, 272, 367 } };
    const short (*w)[4] = map ? sum : sel; const int nw = map ? 1 : 3;
    for (int i = 0; i < l->count; i++) {
        s23_render_entry *e = &l->e[i];
        if (e->type != S23_RE_MODEL) continue;
        if (e->seg == card) { e->vp_fov = 554.25f; continue; }
        const int k = e->seg - card - 1;
        if (k < 0 || k >= nw) continue;
        const int hx = (w[k][2] - w[k][0]) / 2, hy = (w[k][3] - w[k][1]) / 2, cx = w[k][0] + hx, cy = w[k][1] + hy;
        e->vp_size_x = (int16_t)hx; e->vp_size_y = (int16_t)hy;
        e->vp_offset_x = (int16_t)(cx - 320); e->vp_offset_y = (int16_t)(240 - cy);
        /* a window shows the same vertical slice of the view whatever its size: half-height / 0.375 (select screen: 0.72 x 320) */
        e->vp_fov = scale_set ? e->vp_fov * scale : (float)hy / 0.375f;
    }
}

extern const s23_link_ops gw_link_ops;
const s23_game_t s23_game = {
    .gun_x0 = 0, .gun_xw = 0x3FF, .gun_y0 = 0, .gun_yh = 0x3FF,   /* the aim drives the gun's turn (above) */
    .title = "Gunmen Wars", .tag = "gunwars", .window_title = "Gunmen Wars", .mame_set = "gunwars",
    .zip_help = "MAME's gunwars.zip (Gunmen Wars, Japan GM1 Ver. B) and namco_asca5.zip",
    .zips = zips, .nzips = 2, .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .twin = 0, .evo2 = 0,
    .clock = 169344000u,                          /* BUSCLOCK * 5, as System 23 */
    .px_num = 200, .px_den = 1323,                /* 25.6 MHz / 169.344 MHz */
    .sub_div = 10,                                /* 16.9344 MHz */
    .io_num = 64, .io_den = 735,                  /* 14.7456 MHz */
    .c352_div = 1920,                             /* 88200 Hz */
    .ip = { [S23_SRC_VBLANK] = S23_IP(0), [S23_SRC_C361] = S23_IP(1), [S23_SRC_SUB] = S23_IP(1), [S23_SRC_C435] = S23_IP(2),
            [S23_SRC_C422] = S23_IP(3), [S23_SRC_TIMER] = S23_IP(5) },
    .boot_size = 0x400000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x008E, .depth_default = 0,
    .data_rom = { { "gm1mtah.2j", "gm1mtal.2h" }, { NULL, NULL } },
    .npt = 2, .pt_rom = { { "gm1pt0h.7a", "gm1pt0l.7c" }, { "gm1pt1h.5a", "gm1pt1l.5c" } },
    .ccrl = "gm1ccrl.7f", .ccrh = "gm1ccrh.7e", .cg = { "gm1cgll.4m", "gm1cglm.4k", "gm1cgum.4j", "gm1cguu.5f" },
    .wave = { "gm1wave.2c", NULL },
    .hide_crosshair = gw_hide_crosshair, .wide_pillar = gw_wide_pillar, .imm_adjust = gw_imm_adjust,
    .no_smooth = 0, .io_asca = 1, .mem_read = gw_read, .mem_write = gw_write,
    .list_hook = gw_list_hook,
    .hud_wide_ok = gw_hud_wide_ok,
    .zbias_scale = gw_zbias_scale,
    .model_fov_max = 1, .imm_right = 1, .no_xycull = 1,
    .photo_page = 0x1F000,
    .input = &gw_input, .mounted_gun = 1, .menu_pages = gw_menu_pages,       /* its own Controls page; no light-gun extras on the Display page */                        /* the players' photos: the GMEN board supplies this texture page (gw_photo_init) */
    /* FIGHTER VIEW (Display menu, default = behind your fighter, the user's choice): the game draws your own body 1.1-1.45 units in front
     * of the eye; "first person" clips it with this near plane (the footage, 484-553 s, never shows your fighter; the ground at the
     * screen's bottom edge starts ~1.9 away) */
    /* .znear_model = 1.5f -- the "Fighter view: first person" option, REMOVED (the user, 2026-11-06: it is broken); always behind the fighter */
    .idle = { { 0x8000065Cu, 0x80333CB4u }, { 0x8000098Cu, 0x80333CB4u } },   /* the main loops' vblank waits: lw v0,-0x689C(gp); beqz (src/s23_cpu.c) */
    .link = &gw_link_ops,                         /* the cabinet link: the GMEN boards' 1394 bus between PCs (src/gw_link.c, LINK.md) */
};
