/* ===================================================================
 *  BLUETOOTH (Settings > Bluetooth): turn it on, search, pair, connect.
 *  The work is done by /usr/bin/ccos-bt (BlueZ's bluetoothctl); it writes
 *  what it sees to /run/ccos/bt.txt and we show that.
 * =================================================================== */
#define BT_MAXDEV 9
static struct { char mac[18], name[40], icon[24]; int paired, conn; } btd[BT_MAXDEV];
static int nbtd, bt_adapter = -1, bt_scanning;   /* adapter: -1 none, 0 off, 1 on */
static pid_t bt_job;                              /* ccos-bt running now (0: none) */
static char bt_busy_mac[18], bt_busy_what[24];    /* "Pairing..." next to that device */
static long long bt_last_status;

static int bt_job_running(void) {
    if (bt_job > 0 && kill(bt_job, 0) != 0) { bt_job = 0; bt_busy_mac[0] = 0; bt_busy_what[0] = 0; }
    return bt_job > 0;
}
static void bt_run(const char *a, const char *mac) {
    if (bt_job_running()) return;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl("/usr/bin/ccos-bt", "ccos-bt", a, mac, (char *)NULL);
        _exit(127);
    }
    if (p > 0) bt_job = p;
}
static int bt_read(void) {                        /* 1 if anything changed */
    static char last[4096];
    char buf[4096] = ""; size_t n = 0;
    FILE *f = fopen("/run/ccos/bt.txt", "r");
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    if (!strcmp(buf, last)) return 0;
    snprintf(last, sizeof last, "%s", buf);
    nbtd = 0; bt_adapter = f ? -1 : bt_adapter; bt_scanning = 0;
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
        if (!strncmp(l, "A ", 2)) bt_adapter = !strcmp(l + 2, "on") ? 1 : !strcmp(l + 2, "off") ? 0 : -1;
        else if (!strncmp(l, "S ", 2)) bt_scanning = l[2] == '1';
        else if (!strncmp(l, "D ", 2) && nbtd < BT_MAXDEV) {
            char *f5[5] = { l + 2 }; int k = 1;
            for (char *s = l + 2; *s && k < 5; s++) if (*s == '|') { *s = 0; f5[k++] = s + 1; }
            if (k < 5) continue;
            snprintf(btd[nbtd].mac, sizeof btd[0].mac, "%s", f5[0]);
            snprintf(btd[nbtd].name, sizeof btd[0].name, "%s", f5[1]);
            snprintf(btd[nbtd].icon, sizeof btd[0].icon, "%s", f5[2]);
            btd[nbtd].paired = f5[3][0] == '1'; btd[nbtd].conn = f5[4][0] == '1';
            nbtd++;
        }
    }
    /* paired first, then connected first */
    for (int i = 0; i < nbtd; i++) for (int j = i + 1; j < nbtd; j++)
        if (btd[j].conn * 2 + btd[j].paired > btd[i].conn * 2 + btd[i].paired) { __typeof__(btd[0]) t = btd[i]; btd[i] = btd[j]; btd[j] = t; }
    return 1;
}
/* once a second while Settings is open */
static void settings_tick(void) {
    if (st_tab != TAB_BT) return;
    int busy = bt_job_running();
    if (!busy && ui_ms() - bt_last_status > 3000) { bt_last_status = ui_ms(); bt_run("status", NULL); }
    static int was_busy;
    if (bt_read() || busy != was_busy) st_repaint();
    was_busy = busy;
}

/* small device pictures */
static void bt_glyph(const char *icon, int cx, int cy, uint32_t c) {
    if (!strncmp(icon, "audio", 5)) {                         /* headphones */
        for (int a = 180; a <= 360; a += 6) { float r = a * 3.14159f / 180; fill_circle(cx + cosf(r) * 8, cy + 1 + sinf(r) * 8, 1.2f, c, 255); }
        fill_round(cx - 10, cy + 1, 5, 8, 2, c, 255); fill_round(cx + 6, cy + 1, 5, 8, 2, c, 255);
    } else if (strstr(icon, "mouse")) {
        stroke_round(cx - 6, cy - 9, 12, 18, 6, c, 255); stroke_round(cx - 5, cy - 8, 10, 16, 5, c, 255);
        fill_rect(cx, cy - 8, 1, 6, c, 255);
    } else if (strstr(icon, "keyboard")) {
        stroke_round(cx - 11, cy - 6, 22, 13, 2, c, 255);
        for (int r = 0; r < 2; r++) for (int k = 0; k < 5; k++) fill_rect(cx - 8 + k * 4, cy - 3 + r * 4, 2, 2, c, 255);
    } else if (strstr(icon, "phone")) {
        stroke_round(cx - 6, cy - 10, 12, 20, 2, c, 255); fill_rect(cx - 2, cy + 6, 4, 1, c, 255);
    } else if (strstr(icon, "gaming")) {
        stroke_round(cx - 11, cy - 5, 22, 11, 5, c, 255); fill_rect(cx - 7, cy - 1, 5, 1, c, 255); fill_rect(cx - 5, cy - 3, 1, 5, c, 255);
        fill_circle(cx + 5, cy, 1.4f, c, 255);
    } else {                                                  /* the Bluetooth rune */
        int h = 9;
        fill_rect(cx, cy - h, 2, 2 * h, c, 255);
        for (int k = 0; k <= 5; k++) { fill_rect(cx + 1 + k, cy - h + k, 2, 2, c, 255); fill_rect(cx + 1 + k, cy + h - 2 - k, 2, 2, c, 255); }
        for (int k = 0; k <= 9; k++) { fill_rect(cx - 5 + k, cy - 5 + k, 2, 2, c, 255); fill_rect(cx - 5 + k, cy + 4 - k, 2, 2, c, 255); }
    }
}
#define BT_ROW_Y  176                /* first device row (from the page top) */
#define BT_ROW_H  40
static void bt_draw(int cx, int cy, int cw, uint32_t text, uint32_t grey) {
    int y = cy + ST_Y;
    st_heading(cx, y + 20, "Bluetooth");
    if (bt_adapter < 0) {
        draw_text(cx + 24, y + 54, "No Bluetooth found on this computer.", text, 255);
        draw_text(cx + 24, y + 72, "A USB Bluetooth dongle works too: just plug it in.", grey, 255);
        return;
    }
    draw_text(cx + 24, y + 54, bt_adapter ? "On" : "Off", text, 255);
    draw_text(cx + 24, y + 72, bt_adapter ? "Headphones, speakers, mice and keyboards can connect" : "Turn it on to use wireless headphones and mice", grey, 255);
    st_switch(cx + cw - 24 - TG_W, y + 52, bt_adapter == 1);
    st_line(cx, cw, y + 104);
    if (bt_adapter != 1) return;
    st_heading(cx, y + 128, "Devices");
    int busy = bt_job_running();
    st_choice(cx + cw - 24 - 190, y + 118, 190, 30, bt_scanning || (busy && !strcmp(bt_busy_what, "Searching...")) ? "Searching..." : "Search for devices", 0);
    if (!nbtd) draw_text(cx + 24, y + BT_ROW_Y + 10, "Nothing yet. Put your device in pairing mode, then search.", grey, 255);
    for (int i = 0; i < nbtd; i++) {
        int ry = y + BT_ROW_Y + i * BT_ROW_H;
        bt_glyph(btd[i].icon, cx + 40, ry + 18, RGB(0x30, 0x40, 0x58));
        char nm[40]; snprintf(nm, sizeof nm, "%.28s", btd[i].name);
        draw_text(cx + 66, ry + 3, nm, text, 255);
        const char *st = btd[i].conn ? "Connected" : btd[i].paired ? "Paired" : "Not paired";
        if (busy && !strcmp(bt_busy_mac, btd[i].mac)) st = bt_busy_what;
        draw_text(cx + 66, ry + 20, st, btd[i].conn ? RGB(0x20, 0x90, 0x30) : grey, 255);
        const char *b = btd[i].conn ? "Disconnect" : btd[i].paired ? "Connect" : "Pair";
        st_choice(cx + cw - 24 - 110, ry + 4, 110, 30, b, 0);
        if (btd[i].paired) st_choice(cx + cw - 24 - 110 - 8 - 84, ry + 4, 84, 30, "Remove", 0);
        if (i + 1 < nbtd) fill_rect(cx + 24, ry + BT_ROW_H - 2, cw - 48, 1, RGB(0xee, 0xf0, 0xf4), 255);
    }
}
static void bt_click(int lx, int ly, int cw) {
    if (bt_adapter < 0) return;
    if (ly >= ST_Y + 44 && ly < ST_Y + 90 && lx >= cw - 24 - TG_W - 40) {          /* on / off */
        bt_job_running();
        if (!bt_job) { bt_adapter = !bt_adapter; bt_run("power", bt_adapter ? "on" : "off"); st_repaint(); }
        return;
    }
    if (bt_adapter != 1 || bt_job_running()) return;
    if (ly >= ST_Y + 118 && ly < ST_Y + 148 && lx >= cw - 24 - 190 && lx < cw - 24) {
        bt_run("scan", NULL); snprintf(bt_busy_what, sizeof bt_busy_what, "Searching..."); bt_busy_mac[0] = 0;
        st_repaint(); return;
    }
    for (int i = 0; i < nbtd; i++) {
        int ry = ST_Y + BT_ROW_Y + i * BT_ROW_H + 4;
        if (ly < ry || ly >= ry + 30) continue;
        const char *a = NULL, *w = NULL;
        if (lx >= cw - 24 - 110 && lx < cw - 24) {
            if (btd[i].conn) { a = "disconnect"; w = "Disconnecting..."; }
            else if (btd[i].paired) { a = "connect"; w = "Connecting..."; }
            else { a = "pair"; w = "Pairing..."; }
        } else if (btd[i].paired && lx >= cw - 24 - 110 - 8 - 84 && lx < cw - 24 - 110 - 8) { a = "remove"; w = "Removing..."; }
        if (a) {
            bt_run(a, btd[i].mac);
            snprintf(bt_busy_mac, sizeof bt_busy_mac, "%s", btd[i].mac);
            snprintf(bt_busy_what, sizeof bt_busy_what, "%s", w);
            st_repaint();
        }
        return;
    }
}
