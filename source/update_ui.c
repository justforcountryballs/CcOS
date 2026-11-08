/* ===================================================================
 *  Settings > Update: new versions of ccos from GitHub
 *  (github.com/justforcountryballs/CcOS, its Releases page).
 *  The work is done by /usr/bin/ccos-update (update.c); this is the
 *  page, the daily check and the "updated / went back" notices.
 * =================================================================== */
static void st_repaint(void);
static void power_action(const char *mode);
static void wl_spawn(const char *prog, char *const argv[]);
static int vcmp_ui(const char *a, const char *b) {         /* "1.10" > "1.9" */
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    while (*a || *b) {
        char *ea, *eb; long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        if ((*ea && *ea != '.') || (*eb && *eb != '.')) return strcmp(ea, eb);
        a = *ea ? ea + 1 : ea; b = *eb ? eb + 1 : eb;
    }
    return 0;
}

enum { UP_IDLE, UP_CHECKING, UP_NEWER, UP_LATEST, UP_ERR, UP_WORKING, UP_DONE };
static struct {
    int state, quiet;                         /* quiet: the daily check (no page open) */
    pid_t pid; int kind;                      /* 0 check, 1 install, 2 confirm */
    char cur[32], latest[32], url[600], page[300], err[160], step[48], newver[32];
    long long size, total, done;
    char notes[8][90]; int nnotes;
    int stick;
    time_t checked;
} up = { .stick = -1 };
#define UP_OUT "/run/ccos/update-out.txt"

static int up_alive(void) { return up.pid > 0 && kill(up.pid, 0) == 0; }
static void up_run(int kind, const char *a1, const char *a2) {
    if (up_alive()) return;
    mkdir("/run/ccos", 0755);
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int fd = open(UP_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); close(fd); }
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        execl("/usr/bin/ccos-update", "ccos-update", a1, a2, (char *)NULL);
        _exit(127);
    }
    up.pid = p > 0 ? p : 0; up.kind = kind;
}
static void up_api_url(char *out, size_t n) {
    if (!set_upd_api[0]) {                       /* a test server, built into a test system */
        FILE *f = fopen("/etc/ccos-update-api", "r");
        if (f) { if (fgets(set_upd_api, sizeof set_upd_api, f)) set_upd_api[strcspn(set_upd_api, "\r\n")] = 0; fclose(f); }
    }
    if (set_upd_api[0]) snprintf(out, n, "%s/repos/%s/releases/latest", set_upd_api, set_upd_repo);
    else snprintf(out, n, "https://api.github.com/repos/%s/releases/latest", set_upd_repo);
}
static void up_check(int quiet) {
    if (up_alive() || up.state == UP_WORKING || up.state == UP_DONE) return;
    char u[400]; up_api_url(u, sizeof u);
    up.state = UP_CHECKING; up.quiet = quiet; up.err[0] = 0;
    up_run(0, "check", u);
    st_repaint();
}
static void up_install(const char *src) {
    if (up_alive() || up.state == UP_DONE) return;
    up.state = UP_WORKING; up.err[0] = 0; up.total = up.done = 0;
    snprintf(up.step, sizeof up.step, "Starting");
    static char s[600]; snprintf(s, sizeof s, "%s", src);
    up_run(1, "install", s);
    st_repaint();
}
/* double-click on a .ccupd file */
static int is_ccupd(const char *name) { const char *e = strrchr(name, '.'); return e && !strcasecmp(e, ".ccupd"); }
static void up_install_file(const char *path) {
    st_tab = TAB_UPDATE; run("@settings");
    up_install(path);
}
/* release notes: Markdown, shown plainly */
static void up_read_notes(void) {
    up.nnotes = 0;
    FILE *f = fopen("/run/ccos/update-notes.txt", "r"); if (!f) return;
    char l[400];
    while (fgets(l, sizeof l, f) && up.nnotes < 8) {
        l[strcspn(l, "\r\n")] = 0;
        char *s = l; while (*s == '#' || *s == ' ') s++;
        if (!*s) continue;
        char o[400]; size_t k = 0;
        for (char *p = s; *p && k + 1 < sizeof o; p++) { if (*p == '*' || *p == '`' || *p == '_') continue; o[k++] = *p; }
        o[k] = 0;
        snprintf(up.notes[up.nnotes], sizeof up.notes[0], "%s", o);
        if (text_len(up.notes[up.nnotes]) > 74) { u8_trunc(up.notes[up.nnotes], 72); strcat(up.notes[up.nnotes], ".."); }
        up.nnotes++;
    }
    fclose(f);
}

static void update_started(void);
/* once a second */
static void update_poll(void) {
    static time_t boot; if (!boot) boot = time(NULL);
    static int confirm_done;                     /* tell the stick this version works (once it's found) */
    if (!confirm_done && !up_alive() && time(NULL) - boot >= 5) {
        if (access("/mnt/usb/ccos/ccos.txt", F_OK) == 0) { confirm_done = 1; update_started(); return; }
        if (time(NULL) - boot > 600) confirm_done = 1;
    }
    /* a minute after start, then daily: look for a new version */
    if (set_upd_auto && !up_alive() && up.state != UP_WORKING && up.state != UP_DONE && time(NULL) - boot > 60 &&
        time(NULL) - set_upd_last > 24 * 3600 && (!up.checked || time(NULL) - up.checked > 3600)) {
        up.checked = time(NULL);                 /* even if it fails: try again in an hour */
        up_check(1);
    }
    if (!up.pid) return;
    int alive = up_alive();
    FILE *f = fopen(UP_OUT, "r");
    char l[700]; int fin = 0, ok = 0;
    if (f) {
        while (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\n")] = 0;
            if (!strncmp(l, "CURRENT ", 8)) snprintf(up.cur, sizeof up.cur, "%s", l + 8);
            else if (!strncmp(l, "LATEST ", 7)) snprintf(up.latest, sizeof up.latest, "%s", l + 7);
            else if (!strncmp(l, "URL ", 4)) snprintf(up.url, sizeof up.url, "%s", l + 4);
            else if (!strncmp(l, "PAGE ", 5)) snprintf(up.page, sizeof up.page, "%s", l + 5);
            else if (!strncmp(l, "SIZE ", 5)) up.size = atoll(l + 5);
            else if (!strncmp(l, "STEP ", 5)) { snprintf(up.step, sizeof up.step, "%s", l + 5); up.done = 0; up.total = 0; }
            else if (!strncmp(l, "T ", 2)) up.total = atoll(l + 2);
            else if (!strncmp(l, "P ", 2)) up.done = atoll(l + 2);
            else if (!strcmp(l, "NEWER")) { fin = 1; ok = 1; }
            else if (!strcmp(l, "UPTODATE")) { fin = 1; ok = 2; }
            else if (!strncmp(l, "OK ", 3)) { fin = 1; ok = 1; snprintf(up.newver, sizeof up.newver, "%s", l + 3); }
            else if (!strcmp(l, "CANCELED")) { fin = 1; ok = 3; }
            else if (!strncmp(l, "ERR ", 4)) { fin = 1; snprintf(up.err, sizeof up.err, "%s", l + 4); }
            else if (!strncmp(l, "CONFIRMED ", 10)) { fin = 1; ok = 4; snprintf(up.newver, sizeof up.newver, "%s", l + 10); }
            else if (!strncmp(l, "ROLLEDBACK", 10)) { fin = 1; ok = 5; snprintf(up.newver, sizeof up.newver, "%s", l[10] ? l + 11 : ""); }
            else if (!strcmp(l, "NOTHING")) { fin = 1; ok = 6; }
        }
        fclose(f);
    }
    if (alive && !fin) { st_repaint(); return; }
    int kind = up.kind;
    up.pid = 0;
    if (!fin && !up.err[0]) snprintf(up.err, sizeof up.err, "The updater stopped unexpectedly.");
    if (kind == 0) {                                     /* check */
        if (ok == 1 || ok == 2) { set_upd_last = (long long)time(NULL); save_settings(); }
        up.checked = time(NULL);
        if (ok == 1) {
            up.state = UP_NEWER; up_read_notes();
            if (up.quiet) {
                char t[64]; snprintf(t, sizeof t, "ccos %s is available", up.latest);
                toast(ICON_GEAR, t, "Install it in Settings > Update. Your files and apps stay.");
            }
        } else if (ok == 2) up.state = UP_LATEST;
        else up.state = up.quiet ? UP_IDLE : UP_ERR;
    } else if (kind == 1) {                              /* install */
        if (ok == 1) {
            up.state = UP_DONE;
            char t[64]; snprintf(t, sizeof t, "ccos %s is ready", up.newver);
            toast(ICON_GEAR, t, "Restart to start using it.");
        } else if (ok == 3) up.state = up.latest[0] && vcmp_ui(up.latest, up.cur) > 0 ? UP_NEWER : UP_IDLE;
        else up.state = UP_ERR;
    } else {                                             /* confirm, at start */
        char b[120];
        if (ok == 4) {
            snprintf(b, sizeof b, "ccos was updated to %s. Your files and apps are all here.", up.newver);
            toast(ICON_GEAR, "Update finished", b);
        } else if (ok == 5) {
            snprintf(b, sizeof b, "ccos %s didn't start properly, so ccos went back to the version before.", up.newver);
            toast(ICON_GEAR, "The update didn't work", b);
        }
        up.state = UP_IDLE; up.err[0] = 0;
    }
    st_repaint();
}
/* when the desktop is up: tell the stick this version works */
static void update_started(void) {
    up_run(2, "confirm", NULL);
}

/* ---- the page ---- */
#define UP_BTN_Y 100
static void up_status(char *s, size_t n) {
    struct tm tm; localtime_r(&up.checked, &tm);
    char when[16]; strftime(when, sizeof when, set_clock24 ? "%H:%M" : "%I:%M %p", &tm);
    switch (up.state) {
    case UP_CHECKING: snprintf(s, n, "Looking for updates..."); break;
    case UP_LATEST: snprintf(s, n, "ccos is up to date (checked at %s).", when); break;
    case UP_NEWER: {
        char sz[24] = ""; if (up.size > 0) snprintf(sz, sizeof sz, " (%lld MB)", (up.size + (1 << 19)) >> 20);
        snprintf(s, n, "ccos %s is available%s.", up.latest, sz); break;
    }
    case UP_ERR: snprintf(s, n, "%s", up.err); break;
    case UP_WORKING: snprintf(s, n, "%s...", up.step); break;
    case UP_DONE: snprintf(s, n, "ccos %s is installed. Restart to start using it.", up.newver); break;
    default: snprintf(s, n, "Press the button to look for a new version."); break;
    }
}
static void update_draw(int cx, int cy, int cw, uint32_t text, uint32_t grey) {
    int y = cy + ST_Y;
    if (up.stick < 0) up.stick = access("/mnt/usb/ccos/ccos.txt", F_OK) == 0;
    if (!up.cur[0]) { FILE *f = fopen("/etc/ccos-version", "r"); if (f) { if (fgets(up.cur, sizeof up.cur, f)) up.cur[strcspn(up.cur, "\r\n")] = 0; fclose(f); } }
    st_heading(cx, y + 20, "ccos updates");
    char b[200];
    snprintf(b, sizeof b, "You have ccos %s", up.cur[0] ? up.cur : "?");
    draw_text(cx + 24, y + 50, b, text, 255);
    draw_text(cx + 24, y + 68, up.stick ? "Updates install on this USB stick. Your files, settings and apps stay."
                                        : "Running in QEMU: updates come as a new ccos.zip to download.", grey, 255);
    int bx = cx + 24, by = y + UP_BTN_Y, hid = hot.type == HOT_CONTENT && wins[hot.a].draw == settings_draw ? hot.b : -1;
    (void)hid;
    int busy = up_alive() || up.state == UP_WORKING;
    if (up.state == UP_NEWER) win7_button(bx, by, 150, 28, up.stick ? "Install now" : "Download page", 0, 1);
    else if (up.state == UP_DONE) win7_button(bx, by, 150, 28, "Restart now", 0, 1);
    else if (up.state == UP_WORKING) win7_button(bx, by, 150, 28, "Cancel", 0, 0);
    else win7_button(bx, by, 150, 28, up.state == UP_CHECKING ? "Looking..." : "Check for updates", 0, !busy);
    up_status(b, sizeof b);
    uint32_t sc = up.state == UP_ERR ? RGB(0xb0, 0x30, 0x20) : up.state == UP_NEWER || up.state == UP_DONE ? RGB(0x1e, 0x6c, 0x2a) : text;
    if (text_len(b) > 58) {                                  /* long messages: two lines */
        char a[200]; snprintf(a, sizeof a, "%s", b);
        int len = (int)strlen(a), col = 0, br = 0;
        for (int i = 0; i < len; i = u8_next(a, len, i)) { if (a[i] == ' ' && col <= 58) br = i; col++; }
        if (!br || br >= len) { u8_trunc(a, 58); draw_text(cx + 190, by + 6, a, sc, 255); }
        else { a[br] = 0; draw_text(cx + 190, by - 2, a, sc, 255); draw_text(cx + 190, by + 15, a + br + 1, sc, 255); }
    } else draw_text(cx + 190, by + 6, b, sc, 255);
    int ny = by + 46;
    if (up.state == UP_WORKING) {                              /* progress */
        int pct = up.total > 0 ? (int)(up.done * 100 / up.total) : 0;
        if (!strcmp(up.step, "Installing") && up.total == 1000) pct = (int)(up.done / 10);
        if (pct > 100) pct = 100;
        int w = cw - 48;
        fill_round(cx + 24, ny, w, 18, 3, RGB(0xe2, 0xe6, 0xec), 255);
        stroke_round(cx + 24, ny, w, 18, 3, RGB(0xb8, 0xc0, 0xcc), 255);
        if (pct) fill_round_grad(cx + 25, ny + 1, imax(4, (w - 2) * pct / 100), 16, 2, RGB(0x6c, 0xd8, 0x6c), RGB(0x2c, 0xa8, 0x3c), 255);
        if (!strcmp(up.step, "Downloading") && up.total > 0) snprintf(b, sizeof b, "%d%%   %lld MB of %lld MB", pct, up.done >> 20, up.total >> 20);
        else snprintf(b, sizeof b, "%d%%", pct);
        draw_text(cx + 24, ny + 26, b, grey, 255);
        draw_text(cx + 24, ny + 46, "You can keep using ccos meanwhile. Don't pull out the USB stick.", grey, 255);
        ny += 76;
    }
    if (up.state == UP_DONE) {
        draw_text(cx + 24, ny, "If the new version doesn't start properly, ccos goes back to", grey, 255);
        snprintf(b, sizeof b, "version %s by itself. Nothing is lost either way.", up.cur);
        draw_text(cx + 24, ny + 18, b, grey, 255);
        ny += 46;
    }
    if ((up.state == UP_NEWER || up.state == UP_WORKING || up.state == UP_DONE) && up.nnotes) {
        draw_text_bold(cx + 24, ny, "What's new", text);
        fill_round(cx + 24, ny + 20, cw - 48, 10 + up.nnotes * 18, 3, RGB(0xf6, 0xf8, 0xfb), 255);
        stroke_round(cx + 24, ny + 20, cw - 48, 10 + up.nnotes * 18, 3, RGB(0xdc, 0xe4, 0xec), 255);
        for (int i = 0; i < up.nnotes; i++) draw_text(cx + 34, ny + 26 + i * 18, up.notes[i], text, 255);
        ny += 36 + up.nnotes * 18;
    }
    int ay = y + 400;
    st_line(cx, cw, ay - 14);
    draw_text(cx + 24, ay, "Look for updates automatically", text, 255);
    draw_text(cx + 24, ay + 18, "Once a day. You get a notification; nothing installs by itself.", grey, 255);
    st_switch(cx + cw - 24 - TG_W, ay - 2, set_upd_auto);
    snprintf(b, sizeof b, "Updates come from github.com/%s (Releases).", set_upd_repo);
    draw_text(cx + 24, ay + 50, b, grey, 255);
    draw_text(cx + 24, ay + 68, "Only updates signed by ccos are installed.", grey, 255);
}
static void update_click(int lx, int ly, int cw) {
    int by = ST_Y + UP_BTN_Y;
    if (lx >= 24 && lx < 174 && ly >= by && ly < by + 28) {
        if (up.state == UP_NEWER) {
            if (up.stick && up.url[0]) up_install(up.url);
            else if (up.page[0]) { char *const argv[] = { "xdg-open", up.page, NULL }; wl_spawn("/usr/bin/xdg-open", argv); }
        } else if (up.state == UP_DONE) power_action("reboot");
        else if (up.state == UP_WORKING) { if (up_alive()) kill(up.pid, SIGTERM); }
        else up_check(0);
        st_repaint();
        return;
    }
    int ay = ST_Y + 400;
    if (ly >= ay - 6 && ly < ay + 36 && (lx >= cw - 24 - TG_W - 40 || lx < 24 + 32 * 8)) {
        set_upd_auto = !set_upd_auto; save_settings(); st_repaint();
    }
}
