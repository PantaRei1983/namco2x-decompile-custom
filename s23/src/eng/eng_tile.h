/* eng_tile.h -- our fork: the windows of a test on one PC (2, 4 or 8 linked cabinets, Time Crisis II's two sides) laid out the way
 * the Viper port's World Combat test does it: an even grid on the screen's usable area (above the task bar), every window as big
 * as its cell allows at the picture's own shape, the windows edge to edge (title bars and frames included, nothing overlapping),
 * the whole group in the middle of the screen. 2 = side by side, 3-4 = 2 x 2, 5-8 = 4 x 2. Header only.
 * Windows 10/11 frames carry an invisible resize border (about 8 pixels left, right and below): the visible edges are measured from
 * the window manager (DWM's extended frame bounds), so neighbours meet without a gap. */
#ifndef ENG_TILE_H
#define ENG_TILE_H
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* how far the frame SDL reports reaches past what is drawn (invisible borders); 0 elsewhere */
static inline void eng_tile_invisible(SDL_Window *w, int *il, int *it, int *ir, int *ib)
{
    *il = *it = *ir = *ib = 0;
#ifdef _WIN32
    SDL_SysWMinfo i; SDL_VERSION(&i.version);
    if (!SDL_GetWindowWMInfo(w, &i) || i.subsystem != SDL_SYSWM_WINDOWS) return;
    static HRESULT (WINAPI *get)(HWND, DWORD, PVOID, DWORD);
    static int tried;
    if (!tried) { tried = 1; HMODULE m = LoadLibraryA("dwmapi.dll"); if (m) get = (HRESULT (WINAPI *)(HWND, DWORD, PVOID, DWORD))(void *)GetProcAddress(m, "DwmGetWindowAttribute"); }
    RECT outer, seen;
    if (!get || !GetWindowRect(i.info.win.window, &outer) || get(i.info.win.window, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &seen, sizeof seen) != 0) return;
    {   /* a program Windows scales for it (not DPI aware, Time Crisis II): DWM answers in the screen's real pixels, the rest in the
         * program's -- the factor is the screen's real width over the width the program sees */
        DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        const int seen_w = GetSystemMetrics(SM_CXSCREEN);
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && seen_w > 0 && (int)dm.dmPelsWidth > seen_w) {
            const double k = (double)dm.dmPelsWidth / seen_w;
            seen.left = (LONG)(seen.left / k + 0.5); seen.top = (LONG)(seen.top / k + 0.5);
            seen.right = (LONG)(seen.right / k + 0.5); seen.bottom = (LONG)(seen.bottom / k + 0.5);
        }
    }
    *il = (int)(seen.left - outer.left); *it = (int)(seen.top - outer.top); *ir = (int)(outer.right - seen.right); *ib = (int)(outer.bottom - seen.bottom);
    if (*il < 0 || *il > 16) *il = 0; if (*it < 0 || *it > 16) *it = 0; if (*ir < 0 || *ir > 16) *ir = 0; if (*ib < 0 || *ib > 16) *ib = 0;
#else
    (void)w;
#endif
}

static inline void eng_tile_window(SDL_Window *w, int index, int count)
{
    SDL_Rect b;
    if (!w || count < 2 || index < 0 || index >= count || SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(w), &b) != 0) return;
    const int cols = count <= 4 ? 2 : 4, rows = (count + cols - 1) / cols;
    int w0, h0, top = 0, l = 0, bo = 0, r = 0, il, it, ir, ib;
    SDL_GetWindowSize(w, &w0, &h0);
    if (w0 <= 0 || h0 <= 0) { w0 = 4; h0 = 3; }
    SDL_GetWindowBordersSize(w, &top, &l, &bo, &r);                    /* the title bar and frame, outside the size */
    eng_tile_invisible(w, &il, &it, &ir, &ib);
    l -= il; r -= ir; top -= it; bo -= ib;                             /* the frame as it is SEEN */
    if (top < 0) top = 0; if (l < 0) l = 0; if (r < 0) r = 0; if (bo < 0) bo = 0;
    const int aw = b.w / cols - l - r, ah = b.h / rows - top - bo;     /* the picture's room in its cell */
    int pw = aw, ph = (int)((long)aw * h0 / w0);
    if (ph > ah) { ph = ah; pw = (int)((long)ah * w0 / h0); }
    if (pw < 160 || ph < 120) return;                                  /* a tiny screen: leave the window where it is */
    const int fw = pw + l + r, fh = ph + top + bo;                     /* the window as seen, frame included */
    const int ox = b.x + (b.w - cols * fw) / 2, oy = b.y + (b.h - rows * fh) / 2;
    SDL_SetWindowSize(w, pw, ph);
    SDL_SetWindowPosition(w, ox + (index % cols) * fw + l, oy + (index / cols) * fh + top);
    fprintf(stderr, "[HOST] window %d of %d: %d x %d, a %d x %d grid on the %d x %d screen (frame seen %d/%d/%d/%d)\n",
            index + 1, count, pw, ph, cols, rows, b.w, b.h, l, top, r, bo);
}
#endif
