/* ===================================================================
 *  Paint: draw on a picture. Tools: pencil, brush, eraser, line,
 *  rectangle, ellipse, fill, colour picker. Ctrl+Z undo, Ctrl+Y redo,
 *  Ctrl+V pastes the last copied picture (a screenshot...), Ctrl+S saves
 *  (Images > "Drawing ....png", or back into the picture you opened).
 * =================================================================== */
#define PT_W 920
#define PT_H 600
#define PT_BAR 54                       /* the toolbar */
#define PT_UNDO 12
enum { PT_PENCIL, PT_BRUSH, PT_ERASER, PT_LINE, PT_RECT, PT_ELLIPSE, PT_FILL, PT_PICK, PT_NTOOL };
static const char *pt_tool_name[PT_NTOOL] = { "Pencil", "Brush", "Eraser", "Line", "Box", "Circle", "Fill", "Pick" };
static const uint32_t pt_pal[16] = {
    0x000000, 0x7f7f7f, 0x880015, 0xed1c24, 0xff7f27, 0xfff200, 0x22b14c, 0x00a2e8,
    0xffffff, 0xc3c3c3, 0xb97a57, 0xffaec9, 0xffc90e, 0xefe4b0, 0xb5e61d, 0x3f48cc };
static const int pt_sizes[4] = { 2, 5, 10, 20 };
static struct {
    uint32_t *px; int w, h;                /* the picture */
    uint32_t *undo[PT_UNDO]; int nundo, nredo; uint32_t *redo[PT_UNDO];
    int tool, size, sx, sy, lx, ly, down, scrollx, scrolly, dirty;
    uint32_t col;
    char path[300];                        /* where it's saved ("" = not yet) */
} pt = { .tool = PT_BRUSH, .size = 1, .col = 0x000000 };

static void paint_draw(int cx, int cy, int cw, int ch);
static unsigned char *png_encode(const uint32_t *src, int sw, int x, int y, int w, int h, size_t *outn);

static void pt_new(int w, int h) {
    free(pt.px); pt.px = malloc((size_t)w * h * 4); pt.w = w; pt.h = h;
    if (pt.px) for (size_t i = 0; i < (size_t)w * h; i++) pt.px[i] = 0xffffff;
    for (int i = 0; i < pt.nundo; i++) free(pt.undo[i]);
    for (int i = 0; i < pt.nredo; i++) free(pt.redo[i]);
    pt.nundo = pt.nredo = 0; pt.scrollx = pt.scrolly = 0; pt.dirty = 0; pt.path[0] = 0;
}
static uint32_t *pt_copy(void) { uint32_t *c = malloc((size_t)pt.w * pt.h * 4); if (c) memcpy(c, pt.px, (size_t)pt.w * pt.h * 4); return c; }
static void pt_push_undo(void) {
    if (!pt.px) return;
    if (pt.nundo == PT_UNDO) { free(pt.undo[0]); memmove(pt.undo, pt.undo + 1, sizeof pt.undo[0] * (PT_UNDO - 1)); pt.nundo--; }
    pt.undo[pt.nundo] = pt_copy(); if (pt.undo[pt.nundo]) pt.nundo++;
    for (int i = 0; i < pt.nredo; i++) free(pt.redo[i]);
    pt.nredo = 0; pt.dirty = 1;
}
static void pt_undo(int redo) {
    uint32_t **from = redo ? pt.redo : pt.undo, **to = redo ? pt.undo : pt.redo;
    int *nf = redo ? &pt.nredo : &pt.nundo, *nt = redo ? &pt.nundo : &pt.nredo;
    if (!*nf) return;
    uint32_t *cur = pt_copy();
    if (cur && *nt < PT_UNDO) to[(*nt)++] = cur; else free(cur);
    uint32_t *p = from[--(*nf)];
    memcpy(pt.px, p, (size_t)pt.w * pt.h * 4); free(p);
}

/* ---- drawing into the picture ---- */
static void pt_dot(uint32_t *dst, int x, int y, int r, uint32_t c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        if (i * i + j * j > r * r + r / 2) continue;
        int px = x + i, py = y + j;
        if (px >= 0 && py >= 0 && px < pt.w && py < pt.h) dst[py * pt.w + px] = c;
    }
}
static void pt_line(uint32_t *dst, int x0, int y0, int x1, int y1, int r, uint32_t c) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = imax(dx, dy);
    for (int k = 0; k <= n; k++) {
        int x = n ? x0 + (x1 - x0) * k / n : x0, y = n ? y0 + (y1 - y0) * k / n : y0;
        if (r <= 0) { if (x >= 0 && y >= 0 && x < pt.w && y < pt.h) dst[y * pt.w + x] = c; }
        else pt_dot(dst, x, y, r, c);
    }
}
static int pt_radius(void) { return pt.tool == PT_PENCIL ? 0 : pt.tool == PT_ERASER ? pt_sizes[pt.size] : pt_sizes[pt.size] / 2; }
static void pt_shape(uint32_t *dst, int tool, int x0, int y0, int x1, int y1) {
    int r = pt_sizes[pt.size] / 2;
    if (tool == PT_LINE) pt_line(dst, x0, y0, x1, y1, r, pt.col);
    else if (tool == PT_RECT) {
        pt_line(dst, x0, y0, x1, y0, r, pt.col); pt_line(dst, x1, y0, x1, y1, r, pt.col);
        pt_line(dst, x1, y1, x0, y1, r, pt.col); pt_line(dst, x0, y1, x0, y0, r, pt.col);
    } else if (tool == PT_ELLIPSE) {
        float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f, rx = fabsf(x1 - x0) / 2.0f, ry = fabsf(y1 - y0) / 2.0f;
        int n = (int)(rx + ry) * 2 + 16, px = (int)(cx + rx), py = (int)cy;
        for (int k = 1; k <= n; k++) {
            float t = k * 6.2832f / n; int x = (int)(cx + cosf(t) * rx), y = (int)(cy + sinf(t) * ry);
            pt_line(dst, px, py, x, y, r, pt.col); px = x; py = y;
        }
    }
}
static void pt_fill(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= pt.w || y >= pt.h) return;
    uint32_t old = pt.px[y * pt.w + x];
    if (old == c) return;
    int cap = 1 << 16, n = 0; int *st = malloc(sizeof(int) * 2 * cap);
    if (!st) return;
    st[n++] = x; st[n++] = y;
    while (n) {
        int py = st[--n], px = st[--n];
        int l = px; while (l > 0 && pt.px[py * pt.w + l - 1] == old) l--;
        int r = px; while (r < pt.w - 1 && pt.px[py * pt.w + r + 1] == old) r++;
        for (int i = l; i <= r; i++) {
            pt.px[py * pt.w + i] = c;
            for (int d = -1; d <= 1; d += 2) {
                int ny = py + d;
                if (ny < 0 || ny >= pt.h || pt.px[ny * pt.w + i] != old) continue;
                if (i > l && pt.px[ny * pt.w + i - 1] == old) continue;    /* one seed per run */
                if (n + 2 > 2 * cap) { int *ns = realloc(st, sizeof(int) * 4 * cap); if (!ns) { free(st); return; } st = ns; cap *= 2; }
                st[n++] = i; st[n++] = ny;
            }
        }
    }
    free(st);
}

/* ---- where things are ---- */
static void pt_canvas_area(int cw, int ch, int *ax, int *ay, int *aw, int *ah) { *ax = 8; *ay = PT_BAR + 8; *aw = cw - 16; *ah = ch - PT_BAR - 16; }
static int pt_to_pic(int lx, int ly, int cw, int ch, int *px, int *py) {
    int ax, ay, aw, ah; pt_canvas_area(cw, ch, &ax, &ay, &aw, &ah);
    *px = lx - ax + pt.scrollx; *py = ly - ay + pt.scrolly;
    return lx >= ax && ly >= ay && lx < ax + aw && ly < ay + ah;
}
/* toolbar: buttons (ids) laid out left to right */
enum { PB_NEW = 1, PB_SAVE, PB_UNDO, PB_REDO, PB_TOOL = 10, PB_SIZE = 30, PB_COL = 40 };
static void pt_btn_rect(int id, int *x, int *y, int *w, int *h) {
    *y = 8; *h = 38;
    if (id >= PB_NEW && id <= PB_REDO) { *x = 8 + (id - PB_NEW) * 46; *w = 44; return; }
    if (id >= PB_TOOL && id < PB_TOOL + PT_NTOOL) { *x = 198 + (id - PB_TOOL) * 50; *w = 48; return; }
    if (id >= PB_SIZE && id < PB_SIZE + 4) { *x = 604 + (id - PB_SIZE) * 28; *w = 26; return; }
    if (id >= PB_COL && id < PB_COL + 16) { int k = id - PB_COL; *x = 764 + (k % 8) * 18; *y = 8 + (k / 8) * 20; *w = 16; *h = 18; return; }
    *x = *y = *w = *h = 0;
}
static const char *pt_btn_label[] = { "", "New", "Save", "Undo", "Redo" };

static void pt_tool_icon(int t, int cx, int cy, uint32_t c) {
    switch (t) {
    case PT_PENCIL: aa_line(cx - 7, cy + 7, cx + 6, cy - 6, 2.4f, c); fill_circle(cx - 7, cy + 7, 1.6f, c, 255); break;
    case PT_BRUSH: aa_line(cx + 6, cy - 7, cx - 1, cy + 1, 2.2f, c); fill_circle(cx - 3, cy + 4, 4, c, 255); break;
    case PT_ERASER: fill_round(cx - 8, cy - 4, 16, 9, 2, RGB(0xe8, 0x80, 0x90), 255); stroke_round(cx - 8, cy - 4, 16, 9, 2, c, 255); break;
    case PT_LINE: aa_line(cx - 8, cy + 7, cx + 8, cy - 7, 1.8f, c); break;
    case PT_RECT: stroke_round(cx - 8, cy - 6, 17, 13, 1, c, 255); break;
    case PT_ELLIPSE:
        for (int a = 0; a < 32; a++) { float t0 = a * 6.2832f / 32, t1 = (a + 1) * 6.2832f / 32;
            aa_line(cx + cosf(t0) * 9, cy + sinf(t0) * 6.5f, cx + cosf(t1) * 9, cy + sinf(t1) * 6.5f, 1.5f, c); }
        break;
    case PT_FILL: fill_round(cx - 6, cy - 4, 11, 11, 2, c, 255); fill_circle(cx + 7, cy + 5, 2.5f, RGB(0x00, 0xa2, 0xe8), 255); break;
    case PT_PICK: aa_line(cx - 7, cy + 7, cx + 3, cy - 3, 2, c); fill_circle(cx + 5, cy - 5, 3.5f, c, 255); break;
    }
}
static void paint_draw(int cx, int cy, int cw, int ch) {
    const int dark = set_style && set_dark;
    uint32_t bar = dark ? RGB(0x2a, 0x2b, 0x30) : RGB(0xf3, 0xf5, 0xf8), text = dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28);
    fill_rect(cx, cy, cw, PT_BAR, bar, 255);
    fill_rect(cx, cy + PT_BAR - 1, cw, 1, dark ? RGB(0x44, 0x46, 0x4c) : RGB(0xd4, 0xd8, 0xde), 255);
    fill_rect(cx, cy + PT_BAR, cw, ch - PT_BAR, dark ? RGB(0x18, 0x19, 0x1c) : RGB(0xc8, 0xd0, 0xdc), 255);
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == paint_draw ? hot.b : -1;
    for (int id = PB_NEW; id <= PB_REDO; id++) {
        int x, y, w, h; pt_btn_rect(id, &x, &y, &w, &h);
        if (hv == id) fill_round(cx + x, cy + y, w, h, 5, dark ? RGB(0x40, 0x42, 0x48) : RGB(0xe0, 0xe6, 0xee), 255);
        draw_text(cx + x + (w - text_w(pt_btn_label[id])) / 2, cy + y + 11, pt_btn_label[id], text, 255);
    }
    for (int t = 0; t < PT_NTOOL; t++) {
        int x, y, w, h; pt_btn_rect(PB_TOOL + t, &x, &y, &w, &h);
        int on = pt.tool == t;
        if (on || hv == PB_TOOL + t) fill_round(cx + x, cy + y, w, h, 5, on ? (dark ? RGB(0x3a, 0x5a, 0x8a) : RGB(0xd2, 0xe4, 0xfb)) : (dark ? RGB(0x40, 0x42, 0x48) : RGB(0xe0, 0xe6, 0xee)), 255);
        pt_tool_icon(t, cx + x + w / 2, cy + y + 11, text);
        draw_text(cx + x + (w - text_w(pt_tool_name[t])) / 2, cy + y + 21, pt_tool_name[t], text, 255);
    }
    for (int k = 0; k < 4; k++) {
        int x, y, w, h; pt_btn_rect(PB_SIZE + k, &x, &y, &w, &h);
        if (pt.size == k || hv == PB_SIZE + k) fill_round(cx + x, cy + y, w, h, 5, pt.size == k ? (dark ? RGB(0x3a, 0x5a, 0x8a) : RGB(0xd2, 0xe4, 0xfb)) : (dark ? RGB(0x40, 0x42, 0x48) : RGB(0xe0, 0xe6, 0xee)), 255);
        fill_circle(cx + x + w / 2.0f, cy + y + h / 2.0f, 1.2f + k * 2.2f, text, 255);
    }
    for (int k = 0; k < 16; k++) {
        int x, y, w, h; pt_btn_rect(PB_COL + k, &x, &y, &w, &h);
        fill_rect(cx + x, cy + y, w, h, pt_pal[k], 255);
        stroke_round(cx + x, cy + y, w, h, 1, hv == PB_COL + k ? RGB(0x2a, 0x78, 0xd8) : RGB(0x80, 0x84, 0x8c), 255);
    }
    fill_rect(cx + 722, cy + 10, 34, 34, pt.col, 255);                     /* the colour now */
    stroke_round(cx + 722, cy + 10, 34, 34, 1, RGB(0x40, 0x44, 0x4c), 255);
    /* the picture */
    if (!pt.px) return;
    int ax, ay, aw, ah; pt_canvas_area(cw, ch, &ax, &ay, &aw, &ah);
    int dw = imin(aw, pt.w - pt.scrollx), dh = imin(ah, pt.h - pt.scrolly);
    fill_rect(cx + ax + 3, cy + ay + 3, dw, dh, RGB(0, 0, 0), 50);
    int x0 = imax(cx + ax, clip_x0), x1 = imin(cx + ax + dw, clip_x1), y0 = imax(cy + ay, clip_y0), y1 = imin(cy + ay + dh, clip_y1);
    for (int y = y0; y < y1; y++)
        memcpy(&back[(size_t)y * W + x0], &pt.px[(size_t)(y - cy - ay + pt.scrolly) * pt.w + (x0 - cx - ax + pt.scrollx)], (size_t)imax(0, x1 - x0) * 4);
    if (pt.down && (pt.tool == PT_LINE || pt.tool == PT_RECT || pt.tool == PT_ELLIPSE)) {   /* the shape being dragged */
        uint32_t *tmp = pt_copy();
        if (tmp) {
            pt_shape(tmp, pt.tool, pt.sx, pt.sy, pt.lx, pt.ly);
            for (int y = y0; y < y1; y++)
                memcpy(&back[(size_t)y * W + x0], &tmp[(size_t)(y - cy - ay + pt.scrolly) * pt.w + (x0 - cx - ax + pt.scrollx)], (size_t)imax(0, x1 - x0) * 4);
            free(tmp);
        }
    }
}
static int paint_hit(int lx, int ly, int cw, int ch) {
    (void)cw; (void)ch;
    static const int ids[] = { PB_NEW, PB_SAVE, PB_UNDO, PB_REDO };
    for (int i = 0; i < 4; i++) { int x, y, w, h; pt_btn_rect(ids[i], &x, &y, &w, &h); if (lx >= x && lx < x + w && ly >= y && ly < y + h) return ids[i]; }
    for (int t = 0; t < PT_NTOOL; t++) { int x, y, w, h; pt_btn_rect(PB_TOOL + t, &x, &y, &w, &h); if (lx >= x && lx < x + w && ly >= y && ly < y + h) return PB_TOOL + t; }
    for (int k = 0; k < 4; k++) { int x, y, w, h; pt_btn_rect(PB_SIZE + k, &x, &y, &w, &h); if (lx >= x && lx < x + w && ly >= y && ly < y + h) return PB_SIZE + k; }
    for (int k = 0; k < 16; k++) { int x, y, w, h; pt_btn_rect(PB_COL + k, &x, &y, &w, &h); if (lx >= x && lx < x + w && ly >= y && ly < y + h) return PB_COL + k; }
    return -1;
}
static void paint_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(paint_draw); int cx = 0, cy = 0, cw = 0, ch = 0;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    pt_btn_rect(id, x, y, w, h); *x += cx - 2; *y += cy - 2; *w += 4; *h += 4;
}
static void pt_save(void) {
    if (!pt.px) return;
    char p[320];
    const char *e = strrchr(pt.path, '.');
    if (pt.path[0] && e && !strcasecmp(e, ".png")) snprintf(p, sizeof p, "%s", pt.path);
    else {
        mkdir("/data", 0755); mkdir("/data/Images", 0755);
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H-%M-%S", &tm);
        snprintf(p, sizeof p, "/data/Images/Drawing %s.png", b);
    }
    size_t n; unsigned char *png = png_encode(pt.px, pt.w, 0, 0, pt.w, pt.h, &n);
    FILE *f = png ? fopen(p, "wb") : NULL;
    int ok = f && fwrite(png, 1, n, f) == n;
    if (f) fclose(f);
    free(png);
    if (ok) {
        snprintf(pt.path, sizeof pt.path, "%s", p); pt.dirty = 0; bg_sync();
        const char *nm = strrchr(p, '/'); toast(ICON_PAINT, "Picture saved", nm ? nm + 1 : p);
    } else toast(ICON_PAINT, "Couldn't save the picture", "Is D: full?");
}
static void pt_paste(void) {                         /* the newest picture in the clipboard history */
    for (int i = 0; i < ch_n; i++) {
        if (!ch_list[i].png) continue;
        int iw, ih, c; uint8_t *px = stbi_load_from_memory((const uint8_t *)ch_list[i].data, (int)ch_list[i].n, &iw, &ih, &c, 4);
        if (!px) return;
        if (iw > pt.w || ih > pt.h) {                    /* bigger than the picture: the picture grows */
            int nw = imax(pt.w, iw), nh = imax(pt.h, ih);
            uint32_t *np = malloc((size_t)nw * nh * 4);
            if (np) {
                for (size_t k = 0; k < (size_t)nw * nh; k++) np[k] = 0xffffff;
                for (int y = 0; y < pt.h; y++) memcpy(np + (size_t)y * nw, pt.px + (size_t)y * pt.w, (size_t)pt.w * 4);
                for (int k = 0; k < pt.nundo; k++) free(pt.undo[k]);
                for (int k = 0; k < pt.nredo; k++) free(pt.redo[k]);
                pt.nundo = pt.nredo = 0;
                free(pt.px); pt.px = np; pt.w = nw; pt.h = nh;
            }
        }
        pt_push_undo();
        for (int y = 0; y < imin(ih, pt.h); y++) for (int x = 0; x < imin(iw, pt.w); x++) {
            const uint8_t *q = px + ((size_t)y * iw + x) * 4;
            if (q[3] > 127) pt.px[y * pt.w + x] = RGB(q[0], q[1], q[2]);
        }
        stbi_image_free(px);
        repaint_client(paint_draw);
        return;
    }
}
static void paint_click(int lx, int ly, int cw, int ch) {
    int id = paint_hit(lx, ly, cw, ch);
    if (id == PB_NEW) { pt_new(800, 480); repaint_client(paint_draw); return; }
    if (id == PB_SAVE) { pt_save(); return; }
    if (id == PB_UNDO || id == PB_REDO) { pt_undo(id == PB_REDO); repaint_client(paint_draw); return; }
    if (id >= PB_TOOL && id < PB_TOOL + PT_NTOOL) { pt.tool = id - PB_TOOL; repaint_client(paint_draw); return; }
    if (id >= PB_SIZE && id < PB_SIZE + 4) { pt.size = id - PB_SIZE; repaint_client(paint_draw); return; }
    if (id >= PB_COL && id < PB_COL + 16) { pt.col = pt_pal[id - PB_COL]; if (pt.tool == PT_ERASER || pt.tool == PT_PICK) pt.tool = PT_BRUSH; repaint_client(paint_draw); return; }
    int px, py;
    if (!pt.px || !pt_to_pic(lx, ly, cw, ch, &px, &py) || px >= pt.w || py >= pt.h) return;
    if (pt.tool == PT_PICK) { pt.col = pt.px[py * pt.w + px] & 0xffffff; pt.tool = PT_BRUSH; repaint_client(paint_draw); return; }
    pt_push_undo();
    if (pt.tool == PT_FILL) { pt_fill(px, py, pt.col); repaint_client(paint_draw); return; }
    pt.down = 1; pt.sx = pt.lx = px; pt.sy = pt.ly = py;
    if (pt.tool == PT_PENCIL || pt.tool == PT_BRUSH || pt.tool == PT_ERASER) {
        pt_line(pt.px, px, py, px, py, pt_radius(), pt.tool == PT_ERASER ? 0xffffff : pt.col);
        repaint_client(paint_draw);
    }
}
static void paint_drag(int lx, int ly, int cw, int ch) {
    if (!pt.down) return;
    int px, py; pt_to_pic(lx, ly, cw, ch, &px, &py);
    if (pt.tool == PT_PENCIL || pt.tool == PT_BRUSH || pt.tool == PT_ERASER)
        pt_line(pt.px, pt.lx, pt.ly, px, py, pt_radius(), pt.tool == PT_ERASER ? 0xffffff : pt.col);
    pt.lx = px; pt.ly = py;
    repaint_client(paint_draw);
}
static void paint_release(void) {
    if (!pt.down) return;
    pt.down = 0;
    if (pt.tool == PT_LINE || pt.tool == PT_RECT || pt.tool == PT_ELLIPSE) pt_shape(pt.px, pt.tool, pt.sx, pt.sy, pt.lx, pt.ly);
    repaint_client(paint_draw);
}
static void paint_wheel(int amount) {
    int i = win_of(paint_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    int ax, ay, aw, ah; pt_canvas_area(cw, ch, &ax, &ay, &aw, &ah);
    if (shift) pt.scrollx = clampi(pt.scrollx - amount * 60, 0, imax(0, pt.w - aw));
    else pt.scrolly = clampi(pt.scrolly - amount * 60, 0, imax(0, pt.h - ah));
    repaint_client(paint_draw);
}
static void paint_key(int code, int mods) {
    if (!(mods & MOD_CTRL)) return;
    if (code == KEY_Z) pt_undo(0);
    else if (code == KEY_Y) pt_undo(1);
    else if (code == KEY_S) { pt_save(); return; }
    else if (code == KEY_V) pt_paste();
    else if (code == KEY_N) pt_new(800, 480);
    else return;
    repaint_client(paint_draw);
}
/* open Paint, with a picture (path) or a new white one */
static void paint_open(const char *path) {
    if (path && path[0]) {
        int iw, ih, c; uint8_t *px = stbi_load(path, &iw, &ih, &c, 4);
        if (px) {
            pt_new(iw, ih);
            if (pt.px) for (size_t k = 0; k < (size_t)iw * ih; k++) pt.px[k] = RGB(px[k * 4], px[k * 4 + 1], px[k * 4 + 2]);
            stbi_image_free(px);
            snprintf(pt.path, sizeof pt.path, "%s", path);
        }
    } else if (!pt.px) pt_new(800, 480);
    static const Win t = { .title = "Paint", .icon = ICON_PAINT, .draw = paint_draw, .click = paint_click, .hit = paint_hit,
                           .elem_rect = paint_elem_rect, .drag = paint_drag, .release = paint_release, .wheel = paint_wheel,
                           .key = paint_key };
    open_win(&t, imin(W - 60, PT_W), imin(H - TASKBAR_H - 60, PT_H));
    repaint_client(paint_draw);
}
static void draw_paint_icon(int cx, int cy, int r) {          /* a palette */
    fill_circle(cx, cy, r * 0.95f, RGB(0xf0, 0xd8, 0xa8), 255);
    fill_circle(cx + r * 0.35f, cy + r * 0.4f, r * 0.22f, RGB(0xff, 0xff, 0xff), 255);
    fill_circle(cx - r * 0.45f, cy - r * 0.2f, r * 0.2f, RGB(0xed, 0x1c, 0x24), 255);
    fill_circle(cx - r * 0.1f, cy - r * 0.55f, r * 0.2f, RGB(0xff, 0xc9, 0x0e), 255);
    fill_circle(cx + r * 0.38f, cy - r * 0.35f, r * 0.2f, RGB(0x22, 0xb1, 0x4c), 255);
    fill_circle(cx - r * 0.45f, cy + r * 0.38f, r * 0.2f, RGB(0x00, 0xa2, 0xe8), 255);
}
