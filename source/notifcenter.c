/* ===================================================================
 *  Notification centre: the bell next to the clock. Every notification
 *  (downloads, screenshots, apps...) is kept here after its little
 *  card goes away. A dot on the bell counts the ones not seen yet.
 * =================================================================== */
#define NC_MAX 50
#define NC_W 380
#define NC_IH 68                       /* one notification */
static struct { int icon; char title[64], body[160]; time_t t; } nc_list[NC_MAX];
static int nc_n, nc_unread, nc_open, nc_scroll;

static int bell_x(void) { return TB_RIGHT; }
static int nc_h(void) { return imin(H - TASKBAR_H - 24, 56 + imax(1, nc_n) * NC_IH + 8); }
static int nc_x(void) { return W - NC_W - 12; }
static int nc_y(void) { return H - TASKBAR_H - 10 - nc_h(); }
static void nc_repaint(void) { repaint(nc_x() - 4, 0, NC_W + 16, H - TASKBAR_H); }
static void repaint_bell(void) { repaint(bell_x(), TBI_Y, BELL_W, TBI_H); }

static void nc_add(int icon, const char *title, const char *body) {
    if (nc_n == NC_MAX) nc_n--;
    memmove(nc_list + 1, nc_list, sizeof nc_list[0] * (size_t)nc_n);
    nc_list[0].icon = icon; nc_list[0].t = time(NULL);
    snprintf(nc_list[0].title, sizeof nc_list[0].title, "%s", title);
    snprintf(nc_list[0].body, sizeof nc_list[0].body, "%s", body ? body : "");
    nc_n++;
    if (!nc_open) nc_unread++;
    repaint_bell();
    if (nc_open) nc_repaint();
}
static void nc_set(int open) {
    if (open == nc_open) return;
    int was = nc_open;
    nc_open = open; nc_scroll = 0;
    if (was) nc_repaint();
    if (open) { nc_unread = 0; nc_repaint(); }
    repaint_bell();
}

/* the bell in the corner */
static void draw_bell(void) {
    int x = bell_x(), y = TBI_Y + 3, w = BELL_W - 2, h = TBI_H - 6;
    if (x >= clip_x1 || x + w <= clip_x0 || y >= clip_y1 || y + h <= clip_y0) return;
    int hv = hot.type == HOT_BELL;
    if (hv || nc_open) {
        if (set_style) fill_round(x, y, w, h, 4, nc_open ? TH()->bar_active : TH()->bar_hover, 255);
        else { fill_round_grad(x, y, w, h, 3, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x90, 0xa0), nc_open ? 70 : 45);
               stroke_round(x, y, w, h, 3, RGB(0, 0, 0), 100); }
    }
    uint32_t c = set_style ? TH()->text : RGB(0xf0, 0xf4, 0xf8);
    float cx = x + w / 2.0f, cy = y + h / 2.0f;
    fill_circle(cx, cy - 4, 5.5f, c, 255);                     /* the dome */
    fill_rect((int)cx - 6, (int)cy - 4, 12, 7, c, 255);
    fill_round((int)cx - 8, (int)cy + 2, 16, 3, 1, c, 255);    /* the rim */
    fill_circle(cx, cy + 6.5f, 2, c, 255);                     /* the clapper */
    if (nc_unread) {
        char b[8]; snprintf(b, sizeof b, "%d", imin(nc_unread, 99));
        int bw = imax(16, text_w(b) + 8);
        fill_round(x + w - bw + 2, y - 1, bw, 16, 8, RGB(0x1e, 0x78, 0xe0), 255);
        draw_text(x + w - bw + 2 + (bw - text_w(b)) / 2, y - 1, b, RGB(255, 255, 255), 255);
    }
}

static void nc_when(time_t t, char *b, size_t n) {
    long d = (long)(time(NULL) - t);
    if (d < 60) snprintf(b, n, "now");
    else if (d < 3600) snprintf(b, n, "%ld min ago", d / 60);
    else { struct tm tm; localtime_r(&t, &tm); strftime(b, n, set_clock24 ? "%H:%M" : "%I:%M %p", &tm); }
}
static int nc_rows(void) { return (nc_h() - 56 - 8) / NC_IH; }
static void draw_nc(void) {
    if (!nc_open) return;
    int x = nc_x(), y = nc_y(), w = NC_W, h = nc_h();
    if (x >= clip_x1 || x + w + 4 <= clip_x0 || y >= clip_y1 || y + h + 4 <= clip_y0) return;
    uint32_t fg = set_style ? TH()->text : RGB(0xf2, 0xf6, 0xfa), sub = set_style ? TH()->sub : RGB(0xb4, 0xc4, 0xd6);
    fill_round(x + 2, y + 4, w, h, 10, RGB(0, 0, 0), 60);
    if (set_style) { fill_round(x, y, w, h, 10, TH()->menu, 255); stroke_round(x, y, w, h, 10, TH()->menu_line, 255); }
    else { fill_round_grad(x, y, w, h, 10, RGB(0x2c, 0x3c, 0x52), RGB(0x14, 0x1e, 0x2c), 245); stroke_round(x, y, w, h, 10, RGB(0x6a, 0x84, 0xa8), 200); }
    draw_text_bold(x + 18, y + 18, "Notifications", fg);
    if (nc_n) {
        int hv = hot.type == HOT_NC && hot.a == 1000;
        const char *cl = "Clear all"; int cw = text_w(cl);
        if (hv) fill_round(x + w - cw - 30, y + 12, cw + 16, 28, 5, set_style ? TH()->hover : RGB(255, 255, 255), set_style ? 255 : 40);
        draw_text(x + w - cw - 22, y + 18, cl, set_style ? TH()->accent : RGB(0x9c, 0xd0, 0xff), 255);
    } else {
        draw_text(x + (w - text_w("No new notifications")) / 2, y + 56 + 18, "No new notifications", sub, 255);
        return;
    }
    int rows = nc_rows();
    for (int r = 0; r < rows && nc_scroll + r < nc_n; r++) {
        int i = nc_scroll + r, iy = y + 52 + r * NC_IH;
        int hv = hot.type == HOT_NC && (hot.a == i || hot.a == 2000 + i);
        if (hv) fill_round(x + 8, iy, w - 16, NC_IH - 4, 8, set_style ? TH()->hover : RGB(255, 255, 255), set_style ? 255 : 22);
        draw_icon(nc_list[i].icon ? nc_list[i].icon : ICON_APP, x + 34, iy + 24, 12);
        char tb[24]; nc_when(nc_list[i].t, tb, sizeof tb);
        int maxc = (w - 70 - text_w(tb) - 12) / 8;
        char b[200]; snprintf(b, sizeof b, "%s", nc_list[i].title);
        if (text_len(b) > maxc) { u8_trunc(b, imax(1, maxc - 2)); strcat(b, ".."); }
        draw_text_bold(x + 58, iy + 6, b, fg);
        if (hv) {                                                /* x: remove it */
            int bx = x + w - 34, by = iy + 6, on = hot.a == 2000 + i;
            if (on) fill_round(bx - 4, by - 2, 22, 20, 4, RGB(0xc0, 0x30, 0x30), 255);
            aa_line(bx + 2, by + 3, bx + 12, by + 13, 1.4f, on ? RGB(255, 255, 255) : fg);
            aa_line(bx + 12, by + 3, bx + 2, by + 13, 1.4f, on ? RGB(255, 255, 255) : fg);
        } else draw_text(x + w - 18 - text_w(tb), iy + 6, tb, sub, 255);
        const char *s = nc_list[i].body; int line = 0, mc = (w - 80) / 8;
        while (*s && line < 2) {
            int L = (int)strlen(s), n = imin(L, mc);
            if (n < L) { int k = n; while (k > 0 && s[k] != ' ') k--; if (k > 0) n = k; }
            if (line == 1 && n < L) { snprintf(b, sizeof b, "%.*s...", imax(0, mc - 3), s); n = L; }
            else snprintf(b, sizeof b, "%.*s", n, s);
            draw_text(x + 58, iy + 25 + line * 17, b, sub, 255);
            s += n; while (*s == ' ') s++;
            line++;
        }
    }
}
/* what the mouse is on: 1000 Clear all, i a notification, 2000+i its x; -1 nothing, -2 the panel */
static int nc_hit(void) {
    int x = nc_x(), y = nc_y(), w = NC_W;
    if (!in_rect(x, y, w, nc_h())) return -1;
    if (nc_n && my >= y + 10 && my < y + 42 && mx >= x + w - 140) return 1000;
    int r = (my - y - 52) / NC_IH, i = nc_scroll + r;
    if (my >= y + 52 && r < nc_rows() && i < nc_n) {
        if (mx >= x + w - 40 && my < y + 52 + r * NC_IH + 28) return 2000 + i;
        return i;
    }
    return -2;
}
static void nc_click(int id) {
    if (id == 1000) { nc_n = 0; nc_scroll = 0; nc_repaint(); return; }
    if (id >= 2000 && id - 2000 < nc_n) {
        int i = id - 2000;
        memmove(nc_list + i, nc_list + i + 1, sizeof nc_list[0] * (size_t)(nc_n - i - 1)); nc_n--;
        nc_scroll = imin(nc_scroll, imax(0, nc_n - nc_rows()));
        nc_repaint(); update_hover(); return;
    }
}
static void nc_wheel(int amount) {
    int ns = clampi(nc_scroll + amount, 0, imax(0, nc_n - nc_rows()));
    if (ns != nc_scroll) { nc_scroll = ns; nc_repaint(); }
}
