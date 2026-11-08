/* ===================================================================
 *  TIME: time zones, setting the clock from the internet, and the
 *  calendar panel that opens from the clock on the taskbar.
 *
 *  - The time zone is a link, /etc/localtime -> /usr/share/zoneinfo/<zone>,
 *    so every program (Flathub apps too) uses it.
 *  - "Set automatically" asks ip-api.com which time zone the internet
 *    connection is in (once per start, when online).
 *  - The clock is set from the internet (NTP: pool.ntp.org) when online,
 *    then every 6 hours.
 *  - Windows keeps the computer's hardware clock in LOCAL time; Linux
 *    expects UTC. With "Windows is on this computer too" on (the default)
 *    ccos reads and writes the hardware clock the Windows way, so both
 *    show the right time.
 * =================================================================== */
#define TZDIR "/usr/share/zoneinfo"
typedef struct { char zone[48], city[40], country[40]; int off; } TzEnt;   /* off: seconds from UTC, now */
static TzEnt *tzl; static int ntz;
static char tz_search[40]; static int tz_search_focus, tz_scroll;
static long long tz_ntp_last_try, tz_auto_last_try;
static int tz_auto_done;
static pid_t tz_ntp_pid, tz_auto_pid;

static int tz_valid(const char *z) {
    if (!z[0] || strstr(z, "..") || z[0] == '/') return 0;
    char p[160]; snprintf(p, sizeof p, TZDIR "/%s", z);
    struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
/* "UTC+01:00" */
static void tz_off_str(int off, char *out, size_t n) {
    int a = off < 0 ? -off : off;
    if (!off) snprintf(out, n, "UTC");
    else snprintf(out, n, "UTC%c%02d:%02d", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
}
static int tz_now_off(void) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); return (int)tm.tm_gmtoff; }
/* "Warsaw, Poland" for Europe/Warsaw */
static void tz_pretty(const char *z, char *out, size_t n) {
    if (!z[0] || !strcmp(z, "UTC")) { snprintf(out, n, "UTC (no time zone)"); return; }
    for (int i = 0; i < ntz; i++) if (!strcmp(tzl[i].zone, z)) { snprintf(out, n, "%s, %s", tzl[i].city, tzl[i].country); return; }
    const char *s = strrchr(z, '/'); snprintf(out, n, "%s", s ? s + 1 : z);
    for (char *c = out; *c; c++) if (*c == '_') *c = ' ';
}
/* "Côte d'Ivoire" -> "Cote d'Ivoire": the pixel font only has plain letters */
static void tz_ascii(char *s) {
    static const char *from = "\xc0\xc1\xc2\xc3\xc4\xc5\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd1\xd2\xd3\xd4\xd5\xd6\xd8\xd9\xda\xdb\xdc\xdd"
                              "\xe0\xe1\xe2\xe3\xe4\xe5\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf1\xf2\xf3\xf4\xf5\xf6\xf8\xf9\xfa\xfb\xfc\xfd\xff";
    static const char *to = "AAAAAACEEEEIIIINOOOOOOUUUUYaaaaaaceeeeiiiinoooooouuuuyy";
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p < 0x80) { *o++ = (char)*p; continue; }
        if ((*p & 0xE0) == 0xC0 && p[1]) {                     /* two bytes: Latin-1 letters */
            unsigned c = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p++;
            const char *f = c >= 0xC0 && c <= 0xFF ? strchr(from, (char)c) : NULL;
            if (f) *o++ = to[f - from];
            else if (c == 0x2019) *o++ = '\'';
            continue;
        }
        if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {             /* three bytes: curly quotes */
            unsigned c = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 2;
            if (c == 0x2019 || c == 0x2018) *o++ = '\'';
            continue;
        }
        while (p[1] && (p[1] & 0xC0) == 0x80) p++;             /* anything else: drop */
    }
    *o = 0;
}
static int tz_cmp(const void *a, const void *b) { return strcasecmp(((const TzEnt *)a)->city, ((const TzEnt *)b)->city); }
static void tz_load(void) {
    if (tzl) return;
    struct { char cc[3], name[40]; } *cn = calloc(300, sizeof *cn); int ncn = 0;
    FILE *f = fopen(TZDIR "/iso3166.tab", "r"); char l[256];
    while (f && cn && fgets(l, sizeof l, f) && ncn < 300) {
        if (l[0] == '#' || strlen(l) < 4) continue;
        l[strcspn(l, "\n")] = 0;
        snprintf(cn[ncn].cc, 3, "%.2s", l); snprintf(cn[ncn].name, 40, "%s", l + 3); tz_ascii(cn[ncn].name); ncn++;
    }
    if (f) fclose(f);
    tzl = calloc(512, sizeof *tzl);
    if (!tzl) { free(cn); return; }
    f = fopen(TZDIR "/zone.tab", "r");
    char *saved = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
    while (f && fgets(l, sizeof l, f) && ntz < 511) {
        if (l[0] == '#') continue;
        l[strcspn(l, "\n")] = 0;
        char *cc = strtok(l, "\t"), *coord = strtok(NULL, "\t"), *zone = strtok(NULL, "\t");
        (void)coord;
        if (!cc || !zone || !tz_valid(zone)) continue;
        TzEnt *e = &tzl[ntz++];
        snprintf(e->zone, sizeof e->zone, "%s", zone);
        const char *s = strrchr(zone, '/'); snprintf(e->city, sizeof e->city, "%s", s ? s + 1 : zone);
        for (char *c = e->city; *c; c++) if (*c == '_') *c = ' ';
        snprintf(e->country, sizeof e->country, "%s", cc);
        for (int k = 0; k < ncn; k++) if (!strcmp(cn[k].cc, cc)) snprintf(e->country, sizeof e->country, "%s", cn[k].name);
        char tzv[80]; snprintf(tzv, sizeof tzv, ":" TZDIR "/%s", zone);
        setenv("TZ", tzv, 1); tzset();
        e->off = tz_now_off();
    }
    if (f) fclose(f);
    if (saved) { setenv("TZ", saved, 1); free(saved); } else unsetenv("TZ");
    tzset();
    TzEnt *u = &tzl[ntz++];                                 /* and plain UTC */
    snprintf(u->zone, sizeof u->zone, "UTC"); snprintf(u->city, sizeof u->city, "UTC"); snprintf(u->country, sizeof u->country, "Coordinated Universal Time");
    qsort(tzl, (size_t)ntz, sizeof *tzl, tz_cmp);
    free(cn);
}

/* the Windows-style hardware clock: once per start, after the zone is known */
static void tz_fix_rtc(void) {
    if (!set_rtc_local || access("/run/ccos/rtc-fixed", F_OK) == 0) return;
    if (access("/run/ccos/time-synced", F_OK) == 0) return;   /* already right from the internet */
    if (!set_tz[0] || !strcmp(set_tz, "UTC")) return;
    int off = tz_now_off();
    struct timeval tv; gettimeofday(&tv, NULL);
    tv.tv_sec -= off;                                       /* the clock said local time, not UTC */
    settimeofday(&tv, NULL);
    mkdir("/run/ccos", 0755);
    FILE *f = fopen("/run/ccos/rtc-fixed", "w"); if (f) fclose(f);
}
static void clkp_repaint(void);
/* use this time zone, for ccos and every program started from now on */
static void tz_apply(const char *z) {
    if (!z[0] || !tz_valid(z)) z = "UTC";
    char tgt[160]; snprintf(tgt, sizeof tgt, TZDIR "/%s", z);
    unlink("/etc/localtime.new");
    if (symlink(tgt, "/etc/localtime.new") == 0) rename("/etc/localtime.new", "/etc/localtime");
    FILE *f = fopen("/etc/timezone", "w"); if (f) { fprintf(f, "%s\n", z); fclose(f); }
    unsetenv("TZ"); tzset();
    tz_fix_rtc();
    repaint_clock();
    if (win_of(settings_draw) >= 0) st_repaint();
    clkp_repaint();
}
static void tz_set(const char *z, int by_hand) {
    int old_off = tz_now_off();
    int fixed = access("/run/ccos/rtc-fixed", F_OK) == 0 && access("/run/ccos/time-synced", F_OK) != 0;
    snprintf(set_tz, sizeof set_tz, "%s", z);
    if (by_hand) set_tz_auto = 0;
    save_settings();
    tz_apply(set_tz);
    if (fixed && set_rtc_local) {           /* the clock came from the hardware clock read in the old zone: redo it */
        struct timeval tv; gettimeofday(&tv, NULL);
        tv.tv_sec += old_off - tz_now_off();
        settimeofday(&tv, NULL);
        repaint_clock(); clkp_repaint();
    }
}
static pid_t tz_sh(const char *cmd) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        int n = open("/dev/null", O_RDWR); if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    return p;
}
static int tz_alive(pid_t *p) { if (*p > 0 && kill(*p, 0) != 0) *p = 0; return *p > 0; }
/* set the clock from the internet now */
static void tz_sync_now(void) {
    if (tz_alive(&tz_ntp_pid)) return;
    tz_ntp_last_try = ui_ms();
    mkdir("/run/ccos", 0755);
    char cmd[400];
    snprintf(cmd, sizeof cmd,
             "timeout 25 ntpd -n -q -p pool.ntp.org -p time.cloudflare.com -p time.google.com && "
             "date +%%s > /run/ccos/time-synced && hwclock -w %s", set_rtc_local ? "-l" : "-u");
    tz_ntp_pid = tz_sh(cmd);
}
static int tz_online(void) { return wifi_online() || net.state == NET_CONNECTED; }
/* once a second */
static void tz_tick(void) {
    static int was_ntp, was_auto;
    int ntp = tz_alive(&tz_ntp_pid), aut = tz_alive(&tz_auto_pid);
    if (was_ntp && !ntp) {                                  /* a sync finished */
        repaint_clock(); clkp_repaint();
        if (win_of(settings_draw) >= 0 && st_tab == TAB_CLOCK) st_repaint();
    }
    if (was_auto && !aut) {                                 /* the location answer is in */
        char z[64] = ""; FILE *f = fopen("/run/ccos/tz-auto.txt", "r");
        if (f) { if (!fgets(z, sizeof z, f)) z[0] = 0; fclose(f); }
        z[strcspn(z, "\r\n ")] = 0;
        if (tz_valid(z)) { tz_auto_done = 1; if (set_tz_auto && strcmp(z, set_tz)) tz_set(z, 0); }
    }
    was_ntp = ntp; was_auto = aut;
    if (!tz_online()) return;
    if (set_tz_auto && !tz_auto_done && !aut && ui_ms() - tz_auto_last_try > 60000) {
        tz_auto_last_try = ui_ms();
        tz_auto_pid = tz_sh("wget -q -T 10 -O /run/ccos/tz-auto.txt 'http://ip-api.com/line/?fields=timezone'");
        was_auto = 1;
    }
    if (set_ntp && !ntp) {
        struct stat st; int synced = stat("/run/ccos/time-synced", &st) == 0;
        long long gap = synced ? 6LL * 3600 * 1000 : 60000;  /* retry a minute later; once right, every 6 hours */
        if (!tz_ntp_last_try || ui_ms() - tz_ntp_last_try > gap) { tz_sync_now(); was_ntp = 1; }
    }
}
static void tz_init(void) {
    mkdir("/run/ccos", 0755);
    tz_apply(set_tz);
}
/* "Set from the internet at 10:52" */
static void tz_sync_text(char *out, size_t n) {
    if (tz_alive(&tz_ntp_pid)) { snprintf(out, n, "Asking the internet for the time..."); return; }
    FILE *f = fopen("/run/ccos/time-synced", "r"); long long t = 0;
    if (f) { if (fscanf(f, "%lld", &t) != 1) t = 0; fclose(f); }
    if (t > 0) {
        time_t tt = (time_t)t; struct tm tm; localtime_r(&tt, &tm); char b[16];
        strftime(b, sizeof b, set_clock24 ? "%H:%M" : "%I:%M %p", &tm);
        snprintf(out, n, "Set from the internet at %s", b);
    } else snprintf(out, n, tz_online() ? "Not set from the internet yet" : "Connect to the internet to set it");
}

/* ---- the calendar panel (click the clock) ---- */
static int clk_vy, clk_vm;                         /* the month shown (clk_panel: desktop.c) */
enum { CK_PREV = 1, CK_NEXT, CK_ZONE, CK_TODAY };
static int clkp_x0(void) { return W - CLKP_W - 6; }
static int clkp_y0(void) { return imax(4, H - TASKBAR_H - CLKP_H - 6); }
static void clkp_repaint(void) { if (clk_panel || 1) repaint(W - CLKP_W - 12, 0, CLKP_W + 12, H - TASKBAR_H); }
static void clkp_rect(int id, int *x, int *y, int *w, int *h) {
    int x0 = clkp_x0(), y0 = clkp_y0();
    switch (id) {
    case CK_PREV: *x = x0 + CLKP_W - 74; *y = y0 + 98; *w = 28; *h = 26; break;
    case CK_NEXT: *x = x0 + CLKP_W - 42; *y = y0 + 98; *w = 28; *h = 26; break;
    case CK_TODAY: *x = x0 + 14; *y = y0 + 98; *w = 180; *h = 26; break;
    default: *x = x0 + 14; *y = y0 + CLKP_H - 50; *w = CLKP_W - 28; *h = 36; break;
    }
}
static int clkp_hit(int px, int py) {
    for (int id = CK_PREV; id <= CK_TODAY; id++) {
        int x, y, w, h; clkp_rect(id, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return id;
    }
    return 0;
}
static void clk_panel_set(int open) {
    if (open == clk_panel) return;
    clk_panel = open;
    if (open) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); clk_vy = tm.tm_year + 1900; clk_vm = tm.tm_mon; }
    clkp_repaint(); repaint_clock();
}
static void draw_clock_panel(int hot_id) {
    int x = clkp_x0(), y = clkp_y0(), w = CLKP_W, h = CLKP_H;
    if (x >= clip_x1 || x + w + 4 <= clip_x0 || y >= clip_y1 || y + h + 4 <= clip_y0) return;
    fill_round(x + 3, y + 3, w, h, 6, RGB(0, 0, 0), 70);
    if (set_style) { fill_round(x, y, w, h, 8, RGB(255, 255, 255), 255); stroke_round(x, y, w, h, 8, RGB(0xc8, 0xcc, 0xd2), 255); }
    else {
        fill_round(x, y, w, h, 6, RGB(0x16, 0x34, 0x56), 215);
        stroke_round(x, y, w, h, 6, RGB(0, 0, 0), 170);
        stroke_round(x + 1, y + 1, w - 2, h - 2, 5, RGB(255, 255, 255), 80);
        fill_round(x + 6, y + 6, w - 12, h - 12, 4, RGB(255, 255, 255), 250);
    }
    uint32_t text = RGB(0x20, 0x24, 0x2a), grey = RGB(0x70, 0x78, 0x84), blue = RGB(0x1e, 0x60, 0xc0);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char b[64];
    strftime(b, sizeof b, set_clock24 ? "%H:%M" : "%I:%M %p", &tm);
    draw_text_scaled(x + 18, y + 18, b, text, 3);
    strftime(b, sizeof b, "%A, %d %B %Y", &tm);
    draw_text(x + 20, y + 66, b, blue, 255);
    fill_rect(x + 14, y + 90, w - 28, 1, RGB(0xe0, 0xe4, 0xea), 255);
    /* the month */
    static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };
    snprintf(b, sizeof b, "%s %d", months[clk_vm], clk_vy);
    draw_text_bold(x + 20, y + 103, b, text);
    for (int id = CK_PREV; id <= CK_NEXT; id++) {
        int bx, by, bw, bh; clkp_rect(id, &bx, &by, &bw, &bh);
        if (hot_id == id) fill_round(bx, by, bw, bh, 4, RGB(0xe4, 0xee, 0xfa), 255);
        int cx = bx + bw / 2, cy = by + bh / 2, d = id == CK_PREV ? -1 : 1;
        for (int k = 0; k < 5; k++) fill_rect(cx - d * 2 + d * k - (d < 0 ? 0 : 0), cy - 4 + k, 2, 1, text, 255),
                                    fill_rect(cx - d * 2 + d * k, cy + 4 - k, 2, 1, text, 255);
    }
    static const char *wd[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    int gx = x + 20, gy = y + 134, cwid = (w - 40) / 7;
    for (int i = 0; i < 7; i++) draw_text(gx + i * cwid + (cwid - 16) / 2, gy, wd[i], i >= 5 ? RGB(0xb0, 0x40, 0x40) : grey, 255);
    struct tm first = { .tm_year = clk_vy - 1900, .tm_mon = clk_vm, .tm_mday = 1, .tm_hour = 12 };
    time_t ft = mktime(&first); struct tm f1; localtime_r(&ft, &f1);
    int start = (f1.tm_wday + 6) % 7;                            /* Monday first */
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int nd = mdays[clk_vm] + (clk_vm == 1 && ((clk_vy % 4 == 0 && clk_vy % 100) || clk_vy % 400 == 0));
    for (int d = 1; d <= nd; d++) {
        int cell = start + d - 1, cx = gx + (cell % 7) * cwid, cy = gy + 22 + (cell / 7) * 30;
        int today = d == tm.tm_mday && clk_vm == tm.tm_mon && clk_vy == tm.tm_year + 1900;
        if (today) { fill_round(cx + 2, cy - 4, cwid - 4, 26, 5, RGB(0x2c, 0x78, 0xd8), 255); }
        char s[4]; snprintf(s, sizeof s, "%d", d);
        uint32_t c = today ? RGB(255, 255, 255) : (cell % 7 >= 5 ? RGB(0xb0, 0x40, 0x40) : text);
        if (today) draw_text_bold(cx + (cwid - text_w(s)) / 2, cy + 2, s, c);
        else draw_text(cx + (cwid - text_w(s)) / 2, cy + 2, s, c, 255);
    }
    /* the time zone, and the way to change it */
    int zx, zy, zw, zh; clkp_rect(CK_ZONE, &zx, &zy, &zw, &zh);
    fill_rect(x + 14, zy - 8, w - 28, 1, RGB(0xe0, 0xe4, 0xea), 255);
    if (hot_id == CK_ZONE) fill_round(zx, zy, zw, zh, 4, RGB(0xe4, 0xee, 0xfa), 255);
    char zn[64], off[16]; tz_pretty(set_tz, zn, sizeof zn); tz_off_str(tz_now_off(), off, sizeof off);
    char line[96]; snprintf(line, sizeof line, "%.26s (%s)", zn, off);
    draw_text(zx + 8, zy + 2, line, text, 255);
    draw_text(zx + 8, zy + 19, "Change time zone and time settings", blue, 255);
    if (DARK) { if (dk_mask) dk_clear(x, y, w, h); dark_xform(x, y, w, h); }
}
static void clkp_click(int id) {
    if (id == CK_PREV || id == CK_NEXT) {
        clk_vm += id == CK_PREV ? -1 : 1;
        if (clk_vm < 0) { clk_vm = 11; clk_vy--; } else if (clk_vm > 11) { clk_vm = 0; clk_vy++; }
        clkp_repaint();
    } else if (id == CK_TODAY) {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); clk_vy = tm.tm_year + 1900; clk_vm = tm.tm_mon; clkp_repaint();
    } else if (id == CK_ZONE) {
        clk_panel_set(0);
        st_tab = TAB_CLOCK; run("@settings"); st_repaint();
    }
}

/* ---- Settings > Clock: the time zone list ---- */
#define TZ_LIST_Y 276
#define TZ_ROW 24
#define TZ_ROWS 5
static int tz_match(const TzEnt *e) {
    if (!tz_search[0]) return 1;
    return strcasestr(e->city, tz_search) || strcasestr(e->country, tz_search) || strcasestr(e->zone, tz_search);
}
static int tz_starts(const char *s, const char *q) { return !strncasecmp(s, q, strlen(q)); }
static int tz_filtered(int *idx, int max) {          /* names starting with the search come first */
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ntz && n < max; i++) {
            if (!tz_match(&tzl[i])) continue;
            int first = !tz_search[0] || tz_starts(tzl[i].city, tz_search) || tz_starts(tzl[i].country, tz_search);
            if (first == !pass) idx[n++] = i;
        }
    return n;
}
