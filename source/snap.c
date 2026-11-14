/* ===================================================================
 *  Snapping: drag a window by its title to the left or right edge and
 *  it fills that half; to a corner, a quarter; to the top, the whole
 *  screen. A see-through box shows where it will go. Dragging it away
 *  gives it back its old size. Windows + arrows do the same.
 * =================================================================== */
static void move_win(int idx, int nx, int ny);
static void minimize_win(int idx);
static void toggle_max(int idx);
enum { SZ_NONE, SZ_L, SZ_R, SZ_TL, SZ_TR, SZ_BL, SZ_BR, SZ_MAX };
static int snap_zone, drag_sx, drag_sy, drag_armed;      /* armed: a maximized/snapped window, not moved yet */

static int snap_mon_x;                                    /* a point on the screen being snapped on */
static void mon_of(int x, int *x0, int *w, int *h, int *wh);
static void snap_rect(int z, int *x, int *y, int *w, int *h) {
    int mx0, mw, mh, wh; mon_of(snap_mon_x, &mx0, &mw, &mh, &wh);
    int aw = mw, ah = wh, hw = aw / 2, hh = ah / 2;
    switch (z) {
    case SZ_L:  *x = 0;  *y = 0;  *w = hw;      *h = ah; break;
    case SZ_R:  *x = hw; *y = 0;  *w = aw - hw; *h = ah; break;
    case SZ_TL: *x = 0;  *y = 0;  *w = hw;      *h = hh; break;
    case SZ_TR: *x = hw; *y = 0;  *w = aw - hw; *h = hh; break;
    case SZ_BL: *x = 0;  *y = hh; *w = hw;      *h = ah - hh; break;
    case SZ_BR: *x = hw; *y = hh; *w = aw - hw; *h = ah - hh; break;
    default:    *x = 0;  *y = 0;  *w = aw;      *h = ah; break;
    }
    *x += mx0;
}
static int snap_zone_at(int x, int y) {
    int corner = 70, mx0, mw, mh, wh; mon_of(x, &mx0, &mw, &mh, &wh);
    snap_mon_x = x;
    if (x <= mx0 + 3) return y < corner ? SZ_TL : y > wh - corner ? SZ_BL : SZ_L;
    if (x >= mx0 + mw - 4) return y < corner ? SZ_TR : y > wh - corner ? SZ_BR : SZ_R;
    if (y <= 2) return SZ_MAX;
    return SZ_NONE;
}
static void snap_restore(int idx);
static void snap_repaint_zone(int z) { if (z) { int x, y, w, h; snap_rect(z, &x, &y, &w, &h); repaint(x, y, w, h); } }

/* change a window's place and size */
static void win_set_geom(int idx, int nx, int ny, int nw, int nh) {
    Win *w = &wins[idx];
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    w->x = nx; w->y = ny; w->w = nw; w->h = nh;
    repaint(ox - 2, oy - 1, ow + 7, oh + 7);
    repaint_window(idx);
    if (w->resized) w->resized();
}
/* put window idx into zone z (remembering its size to go back to) */
static void snap_to(int idx, int z) {
    Win *w = &wins[idx];
    if (!w->open || w->fullscreen) return;
    if (z == SZ_NONE) { snap_restore(idx); return; }
    if (z == SZ_MAX) { if (!w->maximized) { if (w->snap) { w->snap = 0; } toggle_max(idx); } return; }
    if (!w->maximized && !w->snap) { w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
    w->maximized = 0; w->snap = z;
    int x, y, ww, hh; snap_rect(z, &x, &y, &ww, &hh);
    win_set_geom(idx, x, y, ww, hh);
}
/* back to its size from before (snapped or maximized) */
static void snap_restore(int idx) {
    Win *w = &wins[idx];
    if (w->maximized) { toggle_max(idx); return; }
    if (!w->snap) return;
    w->snap = 0;
    win_set_geom(idx, w->rx, w->ry, w->rw, w->rh);
}

/* the title bar was pressed: start dragging */
static void snap_drag_start(int idx) {
    Win *w = &wins[idx];
    drag_win = idx; drag_dx = mx - w->x; drag_dy = my - w->y;
    drag_sx = mx; drag_sy = my;
    drag_armed = w->maximized || w->snap;
    snap_zone = SZ_NONE;
}
/* the mouse moved while dragging */
static void snap_drag_motion(void) {
    if (drag_win < 0) return;
    Win *w = &wins[drag_win];
    if (drag_armed) {
        if (abs(mx - drag_sx) + abs(my - drag_sy) < 10) return;   /* a little nudge: stay put */
        drag_armed = 0;
        /* pulled off the side: its old size, with the mouse still on the title at the same spot */
        int rw = w->rw > 0 ? w->rw : w->w * 2 / 3, rh = w->rh > 0 ? w->rh : w->h * 2 / 3;
        float fx = (float)(drag_sx - w->x) / imax(1, w->w);
        int nx = mx - (int)(fx * rw), ny = my - imin(drag_dy, TITLE_H - 4);
        w->maximized = 0; w->snap = 0;
        win_set_geom(drag_win, nx, imax(0, ny), rw, rh);
        drag_dx = mx - w->x; drag_dy = my - w->y;
    }
    move_win(drag_win, mx - drag_dx, my - drag_dy);
    int z = wins[drag_win].no_max ? SZ_NONE : snap_zone_at(mx, my);
    if (z != snap_zone) { int old = snap_zone; snap_zone = z; snap_repaint_zone(old); snap_repaint_zone(z); }
}
/* the mouse button went up */
static void snap_drag_end(void) {
    int z = snap_zone, i = drag_win;
    snap_zone = SZ_NONE; drag_armed = 0;
    if (z) snap_repaint_zone(z);
    if (z && i >= 0 && wins[i].open) snap_to(i, z);
}
/* in repaint(): the see-through box, under the window being dragged */
static void snap_draw(void) {
    if (!snap_zone) return;
    int x, y, w, h; snap_rect(snap_zone, &x, &y, &w, &h);
    x += 6; y += 6; w -= 12; h -= 12;
    uint32_t c = set_style ? TH()->accent : RGB(0x3c, 0x8c, 0xe8);
    fill_round(x, y, w, h, 10, RGB(255, 255, 255), 70);
    fill_round(x, y, w, h, 10, c, 60);
    stroke_round(x, y, w, h, 10, c, 220);
    stroke_round(x + 1, y + 1, w - 2, h - 2, 9, RGB(255, 255, 255), 120);
}
/* Windows + Left/Right/Up/Down */
static void snap_key(int code) {
    int i = active_win();
    if (i < 0 || !wins[i].open || WHIDDEN(&wins[i]) || wins[i].no_max) {
        if (i >= 0 && code == KEY_DOWN) minimize_win(i);
        return;
    }
    Win *w = &wins[i];
    snap_mon_x = w->x + w->w / 2;
    int s = w->maximized ? SZ_MAX : w->snap;
    if (code == KEY_LEFT)  snap_to(i, s == SZ_R ? SZ_NONE : s == SZ_TR ? SZ_TL : s == SZ_BR ? SZ_BL : SZ_L);
    else if (code == KEY_RIGHT) snap_to(i, s == SZ_L ? SZ_NONE : s == SZ_TL ? SZ_TR : s == SZ_BL ? SZ_BR : SZ_R);
    else if (code == KEY_UP) snap_to(i, s == SZ_L ? SZ_TL : s == SZ_R ? SZ_TR : s == SZ_BL ? SZ_L : s == SZ_BR ? SZ_R : SZ_MAX);
    else if (code == KEY_DOWN) {
        if (s == SZ_L) snap_to(i, SZ_BL); else if (s == SZ_R) snap_to(i, SZ_BR);
        else if (s == SZ_TL) snap_to(i, SZ_L); else if (s == SZ_TR) snap_to(i, SZ_R);
        else if (s) snap_restore(i); else minimize_win(i);
    }
}
