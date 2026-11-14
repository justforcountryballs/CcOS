/* ===================================================================
 *  The Dock: the taskbar's other look (Settings > Appearance > Taskbar).
 *  A floating shelf in the middle of the bottom edge: the /o\ Finder
 *  first, then the pinned apps and open windows, then the Recycle Bin.
 *  Pointing at it makes the icons near the mouse grow and push the
 *  others aside; the name of the one under the mouse shows above it.
 *  The clock, network, battery and a power button sit in a small pill
 *  in the bottom right corner.
 * =================================================================== */
#define DOCK_B     dock_b()           /* icon size (Themes > Taskbar) */
#define DOCK_GAP   10
#define DOCK_PAD   9
#define DOCK_MAG   0.72f              /* the icon under the mouse grows by this much */
#define DOCK_BOT   8                  /* space under the shelf */
#define DOCK_SH    (DOCK_B + 2 * DOCK_PAD)
#define DOCK_TOP   (DOCK_B * 2 + 60)  /* how far above the bottom it can draw (grown icons + the name) */
#define DOCK_MAXS  (MAXPINS + MAXWIN + 2)
enum { DS_FINDER, DS_TASK, DS_BIN };
typedef struct { int kind, k; float cx, size; } DockSlot;
static float dock_mag, dock_mag_to;     /* 0..1: how much the icons grow (animated) */
static float dock_mx = -1;              /* where the mouse is along the dock */
static long long dock_anim_t;

static void finder_open(void);
static int hp_shown_(void);
/* the slots, laid out with the current growing; returns how many. Also the shelf. */
static int dock_layout(DockSlot *s, int *shelf_x, int *shelf_w) {
    TbItem it[MAXPINS + MAXWIN];
    int nt = tb_items(it), n = 0;
    s[n++] = (DockSlot){ DS_FINDER, 0, 0, 0 };
    for (int k = 0; k < nt; k++) s[n++] = (DockSlot){ DS_TASK, k, 0, 0 };
    s[n++] = (DockSlot){ DS_BIN, 0, 0, 0 };
    /* where each would be without growing: the growing goes by that */
    float basew = n * DOCK_B + (n - 1) * DOCK_GAP + 14, x0 = (W - basew) / 2 + DOCK_B / 2.0f;
    float sep = 14;                                         /* a little gap before the Bin */
    float total = 0;
    for (int i = 0; i < n; i++) {
        float c = x0 + i * (DOCK_B + DOCK_GAP) + (i == n - 1 ? sep : 0);
        float d = dock_mx < 0 ? 1e9f : (dock_mx - c) / (DOCK_B * 1.35f);
        s[i].size = DOCK_B * (1 + DOCK_MAG * dock_mag * expf(-d * d));
        total += s[i].size + (i ? DOCK_GAP : 0) + (i == n - 1 ? sep : 0);
    }
    float x = (W - total) / 2;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) x += sep;
        s[i].cx = x + s[i].size / 2; x += s[i].size + DOCK_GAP;
    }
    *shelf_w = (int)(total + 2 * DOCK_PAD + 0.5f); *shelf_x = (W - *shelf_w) / 2;
    return n;
}
static int dock_shelf_y(void) { return H - DOCK_BOT - DOCK_SH; }
static void repaint_dock(void) { repaint(0, H - DOCK_TOP, W, DOCK_TOP); }

/* the slot under the mouse, -1 none */
static int dock_hit_slot(DockSlot *s, int n) {
    int sx, sw; (void)sx; (void)sw;
    for (int i = 0; i < n; i++) {
        float half = s[i].size / 2 + DOCK_GAP / 2.0f;
        int top = H - DOCK_BOT - DOCK_PAD - (int)s[i].size - 4;
        if (mx >= s[i].cx - half && mx < s[i].cx + half && my >= top && my < H) return i;
    }
    return -1;
}

/* the mouse moved: grow (or shrink back) */
static void dock_motion(void) {
    if (!set_dock || taskbar_hidden()) return;
    DockSlot s[DOCK_MAXS]; int sx, sw, n = dock_layout(s, &sx, &sw);
    int in = my >= H - TASKBAR_H - 4 && mx >= sx - 10 && mx < sx + sw + 10;
    if (dock_mag > 0.01f && dock_hit_slot(s, n) >= 0) in = 1;     /* over a grown icon */
    if (menu_open || pop.open) in = 0;
    float old_mx = dock_mx;
    if (in) dock_mx = (float)mx;
    if (in != (dock_mag_to > 0.5f)) { dock_mag_to = in ? 1 : 0; dock_anim_t = ui_ms(); }
    if (in && old_mx != dock_mx && dock_mag > 0.01f) repaint_dock();
}
/* in the main loop: the growing / shrinking animation. How long to wait (ms), or -1 */
static int dock_tick(void) {
    if (!set_dock || dock_mag == dock_mag_to) return -1;
    long long now = ui_ms(); float dt = (float)(now - dock_anim_t) / 130.0f; dock_anim_t = now;
    if (dt > 0.5f) dt = 0.5f;
    if (dock_mag < dock_mag_to) dock_mag = fminf(dock_mag_to, dock_mag + dt);
    else dock_mag = fmaxf(dock_mag_to, dock_mag - dt);
    if (dock_mag == 0) dock_mx = -1;
    repaint_dock();
    return dock_mag == dock_mag_to ? -1 : 16;
}

static const char *dock_name(const DockSlot *d, char *buf, size_t n) {
    if (d->kind == DS_FINDER) return "Finder";
    if (d->kind == DS_BIN) return "Recycle Bin";
    TbItem it[MAXPINS + MAXWIN]; int nt = tb_items(it);
    if (d->k >= nt) return "";
    if (it[d->k].win >= 0) { snprintf(buf, n, "%s", wins[it[d->k].win].title); return buf; }
    const AppInfo *ai = app_info(pins[it[d->k].pin]);
    return ai ? ai->label : "";
}

/* the /o\ logo, drawn with lines so it can be any size */
static void dock_logo(float cx, float cy, float s, uint32_t c) {
    float h = s * 0.42f, w = s * 0.2f, lw = fmaxf(1.6f, s * 0.07f);
    aa_line(cx - s * 0.36f, cy + h / 2, cx - s * 0.36f + w * 0.8f, cy - h / 2, lw, c);
    aa_line(cx + s * 0.36f, cy + h / 2, cx + s * 0.36f - w * 0.8f, cy - h / 2, lw, c);
    float r = s * 0.13f;
    for (int a = 0; a < 48; a++) {                          /* the o */
        float t0 = a * 6.2832f / 48, t1 = (a + 1) * 6.2832f / 48;
        aa_line(cx + cosf(t0) * r, cy + h / 2 - r - lw * 0.3f + sinf(t0) * r, cx + cosf(t1) * r, cy + h / 2 - r - lw * 0.3f + sinf(t1) * r, lw, c);
    }
}

/* a rounded tile behind each icon, like app icons on a phone */
static void dock_tile(int x, int y, int s, uint32_t top, uint32_t bot) {
    int r = s * 22 / 100;
    fill_round(x + 1, y + 3, s, s, r, RGB(0, 0, 0), 40);
    fill_round_grad(x, y, s, s, r, top, bot, 255);
    stroke_round(x, y, s, s, r, RGB(0, 0, 0), 30);
}

static void draw_dock(void) {
    if (clip_y1 <= H - DOCK_TOP) return;
    DockSlot s[DOCK_MAXS]; int sx, sw, n = dock_layout(s, &sx, &sw);
    int sy = dock_shelf_y();
    const int dark = set_style && set_dark;
    /* the shelf: frosted glass */
    fill_round(sx + 2, sy + 4, sw, DOCK_SH, 18, RGB(0, 0, 0), 45);
    fill_round(sx, sy, sw, DOCK_SH, 18, dark ? RGB(0x28, 0x2a, 0x30) : RGB(0xf4, 0xf7, 0xfb), dark ? 190 : 140);
    stroke_round(sx, sy, sw, DOCK_SH, 18, dark ? RGB(0x60, 0x64, 0x6c) : RGB(255, 255, 255), dark ? 160 : 200);
    { int bx = (int)(s[n - 1].cx - s[n - 1].size / 2) - 12;   /* the line before the Bin */
      fill_rect(bx, sy + 12, 1, DOCK_SH - 24, dark ? RGB(0x80, 0x84, 0x8c) : RGB(0x70, 0x78, 0x84), 120); }
    TbItem it[MAXPINS + MAXWIN]; tb_items(it);
    int act = active_win(), hov = hot.type == HOT_ORB ? 0 : hot.type == HOT_TASK ? hot.a + 1 : hot.type == HOT_DOCKBIN ? n - 1 : -1;
    for (int i = 0; i < n; i++) {
        int sz = (int)(s[i].size + 0.5f), x = (int)(s[i].cx - sz / 2.0f + 0.5f), y = H - DOCK_BOT - DOCK_PAD - sz;
        int running = 0;
        if (s[i].kind == DS_FINDER) {
            dock_tile(x, y, sz, RGB(0x4f, 0xa8, 0xff), RGB(0x16, 0x5c, 0xd0));
            dock_logo(x + sz / 2.0f, y + sz / 2.0f, (float)sz, RGB(255, 255, 255));
            running = finder_running();
        } else if (s[i].kind == DS_BIN) {
            dock_tile(x, y, sz, RGB(0xfa, 0xfb, 0xfd), RGB(0xd8, 0xde, 0xe6));
            draw_icon(bin_count() ? ICON_BIN_FULL : ICON_BIN, x + sz / 2, y + sz / 2, sz * 30 / 100);
        } else {
            int w = it[s[i].k].win, icon = w >= 0 ? wins[w].icon : ICON_NONE;
            if (w < 0) { const AppInfo *ai = app_info(pins[it[s[i].k].pin]); if (ai) icon = ai->icon; }
            dock_tile(x, y, sz, RGB(0xfa, 0xfb, 0xfd), RGB(0xd8, 0xde, 0xe6));
            if (icon != ICON_NONE) draw_icon(icon, x + sz / 2, y + sz / 2, sz * 30 / 100);
            running = w >= 0 ? (w == act ? 2 : 1) : 0;
        }
        if (running)                                         /* a dot under open apps */
            fill_circle(x + sz / 2.0f, H - DOCK_BOT - 3.5f, running == 2 ? 2.6f : 2.0f, dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x20, 0x24, 0x2c), 230);
    }
    /* the name of the one under the mouse, above it */
    if (hov >= 0 && hov < n && dock_mag > 0.3f && !menu_open && !pop.open && !hp_shown_()) {
        char b[80]; const char *nm = dock_name(&s[hov], b, sizeof b);
        char t[44]; snprintf(t, sizeof t, "%s", nm); if (text_len(t) > 36) { u8_trunc(t, 34); strcat(t, ".."); }
        int tw = text_w(t), sz = (int)s[hov].size, top = H - DOCK_BOT - DOCK_PAD - sz;
        int px = clampi((int)s[hov].cx - tw / 2 - 10, 4, W - tw - 24), py = top - 34;
        fill_round(px, py, tw + 20, 26, 7, RGB(0x18, 0x1a, 0x20), (int)(200 * dock_mag));
        draw_text(px + 10, py + 5, t, RGB(255, 255, 255), (int)(255 * fminf(1, dock_mag * 1.5f)));
    }
}

/* ---- the little pill in the corner: battery, network, clock, power ---- */
#define DPOW_W 34
static int dpow_x(void) { return W - DPOW_W - 8; }
static int dock_pill_x(void) { return (rec_on() ? rec_tb_x() : bat.present ? bat_x() : tray_x()) - 8; }
static void draw_dock_pill(void) {
    int x = dock_pill_x(), y = TBI_Y - 1, w = W - 4 - x, h = TBI_H + 2;
    const int dark = set_style && set_dark;
    if (set_style) {
        fill_round(x + 1, y + 3, w, h, 12, RGB(0, 0, 0), 40);
        fill_round(x, y, w, h, 12, dark ? RGB(0x28, 0x2a, 0x30) : RGB(0xf4, 0xf7, 0xfb), dark ? 215 : 205);
        stroke_round(x, y, w, h, 12, dark ? RGB(0x60, 0x64, 0x6c) : RGB(255, 255, 255), 200);
    } else {                                                 /* Classic: dark glass, like the taskbar */
        fill_round(x, y, w, h, 12, RGB(0x0c, 0x1c, 0x2c), 185);
        stroke_round(x, y, w, h, 12, RGB(255, 255, 255), 70);
    }
    /* power: a circle with a line */
    int px = dpow_x(), cy = TBI_Y + TBI_H / 2, hv = hot.type == HOT_DPOWER;
    if (hv) fill_round(px, TBI_Y + 3, DPOW_W, TBI_H - 6, 6, set_style ? TH()->bar_hover : RGB(255, 255, 255), set_style ? 255 : 45);
    uint32_t c = set_style ? TH()->text : RGB(0xf0, 0xf4, 0xf8);
    for (int a = 0; a < 40; a++) {
        float t0 = -1.0f + a * 5.3f / 40 - 1.57f, t1 = t0 + 5.3f / 40;
        aa_line(px + 17 + cosf(t0 + 2.6f) * 7, cy + sinf(t0 + 2.6f) * 7, px + 17 + cosf(t1 + 2.6f) * 7, cy + sinf(t1 + 2.6f) * 7, 1.6f, c);
    }
    aa_line(px + 17, cy - 9, px + 17, cy - 1, 1.6f, c);
}
static void dock_power_menu(void) {
    static const char *const lb[] = { "Sleep", "Restart", "Shut down" };
    static const int ac[] = { PA_SLEEP, PA_RESTART, PA_SHUTDOWN };
    pop_show_list(dpow_x() - POP_W + DPOW_W, H, ICON_NONE, NULL, 3, lb, ac);
}

/* Settings: switch between the bar and the Dock */
static void toggle_max(int idx);
static void set_menu(int open);
static void pop_close(void);
static void dock_set(int on) {
    if (on == set_dock) return;
    set_menu(0); pop_close();
    set_dock = on; dock_mag = dock_mag_to = 0; dock_mx = -1;
    for (int i = 0; i < MAXWIN; i++)                         /* maximized windows: to the new height */
        if (wins[i].open && wins[i].maximized && !wins[i].fullscreen) { toggle_max(i); toggle_max(i); }
    save_settings();
    repaint_all();
}
