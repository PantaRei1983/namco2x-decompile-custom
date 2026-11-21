/* eng_timing.h -- which frame timing this run uses (header only): N22_TIMING=classic or new (the default).
 *
 *   new      the author's current pacer (engine/eng_vsync.h, since 0.5.4): Display > Frame rate, Auto = the display's own rate on 120/144 Hz screens,
 *            vsync re-checked every 50 frames.
 *   classic  0.4.0's pacer, the one the link play was first tested with: one picture per game frame, vsync only on a ~60 Hz
 *            display and only if it blocks (checked once, in the first second), else a 59.906 Hz timer. Display > Frame rate is
 *            ignored.
 *
 * engine/link_host.c sets N22_TIMING from link.cfg (timing=classic|new, the Network page's "Frame timing") or --timing before the
 * window opens; an N22_TIMING already in the environment wins. The game itself runs 59.906 frames a second either way. */
#ifndef ENG_TIMING_H
#define ENG_TIMING_H
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool eng_timing_classic(void)
{
    const char *e = getenv("N22_TIMING");
    return e && !strcmp(e, "classic");
}
#endif
