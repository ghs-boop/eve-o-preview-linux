/* Thumbnail windows: creation, live capture and drawing.
 *
 * Each client gets one small always-on-top window. Every frame we composite
 * the client's Composite-backed pixmap into an offscreen buffer with XRender
 * (scaling happens on the GPU - the 3440x1440 source is never read back),
 * draw the border and label over it, then blit the buffer to the window.
 */
#include "eveo.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* offscreen buffer, recreated whenever a thumbnail changes size */
typedef struct { xcb_pixmap_t pix; xcb_render_picture_t pic; int w, h; } Backing;
static Backing backing[MAX_CLIENTS];
static int     backing_slot(Client *c);

static Client *clients_base;      /* set by main so we can index slots */
void thumbs_bind(Client *base) { clients_base = base; }
static int backing_slot(Client *c) { return (int)(c - clients_base); }

static void set_fg(uint32_t rgb)
{
    xcb_change_gc(x.conn, x.gc, XCB_GC_FOREGROUND, &rgb);
}

static void fill_rect(xcb_drawable_t d, int X, int Y, int W, int H, uint32_t rgb)
{
    if (W <= 0 || H <= 0) return;
    set_fg(rgb);
    xcb_rectangle_t r = { (int16_t)X, (int16_t)Y, (uint16_t)W, (uint16_t)H };
    xcb_poly_fill_rectangle(x.conn, d, x.gc, 1, &r);
}

/* Draw a string using the embedded bitmap font. */
static void draw_text(xcb_drawable_t d, int X, int Y, const char *s,
                      uint32_t rgb, int scale)
{
    enum { MAX_R = 2048 };
    static xcb_rectangle_t rects[MAX_R];
    int n = 0, cx = X;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < MAX_R; p++) {
        int ch = *p;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const uint8_t *g = font_glyphs[ch - FONT_FIRST];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = g[row];
            if (!bits) continue;
            for (int col = 0; col < FONT_W && n < MAX_R; col++) {
                if (!(bits & (1u << col))) continue;
                /* merge horizontally adjacent pixels into one rectangle */
                int run = 1;
                while (col + run < FONT_W && (bits & (1u << (col + run)))) run++;
                rects[n].x      = (int16_t)(cx + col * scale);
                rects[n].y      = (int16_t)(Y + row * scale);
                rects[n].width  = (uint16_t)(run * scale);
                rects[n].height = (uint16_t)scale;
                n++;
                col += run - 1;
            }
        }
        cx += (FONT_W + 1) * scale;
    }
    if (n) { set_fg(rgb); xcb_poly_fill_rectangle(x.conn, d, x.gc, n, rects); }
}

static int text_width(const char *s, int scale)
{
    return (int)strlen(s) * (FONT_W + 1) * scale;
}

/* ---------- source capture ---------- */

void thumb_release_source(Client *c)
{
    if (c->src_pic) { xcb_render_free_picture(x.conn, c->src_pic); c->src_pic = 0; }
    if (c->src_pix) { xcb_free_pixmap(x.conn, c->src_pix);        c->src_pix = 0; }
    c->pix_w = c->pix_h = 0;
}

bool thumb_acquire_source(Client *c)
{
    /* The named pixmap stays live for the window's contents, so it only has
     * to be re-taken when the client is resized or remapped. */
    if (c->src_pix && c->pix_w == c->sw && c->pix_h == c->sh) return true;
    thumb_release_source(c);
    if (!c->viewable) return false;

    xcb_pixmap_t pix = xcb_generate_id(x.conn);
    xcb_generic_error_t *e = xcb_request_check(x.conn,
        xcb_composite_name_window_pixmap_checked(x.conn, c->win, pix));
    if (e) { free(e); return false; }   /* BadMatch: window is unmapped */

    xcb_get_window_attributes_reply_t *at =
        xcb_get_window_attributes_reply(x.conn, xcb_get_window_attributes(x.conn, c->win), NULL);
    xcb_render_pictformat_t fmt = at ? x_format_for_visual(at->visual) : x.fmt_rgb;
    free(at);
    if (!fmt) fmt = x.fmt_rgb;

    xcb_render_picture_t pic = xcb_generate_id(x.conn);
    uint32_t v = XCB_RENDER_REPEAT_PAD;
    xcb_render_create_picture(x.conn, pic, pix, fmt, XCB_RENDER_CP_REPEAT, &v);
    /* "good" downsamples far more cleanly than the default nearest filter */
    xcb_render_set_picture_filter(x.conn, pic, 4, "good", 0, NULL);

    c->src_pix = pix; c->src_pic = pic;
    c->pix_w = c->sw; c->pix_h = c->sh;
    return true;
}

/* ---------- thumbnail window ---------- */

static void set_window_props(Client *c)
{
    char title[TITLE_LEN];
    snprintf(title, sizeof title, "eveo: %s", c->name);
    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb, XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING, 8, strlen(title), title);
    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb, x.a_net_wm_name,
        x.a_utf8, 8, strlen(title), title);
    static const char cls[] = "eveo\0Eveo";
    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, sizeof cls, cls);

    if (!cfg.override_redirect) {
        xcb_atom_t type = x.a_net_wm_window_type_utility;
        xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb,
            x.a_net_wm_window_type, XCB_ATOM_ATOM, 32, 1, &type);

        xcb_atom_t st[3] = { x.a_net_wm_state_above,
                             x.a_net_wm_state_skip_taskbar,
                             x.a_net_wm_state_skip_pager };
        xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb,
            x.a_net_wm_state, XCB_ATOM_ATOM, 32, 3, st);

        uint32_t all_desktops = 0xffffffff;
        xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb,
            x.a_net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &all_desktops);

        /* Motif hints: no decorations. */
        uint32_t mwm[5] = { 2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0 };
        xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb,
            x.a_motif_hints, x.a_motif_hints, 32, 5, mwm);

        /* WM_HINTS with input=False: the thumbnail must never take keyboard
         * focus, or clicking it would pull focus away from the game. */
        uint32_t hints[9] = { 1 /* InputHint */, 0 /* input = False */,
                              1 /* NormalState */, 0,0,0,0,0,0 };
        xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb,
            XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, hints);
    }

    if (cfg.opacity < 1.0) {
        uint32_t o = (uint32_t)(cfg.opacity * 0xffffffffu);
        xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, c->thumb,
            x.a_net_wm_opacity, XCB_ATOM_CARDINAL, 32, 1, &o);
    }
}

static void backing_ensure(Client *c)
{
    Backing *b = &backing[backing_slot(c)];
    if (b->pix && b->w == c->tw && b->h == c->th) return;
    if (b->pic) xcb_render_free_picture(x.conn, b->pic);
    if (b->pix) xcb_free_pixmap(x.conn, b->pix);
    b->pix = xcb_generate_id(x.conn);
    xcb_create_pixmap(x.conn, x.screen->root_depth, b->pix, x.root, c->tw, c->th);
    b->pic = xcb_generate_id(x.conn);
    xcb_render_create_picture(x.conn, b->pic, b->pix, x.fmt_rgb, 0, NULL);
    b->w = c->tw; b->h = c->th;
}

void thumb_create(Client *c)
{
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        cfg.color_bg,
        cfg.override_redirect ? 1u : 0u,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE
    };
    c->thumb = xcb_generate_id(x.conn);
    xcb_create_window(x.conn, x.screen->root_depth, c->thumb, x.root,
        c->tx, c->ty, c->tw, c->th, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        x.screen->root_visual, mask, vals);

    set_window_props(c);

    c->thumb_pic = xcb_generate_id(x.conn);
    xcb_render_create_picture(x.conn, c->thumb_pic, c->thumb, x.fmt_rgb, 0, NULL);

    backing_ensure(c);
    xcb_map_window(x.conn, c->thumb);
    c->mapped = true;

    /* Managed windows need the position re-applied after the WM maps them. */
    if (!cfg.override_redirect) {
        uint32_t cv[2] = { (uint32_t)c->tx, (uint32_t)c->ty };
        xcb_configure_window(x.conn, c->thumb,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, cv);
    }
    xcb_flush(x.conn);
}

void thumb_destroy(Client *c)
{
    Backing *b = &backing[backing_slot(c)];
    thumb_release_source(c);
    if (b->pic) { xcb_render_free_picture(x.conn, b->pic); b->pic = 0; }
    if (b->pix) { xcb_free_pixmap(x.conn, b->pix);         b->pix = 0; }
    b->w = b->h = 0;
    if (c->thumb_pic) { xcb_render_free_picture(x.conn, c->thumb_pic); c->thumb_pic = 0; }
    if (c->thumb)     { xcb_destroy_window(x.conn, c->thumb);          c->thumb = 0; }
    c->mapped = false;
}

void thumb_move_resize(Client *c, int px, int py, int pw, int ph)
{
    bool resized = (pw != c->tw || ph != c->th);
    c->tx = px; c->ty = py; c->tw = pw; c->th = ph;
    uint32_t v[4] = { (uint32_t)px, (uint32_t)py, (uint32_t)pw, (uint32_t)ph };
    xcb_configure_window(x.conn, c->thumb,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    if (resized) backing_ensure(c);
}

void thumb_render(Client *c)
{
    if (!c->thumb) return;
    backing_ensure(c);
    Backing *b = &backing[backing_slot(c)];
    const int bw = cfg.border;
    const int iw = c->tw - 2 * bw, ih = c->th - 2 * bw;   /* image area */

    fill_rect(b->pix, 0, 0, c->tw, c->th, cfg.color_bg);

    bool drew = false;
    if (thumb_acquire_source(c) && iw > 0 && ih > 0 && c->sw > 0 && c->sh > 0) {
        /* letterbox: uniform scale, centred, so the view is never distorted */
        double s = (double)iw / c->sw;
        double sy = (double)ih / c->sh;
        if (sy < s) s = sy;
        int dw = (int)(c->sw * s), dh = (int)(c->sh * s);
        if (dw > 0 && dh > 0) {
            xcb_render_transform_t t = {0};
            t.matrix11 = (xcb_render_fixed_t)((double)c->sw / dw * 65536.0);
            t.matrix22 = (xcb_render_fixed_t)((double)c->sh / dh * 65536.0);
            t.matrix33 = 65536;
            xcb_render_set_picture_transform(x.conn, c->src_pic, t);
            xcb_render_composite(x.conn, XCB_RENDER_PICT_OP_SRC,
                c->src_pic, XCB_NONE, b->pic,
                0, 0, 0, 0,
                bw + (iw - dw) / 2, bw + (ih - dh) / 2, dw, dh);
            drew = true;
        }
    }

    /* border */
    uint32_t col = c->active ? cfg.color_active : cfg.color_inactive;
    if (bw > 0) {
        fill_rect(b->pix, 0, 0, c->tw, bw, col);
        fill_rect(b->pix, 0, c->th - bw, c->tw, bw, col);
        fill_rect(b->pix, 0, 0, bw, c->th, col);
        fill_rect(b->pix, c->tw - bw, 0, bw, c->th, col);
    }

    if (cfg.show_label && c->name[0]) {
        int sc = cfg.label_scale;
        int tw = text_width(c->name, sc), th = FONT_H * sc;
        int lx = bw + 4, ly = c->th - bw - th - 4;
        fill_rect(b->pix, lx - 3, ly - 2, tw + 6, th + 4, 0x000000);
        draw_text(b->pix, lx, ly, c->name, drew ? 0xd8e4f0 : 0x8892a0, sc);
    }
    if (!drew) {
        const char *msg = c->viewable ? "no image" : "minimised";
        int sc = cfg.label_scale;
        draw_text(b->pix, (c->tw - text_width(msg, sc)) / 2,
                  (c->th - FONT_H * sc) / 2 - 8, msg, 0x707a86, sc);
    }

    xcb_copy_area(x.conn, b->pix, c->thumb, x.gc, 0, 0, 0, 0, c->tw, c->th);
}
