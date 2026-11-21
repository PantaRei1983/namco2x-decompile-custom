/*
 * s23_guns.c -- ONE GUN A SCREEN (our fork, the LAN link project): both Time Crisis 2 cabinets on one PC, each game full screen on its own
 * monitor and linked to the other (src/s23_c422.c), each reading ONLY its own player's gun or mouse. Windows merges every mouse into one
 * pointer; Raw Input still tells them apart (a Gun4IR, a Sinden, an AimTrak are all mice to Windows), and with RIDEV_INPUTSINK a game
 * gets them while the other game's window has the focus. The same scheme as World Combat's 4-player cabinet (Konami Viper Recomp,
 * src/runtime/frontend_sdl.c).
 *
 *   $S23_OWN_GUN=1   on (the "two screens, one PC" launcher); off, the game reads the system pointer as before
 *   $S23_MONITOR=N   this game full screen on the Nth monitor from the left (src/s23_window.c)
 *
 * WHICH GUN IS WHOSE: guns.cfg in the program's folder, "left=<part of the device name>" and "right=..." (the log lists the names).
 * Without it the first gun to shoot is the LEFT (red) player's and the next one the RIGHT (blue) player's: both games see every shot,
 * so the left game takes the first device that shoots and tells the right one ("C42G" over the link); the right game takes a device
 * that shoots only once the left one has a different one. When both have one and guns.cfg did not name them, the left game writes them
 * to guns.cfg: they stay on their sides from then on (delete guns.cfg to choose again).
 *
 * A GUN: absolute positions (0..65535) cover its own screen -- a light gun is calibrated to the screen it points at; a mouse moves this
 * game's crosshair by its counts. Left button = trigger, right / middle = pedal (as the system pointer's), side button 1 = off-screen;
 * a gun pointing off the screen (at an edge value) is off-screen too (the reload).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif

int s23_c422_link_right(void);
void s23_c422_send_claim(const char *name);
void eng_ui_set_hint(const char *text, int frames);

static int on = -1;
int s23_guns_on(void) { if (on < 0) { const char *e = getenv("S23_OWN_GUN"); on = e && *e && *e != '0'; } return on; }

#ifdef _WIN32
typedef struct { HANDLE h; char name[256], label[48]; int whose; } dev_t2;   /* whose: 0 nobody yet, 1 this game, 2 the other game */
static dev_t2 devs[16]; static int ndev;
static int mine = -1;                     /* this game's device (index), -1 none yet */
static char cfg_name[2][128];             /* guns.cfg: [0] left, [1] right */
static char other_claim[256];             /* the device the other game took (its "C42G") */
static char cfg_path[300] = "guns.cfg";
static double gx = 0.5, gy = 0.5;         /* the aim, as a fraction of this screen (outside 0..1 = off it) */
static uint32_t gbuttons;                 /* SDL_BUTTON masks */
static int gmoves, gabs;
static SDL_Window *gwin;
#define NO_HANDLE ((HANDLE)(intptr_t)-2)  /* precision touchpads arrive with no device handle: one device */

static int me_right(void) { return s23_c422_link_right(); }

static void take(int i)
{
    mine = i; devs[i].whose = 1;
    fprintf(stderr, "[GUNS] this game (%s) uses %s = %s\n", me_right() ? "right / blue" : "left / red", devs[i].label, devs[i].name);
    s23_c422_send_claim(devs[i].name);
    static char h[96]; snprintf(h, sizeof h, "THIS SCREEN'S GUN: %s", devs[i].label); eng_ui_set_hint(h, 60 * 4);
}

static void save_cfg(void)
{
    if (me_right() || mine < 0 || !other_claim[0] || (cfg_name[0][0] && cfg_name[1][0])) return;
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "# Time Crisis 2, two screens on one PC: which gun is whose (written when both had shot; delete this file to choose again)\n"
               "# A part of each device's name is enough. The left screen is the red player, the right screen the blue player.\n"
               "left=%s\nright=%s\n", devs[mine].name, other_claim);
    fclose(f);
    snprintf(cfg_name[0], sizeof cfg_name[0], "%s", devs[mine].name); snprintf(cfg_name[1], sizeof cfg_name[1], "%s", other_claim);
    fprintf(stderr, "[GUNS] both guns chosen: saved to guns.cfg\n");
}

static dev_t2 *dev_of(HANDLE h, int *idx)
{
    for (int i = 0; i < ndev; i++) if (devs[i].h == h) { *idx = i; return &devs[i]; }
    if (ndev == (int)(sizeof devs / sizeof *devs)) return NULL;
    dev_t2 *d = &devs[ndev]; *idx = ndev++;
    memset(d, 0, sizeof *d); d->h = h;
    UINT n = sizeof d->name - 1;
    if (h == NO_HANDLE) strcpy(d->name, "touchpad");
    else if (GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, d->name, &n) == (UINT)-1) strcpy(d->name, "?");
    const char *v = SDL_strcasestr(d->name, "VID_"), *p = SDL_strcasestr(d->name, "PID_");
    if (v && p) snprintf(d->label, sizeof d->label, "MOUSE %.4s:%.4s", v + 4, p + 4);
    else snprintf(d->label, sizeof d->label, "DEVICE %d", ndev);
    fprintf(stderr, "[GUNS] %s = %s\n", d->label, d->name);
    const int me = me_right();
    if (cfg_name[!me][0] && SDL_strcasestr(d->name, cfg_name[!me])) d->whose = 2;          /* named for the other screen */
    else if (mine < 0 && cfg_name[me][0] && SDL_strcasestr(d->name, cfg_name[me])) take(*idx);
    if (other_claim[0] && !strcmp(d->name, other_claim) && d->whose == 0) d->whose = 2;
    return d;
}

/* the other game took a device (src/s23_c422.c, "C42G") */
void s23_guns_other_claimed(const char *name)
{
    if (!strcmp(other_claim, name)) return;
    snprintf(other_claim, sizeof other_claim, "%s", name);
    fprintf(stderr, "[GUNS] the other screen's game uses %s\n", name);
    for (int i = 0; i < ndev; i++) if (!strcmp(devs[i].name, name) && devs[i].whose == 0) devs[i].whose = 2;
    save_cfg();
}

static void raw_mouse(HANDLE h, const RAWMOUSE *m)
{
    int i; dev_t2 *d = dev_of(h, &i);
    if (!d) return;
    const USHORT bf = m->usButtonFlags;
    if (d->whose == 0 && mine < 0 && (bf & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN))) {
        const int named = cfg_name[me_right()][0] != 0;                                  /* guns.cfg names ours: wait for that one */
        if (!named && (!me_right() || other_claim[0])) { take(i); save_cfg(); }          /* left: the first shot; right: once left has one */
    }
    if (i != mine) return;
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {                  /* a light gun: 0..65535 across the screen it points at */
        gabs = 1;
        gx = m->lLastX / 65535.0; gy = m->lLastY / 65535.0;
        if (m->lLastX <= 0 || m->lLastX >= 65535 || m->lLastY <= 0 || m->lLastY >= 65535) gx = -1;   /* off the screen */
        gmoves++;
    } else if (m->lLastX || m->lLastY) {                     /* a mouse: one count = one pixel of this screen */
        int ww = 1, wh = 1; if (gwin) SDL_GetWindowSize(gwin, &ww, &wh);
        if (gabs || gx < -0.02 || gx > 1.02) { gx = gx < 0 ? 0.5 : gx; gabs = 0; }
        gx += (double)m->lLastX / (ww > 0 ? ww : 1); gy += (double)m->lLastY / (wh > 0 ? wh : 1);
        if (gx < -0.02) gx = -0.02; if (gx > 1.02) gx = 1.02; if (gy < -0.02) gy = -0.02; if (gy > 1.02) gy = 1.02;   /* past the edge = off-screen */
        gmoves++;
    }
    if (bf & RI_MOUSE_LEFT_BUTTON_DOWN) gbuttons |= SDL_BUTTON_LMASK;   if (bf & RI_MOUSE_LEFT_BUTTON_UP) gbuttons &= ~SDL_BUTTON_LMASK;
    if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN) gbuttons |= SDL_BUTTON_RMASK;  if (bf & RI_MOUSE_RIGHT_BUTTON_UP) gbuttons &= ~SDL_BUTTON_RMASK;
    if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) gbuttons |= SDL_BUTTON_MMASK; if (bf & RI_MOUSE_MIDDLE_BUTTON_UP) gbuttons &= ~SDL_BUTTON_MMASK;
    if (bf & RI_MOUSE_BUTTON_4_DOWN) gbuttons |= SDL_BUTTON_X1MASK;     if (bf & RI_MOUSE_BUTTON_4_UP) gbuttons &= ~SDL_BUTTON_X1MASK;
    if (bf & RI_MOUSE_BUTTON_5_DOWN) gbuttons |= SDL_BUTTON_X2MASK;     if (bf & RI_MOUSE_BUTTON_5_UP) gbuttons &= ~SDL_BUTTON_X2MASK;
}

static void SDLCALL hook(void *ud, void *hwnd, unsigned int msg, Uint64 wp, Sint64 lp)
{
    (void)ud; (void)hwnd; (void)wp;
    if (msg != WM_INPUT) return;
    RAWINPUT ri; UINT sz = sizeof ri;
    if (GetRawInputData((HRAWINPUT)(intptr_t)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (ri.header.dwType == RIM_TYPEMOUSE) raw_mouse(ri.header.hDevice ? ri.header.hDevice : NO_HANDLE, &ri.data.mouse);
}

void s23_guns_init(SDL_Window *w)
{
    if (!s23_guns_on() || !w) return;
    gwin = w;
    if (getenv("S23_CABINET_DIR")) snprintf(cfg_path, sizeof cfg_path, "../guns.cfg");   /* beside the program, shared by both games */
    FILE *f = fopen(cfg_path, "r");
    if (f) {
        char line[300];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "left=", 5)) snprintf(cfg_name[0], sizeof cfg_name[0], "%s", line + 5);
            if (!strncmp(line, "right=", 6)) snprintf(cfg_name[1], sizeof cfg_name[1], "%s", line + 6);
        }
        fclose(f);
    }
    UINT n = 0; GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    RAWINPUTDEVICELIST list[32]; if (n > 32) n = 32;
    n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    if (n == (UINT)-1) n = 0;
    for (UINT i = 0; i < n; i++) if (list[i].dwType == RIM_TYPEMOUSE) { int k; dev_of(list[i].hDevice, &k); }
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    HWND hw = SDL_GetWindowWMInfo(w, &wm) ? wm.info.win.window : NULL;
    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, hw };   /* generic desktop / mouse, also while the other game has the focus */
    SDL_SetWindowsMessageHook(hook, NULL);
    if (!hw || !RegisterRawInputDevices(&rid, 1, sizeof rid)) fprintf(stderr, "[GUNS] Raw Input unavailable: the system pointer aims\n");
    else fprintf(stderr, "[GUNS] one gun a screen: %s%s\n", cfg_name[me_right()][0] ? "guns.cfg names this screen's gun: " : "the first gun to shoot is the left (red) player's, the next the right (blue) player's",
                 cfg_name[me_right()][0] ? cfg_name[me_right()] : "");
    if (!cfg_name[me_right()][0]) eng_ui_set_hint(me_right() ? "BLUE PLAYER: SHOOT AFTER THE RED PLAYER TO TAKE THIS SCREEN'S GUN" : "RED PLAYER: SHOOT FIRST TO TAKE THIS SCREEN'S GUN", 60 * 15);
}

/* this game's gun (src/eng/ss22_input.c, src/s23_window.c ss22_host_pointer): false = not in this mode (the system pointer aims).
 * sx, sy: where it points as a fraction of the window (the whole screen, full screen); on = it has a gun and points at the screen */
bool s23_guns_state(float *sx, float *sy, bool *on_screen, uint32_t *buttons, int *moves)
{
    if (!s23_guns_on() || !gwin) return false;
    *sx = (float)gx; *sy = (float)gy;
    *on_screen = mine >= 0 && gx >= 0 && gx <= 1 && gy >= 0 && gy <= 1;
    *buttons = mine >= 0 ? gbuttons : 0;
    *moves = gmoves;
    return true;
}
#else
void s23_guns_init(SDL_Window *w) { (void)w; }
void s23_guns_other_claimed(const char *name) { (void)name; }
bool s23_guns_state(float *nx, float *ny, bool *inside, uint32_t *buttons, int *moves) { (void)nx; (void)ny; (void)inside; (void)buttons; (void)moves; return false; }
#endif
