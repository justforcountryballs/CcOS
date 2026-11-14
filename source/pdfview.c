/* ===================================================================
 *  PDF viewer. Pages are drawn by a helper (ccos-pdf, using MuPDF) that
 *  runs next to the desktop; this is the window: scroll through the
 *  pages, zoom, go to a page, copy a page's text.
 * =================================================================== */
#define PV_BAR 44
#define PV_GAP 14
#define PV_NCACHE 8
static struct {
    pid_t pid; int to, from;                      /* the helper's stdin / stdout */
    char path[512], name[128], err[160];
    int npages, loading; float *pw, *ph;          /* page sizes, in points */
    int zoom;                                     /* 0: fit the width; else percent */
    long long scroll;                             /* pixels from the top of the first page */
    struct { int page, width, w, h; uint32_t *px; long long used; } cache[PV_NCACHE];
    int busy_page, busy_width;                    /* asked the helper for this one */
    char rbuf[1 << 16]; int rlen;
    int want_text;                                /* waiting for a page's text (to copy) */
    long long tick;
} pv = { .pid = 0, .to = -1, .from = -1, .busy_page = -1 };
static void pdfview_draw(int cx, int cy, int cw, int ch);
static int is_pdf(const char *name) { const char *e = strrchr(name, '.'); return e && !strcasecmp(e, ".pdf"); }

static void pv_stop_helper(void) {
    if (pv.to >= 0) close(pv.to);
    if (pv.from >= 0) close(pv.from);
    pv.to = pv.from = -1;
    if (pv.pid > 0) { kill(pv.pid, SIGTERM); pv.pid = 0; }
}
static int pv_start_helper(void) {
    if (pv.pid > 0 && kill(pv.pid, 0) == 0 && pv.to >= 0) return 1;
    pv_stop_helper();
    int a[2], b[2];
    if (pipe(a) < 0) return 0;
    if (pipe(b) < 0) { close(a[0]); close(a[1]); return 0; }
    pid_t p = fork();
    if (p == 0) {
        dup2(a[0], 0); dup2(b[1], 1);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl("/usr/bin/ccos-pdf", "ccos-pdf", (char *)NULL);
        _exit(1);
    }
    close(a[0]); close(b[1]);
    if (p < 0) { close(a[1]); close(b[0]); return 0; }
    pv.pid = p; pv.to = a[1]; pv.from = b[0];
    fcntl(pv.to, F_SETFD, FD_CLOEXEC); fcntl(pv.from, F_SETFD, FD_CLOEXEC);
    fcntl(pv.from, F_SETFL, O_NONBLOCK);
    pv.rlen = 0; pv.busy_page = -1;
    return 1;
}
static void pv_send(const char *s) { if (pv.to >= 0 && write(pv.to, s, strlen(s)) < 0) {} }
static void pv_drop_cache(void) {
    for (int i = 0; i < PV_NCACHE; i++) { free(pv.cache[i].px); pv.cache[i].px = NULL; pv.cache[i].page = -1; }
}

/* sizes on screen */
static float pv_scale(int cw) {                   /* pixels per point */
    float maxw = 1; for (int i = 0; i < pv.npages; i++) if (pv.pw[i] > maxw) maxw = pv.pw[i];
    if (!pv.zoom) return imax(100, cw - 2 * PV_GAP - 14) / maxw;
    return pv.zoom / 100.0f * 96 / 72;            /* 100% = a page at its printed size */
}
static int pv_pw(int i, float s) { return (int)(pv.pw[i] * s + 0.5f); }
static int pv_ph(int i, float s) { return (int)(pv.ph[i] * s + 0.5f); }
static long long pv_page_y(int i, float s) { long long y = PV_GAP; for (int k = 0; k < i; k++) y += pv_ph(k, s) + PV_GAP; return y; }
static long long pv_total_h(float s) { return pv_page_y(pv.npages, s); }
static int pv_view_h(int ch) { return ch - PV_BAR; }
static void pv_clamp(int cw, int ch) {
    long long m = pv_total_h(pv_scale(cw)) - pv_view_h(ch);
    if (pv.scroll > m) pv.scroll = m;
    if (pv.scroll < 0) pv.scroll = 0;
}
static int pv_cur_page(int cw, int ch) {          /* the page in the middle of the view */
    float s = pv_scale(cw); long long mid = pv.scroll + pv_view_h(ch) / 3;
    for (int i = 0; i < pv.npages; i++) if (pv_page_y(i + 1, s) > mid) return i;
    return imax(0, pv.npages - 1);
}
static int pv_find(int page, int width) {
    for (int i = 0; i < PV_NCACHE; i++) if (pv.cache[i].px && pv.cache[i].page == page && pv.cache[i].width == width) return i;
    return -1;
}
static void pv_request(int cw, int ch) {          /* ask for the next missing page that's on screen (or near) */
    if (pv.busy_page >= 0 || pv.to < 0 || !pv.npages) return;
    float s = pv_scale(cw);
    long long top = pv.scroll - 200, bot = pv.scroll + pv_view_h(ch) + 400;
    int best = -1;
    for (int i = 0; i < pv.npages; i++) {
        long long y = pv_page_y(i, s);
        if (y + pv_ph(i, s) < top || y > bot) continue;
        if (pv_find(i, pv_pw(i, s)) < 0) { best = i; if (y + pv_ph(i, s) >= pv.scroll) break; }
    }
    if (best < 0) return;
    int w = pv_pw(best, s);
    if ((long long)w * pv_ph(best, s) > 40LL * 1000 * 1000) return;   /* too big to keep */
    char cmd[700]; snprintf(cmd, sizeof cmd, "R %d %d /tmp/ccos-pdf-%d.raw\n", best, w, (int)getpid());
    pv.busy_page = best; pv.busy_width = w;
    pv_send(cmd);
}
static void pv_got_page(int page, int width) {
    pv.busy_page = -1;
    if (width < 0) return;
    char p[64]; snprintf(p, sizeof p, "/tmp/ccos-pdf-%d.raw", (int)getpid());
    FILE *f = fopen(p, "rb"); if (!f) return;
    int w, h;
    if (fscanf(f, "%d %d", &w, &h) != 2 || w <= 0 || h <= 0 || w > 20000 || h > 20000) { fclose(f); return; }
    fgetc(f);
    int slot = 0;                                  /* the one used longest ago */
    for (int i = 1; i < PV_NCACHE; i++) if (!pv.cache[i].px || (pv.cache[slot].px && pv.cache[i].used < pv.cache[slot].used)) slot = i;
    free(pv.cache[slot].px);
    uint32_t *px = malloc((size_t)w * h * 4);
    unsigned char *row = malloc((size_t)w * 3);
    if (!px || !row) { free(px); free(row); fclose(f); pv.cache[slot].px = NULL; return; }
    for (int y = 0; y < h; y++) {
        if (fread(row, 3, (size_t)w, f) != (size_t)w) break;
        for (int x = 0; x < w; x++) px[(size_t)y * w + x] = RGB(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
    }
    free(row); fclose(f); unlink(p);
    pv.cache[slot].px = px; pv.cache[slot].page = page; pv.cache[slot].width = width;
    pv.cache[slot].w = w; pv.cache[slot].h = h; pv.cache[slot].used = ui_ms();
}
static int pv_fd(void) { return pv.from; }
static void pv_line(char *l) {
    if (l[0] == 'P') {
        int n = atoi(l + 2); free(pv.pw); free(pv.ph);
        pv.npages = 0; pv.pw = calloc((size_t)imax(1, n), sizeof(float)); pv.ph = calloc((size_t)imax(1, n), sizeof(float));
        pv.loading = n;                            /* sizes follow */
        if (!n) { pv.loading = 0; snprintf(pv.err, sizeof pv.err, "This PDF has no pages."); }
    } else if (l[0] == 'S' && pv.loading > 0) {
        float w = 612, h = 792; sscanf(l + 2, "%f %f", &w, &h);
        pv.pw[pv.npages] = fmaxf(10, w); pv.ph[pv.npages] = fmaxf(10, h); pv.npages++; pv.loading--;
    } else if (l[0] == 'E') { snprintf(pv.err, sizeof pv.err, "%s", l + 2); pv.loading = 0; pv.npages = 0; }
    else if (l[0] == 'D') { int pg = -1, w = -1; sscanf(l + 2, "%d %d", &pg, &w); pv_got_page(pg, w); }
}
static void pv_wake(void) {
    if (pv.from < 0) return;
    ssize_t k;
    while ((k = read(pv.from, pv.rbuf + pv.rlen, sizeof pv.rbuf - 1 - (size_t)pv.rlen)) > 0) pv.rlen += (int)k;
    if (k == 0) {                                  /* the helper stopped */
        pv_stop_helper();
        if (!pv.npages && !pv.err[0]) snprintf(pv.err, sizeof pv.err, "The PDF reader stopped. The file may be damaged.");
    }
    for (;;) {
        pv.rbuf[pv.rlen] = 0;
        if (pv.rbuf[0] == 'X') {                   /* text: "X page bytes\n" + bytes */
            char *nl = memchr(pv.rbuf, '\n', (size_t)pv.rlen); if (!nl) break;
            int pg = 0; size_t n = 0; sscanf(pv.rbuf + 2, "%d %zu", &pg, &n);
            size_t hl = (size_t)(nl - pv.rbuf) + 1;
            if (n > sizeof pv.rbuf - hl - 1) n = sizeof pv.rbuf - hl - 1;
            if ((size_t)pv.rlen < hl + n) { if (pv.rlen >= (int)sizeof pv.rbuf - 1) pv.rlen = 0; break; }
            if (pv.want_text) {
                pv.want_text = 0;
                wl_clip_set_text(pv.rbuf + hl, n);
                char b[80]; snprintf(b, sizeof b, n ? "Page %d's text is on the clipboard." : "Page %d has no text (it may be a scan).", pg + 1);
                toast(ICON_PDF, n ? "Copied" : "No text", b);
            }
            memmove(pv.rbuf, pv.rbuf + hl + n, (size_t)pv.rlen - hl - n + 1); pv.rlen -= (int)(hl + n);
            continue;
        }
        char *nl = memchr(pv.rbuf, '\n', (size_t)pv.rlen);
        if (!nl) { if (pv.rlen >= (int)sizeof pv.rbuf - 1) pv.rlen = 0; break; }
        *nl = 0; pv_line(pv.rbuf);
        int used = (int)(nl - pv.rbuf) + 1;
        memmove(pv.rbuf, pv.rbuf + used, (size_t)(pv.rlen - used) + 1); pv.rlen -= used;
    }
    int w = win_of(pdfview_draw);
    if (w >= 0) {
        int cx, cy, cw, ch; client_rect(&wins[w], &cx, &cy, &cw, &ch);
        pv_request(cw, ch);
        repaint_client(pdfview_draw);
    }
}

/* ---- drawing ---- */
enum { PB_PREV = 1, PB_NEXT, PB_OUT, PB_FIT, PB_IN, PB_COPY, PB_TRACK };
static void pv_bar_btn(int x, int y, int w, const char *l, int hv, int on) {
    if (hv || on) fill_round(x, y, w, 30, 6, on ? RGB(0xdc, 0xea, 0xfb) : RGB(0xe8, 0xee, 0xf6), 255);
    draw_text(x + (w - text_w(l)) / 2, y + 7, l, RGB(0x1e, 0x22, 0x28), 255);
}
static int pv_bar_x[8];
static void pdfview_draw(int cx, int cy, int cw, int ch) {
    fill_rect(cx, cy + PV_BAR, cw, ch - PV_BAR, RGB(0x52, 0x56, 0x5e), 255);
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == pdfview_draw ? hot.b : -1;
    /* the bar */
    fill_rect(cx, cy, cw, PV_BAR, RGB(0xf6, 0xf7, 0xf9), 255);
    fill_rect(cx, cy + PV_BAR - 1, cw, 1, RGB(0xdc, 0xe0, 0xe6), 255);
    char b[96];
    int x = cx + 10, y = cy + 7;
    pv_bar_x[PB_PREV] = x - cx; pv_bar_btn(x, y, 30, "<", hv == PB_PREV, 0); x += 34;
    pv_bar_x[PB_NEXT] = x - cx; pv_bar_btn(x, y, 30, ">", hv == PB_NEXT, 0); x += 40;
    if (pv.npages) { snprintf(b, sizeof b, "Page %d of %d", pv_cur_page(cw, ch) + 1, pv.npages); draw_text(x, y + 7, b, RGB(0x1e, 0x22, 0x28), 255); }
    x += 150;
    pv_bar_x[PB_OUT] = x - cx; pv_bar_btn(x, y, 30, "-", hv == PB_OUT, 0); x += 34;
    if (pv.zoom) snprintf(b, sizeof b, "%d%%", pv.zoom); else snprintf(b, sizeof b, "Fit");
    pv_bar_x[PB_FIT] = x - cx; pv_bar_btn(x, y, 56, b, hv == PB_FIT, !pv.zoom); x += 60;
    pv_bar_x[PB_IN] = x - cx; pv_bar_btn(x, y, 30, "+", hv == PB_IN, 0); x += 44;
    pv_bar_x[PB_COPY] = x - cx;
    if (cw - (x - cx) > 140) pv_bar_btn(x, y, 130, "Copy page text", hv == PB_COPY, 0);
    /* pages */
    int ox0 = clip_y0; clip_y0 = imax(clip_y0, cy + PV_BAR);
    if (pv.err[0]) {
        draw_text(cx + (cw - text_w(pv.err)) / 2, cy + ch / 2, pv.err, RGB(255, 255, 255), 255);
    } else if (!pv.npages) {
        draw_text(cx + (cw - text_w("Opening...")) / 2, cy + ch / 2, "Opening...", RGB(255, 255, 255), 255);
    } else {
        float s = pv_scale(cw);
        long long ty = pv_total_h(s);
        int vh = pv_view_h(ch);
        for (int i = 0; i < pv.npages; i++) {
            long long py = pv_page_y(i, s) - pv.scroll;
            int w = pv_pw(i, s), h = pv_ph(i, s);
            if (py + h < 0) continue;
            if (py > vh) break;
            int px = cx + imax(PV_GAP, (cw - 14 - w) / 2) - (w > cw - 14 - 2 * PV_GAP ? 0 : 0), pyy = cy + PV_BAR + (int)py;
            fill_rect(px + 3, pyy + 4, w, h, RGB(0, 0, 0), 60);
            int k = pv_find(i, w);
            if (k >= 0) {
                pv.cache[k].used = ui_ms();
                uint32_t *src = pv.cache[k].px; int sw = pv.cache[k].w, sh = pv.cache[k].h;
                for (int yy = imax(pyy, clip_y0); yy < imin(pyy + imin(h, sh), clip_y1); yy++)
                    for (int xx = imax(px, clip_x0); xx < imin(px + imin(w, sw), clip_x1); xx++)
                        back[(size_t)yy * BW + xx] = src[(size_t)(yy - pyy) * sw + (xx - px)];
            } else {
                fill_rect(px, pyy, w, h, RGB(255, 255, 255), 255);
                /* an older, different size of it: better than blank? keep it simple: a note */
                draw_text(px + (w - text_w("Loading page...")) / 2, pyy + imin(h / 2, 200), "Loading page...", RGB(0x90, 0x98, 0xa4), 255);
            }
        }
        /* scroll bar */
        if (ty > vh) {
            int tx = cx + cw - 12, th = vh - 8;
            fill_round(tx, cy + PV_BAR + 4, 8, th, 4, RGB(0x66, 0x6a, 0x72), 255);
            int kh = imax(30, (int)((long long)th * vh / ty)), ky = (int)((long long)(th - kh) * pv.scroll / imax(1, ty - vh));
            fill_round(tx, cy + PV_BAR + 4 + ky, 8, kh, 4, RGB(0xc8, 0xcc, 0xd4), 255);
        }
    }
    clip_y0 = ox0;
}
static int pdfview_hit(int lx, int ly, int cw, int ch) {
    (void)ch;
    if (ly < PV_BAR) {
        static const int ws[8] = { 0, 30, 30, 30, 56, 30, 130, 0 };
        for (int id = PB_PREV; id <= PB_COPY; id++) if (lx >= pv_bar_x[id] && lx < pv_bar_x[id] + ws[id] && ly >= 7 && ly < 37) return id;
        return -1;
    }
    if (lx >= cw - 14) return PB_TRACK;
    return -1;
}
static void pdfview_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(pdfview_draw), cx = 0, cy = 0, cw = 800, ch = 600;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    *x = cx; *y = cy; *w = cw; *h = id > 0 && id <= PB_COPY ? PV_BAR : ch;
}
static void pv_go_page(int pg, int cw, int ch) {
    if (!pv.npages) return;
    pg = clampi(pg, 0, pv.npages - 1);
    pv.scroll = pv_page_y(pg, pv_scale(cw)) - PV_GAP;
    pv_clamp(cw, ch);
}
static void pv_set_zoom(int z, int cw, int ch) {      /* keep the same spot of the document in view */
    float s0 = pv_scale(cw); double frac = (pv.scroll + pv_view_h(ch) / 2.0) / fmax(1, pv_total_h(s0));
    pv.zoom = z;
    float s1 = pv_scale(cw);
    pv.scroll = (long long)(frac * pv_total_h(s1) - pv_view_h(ch) / 2.0);
    pv_clamp(cw, ch);
}
static int pv_cur_zoom(int cw) { return (int)(pv_scale(cw) * 72 / 96 * 100 + 0.5f); }
static void pdfview_click(int lx, int ly, int cw, int ch) {
    int id = pdfview_hit(lx, ly, cw, ch);
    static const int steps[] = { 25, 33, 50, 67, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400 };
    int nst = (int)(sizeof steps / sizeof steps[0]);
    if (id == PB_PREV) pv_go_page(pv_cur_page(cw, ch) - 1, cw, ch);
    else if (id == PB_NEXT) pv_go_page(pv_cur_page(cw, ch) + 1, cw, ch);
    else if (id == PB_FIT) pv_set_zoom(0, cw, ch);
    else if (id == PB_IN || id == PB_OUT) {
        int cur = pv_cur_zoom(cw), k;
        if (id == PB_IN) { for (k = 0; k < nst && steps[k] <= cur; k++) {} pv_set_zoom(steps[imin(k, nst - 1)], cw, ch); }
        else { for (k = nst - 1; k >= 0 && steps[k] >= cur; k--) {} pv_set_zoom(steps[imax(k, 0)], cw, ch); }
    }
    else if (id == PB_COPY && pv.npages && pv.to >= 0) {
        char c[32]; snprintf(c, sizeof c, "T %d\n", pv_cur_page(cw, ch)); pv.want_text = 1; pv_send(c);
    }
    else if (id == PB_TRACK && pv.npages) {
        long long ty = pv_total_h(pv_scale(cw));
        pv.scroll = (long long)((double)(ly - PV_BAR) / imax(1, ch - PV_BAR) * ty) - pv_view_h(ch) / 2;
        pv_clamp(cw, ch);
    }
    pv_request(cw, ch);
    repaint_client(pdfview_draw);
}
static void pv_scroll_by(long long d) {
    int w = win_of(pdfview_draw); if (w < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[w], &cx, &cy, &cw, &ch);
    pv.scroll += d; pv_clamp(cw, ch); pv_request(cw, ch);
    repaint_client(pdfview_draw);
}
static void pdfview_wheel(int amount) { pv_scroll_by(-amount * 70); }
static void pdfview_key(int code, int mods) {
    int w = win_of(pdfview_draw); if (w < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[w], &cx, &cy, &cw, &ch);
    int vh = pv_view_h(ch);
    if ((mods & MOD_CTRL) && (code == KEY_EQUAL || code == KEY_KPPLUS)) { pdfview_click(pv_bar_x[PB_IN] + 2, 10, cw, ch); return; }
    if ((mods & MOD_CTRL) && (code == KEY_MINUS || code == KEY_KPMINUS)) { pdfview_click(pv_bar_x[PB_OUT] + 2, 10, cw, ch); return; }
    if ((mods & MOD_CTRL) && code == KEY_0) { pdfview_click(pv_bar_x[PB_FIT] + 2, 10, cw, ch); return; }
    switch (code) {
    case KEY_DOWN: pv_scroll_by(60); break;
    case KEY_UP: pv_scroll_by(-60); break;
    case KEY_PAGEDOWN: case KEY_SPACE: pv_scroll_by(vh - 40); break;
    case KEY_PAGEUP: pv_scroll_by(-(vh - 40)); break;
    case KEY_RIGHT: pv_go_page(pv_cur_page(cw, ch) + 1, cw, ch); pv_request(cw, ch); repaint_client(pdfview_draw); break;
    case KEY_LEFT: pv_go_page(pv_cur_page(cw, ch) - 1, cw, ch); pv_request(cw, ch); repaint_client(pdfview_draw); break;
    case KEY_HOME: pv.scroll = 0; pv_scroll_by(0); break;
    case KEY_END: pv.scroll = 1LL << 40; pv_scroll_by(0); break;
    }
}
static void pdfview_tick(void) {                  /* the window was resized: new sizes of the pages */
    int w = win_of(pdfview_draw); if (w < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[w], &cx, &cy, &cw, &ch);
    pv_clamp(cw, ch); pv_request(cw, ch);
}
static int pdfview_on_close(void) {
    pv_stop_helper(); pv_drop_cache();
    free(pv.pw); free(pv.ph); pv.pw = pv.ph = NULL; pv.npages = 0;
    char p[64]; snprintf(p, sizeof p, "/tmp/ccos-pdf-%d.raw", (int)getpid()); unlink(p);
    return 1;
}
static void pdfview_open(const char *path) {
    snprintf(pv.path, sizeof pv.path, "%s", path);
    const char *sl = strrchr(path, '/'); snprintf(pv.name, sizeof pv.name, "%s", sl ? sl + 1 : path);
    pv.err[0] = 0; pv.npages = 0; pv.loading = 0; pv.scroll = 0; pv.zoom = 0; pv.want_text = 0;
    pv_drop_cache();
    if (!pv_start_helper()) snprintf(pv.err, sizeof pv.err, "The PDF reader is missing.");
    else {
        pv.busy_page = -1;
        char c[600]; snprintf(c, sizeof c, "O %s\n", path); pv_send(c);
    }
    static char title[160];
    snprintf(title, sizeof title, "%s - PDF", pv.name);
    static Win t;
    t = (Win){ .title = title, .icon = ICON_PDF, .draw = pdfview_draw, .click = pdfview_click, .hit = pdfview_hit, .elem_rect = pdfview_elem_rect,
               .key = pdfview_key, .wheel = pdfview_wheel, .fd = pv_fd, .wake = pv_wake, .tick = pdfview_tick, .on_close = pdfview_on_close };
    int w = win_of(pdfview_draw);
    if (w >= 0) { wins[w].title = title; restore_win(w); repaint_window(w); return; }
    open_win(&t, imin(W - 60, 860), imin(H - TASKBAR_H - 40, 980));
}
static void draw_pdf_icon(int cx, int cy, int r) {
    int w = r * 3 / 2, h = 2 * r, x = cx - w / 2, y = cy - r;
    fill_round(x, y, w, h, imax(2, r / 6), RGB(0xff, 0xff, 0xff), 255);
    stroke_round(x, y, w, h, imax(2, r / 6), RGB(0xa0, 0xa8, 0xb4), 255);
    fill_round(x - r / 5, y + h / 2 - r / 4, w + r / 5, r * 2 / 3, imax(2, r / 8), RGB(0xd9, 0x30, 0x25), 255);
    if (r >= 14) draw_text(cx - 12, y + h / 2 - r / 4 + (r * 2 / 3 - 16) / 2, "PDF", RGB(255, 255, 255), 255);
    else fill_rect(cx - r / 2, y + h / 2, r, imax(1, r / 6), RGB(255, 255, 255), 255);
    for (int k = 0; k < 2; k++) fill_rect(x + w / 5, y + h / 6 + k * (r / 4), w * 3 / 5, imax(1, r / 10), RGB(0xb8, 0xc0, 0xcc), 255);
}
