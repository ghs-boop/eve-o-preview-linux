/* fake-client: a stand-in EVE client window for developing/testing eveo
 * without running the game. Creates a normal window titled "EVE - <name>"
 * and animates, so thumbnail liveness is visually obvious.
 *
 *   gcc -O2 -o fake-client tools/fake-client.c $(pkg-config --cflags --libs xcb)
 *   ./fake-client "Test Pilot One" [width] [height]
 */
#include <xcb/xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv){
    const char *who = argc > 1 ? argv[1] : "Test Pilot";
    int W = argc > 2 ? atoi(argv[2]) : 1280;
    int H = argc > 3 ? atoi(argv[3]) : 720;

    int n; xcb_connection_t *c = xcb_connect(NULL, &n);
    if(xcb_connection_has_error(c)){ fprintf(stderr,"X connect failed\n"); return 1; }
    const xcb_setup_t *s = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(s);
    for(int i=0;i<n;i++) xcb_screen_next(&it);
    xcb_screen_t *scr = it.data;

    xcb_window_t w = xcb_generate_id(c);
    uint32_t v[2] = { 0x00101820, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, scr->root, 0,0, W,H, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
        XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK, v);

    char title[256]; snprintf(title,sizeof title,"EVE - %s", who);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING, 8, strlen(title), title);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
        xcb_intern_atom(c,0,12,"_NET_WM_NAME"), NULL);
    xcb_intern_atom_reply_t *u = xcb_intern_atom_reply(c,
        xcb_intern_atom(c,0,11,"UTF8_STRING"), NULL);
    if(r&&u) xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, r->atom, u->atom, 8,
        strlen(title), title);
    /* mimic the real client's WM_CLASS under Proton */
    const char cls[] = "steam_app_default\0steam_app_default";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, sizeof cls, cls);

    xcb_map_window(c, w);
    xcb_flush(c);

    xcb_gcontext_t gc = xcb_generate_id(c);
    uint32_t gv = scr->white_pixel;
    xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, &gv);

    unsigned long t = 0;
    for(;;){
        /* starfield-ish background wash + a moving marker */
        uint32_t bg = 0x00081018 + ((t*2)&0x3f);
        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &bg);
        xcb_rectangle_t full = {0,0,W,H};
        xcb_poly_fill_rectangle(c, w, gc, 1, &full);

        for(int i=0;i<40;i++){
            uint32_t col = 0x00404040 + (uint32_t)((i*37 + t*3) % 0xB0) * 0x010101;
            xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &col);
            xcb_rectangle_t st = { (int16_t)((i*193 + t*4) % W),
                                   (int16_t)((i*127) % H), 3, 3 };
            xcb_poly_fill_rectangle(c, w, gc, 1, &st);
        }
        uint32_t marker = 0x00ff9020;
        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &marker);
        xcb_rectangle_t box = { (int16_t)((t*9) % (W-160)), (int16_t)(H/2 - 60), 160, 120 };
        xcb_poly_fill_rectangle(c, w, gc, 1, &box);

        uint32_t bar = 0x0030c0ff;
        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &bar);
        xcb_rectangle_t hdr = {0,0,W,48};
        xcb_poly_fill_rectangle(c, w, gc, 1, &hdr);

        xcb_flush(c);
        xcb_generic_event_t *ev;
        while((ev = xcb_poll_for_event(c))) free(ev);
        usleep(60000);
        t++;
    }
    return 0;
}
