/* ===================================================================
 *  Clock: alarms, a timer, a stopwatch and clocks around the world.
 *  Alarms and the timer ring even when the Clock window is closed, and
 *  in Focus mode too: a little window pops up with Snooze / Stop.
 *  Saved in D:/clock.txt.
 * =================================================================== */
#define CK_W 660
#define CK_H 500
#define CK_FILE "/data/clock.txt"
#define CK_MAXAL 20
enum { CT_ALARM, CT_TIMER, CT_STOP, CT_WORLD, CT_N };
static const char *ck_tabs[CT_N] = { "Alarm", "Timer", "Stopwatch", "World clock" };
static struct { int hh, mm, days, on; char label[40]; long fired; } ck_al[CK_MAXAL];  /* days: bit 0 Monday .. bit 6 Sunday; 0 = once */
static struct {
    int loaded, tab, nal, scroll, field;                /* field: 1 alarm time, 2 alarm name, 3 timer, 4 city search */
    char f_time[8], f_label[40], f_timer[8], f_city[32], msg[64];
    long long t_total, t_left, t_end; int t_run;       /* the timer, in ms */
    long long sw_start, sw_acc; int sw_run; long long laps[60]; int nlaps;
    char zones[8][48]; int nz;
    int ring;                                           /* 0 no, 1 an alarm, 2 the timer */
    char ring_title[48], ring_text[64]; long long ring_sound, ring_until; pid_t ring_pid;
    long long last_frame;
} ck;
static void clockapp_draw(int cx, int cy, int cw, int ch);
static void ckr_draw(int cx, int cy, int cw, int ch);
static int ckr_hit(int lx, int ly, int cw, int ch);
static void ckr_click(int lx, int ly, int cw, int ch);
static void ckr_key(int code, int mods);
static int ckr_on_close(void);
static void ck_ring_stop(void);

/* ---- saving ---- */
static void ck_save(void) {
    char *mb = NULL; size_t ml = 0; FILE *f = open_memstream(&mb, &ml); if (!f) return;
    for (int i = 0; i < ck.nal; i++) fprintf(f, "A %d %d %d %d %s\n", ck_al[i].hh, ck_al[i].mm, ck_al[i].days, ck_al[i].on, ck_al[i].label);
    for (int i = 0; i < ck.nz; i++) fprintf(f, "W %s\n", ck.zones[i]);
    fprintf(f, "Z\n");                                  /* (the world list was set: don't add the defaults again) */
    fclose(f);
    if (mb && conf_persistent) aw_save(CK_FILE, mb, ml, 1, "the clock");
    free(mb);
}
static void ck_load(void) {
    if (ck.loaded) return;
    ck.loaded = 1;
    FILE *f = fopen(CK_FILE, "r"); int seen_z = 0;
    char l[200];
    while (f && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        if (l[0] == 'A' && ck.nal < CK_MAXAL) {
            int hh, mm, d, on, k = 0;
            if (sscanf(l, "A %d %d %d %d %n", &hh, &mm, &d, &on, &k) >= 4 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
                ck_al[ck.nal].hh = hh; ck_al[ck.nal].mm = mm; ck_al[ck.nal].days = d & 127; ck_al[ck.nal].on = !!on;
                snprintf(ck_al[ck.nal].label, sizeof ck_al[0].label, "%s", k ? l + k : "");
                ck.nal++;
            }
        } else if (l[0] == 'W' && l[1] == ' ' && ck.nz < 8) snprintf(ck.zones[ck.nz++], 48, "%s", l + 2);
        else if (l[0] == 'Z') seen_z = 1;
    }
    if (f) fclose(f);
    if (!seen_z && !ck.nz) {
        static const char *def[3] = { "Europe/London", "America/New_York", "Asia/Tokyo" };
        for (int i = 0; i < 3; i++) snprintf(ck.zones[ck.nz++], 48, "%s", def[i]);
    }
}

/* ---- time helpers ---- */
static void ck_hm(int hh, int mm, char *b, size_t n) {
    if (set_clock24) snprintf(b, n, "%02d:%02d", hh, mm);
    else snprintf(b, n, "%d:%02d %s", (hh + 11) % 12 + 1, mm, hh < 12 ? "AM" : "PM");
}
static void ck_dur(long long ms, int hundredths, char *b, size_t n) {
    if (ms < 0) ms = 0;
    long long s = ms / 1000; int h = (int)(s / 3600), m = (int)(s / 60 % 60), sec = (int)(s % 60);
    if (hundredths) {
        if (h) snprintf(b, n, "%d:%02d:%02d.%02d", h, m, sec, (int)(ms / 10 % 100));
        else snprintf(b, n, "%02d:%02d.%02d", m, sec, (int)(ms / 10 % 100));
    } else if (h) snprintf(b, n, "%d:%02d:%02d", h, m, sec);
    else snprintf(b, n, "%02d:%02d", m, sec);
}
static long long ck_sw_now(void) { return ck.sw_acc + (ck.sw_run ? ui_ms() - ck.sw_start : 0); }
static long long ck_t_left(void) { return ck.t_run ? ck.t_end - ui_ms() : ck.t_left; }
/* the time in another zone (TZ for a moment, then back) */
static int ck_zone_tm(const char *zone, struct tm *out) {
    const char *was = getenv("TZ"); char keep[64] = ""; if (was) snprintf(keep, sizeof keep, "%s", was);
    char z[64]; snprintf(z, sizeof z, ":%s", zone);
    setenv("TZ", z, 1); tzset();
    time_t now = time(NULL); localtime_r(&now, out);
    if (keep[0]) setenv("TZ", keep, 1); else unsetenv("TZ");
    tzset();
    return 1;
}

/* big digits: the smooth 16x32 font, scaled to height px */
static void ck_big(int x, int y, const char *s, uint32_t c, int px) {
    float sc = px / 32.0f; int gw = (int)(16 * sc + 0.5f);
    for (const char *p = s; *p; p++, x += gw) {
        int ch = (unsigned char)*p;
        if (!set_smooth) { draw_text_scaled(x, y, (char[2]){ *p, 0 }, c, imax(1, px / 16)); continue; }
        const unsigned char *g = sf2_reg + ch * 512;
        for (int j = imax(0, clip_y0 - y); j < px && y + j < clip_y1; j++)
            for (int i = imax(0, clip_x0 - x); i < gw && x + i < clip_x1; i++) {
                float fx = (i + 0.5f) / sc - 0.5f, fy = (j + 0.5f) / sc - 0.5f;
                int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float ax = fx - x0, ay = fy - y0;
                float v = 0;
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                    int xx = x0 + dx, yy = y0 + dy;
                    if (xx < 0 || yy < 0 || xx >= 16 || yy >= 32) continue;
                    v += g[yy * 16 + xx] * (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
                }
                if (v > 2) blend(x + i, y + j, c, (int)v);
            }
    }
}
static int ck_big_w(const char *s, int px) { return (int)strlen(s) * (int)(16 * px / 32.0f + 0.5f); }
static void ck_arc(float cx, float cy, float r, float from, float to, float wdt, uint32_t c) {   /* angles in turns, 0 = top, clockwise */
    float r0 = r - wdt / 2, r1 = r + wdt / 2;
    for (int y = (int)(cy - r1) - 1; y <= (int)(cy + r1) + 1; y++)
        for (int x = (int)(cx - r1) - 1; x <= (int)(cx + r1) + 1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            if (d < r0 - 1 || d > r1 + 1) continue;
            float a = atan2f(dx, -dy) / 6.2831853f; if (a < 0) a += 1;
            if (a < from || a > to) continue;
            float cov = fminf(1, fminf(d - (r0 - 1), (r1 + 1) - d));
            if (cov > 0) blend(x, y, c, (int)(255 * cov));
        }
}

/* ---- ringing: alarms and the timer ---- */
static void ck_ring_sound(void) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl("/bin/playsound", "playsound", "/usr/share/sounds/alarm.wav", (char *)NULL);
        _exit(0);
    }
    if (p > 0) ck.ring_pid = p;
}
static void ck_ring_start(int kind, const char *title, const char *text) {
    ck.ring = kind;
    snprintf(ck.ring_title, sizeof ck.ring_title, "%s", title);
    snprintf(ck.ring_text, sizeof ck.ring_text, "%s", text);
    ck.ring_sound = 0; ck.ring_until = ui_ms() + 5 * 60 * 1000;     /* five minutes at most */
    nc_add(ICON_CLOCKAPP, title, text);
    int w = win_of(ckr_draw);
    static const Win t = { .title = "Clock", .icon = ICON_CLOCKAPP, .no_max = 1, .draw = ckr_draw, .click = ckr_click, .hit = ckr_hit,
                           .key = ckr_key, .on_close = ckr_on_close };
    if (w < 0) open_win(&t, 380, 210);
    else { restore_win(w); repaint_client(ckr_draw); }
}
static void ck_ring_stop(void) {
    ck.ring = 0;
    if (ck.ring_pid > 0) { kill(-ck.ring_pid, SIGTERM); kill(ck.ring_pid, SIGTERM); ck.ring_pid = 0; }
    int w = win_of(ckr_draw);
    if (w >= 0) close_win(w);
    if (win_of(clockapp_draw) >= 0) repaint_client(clockapp_draw);
}
static void ck_snooze(void) {                          /* 5 minutes: a one-time alarm */
    time_t t = time(NULL) + 5 * 60; struct tm tm; localtime_r(&t, &tm);
    if (ck.nal < CK_MAXAL) {
        ck_al[ck.nal] = (typeof(ck_al[0])){ tm.tm_hour, tm.tm_min, 0, 1, "", 0 };
        snprintf(ck_al[ck.nal].label, sizeof ck_al[0].label, "Snoozed: %.28s", ck.ring_text);
        ck.nal++; ck_save();
    }
    ck_ring_stop();
    toast(ICON_CLOCKAPP, "Snoozed", "The alarm rings again in 5 minutes.");
    if (win_of(clockapp_draw) >= 0) repaint_client(clockapp_draw);
}
/* once a second (from the main loop, also when Clock is closed) */
static void ck_tick(void) {
    if (!ck.loaded) { if (!conf_persistent) return; ck_load(); }
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    long key = (long)(now / 60);
    int wd = (tm.tm_wday + 6) % 7, changed = 0;
    for (int i = 0; i < ck.nal; i++) {
        if (!ck_al[i].on || ck_al[i].hh != tm.tm_hour || ck_al[i].mm != tm.tm_min || ck_al[i].fired == key) continue;
        if (ck_al[i].days && !(ck_al[i].days & (1 << wd))) continue;
        ck_al[i].fired = key;
        char b[64], hm[16]; ck_hm(ck_al[i].hh, ck_al[i].mm, hm, sizeof hm);
        snprintf(b, sizeof b, "%s%s%s", hm, ck_al[i].label[0] ? " - " : "", ck_al[i].label);
        if (!ck_al[i].days) {                            /* a one-time alarm: done (snoozes go away) */
            if (!strncmp(ck_al[i].label, "Snoozed:", 8)) { memmove(ck_al + i, ck_al + i + 1, sizeof ck_al[0] * (size_t)(ck.nal - i - 1)); ck.nal--; i--; }
            else ck_al[i].on = 0;
            changed = 1;
        }
        ck_ring_start(1, "Alarm", b);
    }
    if (changed) { ck_save(); if (win_of(clockapp_draw) >= 0) repaint_client(clockapp_draw); }
    if (ck.ring && ui_ms() > ck.ring_until) ck_ring_stop();
}
/* how soon the main loop should come back (the stopwatch and timer move; the timer ends) */
static int ck_frame(void) {
    long long now = ui_ms();
    int want = -1;
    if (ck.t_run) {
        long long left = ck.t_end - now;
        if (left <= 0) {
            ck.t_run = 0; ck.t_left = 0;
            char b[48], d[24]; ck_dur(ck.t_total, 0, d, sizeof d); snprintf(b, sizeof b, "%s timer finished", d);
            ck_ring_start(2, "Time's up", b);
            if (win_of(clockapp_draw) >= 0) repaint_client(clockapp_draw);
        } else want = (int)imin(left, 250);
    }
    if (ck.ring) {
        if (now >= ck.ring_sound) { ck_ring_sound(); ck.ring_sound = now + 2400; }
        int d = (int)(ck.ring_sound - now); if (want < 0 || d < want) want = d;
    }
    int w = win_of(clockapp_draw);
    if (w >= 0 && !wins[w].minimized && ((ck.tab == CT_STOP && ck.sw_run) || (ck.tab == CT_TIMER && ck.t_run))) {
        int every = ck.tab == CT_STOP ? 40 : 200;
        if (now - ck.last_frame >= every) { ck.last_frame = now; repaint_client(clockapp_draw); }
        if (want < 0 || every < want) want = every;
    }
    return want;
}

/* ---- the window ---- */
enum { CB_TAB = 1, CB_AL_ROW = 100, CB_AL_SW = 200, CB_AL_DEL = 300, CB_AL_DAY = 400 /* +row*8+day */, CB_AL_TIME = 600, CB_AL_LABEL, CB_AL_ADD,
       CB_T_PRESET = 700, CB_T_FIELD = 720, CB_T_START, CB_T_RESET, CB_SW_START = 740, CB_SW_LAP, CB_W_FIELD = 760, CB_W_DEL = 780, CB_W_SUG = 800 };
#define CK_AL_ROWH 64
#define CK_AL_Y 70
static int ck_al_rows(int ch) { return (ch - CK_AL_Y - 110) / CK_AL_ROWH; }
static const int ck_presets[6] = { 1, 3, 5, 10, 15, 30 };
static int ck_sug[5], ck_nsug;
static void ck_find_cities(void) {
    ck_nsug = 0;
    if (!ck.f_city[0]) return;
    if (!ntz) tz_load();
    for (int i = 0; i < ntz && ck_nsug < 5; i++)
        if (strcasestr(tzl[i].city, ck.f_city) || strcasestr(tzl[i].country, ck.f_city)) ck_sug[ck_nsug++] = i;
}
static void ck_btn(int x, int y, int w, int h, const char *l, int primary, int hv) {
    const Theme *t = TH();
    uint32_t acc = t->accent;
    if (primary) fill_round(x, y, w, h, h / 2, hv ? mix(acc, RGB(0, 0, 0), 0.12f) : acc, 255);
    else { fill_round(x, y, w, h, h / 2, hv ? RGB(0xe4, 0xea, 0xf2) : RGB(0xee, 0xf1, 0xf5), 255); stroke_round(x, y, w, h, h / 2, RGB(0xd4, 0xda, 0xe2), 255); }
    draw_text(x + (w - text_w(l)) / 2, y + (h - 16) / 2, l, primary ? RGB(255, 255, 255) : RGB(0x1e, 0x22, 0x28), 255);
}
static void ck_field(int x, int y, int w, const char *v, const char *ph, int focus) {
    fill_round(x, y, w, 32, 6, RGB(255, 255, 255), 255);
    stroke_round(x, y, w, 32, 6, focus ? TH()->accent : RGB(0xc4, 0xcc, 0xd8), 255);
    char b[48]; snprintf(b, sizeof b, "%s", v);
    while (b[0] && text_w(b) > w - 20) memmove(b, b + 1, strlen(b));
    draw_text(x + 10, y + 8, b[0] ? b : (focus ? "" : ph), b[0] ? RGB(0x1e, 0x22, 0x28) : RGB(0x90, 0x98, 0xa4), 255);
    if (focus) fill_rect(x + 11 + text_w(b), y + 8, 1, 16, RGB(0x1e, 0x22, 0x28), 255);
}
static void clockapp_draw(int cx, int cy, int cw, int ch) {
    const int dark = 0;   /* (dark mode darkens this window itself) */
    (void)dark;
    uint32_t bg = RGB(255, 255, 255), text = RGB(0x1e, 0x22, 0x28), grey = RGB(0x70, 0x78, 0x84), line = RGB(0xe2, 0xe6, 0xec), acc = TH()->accent;
    fill_rect(cx, cy, cw, ch, bg, 255);
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == clockapp_draw ? hot.b : -1;
    /* tabs */
    for (int i = 0, x = cx + 20; i < CT_N; i++) {
        int w = text_w(ck_tabs[i]) + 34, on = ck.tab == i;
        fill_round(x, cy + 16, w, 34, 17, on ? acc : hv == CB_TAB + i ? RGB(0xe8, 0xee, 0xf6) : RGB(0xf2, 0xf4, 0xf7), 255);
        draw_text(x + 17, cy + 25, ck_tabs[i], on ? RGB(255, 255, 255) : text, 255);
        x += w + 8;
    }
    char b[64];
    switch (ck.tab) {
    case CT_ALARM: {
        int rows = ck_al_rows(ch);
        if (!ck.nal) draw_text(cx + 24, cy + CK_AL_Y + 16, "No alarms yet. Add one below.", grey, 255);
        static const char *dl = "MTWTFSS";
        for (int r = 0; r < rows && ck.scroll + r < ck.nal; r++) {
            int i = ck.scroll + r, y = cy + CK_AL_Y + r * CK_AL_ROWH;
            fill_round(cx + 16, y, cw - 32, CK_AL_ROWH - 8, 10, RGB(0xf5, 0xf7, 0xfa), 255);
            ck_hm(ck_al[i].hh, ck_al[i].mm, b, sizeof b);
            draw_text_scaled(cx + 30, y + 6, b, ck_al[i].on ? text : grey, 2);
            char sub[80];
            if (!ck_al[i].days) snprintf(sub, sizeof sub, "Once%s%s", ck_al[i].label[0] ? " - " : "", ck_al[i].label);
            else if (ck_al[i].days == 127) snprintf(sub, sizeof sub, "Every day%s%s", ck_al[i].label[0] ? " - " : "", ck_al[i].label);
            else snprintf(sub, sizeof sub, "%s", ck_al[i].label[0] ? ck_al[i].label : "Repeats");
            while (sub[0] && text_w(sub) > 200) sub[u8_prev(sub, (int)strlen(sub))] = 0;
            draw_text(cx + 30, y + 38, sub, grey, 255);
            for (int d = 0; d < 7; d++) {                 /* the days: click to repeat on them */
                int dx = cx + 262 + d * 30, on = ck_al[i].days & (1 << d), h2 = hv == CB_AL_DAY + r * 8 + d;
                fill_circle(dx + 12, y + 28, 12, on ? acc : h2 ? RGB(0xdc, 0xe4, 0xee) : RGB(0xe8, 0xec, 0xf1), 255);
                char dc[2] = { dl[d], 0 };
                draw_text(dx + 8, y + 20, dc, on ? RGB(255, 255, 255) : text, 255);
            }
            int sx = cx + cw - 110, sy = y + 16;            /* on / off switch */
            fill_round(sx, sy, 44, 24, 12, ck_al[i].on ? acc : RGB(0xc4, 0xcc, 0xd8), 255);
            fill_circle(ck_al[i].on ? sx + 32 : sx + 12, sy + 12, 9, RGB(255, 255, 255), 255);
            int dh = hv == CB_AL_DEL + r, dx = cx + cw - 52;
            if (dh) fill_round(dx - 4, y + 16, 26, 24, 5, RGB(0xc0, 0x30, 0x30), 255);
            aa_line(dx + 3, y + 22, dx + 15, y + 34, 1.5f, dh ? RGB(255, 255, 255) : grey);
            aa_line(dx + 15, y + 22, dx + 3, y + 34, 1.5f, dh ? RGB(255, 255, 255) : grey);
        }
        if (ck.nal > rows) { snprintf(b, sizeof b, "%d more - scroll", ck.nal - rows - ck.scroll > 0 ? ck.nal - rows - ck.scroll : 0); if (ck.nal - rows - ck.scroll > 0) draw_text(cx + cw - 30 - text_w(b), cy + CK_AL_Y + rows * CK_AL_ROWH - 4, b, grey, 255); }
        int fy = cy + ch - 92;
        fill_rect(cx + 16, fy - 12, cw - 32, 1, line, 255);
        draw_text_bold(cx + 24, fy, "New alarm", text);
        ck_field(cx + 24, fy + 26, 100, ck.f_time, set_clock24 ? "07:00" : "7:00", ck.field == 1);
        ck_field(cx + 134, fy + 26, cw - 134 - 24 - 150, ck.f_label, "Name (optional)", ck.field == 2);
        ck_btn(cx + cw - 24 - 140, fy + 26, 140, 32, "Add alarm", 1, hv == CB_AL_ADD);
        if (ck.msg[0]) draw_text(cx + 134, fy, ck.msg, RGB(0xc0, 0x40, 0x30), 255);
        break; }
    case CT_TIMER: {
        float rx = cx + cw / 2.0f, ry = cy + 190, rr = 105;
        long long left = ck_t_left(); if (left < 0) left = 0;
        ck_arc(rx, ry, rr, 0, 1, 10, RGB(0xe8, 0xec, 0xf1));
        if (ck.t_total > 0 && left > 0) ck_arc(rx, ry, rr, 1 - (float)left / ck.t_total, 1, 10, acc);
        ck_dur(ck.t_total ? left + 999 : 0, 0, b, sizeof b);
        ck_big((int)rx - ck_big_w(b, 56) / 2, (int)ry - 30, b, text, 56);
        const char *st = ck.t_run ? "Running" : ck.t_total && left > 0 && left < ck.t_total ? "Paused" : ck.t_total ? "Ready" : "Pick a time";
        draw_text((int)rx - text_w(st) / 2, (int)ry + 34, st, grey, 255);
        int py = cy + 320;
        for (int i = 0; i < 6; i++) {
            snprintf(b, sizeof b, "%d min", ck_presets[i]);
            ck_btn(cx + 24 + i * 78, py, 70, 30, b, 0, hv == CB_T_PRESET + i);
        }
        ck_field(cx + 24 + 6 * 78 + 6, py - 1, cw - (24 + 6 * 78 + 6) - 24, ck.f_timer, "mm:ss", ck.field == 3);
        int by = cy + ch - 70;
        ck_btn(cx + cw / 2 - 150, by, 140, 40, ck.t_run ? "Pause" : "Start", 1, hv == CB_T_START);
        ck_btn(cx + cw / 2 + 10, by, 140, 40, "Reset", 0, hv == CB_T_RESET);
        if (ck.msg[0]) draw_text(cx + 24, py + 40, ck.msg, RGB(0xc0, 0x40, 0x30), 255);
        break; }
    case CT_STOP: {
        long long t = ck_sw_now();
        ck_dur(t, 1, b, sizeof b);
        ck_big(cx + (cw - ck_big_w(b, 72)) / 2, cy + 90, b, text, 72);
        int by = cy + 190;
        ck_btn(cx + cw / 2 - 150, by, 140, 40, ck.sw_run ? "Stop" : t ? "Continue" : "Start", 1, hv == CB_SW_START);
        ck_btn(cx + cw / 2 + 10, by, 140, 40, ck.sw_run ? "Lap" : "Reset", 0, hv == CB_SW_LAP);
        int ly = cy + 250;
        if (ck.nlaps) {
            fill_rect(cx + 60, ly, cw - 120, 1, line, 255);
            draw_text(cx + 70, ly + 8, "Lap", grey, 255); draw_text(cx + cw / 2 - 40, ly + 8, "Lap time", grey, 255); draw_text(cx + cw - 70 - text_w("Total"), ly + 8, "Total", grey, 255);
            long long fast = -1, slow = -1;
            for (int i = 0; i < ck.nlaps; i++) { long long d = ck.laps[i] - (i ? ck.laps[i - 1] : 0); if (fast < 0 || d < fast) fast = d; if (d > slow) slow = d; }
            int shown = imin(ck.nlaps, (ch - (ly - cy) - 40) / 24);
            for (int k = 0; k < shown; k++) {
                int i = ck.nlaps - 1 - k, yy = ly + 32 + k * 24;
                long long d = ck.laps[i] - (i ? ck.laps[i - 1] : 0);
                uint32_t c = ck.nlaps > 2 && d == fast ? RGB(0x1a, 0x8a, 0x40) : ck.nlaps > 2 && d == slow ? RGB(0xc0, 0x40, 0x30) : text;
                snprintf(b, sizeof b, "%d", i + 1); draw_text(cx + 70, yy, b, c, 255);
                ck_dur(d, 1, b, sizeof b); draw_text(cx + cw / 2 - 40, yy, b, c, 255);
                ck_dur(ck.laps[i], 1, b, sizeof b); draw_text(cx + cw - 70 - text_w(b), yy, b, c, 255);
            }
        }
        break; }
    case CT_WORLD: {
        time_t now = time(NULL); struct tm here; localtime_r(&now, &here);
        int y = cy + 70;
        /* here first */
        {
            fill_round(cx + 16, y, cw - 32, 66, 10, RGB(0xee, 0xf4, 0xfd), 255);
            ck_hm(here.tm_hour, here.tm_min, b, sizeof b);
            draw_text_scaled(cx + cw - 30 - text_w(b) * 2, y + 17, b, text, 2);
            char nm[96]; tz_pretty(set_tz, nm, sizeof nm);
            draw_text_bold(cx + 30, y + 14, "Here", text);
            draw_text(cx + 30, y + 36, nm, grey, 255);
            y += 74;
        }
        for (int i = 0; i < ck.nz; i++, y += 66) {
            struct tm t; ck_zone_tm(ck.zones[i], &t);
            fill_round(cx + 16, y, cw - 32, 58, 10, RGB(0xf5, 0xf7, 0xfa), 255);
            char nm[96]; tz_pretty(ck.zones[i], nm, sizeof nm);
            char *comma = strchr(nm, ','); if (comma) *comma = 0;
            draw_text_bold(cx + 30, y + 10, nm, text);
            int dd = (t.tm_year - here.tm_year) * 400 + t.tm_yday - here.tm_yday;
            long off = t.tm_gmtoff - here.tm_gmtoff;
            char rel[64], hrs[24];
            if (off % 3600) snprintf(hrs, sizeof hrs, "%+ld:%02ld h", off / 3600, labs(off % 3600) / 60);
            else snprintf(hrs, sizeof hrs, "%+ld h", off / 3600);
            snprintf(rel, sizeof rel, "%s, %s", dd > 0 ? "Tomorrow" : dd < 0 ? "Yesterday" : "Today", off ? hrs : "same time");
            draw_text(cx + 30, y + 31, rel, grey, 255);
            ck_hm(t.tm_hour, t.tm_min, b, sizeof b);
            draw_text_scaled(cx + cw - 70 - text_w(b) * 2, y + 13, b, text, 2);
            int dh = hv == CB_W_DEL + i, dx = cx + cw - 50;
            if (dh) fill_round(dx - 4, y + 16, 26, 24, 5, RGB(0xc0, 0x30, 0x30), 255);
            aa_line(dx + 3, y + 22, dx + 15, y + 34, 1.5f, dh ? RGB(255, 255, 255) : grey);
            aa_line(dx + 15, y + 22, dx + 3, y + 34, 1.5f, dh ? RGB(255, 255, 255) : grey);
        }
        if (ck.nz < 8) {
            int fy = cy + ch - 60;
            draw_text_bold(cx + 24, fy + 8, "Add a city", text);
            ck_field(cx + 140, fy, cw - 164, ck.f_city, "Type a city or country", ck.field == 4);
            if (ck.field == 4 && ck_nsug) {                /* suggestions, above the box */
                int sh = ck_nsug * 34 + 8, sy = fy - sh - 4;
                fill_round(cx + 140, sy, cw - 164, sh, 8, RGB(255, 255, 255), 255);
                stroke_round(cx + 140, sy, cw - 164, sh, 8, RGB(0xc4, 0xcc, 0xd8), 255);
                for (int k = 0; k < ck_nsug; k++) {
                    TzEnt *z = &tzl[ck_sug[k]];
                    if (hv == CB_W_SUG + k) fill_round(cx + 144, sy + 4 + k * 34, cw - 172, 32, 6, RGB(0xe8, 0xee, 0xf6), 255);
                    char l[96]; snprintf(l, sizeof l, "%s, %s", z->city, z->country);
                    draw_text(cx + 154, sy + 12 + k * 34, l, text, 255);
                }
            }
        }
        break; }
    }
}
static int clockapp_hit(int lx, int ly, int cw, int ch) {
    for (int i = 0, x = 20; i < CT_N; i++) { int w = text_w(ck_tabs[i]) + 34; if (lx >= x && lx < x + w && ly >= 16 && ly < 50) return CB_TAB + i; x += w + 8; }
    switch (ck.tab) {
    case CT_ALARM: {
        int rows = ck_al_rows(ch);
        for (int r = 0; r < rows && ck.scroll + r < ck.nal; r++) {
            int y = CK_AL_Y + r * CK_AL_ROWH;
            if (ly < y || ly >= y + CK_AL_ROWH - 8) continue;
            for (int d = 0; d < 7; d++) { int dx = 262 + d * 30; if (lx >= dx && lx < dx + 26) return CB_AL_DAY + r * 8 + d; }
            if (lx >= cw - 110 && lx < cw - 62) return CB_AL_SW + r;
            if (lx >= cw - 58 && lx < cw - 26) return CB_AL_DEL + r;
            return CB_AL_ROW + r;
        }
        int fy = ch - 92;
        if (ly >= fy + 26 && ly < fy + 58) {
            if (lx >= 24 && lx < 124) return CB_AL_TIME;
            if (lx >= 134 && lx < cw - 174) return CB_AL_LABEL;
            if (lx >= cw - 164 && lx < cw - 24) return CB_AL_ADD;
        }
        return -1; }
    case CT_TIMER: {
        int py = 320;
        if (ly >= py && ly < py + 30) for (int i = 0; i < 6; i++) if (lx >= 24 + i * 78 && lx < 94 + i * 78) return CB_T_PRESET + i;
        if (ly >= py - 1 && ly < py + 31 && lx >= 24 + 6 * 78 + 6 && lx < cw - 24) return CB_T_FIELD;
        int by = ch - 70;
        if (ly >= by && ly < by + 40) { if (lx >= cw / 2 - 150 && lx < cw / 2 - 10) return CB_T_START; if (lx >= cw / 2 + 10 && lx < cw / 2 + 150) return CB_T_RESET; }
        return -1; }
    case CT_STOP: {
        int by = 190;
        if (ly >= by && ly < by + 40) { if (lx >= cw / 2 - 150 && lx < cw / 2 - 10) return CB_SW_START; if (lx >= cw / 2 + 10 && lx < cw / 2 + 150) return CB_SW_LAP; }
        return -1; }
    case CT_WORLD: {
        int y = 70 + 74;
        for (int i = 0; i < ck.nz; i++, y += 66) if (ly >= y + 12 && ly < y + 44 && lx >= cw - 56 && lx < cw - 24) return CB_W_DEL + i;
        int fy = ch - 60;
        if (ck.field == 4 && ck_nsug) {
            int sh = ck_nsug * 34 + 8, sy = fy - sh - 4;
            if (lx >= 140 && lx < cw - 24 && ly >= sy + 4 && ly < sy + 4 + ck_nsug * 34) return CB_W_SUG + (ly - sy - 4) / 34;
        }
        if (ck.nz < 8 && ly >= fy && ly < fy + 32 && lx >= 140 && lx < cw - 24) return CB_W_FIELD;
        return -1; }
    }
    return -1;
}
static void clockapp_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(clockapp_draw), cx = 0, cy = 0, cw = CK_W, ch = CK_H;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    *x = cx; *y = cy; *w = cw; *h = ch; (void)id;
}
static int ck_parse_hm(const char *s, int *hh, int *mm) {
    int h, m = 0; char ap[4] = "";
    if (sscanf(s, "%d:%d %3s", &h, &m, ap) < 1 && sscanf(s, "%d", &h) != 1) return 0;
    if (!strchr(s, ':')) { if (sscanf(s, "%d %3s", &h, ap) < 1) return 0; m = 0; if (h >= 100) { m = h % 100; h /= 100; } }
    if ((ap[0] == 'p' || ap[0] == 'P') && h < 12) h += 12;
    if ((ap[0] == 'a' || ap[0] == 'A') && h == 12) h = 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
    *hh = h; *mm = m; return 1;
}
static void ck_add_alarm(void) {
    int hh, mm;
    if (!ck_parse_hm(ck.f_time[0] ? ck.f_time : "7:00", &hh, &mm)) { snprintf(ck.msg, sizeof ck.msg, "Type a time like 7:30 or 19:30"); return; }
    if (ck.nal >= CK_MAXAL) { snprintf(ck.msg, sizeof ck.msg, "That's a lot of alarms!"); return; }
    ck_al[ck.nal] = (typeof(ck_al[0])){ hh, mm, 0, 1, "", 0 };
    snprintf(ck_al[ck.nal].label, sizeof ck_al[0].label, "%s", ck.f_label);
    ck.nal++; ck_save();
    ck.f_time[0] = ck.f_label[0] = 0; ck.field = 0; ck.msg[0] = 0;
    /* how long until it rings */
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    int mins = (hh * 60 + mm) - (tm.tm_hour * 60 + tm.tm_min); if (mins <= 0) mins += 24 * 60;
    char b[64]; snprintf(b, sizeof b, "Rings in %d h %d min.", mins / 60, mins % 60);
    toast(ICON_CLOCKAPP, "Alarm set", b);
}
static void ck_timer_set(long long ms) {
    ck.t_run = 0; ck.t_total = ms; ck.t_left = ms; ck.msg[0] = 0;
}
static void ck_timer_from_field(void) {
    int m = 0, s = 0, h = 0;
    if (sscanf(ck.f_timer, "%d:%d:%d", &h, &m, &s) == 3) {}
    else if (sscanf(ck.f_timer, "%d:%d", &m, &s) == 2) h = 0;
    else if (sscanf(ck.f_timer, "%d", &m) == 1) { h = 0; s = 0; }
    else { snprintf(ck.msg, sizeof ck.msg, "Type minutes, like 25 or 2:30"); return; }
    long long ms = ((long long)h * 3600 + m * 60 + s) * 1000;
    if (ms <= 0 || ms > 24LL * 3600 * 1000) { snprintf(ck.msg, sizeof ck.msg, "Up to 24 hours"); return; }
    ck_timer_set(ms); ck.f_timer[0] = 0; ck.field = 0;
}
static void clockapp_click(int lx, int ly, int cw, int ch) {
    int id = clockapp_hit(lx, ly, cw, ch);
    int was_field = ck.field;
    if (!(id == CB_AL_TIME || id == CB_AL_LABEL || id == CB_T_FIELD || id == CB_W_FIELD || (id >= CB_W_SUG && id < CB_W_SUG + 5))) ck.field = 0;
    if (id >= CB_TAB && id < CB_TAB + CT_N) { ck.tab = id - CB_TAB; ck.scroll = 0; ck.msg[0] = 0; }
    else if (id >= CB_AL_DAY && id < CB_AL_DAY + 8 * 20) {
        int r = (id - CB_AL_DAY) / 8, d = (id - CB_AL_DAY) % 8, i = ck.scroll + r;
        if (i < ck.nal && d < 7) { ck_al[i].days ^= 1 << d; ck_al[i].on = 1; ck_save(); }
    }
    else if (id >= CB_AL_SW && id < CB_AL_SW + 20) { int i = ck.scroll + id - CB_AL_SW; if (i < ck.nal) { ck_al[i].on = !ck_al[i].on; ck_save(); } }
    else if (id >= CB_AL_DEL && id < CB_AL_DEL + 20) {
        int i = ck.scroll + id - CB_AL_DEL;
        if (i < ck.nal) { memmove(ck_al + i, ck_al + i + 1, sizeof ck_al[0] * (size_t)(ck.nal - i - 1)); ck.nal--; ck_save(); ck.scroll = imax(0, imin(ck.scroll, ck.nal - ck_al_rows(ch))); }
    }
    else if (id == CB_AL_TIME) ck.field = 1;
    else if (id == CB_AL_LABEL) ck.field = 2;
    else if (id == CB_AL_ADD) ck_add_alarm();
    else if (id >= CB_T_PRESET && id < CB_T_PRESET + 6) { ck_timer_set(ck_presets[id - CB_T_PRESET] * 60000LL); }
    else if (id == CB_T_FIELD) ck.field = 3;
    else if (id == CB_T_START) {
        if (was_field == 3 && ck.f_timer[0]) ck_timer_from_field();
        if (ck.t_run) { ck.t_left = ck.t_end - ui_ms(); ck.t_run = 0; }
        else if (ck.t_total > 0) { if (ck.t_left <= 0) ck.t_left = ck.t_total; ck.t_end = ui_ms() + ck.t_left; ck.t_run = 1; }
        else snprintf(ck.msg, sizeof ck.msg, "Pick a time first");
    }
    else if (id == CB_T_RESET) { ck.t_run = 0; ck.t_left = ck.t_total; }
    else if (id == CB_SW_START) {
        if (ck.sw_run) { ck.sw_acc += ui_ms() - ck.sw_start; ck.sw_run = 0; }
        else { ck.sw_start = ui_ms(); ck.sw_run = 1; }
    }
    else if (id == CB_SW_LAP) {
        if (ck.sw_run) { if (ck.nlaps < 60) ck.laps[ck.nlaps++] = ck_sw_now(); }
        else { ck.sw_acc = 0; ck.nlaps = 0; }
    }
    else if (id >= CB_W_DEL && id < CB_W_DEL + 8) {
        int i = id - CB_W_DEL;
        if (i < ck.nz) { memmove(ck.zones[i], ck.zones[i + 1], sizeof ck.zones[0] * (size_t)(ck.nz - i - 1)); ck.nz--; ck_save(); }
    }
    else if (id == CB_W_FIELD) { ck.field = 4; ck_find_cities(); }
    else if (id >= CB_W_SUG && id < CB_W_SUG + 5) {
        int k = id - CB_W_SUG;
        if (k < ck_nsug && ck.nz < 8) { snprintf(ck.zones[ck.nz++], 48, "%s", tzl[ck_sug[k]].zone); ck_save(); }
        ck.f_city[0] = 0; ck_nsug = 0; ck.field = 0;
    }
    repaint_client(clockapp_draw);
}
static void clockapp_key(int code, int mods) {
    if (!ck.field) return;
    if (code == KEY_ESC) { ck.field = 0; ck_nsug = 0; repaint_client(clockapp_draw); return; }
    if (code == KEY_TAB && ck.tab == CT_ALARM) { ck.field = ck.field == 1 ? 2 : 1; repaint_client(clockapp_draw); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER) {
        if (ck.field <= 2) ck_add_alarm();
        else if (ck.field == 3) ck_timer_from_field();
        else if (ck.field == 4 && ck_nsug && ck.nz < 8) { snprintf(ck.zones[ck.nz++], 48, "%s", tzl[ck_sug[0]].zone); ck_save(); ck.f_city[0] = 0; ck_nsug = 0; ck.field = 0; }
        repaint_client(clockapp_draw); return;
    }
    char *f = ck.field == 1 ? ck.f_time : ck.field == 2 ? ck.f_label : ck.field == 3 ? ck.f_timer : ck.f_city;
    size_t cap = ck.field == 1 ? sizeof ck.f_time : ck.field == 2 ? sizeof ck.f_label : ck.field == 3 ? sizeof ck.f_timer : sizeof ck.f_city;
    if (code == KEY_BACKSPACE) { int l = (int)strlen(f); if (l) f[u8_prev(f, l)] = 0; }
    else {
        char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
        if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
        if ((ck.field == 1 || ck.field == 3) && !((t[0] >= '0' && t[0] <= '9') || t[0] == ':' || t[0] == '.' || (ck.field == 1 && strchr("apmAPM ", t[0])))) return;
        if (t[0] == '.') t[0] = ':';
        size_t l = strlen(f);
        if (l + (size_t)k < cap) { memcpy(f + l, t, (size_t)k); f[l + (size_t)k] = 0; }
    }
    if (ck.field == 4) ck_find_cities();
    ck.msg[0] = 0;
    repaint_client(clockapp_draw);
}
static void clockapp_wheel(int amount) {
    if (ck.tab != CT_ALARM) return;
    int w = win_of(clockapp_draw); int cx, cy, cw, ch; if (w < 0) return; client_rect(&wins[w], &cx, &cy, &cw, &ch);
    int ns = clampi(ck.scroll - amount, 0, imax(0, ck.nal - ck_al_rows(ch)));
    if (ns != ck.scroll) { ck.scroll = ns; repaint_client(clockapp_draw); }
}
static void clockapp_open(void) {
    ck_load();
    static const Win t = { .title = "Clock", .icon = ICON_CLOCKAPP, .no_max = 1, .draw = clockapp_draw, .click = clockapp_click,
                           .hit = clockapp_hit, .elem_rect = clockapp_elem_rect, .key = clockapp_key, .wheel = clockapp_wheel };
    open_win(&t, CK_W, CK_H);
}

/* ---- the ringing window ---- */
static void ckr_draw(int cx, int cy, int cw, int ch) {
    fill_rect(cx, cy, cw, ch, RGB(255, 255, 255), 255);
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == ckr_draw ? hot.b : -1;
    /* a bell that wobbles */
    float a = sinf(ui_ms() / 90.0f) * 0.25f;
    float bx = cx + 52, by = cy + 60;
    fill_circle(bx + sinf(a) * 4, by - 4, 16, TH()->accent, 255);
    fill_rect((int)(bx - 16 + sinf(a) * 6), (int)by - 4, 32, 18, TH()->accent, 255);
    fill_round((int)(bx - 21 + sinf(a) * 7), (int)by + 12, 42, 6, 3, TH()->accent, 255);
    fill_circle(bx + sinf(a) * 9, by + 22, 5, TH()->accent, 255);
    draw_text_scaled(cx + 96, cy + 30, ck.ring_title, RGB(0x1e, 0x22, 0x28), 2);
    char b[64]; snprintf(b, sizeof b, "%s", ck.ring_text);
    while (b[0] && text_w(b) > cw - 110) b[u8_prev(b, (int)strlen(b))] = 0;
    draw_text(cx + 96, cy + 72, b, RGB(0x70, 0x78, 0x84), 255);
    int by2 = cy + ch - 62;
    if (ck.ring == 1) {
        ck_btn(cx + 24, by2, (cw - 60) / 2, 40, "Snooze 5 min", 0, hv == 2);
        ck_btn(cx + 36 + (cw - 60) / 2, by2, (cw - 60) / 2, 40, "Stop", 1, hv == 1);
    } else ck_btn(cx + 24, by2, cw - 48, 40, "Stop", 1, hv == 1);
}
static int ckr_hit(int lx, int ly, int cw, int ch) {
    int by = ch - 62;
    if (ly < by || ly >= by + 40) return -1;
    if (ck.ring == 1) { if (lx >= 24 && lx < 24 + (cw - 60) / 2) return 2; if (lx >= 36 + (cw - 60) / 2 && lx < cw - 24) return 1; return -1; }
    return lx >= 24 && lx < cw - 24 ? 1 : -1;
}
static void ckr_click(int lx, int ly, int cw, int ch) {
    int id = ckr_hit(lx, ly, cw, ch);
    if (id == 1) ck_ring_stop();
    else if (id == 2) ck_snooze();
}
static void ckr_key(int code, int mods) { (void)mods; if (code == KEY_ESC || code == KEY_ENTER || code == KEY_SPACE) ck_ring_stop(); }
static int ckr_on_close(void) { ck.ring = 0; if (ck.ring_pid > 0) { kill(-ck.ring_pid, SIGTERM); ck.ring_pid = 0; } return 1; }

/* the app icon: a round clock */
static void draw_clockapp_icon(int cx, int cy, int r) {
    fill_circle(cx, cy, r, RGB(0x2b, 0x31, 0x3a), 255);
    fill_circle(cx, cy, r * 0.84f, RGB(0xfa, 0xfb, 0xfd), 255);
    for (int k = 0; k < 12; k++) {
        float a = k * 6.2831853f / 12, r0 = r * (k % 3 ? 0.70f : 0.62f), r1 = r * 0.76f;
        aa_line(cx + r0 * sinf(a), cy - r0 * cosf(a), cx + r1 * sinf(a), cy - r1 * cosf(a), imax(1, r / 14), RGB(0x60, 0x68, 0x74));
    }
    aa_line(cx, cy, cx + r * 0.36f * sinf(5.4f), cy - r * 0.36f * cosf(5.4f), imax(2, r / 8), RGB(0x2b, 0x31, 0x3a));
    aa_line(cx, cy, cx + r * 0.58f * sinf(1.05f), cy - r * 0.58f * cosf(1.05f), imax(1.5f, r / 10.0f), RGB(0x2b, 0x31, 0x3a));
    aa_line(cx, cy + r * 0.12f, cx, cy - r * 0.62f, imax(1, r / 16), RGB(0xe0, 0x3c, 0x3c));
    fill_circle(cx, cy, imax(2, r / 9), RGB(0xe0, 0x3c, 0x3c), 255);
}
