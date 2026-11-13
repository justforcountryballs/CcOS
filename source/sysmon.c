/* ===================================================================
 *  System Monitor: how busy the computer is. Processor and memory
 *  graphs (the last minute), D:'s space and the processor temperature,
 *  and the running programs - pick one and End task stops it.
 * =================================================================== */
#define SM_W 760
#define SM_H 540
#define SM_HIST 60
#define SM_MAXP 256
static struct {
    float cpu[SM_HIST], mem[SM_HIST]; int nh;
    unsigned long long last_tot, last_idle;
    long mem_total, mem_used;               /* kB */
    int temp;                               /* degrees, -1 unknown */
    struct { int pid; char name[40]; float cpu; long rss; unsigned long long ticks; } p[SM_MAXP];
    int np, sel_pid, scroll;
    long long last;
    char msg[80];
} sm = { .sel_pid = -1, .temp = -1 };
static struct { int pid; unsigned long long ticks; } sm_prev[SM_MAXP]; static int sm_nprev;

static void sysmon_draw(int cx, int cy, int cw, int ch);
static int sm_cmp(const void *a, const void *b) {
    const typeof(sm.p[0]) *x = a, *y = b;
    if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
    return x->rss < y->rss ? 1 : x->rss > y->rss ? -1 : 0;
}
/* friendlier names for what's running */
static void sm_name(const char *comm, const char *cmdline, char *out, size_t n) {
    if (strstr(cmdline, "/app/lib/firefox") || !strcmp(comm, "firefox") || !strcmp(comm, "firefox-bin") || !strncmp(comm, "Web Content", 11) || !strcmp(comm, "Isolated Web Co"))
        { snprintf(out, n, "Firefox"); return; }
    if (!strcmp(comm, "desktop")) { snprintf(out, n, "ccos desktop"); return; }
    if (!strcmp(comm, "sober")) { snprintf(out, n, "Sober (Roblox)"); return; }
    if (!strncmp(comm, "ccos-", 5)) { snprintf(out, n, "%s", comm); return; }
    snprintf(out, n, "%s", comm);
}
static void sm_sample(void) {
    /* processor */
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u, ni, sy, id, io, irq, sirq, st;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &ni, &sy, &id, &io, &irq, &sirq, &st) == 8) {
            unsigned long long tot = u + ni + sy + id + io + irq + sirq + st, idle = id + io;
            float c = sm.last_tot && tot > sm.last_tot ? 100.0f * (1 - (float)(idle - sm.last_idle) / (float)(tot - sm.last_tot)) : 0;
            sm.last_tot = tot; sm.last_idle = idle;
            if (sm.nh == SM_HIST) { memmove(sm.cpu, sm.cpu + 1, sizeof(float) * (SM_HIST - 1)); memmove(sm.mem, sm.mem + 1, sizeof(float) * (SM_HIST - 1)); sm.nh--; }
            sm.cpu[sm.nh] = clampf(c, 0, 100);
        }
        fclose(f);
    }
    /* memory */
    long tot = 0, avail = 0; char l[128];
    if ((f = fopen("/proc/meminfo", "r"))) {
        while (fgets(l, sizeof l, f)) { sscanf(l, "MemTotal: %ld", &tot); sscanf(l, "MemAvailable: %ld", &avail); }
        fclose(f);
    }
    sm.mem_total = tot; sm.mem_used = tot - avail;
    sm.mem[sm.nh] = tot ? 100.0f * sm.mem_used / tot : 0;
    sm.nh++;
    /* temperature: the processor's sensor */
    sm.temp = -1;
    for (int i = 0; i < 16 && sm.temp < 0; i++) {
        char p[96], nm[32] = ""; snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", i);
        if (!(f = fopen(p, "r"))) continue;
        if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0;
        fclose(f);
        if (strcmp(nm, "coretemp") && strcmp(nm, "k10temp") && strcmp(nm, "acpitz")) continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp1_input", i);
        if ((f = fopen(p, "r"))) { long t; if (fscanf(f, "%ld", &t) == 1) sm.temp = (int)(t / 1000); fclose(f); }
    }
    /* programs */
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    static long long last_ms; long long now = ui_ms(); float secs = last_ms ? (now - last_ms) / 1000.0f : 1; last_ms = now;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1;
    DIR *d = opendir("/proc"); if (!d) return;
    struct dirent *e; int np = 0;
    typeof(sm.p[0]) tmp[SM_MAXP];
    while ((e = readdir(d)) && np < SM_MAXP) {
        int pid = atoi(e->d_name); if (pid <= 1) continue;
        char p[64], buf[512], cmd[256] = "";
        snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
        int fd = open(p, O_RDONLY); if (fd < 0) continue;
        ssize_t k = read(fd, cmd, sizeof cmd - 1); close(fd);
        if (k <= 0) continue;                                   /* kernel threads: no command line */
        cmd[k] = 0;
        snprintf(p, sizeof p, "/proc/%d/stat", pid);
        if ((fd = open(p, O_RDONLY)) < 0) continue;
        k = read(fd, buf, sizeof buf - 1); close(fd);
        if (k <= 0) continue;
        buf[k] = 0;
        char *a = strchr(buf, '('), *b = strrchr(buf, ')'); if (!a || !b) continue;
        char comm[32]; snprintf(comm, sizeof comm, "%.*s", (int)(b - a - 1), a + 1);
        unsigned long long ut = 0, stt = 0; long rss = 0;
        if (sscanf(b + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %*u %*u %ld", &ut, &stt, &rss) < 3) continue;
        unsigned long long ticks = ut + stt, prev = ticks;
        for (int j = 0; j < sm_nprev; j++) if (sm_prev[j].pid == pid) { prev = sm_prev[j].ticks; break; }
        tmp[np].pid = pid; tmp[np].ticks = ticks;
        tmp[np].cpu = (float)(ticks - prev) / hz / secs * 100.0f / ncpu;
        tmp[np].rss = rss * (sysconf(_SC_PAGESIZE) / 1024);
        sm_name(comm, cmd, tmp[np].name, sizeof tmp[0].name);
        np++;
    }
    closedir(d);
    sm_nprev = 0;
    for (int i = 0; i < np; i++) { sm_prev[sm_nprev].pid = tmp[i].pid; sm_prev[sm_nprev].ticks = tmp[i].ticks; sm_nprev++; }
    /* the same program in several processes (Firefox): one line */
    sm.np = 0;
    for (int i = 0; i < np; i++) {
        int j = 0;
        if (!strcmp(tmp[i].name, "Firefox")) for (; j < sm.np; j++) if (!strcmp(sm.p[j].name, "Firefox")) break;
        if (strcmp(tmp[i].name, "Firefox") || j == sm.np) sm.p[sm.np++] = tmp[i];
        else { sm.p[j].cpu += tmp[i].cpu; sm.p[j].rss += tmp[i].rss; if (tmp[i].pid < sm.p[j].pid) sm.p[j].pid = tmp[i].pid; }
    }
    qsort(sm.p, (size_t)sm.np, sizeof sm.p[0], sm_cmp);
}

static void sm_graph(int x, int y, int w, int h, const float *v, int n, uint32_t col, const char *title, const char *val, int dark) {
    fill_round(x, y, w, h, 8, dark ? RGB(0x2a, 0x2b, 0x30) : RGB(0xf4, 0xf6, 0xf9), 255);
    draw_text_bold(x + 12, y + 10, title, dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28));
    draw_text(x + w - 12 - text_w(val), y + 10, val, dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28), 255);
    int gx = x + 12, gy = y + 34, gw = w - 24, gh = h - 46;
    for (int k = 1; k < 4; k++) fill_rect(gx, gy + gh * k / 4, gw, 1, dark ? RGB(0x3a, 0x3c, 0x42) : RGB(0xe0, 0xe4, 0xea), 255);
    if (n < 2) return;
    for (int i = 0; i < n; i++) {                              /* filled area */
        int x0 = gx + (SM_HIST - n + i) * gw / (SM_HIST - 1), x1 = gx + (SM_HIST - n + i + 1) * gw / (SM_HIST - 1);
        int top = gy + gh - (int)(v[i] * gh / 100);
        if (i < n - 1) for (int xx = x0; xx < x1; xx++) {
            float t = (float)(xx - x0) / imax(1, x1 - x0); int tt = gy + gh - (int)((v[i] + (v[i + 1] - v[i]) * t) * gh / 100);
            fill_rect(xx, tt, 1, gy + gh - tt, col, 60);
        }
        (void)top;
    }
    for (int i = 0; i + 1 < n; i++) {
        float x0 = gx + (SM_HIST - n + i) * (float)gw / (SM_HIST - 1), x1 = gx + (SM_HIST - n + i + 1) * (float)gw / (SM_HIST - 1);
        aa_line(x0, gy + gh - v[i] * gh / 100, x1, gy + gh - v[i + 1] * gh / 100, 1.8f, col);
    }
}
#define SM_LY 210                      /* the program list */
#define SM_RH 26
static int sm_rows(int ch) { return (ch - SM_LY - 30 - 56) / SM_RH; }
static void sysmon_draw(int cx, int cy, int cw, int ch) {
    const int dark = set_style && set_dark;
    uint32_t bg = dark ? RGB(0x20, 0x21, 0x24) : RGB(255, 255, 255), text = dark ? RGB(0xe8, 0xea, 0xee) : RGB(0x1e, 0x22, 0x28);
    uint32_t grey = dark ? RGB(0x9a, 0xa0, 0xaa) : RGB(0x70, 0x78, 0x84);
    fill_rect(cx, cy, cw, ch, bg, 255);
    char v[64];
    snprintf(v, sizeof v, "%d%%%s", sm.nh ? (int)(sm.cpu[sm.nh - 1] + 0.5f) : 0, sm.temp >= 0 ? "" : "");
    int gw = (cw - 48) / 2;
    sm_graph(cx + 16, cy + 14, gw, 180, sm.cpu, sm.nh, RGB(0x1e, 0x78, 0xe0), "Processor", v, dark);
    snprintf(v, sizeof v, "%.1f / %.1f GB", sm.mem_used / 1048576.0, sm.mem_total / 1048576.0);
    sm_graph(cx + 32 + gw, cy + 14, gw, 180, sm.mem, sm.nh, RGB(0x8a, 0x3a, 0xd8), "Memory", v, dark);
    /* a line under the graphs: temperature, D: */
    char info[160] = "";
    struct statvfs vf;
    if (statvfs("/data", &vf) == 0 && conf_persistent) {
        double fr = (double)vf.f_bavail * vf.f_frsize / 1e9, tt = (double)vf.f_blocks * vf.f_frsize / 1e9;
        if (tt >= 1) snprintf(info, sizeof info, "D: %.1f GB free of %.1f GB", fr, tt);
        else snprintf(info, sizeof info, "D: %.0f MB free of %.0f MB", fr * 1000, tt * 1000);
    }
    if (sm.temp >= 0) { char t[40]; snprintf(t, sizeof t, "%sProcessor %d\xc2\xb0""C", info[0] ? "      " : "", sm.temp); strncat(info, t, sizeof info - strlen(info) - 1); }
    draw_text(cx + 16, cy + SM_LY - 14, info, grey, 255);
    /* the list */
    int ly = cy + SM_LY + 10;
    draw_text_bold(cx + 24, ly, "Program", text);
    draw_text_bold(cx + cw - 250, ly, "Processor", text);
    draw_text_bold(cx + cw - 120, ly, "Memory", text);
    fill_rect(cx + 16, ly + 22, cw - 32, 1, dark ? RGB(0x3a, 0x3c, 0x42) : RGB(0xe0, 0xe4, 0xea), 255);
    int rows = sm_rows(ch), hv = hot.type == HOT_CONTENT && wins[hot.a].draw == sysmon_draw ? hot.b : -1;
    for (int r = 0; r < rows && sm.scroll + r < sm.np; r++) {
        int i = sm.scroll + r, y = ly + 28 + r * SM_RH, sel = sm.p[i].pid == sm.sel_pid;
        if (sel || hv == i) fill_round(cx + 16, y - 4, cw - 32, SM_RH - 2, 5, sel ? (dark ? RGB(0x2e, 0x44, 0x66) : RGB(0xdc, 0xea, 0xfb)) : (dark ? RGB(0x2c, 0x2e, 0x34) : RGB(0xf2, 0xf5, 0xf9)), 255);
        draw_text(cx + 24, y, sm.p[i].name, text, 255);
        char c[16]; snprintf(c, sizeof c, "%.1f%%", sm.p[i].cpu); draw_text(cx + cw - 250, y, c, text, 255);
        char m[24]; if (sm.p[i].rss >= 1024 * 1024) snprintf(m, sizeof m, "%.1f GB", sm.p[i].rss / 1048576.0); else snprintf(m, sizeof m, "%ld MB", sm.p[i].rss / 1024);
        draw_text(cx + cw - 120, y, m, text, 255);
    }
    /* End task */
    int bx = cx + cw - 16 - 120, by = cy + ch - 46, en = sm.sel_pid > 1;
    fill_round(bx, by, 120, 32, 6, en ? (hv == 1000 ? RGB(0xd0, 0x30, 0x30) : RGB(0xc0, 0x28, 0x28)) : (dark ? RGB(0x40, 0x42, 0x48) : RGB(0xd8, 0xdc, 0xe2)), 255);
    draw_text(bx + (120 - text_w("End task")) / 2, by + 8, "End task", en ? RGB(255, 255, 255) : grey, 255);
    draw_text(cx + 16, by + 8, sm.msg[0] ? sm.msg : "Pick a program, then End task to stop it.", grey, 255);
}
static int sysmon_hit(int lx, int ly, int cw, int ch) {
    if (lx >= cw - 136 && lx < cw - 16 && ly >= ch - 46 && ly < ch - 14) return 1000;
    int y0 = SM_LY + 10 + 28 - 4, r = (ly - y0) / SM_RH;
    if (ly >= y0 && r < sm_rows(ch) && sm.scroll + r < sm.np && lx >= 16 && lx < cw - 16) return sm.scroll + r;
    return -1;
}
static void sysmon_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(sysmon_draw), cx = 0, cy = 0, cw = SM_W, ch = SM_H;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (id == 1000) { *x = cx + cw - 140; *y = cy + ch - 50; *w = 128; *h = 40; return; }
    *x = cx + 12; *y = cy + SM_LY + 10 + 24 + (id - sm.scroll) * SM_RH; *w = cw - 24; *h = SM_RH + 2;
}
static void sysmon_click(int lx, int ly, int cw, int ch) {
    int id = sysmon_hit(lx, ly, cw, ch);
    if (id == 1000 && sm.sel_pid > 1) {
        char nm[40] = ""; for (int i = 0; i < sm.np; i++) if (sm.p[i].pid == sm.sel_pid) snprintf(nm, sizeof nm, "%s", sm.p[i].name);
        if (sm.sel_pid == getpid()) snprintf(sm.msg, sizeof sm.msg, "That's ccos itself - it can't be stopped here.");
        else {
            if (!strcmp(nm, "Firefox")) { if (fork() == 0) { execlp("pkill", "pkill", "-f", "/app/lib/firefox/", (char *)NULL); _exit(0); } }
            else kill(sm.sel_pid, SIGTERM);
            int pid = sm.sel_pid;
            if (fork() == 0) { sleep(3); kill(pid, SIGKILL); _exit(0); }    /* still there after 3 s: for good */
            snprintf(sm.msg, sizeof sm.msg, "Stopped %s.", nm);
            sm.sel_pid = -1;
        }
    } else if (id >= 0 && id < sm.np) { sm.sel_pid = sm.p[id].pid; sm.msg[0] = 0; }
    repaint_client(sysmon_draw);
}
static void sysmon_wheel(int amount) {
    int i = win_of(sysmon_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    sm.scroll = clampi(sm.scroll - amount * 3, 0, imax(0, sm.np - sm_rows(ch)));
    repaint_client(sysmon_draw);
}
static void sysmon_tick(void) { sm_sample(); repaint_client(sysmon_draw); }
static void sysmon_open(void) {
    if (win_of(sysmon_draw) < 0) { sm.nh = 0; sm.last_tot = 0; sm_sample(); }
    static const Win t = { .title = "System Monitor", .icon = ICON_SYSMON, .draw = sysmon_draw, .click = sysmon_click, .hit = sysmon_hit,
                           .elem_rect = sysmon_elem_rect, .wheel = sysmon_wheel, .tick = sysmon_tick };
    open_win(&t, SM_W, SM_H);
}
static void draw_sysmon_icon(int cx, int cy, int r) {          /* a little screen with a graph */
    fill_round(cx - r, cy - r * 3 / 4, 2 * r, r * 3 / 2, imax(2, r / 5), RGB(0x1c, 0x22, 0x2c), 255);
    float pts[6][2] = { { -0.75f, 0.35f }, { -0.4f, 0.1f }, { -0.15f, 0.3f }, { 0.15f, -0.35f }, { 0.4f, -0.05f }, { 0.75f, -0.3f } };
    for (int i = 0; i < 5; i++) aa_line(cx + pts[i][0] * r, cy + pts[i][1] * r, cx + pts[i + 1][0] * r, cy + pts[i + 1][1] * r, fmaxf(1.2f, r / 8.0f), RGB(0x3c, 0xe0, 0x7c));
    fill_rect(cx - r / 3, cy + r * 3 / 4, r * 2 / 3, imax(1, r / 6), RGB(0x60, 0x68, 0x74), 255);
}
