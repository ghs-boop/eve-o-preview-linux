/* Configuration and persisted thumbnail positions.
 *
 * Two files live in ~/.config/eve-o-preview:
 *   eveo.conf      hand-edited settings
 *   positions.conf written by us whenever a thumbnail is dragged
 */
#include "eveo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static SavedPos saved[MAX_CLIENTS];
static int      n_saved;
static bool     pos_dirty;

const char *config_dir(void)
{
    static char dir[512];
    if (dir[0]) return dir;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/eve-o-preview", xdg);
    else             snprintf(dir, sizeof dir, "%s/.config/eve-o-preview", home ? home : ".");
    return dir;
}

char *config_path(const char *leaf)
{
    char *p = malloc(640);
    snprintf(p, 640, "%s/%s", config_dir(), leaf);
    return p;
}

static void ensure_dir(void)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", config_dir());
    /* create parents as needed */
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    c->thumb_w         = 320;
    c->thumb_h         = 180;
    c->refresh_ms      = 100;          /* 10 fps */
    c->border          = 2;
    c->color_active    = 0xff5c1a;     /* EVE amber */
    c->color_inactive  = 0x303840;
    c->color_bg        = 0x101418;
    c->hide_active     = false;
    c->show_label      = true;
    c->label_scale     = 1;
    c->opacity         = 1.0;
    c->override_redirect = false;
    c->snap            = 0;
    c->click_activates = true;
    snprintf(c->match_prefix, sizeof c->match_prefix, "EVE - ");
    snprintf(c->ignore[0], 64, "EVE Online Launcher");
    c->n_ignore = 1;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool parse_bool(const char *v)
{
    return !strcasecmp(v,"true") || !strcasecmp(v,"yes") ||
           !strcasecmp(v,"on")   || !strcmp(v,"1");
}

static uint32_t parse_color(const char *v)
{
    while (*v == '#') v++;
    return (uint32_t)strtoul(v, NULL, 16) & 0xffffff;
}

bool config_load(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "eveo.conf:%d: ignoring '%s'\n", lineno, s); continue; }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);

        if      (!strcmp(k,"thumbnail_width"))   c->thumb_w   = atoi(v);
        else if (!strcmp(k,"thumbnail_height"))  c->thumb_h   = atoi(v);
        else if (!strcmp(k,"refresh_ms"))        c->refresh_ms= atoi(v);
        else if (!strcmp(k,"fps"))               c->refresh_ms= atoi(v) > 0 ? 1000/atoi(v) : 100;
        else if (!strcmp(k,"border"))            c->border    = atoi(v);
        else if (!strcmp(k,"color_active"))      c->color_active   = parse_color(v);
        else if (!strcmp(k,"color_inactive"))    c->color_inactive = parse_color(v);
        else if (!strcmp(k,"color_background"))  c->color_bg  = parse_color(v);
        else if (!strcmp(k,"hide_active_client"))c->hide_active = parse_bool(v);
        else if (!strcmp(k,"show_label"))        c->show_label  = parse_bool(v);
        else if (!strcmp(k,"label_scale"))       c->label_scale = atoi(v) < 1 ? 1 : atoi(v);
        else if (!strcmp(k,"opacity"))           c->opacity     = atof(v);
        else if (!strcmp(k,"override_redirect")) c->override_redirect = parse_bool(v);
        else if (!strcmp(k,"snap"))              c->snap        = atoi(v);
        else if (!strcmp(k,"click_activates"))   c->click_activates = parse_bool(v);
        else if (!strcmp(k,"match_prefix"))      snprintf(c->match_prefix, sizeof c->match_prefix, "%s", v);
        else if (!strcmp(k,"ignore")) {
            if (c->n_ignore < MAX_IGNORE)
                snprintf(c->ignore[c->n_ignore++], 64, "%s", v);
        }
        else fprintf(stderr, "eveo.conf:%d: unknown key '%s'\n", lineno, k);
    }
    fclose(f);
    if (c->refresh_ms < 16)  c->refresh_ms = 16;   /* cap at ~60fps */
    if (c->opacity < 0.1)    c->opacity = 0.1;
    if (c->opacity > 1.0)    c->opacity = 1.0;
    if (c->thumb_w < 40)     c->thumb_w = 40;
    if (c->thumb_h < 30)     c->thumb_h = 30;
    return true;
}

/* ---------- positions ---------- */

void pos_load(void)
{
    char *p = config_path("positions.conf");
    FILE *f = fopen(p, "r");
    free(p);
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        /* NAME = x y w h   (name may contain spaces) */
        char *eq = strrchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *name = trim(s);
        int X, Y, W, H;
        if (sscanf(eq + 1, "%d %d %d %d", &X, &Y, &W, &H) != 4) continue;
        if (n_saved >= MAX_CLIENTS) break;
        SavedPos *sp = &saved[n_saved++];
        snprintf(sp->name, NAME_LEN, "%s", name);
        sp->x = X; sp->y = Y; sp->w = W; sp->h = H; sp->valid = true;
    }
    fclose(f);
}

bool pos_get(const char *name, int *X, int *Y, int *W, int *H)
{
    for (int i = 0; i < n_saved; i++)
        if (saved[i].valid && !strcmp(saved[i].name, name)) {
            *X = saved[i].x; *Y = saved[i].y;
            *W = saved[i].w; *H = saved[i].h;
            return true;
        }
    return false;
}

void pos_set(const char *name, int X, int Y, int W, int H)
{
    for (int i = 0; i < n_saved; i++)
        if (saved[i].valid && !strcmp(saved[i].name, name)) {
            if (saved[i].x != X || saved[i].y != Y ||
                saved[i].w != W || saved[i].h != H) pos_dirty = true;
            saved[i].x = X; saved[i].y = Y; saved[i].w = W; saved[i].h = H;
            return;
        }
    if (n_saved >= MAX_CLIENTS) return;
    SavedPos *sp = &saved[n_saved++];
    snprintf(sp->name, NAME_LEN, "%s", name);
    sp->x = X; sp->y = Y; sp->w = W; sp->h = H; sp->valid = true;
    pos_dirty = true;
}

void pos_save(void)
{
    if (!pos_dirty) return;
    ensure_dir();
    char *p = config_path("positions.conf");
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "eveo: cannot write %s: %s\n", tmp, strerror(errno)); free(p); return; }
    fprintf(f, "# Written by eveo. Thumbnail positions, one per character.\n");
    fprintf(f, "# NAME = x y width height\n");
    for (int i = 0; i < n_saved; i++)
        if (saved[i].valid)
            fprintf(f, "%s = %d %d %d %d\n", saved[i].name,
                    saved[i].x, saved[i].y, saved[i].w, saved[i].h);
    fclose(f);
    if (rename(tmp, p) != 0)
        fprintf(stderr, "eveo: cannot save positions: %s\n", strerror(errno));
    free(p);
    pos_dirty = false;
}
