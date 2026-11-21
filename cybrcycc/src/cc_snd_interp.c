/* al_snd_interp.c -- the sound MCU (M37710) interpreted through engine/snd/m377_sem.h: see cc_interp.c. The contract is the
 * translated drivers' (gen/<game>_snd_driver.c): snd_executor_init, snd_run, snd_spin_fast. */
#include <stdio.h>
#include "m37710.h"
#include "m377_sem.h"

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

uint64_t snd_spin_fast(m37710_t *c, uint64_t limit) { (void)c; (void)limit; return 0; }   /* no fast-forward: every pass is executed */
void snd_executor_init(void) { printf("[SND] sound program INTERPRETED (not translated)\n"); }
int snd_run(m37710_t *c, int cycles) { run_chunk(c, c->cycles + (uint64_t)cycles); return 0; }
