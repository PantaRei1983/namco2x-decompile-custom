/* s23_ss_begin.c -- SAVE STATES (tests; src/s23_savestate.c): the start of the game's own static data. Linked FIRST of the game's
 * objects (s23_ss_end.c last, s23_savestate.c after it): the linker lays .data and .bss out in input order, so everything between the
 * two markers is the emulator's state -- the main RAM, the CPUs, the chips -- and nothing of SDL's or the C library's. */
char s23_ss_data_begin[16] = { 1 };
char s23_ss_bss_begin[16];
