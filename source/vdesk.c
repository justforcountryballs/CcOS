/* ===================================================================
 *  Virtual desktops and Task View.
 *    Windows + Ctrl + Left / Right     the desktop before / after
 *    Windows + Ctrl + D                a new desktop
 *    Windows + Ctrl + F4               close this desktop (its windows move over)
 *    Windows + Ctrl + Shift + arrows   take the active window to the next desktop
 *    Windows + Tab                     Task View: every window of this desktop,
 *                                      and the desktops along the top. Drag a
 *                                      window onto a desktop to move it there.
 * =================================================================== */
#define VD_MAX 6
#define TV_DW 192                      /* a desktop card */
#define TV_DH 108
#define TV_TOP 26
static struct {
    int open, n, win[MAXWIN];
    Thumb th[MAXWIN];
    int tx[MAXWIN], ty[MAXWIN];        /* where each picture is */
    int press_win, dragging, px, py;   /* a window picture being dragged */
} tv;
static int tv_open_(void) { return tv.open; }

static void vd_apply(void) {
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open) wins[i].offdesk = wins[i].desk != cur_desk;
}
static void vd_switch(int n) {
    if (n < 0 || n >= nvdesk) return;
    if (n != cur_desk) {
        cur_desk = n;
        vd_apply();
        repaint(0, 0, BW, H);
        repaint_taskbar();
    }
    osd_kind = OSD_DESK; osd_until = ui_ms() + 1200; repaint_osd();
}
static void vd_new(void) {
    if (nvdesk >= VD_MAX) { toast(ICON_APP, "Desktops", "That's the most desktops ccos can have (6)."); return; }
    nvdesk++;
    vd_switch(nvdesk - 1);
}
static void vd_close(int n) {                    /* its windows go to the desktop before it */
    if (nvdesk <= 1 || n < 0 || n >= nvdesk) return;
    int to = n > 0 ? n - 1 : 0;
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open) {
        if (wins[i].desk == n) wins[i].desk = to;
        else if (wins[i].desk > n) wins[i].desk--;
    }
    nvdesk--;
    int c = cur_desk == n ? to : cur_desk > n ? cur_desk - 1 : cur_desk;
    cur_desk = -1;                               /* (forces a full redraw) */
    vd_switch(c);
}
static void vd_move_win(int idx, int n, int follow) {
    if (idx < 0 || !wins[idx].open || n < 0 || n >= nvdesk) return;
    wins[idx].desk = n;
    if (follow) vd_switch(n);
    else { vd_apply(); repaint(0, 0, BW, H); repaint_taskbar(); }
}
/* Windows + Ctrl + ... */
static void vd_key(int code, int shift) {
    if (code == KEY_LEFT || code == KEY_RIGHT) {
        int d = code == KEY_LEFT ? -1 : 1, to = cur_desk + d;
        if (shift) {                                  /* take the active window along */
            int a = active_win();
            if (to >= nvdesk && nvdesk < VD_MAX && a >= 0) nvdesk++;
            if (to >= 0 && to < nvdesk && a >= 0) { vd_move_win(a, to, 1); restore_win(a); }
            return;
        }
        if (to >= 0 && to < nvdesk) vd_switch(to);
        else vd_switch(cur_desk);                     /* (just show where you are) */
    } else if (code == KEY_D) vd_new();
    else if (code == KEY_F4) vd_close(cur_desk);
}

/* ---- Task View ---- */
static void tv_card_rect(int k, int *x, int *y, int *w, int *h) {   /* k == nvdesk: "New desktop" */
    int n = nvdesk + (nvdesk < VD_MAX), total = n * (TV_DW + 18) - 18;
    *x = (W - total) / 2 + k * (TV_DW + 18); *y = TV_TOP; *w = TV_DW; *h = TV_DH;
}
static void tv_layout(void) {                    /* the window pictures: a grid in the middle */
    int top = TV_TOP + TV_DH + 60, bottom = H - (set_dock ? DOCK_TOP : TASKBAR_H) - 30;
    int n = tv.n; if (!n) return;
    int best_c = 1; float best = 0;
    for (int c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        float cw = (W - 80.0f) / c - 30, ch = (float)(bottom - top) / r - 50;
        float s = fminf(cw / 400, ch / 260);
        if (s > best) { best = s; best_c = c; }
    }
    int cols = best_c, rows = (n + cols - 1) / cols;
    int cellw = (W - 80) / cols, cellh = (bottom - top) / rows;
    for (int k = 0; k < n; k++) {
        free(tv.th[k].px);
        tv.th[k] = win_thumb(tv.win[k], imin(cellw - 40, 520), imin(cellh - 56, 360));
        int r = k / cols, c = k % cols, inrow = r == rows - 1 ? n - r * cols : cols;
        int rowx = (W - inrow * cellw) / 2;
        tv.tx[k] = rowx + c * cellw + (cellw - tv.th[k].w) / 2;
        tv.ty[k] = top + r * cellh + (cellh - 30 - tv.th[k].h) / 2 + 30;
    }
}
static void tv_set(int open) {
    if (open == tv.open) return;
    if (open) {
        set_menu(0); pop_close(); hp_hide();
        tv.n = 0;
        for (int k = nz - 1; k >= 0; k--) { int i = zorder[k]; if (wins[i].open && wins[i].desk == cur_desk && !wins[i].minimized) tv.win[tv.n++] = i; }
        for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].desk == cur_desk && wins[i].minimized) tv.win[tv.n++] = i;
        tv.press_win = -1; tv.dragging = 0;
        tv_layout();                             /* (pictures first: the windows are hidden while it's open) */
        tv.open = 1;
    } else {
        tv.open = 0;
        for (int k = 0; k < tv.n; k++) { free(tv.th[k].px); tv.th[k].px = NULL; }
    }
    repaint(0, 0, BW, H);
}
/* a small picture of desktop k: the wallpaper and its windows as boxes */
static void tv_draw_desk(int k, int x, int y, int w, int h) {
    for (int j = imax(y, clip_y0); j < imin(y + h, clip_y1); j++)
        for (int i = imax(x, clip_x0); i < imin(x + w, clip_x1); i++) {
            int sx = (i - x) * W / w, sy = (j - y) * H / h;
            back[(size_t)j * BW + i] = wall[(size_t)sy * BW + sx];
        }
    float s = (float)w / W;
    for (int z = 0; z < nz; z++) {
        int i = zorder[z];
        if (!wins[i].open || wins[i].desk != k || wins[i].minimized) continue;
        int wx = x + (int)(wins[i].x * s), wy = y + (int)(wins[i].y * s), ww = imax(8, (int)(wins[i].w * s)), wh = imax(6, (int)(wins[i].h * s));
        fill_round(wx, wy, ww, wh, 3, RGB(0xf4, 0xf6, 0xf9), 235);
        fill_rect(wx, wy, ww, imax(3, (int)(TITLE_H * s)), TH()->accent, 200);
        if (ww > 24 && wh > 20) draw_icon(wins[i].icon ? wins[i].icon : ICON_APP, wx + ww / 2, wy + wh / 2 + 2, imin(10, wh / 3));
    }
}
static int tv_hit(void);
static void draw_taskview(void) {
    if (!tv.open) return;
    fill_rect(0, 0, BW, H, RGB(0x0c, 0x12, 0x1c), 150);
    int hv = hot.type == HOT_TV ? hot.a : -1;
    /* desktops */
    for (int k = 0; k <= nvdesk && k < VD_MAX + 1; k++) {
        if (k == nvdesk && nvdesk >= VD_MAX) break;
        int x, y, w, h; tv_card_rect(k, &x, &y, &w, &h);
        int drop = tv.dragging && in_rect(x, y, w, h);
        if (k < nvdesk) {
            fill_round(x - 4, y - 4, w + 8, h + 8, 10, k == cur_desk || drop ? TH()->accent : RGB(255, 255, 255), k == cur_desk || drop ? 255 : hv == 1000 + k ? 90 : 40);
            tv_draw_desk(k, x, y, w, h);
            char b[24]; snprintf(b, sizeof b, "Desktop %d", k + 1);
            draw_text_bold(x + (w - text_w(b)) / 2, y + h + 10, b, RGB(255, 255, 255));
            if ((hv == 1000 + k || hv == 1100 + k) && nvdesk > 1) {             /* x: close it */
                int bx = x + w - 26, by = y + 4, on = hv == 1100 + k;
                fill_round(bx, by, 22, 22, 6, on ? RGB(0xc0, 0x30, 0x30) : RGB(0x20, 0x24, 0x2c), on ? 255 : 200);
                aa_line(bx + 7, by + 7, bx + 15, by + 15, 1.5f, RGB(255, 255, 255)); aa_line(bx + 15, by + 7, bx + 7, by + 15, 1.5f, RGB(255, 255, 255));
            }
        } else {
            fill_round(x, y, w, h, 8, RGB(255, 255, 255), hv == 1000 + k || drop ? 70 : 35);
            stroke_round(x, y, w, h, 8, RGB(255, 255, 255), 120);
            fill_rect(x + w / 2 - 12, y + h / 2 - 2, 24, 4, RGB(255, 255, 255), 255);
            fill_rect(x + w / 2 - 2, y + h / 2 - 12, 4, 24, RGB(255, 255, 255), 255);
            draw_text_bold(x + (w - text_w("New desktop")) / 2, y + h + 10, "New desktop", RGB(255, 255, 255));
        }
    }
    /* windows */
    if (!tv.n) {
        const char *m = "No windows on this desktop";
        draw_text_bold((W - text_w(m)) / 2, H / 2, m, RGB(255, 255, 255));
    }
    for (int k = 0; k < tv.n; k++) {
        int i = tv.win[k];
        if (tv.dragging && tv.press_win == k) continue;
        int x = tv.tx[k], y = tv.ty[k], w = tv.th[k].w, h = tv.th[k].h;
        int on = hv == k || hv == 2000 + k;
        if (on) fill_round(x - 8, y - 38, w + 16, h + 46, 10, RGB(255, 255, 255), 50);
        draw_icon(wins[i].icon ? wins[i].icon : ICON_APP, x + 10, y - 18, 9);
        char t[80]; snprintf(t, sizeof t, "%s", wins[i].title ? wins[i].title : "");
        while (t[0] && text_w(t) > w - 60) t[u8_prev(t, (int)strlen(t))] = 0;
        draw_text(x + 26, y - 26, t, RGB(255, 255, 255), 255);
        if (tv.th[k].px) thumb_draw(&tv.th[k], x, y);
        else fill_round(x, y, imax(w, 200), imax(h, 120), 6, RGB(0x30, 0x34, 0x3c), 255);
        if (wins[i].minimized) draw_text(x + 8, y + h - 24, "(minimized)", RGB(255, 255, 255), 200);
        if (on) {                                       /* x: close the window */
            int bx = x + w - 26, by = y - 32, c = hv == 2000 + k;
            fill_round(bx, by, 24, 24, 6, c ? RGB(0xc0, 0x30, 0x30) : RGB(255, 255, 255), c ? 255 : 40);
            aa_line(bx + 8, by + 8, bx + 16, by + 16, 1.5f, RGB(255, 255, 255)); aa_line(bx + 16, by + 8, bx + 8, by + 16, 1.5f, RGB(255, 255, 255));
        }
    }
    if (tv.dragging && tv.press_win >= 0 && tv.press_win < tv.n) {   /* the one being dragged, small, at the mouse */
        Thumb *t = &tv.th[tv.press_win];
        int w = imin(t->w, 220), h = t->w ? t->h * w / t->w : 0;
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int X = mx - w / 2 + i, Y = my - h / 2 + j;
            if (X < clip_x0 || X >= clip_x1 || Y < clip_y0 || Y >= clip_y1) continue;
            back[(size_t)Y * BW + X] = t->px[(size_t)(j * t->h / h) * t->w + i * t->w / w];
        }
    }
    const char *hint = "Drag a window onto a desktop to move it there.   Esc closes.";
    draw_text((W - text_w(hint)) / 2, H - (set_dock ? DOCK_TOP : TASKBAR_H) - 26, hint, RGB(0xd8, 0xde, 0xe6), 255);
}
/* 0..n-1 a window, 2000+k its close button, 1000+k a desktop (k == nvdesk: New), 1100+k a desktop's x; -2 nothing */
static int tv_hit(void) {
    if (!tv.open) return -1;
    for (int k = 0; k <= nvdesk; k++) {
        if (k == nvdesk && nvdesk >= VD_MAX) break;
        int x, y, w, h; tv_card_rect(k, &x, &y, &w, &h);
        if (k < nvdesk && nvdesk > 1 && in_rect(x + w - 26, y + 4, 22, 22)) return 1100 + k;
        if (in_rect(x - 4, y - 4, w + 8, h + 34)) return 1000 + k;
    }
    for (int k = 0; k < tv.n; k++) {
        int x = tv.tx[k], y = tv.ty[k], w = tv.th[k].w, h = tv.th[k].h;
        if (in_rect(x + w - 26, y - 32, 24, 24)) return 2000 + k;
        if (in_rect(x - 8, y - 38, w + 16, h + 46)) return k;
    }
    return -2;
}
static void tv_press(void) {
    int id = tv_hit();
    tv.press_win = id >= 0 && id < tv.n ? id : -1;
    tv.px = mx; tv.py = my; tv.dragging = 0;
    if (id >= 1100 && id < 1100 + VD_MAX) { vd_close(id - 1100); osd_kind = OSD_NONE; tv_set(0); tv_set(1); return; }
    if (id >= 1000 && id <= 1000 + VD_MAX) {
        int k = id - 1000;
        if (k == nvdesk) { if (nvdesk < VD_MAX) { nvdesk++; repaint(0, 0, BW, H); } return; }
        tv_set(0); vd_switch(k); return;
    }
    if (id >= 2000) {
        int i = tv.win[id - 2000];
        tv_set(0);
        if (!wins[i].on_close || wins[i].on_close()) close_win(i);
        tv_set(1); return;
    }
    if (id == -2) tv_set(0);
}
static void tv_motion(void) {
    if (tv.press_win >= 0 && mouse_down && !tv.dragging && (abs(mx - tv.px) > 8 || abs(my - tv.py) > 8)) tv.dragging = 1;
    if (tv.dragging) repaint(0, 0, BW, H);
}
static void tv_release(void) {
    int k = tv.press_win; tv.press_win = -1;
    if (k < 0 || k >= tv.n) return;
    int i = tv.win[k];
    if (!tv.dragging) { tv_set(0); restore_win(i); return; }   /* a click: go to that window */
    tv.dragging = 0;
    for (int d = 0; d <= nvdesk; d++) {
        if (d == nvdesk && nvdesk >= VD_MAX) break;
        int x, y, w, h; tv_card_rect(d, &x, &y, &w, &h);
        if (!in_rect(x - 4, y - 4, w + 8, h + 34)) continue;
        if (d == nvdesk) nvdesk++;                    /* dropped on "New desktop" */
        if (d != cur_desk) { vd_move_win(i, d, 0); tv_set(0); tv_set(1); return; }
    }
    repaint(0, 0, BW, H);
}
static void tv_key(int code) {
    if (code == KEY_ESC) { tv_set(0); return; }
    if ((code == KEY_ENTER || code == KEY_KPENTER) && tv.n) { int i = tv.win[0]; tv_set(0); restore_win(i); }
}
