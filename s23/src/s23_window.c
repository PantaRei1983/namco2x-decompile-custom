/*
 * s23_window.c -- Time Crisis 2 in a WINDOW: the OpenGL context, the menu bar (src/eng/eng_ui.c: File / Display / Audio, Nuklear -- TC2's own
 * copies of the System 22 games' menu), the display settings (src/eng/eng_display.c, saved in tc2.cfg) and THE FRAME PACER
 * (src/eng/eng_vsync.h): the game always runs at the board's 59.906 Hz (25.6 MHz / 814 / 525, the same as System 22); Display > Frame rate
 * chooses how many PICTURES a second -- Auto (the default: the display's rate, 60 on a 60 Hz display), or 24 / 30 / 50 / 60 / 75 / 90 /
 * 120 / 144 / 165 / 240.
 *
 * The lifted program owns the C stack (it never returns), so the window runs from the frame callback: s23_window_frame() at the start of
 * every vertical blank presents the frame, handles the window's events and, while the menu is open or the game is paused, stays there
 * (the game does not run). Keys: Esc the menu, P pause, F11 fullscreen, F12 a screenshot (screenshots/s23_NNNN.ppm), F8 the light gun's
 * border. Display > Crosshair turns the aiming cross on / off.
 * THE PICTURE is drawn on the GPU (src/s23_gl.c) at the internal resolution Display > Resolution chooses (640 x 480 times 1..4); with no
 * GLSL 1.30 (or S23_GPU=0) the software picture (src/s23_render.c) is shown instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "eng/eng_gl.h"
#include "eng/eng_display.h"
#include "eng/eng_ui.h"
#include "eng/eng_net_ui.h"
#include "eng/eng_vsync.h"
#include "eng/eng_cfg.h"
#include "eng/eng_crosshair.h"
#include "eng/eng_pace.h"
#include "eng/eng_pad.h"
#include "eng/eng_tile.h"
#include "eng/ss22_input.h"
#include "eng/ss22_host.h"
#include "s23_build.h"
#include "s23_game.h"
extern const ss22_input_game s23_input_game, ak_input_game;

int g_eng_hud_edges_on;                       /* eng_display.c's widescreen-HUD switch: no 2D HUD to move on this board yet */
void s23_text_render(uint32_t *rgba);

static SDL_Window *win;
static SDL_GLContext glc;
static eng_vsync vs;
static eng_pace pace;
static GLuint tex;
static int gpu;                                                /* the GPU renderer is up */
static GLuint gpu_tex;
static uint32_t pic[640 * 480];
static int paused, shots;
static char party_hint[96];                                    /* party (S23_PARTY): "PLAYER n: ..." for the first seconds */
const char *s23_party_hint(void) { return party_hint; }        /* ("" when not a party; a game's link shows it in place of its "LINKED") */
static int smooth_active;                                    /* motion smoothing: this game frame shows in-between frames */
static const s23_built *smooth_pic[3]; static double smooth_at[3]; static int smooth_np;   /* the frame's pictures and where each belongs (0..1] */
static const s23_built *smooth_shown;
static const char *smooth_tag;                                 /* ENG_SWAPLOG: which picture the next swap shows */
static int smooth_frozen;                                      /* the frame's slot may be rewritten (s23_render_frame_end ran): no more GPU draws from it */
static SDL_Rect pic_r;                                          /* the GAME's 4:3 area in drawable pixels: the gun aims and the crosshair live here */
bool s23_pic_rect_win(float *x, float *y, float *w, float *h);
/* widescreen (Display > Widescreen, GPU only): the picture takes the window's shape, (640 + 2 * ex) x 480 board pixels, Hor+ */
static int wide_ex(void)
{
    if (!gpu || !g_eng_disp.wide || !win) return 0;
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (dh <= 0) return 0;
    int e = (int)(240.0 * dw / dh + 0.5) - 320;
    return e < 0 ? 0 : e > 400 ? 400 : e;
}
/* the whole picture's rectangle for a picture ex board pixels wider each side, and the 4:3 game area inside it */
static SDL_Rect picture_rect(int ex)
{
    const SDL_Rect r = eng_disp_picture_rect(640 + 2 * ex, 480);
    pic_r.x = r.x + (int)((long)ex * r.w / (640 + 2 * ex)); pic_r.w = (int)(640L * r.w / (640 + 2 * ex)); pic_r.y = r.y; pic_r.h = r.h;
    return r;
}
/* the 4:3 game area in WINDOW units (the menu's coordinates; HiDPI: drawable pixels / scale) -- the subtitles (src/s23_subs.c) */
bool s23_pic_rect_win(float *x, float *y, float *w, float *h)
{
    int ww, wh, dw, dh;
    if (!win || pic_r.w <= 0 || pic_r.h <= 0) return false;
    SDL_GetWindowSize(win, &ww, &wh); SDL_GL_GetDrawableSize(win, &dw, &dh);
    const float s = ww > 0 && dw > 0 ? (float)ww / dw : 1.0f;
    *x = pic_r.x * s; *y = pic_r.y * s; *w = pic_r.w * s; *h = pic_r.h * s;
    return true;
}

/* the copied input module asks where the mouse / an absolute-mouse gun points in the picture; the window's outermost pixel = off-screen */
bool ss22_host_pointer(float *nx, float *ny, bool *inside)
{
    if (!win || pic_r.w <= 0 || pic_r.h <= 0) return false;
    { bool s23_guns_state(float *, float *, bool *, uint32_t *, int *);   /* one gun a screen (src/s23_guns.c, our fork): this game's own gun, */
      float sx, sy; bool on; uint32_t b; int mv, dw, dh;                  /* calibrated to the whole screen */
      if (s23_guns_state(&sx, &sy, &on, &b, &mv)) {
          SDL_GL_GetDrawableSize(win, &dw, &dh);
          *nx = (sx * dw - pic_r.x) / pic_r.w; *ny = (sy * dh - pic_r.y) / pic_r.h;
          { const float gx = gpu ? (float)s23_gl_wide() / 640.f : 0.f;   /* the side strips: the nearest 4:3 edge (below) */
            const bool st = *nx >= -gx && *nx <= 1 + gx; if (st) { if (*nx < 0) *nx = 0; if (*nx > 1) *nx = 1; }
            *inside = on && st && *ny >= 0 && *ny <= 1; }
          return true;
      } }
    int mx, my, ww, wh, dw, dh;
    SDL_GetMouseState(&mx, &my); SDL_GetWindowSize(win, &ww, &wh); SDL_GL_GetDrawableSize(win, &dw, &dh);
    const float px = (float)mx * dw / (ww ? ww : 1), py = (float)my * dh / (wh ? wh : 1);
    *nx = (px - pic_r.x) / pic_r.w; *ny = (py - pic_r.y) / pic_r.h;
    const bool edge = mx <= 0 || my <= 0 || mx >= ww - 1 || my >= wh - 1;
    /* widescreen: the side strips are picture the game never had -- a shot there is a shot at the 4:3 screen's nearest edge, not an
     * off-screen one (Crisis Zone reloaded on it: the user, "shots go funny"); the window's outermost pixel stays off-screen */
    const float sx = gpu ? (float)s23_gl_wide() / 640.f : 0.f;
    const bool in_strip = *nx >= -sx && *nx <= 1 + sx;
    if (in_strip) { if (*nx < 0) *nx = 0; if (*nx > 1) *nx = 1; }
    *inside = !edge && in_strip && *ny >= 0 && *ny <= 1 && (SDL_GetWindowFlags(win) & SDL_WINDOW_MOUSE_FOCUS);
    return true;
}

int s23_window_active(void) { return win != NULL; }
/* a fatal stop in a window: say so instead of just vanishing (the details are in the log / terminal) */
void s23_crash_box(const char *why)
{
    if (!win) return;
    char m[512];
    snprintf(m, sizeof m, "%s had to stop:\n\n%s\n\nYour settings and scores were not touched. Please report this, with %s.log if there is one.", s23_game.title, why, s23_game.tag);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, s23_game.title, m, win);
}

int s23_window_open(int scale, int fullscreen)
{
#ifdef _WIN32
    { /* ON BATTERY (the user: slight stutters unplugged, smooth on the charger): Windows' power throttling (EcoQoS) slows a program it
       * thinks can wait, and coarsens its timers -- the frame pacing then oversleeps. This program asks out of both, for 1 ms timers,
       * and a little more priority. (Looked up at run time: older Windows simply skip it.) */
      typedef struct { ULONG Version, ControlMask, StateMask; } PPTS;
      typedef BOOL (WINAPI *SPI)(HANDLE, int, LPVOID, DWORD);
      SPI spi = (SPI)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
      if (spi) { PPTS st = { 1, 0x1 | 0x4, 0 };           /* EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION: not throttled */
                 spi(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st); }
      typedef UINT (WINAPI *TBP)(UINT); HMODULE wm = LoadLibraryA("winmm.dll");
      TBP tbp = wm ? (TBP)(void (*)(void))GetProcAddress(wm, "timeBeginPeriod") : NULL; if (tbp) tbp(1);
      SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
      fprintf(stderr, "[HOST] power: asked out of power throttling, 1 ms timers, above-normal priority\n"); }
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "[HOST] SDL: %s\n", SDL_GetError()); return 0; }
    { void s23_audio_open(void); s23_audio_open(); }                  /* the C352 (src/s23_audio.c) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    { static char cfg[64]; snprintf(cfg, sizeof cfg, "%s.cfg", s23_game.tag); eng_disp_load(cfg, scale, fullscreen != 0); }
    g_eng_disp_light_gun = !s23_game.input || ((const ss22_input_game *)s23_game.input)->light_gun;   /* a light-gun game: the Display page shows the crosshair rows */
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    /* $S23_MONITOR=N (our fork, one gun a screen): full screen on the Nth monitor from the left; one monitor only = the left / right half of it */
    int mon = -1, wpos = (int)SDL_WINDOWPOS_CENTERED;
    { const char *m = getenv("S23_MONITOR");
      if (m && *m) {
          const int want = atoi(m), count = SDL_GetNumVideoDisplays();
          int order[16], k = 0; SDL_Rect r[16];
          for (int i = 0; i < count && k < 16; i++) if (SDL_GetDisplayBounds(i, &r[k]) == 0) order[k] = i, k++;
          for (int i = 1; i < k; i++) for (int j = i; j > 0 && r[j].x < r[j - 1].x; j--) { SDL_Rect t = r[j]; r[j] = r[j - 1]; r[j - 1] = t; int o = order[j]; order[j] = order[j - 1]; order[j - 1] = o; }
          if (k >= 2 && want >= 1 && want <= k) {
              mon = order[want - 1]; wpos = (int)SDL_WINDOWPOS_CENTERED_DISPLAY(mon); g_eng_disp.winmode = 1;
              fprintf(stderr, "[HOST] full screen on monitor %d of %d (from the left), %d x %d\n", want, k, r[want - 1].w, r[want - 1].h);
          } else {
              fprintf(stderr, "[HOST] monitor %d: this PC has %d monitor(s) -- a window on the %s half of the screen instead\n", want, k, want >= 2 ? "right" : "left");
              g_eng_disp.winmode = 0; { static char pe[32]; snprintf(pe, sizeof pe, "S23_WINPOS=%s", want >= 2 ? "right" : "left"); putenv(pe); }
          }
          SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");   /* the other screen's game takes the focus: this one stays up */
      } }
    win = SDL_CreateWindow(s23_game.window_title, wpos, wpos,
                           eng_disp_win_w(g_eng_disp.scale), eng_disp_win_h(g_eng_disp.scale),
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (g_eng_disp.winmode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "[HOST] window: %s\n", SDL_GetError()); return 0; }
    glc = SDL_GL_CreateContext(win);
    if (!glc) { fprintf(stderr, "[HOST] OpenGL: %s\n", SDL_GetError()); return 0; }
    SDL_GL_MakeCurrent(win, glc);
    eng_disp_attach(win);
    { void s23_guns_init(SDL_Window *); s23_guns_init(win); }    /* one gun a screen ($S23_OWN_GUN, src/s23_guns.c, our fork) */
    { const char *wp = getenv("S23_WINPOS");       /* our fork, the link's "try both on this PC": left / right = that half of the screen (no dragging:
                                                    * a window being dragged stops its game, and the other cabinet loses it) -- the two windows
                                                    * side by side, edge to edge, in the middle of the screen (eng/eng_tile.h) */
      int wi = 0, wn = 0;                          /* or N/M: window N of M in a grid (several linked copies, e.g. Gunmen Wars' 4) */
      int s23_party(void), s23_party_pad(void);
      if (wp && s23_party() && sscanf(wp, "%d/%d", &wi, &wn) == 2 && wi >= 1 && wi <= wn) {
          /* PARTY (the launcher's split screen, as the other games' --party): no frames, the copies together fill the WHOLE screen (over the
           * task bar, kept on top) -- side by side for 2, a 2 x 2 grid for 3-4; each picture keeps its shape inside its part */
          SDL_Rect b; const int di = SDL_GetWindowDisplayIndex(win);
          if (SDL_GetDisplayBounds(di, &b) == 0) {
              const int pc = wn <= 1 ? 1 : 2, pr = (wn + pc - 1) / pc, tw = b.w / pc, th = b.h / pr;
              SDL_SetWindowBordered(win, SDL_FALSE); SDL_SetWindowAlwaysOnTop(win, SDL_TRUE);
              SDL_SetWindowSize(win, tw, th); SDL_SetWindowPosition(win, b.x + ((wi - 1) % pc) * tw, b.y + ((wi - 1) / pc) * th);
              fprintf(stderr, "[HOST] party: player %d of %d, %d x %d\n", wi, wn, tw, th);
          }
          { const int pd = s23_party_pad();   /* (shown once the menu is up, below) */
            if (pd >= 0) snprintf(party_hint, sizeof party_hint, "PLAYER %d: CONTROLLER %d", wi, pd + 1);
            else snprintf(party_hint, sizeof party_hint, "PLAYER %d: KEYBOARD AND MOUSE (CLICK HERE FIRST)", wi); }
          if (s23_party_pad() < 0) SDL_RaiseWindow(win);   /* the keyboard's copy in front, so the keys reach it */
      } else if (wp && !g_eng_disp.winmode) { if (sscanf(wp, "%d/%d", &wi, &wn) == 2 && wi >= 1 && wi <= wn) eng_tile_window(win, wi - 1, wn);
                                       else eng_tile_window(win, wp[0] == 'r' || wp[0] == 'R', 2); } }
    eng_vsync_init(&vs, win, getenv("S23_VSYNC"));
    s23_smooth_set(eng_cfg_int("motion_smooth", 0));
    eng_pace_reset(&pace);
    fprintf(stderr, "[HOST] OpenGL: %s; display %d Hz, %s\n", (const char *)glGetString(GL_RENDERER), vs.hz, eng_vsync_mode(&vs));
    if (!eng_ui_init(win, s23_game.window_title)) fprintf(stderr, "[HOST] no menu\n");
    if (party_hint[0]) { void eng_ui_set_hint(const char *, int); eng_ui_set_hint(party_hint, 60 * 15); }   /* party: whose picture this is */
    ss22_input_init(s23_game.input ? (const ss22_input_game *)s23_game.input : s23_game.io_asca3 ? &ak_input_game : &s23_input_game);                    /* the controls (keys, mouse, light gun, pads) and the menu's Controls page */
    eng_ui_add_page(ss22_input_page());
    if (!party_hint[0] && eng_pad_present()) eng_ui_set_hint(ENG_PAD_MENU_HINT, 60 * 8);   /* a pad has no Esc: how to reach the menu (as engine/ss22_host.c) */
    { void s23_subs_ui_init(void); s23_subs_ui_init(); }   /* English subtitles (src/s23_subs.c): only a game with a table */
    if (s23_game.menu_pages) s23_game.menu_pages();
    if (s23_game.twin && !s23_game.link) { extern const eng_ui_page s23_c422_page; eng_ui_add_page(&s23_c422_page); }   /* Time Crisis II: the Network page (src/s23_c422.c) */
    /* (the author's 0.9.1 online link, src/s23_link.c + its pages, is not built in our fork: the twin-cabinet LAN link is ours,
     * src/s23_c422.c, set up in tc2_link.cfg) */
    (void)picture_rect(0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 640, 480, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    { const char *e = getenv("S23_SOFTWARE"); gpu = (e && *e == '1') ? 0 : s23_gl_init(); }   /* $S23_SOFTWARE=1: the software picture */
    { void s23_render_soft(int); s23_render_soft(!gpu); }       /* the worker then only builds the frame; the GPU draws it */
    return 1;
}

static void draw_picture(void)
{
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    const SDL_Rect r = picture_rect(gpu ? s23_gl_wide() : 0);
    if (g_eng_disp.gun_border) {                                /* F8: a white frame round the picture for Sinden-style light guns */
        const int bd = g_eng_disp.gun_border * (dw < dh ? dw : dh) / 100;
        glEnable(GL_SCISSOR_TEST); glScissor(r.x - bd, dh - r.y - r.h - bd, r.w + 2 * bd, r.h + 2 * bd);
        glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 1);
    }
    glViewport(r.x, dh - r.y - r.h, r.w, r.h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, gpu ? gpu_tex : tex);
    const GLint f = eng_disp_sharp() ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0); glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);   /* the default again: the menu (Nuklear) colours a white texel by its vertices */
    glViewport(0, 0, dw, dh);
}

/* the OS pointer, shown or hidden. SDL hides it by answering WM_SETCURSOR with no cursor, and on Windows that is not always enough: a
 * light gun that drives the pointer as an absolute mouse (RS3 Reaper, GitHub #55) left it on screen in play, where Linux hid it. Windows
 * also keeps a per-program display COUNTER (Win32 ShowCursor): below zero the pointer is hidden over our window whatever WM_SETCURSOR
 * does, so on Windows the counter is held below zero too while the pointer should be hidden, and put back when it should show. */
static void set_pointer(int show)
{
    if (SDL_ShowCursor(SDL_QUERY) != (show ? SDL_ENABLE : SDL_DISABLE)) SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
#ifdef _WIN32
    static int hidden;                                           /* our own decrement of the counter is in force */
    if (!show && !hidden) { int guard = 0; while (ShowCursor(FALSE) >= 0 && ++guard < 64) {} hidden = 1; }
    else if (show && hidden) { int guard = 0; while (ShowCursor(TRUE) < 0 && ++guard < 64) {} hidden = 0; }
#endif
}

/* the aiming CROSSHAIR (Display > Crosshair, ON by default, saved as crosshair=): at the aim point -- the mouse until the gun I/O is in --
 * over the picture; the OS cursor hidden there while it is on, and everywhere in play while it is off */
static void draw_crosshair(void)
{
    float ax, ay; int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    const int on = ss22_input_aim(&ax, &ay) && !(s23_game.hide_crosshair && s23_game.hide_crosshair());   /* the copied input module: mouse, absolute-mouse gun, either stick, the keys; a game may hide it */
    /* the OS pointer: shown on the menu; in play hidden over the picture with the crosshair on, and ALWAYS with the crosshair off (a light-gun
     * player turns the crosshair off to see nothing but the game) */
    set_pointer(eng_ui_is_open() ? 1 : (!g_eng_disp.crosshair || on) ? 0 : 1);
    if (!g_eng_disp.crosshair || !on || eng_ui_is_open()) return;
    const SDL_Rect r = pic_r;
    const float x = r.x + ax * r.w, y = r.y + ay * r.h;
    const float k = r.h / 480.0f, a = 14 * k, gap = 4 * k, w = k < 1.5f ? 1.5f : k;
    glViewport(0, 0, dw, dh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, dw, dh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    if (eng_xhair_draw(x, y, (float)r.h)) { glColor3f(1, 1, 1); return; }   /* Display > Crosshair image (src/eng/eng_crosshair.h) */
    for (int pass = 0; pass < 2; pass++) {                    /* a dark outline, then the bright cross: visible on any background */
        glLineWidth(pass ? w : w + 2);
        { int s23_guns_on(void), s23_c422_link_right(void);       /* one gun a screen: the right screen's player is blue (our fork) */
          if (!pass) glColor3f(0, 0, 0); else if (s23_guns_on() && s23_c422_link_right()) glColor3f(0.3f, 0.55f, 1.0f); else glColor3f(1.0f, 0.25f, 0.2f); }
        glBegin(GL_LINES);
        glVertex2f(x - a, y); glVertex2f(x - gap, y); glVertex2f(x + gap, y); glVertex2f(x + a, y);
        glVertex2f(x, y - a); glVertex2f(x, y - gap); glVertex2f(x, y + gap); glVertex2f(x, y + a);
        glEnd();
    }
    glColor3f(1, 1, 1); glLineWidth(1);
}

static void screenshot_to(const char *p);
static void screenshot(void)
{
    mkdir("screenshots", 0755);
    char p[64]; snprintf(p, sizeof p, "screenshots/tc2_%04d.ppm", shots++);
    screenshot_to(p);
}
/* F9, GLITCH CAPTURE (our fork): the picture AND a save state of this moment (src/s23_savestate.c, saved at the next frame) into
 * captures\ beside the game -- to be loaded and looked into later: S23_LOADSTATE=captures/capture_<frame>.state */
static void glitch_capture(void)
{
    extern uint32_t rr_frame; void s23_ss_request(const char *); void eng_ui_set_hint(const char *, int);
    mkdir("captures", 0755);
    char p[96]; snprintf(p, sizeof p, "captures/capture_%06u.ppm", rr_frame); screenshot_to(p);
    snprintf(p, sizeof p, "captures/capture_%06u.state", rr_frame); s23_ss_request(p);
    static char h[96]; snprintf(h, sizeof h, "GLITCH CAPTURED (F9): captures/capture_%06u", rr_frame); eng_ui_set_hint(h, 60 * 4);
}
static void screenshot_to(const char *p)
{
    FILE *f = fopen(p, "wb"); if (!f) return;
    if (gpu) {                                                   /* the whole GPU picture: its internal resolution, widescreen included */
        const int w = (640 + 2 * s23_gl_wide()) * s23_gl_scale(), h = 480 * s23_gl_scale();
        uint8_t *rgb = malloc((size_t)w * h * 3);
        if (rgb && s23_gl_read_full(rgb, w * h * 3)) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); }
        free(rgb);
    } else {
        fprintf(f, "P6\n640 480\n255\n");
        for (int i = 0; i < 640 * 480; i++) { const unsigned char c[3] = { (unsigned char)(pic[i] >> 16), (unsigned char)(pic[i] >> 8), (unsigned char)pic[i] }; fwrite(c, 1, 3, f); }
    }
    fclose(f); fprintf(stderr, "[HOST] screenshot %s\n", p);
}

static void draw_crosshair(void);
/* motion smoothing: the picture for a tick at frac (0..1) of the game frame -- the first of the frame's pictures whose place is at or after it
 * (at 120: 1/2 then N; at 165: 1/3, 2/3, N), so each tick moves on and none shows an older state than the tick before */
static void smooth_draw(double frac)
{
    int k = 0;
    while (k + 1 < smooth_np && smooth_at[k] < frac) k++;
    /* after s23_render_frame_end the old frame's slot is the back slot: its snapshot is overwritten and the worker rebuilds it, so the old
     * frame's last ticks re-show what gpu_tex holds instead of drawing from it (a data race, seen as torn pictures with smoothing on) */
    if (smooth_pic[k] && smooth_pic[k] != smooth_shown && !smooth_frozen) { gpu_tex = s23_gl_draw(smooth_pic[k]); smooth_shown = smooth_pic[k]; }
    { static char t[32]; snprintf(t, sizeof t, "at%.2f", smooth_at[k]); smooth_tag = t; }
}
static void smooth_extra(void *ctx, double frac) { (void)ctx; smooth_draw(frac); eng_vsync_extra_tag = smooth_tag; draw_picture(); draw_crosshair(); }

/* the new frame's picture(s): the newest built frame drawn on the GPU at the chosen internal resolution (or the software picture), and
 * with MOTION SMOOTHING (Display > Motion smoothing, src/s23_render.c) and more than one picture a game frame, its in-between frames */
static void prepare_frame(void *ctx)
{
    (void)ctx;
    smooth_active = 0; smooth_np = 0; smooth_shown = NULL; smooth_tag = "frame"; smooth_frozen = 0;
    if (gpu) {
        /* the internal resolution is a whole multiple of the board's 480 lines; "Native (window size)" (res_h 0) takes the window's height
         * through eng_disp_render_size like every other game -- it used to be a fixed 1x (640x480) whatever the window (GitHub #44) */
        { int rw, rh; eng_disp_render_size(&rw, &rh); s23_gl_set_scale((rh + 240) / 480); }
        s23_gl_set_wide(wide_ex());
        const s23_built *b = s23_built_latest();
        s23_smooth_rate(vs.lock);
        const int nm = s23_smooth_on() && vs.lock && vs.tp < ENG_FRAME_NS && b ? s23_built_mid_count() : 0;
        if (nm) {                                                /* the in-between frames at 1/2 (or 1/3, 2/3), then frame N at 1 */
            smooth_active = 1;
            for (int k = 0; k < nm; k++) { smooth_pic[smooth_np] = s23_built_mid(k); smooth_at[smooth_np++] = nm == 1 ? 0.5 : (k + 1) / 3.0; }
            smooth_pic[smooth_np] = b; smooth_at[smooth_np++] = 1.0;
            smooth_draw(0.0);                                    /* the first one is ready for the period's first tick */
        } else if (b) gpu_tex = s23_gl_draw(b);
    } else { s23_text_render(pic); glBindTexture(GL_TEXTURE_2D, tex); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 640, 480, GL_BGRA, GL_UNSIGNED_BYTE, pic); }
}
/* polled presentation: a tick's picture into the back buffer */
static void present_draw(void *ctx, double frac)
{
    (void)ctx;
    if (smooth_active) smooth_draw(frac); else smooth_tag = "frame";
    eng_vsync_extra_tag = smooth_tag;
    draw_picture();
    draw_crosshair();
    eng_ui_draw(NULL);                                           /* closed: the hint line and the online chat overlay */
}
/* from the CPU's slice loop (src/s23_cpu.c rr_tick): serve a picture tick that came due while the game runs; the clock is read every 8th slice */
void (*s23_present_hook)(void);
static void present_poll(void)
{
    static unsigned n;
    if (++n & 7) return;
    if (vs.live && eng_vsync_ns() >= vs.pnext) eng_vsync_poll(&vs, win);
}

/* one window pass: events, the picture (+ the menu), the swap, the pacing. Returns 0 to quit. */
static int pass(int new_frame)
{
    bool quit = false;
    SDL_Event e;
    if (new_frame) smooth_frozen = 1;                            /* s23_render_frame_end has just run (see smooth_draw) */
    eng_ui_input_begin();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = true;
        if (eng_ui_event(&e)) continue;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE: eng_ui_set_open(true); ss22_input_neutral(); break;
            case SDL_SCANCODE_P: { int s23_c422_linked(void);
                if (s23_c422_linked()) { fprintf(stderr, "[HOST] no pause while linked: the other cabinet would lose this one\n"); break; }
                paused = !paused; fprintf(stderr, "[HOST] %s\n", paused ? "paused" : "running"); break; }
            case SDL_SCANCODE_F11: eng_disp_toggle_fullscreen(); break;
            case SDL_SCANCODE_F12: screenshot(); break;
            case SDL_SCANCODE_F9: glitch_capture(); break;      /* our fork: picture + save state (captures\) */
            case SDL_SCANCODE_F6: if (!g_eng_disp_light_gun) break; { void ss22_input_flash_step(int); ss22_input_flash_step(1); { extern int ss22_input_gun_flash(void); char b[48]; const int f = ss22_input_gun_flash(); void eng_ui_set_hint(const char *, int); if (f) snprintf(b, sizeof b, "Gun shot flash: %d%%", f); else snprintf(b, sizeof b, "Gun shot flash: OFF"); eng_ui_set_hint(b, 120); } } break;   /* the shot flash's strength: 100, 75, 50, 25, OFF */
            case SDL_SCANCODE_F7: if (!g_eng_disp_light_gun) break; { void ss22_input_mouse_step(int); ss22_input_mouse_step(1); } break;   /* the mouse speed: Direct, 125, 150, 200, 300 % */
            case SDL_SCANCODE_F8: if (g_eng_disp_light_gun) eng_disp_cycle_gun_border(); break;     /* the light gun's white border round the picture (Sinden-style guns) */
            default: break;
            }
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) { eng_ui_set_open(true); ss22_input_neutral(); fprintf(stderr, "[HOST] menu open (R3)\n"); }
        if (e.type == SDL_CONTROLLERDEVICEADDED) eng_ui_set_hint(ENG_PAD_MENU_HINT, 60 * 8);
        ss22_input_event(&e);                                    /* pads hot-plugged */
    }
    /* a pad's way to the menu, as every System 22 game (eng/eng_pad.h): R3 above, or Start held a second (a tap stays the game's Start) */
    if (!eng_ui_is_open() && eng_pad_start_hold(60)) { eng_ui_set_open(true); ss22_input_neutral(); fprintf(stderr, "[HOST] menu open (Start held)\n"); }
    eng_ui_input_end();
    { /* tests: S23_MENU_AT=frame:page:row opens the menu there (S23_MENU_KEYS=<u d l r o b n p> then drives it) */
      static long at = -2, n; static int mp, mr;
      if (at == -2) { at = -1; const char *e = getenv("S23_MENU_AT"); if (e) sscanf(e, "%ld:%d:%d", &at, &mp, &mr); }
      if (new_frame && at > 0 && ++n == at) { eng_ui_goto(mp, mr); eng_ui_set_open(true); ss22_input_neutral();
          for (const char *k = getenv("S23_MENU_KEYS"); k && *k; k++) eng_ui_nav(*k); }
    }
    if (eng_ui_is_open() || paused) ss22_input_neutral(); else if (new_frame) ss22_input_update();
    eng_vsync_want(&vs, g_eng_disp.fps);
    /* A FIXED PICTURE RATE (Display > Frame rate other than 60, Auto on a 60+ Hz display), the game running: POLLED PRESENTATION -- the
     * frame on screen keeps its ticks to the end of its period, then this one's pictures are prepared and served on their ticks, also
     * while the CPUs run the next frame (s23_present_poll from the slice loop): before, every tick inside the ~7 ms the emulation takes
     * waited for it (measured: 6.5% of pictures late at 120, 10% at 165, then a short gap -- an uneven rate) */
    if (new_frame && vs.lock && !vs.vsync && !vs.swap_blocks && !eng_ui_is_open() && !paused) {   /* not with vsync: the swap blocks, the timer cannot place the ticks */
        vs.draw = present_draw; vs.extra = NULL;
        eng_vsync_next_frame(&vs, win, prepare_frame, NULL);
        s23_present_hook = present_poll;
        eng_pace_after(&pace, s23_game.tag);
        return !(quit || eng_ui_quit_requested());
    }
    s23_present_hook = NULL;
    if (vs.live) eng_vsync_resync(&vs);                          /* leaving polled presentation (menu, pause, a blocking swap): clocks from now */
    if (new_frame) prepare_frame(NULL);
    if (!new_frame) smooth_active = 0;                            /* the menu / pause: the kept picture (menu included) is re-shown */
    vs.extra = smooth_active ? smooth_extra : NULL;
    if (eng_vsync_show(&vs)) {
        draw_picture();
        draw_crosshair();
        eng_ui_draw(&quit);                                     /* the menu, or with it closed the hint line (our fork: was the menu only) */
        { /* tests: S23_WINSHOT=path:pass reads the whole window back (menu included) to a PPM */
          static long at = -2; static char path[512]; static long n;
          if (at == -2) { at = -1; const char *e = getenv("S23_WINSHOT"); if (e) { snprintf(path, sizeof path, "%s", e); char *c = strrchr(path, ':'); if (c) { *c = 0; at = atol(c + 1); } } }
          n++;                                                  /* window passes: the menu holds the game, not the window */
          static long every = -1; static char dir[512];         /* S23_WINSHOTS=dir:every -- the same, every N passes (README pictures) */
          if (every < 0) { every = 0; const char *e = getenv("S23_WINSHOTS"); if (e) { snprintf(dir, sizeof dir, "%s", e); char *c = strrchr(dir, ':'); if (c) { *c = 0; every = atol(c + 1); } } }
          if (every > 0 && n % every == 0) { snprintf(path, sizeof path, "%s/w%06ld.ppm", dir, n); at = n; }
          if (at > 0 && n == at) {
              int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
              unsigned char *buf = malloc((size_t)dw * dh * 3);
              glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, dw, dh, GL_RGB, GL_UNSIGNED_BYTE, buf);
              FILE *f = fopen(path, "wb");
              if (f) { fprintf(f, "P6\n%d %d\n255\n", dw, dh); for (int y = dh - 1; y >= 0; y--) fwrite(buf + (size_t)y * dw * 3, 1, (size_t)dw * 3, f); fclose(f); }
              free(buf); fprintf(stderr, "[HOST] window -> %s\n", path); at = -1;
          }
        }
        eng_pace_before_swap(&pace);
        eng_vsync_capture(&vs, win);
        SDL_GL_SwapWindow(win);
        eng_vsync_note_swap(smooth_active ? smooth_tag : "frame");
    }
    eng_vsync_after_frame(&vs, win);
    eng_pace_after(&pace, s23_game.tag);
    return !(quit || eng_ui_quit_requested());
}

/* THE LINK'S POWER-ON WAIT (src/s23_c422.c s23_c422_gate; our fork): one pass of the window with two lines in a box. S = play alone, Esc / the
 * window closed = quit. */
int s23_window_wait_pump(const char *l1, const char *l2)
{
    if (!win) { SDL_Delay(10); return 0; }
    int key = 0;
    SDL_Event e;
    eng_ui_input_begin();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) key = 'Q';
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.scancode == SDL_SCANCODE_S) key = 'S';
            if (e.key.keysym.scancode == SDL_SCANCODE_F11) eng_disp_toggle_fullscreen();
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);          /* (a pad's buttons arrive once it is open) */
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_START) key = 'S';   /* a pad's Start = S */
    }
    eng_ui_input_end();
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    glViewport(0, 0, dw, dh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    const char *lines[4] = { s23_game.link && s23_game.link->title ? s23_game.link->title : "TIME CRISIS II  -  TWIN CABINET LINK", "", l1, l2 };
    eng_ui_draw_message(lines, 4);
    { static int n; const char *ws = getenv("S23_WAITSHOT");   /* tests: the wait screen to a PPM (its 30th pass) */
      if (ws && ++n == 30) { unsigned char *buf = malloc((size_t)dw * dh * 3); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
          glReadPixels(0, 0, dw, dh, GL_RGB, GL_UNSIGNED_BYTE, buf); FILE *f = fopen(ws, "wb");
          if (f) { fprintf(f, "P6\n%d %d\n255\n", dw, dh); for (int y = dh - 1; y >= 0; y--) fwrite(buf + (size_t)y * dw * 3, 1, (size_t)dw * 3, f); fclose(f); }
          free(buf); } }
    SDL_GL_SwapWindow(win);
    SDL_Delay(16);
    return key;
}

/* the frame callback (the start of vertical blank): present it; hold here while the menu is open or the game is paused */
void s23_window_frame(void)
{
    if (!win) return;
    int ok = pass(1);
    int s23_c422_linked(void);                                   /* LINKED (our fork): the game plays on under the menu -- a cabinet that stops goes */
    while (ok && !s23_c422_linked() && (eng_ui_is_open() || paused)) { ok = pass(0); if (!eng_ui_is_open() && !paused) eng_vsync_resync(&vs); }   /* silent and the other one carries on without it */
    if (!ok) { fprintf(stderr, "[HOST] quit\n"); { void s23_c422_report(void); s23_c422_report(); } { void s23_backup_save(int); s23_backup_save(1); } { void s23_out_close(void); s23_out_close(); } ss22_input_close();
        bool eng_ui_restart_requested(void); const bool restart = eng_ui_restart_requested();   /* File > Restart: power-cycle -- the same program again */
        eng_ui_shutdown(); SDL_Quit();
        if (restart) { static wchar_t exe[32768], cl[32768]; STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;   /* (it only quit before) */
            const DWORD n = GetModuleFileNameW(NULL, exe, 32768); wcsncpy(cl, GetCommandLineW(), 32767); fflush(NULL);
            if (n > 0 && n < 32768 && CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
            else fprintf(stderr, "[HOST] the restart failed (error %lu)\n", (unsigned long)GetLastError()); }
        exit(0); }
}
