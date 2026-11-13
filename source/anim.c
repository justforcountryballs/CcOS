/* ===================================================================
 *  Window animations, like a Mac:
 *   minimize - the window pours into its Dock/taskbar icon ("genie")
 *   restore  - it pours back out of the icon
 *   open     - it grows in from a little smaller, fading in
 *   close    - it shrinks a little and fades away
 *  A picture of the window is taken once (snap) and bent each frame;
 *  the real window stays hidden until the animation ends.
 * =================================================================== */
static void repaint_window(int i);
enum { AN_NONE, AN_MIN, AN_RESTORE, AN_OPEN, AN_CLOSE };
static struct {
    int kind, win, x, y, w, h, pending;          /* pending: take the picture on the next frame (the app may still be setting up) */
    uint32_t *px;
    int ix, iy, iw;                              /* the icon: centre x, top, width */
    long long t0; int dur;
    int bx0, by0, bx1, by1;                      /* what was drawn last frame */
} an;
/* set_anim: Settings > Appearance, animations on/off (declared with the settings) */

static int anim_hides(int idx) { return an.kind != AN_NONE && an.win == idx && (an.kind == AN_RESTORE || an.kind == AN_OPEN); }

/* a picture of window idx, alone (nothing on top of it) */
static uint32_t *anim_snap(int idx, int *x, int *y, int *w, int *h) {
    Win *wn = &wins[idx];
    int X = imax(0, wn->x), Y = imax(0, wn->y), X1 = imin(W, wn->x + wn->w), Y1 = imin(H, wn->y + wn->h);
    if (X1 <= X || Y1 <= Y) return NULL;
    int ww = X1 - X, hh = Y1 - Y;
    uint32_t *save = malloc((size_t)ww * hh * 4), *out = malloc((size_t)ww * hh * 4);
    if (!save || !out) { free(save); free(out); return NULL; }
    for (int r = 0; r < hh; r++) memcpy(save + (size_t)r * ww, &back[(size_t)(Y + r) * W + X], (size_t)ww * 4);
    int ox0 = clip_x0, oy0 = clip_y0, ox1 = clip_x1, oy1 = clip_y1, om = wn->minimized, ok = an.kind; int ow = an.win;
    clip_x0 = X; clip_y0 = Y; clip_x1 = X1; clip_y1 = Y1;
    for (int r = 0; r < hh; r++) memcpy(&back[(size_t)(Y + r) * W + X], &wall[(size_t)(Y + r) * W + X], (size_t)ww * 4);
    wn->minimized = 0; an.kind = AN_NONE;
    draw_window(idx);
    wn->minimized = om; an.kind = ok; an.win = ow;
    for (int r = 0; r < hh; r++) {
        memcpy(out + (size_t)r * ww, &back[(size_t)(Y + r) * W + X], (size_t)ww * 4);
        memcpy(&back[(size_t)(Y + r) * W + X], save + (size_t)r * ww, (size_t)ww * 4);
    }
    clip_x0 = ox0; clip_y0 = oy0; clip_x1 = ox1; clip_y1 = oy1;
    free(save);
    *x = X; *y = Y; *w = ww; *h = hh;
    return out;
}

/* where the window's icon is: in the Dock, or its taskbar button */
static void anim_icon(int idx, int *cx, int *top, int *w) {
    TbItem it[MAXPINS + MAXWIN]; int n = tb_items(it), k = -1;
    for (int j = 0; j < n; j++) if (it[j].win == idx) k = j;
    if (set_dock) {
        DockSlot s[DOCK_MAXS]; int sx, sw, ns = dock_layout(s, &sx, &sw);
        for (int j = 0; j < ns; j++)
            if ((s[j].kind == DS_TASK && s[j].k == k && k >= 0) || (s[j].kind == DS_FINDER && !strcmp(wins[idx].cmd, "@finder"))) {
                *cx = (int)s[j].cx; *w = (int)s[j].size; *top = H - DOCK_BOT - DOCK_PAD - (int)s[j].size; return;
            }
        *cx = W / 2; *w = DOCK_B; *top = H - DOCK_BOT - DOCK_PAD - DOCK_B; return;
    }
    if (k >= 0) { int x, y, w2, h2; task_rect(k, &x, &y, &w2, &h2); *cx = x + w2 / 2; *top = y; *w = imin(w2, 40); return; }
    *cx = 60; *top = H - TASKBAR_H; *w = 40;
}

static void anim_mark(void) {                     /* repaint what was drawn last frame and what will be now */
    int x0 = an.bx0, y0 = an.by0, x1 = an.bx1, y1 = an.by1;
    int nx0 = imin(an.x, an.ix - an.iw / 2) - 4, ny0 = an.y - 4, nx1 = imax(an.x + an.w, an.ix + an.iw / 2) + 4, ny1 = imax(an.y + an.h, an.iy + an.iw) + 4;
    if (an.kind == AN_OPEN || an.kind == AN_CLOSE) { nx0 = an.x - 4; nx1 = an.x + an.w + 4; ny1 = an.y + an.h + 4; }
    an.bx0 = nx0; an.by0 = ny0; an.bx1 = nx1; an.by1 = ny1;
    if (x1 > x0) { nx0 = imin(nx0, x0); ny0 = imin(ny0, y0); nx1 = imax(nx1, x1); ny1 = imax(ny1, y1); }
    repaint(nx0, ny0, nx1 - nx0, ny1 - ny0);
}

static void anim_stop(void) {
    int k = an.kind, i = an.win;
    an.kind = AN_NONE; free(an.px); an.px = NULL;
    if (an.bx1 > an.bx0) repaint(an.bx0, an.by0, an.bx1 - an.bx0, an.by1 - an.by0);
    if (k == AN_CLOSE || k == AN_MIN) repaint(an.x - 4, an.y - 4, an.w + 8, an.h + 8);   /* (stopped before its first frame) */
    an.bx0 = an.bx1 = 0;
    if ((k == AN_RESTORE || k == AN_OPEN) && i >= 0 && wins[i].open) repaint_window(i);
}
static void anim_start(int kind, int idx) {
    if (!set_anim || W <= 0) return;
    if (an.kind != AN_NONE) anim_stop();
    an.kind = kind; an.win = idx; an.pending = 1; an.px = NULL; an.bx0 = an.bx1 = 0;
    an.dur = kind == AN_MIN || kind == AN_RESTORE ? 420 : kind == AN_OPEN ? 200 : 170;
    if (kind == AN_MIN || kind == AN_CLOSE) {    /* the picture now, while the window is still there */
        an.px = anim_snap(idx, &an.x, &an.y, &an.w, &an.h);
        if (!an.px) { an.kind = AN_NONE; return; }
        an.pending = 0; an.t0 = ui_ms();
        anim_icon(idx, &an.ix, &an.iy, &an.iw);
    }
}

/* each frame (main loop): how long until the next one, or -1 */
static int anim_tick(void) {
    if (an.kind == AN_NONE) return -1;
    if (an.pending) {
        if (an.win < 0 || !wins[an.win].open) { an.kind = AN_NONE; return -1; }
        an.px = anim_snap(an.win, &an.x, &an.y, &an.w, &an.h);
        if (!an.px) { anim_stop(); return -1; }
        anim_icon(an.win, &an.ix, &an.iy, &an.iw);
        an.pending = 0; an.t0 = ui_ms();
    }
    if (ui_ms() - an.t0 >= an.dur) { anim_stop(); return -1; }
    anim_mark();
    return 15;
}

static float an_smooth(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

/* in repaint(): the bent picture, over the windows (under the Dock) */
static void anim_draw(void) {
    if (an.kind == AN_NONE || an.pending || !an.px) return;
    float p = clampf((float)(ui_ms() - an.t0) / an.dur, 0, 1);
    if (an.kind == AN_OPEN || an.kind == AN_CLOSE) {
        float t = an.kind == AN_OPEN ? an_smooth(p) : 1 - an_smooth(p);
        float sc = 0.86f + 0.14f * t; int alpha = (int)(255 * t);
        float cx = an.x + an.w / 2.0f, cy = an.y + an.h / 2.0f;
        int dw = (int)(an.w * sc), dh = (int)(an.h * sc), dx = (int)(cx - dw / 2.0f), dy = (int)(cy - dh / 2.0f);
        int x0 = imax(dx, clip_x0), x1 = imin(dx + dw, clip_x1), y0 = imax(dy, clip_y0), y1 = imin(dy + dh, clip_y1);
        for (int y = y0; y < y1; y++) {
            const uint32_t *row = an.px + (size_t)((y - dy) * an.h / dh) * an.w;
            for (int x = x0; x < x1; x++) blend(x, y, row[(x - dx) * an.w / dw], alpha);
        }
        return;
    }
    /* genie: p 0 = the window, 1 = in the icon */
    if (an.kind == AN_RESTORE) p = 1 - p;
    float e = p < 0.5f ? 0 : an_smooth((p - 0.5f) * 2);          /* second half: it slides in */
    float k = p < 0.5f ? an_smooth(p * 2) : 1;                    /* first half: the bottom bends to the icon */
    float T = (float)an.y, B = (float)(an.y + an.h), L = (float)an.x, R = (float)(an.x + an.w);
    float iyb = (float)(an.iy + an.iw * 0.6f), iL = an.ix - an.iw * 0.45f, iR = an.ix + an.iw * 0.45f;
    float ytop = T + (iyb - T) * e, ybot = B + (iyb - B) * k;
    if (ybot - ytop < 1) return;
    int y0 = imax((int)ytop, clip_y0), y1 = imin((int)ybot, clip_y1);
    for (int y = y0; y < y1; y++) {
        float g = an_smooth((y - T) / fmaxf(1, iyb - T)) * k;    /* the funnel: narrower towards the icon */
        float xl = L + (iL - L) * g, xr = R + (iR - R) * g;
        if (xr - xl < 1) continue;
        float f = (y - ytop) / (ybot - ytop);
        const uint32_t *row = an.px + (size_t)imin(an.h - 1, (int)(f * an.h)) * an.w;
        int xa = imax((int)xl, clip_x0), xb = imin((int)xr, clip_x1);
        uint32_t *d = &back[(size_t)y * W];
        for (int x = xa; x < xb; x++) d[x] = row[imin(an.w - 1, (int)((x - xl) / (xr - xl) * an.w))];
    }
}
