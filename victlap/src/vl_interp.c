/*
 * vl_interp.c -- Victory Lap's master DSP and sound MCU, INTERPRETED.
 *
 * Ace Driver translates these two programs ahead of time (c25_translate.py / snd_translate.py) from coverage recorded by
 * development builds this tree does not have. A translated instruction is only the engine's own instruction routine called
 * with the opcode as a constant (c25_exec, m377_exec), so the same routines given the opcode from program memory run the
 * program untranslated: slower, and exactly as correct (Alpine Racer's al_interp.c).
 */
#include <stdio.h>
#include "c25.h"
#include "c25_sem.h"
#include "m377_sem.h"
#include "snd_board.h"

bool rr_c25_exec(c71_t *d, int pc)
{
    const uint16_t op = d->prog[pc & 0xFFFF];
    d->ops = &d->prog[pc & 0xFFFF];
    d->pc = (uint16_t)(pc + 1);
    const int n = d->rpt + 1; d->rpt = 0;
    for (int it = 0; it < n; it++)
        if (!c25_exec(d, pc, op, it)) { d->ops = NULL; return false; }
    d->ops = NULL;
    return true;
}

static void step(m37710_t *c)
{
    uint8_t op = fetch8(c);
    c->use_b = false;
    if (op == 0x42) { c->use_b = true; op = fetch8(c); }      /* the B-accumulator prefix */
    c->cycles += 1;
    m377_exec(c, op);
}

static void run_chunk(m37710_t *c, uint64_t until)
{
    while (c->cycles < until && !c->unimpl_hit) {
        m37710_service(c);
        if (c->stopped) { c->cycles = until; break; }
        step(c);
    }
}

uint64_t snd_spin_fast(m37710_t *c, uint64_t limit) { (void)c; (void)limit; return 0; }
void snd_executor_init(void) { fprintf(stderr, "[SND] sound program INTERPRETED (not translated)\n"); }
void snd_execute(m37710_t *c, uint64_t end)
{
    snd_board_port4();
    while (c->cycles < end && !c->unimpl_hit) {
        uint64_t rem = end - c->cycles;
        run_chunk(c, c->cycles + (rem < 64 ? rem : 64));
    }
}
