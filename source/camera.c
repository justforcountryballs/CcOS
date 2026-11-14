/* ===================================================================
 *  Camera: the webcam. White button: a photo (Images > Camera).
 *  Red button: a video with the microphone (Videos > Camera); press
 *  again to stop. The camera itself is run by ccos-camera (cam.c).
 * =================================================================== */
#define CAM_W 720
#define CAM_H 470
#define CAM_MAXW 1920
#define CAM_MAXH 1080
static struct {
    pid_t pid; int to, from, memfd; uint32_t *shm;
    int w, h, cur, started, nocam, rec;
    long long rec_start;
    char msg[160], inbuf[2048]; int inlen;
    long long flash;                         /* a short white flash after a photo */
} cam = { .to = -1, .from = -1, .memfd = -1, .cur = -1 };

static void cam_draw(int cx, int cy, int cw, int ch);
static void cam_send(const char *s) { if (cam.to >= 0) { if (write(cam.to, s, strlen(s)) < 0) {} } }
static void cam_stop_helper(void) {
    if (cam.to >= 0) { cam_send("QUIT\n"); close(cam.to); }
    if (cam.from >= 0) close(cam.from);
    if (cam.shm) munmap(cam.shm, (size_t)CAM_MAXW * CAM_MAXH * 4 * 2);
    if (cam.memfd >= 0) close(cam.memfd);
    cam.to = cam.from = cam.memfd = -1; cam.shm = NULL; cam.pid = 0; cam.cur = -1; cam.started = 0; cam.rec = 0;
}
static void cam_start(void) {
    cam_stop_helper();
    cam.nocam = 0; cam.msg[0] = 0; cam.inlen = 0;
    size_t sz = (size_t)CAM_MAXW * CAM_MAXH * 4 * 2;
    cam.memfd = (int)syscall(SYS_memfd_create, "ccos-camera", 0);
    if (cam.memfd < 0 || ftruncate(cam.memfd, (off_t)sz)) { snprintf(cam.msg, sizeof cam.msg, "Not enough memory."); return; }
    cam.shm = mmap(NULL, sz, PROT_READ, MAP_SHARED, cam.memfd, 0);
    if (cam.shm == MAP_FAILED) { cam.shm = NULL; return; }
    int a[2], b[2];
    if (pipe(a) || pipe(b)) return;
    pid_t p = fork();
    if (p == 0) {
        dup2(a[0], 0); dup2(b[1], 1); dup2(cam.memfd, 3);
        int nul = open("/dev/null", O_WRONLY); if (nul >= 0) dup2(nul, 2);
        for (int fd = 4; fd < 256; fd++) close(fd);
        execl("/usr/bin/ccos-camera", "ccos-camera", (char *)NULL);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    cam.pid = p; cam.to = a[1]; cam.from = b[0];
    fcntl(cam.from, F_SETFL, O_NONBLOCK);
    fcntl(cam.to, F_SETFD, FD_CLOEXEC); fcntl(cam.from, F_SETFD, FD_CLOEXEC); fcntl(cam.memfd, F_SETFD, FD_CLOEXEC);
    snprintf(cam.msg, sizeof cam.msg, "Starting the camera...");
}
static int cam_fd(void) { return cam.from; }
static void cam_wake(void) {
    if (cam.from < 0) return;
    ssize_t k;
    while ((k = read(cam.from, cam.inbuf + cam.inlen, sizeof cam.inbuf - 1 - (size_t)cam.inlen)) > 0) cam.inlen += (int)k;
    int eof = k == 0;
    cam.inbuf[cam.inlen] = 0;
    char *l = cam.inbuf, *nl; int redraw = 0;
    while ((nl = strchr(l, '\n'))) {
        *nl = 0;
        int s, w, h;
        if (sscanf(l, "F %d", &s) == 1 && s >= 0 && s < 2) {
            if (cam.cur >= 0 && cam.cur != s) { char d[16]; snprintf(d, sizeof d, "D %d\n", cam.cur); cam_send(d); }
            cam.cur = s; redraw = 1;
        } else if (sscanf(l, "S %d %d", &w, &h) == 2) { cam.w = w; cam.h = h; cam.started = 1; cam.msg[0] = 0; redraw = 1; }
        else if (!strcmp(l, "NOCAM")) { cam.nocam = 1; snprintf(cam.msg, sizeof cam.msg, "No camera found. Is it turned on (some laptops have a switch or F-key)?"); redraw = 1; }
        else if (!strncmp(l, "P ", 2)) toast(ICON_CAMERA, "Photo saved", "In Images > Camera.");
        else if (!strncmp(l, "V ", 2)) toast(ICON_CAMERA, "Video saved", "In Videos > Camera.");
        else if (!strncmp(l, "ERR ", 4)) { snprintf(cam.msg, sizeof cam.msg, "%s", l + 4); cam.rec = 0; redraw = 1; }
        l = nl + 1;
    }
    cam.inlen = (int)strlen(l); memmove(cam.inbuf, l, (size_t)cam.inlen + 1);
    if (eof) { close(cam.from); cam.from = -1; if (!cam.nocam && !cam.msg[0]) snprintf(cam.msg, sizeof cam.msg, "The camera stopped."); redraw = 1; }
    if (redraw) repaint_client(cam_draw);
}

static void cam_buttons(int cw, int ch, int *px, int *py, int *vx, int *vy, int *r) {
    *r = 26; *px = cw - 46; *py = ch / 2 - 40; *vx = cw - 46; *vy = ch / 2 + 40;
}
static void cam_draw(int cx, int cy, int cw, int ch) {
    fill_rect(cx, cy, cw, ch, RGB(0x10, 0x10, 0x12), 255);
    int aw = cw - 92, ah = ch;                                   /* the picture, left of the buttons */
    if (cam.started && cam.cur >= 0 && cam.shm && cam.w > 0) {
        float s = fminf((float)aw / cam.w, (float)ah / cam.h);
        int dw = (int)(cam.w * s), dh = (int)(cam.h * s), dx = cx + (aw - dw) / 2, dy = cy + (ah - dh) / 2;
        const uint32_t *src = cam.shm + (size_t)cam.cur * CAM_MAXW * CAM_MAXH;
        int x0 = imax(dx, clip_x0), x1 = imin(dx + dw, clip_x1), y0 = imax(dy, clip_y0), y1 = imin(dy + dh, clip_y1);
        for (int y = y0; y < y1; y++) {
            const uint32_t *row = src + (size_t)((y - dy) * cam.h / dh) * cam.w;
            uint32_t *d = &back[(size_t)y * BW];
            for (int x = x0; x < x1; x++) d[x] = row[cam.w - 1 - (x - dx) * cam.w / dw] & 0xffffff;   /* mirrored, like a mirror */
        }
        if (ui_ms() < cam.flash) fill_rect(dx, dy, dw, dh, RGB(255, 255, 255), 160);
    }
    if (cam.msg[0]) {
        int tw = text_w(cam.msg);
        if (tw > aw - 20) { draw_text(cx + 12, cy + ah / 2 - 8, cam.msg, RGB(0xe0, 0xe0, 0xe0), 255); }
        else draw_text(cx + (aw - tw) / 2, cy + ah / 2 - 8, cam.msg, RGB(0xe0, 0xe0, 0xe0), 255);
    }
    int px, py, vx, vy, r; cam_buttons(cw, ch, &px, &py, &vx, &vy, &r);
    int hp = hot.type == HOT_CONTENT && wins[hot.a].draw == cam_draw && hot.b == 1;
    int hvv = hot.type == HOT_CONTENT && wins[hot.a].draw == cam_draw && hot.b == 2;
    int en = cam.started;
    fill_circle(cx + px, cy + py, r, en ? (hp ? RGB(255, 255, 255) : RGB(0xe8, 0xe8, 0xec)) : RGB(0x60, 0x60, 0x66), 255);
    draw_icon(ICON_CAMERA, cx + px, cy + py, 13);
    fill_circle(cx + vx, cy + vy, r, en ? (hvv ? RGB(0xff, 0x40, 0x40) : RGB(0xe0, 0x28, 0x28)) : RGB(0x60, 0x60, 0x66), 255);
    if (cam.rec) {
        fill_round(cx + vx - 9, cy + vy - 9, 18, 18, 3, RGB(255, 255, 255), 255);
        long long s = (ui_ms() - cam.rec_start) / 1000;
        char b[16]; snprintf(b, sizeof b, "%lld:%02lld", s / 60, s % 60);
        fill_round(cx + 12, cy + 12, text_w(b) + 32, 26, 13, RGB(0xc0, 0x18, 0x18), 255);
        fill_circle(cx + 26, cy + 25, 5, RGB(255, 255, 255), 255);
        draw_text(cx + 36, cy + 17, b, RGB(255, 255, 255), 255);
    } else fill_circle(cx + vx, cy + vy, 9, RGB(255, 255, 255), 255);
    draw_text(cx + px - text_w("Photo") / 2, cy + py + r + 6, "Photo", RGB(0xc8, 0xc8, 0xcc), 255);
    draw_text(cx + vx - text_w("Video") / 2, cy + vy + r + 6, "Video", RGB(0xc8, 0xc8, 0xcc), 255);
}
static int cam_hit(int lx, int ly, int cw, int ch) {
    int px, py, vx, vy, r; cam_buttons(cw, ch, &px, &py, &vx, &vy, &r);
    if ((lx - px) * (lx - px) + (ly - py) * (ly - py) <= r * r) return 1;
    if ((lx - vx) * (lx - vx) + (ly - vy) * (ly - vy) <= r * r) return 2;
    return -1;
}
static void cam_name(char *out, size_t n, const char *dir, const char *kind, const char *ext) {
    mkdir("/data", 0755);
    char d1[64]; snprintf(d1, sizeof d1, "/data/%s", dir); mkdir(d1, 0755);
    char d2[96]; snprintf(d2, sizeof d2, "%s/Camera", d1); mkdir(d2, 0755);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H-%M-%S", &tm);
    snprintf(out, n, "%s/%s %s.%s", d2, kind, b, ext);
}
static void cam_click(int lx, int ly, int cw, int ch) {
    int id = cam_hit(lx, ly, cw, ch);
    if (!cam.started) { if (id > 0 && (cam.nocam || cam.from < 0)) cam_start(); return; }
    char p[300], c[320];
    if (id == 1) { cam_name(p, sizeof p, "Images", "Photo", "jpg"); snprintf(c, sizeof c, "PHOTO %s\n", p); cam_send(c); cam.flash = ui_ms() + 150; }
    else if (id == 2 && !cam.rec) { cam_name(p, sizeof p, "Videos", "Video", "mp4"); snprintf(c, sizeof c, "REC %s\n", p); cam_send(c); cam.rec = 1; cam.rec_start = ui_ms(); }
    else if (id == 2) { cam_send("STOP\n"); cam.rec = 0; }
    repaint_client(cam_draw);
}
static void cam_key(int code, int mods) {
    (void)mods;
    if (code == KEY_SPACE || code == KEY_ENTER) { int i = win_of(cam_draw); if (i >= 0) { int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch); int px, py, vx, vy, r; cam_buttons(cw, ch, &px, &py, &vx, &vy, &r); cam_click(px, py, cw, ch); } }
}
static void cam_tick(void) { if (cam.rec) repaint_client(cam_draw); }
static int cam_on_close(void) { cam_stop_helper(); return 1; }
