/* Connection setup and the handful of X/EWMH operations eveo needs. */
#include "eveo.h"
#include <xcb/randr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

X x;

xcb_atom_t x_atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(x.conn, xcb_intern_atom(x.conn, 0, strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

xcb_render_pictformat_t x_format_for_visual(xcb_visualid_t vid)
{
    xcb_render_query_pict_formats_reply_t *r =
        xcb_render_query_pict_formats_reply(x.conn, xcb_render_query_pict_formats(x.conn), NULL);
    if (!r) return 0;
    xcb_render_pictformat_t out = 0;
    xcb_render_pictscreen_iterator_t si =
        xcb_render_query_pict_formats_screens_iterator(r);
    for (; si.rem && !out; xcb_render_pictscreen_next(&si)) {
        xcb_render_pictdepth_iterator_t di = xcb_render_pictscreen_depths_iterator(si.data);
        for (; di.rem && !out; xcb_render_pictdepth_next(&di)) {
            xcb_render_pictvisual_iterator_t vi = xcb_render_pictdepth_visuals_iterator(di.data);
            for (; vi.rem; xcb_render_pictvisual_next(&vi))
                if (vi.data->visual == vid) { out = vi.data->format; break; }
        }
    }
    free(r);
    return out;
}

bool x_init(const char *display)
{
    int n;
    x.conn = xcb_connect(display, &n);
    if (xcb_connection_has_error(x.conn)) {
        fprintf(stderr, "eveo: cannot connect to X display %s\n",
                display ? display : getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return false;
    }
    const xcb_setup_t *s = xcb_get_setup(x.conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(s);
    for (int i = 0; i < n; i++) xcb_screen_next(&it);
    x.screen = it.data;
    x.root   = x.screen->root;

    const xcb_query_extension_reply_t *e;
    e = xcb_get_extension_data(x.conn, &xcb_composite_id);
    if (!e || !e->present) { fprintf(stderr, "eveo: the X server has no Composite extension\n"); return false; }
    e = xcb_get_extension_data(x.conn, &xcb_render_id);
    if (!e || !e->present) { fprintf(stderr, "eveo: the X server has no Render extension\n"); return false; }

    /* Composite 0.2+ is needed for NameWindowPixmap. */
    xcb_composite_query_version_reply_t *cv =
        xcb_composite_query_version_reply(x.conn, xcb_composite_query_version(x.conn, 0, 4), NULL);
    bool have_nwp = cv && (cv->major_version > 0 || cv->minor_version >= 2);
    free(cv);
    if (!have_nwp) {
        fprintf(stderr, "eveo: Composite 0.2+ is required for NameWindowPixmap\n");
        return false;
    }
    xcb_render_query_version_reply_t *rv =
        xcb_render_query_version_reply(x.conn, xcb_render_query_version(x.conn, 0, 11), NULL);
    free(rv);

    x.fmt_rgb = x_format_for_visual(x.screen->root_visual);
    if (!x.fmt_rgb) { fprintf(stderr, "eveo: no Render format for the root visual\n"); return false; }

    x.gc = xcb_generate_id(x.conn);
    uint32_t gv = x.screen->black_pixel;
    xcb_create_gc(x.conn, x.gc, x.root, XCB_GC_FOREGROUND, &gv);

    x.a_net_wm_name      = x_atom("_NET_WM_NAME");
    x.a_utf8             = x_atom("UTF8_STRING");
    x.a_wm_state         = x_atom("WM_STATE");
    x.a_net_active_window= x_atom("_NET_ACTIVE_WINDOW");
    x.a_net_wm_state     = x_atom("_NET_WM_STATE");
    x.a_net_wm_state_above        = x_atom("_NET_WM_STATE_ABOVE");
    x.a_net_wm_state_skip_taskbar = x_atom("_NET_WM_STATE_SKIP_TASKBAR");
    x.a_net_wm_state_skip_pager   = x_atom("_NET_WM_STATE_SKIP_PAGER");
    x.a_net_wm_state_hidden       = x_atom("_NET_WM_STATE_HIDDEN");
    x.a_net_wm_window_type        = x_atom("_NET_WM_WINDOW_TYPE");
    x.a_net_wm_window_type_utility= x_atom("_NET_WM_WINDOW_TYPE_UTILITY");
    x.a_net_wm_desktop   = x_atom("_NET_WM_DESKTOP");
    x.a_net_wm_pid       = x_atom("_NET_WM_PID");
    x.a_motif_hints      = x_atom("_MOTIF_WM_HINTS");
    x.a_net_wm_opacity   = x_atom("_NET_WM_WINDOW_OPACITY");
    return true;
}

char *x_window_title(xcb_window_t w)
{
    /* _NET_WM_NAME (UTF-8) is authoritative; fall back to WM_NAME. */
    xcb_get_property_reply_t *r = xcb_get_property_reply(x.conn,
        xcb_get_property(x.conn, 0, w, x.a_net_wm_name, x.a_utf8, 0, 256), NULL);
    if (r && xcb_get_property_value_length(r) > 0) {
        int len = xcb_get_property_value_length(r);
        char *s = malloc(len + 1);
        memcpy(s, xcb_get_property_value(r), len);
        s[len] = 0;
        free(r);
        return s;
    }
    free(r);
    r = xcb_get_property_reply(x.conn,
        xcb_get_property(x.conn, 0, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 256), NULL);
    if (r && xcb_get_property_value_length(r) > 0) {
        int len = xcb_get_property_value_length(r);
        char *s = malloc(len + 1);
        memcpy(s, xcb_get_property_value(r), len);
        s[len] = 0;
        free(r);
        return s;
    }
    free(r);
    return NULL;
}

bool x_window_viewable(xcb_window_t w)
{
    xcb_get_window_attributes_reply_t *a =
        xcb_get_window_attributes_reply(x.conn, xcb_get_window_attributes(x.conn, w), NULL);
    if (!a) return false;
    bool v = a->map_state == XCB_MAP_STATE_VIEWABLE;
    free(a);
    return v;
}

xcb_window_t x_active_window(void)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(x.conn,
        xcb_get_property(x.conn, 0, x.root, x.a_net_active_window, XCB_ATOM_WINDOW, 0, 1), NULL);
    xcb_window_t w = XCB_WINDOW_NONE;
    if (r && xcb_get_property_value_length(r) >= 4)
        w = *(xcb_window_t *)xcb_get_property_value(r);
    free(r);
    return w;
}

void x_activate(xcb_window_t w)
{
    /* Ask the window manager to focus and un-minimise the client. Source
     * indication 2 ("pager") is what lets KWin bypass focus-stealing
     * prevention - a value of 1 would often be ignored. */
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type  = XCB_CLIENT_MESSAGE;
    ev.format         = 32;
    ev.window         = w;
    ev.type           = x.a_net_active_window;
    ev.data.data32[0] = 2;
    ev.data.data32[1] = x.last_time;
    ev.data.data32[2] = 0;
    xcb_send_event(x.conn, 0, x.root,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
        (const char *)&ev);

    /* Raising as well helps when the WM honours activation but not stacking. */
    uint32_t v = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(x.conn, w, XCB_CONFIG_WINDOW_STACK_MODE, &v);
    xcb_flush(x.conn);
}

/* ---------- output geometry ---------- */

int x_monitors(Monitor *out, int max)
{
    int n = 0;
    const xcb_query_extension_reply_t *e = xcb_get_extension_data(x.conn, &xcb_randr_id);
    if (e && e->present) {
        xcb_randr_get_monitors_reply_t *r = xcb_randr_get_monitors_reply(x.conn,
            xcb_randr_get_monitors(x.conn, x.root, 1), NULL);
        if (r) {
            xcb_randr_monitor_info_iterator_t it =
                xcb_randr_get_monitors_monitors_iterator(r);
            for (; it.rem && n < max; xcb_randr_monitor_info_next(&it)) {
                out[n].x = it.data->x;         out[n].y = it.data->y;
                out[n].w = it.data->width;     out[n].h = it.data->height;
                out[n].primary = it.data->primary;
                n++;
            }
            free(r);
        }
    }
    if (n == 0) {   /* no RandR: treat the whole screen as one monitor */
        out[0].x = out[0].y = 0;
        out[0].w = x.screen->width_in_pixels;
        out[0].h = x.screen->height_in_pixels;
        out[0].primary = true;
        n = 1;
    }
    return n;
}

void x_primary_monitor(Monitor *m)
{
    Monitor mon[16];
    int n = x_monitors(mon, 16);
    *m = mon[0];
    for (int i = 0; i < n; i++)
        if (mon[i].primary) { *m = mon[i]; return; }
}

void x_monitor_for_point(int px, int py, Monitor *m)
{
    Monitor mon[16];
    int n = x_monitors(mon, 16);
    for (int i = 0; i < n; i++)
        if (px >= mon[i].x && px < mon[i].x + mon[i].w &&
            py >= mon[i].y && py < mon[i].y + mon[i].h) { *m = mon[i]; return; }
    x_primary_monitor(m);
}

/* Nudge a thumbnail back onto a real output if it would land in the dead
 * space between differently-sized monitors, or off the edge entirely. */
void x_clamp_visible(int *px, int *py, int w, int h)
{
    Monitor mon[16];
    int n = x_monitors(mon, 16);
    int best = -1;
    long best_area = 0;
    for (int i = 0; i < n; i++) {
        int ox = *px > mon[i].x ? *px : mon[i].x;
        int oy = *py > mon[i].y ? *py : mon[i].y;
        int ox2 = (*px + w < mon[i].x + mon[i].w) ? *px + w : mon[i].x + mon[i].w;
        int oy2 = (*py + h < mon[i].y + mon[i].h) ? *py + h : mon[i].y + mon[i].h;
        long area = (long)(ox2 - ox > 0 ? ox2 - ox : 0) * (oy2 - oy > 0 ? oy2 - oy : 0);
        if (area > best_area) { best_area = area; best = i; }
    }
    /* Fully visible somewhere, or at least a quarter on screen: leave it. */
    if (best >= 0 && best_area >= (long)w * h / 4) return;

    Monitor m;
    x_primary_monitor(&m);
    if (*px < m.x)             *px = m.x + 8;
    if (*py < m.y)             *py = m.y + 8;
    if (*px + w > m.x + m.w)   *px = m.x + m.w - w - 8;
    if (*py + h > m.y + m.h)   *py = m.y + m.h - h - 8;
}
