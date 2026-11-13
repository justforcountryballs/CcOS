/* ===================================================================
 *  Alt + Tab: hold Alt, press Tab to step through the open windows
 *  (shown as small pictures), let go of Alt to switch. Shift goes
 *  back, Esc cancels.
 *  Hover previews: rest the mouse on a window's button in the Dock or
 *  taskbar and a little picture of the window pops up above it; click
 *  the picture to go to that window.
 * =================================================================== */
typedef struct { uint32_t *px; int w, h; } Thumb;
/* a small picture of window idx, at most mw x mh */
static Thumb win_thumb(int idx, int mw, int mh) {
    Thumb t = { NULL, 0, 0 };
    int x, y, w, h; uint32_t *big = anim_snap(idx, &x, &y, &w, &h);
    if (!big) return t;
    float s = fminf((float)mw / w, (float)mh / h); if (s > 1) s = 1;
    t.w = imax(1, (int)(w * s)); t.h = imax(1, (int)(h * s));
    t.px = malloc((size_t)t.w * t.h * 4);
    if (t.px)
        for (int j = 0; j < t.h; j++)
            for (int i = 0; i < t.w; i++) {                      /* average of the pixels it covers */
                int x0 = i * w / t.w, x1 = imax(x0 + 1, (i + 1) * w / t.w), y0 = j * h / t.h, y1 = imax(y0 + 1, (j + 1) * h / t.h);
                int sx = imax(1, (x1 - x0) / 3), sy = imax(1, (y1 - y0) / 3);
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int yy = y0; yy < y1; yy += sy) for (int xx = x0; xx < x1; xx += sx) { uint32_t c = big[(size_t)yy * w + xx]; r += R(c); g += G(c); b += B(c); n++; }
                t.px[j * t.w + i] = RGB(r / n, g / n, b / n);
            }
    free(big);
    if (!t.px) t.w = t.h = 0;
    return t;
}
static void thumb_draw(const Thumb *t, int x, int y) {
    if (!t->px) return;
    fill_rect(x + 2, y + 3, t->w, t->h, RGB(0, 0, 0), 50);
    for (int j = imax(y, clip_y0); j < imin(y + t->h, clip_y1); j++)
        for (int i = imax(x, clip_x0); i < imin(x + t->w, clip_x1); i++) back[(size_t)j * W + i] = t->px[(j - y) * t->w + (i - x)];
}

/* ---- Alt + Tab ---- */
#define AT_TW 220
#define AT_TH 140
#define AT_CW (AT_TW + 24)
#define AT_CH (AT_TH + 48)
static struct { int open, n, sel, win[MAXWIN]; Thumb th[MAXWIN]; } at;
static int alt_held, shift_held;
static void at_rect(int *x, int *y, int *w, int *h) {
    int cols = imin(at.n, imax(1, (W - 80) / AT_CW)), rows = (at.n + cols - 1) / cols;
    *w = cols * AT_CW + 24; *h = rows * AT_CH + 24; *x = (W - *w) / 2; *y = (H - TASKBAR_H - *h) / 2;
}
static void at_repaint(void) { int x, y, w, h; at_rect(&x, &y, &w, &h); repaint(x - 4, y - 4, w + 12, h + 12); }
static void at_close(void) {
    if (!at.open) return;
    at.open = 0;
    at_repaint();
    for (int i = 0; i < at.n; i++) { free(at.th[i].px); at.th[i].px = NULL; }
}
/* Tab pressed with Alt held */
static void at_tab(int back_) {
    if (!at.open) {
        at.n = 0;
        for (int k = nz - 1; k >= 0; k--) if (wins[zorder[k]].open) at.win[at.n++] = zorder[k];   /* the newest first */
        for (int i = 0; i < MAXWIN && at.n < MAXWIN; i++) {                   /* (minimized ones too) */
            int in = 0; for (int k = 0; k < at.n; k++) if (at.win[k] == i) in = 1;
            if (wins[i].open && !in) at.win[at.n++] = i;
        }
        if (at.n < 2) { at.n = 0; return; }
        set_menu(0); pop_close();
        for (int i = 0; i < at.n; i++) at.th[i] = win_thumb(at.win[i], AT_TW, AT_TH);
        at.open = 1; at.sel = 0;
    }
    at.sel = (at.sel + (back_ ? at.n - 1 : 1)) % at.n;
    at_repaint();
}
static void at_finish(int go) {
    if (!at.open) return;
    int w = at.win[at.sel];
    at_close();
    if (go && wins[w].open) restore_win(w);
}
static void draw_alttab(void) {
    if (!at.open) return;
    int x, y, w, h; at_rect(&x, &y, &w, &h);
    if (x >= clip_x1 || x + w <= clip_x0 || y >= clip_y1 || y + h <= clip_y0) return;
    const int dark = !set_style || set_dark;
    fill_round(x + 2, y + 5, w, h, 14, RGB(0, 0, 0), 70);
    fill_round(x, y, w, h, 14, dark ? RGB(0x20, 0x22, 0x28) : RGB(0xf4, 0xf6, 0xf9), 235);
    stroke_round(x, y, w, h, 14, dark ? RGB(0x50, 0x54, 0x5c) : RGB(0xc8, 0xcc, 0xd4), 255);
    int cols = imin(at.n, imax(1, (W - 80) / AT_CW));
    uint32_t text = dark ? RGB(0xf0, 0xf2, 0xf5) : RGB(0x1e, 0x22, 0x28);
    for (int i = 0; i < at.n; i++) {
        int cx = x + 12 + (i % cols) * AT_CW, cy = y + 12 + (i / cols) * AT_CH;
        if (i == at.sel) {
            fill_round(cx, cy, AT_CW, AT_CH, 10, set_style ? TH()->accent : RGB(0x3c, 0x8c, 0xe8), 90);
            stroke_round(cx, cy, AT_CW, AT_CH, 10, set_style ? TH()->accent : RGB(0x6c, 0xb0, 0xff), 255);
        }
        Thumb *t = &at.th[i];
        if (t->px) thumb_draw(t, cx + (AT_CW - t->w) / 2, cy + 10 + (AT_TH - t->h) / 2);
        else draw_icon(wins[at.win[i]].icon, cx + AT_CW / 2, cy + 10 + AT_TH / 2, 24);
        draw_icon(wins[at.win[i]].icon, cx + 20, cy + AT_CH - 19, 8);
        char tt[64]; snprintf(tt, sizeof tt, "%s", wins[at.win[i]].title ? wins[at.win[i]].title : "");
        while (tt[0] && text_w(tt) > AT_CW - 44) tt[u8_prev(tt, (int)strlen(tt))] = 0;
        draw_text(cx + 34, cy + AT_CH - 27, tt, text, 255);
    }
}

/* ---- hover previews over the Dock / taskbar ---- */
#define HP_TW 240
#define HP_TH 150
static struct { int win, shown, x, y, w, h; long long since, shot; Thumb th; } hp = { .win = -1 };
static void hp_rect_calc(void) {
    int cx = mx, top = H - TASKBAR_H;
    if (set_dock) {
        DockSlot s[DOCK_MAXS]; int sx, sw, n = dock_layout(s, &sx, &sw);
        TbItem it[MAXPINS + MAXWIN]; tb_items(it);
        for (int i = 0; i < n; i++)
            if ((s[i].kind == DS_TASK && it[s[i].k].win == hp.win) || (s[i].kind == DS_FINDER && !strcmp(wins[hp.win].cmd, "@finder")))
                { cx = (int)s[i].cx; top = H - DOCK_BOT - DOCK_PAD - (int)s[i].size; }
    } else {
        TbItem it[MAXPINS + MAXWIN]; int n = tb_items(it);
        for (int k = 0; k < n; k++) if (it[k].win == hp.win) { int x, y, w, h; task_rect(k, &x, &y, &w, &h); cx = x + w / 2; top = y; }
    }
    hp.w = (hp.th.px ? hp.th.w : HP_TW) + 20; hp.h = (hp.th.px ? hp.th.h : 40) + 44;
    hp.x = clampi(cx - hp.w / 2, 6, W - hp.w - 6); hp.y = top - hp.h - 10;
}
static void hp_repaint(void) { if (hp.shown) repaint(hp.x - 4, hp.y - 4, hp.w + 12, hp.h + 14); }
static void hp_hide(void) {
    int was = hp.shown;
    hp.shown = 0;
    if (was) repaint(hp.x - 4, hp.y - 4, hp.w + 12, hp.h + 14);
    hp.win = -1; free(hp.th.px); hp.th.px = NULL;
}
static int hp_over_card(void) { return hp.shown && mx >= hp.x && mx < hp.x + hp.w && my >= hp.y && my < hp.y + hp.h + 12; }
/* which window's button is under the mouse (-1 none) */
static int hp_hot_win(void) {
    if (hot.type == HOT_TASK) {
        TbItem it[MAXPINS + MAXWIN]; int n = tb_items(it);
        if (hot.a >= 0 && hot.a < n) return it[hot.a].win;
    }
    if (hot.type == HOT_ORB && set_dock) return win_of(finder_draw);
    return -1;
}
/* main loop: show / move / hide; how long until the next look (ms), or -1 */
static int hp_tick(void) {
    if (menu_open || pop.open || at.open || mouse_down) { if (hp.shown || hp.win >= 0) hp_hide(); return -1; }
    int w = hp_hot_win();
    if (w < 0 && hp_over_card()) w = hp.win;                  /* on the picture itself: keep it */
    if (w < 0) { if (hp.shown || hp.win >= 0) hp_hide(); return -1; }
    long long now = ui_ms();
    if (w != hp.win) {
        int was = hp.shown; hp_hide(); hp.win = w; hp.since = now;
        if (was) hp.since -= 1000;                            /* moving along the bar: show straight away */
    }
    if (!hp.shown && now - hp.since >= 450) {
        hp.th = win_thumb(w, HP_TW, HP_TH); hp.shot = now; hp.shown = 1; hp_rect_calc(); hp_repaint();
    } else if (hp.shown && now - hp.shot > 1000 && !wins[w].minimized) {   /* keep it up to date */
        hp_repaint(); free(hp.th.px); hp.th = win_thumb(w, HP_TW, HP_TH); hp.shot = now; hp_rect_calc(); hp_repaint();
    }
    return hp.shown ? 250 : 120;
}
static void draw_hover_preview(void) {
    if (!hp.shown || hp.win < 0 || !wins[hp.win].open) return;
    if (hp.x >= clip_x1 || hp.x + hp.w + 4 <= clip_x0 || hp.y >= clip_y1 || hp.y + hp.h + 4 <= clip_y0) return;
    const int dark = !set_style || set_dark;
    int on = hp_over_card();
    fill_round(hp.x + 2, hp.y + 4, hp.w, hp.h, 10, RGB(0, 0, 0), 60);
    fill_round(hp.x, hp.y, hp.w, hp.h, 10, dark ? RGB(0x22, 0x24, 0x2a) : RGB(0xf8, 0xf9, 0xfb), 240);
    stroke_round(hp.x, hp.y, hp.w, hp.h, 10, on ? (set_style ? TH()->accent : RGB(0x6c, 0xb0, 0xff)) : (dark ? RGB(0x50, 0x54, 0x5c) : RGB(0xc8, 0xcc, 0xd4)), 255);
    uint32_t text = dark ? RGB(0xf0, 0xf2, 0xf5) : RGB(0x1e, 0x22, 0x28);
    draw_icon(wins[hp.win].icon, hp.x + 20, hp.y + 18, 8);
    char tt[64]; snprintf(tt, sizeof tt, "%s", wins[hp.win].title ? wins[hp.win].title : "");
    while (tt[0] && text_w(tt) > hp.w - 44) tt[u8_prev(tt, (int)strlen(tt))] = 0;
    draw_text(hp.x + 34, hp.y + 10, tt, text, 255);
    if (hp.th.px) thumb_draw(&hp.th, hp.x + 10, hp.y + 34);
    else draw_text(hp.x + 34, hp.y + 34, "(minimized)", dark ? RGB(0xa0, 0xa4, 0xac) : RGB(0x70, 0x78, 0x84), 255);
}
/* a click: on the picture? then go to that window */
static int hp_click(void) {
    if (!hp_over_card() || hp.win < 0) return 0;
    int w = hp.win; hp_hide();
    if (wins[w].open) restore_win(w);
    return 1;
}
static int hp_shown_(void) { return hp.shown; }
