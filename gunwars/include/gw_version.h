/* gw_version.h -- WHICH GUNMEN WARS this build is: MAME's three sets, each with its own main program (the build translates the one you have).
 *   GW_SET 0  gunwars    World, GM2 Ver. B (MAME's parent set)   gm2verb.ic2 / .ic1
 *   GW_SET 1  gunwarsj   Japan, GM1 Ver. B                       gm1verb.ic2 / .ic1   (MAME's "gunwars" before it got the World set)
 *   GW_SET 2  gunwarsa   Japan, GM1 Ver. A                       gm1vera.ic2 / .ic1
 * Graphics, models, sound, the sub-CPU (gm1vera.ic3) and the I/O board are the same chips in all three (in gunwars.zip for the clones).
 * gen/gw_set.h, written by tools/make_prog.py with the translation, says which; a translation from before it is gunwarsj.
 *
 * The three programs are the same code, moved: gp (the small-data base every global is read through) is 0x8033A570 in the World set,
 * 0x8033A550 in Ver. B, 0x8033A510 in Ver. A, and the RAM addresses this host reads move with it (GW_RAM). */
#ifndef GW_VERSION_H
#define GW_VERSION_H
#if defined(__has_include)
#  if __has_include("gw_set.h")
#    include "gw_set.h"
#  endif
#endif
#ifndef GW_SET
#define GW_SET 1
#endif

#if GW_SET == 0
#define GW_SETNAME  "gunwars"
#define GW_PROG_HI  "gm2verb.ic2"
#define GW_PROG_LO  "gm2verb.ic1"
#define GW_CRC_HI   0xE413B643u
#define GW_CRC_LO   0xEC00BCF9u
#define GW_GP_DELTA 0x20
#define GW_VERSION_TEXT "World, GM2 Ver. B"
#elif GW_SET == 2
#define GW_SETNAME  "gunwarsa"
#define GW_PROG_HI  "gm1vera.ic2"
#define GW_PROG_LO  "gm1vera.ic1"
#define GW_CRC_HI   0xCF61467Fu
#define GW_CRC_LO   0xABC9FFE6u
#define GW_GP_DELTA (-0x40)
#define GW_VERSION_TEXT "Japan, GM1 Ver. A"
#else
#define GW_SETNAME  "gunwarsj"
#define GW_PROG_HI  "gm1verb.ic2"
#define GW_PROG_LO  "gm1verb.ic1"
#define GW_CRC_HI   0x401F8264u
#define GW_CRC_LO   0xF9FD0F2Bu
#define GW_GP_DELTA 0
#define GW_VERSION_TEXT "Japan, GM1 Ver. B"
#endif

/* a main-RAM address as Ver. B has it (the one this host was written against), in this set */
#define GW_RAM(a) ((uint32_t)((int32_t)(a) + GW_GP_DELTA))
#endif
