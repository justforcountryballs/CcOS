/* ===================================================================
 *  Game controllers: which ones are connected (USB or Bluetooth), and a
 *  live test - press buttons and move the sticks to see them light up,
 *  and a rumble test. Games (Sober, Steam, SuperTuxKart...) read the
 *  controllers themselves; ccos leaves them alone.
 * =================================================================== */
#define GP_W 760
#define GP_H 500
#define GP_MAX 8
static struct {
    struct { int ev; char name[80]; int bus, ff; } pad[GP_MAX]; int n, sel;
    int fd, fd_ev;                                /* the open one (to watch) */
    unsigned char keys[KEY_MAX / 8 + 1];          /* pressed */
    int absv[ABS_CNT], absmin[ABS_CNT], absmax[ABS_CNT], hasabs[ABS_CNT];
    int rumble_id; char msg[80];
} gp = { .fd = -1, .fd_ev = -1, .rumble_id = -1 };
static void gamepad_draw(int cx, int cy, int cw, int ch);
#define GBIT(a, k) ((a)[(k) / 8] & (1 << ((k) % 8)))

static void gp_scan(void) {                       /* the controllers there are now */
    int n = 0;
    for (int i = 0; i < 64 && n < GP_MAX; i++) {
        char p[40]; snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char kb[KEY_MAX / 8 + 1] = { 0 }, ffb[FF_MAX / 8 + 1] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb);
        if (GBIT(kb, BTN_GAMEPAD) || GBIT(kb, BTN_JOYSTICK) || GBIT(kb, BTN_TRIGGER_HAPPY1)) {
            gp.pad[n].ev = i;
            if (ioctl(fd, EVIOCGNAME(sizeof gp.pad[n].name), gp.pad[n].name) < 0) snprintf(gp.pad[n].name, sizeof gp.pad[n].name, "Controller");
            gp.pad[n].name[sizeof gp.pad[n].name - 1] = 0;
            struct input_id id; gp.pad[n].bus = ioctl(fd, EVIOCGID, &id) == 0 ? id.bustype : 0;
            ioctl(fd, EVIOCGBIT(EV_FF, sizeof ffb), ffb);
            gp.pad[n].ff = GBIT(ffb, FF_RUMBLE) ? 1 : 0;
            n++;
        }
        close(fd);
    }
    gp.n = n;
    if (gp.sel >= n) gp.sel = imax(0, n - 1);
}
static void gp_close_dev(void) { if (gp.fd >= 0) close(gp.fd); gp.fd = -1; gp.fd_ev = -1; gp.rumble_id = -1; }
static void gp_open_dev(void) {                   /* watch the chosen one */
    if (!gp.n) { gp_close_dev(); return; }
    if (gp.fd >= 0 && gp.fd_ev == gp.pad[gp.sel].ev) return;
    gp_close_dev();
    char p[40]; snprintf(p, sizeof p, "/dev/input/event%d", gp.pad[gp.sel].ev);
    gp.fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (gp.fd < 0) gp.fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (gp.fd < 0) return;
    gp.fd_ev = gp.pad[gp.sel].ev;
    memset(gp.keys, 0, sizeof gp.keys); memset(gp.hasabs, 0, sizeof gp.hasabs);
    ioctl(gp.fd, EVIOCGKEY(sizeof gp.keys), gp.keys);
    unsigned char ab[ABS_MAX / 8 + 1] = { 0 };
    ioctl(gp.fd, EVIOCGBIT(EV_ABS, sizeof ab), ab);
    for (int a = 0; a < ABS_CNT; a++) if (GBIT(ab, a)) {
        struct input_absinfo ai;
        if (ioctl(gp.fd, EVIOCGABS(a), &ai) == 0) { gp.hasabs[a] = 1; gp.absv[a] = ai.value; gp.absmin[a] = ai.minimum; gp.absmax[a] = ai.maximum; }
    }
}
static int gp_fd(void) { return gp.fd; }
static void gp_wake(void) {
    if (gp.fd < 0) return;
    struct input_event e[64]; ssize_t r; int any = 0;
    while ((r = read(gp.fd, e, sizeof e)) > 0) {
        for (int i = 0; i < (int)(r / (ssize_t)sizeof e[0]); i++) {
            if (e[i].type == EV_KEY && e[i].code < KEY_MAX) {
                if (e[i].value) gp.keys[e[i].code / 8] |= 1 << (e[i].code % 8); else gp.keys[e[i].code / 8] &= ~(1 << (e[i].code % 8));
                any = 1;
            } else if (e[i].type == EV_ABS && e[i].code < ABS_CNT) { gp.absv[e[i].code] = e[i].value; any = 1; }
        }
    }
    if (r < 0 && errno == ENODEV) { gp_close_dev(); gp_scan(); any = 1; }   /* unplugged */
    if (any) repaint_client(gamepad_draw);
}
static float gp_axis(int a) {                     /* -1 .. 1 (0 in the middle) */
    if (!gp.hasabs[a] || gp.absmax[a] == gp.absmin[a]) return 0;
    float v = (gp.absv[a] - gp.absmin[a]) / (float)(gp.absmax[a] - gp.absmin[a]) * 2 - 1;
    return fabsf(v) < 0.06f ? 0 : v;
}
static float gp_trig(int a, int btn) {            /* 0 .. 1 */
    if (gp.hasabs[a] && gp.absmax[a] != gp.absmin[a]) return clampf((gp.absv[a] - gp.absmin[a]) / (float)(gp.absmax[a] - gp.absmin[a]), 0, 1);
    return GBIT(gp.keys, btn) ? 1 : 0;
}
static void gp_rumble(void) {
    if (gp.fd < 0) return;
    struct ff_effect fx; memset(&fx, 0, sizeof fx);
    fx.type = FF_RUMBLE; fx.id = -1; fx.u.rumble.strong_magnitude = 0xc000; fx.u.rumble.weak_magnitude = 0xc000; fx.replay.length = 700;
    if (ioctl(gp.fd, EVIOCSFF, &fx) < 0) { snprintf(gp.msg, sizeof gp.msg, "This controller can't rumble (or ccos can't make it)."); return; }
    gp.rumble_id = fx.id;
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = EV_FF; ev.code = (unsigned short)fx.id; ev.value = 1;
    if (write(gp.fd, &ev, sizeof ev) < 0) snprintf(gp.msg, sizeof gp.msg, "Couldn't start the rumble.");
    else gp.msg[0] = 0;
}
static int gp_battery(int ev) {                   /* % if the controller says (PlayStation, Switch...) */
    char p[200]; snprintf(p, sizeof p, "/sys/class/input/event%d/device/device/power_supply", ev);
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; int v = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char q[400]; snprintf(q, sizeof q, "%s/%s/capacity", p, e->d_name);
        FILE *f = fopen(q, "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    }
    closedir(d);
    return v;
}

/* ---- the window ---- */
enum { GB_PAD = 1, GB_RUMBLE = 50 };
static void gp_button(float x, float y, float r, int on, const char *l, uint32_t col) {
    fill_circle(x, y, r, on ? col : RGB(0x3a, 0x3e, 0x46), 255);
    if (on) fill_circle(x, y, r + 3, col, 70);
    if (l) draw_text((int)x - text_w(l) / 2, (int)y - 8, l, on ? RGB(255, 255, 255) : RGB(0xc8, 0xcc, 0xd4), 255);
}
static void gp_stick(float x, float y, float ax, float ay, int click) {
    fill_circle(x, y, 30, RGB(0x26, 0x29, 0x30), 255);
    stroke_round((int)x - 30, (int)y - 30, 60, 60, 30, RGB(0x50, 0x55, 0x60), 255);
    fill_circle(x + ax * 18, y + ay * 18, 17, click ? TH()->accent : RGB(0x4a, 0x4f, 0x5a), 255);
    fill_circle(x + ax * 18, y + ay * 18, 12, click ? mix(TH()->accent, RGB(255, 255, 255), 0.3f) : RGB(0x5a, 0x60, 0x6c), 255);
}
static void gamepad_draw(int cx, int cy, int cw, int ch) {
    uint32_t bg = RGB(255, 255, 255), text = RGB(0x1e, 0x22, 0x28), grey = RGB(0x70, 0x78, 0x84), acc = TH()->accent;
    fill_rect(cx, cy, cw, ch, bg, 255);
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == gamepad_draw ? hot.b : -1;
    /* the list */
    fill_rect(cx, cy, 220, ch, RGB(0xf4, 0xf6, 0xf9), 255);
    draw_text_bold(cx + 18, cy + 18, "Controllers", text);
    if (!gp.n) draw_text(cx + 18, cy + 50, "None connected", grey, 255);
    for (int i = 0; i < gp.n; i++) {
        int y = cy + 46 + i * 52, on = i == gp.sel;
        if (on || hv == GB_PAD + i) fill_round(cx + 8, y, 204, 46, 8, on ? RGB(0xdc, 0xea, 0xfb) : RGB(0xe8, 0xee, 0xf6), 255);
        char b[80]; snprintf(b, sizeof b, "%s", gp.pad[i].name);
        while (b[0] && text_w(b) > 180) b[u8_prev(b, (int)strlen(b))] = 0;
        draw_text_bold(cx + 18, y + 6, b, text);
        int bat = gp_battery(gp.pad[i].ev);
        char s[48]; snprintf(s, sizeof s, "%s%s", gp.pad[i].bus == BUS_BLUETOOTH ? "Bluetooth" : gp.pad[i].bus == BUS_USB ? "USB cable" : "Connected", "");
        if (bat >= 0) snprintf(s + strlen(s), sizeof s - strlen(s), " - %d%%", bat);
        draw_text(cx + 18, y + 25, s, grey, 255);
    }
    int x0 = cx + 220;
    if (!gp.n) {
        int y = cy + 60;
        draw_text_bold(x0 + 30, y, "No game controller connected", text);
        static const char *tips[] = {
            "Xbox, PlayStation, Switch Pro and most others work.",
            "",
            "With a cable: just plug it into a USB port.",
            "",
            "Wireless: open Settings > Bluetooth, then put the",
            "controller in pairing mode:",
            "  Xbox:         hold the small Pair button on the top",
            "  PlayStation:  hold Share (Create) + the PS button",
            "  Switch Pro:   hold the small Sync button on the top",
            "When it shows up in the list, click it.",
            "",
            "Games like Sober (Roblox) find it by themselves." };
        for (size_t k = 0; k < sizeof tips / sizeof tips[0]; k++) draw_text(x0 + 30, y + 36 + (int)k * 22, tips[k], k == 0 ? text : grey, 255);
        return;
    }
    /* the controller: a picture that lights up */
    float mxp = x0 + (cw - 220) / 2.0f, myp = cy + 220;
    uint32_t body = RGB(0x2e, 0x32, 0x3a);
    fill_round((int)mxp - 210, (int)myp - 90, 420, 170, 70, body, 255);
    fill_circle(mxp - 150, myp + 60, 70, body, 255);
    fill_circle(mxp + 150, myp + 60, 70, body, 255);
    /* shoulders and triggers */
    float lt = gp_trig(ABS_Z, BTN_TL2), rt = gp_trig(ABS_RZ, BTN_TR2);
    if (!gp.hasabs[ABS_Z] && gp.hasabs[ABS_BRAKE]) lt = gp_trig(ABS_BRAKE, BTN_TL2);
    if (!gp.hasabs[ABS_RZ] && gp.hasabs[ABS_GAS]) rt = gp_trig(ABS_GAS, BTN_TR2);
    fill_round((int)mxp - 200, (int)myp - 150, 110, 26, 10, RGB(0x3a, 0x3e, 0x46), 255);
    if (lt > 0) fill_round((int)mxp - 200, (int)myp - 150, (int)(110 * lt), 26, 10, acc, 255);
    fill_round((int)mxp + 90, (int)myp - 150, 110, 26, 10, RGB(0x3a, 0x3e, 0x46), 255);
    if (rt > 0) fill_round((int)mxp + 200 - (int)(110 * rt), (int)myp - 150, (int)(110 * rt), 26, 10, acc, 255);
    draw_text((int)mxp - 160, (int)myp - 146, "LT", RGB(255, 255, 255), 255); draw_text((int)mxp + 136, (int)myp - 146, "RT", RGB(255, 255, 255), 255);
    fill_round((int)mxp - 195, (int)myp - 116, 100, 20, 8, GBIT(gp.keys, BTN_TL) ? acc : RGB(0x4a, 0x4f, 0x5a), 255);
    fill_round((int)mxp + 95, (int)myp - 116, 100, 20, 8, GBIT(gp.keys, BTN_TR) ? acc : RGB(0x4a, 0x4f, 0x5a), 255);
    draw_text((int)mxp - 153, (int)myp - 114, "LB", RGB(255, 255, 255), 255); draw_text((int)mxp + 137, (int)myp - 114, "RB", RGB(255, 255, 255), 255);
    /* sticks */
    gp_stick(mxp - 130, myp - 30, gp_axis(ABS_X), gp_axis(ABS_Y), GBIT(gp.keys, BTN_THUMBL));
    gp_stick(mxp + 60, myp + 40, gp_axis(ABS_RX), gp_axis(ABS_RY), GBIT(gp.keys, BTN_THUMBR));
    /* d-pad */
    int hx = gp.hasabs[ABS_HAT0X] ? gp.absv[ABS_HAT0X] : 0, hy = gp.hasabs[ABS_HAT0Y] ? gp.absv[ABS_HAT0Y] : 0;
    int up = hy < 0 || GBIT(gp.keys, BTN_DPAD_UP), dn = hy > 0 || GBIT(gp.keys, BTN_DPAD_DOWN), lf = hx < 0 || GBIT(gp.keys, BTN_DPAD_LEFT), rg = hx > 0 || GBIT(gp.keys, BTN_DPAD_RIGHT);
    float dx = mxp - 60, dy = myp + 40;
    fill_round((int)dx - 12, (int)dy - 36, 24, 72, 4, RGB(0x3a, 0x3e, 0x46), 255);
    fill_round((int)dx - 36, (int)dy - 12, 72, 24, 4, RGB(0x3a, 0x3e, 0x46), 255);
    if (up) fill_round((int)dx - 12, (int)dy - 36, 24, 26, 4, acc, 255);
    if (dn) fill_round((int)dx - 12, (int)dy + 10, 24, 26, 4, acc, 255);
    if (lf) fill_round((int)dx - 36, (int)dy - 12, 26, 24, 4, acc, 255);
    if (rg) fill_round((int)dx + 10, (int)dy - 12, 26, 24, 4, acc, 255);
    /* face buttons (Xbox letters; on PlayStation: A = cross, B = circle, X = square, Y = triangle) */
    float bx = mxp + 130, by = myp - 30;
    gp_button(bx, by + 30, 16, GBIT(gp.keys, BTN_SOUTH), "A", RGB(0x2e, 0xa0, 0x4c));
    gp_button(bx + 30, by, 16, GBIT(gp.keys, BTN_EAST), "B", RGB(0xd0, 0x3a, 0x3a));
    gp_button(bx - 30, by, 16, GBIT(gp.keys, BTN_WEST), "X", RGB(0x2c, 0x78, 0xe0));
    gp_button(bx, by - 30, 16, GBIT(gp.keys, BTN_NORTH), "Y", RGB(0xe0, 0xb0, 0x20));
    /* middle */
    gp_button(mxp - 40, myp - 40, 10, GBIT(gp.keys, BTN_SELECT), NULL, acc);
    gp_button(mxp + 40, myp - 40, 10, GBIT(gp.keys, BTN_START), NULL, acc);
    gp_button(mxp, myp - 62, 15, GBIT(gp.keys, BTN_MODE), NULL, acc);
    draw_text((int)mxp - 60, (int)myp - 24, "Back", RGB(0xa0, 0xa6, 0xb0), 255);
    draw_text((int)mxp + 22, (int)myp - 24, "Start", RGB(0xa0, 0xa6, 0xb0), 255);
    /* under it */
    int ty = cy + ch - 92;
    draw_text(x0 + 30, ty, "Press buttons and move the sticks: they light up here.", grey, 255);
    if (gp.pad[gp.sel].ff) {
        int bxx = x0 + 30, byy = ty + 30, on = hv == GB_RUMBLE;
        fill_round(bxx, byy, 150, 36, 18, on ? mix(acc, RGB(0, 0, 0), 0.12f) : acc, 255);
        draw_text(bxx + (150 - text_w("Test rumble")) / 2, byy + 10, "Test rumble", RGB(255, 255, 255), 255);
    }
    if (gp.msg[0]) draw_text(x0 + 200, ty + 40, gp.msg, RGB(0xc0, 0x40, 0x30), 255);
}
static int gamepad_hit(int lx, int ly, int cw, int ch) {
    if (lx < 220) { for (int i = 0; i < gp.n; i++) if (ly >= 46 + i * 52 && ly < 92 + i * 52) return GB_PAD + i; return -1; }
    if (gp.n && gp.pad[gp.sel].ff && lx >= 250 && lx < 400 && ly >= ch - 62 && ly < ch - 26) return GB_RUMBLE;
    (void)cw; return -1;
}
static void gamepad_click(int lx, int ly, int cw, int ch) {
    int id = gamepad_hit(lx, ly, cw, ch);
    if (id >= GB_PAD && id < GB_PAD + GP_MAX) { gp.sel = id - GB_PAD; gp.msg[0] = 0; gp_open_dev(); }
    else if (id == GB_RUMBLE) gp_rumble();
    repaint_client(gamepad_draw);
}
static void gamepad_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(gamepad_draw), cx = 0, cy = 0, cw = GP_W, ch = GP_H;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    *x = cx; *y = cy; *w = cw; *h = ch; (void)id;
}
static void gamepad_tick(void) {                  /* plugged in / out? */
    int was = gp.n, ev = gp.n ? gp.pad[gp.sel].ev : -1;
    gp_scan();
    if (gp.n != was || (gp.n && gp.pad[gp.sel].ev != ev)) { gp_open_dev(); repaint_client(gamepad_draw); }
    else if (gp.n) repaint_client(gamepad_draw);   /* (battery) */
}
static int gamepad_on_close(void) { gp_close_dev(); return 1; }
static void gamepad_open(void) {
    gp_scan(); gp.msg[0] = 0; gp_open_dev();
    static const Win t = { .title = "Game controllers", .icon = ICON_GAMEPAD, .no_max = 1, .draw = gamepad_draw, .click = gamepad_click,
                           .hit = gamepad_hit, .elem_rect = gamepad_elem_rect, .fd = gp_fd, .wake = gp_wake, .tick = gamepad_tick, .on_close = gamepad_on_close };
    open_win(&t, GP_W, GP_H);
}
static void draw_gamepad_icon(int cx, int cy, int r) {
    uint32_t c = RGB(0x3a, 0x40, 0x4c);
    fill_round(cx - r, cy - r / 2, 2 * r, r, r / 2, c, 255);
    fill_circle(cx - r * 0.6f, cy + r * 0.35f, r * 0.45f, c, 255);
    fill_circle(cx + r * 0.6f, cy + r * 0.35f, r * 0.45f, c, 255);
    fill_rect(cx - r * 6 / 10 - imax(1, r / 10), cy - r / 5, imax(2, r / 5) + 1, r * 2 / 5, RGB(255, 255, 255), 255);
    fill_rect(cx - r * 6 / 10 - r / 5, cy - imax(1, r / 10), r * 2 / 5, imax(2, r / 5) + 1, RGB(255, 255, 255), 255);
    fill_circle(cx + r * 0.45f, cy - r * 0.05f, imax(1, r / 7), RGB(0x2e, 0xa0, 0x4c), 255);
    fill_circle(cx + r * 0.75f, cy - r * 0.05f, imax(1, r / 7), RGB(0xd0, 0x3a, 0x3a), 255);
}
