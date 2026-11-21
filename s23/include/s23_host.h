/* s23_host.h -- what the lifted program's runtime (src/s23_cpu.c) needs from the Time Crisis 2 host (src/s23_mem.c, src/s23_main.c) */
#ifndef S23_HOST_H
#define S23_HOST_H
#include <stdint.h>
#include <stddef.h>
/* Ghidra's MIPS register space (decompiled/pcode.txt "R" lines): 64-bit registers, the address in the low half */
#define S23_R_SP      0xE8
#define S23_R_COUNT   0x2048
#define S23_R_COMPARE 0x2058
#define S23_R_STATUS  0x2060
#define S23_R_CAUSE   0x2068
#define S23_R_EPC     0x2070
#define S23_R_ERROREPC 0x20F0              /* CP0 30 */
/* save states (src/s23_savestate.c): a debug switch's static, registered BEFORE it is parsed, is reset by a load (re-read) */
void s23_ss_env(void *p, size_t n);
#define S23_SS_ENV(v) s23_ss_env(&(v), sizeof (v))
void s23_ss_keep(void *p, size_t n);                 /* a load leaves this value as this process has it (src/s23_savestate.c) */
#define S23_SS_KEEP(v) s23_ss_keep(&(v), sizeof (v))
#define S23_SLICE     1000                 /* instructions between scheduler polls */
void s23_host_step(uint32_t instructions);  /* time passed: devices, frames */
uint32_t s23_until_event(void);            /* instructions to the next interrupt event: the scheduler polls right there */
void s23_trace_mark(const char *what, uint32_t v);
void s23_on_trap(void);
void s23_on_exit(void);
void s23_advance_count(uint32_t instructions);
void s23_poll_irq(void);
void s23_irq_set(int src, int on);          /* an interrupt SOURCE (S23_SRC_*, include/s23_game.h) on / off: the game's table picks its pin (src/s23_cpu.c) */
int  s23_irq_is(int src);                   /* that source pending */
uint32_t s23_irq_sources(void);             /* all pending sources (bit = S23_SRC_*) */
extern uint32_t rr_frame, rr_n_traps, rr_pc;
extern uint64_t rr_n_irq;
/* THE PHOTO PAGE (s23_game.photo_page): 256 x 256 RGB texels the game's camera board supplies -- u, v from the page's start; written by
 * the game's code (gw_game.c), drawn by both renderers; bump s23_photo_gen after a change so the GPU copy is refreshed */
#define S23_PHOTO_W 1024                           /* the photo page's width: Gunmen Wars puts cabinet n's photo at u = 128 n (3 linked: 0, 128, 256) */
extern uint32_t s23_photo_tex[S23_PHOTO_W * 256];
extern volatile int s23_photo_gen;
#endif
