/* eng_nk_click.h -- QUICK CLICKS for the Nuklear menus (the engine's eng_ui.c, Rave Racer's rr_ui.c). Include AFTER nuklear_sdl_gl2.h.
 *
 * Nuklear fires a button when it sees the mouse button DOWN at the end of an input round (nk_input_is_mouse_pressed). A quick click -- a
 * touchpad tap above all -- delivers the press and the release inside one round (one game frame), so at the end of it the button is up
 * again and the click is lost: "Exit does not click". Here a release that arrives in the same round as its press is held back and given
 * to Nuklear at the start of the next round, so every click is seen down for one frame first. Header-only, one copy per menu. */
#ifndef ENG_NK_CLICK_H
#define ENG_NK_CLICK_H
#include <stdbool.h>
static bool nkc_down_now, nkc_have_up;
static SDL_Event nkc_up;
/* right after nk_input_begin(): the held-back release, if any */
static inline void nkc_begin(void)
{
    nkc_down_now = false;
    if (nkc_have_up) { nkc_have_up = false; nk_sdl_handle_event(&nkc_up); }
}
/* in place of nk_sdl_handle_event() */
static inline void nkc_event(SDL_Event *e)
{
    if (e->type == SDL_MOUSEBUTTONDOWN) nkc_down_now = true;
    else if (e->type == SDL_MOUSEBUTTONUP && nkc_down_now && !nkc_have_up) { nkc_up = *e; nkc_have_up = true; return; }
    nk_sdl_handle_event(e);
}
#endif
