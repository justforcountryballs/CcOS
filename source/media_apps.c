/* ===================================================================
 *  PHOTOS, VIDEOS and MUSIC
 *
 *  Photos decodes pictures itself (stb_image: JPG, PNG, GIF, BMP...; nanosvg: SVG).
 *  Videos and Music start ccos-media (FFmpeg) for each file: it plays the
 *  sound through PulseAudio and puts video frames into a shared memory file
 *  that we draw from.
 * =================================================================== */
static void run(const char *cmd);
static void draw_music_icon(int cx, int cy, int r);

enum { MK_NONE, MK_IMAGE, MK_VIDEO, MK_AUDIO };
static int media_kind(const char *name) {
    const char *d = strrchr(name, '.');
    if (!d) return MK_NONE;
    static const char *img[] = { ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tga", ".psd", ".svg", ".pnm", ".ppm", ".pgm", ".hdr", NULL };
    static const char *vid[] = { ".mp4", ".m4v", ".mkv", ".webm", ".avi", ".mov", ".wmv", ".flv", ".mpg", ".mpeg", ".ts", ".3gp", ".ogv", NULL };
    static const char *aud[] = { ".mp3", ".ogg", ".oga", ".opus", ".flac", ".wav", ".m4a", ".aac", ".wma", ".wv", NULL };
    for (int i = 0; img[i]; i++) if (!strcasecmp(d, img[i])) return MK_IMAGE;
    for (int i = 0; vid[i]; i++) if (!strcasecmp(d, vid[i])) return MK_VIDEO;
    for (int i = 0; aud[i]; i++) if (!strcasecmp(d, aud[i])) return MK_AUDIO;
    return MK_NONE;
}
static const char *media_app_for(const char *name) {
    switch (media_kind(name)) {
    case MK_IMAGE: return "@photos";
    case MK_VIDEO: return "@videos";
    case MK_AUDIO: return "@music";
    }
    return NULL;
}

/* ---- the files the apps know about (a folder's worth) -------------- */
#define MF_MAX 400
typedef struct { char path[MF_MAX][300]; int n; } MFiles;
static int mf_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static void mf_add_dir(MFiles *m, const char *dir, int kind, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && m->n < MF_MAX) {
        if (e->d_name[0] == '.') continue;
        char p[300]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0 && strcmp(e->d_name, "lost+found")) mf_add_dir(m, p, kind, depth - 1); continue; }
        if (media_kind(e->d_name) == kind) snprintf(m->path[m->n++], 300, "%s", p);
    }
    closedir(d);
}
/* "Library": the usual places for this kind of file */
static void mf_library(MFiles *m, int kind) {
    m->n = 0;
    static const char *dirs[] = { "/data/Documents", "/data/Images", "/data/Pictures", "/data/Videos", "/data/Music",
                                  "/apps/home/Downloads", "/apps/home/Pictures", "/apps/home/Videos", "/apps/home/Music",
                                  "/data/home/Downloads", "/root/Downloads", NULL };
    for (int i = 0; dirs[i]; i++) mf_add_dir(m, dirs[i], kind, 2);
    mf_add_dir(m, "/media", kind, 3);                    /* USB drives */
    qsort(m->path, (size_t)m->n, sizeof m->path[0], mf_cmp);
}
static void mf_folder_of(MFiles *m, const char *file, int kind) {
    char dir[300]; snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
    m->n = 0;
    mf_add_dir(m, dir[0] ? dir : "/", kind, 0);
    qsort(m->path, (size_t)m->n, sizeof m->path[0], mf_cmp);
}
static int mf_index(const MFiles *m, const char *p) { for (int i = 0; i < m->n; i++) if (!strcmp(m->path[i], p)) return i; return -1; }
static const char *mf_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void mf_shown(const char *s, char *out, size_t n) { ssid_shown((const unsigned char *)s, (int)strlen(s), out, n); }

/* a small round media button (play, pause, next...) */
enum { MB_PLAY, MB_PAUSE, MB_PREV, MB_NEXT, MB_FULL, MB_ZIN, MB_ZOUT, MB_FIT, MB_LIB, MB_PRINT };
static void media_btn(int cx, int cy, int r, int kind, int hot_, int dark) {
    uint32_t bg0 = dark ? (hot_ ? RGB(0x50, 0x5a, 0x68) : RGB(0x34, 0x3c, 0x48)) : (hot_ ? RGB(0xff, 0xff, 0xff) : RGB(0xf2, 0xf4, 0xf8));
    uint32_t bg1 = dark ? (hot_ ? RGB(0x3a, 0x42, 0x50) : RGB(0x22, 0x28, 0x32)) : (hot_ ? RGB(0xdc, 0xe8, 0xf6) : RGB(0xd8, 0xde, 0xe6));
    uint32_t fg = dark ? RGB(0xf0, 0xf4, 0xf8) : RGB(0x28, 0x34, 0x44);
    if (kind == MB_PLAY || kind == MB_PAUSE) {
        bg0 = hot_ ? RGB(0x5c, 0xb0, 0xff) : RGB(0x3c, 0x94, 0xf4); bg1 = hot_ ? RGB(0x2a, 0x78, 0xdc) : RGB(0x1c, 0x64, 0xc8); fg = RGB(255, 255, 255);
    }
    fill_round_grad(cx - r, cy - r, 2 * r, 2 * r, r, bg0, bg1, 255);
    stroke_round(cx - r, cy - r, 2 * r, 2 * r, r, dark ? RGB(0x10, 0x14, 0x18) : RGB(0xa0, 0xaa, 0xb8), 200);
    int s = r / 2;
    switch (kind) {
    case MB_PLAY:
        for (int y = -s; y <= s; y++) { int w = (s - abs(y)) * 2 * 9 / 10 + 1; fill_rect(cx - s / 2 - 1 + 2, cy + y, w, 1, fg, 255); }
        break;
    case MB_PAUSE: fill_rect(cx - s + 1, cy - s, s * 2 / 3 + 1, 2 * s, fg, 255); fill_rect(cx + s / 3, cy - s, s * 2 / 3 + 1, 2 * s, fg, 255); break;
    case MB_PREV: case MB_NEXT: {
        int dir = kind == MB_NEXT ? 1 : -1;
        for (int y = -s * 3 / 4; y <= s * 3 / 4; y++) {
            int w = s * 3 / 4 - abs(y) + 1;
            for (int k = 0; k < 2; k++) {
                int x0 = dir > 0 ? cx - s + k * (s * 3 / 4) : cx + s - k * (s * 3 / 4) - w;
                fill_rect(x0, cy + y, w, 1, fg, 255);
            }
        }
        fill_rect(dir > 0 ? cx + s / 2 + 1 : cx - s / 2 - 2, cy - s * 3 / 4, 2, s * 3 / 2 + 1, fg, 255);
        break;
    }
    case MB_FULL:
        for (int k = 0; k < 4; k++) {
            int sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1;
            int px = cx + sx * s, py = cy + sy * s;
            fill_rect(sx > 0 ? px - 4 : px, py - (sy > 0 ? 1 : 0), 5, 2, fg, 255);
            fill_rect(px - (sx > 0 ? 1 : 0), sy > 0 ? py - 4 : py, 2, 5, fg, 255);
        }
        break;
    case MB_ZIN: case MB_ZOUT:
        fill_rect(cx - s, cy - 1, 2 * s, 2, fg, 255);
        if (kind == MB_ZIN) fill_rect(cx - 1, cy - s, 2, 2 * s, fg, 255);
        break;
    case MB_FIT: stroke_round(cx - s, cy - s * 3 / 4, 2 * s, s * 3 / 2, 2, fg, 255); stroke_round(cx - s + 3, cy - s * 3 / 4 + 3, 2 * s - 6, s * 3 / 2 - 6, 1, fg, 255); break;
    case MB_PRINT:                                         /* a little printer */
        stroke_round(cx - s / 2, cy - s, s, s * 3 / 4, 0, fg, 255);
        fill_round(cx - s, cy - s / 3, 2 * s, s, 2, fg, 255);
        fill_rect(cx - s / 2, cy + s / 3, s, s * 2 / 3, bg0, 255);
        stroke_round(cx - s / 2, cy + s / 3, s, s * 2 / 3, 0, fg, 255);
        break;
    case MB_LIB:
        for (int k = 0; k < 3; k++) fill_rect(cx - s, cy - s + 1 + k * (s * 2 / 3 + 1), 2 * s, 2, fg, 255);
        break;
    }
}
static void fmt_time(double t, char *o, size_t n) {
    if (t < 0) t = 0;
    int s = (int)t;
    if (s >= 3600) snprintf(o, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(o, n, "%d:%02d", s / 60, s % 60);
}
/* copy a picture (BGRA) scaled to dw x dh at dx, dy (nearest pixel: quick enough for video) */
static void blit_scaled(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh) {
    dark_keep(dx, dy, dw, dh);                         /* a picture: dark mode leaves it alone */
    int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (!clip_range(&x0, &y0, &x1, &y1) || dw <= 0 || dh <= 0) return;
    static int xm[8192];
    for (int x = x0; x < x1 && x - x0 < 8192; x++) xm[x - x0] = (int)((long)(x - dx) * sw / dw);
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = src + (size_t)((long)(y - dy) * sh / dh) * sw;
        uint32_t *d = &back[(size_t)y * BW];
        if (dw == sw) memcpy(d + x0, s + (x0 - dx), (size_t)(x1 - x0) * 4);
        else for (int x = x0; x < x1; x++) d[x] = s[xm[x - x0]] & 0xffffff;
    }
}

/* ---- one running ccos-media ------------------------------------------ */
typedef struct {
    pid_t pid; int in, out;            /* commands to it, its messages */
    uint32_t *shm; size_t shm_len; char shm_path[64];
    int w, h, has_video, has_audio, slot, have_frame, cover;
    double dur, pos; int paused, ended, failed, started;
    char title[120], artist[120], album[120], err[120];
    char buf[1024]; int bl;
} MPlayer;
static void mp_stop(MPlayer *m) {
    if (m->pid > 0) {
        if (m->in >= 0 && write(m->in, "q\n", 2) < 0) {}
        kill(m->pid, SIGTERM);
    }
    if (m->in >= 0) close(m->in);
    if (m->out >= 0) close(m->out);
    if (m->shm) munmap(m->shm, m->shm_len);
    if (m->shm_path[0]) unlink(m->shm_path);
    memset(m, 0, sizeof *m);
    m->in = m->out = -1;
}
static int mp_start(MPlayer *m, const char *file, int maxw, int maxh) {
    mp_stop(m);
    static int seq;
    snprintf(m->shm_path, sizeof m->shm_path, "/dev/shm/ccos-media-%d-%d", (int)getpid(), seq++);
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC) < 0) return -1;
    if (pipe2(b, O_CLOEXEC) < 0) { close(a[0]); close(a[1]); return -1; }
    char ws[16], hs[16]; snprintf(ws, sizeof ws, "%d", maxw); snprintf(hs, sizeof hs, "%d", maxh);
    pid_t p = fork();
    if (p == 0) {
        dup2(a[0], 0); dup2(b[1], 1);
        int nul = open("/dev/null", O_WRONLY); if (nul >= 0) dup2(nul, 2);
        setenv("PULSE_SERVER", "unix:/run/user/0/pulse/native", 1);
        execl("/usr/bin/ccos-media", "ccos-media", file, ws, hs, m->shm_path, (char *)NULL);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    if (p < 0) { close(a[1]); close(b[0]); return -1; }
    m->pid = p; m->in = a[1]; m->out = b[0]; m->started = 1;
    fcntl(m->out, F_SETFL, O_NONBLOCK);
    return 0;
}
static void mp_cmd(MPlayer *m, const char *c) { if (m->in >= 0 && write(m->in, c, strlen(c)) < 0) {} }
/* read what it said; returns flags: 1 new frame, 2 info/meta/position, 4 ended */
static int mp_read(MPlayer *m) {
    int r = 0;
    if (m->out < 0) return 0;
    for (;;) {
        ssize_t n = read(m->out, m->buf + m->bl, sizeof m->buf - 1 - (size_t)m->bl);
        if (n == 0) { if (!m->ended) { m->ended = 1; if (!m->have_frame && !m->dur && !m->err[0]) { m->failed = 1; snprintf(m->err, sizeof m->err, "This file can't be played."); } } close(m->out); m->out = -1; r |= 4; break; }
        if (n < 0) break;
        m->bl += (int)n; m->buf[m->bl] = 0;
        char *line = m->buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            if (!strncmp(line, "INFO ", 5)) {
                sscanf(line + 5, "%d %d %lf %d %d", &m->w, &m->h, &m->dur, &m->has_video, &m->has_audio);
                m->shm_len = (size_t)m->w * m->h * 8;
                int fd = open(m->shm_path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) { void *p = mmap(NULL, m->shm_len, PROT_READ, MAP_SHARED, fd, 0); close(fd); if (p != MAP_FAILED) m->shm = p; }
                r |= 2;
            } else if (!strncmp(line, "META ", 5)) {
                char *t = line + 5, *b1 = strchr(t, '|'), *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
                if (b1) *b1 = 0;
                if (b2) *b2 = 0;
                mf_shown(t, m->title, sizeof m->title);
                if (b1) mf_shown(b1 + 1, m->artist, sizeof m->artist);
                if (b2) mf_shown(b2 + 1, m->album, sizeof m->album);
                r |= 2;
            } else if (!strncmp(line, "COVER", 5)) { m->cover = 1; m->slot = 0; r |= 1; }
            else if (!strncmp(line, "FRAME ", 6)) { m->slot = atoi(line + 6) & 1; m->have_frame = 1; r |= 1; }
            else if (!strncmp(line, "POS ", 4)) { m->pos = atof(line + 4); r |= 2; }
            else if (!strncmp(line, "END", 3)) { m->ended = 1; m->pos = m->dur; r |= 4; }
            else if (!strncmp(line, "ERR ", 4)) { snprintf(m->err, sizeof m->err, "%s", line + 4); if (strncmp(line + 4, "no sound", 8)) m->failed = 1; r |= 2; }
            line = nl + 1;
        }
        m->bl = (int)strlen(line); memmove(m->buf, line, (size_t)m->bl + 1);
    }
    return r;
}
static const uint32_t *mp_frame(const MPlayer *m) { return m->shm ? m->shm + (size_t)m->slot * m->w * m->h : NULL; }

/* a seek bar: returns nothing, but remembers where it is for clicks */
static void seek_bar(int x, int y, int w, double pos, double dur, int dark, int hot_) {
    fill_round(x, y, w, 6, 3, dark ? RGB(0x50, 0x58, 0x64) : RGB(0xd4, 0xda, 0xe2), 255);
    int px = dur > 0 ? (int)(w * fmin(1, pos / dur)) : 0;
    if (px > 0) fill_round(x, y, imax(6, px), 6, 3, RGB(0x3c, 0x94, 0xf4), 255);
    fill_circle(x + px, y + 3, hot_ ? 8 : 6, RGB(255, 255, 255), 255);
    if (!dark) stroke_round(x + px - (hot_ ? 8 : 6), y + 3 - (hot_ ? 8 : 6), (hot_ ? 16 : 12) + 1, (hot_ ? 16 : 12) + 1, hot_ ? 8 : 6, RGB(0x3c, 0x94, 0xf4), 255);
}

/* a list of files (the library page shared by the three apps) */
static void lib_draw(const MFiles *m, int cx, int cy, int cw, int ch, int scroll, int icon, const char *empty, int hotrow) {
    fill_rect(cx, cy, cw, ch, RGB(0xf7, 0xf9, 0xfc), 255);
    draw_text_bold(cx + 20, cy + 16, "Library", RGB(0x1e, 0x39, 0x87));
    draw_text(cx + 20, cy + 36, "Files from Documents, Images, Downloads, Videos and Music. Or open one in File Explorer.", RGB(0x70, 0x78, 0x84), 255);
    if (!m->n) { draw_text(cx + 20, cy + 76, empty, RGB(0x50, 0x58, 0x64), 255); return; }
    int sx0 = clip_y0; clip_y0 = imax(clip_y0, cy + 60);
    for (int i = 0; i < m->n; i++) {
        int y = cy + 64 + i * 40 - scroll;
        if (y + 40 < cy + 60 || y > cy + ch) continue;
        if (i == hotrow) fill_round(cx + 12, y, cw - 24, 38, 5, RGB(0xe4, 0xef, 0xfc), 255);
        draw_icon(icon, cx + 34, y + 19, 11);
        char nm[120]; mf_shown(mf_base(m->path[i]), nm, sizeof nm);
        sm_text(cx + 56, y + 3, nm, (cw - 80) / 8, RGB(0x18, 0x20, 0x2c), 1);
        char dir[300]; snprintf(dir, sizeof dir, "%s", m->path[i]); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
        char ds[120]; mf_shown(dir, ds, sizeof ds);
        sm_text(cx + 56, y + 20, ds, (cw - 80) / 8, RGB(0x80, 0x88, 0x94), 0);
    }
    clip_y0 = sx0;
}
static int lib_row_at(const MFiles *m, int ly, int scroll) {
    if (ly < 64) return -1;
    int i = (ly - 64 + scroll) / 40;
    return i >= 0 && i < m->n ? i : -1;
}

/* =================================================================== PHOTOS */
static struct {
    char path[300]; MFiles folder; MFiles lib; int lib_mode, lib_scroll;
    uint8_t *px; int w, h;                   /* the picture, RGBA */
    float zoom; int fit, panx, pany;         /* zoom 1 = real size */
    uint32_t *cache; int cw, ch, cache_zoomed_w, cache_zoomed_h; float cache_zoom;
    char err[120];
    int drag, dx0, dy0, px0, py0;
} ph = { .fit = 1, .zoom = 1 };
#define PH_BAR 52

static void photos_draw(int cx, int cy, int cw, int ch);
static void ph_free(void) { free(ph.px); ph.px = NULL; free(ph.cache); ph.cache = NULL; ph.w = ph.h = 0; }
static void ph_load(const char *path) {
    ph_free();
    ph.err[0] = 0; ph.fit = 1; ph.panx = ph.pany = 0;
    snprintf(ph.path, sizeof ph.path, "%s", path);
    size_t L = strlen(path);
    if (L > 4 && !strcasecmp(path + L - 4, ".svg")) {
        NSVGimage *im = nsvgParseFromFile(path, "px", 96);
        if (im && im->width > 0 && im->height > 0) {
            float sc = fminf(2048.0f / im->width, 2048.0f / im->height); if (sc > 4) sc = 4;
            ph.w = (int)(im->width * sc); ph.h = (int)(im->height * sc);
            ph.px = calloc((size_t)ph.w * ph.h, 4);
            NSVGrasterizer *rs = nsvgCreateRasterizer();
            if (ph.px && rs) nsvgRasterize(rs, im, 0, 0, sc, ph.px, ph.w, ph.h, ph.w * 4);
            if (rs) nsvgDeleteRasterizer(rs);
        }
        if (im) nsvgDelete(im);
    } else {
        int c; ph.px = stbi_load(path, &ph.w, &ph.h, &c, 4);
    }
    if (!ph.px) { ph.w = ph.h = 0; snprintf(ph.err, sizeof ph.err, "This picture can't be opened."); }
    mf_folder_of(&ph.folder, path, MK_IMAGE);
    ph.lib_mode = 0;
}
static float ph_fit_zoom(int cw, int ch) {
    if (!ph.w) return 1;
    float z = fminf((float)(cw - 20) / ph.w, (float)(ch - PH_BAR - 20) / ph.h);
    return z > 1 ? 1 : z;
}
/* the picture at the current zoom, made once (smooth when shrinking) */
static void ph_make_cache(float z) {
    int zw = imax(1, (int)(ph.w * z)), zh = imax(1, (int)(ph.h * z));
    if (ph.cache && ph.cache_zoom == z && ph.cache_zoomed_w == zw) return;
    free(ph.cache);
    if ((size_t)zw * zh > 40u * 1000 * 1000) { ph.cache = NULL; return; }
    ph.cache = malloc((size_t)zw * zh * 4);
    if (!ph.cache) return;
    ph.cache_zoom = z; ph.cache_zoomed_w = zw; ph.cache_zoomed_h = zh;
    for (int y = 0; y < zh; y++) {
        int sy0 = (int)((long)y * ph.h / zh), sy1 = imax(sy0 + 1, (int)((long)(y + 1) * ph.h / zh));
        if (sy1 - sy0 > 4) { int m = (sy0 + sy1) / 2; sy0 = m - 2; sy1 = m + 2; }    /* enough for a smooth look */
        for (int x = 0; x < zw; x++) {
            int sx0 = (int)((long)x * ph.w / zw), sx1 = imax(sx0 + 1, (int)((long)(x + 1) * ph.w / zw));
            if (sx1 - sx0 > 4) { int m = (sx0 + sx1) / 2; sx0 = m - 2; sx1 = m + 2; }
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t *p = ph.px + ((size_t)sy * ph.w + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            uint32_t al = a / n;
            ph.cache[(size_t)y * zw + x] = a ? (al << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a) : 0;
        }
    }
}
static int ph_bar_btn(int i, int cw) { return cw / 2 + (i - 2) * 48; }   /* 5 buttons centred */
static void photos_draw(int cx, int cy, int cw, int ch) {
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == photos_draw ? hot.b : -1;
    if (ph.lib_mode || !ph.path[0]) {
        lib_draw(&ph.lib, cx, cy, cw, ch, ph.lib_scroll, ICON_PHOTO, "No pictures found yet. Save some to Documents (D:) or Downloads.",
                 hotid >= 100 ? hotid - 100 : -1);
        return;
    }
    int ah = ch - PH_BAR;
    dark_keep(cx, cy, cw, ah);                         /* the picture area is already dark */
    /* dark, checkered-free background like a photo viewer */
    fill_rect(cx, cy, cw, ah, RGB(0x1e, 0x22, 0x28), 255);
    if (ph.px) {
        float z = ph.fit ? ph_fit_zoom(cw, ch) : ph.zoom;
        ph_make_cache(z);
        int zw = imax(1, (int)(ph.w * z)), zh = imax(1, (int)(ph.h * z));
        int x = cx + (cw - zw) / 2 + (ph.fit ? 0 : ph.panx), y = cy + (ah - zh) / 2 + (ph.fit ? 0 : ph.pany);
        int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;
        clip_x0 = imax(clip_x0, cx); clip_y0 = imax(clip_y0, cy); clip_x1 = imin(clip_x1, cx + cw); clip_y1 = imin(clip_y1, cy + ah);
        if (ph.cache) {
            int x0 = x, y0 = y, x1 = x + zw, y1 = y + zh;
            if (clip_range(&x0, &y0, &x1, &y1))
                for (int yy = y0; yy < y1; yy++) {
                    const uint32_t *s = ph.cache + (size_t)(yy - y) * zw - x;
                    uint32_t *d = &back[(size_t)yy * BW];
                    for (int xx = x0; xx < x1; xx++) {
                        uint32_t c = s[xx], a = c >> 24;
                        if (a == 255) d[xx] = c & 0xffffff;
                        else if (a) { uint32_t b = d[xx];
                            d[xx] = RGB((R(c) * a + R(b) * (255 - a)) / 255, (G(c) * a + G(b) * (255 - a)) / 255, (B(c) * a + B(b) * (255 - a)) / 255); }
                    }
                }
        }
        clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;
    } else draw_text(cx + (cw - text_w(ph.err)) / 2, cy + ah / 2 - 8, ph.err, RGB(0xd0, 0xd4, 0xda), 255);
    /* the bar */
    fill_rect(cx, cy + ah, cw, PH_BAR, RGB(0xee, 0xf1, 0xf5), 255);
    fill_rect(cx, cy + ah, cw, 1, RGB(0xc8, 0xd0, 0xda), 255);
    int kinds[5] = { MB_PREV, MB_ZOUT, MB_FIT, MB_ZIN, MB_NEXT };
    for (int i = 0; i < 5; i++) media_btn(cx + ph_bar_btn(i, cw), cy + ah + PH_BAR / 2, 17, kinds[i], hotid == i, 0);
    media_btn(cx + 30, cy + ah + PH_BAR / 2, 17, MB_LIB, hotid == 5, 0);
    if (ph.px) media_btn(cx + 72, cy + ah + PH_BAR / 2, 17, MB_PRINT, hotid == 6, 0);
    char nm[120], info[160]; mf_shown(mf_base(ph.path), nm, sizeof nm);
    int k = mf_index(&ph.folder, ph.path);
    float z = ph.fit ? ph_fit_zoom(cw, ch) : ph.zoom;
    snprintf(info, sizeof info, "%d of %d    %d x %d    %d%%", k + 1, ph.folder.n, ph.w, ph.h, (int)(z * 100 + 0.5f));
    int tx = cx + ph_bar_btn(4, cw) + 34, maxc = (cx + cw - tx - 12) / 8;
    sm_text(tx, cy + ah + 8, nm, maxc, RGB(0x18, 0x20, 0x2c), 1);
    sm_text(tx, cy + ah + 26, info, maxc, RGB(0x70, 0x78, 0x84), 0);
}
static void ph_title(void) {
    int i = win_of(photos_draw); if (i < 0) return;
    static char t[96]; char nm[80]; mf_shown(mf_base(ph.path), nm, sizeof nm);
    if (ph.path[0] && !ph.lib_mode) snprintf(t, sizeof t, "%s - Photos", nm); else snprintf(t, sizeof t, "Photos");
    wins[i].title = t; repaint_window(i); repaint_taskbar();
}
static void ph_step(int d) {
    if (!ph.folder.n) return;
    int k = mf_index(&ph.folder, ph.path);
    k = (k + d + ph.folder.n) % ph.folder.n;
    ph_load(ph.folder.path[k]); ph_title(); repaint_client(photos_draw);
}
static void ph_zoom(float f, int cw, int ch) {
    float z = ph.fit ? ph_fit_zoom(cw, ch) : ph.zoom;
    z *= f; if (z < 0.05f) z = 0.05f; if (z > 8) z = 8;
    ph.zoom = z; ph.fit = 0;
    ph.panx = (int)(ph.panx * f); ph.pany = (int)(ph.pany * f);
    repaint_client(photos_draw);
}
static int photos_hit(int lx, int ly, int cw, int ch) {
    if (ph.lib_mode || !ph.path[0]) { int r = lib_row_at(&ph.lib, ly, ph.lib_scroll); return r >= 0 ? 100 + r : -1; }
    int by = ch - PH_BAR / 2;
    for (int i = 0; i < 5; i++) { int dx = lx - ph_bar_btn(i, cw), dy = ly - by; if (dx * dx + dy * dy <= 18 * 18) return i; }
    { int dx = lx - 30, dy = ly - by; if (dx * dx + dy * dy <= 18 * 18) return 5; }
    { int dx = lx - 72, dy = ly - by; if (ph.px && dx * dx + dy * dy <= 18 * 18) return 6; }
    return -1;
}
static void photos_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(photos_draw); int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (id >= 100) { *x = 0; *y = 64 + (id - 100) * 40 - ph.lib_scroll; *w = cw; *h = 40; return; }
    *x = (id == 5 ? 30 : id == 6 ? 72 : ph_bar_btn(id, cw)) - 20; *y = ch - PH_BAR; *w = 40; *h = PH_BAR;
}
static void photos_click(int lx, int ly, int cw, int ch) {
    int id = photos_hit(lx, ly, cw, ch);
    if (id >= 100) { ph_load(ph.lib.path[id - 100]); ph_title(); repaint_client(photos_draw); return; }
    switch (id) {
    case 0: ph_step(-1); return;
    case 1: ph_zoom(1 / 1.25f, cw, ch); return;
    case 2: ph.fit = !ph.fit; if (!ph.fit) ph.zoom = 1; ph.panx = ph.pany = 0; repaint_client(photos_draw); return;
    case 3: ph_zoom(1.25f, cw, ch); return;
    case 4: ph_step(1); return;
    case 6: print_open(ph.path); return;
    case 5: ph.lib_mode = 1; mf_library(&ph.lib, MK_IMAGE); ph.lib_scroll = 0; ph_title(); repaint_client(photos_draw); return;
    }
    if (ly < ch - PH_BAR && !ph.fit) { ph.drag = 1; ph.dx0 = lx; ph.dy0 = ly; ph.px0 = ph.panx; ph.py0 = ph.pany; }
    static long long last; long long t = ui_ms();
    if (ly < ch - PH_BAR && t - last < 400) { int i = win_of(photos_draw); if (i >= 0) set_fullscreen(i, !wins[i].fullscreen); last = 0; return; }
    last = t;
}
static void photos_drag(int lx, int ly, int cw, int ch) {
    (void)cw; (void)ch;
    if (!ph.drag) return;
    ph.panx = ph.px0 + lx - ph.dx0; ph.pany = ph.py0 + ly - ph.dy0;
    repaint_client_rect(photos_draw, 0, 0, 10000, ch - PH_BAR);
}
static void photos_release(void) { ph.drag = 0; }
static void photos_wheel(int amount) {
    int i = win_of(photos_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (ph.lib_mode || !ph.path[0]) {
        ph.lib_scroll = clampi(ph.lib_scroll - amount * 80, 0, imax(0, ph.lib.n * 40 + 70 - ch));
        repaint_client(photos_draw); return;
    }
    ph_zoom(amount > 0 ? 1.15f : 1 / 1.15f, cw, ch);
}
static void photos_key(int code, int mods) {
    int i = win_of(photos_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (code == KEY_P && (mods & MOD_CTRL) && ph.px && !ph.lib_mode) { print_open(ph.path); return; }
    if (code == KEY_RIGHT || code == KEY_SPACE || code == KEY_PAGEDOWN) ph_step(1);
    else if (code == KEY_LEFT || code == KEY_BACKSPACE || code == KEY_PAGEUP) ph_step(-1);
    else if (code == KEY_EQUAL || code == KEY_KPPLUS) ph_zoom(1.25f, cw, ch);
    else if (code == KEY_MINUS || code == KEY_KPMINUS) ph_zoom(1 / 1.25f, cw, ch);
    else if (code == KEY_0 || code == KEY_KP0) { ph.fit = 1; repaint_client(photos_draw); }
    else if (code == KEY_ESC && wins[i].fullscreen) set_fullscreen(i, 0);
}
static void photos_resized(void) { free(ph.cache); ph.cache = NULL; }
static int photos_on_close(void) { ph_free(); ph.path[0] = 0; return 1; }
static void photos_open(const char *path) {
    if (path && *path) ph_load(path);
    else { ph.lib_mode = 1; mf_library(&ph.lib, MK_IMAGE); ph.lib_scroll = 0; }
}

/* =================================================================== VIDEOS */
static struct { MPlayer mp; char path[300]; MFiles lib; int lib_mode, lib_scroll; long long last_move; int seeking; } vd = { .mp = { .in = -1, .out = -1 } };
#define VD_BAR 56
static void videos_draw(int cx, int cy, int cw, int ch);
static int vd_full(void) { int i = win_of(videos_draw); return i >= 0 && wins[i].fullscreen; }
static int vd_bar_shown(void) { return !vd_full() || vd.mp.paused || vd.mp.ended || ui_ms() - vd.last_move < 2500; }
static void vd_title(void) {
    int i = win_of(videos_draw); if (i < 0) return;
    static char t[96]; char nm[80]; mf_shown(mf_base(vd.path), nm, sizeof nm);
    if (vd.path[0] && !vd.lib_mode) snprintf(t, sizeof t, "%s - Videos", nm); else snprintf(t, sizeof t, "Videos");
    wins[i].title = t; repaint_window(i); repaint_taskbar();
}
static void vd_play(const char *path, double at) {
    snprintf(vd.path, sizeof vd.path, "%s", path);
    vd.lib_mode = 0;
    mp_start(&vd.mp, path, imin(W, 1920), imin(H, 1080));
    if (at > 0) { char c[32]; snprintf(c, sizeof c, "s %.2f\n", at); mp_cmd(&vd.mp, c); }
    vd_title();
}
static void videos_draw(int cx, int cy, int cw, int ch) {
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == videos_draw ? hot.b : -1;
    if (vd.lib_mode || !vd.path[0]) {
        lib_draw(&vd.lib, cx, cy, cw, ch, vd.lib_scroll, ICON_VIDEO, "No videos found yet. Save some to Documents (D:) or Downloads.", hotid >= 100 ? hotid - 100 : -1);
        return;
    }
    int full = vd_full(), bar = vd_bar_shown();
    int ah = full ? ch : ch - VD_BAR;
    fill_rect(cx, cy, cw, ah, RGB(0, 0, 0), 255);
    const MPlayer *m = &vd.mp;
    if (m->have_frame && m->shm) {
        double sc = fmin((double)cw / m->w, (double)ah / m->h);
        int dw = (int)(m->w * sc), dh = (int)(m->h * sc);
        int sx0 = clip_y1; clip_y1 = imin(clip_y1, cy + ah);
        blit_scaled(mp_frame(m), m->w, m->h, cx + (cw - dw) / 2, cy + (ah - dh) / 2, dw, dh);
        clip_y1 = sx0;
    } else {
        const char *s = m->failed ? m->err : "Loading...";
        draw_text(cx + (cw - text_w(s)) / 2, cy + ah / 2 - 8, s, RGB(0xc8, 0xcc, 0xd2), 255);
        if (!m->failed && !m->has_video && m->dur > 0) draw_text(cx + (cw - 8 * 22) / 2, cy + ah / 2 + 14, "(sound only, no picture)", RGB(0x90, 0x98, 0xa0), 255);
    }
    if (!bar) return;
    int by = cy + ch - VD_BAR;
    if (full) fill_rect(cx, by, cw, VD_BAR, RGB(0, 0, 0), 170);
    else fill_rect(cx, by, cw, VD_BAR, RGB(0x18, 0x1c, 0x22), 255);
    media_btn(cx + 34, by + VD_BAR / 2, 19, (m->paused || m->ended || !m->started) ? MB_PLAY : MB_PAUSE, hotid == 0, 1);
    char a[24], b[24], t[56]; fmt_time(m->pos, a, sizeof a); fmt_time(m->dur, b, sizeof b);
    snprintf(t, sizeof t, "%s / %s", a, b);
    draw_text(cx + 64, by + VD_BAR / 2 - 8, t, RGB(0xe0, 0xe4, 0xea), 255);
    int sx = cx + 64 + text_w(t) + 18, sw = cw - (sx - cx) - 70;
    seek_bar(sx, by + VD_BAR / 2 - 3, sw, m->pos, m->dur, 1, hotid == 1 || vd.seeking);
    media_btn(cx + cw - 34, by + VD_BAR / 2, 17, MB_FULL, hotid == 2, 1);
}
static void vd_seek_geom(int cw, int *sx, int *sw) {
    char a[24], b[24], t[56]; fmt_time(vd.mp.pos, a, sizeof a); fmt_time(vd.mp.dur, b, sizeof b);
    snprintf(t, sizeof t, "%s / %s", a, b);
    *sx = 64 + text_w(t) + 18; *sw = cw - *sx - 70;
}
static int videos_hit(int lx, int ly, int cw, int ch) {
    if (vd.lib_mode || !vd.path[0]) { int r = lib_row_at(&vd.lib, ly, vd.lib_scroll); return r >= 0 ? 100 + r : -1; }
    if (!vd_bar_shown() || ly < ch - VD_BAR) return -1;
    if (lx < 58) return 0;
    if (lx > cw - 56) return 2;
    int sx, sw; vd_seek_geom(cw, &sx, &sw);
    if (lx >= sx - 8 && lx <= sx + sw + 8) return 1;
    return -1;
}
static void videos_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(videos_draw); int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (id >= 100) { *x = 0; *y = 64 + (id - 100) * 40 - vd.lib_scroll; *w = cw; *h = 40; return; }
    *x = 0; *y = ch - VD_BAR; *w = cw; *h = VD_BAR;
}
static void vd_toggle(void) {
    MPlayer *m = &vd.mp;
    if (m->ended || !m->started) { vd_play(vd.path, 0); return; }
    m->paused = !m->paused; mp_cmd(m, "p\n");
    repaint_client(videos_draw);
}
static void vd_seek_to(double t) {
    MPlayer *m = &vd.mp;
    if (m->dur <= 0) return;
    t = fmax(0, fmin(t, m->dur - 0.5));
    if (m->ended || m->out < 0) { vd_play(vd.path, t); return; }
    char c[32]; snprintf(c, sizeof c, "s %.2f\n", t); mp_cmd(m, c);
    m->pos = t;
    repaint_client_rect(videos_draw, 0, 0, 10000, 10000);
}
static void videos_drag(int lx, int ly, int cw, int ch) {
    (void)ly; (void)ch;
    if (!vd.seeking) return;
    int sx, sw; vd_seek_geom(cw, &sx, &sw);
    vd.mp.pos = fmax(0, fmin(1, (double)(lx - sx) / sw)) * vd.mp.dur;
    repaint_client(videos_draw);
}
static void videos_release(void) {
    if (vd.seeking) { vd.seeking = 0; vd_seek_to(vd.mp.pos); }
}
static void videos_click(int lx, int ly, int cw, int ch) {
    int id = videos_hit(lx, ly, cw, ch);
    vd.last_move = ui_ms();
    if (id >= 100) { vd_play(vd.lib.path[id - 100], 0); repaint_client(videos_draw); return; }
    if (id == 0) { vd_toggle(); return; }
    if (id == 2) { int i = win_of(videos_draw); if (i >= 0) set_fullscreen(i, !wins[i].fullscreen); return; }
    if (id == 1) { vd.seeking = 1; videos_drag(lx, ly, cw, ch); return; }
    if (vd.lib_mode || !vd.path[0]) return;
    static long long last; long long t = ui_ms();
    if (t - last < 400) { int i = win_of(videos_draw); if (i >= 0) set_fullscreen(i, !wins[i].fullscreen); last = 0; return; }
    last = t;
    vd_toggle();                                         /* a click on the picture: pause / play */
}
static void videos_motion(int lx, int ly, int inside) {
    (void)lx; (void)ly;
    if (!inside || !vd_full()) return;
    int was = vd_bar_shown();
    vd.last_move = ui_ms();
    if (!was) repaint_client(videos_draw);
}
static void videos_wheel(int amount) {
    int i = win_of(videos_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (vd.lib_mode || !vd.path[0]) { vd.lib_scroll = clampi(vd.lib_scroll - amount * 80, 0, imax(0, vd.lib.n * 40 + 70 - ch)); repaint_client(videos_draw); return; }
    vd_seek_to(vd.mp.pos + (amount > 0 ? 5 : -5));
}
static void videos_key(int code, int mods) {
    (void)mods;
    int i = win_of(videos_draw); if (i < 0) return;
    if (code == KEY_SPACE || code == KEY_K) vd_toggle();
    else if (code == KEY_RIGHT || code == KEY_L) vd_seek_to(vd.mp.pos + (code == KEY_L ? 10 : 5));
    else if (code == KEY_LEFT || code == KEY_J) vd_seek_to(vd.mp.pos - (code == KEY_J ? 10 : 5));
    else if (code == KEY_F) set_fullscreen(i, !wins[i].fullscreen);
    else if (code == KEY_ESC && wins[i].fullscreen) set_fullscreen(i, 0);
    else if (code == KEY_HOME) vd_seek_to(0);
}
static int videos_fd(void) { return vd.mp.out; }
static void videos_wake(void) {
    int r = mp_read(&vd.mp);
    int i = win_of(videos_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (r & 1) repaint_client_rect(videos_draw, 0, 0, cw, vd_full() ? ch : ch - VD_BAR);
    if (r & 6) { if (r & 4) vd.mp.paused = 0; repaint_client_rect(videos_draw, 0, ch - VD_BAR, cw, VD_BAR); }
    if (r & 4) repaint_client(videos_draw);
}
static void videos_tick(void) {
    if (vd_full() && !vd_bar_shown()) repaint_client(videos_draw);   /* hide the bar */
}
static int videos_on_close(void) { mp_stop(&vd.mp); vd.path[0] = 0; return 1; }
static void videos_open(const char *path) {
    if (path && *path) vd_play(path, 0);
    else { mp_stop(&vd.mp); vd.path[0] = 0; vd.lib_mode = 1; mf_library(&vd.lib, MK_VIDEO); vd.lib_scroll = 0; }
}

/* =================================================================== MUSIC */
static struct { MPlayer mp; char path[300]; MFiles list; int scroll, seeking, hotrow; } mu = { .mp = { .in = -1, .out = -1 } };
#define MU_TOP 196                          /* the player part; the list is below */
static void music_draw(int cx, int cy, int cw, int ch);
static void mu_title(void) {
    int i = win_of(music_draw); if (i < 0) return;
    static char t[96];
    if (mu.path[0]) { char nm[80]; mf_shown(mu.mp.title[0] ? mu.mp.title : mf_base(mu.path), nm, sizeof nm); snprintf(t, sizeof t, "%s - Music", mu.mp.title[0] ? mu.mp.title : nm); }
    else snprintf(t, sizeof t, "Music");
    wins[i].title = t; repaint_window(i); repaint_taskbar();
}
static void mu_play(const char *path, double at) {
    snprintf(mu.path, sizeof mu.path, "%s", path);
    mp_start(&mu.mp, path, 152, 152);
    if (at > 0) { char c[32]; snprintf(c, sizeof c, "s %.2f\n", at); mp_cmd(&mu.mp, c); }
    mu_title();
}
static void music_draw(int cx, int cy, int cw, int ch) {
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == music_draw ? hot.b : -1;
    const MPlayer *m = &mu.mp;
    dark_keep(cx, cy, cw, MU_TOP);                     /* the player part has its own colours */
    /* the player: a warm gradient */
    for (int j = 0; j < MU_TOP; j++) {
        int t = j * 255 / MU_TOP;
        fill_rect(cx, cy + j, cw, 1, RGB(0x3a - t / 16, 0x2a - t / 20, 0x5a - t / 10), 255);
    }
    int ax = cx + 22, ay = cy + 22, as_ = 152;
    if (m->cover && m->shm) blit_scaled(m->shm, m->w, m->h, ax, ay, as_, as_);
    else {
        fill_round_grad(ax, ay, as_, as_, 10, RGB(0xff, 0x8a, 0x4c), RGB(0xc8, 0x3c, 0x8c), 255);
        draw_music_icon(ax + as_ / 2, ay + as_ / 2, 46);
    }
    int tx = ax + as_ + 22, maxc = (cx + cw - tx - 16) / 8;
    if (mu.path[0]) {
        char nm[120]; mf_shown(mf_base(mu.path), nm, sizeof nm);
        sm_text(tx, ay + 4, m->title[0] ? m->title : nm, maxc, RGB(255, 255, 255), 1);
        sm_text(tx, ay + 26, m->artist[0] ? m->artist : "Unknown artist", maxc, RGB(0xd8, 0xd0, 0xe8), 0);
        if (m->album[0]) sm_text(tx, ay + 46, m->album, maxc, RGB(0xb0, 0xa8, 0xc8), 0);
        if (m->failed) sm_text(tx, ay + 66, m->err, maxc, RGB(0xff, 0xb0, 0xa0), 0);
    } else sm_text(tx, ay + 4, "Pick a song below", maxc, RGB(255, 255, 255), 1);
    /* seek bar and buttons */
    int sw = cx + cw - tx - 24;
    char a[24], b[24]; fmt_time(m->pos, a, sizeof a); fmt_time(m->dur, b, sizeof b);
    seek_bar(tx, ay + 92, sw, m->pos, m->dur, 1, hotid == 3 || mu.seeking);
    draw_text(tx, ay + 104, a, RGB(0xc8, 0xc0, 0xd8), 255);
    draw_text(tx + sw - text_w(b), ay + 104, b, RGB(0xc8, 0xc0, 0xd8), 255);
    int bcx = tx + sw / 2, bcy = ay + 140;
    media_btn(bcx - 56, bcy, 17, MB_PREV, hotid == 0, 1);
    media_btn(bcx, bcy, 22, (m->paused || m->ended || !m->started) ? MB_PLAY : MB_PAUSE, hotid == 1, 1);
    media_btn(bcx + 56, bcy, 17, MB_NEXT, hotid == 2, 1);
    /* the list: songs in this folder (or the library) */
    int ly0 = cy + MU_TOP;
    fill_rect(cx, ly0, cw, ch - MU_TOP, RGB(0xf7, 0xf6, 0xfb), 255);
    if (!mu.list.n) { draw_text(cx + 20, ly0 + 20, "No songs found. Save some to Documents (D:), Downloads or Music.", RGB(0x60, 0x68, 0x74), 255); return; }
    int sx0 = clip_y0; clip_y0 = imax(clip_y0, ly0);
    for (int i = 0; i < mu.list.n; i++) {
        int y = ly0 + 6 + i * 34 - mu.scroll;
        if (y + 34 < ly0 || y > cy + ch) continue;
        int cur = !strcmp(mu.list.path[i], mu.path);
        if (cur) fill_round(cx + 10, y, cw - 20, 32, 5, RGB(0xe8, 0xe0, 0xf6), 255);
        else if (hotid == 100 + i) fill_round(cx + 10, y, cw - 20, 32, 5, RGB(0xee, 0xec, 0xf6), 255);
        char nm[120]; mf_shown(mf_base(mu.list.path[i]), nm, sizeof nm);
        char *dot = strrchr(nm, '.'); if (dot) *dot = 0;
        char num[8]; snprintf(num, sizeof num, "%d", i + 1);
        draw_text(cx + 22, y + 8, num, RGB(0x98, 0x90, 0xa8), 255);
        sm_text(cx + 58, y + 8, nm, (cw - 80) / 8, cur ? RGB(0x5a, 0x2a, 0x9a) : RGB(0x20, 0x20, 0x2c), cur);
    }
    clip_y0 = sx0;
}
static int music_hit(int lx, int ly, int cw, int ch) {
    (void)ch;
    int tx = 22 + 152 + 22, sw = cw - tx - 24, bcx = tx + sw / 2, bcy = 22 + 140;
    if (ly < MU_TOP) {
        int cxs[3] = { bcx - 56, bcx, bcx + 56 };
        for (int i = 0; i < 3; i++) { int dx = lx - cxs[i], dy = ly - bcy; if (dx * dx + dy * dy <= 24 * 24) return i; }
        if (ly >= 22 + 84 && ly <= 22 + 102 && lx >= tx - 8 && lx <= tx + sw + 8) return 3;
        return -1;
    }
    int i = (ly - MU_TOP - 6 + mu.scroll) / 34;
    return i >= 0 && i < mu.list.n ? 100 + i : -1;
}
static void music_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(music_draw); int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (id >= 100) { *x = 0; *y = MU_TOP + 6 + (id - 100) * 34 - mu.scroll; *w = cw; *h = 34; if (*y < MU_TOP) { *h -= MU_TOP - *y; *y = MU_TOP; } return; }
    *x = 0; *y = 0; *w = cw; *h = MU_TOP;
}
static void mu_step(int d) {
    if (!mu.list.n) return;
    int k = mf_index(&mu.list, mu.path);
    k = k < 0 ? 0 : (k + d + mu.list.n) % mu.list.n;
    mu_play(mu.list.path[k], 0);
    repaint_client(music_draw);
}
static void mu_toggle(void) {
    MPlayer *m = &mu.mp;
    if (!mu.path[0]) { if (mu.list.n) mu_play(mu.list.path[0], 0); repaint_client(music_draw); return; }
    if (m->ended || !m->started) { mu_play(mu.path, 0); repaint_client(music_draw); return; }
    m->paused = !m->paused; mp_cmd(m, "p\n");
    repaint_client_rect(music_draw, 0, 0, 10000, MU_TOP);
}
static void mu_seek_to(double t) {
    MPlayer *m = &mu.mp;
    if (m->dur <= 0) return;
    t = fmax(0, fmin(t, m->dur - 0.5));
    if (m->ended || m->out < 0) { mu_play(mu.path, t); return; }
    char c[32]; snprintf(c, sizeof c, "s %.2f\n", t); mp_cmd(m, c);
    m->pos = t;
    repaint_client_rect(music_draw, 0, 0, 10000, MU_TOP);
}
static void music_drag(int lx, int ly, int cw, int ch) {
    (void)ly; (void)ch;
    if (!mu.seeking) return;
    int tx = 22 + 152 + 22, sw = cw - tx - 24;
    mu.mp.pos = fmax(0, fmin(1, (double)(lx - tx) / sw)) * mu.mp.dur;
    repaint_client_rect(music_draw, 0, 0, 10000, MU_TOP);
}
static void music_release(void) { if (mu.seeking) { mu.seeking = 0; mu_seek_to(mu.mp.pos); } }
static void music_click(int lx, int ly, int cw, int ch) {
    int id = music_hit(lx, ly, cw, ch);
    if (id == 0) mu_step(-1);
    else if (id == 1) mu_toggle();
    else if (id == 2) mu_step(1);
    else if (id == 3) { mu.seeking = 1; music_drag(lx, ly, cw, ch); }
    else if (id >= 100) { mu_play(mu.list.path[id - 100], 0); repaint_client(music_draw); }
}
static void music_wheel(int amount) {
    int i = win_of(music_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    mu.scroll = clampi(mu.scroll - amount * 68, 0, imax(0, mu.list.n * 34 + 12 - (ch - MU_TOP)));
    repaint_client_rect(music_draw, 0, MU_TOP, cw, ch - MU_TOP);
}
static void music_key(int code, int mods) {
    (void)mods;
    if (code == KEY_SPACE || code == KEY_K) mu_toggle();
    else if (code == KEY_RIGHT) mu_seek_to(mu.mp.pos + 5);
    else if (code == KEY_LEFT) mu_seek_to(mu.mp.pos - 5);
    else if (code == KEY_DOWN || code == KEY_N) mu_step(1);
    else if (code == KEY_UP || code == KEY_P) mu_step(-1);
}
static int music_fd(void) { return mu.mp.out; }
static void music_wake(void) {
    int r = mp_read(&mu.mp);
    if (r & 2) mu_title();
    if (r) repaint_client_rect(music_draw, 0, 0, 10000, MU_TOP);
    if ((r & 4) && !mu.mp.failed && mu.list.n > 1) {      /* next song */
        int k = mf_index(&mu.list, mu.path);
        if (k >= 0 && k + 1 < mu.list.n) mu_step(1);
    } else if ((r & 4) && mu.mp.failed && mu.list.n > 1) {}
}
static int music_on_close(void) { mp_stop(&mu.mp); mu.path[0] = 0; return 1; }
static void music_open(const char *path) {
    if (path && *path) {
        mf_folder_of(&mu.list, path, MK_AUDIO);
        if (mu.list.n <= 1) { MFiles lib; mf_library(&lib, MK_AUDIO); if (lib.n > mu.list.n && mf_index(&lib, path) >= 0) mu.list = lib; }
        mu_play(path, 0);
    } else {
        mf_library(&mu.list, MK_AUDIO);
        if (!mu.path[0]) mu.mp = (MPlayer){ .in = -1, .out = -1 };
    }
    mu.scroll = 0;
}

/* ---- icons ------------------------------------------------------------ */
static void draw_photo_icon(int cx, int cy, int r) {          /* a landscape picture */
    int w = r * 2, h = r * 3 / 2 + 2, x = cx - r, y = cy - h / 2;
    fill_round(x, y, w, h, imax(1, r / 6), RGB(0xf4, 0xf6, 0xf8), 255);
    int ix = x + imax(1, r / 6), iy = y + imax(1, r / 6), iw = w - 2 * imax(1, r / 6), ih = h - 2 * imax(1, r / 6);
    for (int j = 0; j < ih; j++) fill_rect(ix, iy + j, iw, 1, RGB(0x6c + j * 40 / imax(1, ih), 0xb4 + j * 30 / imax(1, ih), 0xf4), 255);
    fill_circle(ix + iw * 3 / 4, iy + ih / 3, imax(1, r / 5), RGB(0xff, 0xd8, 0x40), 255);
    for (int k = 0; k < iw; k++) {                               /* hills */
        float t = (float)k / iw;
        int hh = (int)(ih * (0.38f + 0.18f * sinf(t * 6.0f + 0.5f)));
        fill_rect(ix + k, iy + ih - hh, 1, hh, RGB(0x3c, 0xa0 + (int)(30 * t), 0x48), 255);
    }
    stroke_round(x, y, w, h, imax(1, r / 6), RGB(0x88, 0x94, 0xa4), 255);
}
static void draw_video_icon(int cx, int cy, int r) {           /* a film frame with a play button */
    int w = r * 2, h = r * 3 / 2 + 2, x = cx - r, y = cy - h / 2;
    fill_round_grad(x, y, w, h, imax(1, r / 5), RGB(0x46, 0x4e, 0x5c), RGB(0x1c, 0x20, 0x28), 255);
    for (int k = 0; k < 5; k++) {                                 /* film holes */
        int hx = x + 2 + k * (w - 4) / 5 + (w - 4) / 10 - 1;
        fill_rect(hx, y + 2, imax(1, r / 6), imax(1, r / 7), RGB(0xd0, 0xd4, 0xda), 255);
        fill_rect(hx, y + h - 2 - imax(1, r / 7), imax(1, r / 6), imax(1, r / 7), RGB(0xd0, 0xd4, 0xda), 255);
    }
    int s = r / 2;
    for (int yy = -s; yy <= s; yy++) fill_rect(cx - s / 2, cy + yy, (s - abs(yy)) * 9 / 10 + 1, 1, RGB(0xff, 0x5a, 0x4a), 255);
}
static void draw_music_icon(int cx, int cy, int r) {           /* two joined notes */
    uint32_t c = RGB(255, 255, 255), sh = RGB(0x60, 0x20, 0x70);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t col = pass ? c : sh; int o = pass ? 0 : imax(1, r / 12);
        float hr = r * 0.30f;
        fill_circle(cx - r * 0.45f + o, cy + r * 0.50f + o, hr, col, 255);
        fill_circle(cx + r * 0.45f + o, cy + r * 0.35f + o, hr, col, 255);
        int st = imax(2, r / 7);
        fill_rect((int)(cx - r * 0.45f + hr - st) + o, (int)(cy - r * 0.55f) + o, st, (int)(r * 1.05f), col, 255);
        fill_rect((int)(cx + r * 0.45f + hr - st) + o, (int)(cy - r * 0.70f) + o, st, (int)(r * 1.05f), col, 255);
        int bx0 = (int)(cx - r * 0.45f + hr - st), bx1 = (int)(cx + r * 0.45f + hr), by0 = (int)(cy - r * 0.55f), by1 = (int)(cy - r * 0.70f);
        for (int x = bx0; x < bx1; x++) {                       /* the beam joining them */
            int y = by0 + (by1 - by0) * (x - bx0) / imax(1, bx1 - bx0);
            fill_rect(x + o, y + o, 1, imax(2, r / 4), col, 255);
        }
    }
}
/* the start menu / desktop version of the music icon: a coloured circle */
static void draw_music_app_icon(int cx, int cy, int r) {
    fill_circle(cx, cy, r, RGB(0xc8, 0x3c, 0x8c), 255);
    fill_circle(cx - r / 5, cy - r / 4, r * 0.7f, RGB(0xff, 0x7a, 0x50), 140);
    draw_music_icon(cx, cy, r * 3 / 4);
}
