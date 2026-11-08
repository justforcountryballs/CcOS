/* ===================================================================
 *  POWER: battery, screen brightness, the lid, sleep, and turning the
 *  screen off when nobody is using the computer.
 *
 *  Everything comes from the kernel's files:
 *    /sys/class/power_supply/BAT0   battery level and charging
 *    /sys/class/power_supply/AC     plugged in or not
 *    /sys/class/backlight/...       screen brightness (laptops)
 *    /sys/power/state               "mem" puts the computer to sleep
 *  The lid is an input device ("Lid Switch") like the keyboard.
 * =================================================================== */

/* ---- screen brightness ---------------------------------------------- */
static char bl_dir[160];          /* /sys/class/backlight/<name>, "" if this screen has none */
static int bl_max;
static int bl_read_int(const char *dir, const char *name) {
    char p[224]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r"); int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}
static void bl_find(void) {
    /* laptops often have two: the graphics chip's own ("raw", best) and the
       firmware's. Take the raw one, then firmware, then anything. */
    bl_dir[0] = 0; bl_max = 0;
    static const char *pref[] = { "raw", "firmware", "platform", NULL };
    for (int k = 0; pref[k] && !bl_dir[0]; k++) {
        DIR *d = opendir("/sys/class/backlight");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char dir[160], p[224], type[16] = "";
            snprintf(dir, sizeof dir, "/sys/class/backlight/%s", e->d_name);
            snprintf(p, sizeof p, "%s/type", dir);
            FILE *f = fopen(p, "r");
            if (f) { if (!fgets(type, sizeof type, f)) type[0] = 0; fclose(f); }
            type[strcspn(type, "\n")] = 0;
            int mx_ = bl_read_int(dir, "max_brightness");
            if (strcmp(type, pref[k]) || mx_ <= 0) continue;
            snprintf(bl_dir, sizeof bl_dir, "%s", dir); bl_max = mx_;
            break;
        }
        if (d) closedir(d);
    }
}
static int bl_get(void) {                     /* 0-100, or -1 */
    if (!bl_dir[0]) return -1;
    int v = bl_read_int(bl_dir, "brightness");
    return v < 0 ? -1 : (v * 100 + bl_max / 2) / bl_max;
}
static void bl_write(int pct) {               /* 0 = dark (screen off) */
    if (!bl_dir[0]) return;
    char p[224]; snprintf(p, sizeof p, "%s/brightness", bl_dir);
    int raw = pct <= 0 ? 0 : imax(1, (int)((long)bl_max * pct / 100));
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d", raw); fclose(f); }
}
static void bright_set(int pct, int popup) {
    if (!bl_dir[0]) return;
    set_bright = clampi(pct, 5, 100);         /* never all the way to black */
    bl_write(set_bright);
    save_settings();
    if (popup) osd_show(OSD_BRIGHT);
    if (win_of(settings_draw) >= 0) st_repaint();
}

/* ---- battery ------------------------------------------------------- */
/* (bat itself is declared in desktop.c, next to the settings) */
static int ps_read(const char *dir, const char *name, char *out, size_t n) {
    char p[224]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}
static long ps_long(const char *dir, const char *name) {
    char b[32]; return ps_read(dir, name, b, sizeof b) ? atol(b) : -1;
}
static int bat_poll(void) {                  /* returns 1 if something shown changed */
    int old_p = bat.present, old_pct = bat.pct, old_c = bat.charging, old_ac = bat.ac, old_m = bat.mins;
    bat.present = 0; bat.ac = 0;
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[160], type[24] = "", scope[24] = "";
        snprintf(dir, sizeof dir, "/sys/class/power_supply/%s", e->d_name);
        ps_read(dir, "type", type, sizeof type);
        ps_read(dir, "scope", scope, sizeof scope);
        if (!strcmp(type, "Mains") || !strcmp(type, "USB")) { if (ps_long(dir, "online") == 1) bat.ac = 1; continue; }
        if (strcmp(type, "Battery") || !strcmp(scope, "Device")) continue;   /* a mouse's battery: not ours */
        if (ps_long(dir, "present") == 0) continue;
        bat.present = 1;
        snprintf(bat.dir, sizeof bat.dir, "%s", dir);
    }
    if (d) closedir(d);
    if (bat.present) {
        char st[24] = "";
        ps_read(bat.dir, "status", st, sizeof st);
        long cap = ps_long(bat.dir, "capacity");
        long now = ps_long(bat.dir, "energy_now"), full = ps_long(bat.dir, "energy_full"), rate = ps_long(bat.dir, "power_now");
        if (now < 0) { now = ps_long(bat.dir, "charge_now"); full = ps_long(bat.dir, "charge_full"); rate = ps_long(bat.dir, "current_now"); }
        if (cap < 0 && now >= 0 && full > 0) cap = now * 100 / full;
        bat.pct = clampi((int)cap, 0, 100);
        bat.charging = !strcmp(st, "Charging");
        bat.full = !strcmp(st, "Full") || (bat.ac && !strcmp(st, "Not charging"));
        if (!strcmp(st, "Discharging")) bat.ac = 0;
        bat.mins = -1;
        if (rate > 0 && now >= 0) {
            if (!strcmp(st, "Discharging")) bat.mins = (int)(now * 60 / rate);
            else if (bat.charging && full > now) bat.mins = (int)((full - now) * 60 / rate);
        }
    }
    return bat.present != old_p || bat.pct != old_pct || bat.charging != old_c || bat.ac != old_ac || bat.mins / 5 != old_m / 5;
}
static void bat_text(char *out, size_t n) {   /* "85% - 3 h 10 min left" */
    if (!bat.present) { snprintf(out, n, "No battery: this computer runs on mains power."); return; }
    char t[48] = "";
    if (bat.mins > 0 && bat.mins < 48 * 60) {
        if (bat.mins >= 60) snprintf(t, sizeof t, "%d h %d min", bat.mins / 60, bat.mins % 60);
        else snprintf(t, sizeof t, "%d min", bat.mins);
    }
    if (bat.full) snprintf(out, n, "%d%% - fully charged", bat.pct);
    else if (bat.charging) snprintf(out, n, t[0] ? "%d%% - charging, full in %s" : "%d%% - charging%s", bat.pct, t);
    else if (bat.ac) snprintf(out, n, "%d%% - plugged in, not charging", bat.pct);
    else snprintf(out, n, t[0] ? "%d%% - %s left" : "%d%% - on battery%s", bat.pct, t);
}

/* a battery lying on its side, about 22 x 12, filled to the level */
static void draw_battery(int x, int y, int pct, int charging, uint32_t line) {
    int w = 20, h = 11;
    stroke_round(x, y, w, h, 2, line, 255);
    stroke_round(x + 1, y + 1, w - 2, h - 2, 1, line, 90);
    fill_rect(x + w, y + 3, 2, h - 6, line, 255);                       /* the bump */
    uint32_t c = pct <= 10 ? RGB(0xe8, 0x3c, 0x30) : pct <= 20 ? RGB(0xf0, 0xa0, 0x20) : RGB(0x3c, 0xc8, 0x4c);
    int fw = (w - 4) * clampi(pct, 0, 100) / 100;
    if (fw > 0) fill_rect(x + 2, y + 2, imax(1, fw), h - 4, c, 255);
    if (charging) {                                                     /* a little lightning bolt */
        int bx = x + w / 2 - 2, by = y - 1;
        for (int k = 0; k < 7; k++) {
            int xx = bx + 3 - k / 2 + (k >= 4 ? 2 : 0);
            fill_rect(xx - 1, by + k, 3, 1, RGB(0x10, 0x10, 0x10), 255);
            fill_rect(xx, by + k, 1, 1, RGB(0xff, 0xe0, 0x30), 255);
        }
    }
}

/* low battery: warn at 20% and 10%, and at 4% save everything and shut down */
static void bat_warn(void) {
    static int warned = 100, bye_at;
    if (!bat.present) return;
    if (bat.ac || bat.charging) { warned = 100; bye_at = 0; return; }
    char b[96];
    if (bat.pct <= 10 && warned > 10) {
        warned = 10; snprintf(b, sizeof b, "%d%% left. Plug in now, or save your work.", bat.pct);
        toast(ICON_BATTERY, "Battery very low", b);
    } else if (bat.pct <= 20 && warned > 20) {
        warned = 20; snprintf(b, sizeof b, "%d%% left. Plug in your charger soon.", bat.pct);
        toast(ICON_BATTERY, "Battery low", b);
    }
    if (bat.pct <= 4 && !bye_at) {
        bye_at = (int)time(NULL) + 60;
        toast(ICON_BATTERY, "Battery almost empty", "ccos will shut down in 1 minute unless you plug in.");
    }
    if (bye_at && time(NULL) >= bye_at) shutdown_now();
}

/* ---- sleep ---------------------------------------------------------- */
static long long idle_last;       /* when the keyboard, mouse or touchpad was last used (ms) */
static int idle_state;            /* 0 normal, 1 dimmed, 2 screen off */
static int fb_fd = -1;
static void screen_power(int on) {
    if (on) { bl_write(set_bright > 0 ? set_bright : 100); if (fb_fd >= 0) ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK); }
    else { bl_write(0); if (fb_fd >= 0) ioctl(fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN); }
}
static void power_wake_screen(void) {
    if (idle_state == 0) return;
    int was_off = idle_state == 2;
    idle_state = 0;
    if (was_off) screen_power(1); else bl_write(set_bright > 0 ? set_bright : 100);
    if (was_off) repaint_all();
}
static void power_activity(void) { idle_last = ui_ms(); if (idle_state) power_wake_screen(); }

static int sleep_supported(void) {
    char b[64] = "";
    FILE *f = fopen("/sys/power/state", "r");
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    return strstr(b, "mem") || strstr(b, "freeze");
}
static void power_sleep(void) {
    char b[64] = "";
    FILE *f = fopen("/sys/power/state", "r");
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    const char *how = strstr(b, "mem") ? "mem" : strstr(b, "freeze") ? "freeze" : NULL;
    if (!how) { toast(ICON_MOON, "Sleep isn't available", "This computer doesn't support sleep with ccos."); return; }
    set_menu(0); pop_close();
    save_settings(); sync();
    f = fopen("/sys/power/state", "w");               /* this returns when the computer wakes up */
    if (f) { fputs(how, f); fclose(f); }
    /* awake again */
    idle_state = 0; idle_last = ui_ms();
    screen_power(1);
    net_fast = 15;                                    /* look for the Wi-Fi again quickly */
    bat_poll();
    repaint_all();
}

static const int off_mins[NOFF] = { 0, 2, 5, 10, 30 };
static const char *off_names[NOFF] = { "Never", "2 min", "5 min", "10 min", "30 min" };
static const int sleep_mins[NSLEEPT] = { 0, 15, 30, 60 };
static const char *sleep_names[NSLEEPT] = { "Never", "15 min", "30 min", "1 hour" };

/* something is playing a video or is full screen (a game, YouTube): keep the screen on */
static int media_video_playing(void), media_music_playing(void);
static int power_busy_screen(void) {
    if (media_video_playing()) return 1;
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open && !wins[i].minimized && wins[i].fullscreen) return 1;
    return 0;
}
static int power_busy_sleep(void) {
    if (power_busy_screen()) return 1;
    if (media_music_playing()) return 1;             /* music playing */
    return 0;
}

/* once a second */
static void power_tick(void) {
    static int n;
    if (n++ % 5 == 0) {                               /* battery: every 5 seconds */
        if (bat_poll()) { repaint_taskbar(); if (win_of(settings_draw) >= 0 && st_tab == TAB_POWER) st_repaint(); }
        bat_warn();
    }
    if (!idle_last) idle_last = ui_ms();
    long long idle = (ui_ms() - idle_last) / 1000;
    int off = off_mins[set_off_idx] * 60, slp = sleep_mins[set_sleep_idx] * 60;
    if (power_busy_screen()) { idle_last = ui_ms(); return; }
    if (off) {
        if (idle_state == 0 && idle >= off - 15 && bl_dir[0]) {           /* dim first: a warning */
            idle_state = 1; bl_write(imax(5, (set_bright > 0 ? set_bright : 100) * 3 / 10));
        }
        if (idle_state < 2 && idle >= off) { idle_state = 2; screen_power(0); }
    }
    if (slp && idle >= slp && !power_busy_sleep() && sleep_supported()) power_sleep();
}

/* the lid was closed (1) or opened (0) */
static void power_lid(int closed) {
    if (closed && set_lid == 0) power_sleep();
    else if (!closed) power_activity();
}

static void power_init(void) {
    bl_find();
    /* the firmware also changes the brightness on the brightness keys; ccos does it, once */
    FILE *f = fopen("/sys/module/video/parameters/brightness_switch_enabled", "w");
    if (f) { fputs("N", f); fclose(f); }
    if (bl_dir[0]) {
        if (set_bright > 0) bl_write(set_bright);
        else set_bright = bl_get();
    }
    bat_poll();
    idle_last = ui_ms();
}
