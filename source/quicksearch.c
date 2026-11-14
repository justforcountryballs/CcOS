/* ===================================================================
 *  Quick Search: Windows + Space, like a Mac's Spotlight. A bar in the
 *  middle of the screen. Type and press Enter:
 *    - something like a web address (https://..., youtu.be, google.com)
 *      opens that site in your browser
 *    - anything else is searched for online, in your browser
 *  Apps, Settings pages and files that match show underneath too.
 *  The first time it asks which browser to use (and which search site).
 * =================================================================== */
#define QS_W 660
#define QS_BAR 58
#define QS_ROW 40
#define QS_MAXR 7
static struct { int open, sel, setup, nb; char q[200]; char br_id[8][96], br_name[8][40]; int br_icon[8]; } qs;
/* set_browser: the app id ("org.mozilla.firefox"), "" = not chosen yet; set_engine: 0 Google, 1 DuckDuckGo, 2 Bing */
static const char *qs_engines[3] = { "Google", "DuckDuckGo", "Bing" };
static const char *qs_engine_url[3] = { "https://www.google.com/search?q=", "https://duckduckgo.com/?q=", "https://www.bing.com/search?q=" };

static int qs_y(void) { return H / 5; }
static int qs_x(void) { return (W - QS_W) / 2; }
static int qs_nrows(void);
static int qs_h(void) {
    if (qs.setup) return 92 + imax(1, qs.nb) * 50 + 76;
    return QS_BAR + (qs.q[0] ? 8 + qs_nrows() * QS_ROW + 6 : 0);
}
static void qs_repaint(void) { repaint(qs_x() - 6, qs_y() - 6, QS_W + 16, imin(H - qs_y() + 6, qs_h() + 40 + QS_MAXR * QS_ROW)); }

/* which browsers are installed (from Flathub) */
static void qs_find_browsers(void) {
    static const char *known[] = { "org.mozilla.firefox", "com.google.Chrome", "org.chromium.Chromium", "com.brave.Browser",
        "com.microsoft.Edge", "io.gitlab.librewolf-community", "com.opera.Opera", "com.vivaldi.Vivaldi", "app.zen_browser.zen",
        "org.gnome.Epiphany", "io.github.ungoogled_software.ungoogled_chromium", "org.torproject.torbrowser-launcher" };
    qs.nb = 0;
    for (int i = 0; i < nfp && qs.nb < 8; i++) {
        const char *id = fp_info[i].cmd + 8;
        int is = 0;
        for (size_t k = 0; k < sizeof known / sizeof known[0]; k++) if (!strcmp(id, known[k])) is = 1;
        if (!is) {                                             /* or its .desktop file says so */
            char p[400]; snprintf(p, sizeof p, "%s/%s.desktop", FP_DIR, id);
            FILE *f = fopen(p, "r"); char l[512];
            if (f) { while (fgets(l, sizeof l, f)) if (!strncmp(l, "Categories=", 11) && strstr(l, "WebBrowser")) is = 1; fclose(f); }
        }
        if (!is) continue;
        snprintf(qs.br_id[qs.nb], sizeof qs.br_id[0], "%s", id);
        snprintf(qs.br_name[qs.nb], sizeof qs.br_name[0], "%s", fp_info[i].label);
        qs.br_icon[qs.nb] = fp_info[i].icon;
        qs.nb++;
    }
}
static int qs_browser_icon(void) {
    for (int i = 0; i < nfp; i++) if (!strcmp(fp_info[i].cmd + 8, set_browser)) return fp_info[i].icon;
    return ICON_APP;
}
static const char *qs_browser_name(void) {
    for (int i = 0; i < nfp; i++) if (!strcmp(fp_info[i].cmd + 8, set_browser)) return fp_info[i].label;
    return set_browser;
}
static int qs_browser_ok(void) {
    if (!set_browser[0]) return 0;
    for (int i = 0; i < nfp; i++) if (!strcmp(fp_info[i].cmd + 8, set_browser)) return 1;
    return 0;                                                  /* (it was uninstalled) */
}

/* does it look like a web address? */
static int qs_is_url(const char *s) {
    if (!strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8)) return 1;
    if (strchr(s, ' ') || strchr(s, '@')) return 0;
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (hl < 4) return 0;
    if (!strncasecmp(s, "localhost", 9)) return 1;
    const char *dot = memchr(s, '.', hl);
    if (!dot || dot == s) return 0;
    const char *last = s + hl - 1; while (last > s && *last != '.') last--;     /* the ending: letters, 2+ */
    size_t tl = (size_t)(s + hl - last - 1);
    if (tl < 2) return 0;
    for (const char *p = last + 1; p < s + hl; p++) if (!isalpha((unsigned char)*p)) {
        for (const char *q = s; q < s + hl; q++) if (!isdigit((unsigned char)*q) && *q != '.' && *q != ':') return 0;   /* an IP address */
        return 1;
    }
    return 1;
}
static void qs_launch_url(const char *url) {
    char *const argv[] = { "ccos-flatpak-run", set_browser, (char *)url, NULL };
    wl_spawn("/usr/bin/ccos-flatpak-run", argv);
}
static void qs_go_web(void) {
    char url[900];
    if (qs_is_url(qs.q)) {
        if (!strncasecmp(qs.q, "http://", 7) || !strncasecmp(qs.q, "https://", 8)) snprintf(url, sizeof url, "%s", qs.q);
        else snprintf(url, sizeof url, "https://%s", qs.q);
    } else {                                                   /* a search: the words, made safe for an address */
        size_t n = (size_t)snprintf(url, sizeof url, "%s", qs_engine_url[set_engine]);
        for (const unsigned char *p = (const unsigned char *)qs.q; *p && n + 4 < sizeof url; p++) {
            if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') url[n++] = (char)*p;
            else if (*p == ' ') url[n++] = '+';
            else n += (size_t)snprintf(url + n, sizeof url - n, "%%%02X", *p);
        }
        url[n] = 0;
    }
    qs_launch_url(url);
}

static int qs_nrows(void) { return imin(QS_MAXR, 1 + ms_n); }
static void qs_set(int open) {
    if (open == qs.open) return;
    if (open) {
        set_menu(0); pop_close(); ch_set(0); nc_set(0);
        qs.q[0] = 0; qs.sel = 0;
        if (!ms_nfiles) ms_scan_files();
        qs.setup = !qs_browser_ok();
        if (qs.setup) qs_find_browsers();
        qs.open = 1; qs_repaint();
    } else {
        qs.open = 0; qs_repaint();
        ms_q[0] = 0; ms_n = 0;
    }
}
static void qs_update(void) {
    snprintf(ms_q, sizeof ms_q, "%.*s", (int)sizeof ms_q - 1, qs.q);
    ms_update();                                               /* apps, Settings, files: the Start menu's search */
    qs.sel = 0;
}
static void qs_open_row(int r) {
    if (r == 0) { qs_go_web(); qs_set(0); return; }
    int i = r - 1;
    if (i < 0 || i >= ms_n) return;
    char cmd[300]; snprintf(cmd, sizeof cmd, "%s", ms_res[i].cmd);
    int kind = ms_res[i].kind, tab = ms_res[i].tab;
    qs_set(0);
    if (kind == MSR_APP) run(cmd);
    else if (kind == MSR_SET) { st_tab = tab; run("@settings"); }
    else open_file(cmd);
}

static void qs_draw_box(int x, int y, int w, int h) {
    const int dark = set_style && set_dark;
    fill_round(x + 2, y + 8, w, h, 16, RGB(0, 0, 0), 60);
    fill_round(x, y, w, h, 16, dark ? RGB(0x26, 0x28, 0x2e) : RGB(0xf6, 0xf7, 0xf9), 240);
    stroke_round(x, y, w, h, 16, dark ? RGB(0x54, 0x58, 0x60) : RGB(0xc6, 0xcc, 0xd4), 255);
}
static void draw_quicksearch(void) {
    if (!qs.open) return;
    int x = qs_x(), y = qs_y(), w = QS_W, h = qs_h();
    if (x >= clip_x1 || x + w + 4 <= clip_x0 || y >= clip_y1 || y + h + 10 <= clip_y0) return;
    const int dark = set_style && set_dark;
    uint32_t fg = dark ? RGB(0xf2, 0xf4, 0xf7) : RGB(0x1e, 0x22, 0x28), sub = dark ? RGB(0xa0, 0xa6, 0xb0) : RGB(0x74, 0x7c, 0x88);
    uint32_t acc = set_style ? TH()->accent : RGB(0x2a, 0x7c, 0xe0);
    qs_draw_box(x, y, w, h);
    int hv = hot.type == HOT_QS ? hot.a : -1;
    if (qs.setup) {
        draw_text_bold(x + 24, y + 22, "Select a browser to use Quick Search.", fg);
        draw_text(x + 24, y + 46, "What you type is searched for (or opened) in it.", sub, 255);
        if (!qs.nb) {
            draw_text(x + 24, y + 100, "No browser is installed yet.", fg, 255);
            int on = hv == 50;
            fill_round(x + 24, y + 128, 260, 34, 8, on ? RGB(0x16, 0x68, 0xd0) : acc, 255);
            draw_text(x + 40, y + 137, "Get Firefox in Software Manager", RGB(255, 255, 255), 255);
            return;
        }
        for (int i = 0; i < qs.nb; i++) {
            int ry = y + 80 + i * 50, on = hv == i, cur = !strcmp(set_browser, qs.br_id[i]);
            fill_round(x + 16, ry, w - 32, 44, 10, on ? (dark ? RGB(0x34, 0x4a, 0x6c) : RGB(0xdc, 0xea, 0xfb)) : (dark ? RGB(0x30, 0x32, 0x38) : RGB(255, 255, 255)), 255);
            if (cur) stroke_round(x + 16, ry, w - 32, 44, 10, acc, 255);
            draw_icon(qs.br_icon[i], x + 40, ry + 22, 12);
            draw_text_bold(x + 64, ry + 13, qs.br_name[i], fg);
        }
        int ey = y + 80 + qs.nb * 50 + 12;
        draw_text(x + 24, ey + 8, "Search with:", sub, 255);
        for (int k = 0; k < 3; k++) {
            int bx = x + 130 + k * 120, on = hv == 20 + k;
            fill_round(bx, ey, 110, 32, 8, set_engine == k ? acc : on ? (dark ? RGB(0x3a, 0x3e, 0x46) : RGB(0xe4, 0xe8, 0xee)) : (dark ? RGB(0x30, 0x32, 0x38) : RGB(255, 255, 255)), 255);
            stroke_round(bx, ey, 110, 32, 8, dark ? RGB(0x54, 0x58, 0x60) : RGB(0xc6, 0xcc, 0xd4), 255);
            draw_text(bx + (110 - text_w(qs_engines[k])) / 2, ey + 8, qs_engines[k], set_engine == k ? RGB(255, 255, 255) : fg, 255);
        }
        return;
    }
    /* the bar */
    draw_icon(ICON_SEARCH, x + 30, y + QS_BAR / 2, 11);
    if (qs.q[0]) {
        char q[200]; snprintf(q, sizeof q, "%s", qs.q);
        while (q[0] && text_w(q) * 2 > w - 240) memmove(q, q + u8_next(q, (int)strlen(q), 0), strlen(q));
        draw_text_scaled(x + 54, y + QS_BAR / 2 - 16, q, fg, 2);
        fill_rect(x + 56 + text_w(q) * 2, y + QS_BAR / 2 - 15, 2, 30, acc, 255);
    } else {
        draw_text_scaled(x + 58, y + QS_BAR / 2 - 16, "Quick Search", sub, 2);
        fill_rect(x + 54, y + QS_BAR / 2 - 15, 2, 30, acc, 255);
    }
    {   char b[60]; snprintf(b, sizeof b, "%s", qs_browser_name());           /* the browser: click to change */
        int bw = text_w(b) + 20, bx = x + w - bw - 14, on = hv == 40;
        if (on) fill_round(bx, y + 16, bw, 26, 6, dark ? RGB(0x3a, 0x3e, 0x46) : RGB(0xe4, 0xe8, 0xee), 255);
        draw_text(bx + 10, y + 21, b, sub, 255); }
    if (!qs.q[0]) return;
    fill_rect(x + 14, y + QS_BAR, w - 28, 1, dark ? RGB(0x44, 0x48, 0x50) : RGB(0xdc, 0xe0, 0xe6), 255);
    int n = qs_nrows();
    for (int r = 0; r < n; r++) {
        int ry = y + QS_BAR + 8 + r * QS_ROW, on = (hv == 100 + r) || (hv < 100 && r == qs.sel);
        if (on) fill_round(x + 10, ry, w - 20, QS_ROW - 4, 8, acc, 255);
        uint32_t c = on ? RGB(255, 255, 255) : fg, c2 = on ? RGB(0xe0, 0xec, 0xff) : sub;
        char l[220], kind[24];
        if (r == 0) {
            int url = qs_is_url(qs.q);
            char q[120]; snprintf(q, sizeof q, "%s", qs.q);
            while (q[0] && text_w(q) > w - 300) q[u8_prev(q, (int)strlen(q))] = 0;
            snprintf(l, sizeof l, url ? "Open %s" : "Search %s for \"%s\"", url ? q : qs_engines[set_engine], q);
            snprintf(kind, sizeof kind, "%s", qs_browser_name());
            draw_icon(qs_browser_icon(), x + 34, ry + (QS_ROW - 4) / 2, 10);
        } else {
            int i = r - 1;
            snprintf(l, sizeof l, "%s", ms_res[i].label);
            while (l[0] && text_w(l) > w - 200) l[u8_prev(l, (int)strlen(l))] = 0;
            snprintf(kind, sizeof kind, "%s", ms_res[i].kind == MSR_APP ? "App" : ms_res[i].kind == MSR_SET ? "Settings" : ms_res[i].kind == MSR_DIR ? "Folder" : "File");
            draw_icon(ms_res[i].icon, x + 34, ry + (QS_ROW - 4) / 2, 10);
        }
        draw_text(x + 56, ry + 10, l, c, 255);
        draw_text(x + w - 24 - text_w(kind), ry + 10, kind, c2, 255);
    }
}
/* what's under the mouse: 0..7 a browser, 20..22 a search site, 40 the browser button, 50 Get Firefox, 100+r a row; -2 the box, -1 outside */
static int qs_hit(void) {
    int x = qs_x(), y = qs_y(), w = QS_W;
    if (!in_rect(x, y, w, qs_h())) return -1;
    if (qs.setup) {
        if (!qs.nb) return in_rect(x + 24, y + 128, 260, 34) ? 50 : -2;
        for (int i = 0; i < qs.nb; i++) if (in_rect(x + 16, y + 80 + i * 50, w - 32, 44)) return i;
        int ey = y + 80 + qs.nb * 50 + 12;
        for (int k = 0; k < 3; k++) if (in_rect(x + 130 + k * 120, ey, 110, 32)) return 20 + k;
        return -2;
    }
    if (my < y + QS_BAR) return mx > x + w - 200 ? 40 : -2;
    int r = (my - y - QS_BAR - 8) / QS_ROW;
    if (qs.q[0] && my >= y + QS_BAR + 8 && r < qs_nrows()) return 100 + r;
    return -2;
}
static void qs_click(int id) {
    if (qs.setup) {
        if (id == 50) { qs_set(0); run("@store"); return; }
        if (id >= 0 && id < qs.nb) { snprintf(set_browser, sizeof set_browser, "%s", qs.br_id[id]); save_settings(); qs.setup = 0; qs_repaint(); return; }
        if (id >= 20 && id < 23) { set_engine = id - 20; save_settings(); qs_repaint(); return; }
        return;
    }
    if (id == 40) { qs_find_browsers(); qs.setup = 1; qs_repaint(); return; }
    if (id >= 100) qs_open_row(id - 100);
}
static void qs_key(int code, int mods) {
    if (code == KEY_ESC) { if (qs.q[0] && !qs.setup) { qs.q[0] = 0; qs_update(); qs_repaint(); } else qs_set(0); return; }
    if (qs.setup) {
        if (code == KEY_ENTER && qs.nb == 1) qs_click(0);
        return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) { if (qs.q[0]) qs_open_row(qs.sel); return; }
    if (code == KEY_DOWN) { if (qs.sel < qs_nrows() - 1) qs.sel++; qs_repaint(); return; }
    if (code == KEY_UP) { if (qs.sel > 0) qs.sel--; qs_repaint(); return; }
    if (code == KEY_BACKSPACE) {
        int l = (int)strlen(qs.q);
        if (l) { qs_repaint(); qs.q[u8_prev(qs.q, l)] = 0; qs_update(); qs_repaint(); }
        return;
    }
    if ((mods & MOD_CTRL) && code == KEY_V && clipboard) {      /* paste an address */
        size_t l = strlen(qs.q);
        for (int i = 0; i < clip_len && l + 1 < sizeof qs.q; i++) if (clipboard[i] != '\n' && clipboard[i] != '\r') qs.q[l++] = clipboard[i];
        qs.q[l] = 0; qs_update(); qs_repaint(); return;
    }
    char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
    if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
    size_t l = strlen(qs.q);
    if (l + (size_t)k < sizeof qs.q) { memcpy(qs.q + l, t, (size_t)k); qs.q[l + (size_t)k] = 0; }
    qs_update(); qs_repaint();
}
