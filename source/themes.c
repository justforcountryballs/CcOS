/* ===================================================================
 *  Themes (the brush): how ccos looks, in one place - like Linux Mint's
 *  Themes app.
 *    Themes      complete looks: style, colours, folders, pointer, background
 *    Windows     light or dark, how much glass, accent colour
 *    Taskbar     Bar or Dock, Dock size, window animations
 *    Icons       folder colour
 *    Mouse       pointer colour and size
 *    Apps        how Flathub apps (Firefox...) look: default, dark, or a
 *                GTK theme downloaded from Flathub
 *    Get more    more ccos themes, downloaded from ccos's GitHub
 *  A theme file (.cctheme) is lines of key=value:
 *    name dark(0|1) glass(0-100) accent(rrggbb) folder cursor
 *    wallpaper gtk colors(title=rrggbb bar=rrggbb ...)
 * =================================================================== */
#define TM_W 860
#define TM_H 580
#define TM_SIDE 168
#define TM_DIR "/data/.themes"
#define TM_INDEX_URL "https://raw.githubusercontent.com/justforcountryballs/CcOS/main/themes/index.txt"
#define TM_FILE_URL "https://raw.githubusercontent.com/justforcountryballs/CcOS/main/themes/"
enum { TT_THEMES, TT_WINDOWS, TT_TASKBAR, TT_ICONS, TT_MOUSE, TT_APPS, TT_MORE, TT_N };
static const char *tm_tab_name[TT_N] = { "Themes", "Windows", "Taskbar", "Icons", "Mouse pointer", "Apps", "Get more" };
static const char *folder_names[8] = { "yellow", "blue", "green", "red", "purple", "grey", "orange", "pink" };
static const char *cursor_names[8] = { "white", "black", "blue", "red", "green", "purple", "pink", "yellow" };
static const uint32_t tm_accents[10] = { 0x2674d8, 0x0f9d9a, 0x35a854, 0x7cb342, 0xe0a020, 0xe8743c, 0xd64545, 0xd6578c, 0x8a5cd6, 0x5c6b7a };

static const char *tm_builtin[] = {
    "name=ccos Light\nstyle=modern\ndark=0\nfolder=yellow\ncursor=black\nwallpaper=water\ngtk=\n",
    "name=ccos Dark\nstyle=modern\ndark=1\nfolder=yellow\ncursor=white\nwallpaper=stardust\ngtk=dark\n",
    "name=Mint\nstyle=modern\ndark=0\naccent=35a854\nfolder=green\ncursor=black\nwallpaper=mountain-lake\ngtk=\n",
    "name=Mint Dark\nstyle=modern\ndark=1\naccent=35a854\nfolder=green\ncursor=white\nwallpaper=mountain-lake\ngtk=dark\n"
        "colors=title=2b2b2b title_in=303030 bar=222222 bar_line=333333 menu=2b2b2b menu_left=333333 btn=3a3a3a\n",
    "name=Nord\nstyle=modern\ndark=1\naccent=88c0d0\nfolder=blue\ncursor=white\nwallpaper=snowy-lake\ngtk=dark\n"
        "colors=title=2e3440 title_in=3b4252 border_in=434c5e text=eceff4 sub=d8dee9 bar=2e3440 bar_line=3b4252 bar_hover=434c5e menu=2e3440 menu_left=3b4252 menu_line=4c566a btn=434c5e\n",
    "name=Dracula\nstyle=modern\ndark=1\naccent=bd93f9\nfolder=purple\ncursor=purple\nwallpaper=purple-slabs\ngtk=dark\n"
        "colors=title=282a36 title_in=21222c border_in=44475a text=f8f8f2 sub=bfbfd0 bar=21222c bar_line=343746 bar_hover=343746 menu=282a36 menu_left=343746 menu_line=44475a btn=44475a\n",
    "name=Sunset\nstyle=modern\ndark=0\naccent=e8743c\nfolder=orange\ncursor=black\nwallpaper=above-the-clouds\ngtk=\n",
    "name=Rose\nstyle=modern\ndark=0\naccent=d6578c\nfolder=pink\ncursor=pink\nwallpaper=floating-peaks\ngtk=\n",
};
#define TM_NB (int)(sizeof tm_builtin / sizeof tm_builtin[0])
#define TM_MAXT 40
#define TM_GL_W 440
static int tm_gl_drag;
typedef struct { char name[48], file[200]; int style, dark, folder, cursor, wall, dock, glass; uint32_t accent; char gtk[64], colors[600]; } TmTheme;
static TmTheme tm_list[TM_MAXT]; static int tm_n;

/* jobs in the background (a download, an install): one at a time */
enum { TJ_NONE, TJ_GTKLIST, TJ_GTKINSTALL, TJ_INDEX, TJ_FETCH };
static struct {
    int tab, scroll, job, jfd; pid_t jpid; char jbuf[65536]; int jlen; char jarg[200], msg[160];
    char gtk_avail[400][48]; int ngtk_avail; int gtk_listed;
    char gtk_inst[64][64]; int ngtk_inst;
    struct { char file[64], name[48], desc[120]; } online[60]; int nonline, online_listed;
} tm = { .jfd = -1 };

static void themes_draw(int cx, int cy, int cw, int ch);
static void tm_repaint(void) { if (win_of(themes_draw) >= 0) repaint_client(themes_draw); }
static const char *tm_home(void) { const char *h = getenv("HOME"); return h && h[0] && strcmp(h, "/") ? h : "/apps/home"; }

static int tm_parse(const char *txt, TmTheme *t) {
    memset(t, 0, sizeof *t); t->wall = -1; t->dock = -1; t->glass = -1; t->cursor = -1; t->folder = -1; t->style = 1;
    const char *p = txt;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t L = e ? (size_t)(e - p) : strlen(p);
        char l[700]; snprintf(l, sizeof l, "%.*s", (int)imin((int)L, (int)sizeof l - 1), p);
        l[strcspn(l, "\r")] = 0;
        char *v = strchr(l, '=');
        if (v && l[0] != '#') {
            *v++ = 0;
            if (!strcmp(l, "name")) snprintf(t->name, sizeof t->name, "%s", v);
            else if (!strcmp(l, "style")) t->style = strcmp(v, "classic") != 0;
            else if (!strcmp(l, "dark")) t->dark = atoi(v) != 0;
            else if (!strcmp(l, "accent")) { unsigned u; if (sscanf(v, "%x", &u) == 1) t->accent = 0x1000000 | (u & 0xffffff); }
            else if (!strcmp(l, "folder")) { for (int i = 0; i < 8; i++) if (!strcasecmp(v, folder_names[i])) t->folder = i; }
            else if (!strcmp(l, "cursor")) { for (int i = 0; i < 8; i++) if (!strcasecmp(v, cursor_names[i])) t->cursor = i; }
            else if (!strcmp(l, "wallpaper")) { for (int i = 0; i < NWALL; i++) if (!strcmp(v, wallpapers[i].file)) t->wall = i; }
            else if (!strcmp(l, "gtk")) { snprintf(t->gtk, sizeof t->gtk, "%s", v); for (char *q = t->gtk; *q; q++) if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.') *q = 0; }
            else if (!strcmp(l, "dock")) t->dock = atoi(v) != 0;
            else if (!strcmp(l, "glass")) t->glass = clampi(atoi(v), 0, 100);
            else if (!strcmp(l, "colors")) snprintf(t->colors, sizeof t->colors, "%s", v);
        }
        p += L; if (*p) p++;
    }
    return t->name[0] != 0;
}
static void tm_load_list(void) {
    tm_n = 0;
    for (int i = 0; i < TM_NB && tm_n < TM_MAXT; i++) if (tm_parse(tm_builtin[i], &tm_list[tm_n])) tm_n++;
    DIR *d = opendir(TM_DIR); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && tm_n < TM_MAXT) {
        size_t L = strlen(e->d_name);
        if (L < 9 || strcmp(e->d_name + L - 8, ".cctheme")) continue;
        char p[300]; snprintf(p, sizeof p, TM_DIR "/%s", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
        if (tm_parse(b, &tm_list[tm_n])) { snprintf(tm_list[tm_n].file, sizeof tm_list[0].file, "%s", p); tm_n++; }
    }
    closedir(d);
}
static void tm_scan_gtk_installed(void) {
    tm.ngtk_inst = 0;
    char p[300]; snprintf(p, sizeof p, "%s/.themes", tm_home());
    DIR *d = opendir(p); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && tm.ngtk_inst < 64) {
        if (e->d_name[0] == '.') continue;
        snprintf(tm.gtk_inst[tm.ngtk_inst++], sizeof tm.gtk_inst[0], "%s", e->d_name);
    }
    closedir(d);
}

static void tm_changed(void) { th_ver++; save_settings(); repaint_all(); }
static void tm_apply(const TmTheme *t) {
    if (t->wall >= 0) { set_wall = t->wall; if (set_wall_img[0]) { unlink(set_wall_img); set_wall_img[0] = 0; wimg.path[0] = 0; } make_wallpaper(); }
    set_accent = t->accent;
    snprintf(set_tover, sizeof set_tover, "%s", t->colors);
    if (t->folder >= 0) set_folder = t->folder;
    if (t->cursor >= 0) set_cursor_col = t->cursor;
    if (t->glass >= 0) set_glass = t->glass;
    if (t->dock >= 0 && t->dock != set_dock) dock_set(t->dock);
    if (!strcmp(t->gtk, "dark") || !t->gtk[0]) snprintf(set_gtk, sizeof set_gtk, "%s", t->gtk);
    else { tm_scan_gtk_installed(); for (int i = 0; i < tm.ngtk_inst; i++) if (!strcmp(tm.gtk_inst[i], t->gtk)) snprintf(set_gtk, sizeof set_gtk, "%s", t->gtk); }
    snprintf(set_theme, sizeof set_theme, "%s", t->name);
    th_ver++;
    style_set(t->style, t->dark);          /* (saves and repaints) */
    tm_changed();
}
/* any one setting changed by hand: it's no longer exactly that theme */
static void tm_custom(void) { set_theme[0] = 0; }

/* ---- background jobs ---- */
static void tm_job(int kind, const char *arg, const char *shell) {
    if (tm.job) return;
    int pp[2]; if (pipe(pp)) return;
    pid_t p = fork();
    if (p == 0) {
        dup2(pp[1], 1); dup2(pp[1], 2);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", shell, (char *)NULL);
        _exit(127);
    }
    close(pp[1]);
    if (p < 0) { close(pp[0]); return; }
    tm.jpid = p; tm.jfd = pp[0]; tm.job = kind; tm.jlen = 0;
    snprintf(tm.jarg, sizeof tm.jarg, "%s", arg ? arg : "");
    fcntl(tm.jfd, F_SETFL, O_NONBLOCK); fcntl(tm.jfd, F_SETFD, FD_CLOEXEC);
    tm_repaint();
}
static int tm_fd(void) { return tm.jfd; }
static void tm_job_done(int ok) {
    int kind = tm.job; tm.job = TJ_NONE;
    tm.jbuf[tm.jlen] = 0;
    if (kind == TJ_GTKLIST) {
        tm.ngtk_avail = 0;
        for (char *l = strtok(tm.jbuf, "\n"); l && tm.ngtk_avail < 400; l = strtok(NULL, "\n")) {
            char *p = strstr(l, "org.gtk.Gtk3theme."); if (!p) continue;
            p += 18; p[strcspn(p, " \t\r")] = 0;
            int dup = 0; for (int i = 0; i < tm.ngtk_avail; i++) if (!strcmp(tm.gtk_avail[i], p)) dup = 1;
            if (!dup && strlen(p) < 48) snprintf(tm.gtk_avail[tm.ngtk_avail++], 48, "%s", p);
        }
        tm.gtk_listed = tm.ngtk_avail > 0;            /* nothing came back: keep the button to try again */
        snprintf(tm.msg, sizeof tm.msg, tm.ngtk_avail ? "" : "Couldn't get the list from Flathub. Check the internet and click Try again.");
    } else if (kind == TJ_GTKINSTALL) {
        tm_scan_gtk_installed();
        int have = 0; for (int i = 0; i < tm.ngtk_inst; i++) if (!strcmp(tm.gtk_inst[i], tm.jarg)) have = 1;
        if (have) {
            snprintf(set_gtk, sizeof set_gtk, "%s", tm.jarg); tm_custom(); save_settings();
            snprintf(tm.msg, sizeof tm.msg, "%s is on. Apps you open from now on use it.", tm.jarg);
        } else snprintf(tm.msg, sizeof tm.msg, "Couldn't install %s (no internet?).", tm.jarg);
    } else if (kind == TJ_INDEX) {
        tm.nonline = 0; tm.online_listed = 1;
        char p[300]; snprintf(p, sizeof p, TM_DIR "/.index.txt");
        FILE *f = ok ? fopen(p, "r") : NULL; char l[300];
        while (f && fgets(l, sizeof l, f) && tm.nonline < 60) {
            l[strcspn(l, "\r\n")] = 0;
            char *a = strtok(l, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t");
            if (!a || !b || l[0] == '#' || strchr(a, '/') || strstr(a, "..")) continue;
            snprintf(tm.online[tm.nonline].file, 64, "%s", a); snprintf(tm.online[tm.nonline].name, 48, "%s", b);
            snprintf(tm.online[tm.nonline].desc, 120, "%s", c ? c : ""); tm.nonline++;
        }
        if (f) fclose(f);
        if (tm.nonline) tm.msg[0] = 0;
        else if (strstr(tm.jbuf, "No ccos release") || strstr(tm.jbuf, "404")) snprintf(tm.msg, sizeof tm.msg, "The theme list isn't on GitHub yet - it appears once this ccos is published.");
        else { char *e = strstr(tm.jbuf, "ERR "); snprintf(tm.msg, sizeof tm.msg, "Couldn't get the list: %.100s", e ? e + 4 : "no internet?"); tm.msg[strcspn(tm.msg, "\n")] = 0; }
    } else if (kind == TJ_FETCH) {
        tm_load_list();
        snprintf(tm.msg, sizeof tm.msg, ok ? "Downloaded - it's in the Themes tab now." : "Couldn't download it (no internet?).");
        if (ok) for (int i = 0; i < tm_n; i++) if (strstr(tm_list[i].file, tm.jarg)) { tm_apply(&tm_list[i]); snprintf(tm.msg, sizeof tm.msg, "%s is on (and kept in the Themes tab).", tm_list[i].name); }
    }
    tm_repaint();
}
static void tm_wake(void) {
    if (tm.jfd < 0) return;
    ssize_t k;
    k = -1;
    while (tm.jlen >= 0 && tm.jlen < (int)sizeof tm.jbuf - 1 && (k = read(tm.jfd, tm.jbuf + tm.jlen, sizeof tm.jbuf - 1 - (size_t)tm.jlen)) > 0) tm.jlen += (int)k;
    if (k == 0 || tm.jlen >= (int)sizeof tm.jbuf - 1) {
        close(tm.jfd); tm.jfd = -1;
        int st = 0; waitpid(tm.jpid, &st, 0);
        tm_job_done(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
}
static void tm_get_gtk_list(void) {
    snprintf(tm.msg, sizeof tm.msg, "Getting the list from Flathub...");
    tm_job(TJ_GTKLIST, NULL, "ccos-fp flatpak remote-ls --columns=application flathub 2>/dev/null | grep '^org.gtk.Gtk3theme\\.' | sort -u");
}
static void tm_install_gtk(const char *name) {
    for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return;
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
        "ccos-fp flatpak install -y --noninteractive flathub org.gtk.Gtk3theme.%s >/dev/null 2>&1; "
        "L=$(flatpak info --show-location org.gtk.Gtk3theme.%s 2>/dev/null) && [ -d \"$L/files\" ] && "
        "mkdir -p '%s/.themes/%s/gtk-3.0' && cp -r \"$L/files/.\" '%s/.themes/%s/gtk-3.0/'",
        name, name, tm_home(), name, tm_home(), name);
    snprintf(tm.msg, sizeof tm.msg, "Installing %s...", name);
    tm_job(TJ_GTKINSTALL, name, cmd);
}
static void tm_get_index(void) {
    mkdir(TM_DIR, 0755);
    snprintf(tm.msg, sizeof tm.msg, "Getting the list of themes...");
    tm_job(TJ_INDEX, NULL, "/usr/bin/ccos-update fetch '" TM_INDEX_URL "' '" TM_DIR "/.index.txt'");
}
static void tm_fetch(const char *file) {
    for (const char *p = file; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return;
    mkdir(TM_DIR, 0755);
    char cmd[600]; snprintf(cmd, sizeof cmd, "/usr/bin/ccos-update fetch '" TM_FILE_URL "%s' '" TM_DIR "/%s'", file, file);
    snprintf(tm.msg, sizeof tm.msg, "Downloading...");
    tm_job(TJ_FETCH, file, cmd);
}

/* ---- drawing ---- */
static uint32_t tm_bg, tm_fg, tm_sub, tm_line, tm_card, tm_acc;
static void tm_colors(void) {
    const int dark = 0;   /* (in dark mode, ccos darkens windows like this one itself) */
    tm_bg = dark ? RGB(0x20, 0x21, 0x24) : RGB(255, 255, 255); tm_fg = dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28);
    tm_sub = dark ? RGB(0x9a, 0xa0, 0xaa) : RGB(0x70, 0x78, 0x84); tm_line = dark ? RGB(0x3a, 0x3c, 0x42) : RGB(0xe0, 0xe4, 0xea);
    tm_card = dark ? RGB(0x2a, 0x2b, 0x30) : RGB(0xf4, 0xf6, 0xf9); tm_acc = set_style ? TH()->accent : RGB(0x2a, 0x7c, 0xe0);
}
static int tm_hv(void) { return hot.type == HOT_CONTENT && wins[hot.a].draw == themes_draw ? hot.b : -1; }
static void tm_pill(int x, int y, int w, int h, const char *label, int on, int hv) {
    fill_round(x, y, w, h, 8, on ? tm_acc : hv ? mix(tm_card, tm_acc, 0.15f) : tm_card, 255);
    stroke_round(x, y, w, h, 8, on ? tm_acc : tm_line, 255);
    draw_text(x + (w - text_w(label)) / 2, y + (h - 16) / 2, label, on ? RGB(255, 255, 255) : tm_fg, 255);
}
/* a little picture of a theme: its background, a window, a bar */
static void tm_preview(const TmTheme *t, int x, int y, int w, int h) {
    int wi = t->wall >= 0 ? t->wall : 0;
    static uint8_t *big[16]; static int bw_[16], bh_[16];      /* the backgrounds' small pictures (144 x 81) */
    if (!big[wi]) { char p[200]; int c; snprintf(p, sizeof p, WALL_DIR "/%s-small.png", wallpapers[wi].file); big[wi] = stbi_load(p, &bw_[wi], &bh_[wi], &c, 4); }
    if (big[wi])
        for (int j = imax(y, clip_y0); j < imin(y + h, clip_y1); j++)
            for (int i = imax(x, clip_x0); i < imin(x + w, clip_x1); i++) {
                const uint8_t *q = big[wi] + ((size_t)((j - y) * bh_[wi] / h) * bw_[wi] + (i - x) * bw_[wi] / w) * 4;
                back[(size_t)j * W + i] = RGB(q[0], q[1], q[2]);
            }
    /* the theme's colours, worked out like TH() does */
    int o_dark = set_dark, o_tv = th_ver; uint32_t o_acc = set_accent; char o_over[600]; snprintf(o_over, sizeof o_over, "%s", set_tover);
    set_dark = t->dark; set_accent = t->accent; snprintf(set_tover, sizeof set_tover, "%s", t->colors); th_ver += 1000;
    Theme c = *TH();
    set_dark = o_dark; set_accent = o_acc; snprintf(set_tover, sizeof set_tover, "%s", o_over); th_ver = o_tv;
    int wx = x + w / 6, wy = y + h / 6, ww = w * 2 / 3, wh = h / 2;
    if (t->style) {
        fill_round(wx, wy, ww, wh, 4, c.title, 255); stroke_round(wx, wy, ww, wh, 4, c.border, 255);
        fill_rect(wx + 6, wy + 14, ww / 2, 4, c.text, 200); fill_rect(wx + 6, wy + 22, ww / 3, 4, c.sub, 200);
        fill_round(wx + ww - 30, wy + wh - 14, 24, 9, 3, c.accent, 255);
        fill_rect(x, y + h - 12, w, 12, c.bar, 255);
        fill_circle(x + 9, y + h - 6, 4, c.accent, 255);
    } else {                                              /* old theme files only */
        fill_round_grad(wx, wy, ww, wh, 4, RGB(0xc8, 0xdc, 0xf0), RGB(0x8c, 0xb0, 0xd8), 230);
        fill_rect(wx + 4, wy + 12, ww - 8, wh - 16, RGB(255, 255, 255), 255);
        stroke_round(wx, wy, ww, wh, 4, RGB(0x40, 0x60, 0x88), 255);
        fill_rect(x, y + h - 12, w, 12, RGB(0x1c, 0x30, 0x48), 220);
        fill_circle(x + 9, y + h - 6, 4, RGB(0x4c, 0x9c, 0xe8), 255);
    }
    const uint32_t *fc = folder_cols[t->folder >= 0 ? t->folder : 0];
    fill_round(x + 8, y + 8, 14, 10, 2, fc[2], 255);
}
#define TM_CW 176
#define TM_CH 140
static void tm_grid_xy(int i, int cw, int *x, int *y) {
    int cols = imax(1, (cw - TM_SIDE - 32) / (TM_CW + 12));
    *x = TM_SIDE + 20 + (i % cols) * (TM_CW + 12); *y = 56 + (i / cols) * (TM_CH + 12) - tm.scroll;
}
static void themes_draw(int cx, int cy, int cw, int ch) {
    tm_colors();
    int hv = tm_hv();
    fill_rect(cx, cy, cw, ch, tm_bg, 255);
    fill_rect(cx, cy, TM_SIDE, ch, tm_card, 255);
    fill_rect(cx + TM_SIDE, cy, 1, ch, tm_line, 255);
    for (int i = 0; i < TT_N; i++) {
        int ty = cy + 14 + i * 38, on = i == tm.tab;
        if (on || hv == 1000 + i) fill_round(cx + 8, ty, TM_SIDE - 16, 32, 7, on ? mix(tm_card, tm_acc, 0.22f) : mix(tm_card, tm_fg, 0.06f), 255);
        if (on) draw_text_bold(cx + 22, ty + 8, tm_tab_name[i], tm_fg); else draw_text(cx + 22, ty + 8, tm_tab_name[i], tm_fg, 255);
    }
    int X = cx + TM_SIDE + 24, Y = cy + 18;
    int ox0 = clip_x0, oy0 = clip_y0, ox1 = clip_x1, oy1 = clip_y1;
    if (tm.msg[0]) { fill_round(X - 4, cy + ch - 40, cw - TM_SIDE - 40, 28, 6, mix(tm_bg, tm_acc, 0.12f), 255); draw_text(X + 6, cy + ch - 34, tm.msg, tm_fg, 255); }
    switch (tm.tab) {
    case TT_THEMES: {
        draw_text_bold(X, Y, "Pick a theme", tm_fg);
        draw_text(X + 120, Y, "(changes everything at once; then fine-tune in the other tabs)", tm_sub, 255);
        clip_y0 = imax(clip_y0, cy + 48); clip_y1 = imin(clip_y1, cy + ch - (tm.msg[0] ? 44 : 4));
        for (int i = 0; i < tm_n; i++) {
            int x, y; tm_grid_xy(i, cw, &x, &y); x += cx; y += cy;
            if (y + TM_CH < clip_y0 || y > clip_y1) continue;
            int on = !strcmp(set_theme, tm_list[i].name);
            fill_round(x, y, TM_CW, TM_CH, 10, hv == i ? mix(tm_card, tm_acc, 0.12f) : tm_card, 255);
            stroke_round(x, y, TM_CW, TM_CH, 10, on ? tm_acc : tm_line, 255);
            if (on) stroke_round(x + 1, y + 1, TM_CW - 2, TM_CH - 2, 9, tm_acc, 255);
            dark_keep(x + 8, y + 8, TM_CW - 16, 92);
            tm_preview(&tm_list[i], x + 8, y + 8, TM_CW - 16, 92);
            char nm[48]; snprintf(nm, sizeof nm, "%s", tm_list[i].name);
            while (nm[0] && text_w(nm) > TM_CW - 16) nm[u8_prev(nm, (int)strlen(nm))] = 0;
            draw_text(x + (TM_CW - text_w(nm)) / 2, y + 110, nm, tm_fg, 255);
        }
        break; }
    case TT_WINDOWS: {
        draw_text_bold(X, Y, "Mode", tm_fg);
        tm_pill(X, Y + 26, 170, 34, "Light", !set_dark, hv == 3); tm_pill(X + 182, Y + 26, 170, 34, "Dark", set_dark, hv == 4);
        {   char gl[32]; snprintf(gl, sizeof gl, "Glass  %d%%", set_glass);
            draw_text_bold(X, Y + 84, gl, tm_fg);
            draw_text(X + 120, Y + 84, "how much of the background shows through frames, taskbar, menu", tm_sub, 255);
            int sx = X + 8, sw = TM_GL_W, sy = Y + 124;          /* a normal slider */
            fill_round(sx, sy - 3, sw, 6, 3, tm_line, 255);
            fill_round(sx, sy - 3, sw * set_glass / 100, 6, 3, tm_acc, 255);
            fill_circle(sx + sw * set_glass / 100.0f, sy, hv == 40 || tm_gl_drag ? 11 : 10, RGB(255, 255, 255), 255);
            stroke_round(sx + sw * set_glass / 100 - 10, sy - 10, 20, 20, 10, RGB(0x90, 0x98, 0xa4), 255);
            draw_text(sx, sy + 14, "Solid", tm_sub, 255);
            draw_text(sx + sw - text_w("Most glass"), sy + 14, "Most glass", tm_sub, 255);
        }
        draw_text_bold(X, Y + 168, "Accent colour", tm_fg);
        draw_text(X + 130, Y + 168, "buttons, highlights, the window you're in", tm_sub, 255);
        for (int i = 0; i <= 10; i++) {
            int x = X + i * 46, y = Y + 196, on = i == 0 ? !set_accent : set_accent == (0x1000000u | tm_accents[i - 1]);
            if (on) { fill_circle(x + 18, y + 18, 20, tm_fg, 255); fill_circle(x + 18, y + 18, 18, tm_bg, 255); }
            if (i == 0) { fill_circle(x + 18, y + 18, 15, set_dark ? th_dark.accent : th_light.accent, 255); draw_text(x + 14, y + 10, "A", RGB(255, 255, 255), 255); }
            else fill_circle(x + 18, y + 18, hv == 10 + i ? 16 : 15, tm_accents[i - 1], 255);
        }
        draw_text(X, Y + 242, "A = the style's own blue", tm_sub, 255);
        break; }
    case TT_TASKBAR: {
        draw_text_bold(X, Y, "Taskbar", tm_fg);
        tm_pill(X, Y + 26, 170, 34, "Bar", !set_dock, hv == 1); tm_pill(X + 182, Y + 26, 170, 34, "Dock", set_dock, hv == 2);
        draw_text_bold(X, Y + 84, "Dock size", tm_fg);
        static const char *sz[3] = { "Small", "Medium", "Large" };
        for (int i = 0; i < 3; i++) tm_pill(X + i * 122, Y + 110, 110, 34, sz[i], set_dock_size == i, hv == 3 + i);
        draw_text_bold(X, Y + 170, "Window animations", tm_fg);
        draw_text(X, Y + 192, "Windows pour into the Dock, grow in, fade out", tm_sub, 255);
        st_switch(X + 420, Y + 176, set_anim);
        break; }
    case TT_ICONS: {
        draw_text_bold(X, Y, "Folder colour", tm_fg);
        draw_text(X, Y + 22, "Documents, folders on the desktop and in File Explorer", tm_sub, 255);
        int of = set_folder;
        for (int i = 0; i < 8; i++) {
            int x = X + (i % 4) * 140, y = Y + 54 + (i / 4) * 110, on = i == of;
            fill_round(x, y, 128, 98, 10, hv == 10 + i ? mix(tm_card, tm_acc, 0.12f) : tm_card, 255);
            stroke_round(x, y, 128, 98, 10, on ? tm_acc : tm_line, 255);
            set_folder = i; dark_keep(x + 34, y + 10, 60, 50); draw_folder_icon(x + 64, y + 38, 26); set_folder = of;
            char b[16]; snprintf(b, sizeof b, "%s", folder_names[i]); b[0] = (char)toupper((unsigned char)b[0]);
            draw_text(x + (128 - text_w(b)) / 2, y + 72, b, tm_fg, 255);
        }
        break; }
    case TT_MOUSE: {
        draw_text_bold(X, Y, "Pointer colour", tm_fg);
        for (int i = 0; i < 8; i++) {
            int x = X + (i % 4) * 140, y = Y + 30 + (i / 4) * 92, on = i == set_cursor_col;
            fill_round(x, y, 128, 80, 10, hv == 10 + i ? mix(tm_card, tm_acc, 0.12f) : RGB(0x9c, 0xa8, 0xb8), 255);
            stroke_round(x, y, 128, 80, 10, on ? tm_acc : tm_line, 255);
            if (on) stroke_round(x + 1, y + 1, 126, 78, 9, tm_acc, 255);
            dark_keep(x, y, 128, 80);
            render_cursor(CUR_ARROW, i, 1.4f, x + 52, y + 14, put_back);
            char b[16]; snprintf(b, sizeof b, "%s", cursor_names[i]); b[0] = (char)toupper((unsigned char)b[0]);
            draw_text(x + 8, y + 58, b, RGB(0x10, 0x14, 0x1a), 255);
        }
        draw_text_bold(X, Y + 222, "Size", tm_fg);
        static const char *sz[3] = { "Normal", "Large", "Extra large" };
        for (int i = 0; i < 3; i++) tm_pill(X + i * 142, Y + 248, 130, 34, sz[i], set_cursor_size == i, hv == 30 + i);
        break; }
    case TT_APPS: {
        draw_text_bold(X, Y, "How apps from Flathub look", tm_fg);
        draw_text(X, Y + 22, "Firefox, Calculator... (apps you open after changing it)", tm_sub, 255);
        int y = Y + 50;
        struct { const char *label, *val; } fixed[2] = { { "Default", "" }, { "Dark", "dark" } };
        for (int i = 0; i < 2; i++) tm_pill(X + i * 142, y, 130, 34, fixed[i].label, !strcmp(set_gtk, fixed[i].val), hv == 1 + i);
        tm_scan_gtk_installed();
        for (int i = 0; i < tm.ngtk_inst && i < 12; i++) {
            int k = i + 2, x = X + (k % 4) * 142, yy = y + (k / 4) * 44;
            char b[24]; snprintf(b, sizeof b, "%.15s", tm.gtk_inst[i]);
            tm_pill(x, yy, 130, 34, b, !strcmp(set_gtk, tm.gtk_inst[i]), hv == 3 + i);
        }
        int ly = y + ((tm.ngtk_inst + 2 + 3) / 4) * 44 + 16;
        draw_text_bold(X, ly, "Get app themes from Flathub", tm_fg);
        if (!tm.gtk_listed) {
            tm_pill(X, ly + 26, 200, 34, tm.job == TJ_GTKLIST ? "Looking..." : tm.msg[0] == 'C' ? "Try again" : "Show themes", 0, hv == 200);
            break;
        }
        clip_y0 = imax(clip_y0, cy + ly - Y + 18 + 22); clip_y1 = imin(clip_y1, cy + ch - (tm.msg[0] ? 44 : 4));
        int cols = 3, bw = (cw - TM_SIDE - 60) / cols;
        for (int i = 0; i < tm.ngtk_avail; i++) {
            int x = X + (i % cols) * bw, yy = ly + 28 + (i / cols) * 34 - tm.scroll;
            if (yy + 30 < clip_y0 || yy > clip_y1) continue;
            int have = 0; for (int k = 0; k < tm.ngtk_inst; k++) if (!strcmp(tm.gtk_inst[k], tm.gtk_avail[i])) have = 1;
            fill_round(x, yy, bw - 8, 30, 6, hv == 300 + i ? mix(tm_card, tm_acc, 0.15f) : tm_card, 255);
            char b[48]; snprintf(b, sizeof b, "%s", tm.gtk_avail[i]);
            while (b[0] && text_w(b) > bw - 70) b[u8_prev(b, (int)strlen(b))] = 0;
            draw_text(x + 8, yy + 7, b, tm_fg, 255);
            draw_text(x + bw - 8 - 8 - text_w(have ? "Got" : "Get"), yy + 7, have ? "Got" : "Get", have ? tm_sub : tm_acc, 255);
        }
        break; }
    case TT_MORE: {
        draw_text_bold(X, Y, "More ccos themes", tm_fg);
        draw_text(X, Y + 22, "Made for ccos, downloaded from ccos's GitHub", tm_sub, 255);
        if (!tm.online_listed) { tm_pill(X, Y + 54, 200, 34, tm.job == TJ_INDEX ? "Looking..." : "Show themes", 0, hv == 200); break; }
        for (int i = 0; i < tm.nonline; i++) {
            int y = Y + 54 + i * 58 - tm.scroll;
            if (y + 50 < cy + 48 || y > cy + ch - 44) continue;
            int have = 0; char p[300]; snprintf(p, sizeof p, TM_DIR "/%s", tm.online[i].file); have = access(p, F_OK) == 0;
            fill_round(X, y, cw - TM_SIDE - 48, 50, 8, tm_card, 255);
            draw_text_bold(X + 14, y + 8, tm.online[i].name, tm_fg);
            draw_text(X + 14, y + 27, tm.online[i].desc, tm_sub, 255);
            tm_pill(X + cw - TM_SIDE - 48 - 112, y + 9, 100, 32, have ? "Use" : "Get", 0, hv == 300 + i);
        }
        break; }
    }
    clip_x0 = ox0; clip_y0 = oy0; clip_x1 = ox1; clip_y1 = oy1;
}
static int in_r(int lx, int ly, int x, int y, int w, int h) { return lx >= x && ly >= y && lx < x + w && ly < y + h; }
static int themes_hit(int lx, int ly, int cw, int ch) {
    if (lx < TM_SIDE) { int i = (ly - 14) / 38; return ly >= 14 && i < TT_N && (ly - 14) % 38 < 32 ? 1000 + i : -1; }
    int X = TM_SIDE + 24, Y = 18;
    switch (tm.tab) {
    case TT_THEMES:
        if (ly < 48 || ly > ch - 44) return -1;
        for (int i = 0; i < tm_n; i++) { int x, y; tm_grid_xy(i, cw, &x, &y); if (in_r(lx, ly, x, y, TM_CW, TM_CH)) return i; }
        return -1;
    case TT_WINDOWS:
        if (in_r(lx, ly, X, Y + 26, 170, 34)) return 3;
        if (in_r(lx, ly, X + 182, Y + 26, 170, 34)) return 4;
        if (in_r(lx, ly, X - 6, Y + 110, TM_GL_W + 28, 30)) return 40;
        for (int i = 0; i <= 10; i++) if (in_r(lx, ly, X + i * 46, Y + 196, 38, 38)) return 10 + i;
        return -1;
    case TT_TASKBAR:
        if (in_r(lx, ly, X, Y + 26, 170, 34)) return 1;
        if (in_r(lx, ly, X + 182, Y + 26, 170, 34)) return 2;
        for (int i = 0; i < 3; i++) if (in_r(lx, ly, X + i * 122, Y + 110, 110, 34)) return 3 + i;
        if (in_r(lx, ly, X, Y + 166, 480, 44)) return 6;
        return -1;
    case TT_ICONS:
        for (int i = 0; i < 8; i++) if (in_r(lx, ly, X + (i % 4) * 140, Y + 54 + (i / 4) * 110, 128, 98)) return 10 + i;
        return -1;
    case TT_MOUSE:
        for (int i = 0; i < 8; i++) if (in_r(lx, ly, X + (i % 4) * 140, Y + 30 + (i / 4) * 92, 128, 80)) return 10 + i;
        for (int i = 0; i < 3; i++) if (in_r(lx, ly, X + i * 142, Y + 248, 130, 34)) return 30 + i;
        return -1;
    case TT_APPS: {
        int y = Y + 50;
        for (int i = 0; i < 2; i++) if (in_r(lx, ly, X + i * 142, y, 130, 34)) return 1 + i;
        for (int i = 0; i < tm.ngtk_inst && i < 12; i++) { int k = i + 2; if (in_r(lx, ly, X + (k % 4) * 142, y + (k / 4) * 44, 130, 34)) return 3 + i; }
        int ly0 = y + ((tm.ngtk_inst + 2 + 3) / 4) * 44 + 16;
        if (!tm.gtk_listed) return in_r(lx, ly, X, ly0 + 26, 200, 34) ? 200 : -1;
        if (ly < ly0 + 40 || ly > ch - 44) return -1;
        int cols = 3, bw = (cw - TM_SIDE - 60) / cols;
        for (int i = 0; i < tm.ngtk_avail; i++) if (in_r(lx, ly, X + (i % cols) * bw, ly0 + 28 + (i / cols) * 34 - tm.scroll, bw - 8, 30)) return 300 + i;
        return -1; }
    case TT_MORE:
        if (!tm.online_listed) return in_r(lx, ly, X, Y + 54, 200, 34) ? 200 : -1;
        for (int i = 0; i < tm.nonline; i++) if (in_r(lx, ly, X + cw - TM_SIDE - 48 - 112, Y + 54 + i * 58 - tm.scroll + 9, 100, 32)) return 300 + i;
        return -1;
    }
    return -1;
}
static void themes_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(themes_draw), cx = 0, cy = 0, cw = TM_W, ch = TM_H;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (id >= 1000) { *x = cx; *y = cy + 14 + (id - 1000) * 38; *w = TM_SIDE; *h = 32; return; }
    *x = cx + TM_SIDE; *y = cy; *w = cw - TM_SIDE; *h = ch;        /* the page */
}
static void themes_drag(int lx, int ly, int cw, int ch) {      /* moving the glass slider */
    (void)ly; (void)cw; (void)ch;
    if (!tm_gl_drag || tm.tab != TT_WINDOWS) return;
    int v = clampi((lx - (TM_SIDE + 24 + 8)) * 100 / TM_GL_W, 0, 100);
    if (v != set_glass) { set_glass = v; th_ver++; repaint_all(); }
}
static void themes_release(void) { if (tm_gl_drag) { tm_gl_drag = 0; save_settings(); repaint_all(); } }
static void themes_click(int lx, int ly, int cw, int ch) {
    int id = themes_hit(lx, ly, cw, ch);
    if (id < 0) return;
    if (id >= 1000) { tm.tab = id - 1000; tm.scroll = 0; tm.msg[0] = 0; if (tm.tab == TT_THEMES) tm_load_list(); tm_repaint(); return; }
    switch (tm.tab) {
    case TT_THEMES: if (id < tm_n) tm_apply(&tm_list[id]); break;
    case TT_WINDOWS:
        if (id == 3 || id == 4) { tm_custom(); style_set(1, id == 4); }
        else if (id == 40) { tm_gl_drag = 1; themes_drag(lx, ly, cw, ch); }
        else if (id >= 10 && id <= 20) { tm_custom(); set_accent = id == 10 ? 0 : 0x1000000u | tm_accents[id - 11]; tm_changed(); }
        break;
    case TT_TASKBAR:
        if (id == 1 || id == 2) dock_set(id == 2);
        else if (id >= 3 && id <= 5) { set_dock_size = id - 3; for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].maximized && !wins[i].fullscreen) { toggle_max(i); toggle_max(i); } tm_changed(); }
        else if (id == 6) { set_anim = !set_anim; save_settings(); }
        break;
    case TT_ICONS: if (id >= 10 && id < 18) { tm_custom(); set_folder = id - 10; tm_changed(); } break;
    case TT_MOUSE:
        if (id >= 10 && id < 18) { tm_custom(); set_cursor_col = id - 10; tm_changed(); }
        else if (id >= 30 && id < 33) { set_cursor_size = id - 30; tm_changed(); }
        break;
    case TT_APPS:
        if (id == 1 || id == 2) { snprintf(set_gtk, sizeof set_gtk, "%s", id == 1 ? "" : "dark"); tm_custom(); save_settings();
            snprintf(tm.msg, sizeof tm.msg, "Apps you open from now on look like this."); }
        else if (id >= 3 && id < 15) { snprintf(set_gtk, sizeof set_gtk, "%s", tm.gtk_inst[id - 3]); tm_custom(); save_settings();
            snprintf(tm.msg, sizeof tm.msg, "Apps you open from now on use %s.", set_gtk); }
        else if (id == 200) tm_get_gtk_list();
        else if (id >= 300 && id - 300 < tm.ngtk_avail) {
            const char *n = tm.gtk_avail[id - 300]; int have = 0;
            for (int k = 0; k < tm.ngtk_inst; k++) if (!strcmp(tm.gtk_inst[k], n)) have = 1;
            if (have) { snprintf(set_gtk, sizeof set_gtk, "%s", n); tm_custom(); save_settings(); snprintf(tm.msg, sizeof tm.msg, "Apps you open from now on use %s.", n); }
            else if (!tm.job) tm_install_gtk(n);
        }
        break;
    case TT_MORE:
        if (id == 200) tm_get_index();
        else if (id >= 300 && id - 300 < tm.nonline && !tm.job) {
            char p[300]; snprintf(p, sizeof p, TM_DIR "/%s", tm.online[id - 300].file);
            if (access(p, F_OK) == 0) { tm_load_list(); for (int i = 0; i < tm_n; i++) if (!strcmp(tm_list[i].file, p)) tm_apply(&tm_list[i]); }
            else tm_fetch(tm.online[id - 300].file);
        }
        break;
    }
    tm_repaint();
}
static void themes_wheel(int amount) {
    tm.scroll = imax(0, tm.scroll - amount * 50);
    int lim = tm.tab == TT_THEMES ? ((tm_n + 2) / 3) * (TM_CH + 12) : tm.tab == TT_APPS ? ((tm.ngtk_avail + 2) / 3) * 34 : tm.tab == TT_MORE ? tm.nonline * 58 : 0;
    tm.scroll = imin(tm.scroll, imax(0, lim - 300));
    tm_repaint(); update_hover();
}
static int themes_on_close(void) { return 1; }
static void themes_open(void) {
    tm_load_list(); tm_scan_gtk_installed();
    static const Win t = { .title = "Themes", .icon = ICON_THEMES, .draw = themes_draw, .click = themes_click, .drag = themes_drag, .release = themes_release, .hit = themes_hit,
                           .elem_rect = themes_elem_rect, .wheel = themes_wheel, .fd = tm_fd, .wake = tm_wake, .on_close = themes_on_close };
    open_win(&t, imin(W - 40, TM_W), imin(H - TASKBAR_H - 40, TM_H));
}
/* the icon: a paint brush */
static void draw_themes_icon(int cx, int cy, int r) {
    float s = r / 10.0f;
    aa_line(cx + 7 * s, cy - 8 * s, cx - 1 * s, cy + 1 * s, fmaxf(2.0f, 3.2f * s), RGB(0x8a, 0x5a, 0x30));       /* handle */
    aa_line(cx + 2.5f * s, cy - 3 * s, cx + 0.5f * s, cy - 0.5f * s, fmaxf(2.0f, 3.6f * s), RGB(0xb8, 0xbc, 0xc4)); /* metal */
    fill_circle(cx - 3 * s, cy + 3.5f * s, 3.8f * s, RGB(0xe0, 0x3c, 0x6c), 255);                                  /* paint */
    fill_circle(cx - 6 * s, cy + 7 * s, 2.2f * s, RGB(0xe0, 0x3c, 0x6c), 255);
    fill_circle(cx - 4 * s, cy + 2 * s, 1.4f * s, RGB(255, 255, 255), 140);
}
