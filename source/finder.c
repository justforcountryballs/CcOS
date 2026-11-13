/* ===================================================================
 *  Finder: the Dock's first icon. Everything in one window, in tabs:
 *  Apps, Files, Images, Videos, Music, Downloads. Type to search the
 *  tab; click something to open it. Pictures show small previews,
 *  made by a helper thread so the window never waits for the stick.
 *  The file list is the Start menu search's (menusearch.c).
 * =================================================================== */
#define FD_W 860
#define FD_H 560
#define FD_SIDE 176                       /* the tabs, on the left */
#define FD_TOP 58                         /* the search box, above the grid */
#define FD_TW 118                         /* one tile */
#define FD_TH 112
#define FD_PW 96                          /* a preview, at most */
#define FD_PH 66
enum { FT_APPS, FT_FILES, FT_IMAGES, FT_VIDEOS, FT_MUSIC, FT_DOWNLOADS, FT_N };
static const char *fd_tab_name[FT_N] = { "Apps", "Files", "Images", "Videos", "Music", "Downloads" };
static const int fd_tab_icon[FT_N] = { ICON_BAG, ICON_FILE, ICON_PHOTO, ICON_VIDEO, ICON_MUSIC, ICON_FOLDER };
#define FD_MAX 4000
static struct {
    int tab, scroll, n, focus_search;
    char q[64];
    struct { char label[64]; char path[300]; int icon, app; long long t; } *it;
} fd;

static void finder_draw(int cx, int cy, int cw, int ch);
static int finder_running(void) { return win_of(finder_draw) >= 0; }

/* ---- previews: a little cache, filled by a thread ---- */
#define FD_NTH 192
static struct { char path[300]; uint32_t *px; int w, h, state; long long used; } fd_th[FD_NTH];   /* state: 0 free, 1 asked, 2 ready, 3 failed */
static pthread_mutex_t fd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fd_cv = PTHREAD_COND_INITIALIZER;
static int fd_thread_on;
static void *fd_thumb_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&fd_mu);
        int j = -1;
        for (;;) {
            for (int i = 0; i < FD_NTH; i++) if (fd_th[i].state == 1) { j = i; break; }
            if (j >= 0) break;
            pthread_cond_wait(&fd_cv, &fd_mu);
        }
        char path[300]; snprintf(path, sizeof path, "%s", fd_th[j].path);
        pthread_mutex_unlock(&fd_mu);
        int iw = 0, ih = 0, c; uint8_t *src = stbi_load(path, &iw, &ih, &c, 4);
        uint32_t *px = NULL; int w = 0, h = 0;
        if (src && iw > 0 && ih > 0) {
            float s = fminf((float)FD_PW / iw, (float)FD_PH / ih);
            if (s > 1) s = 1;
            w = imax(1, (int)(iw * s)); h = imax(1, (int)(ih * s));
            px = malloc((size_t)w * h * 4);
            if (px)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {                /* average the pixels each one covers */
                        int x0 = x * iw / w, x1 = imax(x0 + 1, (x + 1) * iw / w), y0 = y * ih / h, y1 = imax(y0 + 1, (y + 1) * ih / h);
                        int stx = imax(1, (x1 - x0) / 4), sty = imax(1, (y1 - y0) / 4);
                        unsigned r = 0, g = 0, b = 0, n = 0;
                        for (int yy = y0; yy < y1; yy += sty) for (int xx = x0; xx < x1; xx += stx) {
                            const uint8_t *q = src + ((size_t)yy * iw + xx) * 4; r += q[0]; g += q[1]; b += q[2]; n++;
                        }
                        px[y * w + x] = RGB(r / n, g / n, b / n);
                    }
        }
        if (src) stbi_image_free(src);
        pthread_mutex_lock(&fd_mu);
        if (fd_th[j].state == 1 && !strcmp(fd_th[j].path, path)) {
            fd_th[j].px = px; fd_th[j].w = w; fd_th[j].h = h; fd_th[j].state = px ? 2 : 3;
        } else free(px);
        pthread_mutex_unlock(&fd_mu);
        if (wake_pipe[1] >= 0) { if (write(wake_pipe[1], "t", 1) < 0) {} }
    }
    return NULL;
}
/* the preview of a picture: ready (draw it), or asked for */
/* (only the main thread asks and throws old ones out, so a ready one stays put while it's drawn) */
static int fd_thumb(const char *path) {
    int free_i = -1, old = -1;
    pthread_mutex_lock(&fd_mu);
    for (int i = 0; i < FD_NTH; i++) {
        if (fd_th[i].state && !strcmp(fd_th[i].path, path)) { fd_th[i].used = ui_ms(); int r = fd_th[i].state == 2 ? i : -1; pthread_mutex_unlock(&fd_mu); return r; }
        if (!fd_th[i].state && free_i < 0) free_i = i;
        if (fd_th[i].state >= 2 && (old < 0 || fd_th[i].used < fd_th[old].used)) old = i;
    }
    pthread_mutex_unlock(&fd_mu);
    if (free_i < 0) free_i = old;
    if (free_i < 0) return -1;
    if (!fd_thread_on) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, fd_thumb_thread, NULL) == 0) fd_thread_on = 1;
        pthread_attr_destroy(&at);
        if (!fd_thread_on) return -1;
    }
    pthread_mutex_lock(&fd_mu);
    free(fd_th[free_i].px); fd_th[free_i].px = NULL;
    snprintf(fd_th[free_i].path, sizeof fd_th[0].path, "%s", path);
    fd_th[free_i].state = 1; fd_th[free_i].used = ui_ms();
    pthread_cond_signal(&fd_cv);
    pthread_mutex_unlock(&fd_mu);
    return -1;
}

/* ---- what the tab shows ---- */
static int fd_user_file(const char *p) {       /* the user's own files: in a folder like Documents, or Downloads */
    const char *dl = dl_dir(); size_t dn = strlen(dl);
    if (!strncmp(p, dl, dn) && p[dn] == '/') return 2;
    return !strncmp(p, "/data/", 6) && p[6] >= 'A' && p[6] <= 'Z' && strchr(p + 6, '/');
}
static int fd_cmp(const void *a, const void *b) {
    const typeof(*fd.it) *x = a, *y = b;
    if (x->app) return strcasecmp(x->label, y->label);
    return x->t < y->t ? 1 : x->t > y->t ? -1 : 0;           /* newest first */
}
static void fd_add(const char *label, const char *path, int icon, int app, long long t) {
    if (fd.n >= FD_MAX) return;
    if (!fd.it && !(fd.it = calloc(FD_MAX, sizeof *fd.it))) return;
    if (fd.q[0] && !strcasestr(label, fd.q)) return;
    for (int i = 0; app && i < fd.n; i++) if (!strcmp(fd.it[i].path, path)) return;
    snprintf(fd.it[fd.n].label, sizeof fd.it[0].label, "%s", label);
    snprintf(fd.it[fd.n].path, sizeof fd.it[0].path, "%s", path);
    fd.it[fd.n].icon = icon; fd.it[fd.n].app = app; fd.it[fd.n].t = t;
    fd.n++;
}
static void fd_build(void) {
    fd.n = 0;
    if (fd.tab == FT_APPS) {
        for (int i = 0; i < (int)(sizeof apps / sizeof apps[0]); i++) fd_add(apps[i].label, apps[i].cmd, apps[i].icon, 1, 0);
        for (int i = 0; i < nfp; i++) fd_add(fp_info[i].label, fp_info[i].cmd, fp_info[i].icon, 1, 0);
    } else {
        for (int i = 0; i < ms_nfiles; i++) {
            if (ms_files[i][0] != 'f') continue;
            const char *p = ms_files[i] + 1, *nm = strrchr(p, '/'); nm = nm ? nm + 1 : p;
            int uf = fd_user_file(p); if (!uf) continue;
            int mk = media_kind(nm);
            if (fd.tab == FT_IMAGES && mk != MK_IMAGE) continue;
            if (fd.tab == FT_VIDEOS && mk != MK_VIDEO) continue;
            if (fd.tab == FT_MUSIC && mk != MK_AUDIO) continue;
            if (fd.tab == FT_DOWNLOADS && uf != 2 && strncmp(p, "/data/Downloads/", 16)) continue;
            int icon = mk == MK_IMAGE ? ICON_PHOTO : mk == MK_VIDEO ? ICON_VIDEO : mk == MK_AUDIO ? ICON_MUSIC : ex_is_archive(nm) ? ICON_ZIP : ICON_FILE;
            fd_add(nm, p, icon, 0, ms_mtime ? ms_mtime[i] : 0);
        }
    }
    if (fd.n > 1) qsort(fd.it, (size_t)fd.n, sizeof *fd.it, fd_cmp);
    int cols = imax(1, (FD_W - FD_SIDE - 24) / FD_TW), rows = (fd.n + cols - 1) / cols;
    fd.scroll = imin(fd.scroll, imax(0, rows * FD_TH - (FD_H - FD_TOP - 16)));
}
/* the Start menu search's file list got read again */
static void finder_files_changed(void) { if (finder_running()) { fd_build(); repaint_client(finder_draw); } }

static void fd_grid(int cw, int ch, int *gx, int *gy, int *cols, int *gh) {
    *gx = FD_SIDE + 12; *gy = FD_TOP; *cols = imax(1, (cw - FD_SIDE - 24) / FD_TW); *gh = ch - FD_TOP - 4;
}
static void finder_draw(int cx, int cy, int cw, int ch) {
    const int dark = 0;   /* (in dark mode, ccos darkens windows like this one itself) */
    uint32_t bg = dark ? RGB(0x20, 0x21, 0x24) : RGB(255, 255, 255), side = dark ? RGB(0x2a, 0x2b, 0x30) : RGB(0xf2, 0xf4, 0xf8);
    uint32_t text = dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28), grey = dark ? RGB(0x9a, 0xa0, 0xaa) : RGB(0x70, 0x78, 0x84);
    fill_rect(cx, cy, cw, ch, bg, 255);
    fill_rect(cx, cy, FD_SIDE, ch, side, 255);
    fill_rect(cx + FD_SIDE, cy, 1, ch, dark ? RGB(0x3a, 0x3c, 0x42) : RGB(0xdc, 0xe0, 0xe6), 255);
    for (int i = 0; i < FT_N; i++) {                          /* the tabs */
        int ty = cy + 14 + i * 38, on = i == fd.tab, hv = hot.type == HOT_CONTENT && wins[hot.a].draw == finder_draw && hot.b == 1000 + i;
        if (on || hv) fill_round(cx + 8, ty, FD_SIDE - 16, 32, 7, on ? (dark ? RGB(0x3a, 0x5a, 0x8a) : RGB(0xd6, 0xe6, 0xfb)) : (dark ? RGB(0x34, 0x36, 0x3c) : RGB(0xe6, 0xea, 0xf0)), 255);
        draw_icon(fd_tab_icon[i], cx + 30, ty + 16, 9);
        if (on) draw_text_bold(cx + 48, ty + 8, fd_tab_name[i], text); else draw_text(cx + 48, ty + 8, fd_tab_name[i], text, 255);
    }
    /* the search box */
    int bx = cx + FD_SIDE + 16, bw = cw - FD_SIDE - 32, by = cy + 14;
    fill_round(bx, by, bw, 30, 8, dark ? RGB(0x2e, 0x30, 0x36) : RGB(0xf2, 0xf4, 0xf8), 255);
    stroke_round(bx, by, bw, 30, 8, fd.focus_search ? RGB(0x3c, 0x84, 0xd8) : (dark ? RGB(0x44, 0x46, 0x4c) : RGB(0xd8, 0xdc, 0xe2)), 255);
    draw_icon(ICON_SEARCH, bx + 16, by + 15, 7);
    char ph[64]; snprintf(ph, sizeof ph, "Search %s", fd_tab_name[fd.tab]);
    if (fd.q[0]) {
        draw_text(bx + 32, by + 7, fd.q, text, 255);
        if (fd.focus_search) fill_rect(bx + 33 + text_w(fd.q), by + 7, 1, 16, text, 255);
    } else {
        draw_text(bx + 32, by + 7, ph, grey, 255);
        if (fd.focus_search) fill_rect(bx + 32, by + 7, 1, 16, text, 255);
    }
    /* the grid */
    int gx, gy, cols, gh; fd_grid(cw, ch, &gx, &gy, &cols, &gh);
    int ox0 = clip_x0, oy0 = clip_y0, ox1 = clip_x1, oy1 = clip_y1;
    clip_y0 = imax(clip_y0, cy + gy); clip_y1 = imin(clip_y1, cy + gy + gh);
    if (!fd.n) {
        const char *m = fd.tab != FT_APPS && ms_scanning && !ms_nfiles ? "Looking for your files..." :
                        fd.q[0] ? "Nothing found." : fd.tab == FT_APPS ? "No apps." : "Nothing here yet.";
        draw_text(cx + gx + (cw - gx - text_w(m)) / 2, cy + gy + 60, m, grey, 255);
    }
    int first = imax(0, fd.scroll / FD_TH * cols), last = imin(fd.n, (fd.scroll + gh) / FD_TH * cols + 2 * cols);
    for (int i = first; i < last; i++) {
        int tx = cx + gx + (i % cols) * FD_TW, ty = cy + gy + (i / cols) * FD_TH - fd.scroll;
        if (ty + FD_TH < clip_y0 || ty >= clip_y1) continue;
        int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == finder_draw && hot.b == i;
        if (hv) fill_round(tx + 3, ty + 3, FD_TW - 6, FD_TH - 6, 8, dark ? RGB(0x34, 0x36, 0x3c) : RGB(0xea, 0xf1, 0xfb), 255);
        int done = 0;
        if (fd.it[i].icon == ICON_PHOTO && !fd.it[i].app) {   /* a picture: its preview */
            int t = fd_thumb(fd.it[i].path);
            if (t >= 0) {
                int w = fd_th[t].w, h = fd_th[t].h, x0 = tx + (FD_TW - w) / 2, y0 = ty + 8 + (FD_PH - h) / 2;
                fill_rect(x0 + 1, y0 + 2, w, h, RGB(0, 0, 0), 40);
                for (int y = imax(y0, clip_y0); y < imin(y0 + h, clip_y1); y++)
                    for (int x = imax(x0, clip_x0); x < imin(x0 + w, clip_x1); x++) back[y * W + x] = fd_th[t].px[(y - y0) * w + (x - x0)];
                done = 1;
            }
        }
        if (!done) {
            if (fd.it[i].app) dock_tile(tx + (FD_TW - 56) / 2, ty + 10, 56, RGB(0xfa, 0xfb, 0xfd), RGB(0xd8, 0xde, 0xe6));
            draw_icon(fd.it[i].icon, tx + FD_TW / 2, ty + 10 + 28, fd.it[i].app ? 17 : 20);
        }
        char l1[64], l2[64]; snprintf(l1, sizeof l1, "%s", fd.it[i].label); l2[0] = 0;
        int maxw = FD_TW - 8;
        if (text_w(l1) > maxw) {                             /* two lines, then .. */
            int len = (int)strlen(l1), cut = 0;
            for (;;) {
                int nx = u8_next(l1, len, cut); char c = l1[nx]; l1[nx] = 0;
                int fits = text_w(l1) <= maxw; l1[nx] = c;
                if (!fits || nx >= len) break;
                cut = nx;
            }
            snprintf(l2, sizeof l2, "%s", l1 + cut); l1[cut] = 0;
            if (text_w(l2) > maxw) {
                while (l2[0] && text_w(l2) > maxw - text_w("..")) l2[u8_prev(l2, (int)strlen(l2))] = 0;
                strcat(l2, "..");
            }
        }
        draw_text(tx + (FD_TW - text_w(l1)) / 2, ty + 76, l1, text, 255);
        if (l2[0]) draw_text(tx + (FD_TW - text_w(l2)) / 2, ty + 93, l2, text, 255);
    }
    clip_x0 = ox0; clip_y0 = oy0; clip_x1 = ox1; clip_y1 = oy1;
}
static int finder_hit(int lx, int ly, int cw, int ch) {
    if (lx < FD_SIDE) { int i = (ly - 14) / 38; if (ly >= 14 && i < FT_N && (ly - 14) % 38 < 32) return 1000 + i; return -1; }
    if (ly >= 14 && ly < 44) return 2000;
    int gx, gy, cols, gh; fd_grid(cw, ch, &gx, &gy, &cols, &gh);
    if (ly < gy || ly >= gy + gh || lx < gx || lx >= gx + cols * FD_TW) return -1;
    int i = (ly - gy + fd.scroll) / FD_TH * cols + (lx - gx) / FD_TW;
    return i < fd.n ? i : -1;
}
static void finder_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(finder_draw); int cx = 0, cy = 0, cw = FD_W, ch = FD_H;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (id >= 1000 && id < 1000 + FT_N) { *x = cx; *y = cy + 14 + (id - 1000) * 38; *w = FD_SIDE; *h = 32; return; }
    int gx, gy, cols, gh; fd_grid(cw, ch, &gx, &gy, &cols, &gh);
    *x = cx + gx + (id % cols) * FD_TW; *y = cy + gy + (id / cols) * FD_TH - fd.scroll; *w = FD_TW; *h = FD_TH;
    if (*y < cy + gy) { *h -= cy + gy - *y; *y = cy + gy; }
    if (*y + *h > cy + gy + gh) *h = imax(0, cy + gy + gh - *y);
}
static void fd_open(int i) {
    if (i < 0 || i >= fd.n) return;
    char p[300]; snprintf(p, sizeof p, "%s", fd.it[i].path);
    int me = win_of(finder_draw);
    if (me >= 0) { fd.q[0] = 0; close_win(me); }               /* the Finder goes away, like a launcher */
    if (fd.it[i].app) run(p); else open_file(p);
}
static void fd_set_tab(int t) { if (t != fd.tab) { fd.tab = t; fd.scroll = 0; fd_build(); } repaint_client(finder_draw); }
static void finder_click(int lx, int ly, int cw, int ch) {
    int id = finder_hit(lx, ly, cw, ch);
    if (id >= 1000 && id < 1000 + FT_N) { fd_set_tab(id - 1000); return; }
    if (id == 2000) { fd.focus_search = 1; repaint_client(finder_draw); return; }
    if (id >= 0) fd_open(id);
}
static void finder_wheel(int amount) {
    int i = win_of(finder_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    int gx, gy, cols, gh; fd_grid(cw, ch, &gx, &gy, &cols, &gh);
    int rows = (fd.n + cols - 1) / cols, maxs = imax(0, rows * FD_TH - gh);
    fd.scroll = clampi(fd.scroll + amount * 60, 0, maxs);
    repaint_client(finder_draw); update_hover();
}
static void finder_key(int code, int mods) {
    if (code == KEY_ESC) { if (fd.q[0]) { fd.q[0] = 0; fd_build(); repaint_client(finder_draw); } return; }
    if (code == KEY_ENTER || code == KEY_KPENTER) { if (fd.n) fd_open(0); return; }
    if (code == KEY_TAB) { fd_set_tab((fd.tab + ((mods & MOD_SHIFT) ? FT_N - 1 : 1)) % FT_N); return; }
    if (code == KEY_BACKSPACE) {
        int l = (int)strlen(fd.q);
        if (l) { fd.q[u8_prev(fd.q, l)] = 0; fd.scroll = 0; fd_build(); repaint_client(finder_draw); }
        return;
    }
    char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
    if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
    size_t l = strlen(fd.q);
    if (l + (size_t)k < sizeof fd.q) { memcpy(fd.q + l, t, (size_t)k); fd.q[l + (size_t)k] = 0; }
    fd.focus_search = 1; fd.scroll = 0; fd_build(); repaint_client(finder_draw);
}
static void finder_wake(void) { repaint_client(finder_draw); }      /* previews are ready */
static int finder_on_close(void) { fd.q[0] = 0; return 1; }

static void finder_open(void) {
    int i = win_of(finder_draw);
    if (i >= 0) {                                           /* already open: to the front, or away if it is in front */
        if (!wins[i].minimized && i == active_win()) { minimize_win(i); return; }
        restore_win(i); return;
    }
    ms_scan_files();                                       /* read the files again (in the background) */
    fd.focus_search = 1; fd.scroll = 0; fd_build();
    static const Win t = { .title = "Finder", .icon = ICON_SEARCH, .draw = finder_draw, .click = finder_click, .hit = finder_hit,
                           .elem_rect = finder_elem_rect, .key = finder_key, .wheel = finder_wheel, .wake = finder_wake,
                           .on_close = finder_on_close };
    int w = open_win(&t, imin(W - 60, FD_W), imin(H - TASKBAR_H - 60, FD_H));
    if (w >= 0) { snprintf(wins[w].cmd, sizeof wins[0].cmd, "@finder"); repaint_taskbar(); }
}
