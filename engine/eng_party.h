/* eng_party.h -- SEVERAL CABINETS ON ONE PC, A PLAYER AT EACH (header only): which game controller this copy of the game reads.
 *
 * The one-PC launchers start two or four linked copies of the game (--link N). For people to PLAY them, each copy must listen to its
 * own controller only -- by default every copy opens every controller, so one pad would drive all four cabinets -- and must go on
 * reading it while another copy's window has the focus.
 *
 *   --pad K   (engine/link_host.c sets N22_PAD=K before the window opens)
 *       K = 1..8   this copy uses the K-th game controller only (in the order the system lists them), no other device
 *       K = 0      no controller at all: this copy is the keyboard's (the keyboard goes to whichever window was clicked last)
 *       not given  every controller and raw device, as always
 *   and SDL is told to deliver controller input to a window in the background (SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS).
 *
 * Only standard game controllers count (Xbox, DualShock / DualSense, Switch Pro, 8BitDo ...): a raw device (a wheel, an arcade
 * encoder) is left to a copy started without --pad. The order is the system's: plug the controllers in before starting. */
#ifndef ENG_PARTY_H
#define ENG_PARTY_H
#include <stdbool.h>
#include <stdlib.h>
#include <SDL2/SDL.h>

/* -1 = no --pad (every device); 0 = none (keyboard); K = the K-th game controller */
static inline int eng_party_pad(void)
{
    const char *e = getenv("N22_PAD");
    return e && *e ? atoi(e) : -1;
}

/* may this copy open joystick device index j? */
static inline bool eng_party_allows(int j)
{
    const int k = eng_party_pad();
    if (k < 0) return true;
    if (k == 0 || !SDL_IsGameController(j)) return false;
    int ord = 0;
    for (int i = 0; i <= j; i++) if (SDL_IsGameController(i)) ord++;
    return ord == k;
}
#endif
