/*
 * ccos-xwm - a very small window manager for X11 apps on ccos
 *
 * X11 apps (older programs and many games) run inside the "X11 apps" window
 * (Xwayland). Without a window manager they would all pile up in the
 * top-left corner. This one:
 *   - places new windows in the middle (a little cascaded)
 *   - clicking a window brings it to the front and gives it the keyboard
 *   - Alt + drag moves a window
 *   - "full screen" requests (games, videos) fill the whole X11 area
 *   - quits by itself a while after the last window closes, so Xwayland
 *     can quit too
 *
 * Build: gcc -O2 -o ccos-xwm xwm.c -lxcb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_atom_t A_STATE, A_FULL, A_SUPPORTED, A_CHECK, A_NAME, A_UTF8, A_ACTIVE, A_PROTOCOLS, A_TAKE_FOCUS;

#define MAXW 256
static struct { xcb_window_t w; int full; int16_t x, y; uint16_t wd, ht; } wins[MAXW];
static int nw, cascade;
static xcb_window_t drag_w; static int drag_dx, drag_dy;

static xcb_atom_t atom(const char *n) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}
static int find(xcb_window_t w) { for (int i = 0; i < nw; i++) if (wins[i].w == w) return i; return -1; }

static void focus(xcb_window_t w) {
    uint32_t v = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_STACK_MODE, &v);
    xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, w, XCB_CURRENT_TIME);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, scr->root, A_ACTIVE, XCB_ATOM_WINDOW, 32, 1, &w);
}

static void set_full(int i, int on) {
    xcb_window_t w = wins[i].w;
    if (on == wins[i].full) return;
    if (on) {
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
        if (g) { wins[i].x = g->x; wins[i].y = g->y; wins[i].wd = g->width; wins[i].ht = g->height; free(g); }
        uint32_t v[5] = { 0, 0, scr->width_in_pixels, scr->height_in_pixels, 0 };
        xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, v);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, A_STATE, XCB_ATOM_ATOM, 32, 1, &A_FULL);
    } else {
        uint32_t v[4] = { (uint32_t)wins[i].x, (uint32_t)wins[i].y, wins[i].wd, wins[i].ht };
        xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT, v);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, A_STATE, XCB_ATOM_ATOM, 32, 0, NULL);
    }
    wins[i].full = on;
    focus(w);
}

static int wants_full(xcb_window_t w) {   /* asked for full screen before showing up */
    xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, A_STATE, XCB_ATOM_ATOM, 0, 32), NULL);
    int full = 0;
    if (r) {
        xcb_atom_t *a = xcb_get_property_value(r);
        for (int k = 0; k < xcb_get_property_value_length(r) / 4; k++) if (a[k] == A_FULL) full = 1;
        free(r);
    }
    return full;
}

static void manage(xcb_window_t w) {
    if (find(w) >= 0 || nw >= MAXW) { xcb_map_window(c, w); return; }
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
    int ww = g ? g->width : 400, hh = g ? g->height : 300, gx = g ? g->x : 0, gy = g ? g->y : 0;
    free(g);
    int sw = scr->width_in_pixels, sh = scr->height_in_pixels;
    if (gx == 0 && gy == 0) {               /* no position of its own: middle, cascaded */
        gx = (sw - ww) / 2 + cascade * 24; gy = (sh - hh) / 2 + cascade * 24;
        cascade = (cascade + 1) % 6;
    }
    if (gx + ww > sw) gx = sw - ww;
    if (gy + hh > sh) gy = sh - hh;
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    uint32_t v[2] = { (uint32_t)gx, (uint32_t)gy };
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    uint32_t ev = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, w, XCB_CW_EVENT_MASK, &ev);
    /* a click anywhere in it brings it to the front (the click still goes to the app) */
    xcb_grab_button(c, 0, w, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);
    /* Alt + drag moves it */
    xcb_grab_button(c, 0, w, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_1);
    wins[nw].w = w; wins[nw].full = 0; nw++;
    xcb_map_window(c, w);
    if (wants_full(w)) set_full(nw - 1, 1);
    focus(w);
}

static void unmanage(xcb_window_t w) {
    int i = find(w);
    if (i < 0) return;
    wins[i] = wins[--nw];
    if (nw) focus(wins[nw - 1].w);
}

int main(void) {
    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) return 1;
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_generic_error_t *err = xcb_request_check(c, xcb_change_window_attributes_checked(c, scr->root, XCB_CW_EVENT_MASK, &mask));
    if (err) { fprintf(stderr, "ccos-xwm: another window manager is running\n"); return 1; }

    A_STATE = atom("_NET_WM_STATE"); A_FULL = atom("_NET_WM_STATE_FULLSCREEN");
    A_SUPPORTED = atom("_NET_SUPPORTED"); A_CHECK = atom("_NET_SUPPORTING_WM_CHECK");
    A_NAME = atom("_NET_WM_NAME"); A_UTF8 = atom("UTF8_STRING"); A_ACTIVE = atom("_NET_ACTIVE_WINDOW");
    A_PROTOCOLS = atom("WM_PROTOCOLS"); A_TAKE_FOCUS = atom("WM_TAKE_FOCUS");
    /* tell apps what we can do (games look for full-screen support) */
    xcb_atom_t sup[] = { A_STATE, A_FULL, A_ACTIVE, A_CHECK, A_NAME };
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, scr->root, A_SUPPORTED, XCB_ATOM_ATOM, 32, 5, sup);
    xcb_window_t chk = xcb_generate_id(c);
    xcb_create_window(c, 0, chk, scr->root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, scr->root, A_CHECK, XCB_ATOM_WINDOW, 32, 1, &chk);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, chk, A_CHECK, XCB_ATOM_WINDOW, 32, 1, &chk);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, chk, A_NAME, A_UTF8, 8, 4, "ccos");

    /* windows that were already there */
    xcb_query_tree_reply_t *t = xcb_query_tree_reply(c, xcb_query_tree(c, scr->root), NULL);
    if (t) {
        xcb_window_t *ch = xcb_query_tree_children(t);
        for (int i = 0; i < xcb_query_tree_children_length(t); i++) {
            xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, ch[i]), NULL);
            if (a && !a->override_redirect && a->map_state == XCB_MAP_STATE_VIEWABLE && ch[i] != chk) manage(ch[i]);
            free(a);
        }
        free(t);
    }
    xcb_flush(c);

    time_t empty_since = time(NULL);
    struct pollfd pfd = { xcb_get_file_descriptor(c), POLLIN, 0 };
    for (;;) {
        xcb_generic_event_t *e;
        while ((e = xcb_poll_for_event(c))) {
            switch (e->response_type & 0x7f) {
            case XCB_MAP_REQUEST: manage(((xcb_map_request_event_t *)e)->window); break;
            case XCB_UNMAP_NOTIFY: unmanage(((xcb_unmap_notify_event_t *)e)->window); break;
            case XCB_DESTROY_NOTIFY: unmanage(((xcb_destroy_notify_event_t *)e)->window); break;
            case XCB_CONFIGURE_REQUEST: {
                xcb_configure_request_event_t *r = (xcb_configure_request_event_t *)e;
                int i = find(r->window);
                if (i >= 0 && wins[i].full) break;          /* full screen stays full screen */
                uint32_t v[7]; int k = 0; uint16_t m = 0;
                if (r->value_mask & XCB_CONFIG_WINDOW_X) { m |= XCB_CONFIG_WINDOW_X; v[k++] = (uint32_t)r->x; }
                if (r->value_mask & XCB_CONFIG_WINDOW_Y) { m |= XCB_CONFIG_WINDOW_Y; v[k++] = (uint32_t)r->y; }
                if (r->value_mask & XCB_CONFIG_WINDOW_WIDTH) { m |= XCB_CONFIG_WINDOW_WIDTH; v[k++] = r->width; }
                if (r->value_mask & XCB_CONFIG_WINDOW_HEIGHT) { m |= XCB_CONFIG_WINDOW_HEIGHT; v[k++] = r->height; }
                if (r->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) { m |= XCB_CONFIG_WINDOW_BORDER_WIDTH; v[k++] = r->border_width; }
                if (r->value_mask & XCB_CONFIG_WINDOW_SIBLING) { m |= XCB_CONFIG_WINDOW_SIBLING; v[k++] = r->sibling; }
                if (r->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) { m |= XCB_CONFIG_WINDOW_STACK_MODE; v[k++] = r->stack_mode; }
                xcb_configure_window(c, r->window, m, v);
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t *m = (xcb_client_message_event_t *)e;
                int i = find(m->window);
                if (m->type == A_STATE && i >= 0 &&
                    (m->data.data32[1] == A_FULL || m->data.data32[2] == A_FULL)) {
                    int act = (int)m->data.data32[0];          /* 0 remove, 1 add, 2 toggle */
                    set_full(i, act == 2 ? !wins[i].full : act == 1);
                } else if (m->type == A_ACTIVE && i >= 0) focus(m->window);
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *b = (xcb_button_press_event_t *)e;
                int i = find(b->event);
                if (b->state & XCB_MOD_MASK_1) {            /* Alt + drag: move */
                    if (i >= 0 && !wins[i].full) {
                        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, b->event), NULL);
                        if (g) { drag_w = b->event; drag_dx = b->root_x - g->x; drag_dy = b->root_y - g->y; free(g); }
                    }
                    if (i >= 0) focus(b->event);
                } else {
                    if (i >= 0) focus(b->event);
                    xcb_allow_events(c, XCB_ALLOW_REPLAY_POINTER, b->time);   /* the app gets the click */
                }
                break;
            }
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *mo = (xcb_motion_notify_event_t *)e;
                if (drag_w) {
                    uint32_t v[2] = { (uint32_t)(mo->root_x - drag_dx), (uint32_t)(mo->root_y - drag_dy) };
                    xcb_configure_window(c, drag_w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
                }
                break;
            }
            case XCB_BUTTON_RELEASE: drag_w = 0; break;
            case XCB_CONFIGURE_NOTIFY: {                     /* the X11 area changed size */
                xcb_configure_notify_event_t *cn = (xcb_configure_notify_event_t *)e;
                if (cn->window == scr->root) {
                    scr->width_in_pixels = cn->width; scr->height_in_pixels = cn->height;
                    for (int k = 0; k < nw; k++) if (wins[k].full) { wins[k].full = 0; set_full(k, 1); }
                }
                break;
            }
            }
            free(e);
        }
        xcb_flush(c);
        if (xcb_connection_has_error(c)) return 0;
        /* still any app windows at all (shown or not)? none for a few seconds: let Xwayland quit */
        int others = nw;
        if (!others) {
            xcb_query_tree_reply_t *q = xcb_query_tree_reply(c, xcb_query_tree(c, scr->root), NULL);
            if (q) {
                xcb_window_t *ch = xcb_query_tree_children(q);
                for (int i = 0; i < xcb_query_tree_children_length(q); i++) if (ch[i] != chk) others++;
                free(q);
            }
        }
        if (others) empty_since = time(NULL);
        else if (time(NULL) - empty_since > 5) return 0;
        poll(&pfd, 1, 1000);
    }
}
