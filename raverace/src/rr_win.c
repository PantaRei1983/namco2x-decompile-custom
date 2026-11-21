/*
 * rr_win.c -- Windows start-up (MinGW-w64 build only).
 *
 *  - work from the program's own folder, found with the WIDE API: double-clicked
 *    or started from a shortcut, extracted/, roms/ and rr_controls.cfg are there;
 *    the ANSI calls cannot name a folder outside the system code page
 *    (C:\Users\Zoë\...), so everything after this uses relative paths;
 *  - a windowed program has no console: stdout and stderr go to raveracer.log
 *    beside the .exe -- the first thing to read when it does not start;
 *  - report real pixel sizes, or Windows stretches (blurs) the window by the
 *    display scaling.
 */
#ifdef _WIN32
#include <stdio.h>
#include <io.h>
#include <direct.h>
#include <wchar.h>
#include <windows.h>
#include <SDL.h>
#include "rr_game.h"

void rr_win_startup(const char *logname)
{
    static wchar_t p[32768];
    DWORD n = GetModuleFileNameW(NULL, p, 32768);
    if (n > 0 && n < 32768) {
        wchar_t *sl = wcsrchr(p, L'\\');
        if (sl) { *sl = 0; _wchdir(p); }
    }
    /* both streams into the log. A GUI program started from Explorer has no stderr at all, so it is opened here rather than
     * _dup2'd onto: a stream that started without a handle stays broken (the log only got stdout). Append, so the two interleave. */
    FILE *t = getenv("N22_LOG_APPEND") ? NULL : fopen(logname, "w");     /* (a restart by the link's safety net: the log goes on) */
    if (t) fclose(t);
    if (freopen(logname, "a", stdout)) setvbuf(stdout, NULL, _IONBF, 0);
    if (freopen(logname, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
}
#endif
