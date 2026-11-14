/*
 * wayland.c - the ccos desktop as a Wayland compositor
 *
 * Wayland is the common language Linux apps use to put a window on the
 * screen. With this, any Wayland app (the foot terminal, and later GTK/Qt
 * apps, Firefox...) draws its own picture and hands it to ccos, and ccos
 * shows it inside one of its glass windows, with the ccos title bar and
 * buttons around it. Mouse and keyboard go to the app under the pointer /
 * the active window, just like ccos's own apps.
 *
 * What's here:
 *   wl_compositor, wl_region, wl_subcompositor  surfaces (and sub-surfaces)
 *   wl_shm                                      pictures in shared memory
 *   wl_seat: wl_pointer, wl_keyboard            mouse + keyboard (xkb keymap)
 *   wl_output                                   the screen
 *   xdg_wm_base: xdg_surface, xdg_toplevel      app windows
 *   zxdg_decoration_manager_v1                  "ccos draws the title bar"
 *   wl_data_device_manager                      the clipboard between Wayland apps
 * Not yet: popup menus (they close at once), drag and drop, GPU.
 *
 * Included from desktop.c (it uses the desktop's windows and drawing).
 */
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include "wl/xdg-shell-protocol.h"
#include "wl/xdg-decoration-unstable-v1-protocol.h"
#include "wl/xdg-shell-protocol.c"
#include "wl/xdg-decoration-unstable-v1-protocol.c"
#include "wl/relative-pointer-unstable-v1-protocol.h"
#include "wl/pointer-constraints-unstable-v1-protocol.h"
#include "wl/relative-pointer-unstable-v1-protocol.c"
#include "wl/pointer-constraints-unstable-v1-protocol.c"
#include "wl/linux-dmabuf-unstable-v1-protocol.h"
#include "wl/linux-dmabuf-unstable-v1-protocol.c"

#define WLS 8                       /* how many Wayland windows at once */

static struct wl_display *wl_dpy;
static struct wl_event_loop *wl_loop;
static struct wl_list wl_pointers, wl_keyboards, wl_outputs;   /* resources, linked via wl_resource_get_link */
static struct wl_list wl_relptrs;                               /* relative mouse movement (games) */
static struct wl_client *cursor_hide_client;                    /* asked for no pointer picture (games, videos) */

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;
static int kmap_fd = -1; static uint32_t kmap_size;
static uint32_t mods_dep, mods_lat, mods_lck, mods_grp;

typedef struct wsurf wsurf;
typedef struct { int n; int r[8][4]; int sub; } wregion;   /* a wl_region: a few rectangles (sub: something was cut out) */
typedef struct wtop wtop;

struct wsurf {
    struct wl_resource *res;
    struct wl_resource *pbuf;                  /* attached, not yet committed */
    struct wl_listener pbuf_gone;
    int has_pbuf;
    struct wl_list pframes, frames;            /* frame callbacks (pending, current) */
    uint32_t *px; int w, h, alpha;             /* our copy of the last picture */
    int role;                                  /* 0 none, 1 window, 2 popup, 3 sub-surface */
    wtop *top;                                 /* role 1 */
    struct wpop *pop;                          /* role 2 */
    struct wl_resource *xdg;                   /* its xdg_surface */
    int configured, entered;
    int in_set, in_n, pin_set, pin_n, pin_dirty;   /* input region: where the surface takes the mouse */
    int in_r[8][4], pin_r[8][4];                    /* (0 = everywhere; set + no rects = nowhere) */
    wsurf *parent; int sx, sy;                 /* role 3 */
    struct wl_resource *subres;
    wsurf *subs[8]; int nsubs;                 /* sub-surfaces drawn on top of this one */
    int dx0, dy0, dx1, dy1, dset;              /* damage: what changed, since the last commit (pending) */
    int rx0, ry0, rx1, ry1, rset;              /* ...and in the last commit: only that is copied and drawn again */
    int op_n, op_r[8][4], pop_n, pop_r[8][4], pop_dirty;   /* opaque region: no need to draw what's under it */
};

struct wtop {
    int used;
    wsurf *s;
    struct wl_resource *res, *deco;
    struct wl_client *client;
    char title[96], app_id[64];
    int win;                                   /* index into wins[], -1 until it has a picture */
    int gx, gy, gw, gh, has_geo;               /* the window part of the picture (without shadows) */
    int maxed;
    int sized;                                 /* the user resized it: the app must fit the window */
    int cfg_pending; long long cfg_ms;         /* a new size to tell the app (a few times a second while dragging) */
};
static wtop tops[WLS];

typedef struct { int w, h, ax, ay, aw, ah, anchor, gravity, cadj, ox, oy; } wpos;
typedef struct wpop {
    int used;
    wsurf *s, *parent;                         /* the popup and what it belongs to (a window or another popup) */
    struct wl_resource *res;
    wpos pos;
    int x, y;                                  /* where, from the parent's top-left (window geometry) */
    int gx, gy, has_geo;                       /* its own geometry offset (shadows) */
    int mapped, grab;
} wpop;
#define WPOPS 8
static wpop pops[WPOPS];
static int popstack[WPOPS], npop;              /* open popups, newest last */
static int pop_btn;                            /* a mouse button was pressed on a popup */
static void popup_send_cfg(wpop *p);
static void pop_repaint(wpop *p);
static void pop_unstack(wpop *p);
static wsurf *popup_at(int x, int y, int *lx, int *ly);
static void (*wl_resize_hook)(int win, int edges);   /* set by the desktop: start resizing a window */

static wsurf *kb_focus, *ptr_focus;            /* who gets keys / the mouse */
static struct wl_client *ptr_client;
static int ptr_btn_slot = -1;                  /* left button held down in this window */
static void wl_send_selection(struct wl_client *to);
static void cons_update(void);

static uint32_t wl_now(void) { return (uint32_t)ui_ms(); }

/* apps can send many frames at once: collect what changed, redraw once per loop */
static int wl_rp_on, wl_rp_x0, wl_rp_y0, wl_rp_x1, wl_rp_y1;
static void wl_repaint_later(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!wl_rp_on) { wl_rp_on = 1; wl_rp_x0 = x; wl_rp_y0 = y; wl_rp_x1 = x + w; wl_rp_y1 = y + h; return; }
    wl_rp_x0 = imin(wl_rp_x0, x); wl_rp_y0 = imin(wl_rp_y0, y);
    wl_rp_x1 = imax(wl_rp_x1, x + w); wl_rp_y1 = imax(wl_rp_y1, y + h);
}
static void wl_repaint_now(void) {
    if (!wl_rp_on) return;
    wl_rp_on = 0;
    repaint(wl_rp_x0, wl_rp_y0, wl_rp_x1 - wl_rp_x0, wl_rp_y1 - wl_rp_y0);
}
static uint32_t wl_serial(void) { return wl_display_next_serial(wl_dpy); }

/* ---- small helpers ----------------------------------------------------- */
static void res_unlink(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void res_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static int slot_of_win(int win) {
    for (int i = 0; i < WLS; i++) if (tops[i].used && tops[i].win == win) return i;
    return -1;
}

/* the size of a window's content area */
static void wl_client_size(int win, int *cw, int *ch) {
    int x, y; client_rect(&wins[win], &x, &y, cw, ch);
}

/* ======================================================================
 *  KEYBOARD AND MOUSE FOCUS
 * ====================================================================== */
static void kb_send_mods(struct wl_client *only) {
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_keyboards) {
        if (!kb_focus || wl_resource_get_client(r) != wl_resource_get_client(kb_focus->res)) continue;
        if (only && wl_resource_get_client(r) != only) continue;
        wl_keyboard_send_modifiers(r, wl_serial(), mods_dep, mods_lat, mods_lck, mods_grp);
    }
}

static void kb_set_focus(wsurf *s) {
    if (s == kb_focus) return;
    struct wl_resource *r;
    if (kb_focus) {
        wl_resource_for_each(r, &wl_keyboards)
            if (wl_resource_get_client(r) == wl_resource_get_client(kb_focus->res))
                wl_keyboard_send_leave(r, wl_serial(), kb_focus->res);
    }
    kb_focus = s;
    if (s) {
        wl_send_selection(wl_resource_get_client(s->res));   /* what's on the clipboard */
        struct wl_array keys; wl_array_init(&keys);
        wl_resource_for_each(r, &wl_keyboards)
            if (wl_resource_get_client(r) == wl_resource_get_client(s->res)) {
                wl_keyboard_send_enter(r, wl_serial(), s->res, &keys);
                wl_keyboard_send_modifiers(r, wl_serial(), mods_dep, mods_lat, mods_lck, mods_grp);
            }
        wl_array_release(&keys);
    }
}

static void ptr_frame(struct wl_resource *r) {
    if (wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION) wl_pointer_send_frame(r);
}

static void ptr_set_focus(wsurf *s, int lx, int ly) {
    struct wl_resource *r;
    if (s == ptr_focus) return;
    if (ptr_focus) {
        wl_resource_for_each(r, &wl_pointers)
            if (wl_resource_get_client(r) == ptr_client) { wl_pointer_send_leave(r, wl_serial(), ptr_focus->res); ptr_frame(r); }
    }
    ptr_focus = s;
    ptr_client = s ? wl_resource_get_client(s->res) : NULL;
    if (s) {
        wl_resource_for_each(r, &wl_pointers)
            if (wl_resource_get_client(r) == ptr_client) {
                wl_pointer_send_enter(r, wl_serial(), s->res, wl_fixed_from_int(lx), wl_fixed_from_int(ly));
                ptr_frame(r);
            }
    }
}

/* called by the desktop every loop: the active ccos window gets the keyboard */
static void wl_update_focus(void) {
    if (!wl_dpy) return;
    int a = active_win();
    int s = (a >= 0 && !WHIDDEN(&wins[a]) && !menu_open) ? slot_of_win(a) : -1;
    wsurf *f = s >= 0 ? tops[s].s : NULL;
    for (int i = npop - 1; i >= 0 && f; i--)              /* an open menu takes the keys */
        if (pops[popstack[i]].grab && pops[popstack[i]].mapped && pops[popstack[i]].s) { f = pops[popstack[i]].s; break; }
    kb_set_focus(f);
    cons_update();
}

/* ======================================================================
 *  SURFACES
 * ====================================================================== */
static void surf_frames_done(wsurf *s) {
    struct wl_resource *r, *t;
    wl_resource_for_each_safe(r, t, &s->frames) {
        wl_callback_send_done(r, wl_now());
        wl_resource_destroy(r);
    }
}

/* where a surface is on screen (its top-left), -1 if not shown */
static int surf_screen_pos(wsurf *s, int *x, int *y) {
    int ox = 0, oy = 0;
    while (s && s->role == 3) { ox += s->sx; oy += s->sy; s = s->parent; }
    if (!s || s->role != 1 || !s->top || s->top->win < 0) return -1;
    wtop *t = s->top;
    int cx, cy, cw, ch; client_rect(&wins[t->win], &cx, &cy, &cw, &ch);
    *x = cx - (t->has_geo ? t->gx : 0) + ox; *y = cy - (t->has_geo ? t->gy : 0) + oy;
    return t->win;
}

static void wl_open_window(wtop *t);

/* "you are on this screen": apps (Firefox!) only draw once they know */
static void surf_enter_output(wsurf *s, int depth) {
    if (!s || depth > 6) return;
    if (!s->entered) {
        s->entered = 1;
        struct wl_resource *o;
        wl_resource_for_each(o, &wl_outputs)
            if (wl_resource_get_client(o) == wl_resource_get_client(s->res)) wl_surface_send_enter(s->res, o);
    }
    for (int i = 0; i < s->nsubs; i++) surf_enter_output(s->subs[i], depth + 1);
}
static void wl_window_resized_by_app(wtop *t);

static int wl_is_dmabuf(struct wl_resource *buf);
static int dmabuf_copy(struct wl_resource *buf, uint32_t **px, int *pw, int *ph, int *alpha, const int *rect);
/* the new picture: only the part the app says changed is copied (all of it when the size changes) */
static void surf_commit_buffer(wsurf *s) {
    int rect[4], part = 0;
    s->rset = 0;
    if (wl_is_dmabuf(s->pbuf)) {                 /* drawn by the graphics chip */
        int a = 0, ow = s->w, oh = s->h;
        if (s->dset && s->px && s->alpha >= 0) { rect[0] = s->dx0; rect[1] = s->dy0; rect[2] = s->dx1; rect[3] = s->dy1; part = 1; }
        if (dmabuf_copy(s->pbuf, &s->px, &s->w, &s->h, &a, part ? rect : NULL)) {
            s->alpha = a;
            if (part && s->w == ow && s->h == oh) { s->rx0 = rect[0]; s->ry0 = rect[1]; s->rx1 = rect[2]; s->ry1 = rect[3]; s->rset = 1; }
        }
        wl_buffer_send_release(s->pbuf);
        s->dset = 0;
        return;
    }
    struct wl_shm_buffer *b = wl_shm_buffer_get(s->pbuf);
    if (b) {
        int w = wl_shm_buffer_get_width(b), h = wl_shm_buffer_get_height(b);
        int stride = wl_shm_buffer_get_stride(b);
        uint32_t fmt = wl_shm_buffer_get_format(b);
        int x0 = 0, y0 = 0, x1 = w, y1 = h;
        if (w != s->w || h != s->h || !s->px) {
            free(s->px); s->px = malloc((size_t)w * h * 4);
            s->w = s->px ? w : 0; s->h = s->px ? h : 0;
        } else if (s->dset) {                                /* same size: just what changed */
            x0 = imax(0, s->dx0); y0 = imax(0, s->dy0); x1 = imin(w, s->dx1); y1 = imin(h, s->dy1);
            if (x0 >= x1 || y0 >= y1) { x0 = y0 = x1 = y1 = 0; }
            s->rx0 = x0; s->ry0 = y0; s->rx1 = x1; s->ry1 = y1; s->rset = 1;
        }
        if (s->px && x1 > x0) {
            wl_shm_buffer_begin_access(b);
            const unsigned char *src = wl_shm_buffer_get_data(b);
            for (int y = y0; y < y1; y++) memcpy(s->px + (size_t)y * w + x0, src + (size_t)y * stride + (size_t)x0 * 4, (size_t)(x1 - x0) * 4);
            wl_shm_buffer_end_access(b);
        }
        s->alpha = fmt == WL_SHM_FORMAT_ARGB8888;
    }
    s->dset = 0;
    wl_buffer_send_release(s->pbuf);
}

static void surf_repaint(wsurf *s) {
    int x, y, win = surf_screen_pos(s, &x, &y);
    if (win < 0 || WHIDDEN(&wins[win])) return;
    int cx, cy, cw, ch; client_rect(&wins[win], &cx, &cy, &cw, &ch);
    int sx0 = 0, sy0 = 0, sx1 = s->w, sy1 = s->h;
    if (s->rset) { sx0 = s->rx0; sy0 = s->ry0; sx1 = s->rx1; sy1 = s->ry1; s->rset = 0; if (sx1 <= sx0) return; }   /* only what changed */
    int x0 = imax(x + sx0, cx), y0 = imax(y + sy0, cy), x1 = imin(x + sx1, cx + cw), y1 = imin(y + sy1, cy + ch);
    if (x0 < x1 && y0 < y1) wl_repaint_later(x0, y0, x1 - x0, y1 - y0);
}

static void surf_send_configure(wtop *t) {
    struct wl_array st; wl_array_init(&st);
    uint32_t *p;
    int full = t->win >= 0 && wins[t->win].fullscreen;
    if (full) { p = wl_array_add(&st, 4); *p = XDG_TOPLEVEL_STATE_FULLSCREEN; }
    else if (t->maxed) { p = wl_array_add(&st, 4); *p = XDG_TOPLEVEL_STATE_MAXIMIZED; }
    if (t->win >= 0 && active_win() == t->win) { p = wl_array_add(&st, 4); *p = XDG_TOPLEVEL_STATE_ACTIVATED; }
    int w = 0, h = 0;
    if (t->win >= 0 && (t->maxed || t->sized || full)) wl_client_size(t->win, &w, &h);
    xdg_toplevel_send_configure(t->res, w, h, &st);
    wl_array_release(&st);
    xdg_surface_send_configure(t->s->xdg, wl_serial());
}

static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    (void)c; (void)x; (void)y;
    wsurf *s = wl_resource_get_user_data(r);
    if (s->pbuf) wl_list_remove(&s->pbuf_gone.link);
    s->pbuf = buf; s->has_pbuf = 1;
    if (buf) wl_resource_add_destroy_listener(buf, &s->pbuf_gone);
}
static void surf_pbuf_gone(struct wl_listener *l, void *data) {
    (void)data;
    wsurf *s = wl_container_of(l, s, pbuf_gone);
    wl_list_remove(&s->pbuf_gone.link);
    s->pbuf = NULL;
}
static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    wsurf *s = wl_resource_get_user_data(r);
    if (w <= 0 || h <= 0) return;
    long long x1 = (long long)x + w, y1 = (long long)y + h;
    if (x1 > 1 << 20) x1 = 1 << 20;
    if (y1 > 1 << 20) y1 = 1 << 20;
    if (!s->dset) { s->dx0 = x; s->dy0 = y; s->dx1 = (int)x1; s->dy1 = (int)y1; s->dset = 1; }
    else { s->dx0 = imin(s->dx0, x); s->dy0 = imin(s->dy0, y); s->dx1 = imax(s->dx1, (int)x1); s->dy1 = imax(s->dy1, (int)y1); }
}
static void frame_gone(struct wl_resource *r) { res_unlink(r); }
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    wsurf *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(cb, NULL, NULL, frame_gone);
    wl_list_insert(s->pframes.prev, wl_resource_get_link(cb));
}
static void surf_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {   /* the opaque region */
    (void)c;
    wsurf *s = wl_resource_get_user_data(r);
    wregion *g = reg ? wl_resource_get_user_data(reg) : NULL;
    s->pop_dirty = 1;
    s->pop_n = g && !g->sub ? g->n : 0;
    if (s->pop_n) memcpy(s->pop_r, g->r, sizeof s->pop_r);
}
static void surf_input_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {
    (void)c;
    wsurf *s = wl_resource_get_user_data(r);
    wregion *g = reg ? wl_resource_get_user_data(reg) : NULL;
    s->pin_dirty = 1;
    if (!g) { s->pin_set = 0; s->pin_n = 0; return; }        /* everywhere */
    s->pin_set = 1; s->pin_n = g->n;
    memcpy(s->pin_r, g->r, sizeof s->pin_r);
}
/* does the surface take the mouse at (x, y)? */
static int surf_takes_input(const wsurf *s, int x, int y) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    if (!s->in_set) return 1;
    for (int i = 0; i < s->in_n; i++)
        if (x >= s->in_r[i][0] && y >= s->in_r[i][1] && x < s->in_r[i][0] + s->in_r[i][2] && y < s->in_r[i][1] + s->in_r[i][3]) return 1;
    return 0;
}

static void surf_commit(struct wl_client *c, struct wl_resource *r) {
    wsurf *s = wl_resource_get_user_data(r);
    /* frame callbacks move to "current" */
    struct wl_resource *fr, *ft;
    wl_resource_for_each_safe(fr, ft, &s->pframes) { res_unlink(fr); wl_list_insert(s->frames.prev, wl_resource_get_link(fr)); }
    if (s->pop_dirty) { s->pop_dirty = 0; s->op_n = s->pop_n; memcpy(s->op_r, s->pop_r, sizeof s->op_r); }
    if (s->pin_dirty) {                                  /* the new input region takes effect */
        s->pin_dirty = 0; s->in_set = s->pin_set; s->in_n = s->pin_n;
        memcpy(s->in_r, s->pin_r, sizeof s->in_r);
    }
    if (!s->has_pbuf) s->rset = 0;                       /* no new picture: draw it all (it may have moved) */
    if (s->has_pbuf) {
        if (s->pbuf) surf_commit_buffer(s);
        else { free(s->px); s->px = NULL; s->w = s->h = 0; }     /* null buffer: hide */
        s->has_pbuf = 0;
        if (s->pbuf) { wl_list_remove(&s->pbuf_gone.link); s->pbuf = NULL; }
    }
    if (s->role == 2 && s->pop) {
        wpop *p = s->pop;
        if (!s->configured) { s->configured = 1; popup_send_cfg(p); }
        else if (s->px && !p->mapped) {
            p->mapped = 1;
            int k = (int)(p - pops); popstack[npop++] = k;
            surf_enter_output(s, 0);
        }
        pop_repaint(p);
        surf_frames_done(s);
        wl_client_flush(c);
        return;
    }
    if (s->role == 3) {                                  /* a sub-surface of a popup: repaint the popup */
        wsurf *b = s; while (b && b->role == 3) b = b->parent;
        if (b && b->role == 2 && b->pop) { pop_repaint(b->pop); surf_frames_done(s); wl_client_flush(c); return; }
    }
    if (s->role == 1 && s->top) {
        wtop *t = s->top;
        if (!s->configured) { s->configured = 1; surf_send_configure(t); }
        else if (s->px && t->win < 0) wl_open_window(t);
        else if (t->win >= 0) wl_window_resized_by_app(t);
    }
    if (s->role == 3 && !s->entered) {                   /* a sub-surface of something on screen */
        wsurf *b = s; while (b && b->role == 3) b = b->parent;
        if (b && b->entered) surf_enter_output(s, 0);
    }
    surf_repaint(s);
    /* the picture is taken: tell the app it may draw the next one, right now */
    surf_frames_done(s);
    wl_client_flush(c);
}
static void surf_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c; (void)r; (void)t; }

static const struct wl_surface_interface surf_impl = {
    .destroy = res_destroy, .attach = surf_attach, .damage = surf_damage, .frame = surf_frame,
    .set_opaque_region = surf_region, .set_input_region = surf_input_region, .commit = surf_commit,
    .set_buffer_transform = surf_transform, .set_buffer_scale = surf_transform, .damage_buffer = surf_damage,
};

static void top_unmap(wtop *t);

static void cons_surface_gone(wsurf *s);
static void surf_gone(struct wl_resource *r) {
    wsurf *s = wl_resource_get_user_data(r);
    cons_surface_gone(s);
    for (int i = 0; i < WPOPS; i++)
        if (pops[i].used && pops[i].parent == s) { pops[i].parent = NULL; if (pops[i].res) xdg_popup_send_popup_done(pops[i].res); }
    if (s->role == 2 && s->pop) { pop_repaint(s->pop); pop_unstack(s->pop); s->pop->s = NULL; s->pop->mapped = 0; }
    if (kb_focus == s) kb_focus = NULL;
    if (ptr_focus == s) { ptr_focus = NULL; ptr_client = NULL; }
    if (s->role == 1 && s->top) { top_unmap(s->top); s->top->s = NULL; }
    if (s->role == 3 && s->parent) {
        wsurf *p = s->parent;
        for (int i = 0; i < p->nsubs; i++) if (p->subs[i] == s) { memmove(&p->subs[i], &p->subs[i + 1], (size_t)(p->nsubs - 1 - i) * sizeof *p->subs); p->nsubs--; break; }
        surf_repaint(p);
    }
    for (int i = 0; i < s->nsubs; i++) s->subs[i]->parent = NULL;
    if (s->pbuf) wl_list_remove(&s->pbuf_gone.link);
    struct wl_resource *fr, *ft;
    wl_resource_for_each_safe(fr, ft, &s->pframes) wl_resource_destroy(fr);
    wl_resource_for_each_safe(fr, ft, &s->frames) wl_resource_destroy(fr);
    free(s->px);
    free(s);
}

/* ---- wl_compositor / wl_region ----------------------------------------- */
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    wregion *g = wl_resource_get_user_data(r);
    if (g && g->n < 8 && w > 0 && h > 0) { g->r[g->n][0] = x; g->r[g->n][1] = y; g->r[g->n][2] = w; g->r[g->n][3] = h; g->n++; }
}
static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)x; (void)y; (void)w; (void)h;                   /* rarely used for input: ignored */
    wregion *g = wl_resource_get_user_data(r); if (g) g->sub = 1;   /* (an opaque region with holes: not trusted) */
}
static const struct wl_region_interface region_impl = { .destroy = res_destroy, .add = region_add, .subtract = region_sub };
static void region_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    wsurf *s = calloc(1, sizeof *s);
    if (!s) { wl_client_post_no_memory(c); return; }
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    if (!s->res) { free(s); wl_client_post_no_memory(c); return; }
    wl_list_init(&s->pframes); wl_list_init(&s->frames);
    s->pbuf_gone.notify = surf_pbuf_gone;
    wl_resource_set_implementation(s->res, &surf_impl, s, surf_gone);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *g = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id);
    wregion *rg = calloc(1, sizeof *rg);
    if (!g || !rg) { free(rg); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(g, &region_impl, rg, region_gone);
}
static const struct wl_compositor_interface comp_impl = { .create_surface = comp_create_surface, .create_region = comp_create_region };
static void comp_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

/* ---- wl_subcompositor ---------------------------------------------------- */
static void sub_set_pos(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c;
    wsurf *s = wl_resource_get_user_data(r);
    if (!s) return;
    surf_repaint(s);
    s->sx = x; s->sy = y;
    surf_repaint(s);
}
static void sub_place(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib) { (void)c; (void)r; (void)sib; }
static void sub_sync(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static const struct wl_subsurface_interface sub_impl = {
    .destroy = res_destroy, .set_position = sub_set_pos, .place_above = sub_place, .place_below = sub_place,
    .set_sync = sub_sync, .set_desync = sub_sync,
};
static void sub_gone(struct wl_resource *r) {
    wsurf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->subres = NULL;
    if (s->parent) {
        wsurf *p = s->parent;
        for (int i = 0; i < p->nsubs; i++) if (p->subs[i] == s) { memmove(&p->subs[i], &p->subs[i + 1], (size_t)(p->nsubs - 1 - i) * sizeof *p->subs); p->nsubs--; break; }
        surf_repaint(p);
    }
    s->parent = NULL; s->role = 0;
}
static void subc_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *sr, struct wl_resource *pr) {
    wsurf *s = wl_resource_get_user_data(sr), *p = wl_resource_get_user_data(pr);
    struct wl_resource *res = wl_resource_create(c, &wl_subsurface_interface, wl_resource_get_version(r), id);
    if (!res) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &sub_impl, s, sub_gone);
    if (p->nsubs < 8) p->subs[p->nsubs++] = s;
    s->role = 3; s->parent = p; s->subres = res; s->sx = s->sy = 0;
}
static const struct wl_subcompositor_interface subc_impl = { .destroy = res_destroy, .get_subsurface = subc_get };
static void subc_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &subc_impl, NULL, NULL);
}

/* ======================================================================
 *  SEAT: MOUSE AND KEYBOARD
 * ====================================================================== */
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surf, int32_t hx, int32_t hy) {
    (void)r; (void)serial; (void)hx; (void)hy;
    if (surf) { wsurf *s = wl_resource_get_user_data(surf); if (s && !s->role) s->role = 4; }  /* ccos draws its own arrow */
    if (!surf) cursor_hide_client = c;                 /* "no pointer, please" */
    else if (cursor_hide_client == c) cursor_hide_client = NULL;
}
static void ptr_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface ptr_impl = { .set_cursor = ptr_set_cursor, .release = ptr_release };
static void kb_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface kb_impl = { .release = kb_release };

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &ptr_impl, NULL, res_unlink);
    wl_list_insert(&wl_pointers, wl_resource_get_link(p));
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &kb_impl, NULL, res_unlink);
    wl_list_insert(&wl_keyboards, wl_resource_get_link(k));
    if (kmap_fd >= 0) wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, kmap_fd, kmap_size);
    if (wl_resource_get_version(k) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) wl_keyboard_send_repeat_info(k, 30, 450);
    if (kb_focus && wl_resource_get_client(kb_focus->res) == c) {
        struct wl_array keys; wl_array_init(&keys);
        wl_keyboard_send_enter(k, wl_serial(), kb_focus->res, &keys);
        wl_keyboard_send_modifiers(k, wl_serial(), mods_dep, mods_lat, mods_lck, mods_grp);
        wl_array_release(&keys);
    }
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) { (void)c; (void)r; (void)id; }
static void seat_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer, .get_keyboard = seat_get_keyboard, .get_touch = seat_get_touch, .release = seat_release,
};
static void seat_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, (int)v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (v >= WL_SEAT_NAME_SINCE_VERSION) wl_seat_send_name(r, "seat0");
}

/* ---- wl_output: the screen ---------------------------------------------- */
static void out_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_output_interface out_impl = { .release = out_release };
static void out_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, (int)v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &out_impl, NULL, res_unlink);
    wl_list_insert(&wl_outputs, wl_resource_get_link(r));
    wl_output_send_geometry(r, 0, 0, W * 254 / 960, H * 254 / 960, WL_OUTPUT_SUBPIXEL_UNKNOWN, "ccos", "screen", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, W, H, 60000);
    if (v >= WL_OUTPUT_SCALE_SINCE_VERSION) wl_output_send_scale(r, 1);
    if (v >= WL_OUTPUT_NAME_SINCE_VERSION) wl_output_send_name(r, "SCREEN-1");
    if (v >= WL_OUTPUT_DONE_SINCE_VERSION) wl_output_send_done(r);
}

/* the screen changed size (a second screen, Windows + P): tell the apps */
static void wl_outputs_changed(void) {
    if (!wl_dpy) return;
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_outputs) {
        wl_output_send_geometry(r, 0, 0, W * 254 / 960, H * 254 / 960, WL_OUTPUT_SUBPIXEL_UNKNOWN, "ccos", "screen", WL_OUTPUT_TRANSFORM_NORMAL);
        wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, W, H, 60000);
        if (wl_resource_get_version(r) >= WL_OUTPUT_DONE_SINCE_VERSION) wl_output_send_done(r);
    }
}

/* ======================================================================
 *  XDG SHELL: APP WINDOWS
 * ====================================================================== */
/* each window slot gets its own little set of functions for the desktop */
static void wl_draw(int n, int cx, int cy, int cw, int ch);
static void wl_click(int n, int lx, int ly);
static void wl_release(int n);
static void wl_rclick(int n, int lx, int ly);
static void wl_wheel(int n, int amount);
static void wl_motion(int n, int lx, int ly, int inside);
static int  wl_close(int n);
static void wl_rawkey(int n, int code, int value);
static void wl_resized(int n);
#define WL_TRAMPOLINES(n) \
    static void wld##n(int a, int b, int c, int d) { wl_draw(n, a, b, c, d); } \
    static void wlc##n(int a, int b, int c, int d) { (void)c; (void)d; wl_click(n, a, b); } \
    static void wlr##n(void) { wl_release(n); } \
    static void wlrc##n(int a, int b, int c, int d) { (void)c; (void)d; wl_rclick(n, a, b); } \
    static void wlw##n(int a) { wl_wheel(n, a); } \
    static void wlm##n(int a, int b, int c) { wl_motion(n, a, b, c); } \
    static int  wlx##n(void) { return wl_close(n); } \
    static void wlk##n(int a, int b) { wl_rawkey(n, a, b); } \
    static void wlz##n(void) { wl_resized(n); }
WL_TRAMPOLINES(0) WL_TRAMPOLINES(1) WL_TRAMPOLINES(2) WL_TRAMPOLINES(3)
WL_TRAMPOLINES(4) WL_TRAMPOLINES(5) WL_TRAMPOLINES(6) WL_TRAMPOLINES(7)
#define WL_T(n) { .draw = wld##n, .click = wlc##n, .release = wlr##n, .rclick = wlrc##n, .wheel = wlw##n, \
                  .motion = wlm##n, .on_close = wlx##n, .rawkey = wlk##n, .resized = wlz##n }
static const Win wl_tmpl[WLS] = { WL_T(0), WL_T(1), WL_T(2), WL_T(3), WL_T(4), WL_T(5), WL_T(6), WL_T(7) };

/* which ccos app a Wayland app counts as (for pinning and the start menu) */
static const char *wl_app_cmd(const char *app_id) {
    if (!strcmp(app_id, "foot") || !strcmp(app_id, "footclient")) return "@terminal";
    const char *fp = fp_cmd_for(app_id);              /* a Flatpak app */
    return fp ? fp : "";
}

static void wl_open_window(wtop *t) {
    int n = (int)(t - tops);
    wsurf *s = t->s;
    int cw = t->has_geo ? t->gw : s->w, ch = t->has_geo ? t->gh : s->h;
    cw = clampi(cw, 60, W - 2 * FRAME); ch = clampi(ch, 30, H - TASKBAR_H - TITLE_H - FRAME);
    Win tmpl = wl_tmpl[n];
    if (!t->title[0] || (!strcmp(wl_app_cmd(t->app_id), "@terminal") && !strncmp(t->title, "foot", 4)))
        snprintf(t->title, sizeof t->title, "%s", !strcmp(wl_app_cmd(t->app_id), "@terminal") ? "Terminal" : (t->app_id[0] ? t->app_id : "App"));
    tmpl.title = t->title;
    {   const AppInfo *ai = app_info(wl_app_cmd(t->app_id));      /* Flathub apps: their own picture */
        tmpl.icon = !strcmp(wl_app_cmd(t->app_id), "@terminal") ? ICON_TERMINAL : ai ? ai->icon : ICON_APP; }
    snprintf(launch_cmd, sizeof launch_cmd, "%s", wl_app_cmd(t->app_id));
    t->win = open_win(&tmpl, cw, ch);
    launch_cmd[0] = 0;
    surf_enter_output(s, 0);
    if (t->win >= 0) surf_send_configure(t);          /* now "activated" */
}

/* the app drew a picture of a new size: make the ccos window fit it */
static void wl_window_resized_by_app(wtop *t) {
    wsurf *s = t->s;
    if (!s->px) return;
    int w = t->has_geo ? t->gw : s->w, h = t->has_geo ? t->gh : s->h;
    int cw, ch; wl_client_size(t->win, &cw, &ch);
    if (t->maxed || t->sized || (w == cw && h == ch)) return;
    Win *wn = &wins[t->win];
    int ox = wn->x, oy = wn->y, ow = wn->w, oh = wn->h;
    wn->w = clampi(w, 60, W - 2 * FRAME) + 2 * FRAME;
    wn->h = clampi(h, 30, H - TASKBAR_H - TITLE_H - FRAME) + TITLE_H + FRAME;
    repaint(ox - 2, oy - 1, ow + 7, oh + 7);
    repaint_window(t->win);
}

static void top_unmap(wtop *t) {
    if (t->win >= 0 && wins[t->win].open && wins[t->win].draw == wl_tmpl[t - tops].draw) {
        int w = t->win; t->win = -1;
        if (ptr_btn_slot == (int)(t - tops)) ptr_btn_slot = -1;
        close_win(w);
    }
    t->win = -1;
}

/* does window win's app picture hide all of (x0,y0)-(x1,y1) on screen? then nothing under it needs drawing */
static int wl_covers(int win, int x0, int y0, int x1, int y1) {
    int n = slot_of_win(win);
    if (n < 0 || !tops[n].s || !tops[n].s->px) return 0;
    wtop *t = &tops[n]; wsurf *s = t->s;
    int cx, cy, cw, ch; client_rect(&wins[win], &cx, &cy, &cw, &ch);
    if (x0 < cx || y0 < cy || x1 > cx + cw || y1 > cy + ch) return 0;
    int ox = cx - (t->has_geo ? t->gx : 0), oy = cy - (t->has_geo ? t->gy : 0);
    int sx0 = x0 - ox, sy0 = y0 - oy, sx1 = x1 - ox, sy1 = y1 - oy;      /* in the surface */
    if (sx0 < 0 || sy0 < 0 || sx1 > s->w || sy1 > s->h) return 0;
    if (!s->alpha) return 1;
    for (int i = 0; i < s->op_n; i++)
        if (sx0 >= s->op_r[i][0] && sy0 >= s->op_r[i][1] && sx1 <= s->op_r[i][0] + s->op_r[i][2] && sy1 <= s->op_r[i][1] + s->op_r[i][3]) return 1;
    return 0;
}

/* ---- the desktop's callbacks for a Wayland window ---- */
static void wl_draw(int n, int cx, int cy, int cw, int ch) {
    wtop *t = &tops[n];
    uint32_t bg = RGB(0xf0, 0xf0, 0xf0);
    if (t->used && t->s && t->s->px) bg = t->s->px[(size_t)t->s->h * t->s->w - 1] & 0xffffff;   /* the app's own background */
    int x0 = imax(clip_x0, cx), x1 = imin(clip_x1, cx + cw), y0 = imax(clip_y0, cy), y1 = imin(clip_y1, cy + ch);
    if (x0 >= x1 || y0 >= y1) return;
    wsurf *s = t->used ? t->s : NULL;
    int ox = cx - (t->has_geo ? t->gx : 0), oy = cy - (t->has_geo ? t->gy : 0);
    for (int y = y0; y < y1; y++) {
        uint32_t *d = &back[(size_t)y * BW];
        int sy = y - oy;
        /* the part of this row the picture covers */
        int ax = x0, bx = x1;
        if (s && s->px && sy >= 0 && sy < s->h) { ax = imax(x0, ox); bx = imin(x1, ox + s->w); if (ax > bx) ax = bx = x0; }
        else ax = bx = x0;
        for (int x = x0; x < ax; x++) d[x] = bg;
        for (int x = bx; x < x1; x++) d[x] = bg;
        if (ax >= bx) continue;
        const uint32_t *p = s->px + (size_t)sy * s->w - ox;
        if (!s->alpha) { memcpy(d + ax, p + ax, (size_t)(bx - ax) * 4); continue; }   /* opaque: one copy */
        for (int x = ax; x < bx; x++) {
            uint32_t c = p[x], a = c >> 24;
            if (a == 255) d[x] = c & 0xffffff;
            else if (a == 0 && !(c & 0xffffff)) continue;
            else {                                   /* rounded corners: show what's behind */
                uint32_t b = d[x];
                uint32_t r = ((c >> 16) & 255) + ((b >> 16) & 255) * (255 - a) / 255;
                uint32_t g = ((c >> 8) & 255) + ((b >> 8) & 255) * (255 - a) / 255;
                uint32_t bl = (c & 255) + (b & 255) * (255 - a) / 255;
                d[x] = (imin(r, 255) << 16) | (imin(g, 255) << 8) | imin(bl, 255);
            }
        }
    }
    if (!s) return;
    /* sub-surfaces (and theirs) on top, see-through where they are */
    wsurf *stack[16]; int sxs[16], sys[16], ns = 0;
    for (int i = 0; i < s->nsubs && ns < 16; i++) { stack[ns] = s->subs[i]; sxs[ns] = ox + s->subs[i]->sx; sys[ns] = oy + s->subs[i]->sy; ns++; }
    for (int k = 0; k < ns; k++) {
        wsurf *u = stack[k];
        for (int i = 0; i < u->nsubs && ns < 16; i++) { stack[ns] = u->subs[i]; sxs[ns] = sxs[k] + u->subs[i]->sx; sys[ns] = sys[k] + u->subs[i]->sy; ns++; }
        if (!u->px) continue;
        int ux0 = imax(x0, sxs[k]), ux1 = imin(x1, sxs[k] + u->w), uy0 = imax(y0, sys[k]), uy1 = imin(y1, sys[k] + u->h);
        for (int y = uy0; y < uy1; y++) {
            uint32_t *d = &back[(size_t)y * BW];
            const uint32_t *p = u->px + (size_t)(y - sys[k]) * u->w - sxs[k];
            for (int x = ux0; x < ux1; x++) {
                uint32_t c = p[x], a = u->alpha ? c >> 24 : 255;
                if (a == 255) d[x] = c & 0xffffff;
                else if (a) {      /* premultiplied colours */
                    uint32_t b = d[x];
                    uint32_t r = ((c >> 16) & 255) + ((b >> 16) & 255) * (255 - a) / 255;
                    uint32_t g = ((c >> 8) & 255) + ((b >> 8) & 255) * (255 - a) / 255;
                    uint32_t bl = (c & 255) + (b & 255) * (255 - a) / 255;
                    d[x] = (imin(r, 255) << 16) | (imin(g, 255) << 8) | imin(bl, 255);
                }
            }
        }
    }
}

/* which surface is under the mouse (a sub-surface or the window itself) */
static wsurf *wl_surface_at(wtop *t, int lx, int ly, int *sx, int *sy) {
    wsurf *s = t->s;
    int x = lx + (t->has_geo ? t->gx : 0), y = ly + (t->has_geo ? t->gy : 0);
    for (int i = s->nsubs - 1; i >= 0; i--) {
        wsurf *u = s->subs[i];
        if (u->px && surf_takes_input(u, x - u->sx, y - u->sy)) { *sx = x - u->sx; *sy = y - u->sy; return u; }
    }
    *sx = x; *sy = y;
    return s;
}

static void ptr_send_motion(wsurf *s, int sx, int sy) {
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_pointers)
        if (wl_resource_get_client(r) == ptr_client) {
            wl_pointer_send_motion(r, wl_now(), wl_fixed_from_int(sx), wl_fixed_from_int(sy));
            ptr_frame(r);
        }
    (void)s;
}

static void wl_motion(int n, int lx, int ly, int inside) {
    wtop *t = &tops[n];
    if (!t->used || !t->s) return;
    if (npop && popup_at(mx, my, &lx, &ly)) return;       /* over a menu: wl_popup_motion() has it */
    if (ptr_btn_slot == n) inside = 1;                 /* dragging: the window keeps the mouse */
    if (!inside) {
        if (ptr_focus && (ptr_focus == t->s || (ptr_focus->role == 3 && ptr_focus->parent == t->s))) ptr_set_focus(NULL, 0, 0);
        return;
    }
    int sx, sy; wsurf *s = wl_surface_at(t, lx, ly, &sx, &sy);
    if (ptr_btn_slot == n && ptr_focus) {              /* keep sending to where the press happened */
        int px, py, win = surf_screen_pos(ptr_focus, &px, &py);
        if (win >= 0) ptr_send_motion(ptr_focus, mx - px, my - py);
        return;
    }
    if (s != ptr_focus) ptr_set_focus(s, sx, sy);
    else ptr_send_motion(s, sx, sy);
}

static void ptr_button(uint32_t button, int down) {
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_pointers)
        if (wl_resource_get_client(r) == ptr_client) {
            wl_pointer_send_button(r, wl_serial(), wl_now(), button,
                                   down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            ptr_frame(r);
        }
}

static void wl_click(int n, int lx, int ly) {
    wl_motion(n, lx, ly, 1);
    if (!ptr_focus) return;
    ptr_btn_slot = n;
    ptr_button(0x110 /* BTN_LEFT */, 1);
}
static void wl_release(int n) {
    if (ptr_btn_slot != n) return;
    ptr_btn_slot = -1;
    if (ptr_focus) ptr_button(0x110, 0);
}
static int rbtn_down;
static void wl_rclick(int n, int lx, int ly) {
    wl_motion(n, lx, ly, 1);
    if (!ptr_focus) return;
    ptr_button(0x111 /* BTN_RIGHT */, 1);             /* released when the real button is */
    rbtn_down = 1;
}
/* the right button let go, and the middle / side buttons (games use them all) */
static void wl_raw_button(int code, int down) {
    if (!wl_dpy) return;
    if (code == 0x111) { if (!down && rbtn_down) { rbtn_down = 0; if (ptr_focus) ptr_button(0x111, 0); } return; }
    if (ptr_focus) ptr_button((uint32_t)code, down);
}
static void wl_wheel(int n, int amount) {
    (void)n;
    if (!ptr_focus) return;
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_pointers)
        if (wl_resource_get_client(r) == ptr_client) {
            int v = wl_resource_get_version(r);
            if (v >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) wl_pointer_send_axis_source(r, WL_POINTER_AXIS_SOURCE_WHEEL);
            if (v >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) wl_pointer_send_axis_discrete(r, WL_POINTER_AXIS_VERTICAL_SCROLL, -amount);
            wl_pointer_send_axis(r, wl_now(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-amount * 15));
            ptr_frame(r);
        }
}
static int wl_close(int n) {
    wtop *t = &tops[n];
    if (t->used && t->res) xdg_toplevel_send_close(t->res);   /* ask nicely; it closes itself */
    return 0;
}
static void wl_rawkey(int n, int code, int value) {
    (void)n;
    if (!kb_focus || value == 2) return;              /* the app repeats keys itself */
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_keyboards)
        if (wl_resource_get_client(r) == wl_resource_get_client(kb_focus->res))
            wl_keyboard_send_key(r, wl_serial(), wl_now(), (uint32_t)code,
                                 value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    xkb_state_update_key(xkb_st, (uint32_t)code + 8, value ? XKB_KEY_DOWN : XKB_KEY_UP);
    uint32_t d = xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_DEPRESSED);
    uint32_t la = xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LATCHED);
    uint32_t lo = xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LOCKED);
    uint32_t g = xkb_state_serialize_layout(xkb_st, XKB_STATE_LAYOUT_EFFECTIVE);
    if (d != mods_dep || la != mods_lat || lo != mods_lck || g != mods_grp) {
        mods_dep = d; mods_lat = la; mods_lck = lo; mods_grp = g;
        kb_send_mods(NULL);
    }
}
/* Caps Lock is kept by ccos (it lights the lamp); tell the apps */
static void wl_set_capslock(int on) {
    if (!xkb_st) return;
    int now = xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
    if (now == !!on) return;
    xkb_state_update_key(xkb_st, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
    xkb_state_update_key(xkb_st, KEY_CAPSLOCK + 8, XKB_KEY_UP);
    mods_lck = xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LOCKED);
    kb_send_mods(NULL);
}
static void wl_resized(int n) {               /* ccos maximized / restored / resized the window */
    wtop *t = &tops[n];
    if (!t->used || t->win < 0) return;
    t->maxed = wins[t->win].maximized;
    t->sized = 1;                              /* from now on the window decides the size */
    t->cfg_pending = 1;                        /* sent by wl_send_pending() */
}

/* tell apps their new window size; while the user drags an edge, at most ~4 times a second */
static void wl_send_pending(int dragging) {
    long long now = ui_ms();
    for (int i = 0; i < WLS; i++) {
        wtop *t = &tops[i];
        if (!t->used || !t->cfg_pending || !t->res || !t->s || t->win < 0) continue;
        if (dragging && now - t->cfg_ms < 250) continue;
        t->cfg_pending = 0; t->cfg_ms = now;
        surf_send_configure(t);
    }
}

/* ---- xdg_toplevel requests ---- */
static void top_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) { (void)c; (void)r; (void)p; }
static void top_set_title(struct wl_client *c, struct wl_resource *r, const char *title) {
    (void)c;
    wtop *t = wl_resource_get_user_data(r);
    if (!t) return;
    ssid_shown((const unsigned char *)title, (int)strlen(title), t->title, sizeof t->title);   /* UTF-8 -> our font */
    if (!strcmp(wl_app_cmd(t->app_id), "@terminal") && !strncmp(title, "foot", 4))
        snprintf(t->title, sizeof t->title, "Terminal");      /* nicer than "foot" */
    if (!strncmp(title, "Xwayland on", 11)) snprintf(t->title, sizeof t->title, "X11 apps");
    if (t->win >= 0) { wins[t->win].title = t->title; repaint_window(t->win); repaint_taskbar(); }
}
static void top_set_app_id(struct wl_client *c, struct wl_resource *r, const char *id) {
    (void)c;
    wtop *t = wl_resource_get_user_data(r);
    if (t) snprintf(t->app_id, sizeof t->app_id, "%s", id);
}
static void top_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
    (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}
static void top_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)seat; (void)serial;
    wtop *t = wl_resource_get_user_data(r);       /* the app's own title bar was dragged */
    if (!t || t->win < 0 || !mouse_down) return;
    drag_win = t->win; drag_dx = mx - wins[t->win].x; drag_dy = my - wins[t->win].y;
    ptr_btn_slot = -1;
}
static void top_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    (void)c; (void)seat; (void)serial;
    wtop *t = wl_resource_get_user_data(r);      /* the app's own window edge was dragged */
    if (!t || t->win < 0 || !mouse_down || !wl_resize_hook) return;
    int e = 0;                                   /* xdg edges -> ccos edges (left 1, right 2, top 4, bottom 8) */
    if (edges == 1 || edges == 5 || edges == 9) e |= 4;
    if (edges == 2 || edges == 6 || edges == 10) e |= 8;
    if (edges == 4 || edges == 5 || edges == 6) e |= 1;
    if (edges == 8 || edges == 9 || edges == 10) e |= 2;
    ptr_btn_slot = -1;
    if (e) wl_resize_hook(t->win, e);
}
static void top_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void top_set_max(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wtop *t = wl_resource_get_user_data(r);
    if (t && t->win >= 0 && !wins[t->win].maximized) toggle_max(t->win);
    else if (t) surf_send_configure(t);
}
static void top_unset_max(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wtop *t = wl_resource_get_user_data(r);
    if (t && t->win >= 0 && wins[t->win].maximized) toggle_max(t->win);
    else if (t) surf_send_configure(t);
}
static void top_set_full(struct wl_client *c, struct wl_resource *r, struct wl_resource *out) {
    (void)c; (void)out;                        /* a video's full-screen button, F11 in a browser... */
    wtop *t = wl_resource_get_user_data(r);
    if (t && t->win >= 0 && !wins[t->win].fullscreen) set_fullscreen(t->win, 1);
    else if (t) surf_send_configure(t);
}
static void top_unset_full(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wtop *t = wl_resource_get_user_data(r);
    if (t && t->win >= 0 && wins[t->win].fullscreen) set_fullscreen(t->win, 0);
    else if (t) surf_send_configure(t);
}
static void top_set_min(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wtop *t = wl_resource_get_user_data(r);
    if (t && t->win >= 0) minimize_win(t->win);
}
static const struct xdg_toplevel_interface top_impl = {
    .destroy = res_destroy, .set_parent = top_set_parent, .set_title = top_set_title, .set_app_id = top_set_app_id,
    .show_window_menu = top_menu, .move = top_move, .resize = top_resize, .set_max_size = top_size,
    .set_min_size = top_size, .set_maximized = top_set_max, .unset_maximized = top_unset_max,
    .set_fullscreen = top_set_full, .unset_fullscreen = top_unset_full, .set_minimized = top_set_min,
};
static void top_gone(struct wl_resource *r) {
    wtop *t = wl_resource_get_user_data(r);
    if (!t) return;
    top_unmap(t);
    if (t->s) { t->s->top = NULL; t->s->role = 0; }
    if (t->deco) wl_resource_set_user_data(t->deco, NULL);
    memset(t, 0, sizeof *t);
}

/* ---- xdg_popup: menus, drop-downs and tooltips, drawn above every window ---- */

/* screen position of a surface's window-geometry corner (toplevel: the content area) */
static int geom_origin(wsurf *s, int *x, int *y) {
    if (!s) return -1;
    if (s->role == 1 && s->top && s->top->win >= 0) {
        int cx, cy, cw, ch; client_rect(&wins[s->top->win], &cx, &cy, &cw, &ch);
        *x = cx; *y = cy; return 0;
    }
    if (s->role == 2 && s->pop && s->pop->parent) {
        int px, py;
        if (geom_origin(s->pop->parent, &px, &py) < 0) return -1;
        *x = px + s->pop->x; *y = py + s->pop->y; return 0;
    }
    return -1;
}

/* place a popup like the protocol says, then keep it on screen */
static void pop_place(wpop *p) {
    wpos *q = &p->pos;
    int px = 0, py = 0; geom_origin(p->parent, &px, &py);
    for (int pass = 0; pass < 2; pass++) {
        int an = q->anchor, gr = q->gravity;
        int ax = q->ax + ((an == 3 || an == 5 || an == 6) ? 0 : (an == 4 || an == 7 || an == 8) ? q->aw : q->aw / 2);
        int ay = q->ay + ((an == 1 || an == 5 || an == 7) ? 0 : (an == 2 || an == 6 || an == 8) ? q->ah : q->ah / 2);
        int x = (gr == 3 || gr == 5 || gr == 6) ? ax - q->w : (gr == 4 || gr == 7 || gr == 8) ? ax : ax - q->w / 2;
        int y = (gr == 1 || gr == 5 || gr == 7) ? ay - q->h : (gr == 2 || gr == 6 || gr == 8) ? ay : ay - q->h / 2;
        x += q->ox; y += q->oy;
        int sx = px + x, sy = py + y, redo = 0;
        int SL = px >= W && BW > W ? W : 0, SR = px >= W && BW > W ? BW : W;   /* the screen the window is on */
        /* off the screen? flip to the other side if allowed (once), otherwise slide */
        if (pass == 0 && (q->cadj & 4) && (sx + q->w > SR || sx < SL)) {
            static const int fh[9] = { 0, 1, 2, 4, 3, 7, 8, 5, 6 };
            q->anchor = fh[q->anchor % 9]; q->gravity = fh[q->gravity % 9]; redo = 1;
        }
        if (pass == 0 && (q->cadj & 8) && (sy + q->h > H || sy < 0)) {
            static const int fv[9] = { 0, 2, 1, 3, 4, 6, 5, 8, 7 };
            q->anchor = fv[q->anchor % 9]; q->gravity = fv[q->gravity % 9]; redo = 1;
        }
        if (redo) continue;
        if (sx + q->w > SR) sx = SR - q->w;
        if (sx < SL) sx = SL;
        if (sy + q->h > H) sy = H - q->h;
        if (sy < 0) sy = 0;
        p->x = sx - px; p->y = sy - py;
        return;
    }
    p->x = q->ax; p->y = q->ay + q->ah;
}

static void pop_screen_rect(wpop *p, int *x, int *y, int *w, int *h) {
    int gx, gy;
    if (!p->s || geom_origin(p->s, &gx, &gy) < 0) { *x = *y = *w = *h = 0; return; }
    *x = gx - (p->has_geo ? p->gx : 0); *y = gy - (p->has_geo ? p->gy : 0);
    *w = p->s->w; *h = p->s->h;
}
static void pop_repaint(wpop *p) {
    int x, y, w, h; pop_screen_rect(p, &x, &y, &w, &h);
    if (w > 0) wl_repaint_later(x - 1, y - 1, w + 2, h + 2);
}
static void pop_unstack(wpop *p) {
    int n = (int)(p - pops);
    for (int i = 0; i < npop; i++)
        if (popstack[i] == n) { memmove(&popstack[i], &popstack[i + 1], (size_t)(npop - 1 - i) * sizeof *popstack); npop--; break; }
}

/* close every open menu (a click somewhere else) */
static void wl_popups_dismiss(void) {
    for (int i = npop - 1; i >= 0; i--) {
        wpop *p = &pops[popstack[i]];
        if (p->used && p->res) xdg_popup_send_popup_done(p->res);
    }
}

/* draw a surface (and its sub-surfaces), see-through where the app says so */
static void draw_surface_at(wsurf *s, int sx, int sy, int depth) {
    if (!s || !s->px || depth > 4) return;
    int x0 = imax(clip_x0, sx), x1 = imin(clip_x1, sx + s->w), y0 = imax(clip_y0, sy), y1 = imin(clip_y1, sy + s->h);
    for (int y = y0; y < y1; y++) {
        uint32_t *d = &back[(size_t)y * BW];
        const uint32_t *pp = s->px + (size_t)(y - sy) * s->w - sx;
        for (int x = x0; x < x1; x++) {
            uint32_t c = pp[x], a = s->alpha ? c >> 24 : 255;
            if (a == 255) d[x] = c & 0xffffff;
            else if (a) {
                uint32_t b = d[x];
                uint32_t r = ((c >> 16) & 255) + ((b >> 16) & 255) * (255 - a) / 255;
                uint32_t g = ((c >> 8) & 255) + ((b >> 8) & 255) * (255 - a) / 255;
                uint32_t bl = (c & 255) + (b & 255) * (255 - a) / 255;
                d[x] = (imin(r, 255) << 16) | (imin(g, 255) << 8) | imin(bl, 255);
            }
        }
    }
    for (int i = 0; i < s->nsubs; i++) draw_surface_at(s->subs[i], sx + s->subs[i]->sx, sy + s->subs[i]->sy, depth + 1);
}

/* called by the desktop after it drew the windows */
static void wl_draw_popups(void) {
    for (int i = 0; i < npop; i++) {
        wpop *p = &pops[popstack[i]];
        if (!p->used || !p->mapped) continue;
        int x, y, w, h; pop_screen_rect(p, &x, &y, &w, &h);
        if (w > 0) draw_surface_at(p->s, x, y, 0);
    }
}

/* the newest popup under the mouse, or NULL; *lx,*ly = position on it */
static wsurf *popup_at(int x, int y, int *lx, int *ly) {
    for (int i = npop - 1; i >= 0; i--) {
        wpop *p = &pops[popstack[i]];
        if (!p->used || !p->mapped) continue;
        int px, py, pw, ph; pop_screen_rect(p, &px, &py, &pw, &ph);
        if (x >= px && y >= py && x < px + pw && y < py + ph) {
            wsurf *s = p->s;            /* a sub-surface of the popup? */
            for (int k = s->nsubs - 1; k >= 0; k--) {
                wsurf *u = s->subs[k];
                if (u->px && surf_takes_input(u, x - px - u->sx, y - py - u->sy)) {
                    *lx = x - px - u->sx; *ly = y - py - u->sy; return u;
                }
            }
            *lx = x - px; *ly = y - py; return s;
        }
    }
    return NULL;
}

static void ptr_send_motion(wsurf *s, int sx, int sy);
static void ptr_button(uint32_t button, int down);

/* mouse moved: if it's over a menu, the menu gets it (1 = handled) */
static int wl_popup_motion(void) {
    if (!npop) return 0;
    int lx, ly; wsurf *s = popup_at(mx, my, &lx, &ly);
    if (!s) return 0;
    if (s != ptr_focus) ptr_set_focus(s, lx, ly); else ptr_send_motion(s, lx, ly);
    return 1;
}
/* a mouse button while menus are open (1 = handled, don't pass it on) */
static int wl_popup_button(uint32_t button, int down) {
    if (!down) {
        if (!pop_btn) return 0;
        pop_btn = 0;
        if (ptr_focus) ptr_button(button, 0);
        return 1;
    }
    if (!npop) return 0;
    int lx, ly; wsurf *s = popup_at(mx, my, &lx, &ly);
    if (!s) { wl_popups_dismiss(); return 1; }            /* clicked elsewhere: close the menus */
    if (s != ptr_focus) ptr_set_focus(s, lx, ly);
    ptr_button(button, 1);
    pop_btn = 1;
    return 1;
}

static void popup_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)seat; (void)serial;
    wpop *p = wl_resource_get_user_data(r);
    if (p) p->grab = 1;
}
static void popup_send_cfg(wpop *p) {
    xdg_popup_send_configure(p->res, p->x, p->y, p->pos.w, p->pos.h);
    if (p->s && p->s->xdg) xdg_surface_send_configure(p->s->xdg, wl_serial());
}
static void popup_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *posr, uint32_t token) {
    (void)c;
    wpop *p = wl_resource_get_user_data(r);
    wpos *q = posr ? wl_resource_get_user_data(posr) : NULL;
    if (!p || !q) return;
    pop_repaint(p);
    p->pos = *q; pop_place(p);
    if (wl_resource_get_version(r) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION) xdg_popup_send_repositioned(r, token);
    popup_send_cfg(p);
}
static const struct xdg_popup_interface popup_impl = { .destroy = res_destroy, .grab = popup_grab, .reposition = popup_reposition };
static void popup_gone(struct wl_resource *r) {
    wpop *p = wl_resource_get_user_data(r);
    if (!p) return;
    pop_repaint(p);
    pop_unstack(p);
    if (p->s) { p->s->pop = NULL; p->s->role = 0; if (ptr_focus == p->s) { ptr_focus = NULL; ptr_client = NULL; } }
    memset(p, 0, sizeof *p);
}

/* ---- xdg_surface ---- */
static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    wsurf *s = wl_resource_get_user_data(r);
    int n;
    for (n = 0; n < WLS && tops[n].used; n++) {}
    struct wl_resource *tr = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(c); return; }
    if (n == WLS) {                                  /* too many windows: refuse politely */
        wl_resource_set_implementation(tr, &top_impl, NULL, NULL);
        xdg_toplevel_send_close(tr);
        return;
    }
    wtop *t = &tops[n];
    memset(t, 0, sizeof *t);
    t->used = 1; t->s = s; t->res = tr; t->client = c; t->win = -1;
    s->role = 1; s->top = t;
    wl_resource_set_implementation(tr, &top_impl, t, top_gone);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *posr) {
    wsurf *s = wl_resource_get_user_data(r);
    wsurf *ps = parent ? wl_resource_get_user_data(parent) : NULL;
    wpos *q = posr ? wl_resource_get_user_data(posr) : NULL;
    struct wl_resource *pr = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    int n;
    for (n = 0; n < WPOPS && pops[n].used; n++) {}
    if (n == WPOPS || !ps || !q) {                       /* no room or no parent: close it */
        wl_resource_set_implementation(pr, &popup_impl, NULL, NULL);
        xdg_popup_send_popup_done(pr);
        return;
    }
    wpop *p = &pops[n];
    memset(p, 0, sizeof *p);
    p->used = 1; p->s = s; p->parent = ps; p->res = pr; p->pos = *q;
    s->role = 2; s->pop = p;
    wl_resource_set_implementation(pr, &popup_impl, p, popup_gone);
    pop_place(p);
}
static void xs_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    wsurf *s = wl_resource_get_user_data(r);
    if (!s || w <= 0 || h <= 0) return;
    if (s->pop) { s->pop->gx = x; s->pop->gy = y; s->pop->has_geo = 1; return; }
    if (!s->top) return;
    wtop *t = s->top;
    t->gx = x; t->gy = y; t->gw = w; t->gh = h; t->has_geo = 1;
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c; (void)r; (void)serial; }
static const struct xdg_surface_interface xs_impl = {
    .destroy = res_destroy, .get_toplevel = xs_get_toplevel, .get_popup = xs_get_popup,
    .set_window_geometry = xs_geometry, .ack_configure = xs_ack,
};
static void xs_gone(struct wl_resource *r) {
    wsurf *s = wl_resource_get_user_data(r);
    if (s) s->xdg = NULL;
}

/* ---- xdg_positioner: where a popup wants to be ---- */
static void pos_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; wpos *q = wl_resource_get_user_data(r); q->w = w; q->h = h; }
static void pos_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; wpos *q = wl_resource_get_user_data(r); q->ax = x; q->ay = y; q->aw = w; q->ah = h;
}
static void pos_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; ((wpos *)wl_resource_get_user_data(r))->anchor = (int)a; }
static void pos_gravity(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; ((wpos *)wl_resource_get_user_data(r))->gravity = (int)a; }
static void pos_cadj(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; ((wpos *)wl_resource_get_user_data(r))->cadj = (int)a; }
static void pos_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c; wpos *q = wl_resource_get_user_data(r); q->ox = x; q->oy = y; }
static void pos_v(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void pos_psize(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b) { (void)c; (void)r; (void)a; (void)b; }
static void pos_parent_cfg(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; (void)r; (void)a; }
static const struct xdg_positioner_interface pos_impl = {
    .destroy = res_destroy, .set_size = pos_size, .set_anchor_rect = pos_rect, .set_anchor = pos_anchor,
    .set_gravity = pos_gravity, .set_constraint_adjustment = pos_cadj, .set_offset = pos_offset,
    .set_reactive = pos_v, .set_parent_size = pos_psize, .set_parent_configure = pos_parent_cfg,
};
static void pos_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

/* ---- xdg_wm_base ---- */
static void wm_create_pos(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    wpos *q = calloc(1, sizeof *q);
    if (!p || !q) { free(q); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &pos_impl, q, pos_gone);
}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf) {
    wsurf *s = wl_resource_get_user_data(surf);
    struct wl_resource *x = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    if (!x) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(x, &xs_impl, s, xs_gone);
    s->xdg = x;
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c; (void)r; (void)serial; }
static const struct xdg_wm_base_interface wm_impl = {
    .destroy = res_destroy, .create_positioner = wm_create_pos, .get_xdg_surface = wm_get_xdg_surface, .pong = wm_pong,
};
static void wm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
}

/* ---- xdg-decoration: ccos always draws the title bar ---- */
static void deco_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode) {
    (void)c; (void)mode;
    zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_unset(struct wl_client *c, struct wl_resource *r) { deco_mode(c, r, 0); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy = res_destroy, .set_mode = deco_mode, .unset_mode = deco_unset,
};
static void deco_gone(struct wl_resource *r) {
    wtop *t = wl_resource_get_user_data(r);
    if (t) t->deco = NULL;
}
static void decom_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *top) {
    wtop *t = wl_resource_get_user_data(top);
    struct wl_resource *d = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &deco_impl, t, deco_gone);
    if (t) t->deco = d;
    zxdg_toplevel_decoration_v1_send_configure(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_decoration_manager_v1_interface decom_impl = { .destroy = res_destroy, .get_toplevel_decoration = decom_get };
static void decom_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &decom_impl, NULL, NULL);
}


/* ======================================================================
 *  CLIPBOARD (wl_data_device_manager): copy in one app, paste in another
 * ====================================================================== */
static void ch_fetch(struct wl_resource *src, const char *mime, int png);
static void ch_put(int png, const char *data, size_t n);
typedef struct { struct wl_resource *res; char mime[16][64]; int n; } wsrc;
static wsrc *wl_sel;                          /* what was copied last */
static unsigned char *own_clip; static size_t own_clip_n; static int own_clip_on, own_clip_text;   /* ccos's own: a screenshot, or text copied in a ccos app */
static int text_mime(const char *m) { return !strcmp(m, "text/plain;charset=utf-8") || !strcmp(m, "text/plain") || !strcmp(m, "UTF8_STRING") || !strcmp(m, "TEXT") || !strcmp(m, "STRING"); }
static struct wl_list wl_devices;             /* wl_data_device resources */

static void src_offer(struct wl_client *c, struct wl_resource *r, const char *mime) {
    (void)c;
    wsrc *s = wl_resource_get_user_data(r);
    if (s->n < 16) snprintf(s->mime[s->n++], 64, "%s", mime);
}
static void src_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; (void)r; (void)a; }
static const struct wl_data_source_interface src_impl = { .offer = src_offer, .destroy = res_destroy, .set_actions = src_actions };
static void wl_send_selection(struct wl_client *to);
static void src_gone(struct wl_resource *r) {
    wsrc *s = wl_resource_get_user_data(r);
    if (wl_sel == s) { wl_sel = NULL; if (kb_focus) wl_send_selection(wl_resource_get_client(kb_focus->res)); }
    free(s);
}

static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime) { (void)c; (void)r; (void)serial; (void)mime; }
static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd) {
    (void)c; (void)r;
    if (wl_sel) wl_data_source_send_send(wl_sel->res, mime, fd);   /* the copying app writes it straight in */
    else if (own_clip_on && own_clip && (own_clip_text ? text_mime(mime) : !strcmp(mime, "image/png")) && fork() == 0) {   /* ours: written by a helper */
        size_t off = 0;
        while (off < own_clip_n) { ssize_t k = write(fd, own_clip + off, own_clip_n - off); if (k <= 0) break; off += (size_t)k; }
        _exit(0);
    }
    close(fd);
}
static void offer_finish(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void offer_actions(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t p) { (void)c; (void)r; (void)a; (void)p; }
static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept, .receive = offer_receive, .destroy = res_destroy, .finish = offer_finish, .set_actions = offer_actions,
};

static void wl_send_selection(struct wl_client *to) {
    struct wl_resource *d;
    wl_resource_for_each(d, &wl_devices) {
        if (wl_resource_get_client(d) != to) continue;
        if (!wl_sel && !own_clip_on) { wl_data_device_send_selection(d, NULL); continue; }
        struct wl_resource *o = wl_resource_create(to, &wl_data_offer_interface, wl_resource_get_version(d), 0);
        if (!o) continue;
        wl_resource_set_implementation(o, &offer_impl, NULL, NULL);
        wl_data_device_send_data_offer(d, o);
        if (wl_sel) for (int i = 0; i < wl_sel->n; i++) wl_data_offer_send_offer(o, wl_sel->mime[i]);
        else if (own_clip_text) {
            static const char *tm[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" };
            for (int i = 0; i < 5; i++) wl_data_offer_send_offer(o, tm[i]);
        } else wl_data_offer_send_offer(o, "image/png");
        wl_data_device_send_selection(d, o);
    }
}

/* ccos puts a picture on the clipboard (a screenshot) */
static void wl_clip_set_png(const unsigned char *png, size_t n) {
    unsigned char *c = malloc(n);
    if (!c) return;
    memcpy(c, png, n);
    free(own_clip); own_clip = c; own_clip_n = n; own_clip_on = 1; own_clip_text = 0;
    if (wl_sel) { wl_data_source_send_cancelled(wl_sel->res); wl_sel = NULL; }
    if (kb_focus) wl_send_selection(wl_resource_get_client(kb_focus->res));
    ch_put(1, (const char *)png, n);
}
/* text copied in a ccos app: Wayland apps can paste it too */
static void wl_clip_set_text(const char *s, size_t n) {
    unsigned char *c = malloc(n + 1);
    if (!c) return;
    memcpy(c, s, n); c[n] = 0;
    free(own_clip); own_clip = c; own_clip_n = n; own_clip_on = 1; own_clip_text = 1;
    if (wl_sel) { wl_data_source_send_cancelled(wl_sel->res); wl_sel = NULL; }
    if (kb_focus) wl_send_selection(wl_resource_get_client(kb_focus->res));
}
static void dev_start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, struct wl_resource *origin, struct wl_resource *icon, uint32_t serial) {
    (void)c; (void)r; (void)origin; (void)icon; (void)serial;
    if (src) wl_data_source_send_cancelled(src);        /* no drag and drop yet */
}
static void dev_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, uint32_t serial) {
    (void)c; (void)r; (void)serial;
    wsrc *s = src ? wl_resource_get_user_data(src) : NULL;
    if (wl_sel && wl_sel != s) wl_data_source_send_cancelled(wl_sel->res);
    wl_sel = s;
    own_clip_on = 0;
    if (kb_focus) wl_send_selection(wl_resource_get_client(kb_focus->res));
    if (s) {                                         /* keep a copy for the clipboard history (and ccos's apps) */
        static const char *tm[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING" };
        int done = 0;
        for (int k = 0; k < 5 && !done; k++) for (int i = 0; i < s->n; i++) if (!strcmp(s->mime[i], tm[k])) { ch_fetch(s->res, tm[k], 0); done = 1; break; }
        for (int i = 0; i < s->n && !done; i++) if (!strcmp(s->mime[i], "image/png")) { ch_fetch(s->res, "image/png", 1); done = 1; }
    }
}
static const struct wl_data_device_interface dev_impl = {
    .start_drag = dev_start_drag, .set_selection = dev_set_selection, .release = res_destroy,
};
static void ddm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    wsrc *s = calloc(1, sizeof *s);
    if (!s) { wl_client_post_no_memory(c); return; }
    s->res = wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
    if (!s->res) { free(s); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(s->res, &src_impl, s, src_gone);
}
static void ddm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &dev_impl, NULL, res_unlink);
    wl_list_insert(&wl_devices, wl_resource_get_link(d));
    if (kb_focus && wl_resource_get_client(kb_focus->res) == c) wl_send_selection(c);
}
static const struct wl_data_device_manager_interface ddm_impl = { .create_data_source = ddm_create_source, .get_data_device = ddm_get_device };
static void ddm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &ddm_impl, NULL, NULL);
}

/* ======================================================================
 *  START / RUN
 * ====================================================================== */
static int wl_keymap_setup(void) {
    if (!xkb_ctx) xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx) return -1;
    /* Polish programmer's layout: a US keyboard with AltGr for ą ę ś ł ż ź ć ń ó (Settings can pick plain US) */
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = set_kbd == KBD_PL ? "pl" : "us" };
    xkb_map = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!xkb_map) { names.layout = "us"; xkb_map = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS); }
    if (!xkb_map) return -1;
    xkb_st = xkb_state_new(xkb_map);
    char *str = xkb_keymap_get_as_string(xkb_map, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) return -1;
    kmap_size = (uint32_t)strlen(str) + 1;
    kmap_fd = memfd_create("ccos-keymap", MFD_CLOEXEC);
    if (kmap_fd < 0 || write(kmap_fd, str, kmap_size) != (ssize_t)kmap_size) { free(str); return -1; }
    free(str);
    return 0;
}

/* Type text (an emoji) into the focused app: give it, for a moment, a keyboard
   whose keys are exactly those characters, press them, then give its normal
   keyboard back. (The way tools like wtype do it.) Returns 0 if no app has focus. */
static int wl_type_text(const char *utf8) {
    if (!kb_focus || !xkb_ctx) return 0;
    uint32_t cps[16]; int n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && n < 16; ) {
        uint32_t c; int l;
        if (*p < 0x80) { c = *p; l = 1; } else if ((*p >> 5) == 6) { c = *p & 31; l = 2; } else if ((*p >> 4) == 14) { c = *p & 15; l = 3; } else { c = *p & 7; l = 4; }
        for (int k = 1; k < l; k++) { if (!p[k]) return 0; c = (c << 6) | (p[k] & 63); }
        p += l; cps[n++] = c;
    }
    if (!n) return 0;
    char *src = NULL; size_t sl = 0; FILE *f = open_memstream(&src, &sl); if (!f) return 0;
    fprintf(f, "xkb_keymap {\nxkb_keycodes \"ccos\" { minimum = 8; maximum = 255;");
    for (int i = 0; i < n; i++) fprintf(f, " <T%d> = %d;", i, 10 + i);       /* evdev keys 2.. */
    fprintf(f, " };\nxkb_types \"ccos\" { include \"complete\" };\nxkb_compatibility \"ccos\" { include \"complete\" };\nxkb_symbols \"ccos\" {");
    for (int i = 0; i < n; i++) fprintf(f, " key <T%d> { [ U%04X ] };", i, cps[i]);
    fprintf(f, " };\n};\n");
    fclose(f);
    struct xkb_keymap *km = xkb_keymap_new_from_string(xkb_ctx, src, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    free(src);
    if (!km) return 0;
    char *full = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(km);
    if (!full) return 0;
    uint32_t sz = (uint32_t)strlen(full) + 1;
    int fd = memfd_create("ccos-type", MFD_CLOEXEC);
    if (fd < 0 || write(fd, full, sz) != (ssize_t)sz) { free(full); if (fd >= 0) close(fd); return 0; }
    free(full);
    struct wl_client *c = wl_resource_get_client(kb_focus->res);
    struct wl_resource *r;
    wl_resource_for_each(r, &wl_keyboards) {
        if (wl_resource_get_client(r) != c) continue;
        wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, sz);
        wl_keyboard_send_modifiers(r, wl_serial(), 0, 0, 0, 0);
        for (int i = 0; i < n; i++) {
            wl_keyboard_send_key(r, wl_serial(), wl_now(), (uint32_t)(2 + i), WL_KEYBOARD_KEY_STATE_PRESSED);
            wl_keyboard_send_key(r, wl_serial(), wl_now(), (uint32_t)(2 + i), WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, kmap_fd, kmap_size);   /* the real keyboard again */
        wl_keyboard_send_modifiers(r, wl_serial(), mods_dep, mods_lat, mods_lck, mods_grp);
    }
    close(fd);
    if (wl_dpy) wl_display_flush_clients(wl_dpy);
    return 1;
}

/* the keyboard layout changed in Settings: tell every app */
static void wl_keymap_changed(void) {
    if (!xkb_ctx) return;
    if (xkb_st) { xkb_state_unref(xkb_st); xkb_st = NULL; }
    if (xkb_map) { xkb_keymap_unref(xkb_map); xkb_map = NULL; }
    if (kmap_fd >= 0) { close(kmap_fd); kmap_fd = -1; }
    if (wl_keymap_setup() < 0) return;
    struct wl_resource *k;
    wl_resource_for_each(k, &wl_keyboards) wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, kmap_fd, kmap_size);
}

/* ======================================================================
 *  GAMES: RELATIVE MOUSE MOVEMENT AND POINTER LOCK
 *  A game that turns the camera with the mouse locks the pointer: the
 *  arrow stays put (hidden) and the game gets how far the mouse moved.
 * ====================================================================== */
static void relm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *ptr) {
    (void)ptr;
    static const struct zwp_relative_pointer_v1_interface relp_impl = { .destroy = res_destroy };
    struct wl_resource *p = wl_resource_create(c, &zwp_relative_pointer_v1_interface, wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &relp_impl, NULL, res_unlink);
    wl_list_insert(&wl_relptrs, wl_resource_get_link(p));
}
static const struct zwp_relative_pointer_manager_v1_interface relm_impl = { .destroy = res_destroy, .get_relative_pointer = relm_get };
static void relm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &zwp_relative_pointer_manager_v1_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &relm_impl, NULL, NULL);
}

typedef struct {
    int used, lock, persistent, active, defunct;
    struct wl_resource *res;
    wsurf *s;
} wcons;
#define WCONS 8
static wcons cons[WCONS];
static void cons_res_gone(struct wl_resource *r) {
    wcons *k = wl_resource_get_user_data(r);
    if (k) memset(k, 0, sizeof *k);
}
static void cons_surface_gone(wsurf *s) {
    for (int i = 0; i < WCONS; i++) if (cons[i].used && cons[i].s == s) { cons[i].s = NULL; cons[i].defunct = 1; cons[i].active = 0; }
}
static void cons_noop_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) { (void)c; (void)r; (void)reg; }
static void cons_hint(struct wl_client *c, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y) { (void)c; (void)r; (void)x; (void)y; }
static const struct zwp_locked_pointer_v1_interface locked_impl = { .destroy = res_destroy, .set_cursor_position_hint = cons_hint, .set_region = cons_noop_region };
static const struct zwp_confined_pointer_v1_interface confined_impl = { .destroy = res_destroy, .set_region = cons_noop_region };
static void cons_new(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf, uint32_t lifetime, int lock) {
    wsurf *s = wl_resource_get_user_data(surf);
    for (int i = 0; i < WCONS; i++)
        if (cons[i].used && cons[i].s == s && !cons[i].defunct) {
            wl_resource_post_error(r, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED, "already constrained");
            return;
        }
    int k = -1;
    for (int i = 0; i < WCONS; i++) if (!cons[i].used) { k = i; break; }
    if (k < 0) { wl_client_post_no_memory(c); return; }
    struct wl_resource *res = wl_resource_create(c, lock ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface,
                                                 wl_resource_get_version(r), id);
    if (!res) { wl_client_post_no_memory(c); return; }
    cons[k] = (wcons){ 1, lock, lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT, 0, 0, res, s };
    if (lock) wl_resource_set_implementation(res, &locked_impl, &cons[k], cons_res_gone);
    else wl_resource_set_implementation(res, &confined_impl, &cons[k], cons_res_gone);
}
static void pc_lock(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf,
                    struct wl_resource *ptr, struct wl_resource *reg, uint32_t lifetime) {
    (void)ptr; (void)reg; cons_new(c, r, id, surf, lifetime, 1);
}
static void pc_confine(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf,
                       struct wl_resource *ptr, struct wl_resource *reg, uint32_t lifetime) {
    (void)ptr; (void)reg; cons_new(c, r, id, surf, lifetime, 0);
}
static const struct zwp_pointer_constraints_v1_interface pc_impl = { .destroy = res_destroy, .lock_pointer = pc_lock, .confine_pointer = pc_confine };
static void pc_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &zwp_pointer_constraints_v1_interface, (int)v, id);
    if (r) wl_resource_set_implementation(r, &pc_impl, NULL, NULL);
}
/* a lock/confine is on while its window is the active one and the mouse is over it */
static void cons_update(void) {
    int a = active_win();
    for (int i = 0; i < WCONS; i++) {
        wcons *k = &cons[i];
        if (!k->used || k->defunct || !k->s) continue;
        int x, y, win = surf_screen_pos(k->s, &x, &y), want = 0;
        if (win >= 0 && win == a && !menu_open && !pop.open) {
            int cx, cy, cw, ch; client_rect(&wins[win], &cx, &cy, &cw, &ch);
            want = k->active || (mx >= cx && my >= cy && mx < cx + cw && my < cy + ch);
        }
        if (want == k->active) continue;
        k->active = want;
        if (k->lock) { if (want) zwp_locked_pointer_v1_send_locked(k->res); else zwp_locked_pointer_v1_send_unlocked(k->res); }
        else { if (want) zwp_confined_pointer_v1_send_confined(k->res); else zwp_confined_pointer_v1_send_unconfined(k->res); }
        if (!want && !k->persistent) k->defunct = 1;
    }
}
/* for the desktop: is the mouse locked (it must not move), or confined to a rectangle? */
static int wl_pointer_locked(void) {
    for (int i = 0; i < WCONS; i++) if (cons[i].used && cons[i].active && cons[i].lock) return 1;
    return 0;
}
static int wl_pointer_confine(int *x0, int *y0, int *x1, int *y1) {
    for (int i = 0; i < WCONS; i++)
        if (cons[i].used && cons[i].active && !cons[i].lock && cons[i].s) {
            int x, y, win = surf_screen_pos(cons[i].s, &x, &y);
            if (win < 0) continue;
            int cx, cy, cw, ch; client_rect(&wins[win], &cx, &cy, &cw, &ch);
            *x0 = cx; *y0 = cy; *x1 = cx + cw - 1; *y1 = cy + ch - 1;
            return 1;
        }
    return 0;
}
/* the mouse moved by (dx, dy), before ccos keeps it on the screen */
static void wl_rel_motion(int dx, int dy) {
    if (!wl_dpy || (!dx && !dy)) return;
    struct wl_client *to = ptr_client;
    for (int i = 0; i < WCONS; i++)
        if (cons[i].used && cons[i].active && cons[i].lock && cons[i].s) to = wl_resource_get_client(cons[i].s->res);
    if (!to) return;
    uint64_t us = (uint64_t)ui_ms() * 1000;
    struct wl_resource *r; int sent = 0;
    wl_resource_for_each(r, &wl_relptrs)
        if (wl_resource_get_client(r) == to) {
            zwp_relative_pointer_v1_send_relative_motion(r, (uint32_t)(us >> 32), (uint32_t)us,
                wl_fixed_from_int(dx), wl_fixed_from_int(dy), wl_fixed_from_int(dx), wl_fixed_from_int(dy));
            sent = 1;
        }
    if (sent) wl_resource_for_each(r, &wl_pointers) if (wl_resource_get_client(r) == to) ptr_frame(r);
}
/* the app over which the mouse is asked for no pointer picture (or locked it) */
static int wl_cursor_hidden(void) {
    if (wl_pointer_locked()) return 1;
    return ptr_focus && ptr_client && ptr_client == cursor_hide_client;
}

/* ======================================================================
 *  X11 APPS (Xwayland)
 *  Older apps and many games only speak X11. ccos listens on X display :0
 *  and starts Xwayland the moment such an app connects; their windows show
 *  up inside one "X11 apps" window. Xwayland quits when the last one closes.
 * ====================================================================== */
static int x_listen = -1;
static pid_t x_pid;
static void xw_setup(void) {
    if (access("/usr/bin/Xwayland", X_OK) != 0) return;
    mkdir("/tmp/.X11-unix", 01777); chmod("/tmp/.X11-unix", 01777);
    unlink("/tmp/.X11-unix/X0"); unlink("/tmp/.X0-lock");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "/tmp/.X11-unix/X0");
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) { close(fd); return; }
    chmod(a.sun_path, 0777);
    x_listen = fd;
    setenv("DISPLAY", ":0", 1);
}
static int xw_poll_fd(void) {                  /* watch for X11 apps only while Xwayland isn't running */
    if (x_pid > 0 && kill(x_pid, 0) != 0) x_pid = 0;            /* it quit */
    return x_pid > 0 ? -1 : x_listen;
}
static void xw_start(void) {
    if (x_listen < 0 || x_pid > 0) return;
    int cw = W - 2 * FRAME, ch = H - TASKBAR_H - TITLE_H - FRAME;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int lf = open("/tmp/xwayland.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
        int fd = dup(x_listen);                   /* without close-on-exec */
        for (int k = 3; k < 256; k++) if (k != fd) close(k);
        char sfd[16], geo[32];
        snprintf(sfd, sizeof sfd, "%d", fd);
        snprintf(geo, sizeof geo, "%dx%d", cw, ch);
        execl("/usr/bin/Xwayland", "Xwayland", ":0", "-listenfd", sfd, "-geometry", geo,
              "-terminate", "-nolisten", "tcp", "-shm", (char *)NULL);
        _exit(127);
    }
    if (p > 0) x_pid = p;
    if (p > 0 && access("/usr/bin/ccos-xwm", X_OK) == 0 && fork() == 0) {   /* and our little window manager */
        setsid();
        for (int k = 3; k < 256; k++) close(k);
        int lf = open("/tmp/xwayland.log", O_WRONLY | O_APPEND);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
        setenv("DISPLAY", ":0", 1);
        usleep(300000);
        execl("/usr/bin/ccos-xwm", "ccos-xwm", (char *)NULL);
        _exit(127);
    }
}
static int wl_is_xwayland(int win) {
    int n = slot_of_win(win);
    return n >= 0 && !strcmp(tops[n].app_id, "org.freedesktop.Xwayland");
}

#include "dmabuf.c"                 /* GPU-drawn pictures from apps */

/* what every app (and the helpers D-Bus starts for them) sees */
static void app_env(void) {
    mkdir("/data/home", 0755); mkdir("/root", 0700);
    /* home folder: the apps disk (lots of room), else the save disk, else memory */
    const char *home = access("/apps/home", W_OK) == 0 ? "/apps/home" : access("/data/home", W_OK) == 0 ? "/data/home" : "/root";
    setenv("HOME", home, 1);
    if (chdir(home) != 0) { /* stay where we are */ }
    setenv("SHELL", "/bin/sh", 1);
    setenv("TERM", "foot", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "ccos", 1);
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("QT_QPA_PLATFORM", "wayland", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("GSETTINGS_BACKEND", "memory", 1);
    setenv("GTK_A11Y", "none", 1);
    setenv("NO_AT_BRIDGE", "1", 1);
    setenv("XDG_DATA_DIRS", "/var/lib/flatpak/exports/share:/usr/local/share:/usr/share", 1);
    {   /* the folders in file dialogs' sidebar (GTK bookmarks) */
        char d[256]; snprintf(d, sizeof d, "%s/.config/gtk-3.0", home);
        char c[256]; snprintf(c, sizeof c, "%s/.config", home); mkdir(c, 0755); mkdir(d, 0755);
        snprintf(d, sizeof d, "%s/.config/gtk-3.0/bookmarks", home);
        FILE *f = fopen(d, "w");
        if (f) {
            fputs("file:///data/Documents Documents\nfile:///data/Images Images\n", f);
            fprintf(f, "file://%s Downloads\n", dl_dir());
            fputs("file:///data/Videos Videos\nfile:///media USB drives\n", f);
            fclose(f);
        }
        mkdir("/media", 0755);
        /* where apps save downloads (Firefox and others ask for the "Downloads" folder) */
        mkdir(dl_dir(), 0755);
        snprintf(d, sizeof d, "%s/.config/user-dirs.dirs", home);
        f = fopen(d, "w");
        if (f) {
            fprintf(f, "XDG_DOWNLOAD_DIR=\"%s\"\nXDG_DOCUMENTS_DIR=\"/data/Documents\"\n"
                       "XDG_PICTURES_DIR=\"/data/Images\"\nXDG_VIDEOS_DIR=\"/data/Videos\"\n"
                       "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\n", dl_dir());
            fclose(f);
        }
        setenv("XDG_DOWNLOAD_DIR", dl_dir(), 1);
    }
    /* links open in Firefox (once it's installed) */
    char p[256]; snprintf(p, sizeof p, "%s/.config", home); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.config/mimeapps.list", home);
    if (access(p, F_OK) != 0) {
        FILE *f = fopen(p, "w");
        if (f) {
            fputs("[Default Applications]\n", f);
            static const char *types[] = { "x-scheme-handler/http", "x-scheme-handler/https", "text/html",
                                           "application/xhtml+xml", "application/pdf", NULL };
            for (int i = 0; types[i]; i++) fprintf(f, "%s=org.mozilla.firefox.desktop\n", types[i]);
            fclose(f);
        }
    }
}

/* start the Wayland server; returns its fd for the desktop's main loop, or -1 */
static int wl_start(void) {
    mkdir("/run/user", 0755);
    mkdir("/run/user/0", 0700);
    setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
    wl_dpy = wl_display_create();
    if (!wl_dpy) return -1;
    const char *sock = wl_display_add_socket_auto(wl_dpy);
    if (!sock) { wl_display_destroy(wl_dpy); wl_dpy = NULL; return -1; }
    setenv("WAYLAND_DISPLAY", sock, 1);
    wl_list_init(&wl_pointers); wl_list_init(&wl_keyboards); wl_list_init(&wl_devices); wl_list_init(&wl_outputs);
    wl_display_init_shm(wl_dpy);
    wl_global_create(wl_dpy, &wl_compositor_interface, 4, NULL, comp_bind);
    wl_global_create(wl_dpy, &wl_subcompositor_interface, 1, NULL, subc_bind);
    wl_global_create(wl_dpy, &wl_seat_interface, 5, NULL, seat_bind);
    wl_global_create(wl_dpy, &wl_output_interface, 4, NULL, out_bind);
    wl_global_create(wl_dpy, &xdg_wm_base_interface, 3, NULL, wm_bind);
    wl_global_create(wl_dpy, &zxdg_decoration_manager_v1_interface, 1, NULL, decom_bind);
    wl_global_create(wl_dpy, &wl_data_device_manager_interface, 3, NULL, ddm_bind);
    wl_list_init(&wl_relptrs);
    wl_global_create(wl_dpy, &zwp_relative_pointer_manager_v1_interface, 1, NULL, relm_bind);
    wl_global_create(wl_dpy, &zwp_pointer_constraints_v1_interface, 1, NULL, pc_bind);
    dmabuf_enable(set_gpu);
    xw_setup();
    if (wl_keymap_setup() < 0) fprintf(stderr, "ccos: no keyboard map for Wayland apps\n");
    wl_loop = wl_display_get_event_loop(wl_dpy);
    /* a session message bus: many apps (and Flatpak) talk to each other over D-Bus */
    if (access("/usr/bin/dbus-daemon", X_OK) == 0) {
        setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/0/bus", 1);
        if (fork() == 0) {
            setsid();
            for (int fd = 3; fd < 256; fd++) close(fd);
            int lf = open("/tmp/dbus-session.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
            app_env();
            setenv("DISPLAY", ":0", 1);
            execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile",
                  "--address=unix:path=/run/user/0/bus", (char *)NULL);
            _exit(127);
        }
    }
    return wl_event_loop_get_fd(wl_loop);
}

static void wl_dispatch(void) {
    if (!wl_dpy) return;
    wl_event_loop_dispatch(wl_loop, 0);
    wl_repaint_now();                          /* draw what the apps just sent, once */
}
static void wl_flush(void) {
    if (wl_dpy) wl_display_flush_clients(wl_dpy);
}

/* start a Wayland app (e.g. "foot") */
static void wl_spawn(const char *prog, char *const argv[]) {
    if (!wl_dpy) return;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        int lf = open("/tmp/wayland-apps.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
        app_env();
        if (x_listen >= 0) setenv("DISPLAY", ":0", 1); else unsetenv("DISPLAY");
        execvp(prog, argv);
        _exit(127);
    }
}
