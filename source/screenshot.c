/* ===================================================================
 *  Screenshots
 *    Print Screen        the whole screen
 *    Win + Shift + S     drag a box over part of the screen (Esc: cancel)
 *  Saved as PNG in Images > Screenshots, and put on the clipboard so it
 *  can be pasted into apps (Firefox, GIMP, LibreOffice...).
 * =================================================================== */
#include <zlib.h>
#define SHOT_DIR "/data/Images/Screenshots"
static int snip_drag, snip_x0, snip_y0, snip_x1, snip_y1;
static uint32_t *snip_img;                     /* the screen as it was when Win+Shift+S was pressed */
static void wl_clip_set_png(const unsigned char *png, size_t n);

static void png_chunk(FILE *f, const char *type, const unsigned char *d, uint32_t n) {
    unsigned char b[4] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n };
    fwrite(b, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4); if (n) c = crc32(c, d, n);
    unsigned char cb[4] = { (unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
    fwrite(cb, 1, 4, f);
}
/* a w x h part of src (row length sw) as PNG bytes in memory; free() them */
static unsigned char *png_encode(const uint32_t *src, int sw, int x, int y, int w, int h, size_t *outn) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    unsigned char *raw = malloc(raw_n);
    if (!raw) return NULL;
    unsigned char *p = raw;
    for (int j = 0; j < h; j++) {
        *p++ = 0;
        const uint32_t *row = src + (size_t)(y + j) * sw + x;
        for (int i = 0; i < w; i++) { uint32_t c = row[i]; *p++ = (unsigned char)(c >> 16); *p++ = (unsigned char)(c >> 8); *p++ = (unsigned char)c; }
    }
    uLongf zn = compressBound((uLong)raw_n);
    unsigned char *z = malloc(zn);
    if (!z || compress2(z, &zn, raw, (uLong)raw_n, 3) != Z_OK) { free(raw); free(z); return NULL; }
    free(raw);
    char *buf = NULL; size_t bn = 0;
    FILE *f = open_memstream(&buf, &bn);
    if (!f) { free(z); return NULL; }
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ih[13] = { (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w,
                             (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8), (unsigned char)h, 8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ih, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(z);
    *outn = bn;
    return (unsigned char *)buf;
}

static void shot_save(const uint32_t *src, int x, int y, int w, int h) {
    if (w < 2 || h < 2) return;
    size_t n; unsigned char *png = png_encode(src, W, x, y, w, h, &n);
    if (!png) { toast(ICON_PHOTO, "Screenshot", "Couldn't make the picture."); return; }
    mkdir("/data", 0755); mkdir("/data/Images", 0755); mkdir(SHOT_DIR, 0755);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char p[200], base[100]; strftime(base, sizeof base, "Screenshot %Y-%m-%d %H-%M-%S", &tm);
    snprintf(p, sizeof p, SHOT_DIR "/%s.png", base);
    for (int k = 2; access(p, F_OK) == 0 && k < 100; k++) snprintf(p, sizeof p, SHOT_DIR "/%s (%d).png", base, k);
    FILE *f = fopen(p, "wb");
    int ok = f && fwrite(png, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    wl_clip_set_png(png, n);
    free(png);
    if (ok) toast(ICON_PHOTO, "Screenshot saved", "In Images > Screenshots, and on the clipboard: paste it into an app.");
    else toast(ICON_PHOTO, "Screenshot", "It's on the clipboard, but couldn't be saved (is D: full?).");
}
static void shot_full(void) { shot_save(back, 0, 0, W, H); }

/* ---- Win+Shift+S: pick a part ---- */
static void snip_rect(int *x, int *y, int *w, int *h) {
    *x = imin(snip_x0, snip_x1); *y = imin(snip_y0, snip_y1);
    *w = abs(snip_x1 - snip_x0); *h = abs(snip_y1 - snip_y0);
}
/* the frozen screen, darkened outside the box; drawn instead of the desktop */
static void snip_render(int x0, int y0, int x1, int y1) {
    int bx, by, bw, bh; snip_rect(&bx, &by, &bw, &bh);
    if (!snip_drag) bw = bh = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t c = snip_img[y * W + x];
            int in = x >= bx && x < bx + bw && y >= by && y < by + bh;
            if (!in) c = RGB(R(c) * 45 / 100, G(c) * 45 / 100, B(c) * 45 / 100);
            if (bw && ((y >= by - 1 && y <= by + bh && (x == bx - 1 || x == bx + bw)) ||
                       (x >= bx - 1 && x <= bx + bw && (y == by - 1 || y == by + bh)))) c = RGB(0x3c, 0x9c, 0xff);
            back[y * W + x] = c;
        }
    if (!snip_drag) {                                  /* the hint, top middle */
        clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
        const char *s = "Drag over the part of the screen to save. Esc cancels.";
        int tw = text_w(s) + 32, tx = (W - tw) / 2;
        fill_round(tx, 18, tw, 32, 6, RGB(0x20, 0x22, 0x28), 235);
        draw_text(tx + 16, 26, s, RGB(255, 255, 255), 255);
    }
    mark(x0, y0, x1 - x0, y1 - y0);
}
static void snip_start(void) {
    if (snip_on) return;
    free(snip_img);
    snip_img = malloc((size_t)W * H * 4);
    if (!snip_img) return;
    memcpy(snip_img, back, (size_t)W * H * 4);
    snip_on = 1; snip_drag = 0;
    snip_render(0, 0, W, H);
}
static void snip_end(int save) {
    if (!snip_on) return;
    int x, y, w, h; snip_rect(&x, &y, &w, &h);
    int had = snip_drag;
    snip_on = 0; snip_drag = 0;
    repaint_all();
    if (save && had && w > 3 && h > 3) shot_save(snip_img, x, y, w, h);
    free(snip_img); snip_img = NULL;
}
static void snip_button(int down) {
    if (down) { snip_drag = 1; snip_x0 = snip_x1 = mx; snip_y0 = snip_y1 = my; snip_render(0, 0, W, H); }
    else if (snip_drag) snip_end(1);
}
static void snip_motion(void) {
    if (!snip_drag) return;
    int ox, oy, ow, oh; snip_rect(&ox, &oy, &ow, &oh);
    snip_x1 = mx; snip_y1 = my;
    int nx, ny, nw, nh; snip_rect(&nx, &ny, &nw, &nh);
    int x0 = imax(0, imin(ox, nx) - 2), y0 = imax(0, imin(oy, ny) - 2);
    int x1 = imin(W, imax(ox + ow, nx + nw) + 2), y1 = imin(H, imax(oy + oh, ny + nh) + 2);
    snip_render(x0, y0, x1, y1);
}
