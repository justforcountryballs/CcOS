/* ===================================================================
 *  GPU BUFFERS (zwp_linux_dmabuf_v1): apps that draw with the graphics
 *  chip (Firefox, games, anything using OpenGL/Vulkan through Mesa) hand
 *  us their finished pictures as "dma-bufs" - memory the GPU drew into.
 *
 *  ccos's screen is still drawn by the CPU, so we ask for plain
 *  row-by-row ("LINEAR") pictures, map them and copy them into the
 *  window. The app still gets to render with the GPU, which is the part
 *  that makes it fast.
 *
 *  Only offered when the computer has a GPU render node (/dev/dri/renderD*)
 *  and "Use the graphics chip for apps" is on in Settings.
 * =================================================================== */
#include <linux/dma-buf.h>
#include <sys/sysmacros.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_ARGB8888 FOURCC('A', 'R', '2', '4')
#define DRM_XRGB8888 FOURCC('X', 'R', '2', '4')
#define DRM_ABGR8888 FOURCC('A', 'B', '2', '4')
#define DRM_XBGR8888 FOURCC('X', 'B', '2', '4')
#define MOD_LINEAR 0ULL
#define MOD_INVALID 0x00ffffffffffffffULL
static const uint32_t dmb_formats[] = { DRM_ARGB8888, DRM_XRGB8888, DRM_ABGR8888, DRM_XBGR8888 };
#define DMB_NFMT 4

static struct wl_global *dmb_global;
static dev_t dmb_dev;                 /* the GPU's render node */
static int dmb_table_fd = -1;         /* the format table, shared with apps */

typedef struct {
    int fd; uint32_t offset, stride;
    int w, h; uint32_t format;
    void *map; size_t map_len;
} dmabuf_t;
typedef struct {
    int fd; uint32_t offset, stride; uint64_t mod; int set;
    int used;
} dmb_params;

/* ---- the wl_buffer for a dma-buf ---- */
static void dmb_buf_destroy_req(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_buffer_interface dmb_buf_impl = { dmb_buf_destroy_req };
static void dmb_buf_free(struct wl_resource *r) {
    dmabuf_t *b = wl_resource_get_user_data(r);
    if (!b) return;
    if (b->map) munmap(b->map, b->map_len);
    if (b->fd >= 0) close(b->fd);
    free(b);
}
static int wl_is_dmabuf(struct wl_resource *buf) {
    return buf && wl_resource_instance_of(buf, &wl_buffer_interface, &dmb_buf_impl);
}
/* copy the picture into the surface's pixels (our 0xAARRGGBB) */
static int dmabuf_copy(struct wl_resource *buf, uint32_t **px, int *pw, int *ph, int *alpha, const int *rect) {
    dmabuf_t *b = wl_resource_get_user_data(buf);
    if (!b) return 0;
    if (!b->map) {
        b->map_len = (size_t)b->offset + (size_t)b->stride * (size_t)b->h;
        b->map = mmap(NULL, b->map_len, PROT_READ, MAP_SHARED, b->fd, 0);
        if (b->map == MAP_FAILED) { b->map = NULL; return 0; }
    }
    int x0 = 0, y0 = 0, x1 = b->w, y1 = b->h;
    if (b->w != *pw || b->h != *ph || !*px) {
        free(*px); *px = malloc((size_t)b->w * b->h * 4);
        *pw = *px ? b->w : 0; *ph = *px ? b->h : 0;
        if (!*px) return 0;
    } else if (rect) {                                   /* only what changed (reading GPU memory is slow) */
        x0 = rect[0] < 0 ? 0 : rect[0]; y0 = rect[1] < 0 ? 0 : rect[1];
        x1 = rect[2] > b->w ? b->w : rect[2]; y1 = rect[3] > b->h ? b->h : rect[3];
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        ((int *)rect)[0] = x0; ((int *)rect)[1] = y0; ((int *)rect)[2] = x1; ((int *)rect)[3] = y1;
    }
    struct dma_buf_sync sy = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sy);            /* waits until the GPU has finished drawing */
    const unsigned char *src = (const unsigned char *)b->map + b->offset;
    int swap = b->format == DRM_ABGR8888 || b->format == DRM_XBGR8888;
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * b->stride);
        uint32_t *d = *px + (size_t)y * b->w;
        if (!swap) memcpy(d + x0, s + x0, (size_t)(x1 - x0) * 4);
        else for (int x = x0; x < x1; x++) { uint32_t v = s[x]; d[x] = (v & 0xff00ff00u) | ((v & 0xff) << 16) | ((v >> 16) & 0xff); }
    }
    sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sy);
    *alpha = b->format == DRM_ARGB8888 || b->format == DRM_ABGR8888;
    return 1;
}

/* ---- zwp_linux_buffer_params_v1: the app describes its buffer ---- */
static void par_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void par_free(struct wl_resource *r) {
    dmb_params *p = wl_resource_get_user_data(r);
    if (p) { if (p->fd >= 0) close(p->fd); free(p); }
}
static void par_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane, uint32_t offset,
                    uint32_t stride, uint32_t mod_hi, uint32_t mod_lo) {
    (void)c;
    dmb_params *p = wl_resource_get_user_data(r);
    if (plane != 0) {                                  /* we only take one-plane pictures */
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "only one plane is supported");
        return;
    }
    if (p->set) { close(fd); wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane already set"); return; }
    p->fd = fd; p->offset = offset; p->stride = stride; p->mod = ((uint64_t)mod_hi << 32) | mod_lo; p->set = 1;
}
static struct wl_resource *par_make_buffer(struct wl_client *c, struct wl_resource *r, uint32_t id,
                                           int32_t w, int32_t h, uint32_t format, int immed) {
    dmb_params *p = wl_resource_get_user_data(r);
    if (p->used) { wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "already used"); return NULL; }
    p->used = 1;
    int okfmt = 0;
    for (int i = 0; i < DMB_NFMT; i++) if (dmb_formats[i] == format) okfmt = 1;
    if (!p->set || w <= 0 || h <= 0 || w > 8192 || h > 8192 || !okfmt ||
        (p->mod != MOD_LINEAR && p->mod != MOD_INVALID) || p->stride < (uint32_t)w * 4) {
        if (immed) wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "unsupported buffer");
        else zwp_linux_buffer_params_v1_send_failed(r);
        return NULL;
    }
    dmabuf_t *b = calloc(1, sizeof *b);
    if (!b) { wl_client_post_no_memory(c); return NULL; }
    b->fd = p->fd; p->fd = -1;
    b->offset = p->offset; b->stride = p->stride; b->w = w; b->h = h; b->format = format;
    struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!buf) { close(b->fd); free(b); wl_client_post_no_memory(c); return NULL; }
    wl_resource_set_implementation(buf, &dmb_buf_impl, b, dmb_buf_free);
    return buf;
}
static void par_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
    (void)flags;
    struct wl_resource *buf = par_make_buffer(c, r, 0, w, h, format, 0);
    if (buf) zwp_linux_buffer_params_v1_send_created(r, buf);
}
static void par_create_immed(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t w, int32_t h,
                             uint32_t format, uint32_t flags) {
    (void)flags;
    par_make_buffer(c, r, id, w, h, format, 1);
}
static const struct zwp_linux_buffer_params_v1_interface par_impl = { par_destroy, par_add, par_create, par_create_immed };

/* ---- feedback (v4): which GPU, which formats ---- */
static void fb_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface fb_impl = { fb_destroy };
static void dmb_send_feedback(struct wl_resource *fb) {
    zwp_linux_dmabuf_feedback_v1_send_format_table(fb, dmb_table_fd, DMB_NFMT * 16);
    struct wl_array dev; wl_array_init(&dev);
    dev_t *d = wl_array_add(&dev, sizeof(dev_t)); *d = dmb_dev;
    zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &dev);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &dev);
    wl_array_release(&dev);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);
    struct wl_array idx; wl_array_init(&idx);
    for (uint16_t i = 0; i < DMB_NFMT; i++) { uint16_t *v = wl_array_add(&idx, 2); *v = i; }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
    wl_array_release(&idx);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
    zwp_linux_dmabuf_feedback_v1_send_done(fb);
}

/* ---- zwp_linux_dmabuf_v1 itself ---- */
static void dmb_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void dmb_create_params(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    dmb_params *p = calloc(1, sizeof *p);
    if (!p) { wl_client_post_no_memory(c); return; }
    p->fd = -1;
    struct wl_resource *pr = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(r), id);
    if (!pr) { free(p); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &par_impl, p, par_free);
}
static void dmb_get_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *fb = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(r), id);
    if (!fb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(fb, &fb_impl, NULL, NULL);
    dmb_send_feedback(fb);
}
static void dmb_get_surface_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf) {
    (void)surf; dmb_get_feedback(c, r, id);
}
static const struct zwp_linux_dmabuf_v1_interface dmb_impl = { dmb_destroy, dmb_create_params, dmb_get_feedback, dmb_get_surface_feedback };

static void dmb_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dmb_impl, NULL, NULL);
    if (version < 4)                                   /* older apps: the list the old way */
        for (int i = 0; i < DMB_NFMT; i++) {
            if (version >= 3) zwp_linux_dmabuf_v1_send_modifier(r, dmb_formats[i], 0, 0);
            else zwp_linux_dmabuf_v1_send_format(r, dmb_formats[i]);
        }
}

/* the GPU's render node, or 0 */
static int dmb_find_gpu(void) {
    char p[64];
    for (int i = 128; i < 136; i++) {
        snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISCHR(st.st_mode)) { dmb_dev = st.st_rdev; return 1; }
    }
    return 0;
}
/* offer it to apps started from now on (on = 1), or stop offering it */
static void dmabuf_enable(int on) {
    if (!wl_dpy) return;
    if (on && !dmb_global) {
        if (!dmb_find_gpu() && !getenv("CCOS_DMABUF_TEST")) return;
        if (dmb_table_fd < 0) {                          /* the table: format + modifier, 16 bytes each */
            dmb_table_fd = memfd_create("ccos-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (dmb_table_fd < 0) return;
            struct { uint32_t fmt, pad; uint64_t mod; } tab[DMB_NFMT];
            for (int i = 0; i < DMB_NFMT; i++) { tab[i].fmt = dmb_formats[i]; tab[i].pad = 0; tab[i].mod = MOD_LINEAR; }
            if (write(dmb_table_fd, tab, sizeof tab) != (ssize_t)sizeof tab) { close(dmb_table_fd); dmb_table_fd = -1; return; }
            fcntl(dmb_table_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
        }
        dmb_global = wl_global_create(wl_dpy, &zwp_linux_dmabuf_v1_interface, 4, NULL, dmb_bind);
    } else if (!on && dmb_global) {
        wl_global_destroy(dmb_global); dmb_global = NULL;
    }
}
static int gpu_available(void) { return dmb_find_gpu(); }
