/* ===================================================================
 *  SOFTWARE MANAGER - apps from Flathub
 *
 *  Flatpak downloads Flathub's app catalog (names, descriptions, icons):
 *      flatpak update --appstream flathub
 *  ccos-store-index turns it into a small list (one line per app) that we
 *  load here. Installing / removing runs flatpak in the background on a
 *  pseudo-terminal, so we can read its progress bar.
 * =================================================================== */
#include <pty.h>
static void run(const char *cmd);

#define ST_AS     "/var/lib/flatpak/appstream/flathub/x86_64/active"
#define ST_INDEX  "/var/lib/flatpak/ccos-store.idx"
#define SM_TOP    58                       /* header bar height */

typedef struct {
    char *id, *name, *sum, *dev, *cats, *icon, *lic, *ver, *home, *desc;
    int pop;                               /* place in the "popular" list (0 = not popular) */
} StApp;
static StApp *sm_apps; static int sm_n;
static char *sm_buf;                     /* the whole index file; fields point into it */

/* apps most people look for first */
static const char *sm_popular[] = {
    "org.mozilla.firefox", "com.google.Chrome", "com.discordapp.Discord", "com.spotify.Client",
    "com.valvesoftware.Steam", "org.prismlauncher.PrismLauncher", "org.videolan.VLC", "com.obsproject.Studio",
    "org.libreoffice.LibreOffice", "org.gimp.GIMP", "org.telegram.desktop", "com.visualstudio.code",
    "org.kde.krita", "org.blender.Blender", "org.audacityteam.Audacity", "net.supertuxkart.SuperTuxKart",
    "com.brave.Browser", "org.inkscape.Inkscape", "org.kde.kdenlive", "com.heroicgameslauncher.hgl",
};
#define SM_NPOP ((int)(sizeof sm_popular / sizeof *sm_popular))

static const struct { const char *label, *cats; } sm_cat[] = {
    { "Games", "Game" }, { "Internet", "Network" }, { "Audio & Video", "AudioVideo;Audio;Video" },
    { "Graphics & Photos", "Graphics" }, { "Office", "Office" }, { "Development", "Development" },
    { "Education & Science", "Education;Science" }, { "Utilities", "Utility;System" },
};
#define SM_NCAT ((int)(sizeof sm_cat / sizeof *sm_cat))

enum { SP_HOME, SP_LIST, SP_DETAIL, SP_INSTALLED };
static int sm_page = SP_HOME, sm_prev_page = SP_HOME, sm_cat_sel = -1, sm_detail = -1, sm_scroll, sm_content_h;
static char sm_search[48]; static int sm_search_on;
static int *sm_list; static int sm_nlist;              /* apps shown on the list page */

/* ---- background jobs: refresh the catalog, install, remove, update ---- */
enum { SJ_NONE, SJ_REFRESH, SJ_INSTALL, SJ_REMOVE, SJ_UPDATE };
static struct { int kind; char id[96]; int fd; pid_t pid; int pct, step, steps; char out[4096]; int outn; long long t0; } sm_job;
static char sm_msg[160]; static int sm_msg_bad;        /* the line at the bottom: "Firefox is installed" */
static int sm_loading;                                  /* catalog being fetched */

/* ---- small helpers ---------------------------------------------------- */
static char *sm_unescape(char *s) {                     /* "\n" -> newline, "\\" -> backslash */
    char *o = s, *r = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'n') { *o++ = '\n'; r += 2; }
        else if (r[0] == '\\' && r[1] == '\\') { *o++ = '\\'; r += 2; }
        else *o++ = *r++;
    }
    *o = 0;
    return s;
}
static char *sm_cp437(const char *s) {                  /* UTF-8 -> our font's letters (a new string) */
    size_t n = strlen(s);
    char *o = malloc(n + 1);
    if (!o) return strdup("");
    size_t w = 0;
    while (*s) {                                         /* line by line: keep the line breaks */
        const char *nl = strchr(s, '\n');
        size_t L = nl ? (size_t)(nl - s) : strlen(s);
        if (L) { ssid_shown((const unsigned char *)s, (int)L, o + w, n + 1 - w); w += strlen(o + w); }
        if (!nl) break;
        o[w++] = '\n'; o[w] = 0;
        s = nl + 1;
    }
    o[w] = 0;
    return o;
}
static int sm_find(const char *id) {
    for (int i = 0; i < sm_n; i++) if (!strcmp(sm_apps[i].id, id)) return i;
    return -1;
}
static int sm_installed(const char *id) {
    char p[256]; snprintf(p, sizeof p, "/var/lib/flatpak/app/%s", id);
    return access(p, F_OK) == 0;
}
static int sm_has_cat(const StApp *a, const char *cats) {   /* cats: "A;B" - any of them */
    char want[64]; snprintf(want, sizeof want, "%s", cats);
    for (char *sv = NULL, *t = strtok_r(want, ";", &sv); t; t = strtok_r(NULL, ";", &sv)) {
        const char *c = a->cats; size_t L = strlen(t);
        while ((c = strstr(c, t))) {
            if ((c == a->cats || c[-1] == ';') && (c[L] == 0 || c[L] == ';')) return 1;
            c += L;
        }
    }
    return 0;
}
static int sm_ci_find(const char *hay, const char *needle) {   /* case-insensitive: position or -1 */
    size_t L = strlen(needle);
    for (const char *h = hay; *h; h++) if (!strncasecmp(h, needle, L)) return (int)(h - hay);
    return -1;
}

/* ---- loading the catalog ---------------------------------------------- */
static void sm_free(void) {
    for (int i = 0; i < sm_n; i++) { free(sm_apps[i].name); free(sm_apps[i].sum); free(sm_apps[i].dev); }
    free(sm_apps); free(sm_buf); free(sm_list);
    sm_apps = NULL; sm_buf = NULL; sm_list = NULL; sm_n = sm_nlist = 0;
}
static int sm_load(void) {
    FILE *f = fopen(ST_INDEX, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)len + 1);
    if (!t || fread(t, 1, (size_t)len, f) != (size_t)len) { free(t); fclose(f); return 0; }
    fclose(f);
    t[len] = 0;
    int lines = 0; for (long i = 0; i < len; i++) if (t[i] == '\n') lines++;
    StApp *a = calloc((size_t)lines + 1, sizeof *a);
    if (!a) { free(t); return 0; }
    sm_free();
    sm_buf = t; sm_apps = a;
    char *line = t, *nl;
    while (*line && (nl = strchr(line, '\n'))) {
        *nl = 0;
        char *fld[10] = { 0 }; int k = 0; char *p = line;
        fld[k++] = p;
        while (k < 10 && (p = strchr(p, '\t'))) { *p++ = 0; fld[k++] = p; }
        if (k == 10) {
            StApp *x = &sm_apps[sm_n];
            x->id = fld[0]; x->name = sm_cp437(fld[1]); x->sum = sm_cp437(fld[2]); x->dev = sm_cp437(fld[3]);
            x->cats = fld[4]; x->icon = fld[5]; x->lic = fld[6]; x->ver = fld[7]; x->home = fld[8];
            x->desc = sm_unescape(fld[9]);
            for (int j = 0; j < SM_NPOP; j++) if (!strcmp(sm_popular[j], x->id)) x->pop = j + 1;
            sm_n++;
        }
        line = nl + 1;
    }
    sm_list = malloc(sizeof *sm_list * (size_t)(sm_n + 1));
    return sm_n;
}

/* ---- app icons from the catalog (64 x 64 PNGs), kept for the last ~120 shown ---- */
#define SM_ICONS 120
static struct { int app; long long used; uint8_t px[64 * 64 * 4]; int ok; } sm_ic[SM_ICONS];
static const uint8_t *sm_icon(int app) {
    if (app < 0 || app >= sm_n) return NULL;
    int lru = 0;
    for (int i = 0; i < SM_ICONS; i++) {
        if (sm_ic[i].used && sm_ic[i].app == app) { sm_ic[i].used = ui_ms(); return sm_ic[i].ok ? sm_ic[i].px : NULL; }
        if (sm_ic[i].used < sm_ic[lru].used) lru = i;
    }
    sm_ic[lru].app = app; sm_ic[lru].used = ui_ms(); sm_ic[lru].ok = 0;
    if (sm_apps[app].icon[0]) {
        char p[400];
        snprintf(p, sizeof p, ST_AS "/icons/128x128/%s", sm_apps[app].icon);
        if (access(p, R_OK) != 0) snprintf(p, sizeof p, ST_AS "/icons/64x64/%s", sm_apps[app].icon);
        sm_ic[lru].ok = img_load_file(p, sm_ic[lru].px);
    }
    return sm_ic[lru].ok ? sm_ic[lru].px : NULL;
}
static void sm_draw_px(const uint8_t *px, int x0, int y0, int s) {   /* a 64 x 64 picture at size s */
    dk_begin(x0, y0, s, s);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float fx0 = (float)x * 64 / s, fx1 = (float)(x + 1) * 64 / s, fy0 = (float)y * 64 / s, fy1 = (float)(y + 1) * 64 / s;
            int ix0 = (int)fx0, ix1 = imax(ix0 + 1, (int)ceilf(fx1)), iy0 = (int)fy0, iy1 = imax(iy0 + 1, (int)ceilf(fy1));
            unsigned r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = iy0; sy < iy1 && sy < 64; sy++)
                for (int sx = ix0; sx < ix1 && sx < 64; sx++) {
                    const uint8_t *p = px + ((size_t)sy * 64 + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; cnt++;
                }
            if (a) blend(x0 + x, y0 + y, RGB(r / a, g / a, b / a), (int)(a / cnt));
        }
    dk_end();
}
static void sm_app_icon(int app, int x, int y, int s) {
    const uint8_t *px = sm_icon(app);
    if (px) sm_draw_px(px, x, y, s);
    else draw_app_icon(x + s / 2, y + s / 2, s / 2 - 2);
}

/* ---- running flatpak in the background -------------------------------- */
static void sm_repaint(void);
static void sm_refresh_lists(void);
static void sm_job_start(int kind, const char *id) {
    if (sm_job.kind) return;
    char cmd[400];
    if (kind == SJ_REFRESH)
        snprintf(cmd, sizeof cmd, "flatpak update --appstream flathub && ccos-store-index " ST_AS "/appstream.xml.gz " ST_INDEX);
    else if (kind == SJ_INSTALL) snprintf(cmd, sizeof cmd, "flatpak install -y flathub %s", id);
    else if (kind == SJ_REMOVE) snprintf(cmd, sizeof cmd, "flatpak uninstall -y %s && flatpak uninstall -y --unused", id);
    else snprintf(cmd, sizeof cmd, "flatpak update -y");
    char full[512]; snprintf(full, sizeof full, "%s; echo; echo \"@@EXIT $?\"", cmd);
    int mfd;
    struct winsize ws = { .ws_row = 40, .ws_col = 110 };
    pid_t p = forkpty(&mfd, NULL, NULL, &ws);
    if (p < 0) { snprintf(sm_msg, sizeof sm_msg, "Couldn't start Flatpak."); sm_msg_bad = 1; return; }
    if (p == 0) {
        setenv("TERM", "xterm", 1); setenv("LANG", "C.UTF-8", 1);
        setenv("HOME", access("/apps/home", W_OK) == 0 ? "/apps/home" : "/root", 1);
        execl("/bin/sh", "sh", "-c", full, (char *)NULL);
        _exit(127);
    }
    fcntl(mfd, F_SETFL, O_NONBLOCK);
    memset(&sm_job, 0, sizeof sm_job);
    sm_job.kind = kind; sm_job.fd = mfd; sm_job.pid = p; sm_job.t0 = ui_ms();
    snprintf(sm_job.id, sizeof sm_job.id, "%s", id ? id : "");
    sm_msg[0] = 0;
    if (kind == SJ_REFRESH) sm_loading = 1;
    sm_repaint();
}
static const char *sm_name_of(const char *id) { int i = sm_find(id); return i >= 0 ? sm_apps[i].name : id; }
static void sm_job_done(int code) {
    int kind = sm_job.kind;
    char id[96]; snprintf(id, sizeof id, "%s", sm_job.id);
    close(sm_job.fd);
    /* the last thing flatpak said, for error messages */
    char last[160] = "";
    sm_job.out[sm_job.outn] = 0;
    for (char *sv = NULL, *l = strtok_r(sm_job.out, "\r\n", &sv); l; l = strtok_r(NULL, "\r\n", &sv)) {
        while (*l == ' ') l++;
        if (*l && strncmp(l, "@@EXIT", 6) && (strstr(l, "rror") || strstr(l, "not ") || strstr(l, "ailed")))
            snprintf(last, sizeof last, "%.150s", l);
    }
    memset(&sm_job, 0, sizeof sm_job);
    sm_msg_bad = code != 0;
    if (kind == SJ_REFRESH) {
        sm_loading = 0;
        if (code == 0) { sm_load(); sm_refresh_lists(); }
        else snprintf(sm_msg, sizeof sm_msg, "Couldn't get the app list from Flathub. Check the internet connection and click Retry.");
    } else if (kind == SJ_INSTALL) {
        if (code == 0) snprintf(sm_msg, sizeof sm_msg, "%s is installed. Find it in the start menu or on the desktop.", sm_name_of(id));
        else snprintf(sm_msg, sizeof sm_msg, "Couldn't install %s. %s", sm_name_of(id), last);
    } else if (kind == SJ_REMOVE) {
        if (code == 0) snprintf(sm_msg, sizeof sm_msg, "%s was removed.", sm_name_of(id));
        else snprintf(sm_msg, sizeof sm_msg, "Couldn't remove %s. %s", sm_name_of(id), last);
    } else {
        if (code == 0) snprintf(sm_msg, sizeof sm_msg, "Everything is up to date.");
        else snprintf(sm_msg, sizeof sm_msg, "Updating didn't finish. %s", last);
    }
    fp_scan();                                   /* start menu and desktop shortcuts */
    if (sm_page == SP_INSTALLED) sm_refresh_lists();
    repaint_all();
}
static int sm_fd(void) { return sm_job.kind ? sm_job.fd : -1; }
static void sm_wake(void) {
    if (!sm_job.kind) return;
    char b[2048]; ssize_t n;
    int changed = 0;
    while ((n = read(sm_job.fd, b, sizeof b)) > 0) {
        /* keep the newest output */
        if (sm_job.outn + n >= (ssize_t)sizeof sm_job.out - 1) {
            int keep = (int)sizeof sm_job.out / 2;
            memmove(sm_job.out, sm_job.out + sm_job.outn - keep, (size_t)keep); sm_job.outn = keep;
        }
        if (n > (ssize_t)sizeof sm_job.out - 1 - sm_job.outn) n = (ssize_t)sizeof sm_job.out - 1 - sm_job.outn;
        memcpy(sm_job.out + sm_job.outn, b, (size_t)n); sm_job.outn += (int)n;
        sm_job.out[sm_job.outn] = 0;
        changed = 1;
    }
    if (changed) {
        /* flatpak's progress: "Installing 2/3… ████  45%" - take the newest step and percentage */
        const char *s = sm_job.out;
        for (const char *p = s; (p = strstr(p, "%")); p++) {
            const char *q = p; int v = 0, mul = 1, digits = 0;
            while (q > s && q[-1] >= '0' && q[-1] <= '9' && digits < 3) { q--; v += (*q - '0') * mul; mul *= 10; digits++; }
            if (digits && v <= 100) sm_job.pct = v;
        }
        for (const char *p = s; (p = strstr(p, "/")); p++) {
            const char *q = p; int a = 0, b2 = 0, mul = 1, da = 0, db = 0;
            while (q > s && q[-1] >= '0' && q[-1] <= '9' && da < 3) { q--; a += (q[0] - '0') * mul; mul *= 10; da++; }
            const char *r = p + 1;
            while (*r >= '0' && *r <= '9' && db < 3) { b2 = b2 * 10 + (*r - '0'); r++; db++; }
            if (da && db && a >= 1 && a <= b2 && b2 < 50 && q > s + 1 &&
                (!strncmp(q - 11, "Installing ", 11) || !strncmp(q - 9, "Updating ", 9) || !strncmp(q - 13, "Uninstalling ", 13))) {
                sm_job.step = a; sm_job.steps = b2;
            }
        }
        const char *ex = strstr(s, "@@EXIT ");
        if (ex) { sm_job_done(atoi(ex + 7)); return; }
        sm_repaint();
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {   /* it went away */
        const char *ex = strstr(sm_job.out, "@@EXIT ");
        sm_job_done(ex ? atoi(ex + 7) : 1);
    }
}
static int sm_overall_pct(void) {
    if (sm_job.steps > 0 && sm_job.step > 0) return ((sm_job.step - 1) * 100 + sm_job.pct) / sm_job.steps;
    return sm_job.pct;
}

/* ---- lists ------------------------------------------------------------ */
static int sm_cmp_pop_name(const void *a, const void *b) {
    const StApp *x = &sm_apps[*(const int *)a], *y = &sm_apps[*(const int *)b];
    if ((x->pop > 0) != (y->pop > 0)) return x->pop > 0 ? -1 : 1;
    if (x->pop && y->pop) return x->pop - y->pop;
    return strcasecmp(x->name, y->name);
}
static int sm_score[1 << 13];
static int sm_cmp_score(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (sm_score[x & 8191] != sm_score[y & 8191]) return sm_score[y & 8191] - sm_score[x & 8191];
    return strcasecmp(sm_apps[x].name, sm_apps[y].name);
}
static void sm_refresh_lists(void) {
    sm_nlist = 0;
    if (!sm_list && sm_page != SP_INSTALLED) return;
    if (sm_page == SP_INSTALLED) {
        static int inst[256];
        DIR *d = opendir("/var/lib/flatpak/app");
        struct dirent *e;
        free(sm_list); sm_list = malloc(sizeof *sm_list * (size_t)(sm_n + 256));
        if (!sm_list) return;
        int ni = 0;
        while (d && (e = readdir(d)) && ni < 256) {
            if (e->d_name[0] == '.') continue;
            int k = sm_find(e->d_name);
            inst[ni++] = k;                         /* -1: not in the catalog (shown by its name) */
            sm_list[sm_nlist++] = k >= 0 ? k : -1 - 0;
        }
        if (d) closedir(d);
        /* keep only the ones we know; sort by name */
        int m = 0;
        for (int i = 0; i < sm_nlist; i++) if (sm_list[i] >= 0) sm_list[m++] = sm_list[i];
        sm_nlist = m;
        qsort(sm_list, (size_t)sm_nlist, sizeof *sm_list, sm_cmp_pop_name);
        (void)inst;
        return;
    }
    if (sm_search[0]) {                              /* search: names first, then descriptions */
        for (int i = 0; i < sm_n && i < 8192; i++) {
            const StApp *a = &sm_apps[i];
            int s = 0, pn = sm_ci_find(a->name, sm_search);
            if (pn == 0) s = 1000; else if (pn > 0) s = 700;
            else if (sm_ci_find(a->id, sm_search) >= 0) s = 400;
            else if (sm_ci_find(a->sum, sm_search) >= 0) s = 300;
            else if (strlen(sm_search) >= 3 && sm_ci_find(a->desc, sm_search) >= 0) s = 100;
            if (!s) continue;
            if (a->pop) s += 200 - a->pop;
            sm_score[i] = s;
            sm_list[sm_nlist++] = i;
        }
        qsort(sm_list, (size_t)sm_nlist, sizeof *sm_list, sm_cmp_score);
        if (sm_nlist > 300) sm_nlist = 300;
    } else if (sm_cat_sel >= 0) {
        for (int i = 0; i < sm_n; i++) if (sm_has_cat(&sm_apps[i], sm_cat[sm_cat_sel].cats)) sm_list[sm_nlist++] = i;
        qsort(sm_list, (size_t)sm_nlist, sizeof *sm_list, sm_cmp_pop_name);
    }
}

/* ---- clickable things (rebuilt on every draw) ------------------------- */
enum { SR_TAB_HOME, SR_TAB_INST, SR_SEARCH, SR_CLEAR, SR_CAT, SR_APP, SR_BTN_INSTALL, SR_BTN_OPEN, SR_BTN_REMOVE,
       SR_BACK, SR_RETRY, SR_UPDATE_ALL, SR_HOMEPAGE };
#define SM_MAXR 160
static struct { int x, y, w, h, kind, arg; } sm_r[SM_MAXR];
static int sm_nr;
static int sm_add(int x, int y, int w, int h, int kind, int arg) {
    if (sm_nr >= SM_MAXR) return -1;
    sm_r[sm_nr] = (typeof(sm_r[0])){ x, y, w, h, kind, arg };
    return sm_nr++;
}
static void store_draw(int cx, int cy, int cw, int ch);
static int sm_is_hot(int id) { return hot.type == HOT_CONTENT && wins[hot.a].draw == store_draw && hot.b == id; }

/* a button: blue (main action), or white */
static void sm_button(int x, int y, int w, int h, const char *label, int primary, int danger, int id) {
    int hv = sm_is_hot(id);
    if (primary) fill_round_grad(x, y, w, h, 5, hv ? RGB(0x52, 0xa6, 0xff) : RGB(0x3c, 0x94, 0xf4), hv ? RGB(0x2a, 0x74, 0xd8) : RGB(0x1e, 0x66, 0xcc), 255);
    else fill_round_grad(x, y, w, h, 5, hv ? RGB(0xff, 0xff, 0xff) : RGB(0xfa, 0xfb, 0xfd), hv ? RGB(0xe6, 0xee, 0xf8) : RGB(0xe4, 0xe8, 0xee), 255);
    stroke_round(x, y, w, h, 5, primary ? RGB(0x16, 0x56, 0xb0) : RGB(0xa8, 0xb2, 0xc0), 255);
    uint32_t tc = primary ? RGB(255, 255, 255) : danger ? RGB(0xb0, 0x28, 0x20) : RGB(0x20, 0x28, 0x34);
    draw_text_bold(x + (w - text_w(label)) / 2, y + (h - 16) / 2, label, tc);
}
static void sm_progress(int x, int y, int w, int pct) {
    fill_round(x, y, w, 8, 4, RGB(0xdc, 0xe2, 0xea), 255);
    if (pct > 0) fill_round(x, y, imax(8, w * pct / 100), 8, 4, RGB(0x2c, 0x84, 0xe8), 255);
}
/* text cut to fit n characters, with "..." */
static void sm_text(int x, int y, const char *s, int maxc, uint32_t c, int bold) {
    char b[256]; int L = (int)strlen(s);
    if (maxc < 4) return;
    if (L > maxc) snprintf(b, sizeof b, "%.*s...", imin(maxc - 3, 250), s);
    else snprintf(b, sizeof b, "%s", s);
    if (bold) draw_text_bold(x, y, b, c); else draw_text(x, y, b, c, 255);
}
/* word-wrapped text; returns the height used */
static int sm_wrap(int x, int y, const char *s, int maxc, uint32_t c, int draw) {
    int lines = 0; char line[256];
    if (maxc > 250) maxc = 250;
    while (*s) {
        if (*s == '\n') { lines++; s++; continue; }
        int L = 0, cut = -1;
        while (s[L] && s[L] != '\n' && L < maxc) { if (s[L] == ' ') cut = L; L++; }
        if (s[L] && s[L] != '\n' && s[L] != ' ' && cut > 0) L = cut;
        snprintf(line, sizeof line, "%.*s", L, s);
        if (draw) draw_text(x, y + lines * 18, line, c, 255);
        lines++;
        s += L;
        if (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return lines * 18;
}

/* what the app's main button says, and whether it is busy */
static int sm_state(int app, int *busy_pct) {   /* 0 install, 1 installed, 2 installing, 3 removing */
    *busy_pct = 0;
    if (sm_job.kind == SJ_INSTALL && !strcmp(sm_job.id, sm_apps[app].id)) { *busy_pct = sm_overall_pct(); return 2; }
    if (sm_job.kind == SJ_REMOVE && !strcmp(sm_job.id, sm_apps[app].id)) return 3;
    return sm_installed(sm_apps[app].id) ? 1 : 0;
}
static void sm_action_button(int app, int x, int y, int w, int h) {
    int pct, st = sm_state(app, &pct);
    if (st == 2) {
        char b[32]; snprintf(b, sizeof b, "%d%%", pct);
        fill_round(x, y, w, h, 5, RGB(0xee, 0xf2, 0xf7), 255);
        stroke_round(x, y, w, h, 5, RGB(0xc0, 0xc8, 0xd2), 255);
        fill_round(x + 3, y + h - 7, imax(6, (w - 6) * pct / 100), 4, 2, RGB(0x2c, 0x84, 0xe8), 255);
        draw_text(x + (w - text_w(b)) / 2, y + (h - 20) / 2, b, RGB(0x30, 0x40, 0x50), 255);
    } else if (st == 3) {
        fill_round(x, y, w, h, 5, RGB(0xee, 0xf2, 0xf7), 255);
        draw_text(x + (w - 8 * 8) / 2, y + (h - 16) / 2, "Removing", RGB(0x60, 0x68, 0x70), 255);
    } else {
        int id = sm_add(x, y, w, h, st == 1 ? SR_BTN_OPEN : SR_BTN_INSTALL, app);
        sm_button(x, y, w, h, st == 1 ? "Open" : "Install", st == 0, 0, id);
    }
}

/* a row in a list: icon, name, summary, button */
static void sm_row(int app, int x, int y, int w) {
    int id = sm_add(x, y, w - 120, 64, SR_APP, app);
    if (sm_is_hot(id)) fill_round(x, y, w, 64, 6, RGB(0xe8, 0xf1, 0xfc), 255);
    sm_app_icon(app, x + 10, y + 8, 48);
    int maxc = (w - 76 - 120) / 8;
    sm_text(x + 70, y + 13, sm_apps[app].name, maxc, RGB(0x10, 0x18, 0x24), 1);
    sm_text(x + 70, y + 33, sm_apps[app].sum, maxc, RGB(0x60, 0x68, 0x74), 0);
    sm_action_button(app, x + w - 108, y + 16, 96, 32);
}

static void sm_heading(int x, int y, const char *s) { draw_text_bold(x, y, s, RGB(0x1e, 0x39, 0x87)); }

/* ---- the window ------------------------------------------------------- */
static void store_draw(int cx, int cy, int cw, int ch) {
    sm_nr = 0;
    uint32_t text = RGB(0x20, 0x26, 0x30), grey = RGB(0x68, 0x70, 0x7c);
    /* page background */
    fill_rect(cx, cy, cw, ch, RGB(0xf6, 0xf8, 0xfb), 255);

    /* ---- the page (scrolls) ---- */
    int top = cy + SM_TOP, y = top + 16 - sm_scroll, pad = 20, iw = cw - 2 * pad;
    int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;
    clip_y0 = imax(clip_y0, top);
    int y_start = y;
    if (!sm_n && sm_page != SP_INSTALLED) {
        if (sm_loading) {
            draw_text_bold(cx + pad, y + 10, "Getting the list of apps from Flathub...", text);
            draw_text(cx + pad, y + 32, "The first time this takes a minute or two.", grey, 255);
            sm_progress(cx + pad, y + 60, imin(iw, 420), sm_job.pct);
        } else {
            draw_text_bold(cx + pad, y + 10, "No app list yet.", text);
            int id = sm_add(pad, y + 40 - cy, 120, 32, SR_RETRY, 0);
            sm_button(cx + pad, y + 40, 120, 32, "Retry", 1, 0, id);
        }
        y += 120;
    } else if (sm_page == SP_HOME) {
        sm_heading(cx + pad, y, "Popular apps");
        y += 28;
        int cols = imax(1, (iw + 12) / (250 + 12)), cwid = (iw - (cols - 1) * 12) / cols, k = 0;
        for (int j = 0; j < SM_NPOP; j++) {
            int a = sm_find(sm_popular[j]);
            if (a < 0) continue;
            int x = cx + pad + (k % cols) * (cwid + 12), yy = y + (k / cols) * 84;
            int id = sm_add(x - cx, yy - cy, cwid, 74, SR_APP, a);
            int hv = sm_is_hot(id);
            fill_round(x, yy, cwid, 74, 8, hv ? RGB(0xee, 0xf5, 0xff) : RGB(255, 255, 255), 255);
            stroke_round(x, yy, cwid, 74, 8, hv ? RGB(0x8c, 0xb8, 0xf0) : RGB(0xdc, 0xe2, 0xea), 255);
            sm_app_icon(a, x + 12, yy + 13, 48);
            int maxc = (cwid - 76) / 8;
            sm_text(x + 70, yy + 18, sm_apps[a].name, maxc, RGB(0x10, 0x18, 0x24), 1);
            sm_text(x + 70, yy + 38, sm_apps[a].sum, maxc, grey, 0);
            k++;
        }
        y += ((k + cols - 1) / cols) * 84 + 18;
        sm_heading(cx + pad, y, "Categories");
        y += 28;
        int ccols = imax(1, (iw + 12) / (190 + 12)), ccw = (iw - (ccols - 1) * 12) / ccols;
        for (int i = 0; i < SM_NCAT; i++) {
            int x = cx + pad + (i % ccols) * (ccw + 12), yy = y + (i / ccols) * 52;
            int id = sm_add(x - cx, yy - cy, ccw, 42, SR_CAT, i);
            int hv = sm_is_hot(id);
            fill_round_grad(x, yy, ccw, 42, 8, hv ? RGB(0xff, 0xb4, 0x5a) : RGB(0xff, 0xa6, 0x40),
                            hv ? RGB(0xf0, 0x7c, 0x1c) : RGB(0xe8, 0x70, 0x10), 255);
            draw_text_bold(x + 14, yy + 13, sm_cat[i].label, RGB(255, 255, 255));
        }
        y += ((SM_NCAT + ccols - 1) / ccols) * 52 + 10;
        char b[96]; snprintf(b, sizeof b, "%d apps from flathub.org", sm_n);
        draw_text(cx + pad, y, b, grey, 255);
        y += 30;
    } else if (sm_page == SP_LIST || sm_page == SP_INSTALLED) {
        char head[96];
        if (sm_page == SP_INSTALLED) {
            snprintf(head, sizeof head, "Installed apps (%d)", sm_nlist);
            sm_heading(cx + pad, y + 8, head);
            if (sm_job.kind == SJ_UPDATE) {
                char b[48]; snprintf(b, sizeof b, "Updating... %d%%", sm_overall_pct());
                draw_text(cx + cw - pad - 260, y + 8, b, grey, 255);
                sm_progress(cx + cw - pad - 260, y + 28, 260, sm_overall_pct());
            } else {
                int id = sm_add(cw - pad - 140, y - cy, 140, 32, SR_UPDATE_ALL, 0);
                sm_button(cx + cw - pad - 140, y, 140, 32, "Update all", 0, 0, id);
            }
            y += 44;
            struct statvfs sv; char fb[48] = "";
            if (statvfs("/var/lib/flatpak/", &sv) == 0) {
                double fr = (double)sv.f_bavail * sv.f_frsize;
                snprintf(fb, sizeof fb, "Free space for apps: %.1f GB", fr / 1e9);
                draw_text(cx + pad, y, fb, grey, 255);
            }
            struct stat a1, a2;
            if (stat("/apps", &a1) != 0 || stat("/", &a2) != 0 || a1.st_dev == a2.st_dev)
                draw_text(cx + pad, y + 18, "No apps disk: installed apps are kept in memory until ccos restarts.", RGB(0xb0, 0x60, 0x10), 255);
            y += 44;
            if (!sm_nlist) { draw_text(cx + pad, y, "Nothing installed yet. Explore to find apps.", grey, 255); y += 30; }
        } else {
            if (sm_search[0]) snprintf(head, sizeof head, "Results for \"%.30s\" (%d)", sm_search, sm_nlist);
            else snprintf(head, sizeof head, "%s (%d)", sm_cat[sm_cat_sel].label, sm_nlist);
            int id = sm_add(pad, y - cy, 80, 28, SR_BACK, 0);
            sm_button(cx + pad, y, 80, 28, "< Back", 0, 0, id);
            sm_heading(cx + pad + 96, y + 6, head);
            y += 44;
            if (!sm_nlist) { draw_text(cx + pad, y, "No apps found. Try another word.", grey, 255); y += 30; }
        }
        for (int i = 0; i < sm_nlist; i++) {
            if (y + 64 >= top && y < cy + ch) {             /* only rows in the window (clicks need them all) */
                int id0 = sm_nr;
                sm_row(sm_list[i], cx + pad, y, iw);
                for (int k = id0; k < sm_nr; k++) { sm_r[k].x -= cx; sm_r[k].y -= cy; }
            }
            y += 68;
        }
        y += 10;
    } else if (sm_page == SP_DETAIL && sm_detail >= 0) {
        const StApp *a = &sm_apps[sm_detail];
        int id = sm_add(pad, y - cy, 80, 28, SR_BACK, 0);
        sm_button(cx + pad, y, 80, 28, "< Back", 0, 0, id);
        y += 44;
        sm_app_icon(sm_detail, cx + pad, y, 96);
        int tx = cx + pad + 116, maxc = (cw - 116 - 2 * pad - 170) / 8;
        if (set_smooth && (int)strlen(a->name) * 16 < cw - 300) draw_text_scaled(tx, y + 4, a->name, text, 2);
        else sm_text(tx, y + 10, a->name, maxc, text, 1);
        sm_text(tx, y + 44, a->sum, maxc, grey, 0);
        if (a->dev[0]) { char b[96]; snprintf(b, sizeof b, "by %.80s", a->dev); sm_text(tx, y + 66, b, maxc, grey, 0); }
        /* buttons on the right */
        int bx = cx + cw - pad - 150, pct, st = sm_state(sm_detail, &pct);
        if (st == 2) {
            char b[48]; snprintf(b, sizeof b, "Installing... %d%%", pct);
            draw_text(bx - 20, y + 8, b, text, 255);
            sm_progress(bx - 20, y + 30, 170, pct);
            if (sm_job.steps > 1) { char s2[48]; snprintf(s2, sizeof s2, "Part %d of %d", sm_job.step, sm_job.steps); draw_text(bx - 20, y + 44, s2, grey, 255); }
        } else if (st == 3) draw_text(bx, y + 8, "Removing...", text, 255);
        else if (st == 1) {
            int i1 = sm_add(bx - cx, y - cy, 150, 34, SR_BTN_OPEN, sm_detail);
            sm_button(bx, y, 150, 34, "Open", 1, 0, i1);
            int i2 = sm_add(bx - cx, y + 44 - cy, 150, 34, SR_BTN_REMOVE, sm_detail);
            sm_button(bx, y + 44, 150, 34, "Remove", 0, 1, i2);
        } else {
            int i1 = sm_add(bx - cx, y - cy, 150, 34, SR_BTN_INSTALL, sm_detail);
            sm_button(bx, y, 150, 34, sm_job.kind ? "Install (wait)" : "Install", 1, 0, i1);
        }
        y += 112;
        /* facts */
        char info[200];
        snprintf(info, sizeof info, "Version %s    Licence %s", a->ver[0] ? a->ver : "?", a->lic[0] ? a->lic : "?");
        draw_text(cx + pad, y, info, grey, 255);
        y += 20;
        if (a->home[0]) {
            char hb[128]; char *hc = sm_cp437(a->home); snprintf(hb, sizeof hb, "Website: %.110s", hc); free(hc);
            draw_text(cx + pad, y, hb, grey, 255);
            y += 20;
        }
        snprintf(info, sizeof info, "Flatpak name: %s", a->id);
        draw_text(cx + pad, y, info, grey, 255);
        y += 30;
        fill_rect(cx + pad, y, iw, 1, RGB(0xdc, 0xe2, 0xea), 255);
        y += 16;
        char *d = sm_cp437(a->desc);
        y += sm_wrap(cx + pad, y, d[0] ? d : "No description.", iw / 8, text, 1);
        free(d);
        y += 20;
    }
    sm_content_h = y - y_start + 16;
    clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;

    /* scroll bar */
    int view = ch - SM_TOP;
    if (sm_content_h > view) {
        int bh = imax(30, view * view / sm_content_h), by = SM_TOP + (view - bh) * sm_scroll / imax(1, sm_content_h - view);
        fill_round(cx + cw - 8, cy + by + 2, 5, bh - 4, 2, RGB(0xa8, 0xb4, 0xc4), 255);
    }

    /* ---- header (fixed) ---- */
    for (int j = 0; j < SM_TOP; j++) {
        int t = j * 255 / SM_TOP;
        fill_rect(cx, cy + j, cw, 1, RGB(0xff - t / 12, 0xff - t / 10, 0xff - t / 8), 255);
    }
    fill_rect(cx, cy + SM_TOP - 1, cw, 1, RGB(0xd0, 0xd8, 0xe2), 255);
    /* the bag and the name */
    draw_icon(ICON_BAG, cx + 26, cy + SM_TOP / 2, 14);
    /* tabs */
    const char *tabs[2] = { "Explore", "Installed" };
    int tx = cx + 52;
    for (int i = 0; i < 2; i++) {
        int tw = text_w(tabs[i]) + 28;
        int inst = sm_page == SP_INSTALLED || (sm_page == SP_DETAIL && sm_prev_page == SP_INSTALLED);
        int on = i == 0 ? !inst : inst;
        int id = sm_add(tx - cx, 13, tw, 32, i == 0 ? SR_TAB_HOME : SR_TAB_INST, 0);
        int hv = sm_is_hot(id);
        if (on) fill_round(tx, cy + 13, tw, 32, 16, RGB(0xff, 0x8c, 0x1a), 255);
        else if (hv) fill_round(tx, cy + 13, tw, 32, 16, RGB(0xe6, 0xec, 0xf4), 255);
        draw_text_bold(tx + 14, cy + 21, tabs[i], on ? RGB(255, 255, 255) : text);
        tx += tw + 6;
    }
    /* search box */
    int sw = imin(320, cw - (tx - cx) - 30), sxx = cx + cw - sw - 18;
    if (sw > 120) {
        int id = sm_add(sxx - cx, 13, sw, 32, SR_SEARCH, 0);
        fill_round(sxx, cy + 13, sw, 32, 16, RGB(255, 255, 255), 255);
        stroke_round(sxx, cy + 13, sw, 32, 16, sm_search_on ? RGB(0x3c, 0x94, 0xf4) : sm_is_hot(id) ? RGB(0x8c, 0xb8, 0xf0) : RGB(0xb8, 0xc2, 0xce), 255);
        /* magnifier */
        for (int a = 0; a < 360; a += 15) {
            float s = a * 3.14159f / 180;
            fill_rect(sxx + 18 + (int)(5 * cosf(s)), cy + 26 + (int)(5 * sinf(s)), 2, 2, grey, 255);
        }
        for (int k = 0; k < 4; k++) fill_rect(sxx + 22 + k, cy + 30 + k, 2, 2, grey, 255);
        int maxc = (sw - 70) / 8;
        const char *q = sm_search; int L = (int)strlen(q);
        if (L > maxc) q += L - maxc;
        if (sm_search[0]) draw_text(sxx + 34, cy + 21, q, text, 255);
        else draw_text(sxx + 34, cy + 21, "Search apps", RGB(0x98, 0xa0, 0xaa), 255);
        if (sm_search_on && (ui_ms() / 500) % 2 == 0) fill_rect(sxx + 34 + text_w(q), cy + 20, 1, 18, text, 255);
        if (sm_search[0]) {
            int ci = sm_add(sxx + sw - 30 - cx, 17, 24, 24, SR_CLEAR, 0);
            uint32_t xc = sm_is_hot(ci) ? RGB(0xc0, 0x30, 0x20) : grey;
            for (int k = 0; k < 9; k++) {
                fill_rect(sxx + sw - 23 + k, cy + 25 + k, 2, 1, xc, 255);
                fill_rect(sxx + sw - 15 - k, cy + 25 + k, 2, 1, xc, 255);
            }
        }
    }

    /* message at the bottom */
    if (sm_msg[0] || (sm_job.kind && sm_page != SP_DETAIL)) {
        char b[200];
        if (sm_msg[0]) snprintf(b, sizeof b, "%s", sm_msg);
        else if (sm_job.kind == SJ_INSTALL) snprintf(b, sizeof b, "Installing %s... %d%%", sm_name_of(sm_job.id), sm_overall_pct());
        else if (sm_job.kind == SJ_REMOVE) snprintf(b, sizeof b, "Removing %s...", sm_name_of(sm_job.id));
        else if (sm_job.kind == SJ_UPDATE) snprintf(b, sizeof b, "Updating apps... %d%%", sm_overall_pct());
        else snprintf(b, sizeof b, "Getting the app list from Flathub...");
        int bh = 34, by = cy + ch - bh;
        fill_rect(cx, by, cw, bh, sm_msg_bad ? RGB(0xfd, 0xe8, 0xe4) : sm_msg[0] ? RGB(0xe4, 0xf6, 0xe6) : RGB(0xe6, 0xef, 0xfa), 255);
        fill_rect(cx, by, cw, 1, RGB(0xc8, 0xd0, 0xda), 255);
        sm_text(cx + 14, by + 9, b, (cw - 28) / 8, sm_msg_bad ? RGB(0x90, 0x20, 0x18) : RGB(0x20, 0x40, 0x30), 0);
    }
}

static void sm_repaint(void) { repaint_client(store_draw); }
static void sm_scroll_to(int v) {
    int i = win_of(store_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    int maxs = imax(0, sm_content_h - (ch - SM_TOP));
    v = clampi(v, 0, maxs);
    if (v != sm_scroll) { sm_scroll = v; sm_repaint(); }
}
static void sm_go(int page) {
    if (page == SP_DETAIL) sm_prev_page = sm_page;
    sm_page = page; sm_scroll = 0;
    if (page != SP_DETAIL) sm_refresh_lists();
    sm_repaint();
}
static void sm_search_changed(void) {
    if (sm_search[0]) { sm_page = SP_LIST; sm_cat_sel = -1; }
    else if (sm_page == SP_LIST && sm_cat_sel < 0) sm_page = SP_HOME;
    sm_scroll = 0;
    sm_refresh_lists();
    sm_repaint();
}

static int store_hit(int lx, int ly, int cw, int ch) {
    (void)cw; (void)ch;
    for (int i = sm_nr - 1; i >= 0; i--) {
        if (lx >= sm_r[i].x && lx < sm_r[i].x + sm_r[i].w && ly >= sm_r[i].y && ly < sm_r[i].y + sm_r[i].h) {
            int header = sm_r[i].kind == SR_TAB_HOME || sm_r[i].kind == SR_TAB_INST || sm_r[i].kind == SR_SEARCH || sm_r[i].kind == SR_CLEAR;
            if (!header && ly < SM_TOP) continue;           /* scrolled under the header */
            return i;
        }
    }
    return -1;
}
static void store_elem_rect(int id, int *x, int *y, int *w, int *h) {
    if (id < 0 || id >= sm_nr) { *x = *y = *w = *h = 0; return; }
    *x = sm_r[id].x - 2; *y = sm_r[id].y - 2; *w = sm_r[id].w + 4; *h = sm_r[id].h + 4;
}
static void store_click(int lx, int ly, int cw, int ch) {
    int i = store_hit(lx, ly, cw, ch);
    int was_on = sm_search_on;
    sm_search_on = i >= 0 && (sm_r[i].kind == SR_SEARCH || sm_r[i].kind == SR_CLEAR);
    if (i < 0) { if (was_on) sm_repaint(); return; }
    int kind = sm_r[i].kind, arg = sm_r[i].arg;
    if (kind != SR_BTN_INSTALL && kind != SR_BTN_REMOVE && kind != SR_UPDATE_ALL) sm_msg[0] = 0;
    switch (kind) {
    case SR_TAB_HOME: sm_search[0] = 0; sm_cat_sel = -1; sm_go(SP_HOME); break;
    case SR_TAB_INST: sm_search[0] = 0; sm_go(SP_INSTALLED); break;
    case SR_SEARCH: sm_repaint(); break;
    case SR_CLEAR: sm_search[0] = 0; sm_search_changed(); break;
    case SR_CAT: sm_cat_sel = arg; sm_search[0] = 0; sm_go(SP_LIST); break;
    case SR_APP: sm_detail = arg; sm_go(SP_DETAIL); break;
    case SR_BACK:
        if (sm_page == SP_DETAIL) { sm_page = sm_prev_page; sm_scroll = 0; sm_refresh_lists(); sm_repaint(); }
        else { sm_cat_sel = -1; sm_search[0] = 0; sm_go(SP_HOME); }
        break;
    case SR_BTN_INSTALL:
        if (sm_job.kind) { snprintf(sm_msg, sizeof sm_msg, "Please wait until the current job is done."); sm_msg_bad = 1; sm_repaint(); break; }
        sm_job_start(SJ_INSTALL, sm_apps[arg].id);
        break;
    case SR_BTN_OPEN: { char c[128]; snprintf(c, sizeof c, "flatpak:%s", sm_apps[arg].id); run(c); break; }
    case SR_BTN_REMOVE:
        if (sm_job.kind) { snprintf(sm_msg, sizeof sm_msg, "Please wait until the current job is done."); sm_msg_bad = 1; sm_repaint(); break; }
        sm_job_start(SJ_REMOVE, sm_apps[arg].id);
        break;
    case SR_RETRY: sm_job_start(SJ_REFRESH, NULL); break;
    case SR_UPDATE_ALL:
        if (!sm_job.kind) sm_job_start(SJ_UPDATE, NULL);
        break;
    }
}
static void store_wheel(int amount) { sm_scroll_to(sm_scroll - amount * 60); }
static void store_key(int code, int mods) {
    int i = win_of(store_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (code == KEY_PAGEDOWN) { sm_scroll_to(sm_scroll + ch - SM_TOP - 40); return; }
    if (code == KEY_PAGEUP) { sm_scroll_to(sm_scroll - (ch - SM_TOP - 40)); return; }
    if (code == KEY_DOWN) { sm_scroll_to(sm_scroll + 40); return; }
    if (code == KEY_UP) { sm_scroll_to(sm_scroll - 40); return; }
    if (code == KEY_HOME && !sm_search_on) { sm_scroll_to(0); return; }
    if (code == KEY_ESC) {
        if (sm_search[0]) { sm_search[0] = 0; sm_search_changed(); }
        else if (sm_page == SP_DETAIL) { sm_page = sm_prev_page; sm_scroll = 0; sm_refresh_lists(); sm_repaint(); }
        else if (sm_page != SP_HOME) { sm_cat_sel = -1; sm_go(SP_HOME); }
        sm_search_on = 0;
        return;
    }
    if (code == KEY_BACKSPACE) {
        size_t L = strlen(sm_search);
        if (L) { sm_search[u8_prev(sm_search, (int)L)] = 0; sm_search_on = 1; sm_search_changed(); }
        return;
    }
    if (code == KEY_ENTER) { sm_search_on = 0; sm_repaint(); return; }
    char s[8]; int n = key_text(code, mods, s);
    if (n && (n > 1 || (s[0] >= 32 && s[0] < 127))) {   /* typing anywhere searches */
        size_t L = strlen(sm_search);
        if (L + (size_t)n < sizeof sm_search) memcpy(sm_search + L, s, (size_t)n + 1);
        sm_search_on = 1;
        if (sm_page == SP_INSTALLED || sm_page == SP_DETAIL) sm_page = SP_LIST;
        sm_search_changed();
    }
}
static void store_tick(void) {
    if (sm_search_on) {                              /* blink the text cursor */
        int i = win_of(store_draw); if (i < 0) return;
        repaint_client_rect(store_draw, 0, 0, 10000, SM_TOP);
    }
}
static void store_resized(void) { sm_scroll = 0; }
static int store_on_close(void) { return 1; }        /* jobs keep running; reopening shows them */

/* opening the window: load the list; fetch it if it's missing or a day old */
static void store_open(void) {
    if (!sm_n) sm_load();
    struct stat si, sa;
    int have_idx = stat(ST_INDEX, &si) == 0, have_as = stat(ST_AS "/appstream.xml.gz", &sa) == 0;
    if (!sm_job.kind) {
        if (!have_as || !have_idx || time(NULL) - sa.st_mtime > 24 * 3600) sm_job_start(SJ_REFRESH, NULL);
        else if (si.st_mtime < sa.st_mtime) sm_job_start(SJ_REFRESH, NULL);
    }
    if (sm_page == SP_INSTALLED || sm_page == SP_LIST) sm_refresh_lists();
}

/* the orange shopping bag */
static void draw_bag_icon(int cx, int cy, int r) {
    int w = r * 2 - r / 4, h = r * 2 - r / 2, x = cx - w / 2, y = cy - h / 2 + r / 5;
    /* handle */
    float hr = w * 0.26f;
    for (int a = 180; a <= 360; a += 4) {
        float s = a * 3.14159f / 180;
        float px = cx + hr * cosf(s), py = y + 1 + hr * 1.15f * sinf(s);
        fill_circle(px, py, imax(1, r / 9), RGB(0xb0, 0x4c, 0x08), 255);
    }
    fill_round_grad(x, y, w, h, imax(2, r / 5), RGB(0xff, 0xae, 0x40), RGB(0xe8, 0x66, 0x0c), 255);
    stroke_round(x, y, w, h, imax(2, r / 5), RGB(0xb0, 0x4c, 0x08), 255);
    /* shine and the two handle holes */
    fill_round(x + 2, y + 2, w - 4, imax(2, h / 4), imax(1, r / 6), RGB(255, 255, 255), 50);
    fill_circle(cx - hr, y + r / 5 + 2, imax(1, r / 10), RGB(0x8a, 0x3a, 0x06), 255);
    fill_circle(cx + hr, y + r / 5 + 2, imax(1, r / 10), RGB(0x8a, 0x3a, 0x06), 255);
}
