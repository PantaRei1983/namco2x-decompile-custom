/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/eng_ui.c (copied 2026-10-05). TC2 is independent of the System 22 project: change it here. */
/*
 * eng_ui.c -- see eng_ui.h. The panel is Rave Racer's (raverace/src/rr_ui.c), made game-independent: the pages are
 * tables of callbacks, and the File / Display / Audio pages every game shares live here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <SDL.h>
#include "eng_gl.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION
#include "../../third_party/nuklear.h"
#include "../../third_party/nuklear_sdl_gl2.h"
#include "eng_nk_style.h"                      /* our fork: the amber selected row, the volume boxes on the keys */
#include "eng_nk_click.h"                      /* our fork: quick clicks / touchpad taps */

#include "eng_ui.h"
#include "eng_crosshair.h"
#include "ss22_input.h"
#include "eng_cfg.h"
#include "eng_vsync.h"
#include "eng_display.h"

#define MAXPAGES 8
static struct nk_context *ctx;
static SDL_Window *uwin;
static bool open_, quit_req, restart_req;
static struct nk_font *big_font[3];            /* larger copies of the default font (20 / 28 / 40): the subtitles */
const struct nk_user_font *eng_ui_font_big(float h)   /* the largest of them no taller than h window units (else the smallest) */
{ struct nk_font *f = big_font[0]; for (int i = 1; i < 3; i++) if (big_font[i] && big_font[i]->handle.height <= h) f = big_font[i]; return f ? &f->handle : NULL; }
static float ui_scale = 1.0f;                 /* drawable pixels per window unit (HiDPI) */
static const eng_ui_page *pages[MAXPAGES];
static int npages;
static int tab, row;                          /* row -1 = the title strip */
static bool kb_moved;                         /* keep the selected row in view after a key */
static void (*cap_cb)(SDL_Scancode, void *);
static bool (*input_cap_cb)(const SDL_Event *, void *);
static void *cap_u;

static int cyc(int v, int d, int n) { return ((v + d) % n + n) % n; }

/* ---- File ------------------------------------------------------------------ */
static int file_n(void) { return 3; }
static bool file_val(int r) { (void)r; return false; }
static void file_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    (void)vn; *v = 0;
    snprintf(l, ln, "%s", r == 0 ? "Resume" : r == 1 ? "Restart" : "Exit");
}
static void file_change(int r, int dir)
{
    if (dir != 0) return;
    if (r == 0) open_ = false;
    else if (r == 1) { restart_req = true; quit_req = true; }     /* power-cycle: the host re-launches the program */
    else quit_req = true;
}
static const eng_ui_page page_file = { "File", 220, 0, 0, file_n, file_val, NULL, file_text, file_change, NULL };

int s23_depth_on(void); void s23_depth_set(int on);     /* src/s23_render.c: exact depth */
int s23_smooth_on(void); void s23_smooth_set(int on);   /* src/s23_render.c: motion smoothing */
extern int s23_out_net, s23_out_rumble, s23_out_lamps;                  /* src/s23_out.c: the recoil outputs */
void s23_out_set_net(int on), s23_out_set_rumble(int on), s23_out_set_lamps(int on);
extern int s23_pic_c404, s23_pic_gamma, s23_pic_bright;   /* src/s23_render.c: the picture adjustments */
extern int s23_first_person; int s23_game_has_view(void); int s23_game_mounted_gun(void); int s23_game_no_smooth(void);   /* src/s23_render.c: Gunmen Wars' fighter view */
static void bar(char *v, size_t vn, int val, int lo, int hi, const char *txt)   /* "[######----] txt": where val sits in lo..hi */
{
    char b[11]; const int k = (val - lo) * 10 / (hi - lo);
    for (int i = 0; i < 10; i++) b[i] = i < k ? '#' : '-';
    b[10] = 0; snprintf(v, vn, "[%s] %s", b, txt);
}
/* ---- Display: the choices of engine/eng_display.h ------------------------------ */
enum { D_WIDE, D_MODE, D_SIZE, D_RES, D_ASPECT, D_SCALING, D_HUD, D_FPS, D_TEXF, D_ANISO, D_SMOOTH, D_DEPTH, D_C404, D_GAMMA, D_BRIGHT, D_VIEW, D_CROSS, D_XIMG, D_FLASH, D_OUTNET, D_LAMPS, D_RUMBLE, D_N };   /* D_CROSS.. last: only light-gun games show them */
/* the rows THIS game shows (each game its own: Fighter view only where the game has it, the crosshair for an aimed gun, the shot flash and
 * the recoil / lamp outputs only for a light gun) -- the page's row r is vis[r] */
static int vis[D_N], nvis;
static int disp_n(void)
{
    nvis = 0;
    for (int d = 0; d < D_N; d++) {
        if (d == D_VIEW && !s23_game_has_view()) continue;
        if (d == D_SMOOTH && s23_game_no_smooth()) continue;          /* (off for good in this game) */
        if (d >= D_CROSS && !g_eng_disp_light_gun) continue;
        if (d >= D_FLASH && s23_game_mounted_gun()) continue;
        if (d == D_OUTNET || d == D_LAMPS || d == D_RUMBLE) continue;   /* (on the Controls page now, with the gun) */
        vis[nvis++] = d;
    }
    return nvis;
}
static int vrow(int r) { if (!nvis) disp_n(); return r >= 0 && r < nvis ? vis[r] : D_N; }
static bool disp_enabled(int r)
{
    r = vrow(r);
    if (r == D_SIZE) return g_eng_disp.winmode == 0;
    if (r == D_ASPECT) return !g_eng_disp.wide;
    if (r == D_HUD) return g_eng_disp.wide;
    if (r == D_ANISO) return g_eng_disp.tex_filter == 2 || g_eng_disp.tex_filter == 4;     /* anisotropy rides on the trilinear modes */
    if (r == D_XIMG) return g_eng_disp.crosshair;
    if (r == D_VIEW) return s23_game_has_view();
    return true;
}
static void disp_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    *v = 0; *l = 0; r = vrow(r);
    switch (r) {
    case D_WIDE:    snprintf(l, ln, "Widescreen"); snprintf(v, vn, "%s", g_eng_disp.wide ? "ON (fill the window)" : "OFF (4:3)"); break;
    case D_MODE:    snprintf(l, ln, "Window mode"); snprintf(v, vn, "%s", eng_disp_winmode_name(g_eng_disp.winmode)); break;
    case D_SIZE:    snprintf(l, ln, "Window size");
                    snprintf(v, vn, "%dx  (%d x %d)", g_eng_disp.scale, eng_disp_win_w(g_eng_disp.scale), eng_disp_win_h(g_eng_disp.scale)); break;
    case D_RES:     snprintf(l, ln, "Resolution"); eng_disp_res_label(g_eng_disp.res_w, g_eng_disp.res_h, v, vn); break;
    case D_ASPECT:  snprintf(l, ln, "Aspect ratio"); snprintf(v, vn, "%s", eng_disp_aspect_name(g_eng_disp.aspect)); break;
    case D_SCALING: snprintf(l, ln, "Scaling"); snprintf(v, vn, "%s", eng_disp_scaling_name(g_eng_disp.scaling)); break;
    case D_HUD:     snprintf(l, ln, "Widescreen HUD"); snprintf(v, vn, "%s", g_eng_disp.hud_edges ? "at the screen edges" : "original (4:3 centre)"); break;
    case D_TEXF:    snprintf(l, ln, "3D texture filter"); snprintf(v, vn, "%s", g_eng_disp.tex_filter == 4 ? "Sharp trilinear (3D only)" : g_eng_disp.tex_filter == 3 ? "Sharp bilinear (3D only)" : g_eng_disp.tex_filter == 2 ? "Trilinear (3D only)" : g_eng_disp.tex_filter ? "Bilinear (3D only)" : "Off (arcade)"); break;
    case D_ANISO:   snprintf(l, ln, "Anisotropic filter"); if (g_eng_disp.tex_aniso) snprintf(v, vn, "%dx", g_eng_disp.tex_aniso); else snprintf(v, vn, "Off"); break;
    case D_FPS:     snprintf(l, ln, "Frame rate"); snprintf(v, vn, "%s", eng_vsync_rate_name(g_eng_disp.fps)); break;
    case D_SMOOTH:  snprintf(l, ln, "Motion smoothing (100+ fps)"); snprintf(v, vn, "%s", s23_smooth_on() ? "ON (in-between pictures, +8 ms lag)" : "OFF (as the arcade)"); break;
    case D_DEPTH:   snprintf(l, ln, "Depth"); snprintf(v, vn, "%s", s23_depth_on() ? "exact (no z-fighting)" : "arcade sorting"); break;
    case D_C404:    { char t[48]; snprintf(t, sizeof t, s23_pic_c404 == 100 ? "100%% (the board's table)" : s23_pic_c404 ? "%d%%" : "0%% (as MAME)", s23_pic_c404);
                      snprintf(l, ln, "Arcade gamma"); bar(v, vn, s23_pic_c404, 0, 100, t); break; }
    case D_GAMMA:   { char t[32]; snprintf(t, sizeof t, "%.2f%s", s23_pic_gamma / 100.0, s23_pic_gamma == 100 ? " (neutral)" : s23_pic_gamma > 100 ? " (brighter mids)" : " (darker mids)");
                      snprintf(l, ln, "Gamma"); bar(v, vn, s23_pic_gamma, 50, 200, t); break; }
    case D_BRIGHT:  { char t[32]; snprintf(t, sizeof t, "%d%%%s", s23_pic_bright, s23_pic_bright == 100 ? " (neutral)" : "");
                      snprintf(l, ln, "Brightness"); bar(v, vn, s23_pic_bright, 50, 150, t); break; }
    case D_VIEW:    snprintf(l, ln, "Fighter view"); snprintf(v, vn, "%s", !s23_game_has_view() ? "(not in this game)" : s23_first_person ? "first person (your fighter hidden)" : "behind your fighter"); break;
    case D_CROSS:   snprintf(l, ln, "Crosshair"); snprintf(v, vn, "%s", g_eng_disp.crosshair ? "ON" : "OFF"); break;
    case D_XIMG:    snprintf(l, ln, "Crosshair image"); snprintf(v, vn, eng_xhair_count() ? "%s" : "%s (no PNG in crosshairs/)", eng_xhair_name()); break;
    case D_FLASH:   snprintf(l, ln, "Gun shot flash"); if (ss22_input_gun_flash() == 100) snprintf(v, vn, "100%% (as the arcade)"); else if (ss22_input_gun_flash()) snprintf(v, vn, "%d%%", ss22_input_gun_flash()); else snprintf(v, vn, "OFF (no white flash)"); break;
    case D_OUTNET:  snprintf(l, ln, "Recoil outputs (Hook of the Reaper)"); snprintf(v, vn, "%s", s23_out_net ? "ON (tcp port 8000)" : "OFF"); break;
    case D_LAMPS:   snprintf(l, ln, "Lamp outputs (lamp0-7)"); snprintf(v, vn, "%s", s23_out_lamps ? "ON" : "OFF (recoil only)"); break;
    case D_RUMBLE:  snprintf(l, ln, "Pad rumble on recoil"); snprintf(v, vn, "%s", s23_out_rumble ? "ON" : "OFF"); break;
    }
}
static void disp_change(int r, int dir)
{
    if (!disp_enabled(r)) return;
    r = vrow(r);
    const int d = dir ? dir : 1;
    switch (r) {
    case D_WIDE:    eng_disp_set_wide(!g_eng_disp.wide); break;
    case D_MODE:    eng_disp_set_winmode(cyc(g_eng_disp.winmode, d, 3)); break;
    case D_SIZE:    eng_disp_set_scale(cyc(g_eng_disp.scale - 1, d, 4) + 1); break;
    case D_RES: {
        int i = 0, n = eng_disp_res_count(), w, h;
        for (int k = 0; k < n; k++) { eng_disp_res_get(k, &w, &h); if (w == g_eng_disp.res_w && h == g_eng_disp.res_h) i = k; }
        eng_disp_res_get(cyc(i, d, n), &w, &h);
        eng_disp_set_res(w, h);
        break; }
    case D_ASPECT:  eng_disp_set_aspect(cyc(g_eng_disp.aspect, d, 4)); break;
    case D_SCALING: eng_disp_set_scaling(cyc(g_eng_disp.scaling, d, 3)); break;
    case D_HUD:     eng_disp_set_hud_edges(!g_eng_disp.hud_edges); break;
    case D_FPS:     eng_disp_set_fps(eng_vsync_rate_step(g_eng_disp.fps, d)); break;
    case D_TEXF:    { static const int ord[5] = { 0, 1, 3, 2, 4 }; int i = 0; for (int k = 0; k < 5; k++) if (ord[k] == g_eng_disp.tex_filter) i = k;   /* Off, Bilinear, Sharp bilinear, Trilinear, Sharp trilinear */
                      eng_disp_set_texfilter(ord[cyc(i, d, 5)]); break; }
    case D_ANISO:   { static const int lv[5] = { 0, 2, 4, 8, 16 }; int i = 0; for (int k = 0; k < 5; k++) if (lv[k] == g_eng_disp.tex_aniso) i = k;
                      eng_disp_set_texaniso(lv[cyc(i, d, 5)]); break; }
    case D_SMOOTH:  s23_smooth_set(!s23_smooth_on()); eng_cfg_set_int("motion_smooth", s23_smooth_on()); break;
    case D_DEPTH:   s23_depth_set(!s23_depth_on()); eng_cfg_set_int("depth", s23_depth_on()); break;
    case D_C404:    s23_pic_c404 = dir ? s23_pic_c404 + 10 * dir : (s23_pic_c404 >= 100 ? 0 : s23_pic_c404 + 10);     /* Left/Right 10% steps, Enter cycles */
                    if (s23_pic_c404 < 0) s23_pic_c404 = 0; if (s23_pic_c404 > 100) s23_pic_c404 = 100; eng_cfg_set_int("pic_c404", s23_pic_c404); break;
    case D_GAMMA:   s23_pic_gamma = dir ? s23_pic_gamma + 5 * dir : (s23_pic_gamma >= 200 ? 50 : s23_pic_gamma + 5);    /* 0.05 steps */
                    if (s23_pic_gamma < 50) s23_pic_gamma = 50; if (s23_pic_gamma > 200) s23_pic_gamma = 200; eng_cfg_set_int("pic_gamma", s23_pic_gamma); break;
    case D_BRIGHT:  s23_pic_bright = dir ? s23_pic_bright + 5 * dir : (s23_pic_bright >= 150 ? 50 : s23_pic_bright + 5); /* 5% steps */
                    if (s23_pic_bright < 50) s23_pic_bright = 50; if (s23_pic_bright > 150) s23_pic_bright = 150; eng_cfg_set_int("pic_bright", s23_pic_bright); break;
    case D_VIEW:    s23_first_person = !s23_first_person; eng_cfg_set_int("first_person", s23_first_person); break;
    case D_CROSS:   eng_disp_set_crosshair(!g_eng_disp.crosshair); break;
    case D_XIMG:    eng_xhair_step(d); break;
    case D_FLASH:   ss22_input_flash_step(dir); break;
    case D_OUTNET:  s23_out_set_net(!s23_out_net); break;
    case D_LAMPS:   s23_out_set_lamps(!s23_out_lamps); break;
    case D_RUMBLE:  s23_out_set_rumble(!s23_out_rumble); break;
    }
}
static void labelf(nk_flags align, const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, align);
}
static void disp_notes(void (*line)(const char *fmt, ...))
{
    (void)line;
    int rw, rh, ow, oh;
    eng_disp_render_size(&rw, &rh);
    SDL_GL_GetDrawableSize(uwin, &ow, &oh);
    nk_spacing(ctx, 1);
    labelf(NK_TEXT_LEFT, "Rendering %d x %d  ->  window %d x %d", rw, rh, ow, oh);
    labelf(NK_TEXT_LEFT, "Resolution = render size; the window keeps its size.");
    labelf(NK_TEXT_LEFT, g_eng_disp.wide ? "Widescreen: more of the scene at the sides." : " ");
    /* the resolutions as a scrollable list, as in Prop Cycle's Display menu (a combo would be a popup inside a popup) */
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, "Resolution (render size, scaled to the window)", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 132, 1);
    if (nk_group_begin(ctx, "resolutions", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 18, 1);
        for (int i = 0; i < eng_disp_res_count(); i++) {
            int w, h; char b[64];
            eng_disp_res_get(i, &w, &h);
            eng_disp_res_label(w, h, b, sizeof b);
            const bool on = w == g_eng_disp.res_w && h == g_eng_disp.res_h;
            if (nk_option_label(ctx, b, on) && !on) eng_disp_set_res(w, h);
        }
        nk_group_end(ctx);
    }
}
static const eng_ui_page page_display = { "Display", 440, 120, 26, disp_n, NULL, disp_enabled, disp_text, disp_change, disp_notes };

/* ---- Audio ------------------------------------------------------------------- */
extern int g_c352_gain_music, g_c352_gain_fx, g_c352_gain_voice;   /* c352.c: the music, effects and speech gains (percent) */
static int aud_n(void) { return 5; }                  /* Volume, Music, Effects, Voice; 4 = the volume boxes (our fork: a row for the keys) */
static int *aud_ptr(int r) { return r == 1 ? &g_c352_gain_music : r == 3 ? &g_c352_gain_voice : &g_c352_gain_fx; }
static void aud_set(int r, int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (r == 0) { eng_disp_set_volume(v); return; }
    *aud_ptr(r) = v; eng_cfg_set_int(r == 1 ? "music_volume" : r == 3 ? "voice_volume" : "effects_volume", v);
}
static int aud_get(int r) { return r == 0 ? g_eng_disp.volume : *aud_ptr(r); }
static void aud_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    if (r == 4) { snprintf(l, ln, "Volume boxes"); *v = 0; return; }
    snprintf(l, ln, "%s", r == 0 ? "Volume" : r == 1 ? "Music volume" : r == 2 ? "Effects volume" : "Voice volume"); snprintf(v, vn, "%d%%", aud_get(r));
}
static void aud_change(int r, int dir)
{
    if (r == 4) { const int b = eng_vol_box_key(dir, g_eng_disp.volume); if (b >= 0) eng_disp_set_volume(b); return; }   /* Left / Right pick, Enter sets */
    int v = aud_get(r) + (dir ? dir * 5 : 10);
    if (v > 100) v = dir ? 100 : 0;                          /* Enter wraps 100 -> mute */
    aud_set(r, v);
}
static void aud_notes(void (*line)(const char *fmt, ...))
{
    (void)line;
    eng_vol_boxes(ctx, row == 4, g_eng_disp.volume, eng_disp_set_volume);
}
static const eng_ui_page page_audio = { "Audio", 300, 120, 26, aud_n, NULL, NULL, aud_text, aud_change, aud_notes };

/* ---- the panel ------------------------------------------------------------------- */
static int nrows(int t) { return pages[t]->nrows(); }
static bool has_value(int t, int r) { return pages[t]->has_value ? pages[t]->has_value(r) : true; }
static bool row_enabled(int t, int r) { return pages[t]->enabled ? pages[t]->enabled(r) : true; }
static bool is_audio(int t) { return pages[t] == &page_audio; }

void eng_ui_add_page(const eng_ui_page *p) { if (npages < MAXPAGES) pages[npages++] = p; }

/* ENG_UI_DUMP=1 (tests: menu audits): every page's rows as text to the log, once, the first time the menu is drawn */
static void ui_dump(void)
{
    static int done; if (done || !getenv("ENG_UI_DUMP")) return; done = 1;
    for (int t = 0; t < npages; t++) {
        const int n = pages[t]->nrows ? pages[t]->nrows() : 0;
        fprintf(stderr, "[UIDUMP] == %s (%d rows)\n", pages[t]->name, n);
        for (int r = 0; r < n; r++) { char l[160] = "", v[200] = ""; if (pages[t]->text) pages[t]->text(r, l, sizeof l, v, sizeof v); fprintf(stderr, "[UIDUMP] %s | %s | %s\n", pages[t]->name, l, v); }
    }
}

bool eng_ui_init(SDL_Window *win, const char *title)
{
    (void)title;
    uwin = win;
    ctx = nk_sdl_init(win);
    if (!ctx) return false;
    /* HiDPI: draw at the drawable's density, lay out in window units (the backend's mouse coordinates are window units) */
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GL_GetDrawableSize(win, &ow, &oh);
    ui_scale = ww > 0 ? (float)ow / ww : 1.0f;
    if (ui_scale < 1.0f) ui_scale = 1.0f;
    struct nk_font_atlas *atlas;
    struct nk_font_config cfg = nk_font_config(0);
    nk_sdl_font_stash_begin(&atlas);
    struct nk_font *font = nk_font_atlas_add_default(atlas, 15 * ui_scale, &cfg);
    { static const float bs[3] = { 20, 28, 40 }; for (int i = 0; i < 3; i++) big_font[i] = nk_font_atlas_add_default(atlas, bs[i] * ui_scale, &cfg); }   /* the subtitles' (src/s23_subs.c) */
    nk_sdl_font_stash_end();
    font->handle.height /= ui_scale;
    eng_nk_style_highlight(ctx);
    for (int i = 0; i < 3; i++) big_font[i]->handle.height /= ui_scale;
    nk_style_set_font(ctx, &font->handle);
    /* the three every game shares come first; the game's pages are added after */
    if (npages == 0) { pages[npages++] = &page_file; pages[npages++] = &page_display; pages[npages++] = &page_audio; }
    else {                                     /* pages were added before init: put the standard ones in front */
        const eng_ui_page *g[MAXPAGES]; int n = npages;
        memcpy(g, pages, sizeof g);
        npages = 0;
        pages[npages++] = &page_file; pages[npages++] = &page_display; pages[npages++] = &page_audio;
        for (int i = 0; i < n && npages < MAXPAGES; i++) pages[npages++] = g[i];
    }
    tab = 1; row = 0;
    if (getenv("ENG_UI_OPEN")) {               /* tests: open on a page at startup, "<page>[:<row>]" */
        int p = 1, r = 0; sscanf(getenv("ENG_UI_OPEN"), "%d:%d", &p, &r);
        eng_ui_goto(p, r); open_ = true;
    }
    return true;
}
void eng_ui_shutdown(void) { if (ctx) nk_sdl_shutdown(); ctx = NULL; }
bool eng_ui_is_open(void) { return open_; }
bool eng_ui_quit_requested(void) { return quit_req; }
bool eng_ui_restart_requested(void) { return restart_req; }
void eng_ui_set_open(bool on) {
    open_ = on; cap_cb = NULL;
    if (input_cap_cb) { input_cap_cb(NULL, cap_u); input_cap_cb = NULL; }
    if (on && row >= nrows(tab)) row = 0;
}
void eng_ui_input_begin(void) { if (ctx) { nk_input_begin(ctx); nkc_begin(); } }
void eng_ui_input_end(void)   { if (ctx) nk_input_end(ctx); }
void eng_ui_capture_key(void (*cb)(SDL_Scancode, void *), void *u) { input_cap_cb = NULL; cap_cb = cb; cap_u = u; }
void eng_ui_capture_input(bool (*cb)(const SDL_Event *, void *), void *u) { cap_cb = NULL; input_cap_cb = cb; cap_u = u; }
bool eng_ui_capturing(void) { return cap_cb != NULL || input_cap_cb != NULL; }
#define MAXLAYERS 4
static const eng_ui_layer *layers[MAXLAYERS]; static int nlayers;
void eng_ui_add_layer(const eng_ui_layer *l) { if (nlayers < MAXLAYERS) layers[nlayers++] = l; }
static bool layer_modal(void) { for (int i = 0; i < nlayers; i++) if (layers[i]->modal && layers[i]->modal()) return true; return false; }
void eng_ui_goto(int p, int r) { if (p >= 0 && p < npages) { tab = p; row = r; } }

/* one navigation step from the keyboard, a pad or a hat */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_TABPREV, K_TABNEXT };
static void nav(int k)
{
    const int n = nrows(tab);
    switch (k) {
    case K_UP:    row = row <= -1 ? n - 1 : row - 1; break;
    case K_DOWN:  row = row >= n - 1 ? -1 : row + 1; break;
    case K_LEFT:  if (row < 0) tab = cyc(tab, -1, npages); else if (has_value(tab, row)) pages[tab]->change(row, -1); break;
    case K_RIGHT: if (row < 0) tab = cyc(tab, +1, npages); else if (has_value(tab, row)) pages[tab]->change(row, +1); break;
    case K_OK:    if (row < 0) row = 0; else if (row_enabled(tab, row)) pages[tab]->change(row, 0); break;
    case K_BACK:  open_ = false; break;
    case K_TABPREV: tab = cyc(tab, -1, npages); row = 0; break;
    case K_TABNEXT: tab = cyc(tab, +1, npages); row = 0; break;
    }
    if (row >= nrows(tab)) row = nrows(tab) - 1;
    kb_moved = true;
}
void eng_ui_nav(char c)
{
    static const char keys[] = "udlrobpn";
    static const int code[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_TABPREV, K_TABNEXT };
    const char *p = strchr(keys, c);
    if (p && *p) nav(code[p - keys]);
}

bool eng_ui_event(SDL_Event *e)
{
    if (!ctx) return false;
    for (int i = 0; i < nlayers; i++)                    /* a layer first (the chat box, the lobby window's keys) */
        if (layers[i]->event && layers[i]->event(e, open_)) return true;
    if (!open_) return false;
    if (input_cap_cb) {
        if (input_cap_cb(e, cap_u)) input_cap_cb = NULL;
        return true;
    }
    if (cap_cb) {                                        /* the next key is the new binding */
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            void (*cb)(SDL_Scancode, void *) = cap_cb; cap_cb = NULL;
            cb(e->key.keysym.scancode == SDL_SCANCODE_ESCAPE ? SDL_SCANCODE_UNKNOWN : e->key.keysym.scancode, cap_u);
        } else if (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            void (*cb)(SDL_Scancode, void *) = cap_cb; cap_cb = NULL;
            cb(SDL_SCANCODE_UNKNOWN, cap_u);
        }
        return true;
    }
    if (layer_modal()) {                                 /* a layer's window has the keyboard: only the mouse goes on to Nuklear */
        if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP && e->type != SDL_TEXTINPUT && e->type != SDL_CONTROLLERBUTTONDOWN) nk_sdl_handle_event(e);
        return true;
    }
    int k = -1;
    if (e->type == SDL_KEYDOWN) {
        const bool rep = e->key.repeat;                  /* held arrows repeat; the rest do not */
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: k = K_UP; break;
        case SDL_SCANCODE_DOWN: k = K_DOWN; break;
        case SDL_SCANCODE_LEFT: k = K_LEFT; break;
        case SDL_SCANCODE_RIGHT: k = K_RIGHT; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (!rep) k = K_OK; break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: if (!rep) k = K_BACK; break;
        case SDL_SCANCODE_TAB: if (!rep) k = (e->key.keysym.mod & KMOD_SHIFT) ? K_TABPREV : K_TABNEXT; break;
        case SDL_SCANCODE_PAGEUP: if (!rep) k = K_TABPREV; break;
        case SDL_SCANCODE_PAGEDOWN: if (!rep) k = K_TABNEXT; break;
        default: break;
        }
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: k = K_UP; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k = K_DOWN; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k = K_LEFT; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k = K_RIGHT; break;
        case SDL_CONTROLLER_BUTTON_A: k = K_OK; break;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_RIGHTSTICK: k = K_BACK; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: k = K_TABPREV; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: k = K_TABNEXT; break;
        default: break;
        }
    } else if (e->type == SDL_JOYHATMOTION && !SDL_GameControllerFromInstanceID(e->jhat.which)) {   /* raw sticks: the hat navigates
                                                         * (a game controller's D-pad is also its joystick's hat: one press, one step -- as engine/eng_ui.c) */
        if (e->jhat.value & SDL_HAT_UP) k = K_UP; else if (e->jhat.value & SDL_HAT_DOWN) k = K_DOWN;
        else if (e->jhat.value & SDL_HAT_LEFT) k = K_LEFT; else if (e->jhat.value & SDL_HAT_RIGHT) k = K_RIGHT;
    }
    else if (e->type == SDL_CONTROLLERAXISMOTION && (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)) {
        /* the LEFT STICK navigates as the d-pad does (our fork: the user could not reach Restart with a pad's stick) -- one step per push */
        static int held[2]; const int ax = e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY, v = e->caxis.value;
        const int dir = v > 20000 ? 1 : v < -20000 ? -1 : 0;
        if (dir && dir != held[ax]) k = ax ? (dir > 0 ? K_DOWN : K_UP) : (dir > 0 ? K_RIGHT : K_LEFT);
        if (v > -12000 && v < 12000) held[ax] = 0; else if (dir) held[ax] = dir;
    }
    if (k >= 0) { nav(k); return true; }
    if (e->type == SDL_CONTROLLERAXISMOTION) return true;   /* (the sticks do not move the game while the menu is open) */
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) return true;   /* the game does not see keys while open */
    nkc_event(e);                                        /* mouse (a quick click held a frame: eng_nk_click.h) */
    return true;
}

/* ---- drawing ---------------------------------------------------------------- */
static int note_count;
static void count_line(const char *fmt, ...) { (void)fmt; note_count++; }   /* a page's notes, counted (the dropdown's height) */
static void note_line(const char *fmt, ...)      /* what a page's notes() calls: one line under the rows */
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, NK_TEXT_LEFT);
}

static char hint_text[96]; static int hint_left;
void eng_ui_set_hint(const char *text, int frames) { snprintf(hint_text, sizeof hint_text, "%s", text ? text : ""); hint_left = frames; }

/* a few centred lines in a box over the picture (the twin-cabinet link's power-on wait, src/s23_c422.c; our fork) */
void eng_ui_draw_message(const char *const *lines, int n)
{
    if (!ctx || n <= 0) return;
    int ww, wh; SDL_GetWindowSize(uwin, &ww, &wh);
    const float w = 600 < ww - 16 ? 600.0f : (float)ww - 16, h = 24.0f + (float)n * 22;
    if (nk_begin(ctx, "linkwait", nk_rect(((float)ww - w) / 2, ((float)wh - h) / 2, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER | NK_WINDOW_NO_INPUT)) {
        nk_layout_row_dynamic(ctx, 18, 1);
        for (int i = 0; i < n; i++) nk_label(ctx, lines[i], NK_TEXT_CENTERED);
    }
    nk_end(ctx);
    nk_sdl_render(NK_ANTI_ALIASING_ON);
}

/* THE LINK LIGHTS (our fork; src/s23_c422.c s23_c422_lights, drawn as Tokyo Wars' and Rave Racer's engine/link_lights_nk.h): "LINK" and one
 * light per cabinet, 1 = left (red player), 2 = right (blue player), while the menu is closed. true = drawn */
/* a game with more cabinets (Gunmen Wars, gunwars/src/gw_link.c) sets this: n lights, cabinet k's kind / lit, *self = this one (0-based) */
int (*eng_link_lights_n)(int kind[8], int lit[8], int *self, int *n);
static bool draw_link_lights(void)
{
    int s23_c422_lights(int kind[2], int lit[2]), s23_c422_link_right(void);
    int kind[8], lit[8], self = -1, n = 2;
    const int pos = eng_link_lights_n ? eng_link_lights_n(kind, lit, &self, &n) : s23_c422_lights(kind, lit);
    if (!pos || n < 1) return false;
    if (n > 8) n = 8;
    int ww, wh; SDL_GetWindowSize(uwin, &ww, &wh);
    const float bw = 24, bh = 20, lw = 36, w = lw + (float)n * (bw + 4) + 20, h = bh + 14;
    const float x = pos == 1 || pos == 4 ? 6.0f : pos == 3 || pos == 6 ? (float)ww - w - 6 : ((float)ww - w) / 2;
    const float y = pos >= 4 ? 6.0f : (float)wh - h - 6;
    if (nk_begin(ctx, "linklights", nk_rect(x, y, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
        static const struct nk_color col[] = { { 60, 130, 255, 255 }, { 40, 200, 70, 255 }, { 255, 165, 0, 255 }, { 215, 35, 35, 255 }, { 85, 85, 85, 255 } };
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        const struct nk_user_font *f = ctx->style.font;
        const int right = s23_c422_link_right();
        nk_layout_row_begin(ctx, NK_STATIC, bh, n + 1);
        nk_layout_row_push(ctx, lw);
        nk_label(ctx, "LINK", NK_TEXT_LEFT);
        for (int cab = 0; cab < n; cab++) {                      /* cabinet 1 (left) first */
            const int i = self >= 0 ? cab : (cab == 1) == (right != 0) ? 0 : 1;   /* (two-light games: 0 = this one) */
            nk_layout_row_push(ctx, bw);
            struct nk_rect r;
            if (!nk_widget(&r, ctx)) continue;
            struct nk_color c = col[kind[i]];
            if (!lit[i]) c = nk_rgba(c.r / 3, c.g / 3, c.b / 3, 255);
            nk_fill_rect(cv, r, 3, c);
            if (self >= 0 ? cab == self : i == 0) nk_stroke_rect(cv, r, 3, 2, nk_rgb(255, 255, 255));
            char t[4]; const int len = snprintf(t, sizeof t, "%d", cab + 1);
            const float tw = f->width(f->userdata, f->height, t, len);
            nk_draw_text(cv, nk_rect(r.x + (r.w - tw) / 2, r.y + (r.h - f->height) / 2, tw + 2, f->height), t, len, f, c, nk_rgb(255, 255, 255));
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    return true;
}

void eng_ui_draw(bool *quit)
{
    if (quit_req && quit) *quit = true;
    if (!ctx) return;
    if (!open_) {                                    /* the menu-button hint (a pad has no Esc), for a few seconds after the start */
        int lw, lh; SDL_GetWindowSize(uwin, &lw, &lh);
        bool any = draw_link_lights() || (hint_left > 0 && hint_text[0]);   /* (our fork: the link lights) */
        for (int i = 0; i < nlayers; i++) if (layers[i]->draw) { layers[i]->draw(ctx, lw, lh, false); any = true; }
        if (hint_left > 0 && hint_text[0]) {
            hint_left--;
            int hw, hh; SDL_GetWindowSize(uwin, &hw, &hh);
            const float w = 440 < hw - 8 ? 440.0f : (float)hw - 8;
            if (nk_begin(ctx, "hint", nk_rect(((float)hw - w) / 2, (float)hh - 36, w, 28), NK_WINDOW_NO_SCROLLBAR)) {
                nk_layout_row_dynamic(ctx, 18, 1);
                nk_label(ctx, hint_text, NK_TEXT_CENTERED);
            }
            nk_end(ctx);
        }
        if (any) nk_sdl_render(NK_ANTI_ALIASING_ON);
        return;
    }
    int ww, wh;
    SDL_GetWindowSize(uwin, &ww, &wh);
    /* THE MENU BAR across the top of the window. The chosen page drops down under its title; on the keyboard the bar is the
     * row above the first row of the dropdown. */
    float title_w[MAXPAGES], title_x[MAXPAGES], x = 4;
    for (int t = 0; t < npages; t++) {
        title_w[t] = 30.0f + 9.0f * (float)strlen(pages[t]->name);
        if (title_w[t] < 50) title_w[t] = 50;
        title_x[t] = x; x += title_w[t] + 4;
    }
    if (nk_begin(ctx, "menubar", nk_rect(0, 0, (float)ww, 28), NK_WINDOW_NO_SCROLLBAR)) {
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 20, npages + 1);
        for (int t = 0; t < npages; t++) {
            nk_layout_row_push(ctx, title_w[t]);
            if (nk_select_label(ctx, pages[t]->name, NK_TEXT_CENTERED, t == tab) && t != tab) { tab = t; row = 0; }
        }
        nk_layout_row_push(ctx, (float)ww - x - 8 > 60 ? (float)ww - x - 8 : 60);
        if ((float)ww - x - 8 >= 190) nk_label(ctx, row < 0 ? "Down: open   Esc: close" : "Enter: choose   Esc: close", NK_TEXT_RIGHT);
        else nk_label(ctx, "", NK_TEXT_RIGHT);                    /* (no room: no hint, rather than a cut one) */
        nk_menubar_end(ctx);
    }
    nk_end(ctx);

    /* the dropdown: sized to its page, under its title, inside the window */
    ui_dump();
    const eng_ui_page *pg = pages[tab];
    const int n0 = nrows(tab);
    const float rh = pg->row_h > 0 ? pg->row_h : 26;
    const float lw = pg->label_w > 0 ? pg->label_w : 120;
    int nnotes = 0;                                                /* the page's notes, counted: the dropdown fits them all (they were cut at 2) */
    if (pg->notes && pg != &page_display && pg != &page_audio) { note_count = 0; pg->notes(count_line); nnotes = note_count; }
    else if (pg->notes && pg == &page_audio) nnotes = 2;          /* (its volume boxes: the old fixed room) */
    float dh = 48 + (n0 - (pg == &page_audio ? 1 : 0)) * (rh + 4) + (pg == &page_display ? 88 + 160 : nnotes ? 12 + 21 * nnotes : 0);
    if (dh > wh - 34) dh = (float)wh - 34;
    float dw = (pg->drop_w > 0 ? pg->drop_w : 340) < ww - 8 ? (pg->drop_w > 0 ? pg->drop_w : 340) : (float)ww - 8;
    float dx = title_x[tab];
    if (dx + dw > ww - 4) dx = ww - 4 - dw;
    if (dx < 4) dx = 4;
    char dname[16]; snprintf(dname, sizeof dname, "drop%d", tab);   /* one window per page: its own size */
    const struct nk_rect r = nk_rect(dx, 30, dw, dh);

    if (nk_begin(ctx, dname, r, NK_WINDOW_BORDER)) {
        nk_window_set_bounds(ctx, dname, r);
        if (row < 0 && kb_moved) { nk_window_set_scroll(ctx, 0, 0); kb_moved = false; }

        /* the rows: [label][<][value][>]; the selected row is highlighted */
        for (int i = 0; i < n0; i++) {
            if (pg == &page_audio && i == 4) continue;           /* the volume boxes: drawn under the rows (aud_notes) */
            char label[64], value[96];
            pg->text(i, label, sizeof label, value, sizeof value);
            const bool en = row_enabled(tab, i), val = has_value(tab, i);
            nk_layout_row_template_begin(ctx, rh);
            nk_layout_row_template_push_static(ctx, pg == &page_file ? 0 : lw);
            if (val) nk_layout_row_template_push_static(ctx, 24);
            nk_layout_row_template_push_dynamic(ctx);
            if (val) nk_layout_row_template_push_static(ctx, 24);
            nk_layout_row_template_end(ctx);
            if (!en) nk_widget_disable_begin(ctx);
            if (i == row && kb_moved) {                  /* scroll the selected row into view */
                const struct nk_rect b = nk_widget_bounds(ctx), c = nk_window_get_content_region(ctx);
                nk_uint sx = 0, sy = 0; nk_window_get_scroll(ctx, &sx, &sy);
                if (b.y < c.y) { float ny = (float)sy - (c.y - b.y) - 4; nk_window_set_scroll(ctx, sx, (nk_uint)(ny < 0 ? 0 : ny)); }
                else if (b.y + b.h > c.y + c.h) nk_window_set_scroll(ctx, sx, (nk_uint)(sy + (b.y + b.h - (c.y + c.h)) + 4));
                kb_moved = false;
            }
            if (nk_select_label(ctx, label, NK_TEXT_LEFT, i == row)) row = i;
            if (val && nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_LEFT)) { row = i; pg->change(i, -1); }
            if (is_audio(tab)) {                         /* the volume is a slider too */
                int v = aud_get(i);
                if (nk_slider_int(ctx, 0, &v, 100, 1) && v != aud_get(i)) aud_set(i, v);
            } else if (eng_nk_button_sel(ctx, value[0] ? value : label, pg == &page_file && i == row)) { row = i; pg->change(i, 0); }
            if (val && nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_RIGHT)) { row = i; pg->change(i, +1); }
            if (!en) nk_widget_disable_end(ctx);
        }
        if (pg->notes) { nk_layout_row_dynamic(ctx, 18, 1); pg->notes(note_line); }
    }
    nk_end(ctx);
    for (int i = 0; i < nlayers; i++) if (layers[i]->draw) layers[i]->draw(ctx, ww, wh, true);   /* on top of the menu */

    nk_sdl_render(NK_ANTI_ALIASING_ON);        /* scales window units to the drawable itself */
}
