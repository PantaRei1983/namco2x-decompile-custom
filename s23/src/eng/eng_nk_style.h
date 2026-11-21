/* eng_nk_style.h -- the look every Nuklear menu in the tree shares (the engine's eng_ui.c, Rave Racer's rr_ui.c, Prop Cycle's ui_menu.c).
 * Include AFTER nuklear.h. Header-only because each of those menus is its own Nuklear implementation. */
#ifndef ENG_NK_STYLE_H
#define ENG_NK_STYLE_H
#include <stdbool.h>
/* THE SELECTED ROW / TAB (GitHub #43): Nuklear's default marks the selected item with a grey a few shades off the window's, hard to see
 * (worst with a dark desktop theme). Amber with black text; a hovered row is lifted a little. */
static inline void eng_nk_style_highlight(struct nk_context *c)
{
    struct nk_style_selectable *s = &c->style.selectable;
    s->normal_active  = nk_style_item_color(nk_rgb(240, 170, 40));
    s->hover_active   = nk_style_item_color(nk_rgb(255, 196, 80));
    s->pressed_active = nk_style_item_color(nk_rgb(255, 196, 80));
    s->text_normal_active = s->text_hover_active = s->text_pressed_active = nk_rgb(0, 0, 0);
    s->hover = nk_style_item_color(nk_rgb(70, 70, 78));
}
/* A BUTTON THAT IS THE SELECTED ROW: the File page's rows are buttons only (no label column), so the keyboard's place was invisible --
 * "the arrow keys do nothing there". Drawn in the same amber as a selected row. Our fork. */
static inline nk_bool eng_nk_button_sel(struct nk_context *c, const char *text, bool sel)
{
    if (!sel) return nk_button_label(c, text);
    struct nk_style_button b = c->style.button;
    b.normal = nk_style_item_color(nk_rgb(240, 170, 40));
    b.hover = b.active = nk_style_item_color(nk_rgb(255, 196, 80));
    b.text_normal = b.text_hover = b.text_active = nk_rgb(0, 0, 0);
    return nk_button_label_styled(c, &b, text);
}
/* THE VOLUME BOXES (Mute / 25% / 50% / 100%) on the keyboard too: they are the Audio page's last row -- Down onto them, Left / Right picks
 * a box (amber), Enter sets it. Our fork. eng_vol_box_key(dir): the box the keys are on moved by dir; returns the volume to set (Enter),
 * or -1. eng_vol_boxes(c, on_row, set): draws the four, the keys' box amber while on_row; a click sets the volume through set(). */
static const int eng_vol_box_val[4] = { 0, 25, 50, 100 };
static const char *const eng_vol_box_name[4] = { "Mute", "25%", "50%", "100%" };
static int eng_vol_box_at = 3;
static bool eng_vol_box_fresh = true;                          /* the keys are not on the boxes yet: start from the one that is set */
static inline int eng_vol_box_key(int dir, int volume)
{
    if (eng_vol_box_fresh) { eng_vol_box_fresh = false; for (int k = 0; k < 4; k++) if (eng_vol_box_val[k] == volume) eng_vol_box_at = k; }
    if (dir == 0) return eng_vol_box_val[eng_vol_box_at];
    eng_vol_box_at += dir;
    if (eng_vol_box_at < 0) eng_vol_box_at = 0;
    if (eng_vol_box_at > 3) eng_vol_box_at = 3;
    return -1;
}
static inline void eng_vol_boxes(struct nk_context *c, bool on_row, int volume, void (*set)(int))
{
    if (!on_row) { eng_vol_box_fresh = true; for (int k = 0; k < 4; k++) if (eng_vol_box_val[k] == volume) eng_vol_box_at = k; }   /* start from the box that is set */
    nk_layout_row_dynamic(c, 24, 4);
    for (int k = 0; k < 4; k++)
        if (eng_nk_button_sel(c, eng_vol_box_name[k], on_row && k == eng_vol_box_at)) { eng_vol_box_at = k; set(eng_vol_box_val[k]); }
}
#endif
