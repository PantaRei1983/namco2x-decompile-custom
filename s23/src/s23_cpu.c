/*
 * s23_cpu.c -- the runtime of Time Crisis 2's LIFTED MIPS program (tools/lift/lift.py): the register file, the shadow return stack, traps,
 * the R4650's exception entry (one vector, 0x80000180, with SR.BEV clear) and eret, the CP0 timer (Count / Compare), and the trace hook.
 * Adapted from the System 22 project's 68K runtime (lift_cpu.c) and independent of it.
 *
 * The lifted code turns every jal into a C call, so `jr ra` needs a shadow stack to find the C frame it returns to (include/lift_rt.h).
 * MIPS pops nothing on a return: a call site matches when the return address AND the stack pointer equal what they were at the call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lift_rt.h"
#include "s23_mips_rt.h"
#include "s23_lifted.h"
#include "s23_host.h"
#include "s23_game.h"

uint8_t R[RR_REGSPACE];
int32_t rr_budget;
uint32_t rr_pc, rr_n_traps, rr_frame;
int rr_in_irq;
uint64_t rr_n_irq;

/* the readable-C checker's hooks (the System 22 project's src/rd): not used here, the generated code still names them */
int rd_on, rd_stop_on, rd_poll_rec;
int rd_hook(uint32_t ep) { (void)ep; return 0; }
int rd_jump_stop(uint32_t t, uint32_t at) { (void)t; (void)at; return 0; }
void rd_poll_snap(void) {}

static uint32_t sp32(void) { return (uint32_t)RG4(S23_R_SP + 4); }     /* the low half of the 64-bit sp: the address */

void rr_div0(const char *what)
{
    static unsigned n;
    if (++n <= 3) fprintf(stderr, "[S23] %s at %08X (MIPS: the result is undefined, no exception); continuing with 0\n", what, rr_pc);
}

void rr_trap(uint32_t at, uint32_t target, const char *what)
{
    if (++rr_n_traps <= 40) fprintf(stderr, "[TRAP] f%u at 0x%08X -> 0x%08X: %s\n", rr_frame, at, target, what);
    if (rr_n_traps == 40) fprintf(stderr, "[TRAP] (further traps counted, not printed)\n");
    s23_on_trap();
}

/* an FP op the R4650 traps on (include/lift_rt.h rr_fp_cause): the game's handler 0x80000358 clears the cause bits and jumps to the boot
 * monitor, whose FPE slot is the default (0x803F0F5C = 0xBFC015C0): its register dump and debugger -- the board stops. Reported like a trap;
 * execution goes on with MAME's result. MAME takes the same exception only for div.s by zero with Z enabled: there our run and MAME's part. */
uint32_t rr_n_fpe;
void rr_fpe(uint32_t cause, uint32_t fcsr, char op)
{
    static const char *nm[6] = { "inexact", "underflow", "overflow", "divide-by-zero", "invalid", "unimplemented (E: denormal operand / conversion)" };
    if (++rr_n_fpe <= 40) {
        char w[160] = ""; uint32_t hit = (cause & 0x20000u) | ((cause >> 5 & fcsr & 0xF80u) << 5);
        for (int k = 0; k < 6; k++) if (hit >> (12 + k) & 1) { strcat(w, w[0] ? ", " : ""); strcat(w, nm[k]); }
        const int mame = op == '/' && (hit & 0x8000u) && (fcsr & 0x400u);
        fprintf(stderr, "[FPE] f%u at 0x%08X (%c): %s -- the R4650 traps here and the game stops in the boot monitor; %s\n", rr_frame, rr_pc, op, w,
                mame ? "MAME raises it too" : "MAME does not raise it (its result is used)");
        if (rr_n_fpe == 40) fprintf(stderr, "[FPE] (further FP exceptions counted, not printed)\n");
    }
    s23_on_trap();
}

static void fatal(int code, const char *why)
{
    fprintf(stderr, "[S23] %s -- stopping (pc %08X, frame %u)\n", why, rr_pc, rr_frame);
    { extern int s23_crashing; s23_crashing = 1; }          /* s23_on_exit: the backup RAM is NOT saved from a broken state */
    s23_on_exit();
    { char m[256]; snprintf(m, sizeof m, "%s\n(pc %08X, frame %u)", why, rr_pc, rr_frame); void s23_crash_box(const char *); s23_crash_box(m); }
    exit(code);
}

/* ---- the shadow return stack ---- */
#define SHADOW_MAX 4096
#define SH_IRQ 0xFFFFFFFFu
static uint32_t sh_ret[SHADOW_MAX], sh_sp[SHADOW_MAX];
static int sh_n, sh_target = -1;
uint32_t rr_ret_to;

int rr_call_push(uint32_t ret)
{
    if (sh_n >= SHADOW_MAX) fatal(4, "shadow stack overflow");
    sh_ret[sh_n] = ret; sh_sp[sh_n] = sp32();
    return sh_n++;
}

/* ---- SAVE STATES (src/s23_savestate.c). Saved at an interrupt's entry (take_exception), which is reached from a POLL: the C code the
 * poll sits in carries on after the eret -- into the callee of a call (its shadow frame already pushed), or to a taken branch's target, or
 * a jump's. rr_pc is that branch's delay slot (RR_INS), so the continuation is decoded from the instruction before it. A loaded run keeps
 * the saved shadow stack but has none of its C frames: a return into one unwinds to the host (src/s23_main.c), which carries on there. */
static void after_exception(void);
int rr_ss_cont(uint32_t *cont)
{
    const uint32_t pc = rr_pc, ins = MRD4(pc - 4), op = ins >> 26, fn = ins & 63;
    if (op == 2 || op == 3) { *cont = (pc & 0xF0000000u) | (ins & 0x3FFFFFFu) << 2; return 1; }          /* j / jal */
    if (op == 0 && (fn == 8 || fn == 9)) { *cont = (uint32_t)RG8(((ins >> 21) & 31) * 8); return 1; }     /* jr / jalr */
    if (op == 1 || (op >= 4 && op <= 7) || (op >= 20 && op <= 23)) { *cont = pc + (uint32_t)((int32_t)(int16_t)ins * 4); return 1; }   /* branches */
    return 0;
}
int rr_ss_resumed; uint32_t rr_ss_after_irq;
/* the lifted code came back to the host: a return into a frame of the loaded state (or its interrupt's eret) -- where to carry on */
int rr_ss_base_unwound(uint32_t *next)
{
    if (!rr_ss_resumed || sh_target < 0) return 0;
    sh_target = -1;
    if (rr_ret_to == SH_IRQ) { rr_in_irq--; s23_trace_mark("ERET", 0); after_exception(); *next = rr_ss_after_irq; return 1; }
    *next = rr_ret_to;
    return 1;
}
int rr_after_call(int j)
{
    if (sh_target < 0) { char w[96]; snprintf(w, sizeof w, "call frame %d came back without a return", j); fatal(5, w); }
    if (sh_target < j) return 1;           /* unwinding further up */
    sh_target = -1;
    return 0;
}

int rr_return(uint32_t t)
{
    const uint32_t sp = sp32();
    for (int k = sh_n - 1; k >= 0; k--)    /* exact: the return address and the stack pointer of the call */
        if (sh_ret[k] == t && sh_sp[k] == sp) { sh_n = k; sh_target = k; rr_ret_to = t; return 1; }
    for (int k = sh_n - 1; k >= 0; k--)    /* the address only (a function that left sp moved) */
        if (sh_ret[k] == t) { sh_n = k; sh_target = k; rr_ret_to = t; return 1; }
    return 0;
}

/* eret, as the R4650 does it: with Status.ERL set, to ErrorEPC clearing ERL; otherwise to EPC clearing EXL (Ghidra's p-code only jumps to EPC).
 * Back from an exception the host delivered (an IRQ frame on the shadow stack): unwind to its C frame, s23_take_exception resumes there.
 * With none, eret is a JUMP -- the boot monitor enters the game that way: EPC = 0x80000380 (_start), then eret at 0xBFC011EC. */
static void after_exception(void);
void rr_rte(void)
{
    const uint64_t sr = RG8(S23_R_STATUS);
    uint32_t target;
    if (sr & 4) { target = (uint32_t)RG8(S23_R_ERROREPC); RS8(S23_R_STATUS, sr & ~4ULL); }
    else        { target = (uint32_t)RG8(S23_R_EPC);      RS8(S23_R_STATUS, sr & ~2ULL); }
    for (int k = sh_n - 1; k >= 0; k--)
        if (sh_ret[k] == SH_IRQ) { sh_n = k; sh_target = k; rr_ret_to = SH_IRQ; return; }
    s23_trace_mark("ERET-JUMP", target);
    rr_jump(target, rr_pc);
}

/* ---- the R4650's interrupts ----
 * Taken when SR.IE = 1, SR.EXL = SR.ERL = 0 and (Cause.IP & SR.IM) != 0: EPC = the instruction about to run, Cause.ExcCode = 0 (Int),
 * SR.EXL = 1, and the general exception vector runs (0x80000180; 0xBFC00380 with SR.BEV set). The lifted code polls at calls and backward
 * branches, so an interrupt lands there -- between instructions, as on the CPU, but only at those points. */
static int irq_pending(void)
{
    const uint64_t sr = RG8(S23_R_STATUS), cause = RG8(S23_R_CAUSE);
    return (sr & 1) && !(sr & 6) && ((cause & sr) & 0xFF00);
}

/* S23_IRQSTAT=1: at exit, how many taken interrupts had each source pending (a storm names its source) */
static uint32_t irq_on;                 /* (defined with the lines below) */
static uint64_t irq_stat[S23_SRC_N], pin_stat[6];
static void irq_stat_print(void)
{
    static const char *nm[S23_SRC_N] = { "vblank", "C361", "sub", "C435", "C422", "C450", "C451", "RS232", "timer" };
    fprintf(stderr, "[IRQSTAT]");
    for (int s = 0; s < S23_SRC_N; s++) fprintf(stderr, " %s %llu", nm[s], (unsigned long long)irq_stat[s]);
    fprintf(stderr, "; taken on pin (pending & enabled):");
    for (int k = 0; k < 6; k++) fprintf(stderr, " IP%d %llu", k + 2, (unsigned long long)pin_stat[k]);
    fprintf(stderr, "\n");
}
static void take_exception(void)
{
    static int st = -1;
    if (st < 0) { st = getenv("S23_IRQSTAT") != NULL; if (st) atexit(irq_stat_print); }
    if (st) { for (int s = 0; s < S23_SRC_N; s++) if (irq_on >> s & 1) irq_stat[s]++;
              const uint32_t pe = (uint32_t)(RG8(S23_R_CAUSE) & RG8(S23_R_STATUS)) & 0xFC00u;
              for (int k = 0; k < 6; k++) if (pe >> (10 + k) & 1) pin_stat[k]++; }
    const uint64_t sr = RG8(S23_R_STATUS);
    RS8(S23_R_EPC, (uint64_t)(int64_t)(int32_t)rr_pc);
    RS8(S23_R_CAUSE, RG8(S23_R_CAUSE) & ~0x7CULL);                    /* ExcCode = 0: an interrupt */
    RS8(S23_R_STATUS, sr | 2);                                        /* EXL */
    const uint32_t vec = (sr & (1u << 22)) ? 0xBFC00380u : 0x80000180u;
    rr_n_irq++; rr_in_irq++;
    s23_trace_mark("IRQ", (uint32_t)RG8(S23_R_CAUSE));
    if (sh_n >= SHADOW_MAX) fatal(4, "shadow stack overflow");
    sh_ret[sh_n] = SH_IRQ; sh_sp[sh_n] = sp32(); const int j = sh_n++;
    { void s23_ss_irq_point(uint32_t); s23_ss_irq_point(vec); }      /* save states (tests): saved / loaded here, the vector next */
    rr_jump(vec, rr_pc);
    if (sh_target != j) fatal(6, "the exception handler did not end in eret");
    sh_target = -1;
    rr_in_irq--;
    s23_trace_mark("ERET", 0);
}

/* ---- CP0: the timer. Count runs at half the pipeline clock; Count == Compare raises IP7 (Cause bit 15); writing Compare clears it. ---- */
static uint64_t count_frac;                     /* instructions not yet turned into Count ticks */
void s23_advance_count(uint32_t instructions)
{
    count_frac += instructions;
    const uint32_t ticks = (uint32_t)(count_frac / 2); count_frac %= 2;
    const uint32_t c0 = (uint32_t)RG8(S23_R_COUNT), cmp = (uint32_t)RG8(S23_R_COMPARE);
    const uint32_t c1 = c0 + ticks;
    if ((uint32_t)(cmp - c0 - 1) < ticks)       /* Count passed Compare in this step */
        s23_irq_set(S23_SRC_TIMER, 1);
    RS8(S23_R_COUNT, (uint64_t)(int64_t)(int32_t)c1);
}

/* THE INTERRUPT LINES: Cause.IP2..IP7 (0xFC00) are the CPU's interrupt PINS, read-only (the R4650 manual; MAME's set_cop0_reg keeps them:
 * CAUSE = (CAUSE & 0xfc00) | (val & ~0xfc00)). The host's devices drive them through s23_irq_set (a source; the game table picks the pin); an mtc0 to Cause changes only the rest
 * (the software bits IP0/IP1) -- the game's exception entry does `mtc0 zero,Cause` (0x80000284), which must not drop a pending line. */
static uint32_t irq_lines;                                             /* 0xFC00: the pins as the sources drive them */
static uint32_t irq_on;                                                /* the pending SOURCES (bit = S23_SRC_*): MAME's m_main_irqcause. The game's
                                                                        * table maps each to its pin (System 23: IP2 vblank, IP3 C361 + sub-CPU, IP4 C435,
                                                                        * IP5 C422; Evolution 2: see ../crszone); sources sharing a pin are ORed */
static void irq_apply(void)
{
    uint32_t l = 0;
    for (int s = 0; s < S23_SRC_N; s++) if (irq_on >> s & 1) l |= s23_game.ip[s];
    irq_lines = l & 0xFC00u;
    RS8(S23_R_CAUSE, (RG8(S23_R_CAUSE) & ~0xFC00ULL) | irq_lines);
}
static int32_t granted;                                                 /* the scheduler's current slice (below) */
void s23_irq_set(int src, int on)
{
    const uint32_t was = irq_lines;
    irq_on = on ? (irq_on | 1u << src) : (irq_on & ~(1u << src));
    irq_apply();
    if (s23_game.irq_prompt && (irq_lines & ~was) && rr_budget > 0) { granted -= rr_budget; rr_budget = 0; }   /* end the slice here: the next poll takes it */
}
int s23_irq_is(int src) { return (irq_on >> src & 1) != 0; }
uint32_t s23_irq_sources(void) { return irq_on; }
/* a snapshot's lines, as MAME's Cause shows them: each pin to the FIRST source wired to it (on System 23 IP3 = the C361, not the sub-CPU) */
void s23_irq_sync_from_cause(void)
{
    const uint32_t c = (uint32_t)RG8(S23_R_CAUSE) & 0xFC00u; uint32_t seen = 0;
    irq_on = 0;
    for (int s = 0; s < S23_SRC_N; s++) { const uint32_t b = s23_game.ip[s]; if (b && (c & b) && !(seen & b)) { irq_on |= 1u << s; seen |= b; } }
    irq_apply();
}

void rr_mips_cop0_write(uint32_t pc, uint32_t off)
{
    (void)pc;
    if (off == S23_R_COMPARE) s23_irq_set(S23_SRC_TIMER, 0);                    /* writing Compare acknowledges the timer */
    if (off == S23_R_CAUSE) RS8(S23_R_CAUSE, (RG8(S23_R_CAUSE) & ~0xFC00ULL) | irq_lines);   /* the pins are read-only */
}
void rr_mips_setCopReg(uint32_t pc, uint64_t sel, uint64_t reg, uint64_t val) { (void)sel; (void)reg; (void)val; rr_trap(pc, 0, "mtc0 to an unnamed register"); }
void rr_mips_cacheOp(uint32_t pc, uint64_t op, uint64_t addr) { (void)pc; (void)op; (void)addr; }   /* no cache to model */
void rr_mips_setISAMode(uint32_t pc, uint64_t mode) { (void)pc; (void)mode; }                         /* no MIPS16 on the R4650 */
void rr_mips_trap(uint32_t pc, uint64_t code) { rr_trap(pc, (uint32_t)code, "trap instruction"); }
void rr_mips_SYNC(uint32_t pc, uint64_t stype) { (void)pc; (void)stype; }

/* the scheduler: every poll once the budget is spent -- time advances, the host's devices step, interrupts are delivered */
static int32_t granted = S23_SLICE;   /* (declared above s23_irq_set) */
static uint64_t cycles_done;                                           /* cycles of finished slices */
uint64_t s23_cycles_now(void) { return cycles_done + (uint64_t)(granted - rr_budget); }   /* the exact cycle, mid-slice too */

/* the cycles run since the last accounting -- time advances, the devices step -- and the slice restarts from here. CLOSED BEFORE an
 * exception is taken: the handler's own polls tick inside take_exception, and counting from the old start would charge the part of the slice
 * already counted a second time (measured on Crisis Zone: ~860 cycles a C450 DMA interrupt, 300k a frame -- the frame landed 43 lines late,
 * past the game's line-259 submit cutoff in its heavier frames, and the 3D dropped out for a frame). Counted again after the eret: the
 * handler's tail after its last poll. */
static void account(void)
{
    const int32_t used = granted - rr_budget;
    granted = rr_budget;
    if (used <= 0) return;
    cycles_done += (uint64_t)used;
    s23_advance_count((uint32_t)used);
    s23_host_step((uint32_t)used);
}
extern void (*s23_present_hook)(void);                                 /* src/s23_window.c; NULL headless */
static void after_exception(void);
void rr_tick(void)
{
    account();
    /* A WAIT LOOP (s23_game.idle): nothing but a read of a flag an interrupt sets -- time goes on to the next event at once (the devices
     * step there as they would have, the CPU takes the interrupt as before) instead of the loop being run a slice at a time.
     * $S23_NOIDLE: off (tests) */
    if (rr_in_irq == 0 && s23_game.idle[0].pc && !irq_pending()) {
        static int off = -1; if (off < 0) off = getenv("S23_NOIDLE") != NULL;
        for (int i = 0; !off && i < 4 && s23_game.idle[i].pc; i++)
            if (rr_pc == s23_game.idle[i].pc && MRD4(s23_game.idle[i].flag) == 0) {
                uint32_t ev = s23_until_event(); if (!ev || ev > 100000u) ev = 100000u;   /* (bounded: a linked cabinet's lockstep margin, gunwars/src/gw_link.c) */
                cycles_done += ev; s23_advance_count(ev); s23_host_step(ev);
                break;
            }
    }
    if (rr_in_irq == 0 && irq_pending()) { take_exception(); after_exception(); return; }
    after_exception();
}
/* what follows an exception's eret in rr_tick (and a loaded save state's first eret, src/s23_savestate.c: it has no rr_tick to return to) */
static void after_exception(void)
{
    account();
    /* an interrupt raised INSIDE the handler (Crisis Zone's C450 handler kicks the next queued DMA, which interrupts again) is taken
     * right after the eret, as the CPU does -- not a slice later: waiting backed the DMA queue up, and the game's command-buffer
     * allocator then walked the blocks not yet freed (30x its usual work every ~55 frames). The prompt boards only (s23_game.irq_prompt) */
    while (s23_game.irq_prompt && rr_in_irq == 0 && irq_pending()) { take_exception(); account(); }
    if (s23_present_hook) s23_present_hook();                          /* the window: a picture tick due while the game runs (src/s23_window.c) */
    const uint32_t ev = s23_until_event();                             /* the next poll lands on the next interrupt event, not up to a slice late */
    granted = (int32_t)(ev && ev < S23_SLICE ? ev : S23_SLICE);
    rr_budget = granted;
}


void s23_poll_irq(void) { if (rr_in_irq == 0 && irq_pending()) take_exception(); }
