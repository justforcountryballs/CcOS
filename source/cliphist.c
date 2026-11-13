/* ===================================================================
 *  Clipboard history: Windows + V. The last 25 things copied - text
 *  from any app (ccos's own and Wayland apps) and pictures (screenshots,
 *  pictures copied in apps). Pick one and it's pasted where you were.
 * =================================================================== */
#define CH_MAX 25
#define CH_W 380
#define CH_IH 62
typedef struct { int png; char *data; size_t n; uint32_t *th; int tw, thh; time_t t; } ChItem;
static ChItem ch_list[CH_MAX];
static int ch_n, ch_open, ch_sel, ch_scroll, ch_target = -1;
static int ch_quiet;                       /* setting the clipboard from the history: don't add it again */

static void ch_free(ChItem *c) { free(c->data); free(c->th); memset(c, 0, sizeof *c); }
static void ch_put(int png, const char *data, size_t n) {
    if (!data || !n || ch_quiet) return;
    if (!png) { size_t k = 0; while (k < n && (data[k] == ' ' || data[k] == '\n' || data[k] == '\t' || data[k] == '\r')) k++; if (k == n) return; }
    for (int i = 0; i < ch_n; i++)                     /* already there: to the top */
        if (ch_list[i].png == png && ch_list[i].n == n && !memcmp(ch_list[i].data, data, n)) {
            ChItem c = ch_list[i];
            memmove(ch_list + 1, ch_list, sizeof *ch_list * (size_t)i); ch_list[0] = c; ch_list[0].t = time(NULL);
            return;
        }
    ChItem c = { png, malloc(n + 1), n, NULL, 0, 0, time(NULL) };
    if (!c.data) return;
    memcpy(c.data, data, n); c.data[n] = 0;
    if (png) {                                         /* a small picture for the list */
        int iw, ih, cc; uint8_t *px = stbi_load_from_memory((const uint8_t *)data, (int)n, &iw, &ih, &cc, 4);
        if (px) {
            float s = fminf(96.0f / iw, 48.0f / ih); if (s > 1) s = 1;
            c.tw = imax(1, (int)(iw * s)); c.thh = imax(1, (int)(ih * s));
            c.th = malloc((size_t)c.tw * c.thh * 4);
            if (c.th) for (int y = 0; y < c.thh; y++) for (int x = 0; x < c.tw; x++) {
                const uint8_t *q = px + ((size_t)(y * ih / c.thh) * iw + x * iw / c.tw) * 4;
                c.th[y * c.tw + x] = RGB(q[0], q[1], q[2]);
            }
            stbi_image_free(px);
        }
    }
    if (ch_n == CH_MAX) ch_free(&ch_list[--ch_n]);
    memmove(ch_list + 1, ch_list, sizeof *ch_list * (size_t)ch_n);
    ch_list[0] = c; ch_n++;
}

/* ---- reading what a Wayland app copied (it writes into a pipe; a thread reads it) ---- */
static pthread_mutex_t ch_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int png; char *data; size_t n; } ch_in[4]; static int ch_nin;
typedef struct { int fd, png; } ChRead;
static void *ch_reader(void *arg) {
    ChRead r = *(ChRead *)arg; free(arg);
    size_t cap = 65536, n = 0; char *b = malloc(cap);
    long long t0 = ui_ms();
    while (b) {
        struct pollfd p = { r.fd, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0 || ui_ms() - t0 > 4000) break;
        if (n == cap) { if (cap >= (size_t)16 << 20) break; char *nb = realloc(b, cap * 2); if (!nb) break; b = nb; cap *= 2; }
        ssize_t k = read(r.fd, b + n, cap - n);
        if (k <= 0) break;
        n += (size_t)k;
    }
    close(r.fd);
    if (b) { char *nb = realloc(b, n + 1); if (nb) { b = nb; b[n] = 0; } else { free(b); b = NULL; } }   /* room for the 0 at the end */
    pthread_mutex_lock(&ch_mu);
    if (b && n && ch_nin < 4) { ch_in[ch_nin].png = r.png; ch_in[ch_nin].data = b; ch_in[ch_nin].n = n; ch_nin++; b = NULL; }
    pthread_mutex_unlock(&ch_mu);
    free(b);
    if (wake_pipe[1] >= 0) { if (write(wake_pipe[1], "c", 1) < 0) {} }
    return NULL;
}
/* a Wayland app copied something: fetch it (text, or a picture) */
static void ch_fetch(struct wl_resource *src, const char *mime, int png) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return;
    wl_data_source_send_send(src, mime, p[1]);
    close(p[1]);
    ChRead *r = malloc(sizeof *r); if (!r) { close(p[0]); return; }
    r->fd = p[0]; r->png = png;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, ch_reader, r)) { close(p[0]); free(r); }
    pthread_attr_destroy(&at);
}
/* main loop: take what the readers got. Text also goes on ccos's own clipboard (paste in Notepad) */
static void ch_tick(void) {
    if (!ch_nin) return;
    pthread_mutex_lock(&ch_mu);
    for (int i = 0; i < ch_nin; i++) {
        ch_put(ch_in[i].png, ch_in[i].data, ch_in[i].n);
        if (!ch_in[i].png) { free(clipboard); clipboard = ch_in[i].data; clip_len = (int)ch_in[i].n; clipboard[clip_len] = 0; ch_in[i].data = NULL; }
        free(ch_in[i].data);
    }
    ch_nin = 0;
    pthread_mutex_unlock(&ch_mu);
    if (ch_open) repaint(0, 0, W, H - TASKBAR_H);
}

/* ---- the panel ---- */
static int ch_h(void) { return imin(H - TASKBAR_H - 40, 56 + imax(1, ch_n) * CH_IH + 10); }
static int ch_x(void) { return (W - CH_W) / 2; }
static int ch_y(void) { return H - TASKBAR_H - 20 - ch_h(); }
static int ch_rows(void) { return (ch_h() - 56 - 10) / CH_IH; }
static void ch_repaint(void) { repaint(ch_x() - 4, ch_y() - 4, CH_W + 12, ch_h() + 12); }
static void ch_set(int open) {
    if (open == ch_open) return;
    int was = ch_open;
    ch_open = open;
    if (was) ch_repaint();                          /* (closed first, so it isn't drawn again) */
    if (open) { ch_target = active_win(); ch_sel = 0; ch_scroll = 0; set_menu(0); pop_close(); ch_repaint(); }
}
static void draw_ch(void) {
    if (!ch_open) return;
    int x = ch_x(), y = ch_y(), w = CH_W, h = ch_h();
    if (x >= clip_x1 || x + w + 4 <= clip_x0 || y >= clip_y1 || y + h + 4 <= clip_y0) return;
    const int dark = set_style && set_dark;
    uint32_t bg = dark ? RGB(0x2a, 0x2b, 0x30) : RGB(0xf6, 0xf7, 0xf9), card = dark ? RGB(0x36, 0x38, 0x3e) : RGB(255, 255, 255);
    uint32_t fg = dark ? RGB(0xee, 0xee, 0xf0) : RGB(0x1e, 0x22, 0x28), sub = dark ? RGB(0xa0, 0xa4, 0xac) : RGB(0x70, 0x78, 0x84);
    fill_round(x + 2, y + 5, w, h, 10, RGB(0, 0, 0), 60);
    fill_round(x, y, w, h, 10, bg, 255);
    stroke_round(x, y, w, h, 10, dark ? RGB(0x50, 0x52, 0x58) : RGB(0xc8, 0xcc, 0xd4), 255);
    draw_text_bold(x + 18, y + 16, "Clipboard", fg);
    if (ch_n) {
        const char *cl = "Clear all"; int cw = text_w(cl), hv = hot.type == HOT_CH && hot.a == 1000;
        if (hv) fill_round(x + w - cw - 30, y + 10, cw + 16, 28, 5, dark ? RGB(0x40, 0x42, 0x48) : RGB(0xe6, 0xea, 0xf0), 255);
        draw_text(x + w - cw - 22, y + 16, cl, RGB(0x2a, 0x78, 0xd8), 255);
    } else {
        const char *m1 = "Nothing copied yet.", *m2 = "What you copy (Ctrl+C) shows up here.";
        draw_text(x + (w - text_w(m1)) / 2, y + 66, m1, fg, 255);
        draw_text(x + (w - text_w(m2)) / 2, y + 86, m2, sub, 255);
        return;
    }
    for (int r = 0; r < ch_rows() && ch_scroll + r < ch_n; r++) {
        int i = ch_scroll + r, iy = y + 50 + r * CH_IH, on = (hot.type == HOT_CH && hot.a == i) || (hot.type != HOT_CH && i == ch_sel);
        fill_round(x + 10, iy, w - 20, CH_IH - 6, 7, on ? (dark ? RGB(0x3a, 0x5a, 0x8a) : RGB(0xdc, 0xea, 0xfb)) : card, 255);
        stroke_round(x + 10, iy, w - 20, CH_IH - 6, 7, dark ? RGB(0x48, 0x4a, 0x50) : RGB(0xe0, 0xe4, 0xea), 255);
        ChItem *c = &ch_list[i];
        if (c->png && c->th) {
            int tx = x + 22, ty = iy + (CH_IH - 6 - c->thh) / 2;
            for (int yy = imax(ty, clip_y0); yy < imin(ty + c->thh, clip_y1); yy++)
                for (int xx = imax(tx, clip_x0); xx < imin(tx + c->tw, clip_x1); xx++) back[yy * W + xx] = c->th[(yy - ty) * c->tw + (xx - tx)];
            draw_text(x + 30 + c->tw, iy + 19, "Picture", sub, 255);
        } else {                                                 /* two lines of the text */
            char ln[2][64]; int L = 0; const char *s = c->data; int maxw = w - 52;
            while (L < 2 && *s) {
                int k = 0; while (s[k] && s[k] != '\n' && k < 60) k++;
                snprintf(ln[L], sizeof ln[0], "%.*s", k, s);
                for (char *q = ln[L]; *q; q++) if (*q == '\t' || *q == '\r') *q = ' ';
                while (ln[L][0] && text_w(ln[L]) > maxw) ln[L][u8_prev(ln[L], (int)strlen(ln[L]))] = 0;
                s += k; while (*s && *s != '\n' && s[0]) s++; if (*s == '\n') s++;
                L++;
            }
            for (int l = 0; l < L; l++) draw_text(x + 24, iy + 10 + l * 19, ln[l], fg, 255);
        }
    }
}
static int ch_hit(void) {
    int x = ch_x(), y = ch_y(), w = CH_W;
    if (!in_rect(x, y, w, ch_h())) return -1;
    if (ch_n && my >= y + 8 && my < y + 40 && mx >= x + w - 140) return 1000;
    int r = (my - y - 50) / CH_IH, i = ch_scroll + r;
    if (my >= y + 50 && r < ch_rows() && i < ch_n && mx >= x + 10 && mx < x + w - 10) return i;
    return -2;
}
static void wl_clip_set_text(const char *s, size_t n);
static void wl_clip_set_png(const unsigned char *png, size_t n);
/* put item i on the clipboard and paste it into the window that was in front */
static void ch_pick(int i) {
    if (i < 0 || i >= ch_n) return;
    ChItem c = ch_list[i];
    memmove(ch_list + 1, ch_list, sizeof *ch_list * (size_t)i); ch_list[0] = c;
    ch_quiet = 1;
    if (c.png) wl_clip_set_png((const unsigned char *)c.data, c.n);
    else { clip_set(c.data, (int)c.n); wl_clip_set_text(c.data, c.n); }
    ch_quiet = 0;
    int t = ch_target;
    ch_set(0);
    if (t >= 0 && wins[t].open && !wins[t].minimized) {           /* paste: Ctrl+V */
        if (wins[t].rawkey) {
            wins[t].rawkey(KEY_LEFTCTRL, 1); wins[t].rawkey(KEY_V, 1); wins[t].rawkey(KEY_V, 0); wins[t].rawkey(KEY_LEFTCTRL, 0);
        } else if (wins[t].key && !c.png) wins[t].key(KEY_V, MOD_CTRL);
    }
}
static void ch_click(int id) {
    if (id == 1000) { for (int i = 0; i < ch_n; i++) ch_free(&ch_list[i]); ch_n = 0; ch_repaint(); return; }
    if (id >= 0) ch_pick(id);
}
static void ch_key(int code) {
    if (code == KEY_ESC) { ch_set(0); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER) { ch_pick(ch_sel); return; }
    if (code == KEY_DOWN && ch_sel < ch_n - 1) ch_sel++;
    else if (code == KEY_UP && ch_sel > 0) ch_sel--;
    else if (code == KEY_DELETE && ch_n) {
        ch_free(&ch_list[ch_sel]);
        memmove(ch_list + ch_sel, ch_list + ch_sel + 1, sizeof *ch_list * (size_t)(ch_n - ch_sel - 1)); ch_n--;
        memset(&ch_list[ch_n], 0, sizeof ch_list[0]);
        if (ch_sel >= ch_n) ch_sel = imax(0, ch_n - 1);
    } else return;
    if (ch_sel < ch_scroll) ch_scroll = ch_sel;
    if (ch_sel >= ch_scroll + ch_rows()) ch_scroll = ch_sel - ch_rows() + 1;
    ch_repaint();
}
static void ch_wheel(int amount) {
    int ns = clampi(ch_scroll + amount, 0, imax(0, ch_n - ch_rows()));
    if (ns != ch_scroll) { ch_scroll = ns; ch_repaint(); }
}
