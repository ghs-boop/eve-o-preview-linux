/* eveo - live thumbnail previews for multiboxed EVE Online clients on Linux.
 *
 * Targets X11 / XWayland (EVE runs under Wine/Proton, so its windows are
 * always XWayland windows even on a Wayland session). Capture uses the
 * Composite extension; scaling is done entirely server-side with XRender so
 * no pixel data ever crosses the socket.
 */
#ifndef EVEO_H
#define EVEO_H

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_CLIENTS   32
#define MAX_IGNORE    16
#define NAME_LEN      192
#define TITLE_LEN     256

/* ---------- configuration ---------- */

typedef struct {
    int      thumb_w, thumb_h;      /* default thumbnail size          */
    int      refresh_ms;            /* frame interval                  */
    int      border;                /* border thickness in px          */
    uint32_t color_active;          /* border colour, focused client   */
    uint32_t color_inactive;
    uint32_t color_bg;              /* shown when a client has no image*/
    bool     hide_active;           /* suppress the focused client's own thumb */
    bool     show_label;
    int      label_scale;
    double   opacity;               /* 0.0 - 1.0                       */
    bool     override_redirect;     /* unmanaged windows vs managed     */
    int      snap;                  /* snap-to-grid when dragging, 0=off*/
    bool     click_activates;
    char     match_prefix[64];      /* window title prefix to match     */
    char     ignore[MAX_IGNORE][64];
    int      n_ignore;
} Config;

void config_defaults(Config *c);
bool config_load(Config *c, const char *path);
const char *config_dir(void);
char *config_path(const char *leaf);   /* caller frees */

/* ---------- saved thumbnail positions ---------- */

typedef struct {
    char name[NAME_LEN];
    int  x, y, w, h;
    bool valid;
} SavedPos;

void  pos_load(void);
bool  pos_get(const char *name, int *x, int *y, int *w, int *h);
void  pos_set(const char *name, int x, int y, int w, int h);
void  pos_save(void);

/* ---------- tracked EVE client ---------- */

typedef struct {
    xcb_window_t win;                 /* the EVE client window          */
    char  title[TITLE_LEN];
    char  name[NAME_LEN];             /* character name, title minus prefix */
    bool  present;                    /* seen in the latest scan        */
    bool  viewable;                   /* mapped & rendering             */
    bool  active;                     /* currently focused              */
    int   sw, sh;                     /* source window size             */

    xcb_pixmap_t        src_pix;      /* Composite name-window-pixmap   */
    xcb_render_picture_t src_pic;
    int   pix_w, pix_h;               /* size the pixmap was taken at   */

    xcb_window_t        thumb;        /* our preview window             */
    xcb_render_picture_t thumb_pic;
    int   tx, ty, tw, th;
    bool  mapped;                     /* thumb currently mapped         */

    bool  dragging;
    int   drag_dx, drag_dy;
    bool  drag_moved;
} Client;

/* ---------- shared X state ---------- */

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t     *screen;
    xcb_window_t      root;
    xcb_render_pictformat_t fmt_rgb;   /* format of the root visual     */
    xcb_gcontext_t    gc;
    xcb_timestamp_t   last_time;

    /* atoms */
    xcb_atom_t a_net_wm_name, a_utf8, a_wm_state, a_net_active_window;
    xcb_atom_t a_net_wm_state, a_net_wm_state_above, a_net_wm_state_skip_taskbar;
    xcb_atom_t a_net_wm_state_skip_pager, a_net_wm_window_type;
    xcb_atom_t a_net_wm_window_type_utility, a_net_wm_desktop, a_net_wm_pid;
    xcb_atom_t a_motif_hints, a_net_wm_opacity, a_net_wm_state_hidden;
} X;

extern X   x;
extern Config cfg;

/* ---------- helpers ---------- */

xcb_atom_t  x_atom(const char *name);
bool        x_init(const char *display);
xcb_render_pictformat_t x_format_for_visual(xcb_visualid_t vid);
char       *x_window_title(xcb_window_t w);            /* caller frees */
bool        x_window_viewable(xcb_window_t w);
xcb_window_t x_active_window(void);
void        x_activate(xcb_window_t w);

/* Physical output geometry. On a multi-monitor desktop the X screen is the
 * bounding box of all outputs, so parts of it are not displayed anywhere;
 * thumbnails must be placed inside a real monitor. */
typedef struct { int x, y, w, h; bool primary; } Monitor;
int  x_monitors(Monitor *out, int max);
void x_monitor_for_point(int px, int py, Monitor *m);
void x_primary_monitor(Monitor *m);
void x_clamp_visible(int *px, int *py, int w, int h);

/* thumbs.c */
void thumb_create(Client *c);
void thumb_destroy(Client *c);
void thumb_move_resize(Client *c, int px, int py, int pw, int ph);
void thumb_render(Client *c);
void thumb_release_source(Client *c);
bool thumb_acquire_source(Client *c);

#endif
