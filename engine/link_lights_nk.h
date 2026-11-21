/*
 * link_lights_nk.h -- THE LINK LIGHTS drawn with Nuklear, for either menu (engine/eng_ui.c, raverace/src/rr_ui.c): include it after
 * nuklear.h. One small light per cabinet at the bottom left of the window while the menu is closed; the data is link_lights()
 * (engine/link_host.c), which also says what the colours mean.
 */
#ifndef ENG_LINK_LIGHTS_NK_H
#define ENG_LINK_LIGHTS_NK_H
#include "link_host.h"

/* true = something was drawn (the caller renders) */
static bool link_lights_nk(struct nk_context *ctx, int ww, int wh)
{
    link_light l[LINK_LIGHTS_MAX];
    const int n = link_lights(l);
    if (n <= 0 || !ctx) return false;
    const float bw = 24, bh = 20, lw = 36, w = lw + (float)n * (bw + 4) + 20, h = bh + 14;
    const int pos = link_lights_pos();
    const float x = pos == LINK_LIGHTS_BOTTOM_LEFT || pos == LINK_LIGHTS_TOP_LEFT ? 6.0f
                  : pos == LINK_LIGHTS_BOTTOM_RIGHT || pos == LINK_LIGHTS_TOP_RIGHT ? (float)ww - w - 6 : ((float)ww - w) / 2;
    const float y = pos >= LINK_LIGHTS_TOP_LEFT ? 6.0f : (float)wh - h - 6;
    if (nk_begin(ctx, "linklights", nk_rect(x, y, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
        static const struct nk_color col[] = { { 60, 130, 255, 255 }, { 40, 200, 70, 255 }, { 255, 165, 0, 255 }, { 215, 35, 35, 255 }, { 85, 85, 85, 255 } };
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        const struct nk_user_font *f = ctx->style.font;
        nk_layout_row_begin(ctx, NK_STATIC, bh, n + 1);
        nk_layout_row_push(ctx, lw);
        nk_label(ctx, "LINK", NK_TEXT_LEFT);
        for (int i = 0; i < n; i++) {
            nk_layout_row_push(ctx, bw);
            struct nk_rect r;
            if (!nk_widget(&r, ctx)) continue;
            struct nk_color c = col[l[i].kind];
            if (!l[i].lit) c = nk_rgba(c.r / 3, c.g / 3, c.b / 3, 255);            /* the flicker: dim between messages */
            nk_fill_rect(cv, r, 3, c);
            if (l[i].kind == LINK_LIGHT_SELF) nk_stroke_rect(cv, r, 3, 2, nk_rgb(255, 255, 255));
            char t[4]; const int len = snprintf(t, sizeof t, "%d", l[i].cabinet);
            const float tw = f->width(f->userdata, f->height, t, len);
            nk_draw_text(cv, nk_rect(r.x + (r.w - tw) / 2, r.y + (r.h - f->height) / 2, tw + 2, f->height), t, len, f, c, nk_rgb(255, 255, 255));
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    return true;
}
/* a hint line at the bottom centre sits above the lights when they are there */
static float link_lights_hint_lift(void) { return link_lights_pos() == LINK_LIGHTS_BOTTOM_CENTRE ? 40.0f : 0.0f; }
#endif
