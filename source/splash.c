/*
 * ccos boot splash - the /o\ logo, circling-dots spinner and progress bar
 *
 * Draws on the framebuffer while init runs the boot steps.
 * init sends it lines on stdin:
 *     "<percent> <message>"   e.g.  "35 Mounting /dev"
 *     "quit"                  stop (the last frame stays on screen)
 * "splash --shutdown" / "--reboot" / "--shell" show the shutdown screen
 * (banner, spinner, "Shutting down...") until they get SIGTERM.
 * "splash --text" just switches the console back to text mode.
 *
 * Build: gcc -O2 -static -o splash splash.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include "font8x16.h"
#include "smoothfont.h"

#define RGB(r,g,b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define R(c) (((c) >> 16) & 255)
#define G(c) (((c) >> 8) & 255)
#define B(c) ((c) & 255)

/* ---- look --------------------------------------------------------- */
#define BG        RGB(0, 0, 0)
#define ACCENT    RGB(0x3c, 0xc8, 0xf0)     /* ccos cyan */
#define DIM       RGB(0x90, 0x98, 0xa0)
#define TRACK     RGB(0x26, 0x2c, 0x34)

#define DOTS      5          /* number of dots in the spinner          */
#define DOT_R     3.6f       /* dot radius                             */
#define ORBIT_R   20.0f      /* radius of the circle they travel on    */
#define PERIOD    1.8        /* seconds for one lap                    */
#define DOT_LAG   0.085      /* how far each dot trails the one ahead  */

#define BAR_W     280
#define BAR_H     6

#define LOGO_R    62         /* radius of the /o\\ orb (the ccos logo) */

/* ---- framebuffer ---------------------------------------------------- */
static int W, H, stride, swap_rb;
static uint32_t *fbmem, *back;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline void blend(int x, int y, uint32_t c, int a) {
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    uint32_t *p = &back[y * W + x];
    if (a >= 255) { *p = c; return; }
    uint32_t d = *p;
    *p = RGB((R(c) * a + R(d) * (255 - a)) / 255,
             (G(c) * a + G(d) * (255 - a)) / 255,
             (B(c) * a + B(d) * (255 - a)) / 255);
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) blend(i, j, c, 255);
}

/* anti-aliased filled circle */
static void fill_circle(float cx, float cy, float r, uint32_t c, int a) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = clampf(r + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
            if (cov > 0) blend(x, y, c, (int)(a * cov));
        }
}

/* rounded bar (pill shape) */
static void fill_pill(int x, int y, int w, int h, uint32_t c) {
    float r = h / 2.0f;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            float px = i + 0.5f, py = j + 0.5f - (y + r), ox = 0;
            if (px < x + r) ox = x + r - px; else if (px > x + w - r) ox = px - (x + w - r);
            float cov = clampf(r + 0.5f - sqrtf(ox * ox + py * py), 0, 1);
            if (cov > 0) blend(i, j, c, (int)(255 * cov));
        }
}

static int smooth;                       /* "Smooth font" is on in Settings */
static void read_font_setting(void) {
    FILE *f = fopen("/data/ccos.conf", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "font=1", 6)) smooth = 1;
    fclose(f);
}

static void draw_text(int x, int y, const char *s, uint32_t c, int scale) {
    if (smooth && (scale == 1 || scale == 2)) {
        int cw = 8 * scale, ch = 16 * scale;
        const unsigned char *atlas = scale == 1 ? sf_reg : sf2_bold;
        for (; *s; s++, x += cw) {
            const unsigned char *g = atlas + (unsigned char)*s * cw * ch;
            for (int j = 0; j < ch; j++)
                for (int i = 0; i < cw; i++) if (g[j * cw + i]) blend(x + i, y + j, c, g[j * cw + i]);
        }
        return;
    }
    for (; *s; s++, x += 8 * scale) {
        const unsigned char *g = &font8x16[(unsigned char)*s * 16];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if (g[j] & (0x80 >> i))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            blend(x + i * scale + sx, y + j * scale + sy, c, 255);
    }
}

static void blit(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = y; j < y + h; j++) {
        uint32_t *dst = fbmem + (size_t)j * stride + x, *src = back + (size_t)j * W + x;
        if (!swap_rb) memcpy(dst, src, (size_t)w * 4);
        else for (int i = 0; i < w; i++) dst[i] = RGB(B(src[i]), G(src[i]), R(src[i]));
    }
}


/* ---- the ccos logo: /o\ on a glossy blue orb (like the start button) --- */
static uint32_t mixc(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0, 1);
    int ar = (int)R(a), ag = (int)G(a), ab = (int)B(a), br = (int)R(b), bg = (int)G(b), bb = (int)B(b);
    return RGB((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t));
}
/* how far point p is from the line segment a-b */
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy), 0, 1);
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}
static void draw_logo(int cx, int cy, int r) {
    /* soft glow around it */
    for (int y = cy - r - 24; y <= cy + r + 24; y++)
        for (int x = cx - r - 24; x <= cx + r + 24; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            if (d > r && d < r + 24) blend(x, y, RGB(0x3c, 0x9c, 0xf0), (int)(60 * (1 - (d - r) / 24) * (1 - (d - r) / 24)));
        }
    for (int y = cy - r - 1; y <= cy + r + 1; y++)
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            float cov = clampf(r + 0.5f - d, 0, 1);
            if (cov <= 0) continue;
            float t = (dy + r) / (2.0f * r);
            uint32_t c = t < 0.5f
                ? mixc(RGB(0x2c, 0x6c, 0xb4), RGB(0x0a, 0x33, 0x6e), t * 2)
                : mixc(RGB(0x0a, 0x33, 0x6e), RGB(0x46, 0xb4, 0xf8), (t - 0.5f) * 2 * clampf(1.2f - fabsf(dx) / r, 0, 1));
            float ex = dx / (r * 0.78f), ey = (dy + r * 0.42f) / (r * 0.52f);       /* shine on top */
            if (ex * ex + ey * ey < 1) c = mixc(c, RGB(255, 255, 255), 0.42f * (1 - (dy + r) / (float)r));
            if (d > r - 2.5f) c = mixc(c, RGB(0x04, 0x16, 0x30), 0.7f);           /* dark rim */
            /* the /o\ strokes, with a soft shadow */
            float s = r * 0.085f;                                                  /* half stroke width */
            float u = dx / r, v = dy / r, best = 9;
            float q[3] = {
                seg_dist(u, v, -0.36f, -0.40f, -0.64f, 0.38f),                      /* "/" */
                fabsf(sqrtf(u * u + (v - 0.12f) * (v - 0.12f)) - 0.21f),            /* "o" */
                seg_dist(u, v, 0.36f, -0.40f, 0.64f, 0.38f),                        /* "\" */
            };
            for (int k = 0; k < 3; k++) if (q[k] < best) best = q[k];
            float us = (dx - 2) / r, vs = (dy - 2.5f) / r, bs = 9;
            float qs[3] = {
                seg_dist(us, vs, -0.36f, -0.40f, -0.64f, 0.38f),
                fabsf(sqrtf(us * us + (vs - 0.12f) * (vs - 0.12f)) - 0.21f),
                seg_dist(us, vs, 0.36f, -0.40f, 0.64f, 0.38f),
            };
            for (int k = 0; k < 3; k++) if (qs[k] < bs) bs = qs[k];
            float sh = clampf((s / r - bs) * r + 0.5f, 0, 1);
            c = mixc(c, RGB(0x00, 0x10, 0x30), 0.55f * sh);
            float ink = clampf((s / r - best) * r + 0.5f, 0, 1);
            c = mixc(c, RGB(255, 255, 255), ink);
            blend(x, y, c, (int)(255 * cov));
        }
}

/* ---- layout ----------------------------------------------------------- */
static int banner_y, spin_x, spin_y, bar_x, bar_y, msg_y;

static void layout(void) {
    int total = 2 * LOGO_R + 22 + 16 + 50 + 2 * (int)ORBIT_R + 40 + BAR_H + 14 + 16;
    banner_y = (H - total) / 2;
    spin_x = W / 2;
    spin_y = banner_y + 2 * LOGO_R + 22 + 16 + 50 + (int)ORBIT_R;
    bar_x = (W - BAR_W) / 2;
    bar_y = spin_y + (int)ORBIT_R + 40;
    msg_y = bar_y + BAR_H + 14;
}

static void draw_static(void) {
    for (int i = 0; i < W * H; i++) back[i] = BG;
    draw_logo(W / 2, banner_y + LOGO_R, LOGO_R);
    const char *sub = "Linux 6.12 + BusyBox";
    draw_text((W - 8 * (int)strlen(sub)) / 2, banner_y + 2 * LOGO_R + 22, sub, DIM, 1);
}

/* ---- spinner: dots chase each other around a circle ------------------ */
/* Each dot speeds up at the bottom and slows down at the top, like the
 * Windows loading spinner. The dots start one after another, so they
 * bunch up at the top and spread out at the bottom. */
static void draw_spinner(double t) {
    int box = (int)(ORBIT_R + DOT_R + 3);
    fill_rect(spin_x - box, spin_y - box, 2 * box, 2 * box, BG);
    for (int i = 0; i < DOTS; i++) {
        double p = t / PERIOD - i * DOT_LAG;
        p -= floor(p);                                      /* 0..1 of a lap */
        double eased = p - 0.65 * sin(2 * M_PI * p) / (2 * M_PI);
        double ang = 2 * M_PI * eased;                      /* 0 = top */
        float x = spin_x + ORBIT_R * (float)sin(ang);
        float y = spin_y - ORBIT_R * (float)cos(ang);
        int a = 255 - i * 25;                               /* trailing dots fade */
        fill_circle(x, y, DOT_R, RGB(255, 255, 255), a);
    }
    blit(spin_x - box, spin_y - box, 2 * box, 2 * box);
}

/* ---- progress bar + message ----------------------------------------- */
static void draw_bar(float pct, const char *msg) {
    fill_rect(bar_x - 2, bar_y - 2, BAR_W + 4, BAR_H + 4, BG);
    fill_pill(bar_x, bar_y, BAR_W, BAR_H, TRACK);
    int fw = (int)(BAR_W * clampf(pct, 0, 100) / 100.0f);
    if (fw >= BAR_H) fill_pill(bar_x, bar_y, fw, BAR_H, ACCENT);
    blit(bar_x - 2, bar_y - 2, BAR_W + 4, BAR_H + 4);

    if (msg) {
        fill_rect(0, msg_y, W, 16, BG);
        draw_text((W - 8 * (int)strlen(msg)) / 2, msg_y, msg, DIM, 1);
        blit(0, msg_y, W, 16);
    }
}

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    read_font_setting();
    int tty = open("/dev/tty0", O_RDWR);
    if (argc > 1 && !strcmp(argv[1], "--text")) {      /* give the console back */
        if (tty >= 0) ioctl(tty, KDSETMODE, KD_TEXT);
        return 0;
    }

    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { perror("splash: /dev/fb0"); return 1; }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &vi) || ioctl(fb, FBIOGET_FSCREENINFO, &fi) ||
        vi.bits_per_pixel != 32) { fprintf(stderr, "splash: unsupported framebuffer\n"); return 1; }
    W = vi.xres; H = vi.yres; stride = fi.line_length / 4;
    swap_rb = vi.red.offset == 0;
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbmem == MAP_FAILED) { perror("splash: mmap"); return 1; }
    back = malloc((size_t)W * H * 4);

    if (tty >= 0) ioctl(tty, KDSETMODE, KD_GRAPHICS);   /* hide the text console */

    layout();
    draw_static();
    blit(0, 0, W, H);

    /* ---- shutdown / restart screen ---- */
    if (argc > 1 && (!strcmp(argv[1], "--shutdown") || !strcmp(argv[1], "--reboot") ||
                     !strcmp(argv[1], "--shell"))) {
        const char *text = !strcmp(argv[1], "--reboot") ? "Restarting..."
                         : !strcmp(argv[1], "--shell") ? "Restarting into shell..." : "Shutting down...";
        signal(SIGTERM, on_term);
        fill_rect(0, msg_y - 20, W, 16 * 2 + 8, BG);
        draw_text((W - 16 * (int)strlen(text)) / 2, bar_y - 8, text, RGB(255, 255, 255), 2);
        blit(0, bar_y - 8, W, 32);
        double t0 = now();
        while (!stop) { draw_spinner(now() - t0); usleep(33000); }
        return 0;
    }

    float shown = 0, target = 0;            /* bar slides smoothly to target */
    char msg[128] = "";
    char buf[512];
    int len = 0, quit = 0, eof = 0;
    double t0 = now();
    draw_bar(0, msg);

    while (!quit) {
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (!eof && poll(&p, 1, 33) > 0) {          /* ~30 frames per second */
            ssize_t n = read(0, buf + len, sizeof buf - 1 - len);
            if (n <= 0) eof = 1;
            else {
                len += n;
                buf[len] = 0;
                char *line, *nl;
                while ((line = buf, nl = strchr(buf, '\n'))) {
                    *nl = 0;
                    if (!strncmp(line, "quit", 4)) quit = 1;
                    else {
                        char *sp = strchr(line, ' ');
                        target = atof(line);
                        snprintf(msg, sizeof msg, "%s", sp ? sp + 1 : "");
                        draw_bar(shown, msg);
                    }
                    len -= (int)(nl + 1 - buf);
                    memmove(buf, nl + 1, len + 1);
                }
                if (len >= (int)sizeof buf - 1) len = 0;
            }
        } else if (eof) {
            usleep(33000);
        }

        if (shown < target) {
            shown += (target - shown) * 0.25f + 0.5f;
            if (shown > target) shown = target;
            draw_bar(shown, NULL);
        }
        draw_spinner(now() - t0);
    }
    if (shown < target) draw_bar(target, NULL);
    return 0;   /* stay in graphics mode: the desktop takes over seamlessly */
}
