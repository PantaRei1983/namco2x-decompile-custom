/*
 * aj_interp.c -- Aqua Jet's master DSP and sound MCU, INTERPRETED (feasibility test).
 *
 * The other games translate these two programs to C ahead of time (tools/gen/c25_translate.py, snd_translate.py), from coverage their
 * development builds record. A translated instruction is only the engine's own instruction routine called with the opcode as a
 * constant (c25_exec, m377_exec) -- so the same routines, given the opcode from program memory, run a program nobody has
 * translated. Slower, and exactly as correct as those routines.
 */
#include <stdio.h>
#include "c25.h"
#include "c25_sem.h"

/* one master-DSP instruction at pc and its RPT repeats: the step the translations make, the opcode read from program memory */
bool aj_c25_exec(c71_t *d, int pc)
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
