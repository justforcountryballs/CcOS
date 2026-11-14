/* ===================================================================
 *  Start menu search: open the Start menu and type.
 *  Finds apps (built-in and Flathub), Settings pages, and files/folders
 *  on D: and in Downloads. Enter opens the top (or picked) one;
 *  Up/Down pick; Esc clears, then closes.
 * =================================================================== */
#define MS_MAXRES 16
enum { MSR_APP, MSR_SET, MSR_FILE, MSR_DIR };
static char ms_q[64];
static struct { int kind, icon, tab; char label[64], cmd[300]; } ms_res[MS_MAXRES];
static char **ms_files; static int ms_nfiles;           /* everything on D: and in Downloads, read when the menu opens */

static const struct { const char *name, *words; int tab; } ms_pages[] = {
    { "Appearance", "background wallpaper theme dark mode style modern classic colour color desktop", TAB_LOOK },
    { "Clock and time zone", "time date zone clock internet ntp 24 hour calendar", TAB_CLOCK },
    { "Sound", "volume audio speakers headphones mute notification chime", TAB_SOUND },
    { "Keyboard & mouse", "keyboard layout polish english mouse touchpad pointer cursor speed", TAB_MOUSE },
    { "Power", "battery brightness sleep lid screen off power saving", TAB_POWER },
    { "Bluetooth", "bluetooth headphones speaker pair devices", TAB_BT },
    { "Update", "update upgrade new version github", TAB_UPDATE },
    { "Themes", "theme themes look colour color accent folder icons pointer cursor dock gtk", -1 },
    { "About", "about version system graphics gpu chip storage save disk", TAB_ABOUT },
};

static int ms_match(const char *hay, const char *q) {         /* 2: starts with it, 1: has it, 0: no */
    if (!q[0]) return 0;
    if (!strncasecmp(hay, q, strlen(q))) return 2;
    for (const char *p = hay; *p; p++) if ((p == hay || p[-1] == ' ' || p[-1] == '-' || p[-1] == '_') && !strncasecmp(p, q, strlen(q))) return 2;
    return strcasestr(hay, q) ? 1 : 0;
}
static void ms_add(int kind, int icon, int tab, const char *label, const char *cmd) {
    if (ms_n >= MS_MAXRES) return;
    for (int i = 0; i < ms_n; i++) if (!strcmp(ms_res[i].cmd, cmd) && ms_res[i].kind == kind && ms_res[i].tab == tab) return;
    ms_res[ms_n].kind = kind; ms_res[ms_n].icon = icon; ms_res[ms_n].tab = tab;
    snprintf(ms_res[ms_n].label, sizeof ms_res[0].label, "%s", label);
    snprintf(ms_res[ms_n].cmd, sizeof ms_res[0].cmd, "%s", cmd);
    ms_n++;
}

/* the file list is read by a helper thread (a USB stick can take a while), then taken over here */
typedef struct { char **v; long long *t; int n; } MsList;
static long long *ms_mtime;                             /* when each was changed (for the Finder) */
static void ms_walk(MsList *L, const char *dir, int depth) {
    if (depth > 6 || L->n >= 4000) return;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && L->n < 4000) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lost+found")) continue;
        char p[600]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (lstat(p, &st)) continue;
        char **nv = realloc(L->v, sizeof *L->v * (size_t)(L->n + 1)); if (!nv) break;
        L->v = nv;
        long long *nt = realloc(L->t, sizeof *L->t * (size_t)(L->n + 1)); if (!nt) break;
        L->t = nt; L->t[L->n] = (long long)st.st_mtime;
        size_t l = strlen(p);
        L->v[L->n] = malloc(l + 2);
        if (!L->v[L->n]) break;
        L->v[L->n][0] = S_ISDIR(st.st_mode) ? 'd' : 'f';
        memcpy(L->v[L->n] + 1, p, l + 1);
        L->n++;
        if (S_ISDIR(st.st_mode)) ms_walk(L, p, depth + 1);
    }
    closedir(d);
}
static MsList ms_new; static volatile int ms_scanning, ms_scan_ready;
static char ms_dl[300];
static void *ms_scan_thread(void *arg) {
    (void)arg;
    MsList L = { NULL, NULL, 0 };
    ms_walk(&L, "/data", 0);
    if (strncmp(ms_dl, "/data", 5)) ms_walk(&L, ms_dl, 0);
    ms_new = L;
    __atomic_store_n(&ms_scan_ready, 1, __ATOMIC_RELEASE);
    if (wake_pipe[1] >= 0) { if (write(wake_pipe[1], "s", 1) < 0) {} }
    return NULL;
}
static void ms_scan_files(void) {
    if (ms_scanning) return;                                  /* still reading from last time */
    snprintf(ms_dl, sizeof ms_dl, "%s", dl_dir());
    ms_scanning = 1; ms_scan_ready = 0;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, ms_scan_thread, NULL)) ms_scanning = 0;
    pthread_attr_destroy(&at);
}
static void ms_update(void);
static void finder_files_changed(void);
static void ex_search_files_changed(void);
/* in the main loop: the helper thread finished - use its list */
static void ms_tick(void) {
    if (!ms_scanning || !__atomic_load_n(&ms_scan_ready, __ATOMIC_ACQUIRE)) return;
    for (int i = 0; i < ms_nfiles; i++) free(ms_files[i]);
    free(ms_files); free(ms_mtime);
    ms_files = ms_new.v; ms_mtime = ms_new.t; ms_nfiles = ms_new.n; ms_new.v = NULL; ms_new.t = NULL; ms_new.n = 0;
    ms_scanning = 0; ms_scan_ready = 0;
    finder_files_changed();
    ex_search_files_changed();
    if (menu_open && ms_q[0]) { int sel = ms_sel; ms_update(); ms_sel = imin(sel, imax(0, ms_n - 1)); repaint_menu(); }
}

static void ms_update(void) {
    ms_n = 0; ms_sel = 0;
    if (!ms_q[0]) return;
    for (int pass = 2; pass >= 1; pass--) {                     /* best matches first */
        for (int i = 0; i < (int)(sizeof apps / sizeof apps[0]); i++)
            if (ms_match(apps[i].label, ms_q) == pass) ms_add(MSR_APP, apps[i].icon, 0, apps[i].label, apps[i].cmd);
        for (int i = 0; i < nfp; i++)
            if (ms_match(fp_info[i].label, ms_q) == pass) ms_add(MSR_APP, fp_info[i].icon, 0, fp_info[i].label, fp_info[i].cmd);
    }
    for (int i = 0; i < (int)(sizeof ms_pages / sizeof ms_pages[0]); i++)
        if (ms_match(ms_pages[i].name, ms_q) || ms_match(ms_pages[i].words, ms_q)) {
            char l[64]; snprintf(l, sizeof l, "Settings: %s", ms_pages[i].name);
            if (ms_pages[i].tab < 0) ms_add(MSR_APP, ICON_THEMES, 0, "Themes", "@themes");
            else ms_add(MSR_SET, ICON_GEAR, ms_pages[i].tab, l, "@settings");
        }
    for (int pass = 2; pass >= 1; pass--)
        for (int i = 0; i < ms_nfiles && ms_n < MS_MAXRES; i++) {
            const char *p = ms_files[i] + 1, *nm = strrchr(p, '/'); nm = nm ? nm + 1 : p;
            if (ms_match(nm, ms_q) != pass) continue;
            int dir = ms_files[i][0] == 'd', mk = dir ? 0 : media_kind(nm);
            ms_add(dir ? MSR_DIR : MSR_FILE, dir ? ICON_FOLDER : mk == MK_IMAGE ? ICON_PHOTO : mk == MK_VIDEO ? ICON_VIDEO :
                   mk == MK_AUDIO ? ICON_MUSIC : ICON_FILE, 0, nm, p);
        }
}

static void msr_open(int i) {
    if (i < 0 || i >= ms_n) return;
    char cmd[300]; snprintf(cmd, sizeof cmd, "%s", ms_res[i].cmd);
    int kind = ms_res[i].kind, tab = ms_res[i].tab;
    set_menu(0);
    if (kind == MSR_APP) run(cmd);
    else if (kind == MSR_SET) { st_tab = tab; run("@settings"); }
    else open_file(cmd);                                 /* a folder opens in File Explorer */
}

/* the menu opened or closed: start over */
static void ms_reset(int open) {
    ms_q[0] = 0; ms_n = 0; ms_sel = 0;
    if (open) ms_scan_files();
}

static void ms_rows(int *n) { *n = (MS_BOX_Y - 6 - ITEMS_Y) / ITEM_H; if (*n > MS_MAXRES) *n = MS_MAXRES; }
/* typing while the menu is open */
static void menu_search_key(int code, int mods) {
    int rows; ms_rows(&rows);
    if (code == KEY_ESC) { if (ms_q[0]) { ms_reset(0); repaint_menu(); } else set_menu(0); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER) { if (ms_n) msr_open(ms_sel); return; }
    if (code == KEY_DOWN) { if (ms_n) ms_sel = imin(ms_sel + 1, imin(ms_n, rows) - 1); repaint_menu(); return; }
    if (code == KEY_UP) { if (ms_sel > 0) ms_sel--; repaint_menu(); return; }
    if (code == KEY_BACKSPACE) {
        int l = (int)strlen(ms_q);
        if (l) { ms_q[u8_prev(ms_q, l)] = 0; ms_update(); repaint_menu(); }
        return;
    }
    char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
    if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
    size_t l = strlen(ms_q);
    if (l + (size_t)k < sizeof ms_q) { memcpy(ms_q + l, t, (size_t)k); ms_q[l + (size_t)k] = 0; }
    ms_update();
    repaint_menu();
}

/* in draw_menu: the box at the bottom of the white panel, and the results over the program list */
static int ms_active(void) { return ms_q[0] != 0; }
static void ms_draw(void) {
    int bx = LI_X, by = MS_BOX_Y, bw = LI_W, bh = MS_BOX_H;
    const Theme *T = TH();
    uint32_t text = set_style ? T->text : RGB(0x1e, 0x1e, 0x1e);
    fill_round(bx, by, bw, bh, set_style ? 6 : 3, set_style ? T->btn : RGB(255, 255, 255), 255);
    stroke_round(bx, by, bw, bh, set_style ? 6 : 3, ms_q[0] ? RGB(0x3c, 0x84, 0xd8) : RGB(0xa8, 0xb4, 0xc4), 255);
    draw_icon(ICON_SEARCH, bx + bw - 16, by + bh / 2, 7);
    if (ms_q[0]) {
        char s[80]; snprintf(s, sizeof s, "%s", ms_q);
        while (text_w(s) > bw - 40 && s[0]) memmove(s, s + u8_next(s, (int)strlen(s), 0), strlen(s));
        draw_text(bx + 8, by + 6, s, text, 255);
        fill_rect(bx + 9 + text_w(s), by + 6, 1, 16, text, 255);           /* caret */
    } else draw_text(bx + 8, by + 6, "Search apps and files", RGB(0x88, 0x90, 0x9c), 255);
    if (!ms_q[0]) return;
    int rows; ms_rows(&rows);
    /* cover the program list */
    fill_rect(LP_X + 2, ITEMS_Y - 2, LP_W - 4, by - ITEMS_Y - 2, set_style ? T->menu_left : RGB(255, 255, 255), 255);
    if (!ms_n) { draw_text(LI_X + 10, ITEMS_Y + 8, "Nothing found.", RGB(0x80, 0x88, 0x94), 255); return; }
    for (int i = 0; i < ms_n && i < rows; i++) {
        int y = ITEMS_Y + i * ITEM_H, on = (hot.type == HOT_SRES && hot.a == i) || (hot.type != HOT_SRES && i == ms_sel);
        if (on) {
            if (set_style) fill_round(LI_X, y, LI_W, ITEM_H, 4, T->hover, 255);
            else { fill_round(LI_X, y, LI_W, ITEM_H, 3, RGB(0xdc, 0xec, 0xfc), 255); stroke_round(LI_X, y, LI_W, ITEM_H, 3, RGB(0x7d, 0xa2, 0xce), 255); }
        }
        draw_icon(ms_res[i].icon, LI_X + 18, y + ITEM_H / 2, 10);
        const char *kind = ms_res[i].kind == MSR_APP ? "App" : ms_res[i].kind == MSR_SET ? "" : ms_res[i].kind == MSR_DIR ? "Folder" : "File";
        char l[64]; snprintf(l, sizeof l, "%s", ms_res[i].label);
        int room = LI_W - 46 - (kind[0] ? text_w(kind) + 8 : 0);
        if (text_w(l) > room) { u8_trunc(l, imax(1, room / 8 - 2)); strcat(l, ".."); }
        draw_text(LI_X + 36, y + (ITEM_H - 16) / 2, l, text, 255);
        if (kind[0]) draw_text(LI_X + LI_W - 6 - text_w(kind), y + (ITEM_H - 16) / 2, kind, RGB(0x88, 0x90, 0x9c), 255);
    }
}
