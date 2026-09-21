/* eveo - live previews for multiboxed EVE Online clients.
 *
 * Run with no arguments to start the previewer. Run with a command
 * (next, prev, activate <name>, reload, quit) to control a running instance;
 * bind those to global shortcuts in System Settings for hotkey switching,
 * which is the only approach that works reliably on a Wayland session.
 */
#include "eveo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

Config cfg;
void thumbs_bind(Client *base);

static Client clients[MAX_CLIENTS];
static int    n_clients;
static volatile sig_atomic_t running = 1;
static int    sock = -1;
static bool   need_scan = true;

static void on_signal(int s) { (void)s; running = 0; }

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* ---------- client discovery ---------- */

static bool has_wm_state(xcb_window_t w)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(x.conn,
        xcb_get_property(x.conn, 0, w, x.a_wm_state, XCB_ATOM_ANY, 0, 2), NULL);
    bool ok = r && r->type != XCB_ATOM_NONE;
    free(r);
    return ok;
}

static bool ignored(const char *title)
{
    for (int i = 0; i < cfg.n_ignore; i++)
        if (strstr(title, cfg.ignore[i])) return true;
    return false;
}

static Client *find_by_win(xcb_window_t w)
{
    for (int i = 0; i < n_clients; i++)
        if (clients[i].win == w) return &clients[i];
    return NULL;
}

static Client *find_by_thumb(xcb_window_t w)
{
    for (int i = 0; i < n_clients; i++)
        if (clients[i].thumb == w) return &clients[i];
    return NULL;
}

/* Which monitor is this client being played on? */
static void client_monitor(Client *c, Monitor *m)
{
    xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(x.conn,
        xcb_translate_coordinates(x.conn, c->win, x.root, 0, 0), NULL);
    int cx = 0, cy = 0;
    if (t) { cx = t->dst_x + c->sw / 2; cy = t->dst_y + c->sh / 2; free(t); }
    x_monitor_for_point(cx, cy, m);
}

/* Default placement for a client we have never seen before: tile the
 * thumbnails down the left edge of the monitor the client is on, wrapping
 * into a new column at the bottom. Placing them relative to a real output
 * matters - the X screen is the bounding box of every monitor, so its
 * top-left corner is often dead space that is not displayed anywhere. */
static void default_position(Client *c, int index)
{
    const int gap = 8;
    Monitor m;
    client_monitor(c, &m);
    c->tw = cfg.thumb_w;
    c->th = cfg.thumb_h;

    int per_col = (m.h - gap) / (c->th + gap);
    if (per_col < 1) per_col = 1;
    int col = index / per_col, row = index % per_col;
    c->tx = m.x + gap + col * (c->tw + gap);
    c->ty = m.y + gap + row * (c->th + gap);
}

static void update_geometry(Client *c)
{
    xcb_get_geometry_reply_t *g =
        xcb_get_geometry_reply(x.conn, xcb_get_geometry(x.conn, c->win), NULL);
    if (!g) return;
    if (g->width != c->sw || g->height != c->sh) {
        c->sw = g->width; c->sh = g->height;
        thumb_release_source(c);        /* pixmap is stale after a resize */
    }
    free(g);
}

static void set_name_from_title(Client *c, const char *title)
{
    snprintf(c->title, TITLE_LEN, "%s", title);
    size_t pl = strlen(cfg.match_prefix);
    snprintf(c->name, NAME_LEN, "%s",
             strlen(title) > pl ? title + pl : title);
}

static void client_add(xcb_window_t w, const char *title)
{
    if (n_clients >= MAX_CLIENTS) return;
    Client *c = &clients[n_clients];
    memset(c, 0, sizeof *c);
    c->win = w;
    set_name_from_title(c, title);
    c->present  = true;
    c->viewable = x_window_viewable(w);
    update_geometry(c);

    if (!pos_get(c->name, &c->tx, &c->ty, &c->tw, &c->th))
        default_position(c, n_clients);
    if (c->tw < 40) c->tw = cfg.thumb_w;
    if (c->th < 30) c->th = cfg.thumb_h;
    x_clamp_visible(&c->tx, &c->ty, c->tw, c->th);

    /* watch for resize, unmap, destroy and title changes */
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(x.conn, w, XCB_CW_EVENT_MASK, &mask);

    n_clients++;
    thumb_create(c);
    printf("eveo: tracking \"%s\" (0x%x, %dx%d)\n", c->name, w, c->sw, c->sh);
}

static void client_remove(int i)
{
    printf("eveo: \"%s\" closed\n", clients[i].name);
    thumb_destroy(&clients[i]);
    memmove(&clients[i], &clients[i+1], (n_clients - i - 1) * sizeof(Client));
    n_clients--;
    memset(&clients[n_clients], 0, sizeof(Client));
}

static void consider(xcb_window_t w)
{
    char *t = x_window_title(w);
    if (!t) return;
    if (strncmp(t, cfg.match_prefix, strlen(cfg.match_prefix)) || ignored(t)) {
        free(t);
        return;
    }
    Client *c = find_by_win(w);
    if (c) {
        c->present = true;
        if (strcmp(c->title, t)) {          /* character changed */
            char old[NAME_LEN];
            snprintf(old, sizeof old, "%s", c->name);
            set_name_from_title(c, t);
            printf("eveo: \"%s\" is now \"%s\"\n", old, c->name);
            int X, Y, W, H;
            if (pos_get(c->name, &X, &Y, &W, &H)) thumb_move_resize(c, X, Y, W, H);
        }
    } else {
        client_add(w, t);
    }
    free(t);
}

static void scan_tree(xcb_window_t w, int depth)
{
    if (depth > 4) return;
    xcb_query_tree_reply_t *t =
        xcb_query_tree_reply(x.conn, xcb_query_tree(x.conn, w), NULL);
    if (!t) return;
    xcb_window_t *kids = xcb_query_tree_children(t);
    int n = xcb_query_tree_children_length(t);
    for (int i = 0; i < n; i++) {
        if (find_by_thumb(kids[i])) continue;       /* never track ourselves */
        if (has_wm_state(kids[i])) consider(kids[i]);
        else scan_tree(kids[i], depth + 1);
    }
    free(t);
}

static void scan(void)
{
    for (int i = 0; i < n_clients; i++) clients[i].present = false;
    scan_tree(x.root, 0);
    for (int i = n_clients - 1; i >= 0; i--)
        if (!clients[i].present) client_remove(i);
    need_scan = false;
}

/* ---------- activation / cycling ---------- */

static void refresh_active(void)
{
    xcb_window_t a = x_active_window();
    for (int i = 0; i < n_clients; i++)
        clients[i].active = (clients[i].win == a);
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((*(Client *const *)a)->name, (*(Client *const *)b)->name);
}

static void cycle(int dir)
{
    if (n_clients == 0) return;
    Client *order[MAX_CLIENTS];
    for (int i = 0; i < n_clients; i++) order[i] = &clients[i];
    qsort(order, n_clients, sizeof *order, cmp_name);

    int cur = -1;
    for (int i = 0; i < n_clients; i++) if (order[i]->active) { cur = i; break; }
    int next = (cur < 0) ? 0 : (cur + dir + n_clients) % n_clients;
    printf("eveo: switching to \"%s\"\n", order[next]->name);
    x_activate(order[next]->win);
}

static void activate_named(const char *needle)
{
    for (int i = 0; i < n_clients; i++)
        if (strcasestr(clients[i].name, needle)) {
            printf("eveo: switching to \"%s\"\n", clients[i].name);
            x_activate(clients[i].win);
            return;
        }
    fprintf(stderr, "eveo: no client matching \"%s\"\n", needle);
}

/* ---------- control socket ---------- */

static char *sock_path(void)
{
    static char p[256];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(p, sizeof p, "%s/eveo.sock", rt && *rt ? rt : "/tmp");
    return p;
}

static bool sock_listen(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path());
    unlink(a.sun_path);
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) return false;
    if (bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "eveo: cannot bind %s: %s\n", a.sun_path, strerror(errno));
        close(sock); sock = -1;
        return false;
    }
    return true;
}

static int sock_send(int argc, char **argv)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path());
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) return 1;
    char msg[512] = {0};
    for (int i = 0; i < argc; i++) {
        if (i) strncat(msg, " ", sizeof msg - strlen(msg) - 1);
        strncat(msg, argv[i], sizeof msg - strlen(msg) - 1);
    }
    if (sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "eveo: no running instance (%s)\n", strerror(errno));
        close(s);
        return 1;
    }
    close(s);
    return 0;
}

static void handle_command(char *msg)
{
    char *cmd = strtok(msg, " \t\n");
    if (!cmd) return;
    if      (!strcmp(cmd, "next"))   cycle(+1);
    else if (!strcmp(cmd, "prev"))   cycle(-1);
    else if (!strcmp(cmd, "quit"))   running = 0;
    else if (!strcmp(cmd, "reload")) {
        char *p = config_path("eveo.conf");
        config_defaults(&cfg);
        config_load(&cfg, p);
        free(p);
        printf("eveo: configuration reloaded\n");
    }
    else if (!strcmp(cmd, "activate")) {
        char *rest = strtok(NULL, "\n");
        if (rest) activate_named(rest);
    }
    else fprintf(stderr, "eveo: unknown command \"%s\"\n", cmd);
}

/* ---------- events ---------- */

static void on_button_press(xcb_button_press_event_t *e)
{
    Client *c = find_by_thumb(e->event);
    if (!c) return;
    x.last_time = e->time;

    if (e->detail == 4 || e->detail == 5) {          /* wheel: resize */
        double f = (e->detail == 4) ? 1.1 : 1.0 / 1.1;
        int nw = (int)(c->tw * f), nh = (int)(c->th * f);
        if (nw < 80) { nw = 80; nh = c->th * 80 / c->tw; }
        if (nw > 1600) return;
        thumb_move_resize(c, c->tx, c->ty, nw, nh);
        pos_set(c->name, c->tx, c->ty, c->tw, c->th);
        pos_save();
        return;
    }
    if (e->detail != 1) return;
    c->dragging   = true;
    c->drag_moved = false;
    c->drag_dx    = e->root_x - c->tx;
    c->drag_dy    = e->root_y - c->ty;
}

static void on_motion(xcb_motion_notify_event_t *e)
{
    Client *c = find_by_thumb(e->event);
    if (!c || !c->dragging) return;
    x.last_time = e->time;
    int nx = e->root_x - c->drag_dx, ny = e->root_y - c->drag_dy;
    if (cfg.snap > 0) {
        nx = (nx + cfg.snap / 2) / cfg.snap * cfg.snap;
        ny = (ny + cfg.snap / 2) / cfg.snap * cfg.snap;
    }
    if (abs(nx - c->tx) > 2 || abs(ny - c->ty) > 2) c->drag_moved = true;
    thumb_move_resize(c, nx, ny, c->tw, c->th);
}

static void on_button_release(xcb_button_release_event_t *e)
{
    Client *c = find_by_thumb(e->event);
    if (!c || e->detail != 1) return;
    x.last_time = e->time;
    c->dragging = false;
    if (c->drag_moved) {
        int nx = c->tx, ny = c->ty;
        x_clamp_visible(&nx, &ny, c->tw, c->th);
        if (nx != c->tx || ny != c->ty) thumb_move_resize(c, nx, ny, c->tw, c->th);
        pos_set(c->name, c->tx, c->ty, c->tw, c->th);
        pos_save();
    } else if (cfg.click_activates) {
        x_activate(c->win);
    }
}

static void handle_event(xcb_generic_event_t *ev)
{
    switch (ev->response_type & 0x7f) {
    case XCB_BUTTON_PRESS:   on_button_press((xcb_button_press_event_t *)ev);   break;
    case XCB_MOTION_NOTIFY:  on_motion((xcb_motion_notify_event_t *)ev);        break;
    case XCB_BUTTON_RELEASE: on_button_release((xcb_button_release_event_t *)ev);break;
    case XCB_EXPOSE: {
        Client *c = find_by_thumb(((xcb_expose_event_t *)ev)->window);
        if (c) thumb_render(c);
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;
        Client *c = find_by_win(e->window);
        if (c && (e->width != c->sw || e->height != c->sh)) {
            c->sw = e->width; c->sh = e->height;
            thumb_release_source(c);
        }
        break;
    }
    case XCB_MAP_NOTIFY: {
        Client *c = find_by_win(((xcb_map_notify_event_t *)ev)->window);
        if (c) { c->viewable = true; thumb_release_source(c); }
        else need_scan = true;
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        Client *c = find_by_win(((xcb_unmap_notify_event_t *)ev)->window);
        if (c) { c->viewable = false; thumb_release_source(c); }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *)ev;
        for (int i = 0; i < n_clients; i++)
            if (clients[i].win == e->window) { client_remove(i); break; }
        break;
    }
    case XCB_CREATE_NOTIFY:
        need_scan = true;
        break;
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)ev;
        x.last_time = e->time;
        if (e->window == x.root && e->atom == x.a_net_active_window) refresh_active();
        else if (e->atom == x.a_net_wm_name || e->atom == XCB_ATOM_WM_NAME ||
                 e->atom == x.a_wm_state) need_scan = true;
        break;
    }
    default: break;
    }
}

/* ---------- main ---------- */

static void usage(void)
{
    puts("eveo - live previews for multiboxed EVE Online clients\n"
         "\n"
         "  eveo                 start the previewer\n"
         "  eveo next | prev     switch to the next/previous client\n"
         "  eveo activate NAME   switch to the client matching NAME\n"
         "  eveo reload          re-read the configuration\n"
         "  eveo quit            stop a running instance\n"
         "  eveo --list          list detected clients and exit\n"
         "\n"
         "Bind 'eveo next' / 'eveo prev' to global shortcuts in System Settings\n"
         "for hotkey switching. Configuration lives in\n"
         "  ~/.config/eve-o-preview/eveo.conf");
}

int main(int argc, char **argv)
{
    bool list_only = false;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 1) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 0; }
        if (!strcmp(argv[1], "--list")) list_only = true;
        else return sock_send(argc - 1, argv + 1);
    }

    config_defaults(&cfg);
    char *cpath = config_path("eveo.conf");
    if (!config_load(&cfg, cpath))
        fprintf(stderr, "eveo: no %s, using defaults\n", cpath);
    free(cpath);
    pos_load();

    if (!x_init(NULL)) return 1;
    thumbs_bind(clients);

    uint32_t rmask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(x.conn, x.root, XCB_CW_EVENT_MASK, &rmask);

    if (list_only) {
        scan();
        refresh_active();
        if (!n_clients) puts("eveo: no EVE clients found");
        for (int i = 0; i < n_clients; i++)
            printf("  %-28s 0x%08x  %dx%d  %s%s\n", clients[i].name, clients[i].win,
                   clients[i].sw, clients[i].sh,
                   clients[i].viewable ? "visible" : "MINIMISED",
                   clients[i].active ? "  (active)" : "");
        return 0;
    }

    if (!sock_listen())
        fprintf(stderr, "eveo: control socket unavailable, hotkeys will not work\n");

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);

    scan();
    refresh_active();
    printf("eveo: watching for clients titled \"%s*\" (%d found)\n",
           cfg.match_prefix, n_clients);

    long last_scan = now_ms(), last_frame = 0;
    while (running) {
        struct pollfd pfd[2];
        int nfd = 0;
        pfd[nfd].fd = xcb_get_file_descriptor(x.conn); pfd[nfd].events = POLLIN; nfd++;
        if (sock >= 0) { pfd[nfd].fd = sock; pfd[nfd].events = POLLIN; nfd++; }

        long wait = cfg.refresh_ms - (now_ms() - last_frame);
        if (wait < 0) wait = 0;
        poll(pfd, nfd, (int)wait);

        if (xcb_connection_has_error(x.conn)) {
            fprintf(stderr, "eveo: lost the X connection\n");
            break;
        }

        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(x.conn))) { handle_event(ev); free(ev); }

        if (sock >= 0 && (pfd[nfd-1].revents & POLLIN)) {
            char buf[512];
            ssize_t n = recv(sock, buf, sizeof buf - 1, 0);
            if (n > 0) { buf[n] = 0; handle_command(buf); }
        }

        long t = now_ms();
        if (need_scan || t - last_scan > 1000) {
            scan();
            refresh_active();
            last_scan = t;
        }
        if (t - last_frame >= cfg.refresh_ms) {
            for (int i = 0; i < n_clients; i++) {
                Client *c = &clients[i];
                bool want = !(cfg.hide_active && c->active);
                if (want && !c->mapped)      { xcb_map_window(x.conn, c->thumb);   c->mapped = true; }
                else if (!want && c->mapped) { xcb_unmap_window(x.conn, c->thumb); c->mapped = false; }
                if (c->mapped) thumb_render(c);
            }
            xcb_flush(x.conn);
            last_frame = t;
        }
    }

    printf("\neveo: shutting down\n");
    pos_save();
    for (int i = n_clients - 1; i >= 0; i--) thumb_destroy(&clients[i]);
    xcb_flush(x.conn);
    if (sock >= 0) { close(sock); unlink(sock_path()); }
    xcb_disconnect(x.conn);
    return 0;
}
