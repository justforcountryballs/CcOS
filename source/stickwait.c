/* ===================================================================
 *  "CcOS needs to find your USB boot stick."
 *
 *  Started from a USB stick, but the stick's storage part (your files and
 *  settings) isn't there yet: some laptops find a stick a few seconds late,
 *  or drop it. Before anything is loaded, ccos waits here, so it loads your
 *  real settings instead of the defaults. ccos-savedisk does the finding
 *  (in the background); this screen shows it and lets you skip.
 *  When the stick is found: "USB boot stick found!", then the usual fade
 *  into the desktop. Skip: straight to the desktop; if the stick turns up
 *  later, your settings are loaded then (see stick_late_check).
 * =================================================================== */
enum { SW_WAIT = 1, SW_FOUND };
static int sw_skip_x, sw_skip_y, sw_skip_w = 150, sw_skip_h = 36, sw_skip_hot;

static int booted_from_stick(void) {
    FILE *f = fopen("/proc/cmdline", "r"); char c[1024] = "";
    if (f) { if (!fgets(c, sizeof c, f)) c[0] = 0; fclose(f); }
    return strstr(c, "ccos.live") != NULL;
}
static int savedisk_running(void) {
    DIR *d = opendir("/proc"); struct dirent *e; int found = 0;
    while (d && !found && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[64], c[256] = ""; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        size_t n = fread(c, 1, sizeof c - 1, f); fclose(f);
        for (size_t i = 0; i < n; i++) if (!c[i]) c[i] = ' ';
        c[n] = 0;
        if (strstr(c, "ccos-savedisk")) found = 1;
    }
    if (d) closedir(d);
    return found;
}

static void sw_center(int y, const char *s, uint32_t c, int sc) {
    draw_text_scaled((W - text_w(s) * sc) / 2, y, s, c, sc);
}
static void sw_draw(int state) {
    clip_x0 = 0; clip_y0 = 0; clip_x1 = W; clip_y1 = H;
    fill_rect(0, 0, W, H, RGB(0, 0, 0), 255);
    int cy = H / 2 - 70;
    if (state == SW_WAIT) {
        sw_center(cy, "CcOS needs to find your USB boot stick.", RGB(255, 255, 255), 2);
        sw_center(cy + 50, "Please unplug and plug in your USB boot stick.", RGB(0xb8, 0xb8, 0xb8), 1);
        sw_skip_x = (W - sw_skip_w) / 2; sw_skip_y = cy + 110;
        uint32_t bg = sw_skip_hot ? RGB(0x3a, 0x3a, 0x3e) : RGB(0x22, 0x22, 0x25);
        fill_round(sw_skip_x, sw_skip_y, sw_skip_w, sw_skip_h, 6, bg, 255);
        stroke_round(sw_skip_x, sw_skip_y, sw_skip_w, sw_skip_h, 6, RGB(0x6a, 0x6a, 0x70), 255);
        draw_text(sw_skip_x + (sw_skip_w - text_w("Skip")) / 2, sw_skip_y + 10, "Skip", RGB(255, 255, 255), 255);
        sw_center(H - 60, "Without the stick, changes aren't kept after a restart.", RGB(0x70, 0x70, 0x70), 1);
    } else {
        sw_center(cy + 20, "USB boot stick found!", RGB(255, 255, 255), 2);
    }
    mark(0, 0, W, H);
    present();
}

static int sw_in_skip(void) {
    return mx >= sw_skip_x && mx < sw_skip_x + sw_skip_w && my >= sw_skip_y && my < sw_skip_y + sw_skip_h;
}
static void stick_wait_click(void) { if (sw_in_skip()) stick_wait = 2; }   /* 2 = skipped */

/* returns when the stick is there or the person skipped */
static void stick_wait_screen(void) {
    check_persistent();
    if (conf_persistent || !booted_from_stick()) return;
    /* a moment first: often the stick is just a little late */
    for (int i = 0; i < 15 && !conf_persistent; i++) { usleep(100000); check_persistent(); }
    if (conf_persistent) return;
    stick_wait = 1;
    mx = W / 2; my = H / 2 + 160;
    sw_draw(SW_WAIT);
    long long last_try = 0;
    while (stick_wait == 1) {
        struct pollfd pfd[MAXDEV]; int map[MAXDEV], n = 0;
        for (int i = 0; i < MAXDEV; i++) if (dev[i].fd > 0) { pfd[n].fd = dev[i].fd; pfd[n].events = POLLIN; map[n++] = i; }
        poll(pfd, (nfds_t)n, 200);
        int omx = mx, omy = my;
        for (int k = 0; k < n; k++) {
            if (!(pfd[k].revents & POLLIN)) continue;
            int d = map[k];
            struct input_event ev[64]; ssize_t got;
            while ((got = read(pfd[k].fd, ev, sizeof ev)) > 0) {
                for (int e = 0; e < got / (ssize_t)sizeof *ev; e++) {
                    struct input_event *v = &ev[e];
                    if (dev[d].tp) { touchpad_event(d, v); continue; }       /* a tap clicks through left_button */
                    if (v->type == EV_REL) {
                        if (v->code == REL_X) mx += v->value;
                        if (v->code == REL_Y) my += v->value;
                    } else if (v->type == EV_ABS && dev[d].abs) {
                        if (v->code == ABS_X) mx = (long)(v->value - dev[d].xmin) * (W - 1) / (dev[d].xmax - dev[d].xmin);
                        if (v->code == ABS_Y) my = (long)(v->value - dev[d].ymin) * (H - 1) / (dev[d].ymax - dev[d].ymin);
                    } else if (v->type == EV_KEY && v->value == 1) {
                        if (v->code == BTN_LEFT) stick_wait_click();
                        else if (v->code == KEY_ESC) stick_wait = 2;
                        else if ((v->code == KEY_ENTER || v->code == KEY_SPACE) && sw_in_skip()) stick_wait = 2;
                    }
                }
            }
        }
        mx = clampi(mx, 0, W - 1); my = clampi(my, 0, H - 1);
        if (stick_wait != 1) break;
        int hot = sw_in_skip();
        if (hot != sw_skip_hot) { sw_skip_hot = hot; sw_draw(SW_WAIT); }
        else if (mx != omx || my != omy) present();               /* just the pointer */
        /* found yet? ccos-savedisk looks in the background; start it again if it gave up */
        check_persistent();
        if (conf_persistent) {
            sw_draw(SW_FOUND);
            usleep(1300000);
            break;
        }
        if (ui_ms() - last_try > 5000) {
            last_try = ui_ms();
            if (!savedisk_running() && fork() == 0) {
                int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
                execl("/sbin/ccos-savedisk", "ccos-savedisk", "--watch", (char *)NULL);   /* keeps looking, and sets up apps when found */
                _exit(127);
            }
        }
    }
    stick_wait = 0;
    sw_skip_hot = 0;
}

/* skipped, and the stick turned up later: your settings are loaded now (not
   overwritten by the defaults this session started with). Called once a second. */
static void stick_late_check(void) {
    if (conf_persistent) return;
    static int live = -1; if (live < 0) live = booted_from_stick();
    if (!live) return;
    check_persistent();
    if (!conf_persistent) {                          /* keep something looking for it */
        static int n; if (n++ % 5) return;
        if (!savedisk_running() && fork() == 0) {
            int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
            execl("/sbin/ccos-savedisk", "ccos-savedisk", "--watch", (char *)NULL);
            _exit(127);
        }
        return;
    }
    load_settings();
    volume_apply();
    make_wallpaper();
    repaint_all();
    toast(ICON_USB, "USB boot stick found", "Your files and settings are back.");
}
