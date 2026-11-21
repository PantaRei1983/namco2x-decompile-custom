/*
 * ss22_host.c -- a Super System 22 game in a window: OpenGL context, the menu bar, the display modes (widescreen, window mode/size,
 * resolution, aspect, scaling), F12 screenshots, vsync or a timer at the board's 59.906 Hz, the sound card. The picture itself
 * is the shared engine's (engine/ss22_gl.c), and so are the menu (engine/eng_ui.c, Nuklear, as in Prop Cycle) and the display
 * settings (engine/eng_display.c); this file is the window around them, the same for Tokyo Wars and Dirt Dash (it was a copy in
 * each, two lines apart): what differs is the ss22_host_game the game passes in.
 *
 * Keys: Esc opens the menu (File / Display / Audio / Controls; pad R3 too), P pause, F11 or Alt+Enter fullscreen, F12
 * screenshot (screenshots/), and the cabinet's (the game's input module, rebindable in the menu).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "render_target.h"
#include "ss22_gl.h"
#include "eng_crosshair.h"
#include "audio_out.h"
#include "eng_display.h"
#include "link_host.h"
#include "eng_ui.h"
#include "eng_pad.h"
#include "eng_pace.h"
#include "eng_vsync.h"
#include "tex_bake.h"
#include "ss22_host.h"
#include "link_host.h"

/* the Esc menu's Network page (engine/link_host.c: the rows, link.cfg); an address row is typed key by key through the menu's key capture */
static void net_key(SDL_Scancode sc, void *u) { if (link_page_type(sc)) eng_ui_capture_key(net_key, u); }
static void net_change(int row, int dir) { link_page_change(row, dir); if (link_page_editing() && !eng_ui_capturing()) eng_ui_capture_key(net_key, NULL); }
static const eng_ui_page network_page = { "Network", 560, 210, 24, link_page_rows, link_page_has_value, NULL, link_page_text, net_change, link_page_notes, 190 };
#include "eng_ffb.h"
#include "gl_warn.h"


static const ss22_host_game *game;
static SDL_Window *win;
static SDL_GLContext glc;
static bool paused, shot_pending, capture_pending, headless_open, restart_req;
/* F9's recording copy and the frame number, from engine/ss22_run.c (NULL: a host without them -- Cyber Sled: the picture only) */
bool (*g_ss22_capture_rec)(const char *dst);
const uint32_t *g_ss22_capture_frame;
static eng_vsync vs;                               /* the frame pacer (engine/eng_vsync.h) */

static uint64_t now_ns(void)                     /* split the scaling: counter * 1e9 overflows 64 bits (and this works on Windows too) */
{
    const uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}

bool ss22_host_active(void) { return win != NULL; }
bool ss22_host_restart_requested(void) { return restart_req; }

static void volume_hook(int pct) { eng_audio_set_volume(pct); }

/* <tag>_<name>: the game's test environment variable */
static const char *genv(const char *name)
{
    char v[64]; snprintf(v, sizeof v, "%s_%s", game->tag, name);
    return getenv(v);
}

bool ss22_host_open(const ss22_host_game *g, int scale, bool fs)      /* scale <= 0: the saved window size */
{
    game = g;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    eng_ffb_start();                                 /* before the joysticks, or Windows never lists a wheel as haptic (engine/eng_ffb.h) */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "[HOST] SDL: %s\n", SDL_GetError()); return false; }
    eng_gl_context_attributes();
    eng_disp_load(game->cfg_file, scale, fs);                /* the saved display choices; --window N / --fullscreen override */
    if (game->aim) SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");      /* a light gun: the click that focuses the window is still a shot */
    g_eng_disp_light_gun = game->aim != NULL;
    win = SDL_CreateWindow(game->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           eng_disp_win_w(g_eng_disp.scale), eng_disp_win_h(g_eng_disp.scale),
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                           (g_eng_disp.winmode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "[HOST] window: %s\n", SDL_GetError()); return false; }
#ifdef _WIN32
    { extern SDL_GLContext eng_gl_create_win(SDL_Window **, const char **); const char *miss = NULL; glc = eng_gl_create_win(&win, &miss); }
#else
    glc = SDL_GL_CreateContext(win);
#endif
    if (!glc) { fprintf(stderr, "[HOST] OpenGL context: %s\n", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(win, glc);
    eng_vsync_init(&vs, win, genv("VSYNC"));      /* vsync only on a ~60 Hz display and only while it blocks; <TAG>_VSYNC=0/1 forces it */
    fprintf(stderr, "[HOST] OpenGL: %s; display %d Hz, %s\n", (const char *)glGetString(GL_RENDERER), vs.hz, eng_vsync_mode(&vs));
    eng_gl_warn_software((const char *)glGetString(GL_RENDERER));
    SDL_SetWindowMinimumSize(win, 320, 240);
    game->input_init();                            /* after the cfg: the key bindings */
    eng_ui_add_page(game->input_page());
    if (!game->no_link) eng_ui_add_page(&network_page);   /* link play: this cabinet's number and the cabinets' addresses (link.cfg) */
    if (game->extra_page) { const eng_ui_page *xp = game->extra_page(); if (xp) eng_ui_add_page(xp); }   /* (NULL: not this run) */
    if (!eng_ui_init(win, game->title)) fprintf(stderr, "[HOST] menu: Nuklear init failed\n");
    else if (eng_pad_present()) eng_ui_set_hint(ENG_PAD_MENU_HINT, 60 * 8);         /* a pad has no Esc: say how to reach the menu */
    tex_bake_window_defaults();                    /* a per-frame budget for cold texture bakes (engine/tex_bake.c; ENG_TEX_BUDGET) */
    eng_disp_set_volume_hook(volume_hook);
    eng_audio_set_gain(game->out_gain);
    const bool audio = eng_audio_open();
    eng_disp_attach(win);                          /* exclusive fullscreen, a size too big for this display; the volume */
    if (audio) game->snd_set_output(true);
    eng_vsync_resync(&vs);
    return true;
}

static int headless_w = 640;                         /* ENG_SHOT_W=<px>: --shots at that width (852 = 16:9) to see widescreen headless */
bool ss22_host_open_headless(void)
{
    const char *e = getenv("ENG_SHOT_W");
    if (e && atoi(e) >= 640 && atoi(e) <= 2560) headless_w = atoi(e);
    headless_open = eng_gl_open_headless(headless_w, 480);
    return headless_open;
}

void ss22_host_shot(const char *path)
{
    /* an offscreen 640x480 draw of the prepared frame, read back exactly as the engine drew it */
    int vw, vh;
    if (win) {
        int rw = 640, rh = 480;
        { static int nat = -1; if (nat < 0) nat = getenv("ENG_SHOT_NATIVE") != NULL;   /* tests: the shot at the window's render size (what the player sees) */
          if (nat) eng_disp_render_size(&rw, &rh); }
        rt_begin(win, rw, rh, &vw, &vh);
        ss22_draw(vw, vh);
        eng_gl_write_ppm(path, vw, vh);
    } else {
        vw = headless_w; vh = 480;
        ss22_draw(vw, vh);
        eng_gl_write_ppm(path, vw, vh);
    }
    FILE *chk = fopen(path, "rb");                   /* say so when the folder is missing, rather than "saved" */
    if (chk) { fclose(chk); fprintf(stderr, "[HOST] saved %s\n", path); } else fprintf(stderr, "[HOST] could not write %s (does the folder exist?)\n", path);
}

/* The prepared frame into the window: the engine draws it at the render size into the shared render target, which is scaled
 * into the picture rectangle; the menu, when open, goes over it. */
static SDL_Rect pic_r, gun_r;                        /* the last picture rectangle (drawable pixels) and the 4:3 part of it the light gun aims over */
static eng_pace *pace_log;                           /* set by present_with(): present() times the frame's work into it */
static void present(void)
{
    int rw, rh, vw, vh;
    eng_disp_render_size(&rw, &rh);
    rt_begin(win, rw, rh, &vw, &vh);
    struct timespec d0, d1; clock_gettime(CLOCK_MONOTONIC, &d0);
    ss22_draw(vw, vh);
    clock_gettime(CLOCK_MONOTONIC, &d1);
    { static int on = -1; if (on < 0) on = getenv("ENG_FTIME") != NULL;
      double ms = (d1.tv_sec - d0.tv_sec) * 1e3 + (d1.tv_nsec - d0.tv_nsec) * 1e-6;
      if (on && ms > 40.0) fprintf(stderr, "[FTIME-DRAW] ss22_draw %.1f ms\n", ms); }   /* the engine's draw (quads, sprites, text, post) vs the swap below */
    if (shot_pending) {                              /* F12: what the engine drew, before it is scaled to the window */
        shot_pending = false;
        char p[96]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); static int n;
        mkdir("screenshots", 0755);
        snprintf(p, sizeof p, "screenshots/%s_%04d%02d%02d_%02d%02d%02d_%d.ppm", game->shot_prefix, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, n++);
        if (eng_gl_write_ppm(p, vw, vh)) fprintf(stderr, "[HOST] saved %s\n", p);
    }
    static uint32_t own_frame; own_frame++;            /* a host without the lifted runtime (Cyber Sled) counts its own frames */
    const uint32_t rr_frame = g_ss22_capture_frame ? *g_ss22_capture_frame : own_frame;
    { static long at = -2;                           /* tests: SS22_CAPTURE_AT=<frame> presses F9 then */
      if (at == -2) { const char *e = getenv("SS22_CAPTURE_AT"); at = e ? atol(e) : -1; }
      if (at >= 0 && rr_frame == (uint32_t)at) { capture_pending = true; at = -1; } }
    if (capture_pending) {                           /* F9, A GLITCH CAPTURE: the picture and this session's recording up to now (replayed with
                                                      * --replay it reaches this frame exactly) -- captures/capture_<frame>.ppm / .rec */
        capture_pending = false;
        char p[96], r[96]; static char h[128];
        mkdir("captures", 0755);
        snprintf(p, sizeof p, "captures/capture_%06u.ppm", rr_frame);
        snprintf(r, sizeof r, "captures/capture_%06u.rec", rr_frame);
        const bool pic = eng_gl_write_ppm(p, vw, vh), rec = g_ss22_capture_rec && g_ss22_capture_rec(r);
        if (rec) snprintf(h, sizeof h, "GLITCH CAPTURED (F9): captures/capture_%06u", rr_frame);
        else snprintf(h, sizeof h, "PICTURE SAVED (F9), NO RECORDING: session recording is off (launcher Settings)");
        eng_ui_set_hint(h, 60 * 4);
        fprintf(stderr, "[HOST] capture at frame %u: %s%s%s\n", rr_frame, pic ? p : "(no picture)", rec ? " + " : "", rec ? r : " (no recording)");
    }
    const SDL_Rect r = eng_disp_picture_rect(rw, rh);
    pic_r = r; gun_r = r;
    if (g_eng_disp.wide) { const int w = (int)(r.h * 4.0 / 3.0 + 0.5); gun_r.x = r.x + (r.w - w) / 2; gun_r.w = w; }   /* the 2D layers and the gun stay 4:3 */
    rt_end_rect(win, r.x, r.y, r.w, r.h, eng_disp_sharp());
    if (game->aim && g_eng_disp.gun_border) {          /* the light gun's border (Sinden-style guns track it): white, all round the window */
        int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
        const float b = (float)(g_eng_disp.gun_border * (dw < dh ? dw : dh) / 100);
        glViewport(0, 0, dw, dh);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, dw, dh, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glDisable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glColor3f(1, 1, 1);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f((float)dw, 0); glVertex2f((float)dw, b); glVertex2f(0, b);
        glVertex2f(0, dh - b); glVertex2f((float)dw, dh - b); glVertex2f((float)dw, (float)dh); glVertex2f(0, (float)dh);
        glVertex2f(0, 0); glVertex2f(b, 0); glVertex2f(b, (float)dh); glVertex2f(0, (float)dh);
        glVertex2f(dw - b, 0); glVertex2f((float)dw, 0); glVertex2f((float)dw, (float)dh); glVertex2f(dw - b, (float)dh);
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
    }
    if (game->aim && g_eng_disp.crosshair && !eng_ui_is_open()) {
        float ax, ay;
        if (game->aim(&ax, &ay)) {                   /* the crosshair, over the picture */
            int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
            const float cx = gun_r.x + ax * gun_r.w, cy = gun_r.y + ay * gun_r.h, s = gun_r.h / 24.0f;
            glViewport(0, 0, dw, dh);
            glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, dw, dh, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
            glDisable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
            if (!eng_xhair_draw(cx, cy, (float)gun_r.h))   /* Display > Crosshair image (engine/eng_crosshair.h), else the built-in cross */
            for (int pass = 0; pass < 2; pass++) {   /* a dark outline under a red cross, readable on any picture */
                if (pass == 0) { glLineWidth(4.0f); glColor3f(0, 0, 0); } else { glLineWidth(2.0f); glColor3f(1, 0.1f, 0.1f); }
                glBegin(GL_LINES);
                glVertex2f(cx - s, cy); glVertex2f(cx - s * 0.3f, cy); glVertex2f(cx + s * 0.3f, cy); glVertex2f(cx + s, cy);
                glVertex2f(cx, cy - s); glVertex2f(cx, cy - s * 0.3f); glVertex2f(cx, cy + s * 0.3f); glVertex2f(cx, cy + s);
                glEnd();
            }
            glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
        }
    }   /* clears the window round the picture itself; a no-op when rt_begin drew straight into it */
    bool quit = false;
    eng_ui_draw(&quit);
    { static int dbg = -1, n, shot = -1;             /* <tag>_HOSTDBG=1: what is in the window's back buffer just before it is shown;
                                                      * <tag>_WINSHOT=<n>: that back buffer, whole, to screenshots/<prefix>_window_<n>.ppm at swap n */
      if (dbg < 0) { dbg = genv("HOSTDBG") != NULL; shot = genv("WINSHOT") ? atoi(genv("WINSHOT")) : 0; }
      ++n;
      if (dbg || shot) {
          int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
          glReadBuffer(GL_BACK);
          if (dbg && n % 60 == 0) {
              unsigned char px[3 * 16]; int lit = 0;
              for (int i = 0; i < 16; i++) {
                  glReadPixels(r.x + r.w * (i % 4 + 1) / 5, r.y + r.h * (i / 4 + 1) / 5, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px + 3 * i);
                  lit += px[3 * i] | px[3 * i + 1] | px[3 * i + 2] ? 1 : 0;
              }
              fprintf(stderr, "[HOST] frame %d: window %dx%d, render %dx%d, picture %d,%d %dx%d, %d of 16 sample pixels lit, GL error 0x%X\n",
                      n, dw, dh, rw, rh, r.x, r.y, r.w, r.h, lit, glGetError());
          }
          if (shot && n == shot) {
              char p[96]; snprintf(p, sizeof p, "screenshots/%s_window_%d.ppm", game->shot_prefix, n);
              mkdir("screenshots", 0755);
              if (eng_gl_write_ppm(p, dw, dh)) fprintf(stderr, "[HOST] saved %s (%dx%d)\n", p, dw, dh);
          }
      } }
    { static int slow = -1, every = 1, n;           /* <tag>_SLOWDRAW=<ms>[:<every>]: TEST, a slow graphics chip (every n-th frame takes ms longer) */
      if (slow < 0) { slow = 0; const char *e = genv("SLOWDRAW"); if (e) sscanf(e, "%d:%d", &slow, &every); if (every < 1) every = 1; }
      if (slow && ++n % every == 0) SDL_Delay((Uint32)slow); }
    if (pace_log) eng_pace_before_swap(pace_log);
    eng_vsync_capture(&vs, win);                     /* the picture, kept for a frame-rate lock above 60 */
    SDL_GL_SwapWindow(win);
}
static void present_with(eng_pace *p) { pace_log = p; present(); pace_log = NULL; }

/* the window's events: the menu first (it swallows them while open), then the host's own keys. false = quit */
static bool pump(void)
{
    SDL_Event e;
    eng_ui_input_begin();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { eng_ui_input_end(); return false; }
        game->input_event(&e);
        if (eng_ui_event(&e)) continue;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            const SDL_Scancode sc = e.key.keysym.scancode;
            if (sc == SDL_SCANCODE_ESCAPE) { eng_ui_set_open(true); game->input_neutral(); }
            if (sc == SDL_SCANCODE_P) {
                if (link_host_on()) fprintf(stderr, "[HOST] no pause while linked: the other cabinets would drop this one\n");
                else { paused = !paused; if (paused) game->input_neutral(); fprintf(stderr, "[HOST] %s\n", paused ? "paused" : "running"); }   /* neutral: no force (wheel motor, a kick) is held through the pause */
            }
            if (sc == SDL_SCANCODE_F12) shot_pending = true;
            if (sc == SDL_SCANCODE_F9) capture_pending = true;                  /* a glitch capture (above) */
            if (sc == SDL_SCANCODE_F8 && game->aim) { eng_disp_cycle_gun_border(); fprintf(stderr, "[HOST] light-gun border %d%%\n", g_eng_disp.gun_border); }
            if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) eng_disp_toggle_fullscreen();
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) { eng_ui_set_open(true); game->input_neutral(); fprintf(stderr, "[HOST] menu open (R3)\n"); }
        if (e.type == SDL_CONTROLLERDEVICEADDED) eng_ui_set_hint(ENG_PAD_MENU_HINT, 60 * 8);
    }
    /* THE STEAM DECK'S MENU BUTTON is Start, and the games need Start themselves (a tap): held for a second it opens the menu */
    if (!eng_ui_is_open() && eng_pad_start_hold(60)) { eng_ui_set_open(true); game->input_neutral(); fprintf(stderr, "[HOST] menu open (Start held)\n"); }
    eng_ui_input_end();
    bool quit = false;
    if (eng_ui_quit_requested()) { quit = true; restart_req = eng_ui_restart_requested(); }
    return !quit;
}

static void pace(void) { eng_vsync_after_frame(&vs, win); }

bool ss22_host_pointer(float *nx, float *ny, bool *inside)
{
    if (!win || gun_r.w <= 0 || gun_r.h <= 0) return false;
    int mx, my, ww, wh, dw, dh;
    SDL_GetMouseState(&mx, &my); SDL_GetWindowSize(win, &ww, &wh); SDL_GL_GetDrawableSize(win, &dw, &dh);
    const float px = (float)mx * dw / (ww ? ww : 1), py = (float)my * dh / (wh ? wh : 1);
    *nx = (px - gun_r.x) / gun_r.w; *ny = (py - gun_r.y) / gun_r.h;
    /* an absolute-mouse gun (Sinden, Gun4IR, OpenFIRE, Reaper, AimTrak) aimed OFF the screen reports the pointer clamped at the screen edge
     * (or a corner): the window's outermost pixel row/column is "off-screen", which is how those guns reload */
    const bool edge = mx <= 0 || my <= 0 || mx >= ww - 1 || my >= wh - 1;
    *inside = !edge && *nx >= 0 && *nx <= 1 && *ny >= 0 && *ny <= 1 && (SDL_GetWindowFlags(win) & SDL_WINDOW_MOUSE_FOCUS);
    return true;
}

bool ss22_host_frame(void)
{
    if (!win) return true;
    { static int at = -2, mp = 1, mr = 0, n; static const char *keys;      /* <tag>_MENU_AT=<frame>[:<page>:<row>], <tag>_MENU_KEYS=<u d l r o b n p ...>: tests drive the menu */
      if (at == -2) { at = -1; const char *e = genv("MENU_AT"); if (e) sscanf(e, "%d:%d:%d", &at, &mp, &mr); keys = genv("MENU_KEYS"); }
      if (at >= 0 && ++n == at) { if (mp >= 0) { eng_ui_goto(mp, mr); eng_ui_set_open(true); game->input_neutral(); for (const char *k = keys; k && *k; k++) eng_ui_nav(*k); }
        if (genv("MENU_PRESS")) {                                        /* <tag>_MENU_PRESS=<key name>: a key press for the menu (a binding row waiting for one) */
            SDL_Event ke; memset(&ke, 0, sizeof ke);
            ke.type = SDL_KEYDOWN; ke.key.state = SDL_PRESSED; ke.key.keysym.scancode = SDL_GetScancodeFromName(genv("MENU_PRESS"));
            SDL_PushEvent(&ke);
        } } }
    { static int cf = -2, cx, cy, n;                                     /* <tag>_MENU_CLICK=<frame>:<x>:<y>: a QUICK click (press and release in one
                                                                          * frame, a touchpad tap) at window x,y -- tests engine/eng_nk_click.h */
      if (cf == -2) { cf = -1; const char *e = genv("MENU_CLICK"); if (e) sscanf(e, "%d:%d:%d", &cf, &cx, &cy); }
      if (cf >= 0 && ++n == cf) {
          SDL_Event me; memset(&me, 0, sizeof me);
          me.type = SDL_MOUSEMOTION; me.motion.windowID = SDL_GetWindowID(win); me.motion.x = cx; me.motion.y = cy; SDL_PushEvent(&me);
          memset(&me, 0, sizeof me);
          me.type = SDL_MOUSEBUTTONDOWN; me.button.windowID = SDL_GetWindowID(win); me.button.button = SDL_BUTTON_LEFT; me.button.state = SDL_PRESSED;
          me.button.clicks = 1; me.button.x = cx; me.button.y = cy; SDL_PushEvent(&me);
          me.type = SDL_MOUSEBUTTONUP; me.button.state = SDL_RELEASED; SDL_PushEvent(&me);
          fprintf(stderr, "[HOST] test: quick click at %d,%d\n", cx, cy);
      } }
    { static int wf = -2, n; static float wy, wx;                       /* <tag>_WHEEL_TEST=<frame>:<y>:<x>: TEST, a touchpad scroll (y, x a frame)
                                                                         * from that frame on, as SDL sends it -- the trackball's scrolling */
      if (wf == -2) { wf = -1; const char *e = genv("WHEEL_TEST"); if (e) sscanf(e, "%d:%f:%f", &wf, &wy, &wx); }
      if (wf >= 0 && ++n >= wf) {
          SDL_Event we; memset(&we, 0, sizeof we);
          we.type = SDL_MOUSEWHEEL; we.wheel.windowID = SDL_GetWindowID(win); we.wheel.preciseY = wy; we.wheel.preciseX = wx;
          we.wheel.y = (Sint32)wy; we.wheel.x = (Sint32)wx; we.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
          SDL_PushEvent(&we);
      } }
    static eng_pace pl;
    static int ftm = -1; if (ftm < 0) ftm = getenv("ENG_FTIME") != NULL;
    struct timespec a0, a1, a2, a3; if (ftm) clock_gettime(CLOCK_MONOTONIC, &a0);
    if (!pump()) return false;
    if (game->aim) SDL_ShowCursor(eng_ui_is_open() || paused ? SDL_ENABLE : SDL_DISABLE);   /* the crosshair is the pointer */
    else { const int want = (!g_eng_disp.winmode || eng_ui_is_open()) ? SDL_ENABLE : SDL_DISABLE;   /* fullscreen hides the pointer for the game, the menu needs it */
           if (SDL_ShowCursor(SDL_QUERY) != want) SDL_ShowCursor(want); }
    if (!paused && !eng_ui_is_open()) game->input_update();
    else if (link_host_on()) game->input_neutral();  /* linked, the menu open: the game plays on, hands off */
    if (link_host_catch_up()) return true;           /* linked and behind the set: this frame is not drawn and not waited for (engine/link_host.c) */
    if (ftm) clock_gettime(CLOCK_MONOTONIC, &a1);
    eng_vsync_want(&vs, g_eng_disp.fps);            /* Display > Frame rate */
    if (eng_vsync_show(&vs)) present_with(&pl);     /* a lock below 60 shows only some frames */
    if (ftm) clock_gettime(CLOCK_MONOTONIC, &a2);
    pace();
    if (ftm) {                                       /* which part of a slow host frame: events / draw+swap / pacing wait */
        clock_gettime(CLOCK_MONOTONIC, &a3);
        double ev = (a1.tv_sec - a0.tv_sec) * 1e3 + (a1.tv_nsec - a0.tv_nsec) * 1e-6, pr = (a2.tv_sec - a1.tv_sec) * 1e3 + (a2.tv_nsec - a1.tv_nsec) * 1e-6,
               pc = (a3.tv_sec - a2.tv_sec) * 1e3 + (a3.tv_nsec - a2.tv_nsec) * 1e-6;
        if (ev + pr + pc > 40.0) fprintf(stderr, "[FTIME-HOST] events %.1f  present %.1f  pace %.1f ms\n", ev, pr, pc);
    }
    eng_pace_after(&pl, game->tag);
    while (!link_host_on() && (paused || eng_ui_is_open())) {   /* the game stops (P, or the menu); the window keeps answering. Not a
                                                     * linked one: a stopped cabinet goes silent and the others drop it until they restart */
        eng_pace_reset(&pl);
        if (game->paused_tick) game->paused_tick();
        if (!pump()) return false;
        if (paused || eng_ui_is_open()) {
            if (eng_ui_is_open()) { present(); if (!vs.vsync) SDL_Delay(12); }
            else SDL_Delay(10);
        }
        if (!paused && !eng_ui_is_open()) eng_vsync_resync(&vs);
    }
    return true;
}

void ss22_host_message(const char *const *lines, int n)
{
    if (!win) return;
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    eng_ui_draw_message(lines, n);
    { static int shot; const char *e = genv("WAITSHOT");       /* <tag>_WAITSHOT=<file.ppm>: TEST, the waiting screen as drawn (a screen
                                                                   * capture cannot see an exclusive full screen) */
      if (e && !shot && ++shot) { glReadBuffer(GL_BACK); if (eng_gl_write_ppm(e, dw, dh)) fprintf(stderr, "[HOST] saved %s (%dx%d)\n", e, dw, dh); } }
    SDL_GL_SwapWindow(win);
}

void ss22_host_close(void)
{
    eng_audio_close();
    eng_ui_shutdown();
    if (glc) SDL_GL_DeleteContext(glc);
    if (win) SDL_DestroyWindow(win);
    win = NULL; glc = NULL;
}
