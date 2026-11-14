/* ===================================================================
 *  Calendar: a month, and what's on each day. Add an event with a time
 *  and a name; at that time ccos shows a notification (a reminder),
 *  even when Calendar is closed. Saved in D:/calendar.txt.
 * =================================================================== */
#define CAL_W 780
#define CAL_H 500
#define CAL_FILE "/data/calendar.txt"
#define CAL_MAX 400
static struct { int y, m, d, hh, mm, done; char title[80]; } cal_ev[CAL_MAX];
static int cal_n, cal_loaded;
static struct { int y, m, sel_d, field; char time[8], title[80]; char msg[64]; } cal;   /* field: 0 none, 1 time, 2 name */
static const char *cal_months[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static void calendar_draw(int cx, int cy, int cw, int ch);
static void cal_load(void) {
    cal_loaded = 1; cal_n = 0;
    FILE *f = fopen(CAL_FILE, "r"); if (!f) return;
    char l[200];
    while (cal_n < CAL_MAX && fgets(l, sizeof l, f)) {
        int y, m, d, hh, mm, done = 0; char t[100] = "";
        if (sscanf(l, "%d-%d-%d %d:%d %d %99[^\n]", &y, &m, &d, &hh, &mm, &done, t) < 6) continue;
        cal_ev[cal_n].y = y; cal_ev[cal_n].m = m; cal_ev[cal_n].d = d; cal_ev[cal_n].hh = hh; cal_ev[cal_n].mm = mm; cal_ev[cal_n].done = done;
        snprintf(cal_ev[cal_n].title, sizeof cal_ev[0].title, "%s", t);
        cal_n++;
    }
    fclose(f);
}
static void cal_save(void) {
    mkdir("/data", 0755);
    char *mb = NULL; size_t ml = 0; FILE *f = open_memstream(&mb, &ml); if (!f) return;
    for (int i = 0; i < cal_n; i++)
        fprintf(f, "%04d-%02d-%02d %02d:%02d %d %s\n", cal_ev[i].y, cal_ev[i].m, cal_ev[i].d, cal_ev[i].hh, cal_ev[i].mm, cal_ev[i].done, cal_ev[i].title);
    fclose(f);
    if (mb) aw_save(CAL_FILE, mb, ml, 1, "the calendar");
    free(mb); bg_sync();
}
static int cal_cmp(const void *a, const void *b) {
    const typeof(cal_ev[0]) *x = a, *y = b;
    long kx = ((((long)x->y * 13 + x->m) * 32 + x->d) * 24 + x->hh) * 60 + x->mm, ky = ((((long)y->y * 13 + y->m) * 32 + y->d) * 24 + y->hh) * 60 + y->mm;
    return kx < ky ? -1 : kx > ky;
}
static int cal_days(int y, int m) { static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1]; }
static int cal_wday(int y, int m, int d) {          /* 0 = Monday */
    struct tm tm = { 0 }; tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12; mktime(&tm);
    return (tm.tm_wday + 6) % 7;
}
static int cal_has(int y, int m, int d) { for (int i = 0; i < cal_n; i++) if (cal_ev[i].y == y && cal_ev[i].m == m && cal_ev[i].d == d) return 1; return 0; }

/* reminders: once a second, from the main loop (also when Calendar is closed) */
static void cal_tick(void) {
    if (!cal_loaded) { if (!conf_persistent) return; cal_load(); }
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    int changed = 0;
    for (int i = 0; i < cal_n; i++) {
        if (cal_ev[i].done) continue;
        struct tm e = { 0 }; e.tm_year = cal_ev[i].y - 1900; e.tm_mon = cal_ev[i].m - 1; e.tm_mday = cal_ev[i].d;
        e.tm_hour = cal_ev[i].hh; e.tm_min = cal_ev[i].mm; e.tm_isdst = -1;
        time_t t = mktime(&e);
        if (t <= now) {
            cal_ev[i].done = 1; changed = 1;
            if (now - t < 3600) {                         /* (not for ones missed long ago) */
                char b[40]; snprintf(b, sizeof b, set_clock24 ? "%02d:%02d" : "%d:%02d %s", set_clock24 ? cal_ev[i].hh : (cal_ev[i].hh + 11) % 12 + 1, cal_ev[i].mm, cal_ev[i].hh < 12 ? "AM" : "PM");
                char body[120]; snprintf(body, sizeof body, "%s - %s", b, cal_ev[i].title);
                toast(ICON_CALENDAR, "Reminder", body);
            }
        }
    }
    if (changed) { cal_save(); if (win_of(calendar_draw) >= 0) repaint_client(calendar_draw); }
}

/* ---- the window: the month on the left, the day on the right ---- */
#define CAL_GX 20
#define CAL_GY 64
#define CAL_CW 62
#define CAL_CH 58
#define CAL_RX (CAL_GX + 7 * CAL_CW + 24)
enum { CB_PREV = 1, CB_NEXT, CB_TODAY, CB_TIME, CB_TITLE, CB_ADD, CB_DAY = 100, CB_DEL = 200 };
static int cal_first_cell(void) { return cal_wday(cal.y, cal.m, 1); }
static void cal_day_events(int *idx, int *n) {
    *n = 0;
    for (int i = 0; i < cal_n && *n < 32; i++) if (cal_ev[i].y == cal.y && cal_ev[i].m == cal.m && cal_ev[i].d == cal.sel_d) idx[(*n)++] = i;
}
static void calendar_draw(int cx, int cy, int cw, int ch) {
    const int dark = 0;   /* (in dark mode, ccos darkens windows like this one itself) */
    uint32_t bg = dark ? RGB(0x20, 0x21, 0x24) : RGB(255, 255, 255), text = dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28);
    uint32_t grey = dark ? RGB(0x9a, 0xa0, 0xaa) : RGB(0x70, 0x78, 0x84), line = dark ? RGB(0x3a, 0x3c, 0x42) : RGB(0xe2, 0xe6, 0xec);
    uint32_t acc = RGB(0x1e, 0x78, 0xe0);
    fill_rect(cx, cy, cw, ch, bg, 255);
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == calendar_draw ? hot.b : -1;
    char t[40]; snprintf(t, sizeof t, "%s %d", cal_months[cal.m - 1], cal.y);
    draw_text_bold(cx + CAL_GX, cy + 22, t, text);
    struct { int id, x; const char *l; } nb[3] = { { CB_PREV, CAL_GX + 7 * CAL_CW - 112, "<" }, { CB_TODAY, CAL_GX + 7 * CAL_CW - 82, "Today" }, { CB_NEXT, CAL_GX + 7 * CAL_CW - 26, ">" } };
    for (int i = 0; i < 3; i++) {
        int w = nb[i].id == CB_TODAY ? 52 : 26;
        if (hv == nb[i].id) fill_round(cx + nb[i].x, cy + 16, w, 28, 5, dark ? RGB(0x34, 0x36, 0x3c) : RGB(0xe8, 0xee, 0xf6), 255);
        draw_text(cx + nb[i].x + (w - text_w(nb[i].l)) / 2, cy + 22, nb[i].l, text, 255);
    }
    static const char *wd[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    for (int i = 0; i < 7; i++) draw_text(cx + CAL_GX + i * CAL_CW + 8, cy + CAL_GY - 20, wd[i], grey, 255);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    int first = cal_first_cell(), nd = cal_days(cal.y, cal.m);
    for (int d = 1; d <= nd; d++) {
        int c = first + d - 1, x = cx + CAL_GX + (c % 7) * CAL_CW, y = cy + CAL_GY + (c / 7) * CAL_CH;
        int today = cal.y == tm.tm_year + 1900 && cal.m == tm.tm_mon + 1 && d == tm.tm_mday, sel = d == cal.sel_d;
        if (sel) fill_round(x + 2, y + 2, CAL_CW - 4, CAL_CH - 4, 6, dark ? RGB(0x2e, 0x44, 0x66) : RGB(0xdc, 0xea, 0xfb), 255);
        else if (hv == CB_DAY + d) fill_round(x + 2, y + 2, CAL_CW - 4, CAL_CH - 4, 6, dark ? RGB(0x2c, 0x2e, 0x34) : RGB(0xf2, 0xf5, 0xf9), 255);
        stroke_round(x + 2, y + 2, CAL_CW - 4, CAL_CH - 4, 6, line, 255);
        char b[4]; snprintf(b, sizeof b, "%d", d);
        if (today) { fill_circle(x + 18, y + 18, 12, acc, 255); draw_text(x + 18 - text_w(b) / 2, y + 10, b, RGB(255, 255, 255), 255); }
        else draw_text(x + 18 - text_w(b) / 2, y + 10, b, text, 255);
        if (cal_has(cal.y, cal.m, d)) fill_circle(x + CAL_CW / 2.0f, y + CAL_CH - 12, 3.5f, acc, 255);
    }
    /* the day on the right */
    int rx = cx + CAL_RX, rw = cw - CAL_RX - 20;
    fill_rect(rx - 12, cy + 12, 1, ch - 24, line, 255);
    char dt[48]; snprintf(dt, sizeof dt, "%s %d, %s", wd[cal_wday(cal.y, cal.m, cal.sel_d)], cal.sel_d, cal_months[cal.m - 1]);
    draw_text_bold(rx, cy + 22, dt, text);
    int idx[32], n; cal_day_events(idx, &n);
    if (!n) draw_text(rx, cy + 58, "Nothing planned.", grey, 255);
    for (int k = 0; k < n && k < 7; k++) {
        int i = idx[k], y = cy + 52 + k * 40;
        fill_round(rx, y, rw, 34, 6, dark ? RGB(0x2c, 0x2e, 0x34) : RGB(0xf2, 0xf5, 0xf9), 255);
        fill_rect(rx, y + 4, 3, 26, cal_ev[i].done ? grey : acc, 255);
        char tb[16]; snprintf(tb, sizeof tb, set_clock24 ? "%02d:%02d" : "%d:%02d%s", set_clock24 ? cal_ev[i].hh : (cal_ev[i].hh + 11) % 12 + 1, cal_ev[i].mm, set_clock24 ? "" : cal_ev[i].hh < 12 ? "am" : "pm");
        draw_text_bold(rx + 10, y + 9, tb, text);
        char tt[80]; snprintf(tt, sizeof tt, "%s", cal_ev[i].title);
        int room = rw - 30 - 10 - text_w(tb) - 14;
        while (tt[0] && text_w(tt) > room) tt[u8_prev(tt, (int)strlen(tt))] = 0;
        draw_text(rx + 24 + text_w(tb), y + 9, tt, text, 255);
        int on = hv == CB_DEL + k;
        if (on) fill_round(rx + rw - 28, y + 6, 22, 22, 4, RGB(0xc0, 0x30, 0x30), 255);
        aa_line(rx + rw - 22, y + 12, rx + rw - 12, y + 22, 1.4f, on ? RGB(255, 255, 255) : grey);
        aa_line(rx + rw - 12, y + 12, rx + rw - 22, y + 22, 1.4f, on ? RGB(255, 255, 255) : grey);
    }
    /* add one */
    int fy = cy + ch - 120;
    draw_text_bold(rx, fy, "Add to this day", text);
    int tw = 80;
    fill_round(rx, fy + 24, tw, 30, 5, dark ? RGB(0x2e, 0x30, 0x36) : RGB(255, 255, 255), 255);
    stroke_round(rx, fy + 24, tw, 30, 5, cal.field == 1 ? acc : (dark ? RGB(0x50, 0x52, 0x58) : RGB(0xc4, 0xcc, 0xd8)), 255);
    draw_text(rx + 8, fy + 31, cal.time[0] ? cal.time : (cal.field == 1 ? "" : "18:00"), cal.time[0] ? text : grey, 255);
    if (cal.field == 1) fill_rect(rx + 9 + text_w(cal.time), fy + 31, 1, 16, text, 255);
    fill_round(rx + tw + 8, fy + 24, rw - tw - 8, 30, 5, dark ? RGB(0x2e, 0x30, 0x36) : RGB(255, 255, 255), 255);
    stroke_round(rx + tw + 8, fy + 24, rw - tw - 8, 30, 5, cal.field == 2 ? acc : (dark ? RGB(0x50, 0x52, 0x58) : RGB(0xc4, 0xcc, 0xd8)), 255);
    {   char tt[80]; snprintf(tt, sizeof tt, "%s", cal.title);
        while (tt[0] && text_w(tt) > rw - tw - 28) memmove(tt, tt + u8_next(tt, (int)strlen(tt), 0), strlen(tt));
        draw_text(rx + tw + 16, fy + 31, tt[0] ? tt : (cal.field == 2 ? "" : "What's happening?"), tt[0] ? text : grey, 255);
        if (cal.field == 2) fill_rect(rx + tw + 17 + text_w(tt), fy + 31, 1, 16, text, 255); }
    fill_round(rx, fy + 62, 140, 32, 6, hv == CB_ADD ? RGB(0x16, 0x68, 0xd0) : acc, 255);
    draw_text(rx + (140 - text_w("Add reminder")) / 2, fy + 70, "Add reminder", RGB(255, 255, 255), 255);
    if (cal.msg[0]) draw_text(rx + 150, fy + 70, cal.msg, RGB(0xc0, 0x40, 0x30), 255);
}
static int calendar_hit(int lx, int ly, int cw, int ch) {
    if (ly >= 16 && ly < 44) {
        int b = CAL_GX + 7 * CAL_CW;
        if (lx >= b - 112 && lx < b - 86) return CB_PREV;
        if (lx >= b - 82 && lx < b - 30) return CB_TODAY;
        if (lx >= b - 26 && lx < b) return CB_NEXT;
    }
    if (lx >= CAL_GX && lx < CAL_GX + 7 * CAL_CW && ly >= CAL_GY) {
        int c = (lx - CAL_GX) / CAL_CW + (ly - CAL_GY) / CAL_CH * 7, d = c - cal_first_cell() + 1;
        if (d >= 1 && d <= cal_days(cal.y, cal.m)) return CB_DAY + d;
    }
    int rx = CAL_RX, rw = cw - CAL_RX - 20, fy = ch - 120;
    int idx[32], n; cal_day_events(idx, &n);
    for (int k = 0; k < n && k < 7; k++) { int y = 52 + k * 40; if (lx >= rx + rw - 28 && lx < rx + rw - 6 && ly >= y + 6 && ly < y + 28) return CB_DEL + k; }
    if (ly >= fy + 24 && ly < fy + 54) { if (lx >= rx && lx < rx + 80) return CB_TIME; if (lx >= rx + 88 && lx < rx + rw) return CB_TITLE; }
    if (ly >= fy + 62 && ly < fy + 94 && lx >= rx && lx < rx + 140) return CB_ADD;
    return -1;
}
static void calendar_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(calendar_draw), cx = 0, cy = 0, cw = CAL_W, ch = CAL_H;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    *x = cx; *y = cy; *w = cw; *h = ch;                  /* simple: the whole window */
    (void)id;
}
static void cal_add(void) {
    int hh, mm;
    const char *ts = cal.time[0] ? cal.time : "18:00";
    if (sscanf(ts, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        if (sscanf(ts, "%d", &hh) == 1 && hh >= 0 && hh <= 23 && !strchr(ts, ':')) mm = 0;
        else { snprintf(cal.msg, sizeof cal.msg, "Time like 18:30"); return; }
    }
    if (!cal.title[0]) { snprintf(cal.msg, sizeof cal.msg, "Give it a name"); return; }
    if (cal_n >= CAL_MAX) return;
    cal_ev[cal_n].y = cal.y; cal_ev[cal_n].m = cal.m; cal_ev[cal_n].d = cal.sel_d; cal_ev[cal_n].hh = hh; cal_ev[cal_n].mm = mm;
    struct tm e = { 0 }; e.tm_year = cal.y - 1900; e.tm_mon = cal.m - 1; e.tm_mday = cal.sel_d; e.tm_hour = hh; e.tm_min = mm; e.tm_isdst = -1;
    cal_ev[cal_n].done = mktime(&e) <= time(NULL);
    snprintf(cal_ev[cal_n].title, sizeof cal_ev[0].title, "%s", cal.title);
    cal_n++;
    qsort(cal_ev, (size_t)cal_n, sizeof cal_ev[0], cal_cmp);
    cal_save();
    cal.title[0] = 0; cal.time[0] = 0; cal.msg[0] = 0; cal.field = 0;
}
static void calendar_click(int lx, int ly, int cw, int ch) {
    int id = calendar_hit(lx, ly, cw, ch);
    cal.msg[0] = 0;
    if (id == CB_PREV || id == CB_NEXT) {
        cal.m += id == CB_NEXT ? 1 : -1;
        if (cal.m < 1) { cal.m = 12; cal.y--; } if (cal.m > 12) { cal.m = 1; cal.y++; }
        cal.sel_d = imin(cal.sel_d, cal_days(cal.y, cal.m));
    } else if (id == CB_TODAY) {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        cal.y = tm.tm_year + 1900; cal.m = tm.tm_mon + 1; cal.sel_d = tm.tm_mday;
    } else if (id > CB_DAY && id <= CB_DAY + 31) cal.sel_d = id - CB_DAY;
    else if (id >= CB_DEL && id < CB_DEL + 32) {
        int idx[32], n; cal_day_events(idx, &n);
        int k = id - CB_DEL;
        if (k < n) { int i = idx[k]; memmove(cal_ev + i, cal_ev + i + 1, sizeof cal_ev[0] * (size_t)(cal_n - i - 1)); cal_n--; cal_save(); }
    } else if (id == CB_TIME) cal.field = 1;
    else if (id == CB_TITLE) cal.field = 2;
    else if (id == CB_ADD) cal_add();
    else cal.field = 0;
    repaint_client(calendar_draw);
}
static void calendar_key(int code, int mods) {
    if (code == KEY_TAB) { cal.field = cal.field == 1 ? 2 : 1; repaint_client(calendar_draw); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER) { if (cal.field) cal_add(); repaint_client(calendar_draw); return; }
    if (code == KEY_ESC) { cal.field = 0; repaint_client(calendar_draw); return; }
    if (!cal.field) {
        if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN) {
            int d = cal.sel_d + (code == KEY_LEFT ? -1 : code == KEY_RIGHT ? 1 : code == KEY_UP ? -7 : 7);
            if (d >= 1 && d <= cal_days(cal.y, cal.m)) cal.sel_d = d;
            repaint_client(calendar_draw);
        }
        return;
    }
    char *f = cal.field == 1 ? cal.time : cal.title; size_t cap = cal.field == 1 ? sizeof cal.time : sizeof cal.title;
    if (code == KEY_BACKSPACE) { int l = (int)strlen(f); if (l) f[u8_prev(f, l)] = 0; repaint_client(calendar_draw); return; }
    char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
    if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
    if (cal.field == 1 && !((t[0] >= '0' && t[0] <= '9') || t[0] == ':' || t[0] == '.')) return;
    if (t[0] == '.') t[0] = ':';
    size_t l = strlen(f);
    if (l + (size_t)k < cap) { memcpy(f + l, t, (size_t)k); f[l + (size_t)k] = 0; }
    repaint_client(calendar_draw);
}
static void calendar_open(void) {
    if (!cal_loaded) cal_load();
    if (!cal.y || win_of(calendar_draw) < 0) {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        cal.y = tm.tm_year + 1900; cal.m = tm.tm_mon + 1; cal.sel_d = tm.tm_mday; cal.field = 0; cal.msg[0] = 0;
    }
    static const Win t = { .title = "Calendar", .icon = ICON_CALENDAR, .no_max = 1, .draw = calendar_draw, .click = calendar_click,
                           .hit = calendar_hit, .elem_rect = calendar_elem_rect, .key = calendar_key };
    open_win(&t, CAL_W, CAL_H);
}
static void draw_calendar_icon(int cx, int cy, int r) {
    int x = cx - r, y = cy - r + r / 6, w = 2 * r, h = 2 * r - r / 6;
    fill_round(x, y, w, h, imax(2, r / 4), RGB(0xff, 0xff, 0xff), 255);
    stroke_round(x, y, w, h, imax(2, r / 4), RGB(0x90, 0x98, 0xa4), 255);
    fill_round(x, y, w, h / 3, imax(2, r / 4), RGB(0xe0, 0x3c, 0x3c), 255);
    fill_rect(x, y + h / 3 - imax(2, r / 4), w, imax(2, r / 4), RGB(0xe0, 0x3c, 0x3c), 255);
    for (int j = 0; j < 2; j++) for (int i = 0; i < 3; i++)
        fill_rect(x + w / 6 + i * w / 3 - r / 8, y + h / 3 + h / 6 + j * h / 4, imax(2, r / 4), imax(2, r / 5), RGB(0x60, 0x68, 0x74), 255);
}
