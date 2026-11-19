/* s23_game.h -- ONE GAME = ONE TABLE. The System 23 host in tc2/src is shared by every System 23 / Evolution 2 game (HARD RULE 3: settings and
 * small hooks, never copies). Time Crisis 2 fills it in src/s23_game.c, Crisis Zone in ../crszone/src/csz_game.c; each binary links one.
 * Everything a game differs in that the host must know is here: the ROM files, the board (System 23 or Evolution 2), the clocks, which
 * CPU interrupt pin each device drives. */
#ifndef S23_GAME_H
#define S23_GAME_H
#include <stdint.h>
#include <stddef.h>
#include "eng/romzip.h"

/* the board's interrupt SOURCES (MAME namcos23.cpp MAIN_*_IRQ); each game's table says which Cause bit (IP2..IP7) a source drives */
enum { S23_SRC_VBLANK, S23_SRC_C361, S23_SRC_SUB, S23_SRC_C435, S23_SRC_C422, S23_SRC_C450, S23_SRC_C451, S23_SRC_RS232, S23_SRC_TIMER, S23_SRC_N };
#define S23_IP(n) (0x400u << (n))                 /* the CPU's IRQn pin = Cause.IP(n+2); the CP0 timer is IP7 = S23_IP(5) */

struct s23_sprite;                                /* (spr_wide, below; src/s23_frame.h) */
typedef struct {
    const char *title;                            /* "Time Crisis 2": windows, messages */
    const char *tag;                              /* "tc2": the cfg / backup / log file names */
    int cz_fog;                                   /* 1 = draw the C412 depth fog (CZ flag + PCZ table, Super 22's model: src/s23_render.c). Angler King on;
                                                   * TC2 asks for it too (frame 3720's forest) but stays off until checked (the author's 0.10) */
    int uv_edge;                                  /* 1 = model polygons sample their texture range as written (no +0.5 texel, MAME's): Angler King's water seams */
    const char *subtitles;                        /* NULL, or the English subtitle table beside the game (src/s23_subs.c): Angler King's captions are pictures */
    int io_asca3;                                 /* 1 = the I/O board is an ASCA-3 / FCA-1 (Angler King: switches 0x6000-3, outputs 0x6004/5, rotary 0x6006/7) */
    int super23;                                  /* 1 = SUPER System 23 (Angler King): a write to 0x0100005C acknowledges the C361 (raster) interrupt */
    const char *window_title;                     /* "Time Crisis II" */
    const char *mame_set;                         /* "timecrs2": the set name the output protocol announces (recoil hooks key on it) */
    const char *zip_help;                         /* what the player must supply, for the missing-ROM message */
    const char *const *zips; int nzips;           /* the MAME zips searched (a split set keeps a device ROM in its own zip) */
    const eng_rom_t *chips; int nchips;           /* every ROM file: name, size, CRC (src/eng/romzip.c) */

    int twin;                                     /* 1 = a TWIN-CABINET game (Time Crisis 2: Left/Red + Right/Blue on the C422 RS-422 link): the menu's
                                                   * Cabinet choice (the I/O board's Link ID) and online play (src/s23_link.c) */
    int evo2;                                     /* 1 = System 23 EVOLUTION 2: the chips at +0x10000000, 29-bit decode, C450 DMA, IRQ status regs */
    const struct s23_gorgon_ops *gorgon;          /* Namco GORgON ("System 22.5": Rapid River, Final Furlong): &s23_gorgon (src/s23_gorgon.c, linked
                                                   * by those games only, so other games' builds and save states are untouched): System 23's chips
                                                   * at other addresses (folded), + sprites (C435 DMA in sprite mode, C404 sprite RAM) and the CZ RAM */
    uint32_t clock;                               /* the main CPU, Hz (cycles as MAME charges them) */
    uint32_t px_num, px_den;                      /* pixel clocks (25.6 MHz) per main cycle, exactly */
    uint32_t sub_div;                             /* main cycles per sub-CPU (H8/3002) state */
    uint32_t io_num, io_den;                      /* I/O board (H8/3334) states = main cycles * io_num / io_den */
    uint32_t c352_div;                            /* main cycles per C352 sample (88200 Hz) */
    uint32_t ip[S23_SRC_N];                       /* the Cause bit each source drives (0 = not wired) */
    int irq_prompt;                               /* 1 = an interrupt a device raises mid-slice is polled at once (MAME takes it at the next instruction);
                                                   * 0 = at the end of the slice, as Time Crisis 2's gates were measured with. Crisis Zone waits on ~550
                                                   * C450 DMA interrupts a frame: a slice's delay each (up to S23_SLICE instructions) halved its frame rate */

    /* the program images, made from the ROM files at start-up (tools/make_prog.py writes the same for the translators) */
    uint32_t boot_size;                           /* the boot ROM as the CPU sees it, ending at 0x1FFFFFFF (4 MB System 23, 8 MB Evolution 2) */
    int (*rom_boot)(const char *dir, uint8_t *dst);
    int (*rom_sub)(const char *dir, uint8_t *dst, size_t n);
    int (*rom_io)(const char *dir, uint8_t *dst, size_t n);
    uint32_t io_rom_size;                         /* how much of the I/O board's program its H8 maps */
    int direct_off_bit;                           /* 1 = a C435 direct quad whose colour word (d[2]) has bit 15 set is not drawn. Crisis Zone: its direct buffer
                                                   * (0x1CC00000, which MAME leaves unmapped) carries the attract demo's RULES panels ("PRESS PEDAL : GO&SHOOT",
                                                   * "OUT OF AMMO"...: bit 15 clear) beside a full-screen quad and two letterbox bars it keeps every frame with
                                                   * bit 15 set -- drawn, they covered the namco / GASHIN logos and the story's sky, which the arcade shows */
    int text_xscroll_from;                        /* optional: the text layer's x scroll only from this screen row down (0 = all rows,
                                                   * MAME). Crisis Zone 448: the zone select writes its bottom ticker's scroll for the whole
                                                   * picture (in vblank, no raster split) yet only the ticker moves on the arcade */
    int (*over_text)(int model);                  /* optional: 1 = this model's polygons stay over the text layer (prioverchar 7, as an
                                                   * immediate's cmode 1). Crisis Zone: the HUD's bullet / life icons over the green bar (arcade footage) */
    int (*zbias_scale)(int model);                /* a model's polygons' z-bias word multiplied by this when sorting (NULL / 0 = 1, as MAME). Gunmen Wars:
                                                   * the blue suit-up's cheek plate 0x0AD7 (bias -0x320) sits on the same surface as the dark head layer;
                                                   * at x1 the two near-tie and the plate blinks out (black), x4 holds it as in the arcade footage. NOT global:
                                                   * x4 everywhere pulled the chest doors' frames (bias -5000) over the closed chest plate */
    int text_no_range;                            /* 1 = the C404's pen range (0x12..0x13) does not make text see-through, only the 0x14 nibble (Rapid River:
                                                   * range EC..EC hit the HOW TO PLAY banner's red, 0x7FEC -- see-through, scenery edges showing through it; the cabinet's is solid) */
    int (*depth_role)(int model);                 /* optional, with exact depth: 1 = the model owns its pixels against role 2; 2 = never in the pre-pass,
                                                   * never drawn where a role-1 model is nearest (Rapid River: the wake's foam disc runs under the raft -- above
                                                   * its sunken floor, so exact depth put water inside the raft; painter order put foam there) */
    int depth_within;                             /* 2 = exact depth always, between models too (Rapid River: the pyramid's inner door, the wake's foam inside the raft);
                                                   * 1 = exact depth between one model's own polygons, always (the menu's Depth adds other models; forcing that on lost the Nile gate's roof) (Rapid River: the Nile gate's sloping
                                                   * sand, part of the wall's model, sorted over the wall's foot -- yellow wedges the arcade does not show) */
    int depth_default;                            /* Display > Depth when tc2.cfg has no choice yet: 1 = "exact" (Time Crisis 2), 0 = the arcade's order.
                                                   * Crisis Zone 0: with "exact" its hotel scene (frame 2280) loses the sky (the background colour shows where
                                                   * coverage proves the sky polygons drawn) and gains black bands top and bottom -- cause not found yet (NOT
                                                   * transparent texels: System 23 has none, only the stencil, which the pre-pass honours) */
    uint32_t beam_wait_lo, beam_wait_hi;          /* 0, or the game's routine that does nothing but wait on the beam counter (0x0682000A): its
                                                   * reads are fast-forwarded to the next line (src/s23_screen.c poll_skip). Crisis Zone's ticker wait */
    uint16_t c417_status;                         /* the C417 status read's fixed bits (MAME: System 23 0x008E; crszone_state::c417_status_r 0) | test mode << 15 */
    uint16_t gun_x0, gun_xw, gun_y0, gun_yh;      /* the light gun's port range (MAME JVS_SCREEN_POSITION_INPUT_X1/Y1 PORT_MINMAX): first value + width */
    uint8_t io_6001_held;                         /* bits of the I/O board's 0x6001 that read as PRESSED always (MAME namco_tss_io_device::in_r: bit 3 =
                                                   * BUTTON5, which crszone's port declares active-low and unused -> pressed; "Motor test shows NG if this is not pressed") */
    uint8_t sub_p7;                               /* what the sub-CPU reads on port 7 (MAME: timecrs2 0, crszone 0x80) */
    const char *data_rom[2][2];                   /* the data ROMs: [half][high, low] byte-interleaved */
    int npt; const char *pt_rom[4][2];            /* the point ROM chip pairs [high, low] */
    const char *ccrl, *ccrh, *cg[4];              /* texture tile maps and tiles */
    const char *spr[4];                           /* GORgON: the sprite tile ROMs (4 MB each, 32x32 8-bit tiles), NULL elsewhere */
    const char *wave[2];                          /* the C352's samples: low, high */
    int recoil_hold;                              /* 1 = 0x6002 bit 0 stays on WHILE the gun fires (Crisis Zone's machine gun: measured on a player's
                                                   * session, on for the whole burst), 0 = a 3-frame pulse per shot (Time Crisis 2) */
    int io_analog_out;                            /* 1 = the I/O board has MAME's namco_csz1 analog output at 0x6003 (Crisis Zone): published as analog0 */
    /* the SPEECH, on its own gain (Audio > Voice volume, GitHub #46): a C352 voice in speech_voices, or a key-on from a sample bank in
     * speech_bank_lo..speech_bank_hi (hi 0 = no range), is mixed as speech. Measured per game with C352_KEYLOG on a recorded session */
    uint32_t speech_voices;
    uint8_t speech_bank_lo, speech_bank_hi;
    /* the game's OWN hardware beside the System 23 chips (Gunmen Wars: the GMEN board): asked first for an address no System 23 chip
     * decodes (the 28-bit physical address); 1 = handled. NULL = none */
    int no_smooth;                                /* 1 = motion smoothing (in-between frames) off: Gunmen Wars crashed Intel's GL driver with it (cause not found yet) */
    int io_asca;                                  /* 1 = the I/O board is an ASCA (Gunmen Wars' ASCA-5): switches at 0x6000-3, controls on its A/D */
    int (*mem_read)(uint32_t p, int size, uint32_t *v);
    int (*mem_write)(uint32_t p, int size, uint32_t v);
    void (*list_hook)(void *list);
    int (*spr_wide)(const struct s23_sprite *s);  /* optional, GORgON: -1 / +1 = this sprite is HUD, moved out to that edge in widescreen; 0 = stays */
    int (*hide_crosshair)(void);                  /* optional: 1 = the crosshair is not drawn now (Gunmen Wars: the FPS layout, where the mouse turns
                                                   * and the crosshair's spot means nothing -- Options page "Crosshair (FPS)") */
    int (*hud_wide_ok)(const void *list);
    void (*imm_adjust)(const void *imm, float *v_add, int *boost, int *uv_pal);   /* imm = the s23_immediate (src/s23_c435.h); uv_pal[3] =
                                                   * u, v, palette to use instead (uv_pal[2] >= 0)   /* optional: an immediate polygon's texture v
                                                   * offset and 2x brightness (Gunmen Wars: the namco ball / letters, matched to the arcade footage) */
    int (*wide_pillar)(void);                     /* optional: 1 = outside the in-game HUD (hud_wide_ok 0) the whole picture stays 4:3 in widescreen
                                                   * (Gunmen Wars: its attract fly-throughs are sets built only as far as the 4:3 camera sees) */        /* the widescreen HUD shift applies to this list? (NULL = always; Gunmen Wars: only with the in-game HUD up) */
    void (*menu_pages)(void);                     /* optional: add the game's own pages to the Esc menu (eng_ui_add_page), after Controls */
    const void *input;                            /* optional: the game's own controls (an ss22_input_game: rows, keys, the Controls page notes);
                                                   * NULL = the light-gun cabinet's (src/s23_input.c: trigger, pedal, reload) */
    int mounted_gun;                              /* 1 = a mounted gun, not a light gun (Gunmen Wars): the Display page leaves out the light gun's
                                                   * shot flash and the recoil / lamp outputs */
    int photo_page;                               /* the texture page (polygon tbase) whose texels the GMEN camera board supplies, not the ROM
                                                   * (Gunmen Wars: 0x1F000, the players' photos); 0 = none. Drawn from s23_photo_tex (src/s23_render.c) */
    int no_xycull;                                /* 1 = keep translucent polygons with far off-screen corners (the Crisis Zone smoke rule, src/s23_render.c
                                                   * finish(), would drop Gunmen Wars' near water: the river went black under the camera) */
    int imm_right;                                /* 1 = C435 0.2 without bit 8 is t = m2 * transpose(immediate) (Gunmen Wars; src/s23_c435.c) */
    int model_fov_max;                            /* 1 = MODELS use the largest projection scale among the frame's clip sets, immediates keep their own
                                                   * (Gunmen Wars, against the footage: the game opens each frame with a 90-degree set (320) its world is
                                                   * NOT drawn with -- fighters, team intro, the select card need the 60-degree set (554) it sends last;
                                                   * the suit-up close-ups open with their own narrower set (1955) and need that one) */
    float znear;                                  /* optional: the near plane for everything; 0 = MAME's 0.0001 */
    float znear_model;                            /* optional: near plane (C435 units) for models in the 3D view; 0 = the default */                /* optional: adjust the frame's render list (a s23_render_list *) before it is drawn */
    struct { uint32_t pc, flag; } idle[4];         /* optional: the game's WAIT LOOPS (src/s23_cpu.c rr_tick): a poll from pc (the loop branch's delay
                                                   * slot) while the word at flag (a main-RAM address) is still 0 skips to the next event --
                                                   * Gunmen Wars waits for its vblank flag so (19% of the main thread, measured) */
    const struct s23_link_ops *link;              /* optional: the game's OWN cabinet link in place of the C422 twin-cabinet one (src/s23_c422.c);
                                                   * Gunmen Wars: the GMEN boards' IEEE 1394 bus (gunwars/src/gw_link.c). NULL = the C422 */
} s23_game_t;

/* A GAME'S OWN CABINET LINK (s23_game.link): the engine calls these where it would call the C422's (src/s23_c422.c) */
typedef struct s23_link_ops {
    const char *title;                            /* the power-on wait screen's first line */
    void (*gate)(int (*pump)(const char *l1, const char *l2));   /* power-on: wait for the other cabinets (pump: the window, or NULL) */
    void (*tick)(void);                           /* time passed (each slice): the cable read, kept in step */
    void (*report)(void);                         /* closing: the totals to the log, the others told */
    int  (*linked)(void);                         /* 1 = in a linked session (no pause: the others would lose this one) */
    int  (*lights)(int kind[2], int lit[2]);      /* the link lights as s23_c422_lights; NULL = none */
} s23_link_ops;

/* THE GORgON BOARD (s23_game.gorgon; src/s23_gorgon.c): the engine calls these only on that board */
struct s23_sprite;                                /* src/s23_frame.h */
typedef struct s23_gorgon_ops {
    uint32_t (*fold)(uint32_t p);                 /* a GORgON physical address -> System 23's for the same chip (unchanged when it has none) */
    int  (*read)(uint32_t p, int size, uint32_t *v);   /* its own registers (CZ RAM ...), before the fold: 1 = answered */
    int  (*write)(uint32_t p, int size, uint32_t v);   /* its own registers (CZ RAM, sprite RAM ...), before the fold: 1 = taken */
    void (*pio_mode)(uint32_t v);                 /* C435 0x0100005C: bit 0 = the DMA carries sprite data */
    int  (*sprite_mode)(void);
    void (*sprite_w)(uint16_t w);                 /* a DMA word in sprite mode (MAME c435_sprite_w) */
    void (*bufpos0)(void);                        /* C435 0x01000060: restart the word count */
    int  (*sprites)(struct s23_sprite *out, int max);   /* this frame's sprites, placed on the screen (MAME gorgon_state::render_run): the count */
    int  (*fog)(uint16_t pcz[256], int *rev, int *delta);   /* the depth fog's table in use (CZ RAM bank), its reverse mask and level offset: 1 = on */
} s23_gorgon_ops;
extern const s23_gorgon_ops s23_gorgon;

extern const s23_game_t s23_game;                 /* the one this binary was built for */
#endif
