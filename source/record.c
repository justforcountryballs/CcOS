/* ===================================================================
 *  Record: records the whole screen (and the sound apps make) into a
 *  video in Videos. Start it in the Record app or with Win+Alt+R; while it
 *  records, a red "REC 0:12" on the taskbar shows the time - click it (or
 *  Win+Alt+R again) to stop. The encoding is done by ccos-record (rec.c).
 * =================================================================== */
#define REC_NSLOT 3
#define REC_W 300
#define REC_H 214
static struct {
    int on, stopping;
    pid_t pid; int to, from;             /* pipes to/from ccos-record */
    int memfd; uint32_t *shm;
    int busy[REC_NSLOT], w, h;
    long long start, last_cap, last_frame;
    int changed;                          /* the screen changed since the last picture */
    char path[256], err[120];
    char inbuf[256]; int inlen;
} rec = { .to = -1, .from = -1, .memfd = -1 };

static int rec_on(void) { return rec.on; }
static void rec_changed(void) { rec.changed = 1; }
static int rec_tb_x(void) { return (bat.present ? bat_x() : tray_x()) - REC_TB_W - 4; }
static void repaint_rec_tb(void) { repaint(rec_tb_x(), H - TASKBAR_H, REC_TB_W, TASKBAR_H); }

static uint32_t *rec_put_dst;
static void rec_put(int x, int y, uint32_t c, int a) {            /* the pointer, into the picture */
    if (x < 0 || y < 0 || x >= rec.w || y >= rec.h) return;
    uint32_t *d = &rec_put_dst[(size_t)y * rec.w + x];
    *d = a >= 255 ? c : mix(*d, c, a / 255.0f);
}

static void rec_cleanup(void) {
    if (rec.to >= 0) close(rec.to);
    if (rec.from >= 0) close(rec.from);
    if (rec.shm) munmap(rec.shm, (size_t)rec.w * rec.h * 4 * REC_NSLOT);
    if (rec.memfd >= 0) close(rec.memfd);
    rec.to = rec.from = rec.memfd = -1; rec.shm = NULL; rec.pid = 0;
    rec.on = rec.stopping = 0;
}

static void rec_draw(int cx, int cy, int cw, int ch);
static void rec_start(void) {
    if (rec.on || rec.stopping) return;
    rec.err[0] = 0;
    mkdir("/data", 0755); mkdir("/data/Videos", 0755);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char base[80]; strftime(base, sizeof base, "Recording %Y-%m-%d %H-%M-%S", &tm);
    snprintf(rec.path, sizeof rec.path, "/data/Videos/%s.mp4", base);
    rec.w = W; rec.h = H;
    size_t sz = (size_t)W * H * 4 * REC_NSLOT;
    rec.memfd = (int)syscall(SYS_memfd_create, "ccos-record", 0);
    if (rec.memfd < 0 || ftruncate(rec.memfd, (off_t)sz)) { snprintf(rec.err, sizeof rec.err, "Not enough memory to record."); rec_cleanup(); return; }
    rec.shm = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, rec.memfd, 0);
    if (rec.shm == MAP_FAILED) { rec.shm = NULL; snprintf(rec.err, sizeof rec.err, "Not enough memory to record."); rec_cleanup(); return; }
    int a[2], b[2];
    if (pipe(a) || pipe(b)) { rec_cleanup(); return; }
    pid_t p = fork();
    if (p == 0) {
        dup2(a[0], 0); dup2(b[1], 1); dup2(rec.memfd, 3);
        int nul = open("/dev/null", O_WRONLY); if (nul >= 0) dup2(nul, 2);
        for (int fd = 4; fd < 256; fd++) close(fd);
        char ws[16], hs[16]; snprintf(ws, sizeof ws, "%d", W); snprintf(hs, sizeof hs, "%d", H);
        execl("/usr/bin/ccos-record", "ccos-record", rec.path, ws, hs, set_rec_sound ? "1" : "0", (char *)NULL);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    if (p < 0) { close(a[1]); close(b[0]); rec_cleanup(); return; }
    rec.pid = p; rec.to = a[1]; rec.from = b[0];
    fcntl(rec.from, F_SETFL, O_NONBLOCK);
    fcntl(rec.to, F_SETFD, FD_CLOEXEC); fcntl(rec.from, F_SETFD, FD_CLOEXEC); fcntl(rec.memfd, F_SETFD, FD_CLOEXEC);
    memset(rec.busy, 0, sizeof rec.busy);
    rec.on = 1; rec.start = ui_ms(); rec.last_cap = 0; rec.last_frame = -1000; rec.changed = 1; rec.inlen = 0;
    int i = win_of(rec_draw);
    if (i >= 0) minimize_win(i);                       /* out of the way of what's being recorded */
    repaint_taskbar();
}
static void rec_stop(void) {
    if (!rec.on || rec.stopping) return;
    rec.stopping = 1;
    if (rec.to >= 0) { if (write(rec.to, "E\n", 2) < 0) {} }
    repaint_taskbar();
    int i = win_of(rec_draw); if (i >= 0) repaint_client(rec_draw);
}
static void rec_toggle(void) { if (rec.on) rec_stop(); else { rec_start(); if (!rec.on && rec.err[0]) toast(ICON_RECORD, "Record", rec.err); } }

/* answers from ccos-record */
static void rec_read(void) {
    if (rec.from < 0) return;
    ssize_t k;
    while ((k = read(rec.from, rec.inbuf + rec.inlen, sizeof rec.inbuf - 1 - (size_t)rec.inlen)) > 0) rec.inlen += (int)k;
    int eof = k == 0;
    rec.inbuf[rec.inlen] = 0;
    char *l = rec.inbuf, *nl;
    while ((nl = strchr(l, '\n'))) {
        *nl = 0;
        int s;
        if (sscanf(l, "D %d", &s) == 1 && s >= 0 && s < REC_NSLOT) rec.busy[s] = 0;
        else if (!strncmp(l, "OK", 2)) {
            char b[200], *n = strrchr(rec.path, '/');
            snprintf(b, sizeof b, "Saved in Videos: %.120s", n ? n + 1 : rec.path);
            toast(ICON_RECORD, "Recording saved", b);
            rec_cleanup(); repaint_taskbar(); return;
        } else if (!strncmp(l, "ERR ", 4)) {
            snprintf(rec.err, sizeof rec.err, "%s", l + 4);
            toast(ICON_RECORD, "Couldn't record", rec.err);
            rec_cleanup(); repaint_taskbar(); return;
        }
        l = nl + 1;
    }
    rec.inlen = (int)strlen(l); memmove(rec.inbuf, l, (size_t)rec.inlen + 1);
    if (eof || (rec.pid && kill(rec.pid, 0) != 0)) {        /* it stopped without saying */
        if (!rec.stopping) toast(ICON_RECORD, "Couldn't record", "The recorder stopped unexpectedly.");
        rec_cleanup(); repaint_taskbar();
    }
}
/* from the main loop: take a picture of the screen when it's time */
static int rec_wait(void) { return rec.on && !rec.stopping ? 15 : -1; }
static void rec_tick(void) {
    if (!rec.on) return;
    rec_read();
    if (!rec.on || rec.stopping) return;
    long long now = ui_ms();
    static long long last_tb;
    if (now / 1000 != last_tb) { last_tb = now / 1000; repaint_rec_tb(); }   /* the timer on the taskbar */
    if (now - rec.last_cap < 33) return;                                       /* 30 pictures a second at most */
    if (!rec.changed && now - rec.last_cap < 1000) return;                     /* nothing new: one a second */
    int s = -1;
    for (int i = 0; i < REC_NSLOT; i++) if (!rec.busy[i]) { s = i; break; }
    if (s < 0) return;                                                          /* the encoder is behind: skip */
    uint32_t *d = rec.shm + (size_t)s * rec.w * rec.h;
    for (int y = 0; y < rec.h; y++) memcpy(d + (size_t)y * rec.w, back + (size_t)y * BW, (size_t)rec.w * 4);   /* (the main screen) */
    if (cur_shape != CUR_HIDDEN && cur_size >= 0) { rec_put_dst = d; render_cursor(cur_shape, cur_col, cursor_scales[cur_size], mx, my, rec_put); }
    char l[48]; int n = snprintf(l, sizeof l, "F %d %lld\n", s, now - rec.start);
    if (write(rec.to, l, (size_t)n) == n) rec.busy[s] = 1;
    rec.last_cap = now; rec.changed = 0;
}

/* the taskbar's red REC button */
static void rec_draw_tb(void) {
    if (!rec.on) return;
    int x = rec_tb_x(), y = H - TASKBAR_H + 7, h = TASKBAR_H - 14;
    int hv = hot.type == HOT_REC;
    fill_round(x, y, REC_TB_W, h, h / 2, hv ? RGB(0xd0, 0x24, 0x24) : RGB(0xb0, 0x18, 0x18), 255);
    long long s = (ui_ms() - rec.start) / 1000;
    char b[24];
    if (rec.stopping) snprintf(b, sizeof b, "Saving");
    else snprintf(b, sizeof b, "%lld:%02lld", s / 60, s % 60);
    fill_circle(x + 13, y + h / 2.0f, 4.5f, RGB(255, 255, 255), (s % 2 || rec.stopping) ? 255 : 150);
    draw_text(x + 24, y + (h - 16) / 2, b, RGB(255, 255, 255), 255);
}

/* ---- the Record app ---- */
static void rec_draw(int cx, int cy, int cw, int ch) {
    (void)ch;
    fill_rect(cx, cy, cw, ch, RGB(0xf4, 0xf6, 0xf9), 255);
    int bx = cx + cw / 2, by = cy + 62;
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == rec_draw && hot.b == 1;
    if (!rec.on) {
        fill_circle(bx, by, 34, RGB(0xd8, 0xdc, 0xe2), 255);
        fill_circle(bx, by, 30, hv ? RGB(0xf0, 0x30, 0x30) : RGB(0xd8, 0x20, 0x20), 255);
        const char *s = "Start recording";
        draw_text(cx + (cw - text_w(s)) / 2, cy + 106, s, RGB(0x1e, 0x29, 0x3b), 255);
    } else {
        fill_circle(bx, by, 34, RGB(0xd8, 0xdc, 0xe2), 255);
        fill_circle(bx, by, 30, hv ? RGB(0x50, 0x58, 0x64) : RGB(0x38, 0x40, 0x4c), 255);
        fill_round(bx - 11, by - 11, 22, 22, 3, RGB(255, 255, 255), 255);
        long long s = (ui_ms() - rec.start) / 1000;
        char b[40]; snprintf(b, sizeof b, rec.stopping ? "Saving..." : "Recording  %lld:%02lld - Stop", s / 60, s % 60);
        draw_text(cx + (cw - text_w(b)) / 2, cy + 106, b, RGB(0xb0, 0x18, 0x18), 255);
    }
    /* sound */
    int ty = cy + 138;
    fill_round(cx + 20, ty, 16, 16, 3, RGB(255, 255, 255), 255);
    stroke_round(cx + 20, ty, 16, 16, 3, RGB(0x88, 0x96, 0xa8), 255);
    if (set_rec_sound) { aa_line(cx + 24, ty + 8, cx + 27, ty + 12, 2, RGB(0x1c, 0x6c, 0xd0)); aa_line(cx + 27, ty + 12, cx + 32, ty + 4, 2, RGB(0x1c, 0x6c, 0xd0)); }
    draw_text(cx + 44, ty, "Record the sound apps make", RGB(0x1e, 0x29, 0x3b), 255);
    draw_text(cx + 20, cy + 168, "Saved in Videos. To stop, click", RGB(0x6a, 0x76, 0x86), 255);
    draw_text(cx + 20, cy + 186, "REC on the taskbar (or Win+Alt+R).", RGB(0x6a, 0x76, 0x86), 255);
}
static int rec_hit(int lx, int ly, int cw, int ch) {
    (void)ch;
    int dx = lx - cw / 2, dy = ly - 62;
    if (dx * dx + dy * dy <= 34 * 34) return 1;
    if (ly >= 134 && ly < 158 && lx >= 16 && lx < 260) return 2;
    return -1;
}
static void rec_click(int lx, int ly, int cw, int ch) {
    int id = rec_hit(lx, ly, cw, ch);
    if (id == 1) rec_toggle();
    else if (id == 2 && !rec.on) { set_rec_sound = !set_rec_sound; save_settings(); }
    repaint_client(rec_draw);
}
static void rec_app_tick(void) { if (rec.on) repaint_client(rec_draw); }
