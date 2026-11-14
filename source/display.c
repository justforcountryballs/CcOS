/* ===================================================================
 *  Windows + P: how to use a second screen, and what happens when one is
 *  plugged in or out (see kms.c for the screens themselves).
 * =================================================================== */
static void snip_end(int save);
#define DP_W 330
#define DP_RH 64
static struct { int open, sel; } dp;
static int dp_open_(void) { return dp.open; }
static int dp_x(void) { return W - DP_W - 12; }
static int dp_y(void) { return imax(8, (H - TASKBAR_H - (DM_N * DP_RH + 64)) / 2); }
static void dp_repaint(void) { repaint(dp_x() - 6, dp_y() - 6, DP_W + 16, DM_N * DP_RH + 80); }

/* the canvas changes size: buffers, wallpaper, windows, apps */
static void display_apply(int mode) {
    if (!kms_on) return;
    if (rec_on()) rec_toggle();                       /* (a recording keeps one size) */
    if (snip_on) snip_end(0);
    at_close(); if (tv_open_()) tv_set(0);
    int ow = W, obw = BW, oh = H, nw, nbw, nh;
    if (!kms_layout(mode, &nw, &nbw, &nh)) return;
    if (nw != ow || nbw != obw || nh != oh) {
        uint32_t *nb = malloc((size_t)nbw * nh * 4), *nwall = malloc((size_t)nbw * nh * 4);
        if (!nb || !nwall) { free(nb); free(nwall); kms_layout(DM_MAIN, &nw, &nbw, &nh); return; }
        free(back); free(wall); back = nb; wall = nwall;
        free(dk_mask); dk_mask = NULL;
        W = nw; BW = nbw; H = nh;
        for (int i = 0; i < MAXWIN; i++) {
            Win *w = &wins[i];
            if (!w->open) continue;
            if (w->fullscreen) { set_fullscreen(i, 0); set_fullscreen(i, 1); continue; }
            if (w->rx >= BW - 80) w->rx = 40;
            if (w->ry + TITLE_H > H) w->ry = 20;
            if (w->maximized) {
                int c = w->x + w->w / 2; if (c >= BW) c = 0;
                int m0, mw, mh, wh; mon_of(c, &m0, &mw, &mh, &wh);
                w->x = m0; w->y = 0; w->w = mw; w->h = wh;
                if (w->resized) w->resized();
                continue;
            }
            if (w->x >= BW - 80) w->x = imax(0, imin(W - w->w, W / 2 - w->w / 2));   /* was on the screen that went away */
            if (w->x + w->w / 2 < W && w->y + w->h > H - TASKBAR_H) w->y = imax(0, H - TASKBAR_H - w->h);
            if (w->x + w->w / 2 >= W && BW > W && w->y + TITLE_H > SH2) w->y = 0;
            if (w->resized) w->resized();
        }
        mx = clampi(mx, 0, BW - 1); my = clampi(my, 0, mon_h_at(mx) - 1);
        make_wallpaper();
        wl_outputs_changed();
    }
    repaint_all();
    repaint_taskbar();
}
static void display_set(int mode) {
    set_display = mode;
    save_settings();
    display_apply(kms_second >= 0 ? mode : DM_MAIN);
}
static void display_startup(void) {
    if (kms_on && kms_second >= 0 && set_display != DM_MAIN) display_apply(set_display);
}
/* once a second: a screen plugged in or out? (the kernel says so on a netlink socket) */
static void display_tick(void) {
    if (!kms_on || kms_ev < 0) return;
    char buf[4096]; int hot = 0; ssize_t n;
    while ((n = recv(kms_ev, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
        buf[n] = 0;
        int drm = 0, hp = 0;
        for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
            if (!strcmp(p, "SUBSYSTEM=drm")) drm = 1;
            if (!strcmp(p, "HOTPLUG=1")) hp = 1;
        }
        if (drm && hp) hot = 1;
    }
    if (!hot) return;
    int had = kms_second >= 0; char was[24] = ""; if (had) snprintf(was, sizeof was, "%s", kout[kms_second].name);
    kms_probe();
    int now = kms_second >= 0;
    if (now && (!had || strcmp(was, kout[kms_second].name))) {
        display_apply(set_display);
        char b[120]; snprintf(b, sizeof b, "Showing: %s. Press Windows + P to change it.", dm_names[kms_cur]);
        toast(ICON_COMPUTER, "Second screen connected", b);
    } else if (!now && had) {
        display_apply(DM_MAIN);
        toast(ICON_COMPUTER, "Second screen unplugged", "Everything is back on this screen.");
    }
}

/* ---- the Windows + P panel ---- */
static void dp_set(int open) {
    if (open == dp.open) return;
    if (open) { set_menu(0); pop_close(); if (kms_on && kms_ev >= 0) { kms_probe(); } dp.sel = kms_cur; }
    dp.open = open;
    dp_repaint();
}
static void dp_draw_icon(int k, int x, int y, uint32_t c, uint32_t fillc) {   /* two little screens */
    int a = k == DM_SECOND ? 0 : 1, b = k == DM_MAIN ? 0 : 1;
    stroke_round(x, y + 4, 26, 18, 2, c, a ? 255 : 90);
    if (a) fill_rect(x + 2, y + 6, 22, 14, fillc, 255);
    stroke_round(x + 30, y + 4, 26, 18, 2, c, b ? 255 : 90);
    if (b) fill_rect(x + 32, y + 6, 22, 14, fillc, k == DM_DUP ? 255 : k == DM_EXTEND ? 160 : 255);
    if (k == DM_DUP) { draw_text(x + 9, y + 5, "1", c, 255); draw_text(x + 39, y + 5, "1", c, 255); }
    if (k == DM_EXTEND) { draw_text(x + 9, y + 5, "1", c, 255); draw_text(x + 39, y + 5, "2", c, 255); }
}
static void draw_display_panel(void) {
    if (!dp.open) return;
    int x = dp_x(), y = dp_y(), w = DP_W, h = DM_N * DP_RH + 64;
    if (x >= clip_x1 || x + w + 4 <= clip_x0 || y >= clip_y1 || y + h + 4 <= clip_y0) return;
    const Theme *t = TH();
    fill_round(x + 2, y + 5, w, h, 12, RGB(0, 0, 0), 70);
    glass_fill(x, y, w, h, 12, t->menu, 0, 0, 0, 0);
    stroke_round(x, y, w, h, 12, t->menu_line, 255);
    draw_text_bold(x + 18, y + 16, "Project", t->text);
    int have = kms_on && kms_second >= 0;
    const char *sub = !kms_on ? "Not available on this computer" : have ? kout[kms_second].name : "No second screen plugged in";
    draw_text(x + w - 18 - text_w(sub), y + 16, sub, t->sub, 255);
    int hv = hot.type == HOT_DP ? hot.a : -1;
    for (int k = 0; k < DM_N; k++) {
        int ry = y + 44 + k * DP_RH, on = k == kms_cur, sel = k == dp.sel, ok = have || k == DM_MAIN;
        if (on || sel || hv == k) fill_round(x + 8, ry, w - 16, DP_RH - 6, 8, on ? mix(t->accent, t->menu, 0.75f) : t->hover, 255);
        if (sel) stroke_round(x + 8, ry, w - 16, DP_RH - 6, 8, t->accent, 255);
        dp_draw_icon(k, x + 22, ry + 14, ok ? t->text : t->sub, t->accent);
        draw_text_bold(x + 96, ry + 12, dm_names[k], ok ? t->text : t->sub);
        static const char *ds[DM_N] = { "Only the laptop's screen", "The same picture on both", "One big desktop over both", "Only the other screen" };
        draw_text(x + 96, ry + 32, ds[k], t->sub, ok ? 255 : 140);
    }
}
static int dp_hit(void) {
    if (!dp.open) return -1;
    int x = dp_x(), y = dp_y();
    if (!in_rect(x, y, DP_W, DM_N * DP_RH + 64)) return -1;
    for (int k = 0; k < DM_N; k++) if (in_rect(x + 8, y + 44 + k * DP_RH, DP_W - 16, DP_RH - 6)) return k;
    return -2;
}
static void dp_choose(int k) {
    if (k < 0 || k >= DM_N) return;
    if (k != DM_MAIN && !(kms_on && kms_second >= 0)) { toast(ICON_COMPUTER, "No second screen", "Plug in a screen or TV (HDMI, DisplayPort, USB-C) first."); return; }
    dp_set(0);
    display_set(k);
}
static void dp_click(int id) { if (id >= 0) dp_choose(id); }
static void dp_key(int code) {
    if (code == KEY_ESC) { dp_set(0); return; }
    if (code == KEY_UP) { dp.sel = (dp.sel + DM_N - 1) % DM_N; dp_repaint(); return; }
    if (code == KEY_DOWN || code == KEY_P) { dp.sel = (dp.sel + 1) % DM_N; dp_repaint(); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE) dp_choose(dp.sel);
}
