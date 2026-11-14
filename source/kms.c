/* ===================================================================
 *  Screens: the laptop's own screen and a second one (HDMI, DisplayPort,
 *  USB-C). ccos draws its picture (the "canvas", BW x H pixels) and this
 *  part copies it to each screen through the kernel's mode setting (KMS).
 *    PC screen only    the laptop screen
 *    Duplicate         the same picture on both
 *    Extend            the second screen continues to the right
 *    Second screen     only the second one
 *  Windows + P picks one; ccos remembers it (display= in ccos.conf).
 *  Without KMS (old computers) it falls back to /dev/fb0, one screen.
 * =================================================================== */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/netlink.h>
#include <sys/socket.h>
enum { DM_MAIN, DM_DUP, DM_EXTEND, DM_SECOND, DM_N };
static const char *dm_names[DM_N] = { "PC screen only", "Duplicate", "Extend", "Second screen only" };
typedef struct {
    int used, connected, internal; uint32_t conn, crtc; char name[24];
    struct drm_mode_modeinfo modes[64]; int nmodes, pick;   /* pick: the mode in use */
    uint32_t fb, handle, pitch; uint64_t size; uint32_t *map; int mw, mh;   /* its picture buffer */
    int on;                                  /* showing something */
    int sx, sy, sw, sh;                      /* the part of the canvas it shows */
    int dx, dy, dw, dh;                      /* ... and where on it (scaled if dw != sw) */
} KOut;
#define KMAX 4
static KOut kout[KMAX]; static int nkout;
static int kms_fd = -1, kms_on;              /* KMS works: we draw through it (else /dev/fb0) */
static int kms_main = -1, kms_second = -1;   /* which output is which */
static int kms_crtcs[16], nkcrtc;
static int kms_ev = -1;                      /* hotplug messages from the kernel */
static int kms_cur = DM_MAIN;                /* what's showing now */

static int kms_ioctl(unsigned long r, void *a) { int e; do e = ioctl(kms_fd, r, a); while (e < 0 && (errno == EINTR || errno == EAGAIN)); return e; }
static void kms_probe(void) {                /* which screens are connected, and their modes */
    struct drm_mode_card_res res = { 0 };
    if (kms_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res)) return;
    uint32_t conns[16], crtcs[16], encs[16], fbs[16];
    res.count_connectors = imin(res.count_connectors, 16); res.count_crtcs = imin(res.count_crtcs, 16);
    res.count_encoders = imin(res.count_encoders, 16); res.count_fbs = imin(res.count_fbs, 16);
    res.connector_id_ptr = (uintptr_t)conns; res.crtc_id_ptr = (uintptr_t)crtcs; res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
    if (kms_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res)) return;
    nkcrtc = imin((int)res.count_crtcs, 16);
    for (int i = 0; i < nkcrtc; i++) kms_crtcs[i] = (int)crtcs[i];
    static const char *tn[] = { "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB" };
    int n = imin((int)res.count_connectors, KMAX);
    for (int i = 0; i < n; i++) {
        KOut *o = &kout[i];
        struct drm_mode_get_connector c = { .connector_id = conns[i] };
        if (kms_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;      /* (count_modes 0: asks the screen) */
        struct drm_mode_modeinfo modes[64]; uint32_t ce[8]; uint32_t props[64]; uint64_t pv[64];
        c.count_modes = imin(c.count_modes, 64); c.count_encoders = imin(c.count_encoders, 8); c.count_props = imin(c.count_props, 64);
        c.modes_ptr = (uintptr_t)modes; c.encoders_ptr = (uintptr_t)ce; c.props_ptr = (uintptr_t)props; c.prop_values_ptr = (uintptr_t)pv;
        if (kms_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        o->used = 1; o->conn = conns[i];
        o->connected = c.connection == 1 && c.count_modes > 0;
        o->internal = c.connector_type == 7 || c.connector_type == 14 || c.connector_type == 16;
        snprintf(o->name, sizeof o->name, "%s-%u", c.connector_type < sizeof tn / sizeof tn[0] ? tn[c.connector_type] : "Out", c.connector_type_id);
        o->nmodes = (int)c.count_modes; memcpy(o->modes, modes, sizeof modes[0] * (size_t)o->nmodes);
        o->pick = 0;
        for (int k = 0; k < o->nmodes; k++) if (o->modes[k].type & DRM_MODE_TYPE_PREFERRED) { o->pick = k; break; }
        /* a CRTC for it: the one it uses now, or a free one it can use */
        if (!o->crtc) {
            for (int e = 0; e < (int)c.count_encoders && !o->crtc; e++) {
                struct drm_mode_get_encoder en = { .encoder_id = ce[e] };
                if (kms_ioctl(DRM_IOCTL_MODE_GETENCODER, &en)) continue;
                for (int k = 0; k < nkcrtc && !o->crtc; k++) {
                    if (!(en.possible_crtcs & (1u << k))) continue;
                    int taken = 0; for (int j = 0; j < KMAX; j++) if (j != i && kout[j].crtc == crtcs[k]) taken = 1;
                    if (!taken && (!en.crtc_id || en.crtc_id == crtcs[k] || 1)) o->crtc = crtcs[k];
                }
            }
        }
    }
    nkout = n;
    /* the main screen: the built-in one (or the first connected); the second: another connected one */
    kms_main = kms_second = -1;
    for (int i = 0; i < nkout; i++) if (kout[i].connected && kout[i].internal && kms_main < 0) kms_main = i;
    for (int i = 0; i < nkout; i++) if (kout[i].connected && kms_main < 0) kms_main = i;
    for (int i = 0; i < nkout; i++) if (kout[i].connected && i != kms_main && kout[i].crtc && kout[i].crtc != (kms_main >= 0 ? kout[kms_main].crtc : 0)) { kms_second = i; break; }
}
static void kms_free_fb(KOut *o) {
    if (o->map) { munmap(o->map, o->size); o->map = NULL; }
    if (o->fb) { kms_ioctl(DRM_IOCTL_MODE_RMFB, &o->fb); o->fb = 0; }
    if (o->handle) { struct drm_mode_destroy_dumb d = { .handle = o->handle }; kms_ioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &d); o->handle = 0; }
}
static int kms_make_fb(KOut *o) {            /* a picture buffer the size of its mode */
    const struct drm_mode_modeinfo *m = &o->modes[o->pick];
    if (o->map && o->mw == m->hdisplay && o->mh == m->vdisplay) return 1;
    kms_free_fb(o);
    struct drm_mode_create_dumb cd = { .width = m->hdisplay, .height = m->vdisplay, .bpp = 32 };
    if (kms_ioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return 0;
    o->handle = cd.handle; o->pitch = cd.pitch; o->size = cd.size;
    struct drm_mode_fb_cmd fc = { .width = m->hdisplay, .height = m->vdisplay, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle };
    if (kms_ioctl(DRM_IOCTL_MODE_ADDFB, &fc)) { kms_free_fb(o); return 0; }
    o->fb = fc.fb_id;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (kms_ioctl(DRM_IOCTL_MODE_MAP_DUMB, &md)) { kms_free_fb(o); return 0; }
    void *p = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms_fd, md.offset);
    if (p == MAP_FAILED) { kms_free_fb(o); return 0; }
    o->map = p; o->mw = m->hdisplay; o->mh = m->vdisplay;
    memset(o->map, 0, o->size);
    return 1;
}
static int kms_show(KOut *o) {               /* light it up with its buffer */
    struct drm_mode_crtc c = { .crtc_id = o->crtc, .fb_id = o->fb, .set_connectors_ptr = (uintptr_t)&o->conn, .count_connectors = 1, .mode_valid = 1 };
    c.mode = o->modes[o->pick];
    return kms_ioctl(DRM_IOCTL_MODE_SETCRTC, &c) == 0;
}
static void kms_off(KOut *o) {
    if (!o->crtc) return;
    struct drm_mode_crtc c = { .crtc_id = o->crtc };
    kms_ioctl(DRM_IOCTL_MODE_SETCRTC, &c);
    o->on = 0;
}
/* the mode of the second screen: the same size as the main one if it can (Duplicate looks sharp,
   Extend lines up), else its own best */
static void kms_pick_second(int same_size) {
    if (kms_second < 0 || kms_main < 0) return;
    KOut *s = &kout[kms_second]; const struct drm_mode_modeinfo *mm = &kout[kms_main].modes[kout[kms_main].pick];
    int pref = 0; for (int k = 0; k < s->nmodes; k++) if (s->modes[k].type & DRM_MODE_TYPE_PREFERRED) { pref = k; break; }
    s->pick = pref;
    if (same_size) { for (int k = 0; k < s->nmodes; k++) if (s->modes[k].hdisplay == mm->hdisplay && s->modes[k].vdisplay == mm->vdisplay) { s->pick = k; return; } }
    else if (s->modes[pref].vdisplay != mm->vdisplay)          /* Extend: the same height keeps the screens lined up */
        for (int k = 0; k < s->nmodes; k++) if (s->modes[k].vdisplay == mm->vdisplay && s->modes[k].hdisplay <= 3840) { s->pick = k; return; }
}

/* ---- copying the picture to the screens ---- */
static inline uint32_t *kpx(KOut *o, int x, int y) { return (uint32_t *)((char *)o->map + (size_t)y * o->pitch) + x; }
static void kms_dirty(KOut *o, int x0, int y0, int x1, int y1) {
    struct drm_clip_rect r = { (unsigned short)x0, (unsigned short)y0, (unsigned short)x1, (unsigned short)y1 };
    struct drm_mode_fb_dirty_cmd d = { .fb_id = o->fb, .num_clips = 1, .clips_ptr = (uintptr_t)&r };
    ioctl(kms_fd, DRM_IOCTL_MODE_DIRTYFB, &d);
}
/* src: a canvas-sized picture (stride BW); x0..x1, y0..y1: the part that changed */
static void kms_put_rect(const uint32_t *src, int x0, int y0, int x1, int y1) {
    for (int i = 0; i < nkout; i++) {
        KOut *o = &kout[i];
        if (!o->on || !o->map) continue;
        int ax = imax(x0, o->sx), ay = imax(y0, o->sy), bx = imin(x1, o->sx + o->sw), by = imin(y1, o->sy + o->sh);
        if (ax >= bx || ay >= by) continue;
        if (o->dw == o->sw && o->dh == o->sh) {               /* 1:1 */
            for (int y = ay; y < by; y++) memcpy(kpx(o, o->dx + ax - o->sx, o->dy + y - o->sy), src + (size_t)y * BW + ax, (size_t)(bx - ax) * 4);
            kms_dirty(o, o->dx + ax - o->sx, o->dy + ay - o->sy, o->dx + bx - o->sx, o->dy + by - o->sy);
        } else {                                               /* scaled (Duplicate on a different-size screen) */
            int dx0 = o->dx + (int)((long long)(ax - o->sx) * o->dw / o->sw), dx1 = o->dx + (int)(((long long)(bx - o->sx) * o->dw + o->sw - 1) / o->sw);
            int dy0 = o->dy + (int)((long long)(ay - o->sy) * o->dh / o->sh), dy1 = o->dy + (int)(((long long)(by - o->sy) * o->dh + o->sh - 1) / o->sh);
            dx1 = imin(dx1, o->dx + o->dw); dy1 = imin(dy1, o->dy + o->dh);
            for (int y = dy0; y < dy1; y++) {
                int sy = o->sy + (int)((long long)(y - o->dy) * o->sh / o->dh);
                const uint32_t *row = src + (size_t)sy * BW + o->sx;
                uint32_t *d = kpx(o, 0, y);
                for (int x = dx0; x < dx1; x++) d[x] = row[(long long)(x - o->dx) * o->sw / o->dw];
            }
            kms_dirty(o, dx0, dy0, dx1, dy1);
        }
    }
}
/* one pixel of the mouse pointer (drawn straight onto the screens) */
static void kms_put_px(int x, int y, uint32_t c) {
    for (int i = 0; i < nkout; i++) {
        KOut *o = &kout[i];
        if (!o->on || !o->map || x < o->sx || y < o->sy || x >= o->sx + o->sw || y >= o->sy + o->sh) continue;
        if (o->dw == o->sw && o->dh == o->sh) *kpx(o, o->dx + x - o->sx, o->dy + y - o->sy) = c;
        else {
            int ax = o->dx + (int)((long long)(x - o->sx) * o->dw / o->sw), bx = o->dx + (int)((long long)(x + 1 - o->sx) * o->dw / o->sw);
            int ay = o->dy + (int)((long long)(y - o->sy) * o->dh / o->sh), by = o->dy + (int)((long long)(y + 1 - o->sy) * o->dh / o->sh);
            for (int yy = ay; yy < imax(by, ay + 1); yy++) for (int xx = ax; xx < imax(bx, ax + 1); xx++) *kpx(o, xx, yy) = c;
        }
    }
}
static void kms_flush_cursor(int x0, int y0, int x1, int y1) {   /* (tell screens that need it) */
    for (int i = 0; i < nkout; i++) {
        KOut *o = &kout[i];
        if (!o->on || !o->map) continue;
        int ax = imax(x0, o->sx), ay = imax(y0, o->sy), bx = imin(x1, o->sx + o->sw), by = imin(y1, o->sy + o->sh);
        if (ax >= bx || ay >= by) continue;
        int dx0 = o->dx + (int)((long long)(ax - o->sx) * o->dw / o->sw), dy0 = o->dy + (int)((long long)(ay - o->sy) * o->dh / o->sh);
        int dx1 = o->dx + (int)(((long long)(bx - o->sx) * o->dw + o->sw - 1) / o->sw), dy1 = o->dy + (int)(((long long)(by - o->sy) * o->dh + o->sh - 1) / o->sh);
        kms_dirty(o, dx0, dy0, imin(dx1, o->mw), imin(dy1, o->mh));
    }
}

/* ---- the layout: which screen shows what, and how big the canvas is ---- */
static int kms_layout(int mode, int *nw, int *nbw, int *nh) {
    if (kms_main < 0) return 0;
    if (kms_second < 0) mode = DM_MAIN;
    KOut *a = &kout[kms_main], *b = kms_second >= 0 ? &kout[kms_second] : NULL;
    if (mode == DM_DUP) kms_pick_second(1); else if (mode == DM_EXTEND) kms_pick_second(0);
    else if (b) { int pref = 0; for (int k = 0; k < b->nmodes; k++) if (b->modes[k].type & DRM_MODE_TYPE_PREFERRED) { pref = k; break; } b->pick = pref; }
    if (mode != DM_SECOND && !kms_make_fb(a)) return 0;
    if (b && mode != DM_MAIN && !kms_make_fb(b)) mode = DM_MAIN;
    for (int i = 0; i < nkout; i++) if (i != kms_main && i != kms_second && kout[i].on) kms_off(&kout[i]);
    switch (mode) {
    case DM_MAIN:
        *nw = *nbw = a->mw; *nh = a->mh;
        a->sx = 0; a->sy = 0; a->sw = a->mw; a->sh = a->mh; a->dx = a->dy = 0; a->dw = a->mw; a->dh = a->mh;
        if (b && b->on) kms_off(b);
        break;
    case DM_DUP: {
        *nw = *nbw = a->mw; *nh = a->mh;
        a->sx = 0; a->sy = 0; a->sw = a->mw; a->sh = a->mh; a->dx = a->dy = 0; a->dw = a->mw; a->dh = a->mh;
        float s = fminf((float)b->mw / a->mw, (float)b->mh / a->mh);
        if (fabsf(s - 1) < 0.01f) s = 1;
        b->sx = 0; b->sy = 0; b->sw = a->mw; b->sh = a->mh;
        b->dw = (int)(a->mw * s); b->dh = (int)(a->mh * s); b->dx = (b->mw - b->dw) / 2; b->dy = (b->mh - b->dh) / 2;
        memset(b->map, 0, b->size);
        break; }
    case DM_EXTEND:
        *nw = a->mw; *nbw = a->mw + b->mw; *nh = a->mh;
        a->sx = 0; a->sy = 0; a->sw = a->mw; a->sh = a->mh; a->dx = a->dy = 0; a->dw = a->mw; a->dh = a->mh;
        b->sx = a->mw; b->sy = 0; b->sw = b->mw; b->sh = imin(b->mh, a->mh); b->dx = b->dy = 0; b->dw = b->sw; b->dh = b->sh;
        memset(b->map, 0, b->size);
        break;
    case DM_SECOND:
        *nw = *nbw = b->mw; *nh = b->mh;
        b->sx = 0; b->sy = 0; b->sw = b->mw; b->sh = b->mh; b->dx = b->dy = 0; b->dw = b->mw; b->dh = b->mh;
        if (a->on) kms_off(a);
        break;
    }
    if (mode != DM_SECOND) a->on = kms_show(a) || a->on;
    if (b && mode != DM_MAIN) b->on = kms_show(b);
    if (mode == DM_SECOND && b && !b->on) return kms_layout(DM_MAIN, nw, nbw, nh);   /* (it didn't work: back to the laptop) */
    SH2 = mode == DM_EXTEND ? b->sh : 0;
    kms_cur = mode;
    return 1;
}
static int kms_init(void) {
    char p[32];
    for (int i = 0; i < 4 && kms_fd < 0; i++) {
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        uint64_t has = 0; struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
        if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0) has = cap.value;
        if (!has) { close(fd); continue; }
        kms_fd = fd;
        ioctl(kms_fd, DRM_IOCTL_SET_MASTER, 0);
        memset(kout, 0, sizeof kout);
        kms_probe();
        if (kms_main < 0) { close(kms_fd); kms_fd = -1; continue; }   /* (a card with no screen, try the next) */
    }
    if (kms_fd < 0) return 0;
    kms_ev = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (kms_ev >= 0) { struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 }; if (bind(kms_ev, (struct sockaddr *)&a, sizeof a) < 0) { close(kms_ev); kms_ev = -1; } }
    kms_on = 1;
    return 1;
}
static void kms_blank(int on) {               /* the screens off (power saving) and on again */
    if (!kms_on) return;
    for (int i = 0; i < nkout; i++) {
        KOut *o = &kout[i];
        if (!o->map) continue;
        int want = (kms_cur == DM_MAIN && i == kms_main) || ((kms_cur == DM_DUP || kms_cur == DM_EXTEND) && (i == kms_main || i == kms_second)) || (kms_cur == DM_SECOND && i == kms_second);
        if (!want) continue;
        if (on) o->on = kms_show(o); else kms_off(o);
    }
}
static void kms_quit(void) {                   /* leave the screens to the console */
    if (kms_fd < 0) return;
    ioctl(kms_fd, DRM_IOCTL_DROP_MASTER, 0);
}
