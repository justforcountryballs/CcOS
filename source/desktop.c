/*
 * ccos desktop - a tiny Windows 7 style desktop for ccos
 *
 * Draws straight to the Linux framebuffer (/dev/fb0) and reads the mouse
 * and keyboard from evdev (/dev/input/event*). No X11, no libraries.
 *
 *   Start orb "/o\"      click it (or press the Windows key) for the menu
 *   Windows              glass frame, drag by the title bar, double-click
 *                        the title to maximize; minimize / maximize / close
 *   Taskbar buttons      one per open window: click to show / minimize
 *   Clock                bottom right, 24-hour or 12-hour (see Settings)
 *   Shut down button     bottom right of the start menu
 *   Power button         QEMU: Machine > Power Down - also shuts down
 *   Ctrl+Alt+Backspace   quit the desktop and drop to the shell
 *
 * Rendering uses "dirty rectangles": the scene (wallpaper, windows, menu,
 * taskbar) is kept in memory, and only the areas that actually change are
 * redrawn and copied to the screen.
 *
 * Build: gcc -O2 -o desktop desktop.c -lm -lwayland-server -lxkbcommon -lz
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/input.h>
#include <linux/soundcard.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include "font8x16.h"
#include "smoothfont.h"
/* app pictures (PNG and SVG) for apps from Flathub */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "third/stb_image.h"
#define NANOSVG_IMPLEMENTATION
#include "third/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third/nanosvgrast.h"

/* ---- icons ---------------------------------------------------------- */
enum { ICON_NONE, ICON_GEAR, ICON_CALC, ICON_NOTE, ICON_MINE, ICON_FOLDER, ICON_FILE, ICON_DRIVE, ICON_COMPUTER, ICON_GLOBE, ICON_TERMINAL, ICON_APP, ICON_CARDS, ICON_BAG, ICON_PHOTO, ICON_VIDEO, ICON_MUSIC,
       ICON_FOLDER_IMG, ICON_FOLDER_DL, ICON_FOLDER_VID, ICON_USB, ICON_BATTERY, ICON_MOON, ICON_SUN, ICON_PRINTER, ICON_ZIP, ICON_BIN, ICON_BIN_FULL, ICON_CAMERA, ICON_RECORD, ICON_SEARCH };   /* coloured folders: Images, Downloads, Videos */
#define ICON_IMG 1000              /* ICON_IMG + n: an app's own picture (Flathub apps) */

/* ===================================================================
 *  START MENU CONTENTS - add your programs here!
 *  Each entry: { "Label", "command", icon }
 *  A command starting with '@' opens a built-in app (e.g. "@settings").
 *  Anything else is run with /bin/sh.
 *  Keep the { NULL } line at the end of each list.
 * =================================================================== */
typedef struct { const char *label; const char *cmd; int icon; } MenuItem;

#define FP_MENU 5                  /* Flatpak apps shown in the start menu */
#define NBUILTIN 13                /* built-in programs in the start menu */
static MenuItem left_items[NBUILTIN + FP_MENU + 1] = {   /* white panel (programs); Flatpak apps are added after these */
    { "Terminal", "@terminal", ICON_TERMINAL },
    { "File Explorer", "@explorer", ICON_FOLDER },
    { "Calculator", "@calc", ICON_CALC },
    { "Minesweeper", "@mines", ICON_MINE },
    { "Solitaire", "@solitaire", ICON_CARDS },
    { "Notepad", "@notepad", ICON_NOTE },
    { "Photos", "@photos", ICON_PHOTO },
    { "Videos", "@videos", ICON_VIDEO },
    { "Music", "@music", ICON_MUSIC },
    { "Camera", "@camera", ICON_CAMERA },
    { "Record", "@record", ICON_RECORD },
    { "Software Manager", "@store", ICON_BAG },
    { "Settings", "@settings", ICON_GEAR },
    { NULL, NULL, 0 }
};
static MenuItem right_items[] = {  /* dark glass panel (places/settings) */
    { "Documents", "@explorer /data/Documents", ICON_FOLDER },
    { "Computer", "@explorer", ICON_COMPUTER },
    { NULL, NULL, 0 }
};

/* ===================================================================
 *  DESKTOP ICONS - { "Label", "command", icon, column, row }
 *  Column/row is the starting spot on the grid; the user can drag them.
 * =================================================================== */
typedef struct { char label[40]; char cmd[112]; int icon; int col, row; } DeskIcon;
#define MAXDESK 64
static DeskIcon desk_icons[MAXDESK] = {
    { "Computer",    "@explorer",                 ICON_COMPUTER, 0, 0 },
    { "Documents",   "@explorer /data/Documents", ICON_FOLDER,   0, 1 },
    { "Notepad",     "@notepad",                  ICON_NOTE,     0, 2 },
    { "Calculator",  "@calc",                     ICON_CALC,     0, 3 },
    { "Minesweeper", "@mines",                    ICON_MINE,     0, 4 },
    { "Solitaire",   "@solitaire",                ICON_CARDS,    0, 5 },
    { "Software Manager", "@store",               ICON_BAG,      0, 6 },
};
static int ndesk = 7;                     /* the built-in ones above; more are added (Flathub apps) */
#define NDESK ndesk

/* ---- background colors offered in Settings ------------------------- */
typedef struct { const char *name; uint32_t top, bottom, glow; } Wallpaper;
#define RGB(r,g,b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
static const Wallpaper wallpapers[] = {
    { "Blue",     RGB(0x1c, 0x5c, 0xa8), RGB(0x06, 0x24, 0x56), RGB(0x6c, 0xc4, 0xf4) },
    { "Teal",     RGB(0x0e, 0x76, 0x7c), RGB(0x03, 0x2c, 0x32), RGB(0x5c, 0xdc, 0xcc) },
    { "Green",    RGB(0x2c, 0x80, 0x36), RGB(0x0a, 0x2e, 0x12), RGB(0x9a, 0xe0, 0x78) },
    { "Purple",   RGB(0x56, 0x38, 0x9a), RGB(0x18, 0x0a, 0x3a), RGB(0xb4, 0x8c, 0xf0) },
    { "Pink",     RGB(0xac, 0x38, 0x78), RGB(0x3c, 0x0a, 0x26), RGB(0xff, 0x9a, 0xd0) },
    { "Red",      RGB(0x9c, 0x28, 0x28), RGB(0x38, 0x08, 0x08), RGB(0xf0, 0x8a, 0x6c) },
    { "Orange",   RGB(0xbc, 0x68, 0x18), RGB(0x48, 0x1c, 0x04), RGB(0xff, 0xc0, 0x70) },
    { "Graphite", RGB(0x48, 0x50, 0x5a), RGB(0x12, 0x16, 0x1a), RGB(0x9a, 0xa6, 0xb4) },
};
#define NWALL ((int)(sizeof wallpapers / sizeof *wallpapers))

/* ---- settings (saved to /data/ccos.conf) ---------------------------- */
#define CONF_PATH "/data/ccos.conf"
static int stick_wait;              /* the "needs to find your USB boot stick" screen is up */
static int rec_on(void);            /* Record: the screen is being recorded (record.c) */
static int rec_tb_x(void);
static void rec_draw_tb(void);
static void rec_stop(void);
static void rec_toggle(void);
#define REC_TB_W 86
#define REC_W 300
#define REC_H 214
static void rec_draw(int cx, int cy, int cw, int ch);
static void rec_click(int lx, int ly, int cw, int ch);
static int rec_hit(int lx, int ly, int cw, int ch);
static void rec_app_tick(void);
#define CAM_W 720
#define CAM_H 470
static void cam_draw(int cx, int cy, int cw, int ch);
static void cam_click(int lx, int ly, int cw, int ch);
static int cam_hit(int lx, int ly, int cw, int ch);
static void cam_key(int code, int mods);
static void cam_tick(void);
static int cam_on_close(void);
static int cam_fd(void);
static void cam_wake(void);
static void cam_start(void);
static int snip_on;                 /* Win+Shift+S: picking a part of the screen */
static int meta_down, meta_used;    /* the Windows key: held / used with another key */
static void snip_button(int down);
static void snip_render(int x0, int y0, int x1, int y1);
static void snip_motion(void);
static void stick_wait_click(void);
static int set_wall = 0;      /* index into wallpapers[] */
static int set_clock24 = 1;   /* 1 = 24-hour (military) time */
static int set_sound = 1;     /* play the startup sound */
static int set_volume = 70;   /* 0-100 */
static char set_wall_img[300]; /* a picture as the background (a copy on D:), or "" */
enum { WF_FILL, WF_FIT, WF_STRETCH, WF_TILE, WF_CENTER, WF_SPAN, NWF };
static int set_wall_fit = WF_FILL;
static const char *wf_names[NWF] = { "Fill", "Fit", "Stretch", "Tile", "Center", "Span" };
static int set_muted;
static int set_net = 1;       /* networking on */
static int set_wifi = 1;      /* Wi-Fi radio on */
static int set_smooth = 0;    /* smooth (anti-aliased) font: on in the Modern style */
static int set_style = 0;     /* 0 Classic (glass, blocky font), 1 Modern (flat, smooth) */
static int set_dark = 0;      /* Modern: 0 light, 1 dark */
static int set_cursor_col = 1;   /* mouse pointer colour: 0 white, 1 black, 2 blue */
static int set_cursor_size = 0;  /* mouse pointer size: 0 normal, 1 large, 2 extra large */
static char set_tz[64];          /* time zone, like "Europe/Warsaw" ("": UTC) */
static int set_tz_auto = 1;      /* find the time zone from the internet connection */
static int set_ntp = 1;          /* set the clock from the internet */
static char set_upd_repo[96] = "justforcountryballs/CcOS";
static char set_upd_api[200];                 /* testing: another server instead of api.github.com */
static int set_rec_sound = 1;     /* Record: with the sound apps make */
static int set_bin_icon;         /* the Recycle Bin got its desktop icon (once; delete it and it stays away) */
static int set_upd_auto = 1;                  /* look once a day */
static long long set_upd_last;                /* when we last looked (time_t) */
static int set_rtc_local = 1;    /* hardware clock in local time, like Windows */
enum { KBD_US, KBD_PL };
static int set_kbd = KBD_PL;     /* keyboard layout: Polish programmer's (US + AltGr letters) */
static int set_gpu = 0;          /* let apps draw with the graphics chip (experimental) */
static int set_notify_sound = 1; /* a chime when a notification pops up */
static int set_bright = -1;      /* screen brightness 0-100 (laptops); -1: not set */
static int set_off_idx = 2;      /* turn the screen off after: Never, 2, 5, 10, 30 min */
static int set_sleep_idx = 0;    /* sleep after: Never, 15, 30, 60 min */
static int set_lid = 0;          /* closing the lid: 0 sleep, 1 nothing */
#define NOFF 5
static struct {                  /* the battery (power.c fills it in) */
    int present, pct, charging, full, ac, mins;   /* mins: time left (on battery) or to full (charging), -1 unknown */
    char dir[160];
} bat = { .mins = -1 };
static void draw_battery(int x, int y, int pct, int charging, uint32_t line);
#define NSLEEPT 4
static int set_tp_speed = 1;     /* laptop touchpad speed: 0 slow, 1 normal, 2 fast */
static int conf_persistent;   /* is /data a real disk? */

/* the apps ccos knows about: used for taskbar pins and right-click menus */
typedef struct { const char *cmd, *label; int icon; } AppInfo;
static const AppInfo apps[] = {
    { "@explorer", "File Explorer", ICON_FOLDER },
    { "@notepad", "Notepad", ICON_NOTE }, { "@calc", "Calculator", ICON_CALC },
    { "@mines", "Minesweeper", ICON_MINE }, { "@settings", "Settings", ICON_GEAR },
    { "@terminal", "Terminal", ICON_TERMINAL }, { "@solitaire", "Solitaire", ICON_CARDS },
    { "@store", "Software Manager", ICON_BAG },
    { "@photos", "Photos", ICON_PHOTO }, { "@videos", "Videos", ICON_VIDEO }, { "@music", "Music", ICON_MUSIC },
    { "@camera", "Camera", ICON_CAMERA }, { "@record", "Record", ICON_RECORD },
};
/* ---- apps installed with Flatpak (from Flathub) -------------------
 * Flatpak puts a .desktop file for each one in its "exports" folder;
 * their ccos command is "flatpak:APP-ID". */
static void ssid_shown(const unsigned char *s, int n, char *out, size_t cap);
#define FP_MAX 64
#define FP_DIR "/var/lib/flatpak/exports/share/applications"
static struct { char id[96], name[40], cmd[112]; char icon[96]; } fpapps[FP_MAX];
static int img_icon_for(const char *name);   /* the app's picture, loaded once */
static AppInfo fp_info[FP_MAX];
static int nfp;
/* where downloads go: Downloads on the apps disk (lots of room), else on the save disk */
static const char *dl_dir(void) { return access("/apps/home", W_OK) == 0 ? "/apps/home/Downloads" : "/data/Downloads"; }
static int fp_cmp(const void *a, const void *b) { return strcasecmp(((const char *)a) + 96, ((const char *)b) + 96); }
static void flathub_sync(void);
static void fp_scan(void) {
    nfp = 0;
    DIR *d = opendir(FP_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nfp < FP_MAX) {
            size_t L = strlen(e->d_name);
            if (L < 9 || strcmp(e->d_name + L - 8, ".desktop") || L - 8 >= 96) continue;
            char path[400]; snprintf(path, sizeof path, "%s/%s", FP_DIR, e->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char line[512], name[40] = "", icon[96] = ""; int in_entry = 0, hidden = 0;
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
                if (!in_entry) continue;
                if (!strncmp(line, "Name=", 5) && !name[0]) ssid_shown((const unsigned char *)line + 5, (int)strlen(line + 5), name, sizeof name);
                if (!strncmp(line, "Icon=", 5) && !icon[0]) snprintf(icon, sizeof icon, "%s", line + 5);
                if (!strcmp(line, "NoDisplay=true")) hidden = 1;
            }
            fclose(f);
            if (hidden) continue;
            snprintf(fpapps[nfp].id, sizeof fpapps[nfp].id, "%.*s", (int)(L - 8), e->d_name);
            snprintf(fpapps[nfp].name, sizeof fpapps[nfp].name, "%s", name[0] ? name : fpapps[nfp].id);
            snprintf(fpapps[nfp].cmd, sizeof fpapps[nfp].cmd, "flatpak:%s", fpapps[nfp].id);
            snprintf(fpapps[nfp].icon, sizeof fpapps[nfp].icon, "%s", icon[0] ? icon : fpapps[nfp].id);
            nfp++;
        }
        closedir(d);
    }
    qsort(fpapps, (size_t)nfp, sizeof *fpapps, fp_cmp);
    for (int i = 0; i < nfp; i++) {
        int im = img_icon_for(fpapps[i].icon);
        fp_info[i] = (AppInfo){ fpapps[i].cmd, fpapps[i].name, im >= 0 ? ICON_IMG + im : ICON_APP };
    }
    /* the start menu: built-in programs, then the first few Flatpak apps */
    int k = 0; while (k < NBUILTIN && left_items[k].label && strncmp(left_items[k].cmd, "flatpak:", 8)) k++;
    for (int i = 0; i < nfp && i < FP_MENU; i++) left_items[k++] = (MenuItem){ fpapps[i].name, fpapps[i].cmd, fp_info[i].icon };
    left_items[k] = (MenuItem){ NULL, NULL, 0 };
    extern void desk_sync_apps(void);
    desk_sync_apps();                         /* new apps get a desktop shortcut */
    flathub_sync();                           /* ...and a launcher in Downloads > Flathub */
}
/* the ccos command for a Wayland app id ("org.gnome.Calculator" -> "flatpak:org.gnome.Calculator") */
static const char *fp_cmd_for(const char *app_id) {
    for (int i = 0; i < nfp; i++)
        if (!strcmp(fpapps[i].id, app_id) || (!strcmp(app_id, "firefox") && !strcmp(fpapps[i].id, "org.mozilla.firefox")))
            return fpapps[i].cmd;
    return NULL;
}

/* ---- desktop shortcuts ------------------------------------------------ */
static int settings_loaded;
static int store_icon_done;                /* older ccos.conf: Software Manager's icon not added yet */
static char fp_known[4096];                 /* ",id,id," : Flathub apps that already got a shortcut */
static const AppInfo *app_info(const char *cmd);
static int desk_icon_for(const char *cmd) {
    if (!strncmp(cmd, "@explorer ", 10)) return ICON_FOLDER;
    if (!strcmp(cmd, "@explorer")) return ICON_COMPUTER;
    if (!strcmp(cmd, "@explorer /data/.bin")) return ICON_BIN;
    const AppInfo *ai = app_info(cmd);
    return ai ? ai->icon : ICON_APP;
}
#define BIN_CMD "@explorer /data/.bin"
static void pretty_path(const char *p, char *out, size_t n);
#include "bin.c"                 /* the Recycle Bin */
static void repaint_all(void);
static void desk_bin_changed(void) {          /* the desktop Bin: empty or full */
    for (int i = 0; i < ndesk; i++)
        if (!strcmp(desk_icons[i].cmd, BIN_CMD)) {
            int ic = bin_count() ? ICON_BIN_FULL : ICON_BIN;
            if (desk_icons[i].icon != ic) { desk_icons[i].icon = ic; repaint_all(); }
        }
}
static int desk_find(const char *cmd) {
    for (int i = 0; i < ndesk; i++) if (!strcmp(desk_icons[i].cmd, cmd)) return i;
    return -1;
}
static int desk_rows(void);
static void desk_free_cell(int *c, int *r) {           /* first empty spot, column by column */
    int rows = desk_rows();
    for (int cc = 0; cc < 40; cc++)
        for (int rr = 0; rr < rows; rr++) {
            int used = 0;
            for (int i = 0; i < ndesk; i++) if (desk_icons[i].col == cc && desk_icons[i].row == rr) { used = 1; break; }
            if (!used) { *c = cc; *r = rr; return; }
        }
    *c = 0; *r = 0;
}
static void repaint_all(void);
static void save_settings(void);
static int desk_add(const char *cmd, const char *label) {
    if (desk_find(cmd) >= 0 || ndesk >= MAXDESK) return 0;
    DeskIcon *d = &desk_icons[ndesk];
    snprintf(d->cmd, sizeof d->cmd, "%s", cmd);
    snprintf(d->label, sizeof d->label, "%s", label);
    d->icon = desk_icon_for(cmd);
    desk_free_cell(&d->col, &d->row);
    ndesk++;
    return 1;
}
static void desk_remove(int i) {
    if (i < 0 || i >= ndesk) return;
    memmove(&desk_icons[i], &desk_icons[i + 1], sizeof *desk_icons * (size_t)(ndesk - i - 1));
    ndesk--;
}
/* after a Flathub app is installed it gets a desktop shortcut (once - delete it and it stays away);
   after it's uninstalled its shortcut goes */
void desk_sync_apps(void) {
    if (!settings_loaded) return;
    int changed = 0;
    for (int i = 0; i < nfp; i++) {
        char key[100]; snprintf(key, sizeof key, ",%s,", fpapps[i].id);
        if (strstr(fp_known, key)) continue;
        if (strlen(fp_known) + strlen(key) + 1 < sizeof fp_known) {
            if (!fp_known[0]) snprintf(fp_known, sizeof fp_known, ",");
            strcat(fp_known, key + 1);
        }
        desk_add(fpapps[i].cmd, fpapps[i].name);
        changed = 1;
    }
    for (int i = ndesk - 1; i >= 0; i--) {
        if (strncmp(desk_icons[i].cmd, "flatpak:", 8)) continue;
        int found = -1;
        for (int k = 0; k < nfp; k++) if (!strcmp(fpapps[k].cmd, desk_icons[i].cmd)) found = k;
        if (found < 0) { desk_remove(i); changed = 1; }
        else if (desk_icons[i].icon != fp_info[found].icon) { desk_icons[i].icon = fp_info[found].icon; changed = 1; }
    }
    /* uninstalled apps are forgotten, so installing again brings the shortcut back */
    char keep[sizeof fp_known] = ",";
    for (int i = 0; i < nfp; i++) {
        char key[100]; snprintf(key, sizeof key, ",%s,", fpapps[i].id);
        if (strstr(fp_known, key) && strlen(keep) + strlen(key) < sizeof keep) strcat(keep, key + 1);
    }
    if (strcmp(keep, fp_known) && !(keep[1] == 0 && fp_known[0] == 0)) { snprintf(fp_known, sizeof fp_known, "%s", keep); changed = 1; }
    if (changed) { save_settings(); repaint_all(); }
}

/* "@explorer /data/Documents" -> "@explorer" */
static void app_base(const char *cmd, char *out, size_t n) {
    snprintf(out, n, "%s", cmd ? cmd : "");
    char *sp = strchr(out, ' '); if (sp) *sp = 0;
}
static const AppInfo *app_info(const char *cmd) {
    char b[112]; app_base(cmd, b, sizeof b);
    for (size_t i = 0; i < sizeof apps / sizeof *apps; i++) if (!strcmp(apps[i].cmd, b)) return &apps[i];
    for (int i = 0; i < nfp; i++) if (!strcmp(fp_info[i].cmd, b)) return &fp_info[i];
    return NULL;
}

/* apps pinned to the taskbar (saved in ccos.conf) */
#define MAXPINS 10
static char pins[MAXPINS][112];
static int npins = -1;                       /* -1 = nothing saved yet: use the defaults */
static int pin_index(const char *cmd) {
    char b[112]; app_base(cmd, b, sizeof b);
    for (int i = 0; i < npins; i++) if (!strcmp(pins[i], b)) return i;
    return -1;
}

/* ---- layout ------------------------------------------------------ */
#define TASKBAR_H  40
#define ORB_R      17
#define MENU_W     400
#define MENU_H     680
#define ITEM_H     32
#define CLOCK_W    100
#define FRAME      (set_style ? 1 : 8)   /* window side/bottom border (Modern: a thin line) */
#define TITLE_H    30         /* window title bar (incl. top border) */
#define TB_BTN_W   160        /* taskbar button */

/* ---- framebuffer + drawing ----------------------------------------- */
static int W, H, stride, swap_rb;
static uint32_t *fbmem, *back, *wall;   /* back = the scene, without cursor */
static uint8_t *glowmap;                /* wallpaper glow strength per pixel */
static int tty = -1;

#define R(c) (((c) >> 16) & 255)
#define G(c) (((c) >> 8) & 255)
#define B(c) ((c) & 255)

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0, 1);
    int r = (int)R(a) + (int)(((int)R(b) - (int)R(a)) * t);
    int g = (int)G(a) + (int)(((int)G(b) - (int)G(a)) * t);
    int bl = (int)B(a) + (int)(((int)B(b) - (int)B(a)) * t);
    return RGB(clampi(r, 0, 255), clampi(g, 0, 255), clampi(bl, 0, 255));
}

/* Clip rectangle: every drawing call only touches pixels inside it.
 * That lets us repaint just one small area of the scene cheaply. */
static int clip_x0, clip_y0, clip_x1, clip_y1;

static int clip_range(int *x0, int *y0, int *x1, int *y1) {
    *x0 = imax(*x0, clip_x0); *y0 = imax(*y0, clip_y0);
    *x1 = imin(*x1, clip_x1); *y1 = imin(*y1, clip_y1);
    return *x0 < *x1 && *y0 < *y1;
}

static inline void blend(int x, int y, uint32_t c, int a) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1 || a <= 0) return;
    uint32_t *p = &back[y * W + x];
    if (a >= 255) { *p = c; return; }
    uint32_t d = *p;
    *p = RGB((R(c) * a + R(d) * (255 - a)) / 255,
             (G(c) * a + G(d) * (255 - a)) / 255,
             (B(c) * a + B(d) * (255 - a)) / 255);
}

static void fill_rect(int x, int y, int w, int h, uint32_t c, int a) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) blend(i, j, c, a);
}

/* how much of pixel (px,py) is inside a rounded rectangle, 0..1 */
static float round_cov(int px, int py, int x, int y, int w, int h, int r) {
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    float cx = px + 0.5f, cy = py + 0.5f, ox = 0, oy = 0;
    if (cx < x + r) ox = x + r - cx; else if (cx > x + w - r) ox = cx - (x + w - r);
    if (cy < y + r) oy = y + r - cy; else if (cy > y + h - r) oy = cy - (y + h - r);
    if (ox == 0 || oy == 0) return 1;
    return clampf(r + 0.5f - sqrtf(ox * ox + oy * oy), 0, 1);
}

static void fill_round(int x, int y, int w, int h, int r, uint32_t c, int a) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            float cov = round_cov(i, j, x, y, w, h, r);
            if (cov > 0) blend(i, j, c, (int)(a * cov));
        }
}

/* vertical gradient inside a rounded rectangle */
static void fill_round_grad(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int a) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) {
        uint32_t c = mix(top, bot, (float)(j - y) / (h > 1 ? h - 1 : 1));
        for (int i = x0; i < x1; i++) {
            float cov = round_cov(i, j, x, y, w, h, r);
            if (cov > 0) blend(i, j, c, (int)(a * cov));
        }
    }
}

static void stroke_round(int x, int y, int w, int h, int r, uint32_t c, int a) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            /* skip the inside quickly: only the 1px border matters */
            if (i > x + r && i < x + w - r - 1 && j > y && j < y + h - 1) { i = x + w - r - 2; continue; }
            float cov = round_cov(i, j, x, y, w, h, r)
                      - round_cov(i, j, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1);
            if (cov > 0) blend(i, j, c, (int)(a * cov));
        }
}

static void fill_circle(float cx, float cy, float r, uint32_t c, int a) {
    int x0 = (int)(cx - r - 1), y0 = (int)(cy - r - 1), x1 = (int)(cx + r + 2), y1 = (int)(cy + r + 2);
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = clampf(r + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
            if (cov > 0) blend(x, y, c, (int)(a * cov));
        }
}

/* ---- text: UTF-8 in, glyphs out --------------------------------------
 * The fonts have the 256 CP437 characters plus the Polish letters
 * (glyphs 256..273). Text is UTF-8; a byte that isn't valid UTF-8 is
 * taken as a CP437 character (old ccos strings). */
#define NGLYPH 274
static const uint16_t pl_cp[18] = { 0x105, 0x107, 0x119, 0x142, 0x144, 0xF3, 0x15B, 0x17A, 0x17C,      /* ąćęłńóśźż */
                                    0x104, 0x106, 0x118, 0x141, 0x143, 0xD3, 0x15A, 0x179, 0x17B };    /* ĄĆĘŁŃÓŚŹŻ */
static const unsigned char uni_to_cp437[0x3000];
static unsigned char fontx[NGLYPH * 16];                 /* the pixel font, with the Polish letters added */
static int glyph_of_cp(unsigned cp) {
    if (cp < 128) return (int)cp;
    for (int k = 0; k < 18; k++) if (pl_cp[k] == cp) return 256 + k;
    if (cp < 0x3000 && uni_to_cp437[cp]) return uni_to_cp437[cp];
    return '?';
}
/* the next character of s: its glyph; *s moves past it. 0 at the end. */
static int next_glyph(const unsigned char **ps) {
    const unsigned char *s = *ps;
    if (!*s) return 0;
    unsigned c = *s;
    if (c < 0x80) { *ps = s + 1; return (int)c; }
    int extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
    int ok = extra > 0;
    for (int e = 1; ok && e <= extra; e++) if ((s[e] & 0xc0) != 0x80) ok = 0;
    if (!ok) {
        /* cut off at the end of a too-short buffer: draw nothing */
        int cut = extra > 0; for (int e = 1; cut && e <= extra; e++) { if (!s[e]) break; if ((s[e] & 0xc0) != 0x80) cut = 0; }
        if (cut) { int e = 1; while (s[e] && (s[e] & 0xc0) == 0x80) e++; *ps = s + e; return next_glyph(ps); }
        *ps = s + 1; return (int)c;                       /* an old CP437 byte */
    }
    unsigned cp = c & (0x3f >> extra);
    for (int e = 1; e <= extra; e++) cp = cp << 6 | (s[e] & 0x3f);
    *ps = s + 1 + extra;
    return glyph_of_cp(cp);
}
/* characters (not bytes) in s */
static int text_len(const char *s) {
    const unsigned char *p = (const unsigned char *)s; int n = 0;
    while (next_glyph(&p)) n++;
    return n;
}
static int text_w(const char *s) { return 8 * text_len(s); }
/* UTF-8 editing: where the character before / after byte i starts */
static int u8_prev(const char *s, int i) { if (i <= 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--; return i; }
static int u8_next(const char *s, int len, int i) { if (i >= len) return len; i++; while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80) i++; return i; }
static int u8_cols(const char *s, int a, int b) {        /* characters between bytes a and b */
    int n = 0; for (int i = a; i < b; i = u8_next(s, b, i)) n++; return n;
}
static int u8_skip_cols(const char *s, int a, int end, int cols) {   /* byte offset cols characters after a */
    int i = a; while (cols-- > 0 && i < end) i = u8_next(s, end, i); return i;
}
/* cut s to at most max characters without breaking a letter in half */
static void u8_trunc(char *s, int max) {
    const unsigned char *p = (const unsigned char *)s; int n = 0;
    while (*p && n < max) { next_glyph(&p); n++; }
    *(char *)p = 0;
}

/* the pixel font's Polish letters: the plain letter plus its accent */
static void font_init(void) {
    memcpy(fontx, font8x16, 256 * 16);
    static const char base[18] = { 'a', 'c', 'e', 'l', 'n', 'o', 's', 'z', 'z', 'A', 'C', 'E', 'L', 'N', 'O', 'S', 'Z', 'Z' };
    static const char mark[18] = { 'g', '\'', 'g', '/', '\'', '\'', '\'', '\'', '.', 'g', '\'', 'g', '/', '\'', '\'', '\'', '\'', '.' };
    for (int k = 0; k < 18; k++) {
        unsigned char *g = fontx + (256 + k) * 16;
        memcpy(g, font8x16 + (unsigned char)base[k] * 16, 16);
        int up = k >= 9;                              /* capitals start higher: accents go right at the top */
        switch (mark[k]) {
        case '\'':                                    /* acute accent */
            if (up) { g[0] |= 0x0c; g[1] |= 0x18; }
            else    { g[2] |= 0x0c; g[3] |= 0x18; }
            break;
        case '.':                                     /* dot above */
            if (up) { g[0] |= 0x18; }
            else    { g[2] |= 0x18; g[3] |= 0x18; }
            break;
        case 'g':                                     /* ogonek: a little hook under the right side */
            if (base[k] == 'a' || base[k] == 'A') { g[12] |= 0x0c; g[13] |= 0x06; }
            else { g[12] |= 0x18; g[13] |= 0x0c; }
            break;
        case '/':                                     /* the stroke through l / L */
            if (up) { g[6] |= 0x18; g[7] |= 0x30; g[8] |= 0x80; }
            else    { g[6] |= 0x06; g[7] |= 0x60; }
            break;
        }
    }
}

/* one character. style: 0 regular, 1 bold, 2 italic */
static void draw_char_style(int x, int y, int ch, uint32_t c, int a, int style) {
    if (x + 8 <= clip_x0 || x >= clip_x1 || y + 16 <= clip_y0 || y >= clip_y1) return;
    if (ch < 0 || ch >= NGLYPH) ch = '?';
    if (set_smooth) {                                    /* anti-aliased: alpha per pixel */
        const unsigned char *g = (style == 1 ? sf_bold : style == 2 ? sf_ital : sf_reg) + ch * 128;
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++) {
                int al = g[j * 8 + i];
                if (al) blend(x + i, y + j, c, a * al / 255);
            }
        return;
    }
    const unsigned char *g = &fontx[ch * 16];
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 8; i++)
            if (g[j] & (0x80 >> i)) {
                int sx = style == 2 ? (15 - j) / 5 : 0;          /* classic italic: slant */
                blend(x + i + sx, y + j, c, a);
                if (style == 1) blend(x + i + 1, y + j, c, a);   /* classic bold: draw twice */
            }
}
static void draw_char(int x, int y, int ch, uint32_t c, int a) { draw_char_style(x, y, ch, c, a, 0); }

static void draw_text(int x, int y, const char *s, uint32_t c, int a) {
    const unsigned char *p = (const unsigned char *)s; int g;
    while ((g = next_glyph(&p))) { draw_char(x, y, g, c, a); x += 8; }
}
static void draw_text_style(int x, int y, const char *s, uint32_t c, int style) {
    const unsigned char *p = (const unsigned char *)s; int g;
    while ((g = next_glyph(&p))) { draw_char_style(x, y, g, c, 255, style); x += 8; }
}

/* big text (sc = 2: the calculator display, headings) */
static void draw_text_scaled(int x, int y, const char *s, uint32_t c, int sc) {
    const unsigned char *p = (const unsigned char *)s; int ch;
    for (; (ch = next_glyph(&p)); x += 8 * sc) {
        if (x + 8 * sc <= clip_x0 || x >= clip_x1 || y + 16 * sc <= clip_y0 || y >= clip_y1) continue;
        if (ch >= NGLYPH) ch = '?';
        if (set_smooth && sc == 2) {
            const unsigned char *g = sf2_reg + ch * 512;
            for (int j = 0; j < 32; j++)
                for (int i = 0; i < 16; i++) { int al = g[j * 16 + i]; if (al) blend(x + i, y + j, c, al); }
            continue;
        }
        const unsigned char *g = &fontx[ch * 16];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if (g[j] & (0x80 >> i)) fill_rect(x + i * sc, y + j * sc, sc, sc, c, 255);
    }
}
static void draw_text_bold(int x, int y, const char *s, uint32_t c) { draw_text_style(x, y, s, c, 1); }

/* white text with a soft dark shadow, like Windows 7 taskbar labels */
static void draw_text_shadow(int x, int y, const char *s) {
    draw_text(x + 1, y + 1, s, RGB(0, 0, 0), 140);
    draw_text(x, y, s, RGB(255, 255, 255), 255);
}

/* dark text with a soft white glow, like Windows 7 window titles */
static void draw_text_glow(int x, int y, const char *s) {
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) draw_text(x + dx, y + dy, s, RGB(255, 255, 255), 90);
    draw_text(x, y, s, RGB(0x10, 0x10, 0x10), 255);
}

/* ---- icons ------------------------------------------------------------- */
static void draw_gear(float cx, float cy, float r) {
    int x0 = (int)(cx - r - 1), y0 = (int)(cy - r - 1), x1 = (int)(cx + r + 2), y1 = (int)(cy + r + 2);
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy), ang = atan2f(dy, dx);
            float edge = cosf(8 * ang) > 0.1f ? r : r * 0.74f;          /* 8 teeth */
            float cov = clampf(edge + 0.5f - d, 0, 1) * clampf(d - r * 0.3f + 0.5f, 0, 1);
            if (cov <= 0) continue;
            uint32_t c = mix(RGB(0xc8, 0xcc, 0xd2), RGB(0x58, 0x60, 0x6c), (dy + r) / (2 * r));
            if (d > edge - 1.2f || d < r * 0.3f + 1.2f) c = mix(c, RGB(0x30, 0x36, 0x40), 0.6f);
            blend(x, y, c, (int)(255 * cov));
        }
}

static void draw_calc_icon(int cx, int cy, int r) {
    int w = r * 3 / 2, h = r * 2, x = cx - w / 2, y = cy - r;
    fill_round_grad(x, y, w, h, 2, RGB(0xe4, 0xe8, 0xee), RGB(0x8c, 0x96, 0xa4), 255);
    stroke_round(x, y, w, h, 2, RGB(0x30, 0x38, 0x44), 230);
    fill_rect(x + 2, y + 2, w - 4, h / 4, RGB(0x9c, 0xd8, 0x9c), 255);      /* screen */
    for (int j = 0; j < 3; j++)                                              /* keys */
        for (int i = 0; i < 3; i++)
            fill_rect(x + 2 + i * (w - 4) / 3 + 1, y + h / 4 + 4 + j * (h * 3 / 4 - 5) / 3,
                      (w - 4) / 3 - 1, (h * 3 / 4 - 5) / 3 - 1,
                      (i == 2 && j == 2) ? RGB(0xe0, 0x70, 0x30) : RGB(0x40, 0x48, 0x56), 255);
}

/* a naval mine: black ball with spikes and a shine */
static void draw_mine(int cx, int cy, int r) {
    for (int k = 0; k < 8; k++) {
        float a = k * 3.14159265f / 4;
        for (float t = 0; t <= r; t += 0.5f)
            fill_rect(cx + (int)roundf(cosf(a) * t), cy + (int)roundf(sinf(a) * t), r > 12 ? 2 : 1, r > 12 ? 2 : 1,
                      RGB(0x10, 0x10, 0x10), 255);
    }
    fill_circle(cx + 0.5f, cy + 0.5f, r * 0.68f, RGB(0x10, 0x10, 0x14), 255);
    fill_circle(cx - r * 0.22f, cy - r * 0.22f, r * 0.2f, RGB(255, 255, 255), 230);
}

static void draw_note_icon(int cx, int cy, int r) {
    int w = r * 3 / 2, h = r * 2, x = cx - w / 2, y = cy - r;
    fill_rect(x, y, w, h, RGB(0xfa, 0xfc, 0xff), 255);
    fill_rect(x, y, w, 3, RGB(0x4a, 0x8c, 0xd8), 255);                         /* binding */
    for (int ly = y + 6; ly < y + h - 2; ly += 3)
        fill_rect(x + 2, ly, w - 4, 1, RGB(0x8c, 0xb4, 0xe0), 255);           /* lines */
    stroke_round(x, y, w, h, 1, RGB(0x50, 0x60, 0x74), 230);
}

static void draw_folder_icon(int cx, int cy, int r);
static void draw_folder_tinted(int cx, int cy, int r, int kind);
static void draw_file_icon(int cx, int cy, int r);
static void draw_drive_icon(int cx, int cy, int r);
static void draw_usb_icon(int cx, int cy, int r);
static void draw_battery_icon(int cx, int cy, int r);
static void draw_moon_icon(int cx, int cy, int r);
static void draw_sun_icon(int cx, int cy, int r);
static void draw_printer_icon(int cx, int cy, int r);
static void draw_zip_icon(int cx, int cy, int r);
static void draw_computer_icon(int cx, int cy, int r);

/* a blue globe with latitude/longitude lines */
static void draw_globe_icon(int cx, int cy, int r) {
    int x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 2, y1 = cy + r + 2;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            float cov = clampf(r + 0.5f - d, 0, 1);
            if (cov <= 0) continue;
            uint32_t c = mix(RGB(0x6c, 0xc4, 0xf8), RGB(0x10, 0x4c, 0xa8), (dy + r) / (2.0f * r));
            float nx = dx / r, ny = dy / r;
            float lat = fabsf(ny - 0.0f), lat2 = fabsf(fabsf(ny) - 0.5f);
            float mer = fabsf(nx), mer2 = fabsf(fabsf(nx) - 0.55f * sqrtf(fmaxf(0, 1 - ny * ny)));
            float lw = 1.2f / r;
            if (lat < lw || lat2 < lw || mer < lw || mer2 < lw) c = mix(c, RGB(255, 255, 255), 0.55f);
            if (dx < -r * 0.2f && dy < -r * 0.2f && d < r * 0.75f) c = mix(c, RGB(255, 255, 255), 0.18f);  /* shine */
            if (d > r - 1.3f) c = mix(c, RGB(0x08, 0x28, 0x60), 0.6f);
            blend(x, y, c, (int)(255 * cov));
        }
}

static void draw_terminal_icon(int cx, int cy, int r);
static void draw_app_icon(int cx, int cy, int r);
static void draw_cards_icon(int cx, int cy, int r);
static void draw_bag_icon(int cx, int cy, int r);
static void draw_photo_icon(int cx, int cy, int r);
static void draw_video_icon(int cx, int cy, int r);
static void draw_music_app_icon(int cx, int cy, int r);
/* ---- app pictures --------------------------------------------------------
 * Flatpak puts each app's icon in its "exports" folder, in several sizes
 * (PNG) and/or as a drawing (SVG). We load it once at 64 x 64 and shrink it
 * smoothly when drawing. */
#define IMG_S 64
#define MAXIMG 32
static struct { char name[96]; uint8_t px[IMG_S * IMG_S * 4]; } imgs[MAXIMG];   /* RGBA, not premultiplied */
static int nimg;
static int img_from_rgba(uint8_t *out, const uint8_t *in, int w, int h) {   /* fit into 64 x 64 */
    memset(out, 0, IMG_S * IMG_S * 4);
    int s = imax(w, h); if (s <= 0) return 0;
    int dw = w * IMG_S / s, dh = h * IMG_S / s, ox = (IMG_S - dw) / 2, oy = (IMG_S - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            /* average the source pixels that land on this one (weighted by alpha) */
            int x0 = x * w / dw, x1 = imax(x0 + 1, (x + 1) * w / dw), y0 = y * h / dh, y1 = imax(y0 + 1, (y + 1) * h / dh);
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t *p = in + ((size_t)sy * w + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            uint8_t *o = out + ((size_t)(oy + y) * IMG_S + ox + x) * 4;
            if (a) { o[0] = r / a; o[1] = g / a; o[2] = b / a; }
            o[3] = a / n;
        }
    return 1;
}
static int img_load_file(const char *path, uint8_t *out) {
    size_t L = strlen(path);
    if (L > 4 && !strcmp(path + L - 4, ".svg")) {
        NSVGimage *im = nsvgParseFromFile(path, "px", 96);
        if (!im) return 0;
        int ok = 0;
        if (im->width > 0 && im->height > 0) {
            float sc = 256.0f / fmaxf(im->width, im->height);
            int w = (int)(im->width * sc + 0.5f), h = (int)(im->height * sc + 0.5f);
            uint8_t *buf = calloc((size_t)w * h, 4);
            NSVGrasterizer *rs = nsvgCreateRasterizer();
            if (buf && rs) { nsvgRasterize(rs, im, 0, 0, sc, buf, w, h, w * 4); ok = img_from_rgba(out, buf, w, h); }
            if (rs) nsvgDeleteRasterizer(rs);
            free(buf);
        }
        nsvgDelete(im);
        return ok;
    }
    int w, h, c;
    uint8_t *buf = stbi_load(path, &w, &h, &c, 4);
    if (!buf) return 0;
    int ok = img_from_rgba(out, buf, w, h);
    stbi_image_free(buf);
    return ok;
}
static int img_icon_for(const char *name) {
    for (int i = 0; i < nimg; i++) if (!strcmp(imgs[i].name, name)) return i;
    if (nimg >= MAXIMG || !name[0] || strchr(name, '/')) return -1;
    static const char *sizes[] = { "128x128", "64x64", "96x96", "256x256", "512x512", "48x48", "scalable", "32x32" };
    char path[400];
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        snprintf(path, sizeof path, "/var/lib/flatpak/exports/share/icons/hicolor/%s/apps/%s.%s",
                 sizes[k], name, strcmp(sizes[k], "scalable") ? "png" : "svg");
        if (access(path, R_OK) == 0 && img_load_file(path, imgs[nimg].px)) {
            snprintf(imgs[nimg].name, sizeof imgs[nimg].name, "%s", name);
            return nimg++;
        }
    }
    return -1;
}
static void draw_img_icon(int n, int cx, int cy, int r) {
    if (n < 0 || n >= nimg) { draw_app_icon(cx, cy, r); return; }
    int s = 2 * r + 2, x0 = cx - s / 2, y0 = cy - s / 2;
    const uint8_t *px = imgs[n].px;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            /* box filter from 64 x 64 down to s x s (or bilinear-ish when larger) */
            float fx0 = (float)x * IMG_S / s, fx1 = (float)(x + 1) * IMG_S / s;
            float fy0 = (float)y * IMG_S / s, fy1 = (float)(y + 1) * IMG_S / s;
            int ix0 = (int)fx0, ix1 = imax(ix0 + 1, (int)ceilf(fx1)), iy0 = (int)fy0, iy1 = imax(iy0 + 1, (int)ceilf(fy1));
            unsigned r_ = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = iy0; sy < iy1 && sy < IMG_S; sy++)
                for (int sx = ix0; sx < ix1 && sx < IMG_S; sx++) {
                    const uint8_t *p = px + ((size_t)sy * IMG_S + sx) * 4;
                    r_ += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; cnt++;
                }
            if (!a) continue;
            blend(x0 + x, y0 + y, RGB(r_ / a, g / a, b / a), (int)(a / cnt));
        }
}

/* ===================================================================
 *  THE MODERN STYLE: colours, dark mode, smooth icons
 * =================================================================== */
typedef struct {
    uint32_t title, title_in, border, border_in, text, text_in, sub,
             bar, bar_line, bar_hover, bar_active, accent,
             menu, menu_left, menu_line, hover, hover_line, btn, btn_hover;
} Theme;
static const Theme th_light = {
    .title = RGB(0xff, 0xff, 0xff), .title_in = RGB(0xf6, 0xf7, 0xf9), .border = RGB(0x5c, 0x8c, 0xd8), .border_in = RGB(0xb8, 0xbe, 0xc6),
    .text = RGB(0x1a, 0x1c, 0x20), .text_in = RGB(0x8a, 0x8e, 0x94), .sub = RGB(0x6a, 0x70, 0x78),
    .bar = RGB(0xf2, 0xf3, 0xf5), .bar_line = RGB(0xd6, 0xd9, 0xde), .bar_hover = RGB(0xe2, 0xe5, 0xea), .bar_active = RGB(0xd8, 0xe4, 0xf4),
    .accent = RGB(0x26, 0x74, 0xd8),
    .menu = RGB(0xf4, 0xf5, 0xf7), .menu_left = RGB(0xff, 0xff, 0xff), .menu_line = RGB(0xd2, 0xd6, 0xdc),
    .hover = RGB(0xe4, 0xee, 0xfa), .hover_line = RGB(0xb6, 0xd0, 0xf2), .btn = RGB(0xe6, 0xe8, 0xec), .btn_hover = RGB(0xd6, 0xe4, 0xf6),
};
static const Theme th_dark = {
    .title = RGB(0x24, 0x26, 0x2a), .title_in = RGB(0x2c, 0x2e, 0x32), .border = RGB(0x4c, 0x7c, 0xc8), .border_in = RGB(0x46, 0x48, 0x4e),
    .text = RGB(0xee, 0xf0, 0xf3), .text_in = RGB(0x8e, 0x92, 0x98), .sub = RGB(0xa4, 0xa8, 0xb0),
    .bar = RGB(0x1c, 0x1d, 0x20), .bar_line = RGB(0x34, 0x36, 0x3a), .bar_hover = RGB(0x34, 0x37, 0x3c), .bar_active = RGB(0x33, 0x3e, 0x4e),
    .accent = RGB(0x4c, 0x9a, 0xff),
    .menu = RGB(0x22, 0x24, 0x28), .menu_left = RGB(0x2c, 0x2e, 0x33), .menu_line = RGB(0x3c, 0x3f, 0x44),
    .hover = RGB(0x3a, 0x44, 0x52), .hover_line = RGB(0x4e, 0x5e, 0x74), .btn = RGB(0x38, 0x3a, 0x40), .btn_hover = RGB(0x40, 0x50, 0x68),
};
static const Theme *TH(void) { return set_dark ? &th_dark : &th_light; }
#define MODERN (set_style == 1)
#define DARK   (set_style == 1 && set_dark)

/* Dark mode for apps that only know light colours: light, greyish colours
 * turn dark (and dark text light); bright colours (blue buttons, a yellow
 * folder) stay. Pictures and icons are left alone ("keep" areas). */
static int dk_collect, nkeep;                 /* nkeep: unused (kept for older calls) */
static uint8_t *dk_mask;                      /* 1 = a picture or icon pixel: leave it */
static void dk_clear(int x, int y, int w, int h) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!dk_mask || !clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) memset(dk_mask + (size_t)j * W + x0, 0, (size_t)(x1 - x0));
}
static void dark_keep(int x, int y, int w, int h) {         /* a whole picture */
    if (!dk_collect || !dk_mask || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) memset(dk_mask + (size_t)j * W + x0, 1, (size_t)(x1 - x0));
}
static inline uint32_t dark_px(uint32_t c) {
    int r = R(c), g = G(c), b = B(c);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn >= 70 && mx + mn <= 400 && mx + mn >= 230) return c;   /* a clear, mid colour (blue button): keep it */
    int s = 255 - mx - mn;                                      /* flip how light it is, keep the tint */
    r = clampi(r + s, 0, 255); g = clampi(g + s, 0, 255); b = clampi(b + s, 0, 255);
    return RGB(0x1c + r * (0xe8 - 0x1c) / 255, 0x1d + g * (0xe8 - 0x1d) / 255, 0x21 + b * (0xea - 0x21) / 255);
}
static void dark_xform(int x, int y, int w, int h) {             /* within the clip, skipping keep areas */
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &back[(size_t)j * W];
        const uint8_t *mk = dk_mask ? dk_mask + (size_t)j * W : NULL;
        for (int i = x0; i < x1; i++) if (!mk || !mk[i]) row[i] = dark_px(row[i]);
    }
}

/* Smooth icons: the same drawings, made at 4x the size and shrunk, so every
 * edge is anti-aliased. Made once per icon and size, then kept. */
static void draw_icon_raw(int icon, int cx, int cy, int r);
#define ICACHE 96
static struct { int icon, r, s; uint32_t *px; } icache[ICACHE];   /* ARGB, premultiplied */
static int nicache;
static const uint32_t *icon_smooth(int icon, int r, int *size) {
    for (int i = 0; i < nicache; i++) if (icache[i].icon == icon && icache[i].r == r) { *size = icache[i].s; return icache[i].px; }
    int m = r / 2 + 4, s = 2 * r + 2 * m, S = s * 4;
    uint32_t *cv[2]; cv[0] = malloc((size_t)S * S * 4); cv[1] = malloc((size_t)S * S * 4);
    uint32_t *out = malloc((size_t)s * s * 4);
    if (!cv[0] || !cv[1] || !out) { free(cv[0]); free(cv[1]); free(out); return NULL; }
    /* draw on black and on white: the difference tells how see-through each pixel is */
    uint32_t *sb = back; int sw = W, sh = H, c0 = clip_x0, c1 = clip_y0, c2 = clip_x1, c3 = clip_y1;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < S * S; i++) cv[k][i] = k ? 0xffffff : 0;
        back = cv[k]; W = S; H = S; clip_x0 = 0; clip_y0 = 0; clip_x1 = S; clip_y1 = S;
        draw_icon_raw(icon, S / 2, S / 2, r * 4);
    }
    back = sb; W = sw; H = sh; clip_x0 = c0; clip_y0 = c1; clip_x1 = c2; clip_y1 = c3;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            unsigned kb[3] = { 0, 0, 0 }, wh[3] = { 0, 0, 0 };
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    uint32_t a = cv[0][(y * 4 + j) * S + x * 4 + i], b = cv[1][(y * 4 + j) * S + x * 4 + i];
                    kb[0] += R(a); kb[1] += G(a); kb[2] += B(a); wh[0] += R(b); wh[1] += G(b); wh[2] += B(b);
                }
            int al = 255 - (int)((wh[0] - kb[0] + wh[1] - kb[1] + wh[2] - kb[2]) / 48);
            al = clampi(al, 0, 255);
            out[y * s + x] = al ? ((uint32_t)al << 24) | RGB(imin(255, kb[0] / 16), imin(255, kb[1] / 16), imin(255, kb[2] / 16)) : 0;
        }
    free(cv[0]); free(cv[1]);
    if (nicache >= ICACHE) { free(icache[0].px); memmove(icache, icache + 1, sizeof icache[0] * (ICACHE - 1)); nicache--; }
    icache[nicache].icon = icon; icache[nicache].r = r; icache[nicache].s = s; icache[nicache].px = out; nicache++;
    *size = s;
    return out;
}
/* dark mode, for something see-through drawn over the app (an icon, a picture
   with soft edges): paint it on the dark version of the background, so its
   edges match, and remember which pixels are its own */
static struct { int x0, y0, w, h; uint32_t *save; } dkb;
static void dk_begin(int x, int y, int w, int h) {
    dkb.save = NULL;
    if (!dk_collect || !dk_mask) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    dkb.x0 = x0; dkb.y0 = y0; dkb.w = x1 - x0; dkb.h = y1 - y0;
    dkb.save = malloc((size_t)dkb.w * dkb.h * 4);
    if (!dkb.save) return;
    for (int j = 0; j < dkb.h; j++)
        for (int i = 0; i < dkb.w; i++) {
            size_t o = (size_t)(y0 + j) * W + x0 + i;
            dkb.save[(size_t)j * dkb.w + i] = back[o];
            if (!dk_mask[o]) back[o] = dark_px(back[o]);
        }
}
static void dk_end(void) {
    if (!dkb.save) return;
    for (int j = 0; j < dkb.h; j++)
        for (int i = 0; i < dkb.w; i++) {
            size_t o = (size_t)(dkb.y0 + j) * W + dkb.x0 + i;
            uint32_t was = dkb.save[(size_t)j * dkb.w + i];
            if (dk_mask[o]) continue;
            if (back[o] != dark_px(was)) dk_mask[o] = 1;          /* painted here */
            else back[o] = was;                                   /* untouched: dark mode does it later */
        }
    free(dkb.save); dkb.save = NULL;
}
static void draw_icon_inner(int icon, int cx, int cy, int r);
static void draw_icon(int icon, int cx, int cy, int r) {
    int m = r / 2 + 6;
    dk_begin(cx - r - m, cy - r - m, 2 * (r + m), 2 * (r + m));
    draw_icon_inner(icon, cx, cy, r);
    dk_end();
}
static void draw_icon_inner(int icon, int cx, int cy, int r) {
    if (MODERN && icon < ICON_IMG && icon != ICON_NONE && r >= 4 && r <= 64) {
        int s; const uint32_t *px = icon_smooth(icon, r, &s);
        if (px) {
            int x0 = cx - s / 2, y0 = cy - s / 2, xa = x0, ya = y0, xb = x0 + s, yb = y0 + s;
            if (!clip_range(&xa, &ya, &xb, &yb)) return;
            for (int y = ya; y < yb; y++)
                for (int x = xa; x < xb; x++) {
                    uint32_t c = px[(y - y0) * s + (x - x0)], a = c >> 24;
                    if (!a) continue;
                    uint32_t d = back[y * W + x];                  /* premultiplied: c + d * (1 - a) */
                    back[y * W + x] = RGB(imin(255, R(c) + R(d) * (255 - a) / 255), imin(255, G(c) + G(d) * (255 - a) / 255),
                                          imin(255, B(c) + B(d) * (255 - a) / 255));
                }
            return;
        }
    }
    draw_icon_raw(icon, cx, cy, r);
}
static void draw_bin_icon(int cx, int cy, int r, int full);
static void draw_camera_icon(int cx, int cy, int r);
static void draw_record_icon(int cx, int cy, int r);
static void draw_search_icon(int cx, int cy, int r);
static void draw_icon_raw(int icon, int cx, int cy, int r) {
    if (icon >= ICON_IMG) draw_img_icon(icon - ICON_IMG, cx, cy, r);
    else if (icon == ICON_BIN || icon == ICON_BIN_FULL) draw_bin_icon(cx, cy, r, icon == ICON_BIN_FULL);
    else if (icon == ICON_CAMERA) draw_camera_icon(cx, cy, r);
    else if (icon == ICON_RECORD) draw_record_icon(cx, cy, r);
    else if (icon == ICON_SEARCH) draw_search_icon(cx, cy, r);
    else if (icon == ICON_GLOBE) draw_globe_icon(cx, cy, r);
    else if (icon == ICON_FOLDER) draw_folder_icon(cx, cy, r);
    else if (icon == ICON_FILE) draw_file_icon(cx, cy, r);
    else if (icon == ICON_DRIVE) draw_drive_icon(cx, cy, r);
    else if (icon == ICON_USB) draw_usb_icon(cx, cy, r);
    else if (icon == ICON_BATTERY) draw_battery_icon(cx, cy, r);
    else if (icon == ICON_MOON) draw_moon_icon(cx, cy, r);
    else if (icon == ICON_SUN) draw_sun_icon(cx, cy, r);
    else if (icon == ICON_PRINTER) draw_printer_icon(cx, cy, r);
    else if (icon == ICON_ZIP) draw_zip_icon(cx, cy, r);
    else if (icon == ICON_COMPUTER) draw_computer_icon(cx, cy, r);
    else if (icon == ICON_GEAR) draw_gear(cx, cy, r);
    else if (icon == ICON_CALC) draw_calc_icon(cx, cy, r);
    else if (icon == ICON_NOTE) draw_note_icon(cx, cy, r);
    else if (icon == ICON_MINE) draw_mine(cx, cy, r);
    else if (icon == ICON_TERMINAL) draw_terminal_icon(cx, cy, r);
    else if (icon == ICON_APP) draw_app_icon(cx, cy, r);
    else if (icon == ICON_CARDS) draw_cards_icon(cx, cy, r);
    else if (icon == ICON_BAG) draw_bag_icon(cx, cy, r);
    else if (icon == ICON_PHOTO) draw_photo_icon(cx, cy, r);
    else if (icon == ICON_VIDEO) draw_video_icon(cx, cy, r);
    else if (icon == ICON_MUSIC) draw_music_app_icon(cx, cy, r);
    else if (icon == ICON_FOLDER_IMG || icon == ICON_FOLDER_DL || icon == ICON_FOLDER_VID) draw_folder_tinted(cx, cy, r, icon);
}

/* ---- wallpaper: gradient + soft glow ------------------------------- */
static void make_glowmap(void) {               /* the expensive part, once */
    float gx = W * 0.5f, gy = H * 0.42f, gr = H * 0.75f;
    for (int y = 0; y < H; y++) {
        float dy2 = (y - gy) * (y - gy) * 1.6f;
        for (int x = 0; x < W; x++) {
            float d = sqrtf((x - gx) * (x - gx) + dy2) / gr;
            glowmap[y * W + x] = (uint8_t)(clampf(1 - d, 0, 1) * 0.45f * 255);
        }
    }
}

/* the background picture, decoded once */
static struct { char path[300]; uint8_t *px; int w, h; } wimg;
static void wall_picture(void) {
    if (!set_wall_img[0]) return;
    if (strcmp(wimg.path, set_wall_img)) {
        if (wimg.px) stbi_image_free(wimg.px);
        int c; wimg.px = stbi_load(set_wall_img, &wimg.w, &wimg.h, &c, 4);
        snprintf(wimg.path, sizeof wimg.path, "%s", set_wall_img);
    }
    if (!wimg.px || wimg.w <= 0 || wimg.h <= 0) return;
    int iw = wimg.w, ih = wimg.h, mode = set_wall_fit;
    /* where the picture goes: dw x dh at (dx, dy); Tile repeats it */
    double sx = 1, sy = 1;
    if (mode == WF_FILL || mode == WF_SPAN) sx = sy = fmax((double)W / iw, (double)H / ih);
    else if (mode == WF_FIT) sx = sy = fmin((double)W / iw, (double)H / ih);
    else if (mode == WF_STRETCH) { sx = (double)W / iw; sy = (double)H / ih; }
    int dw = (int)(iw * sx + 0.5), dh = (int)(ih * sy + 0.5);
    int dx = (W - dw) / 2, dy = (H - dh) / 2;
    if (mode == WF_TILE) { dx = 0; dy = 0; }
    /* how many source pixels to average for each screen pixel (smooth when shrinking) */
    int ax = imax(1, imin(4, (int)(1 / sx))), ay = imax(1, imin(4, (int)(1 / sy)));
    for (int y = 0; y < H; y++) {
        int yy = y - dy;
        if (mode == WF_TILE) yy = y % ih;
        else if (yy < 0 || yy >= dh) continue;
        for (int x = 0; x < W; x++) {
            int xx = x - dx;
            if (mode == WF_TILE) xx = x % iw;
            else if (xx < 0 || xx >= dw) continue;
            double fx = mode == WF_TILE ? xx : (xx + 0.5) / sx - 0.5, fy = mode == WF_TILE ? yy : (yy + 0.5) / sy - 0.5;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int j = 0; j < ay; j++)
                for (int i = 0; i < ax; i++) {
                    int px = clampi((int)(fx + i - ax / 2.0 + 0.5), 0, iw - 1), py = clampi((int)(fy + j - ay / 2.0 + 0.5), 0, ih - 1);
                    const uint8_t *p = wimg.px + ((size_t)py * iw + px) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            uint32_t d = wall[y * W + x], al = a / n;
            wall[y * W + x] = RGB((r / n * al + R(d) * (255 - al)) / 255, (g / n * al + G(d) * (255 - al)) / 255, (b / n * al + B(d) * (255 - al)) / 255);
        }
    }
}
/* use a picture as the background: keep a copy on D: (the original may go away) */
static void save_settings(void);
static int set_wallpaper_file(const char *src) {
    const char *dot = strrchr(src, '.');
    char dst[300]; snprintf(dst, sizeof dst, "/data/wallpaper%s", dot && strlen(dot) < 8 ? dot : ".jpg");
    int w, h, c;
    if (!stbi_info(src, &w, &h, &c)) return 0;                 /* not a picture we can show */
    mkdir("/data", 0755);
    if (strcmp(src, dst)) {
        DIR *d = opendir("/data"); struct dirent *e;               /* the old copy */
        while (d && (e = readdir(d))) if (!strncmp(e->d_name, "wallpaper.", 10)) { char o[300]; snprintf(o, sizeof o, "/data/%s", e->d_name); unlink(o); }
        if (d) closedir(d);
        FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
        if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return 0; }
        char buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
        fclose(in); fflush(out); fsync(fileno(out)); fclose(out);
    }
    snprintf(set_wall_img, sizeof set_wall_img, "%s", dst);
    wimg.path[0] = 0;                                           /* load it again */
    save_settings();
    return 1;
}
static void make_wallpaper(void) {
    const Wallpaper *wp = &wallpapers[set_wall];
    for (int y = 0; y < H; y++) {
        uint32_t row = mix(wp->top, wp->bottom, (float)y / H);
        for (int x = 0; x < W; x++) {
            int g = glowmap[y * W + x];
            uint32_t c = row, gc = wp->glow;
            wall[y * W + x] = g ? RGB((R(c) * (255 - g) + R(gc) * g) / 255,
                                      (G(c) * (255 - g) + G(gc) * g) / 255,
                                      (B(c) * (255 - g) + B(gc) * g) / 255) : c;
        }
    }
    wall_picture();
}

/* ---- settings file -------------------------------------------------- */
static void load_settings(void) {
    settings_loaded = 1;
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[4096];
    int had_desk = 0;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "wallpaper=%d", &v) == 1 && v >= 0 && v < NWALL) set_wall = v;
        if (sscanf(line, "clock24=%d", &v) == 1) set_clock24 = !!v;
        if (sscanf(line, "sound=%d", &v) == 1) set_sound = !!v;
        if (sscanf(line, "volume=%d", &v) == 1) set_volume = clampi(v, 0, 100);
        if (sscanf(line, "wallfit=%d", &v) == 1 && v >= 0 && v < NWF) set_wall_fit = v;
        if (!strncmp(line, "wallimg=", 8)) { snprintf(set_wall_img, sizeof set_wall_img, "%s", line + 8); set_wall_img[strcspn(set_wall_img, "\n")] = 0; }
        if (sscanf(line, "muted=%d", &v) == 1) set_muted = !!v;
        if (sscanf(line, "net=%d", &v) == 1) set_net = !!v;
        if (sscanf(line, "wifi=%d", &v) == 1) set_wifi = !!v;
        if (sscanf(line, "style=%d", &v) == 1) set_style = set_smooth = !!v;
        if (sscanf(line, "dark=%d", &v) == 1) set_dark = !!v;
        if (sscanf(line, "cursor2=%d", &v) == 1 && v >= 0 && v < 3) set_cursor_col = v;   /* (the old "cursor=" was the old pointer: black is the new look) */
        if (sscanf(line, "cursorsize=%d", &v) == 1 && v >= 0 && v < 3) set_cursor_size = v;
        if (sscanf(line, "tpspeed=%d", &v) == 1 && v >= 0 && v < 3) set_tp_speed = v;
        if (sscanf(line, "bright=%d", &v) == 1 && v >= 5 && v <= 100) set_bright = v;
        if (sscanf(line, "screenoff=%d", &v) == 1 && v >= 0 && v < NOFF) set_off_idx = v;
        if (sscanf(line, "sleepafter=%d", &v) == 1 && v >= 0 && v < NSLEEPT) set_sleep_idx = v;
        if (sscanf(line, "lid=%d", &v) == 1) set_lid = !!v;
        if (sscanf(line, "notifysound=%d", &v) == 1) set_notify_sound = !!v;
        if (sscanf(line, "gpu=%d", &v) == 1) set_gpu = !!v;
        if (sscanf(line, "kbd=%d", &v) == 1) set_kbd = v == KBD_US ? KBD_US : KBD_PL;
        if (!strncmp(line, "tz=", 3)) { snprintf(set_tz, sizeof set_tz, "%s", line + 3); set_tz[strcspn(set_tz, "\r\n")] = 0; }
        if (sscanf(line, "tzauto=%d", &v) == 1) set_tz_auto = !!v;
        if (sscanf(line, "ntp=%d", &v) == 1) set_ntp = !!v;
        if (sscanf(line, "upd_auto=%d", &v) == 1) set_upd_auto = !!v;
        if (sscanf(line, "binicon=%d", &v) == 1) set_bin_icon = !!v;
        if (sscanf(line, "recsound=%d", &v) == 1) set_rec_sound = !!v;
        if (!strncmp(line, "upd_last=", 9)) set_upd_last = atoll(line + 9);
        if (!strncmp(line, "upd_repo=", 9)) { snprintf(set_upd_repo, sizeof set_upd_repo, "%s", line + 9); set_upd_repo[strcspn(set_upd_repo, "\r\n")] = 0; }
        if (!strncmp(line, "upd_api=", 8)) { snprintf(set_upd_api, sizeof set_upd_api, "%s", line + 8); set_upd_api[strcspn(set_upd_api, "\r\n")] = 0; }
        if (sscanf(line, "rtclocal=%d", &v) == 1) set_rtc_local = !!v;
        if (!strncmp(line, "pins=", 5)) {
            npins = 0;
            char *save = NULL;
            for (char *tok = strtok_r(line + 5, ",\n", &save); tok && npins < MAXPINS; tok = strtok_r(NULL, ",\n", &save)) {
                if (app_info(tok)) snprintf(pins[npins++], sizeof pins[0], "%s", tok);
            }
        }
        int i, c, r;
        if (!had_desk && sscanf(line, "icon%d=%d,%d", &i, &c, &r) == 3 && i >= 0 && i < 6 && c >= 0 && r >= 0) {
            desk_icons[i].col = c; desk_icons[i].row = r;       /* older ccos: built-in icons only */
        }
        if (!strncmp(line, "desk=", 5)) {                       /* desk=col,row,command|label|icon */
            if (!had_desk) { had_desk = 1; ndesk = 0; }
            char cmd[112] = "", lab[40] = "";
            int n = 0;
            if (sscanf(line + 5, "%d,%d,%n", &c, &r, &n) == 2 && n > 0 && ndesk < MAXDESK) {
                char *s = line + 5 + n, *bar = strchr(s, '|');
                s[strcspn(s, "\n")] = 0;
                if (bar) { *bar = 0; snprintf(lab, sizeof lab, "%s", bar + 1); }
                snprintf(cmd, sizeof cmd, "%s", s);
                const AppInfo *ai = app_info(cmd);
                if (ai || !strncmp(cmd, "flatpak:", 8) || !strncmp(cmd, "@explorer", 9)) {
                    DeskIcon *d = &desk_icons[ndesk++];
                    snprintf(d->cmd, sizeof d->cmd, "%s", cmd);
                    snprintf(d->label, sizeof d->label, "%s", lab[0] ? lab : ai ? ai->label : cmd);
                    d->icon = desk_icon_for(cmd);
                    d->col = c; d->row = r;
                }
            }
        }
        if (sscanf(line, "storeicon=%d", &v) == 1) store_icon_done = v;
        if (!strncmp(line, "fpknown=", 8)) snprintf(fp_known, sizeof fp_known, "%s", line + 8), fp_known[strcspn(fp_known, "\n")] = 0;
    }
    fclose(f);
}

static void stick_late_check(void);
static void save_settings(void) {
    if (!conf_persistent) {                       /* the stick turned up: load its settings, don't overwrite them */
        struct stat a, b;
        if (stat("/data", &a) == 0 && stat("/", &b) == 0 && a.st_dev != b.st_dev) { stick_late_check(); return; }
    }
    mkdir("/data", 0755);
    FILE *f = fopen(CONF_PATH ".tmp", "w");
    if (!f) return;
    fprintf(f, "wallpaper=%d\nclock24=%d\nsound=%d\nnet=%d\nwifi=%d\nfont=%d\n", set_wall, set_clock24, set_sound, set_net, set_wifi, set_smooth);
    fprintf(f, "volume=%d\nmuted=%d\nstyle=%d\ndark=%d\n", set_volume, set_muted, set_style, set_dark);
    fprintf(f, "wallimg=%s\nwallfit=%d\n", set_wall_img, set_wall_fit);
    fprintf(f, "cursor2=%d\ncursorsize=%d\ntpspeed=%d\n", set_cursor_col, set_cursor_size, set_tp_speed);
    fprintf(f, "bright=%d\nscreenoff=%d\nsleepafter=%d\nlid=%d\n", set_bright, set_off_idx, set_sleep_idx, set_lid);
    fprintf(f, "notifysound=%d\ngpu=%d\nkbd=%d\n", set_notify_sound, set_gpu, set_kbd);
    fprintf(f, "tz=%s\ntzauto=%d\nntp=%d\nrtclocal=%d\n", set_tz, set_tz_auto, set_ntp, set_rtc_local);
    fprintf(f, "upd_auto=%d\nupd_last=%lld\nupd_repo=%s\nbinicon=%d\nrecsound=%d\n", set_upd_auto, set_upd_last, set_upd_repo, set_bin_icon, set_rec_sound);
    if (set_upd_api[0]) fprintf(f, "upd_api=%s\n", set_upd_api);
    for (int i = 0; i < NDESK; i++) fprintf(f, "desk=%d,%d,%s|%s\n", desk_icons[i].col, desk_icons[i].row, desk_icons[i].cmd, desk_icons[i].label);
    fprintf(f, "fpknown=%s\nstoreicon=1\n", fp_known);
    fprintf(f, "pins=");
    for (int i = 0; i < npins; i++) fprintf(f, "%s%s", i ? "," : "", pins[i]);
    fprintf(f, "\n");
    fflush(f); fsync(fileno(f)); fclose(f);
    rename(CONF_PATH ".tmp", CONF_PATH);         /* atomic: never half-written */
    sync();
}

static void check_persistent(void) {             /* is /data its own disk? */
    struct stat a, b;
    conf_persistent = stat("/data", &a) == 0 && stat("/", &b) == 0 && a.st_dev != b.st_dev;
}

/* ===================================================================
 *  WINDOWS
 * =================================================================== */
#define MAXWIN 8
typedef struct {
    int open, minimized, maximized;
    int x, y, w, h;               /* outer rectangle on screen        */
    int rx, ry, rw, rh;           /* size/position before maximizing */
    const char *title;
    int icon;
    void (*draw)(int cx, int cy, int cw, int ch);     /* paint the content */
    void (*click)(int lx, int ly, int cw, int ch);    /* click in content  */
    int (*hit)(int lx, int ly, int cw, int ch);       /* which element is under the mouse (-1 none) */
    void (*elem_rect)(int id, int *x, int *y, int *w, int *h);  /* where that element is */
    void (*key)(int code, int mods);                  /* key press while active (MOD_*) */
    void (*release)(void);                            /* mouse button released  */
    void (*rclick)(int lx, int ly, int cw, int ch);   /* right click in content */
    void (*drag)(int lx, int ly, int cw, int ch);     /* mouse moved with the button held */
    void (*wheel)(int amount);                        /* mouse wheel over window */
    void (*tick)(void);                               /* called once a second */
    int (*on_close)(void);                            /* return 0 to keep the window open */
    void (*wake)(void);                               /* background work finished / animation frame */
    int (*fd)(void);                                  /* a connection to watch (-1: none); wake() when it has data */
    void (*motion)(int lx, int ly, int inside);       /* every mouse move (inside = over this window's content) */
    void (*rawkey)(int code, int value);              /* every key press/release, untranslated (Wayland apps) */
    void (*resized)(void);                            /* the window was maximized / restored */
    int no_max;                                       /* fixed size: maximize disabled */
    int no_dark;                                      /* draws its own colours (dark mode leaves it alone) */
    int fullscreen, fx, fy, fw, fh;                   /* whole screen (F11, videos); the size before */
    char cmd[112];                                    /* which app this is ("@notepad"), for pinning */
} Win;
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_ALTGR 8                  /* the right Alt key on Polish keyboards: ą ę ś ł ż ź ć ń ó */
static Win wins[MAXWIN];
static int zorder[MAXWIN], nz;    /* zorder[nz-1] is the top (active) window */

enum { CAP_MIN, CAP_MAX, CAP_CLOSE };

static int active_win(void) {
    for (int k = nz - 1; k >= 0; k--) if (!wins[zorder[k]].minimized) return zorder[k];
    return -1;
}
static int menu_open;
/* a full-screen app on top hides the taskbar (the start menu still brings it back) */
static int taskbar_hidden(void) { int a = active_win(); return a >= 0 && wins[a].fullscreen && !menu_open; }
#define ON_TASKBAR (my >= H - TASKBAR_H && !taskbar_hidden())
static void client_rect(Win *w, int *cx, int *cy, int *cw, int *ch) {
    *cx = w->x + FRAME; *cy = w->y + TITLE_H; *cw = w->w - 2 * FRAME; *ch = w->h - TITLE_H - FRAME;
}
static void cap_rect(Win *w, int b, int *x, int *y, int *bw, int *bh) {
    if (set_style) {                                   /* Modern: flat buttons in the corner */
        int right = w->x + w->w - 1;
        *y = w->y + 1; *bh = TITLE_H - 1; *bw = 46;
        *x = right - 46 * (b == CAP_CLOSE ? 1 : b == CAP_MAX ? 2 : 3);
        return;
    }
    int right = w->x + w->w - 6;
    *y = w->y + 1; *bh = 20;
    if (b == CAP_CLOSE) { *bw = 43; *x = right - 43; }
    else if (b == CAP_MAX) { *bw = 27; *x = right - 43 - 26; }
    else { *bw = 27; *x = right - 43 - 26 - 26; }
}

/* ---- state ---------------------------------------------------------- */
static int mx, my, mouse_down, ctrl, alt, shift, capslock, altgr;
static int drag_win = -1, drag_dx, drag_dy;
static int content_drag = -1;          /* window whose content is being dragged in */
static long long last_click_ms; static int last_click_win = -1;

/* what the mouse is pointing at ("hot"), so it can be highlighted */
enum { HOT_NONE, HOT_ORB, HOT_SD, HOT_LITEM, HOT_RITEM, HOT_CAP, HOT_TASK, HOT_CONTENT, HOT_ICON, HOT_POPUP, HOT_TRAY, HOT_NETP, HOT_BAT, HOT_CLOCK, HOT_CLKP, HOT_SRES, HOT_SBOX, HOT_REC };
typedef struct { int type, a, b; } Hot;
static Hot hot;

static int orb_cx(void) { return 8 + ORB_R; }
static int orb_cy(void) { return H - TASKBAR_H / 2; }
static int menu_x(void) { return 0; }
static int menu_y(void) { return H - TASKBAR_H - MENU_H; }
static int in_menu(void) {
    return menu_open && mx >= menu_x() && mx < menu_x() + MENU_W &&
           my >= menu_y() && my < menu_y() + MENU_H;
}

#define LP_X (menu_x() + 8)
#define LP_Y (menu_y() + 8)
#define LP_W 250
#define LP_H (MENU_H - 16 - 44)
#define RP_X (LP_X + LP_W + 10)
#define RP_W (MENU_W - LP_W - 26)
#define LI_X (LP_X + 4)
#define LI_W (LP_W - 8)
#define ITEMS_Y (LP_Y + 6)
#define SD_Y (menu_y() + MENU_H - 37)   /* power buttons along the bottom */
#define MS_BOX_H 28                     /* the Start menu's search box (menusearch.c) */
#define MS_BOX_Y (LP_Y + LP_H - MS_BOX_H - 8)
static int ms_n, ms_sel;
static int ms_active(void);
static void ms_rows(int *n);
static void ms_draw(void);
static void msr_open(int i);
static void menu_search_key(int code, int mods);
#define SD_H 26
#define NPOWER 3
static const char *power_label[NPOWER] = { "Shut down", "Restart", "Sleep" };
static void power_rect(int i, int *x, int *y, int *w, int *h) {
    static const int ws[NPOWER] = { 96, 80, 66 };
    int xx = menu_x() + 8;
    for (int k = 0; k < i; k++) xx += ws[k] + 6;
    *x = xx; *y = SD_Y; *w = ws[i]; *h = SD_H;
}

/* taskbar items: each pinned app (with its window if it's open), then other windows */
typedef struct { int win, pin; } TbItem;
#define TB_PIN_W 44
static int tb_items(TbItem *it) {
    int n = 0, used[MAXWIN] = { 0 };
    for (int p = 0; p < npins; p++) {
        int w = -1;
        for (int i = 0; i < MAXWIN; i++)
            if (wins[i].open && !used[i] && !strcmp(wins[i].cmd, pins[p])) { w = i; break; }
        if (w >= 0) used[w] = 1;
        it[n].win = w; it[n].pin = p; n++;
    }
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open && !used[i]) { it[n].win = i; it[n].pin = -1; n++; }
    return n;
}
static void task_rect(int k, int *x, int *y, int *w, int *h) {
    TbItem it[MAXPINS + MAXWIN];
    int n = tb_items(it), xx = 8 + 2 * ORB_R + 12;
    for (int j = 0; j < k && j < n; j++) xx += (it[j].win >= 0 ? TB_BTN_W : TB_PIN_W) + 4;
    *x = xx; *y = H - TASKBAR_H + 3; *h = TASKBAR_H - 6;
    *w = (k < n && it[k].win < 0) ? TB_PIN_W : TB_BTN_W;
}

static int in_rect(int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

/* ---- desktop icon geometry ----------------------------------------- */
#define DI_X0 6
#define DI_Y0 6
#define DI_CW 96                  /* grid cell */
#define DI_CH 92
#define DI_W  90                  /* icon tile */
#define DI_H  84
static int desk_rows(void) { return imax(1, (H - TASKBAR_H - DI_Y0) / DI_CH); }
static int di_sel = -1, di_drag = -1, di_dx, di_dy, di_x, di_y, di_moved;
static long long di_last_click; static int di_last = -1;

static void di_rect(int i, int *x, int *y, int *w, int *h) {
    if (i == di_drag && di_moved) { *x = di_x; *y = di_y; }
    else { *x = DI_X0 + desk_icons[i].col * DI_CW; *y = DI_Y0 + desk_icons[i].row * DI_CH; }
    *w = DI_W; *h = DI_H;
}
static int di_at_mouse(void) {
    for (int i = NDESK - 1; i >= 0; i--) {
        int x, y, w, h; di_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
}

/* ---- right-click popup menu ---------------------------------------- */
#define MAXPOP 10
#define POP_W 264
#define POP_IH 26
enum { PA_NONE, PA_OPEN, PA_PIN, PA_UNPIN, PA_CLOSE, PA_DESK_ADD, PA_DESK_DEL, PA_OPEN_FILE, PA_SET_WALL, PA_CUSTOMIZE, PA_EJECT, PA_PRINT,
       PA_CUT, PA_COPY, PA_PASTE, PA_RENAME, PA_DELETE, PA_ZIP, PA_UNZIP, PA_NEWFOLDER, PA_SELALL, PA_RESTORE, PA_RESTOREALL, PA_EMPTYBIN,
       PA_NEWMENU, PA_NEWFILE /* .. PA_NEWFILE + 7: File Explorer's New > list */ };
#define POP_SUBMAX 8
#define SUB_W 236
static struct {
    int open, x, y, n, win, from_menu, desk;     /* desk: the desktop icon right-clicked, or -1 */
    char label[MAXPOP][40]; int act[MAXPOP]; int icon;
    char cmd[112], name[40];
    char path[512];                            /* a file (File Explorer's right-click menu) */
    int sub_item, sub_open, sub_n;             /* a submenu (New >) that opens beside item sub_item */
    char sub_label[POP_SUBMAX][40]; int sub_act[POP_SUBMAX], sub_icon[POP_SUBMAX];
} pop;
static int pop_h(void) { return pop.n * POP_IH + 6; }
static void pop_show_list(int x, int y, int icon, const char *path, int n, const char *const *labels, const int *acts);
static void pop_item_rect(int i, int *x, int *y, int *w, int *h) {
    *x = pop.x + 3; *y = pop.y + 3 + i * POP_IH; *w = POP_W - 6; *h = POP_IH;
}
static void popm_repaint(void);
static void pop_set_sub(int item, int n, const char *const *labels, const int *acts, const int *icons);
static void pop_sub_rect(int *x, int *y, int *w, int *h) {
    int ix, iy, iw, ih; pop_item_rect(pop.sub_item < 0 ? 0 : pop.sub_item, &ix, &iy, &iw, &ih);
    *w = SUB_W; *h = pop.sub_n * POP_IH + 6;
    *x = pop.x + POP_W - 3;
    if (*x + *w + 4 > W) *x = pop.x - *w + 3;
    *y = iy - 3;
    if (*y + *h > H - TASKBAR_H - 2) *y = H - TASKBAR_H - 2 - *h;
    if (*y < 0) *y = 0;
}
static void pop_sub_item_rect(int j, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh; pop_sub_rect(&sx, &sy, &sw, &sh);
    *x = sx + 3; *y = sy + 3 + j * POP_IH; *w = sw - 6; *h = POP_IH;
}

static char *clipboard;
static int clip_len;
static void repaint(int x, int y, int w, int h);
static void update_hover(void);
/* ---- network: cable + Wi-Fi status, tray icon, panel ----------------- */
static void win7_button(int x, int y, int w, int h, const char *label, int hover, int deflt);
static char key_char(int code, int mods);
#define ETH_IF  "eth0"
#define WIFI_IF "wlan0"
enum { NET_NOADAPTER, NET_OFF, NET_DOWN, NET_CONNECTING, NET_CONNECTED };
static struct { int state; char ip[32]; } net;              /* the cable (Ethernet) */
static int net_user_disconnected;            /* the user pressed Disconnect on the cable */
static int net_panel;                        /* the panel is open */
static int net_fast;                         /* poll every second for a while after a change */
#define NETP_W 320
static int clk_panel;                         /* the clock's calendar panel is open (timezone.c) */
#define CLKP_W 316
#define CLKP_H 392
static int clkp_x0(void), clkp_y0(void), clkp_hit(int px, int py);
#define TRAY_W 34
static int tray_x(void) { return W - CLOCK_W - 8 - TRAY_W; }

/* ---- Wi-Fi state, read from wpa_supplicant ---------------------------- */
typedef struct {
    char esc[140];             /* the name as wpa_supplicant prints it (escaped) */
    unsigned char raw[33]; int raw_len;
    char shown[40];            /* the name as we draw it */
    int sig, secure, sae_only, eap, wep, saved_id;
} WifiNet;
#define WIFI_MAXNETS 24
#define WIFI_ROWS 7
static struct {
    int present, ctl, up;      /* adapter exists, wpa_supplicant answers, radio on */
    char state[24], ssid_esc[140], ip[32];
    int rssi, cur_id;
    WifiNet nets[WIFI_MAXNETS]; int n;
    struct { int id; char esc[140]; int temp_disabled; } saved[32]; int nsaved;
} wifi;
static int wifi_sel = -1;                    /* expanded row */
static char wifi_sel_esc[140];
static char wifi_pass[64]; static int wifi_show_pass;
static char wifi_msg[80];
static int pend_id = -1, pend_new; static char pend_esc[140]; static time_t pend_t0;
static time_t wifi_last_scan;

/* talk to wpa_supplicant over its control socket */
static int wpa_fd = -1;
static char wpa_local[64];
static void wpa_close(void) {
    if (wpa_fd >= 0) { close(wpa_fd); unlink(wpa_local); wpa_fd = -1; }
}
static int wpa_cmd(const char *cmd, char *rep, size_t n) {
    if (wpa_fd < 0) {
        wpa_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (wpa_fd < 0) return -1;
        struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
        snprintf(wpa_local, sizeof wpa_local, "/tmp/ccos-wpa-%d", (int)getpid());
        unlink(wpa_local);
        snprintf(a.sun_path, sizeof a.sun_path, "%s", wpa_local);
        struct sockaddr_un d; memset(&d, 0, sizeof d); d.sun_family = AF_UNIX;
        snprintf(d.sun_path, sizeof d.sun_path, "/run/wpa_supplicant/" WIFI_IF);
        if (bind(wpa_fd, (struct sockaddr *)&a, sizeof a) < 0 || connect(wpa_fd, (struct sockaddr *)&d, sizeof d) < 0) {
            wpa_close(); return -1;
        }
    }
    if (send(wpa_fd, cmd, strlen(cmd), 0) < 0) { wpa_close(); return -1; }
    for (;;) {
        struct pollfd p = { .fd = wpa_fd, .events = POLLIN };
        if (poll(&p, 1, 2500) <= 0) { wpa_close(); return -1; }
        ssize_t r = recv(wpa_fd, rep, n - 1, 0);
        if (r < 0) { wpa_close(); return -1; }
        rep[r] = 0;
        if (rep[0] == '<') continue;                     /* an event message: not our reply */
        return (int)r;
    }
}
static int wpa_ok(const char *cmd) { char r[64]; return wpa_cmd(cmd, r, sizeof r) >= 0 && !strncmp(r, "OK", 2); }

/* wpa_supplicant writes odd characters as \xNN, \\ and \" */
static int ssid_decode(const char *e, unsigned char *out, int max) {
    int n = 0;
    while (*e && n < max) {
        if (*e == '\\' && e[1]) {
            if (e[1] == 'x' && isxdigit((unsigned char)e[2]) && isxdigit((unsigned char)e[3])) {
                char h[3] = { e[2], e[3], 0 }; out[n++] = (unsigned char)strtoul(h, NULL, 16); e += 4; continue;
            }
            char c = e[1];
            out[n++] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == 'e' ? 27 : c;
            e += 2; continue;
        }
        out[n++] = (unsigned char)*e++;
    }
    return n;
}
/* a name from outside (Wi-Fi, file, song, window title) made safe to show:
 * valid UTF-8 is kept (the drawing code knows the letters), anything else becomes '?' */
static void ssid_shown(const unsigned char *s, int n, char *out, size_t cap) {
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++) {
        unsigned c = s[i];
        if (c < 0x80) { out[k++] = c < 32 ? '?' : (char)c; continue; }
        int extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0, ok = extra && i + extra < n;
        for (int e = 1; ok && e <= extra; e++) if ((s[i + e] & 0xc0) != 0x80) ok = 0;
        if (!ok) { out[k++] = '?'; continue; }
        if (k + 1 + extra >= cap) break;                   /* don't cut a letter in half */
        for (int e = 0; e <= extra; e++) out[k++] = (char)s[i + e];
        i += extra;
    }
    out[k] = 0;
}

static int if_ip(const char *ifn, char *out, size_t n) {
    out[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifn);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, out, n);
    close(s);
    return out[0] ? 0 : -1;
}
static int if_is_up(const char *ifn) {
    char p[64], buf[32]; snprintf(p, sizeof p, "/sys/class/net/%s/flags", ifn);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    return (strtoul(buf, NULL, 16) & IFF_UP) != 0;
}
static void if_set_up(const char *ifn, int up) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifn);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        if (up) ifr.ifr_flags |= IFF_UP; else ifr.ifr_flags &= ~IFF_UP;
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}
static void route_gw(const char *ifn, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/proc/net/route", "r"); if (!f) return;
    char buf[256], name[32]; unsigned long dst, gw;
    if (fgets(buf, sizeof buf, f))
        while (fscanf(f, "%31s %lx %lx %*[^\n]", name, &dst, &gw) == 3)
            if (!strcmp(name, ifn) && dst == 0) {
                struct in_addr a = { .s_addr = (in_addr_t)gw };
                inet_ntop(AF_INET, &a, out, n);
                break;
            }
    fclose(f);
}
static void dns_server(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/etc/resolv.conf", "r"); if (!f) return;
    char buf[128];
    while (fgets(buf, sizeof buf, f)) if (sscanf(buf, "nameserver %31s", out) == 1) break;
    fclose(f);
    (void)n;
}

static int wifi_connected(void) { return wifi.ctl && !strcmp(wifi.state, "COMPLETED"); }
static int wifi_online(void) { return wifi_connected() && wifi.ip[0]; }
static int wifi_level(int dbm) { return dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : 1; }

/* the cable */
static int net_poll(void) {
    int old = net.state; char oldip[32]; snprintf(oldip, sizeof oldip, "%s", net.ip);
    int up = if_is_up(ETH_IF);
    /* no cable plugged in (a laptop's empty Ethernet socket): show it as if there were no adapter */
    char cb[8] = "";
    FILE *cf = up > 0 ? fopen("/sys/class/net/" ETH_IF "/carrier", "r") : NULL;
    if (cf) { if (!fgets(cb, sizeof cb, cf)) cb[0] = 0; fclose(cf); }
    if (up > 0 && cb[0] == '0') up = -1;
    if (up == 0 && wifi.present) up = -1;               /* networking off: nothing to show for the cable either */
    if (up < 0) net.state = NET_NOADAPTER;
    else if (!up) net.state = NET_OFF;
    else {
        if_ip(ETH_IF, net.ip, sizeof net.ip);
        net.state = net.ip[0] ? NET_CONNECTED : net_user_disconnected ? NET_DOWN : NET_CONNECTING;
    }
    if (net.state != NET_CONNECTED) net.ip[0] = 0;
    return net.state != old || strcmp(oldip, net.ip);
}

static void wifi_select_row_by_name(void) {
    wifi_sel = -1;
    if (!wifi_sel_esc[0]) return;
    for (int i = 0; i < wifi.n && i < WIFI_ROWS; i++) if (!strcmp(wifi.nets[i].esc, wifi_sel_esc)) { wifi_sel = i; return; }
    wifi_sel_esc[0] = 0;                                 /* it went away */
}

static int net_cmp(const void *a, const void *b) {
    const WifiNet *x = a, *y = b;
    int xc = wifi_connected() && !strcmp(x->esc, wifi.ssid_esc), yc = wifi_connected() && !strcmp(y->esc, wifi.ssid_esc);
    if (xc != yc) return yc - xc;
    if ((x->saved_id >= 0) != (y->saved_id >= 0)) return (y->saved_id >= 0) - (x->saved_id >= 0);
    return y->sig - x->sig;
}

static void wifi_pending_done(const char *msg, int ok) {
    if (!ok && pend_new && pend_id >= 0) {
        char c[48]; snprintf(c, sizeof c, "REMOVE_NETWORK %d", pend_id); wpa_ok(c);
    }
    if (ok) {                                            /* the one you chose last wins from now on */
        char c[64];
        for (int s = 0; s < wifi.nsaved; s++) {
            snprintf(c, sizeof c, "SET_NETWORK %d priority %d", wifi.saved[s].id, wifi.saved[s].id == pend_id ? 1 : 0);
            wpa_ok(c);
        }
    }
    wpa_ok("ENABLE_NETWORK all");                        /* SELECT_NETWORK switched the others off */
    if (ok) { wpa_ok("SAVE_CONFIG"); wifi_sel_esc[0] = 0; wifi_pass[0] = 0; }
    else wifi_pass[0] = 0;
    snprintf(wifi_msg, sizeof wifi_msg, "%s", msg);
    pend_id = -1;
}

/* read everything about Wi-Fi; full = also the network list. Returns 1 if anything visible changed. */
static char wifi_sig_last[600];
static int wifi_poll(int full) {
    static char buf[16384];
    wifi.present = access("/sys/class/net/" WIFI_IF, F_OK) == 0;
    wifi.up = wifi.present && if_is_up(WIFI_IF) == 1;
    wifi.ctl = 0; wifi.state[0] = 0; wifi.ssid_esc[0] = 0; wifi.ip[0] = 0; wifi.rssi = -100; wifi.cur_id = -1;
    if (wifi.present && wpa_cmd("STATUS", buf, sizeof buf) >= 0) {
        wifi.ctl = 1;
        for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
            if (!strncmp(l, "wpa_state=", 10)) snprintf(wifi.state, sizeof wifi.state, "%s", l + 10);
            else if (!strncmp(l, "ssid=", 5)) snprintf(wifi.ssid_esc, sizeof wifi.ssid_esc, "%s", l + 5);
            else if (!strncmp(l, "id=", 3)) wifi.cur_id = atoi(l + 3);
        }
        if (wifi_connected()) {
            if_ip(WIFI_IF, wifi.ip, sizeof wifi.ip);
            if (wpa_cmd("SIGNAL_POLL", buf, sizeof buf) >= 0) {
                char *r = strstr(buf, "RSSI="); if (r) wifi.rssi = atoi(r + 5);
            }
        }
    }
    if (wifi.ctl && (full || pend_id >= 0)) {
        /* saved networks */
        wifi.nsaved = 0;
        if (wpa_cmd("LIST_NETWORKS", buf, sizeof buf) >= 0) {
            char *save = NULL, *l = strtok_r(buf, "\n", &save);           /* skip the header */
            for (l = strtok_r(NULL, "\n", &save); l && wifi.nsaved < 32; l = strtok_r(NULL, "\n", &save)) {
                char *f[4] = { 0 }; int k = 0; char *p = l;
                f[k++] = p;
                while (*p && k < 4) { if (*p == '\t') { *p = 0; f[k++] = p + 1; } p++; }
                if (k < 4) continue;
                wifi.saved[wifi.nsaved].id = atoi(f[0]);
                snprintf(wifi.saved[wifi.nsaved].esc, sizeof wifi.saved[0].esc, "%s", f[1]);
                wifi.saved[wifi.nsaved].temp_disabled = strstr(f[3], "TEMP-DISABLED") != NULL;
                wifi.nsaved++;
            }
        }
    }
    if (wifi.ctl && full) {
        time_t now = time(NULL);
        if (wifi.up && now - wifi_last_scan >= 10) { wpa_ok("SCAN"); wifi_last_scan = now; }
        if (wpa_cmd("SCAN_RESULTS", buf, sizeof buf) >= 0) {
            wifi.n = 0;
            char *save = NULL, *l = strtok_r(buf, "\n", &save);
            for (l = strtok_r(NULL, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
                char *f[5] = { 0 }; int k = 0; char *p = l;
                f[k++] = p;
                while (*p && k < 5) { if (*p == '\t') { *p = 0; f[k++] = p + 1; } p++; }
                if (k < 5 || !f[4][0] || !strncmp(f[4], "\\x00", 4)) continue;      /* hidden network */
                int sig = atoi(f[2]), j;
                for (j = 0; j < wifi.n; j++) if (!strcmp(wifi.nets[j].esc, f[4])) break;
                if (j < wifi.n) { if (sig > wifi.nets[j].sig) wifi.nets[j].sig = sig; continue; }
                if (wifi.n == WIFI_MAXNETS) continue;
                WifiNet *w = &wifi.nets[wifi.n++];
                memset(w, 0, sizeof *w);
                snprintf(w->esc, sizeof w->esc, "%s", f[4]);
                w->raw_len = ssid_decode(f[4], w->raw, 32);
                ssid_shown(w->raw, w->raw_len, w->shown, sizeof w->shown);
                w->sig = sig;
                w->eap = strstr(f[3], "EAP") != NULL;
                w->wep = strstr(f[3], "WEP") != NULL;
                w->secure = w->eap || w->wep || strstr(f[3], "PSK") || strstr(f[3], "SAE");
                w->sae_only = strstr(f[3], "SAE") && !strstr(f[3], "PSK");
            }
        }
        for (int j = 0; j < wifi.n; j++) {
            wifi.nets[j].saved_id = -1;
            for (int s = 0; s < wifi.nsaved; s++) if (!strcmp(wifi.saved[s].esc, wifi.nets[j].esc)) wifi.nets[j].saved_id = wifi.saved[s].id;
        }
        qsort(wifi.nets, wifi.n, sizeof *wifi.nets, net_cmp);
        wifi_select_row_by_name();
    }
    /* a connection attempt in progress */
    if (pend_id >= 0) {
        if (wifi_connected() && !strcmp(wifi.ssid_esc, pend_esc)) wifi_pending_done("", 1);
        else {
            int bad = 0;
            for (int s = 0; s < wifi.nsaved; s++) if (wifi.saved[s].id == pend_id && wifi.saved[s].temp_disabled) bad = 1;
            if (bad) wifi_pending_done("Wrong password. Please try again.", 0);
            else if (time(NULL) - pend_t0 > 25) wifi_pending_done("Couldn't connect to this network.", 0);
        }
    }
    /* has anything we show changed? */
    char sig[600]; int k = snprintf(sig, sizeof sig, "%d%d%d|%s|%s|%s|%d|%d|%d|%s|", wifi.present, wifi.ctl, wifi.up, wifi.state,
                                    wifi.ssid_esc, wifi.ip, wifi_level(wifi.rssi), pend_id, wifi_sel, wifi_msg);
    for (int j = 0; j < wifi.n && j < WIFI_ROWS && k < (int)sizeof sig - 60; j++)
        k += snprintf(sig + k, sizeof sig - k, "%.20s%d%d,", wifi.nets[j].esc, wifi_level(wifi.nets[j].sig), wifi.nets[j].saved_id);
    int changed = strcmp(sig, wifi_sig_last) != 0;
    snprintf(wifi_sig_last, sizeof wifi_sig_last, "%s", sig);
    return changed;
}

/* ---- actions ------------------------------------------------------------ */
static void sh_bg(const char *cmd) {                 /* run a shell command in the background */
    if (fork() == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}
#define ETH_PID "/run/udhcpc-" ETH_IF ".pid"
#define ETH_DHCP "udhcpc -i " ETH_IF " -s /usr/share/udhcpc/default.script -t 5 -T 2 -A 5 -b -p " ETH_PID
#define ETH_STOP "[ -f " ETH_PID " ] && kill $(cat " ETH_PID ") 2>/dev/null; rm -f " ETH_PID "; "
static void net_connect(void) {
    net_user_disconnected = 0;
    sh_bg(ETH_STOP "ip link set " ETH_IF " up; " ETH_DHCP);
    net_fast = 15;
}
static void net_disconnect(void) {
    net_user_disconnected = 1;
    sh_bg(ETH_STOP "ip route del default dev " ETH_IF "; ip addr flush dev " ETH_IF);
    net_fast = 10;
}
static void wifi_radio(int on) {
    if (!wifi.present) return;
    if (on) { if_set_up(WIFI_IF, 1); wpa_ok("RECONNECT"); wpa_ok("SCAN"); wifi_last_scan = time(NULL); }
    else { wpa_ok("DISCONNECT"); if_set_up(WIFI_IF, 0); wifi.n = 0; wifi_sel = -1; wifi_sel_esc[0] = 0; pend_id = -1; }
    net_fast = 12;
}
static void net_enable(int on) {                     /* the Networking switch: everything */
    set_net = on;
    int has_eth = if_is_up(ETH_IF) >= 0;              /* the adapter exists (cable or not) */
    if (on) { if (has_eth) net_connect(); if (set_wifi) wifi_radio(1); }
    else {
        if (has_eth) sh_bg(ETH_STOP "ip addr flush dev " ETH_IF "; ip link set " ETH_IF " down");
        wifi_radio(0);
        net_fast = 10;
    }
}

static void wifi_connect(int i) {
    WifiNet *w = &wifi.nets[i];
    if (w->eap) { snprintf(wifi_msg, sizeof wifi_msg, "Work/school Wi-Fi (username login) isn't supported yet."); return; }
    if (w->wep) { snprintf(wifi_msg, sizeof wifi_msg, "This network uses old WEP security, which isn't supported."); return; }
    int need_pass = w->secure && (w->saved_id < 0 || wifi_pass[0]);
    if (need_pass) {
        size_t l = strlen(wifi_pass);
        if (l < 8 || l > 63) { snprintf(wifi_msg, sizeof wifi_msg, "Wi-Fi passwords are 8 to 63 characters."); return; }
    }
    char cmd[300], rep[64];
    int id = w->saved_id, fresh = 0;
    if (id < 0) {
        if (wpa_cmd("ADD_NETWORK", rep, sizeof rep) < 0) { snprintf(wifi_msg, sizeof wifi_msg, "Wi-Fi isn't responding."); return; }
        id = atoi(rep); fresh = 1;
        char hex[80]; int k = 0;
        for (int b = 0; b < w->raw_len; b++) k += snprintf(hex + k, sizeof hex - k, "%02x", w->raw[b]);
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, hex); wpa_ok(cmd);
    }
    if (!w->secure) { snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id); wpa_ok(cmd); }
    else if (need_pass) {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt %s", id, w->sae_only ? "SAE" : "WPA-PSK WPA-PSK-SHA256 SAE"); wpa_ok(cmd);
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d ieee80211w %d", id, w->sae_only ? 2 : 1); wpa_ok(cmd);
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk \"%s\"", id, wifi_pass);
        if (!wpa_ok(cmd)) {
            snprintf(wifi_msg, sizeof wifi_msg, "That password can't be used.");
            if (fresh) { snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id); wpa_ok(cmd); }
            return;
        }
    }
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id); wpa_ok(cmd);
    pend_id = id; pend_new = fresh; pend_t0 = time(NULL);
    snprintf(pend_esc, sizeof pend_esc, "%s", w->esc);
    snprintf(wifi_msg, sizeof wifi_msg, "Connecting...");
    net_fast = 30;
}
static void wifi_disconnect(void) { wpa_ok("DISCONNECT"); wifi_msg[0] = 0; net_fast = 8; }
static void wifi_forget(int i) {
    char cmd[48];
    snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", wifi.nets[i].saved_id);
    wpa_ok(cmd); wpa_ok("SAVE_CONFIG");
    wifi.nets[i].saved_id = -1;
    wifi_msg[0] = 0; wifi_pass[0] = 0;
    net_fast = 8;
}

/* ---- icons -------------------------------------------------------------- */
/* small status badges in the icon's top-right corner */
static void draw_badge_x(float cx, float cy) {
    fill_circle(cx, cy, 4.2f, RGB(0xd8, 0x30, 0x20), 255);
    for (int k = -2; k <= 2; k++) {
        fill_rect((int)cx + k, (int)cy + k, 1, 1, RGB(255, 255, 255), 255);
        fill_rect((int)cx + k, (int)cy - k, 1, 1, RGB(255, 255, 255), 255);
    }
}
static void draw_badge_wait(float cx, float cy) { fill_circle(cx, cy, 3.2f, RGB(0xf0, 0xc0, 0x30), 255); }

/* cable: an Ethernet (RJ45) plug; grey with a red cross when not connected */
static void draw_net_icon(int x, int y, int st) {
    int lit = st == NET_CONNECTED || st == NET_CONNECTING;
    uint32_t c = lit ? RGB(255, 255, 255) : RGB(0x9c, 0xa8, 0xb4);
    uint32_t slot = lit ? RGB(0x40, 0x58, 0x74) : RGB(0x5c, 0x68, 0x74);
    for (int pass = 0; pass < 2; pass++) {               /* pass 0: soft shadow, 1: the plug */
        int o = pass ? 0 : 1, a = pass ? 255 : 110;
        uint32_t col = pass ? c : RGB(0, 0, 0);
        fill_round(x + 2 + o, y + o, 12, 9, 2, col, a);              /* plug head */
        fill_rect(x + 5 + o, y + 9 + o, 6, 2, col, a);               /* latch / strain relief */
        fill_rect(x + 7 + o, y + 11 + o, 2, 5, col, a);              /* the cable */
    }
    for (int k = 0; k < 4; k++) fill_rect(x + 4 + k * 2 + (k > 1), y + 2, 1, 3, slot, 255);   /* contacts */
    if (st == NET_CONNECTING) draw_badge_wait(x + 15, y + 2);
    if (st == NET_DOWN || st == NET_OFF || st == NET_NOADAPTER) draw_badge_x(x + 15, y + 3);
}
/* Wi-Fi: a fan of arcs; `level` arcs lit (0-4) */
static void draw_wifi_icon(int cx, int by, int level, uint32_t on, uint32_t off) {
    for (int y = by - 14; y <= by; y++)
        for (int x = cx - 14; x <= cx + 14; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - by, d = sqrtf(dx * dx + dy * dy);
            if (dy > 0 || fabsf(dx) > -dy * 1.05f) continue;            /* only the upward fan */
            int ring = -1; float edge = 0;
            if (d < 2.6f) { ring = 0; edge = 2.6f - d; }
            else for (int r = 1; r <= 3; r++) {
                float e = 1.3f - fabsf(d - (2.2f + r * 3.9f));
                if (e > 0) { ring = r; edge = e; break; }
            }
            if (ring < 0) continue;
            blend(x, y, ring < level ? on : off, (int)(255 * clampf(edge, 0, 1)));
        }
}
static void draw_lock(int x, int y, uint32_t c) {
    stroke_round(x + 1, y, 6, 8, 3, c, 255);
    fill_rect(x, y + 4, 8, 6, c, 255);
}
static void draw_switch(int x, int y, int on) {
    fill_round(x, y, 46, 24, 12, on ? RGB(0x33, 0x99, 0xff) : RGB(0xc4, 0xc8, 0xcc), 255);
    fill_circle(on ? x + 34 : x + 12, y + 12, 9, RGB(255, 255, 255), 255);
}

/* ---- the panel: one function lays it out, draws it and records where things are ---- */
enum { NETID_SWITCH = 1, NETID_BUTTON, NETID_WIFISW, NETID_PASS, NETID_SHOW, NETID_CONNECT, NETID_FORGET, NETID_ROW = 100 };
typedef struct { int id, x, y, w, h; } NRect;
static NRect nrect[40]; static int nnrect;
static int netp_x0, netp_y0, netp_h;
static void nr_add(int id, int x, int y, int w, int h) {
    if (nnrect < 40) nrect[nnrect++] = (NRect){ id, x, y, w, h };
}
static int nr_hot(int id) { return hot.type == HOT_NETP && hot.a == id; }

static int wifi_row_needs_pass(void) {
    if (wifi_sel < 0 || wifi_sel >= wifi.n) return 0;
    WifiNet *w = &wifi.nets[wifi_sel];
    int conn = wifi_connected() && !strcmp(w->esc, wifi.ssid_esc);
    return !conn && w->secure && !w->eap && !w->wep && (w->saved_id < 0 || !strncmp(wifi_msg, "Wrong", 5));
}

/* draw = 0: only measure + record rects (y0 from the top of the panel) */
static int netp_layout(int draw, int px, int py) {
    nnrect = 0;
    uint32_t grey = RGB(0x6d, 0x6d, 0x6d), dark = RGB(0x10, 0x10, 0x10), blue = RGB(0x1e, 0x39, 0x87);
    uint32_t sep = RGB(0xe0, 0xe6, 0xee), green = RGB(0x20, 0x80, 0x30);
    int ix = px + 6, iw = NETP_W - 12, y = py + 6;
#define SEP() do { if (draw) fill_rect(ix + 8, y + 2, iw - 16, 1, sep, 255); y += 6; } while (0)

    /* summary */
    int won = wifi_online(), eon = net.state == NET_CONNECTED;
    if (draw) {
        draw_text(ix + 12, y + 6, won || eon ? "Currently connected to:" : "Not connected", grey, 255);
        fill_round_grad(ix + 12, y + 26, 32, 32, 4, RGB(0x5c, 0x9c, 0xe0), RGB(0x1c, 0x54, 0xa0), 255);
        if (won) draw_wifi_icon(ix + 28, y + 52, wifi_level(wifi.rssi), RGB(255, 255, 255), RGB(0x8c, 0xb0, 0xd8));
        else if (eon || net.state == NET_CONNECTING || !wifi.present)
            draw_net_icon(ix + 20, y + 33, eon ? NET_CONNECTED : net.state == NET_CONNECTING ? NET_CONNECTING : NET_DOWN);
        else { draw_wifi_icon(ix + 28, y + 52, 0, RGB(255, 255, 255), RGB(0x8c, 0xb0, 0xd8)); draw_badge_x(ix + 38, y + 33); }
        char name[48];
        if (won) { for (int j = 0; j < wifi.n; j++) if (!strcmp(wifi.nets[j].esc, wifi.ssid_esc)) { snprintf(name, sizeof name, "%s", wifi.nets[j].shown); goto have; }
                   unsigned char raw[33]; int rl = ssid_decode(wifi.ssid_esc, raw, 32); ssid_shown(raw, rl, name, sizeof name); }
        else snprintf(name, sizeof name, "%s", eon ? "ccos Network" : "No connection");
    have:
        name[30] = 0;
        draw_text_bold(ix + 54, y + 26, name, dark);
        const char *st = won || eon ? "Internet access" : !set_net ? "Networking is off"
                       : (net.state == NET_CONNECTING || pend_id >= 0) ? "Connecting..." : "Not connected";
        draw_text(ix + 54, y + 43, st, won || eon ? green : grey, 255);
    }
    y += 66; SEP();

    /* networking switch (everything) */
    nr_add(NETID_SWITCH, ix + 4, y, iw - 8, 32);
    if (draw) {
        if (nr_hot(NETID_SWITCH)) fill_round(ix + 4, y, iw - 8, 32, 3, RGB(0xe6, 0xf0, 0xfb), 255);
        draw_text(ix + 12, y + 8, "Networking", dark, 255);
        draw_text(ix + iw - 92, y + 8, set_net ? "On" : "Off", grey, 255);
        draw_switch(ix + iw - 58, y + 4, set_net);
    }
    y += 34; SEP();

    /* cable */
    if (net.state != NET_NOADAPTER || !wifi.present) {
        if (draw) draw_text(ix + 12, y + 2, "Cable (Ethernet)", blue, 255);
        y += 22;
        if (net.state != NET_NOADAPTER && net.state != NET_OFF) {
            if (draw) {
                fill_round_grad(ix + 6, y, iw - 12, 46, 3, RGB(0xf2, 0xf8, 0xff), RGB(0xdc, 0xec, 0xfc), 255);
                stroke_round(ix + 6, y, iw - 12, 46, 3, RGB(0xa8, 0xc8, 0xec), 255);
                fill_round_grad(ix + 12, y + 8, 30, 30, 4, RGB(0x5c, 0x9c, 0xe0), RGB(0x1c, 0x54, 0xa0), 255);
                draw_net_icon(ix + 18, y + 14, net.state == NET_CONNECTING ? NET_CONNECTING : NET_CONNECTED);
                draw_text_bold(ix + 50, y + 6, "ccos Network", dark);
                draw_text(ix + 50, y + 24, net.state == NET_CONNECTED ? "Connected" : net.state == NET_CONNECTING ? "Connecting..." : "Not connected", grey, 255);
            }
            nr_add(NETID_BUTTON, ix + iw - 108, y + 10, 94, 26);
            if (draw) win7_button(ix + iw - 108, y + 10, 94, 26, net.state == NET_DOWN ? "Connect" : "Disconnect", nr_hot(NETID_BUTTON), 0);
            y += 50;
        } else {
            if (draw) draw_text(ix + 12, y, net.state == NET_OFF ? "Turn networking on to connect." : if_is_up(ETH_IF) >= 0 ? "No cable plugged in." : "No network cable adapter.", grey, 255);
            y += 22;
        }
        SEP();
    }

    /* Wi-Fi */
    if (draw) draw_text(ix + 12, y + 6, "Wi-Fi", blue, 255);
    if (wifi.present && set_net) {
        nr_add(NETID_WIFISW, ix + 4, y, iw - 8, 30);
        if (draw) {
            if (nr_hot(NETID_WIFISW)) fill_round(ix + 4, y, iw - 8, 30, 3, RGB(0xe6, 0xf0, 0xfb), 255);
            draw_text(ix + iw - 92, y + 6, set_wifi ? "On" : "Off", grey, 255);
            draw_switch(ix + iw - 58, y + 3, set_wifi);
        }
    }
    y += 32;
    if (!wifi.present) {
        if (draw) {
            draw_text(ix + 12, y, "No Wi-Fi adapter found.", dark, 255);
            if (net.state != NET_NOADAPTER) {
                draw_text(ix + 12, y + 18, "QEMU connects ccos with a virtual", grey, 255);
                draw_text(ix + 12, y + 34, "network cable (Ethernet).", grey, 255);
            }
        }
        y += net.state != NET_NOADAPTER ? 54 : 22;
    } else if (!set_net || !set_wifi || !wifi.ctl || !wifi.n) {
        if (draw) draw_text(ix + 12, y, !set_net ? "Turn networking on to use Wi-Fi." : !set_wifi ? "Wi-Fi is off."
                                       : !wifi.ctl ? "Wi-Fi is starting..." : "Looking for networks...", grey, 255);
        y += 24;
    } else {
        for (int i = 0; i < wifi.n && i < WIFI_ROWS; i++) {
            WifiNet *w = &wifi.nets[i];
            int conn = wifi_connected() && !strcmp(w->esc, wifi.ssid_esc);
            int pending = pend_id >= 0 && !strcmp(w->esc, pend_esc);
            int open = i == wifi_sel;
            int rh = 40;
            nr_add(NETID_ROW + i, ix + 4, y, iw - 8, rh);
            if (draw) {
                if (open || nr_hot(NETID_ROW + i)) {
                    fill_round_grad(ix + 4, y, iw - 8, open ? rh + 2 : rh, 3, open ? RGB(0xf2, 0xf8, 0xff) : RGB(0xf6, 0xfa, 0xff),
                                    open ? RGB(0xdc, 0xec, 0xfc) : RGB(0xe8, 0xf2, 0xfe), 255);
                    stroke_round(ix + 4, y, iw - 8, rh, 3, RGB(0xa8, 0xc8, 0xec), 255);
                }
                draw_wifi_icon(ix + 24, y + 30, wifi_level(w->sig), RGB(0x1c, 0x54, 0xa0), RGB(0xc8, 0xd2, 0xde));
                if (w->secure) draw_lock(ix + 32, y + 22, RGB(0x50, 0x58, 0x64));
                char nm[40]; snprintf(nm, sizeof nm, "%.26s", w->shown);
                draw_text_bold(ix + 50, y + 4, nm, dark);
                const char *sub = conn ? (wifi.ip[0] ? "Connected" : "Connected, getting address...")
                                : pending ? "Connecting..." : w->eap ? "Secured (work/school)"
                                : w->saved_id >= 0 ? "Saved" : w->secure ? "Secured" : "Open";
                draw_text(ix + 50, y + 21, sub, conn ? green : grey, 255);
            }
            y += rh;
            if (open) {                                   /* expanded: password + buttons */
                if (wifi_row_needs_pass()) {
                    int fx = ix + 12, fw = iw - 24;
                    nr_add(NETID_PASS, fx, y + 4, fw - 52, 26);
                    nr_add(NETID_SHOW, fx + fw - 48, y + 4, 48, 26);
                    if (draw) {
                        fill_rect(fx, y + 4, fw - 52, 26, RGB(255, 255, 255), 255);
                        stroke_round(fx, y + 4, fw - 52, 26, 2, RGB(0x3c, 0x7f, 0xb1), 255);
                        char shown[70]; int l = (int)strlen(wifi_pass), maxc = (fw - 66) / 8, k;
                        const char *src = wifi_pass + (l > maxc ? l - maxc : 0);
                        if (wifi_show_pass) snprintf(shown, sizeof shown, "%s", src);
                        else { for (k = 0; src[k] && k < 64; k++) shown[k] = 7; shown[k] = 0; }
                        if (!l) draw_text(fx + 6, y + 9, "Wi-Fi password", RGB(0xa0, 0xa8, 0xb4), 255);
                        draw_text(fx + 6, y + 9, shown, dark, 255);
                        fill_rect(fx + 6 + text_w(shown), y + 9, 1, 16, RGB(0, 0, 0), 255);
                        win7_button(fx + fw - 48, y + 4, 48, 26, wifi_show_pass ? "Hide" : "Show", nr_hot(NETID_SHOW), 0);
                    }
                    y += 32;
                }
                if (wifi_msg[0]) {
                    if (draw) {
                        int bad = strncmp(wifi_msg, "Connecting", 10) != 0;
                        char m1[40], m2[48]; int cut = (int)strlen(wifi_msg);
                        if (cut > 36) { cut = 36; while (cut > 0 && wifi_msg[cut] != ' ') cut--; if (!cut) cut = 36; }
                        snprintf(m1, sizeof m1, "%.*s", cut, wifi_msg);
                        snprintf(m2, sizeof m2, "%s", wifi_msg + cut + (wifi_msg[cut] == ' '));
                        draw_text(ix + 12, y + 2, m1, bad ? RGB(0xc0, 0x28, 0x20) : grey, 255);
                        if (m2[0]) draw_text(ix + 12, y + 18, m2, bad ? RGB(0xc0, 0x28, 0x20) : grey, 255);
                    }
                    y += strlen(wifi_msg) > 36 ? 36 : 20;
                }
                int bx = ix + iw - 106;
                nr_add(NETID_CONNECT, bx, y + 2, 94, 26);
                if (draw) win7_button(bx, y + 2, 94, 26, conn ? "Disconnect" : pending ? "Wait..." : "Connect", nr_hot(NETID_CONNECT), !conn);
                if (w->saved_id >= 0 && !pending) {
                    nr_add(NETID_FORGET, bx - 82, y + 2, 76, 26);
                    if (draw) win7_button(bx - 82, y + 2, 76, 26, "Forget", nr_hot(NETID_FORGET), 0);
                }
                y += 34;
            }
        }
    }
    SEP();

    /* details for the connection in use */
    const char *ifn = won ? WIFI_IF : ETH_IF;
    char ip[32], gw[32] = "", dns[32] = "";
    snprintf(ip, sizeof ip, "%s", won ? wifi.ip : net.ip);
    if (ip[0]) { route_gw(ifn, gw, sizeof gw); dns_server(dns, sizeof dns); }
    if (draw) {
        char t[64];
        snprintf(t, sizeof t, "IP address:  %s", ip[0] ? ip : "-"); draw_text(ix + 12, y, t, grey, 255);
        snprintf(t, sizeof t, "Gateway:     %s", gw[0] ? gw : "-"); draw_text(ix + 12, y + 16, t, grey, 255);
        snprintf(t, sizeof t, "DNS server:  %s", dns[0] ? dns : "-"); draw_text(ix + 12, y + 32, t, grey, 255);
    }
    y += 50;
#undef SEP
    return y + 6 - py;
}

/* work out where the panel goes (it grows upwards from the taskbar) */
static void netp_place(void) {
    netp_x0 = W - NETP_W - 6;
    netp_h = netp_layout(0, netp_x0, 0);
    netp_y0 = imax(4, H - TASKBAR_H - netp_h - 6);
    netp_layout(0, netp_x0, netp_y0);                   /* record the real positions */
}
static void netp_repaint(void) { repaint(W - NETP_W - 12, 0, NETP_W + 12, H - TASKBAR_H); }
static void tray_repaint(void) { repaint(tray_x(), H - TASKBAR_H, TRAY_W, TASKBAR_H); }
#define BAT_W 34
static int bat_x(void) { return tray_x() - BAT_W - 2; }   /* the battery, left of the network icon */

static void draw_net_panel(void) {
    int x = netp_x0, y = netp_y0, h = netp_h;
    if (x >= clip_x1 || x + NETP_W + 4 <= clip_x0 || y >= clip_y1 || y + h + 4 <= clip_y0) return;
    fill_round(x + 3, y + 3, NETP_W, h, 6, RGB(0, 0, 0), 70);
    if (set_style) {
        fill_round(x, y, NETP_W, h, 8, RGB(255, 255, 255), 255);
        stroke_round(x, y, NETP_W, h, 8, RGB(0xc8, 0xcc, 0xd2), 255);
    } else {
        fill_round(x, y, NETP_W, h, 6, RGB(0x16, 0x34, 0x56), 215);
        stroke_round(x, y, NETP_W, h, 6, RGB(0, 0, 0), 170);
        stroke_round(x + 1, y + 1, NETP_W - 2, h - 2, 5, RGB(255, 255, 255), 80);
        fill_round(x + 6, y + 6, NETP_W - 12, h - 12, 4, RGB(255, 255, 255), 250);
    }
    netp_layout(1, x, y);
    if (DARK) { if (dk_mask) dk_clear(x, y, NETP_W, h); dark_xform(x, y, NETP_W, h); }
}

/* the tray icon: Wi-Fi when that's what we're using, otherwise the cable */
static void draw_tray_icon(int tx, int ty) {
    uint32_t on_c = set_style && !set_dark ? RGB(0x20, 0x24, 0x2a) : RGB(255, 255, 255);
    uint32_t off_c = set_style && !set_dark ? RGB(0xb0, 0xb6, 0xbe) : RGB(0x70, 0x80, 0x90);
    if (wifi_online()) draw_wifi_icon(tx + 17, ty + 26, wifi_level(wifi.rssi), on_c, off_c);
    else if (net.state == NET_CONNECTED || net.state == NET_CONNECTING) draw_net_icon(tx + 9, ty + 9, net.state);
    else if (wifi.present) {                             /* a laptop: show Wi-Fi, even when it's off */
        draw_wifi_icon(tx + 17, ty + 26, 0, on_c, set_style && !set_dark ? RGB(0xb0, 0xb6, 0xbe) : RGB(0x9c, 0xa8, 0xb4));
        if (set_net && set_wifi && (pend_id >= 0 || (wifi_connected() && !wifi.ip[0]))) draw_badge_wait(tx + 27, ty + 10);
        else if (!set_net || !set_wifi) draw_badge_x(tx + 27, ty + 11);
    } else draw_net_icon(tx + 9, ty + 9, set_net ? net.state : NET_OFF);
}

/* keyboard while the panel is open: typing the Wi-Fi password */
static void net_panel_set(int open);
static void net_do(int id);
static void net_key(int code, int mods) {
    if (code == KEY_ESC) {
        if (wifi_sel >= 0) { wifi_sel = -1; wifi_sel_esc[0] = 0; wifi_pass[0] = 0; wifi_msg[0] = 0; netp_place(); netp_repaint(); }
        else net_panel_set(0);
        return;
    }
    if (!wifi_row_needs_pass() && !(wifi_sel >= 0 && (code == KEY_ENTER || code == KEY_KPENTER))) return;
    size_t l = strlen(wifi_pass);
    if (code == KEY_ENTER || code == KEY_KPENTER) { net_do(NETID_CONNECT); return; }
    if (code == KEY_BACKSPACE) { if (l) wifi_pass[l - 1] = 0; }
    else if ((mods & MOD_CTRL) && code == KEY_V && clipboard) {
        for (int i = 0; i < clip_len && l < 63; i++) if (clipboard[i] >= 32 && clipboard[i] < 127) { wifi_pass[l++] = clipboard[i]; wifi_pass[l] = 0; }
    } else {
        char c = key_char(code, mods);
        if (!c || l >= 63) return;
        wifi_pass[l] = c; wifi_pass[l + 1] = 0;
    }
    netp_place(); netp_repaint();
}

/* a click on one of the panel's controls */
static void net_do(int id) {
    if (id == NETID_SWITCH) { net_enable(!set_net); save_settings(); }
    else if (id == NETID_BUTTON) { if (net.state == NET_DOWN) net_connect(); else net_disconnect(); }
    else if (id == NETID_WIFISW) { set_wifi = !set_wifi; wifi_radio(set_wifi); save_settings(); }
    else if (id >= NETID_ROW && id - NETID_ROW < wifi.n) {
        int i = id - NETID_ROW;
        if (wifi_sel == i) { wifi_sel = -1; wifi_sel_esc[0] = 0; }
        else { wifi_sel = i; snprintf(wifi_sel_esc, sizeof wifi_sel_esc, "%s", wifi.nets[i].esc); }
        wifi_pass[0] = 0; wifi_show_pass = 0;
        if (pend_id < 0) wifi_msg[0] = 0;
    }
    else if (id == NETID_SHOW) wifi_show_pass = !wifi_show_pass;
    else if (id == NETID_CONNECT && wifi_sel >= 0 && wifi_sel < wifi.n) {
        WifiNet *w = &wifi.nets[wifi_sel];
        if (wifi_connected() && !strcmp(w->esc, wifi.ssid_esc)) wifi_disconnect();
        else if (pend_id < 0) wifi_connect(wifi_sel);
    }
    else if (id == NETID_FORGET && wifi_sel >= 0 && wifi_sel < wifi.n && wifi.nets[wifi_sel].saved_id >= 0) wifi_forget(wifi_sel);
    net_poll();
    wifi_poll(1);
    netp_place();
    netp_repaint(); tray_repaint();
}

static Hot find_hot(void) {
    Hot h = { HOT_NONE, 0, 0 };
    if (clk_panel && in_rect(clkp_x0(), clkp_y0(), CLKP_W, CLKP_H)) {
        h.type = HOT_CLKP; h.a = clkp_hit(mx, my); return h;
    }
    if (net_panel && in_rect(netp_x0, netp_y0, NETP_W, netp_h)) {
        for (int i = 0; i < nnrect; i++)
            if (in_rect(nrect[i].x, nrect[i].y, nrect[i].w, nrect[i].h)) { h.type = HOT_NETP; h.a = nrect[i].id; }
        return h;                                   /* the most specific (last) match wins */
    }
    if (pop.open) {
        if (pop.sub_open) {                                   /* the submenu, over the main one */
            for (int j = 0; j < pop.sub_n; j++) {
                int x, y, w, hh; pop_sub_item_rect(j, &x, &y, &w, &hh);
                if (in_rect(x, y, w, hh)) { h.type = HOT_POPUP; h.a = 100 + j; return h; }
            }
            int x, y, w, hh; pop_sub_rect(&x, &y, &w, &hh);
            if (in_rect(x, y, w, hh)) return h;
        }
        for (int i = 0; i < pop.n; i++) {
            int x, y, w, hh; pop_item_rect(i, &x, &y, &w, &hh);
            if (in_rect(x, y, w, hh)) {
                if (pop.sub_item >= 0) pop.sub_open = i == pop.sub_item;   /* hovering New opens it, others close it */
                h.type = HOT_POPUP; h.a = i; return h;
            }
        }
        if (in_rect(pop.x, pop.y, POP_W, pop_h())) return h;
    }
    int dx = mx - orb_cx(), dy = my - orb_cy();
    if ((dx * dx + dy * dy <= (ORB_R + 2) * (ORB_R + 2) && !taskbar_hidden()) ||
        (mx < orb_cx() && ON_TASKBAR)) { h.type = HOT_ORB; return h; }  /* the corner counts too */
    if (in_menu()) {
        for (int i = 0; i < NPOWER; i++) {
            int x, y, w, hh; power_rect(i, &x, &y, &w, &hh);
            if (in_rect(x, y, w, hh)) { h.type = HOT_SD; h.a = i; return h; }
        }
        if (in_rect(LI_X, MS_BOX_Y, LI_W, MS_BOX_H)) { h.type = HOT_SBOX; return h; }
        if (ms_active()) {                                   /* search results instead of the programs */
            int rows; ms_rows(&rows);
            for (int i = 0; i < ms_n && i < rows; i++)
                if (in_rect(LI_X, ITEMS_Y + i * ITEM_H, LI_W, ITEM_H)) { h.type = HOT_SRES; h.a = i; return h; }
        } else
        for (int i = 0; left_items[i].label; i++)
            if (in_rect(LI_X, ITEMS_Y + i * ITEM_H, LI_W, ITEM_H)) { h.type = HOT_LITEM; h.a = i; return h; }
        for (int i = 0; right_items[i].label; i++)
            if (in_rect(RP_X, ITEMS_Y + i * ITEM_H, RP_W, ITEM_H)) { h.type = HOT_RITEM; h.a = i; return h; }
        return h;
    }
    if (ON_TASKBAR && in_rect(tray_x(), H - TASKBAR_H + 3, TRAY_W, TASKBAR_H - 6)) {
        h.type = HOT_TRAY; return h;
    }
    if (ON_TASKBAR && in_rect(W - CLOCK_W - 8, H - TASKBAR_H + 3, CLOCK_W + 6, TASKBAR_H - 6)) {
        h.type = HOT_CLOCK; return h;
    }
    if (ON_TASKBAR && rec_on() && in_rect(rec_tb_x(), H - TASKBAR_H + 3, REC_TB_W, TASKBAR_H - 6)) { h.type = HOT_REC; return h; }
    if (ON_TASKBAR && bat.present && in_rect(bat_x(), H - TASKBAR_H + 3, BAT_W, TASKBAR_H - 6)) {
        h.type = HOT_BAT; return h;
    }
    if (ON_TASKBAR) {
        TbItem it[MAXPINS + MAXWIN];
        int n = tb_items(it);
        for (int k = 0; k < n; k++) {
            int x, y, w, hh; task_rect(k, &x, &y, &w, &hh);
            if (in_rect(x, y, w, hh)) { h.type = HOT_TASK; h.a = k; return h; }
        }
        return h;
    }
    for (int k = nz - 1; k >= 0; k--) {           /* top window first */
        Win *w = &wins[zorder[k]];
        if (w->minimized || !in_rect(w->x, w->y, w->w, w->h)) continue;
        for (int b = 0; b < 3; b++) {
            int x, y, bw, bh; cap_rect(w, b, &x, &y, &bw, &bh);
            if (in_rect(x, y, bw, bh)) { h.type = HOT_CAP; h.a = zorder[k]; h.b = b; return h; }
        }
        int cx, cy, cw, ch; client_rect(w, &cx, &cy, &cw, &ch);
        if (w->hit && in_rect(cx, cy, cw, ch)) {
            int id = w->hit(mx - cx, my - cy, cw, ch);
            if (id >= 0) { h.type = HOT_CONTENT; h.a = zorder[k]; h.b = id; }
        }
        return h;                                 /* over a window */
    }
    int di = di_at_mouse();
    if (di >= 0) { h.type = HOT_ICON; h.a = di; }
    return h;
}

/* ---- taskbar -------------------------------------------------------- */
static void draw_orb(void) {
    int cx = orb_cx(), cy = orb_cy(), r = ORB_R;
    if (set_style) {                                       /* Modern: a flat round button */
        const Theme *T = TH();
        int hv = hot.type == HOT_ORB;
        uint32_t c = menu_open ? mix(T->accent, RGB(0, 0, 0), 0.15f) : hv ? mix(T->accent, RGB(255, 255, 255), 0.15f) : T->accent;
        fill_circle(cx, cy, r - 1, c, 255);
        draw_text_bold(cx - 12, cy - 8, "/o\\", RGB(255, 255, 255));
        return;
    }
    float hl = menu_open ? 0.30f : hot.type == HOT_ORB ? 0.18f : 0;
    int x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 2, y1 = cy + r + 2;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = clampf(r + 0.5f - d, 0, 1);
            if (cov <= 0) continue;
            float t = (dy + r) / (2.0f * r);
            uint32_t c = t < 0.5f
                ? mix(RGB(0x2c, 0x6c, 0xb4), RGB(0x0a, 0x33, 0x6e), t * 2)
                : mix(RGB(0x0a, 0x33, 0x6e), RGB(0x46, 0xb4, 0xf8), (t - 0.5f) * 2
                      * clampf(1.2f - fabsf(dx) / r, 0, 1));
            c = mix(c, RGB(0x8c, 0xd8, 0xff), hl);
            float ex = dx / (r * 0.78f), ey = (dy + r * 0.42f) / (r * 0.52f);
            if (ex * ex + ey * ey < 1)
                c = mix(c, RGB(255, 255, 255), 0.42f * (1 - (dy + r) / (r * 1.0f)));
            if (d > r - 1.6f) c = mix(c, RGB(0x04, 0x16, 0x30), 0.7f);
            blend(x, y, c, (int)(255 * cov));
        }
    const char *logo = "/o\\";
    int tx = cx - 12, ty = cy - 8;
    draw_text(tx + 1, ty + 1, logo, RGB(0, 0x10, 0x30), 170);
    draw_text(tx, ty, logo, RGB(255, 255, 255), 255);
    draw_text(tx + 1, ty, logo, RGB(255, 255, 255), 255);
}

static void draw_clock(void) {
    char tbuf[16], dbuf[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (set_clock24) strftime(tbuf, sizeof tbuf, "%H:%M", &tm);
    else {
        strftime(tbuf, sizeof tbuf, "%I:%M %p", &tm);
        if (tbuf[0] == '0') memmove(tbuf, tbuf + 1, strlen(tbuf));   /* 09:05 -> 9:05 */
    }
    strftime(dbuf, sizeof dbuf, "%d.%m.%Y", &tm);
    int ax = W - CLOCK_W - 4, y0 = H - TASKBAR_H;
    if (clk_panel || hot.type == HOT_CLOCK) {                  /* hovered, or its panel is open */
        if (set_style) fill_round(ax - 4, y0 + 3, CLOCK_W + 6, TASKBAR_H - 6, 4, clk_panel ? TH()->bar_active : TH()->bar_hover, 255);
        else { fill_round_grad(ax - 4, y0 + 3, CLOCK_W + 6, TASKBAR_H - 6, 3, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x90, 0xa0), clk_panel ? 70 : 45);
               stroke_round(ax - 4, y0 + 3, CLOCK_W + 6, TASKBAR_H - 6, 3, RGB(0, 0, 0), 100); }
    }
    if (set_style) {
        draw_text(ax + (CLOCK_W - text_w(tbuf)) / 2, y0 + 3, tbuf, TH()->text, 255);
        draw_text(ax + (CLOCK_W - text_w(dbuf)) / 2, y0 + 21, dbuf, TH()->text, 255);
        return;
    }
    draw_text_shadow(ax + (CLOCK_W - text_w(tbuf)) / 2, y0 + 3, tbuf);
    draw_text_shadow(ax + (CLOCK_W - text_w(dbuf)) / 2, y0 + 21, dbuf);
}

static void draw_task_buttons(void) {
    int act = active_win();
    TbItem it[MAXPINS + MAXWIN];
    int n = tb_items(it);
    for (int k = 0; k < n; k++) {
        int x, y, w, h; task_rect(k, &x, &y, &w, &h);
        int hv = hot.type == HOT_TASK && hot.a == k;
        if (set_style) {                                   /* Modern: flat, a line under running apps */
            const Theme *T = TH();
            if (it[k].win < 0) {
                if (hv) fill_round(x, y, w, h, 4, T->bar_hover, 255);
                const AppInfo *ai = app_info(pins[it[k].pin]);
                if (ai) draw_icon(ai->icon, x + w / 2, y + h / 2, 11);
                continue;
            }
            int i = it[k].win, on = i == act;
            if (on || hv) fill_round(x, y, w, h, 4, on ? (hv ? mix(T->bar_active, T->bar_hover, 0.5f) : T->bar_active) : T->bar_hover, 255);
            fill_round(x + (on ? 4 : w / 2 - 8), y + h - 3, on ? w - 8 : 16, 3, 1, on ? T->accent : T->text_in, 255);
            draw_icon(wins[i].icon, x + 18, y + h / 2, 9);
            char tt[32]; int maxc = (w - 40) / 8;
            snprintf(tt, sizeof tt, "%s", wins[i].title);
            if (text_len(tt) > maxc) { u8_trunc(tt, maxc - 2); strcat(tt, ".."); }
            draw_text(x + 34, y + (h - 16) / 2, tt, T->text, 255);
            continue;
        }
        if (it[k].win < 0) {                              /* pinned, not running: just the icon */
            if (hv) {
                fill_round_grad(x, y, w, h, 3, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x90, 0xa0), 55);
                stroke_round(x, y, w, h, 3, RGB(0, 0, 0), 110);
                stroke_round(x + 1, y + 1, w - 2, h - 2, 2, RGB(255, 255, 255), 70);
            }
            const AppInfo *ai = app_info(pins[it[k].pin]);
            if (ai) draw_icon(ai->icon, x + w / 2, y + h / 2, 11);
            continue;
        }
        int i = it[k].win;
        int on = i == act;
        if (on || hv) {
            fill_round_grad(x, y, w, h, 3, RGB(0xc8, 0xe4, 0xff), RGB(0x58, 0x8c, 0xc0),
                            on ? (hv ? 120 : 95) : 60);
            stroke_round(x, y, w, h, 3, RGB(0, 0, 0), 150);
            stroke_round(x + 1, y + 1, w - 2, h - 2, 2, RGB(255, 255, 255), on ? 110 : 70);
        } else {
            fill_round_grad(x, y, w, h, 3, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x90, 0xa0), 30);
            stroke_round(x, y, w, h, 3, RGB(0, 0, 0), 110);
            stroke_round(x + 1, y + 1, w - 2, h - 2, 2, RGB(255, 255, 255), 45);
        }
        draw_icon(wins[i].icon, x + 18, y + h / 2, 9);
        char t[32];
        int maxc = (w - 40) / 8;
        snprintf(t, sizeof t, "%s", wins[i].title);
        if (text_len(t) > maxc) { u8_trunc(t, maxc - 2); strcat(t, ".."); }
        draw_text_shadow(x + 34, y + (h - 16) / 2, t);
    }
}

static void draw_taskbar(void) {
    int y0 = H - TASKBAR_H;
    if (clip_y1 <= y0) return;
    if (set_style) {
        fill_rect(0, y0, W, TASKBAR_H, TH()->bar, 255);
        fill_rect(0, y0, W, 1, TH()->bar_line, 255);
    } else {
        fill_rect(0, y0, W, TASKBAR_H, RGB(0x0c, 0x1c, 0x2c), 175);
        for (int j = 0; j < TASKBAR_H / 2; j++)
            fill_rect(0, y0 + j, W, 1, RGB(255, 255, 255), 34 - j * 30 / (TASKBAR_H / 2));
        fill_rect(0, y0, W, 1, RGB(0, 0, 0), 150);
        fill_rect(0, y0 + 1, W, 1, RGB(255, 255, 255), 70);
    }
    draw_orb();
    draw_task_buttons();
    draw_clock();
    rec_draw_tb();
    /* network icon in the tray */
    int tx = tray_x(), ty = H - TASKBAR_H + 3;
    if (net_panel || hot.type == HOT_TRAY) {
        if (set_style) fill_round(tx, ty, TRAY_W, TASKBAR_H - 6, 4, net_panel ? TH()->bar_active : TH()->bar_hover, 255);
        else {
            fill_round_grad(tx, ty, TRAY_W, TASKBAR_H - 6, 3, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x90, 0xa0), net_panel ? 70 : 45);
            stroke_round(tx, ty, TRAY_W, TASKBAR_H - 6, 3, RGB(0, 0, 0), 100);
        }
    }
    draw_tray_icon(tx, ty);
    if (bat.present) {                                     /* the battery */
        int bx = bat_x();
        if (hot.type == HOT_BAT) {
            if (set_style) fill_round(bx, ty, BAT_W, TASKBAR_H - 6, 4, TH()->bar_hover, 255);
            else { fill_round_grad(bx, ty, BAT_W, TASKBAR_H - 6, 3, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x90, 0xa0), 45);
                   stroke_round(bx, ty, BAT_W, TASKBAR_H - 6, 3, RGB(0, 0, 0), 100); }
        }
        draw_battery(bx + 5, ty + (TASKBAR_H - 6) / 2 - 6, bat.pct, bat.charging || bat.ac, set_style ? TH()->text : RGB(0xf0, 0xf4, 0xf8));
    }
}

/* ---- start menu ----------------------------------------------------- */
static void draw_items(MenuItem *items, int x, int w, int dark, int hottype) {
    for (int i = 0; items[i].label; i++) {
        int y = ITEMS_Y + i * ITEM_H;
        if (set_style && hot.type == hottype && hot.a == i) fill_round(x, y, w, ITEM_H, 4, TH()->hover, 255);
        else if (hot.type == hottype && hot.a == i) {
            fill_round(x, y, w, ITEM_H, 3, dark ? RGB(255, 255, 255) : RGB(0xdc, 0xec, 0xfc),
                       dark ? 45 : 255);
            stroke_round(x, y, w, ITEM_H, 3, dark ? RGB(255, 255, 255) : RGB(0x7d, 0xa2, 0xce),
                         dark ? 90 : 255);
        }
        int tx = x + 10;
        if (items[i].icon) { draw_icon(items[i].icon, x + 18, y + ITEM_H / 2, 10); tx = x + 36; }
        if (set_style) draw_text(tx, y + (ITEM_H - 16) / 2, items[i].label, TH()->text, 255);
        else if (dark) draw_text_shadow(tx, y + (ITEM_H - 16) / 2, items[i].label);
        else draw_text(tx, y + (ITEM_H - 16) / 2, items[i].label, RGB(0x1e, 0x1e, 0x1e), 255);
    }
}

static void draw_menu(void) {
    int x = menu_x(), y = menu_y();
    if (clip_x0 >= x + MENU_W + 3 || clip_y1 <= y || clip_y0 >= y + MENU_H + 3) return;
    if (set_style) {                                       /* Modern: a flat panel */
        const Theme *T = TH();
        for (int k = 1; k <= 3; k++) fill_round(x - k + 2, y - k + 3, MENU_W + 2 * k - 2, MENU_H + 2 * k - 2, 8 + k, RGB(0, 0, 0), 14);
        fill_round(x, y, MENU_W, MENU_H, 8, T->menu, 255);
        stroke_round(x, y, MENU_W, MENU_H, 8, T->menu_line, 255);
        fill_round(LP_X, LP_Y, LP_W, LP_H, 6, T->menu_left, 255);
        stroke_round(LP_X, LP_Y, LP_W, LP_H, 6, T->menu_line, 255);
        draw_items(left_items, LI_X, LI_W, 0, HOT_LITEM);
        ms_draw();
        draw_items(right_items, RP_X, RP_W, 1, HOT_RITEM);
        for (int i = 0; i < NPOWER; i++) {
            int bx, by, bw, bh; power_rect(i, &bx, &by, &bw, &bh);
            int sh = hot.type == HOT_SD && hot.a == i;
            fill_round(bx, by, bw, bh, 5, sh ? T->btn_hover : T->btn, 255);
            draw_text(bx + (bw - text_w(power_label[i])) / 2, by + 5, power_label[i], T->text, 255);
        }
        return;
    }
    fill_round(x + 3, y + 3, MENU_W, MENU_H, 7, RGB(0, 0, 0), 70);
    fill_round(x, y, MENU_W, MENU_H, 7, RGB(0x16, 0x34, 0x56), 215);
    for (int j = 0; j < 90; j++)
        fill_rect(x + 1, y + j + 1, MENU_W - 2, 1, RGB(0x8c, 0xc8, 0xf0), 50 - j * 50 / 90);
    stroke_round(x, y, MENU_W, MENU_H, 7, RGB(0, 0, 0), 170);
    stroke_round(x + 1, y + 1, MENU_W - 2, MENU_H - 2, 6, RGB(255, 255, 255), 80);

    fill_round(LP_X, LP_Y, LP_W, LP_H, 4, RGB(255, 255, 255), 250);
    stroke_round(LP_X, LP_Y, LP_W, LP_H, 4, RGB(0x2c, 0x4a, 0x6a), 220);
    draw_items(left_items, LI_X, LI_W, 0, HOT_LITEM);
    ms_draw();
    draw_items(right_items, RP_X, RP_W, 1, HOT_RITEM);

    for (int i = 0; i < NPOWER; i++) {                     /* Shut down / Restart */
        int bx, by, bw, bh; power_rect(i, &bx, &by, &bw, &bh);
        int sh = hot.type == HOT_SD && hot.a == i;
        fill_round_grad(bx, by, bw, bh, 3,
                        sh ? RGB(0x9c, 0xd4, 0xfc) : RGB(0x6a, 0x84, 0x9c),
                        sh ? RGB(0x2a, 0x7c, 0xc8) : RGB(0x2a, 0x40, 0x56), 235);
        fill_rect(bx + 2, by + 1, bw - 4, bh / 2 - 1, RGB(255, 255, 255), 28);
        stroke_round(bx, by, bw, bh, 3, RGB(0x08, 0x14, 0x22), 220);
        stroke_round(bx + 1, by + 1, bw - 2, bh - 2, 2, RGB(255, 255, 255), 60);
        draw_text_shadow(bx + (bw - text_w(power_label[i])) / 2, by + 5, power_label[i]);
    }
}

/* ---- window frame --------------------------------------------------- */
/* white glyph with a dark outline, used for the caption button symbols */
static void glyph_rect(int x, int y, int w, int h, int white) {
    if (!white) fill_rect(x - 1, y - 1, w + 2, h + 2, RGB(0x14, 0x22, 0x34), 200);
    else fill_rect(x, y, w, h, RGB(255, 255, 255), 255);
}
static void glyph_box(int x, int y, int w, int h) {       /* window outline */
    for (int pass = 0; pass < 2; pass++) {
        glyph_rect(x, y, w, 2, pass); glyph_rect(x, y + h - 1, w, 1, pass);
        glyph_rect(x, y, 1, h, pass); glyph_rect(x + w - 1, y, 1, h, pass);
    }
}
static void glyph_x(int x, int y) {
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < 8; k++) {
            uint32_t c = pass ? RGB(255, 255, 255) : RGB(0x40, 0x08, 0x04);
            int a = pass ? 255 : 150;
            for (int t = 0; t < 2; t++) {
                int px1 = x + k + t, px2 = x + 7 - k + t, py = y + k;
                if (!pass) {
                    for (int o = -1; o <= 1; o += 2) {
                        blend(px1 + o, py, c, a); blend(px1, py + o, c, a);
                        blend(px2 + o, py, c, a); blend(px2, py + o, c, a);
                    }
                } else { blend(px1, py, c, a); blend(px2, py, c, a); }
            }
        }
}

/* a thin, smooth line (for the Modern caption symbols) */
static void aa_line(float x0, float y0, float x1, float y1, float wdt, uint32_t c) {
    int xa = (int)fminf(x0, x1) - 2, ya = (int)fminf(y0, y1) - 2, xb = (int)fmaxf(x0, x1) + 3, yb = (int)fmaxf(y0, y1) + 3;
    float dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0, t2 = L2 > 0 ? clampf((px * dx + py * dy) / L2, 0, 1) : 0;
            float ex = px - t2 * dx, ey = py - t2 * dy, d = sqrtf(ex * ex + ey * ey);
            float cov = clampf(wdt / 2 + 0.5f - d, 0, 1);
            if (cov > 0) blend(x, y, c, (int)(255 * cov));
        }
}
static void draw_caption_modern(Win *w, int idx, int b) {
    int x, y, bw, bh; cap_rect(w, b, &x, &y, &bw, &bh);
    if (x >= clip_x1 || x + bw <= clip_x0 || y >= clip_y1 || y + bh <= clip_y0) return;
    int hv = hot.type == HOT_CAP && hot.a == idx && hot.b == b, dis = b == CAP_MAX && w->no_max;
    if (dis) hv = 0;
    const Theme *T = TH();
    int act = idx == active_win();
    uint32_t fg = dis ? T->text_in : act ? T->text : T->text_in;
    if (hv && b == CAP_CLOSE) { fill_rect(x, y, bw, bh, RGB(0xe8, 0x11, 0x23), 255); fg = RGB(255, 255, 255); }
    else if (hv) fill_rect(x, y, bw, bh, set_dark ? RGB(0x40, 0x43, 0x48) : RGB(0xe4, 0xe6, 0xea), 255);
    float gx = x + bw / 2.0f, gy = y + bh / 2.0f;
    if (b == CAP_MIN) aa_line(gx - 5, gy, gx + 5, gy, 1.1f, fg);
    else if (b == CAP_MAX) {
        if (!w->maximized && !w->fullscreen) {
            aa_line(gx - 5, gy - 5, gx + 5, gy - 5, 1.1f, fg); aa_line(gx - 5, gy + 5, gx + 5, gy + 5, 1.1f, fg);
            aa_line(gx - 5, gy - 5, gx - 5, gy + 5, 1.1f, fg); aa_line(gx + 5, gy - 5, gx + 5, gy + 5, 1.1f, fg);
        } else {                                       /* restore: two squares */
            aa_line(gx - 5, gy - 3, gx + 3, gy - 3, 1.1f, fg); aa_line(gx - 5, gy + 5, gx + 3, gy + 5, 1.1f, fg);
            aa_line(gx - 5, gy - 3, gx - 5, gy + 5, 1.1f, fg); aa_line(gx + 3, gy - 3, gx + 3, gy + 5, 1.1f, fg);
            aa_line(gx - 3, gy - 5, gx + 5, gy - 5, 1.1f, fg); aa_line(gx + 5, gy - 5, gx + 5, gy + 3, 1.1f, fg);
        }
    } else { aa_line(gx - 5, gy - 5, gx + 5, gy + 5, 1.2f, fg); aa_line(gx + 5, gy - 5, gx - 5, gy + 5, 1.2f, fg); }
}
static void draw_caption_button(Win *w, int idx, int b) {
    if (set_style) { draw_caption_modern(w, idx, b); return; }
    int x, y, bw, bh; cap_rect(w, b, &x, &y, &bw, &bh);
    if (x >= clip_x1 || x + bw <= clip_x0 || y >= clip_y1 || y + bh <= clip_y0) return;
    int hv = hot.type == HOT_CAP && hot.a == idx && hot.b == b;
    if (b == CAP_MAX && w->no_max) hv = 0;               /* greyed out */
    uint32_t top, bot;
    if (b == CAP_CLOSE) {
        top = hv ? RGB(0xf4, 0x9a, 0x86) : RGB(0xe0, 0x8c, 0x7c);
        bot = hv ? RGB(0xe0, 0x28, 0x10) : RGB(0xb4, 0x2c, 0x1c);
    } else {
        top = hv ? RGB(0xe0, 0xf2, 0xff) : RGB(0xd4, 0xe0, 0xec);
        bot = hv ? RGB(0x3c, 0xa0, 0xe8) : RGB(0x7c, 0x96, 0xb4);
    }
    fill_round_grad(x, y, bw, bh, 3, top, bot, 240);
    fill_rect(x + 2, y + 1, bw - 4, bh / 2 - 1, RGB(255, 255, 255), 40);
    stroke_round(x, y, bw, bh, 3, RGB(0x10, 0x20, 0x34), 210);
    stroke_round(x + 1, y + 1, bw - 2, bh - 2, 2, RGB(255, 255, 255), 90);
    int gx = x + bw / 2, gy = y + bh / 2;
    if (b == CAP_MIN) {
        glyph_rect(gx - 4, gy + 3, 9, 2, 0); glyph_rect(gx - 4, gy + 3, 9, 2, 1);
    } else if (b == CAP_MAX && w->no_max) {           /* disabled: faint box */
        fill_rect(gx - 5, gy - 4, 10, 2, RGB(0xf0, 0xf4, 0xf8), 130);
        fill_rect(gx - 5, gy + 3, 10, 1, RGB(0xf0, 0xf4, 0xf8), 130);
        fill_rect(gx - 5, gy - 4, 1, 8, RGB(0xf0, 0xf4, 0xf8), 130);
        fill_rect(gx + 4, gy - 4, 1, 8, RGB(0xf0, 0xf4, 0xf8), 130);
    } else if (b == CAP_MAX) {
        if (!w->maximized) glyph_box(gx - 5, gy - 4, 10, 8);
        else {                                    /* "restore": two stacked boxes */
            for (int pass = 0; pass < 2; pass++) {
                glyph_rect(gx - 2, gy - 6, 8, 2, pass);
                glyph_rect(gx + 5, gy - 6, 1, 6, pass);
            }
            glyph_box(gx - 5, gy - 3, 9, 8);
        }
    } else {
        glyph_x(gx - 4, gy - 4);
    }
}

static void draw_window(int idx) {
    Win *w = &wins[idx];
    if (!w->open || w->minimized) return;
    if (w->x - 2 >= clip_x1 || w->x + w->w + 6 <= clip_x0 ||
        w->y - 2 >= clip_y1 || w->y + w->h + 6 <= clip_y0) return;
    int act = idx == active_win();
    int cx, cy, cw, ch; client_rect(w, &cx, &cy, &cw, &ch);

    if (set_style) {                                       /* Modern: flat and solid */
        const Theme *T = TH();
        if (!w->fullscreen) {
            for (int k = 1; k <= 4; k++)                       /* a soft shadow */
                fill_round(w->x - k + 2, w->y - k + 4, w->w + 2 * k - 4, w->h + 2 * k - 4, 6 + k, RGB(0, 0, 0), act ? 16 : 9);
            fill_rect(w->x, w->y, w->w, w->h, act ? T->title : T->title_in, 255);
            stroke_round(w->x, w->y, w->w, w->h, 1, act ? T->border : T->border_in, 255);
        }
        fill_rect(cx, cy, cw, ch, RGB(0xff, 0xff, 0xff), 255);
        if (w->draw) {
            int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;
            clip_x0 = imax(clip_x0, cx); clip_y0 = imax(clip_y0, cy);
            clip_x1 = imin(clip_x1, cx + cw); clip_y1 = imin(clip_y1, cy + ch);
            if (clip_x0 < clip_x1 && clip_y0 < clip_y1) {
                int dk = DARK && !w->rawkey && !w->no_dark;
                if (dk && !dk_mask) dk_mask = calloc((size_t)W * H, 1);
                if (dk) dk_clear(cx, cy, cw, ch);
                nkeep = 0; dk_collect = dk;
                w->draw(cx, cy, cw, ch);
                dk_collect = 0;
                if (dk) dark_xform(cx, cy, cw, ch);
                nkeep = 0;
            }
            clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;
        }
        if (w->fullscreen) return;
        int sk = dk_collect; dk_collect = 0;
        draw_icon(w->icon, w->x + 18, w->y + TITLE_H / 2, 8);
        dk_collect = sk;
        int maxc = (w->w - 46 * 3 - 44) / 8;
        char tt[96]; snprintf(tt, sizeof tt, "%s", w->title ? w->title : "");
        if (maxc > 3 && text_len(tt) > maxc) { u8_trunc(tt, maxc - 3); strcat(tt, "..."); }
        draw_text(w->x + 34, w->y + (TITLE_H - 16) / 2, tt, act ? T->text : T->text_in, 255);
        for (int b = 0; b < 3; b++) draw_caption_button(w, idx, b);
        return;
    }

    /* soft shadow */
    fill_round(w->x - 1, w->y, w->w + 4, w->h + 5, 9, RGB(0, 0, 0), act ? 55 : 30);
    fill_round(w->x + 1, w->y + 2, w->w, w->h + 1, 8, RGB(0, 0, 0), act ? 45 : 25);

    /* glass frame: tinted, see-through, with diagonal light streaks */
    uint32_t glass = act ? RGB(0x48, 0x84, 0xc0) : RGB(0xa0, 0xb8, 0xcc);
    int ga = act ? 170 : 140;
    int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h;
    if (clip_range(&x0, &y0, &x1, &y1))
        for (int j = y0; j < y1; j++)
            for (int i = x0; i < x1; i++) {
                if (i >= cx && i < cx + cw && j >= cy && j < cy + ch) { i = cx + cw - 1; continue; }
                float cov = round_cov(i, j, w->x, w->y, w->w, w->h, 7);
                if (cov <= 0) continue;
                blend(i, j, glass, (int)(ga * cov));
                float s = sinf(((i - w->x) + (j - w->y) * 0.55f) * 0.03f);
                if (s > 0.80f) blend(i, j, RGB(255, 255, 255), (int)((s - 0.80f) * 160 * cov));
                if (j - w->y < TITLE_H)
                    blend(i, j, RGB(255, 255, 255), (int)(38 * (1 - (float)(j - w->y) / TITLE_H) * cov));
            }
    stroke_round(w->x, w->y, w->w, w->h, 7, RGB(0, 0, 0), act ? 190 : 120);
    stroke_round(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 6, RGB(255, 255, 255), act ? 110 : 80);

    /* client area */
    fill_rect(cx - 1, cy - 1, cw + 2, ch + 2, RGB(0x2c, 0x48, 0x68), act ? 200 : 140);
    fill_rect(cx, cy, cw, ch, RGB(0xff, 0xff, 0xff), 255);
    if (w->draw) {
        int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;
        clip_x0 = imax(clip_x0, cx); clip_y0 = imax(clip_y0, cy);
        clip_x1 = imin(clip_x1, cx + cw); clip_y1 = imin(clip_y1, cy + ch);
        if (clip_x0 < clip_x1 && clip_y0 < clip_y1) w->draw(cx, cy, cw, ch);
        clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;
    }

    /* title */
    draw_icon(w->icon, w->x + 18, w->y + TITLE_H / 2, 8);
    draw_text_glow(w->x + 34, w->y + (TITLE_H - 16) / 2, w->title);

    for (int b = 0; b < 3; b++) draw_caption_button(w, idx, b);
}

/* ===================================================================
 *  THE MOUSE POINTER - drawn from outlines, so every size stays sharp
 * =================================================================== */
enum { CUR_ARROW, CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW };
static const float cursor_scales[3] = { 1.0f, 1.5f, 2.0f };
/* a plain arrowhead: tip, right corner, notch, bottom of the left edge */
static const float arrow_pts[] = { 0.5f, 0.5f, 16.5f, 16.6f, 9.4f, 17.0f, 0.5f, 23.4f };
static const float dbl_pts[] = { -11, 0, -5.5f, -5.5f, -5.5f, -2.1f, 5.5f, -2.1f, 5.5f, -5.5f, 11, 0,
                                 5.5f, 5.5f, 5.5f, 2.1f, -5.5f, 2.1f, -5.5f, 5.5f };

static void cursor_colors(int col, uint32_t *fill, uint32_t *edge) {
    if (col == 1)      { *fill = RGB(0x12, 0x12, 0x14); *edge = RGB(255, 255, 255); }
    else if (col == 2) { *fill = RGB(0x2e, 0x8c, 0xf0); *edge = RGB(255, 255, 255); }
    else               { *fill = RGB(255, 255, 255);    *edge = RGB(0, 0, 0); }
}
/* the area (around the hot spot) any pointer of size s can cover */
static void cursor_box(float s, int *x0, int *y0, int *x1, int *y1) {
    *x0 = (int)floorf(-12 * s) - 2; *y0 = (int)floorf(-12 * s) - 2;
    *x1 = (int)ceilf(20 * s) + 4;   *y1 = (int)ceilf(27 * s) + 4;
}
/* distance from a point to the outline of a polygon: below 0 = inside */
static float poly_sd(const float *p, int n, float px, float py) {
    float best = 1e9f; int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float ax = p[2 * j], ay = p[2 * j + 1], bx = p[2 * i], by = p[2 * i + 1];
        if ((by > py) != (ay > py) && px < (ax - bx) * (py - by) / (ay - by) + bx) inside = !inside;
        float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
        float t = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy), 0, 1);
        float dx = wx - vx * t, dy = wy - vy * t, d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    best = sqrtf(best);
    return inside ? -best : best;
}
/* draw a pointer with its hot spot at (hx, hy); put() sets one pixel */
static void render_cursor(int shape, int col, float s, int hx, int hy, void (*put)(int, int, uint32_t, int)) {
    uint32_t fill, edge; cursor_colors(col, &fill, &edge);
    const float *pts = shape == CUR_ARROW ? arrow_pts : dbl_pts;
    int n = shape == CUR_ARROW ? 4 : 10;
    float ang = shape == CUR_NS ? 1.5707963f : shape == CUR_NWSE ? 0.7853982f : shape == CUR_NESW ? -0.7853982f : 0;
    float ca = cosf(ang), sa = sinf(ang), o = s > 1.2f ? 1.1f * s : 1.0f;     /* outline width */
    int x0, y0, x1, y1; cursor_box(s, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = (x + 0.5f) / s, py = (y + 0.5f) / s;
            if (shape != CUR_ARROW) { float rx = px * ca + py * sa, ry = -px * sa + py * ca; px = rx; py = ry; }
            float d = poly_sd(pts, n, px, py) * s - 0.6f;              /* in screen pixels (corners a bit round) */
            float outer = clampf(0.5f - d, 0, 1);
            if (outer <= 0) {
                if (shape == CUR_ARROW) {                                /* a soft shadow, down and right */
                    float sx = (x - 1.0f * s + 0.5f) / s, sy = (y - 1.6f * s + 0.5f) / s;
                    float sd = poly_sd(pts, n, sx, sy) * s;
                    float sh = clampf(1.0f - sd / (2.2f * s), 0, 1);
                    if (sh > 0) put(hx + x, hy + y, RGB(0, 0, 0), (int)(80 * sh * sh));
                }
                continue;
            }
            float in = clampf(-d - o + 0.5f, 0, 1);
            put(hx + x, hy + y, mix(edge, fill, in), (int)(255 * outer));
        }
}
/* for previews inside windows: draw into the scene, inside the clip area */
static void put_back(int x, int y, uint32_t c, int a) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    uint32_t *p = &back[(size_t)y * W + x];
    *p = a >= 255 ? c : mix(*p, c, a / 255.0f);
}

/* ===================================================================
 *  SETTINGS APP
 * =================================================================== */
#define SW_SIZE 38                 /* color swatch */
#define SW_GAP  10
#define TG_W    46                 /* on/off switch */
#define TG_H    24
#define ST_W    680                /* window content size */
#define ST_H    590
#define ST_TAB_Y 10                /* tab strip */
#define ST_TAB_H 30
#define ST_Y    (ST_TAB_Y + ST_TAB_H)  /* top of the page */
enum { TAB_LOOK, TAB_CLOCK, TAB_SOUND, TAB_MOUSE, TAB_POWER, TAB_BT, TAB_UPDATE, TAB_ABOUT, NTABS };
static const char *st_tabs[NTABS] = { "Appearance", "Clock", "Sound", "Keyboard & mouse", "Power", "Bluetooth", "Update", "About" };
static int st_tab;

static void st_tab_rect(int i, int *x, int *w) {
    int xx = 12;
    for (int k = 0; k < i; k++) xx += text_w(st_tabs[k]) + 18 + 2;
    *x = xx; *w = text_w(st_tabs[i]) + 18;
}
static void st_switch(int x, int y, int on) {
    fill_round(x, y, TG_W, TG_H, TG_H / 2, on ? RGB(0x33, 0x99, 0xff) : RGB(0xc4, 0xc8, 0xcc), 255);
    fill_circle(on ? x + TG_W - TG_H / 2.0f : x + TG_H / 2.0f, y + TG_H / 2.0f, TG_H / 2.0f - 3, RGB(255, 255, 255), 255);
    draw_text(x - 32, y + 4, on ? "On" : "Off", RGB(0x70, 0x70, 0x70), 255);
}
/* a button in a row of choices; the chosen one is blue */
static void st_choice(int x, int y, int w, int h, const char *label, int on) {
    if (on) {
        fill_round(x, y, w, h, 5, RGB(0xdc, 0xec, 0xfc), 255);
        stroke_round(x, y, w, h, 5, RGB(0x33, 0x99, 0xff), 255);
        stroke_round(x + 1, y + 1, w - 2, h - 2, 4, RGB(0x33, 0x99, 0xff), 255);
    } else {
        fill_round_grad(x, y, w, h, 5, RGB(0xfc, 0xfc, 0xfd), RGB(0xe8, 0xea, 0xee), 255);
        stroke_round(x, y, w, h, 5, RGB(0xa8, 0xb0, 0xbc), 255);
    }
    if (label) draw_text(x + (w - text_w(label)) / 2, y + h - 22, label, RGB(0x20, 0x20, 0x20), 255);
}
static void st_heading(int cx, int y, const char *s) { draw_text_bold(cx + 24, y, s, RGB(0x1e, 0x39, 0x87)); }
static void st_line(int cx, int cw, int y) { fill_rect(cx + 24, y, cw - 48, 1, RGB(0xdc, 0xe4, 0xec), 255); }

/* where things are on each page (client coordinates) */
#define LK_SW_Y   (ST_Y + 50)
#define LK_PIC_Y  (ST_Y + 150)     /* background picture */
#define LK_FONT_Y (ST_Y + 360)
#define LK_POS_W  72
#define VOL_Y     150          /* volume slider row (from the page top) */
#define VOL_X     60
#define VOL_R     60
#define PW_BR_Y   120          /* Power page rows */
#define PW_OFF_Y  200
#define PW_SLP_Y  270
#define PW_LID_Y  340
#define PW_NOW_Y  400
#define PW_CW     96
#define MS_COL_Y  (ST_Y + 46)
#define MS_COL_W  140
#define MS_COL_H  78
#define MS_SIZE_Y (ST_Y + 160)
#define MS_SPD_Y  (ST_Y + 250)
#define MS_BTN_W  140
#define MS_BTN_H  30
#define MS_KBD_Y  (ST_Y + 350)       /* keyboard layout */

/* ---- the little popup above the taskbar (Caps Lock, volume) ---------- */
static long long ui_ms(void);
enum { OSD_NONE, OSD_CAPS, OSD_VOL, OSD_BRIGHT };
static int osd_kind;
static long long osd_until;
#define OSD_W 250
#define OSD_H 54
static int osd_x(void) { return (W - OSD_W) / 2; }
static int osd_y(void) { return H - TASKBAR_H - OSD_H - 28; }
static void repaint_osd(void) { repaint(osd_x() - 4, osd_y() - 4, OSD_W + 12, OSD_H + 12); }
static void osd_show(int kind) {
    osd_kind = kind; osd_until = ui_ms() + 1600;
    repaint_osd();
}
static void draw_speaker(int x, int y, uint32_t c, int muted, int level) {   /* a speaker, 24 x 24 */
    fill_round(x + 1, y + 9, 7, 7, 1, c, 255);                          /* the box */
    for (int k = 0; k < 7; k++) fill_rect(x + 7 + k, y + 9 - k, 1, 7 + 2 * k, c, 255);   /* the cone */
    if (muted) {
        for (int k = 0; k < 9; k++) {
            fill_rect(x + 15 + k, y + 7 + k, 2, 2, RGB(0xe0, 0x50, 0x40), 255);
            fill_rect(x + 23 - k, y + 7 + k, 2, 2, RGB(0xe0, 0x50, 0x40), 255);
        }
        return;
    }
    for (int w = 0; w < 3 && w < level; w++) {          /* sound waves */
        float r = 5.0f + w * 4.0f;
        for (int a = -40; a <= 40; a += 4) {
            float s = a * 3.14159f / 180;
            fill_rect(x + 13 + (int)(r * cosf(s)), y + 12 + (int)(r * sinf(s)), 2, 2, c, 255);
        }
    }
}
static void draw_osd(void) {
    if (!osd_kind) return;
    int x = osd_x(), y = osd_y();
    fill_round(x + 2, y + 3, OSD_W, OSD_H, 10, RGB(0, 0, 0), 70);
    uint32_t white = RGB(0xf2, 0xf6, 0xfa);
    if (set_style) {
        fill_round(x, y, OSD_W, OSD_H, 10, TH()->menu, 255);
        stroke_round(x, y, OSD_W, OSD_H, 10, TH()->menu_line, 255);
        white = TH()->text;
    } else {
        fill_round_grad(x, y, OSD_W, OSD_H, 10, RGB(0x2c, 0x3c, 0x52), RGB(0x14, 0x1e, 0x2c), 235);
        stroke_round(x, y, OSD_W, OSD_H, 10, RGB(0x6a, 0x84, 0xa8), 200);
    }
    if (osd_kind == OSD_CAPS) {
        extern int capslock_on(void);
        const char *s = capslock_on() ? "Caps Lock on" : "Caps Lock off";
        draw_text_bold(x + (OSD_W - text_w(s)) / 2, y + (OSD_H - 16) / 2, s, white);
    } else {
        int v = osd_kind == OSD_BRIGHT ? set_bright : set_muted ? 0 : set_volume;
        int bx = x + 22, bw = OSD_W - 22 - 56, by = y + OSD_H / 2 - 3;
        if (osd_kind == OSD_BRIGHT) { draw_sun_icon(x + 24, y + OSD_H / 2, 10); bx += 22; bw -= 22; }
        fill_round(bx, by, bw, 6, 3, RGB(0x50, 0x5c, 0x6c), 255);
        if (v) fill_round(bx, by, imax(6, bw * v / 100), 6, 3, RGB(0x4c, 0xa0, 0xf0), 255);
        fill_circle(bx + bw * v / 100.0f, by + 3, 7, white, 255);
        char s[8]; snprintf(s, sizeof s, "%d", v);
        draw_text_bold(x + OSD_W - 22 - text_w(s), y + OSD_H / 2 - 8, s, white);
    }
}
static int osd_wait(void) {                 /* ms until the popup should go away, or -1 */
    if (!osd_kind) return -1;
    long long left = osd_until - ui_ms();
    if (left <= 0) { osd_kind = OSD_NONE; repaint_osd(); return -1; }
    return (int)left;
}

/* ---- notifications: small cards above the clock ----------------------- */
#define MAXTOAST 3
#define TOAST_W 330
#define TOAST_H 74
static struct { char title[64], body[160]; int icon; long long until; char app[96]; unsigned nid; } toasts[MAXTOAST];
static int ntoast;
static int toast_y(int i) { return H - TASKBAR_H - 10 - (i + 1) * (TOAST_H + 8); }
static void repaint_toasts(void) { repaint(W - TOAST_W - 16, toast_y(MAXTOAST - 1) - 6, TOAST_W + 22, (TOAST_H + 8) * MAXTOAST + 14); }
static void toast(int icon, const char *title, const char *body) {
    if (ntoast == MAXTOAST) { memmove(toasts, toasts + 1, sizeof toasts[0] * (MAXTOAST - 1)); ntoast--; }
    snprintf(toasts[ntoast].title, sizeof toasts[0].title, "%s", title);
    snprintf(toasts[ntoast].body, sizeof toasts[0].body, "%s", body ? body : "");
    toasts[ntoast].icon = icon; toasts[ntoast].until = ui_ms() + 5500; toasts[ntoast].app[0] = 0; toasts[ntoast].nid = 0;
    ntoast++;
    repaint_toasts();
    static long long last_chime;                 /* the chime (not twice in a row for a burst) */
    if (set_notify_sound && ui_ms() - last_chime > 1500) {
        last_chime = ui_ms();
        if (fork() == 0) {
            setsid();
            for (int fd = 3; fd < 256; fd++) close(fd);
            execl("/bin/playsound", "playsound", "/usr/share/sounds/notify.wav", (char *)NULL);
            _exit(0);
        }
    }
}

/* notifications from apps (ccos-notifyd passes them on through a pipe) */
#define TOAST_FIFO "/run/ccos/toast"
#define TOAST_ACT  "/run/ccos/toast-act"
static int toast_fd = -1;
static void restore_win(int idx);
static void run(const char *cmd);
static void open_file(const char *path);
static void toast_fifo_open(void) {
    mkdir("/run/ccos", 0755);
    unlink(TOAST_FIFO);
    if (mkfifo(TOAST_FIFO, 0622) == 0) toast_fd = open(TOAST_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}
static void toast_clicked(int i) {           /* tell the app; bring its window forward */
    if (!toasts[i].nid) return;
    int fd = open(TOAST_ACT, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) { char b[24]; snprintf(b, sizeof b, "%u\n", toasts[i].nid); if (write(fd, b, strlen(b)) < 0) {} close(fd); }
    if (toasts[i].app[0]) {
        char cmd[112]; snprintf(cmd, sizeof cmd, "flatpak:%s", toasts[i].app);
        for (int w = 0; w < MAXWIN; w++) if (wins[w].open && !strcmp(wins[w].cmd, cmd)) { restore_win(w); return; }
    }
}
static void toast_line(char *l) {
    if (l[0] == 'N' && l[1] == ' ') {            /* N id<TAB>app<TAB>title<TAB>body */
        char *f[4] = { l + 2, NULL, NULL, NULL };
        for (int k = 1; k < 4; k++) { f[k] = f[k - 1] ? strchr(f[k - 1], '\t') : NULL; if (f[k]) *f[k]++ = 0; }
        if (!f[2]) return;
        unsigned id = (unsigned)strtoul(f[0], NULL, 10);
        int icon = ICON_APP; const char *name = f[1];
        for (int k = 0; k < nfp; k++) if (!strcmp(fpapps[k].id, f[1])) { icon = fp_info[k].icon; name = fpapps[k].name; }
        for (int k = 0; k < ntoast; k++) if (id && toasts[k].nid == id) {     /* an update to one on screen */
            memmove(toasts + k, toasts + k + 1, sizeof toasts[0] * (size_t)(ntoast - k - 1)); ntoast--; break;
        }
        char title[200]; snprintf(title, sizeof title, "%s", f[2][0] ? f[2] : name);
        toast(icon, title, f[3] ? f[3] : "");
        toasts[ntoast - 1].nid = id;
        snprintf(toasts[ntoast - 1].app, sizeof toasts[0].app, "%s", f[1]);
        toasts[ntoast - 1].until = ui_ms() + 7000;
    } else if (l[0] == 'C' && l[1] == ' ') {     /* the app took it back */
        unsigned id = (unsigned)strtoul(l + 2, NULL, 10);
        for (int k = 0; k < ntoast; k++) if (toasts[k].nid == id) {
            memmove(toasts + k, toasts + k + 1, sizeof toasts[0] * (size_t)(ntoast - k - 1)); ntoast--; repaint_toasts(); break;
        }
    } else if (l[0] == 'O' && l[1] == ' ') {     /* xdg-open: open a file with a ccos app */
        open_file(l + 2);
    }
}
static void toast_read(void) {
    static char buf[4096]; static int have;
    ssize_t r;
    while ((r = read(toast_fd, buf + have, sizeof buf - 1 - (size_t)have)) > 0) {
        have += (int)r; buf[have] = 0;
        char *s = buf, *nl;
        while ((nl = strchr(s, '\n'))) { *nl = 0; toast_line(s); s = nl + 1; }
        have = (int)strlen(s); memmove(buf, s, (size_t)have + 1);
        if (have >= (int)sizeof buf - 1) have = 0;
    }
}
static void draw_toasts(void) {
    for (int i = 0; i < ntoast; i++) {
        int x = W - TOAST_W - 12, y = toast_y(ntoast - 1 - i);
        fill_round(x + 2, y + 3, TOAST_W, TOAST_H, 8, RGB(0, 0, 0), 60);
        uint32_t bg = set_style ? TH()->menu : RGB(0x1e, 0x2c, 0x3e), fg = set_style ? TH()->text : RGB(0xf2, 0xf6, 0xfa),
                 sub = set_style ? TH()->sub : RGB(0xb4, 0xc4, 0xd6);
        if (set_style) { fill_round(x, y, TOAST_W, TOAST_H, 8, bg, 255); stroke_round(x, y, TOAST_W, TOAST_H, 8, TH()->menu_line, 255); }
        else { fill_round_grad(x, y, TOAST_W, TOAST_H, 8, RGB(0x2c, 0x3c, 0x52), RGB(0x14, 0x1e, 0x2c), 240); stroke_round(x, y, TOAST_W, TOAST_H, 8, RGB(0x6a, 0x84, 0xa8), 200); }
        draw_icon(toasts[i].icon ? toasts[i].icon : ICON_APP, x + 30, y + TOAST_H / 2, 14);
        int maxc = (TOAST_W - 66) / 8;
        char b[200]; snprintf(b, sizeof b, "%.*s", maxc, toasts[i].title);
        draw_text_bold(x + 56, y + 12, b, fg);
        /* the message: up to two lines */
        const char *s = toasts[i].body; int line = 0;
        while (*s && line < 2) {
            int L = (int)strlen(s), n = imin(L, maxc);
            if (n < L) { int k = n; while (k > 0 && s[k] != ' ') k--; if (k > 0) n = k; }
            if (line == 1 && n < L) { snprintf(b, sizeof b, "%.*s...", imax(0, maxc - 3), s); n = L; }
            else snprintf(b, sizeof b, "%.*s", n, s);
            draw_text(x + 56, y + 32 + line * 17, b, sub, 255);
            s += n; while (*s == ' ') s++;
            line++;
        }
        (void)bg;
    }
}
static int toast_wait(void) {                 /* ms until the next one goes away, or -1 */
    if (!ntoast) return -1;
    long long now = ui_ms(); int gone = 0;
    for (int i = 0; i < ntoast; ) {
        if (toasts[i].until <= now) { memmove(toasts + i, toasts + i + 1, sizeof toasts[0] * (size_t)(ntoast - i - 1)); ntoast--; gone = 1; }
        else i++;
    }
    if (gone) repaint_toasts();
    if (!ntoast) return -1;
    long long m = toasts[0].until;
    for (int i = 1; i < ntoast; i++) if (toasts[i].until < m) m = toasts[i].until;
    return (int)(m - now);
}
static int toast_at(int x, int y) {           /* clicking one closes it */
    for (int i = 0; i < ntoast; i++) {
        int tx = W - TOAST_W - 12, ty = toast_y(ntoast - 1 - i);
        if (x >= tx && x < tx + TOAST_W && y >= ty && y < ty + TOAST_H) return i;
    }
    return -1;
}

/* ---- volume: the sound card's master volume (kernel OSS mixer) -------- */
static void volume_apply(void) {
    if (access("/run/user/0/pulse/native", F_OK) == 0) {   /* the sound server is running: ask it */
        static pid_t busy;
        if (busy > 0 && kill(busy, 0) == 0) kill(-busy, SIGKILL);       /* still busy with the last press */
        char cmd[160];
        snprintf(cmd, sizeof cmd, "pactl set-sink-volume @DEFAULT_SINK@ %d%%; pactl set-sink-mute @DEFAULT_SINK@ %d",
                 set_volume, set_muted);
        busy = fork();
        if (busy == 0) {
            setpgid(0, 0);
            setenv("PULSE_SERVER", "unix:/run/user/0/pulse/native", 1);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(0);
        }
        return;
    }
    int fd = open("/dev/mixer", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    int v = set_muted ? 0 : set_volume;
    v = v | v << 8;
    ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &v);
    int pcm = 100 | 100 << 8;                       /* keep the PCM slider open */
    ioctl(fd, SOUND_MIXER_WRITE_PCM, &pcm);
    close(fd);
}
static void st_repaint(void);
static void wl_keymap_changed(void);
static int gpu_available(void);
static void dmabuf_enable(int on);
static int win_of(void (*draw)(int, int, int, int));
static void settings_draw(int cx, int cy, int cw, int ch);
static void volume_set(int v, int muted, int popup) {
    set_volume = clampi(v, 0, 100); set_muted = muted;
    volume_apply();
    save_settings();
    if (popup) osd_show(OSD_VOL);
    if (win_of(settings_draw) >= 0) st_repaint();
}

static void set_menu(int open);
static void pop_close(void);
static void repaint_taskbar(void);
static void shutdown_now(void);
#include "power.c"                /* battery, brightness, lid, sleep */
static void repaint_clock(void);
#include "timezone.c"             /* time zones, internet time, the calendar panel */

/* ---- Caps Lock: remember it, and light the key's lamp on keyboards ---- */
static void caps_leds(void);
static void wl_set_capslock(int on);
static int capslock_state;
int capslock_on(void) { return capslock_state; }

/* switch between Classic and Modern (and light / dark): windows keep their content size */
static void style_set(int style, int dark) {
    if (style == set_style && dark == set_dark) return;
    int oldf = FRAME;
    set_style = style; set_dark = dark; set_smooth = style;
    int d = FRAME - oldf;
    for (int i = 0; i < MAXWIN; i++) {
        Win *w = &wins[i];
        if (!w->open) continue;
        if (!w->maximized && !w->fullscreen) { w->w += 2 * d; w->h += d; }
        else if (w->maximized) { w->rw += 2 * d; w->rh += d; }
        if (w->fullscreen) { w->x = -FRAME; w->w = W + 2 * FRAME; w->h = H + TITLE_H + FRAME; w->fw += 2 * d; w->fh += d; }
    }
    save_settings();
    repaint_all();
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].resized) wins[i].resized();
}
#include "bt_ui.c"                /* Settings > Bluetooth */
#include "update_ui.c"            /* Settings > Update */
static void settings_draw(int cx, int cy, int cw, int ch) {
    uint32_t text = RGB(0x20, 0x20, 0x20), grey = RGB(0x70, 0x70, 0x70);
    /* tab strip and the page under it */
    fill_rect(cx, cy, cw, ST_Y, RGB(0xe9, 0xee, 0xf4), 255);
    int px = cx + 8, py = cy + ST_Y - 1, pw = cw - 16, ph = ch - ST_Y - 36;
    fill_rect(px, py, pw, ph, RGB(255, 255, 255), 255);
    stroke_round(px, py, pw, ph, 2, RGB(0xa8, 0xb4, 0xc4), 255);
    for (int i = 0; i < NTABS; i++) {
        int tx, tw; st_tab_rect(i, &tx, &tw);
        int on = i == st_tab, ty = cy + ST_TAB_Y + (on ? 0 : 3), th = ST_TAB_H - (on ? 0 : 3) + (on ? 2 : 0);
        if (on) fill_round(cx + tx, ty, tw, th, 4, RGB(255, 255, 255), 255);
        else fill_round_grad(cx + tx, ty, tw, th, 4, RGB(0xf6, 0xf8, 0xfb), RGB(0xdc, 0xe2, 0xea), 255);
        stroke_round(cx + tx, ty, tw, th + 4, 4, RGB(0xa8, 0xb4, 0xc4), 255);
        if (on) { fill_rect(cx + tx + 1, cy + ST_Y - 1, tw - 2, 4, RGB(255, 255, 255), 255);
                  fill_rect(cx + tx + 3, ty + 1, tw - 6, 2, RGB(0x33, 0x99, 0xff), 255); }
        draw_text(cx + tx + 9, ty + (on ? 8 : 6), st_tabs[i], on ? RGB(0x1e, 0x39, 0x87) : text, 255);
    }
    fill_rect(px + 1, py + ph - 1, pw - 2, 1, RGB(0xa8, 0xb4, 0xc4), 255);

    int y = cy + ST_Y;
    switch (st_tab) {
    case TAB_LOOK: {
        st_heading(cx, y + 20, "Background color");
        for (int i = 0; i < NWALL; i++) {
            int x = cx + 24 + i * (SW_SIZE + SW_GAP), sy = cy + LK_SW_Y;
            if (i == set_wall) {
                fill_round(x - 4, sy - 4, SW_SIZE + 8, SW_SIZE + 8, 6, RGB(0x33, 0x99, 0xff), 255);
                fill_round(x - 2, sy - 2, SW_SIZE + 4, SW_SIZE + 4, 5, RGB(255, 255, 255), 255);
            }
            dk_begin(x, sy, SW_SIZE, SW_SIZE);
            fill_round_grad(x, sy, SW_SIZE, SW_SIZE, 4, mix(wallpapers[i].top, wallpapers[i].glow, 0.35f), wallpapers[i].bottom, 255);
            stroke_round(x, sy, SW_SIZE, SW_SIZE, 4, RGB(0, 0, 0), 70);
            dk_end();
        }
        draw_text(cx + 24, cy + LK_SW_Y + SW_SIZE + 10, wallpapers[set_wall].name, grey, 255);
        st_line(cx, cw, cy + LK_PIC_Y - 24);
        st_heading(cx, cy + LK_PIC_Y - 12, "Background picture");
        {
            int px = cx + 24, py = cy + LK_PIC_Y + 14, pw = 144, ph = pw * H / W;
            dark_keep(px - 1, py - 1, pw + 2, ph + 2);
            /* a little preview of the real desktop background */
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++) blend(px + x, py + y, wall[(y * H / ph) * W + x * W / pw], 255);
            stroke_round(px - 1, py - 1, pw + 2, ph + 2, 2, RGB(0x70, 0x78, 0x84), 255);
            int tx = px + pw + 16;
            if (set_wall_img[0]) {
                char nm[64]; snprintf(nm, sizeof nm, "%s", wimg.px ? "Your picture" : "The picture couldn't be loaded");
                draw_text(tx, py + 2, nm, text, 255);
                char sz[48]; if (wimg.px) snprintf(sz, sizeof sz, "%d x %d", wimg.w, wimg.h); else sz[0] = 0;
                draw_text(tx, py + 20, sz, grey, 255);
                st_choice(tx, py + 48, 150, 30, "Remove picture", 0);
                draw_text(cx + 24, cy + LK_PIC_Y + 118, "Picture position", text, 255);
                for (int i = 0; i < NWF; i++)
                    st_choice(cx + 24 + i * (LK_POS_W + 8), cy + LK_PIC_Y + 140, LK_POS_W, 30, wf_names[i], i == set_wall_fit);
            } else {
                draw_text(tx, py + 2, "No picture: just the color.", text, 255);
                draw_text(tx, py + 26, "To use one, right-click a picture", grey, 255);
                draw_text(tx, py + 44, "in File Explorer and choose", grey, 255);
                draw_text(tx, py + 62, "\"Set as desktop background\".", grey, 255);
            }
        }
        st_line(cx, cw, cy + LK_FONT_Y - 24);
        st_heading(cx, cy + LK_FONT_Y - 12, "Style");
        st_choice(cx + 24, cy + LK_FONT_Y + 12, 150, 30, "Classic", set_style == 0);
        st_choice(cx + 24 + 162, cy + LK_FONT_Y + 12, 150, 30, "Modern", set_style == 1);
        draw_text(cx + 24, cy + LK_FONT_Y + 50, set_style ? "Flat windows, smooth text and icons" : "Glass windows, blocky pixel text", grey, 255);
        if (set_style) {
            draw_text(cx + 24, cy + LK_FONT_Y + 80, "Mode", text, 255);
            st_choice(cx + 24, cy + LK_FONT_Y + 100, 150, 30, "Light", !set_dark);
            st_choice(cx + 24 + 162, cy + LK_FONT_Y + 100, 150, 30, "Dark", set_dark);
        }
        break;
    }
    case TAB_CLOCK: {
        st_heading(cx, y + 20, "Clock");
        draw_text(cx + 24, y + 54, "24-hour (military) time", text, 255);
        draw_text(cx + 24, y + 72, set_clock24 ? "Like 18:30" : "Like 6:30 PM", grey, 255);
        st_switch(cx + cw - 24 - TG_W, y + 52, set_clock24);
        st_line(cx, cw, y + 104);
        tz_load();
        st_heading(cx, y + 118, "Time zone");
        {
            char zn[64], off[16], line[120], tb[32]; tz_pretty(set_tz, zn, sizeof zn); tz_off_str(tz_now_off(), off, sizeof off);
            time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
            strftime(tb, sizeof tb, set_clock24 ? "%H:%M" : "%I:%M %p", &tm);
            draw_text_bold(cx + 24, y + 146, zn, text);
            snprintf(line, sizeof line, "%s, it's %s there now", off, tb);
            draw_text(cx + 24, y + 164, line, grey, 255);
        }
        draw_text(cx + 24, y + 196, "Set the time zone automatically", text, 255);
        draw_text(cx + 24, y + 214, "From where your internet connection is", grey, 255);
        st_switch(cx + cw - 24 - TG_W, y + 194, set_tz_auto);
        {   /* search, and the list */
            int fx = cx + 24, fw = cw - 48, fy = y + 240;
            fill_round(fx, fy, fw, 28, 3, RGB(255, 255, 255), 255);
            stroke_round(fx, fy, fw, 28, 3, tz_search_focus ? RGB(0x33, 0x99, 0xff) : RGB(0xa8, 0xb4, 0xc4), 255);
            char sb[64]; snprintf(sb, sizeof sb, "%s%s", tz_search, tz_search_focus ? "_" : "");
            draw_text(fx + 8, fy + 6, tz_search[0] || tz_search_focus ? sb : "Search: a city or country, like Warsaw or Poland",
                      tz_search[0] || tz_search_focus ? text : RGB(0xa0, 0xa8, 0xb0), 255);
            static int idx[520]; int n = tz_filtered(idx, 520);
            tz_scroll = clampi(tz_scroll, 0, imax(0, n - TZ_ROWS));
            fill_rect(fx, y + TZ_LIST_Y - 2, fw, TZ_ROWS * TZ_ROW + 4, RGB(255, 255, 255), 255);
            stroke_round(fx, y + TZ_LIST_Y - 2, fw, TZ_ROWS * TZ_ROW + 4, 0, RGB(0xd0, 0xd8, 0xe2), 255);
            if (!n) draw_text(fx + 10, y + TZ_LIST_Y + 6, "No time zone matches that.", grey, 255);
            for (int r = 0; r < TZ_ROWS && tz_scroll + r < n; r++) {
                const TzEnt *e = &tzl[idx[tz_scroll + r]];
                int ry = y + TZ_LIST_Y + r * TZ_ROW, on = !strcmp(e->zone, set_tz[0] ? set_tz : "UTC");
                if (on) fill_rect(fx + 1, ry, fw - 2, TZ_ROW, RGB(0xcc, 0xe8, 0xff), 255);
                char nm[80], off[16]; snprintf(nm, sizeof nm, "%.22s, %.22s", e->city, e->country);
                if (!strcmp(e->zone, "UTC")) snprintf(nm, sizeof nm, "UTC (no time zone)");
                tz_off_str(e->off, off, sizeof off);
                draw_text(fx + 10, ry + 4, nm, text, 255);
                draw_text(fx + fw - 18 - text_w(off), ry + 4, off, grey, 255);
            }
            if (n > TZ_ROWS) {                                   /* where in the list we are */
                int th = imax(16, TZ_ROWS * TZ_ROW * TZ_ROWS / n), ty = (TZ_ROWS * TZ_ROW - th) * tz_scroll / imax(1, n - TZ_ROWS);
                fill_round(fx + fw - 7, y + TZ_LIST_Y + ty, 4, th, 2, RGB(0xb8, 0xc0, 0xcc), 255);
            }
        }
        st_line(cx, cw, y + 406);
        st_heading(cx, y + 418, "Internet time");
        { char s[96]; tz_sync_text(s, sizeof s); draw_text(cx + 24, y + 444, s, grey, 255); }
        st_choice(cx + cw - 24 - 120, y + 436, 120, 28, "Sync now", 0);
        draw_text(cx + 24, y + 472, "Windows is on this computer too", text, 255);
        draw_text(cx + 24, y + 490, "Keeps the clock right in both", grey, 255);
        st_switch(cx + cw - 24 - TG_W, y + 470, set_rtc_local);
        break;
    }
    case TAB_SOUND:
        st_heading(cx, y + 20, "Sound");
        draw_text(cx + 24, y + 54, "Play startup sound", text, 255);
        draw_text(cx + 24, y + 72, "The tune when the desktop fades in", grey, 255);
        st_switch(cx + cw - 24 - TG_W, y + 52, set_sound);
        st_line(cx, cw, y + 104);
        st_heading(cx, y + 118, "Volume");
        {
            int v = set_muted ? 0 : set_volume;
            int bx = cx + VOL_X, bw = cw - VOL_X - VOL_R, by = y + VOL_Y + 9;
            draw_speaker(cx + 22, y + VOL_Y, RGB(0x30, 0x40, 0x58), set_muted || set_volume == 0, v == 0 ? 0 : v < 34 ? 1 : v < 67 ? 2 : 3);
            fill_round(bx, by, bw, 6, 3, RGB(0xd0, 0xd8, 0xe2), 255);
            if (v) fill_round(bx, by, imax(6, bw * v / 100), 6, 3, set_muted ? RGB(0xa0, 0xa8, 0xb0) : RGB(0x2c, 0x78, 0xd8), 255);
            fill_circle(bx + bw * v / 100.0f, by + 3, 8, RGB(0xff, 0xff, 0xff), 255);
            stroke_round(bx + bw * v / 100 - 8, by - 5, 17, 17, 8, RGB(0x2c, 0x78, 0xd8), 255);
            char s[8]; snprintf(s, sizeof s, "%d", v);
            draw_text_bold(cx + cw - VOL_R + 16, y + VOL_Y + 4, s, text);
            st_line(cx, cw, y + VOL_Y + 62);
            draw_text(cx + 24, y + VOL_Y + 78, "Notification sound", text, 255);
            draw_text(cx + 24, y + VOL_Y + 96, "A soft chime when a notification pops up", grey, 255);
            st_switch(cx + cw - 24 - TG_W, y + VOL_Y + 76, set_notify_sound);
            draw_text(cx + 24, y + VOL_Y + 34, set_muted ? "Muted - click the speaker to unmute" : "Click the speaker to mute.  Keyboard volume keys work too.", grey, 255);
        }
        break;
    case TAB_MOUSE: {
        static const char *cols[3] = { "White", "Black", "Blue" };
        static const char *sizes[3] = { "Normal", "Large", "Extra large" };
        static const char *spds[3] = { "Slow", "Normal", "Fast" };
        st_heading(cx, y + 20, "Pointer color");
        for (int i = 0; i < 3; i++) {
            int bx = cx + 24 + i * (MS_COL_W + 12), by = cy + MS_COL_Y;
            st_choice(bx, by, MS_COL_W, MS_COL_H, cols[i], i == set_cursor_col);
            /* a little preview: arrow and a resize arrow, on a grey patch so white and black both show */
            fill_round(bx + 16, by + 8, MS_COL_W - 32, 40, 4, RGB(0x8c, 0x98, 0xa8), 255);
            render_cursor(CUR_ARROW, i, cursor_scales[set_cursor_size] * (set_cursor_size == 2 ? 0.8f : 1), bx + 42, by + 12, put_back);
            render_cursor(CUR_NWSE, i, 1.0f, bx + 96, by + 28, put_back);
        }
        st_heading(cx, cy + MS_SIZE_Y - 24, "Pointer size");
        for (int i = 0; i < 3; i++)
            st_choice(cx + 24 + i * (MS_BTN_W + 12), cy + MS_SIZE_Y, MS_BTN_W, MS_BTN_H, sizes[i], i == set_cursor_size);
        st_line(cx, cw, cy + MS_SPD_Y - 34);
        st_heading(cx, cy + MS_SPD_Y - 24, "Touchpad speed (laptops)");
        for (int i = 0; i < 3; i++)
            st_choice(cx + 24 + i * (MS_BTN_W + 12), cy + MS_SPD_Y, MS_BTN_W, MS_BTN_H, spds[i], i == set_tp_speed);
        st_line(cx, cw, cy + MS_KBD_Y - 34);
        st_heading(cx, cy + MS_KBD_Y - 24, "Keyboard layout");
        st_choice(cx + 24, cy + MS_KBD_Y, 230, MS_BTN_H, "Polish (programmer's)", set_kbd == KBD_PL);
        st_choice(cx + 24 + 242, cy + MS_KBD_Y, 230, MS_BTN_H, "English (US)", set_kbd == KBD_US);
        if (set_kbd == KBD_PL) {
            draw_text(cx + 24, cy + MS_KBD_Y + 42, "Hold the right Alt key (Alt Gr) and press a c e l n o s x z", grey, 255);
            draw_text(cx + 24, cy + MS_KBD_Y + 60, "for \xc4\x85 \xc4\x87 \xc4\x99 \xc5\x82 \xc5\x84 \xc3\xb3 \xc5\x9b \xc5\xba \xc5\xbc  (with Shift: capitals)", grey, 255);
        } else draw_text(cx + 24, cy + MS_KBD_Y + 42, "A plain US keyboard: the right Alt key is just Alt.", grey, 255);
        break;
    }
    case TAB_POWER: {
        st_heading(cx, y + 20, "Battery");
        char b[96]; bat_text(b, sizeof b);
        if (bat.present) { draw_battery(cx + 24, y + 51, bat.pct, bat.charging, text); draw_text(cx + 58, y + 48, b, text, 255); }
        else draw_text(cx + 24, y + 48, b, grey, 255);
        st_line(cx, cw, y + 80);
        st_heading(cx, y + 92, "Screen brightness");
        if (bl_dir[0]) {
            int v = set_bright > 0 ? set_bright : 100;
            int bx = cx + VOL_X, bw = cw - VOL_X - VOL_R, by = y + PW_BR_Y + 9;
            draw_sun_icon(cx + 34, y + PW_BR_Y + 12, 11);
            fill_round(bx, by, bw, 6, 3, RGB(0xd0, 0xd8, 0xe2), 255);
            fill_round(bx, by, imax(6, bw * v / 100), 6, 3, RGB(0x2c, 0x78, 0xd8), 255);
            fill_circle(bx + bw * v / 100.0f, by + 3, 8, RGB(0xff, 0xff, 0xff), 255);
            stroke_round(bx + bw * v / 100 - 8, by - 5, 17, 17, 8, RGB(0x2c, 0x78, 0xd8), 255);
            char s[8]; snprintf(s, sizeof s, "%d", v);
            draw_text_bold(cx + cw - VOL_R + 16, y + PW_BR_Y + 4, s, text);
        } else draw_text(cx + 24, y + PW_BR_Y + 4, "Set it with the buttons on your monitor.", grey, 255);
        st_line(cx, cw, y + 164);
        st_heading(cx, y + 176, "Turn off the screen after");
        for (int i = 0; i < NOFF; i++) st_choice(cx + 24 + i * (PW_CW + 8), y + PW_OFF_Y, PW_CW, 30, off_names[i], i == set_off_idx);
        st_heading(cx, y + PW_SLP_Y - 24, "Go to sleep after");
        for (int i = 0; i < NSLEEPT; i++) st_choice(cx + 24 + i * (PW_CW + 8), y + PW_SLP_Y, PW_CW, 30, sleep_names[i], i == set_sleep_idx);
        st_heading(cx, y + PW_LID_Y - 24, "When I close the lid");
        st_choice(cx + 24, y + PW_LID_Y, 150, 30, "Sleep", set_lid == 0);
        st_choice(cx + 24 + 158, y + PW_LID_Y, 150, 30, "Do nothing", set_lid == 1);
        st_line(cx, cw, y + PW_NOW_Y - 14);
        st_choice(cx + 24, y + PW_NOW_Y, 150, 30, "Sleep now", 0);
        draw_text(cx + 186, y + PW_NOW_Y + 7, "Wake it with the power button or a key.", grey, 255);
        break;
    }
    case TAB_BT: bt_draw(cx, cy, cw, text, grey); break;
    case TAB_UPDATE: update_draw(cx, cy, cw, text, grey); break;
    case TAB_ABOUT: {
        draw_text_scaled(cx + 24, y + 22, "/o\\", RGB(0x2e, 0x7c, 0xe0), 2);
        { char vb[48]; FILE *vf = fopen("/etc/ccos-version", "r"); char vv[24] = "";
          if (vf) { if (fgets(vv, sizeof vv, vf)) vv[strcspn(vv, "\r\n")] = 0; fclose(vf); }
          snprintf(vb, sizeof vb, "ccos %s", vv); draw_text_bold(cx + 86, y + 22, vb, RGB(0x1e, 0x39, 0x87)); }
        draw_text(cx + 86, y + 40, "your own operating system", grey, 255);
        st_line(cx, cw, y + 70);
        char b[96]; struct utsname un; uname(&un);
        snprintf(b, sizeof b, "Linux %s + BusyBox", un.release);
        draw_text(cx + 24, y + 84, "System", grey, 255); draw_text(cx + 150, y + 84, b, text, 255);
        long tot = 0, avail = 0; FILE *f = fopen("/proc/meminfo", "r");
        if (f) { char l[128]; while (fgets(l, sizeof l, f)) { sscanf(l, "MemTotal: %ld", &tot); sscanf(l, "MemAvailable: %ld", &avail); } fclose(f); }
        snprintf(b, sizeof b, "%ld MB (%ld MB free)", tot / 1024, avail / 1024);
        draw_text(cx + 24, y + 104, "Memory", grey, 255); draw_text(cx + 150, y + 104, b, text, 255);
        snprintf(b, sizeof b, "%d x %d", W, H);
        draw_text(cx + 24, y + 124, "Screen", grey, 255); draw_text(cx + 150, y + 124, b, text, 255);
        check_persistent();
        draw_text(cx + 24, y + 144, "Save disk", grey, 255);
        draw_text(cx + 150, y + 144, conf_persistent ? "Yes: settings and files are kept" : "None: changes reset on restart", text, 255);
        draw_text(cx + 24, y + 164, "Apps", grey, 255);
        draw_text(cx + 150, y + 164, "Wayland apps welcome (Terminal: foot)", text, 255);
        st_line(cx, cw, y + 196);
        st_heading(cx, y + 208, "Graphics chip");
        if (gpu_available()) {
            draw_text(cx + 24, y + 240, "Let apps draw with the graphics chip", text, 255);
            draw_text(cx + 24, y + 258, "Experimental. Faster games and video; restart apps after changing", grey, 255);
            st_switch(cx + cw - 24 - TG_W, y + 238, set_gpu);
        } else draw_text(cx + 24, y + 240, "No graphics chip that apps can use was found.", grey, 255);
        break;
    }
    }

    check_persistent();                          /* the USB stick may have been found since */
    draw_text(cx + 16, cy + ch - 26,
              conf_persistent ? "Changes are saved automatically."
                              : "No save disk: changes reset on restart.", grey, 255);
}

static void repaint_all(void);
static void repaint_clock(void);
static void repaint_window(int idx);

static void st_repaint(void) {
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].draw == settings_draw) repaint_window(i);
}

static int st_vol_drag, st_br_drag;
static void settings_key(int code, int mods) {     /* typing in the time zone search */
    if (st_tab != TAB_CLOCK || !tz_search_focus) return;
    size_t l = strlen(tz_search);
    if (code == KEY_ESC) { tz_search_focus = 0; }
    else if (code == KEY_BACKSPACE) { if (l) tz_search[l - 1] = 0; }
    else if (code == KEY_ENTER || code == KEY_KPENTER) {
        static int idx[520]; if (tz_filtered(idx, 520) == 1) tz_set(tzl[idx[0]].zone, 1);
        tz_search_focus = 0;
    } else {
        char c = key_char(code, mods);
        if (c >= 32 && c < 127 && l + 1 < sizeof tz_search) { tz_search[l] = c; tz_search[l + 1] = 0; }
    }
    tz_scroll = 0;
    st_repaint();
}
static void settings_wheel(int amount) {
    if (st_tab != TAB_CLOCK) return;
    tz_scroll += amount > 0 ? -2 : 2;
    if (tz_scroll < 0) tz_scroll = 0;
    st_repaint();
}
static void settings_drag(int lx, int ly, int cw, int ch) {      /* moving the volume slider */
    (void)ly; (void)ch;
    if (st_br_drag && st_tab == TAB_POWER) {
        int v = clampi((lx - VOL_X) * 100 / (cw - VOL_X - VOL_R), 5, 100);
        if (v != set_bright) bright_set(v, 0);
        return;
    }
    if (!st_vol_drag || st_tab != TAB_SOUND) return;
    int bw = cw - VOL_X - VOL_R;
    int v = clampi((lx - VOL_X) * 100 / bw, 0, 100);
    if (v != set_volume || set_muted) volume_set(v, 0, 0);
}
static void settings_click(int lx, int ly, int cw, int ch) {
    st_vol_drag = st_br_drag = 0;
    (void)ch;
    for (int i = 0; i < NTABS; i++) {                     /* tabs */
        int tx, tw; st_tab_rect(i, &tx, &tw);
        if (lx >= tx && lx < tx + tw && ly >= ST_TAB_Y && ly < ST_Y + 2) {
            if (i != st_tab) { st_tab = i; if (i == TAB_BT) { bt_last_status = 0; settings_tick(); } st_repaint(); }
            return;
        }
    }
    int sw_x = cw - 24 - TG_W - 40;                       /* on/off switches: the switch or its label */
    int on_row = (lx >= sw_x || lx < 24 + 26 * 8);
    switch (st_tab) {
    case TAB_LOOK:
        for (int i = 0; i < NWALL; i++) {
            int x = 24 + i * (SW_SIZE + SW_GAP);
            if (lx >= x && lx < x + SW_SIZE && ly >= LK_SW_Y && ly < LK_SW_Y + SW_SIZE) {
                if (i != set_wall) { set_wall = i; save_settings(); make_wallpaper(); repaint_all(); }
                return;
            }
        }
        if (set_wall_img[0]) {
            int tx = 24 + 144 + 16, py = LK_PIC_Y + 14;
            if (lx >= tx && lx < tx + 150 && ly >= py + 48 && ly < py + 78) {        /* Remove picture */
                unlink(set_wall_img); set_wall_img[0] = 0; wimg.path[0] = 0;
                save_settings(); make_wallpaper(); repaint_all(); return;
            }
            for (int i = 0; i < NWF; i++) {
                int x = 24 + i * (LK_POS_W + 8);
                if (lx >= x && lx < x + LK_POS_W && ly >= LK_PIC_Y + 140 && ly < LK_PIC_Y + 170) {
                    if (i != set_wall_fit) { set_wall_fit = i; save_settings(); make_wallpaper(); repaint_all(); }
                    return;
                }
            }
        }
        if (ly >= LK_FONT_Y + 12 && ly < LK_FONT_Y + 42) {
            if (lx >= 24 && lx < 174) style_set(0, set_dark);
            else if (lx >= 186 && lx < 336) style_set(1, set_dark);
        } else if (set_style && ly >= LK_FONT_Y + 100 && ly < LK_FONT_Y + 130) {
            if (lx >= 24 && lx < 174) style_set(1, 0);
            else if (lx >= 186 && lx < 336) style_set(1, 1);
        }
        return;
    case TAB_CLOCK:
        tz_search_focus = 0;
        if (ly >= ST_Y + 44 && ly < ST_Y + 90 && on_row) {
            set_clock24 = !set_clock24; save_settings(); repaint_clock(); clkp_repaint(); st_repaint();
        } else if (ly >= ST_Y + 186 && ly < ST_Y + 230 && on_row) {          /* automatic */
            set_tz_auto = !set_tz_auto; save_settings();
            if (set_tz_auto) { tz_auto_done = 0; tz_auto_last_try = 0; }
            st_repaint();
        } else if (ly >= ST_Y + 240 && ly < ST_Y + 268 && lx >= 24 && lx < cw - 24) {
            tz_search_focus = 1; st_repaint();
        } else if (ly >= ST_Y + TZ_LIST_Y && ly < ST_Y + TZ_LIST_Y + TZ_ROWS * TZ_ROW && lx >= 24 && lx < cw - 24) {
            static int idx[520]; int n = tz_filtered(idx, 520), r = (ly - ST_Y - TZ_LIST_Y) / TZ_ROW + tz_scroll;
            if (r < n) tz_set(tzl[idx[r]].zone, 1);
            st_repaint();
        } else if (ly >= ST_Y + 436 && ly < ST_Y + 464 && lx >= cw - 24 - 120 && lx < cw - 24) {
            tz_sync_now(); st_repaint();
        } else if (ly >= ST_Y + 464 && ly < ST_Y + 508 && on_row) {
            set_rtc_local = !set_rtc_local; save_settings(); st_repaint();
        } else st_repaint();
        return;
    case TAB_SOUND:
        if (ly >= ST_Y + VOL_Y - 6 && ly < ST_Y + VOL_Y + 30) {
            if (lx >= 16 && lx < 52) { volume_set(set_volume, !set_muted, 0); return; }
            if (lx >= VOL_X - 10 && lx < cw - VOL_R + 10) { st_vol_drag = 1; settings_drag(lx, ly, cw, ch); return; }
        }
        if (ly >= ST_Y + VOL_Y + 70 && ly < ST_Y + VOL_Y + 116 && on_row) {
            set_notify_sound = !set_notify_sound; save_settings(); st_repaint();
            if (set_notify_sound && fork() == 0) {         /* hear it */
                execl("/bin/playsound", "playsound", "/usr/share/sounds/notify.wav", (char *)NULL);
                _exit(0);
            }
            return;
        }
        if (ly >= ST_Y + 44 && ly < ST_Y + 90 && on_row) {
            set_sound = !set_sound; save_settings(); st_repaint();
            if (set_sound && fork() == 0) {               /* a little preview */
                execl("/bin/playsound", "playsound", "/usr/share/sounds/startup.wav", (char *)NULL);
                _exit(0);
            }
        }
        return;
    case TAB_BT: bt_click(lx, ly, cw); return;
    case TAB_UPDATE: update_click(lx, ly, cw); return;
    case TAB_ABOUT:
        if (ly >= ST_Y + 230 && ly < ST_Y + 276 && on_row && gpu_available()) {
            set_gpu = !set_gpu; save_settings(); dmabuf_enable(set_gpu); st_repaint();
        }
        return;
    case TAB_POWER:
        if (bl_dir[0] && ly >= ST_Y + PW_BR_Y - 6 && ly < ST_Y + PW_BR_Y + 30 && lx >= VOL_X - 10 && lx < cw - VOL_R + 10) {
            st_br_drag = 1; settings_drag(lx, ly, cw, ch); return;
        }
        for (int i = 0; i < NOFF; i++) {
            int x = 24 + i * (PW_CW + 8);
            if (lx >= x && lx < x + PW_CW && ly >= ST_Y + PW_OFF_Y && ly < ST_Y + PW_OFF_Y + 30) { set_off_idx = i; save_settings(); st_repaint(); return; }
            if (i < NSLEEPT && lx >= x && lx < x + PW_CW && ly >= ST_Y + PW_SLP_Y && ly < ST_Y + PW_SLP_Y + 30) { set_sleep_idx = i; save_settings(); st_repaint(); return; }
        }
        if (ly >= ST_Y + PW_LID_Y && ly < ST_Y + PW_LID_Y + 30) {
            if (lx >= 24 && lx < 174) set_lid = 0; else if (lx >= 182 && lx < 332) set_lid = 1; else return;
            save_settings(); st_repaint(); return;
        }
        if (ly >= ST_Y + PW_NOW_Y && ly < ST_Y + PW_NOW_Y + 30 && lx >= 24 && lx < 174) power_sleep();
        return;
    case TAB_MOUSE:
        for (int i = 0; i < 3; i++) {
            int bx = 24 + i * (MS_COL_W + 12), sx = 24 + i * (MS_BTN_W + 12);
            if (lx >= bx && lx < bx + MS_COL_W && ly >= MS_COL_Y && ly < MS_COL_Y + MS_COL_H) {
                set_cursor_col = i; save_settings(); st_repaint(); return;
            }
            if (lx >= sx && lx < sx + MS_BTN_W && ly >= MS_SIZE_Y && ly < MS_SIZE_Y + MS_BTN_H) {
                set_cursor_size = i; save_settings(); st_repaint(); return;
            }
            if (lx >= sx && lx < sx + MS_BTN_W && ly >= MS_SPD_Y && ly < MS_SPD_Y + MS_BTN_H) {
                set_tp_speed = i; save_settings(); st_repaint(); return;
            }
        }
        if (ly >= MS_KBD_Y && ly < MS_KBD_Y + MS_BTN_H && lx >= 24 && lx < 24 + 242 + 230) {
            int k = lx < 24 + 236 ? KBD_PL : KBD_US;
            if (k != set_kbd) { set_kbd = k; altgr = 0; save_settings(); wl_keymap_changed(); st_repaint(); }
        }
        return;
    }
}


/* ===================================================================
 *  CALCULATOR APP (works like the Windows 7 standard calculator)
 * =================================================================== */
#define CALC_PAD 12
#define CALC_BW  44                 /* button size */
#define CALC_BH  34
#define CALC_GAP 6
#define CALC_DH  58                 /* display height */
#define CALC_BY  (CALC_PAD + CALC_DH + 10)
#define CALC_CW  (2 * CALC_PAD + 5 * CALC_BW + 4 * CALC_GAP)
#define CALC_CH  (CALC_BY + 5 * CALC_BH + 4 * CALC_GAP + CALC_PAD)
#define CALC_DISPLAY 100            /* element id of the display */

/* label, key it acts like, column, row, column span, row span.
 * \x1b, \xf1 and \xfb are the font's arrow, plus-minus and root signs. */
typedef struct { const char *label; char op; int col, row, cs, rs; } CalcBtn;
static const CalcBtn calc_btns[] = {
    { "\x1b", '<', 0, 0, 1, 1 }, { "CE", 'E', 1, 0, 1, 1 }, { "C", 'C', 2, 0, 1, 1 },
    { "\xf1", 'N', 3, 0, 1, 1 }, { "\xfb", 'R', 4, 0, 1, 1 },
    { "7", '7', 0, 1, 1, 1 }, { "8", '8', 1, 1, 1, 1 }, { "9", '9', 2, 1, 1, 1 },
    { "/", '/', 3, 1, 1, 1 }, { "%", '%', 4, 1, 1, 1 },
    { "4", '4', 0, 2, 1, 1 }, { "5", '5', 1, 2, 1, 1 }, { "6", '6', 2, 2, 1, 1 },
    { "*", '*', 3, 2, 1, 1 }, { "1/x", 'I', 4, 2, 1, 1 },
    { "1", '1', 0, 3, 1, 1 }, { "2", '2', 1, 3, 1, 1 }, { "3", '3', 2, 3, 1, 1 },
    { "-", '-', 3, 3, 1, 1 }, { "=", '=', 4, 3, 1, 2 },
    { "0", '0', 0, 4, 2, 1 }, { ".", '.', 2, 4, 1, 1 }, { "+", '+', 3, 4, 1, 1 },
};
#define NCALC ((int)(sizeof calc_btns / sizeof *calc_btns))

/* calculator state */
static double c_acc, c_lastb;          /* running total, last operand (for repeated =) */
static char   c_op, c_lastop;          /* pending operator */
static char   c_entry[40] = "0";       /* what the display shows */
static char   c_expr[64];              /* small line above, e.g. "12 +" */
static int    c_fresh = 1;             /* next digit starts a new number */
static int    c_err;                   /* showing an error message */
static int    c_pressed = -1;          /* button held down with the mouse */

static void repaint(int x, int y, int w, int h);

static void calc_elem_rect(int id, int *x, int *y, int *w, int *h) {
    if (id == CALC_DISPLAY) { *x = CALC_PAD; *y = CALC_PAD; *w = CALC_CW - 2 * CALC_PAD; *h = CALC_DH; return; }
    const CalcBtn *b = &calc_btns[id];
    *x = CALC_PAD + b->col * (CALC_BW + CALC_GAP);
    *y = CALC_BY + b->row * (CALC_BH + CALC_GAP);
    *w = b->cs * CALC_BW + (b->cs - 1) * CALC_GAP;
    *h = b->rs * CALC_BH + (b->rs - 1) * CALC_GAP;
}

static int calc_hit(int lx, int ly, int cw, int ch) {
    for (int i = 0; i < NCALC; i++) {
        int x, y, w, h; calc_elem_rect(i, &x, &y, &w, &h);
        if (lx >= x && lx < x + w && ly >= y && ly < y + h) return i;
    }
    return -1;
}

static int calc_idx(void);
static void calc_repaint_elem(int id) {
    int i = calc_idx(); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    int x, y, w, h; calc_elem_rect(id, &x, &y, &w, &h);
    repaint(cx + x, cy + y, w, h);
}

static void calc_draw(int cx, int cy, int cw, int ch) {
    fill_rect(cx, cy, cw, ch, RGB(0xd9, 0xe4, 0xf1), 255);          /* Win7 calc blue-grey */
    /* display */
    int x, y, w, h; calc_elem_rect(CALC_DISPLAY, &x, &y, &w, &h);
    x += cx; y += cy;
    fill_round_grad(x, y, w, h, 3, RGB(0xf6, 0xfa, 0xfe), RGB(0xe2, 0xec, 0xf6), 255);
    stroke_round(x, y, w, h, 3, RGB(0x8e, 0x9b, 0xbc), 255);
    draw_text(x + w - 8 - text_w(c_expr), y + 4, c_expr, RGB(0x70, 0x78, 0x88), 255);
    int len = (int)strlen(c_entry);
    if (len * 16 <= w - 12)
        draw_text_scaled(x + w - 6 - len * 16, y + h - 36, c_entry, RGB(0x10, 0x10, 0x10), 2);
    else
        draw_text(x + w - 6 - len * 8, y + h - 24, c_entry, RGB(0x10, 0x10, 0x10), 255);
    /* buttons */
    for (int i = 0; i < NCALC; i++) {
        calc_elem_rect(i, &x, &y, &w, &h);
        x += cx; y += cy;
        int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == calc_draw && hot.b == i;
        uint32_t top = RGB(0xf6, 0xf8, 0xfb), bot = RGB(0xd8, 0xe0, 0xea), bd = RGB(0x8e, 0x9a, 0xab);
        if (i == c_pressed)  { top = RGB(0xc2, 0xe4, 0xf6); bot = RGB(0x8e, 0xcc, 0xee); bd = RGB(0x2c, 0x62, 0x8a); }
        else if (hv)         { top = RGB(0xee, 0xf8, 0xfe); bot = RGB(0xbe, 0xe6, 0xfd); bd = RGB(0x3c, 0x7f, 0xb1); }
        fill_round_grad(x, y, w, h, 3, top, bot, 255);
        fill_rect(x + 2, y + 1, w - 4, h / 2 - 1, RGB(255, 255, 255), i == c_pressed ? 20 : 70);
        stroke_round(x, y, w, h, 3, bd, 255);
        const char *lb = calc_btns[i].label;
        int tx = x + (w - text_w(lb)) / 2 + (i == c_pressed), ty = y + (h - 16) / 2 + (i == c_pressed);
        draw_text_bold(tx, ty, lb, RGB(0x1e, 0x1e, 0x1e));
    }
}

/* number -> text that fits the display (at most 14 characters) */
static void calc_fmt(double v, char *out, size_t n) {
    for (int p = 15; p >= 1; p--) {
        snprintf(out, n, "%.*g", p, v);
        if (strlen(out) <= 14) break;
    }
    if (!strcmp(out, "-0")) strcpy(out, "0");
}

static void calc_error(const char *msg) {
    c_err = 1; c_op = 0; c_lastop = 0; c_expr[0] = 0; c_fresh = 1;
    snprintf(c_entry, sizeof c_entry, "%s", msg);
}

static int calc_set(double v) {                   /* 0 if it overflowed */
    if (!isfinite(v)) { calc_error("Overflow"); return 0; }
    calc_fmt(v, c_entry, sizeof c_entry);
    return 1;
}

static int calc_apply(double a, char op, double b, double *r) {
    switch (op) {
    case '+': *r = a + b; return 1;
    case '-': *r = a - b; return 1;
    case '*': *r = a * b; return 1;
    case '/': if (b == 0) { calc_error("Cannot divide by zero"); return 0; } *r = a / b; return 1;
    }
    *r = b; return 1;
}

static void calc_do(char k) {
    if (c_err) {                                       /* any key clears an error */
        c_err = 0; c_acc = 0; c_op = 0; strcpy(c_entry, "0"); c_fresh = 1;
        if (k == 'C' || k == 'E' || k == '<') { calc_repaint_elem(CALC_DISPLAY); return; }
    }
    double v = atof(c_entry), r;
    if (k >= '0' && k <= '9') {
        if (c_fresh || !strcmp(c_entry, "0")) { c_entry[0] = k; c_entry[1] = 0; c_fresh = 0; }
        else {
            int digits = 0;
            for (char *p = c_entry; *p; p++) if (*p >= '0' && *p <= '9') digits++;
            if (digits < 14) { size_t l = strlen(c_entry); c_entry[l] = k; c_entry[l + 1] = 0; }
        }
    } else switch (k) {
    case '.':
        if (c_fresh) { strcpy(c_entry, "0."); c_fresh = 0; }
        else if (!strchr(c_entry, '.') && !strchr(c_entry, 'e')) strcat(c_entry, ".");
        break;
    case '<':
        if (!c_fresh) {
            c_entry[strlen(c_entry) - 1] = 0;
            if (!c_entry[0] || !strcmp(c_entry, "-")) strcpy(c_entry, "0");
        }
        break;
    case 'E': strcpy(c_entry, "0"); c_fresh = 1; break;
    case 'C': c_acc = 0; c_op = 0; c_lastop = 0; c_expr[0] = 0; strcpy(c_entry, "0"); c_fresh = 1; break;
    case 'N':
        if (strcmp(c_entry, "0")) {
            if (c_entry[0] == '-') memmove(c_entry, c_entry + 1, strlen(c_entry));
            else { memmove(c_entry + 1, c_entry, strlen(c_entry) + 1); c_entry[0] = '-'; }
        }
        break;
    case 'R': if (v < 0) calc_error("Invalid input"); else { calc_set(sqrt(v)); c_fresh = 1; } break;
    case 'I': if (v == 0) calc_error("Cannot divide by zero"); else { calc_set(1 / v); c_fresh = 1; } break;
    case '%': calc_set(c_op ? c_acc * v / 100 : 0); c_fresh = 1; break;
    case '+': case '-': case '*': case '/':
        if (c_op && !c_fresh) {                          /* 2 + 3 +  ->  shows 5 */
            if (!calc_apply(c_acc, c_op, v, &r) || !calc_set(r)) break;
            c_acc = r;
        } else if (!c_op) c_acc = v;
        c_op = k; c_fresh = 1;
        { char a[24]; calc_fmt(c_acc, a, sizeof a); snprintf(c_expr, sizeof c_expr, "%s %c", a, k); }
        break;
    case '=':
        if (c_op) {
            if (!calc_apply(c_acc, c_op, v, &r)) break;
            c_lastop = c_op; c_lastb = v;
        } else if (c_lastop) {                           /* pressing = again repeats */
            if (!calc_apply(v, c_lastop, c_lastb, &r)) break;
        } else break;
        if (calc_set(r)) c_acc = r;
        c_op = 0; c_fresh = 1; c_expr[0] = 0;
        break;
    }
    calc_repaint_elem(CALC_DISPLAY);
}

static void calc_click(int lx, int ly, int cw, int ch) {
    int id = calc_hit(lx, ly, cw, ch);
    if (id < 0) return;
    c_pressed = id;
    calc_repaint_elem(id);
    calc_do(calc_btns[id].op);
}

static void calc_release(void) {
    if (c_pressed < 0) return;
    int id = c_pressed; c_pressed = -1;
    calc_repaint_elem(id);
}

static void calc_key(int code, int mods) {
    int shift = mods & MOD_SHIFT;
    char k = 0;
    switch (code) {
    case KEY_1: k = '1'; break; case KEY_2: k = '2'; break; case KEY_3: k = '3'; break;
    case KEY_4: k = '4'; break; case KEY_5: k = shift ? '%' : '5'; break;
    case KEY_6: k = '6'; break; case KEY_7: k = '7'; break; case KEY_8: k = shift ? '*' : '8'; break;
    case KEY_9: k = '9'; break; case KEY_0: k = '0'; break;
    case KEY_KP1: k = '1'; break; case KEY_KP2: k = '2'; break; case KEY_KP3: k = '3'; break;
    case KEY_KP4: k = '4'; break; case KEY_KP5: k = '5'; break; case KEY_KP6: k = '6'; break;
    case KEY_KP7: k = '7'; break; case KEY_KP8: k = '8'; break; case KEY_KP9: k = '9'; break;
    case KEY_KP0: k = '0'; break;
    case KEY_KPPLUS: k = '+'; break;  case KEY_KPMINUS: case KEY_MINUS: k = '-'; break;
    case KEY_KPASTERISK: k = '*'; break; case KEY_KPSLASH: case KEY_SLASH: k = '/'; break;
    case KEY_EQUAL: k = shift ? '+' : '='; break;
    case KEY_ENTER: case KEY_KPENTER: k = '='; break;
    case KEY_DOT: case KEY_KPDOT: case KEY_COMMA: k = '.'; break;
    case KEY_BACKSPACE: k = '<'; break;
    case KEY_ESC: k = 'C'; break;
    case KEY_DELETE: k = 'E'; break;
    }
    if (k) calc_do(k);
}

static int calc_idx(void) {
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].draw == calc_draw) return i;
    return -1;
}

/* ---- dirty rectangles ----------------------------------------------- */
#define MAXRECT 32
static struct { int x0, y0, x1, y1; } dirty[MAXRECT];
static int ndirty;

static void mark(int x, int y, int w, int h) {
    int x0 = imax(x, 0), y0 = imax(y, 0), x1 = imin(x + w, W), y1 = imin(y + h, H);
    if (x0 >= x1 || y0 >= y1) return;
    if (ndirty == MAXRECT) {
        for (int i = 1; i < ndirty; i++) {
            dirty[0].x0 = imin(dirty[0].x0, dirty[i].x0); dirty[0].y0 = imin(dirty[0].y0, dirty[i].y0);
            dirty[0].x1 = imax(dirty[0].x1, dirty[i].x1); dirty[0].y1 = imax(dirty[0].y1, dirty[i].y1);
        }
        ndirty = 1;
    }
    dirty[ndirty].x0 = x0; dirty[ndirty].y0 = y0; dirty[ndirty].x1 = x1; dirty[ndirty].y1 = y1;
    ndirty++;
}

static void draw_desk_icons(void) {
    for (int i = 0; i < NDESK; i++) {
        int x, y, w, h; di_rect(i, &x, &y, &w, &h);
        if (x >= clip_x1 || x + w <= clip_x0 || y >= clip_y1 || y + h <= clip_y0) continue;
        int sel = i == di_sel, hv = hot.type == HOT_ICON && hot.a == i;
        if (sel || hv) {                                  /* Windows 7 style glassy highlight */
            fill_round_grad(x, y, w, h, 3, RGB(0xd8, 0xec, 0xff), RGB(0x9c, 0xcc, 0xf8), sel ? 90 : 50);
            stroke_round(x, y, w, h, 3, RGB(0xd8, 0xf0, 0xff), sel ? 200 : 130);
        }
        draw_icon(desk_icons[i].icon, x + w / 2, y + 28, 20);
        /* label: centred, wrapped onto two lines if it's long */
        const char *lb = desk_icons[i].label;
        int maxc = w / 8, len = (int)strlen(lb);
        if (len <= maxc) draw_text_shadow(x + (w - 8 * len) / 2, y + 56, lb);
        else {
            char a[16], b[16];
            int cut = maxc;
            for (int k = maxc; k > 0; k--) if (lb[k] == ' ') { cut = k; break; }
            snprintf(a, sizeof a, "%.*s", cut, lb);
            snprintf(b, sizeof b, "%.*s", maxc, lb + cut + (lb[cut] == ' '));
            draw_text_shadow(x + (w - text_w(a)) / 2, y + 52, a);
            draw_text_shadow(x + (w - text_w(b)) / 2, y + 68, b);
        }
    }
}

/* repaint one area of the scene: wallpaper, windows, menu, taskbar */
static void wl_draw_popups(void);
static int wl_covers(int win, int x0, int y0, int x1, int y1);
static void repaint(int x, int y, int w, int h) {
    clip_x0 = imax(x, 0); clip_y0 = imax(y, 0);
    clip_x1 = imin(x + w, W); clip_y1 = imin(y + h, H);
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;
    if (snip_on) { snip_render(clip_x0, clip_y0, clip_x1, clip_y1); return; }   /* the screen is frozen meanwhile */
    /* an app window that hides this whole spot: start from it (nothing under it shows) */
    int from = -1;
    for (int k = nz - 1; k >= 0; k--) {
        int wi = zorder[k];
        if (!wins[wi].open || wins[wi].minimized) continue;
        if (wl_covers(wi, clip_x0, clip_y0, clip_x1, clip_y1)) { from = k; break; }
    }
    if (from < 0) {
        for (int j = clip_y0; j < clip_y1; j++)
            memcpy(&back[j * W + clip_x0], &wall[j * W + clip_x0], (size_t)(clip_x1 - clip_x0) * 4);
        draw_desk_icons();
        from = 0;
    }
    for (int k = from; k < nz; k++) draw_window(zorder[k]);
    wl_draw_popups();                                       /* menus of Wayland apps */
    if (menu_open) draw_menu();
    if (!taskbar_hidden()) draw_taskbar();
    if (net_panel) draw_net_panel();
    if (clk_panel) draw_clock_panel(hot.type == HOT_CLKP ? hot.a : 0);
    draw_osd();
    draw_toasts();
    if (pop.open) {                                         /* right-click menu, on top of all */
        int px = pop.x, py = pop.y, ph = pop_h();
        int sx = 0, sy = 0, sw = 0, sh = 0;
        if (pop.sub_open) pop_sub_rect(&sx, &sy, &sw, &sh);
        if (DARK && !dk_mask) dk_mask = calloc((size_t)W * H, 1);
        if (DARK) dk_clear(px, py, POP_W, ph);
        if (DARK && sw) dk_clear(sx, sy, sw, sh);
        nkeep = 0; dk_collect = DARK;
        fill_rect(px + 3, py + 3, POP_W, ph, RGB(0, 0, 0), 60);
        fill_rect(px, py, POP_W, ph, RGB(0xf5, 0xf5, 0xf5), 255);
        fill_rect(px + 30, py + 2, 1, ph - 4, RGB(0xe2, 0xe3, 0xe3), 255);
        fill_rect(px + 31, py + 2, 1, ph - 4, RGB(0xff, 0xff, 0xff), 255);
        stroke_round(px, py, POP_W, ph, 1, RGB(0x97, 0x97, 0x97), 255);
        for (int i = 0; i < pop.n; i++) {
            int ix, iy, iw, ih; pop_item_rect(i, &ix, &iy, &iw, &ih);
            if ((hot.type == HOT_POPUP && hot.a == i) || (pop.sub_open && i == pop.sub_item)) {
                fill_round(ix, iy, iw, ih, 3, RGB(0xe6, 0xf0, 0xfb), 255);
                stroke_round(ix, iy, iw, ih, 3, RGB(0xae, 0xcf, 0xf7), 255);
            }
            if (pop.act[i] == PA_OPEN || pop.act[i] == PA_OPEN_FILE) {
                draw_icon(pop.icon, ix + 13, iy + ih / 2, 8);
                draw_text_bold(ix + 36, iy + 5, pop.label[i], RGB(0x10, 0x10, 0x10));
            } else {
                if (pop.act[i] == PA_PIN || pop.act[i] == PA_UNPIN) {           /* a little pin */
                    fill_circle(ix + 13, iy + 9, 3.5f, RGB(0x30, 0x60, 0xb0), 255);
                    fill_rect(ix + 12, iy + 12, 2, 7, RGB(0x50, 0x58, 0x64), 255);
                } else if (pop.act[i] == PA_DESK_ADD) {               /* a little screen with + */
                    stroke_round(ix + 6, iy + 6, 15, 11, 1, RGB(0x30, 0x60, 0xb0), 255);
                    fill_rect(ix + 11, iy + 19, 5, 2, RGB(0x50, 0x58, 0x64), 255);
                    fill_rect(ix + 10, iy + 11, 7, 1, RGB(0x30, 0x90, 0x40), 255);
                    fill_rect(ix + 13, iy + 8, 1, 7, RGB(0x30, 0x90, 0x40), 255);
                } else if (pop.act[i] == PA_SET_WALL) {
                    draw_icon(ICON_COMPUTER, ix + 13, iy + ih / 2, 8);
                } else if (pop.act[i] == PA_PRINT) {
                    draw_icon(ICON_PRINTER, ix + 13, iy + ih / 2, 8);
                } else if (pop.act[i] == PA_EJECT) {                 /* eject: a triangle over a bar */
                    for (int k = 0; k < 6; k++) fill_rect(ix + 13 - k, iy + 6 + k, 1 + 2 * k, 1, RGB(0x30, 0x60, 0xb0), 255);
                    fill_rect(ix + 8, iy + 14, 11, 2, RGB(0x30, 0x60, 0xb0), 255);
                } else if (pop.act[i] == PA_CUSTOMIZE) {
                    draw_icon(ICON_GEAR, ix + 13, iy + ih / 2, 8);
                } else if (pop.act[i] == PA_DESK_DEL) {               /* a little bin */
                    fill_rect(ix + 8, iy + 7, 11, 2, RGB(0x50, 0x58, 0x64), 255);
                    fill_rect(ix + 11, iy + 5, 5, 2, RGB(0x50, 0x58, 0x64), 255);
                    stroke_round(ix + 9, iy + 9, 9, 11, 1, RGB(0x50, 0x58, 0x64), 255);
                    fill_rect(ix + 12, iy + 11, 1, 7, RGB(0x50, 0x58, 0x64), 255);
                    fill_rect(ix + 15, iy + 11, 1, 7, RGB(0x50, 0x58, 0x64), 255);
                } else if (pop.act[i] == PA_CLOSE) {
                    for (int k = 0; k < 7; k++) {
                        fill_rect(ix + 10 + k, iy + 9 + k, 2, 1, RGB(0xc0, 0x30, 0x20), 255);
                        fill_rect(ix + 16 - k, iy + 9 + k, 2, 1, RGB(0xc0, 0x30, 0x20), 255);
                    }
                }
                draw_text(ix + 36, iy + 5, pop.label[i], RGB(0x10, 0x10, 0x10), 255);
                if (pop.act[i] == PA_NEWMENU) {                  /* > : it opens a list */
                    for (int k = 0; k < 4; k++) fill_rect(ix + iw - 14 + k, iy + 9 + k, 1, 9 - 2 * k, RGB(0x30, 0x30, 0x30), 255);
                }
            }
        }
        if (sw) {                                             /* the submenu */
            fill_rect(sx + 3, sy + 3, sw, sh, RGB(0, 0, 0), 60);
            fill_rect(sx, sy, sw, sh, RGB(0xf5, 0xf5, 0xf5), 255);
            fill_rect(sx + 30, sy + 2, 1, sh - 4, RGB(0xe2, 0xe3, 0xe3), 255);
            fill_rect(sx + 31, sy + 2, 1, sh - 4, RGB(0xff, 0xff, 0xff), 255);
            stroke_round(sx, sy, sw, sh, 1, RGB(0x97, 0x97, 0x97), 255);
            for (int j = 0; j < pop.sub_n; j++) {
                int ix, iy, iw, ih; pop_sub_item_rect(j, &ix, &iy, &iw, &ih);
                if (hot.type == HOT_POPUP && hot.a == 100 + j) {
                    fill_round(ix, iy, iw, ih, 3, RGB(0xe6, 0xf0, 0xfb), 255);
                    stroke_round(ix, iy, iw, ih, 3, RGB(0xae, 0xcf, 0xf7), 255);
                }
                draw_icon(pop.sub_icon[j], ix + 13, iy + ih / 2, 8);
                draw_text(ix + 36, iy + 5, pop.sub_label[j], RGB(0x10, 0x10, 0x10), 255);
                if (j == 0 && pop.sub_n > 1) fill_rect(ix + 32, iy + ih, iw - 34, 1, RGB(0xe0, 0xe0, 0xe0), 255);
            }
        }
        dk_collect = 0;
        if (DARK) dark_xform(px, py, POP_W, ph);
        if (DARK && sw) dark_xform(sx, sy, sw, sh);
        nkeep = 0;
    }
    mark(x, y, w, h);
}

static void repaint_all(void)     { repaint(0, 0, W, H); }
static void repaint_taskbar(void) { repaint(0, H - TASKBAR_H, W, TASKBAR_H); }
static void repaint_menu(void)    { repaint(menu_x(), menu_y(), MENU_W + 4, MENU_H + 4); }
static void repaint_clock(void)   { repaint(W - CLOCK_W - 8, H - TASKBAR_H, CLOCK_W + 8, TASKBAR_H); }
static void repaint_window(int i) { Win *w = &wins[i]; repaint(w->x - 2, w->y - 1, w->w + 7, w->h + 7); }

static void repaint_hot(Hot h) {
    int x, y, w, hh;
    switch (h.type) {
    case HOT_ORB:   repaint(orb_cx() - ORB_R - 2, orb_cy() - ORB_R - 2, 2 * ORB_R + 5, 2 * ORB_R + 5); break;
    case HOT_SD:    power_rect(h.a, &x, &y, &w, &hh); repaint(x, y, w, hh); break;
    case HOT_LITEM: repaint(LI_X, ITEMS_Y + h.a * ITEM_H, LI_W, ITEM_H); break;
    case HOT_SRES: repaint(LI_X, ITEMS_Y + h.a * ITEM_H, LI_W, ITEM_H); if (h.a != ms_sel) repaint(LI_X, ITEMS_Y + ms_sel * ITEM_H, LI_W, ITEM_H); break;
    case HOT_RITEM: repaint(RP_X, ITEMS_Y + h.a * ITEM_H, RP_W, ITEM_H); break;
    case HOT_CAP:   cap_rect(&wins[h.a], h.b, &x, &y, &w, &hh); repaint(x, y, w, hh); break;
    case HOT_TASK:  task_rect(h.a, &x, &y, &w, &hh); repaint(x, y, w, hh); break;
    case HOT_ICON:  di_rect(h.a, &x, &y, &w, &hh); repaint(x, y, w, hh); break;
    case HOT_POPUP:
        if (pop.sub_item >= 0) { popm_repaint(); break; }
        pop_item_rect(h.a, &x, &y, &w, &hh); repaint(x, y, w, hh); break;
    case HOT_TRAY:  repaint(tray_x(), H - TASKBAR_H, TRAY_W, TASKBAR_H); break;
    case HOT_BAT:   repaint(bat_x(), H - TASKBAR_H, BAT_W, TASKBAR_H); break;
    case HOT_REC:   repaint_taskbar(); break;
    case HOT_CLOCK: repaint_clock(); break;
    case HOT_CLKP:  clkp_repaint(); break;
    case HOT_NETP:
        for (int i = 0; i < nnrect; i++) if (nrect[i].id == h.a) repaint(nrect[i].x, nrect[i].y, nrect[i].w, nrect[i].h);
        break;
    case HOT_CONTENT:
        if (wins[h.a].open && wins[h.a].elem_rect) {
            int cx, cy, cw, ch; client_rect(&wins[h.a], &cx, &cy, &cw, &ch);
            wins[h.a].elem_rect(h.b, &x, &y, &w, &hh);
            repaint(cx + x, cy + y, w, hh);
        }
        break;
    }
}

/* ---- window management ---------------------------------------------- */
static void raise_win(int idx) {
    int k;
    for (k = 0; k < nz && zorder[k] != idx; k++) {}
    if (k == nz) { zorder[nz++] = idx; return; }
    if (k == nz - 1) return;
    int old = active_win();
    memmove(&zorder[k], &zorder[k + 1], (size_t)(nz - 1 - k) * sizeof *zorder);
    zorder[nz - 1] = idx;
    repaint_window(idx);
    if (old >= 0 && old != idx) repaint_window(old);   /* now drawn as inactive */
    repaint_taskbar();
}

static void close_win(int idx) {
    Win *w = &wins[idx];
    int x = w->x, y = w->y, ww = w->w, hh = w->h, was_min = w->minimized;
    int k;
    for (k = 0; k < nz && zorder[k] != idx; k++) {}
    if (k < nz) { memmove(&zorder[k], &zorder[k + 1], (size_t)(nz - 1 - k) * sizeof *zorder); nz--; }
    memset(w, 0, sizeof *w);
    if (!was_min) repaint(x - 2, y - 1, ww + 7, hh + 7);
    int a = active_win(); if (a >= 0) repaint_window(a);
    repaint_taskbar();
}

static void minimize_win(int idx) {
    wins[idx].minimized = 1;
    repaint_window(idx);
    int a = active_win(); if (a >= 0) repaint_window(a);
    repaint_taskbar();
}

static void restore_win(int idx) {           /* un-minimize and bring to front */
    wins[idx].minimized = 0;
    int old = active_win();
    int k;
    for (k = 0; k < nz && zorder[k] != idx; k++) {}
    if (k < nz) { memmove(&zorder[k], &zorder[k + 1], (size_t)(nz - 1 - k) * sizeof *zorder); nz--; }
    zorder[nz++] = idx;
    repaint_window(idx);
    if (old >= 0 && old != idx) repaint_window(old);
    repaint_taskbar();
}

static void set_fullscreen(int idx, int on);
static void toggle_max(int idx) {
    Win *w = &wins[idx];
    if (w->fullscreen) { set_fullscreen(idx, 0); return; }
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;    /* old area */
    if (!w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = W; w->h = H - TASKBAR_H;
        w->maximized = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maximized = 0;
    }
    repaint(ox - 2, oy - 1, ow + 7, oh + 7);           /* now uncovered */
    repaint_window(idx);                               /* new area */
    if (w->resized) w->resized();
}

/* full screen: the content covers the whole screen; the frame and title sit just outside it */
static void set_fullscreen(int idx, int on) {
    Win *w = &wins[idx];
    if (!w->open || !!on == w->fullscreen) return;
    if (on) {
        w->fx = w->x; w->fy = w->y; w->fw = w->w; w->fh = w->h;
        w->x = -FRAME; w->y = -TITLE_H; w->w = W + 2 * FRAME; w->h = H + TITLE_H + FRAME;
        w->fullscreen = 1;
        if (w->minimized) w->minimized = 0;
        raise_win(idx);
    } else {
        w->x = w->fx; w->y = w->fy; w->w = w->fw; w->h = w->fh;
        w->fullscreen = 0;
    }
    repaint_all();
    if (w->resized) w->resized();
}

static void move_win(int idx, int nx, int ny) {
    Win *w = &wins[idx];
    nx = clampi(nx, -w->w + 80, W - 80);
    ny = clampi(ny, 0, H - TASKBAR_H - TITLE_H);
    if (nx == w->x && ny == w->y) return;
    int ox = w->x, oy = w->y;
    w->x = nx; w->y = ny;
    int x0 = imin(ox, nx) - 2, y0 = imin(oy, ny) - 1;      /* old + new area */
    int x1 = imax(ox, nx) + w->w + 5, y1 = imax(oy, ny) + w->h + 6;
    repaint(x0, y0, x1 - x0, y1 - y0);
}

static char launch_cmd[112];          /* set by run() just before it opens a window */

/* open an app window from a template (title, icon, callbacks), client size cw x ch */
static int open_win(const Win *tmpl, int cw, int ch) {
    for (int i = 0; i < MAXWIN; i++)          /* already open? just show it */
        if (wins[i].open && wins[i].draw == tmpl->draw) { restore_win(i); return i; }
    for (int i = 0; i < MAXWIN; i++) {
        if (wins[i].open) continue;
        Win *w = &wins[i];
        *w = *tmpl;
        w->open = 1; w->minimized = 0; w->maximized = 0;
        snprintf(w->cmd, sizeof w->cmd, "%s", launch_cmd);
        w->w = cw + 2 * FRAME; w->h = ch + TITLE_H + FRAME;
        w->x = (W - w->w) / 2 + nz * 24; w->y = (H - TASKBAR_H - w->h) / 2 - 20 + nz * 24;
        w->x = clampi(w->x, 0, imax(0, W - w->w));                  /* keep big windows on the screen */
        w->y = clampi(w->y, 0, imax(0, H - TASKBAR_H - w->h));
        int old = active_win();
        zorder[nz++] = i;
        repaint_window(i);
        if (old >= 0) repaint_window(old);
        repaint_taskbar();
        return i;
    }
    return -1;
}

/* ===================================================================
 *  SHARED UI FOR APPS: keyboard map, menu bar, drop-down menu, dialog
 * =================================================================== */
static long long ui_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int win_of(void (*draw)(int, int, int, int)) {
    for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].draw == draw) return i;
    return -1;
}

/* repaint a rectangle given in an app's client coordinates */
static void repaint_client_rect(void (*draw)(int, int, int, int), int x, int y, int w, int h) {
    int i = win_of(draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    int x0 = imax(x, 0), y0 = imax(y, 0), x1 = imin(x + w, cw), y1 = imin(y + h, ch);
    if (x0 < x1 && y0 < y1) repaint(cx + x0, cy + y0, x1 - x0, y1 - y0);
}
static void repaint_client(void (*draw)(int, int, int, int)) {
    repaint_client_rect(draw, 0, 0, 10000, 10000);
}

/* change an app window's size (client area cw x ch) */
static void resize_win(int idx, int cw, int ch) {
    Win *w = &wins[idx];
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    w->w = cw + 2 * FRAME; w->h = ch + TITLE_H + FRAME;
    if (w->x + w->w > W) w->x = imax(0, W - w->w);
    if (w->y + w->h > H - TASKBAR_H) w->y = imax(0, H - TASKBAR_H - w->h);
    repaint(ox - 2, oy - 1, ow + 7, oh + 7);
    repaint_window(idx);
}

/* US keyboard layout: key code -> character */
static const char keymap_lo[128] = {
    [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',[KEY_6]='6',[KEY_7]='7',[KEY_8]='8',
    [KEY_9]='9',[KEY_0]='0',[KEY_MINUS]='-',[KEY_EQUAL]='=',
    [KEY_Q]='q',[KEY_W]='w',[KEY_E]='e',[KEY_R]='r',[KEY_T]='t',[KEY_Y]='y',[KEY_U]='u',[KEY_I]='i',
    [KEY_O]='o',[KEY_P]='p',[KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']',
    [KEY_A]='a',[KEY_S]='s',[KEY_D]='d',[KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_J]='j',[KEY_K]='k',
    [KEY_L]='l',[KEY_SEMICOLON]=';',[KEY_APOSTROPHE]='\'',[KEY_GRAVE]='`',[KEY_BACKSLASH]='\\',
    [KEY_Z]='z',[KEY_X]='x',[KEY_C]='c',[KEY_V]='v',[KEY_B]='b',[KEY_N]='n',[KEY_M]='m',
    [KEY_COMMA]=',',[KEY_DOT]='.',[KEY_SLASH]='/',[KEY_SPACE]=' ',
    [KEY_KP0]='0',[KEY_KP1]='1',[KEY_KP2]='2',[KEY_KP3]='3',[KEY_KP4]='4',[KEY_KP5]='5',
    [KEY_KP6]='6',[KEY_KP7]='7',[KEY_KP8]='8',[KEY_KP9]='9',[KEY_KPDOT]='.',[KEY_KPPLUS]='+',
    [KEY_KPMINUS]='-',[KEY_KPASTERISK]='*',[KEY_KPSLASH]='/',
};
static const char keymap_hi[128] = {
    [KEY_1]='!',[KEY_2]='@',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',[KEY_6]='^',[KEY_7]='&',[KEY_8]='*',
    [KEY_9]='(',[KEY_0]=')',[KEY_MINUS]='_',[KEY_EQUAL]='+',
    [KEY_Q]='Q',[KEY_W]='W',[KEY_E]='E',[KEY_R]='R',[KEY_T]='T',[KEY_Y]='Y',[KEY_U]='U',[KEY_I]='I',
    [KEY_O]='O',[KEY_P]='P',[KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}',
    [KEY_A]='A',[KEY_S]='S',[KEY_D]='D',[KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_J]='J',[KEY_K]='K',
    [KEY_L]='L',[KEY_SEMICOLON]=':',[KEY_APOSTROPHE]='"',[KEY_GRAVE]='~',[KEY_BACKSLASH]='|',
    [KEY_Z]='Z',[KEY_X]='X',[KEY_C]='C',[KEY_V]='V',[KEY_B]='B',[KEY_N]='N',[KEY_M]='M',
    [KEY_COMMA]='<',[KEY_DOT]='>',[KEY_SLASH]='?',[KEY_SPACE]=' ',
    [KEY_KP0]='0',[KEY_KP1]='1',[KEY_KP2]='2',[KEY_KP3]='3',[KEY_KP4]='4',[KEY_KP5]='5',
    [KEY_KP6]='6',[KEY_KP7]='7',[KEY_KP8]='8',[KEY_KP9]='9',[KEY_KPDOT]='.',[KEY_KPPLUS]='+',
    [KEY_KPMINUS]='-',[KEY_KPASTERISK]='*',[KEY_KPSLASH]='/',
};

static char key_char(int code, int mods) {
    if (code < 0 || code >= 128 || (mods & (MOD_CTRL | MOD_ALTGR))) return 0;
    char lo = keymap_lo[code], hi = keymap_hi[code];
    if (!lo) return 0;
    int up = (mods & MOD_SHIFT) != 0;
    if (lo >= 'a' && lo <= 'z' && capslock) up = !up;         /* Caps Lock only flips letters */
    return up ? hi : lo;
}

/* what a key types, as UTF-8 (AltGr + a = ą on the Polish layout); returns the bytes */
static int key_text(int code, int mods, char *out) {
    out[0] = 0;
    if ((mods & MOD_ALTGR) && !(mods & MOD_CTRL)) {
        if (set_kbd != KBD_PL) return 0;
        static const struct { int code; unsigned lo, up; } pl[] = {
            { KEY_A, 0x105, 0x104 }, { KEY_C, 0x107, 0x106 }, { KEY_E, 0x119, 0x118 }, { KEY_L, 0x142, 0x141 },
            { KEY_N, 0x144, 0x143 }, { KEY_O, 0xF3, 0xD3 }, { KEY_S, 0x15B, 0x15A }, { KEY_X, 0x17A, 0x179 },
            { KEY_Z, 0x17C, 0x17B } };
        for (size_t i = 0; i < sizeof pl / sizeof pl[0]; i++)
            if (pl[i].code == code) {
                int up = (mods & MOD_SHIFT) != 0; if (capslock) up = !up;
                unsigned cp = up ? pl[i].up : pl[i].lo;
                out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); out[2] = 0;
                return 2;
            }
        return 0;
    }
    char c = key_char(code, mods);
    if (!c) return 0;
    out[0] = c; out[1] = 0;
    return 1;
}

/* ---- clipboard (shared by every app) -------------------------------- */
static char *clipboard;
static int clip_len;
static void clip_set(const char *s, int n) {
    char *c = malloc((size_t)n + 1);
    if (!c) return;
    memcpy(c, s, n); c[n] = 0;
    free(clipboard); clipboard = c; clip_len = n;
}

/* ---- menu bar + drop-down ------------------------------------------- */
#define MENUBAR_H 22
#define DROP_W    200
#define ID_MBAR   500       /* element ids: menu bar labels 500+, drop-down items 600+ */
#define ID_DROP   600

typedef struct { const char *label, *accel; } DropItem;     /* label "-" = separator */
typedef struct { const char *title; const DropItem *items; int n; } MenuDef;
typedef struct { const MenuDef *menus; int nmenus, open; } MenuBar;

static int mb_x(const MenuBar *m, int i) {
    int x = 4;
    for (int k = 0; k < i; k++) x += text_w(m->menus[k].title) + 16;
    return x;
}
static int mb_w(const MenuBar *m, int i) { return text_w(m->menus[i].title) + 16; }
static int drop_y(const MenuDef *md, int i) {
    int y = MENUBAR_H + 3;
    for (int k = 0; k < i; k++) y += md->items[k].label[0] == '-' ? 8 : 24;
    return y;
}

/* element id -> rectangle (client coords); returns 0 if not a menu id */
static int mb_elem_rect(const MenuBar *m, int id, int *x, int *y, int *w, int *h) {
    if (id >= ID_MBAR && id < ID_MBAR + m->nmenus) {
        *x = mb_x(m, id - ID_MBAR); *y = 0; *w = mb_w(m, id - ID_MBAR); *h = MENUBAR_H; return 1;
    }
    if (id >= ID_DROP && id < ID_DROP + 50 && m->open >= 0) {
        const MenuDef *md = &m->menus[m->open];
        int i = id - ID_DROP;
        *x = mb_x(m, m->open) + 2; *y = drop_y(md, i); *w = DROP_W - 4; *h = 24; return 1;
    }
    return 0;
}

static int mb_hit(const MenuBar *m, int lx, int ly) {
    if (m->open >= 0) {
        const MenuDef *md = &m->menus[m->open];
        int x0 = mb_x(m, m->open);
        for (int i = 0; i < md->n; i++) {
            if (md->items[i].label[0] == '-') continue;
            int y = drop_y(md, i);
            if (lx >= x0 && lx < x0 + DROP_W && ly >= y && ly < y + 24) return ID_DROP + i;
        }
    }
    if (ly >= 0 && ly < MENUBAR_H)
        for (int i = 0; i < m->nmenus; i++)
            if (lx >= mb_x(m, i) && lx < mb_x(m, i) + mb_w(m, i)) return ID_MBAR + i;
    return -1;
}

static void mb_draw_bar(const MenuBar *m, int cx, int cy, int cw, int hotid) {
    fill_rect(cx, cy, cw, MENUBAR_H, RGB(0xf2, 0xf5, 0xfa), 255);
    for (int j = 0; j < MENUBAR_H; j++)
        fill_rect(cx, cy + j, cw, 1, RGB(0xdc, 0xe6, 0xf2), j * 255 / MENUBAR_H);
    fill_rect(cx, cy + MENUBAR_H - 1, cw, 1, RGB(0xb6, 0xc4, 0xd6), 255);
    for (int i = 0; i < m->nmenus; i++) {
        int x = cx + mb_x(m, i), w = mb_w(m, i);
        if (m->open == i || hotid == ID_MBAR + i) {
            fill_round(x, cy + 2, w, MENUBAR_H - 4, 3, RGB(0xd6, 0xe8, 0xfa), 255);
            stroke_round(x, cy + 2, w, MENUBAR_H - 4, 3, RGB(0x86, 0xae, 0xd8), 255);
        }
        draw_text(x + 8, cy + 3, m->menus[i].title, RGB(0x10, 0x10, 0x10), 255);
    }
}

static void mb_draw_drop(const MenuBar *m, int cx, int cy, int hotid) {
    if (m->open < 0) return;
    const MenuDef *md = &m->menus[m->open];
    int x = cx + mb_x(m, m->open), y = cy + MENUBAR_H, h = drop_y(md, md->n) + 3 - MENUBAR_H;
    fill_rect(x + 3, y + 3, DROP_W, h, RGB(0, 0, 0), 50);                  /* shadow */
    fill_rect(x, y, DROP_W, h, RGB(0xf5, 0xf5, 0xf5), 255);
    fill_rect(x + 28, y + 2, 1, h - 4, RGB(0xe2, 0xe3, 0xe3), 255);       /* gutter line */
    fill_rect(x + 29, y + 2, 1, h - 4, RGB(0xff, 0xff, 0xff), 255);
    stroke_round(x, y, DROP_W, h, 1, RGB(0x97, 0x97, 0x97), 255);
    for (int i = 0; i < md->n; i++) {
        int iy = cy + drop_y(md, i);
        if (md->items[i].label[0] == '-') {
            fill_rect(x + 32, iy + 3, DROP_W - 36, 1, RGB(0xe0, 0xe0, 0xe0), 255);
            continue;
        }
        if (hotid == ID_DROP + i) {
            fill_round(x + 3, iy, DROP_W - 6, 24, 3, RGB(0xe6, 0xf0, 0xfb), 255);
            stroke_round(x + 3, iy, DROP_W - 6, 24, 3, RGB(0xae, 0xcf, 0xf7), 255);
        }
        draw_text(x + 36, iy + 4, md->items[i].label, RGB(0x10, 0x10, 0x10), 255);
        const char *ac = md->items[i].accel;
        if (ac && *ac) {
            if (!strcmp(ac, "*")) fill_circle(x + 15, iy + 12, 3.2f, RGB(0x20, 0x20, 0x20), 255);  /* chosen */
            else draw_text(x + DROP_W - 10 - text_w(ac), iy + 4, ac, RGB(0x6d, 0x6d, 0x6d), 255);
        }
    }
}

/* handles a click on the menu bar / drop-down. Returns:
 *   -1  not a menu click        -2  handled (opened/closed a menu)
 *   n>=0  drop-down item n of menu m->open-before-click was chosen (*menu set) */
static int mb_click(MenuBar *m, int lx, int ly, int *menu, void (*draw)(int, int, int, int)) {
    int id = mb_hit(m, lx, ly);
    if (id >= ID_DROP) {
        *menu = m->open; m->open = -1; repaint_client(draw);
        return id - ID_DROP;
    }
    if (id >= ID_MBAR) {
        m->open = m->open == id - ID_MBAR ? -1 : id - ID_MBAR;
        repaint_client(draw);
        return -2;
    }
    if (m->open >= 0) {                              /* click elsewhere closes it */
        m->open = -1; repaint_client(draw);
        return -2;
    }
    return -1;
}

/* ---- dialog box (drawn inside the app's window, blocks the app) ---- */
enum { DLG_MSG, DLG_INPUT, DLG_LIST };
#define ID_DLGBTN  1000
#define ID_DLGITEM 2000
#define DLG_LIST_ROWS 8
#define DLG_MAXITEMS 64

typedef struct {
    int active, kind;
    char title[48], msg[2][80];         /* up to two lines of message */
    const char *btn[3]; int nbtn;
    char field[40];                     /* DLG_INPUT */
    char items[DLG_MAXITEMS][40]; int nitems, sel, scroll;   /* DLG_LIST */
    long long last_click; int last_item;
    void (*done)(int button);           /* button index; Esc = last button */
} Dialog;

static void dlg_geom(const Dialog *d, int cw, int ch, int *x, int *y, int *w, int *h) {
    *w = imin(320, cw - 16);
    *h = 34 + 20 * (d->msg[1][0] ? 2 : 1) + 8 + 46;
    if (d->kind == DLG_INPUT) *h += 34;
    if (d->kind == DLG_LIST) *h += DLG_LIST_ROWS * 20 + 10;
    *x = (cw - *w) / 2; *y = imax(MENUBAR_H + 4, (ch - *h) / 2);
}
static int dlg_body_y(const Dialog *d) { return 34 + 20 * (d->msg[1][0] ? 2 : 1) + 6; }
static void dlg_btn_rect(const Dialog *d, int i, int cw, int ch, int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh; dlg_geom(d, cw, ch, &dx, &dy, &dw, &dh);
    *w = 86; *h = 26;
    *x = dx + dw - 12 - (d->nbtn - i) * (86 + 8) + 8;
    *y = dy + dh - 36;
}
static void dlg_item_rect(const Dialog *d, int vis_row, int cw, int ch, int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh; dlg_geom(d, cw, ch, &dx, &dy, &dw, &dh);
    *x = dx + 17; *y = dy + dlg_body_y(d) + 1 + vis_row * 20; *w = dw - 34; *h = 20;
}

static int dlg_elem_rect(const Dialog *d, int id, int cw, int ch, int *x, int *y, int *w, int *h) {
    if (!d->active) return 0;
    if (id >= ID_DLGBTN && id < ID_DLGBTN + d->nbtn) { dlg_btn_rect(d, id - ID_DLGBTN, cw, ch, x, y, w, h); return 1; }
    if (id >= ID_DLGITEM && id < ID_DLGITEM + DLG_MAXITEMS) {
        dlg_item_rect(d, id - ID_DLGITEM - d->scroll, cw, ch, x, y, w, h); return 1;
    }
    return 0;
}

static int dlg_hit(const Dialog *d, int lx, int ly, int cw, int ch) {
    for (int i = 0; i < d->nbtn; i++) {
        int x, y, w, h; dlg_btn_rect(d, i, cw, ch, &x, &y, &w, &h);
        if (lx >= x && lx < x + w && ly >= y && ly < y + h) return ID_DLGBTN + i;
    }
    if (d->kind == DLG_LIST)
        for (int r = 0; r < DLG_LIST_ROWS && d->scroll + r < d->nitems; r++) {
            int x, y, w, h; dlg_item_rect(d, r, cw, ch, &x, &y, &w, &h);
            if (lx >= x && lx < x + w && ly >= y && ly < y + h) return ID_DLGITEM + d->scroll + r;
        }
    return -1;
}

static void win7_button(int x, int y, int w, int h, const char *label, int hover, int deflt) {
    fill_round_grad(x, y, w, h, 3, hover ? RGB(0xea, 0xf6, 0xfd) : RGB(0xf2, 0xf2, 0xf2),
                    hover ? RGB(0xa7, 0xd9, 0xf5) : RGB(0xcf, 0xcf, 0xcf), 255);
    fill_rect(x + 2, y + 1, w - 4, h / 2 - 1, RGB(255, 255, 255), 90);
    stroke_round(x, y, w, h, 3, hover || deflt ? RGB(0x3c, 0x7f, 0xb1) : RGB(0x70, 0x70, 0x70), 255);
    draw_text(x + (w - text_w(label)) / 2, y + (h - 16) / 2, label, RGB(0x10, 0x10, 0x10), 255);
}

static void dlg_draw(const Dialog *d, int cx, int cy, int cw, int ch, int hotid) {
    if (!d->active) return;
    fill_rect(cx, cy, cw, ch, RGB(255, 255, 255), 90);                     /* dim the app */
    int x, y, w, h; dlg_geom(d, cw, ch, &x, &y, &w, &h);
    x += cx; y += cy;
    fill_round(x + 2, y + 3, w + 2, h + 2, 4, RGB(0, 0, 0), 60);
    fill_round(x, y, w, h, 4, RGB(0xff, 0xff, 0xff), 255);
    fill_round_grad(x, y, w, 28, 4, RGB(0xd8, 0xe8, 0xf8), RGB(0xb4, 0xd0, 0xee), 255);
    fill_rect(x, y + 24, w, 4, RGB(0xb4, 0xd0, 0xee), 255);
    stroke_round(x, y, w, h, 4, RGB(0x55, 0x6a, 0x84), 255);
    draw_text_bold(x + 12, y + 6, d->title, RGB(0x10, 0x1e, 0x3c));
    for (int l = 0; l < 2; l++)
        if (d->msg[l][0]) draw_text(x + 16, y + 38 + 20 * l, d->msg[l], RGB(0x1e, 0x3c, 0x78), 255);
    int by = y + dlg_body_y(d);
    if (d->kind == DLG_INPUT) {
        fill_rect(x + 16, by, w - 32, 26, RGB(255, 255, 255), 255);
        stroke_round(x + 16, by, w - 32, 26, 2, RGB(0x7a, 0x9c, 0xc8), 255);
        draw_text(x + 22, by + 5, d->field, RGB(0x10, 0x10, 0x10), 255);
        fill_rect(x + 22 + text_w(d->field), by + 5, 1, 16, RGB(0, 0, 0), 255);   /* caret */
    }
    if (d->kind == DLG_LIST) {
        int lh = DLG_LIST_ROWS * 20 + 2;
        fill_rect(x + 16, by, w - 32, lh, RGB(255, 255, 255), 255);
        stroke_round(x + 16, by, w - 32, lh, 2, RGB(0x82, 0x87, 0x90), 255);
        if (!d->nitems) draw_text(x + 24, by + 6, "(no saved documents yet)", RGB(0x90, 0x90, 0x90), 255);
        for (int r = 0; r < DLG_LIST_ROWS && d->scroll + r < d->nitems; r++) {
            int i = d->scroll + r, ix, iy, iw, ih;
            dlg_item_rect(d, r, cw, ch, &ix, &iy, &iw, &ih);
            ix += cx; iy += cy;
            if (i == d->sel) fill_rect(ix, iy, iw, ih, RGB(0x33, 0x99, 0xff), 255);
            else if (hotid == ID_DLGITEM + i) fill_rect(ix, iy, iw, ih, RGB(0xe5, 0xf3, 0xfb), 255);
            draw_text(ix + 6, iy + 2, d->items[i], i == d->sel ? RGB(255, 255, 255) : RGB(0x10, 0x10, 0x10), 255);
        }
    }
    fill_rect(x + 1, y + h - 46, w - 2, 45, RGB(0xf0, 0xf0, 0xf0), 255);   /* button strip */
    fill_rect(x + 1, y + h - 46, w - 2, 1, RGB(0xdf, 0xdf, 0xdf), 255);
    for (int i = 0; i < d->nbtn; i++) {
        int bx, byy, bw, bh; dlg_btn_rect(d, i, cw, ch, &bx, &byy, &bw, &bh);
        win7_button(cx + bx, cy + byy, bw, bh, d->btn[i], hotid == ID_DLGBTN + i, i == 0);
    }
}

static void dlg_open(Dialog *d, int kind, const char *title, const char *msg1, const char *msg2,
                     const char *b0, const char *b1, const char *b2, void (*done)(int)) {
    memset(d, 0, sizeof *d);
    d->active = 1; d->kind = kind; d->done = done; d->sel = -1; d->last_item = -1;
    snprintf(d->title, sizeof d->title, "%s", title);
    snprintf(d->msg[0], sizeof d->msg[0], "%s", msg1 ? msg1 : "");
    snprintf(d->msg[1], sizeof d->msg[1], "%s", msg2 ? msg2 : "");
    d->btn[0] = b0; d->btn[1] = b1; d->btn[2] = b2;
    d->nbtn = b2 ? 3 : b1 ? 2 : 1;
}

static void dlg_finish(Dialog *d, int b, void (*draw)(int, int, int, int)) {
    d->active = 0;
    repaint_client(draw);
    if (d->done) d->done(b);
}

/* returns 1 if the dialog took the click (it always does while open) */
static int dlg_click(Dialog *d, int lx, int ly, int cw, int ch, void (*draw)(int, int, int, int)) {
    if (!d->active) return 0;
    int id = dlg_hit(d, lx, ly, cw, ch);
    if (id >= ID_DLGITEM) {
        int i = id - ID_DLGITEM;
        long long t = ui_ms();
        int dbl = i == d->last_item && t - d->last_click < 450;
        d->sel = i; d->last_item = i; d->last_click = t;
        repaint_client(draw);
        if (dbl) dlg_finish(d, 0, draw);
    } else if (id >= ID_DLGBTN) dlg_finish(d, id - ID_DLGBTN, draw);
    return 1;
}

static int dlg_key(Dialog *d, int code, int mods, void (*draw)(int, int, int, int)) {
    if (!d->active) return 0;
    if (code == KEY_ESC) { dlg_finish(d, d->nbtn - 1, draw); return 1; }
    if (code == KEY_ENTER || code == KEY_KPENTER) { dlg_finish(d, 0, draw); return 1; }
    if (d->kind == DLG_INPUT) {
        size_t l = strlen(d->field);
        if ((mods & MOD_CTRL) && code == KEY_C) clip_set(d->field, (int)l);
        else if ((mods & MOD_CTRL) && code == KEY_V && clipboard) {
            for (int i = 0; i < clip_len && l < sizeof d->field - 1; i++) {
                char c = clipboard[i];
                if (c == '\n') break;
                if (isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || strchr(" -_.()", c)) { d->field[l++] = c; d->field[l] = 0; }
            }
        }
        else if (code == KEY_BACKSPACE && l) d->field[u8_prev(d->field, (int)l)] = 0;
        else {
            char s[8]; int n = key_text(code, mods, s);
            /* only characters that are safe in a file name (Polish letters are fine) */
            if (n && (n > 1 || isalnum((unsigned char)s[0]) || strchr(" -_.()", s[0])) && l + (size_t)n < sizeof d->field) {
                memcpy(d->field + l, s, (size_t)n + 1);
            }
        }
    } else if (d->kind == DLG_LIST && d->nitems) {
        if (code == KEY_DOWN) d->sel = imin(d->sel + 1, d->nitems - 1);
        if (code == KEY_UP) d->sel = imax(d->sel - 1, 0);
        if (d->sel < d->scroll) d->scroll = d->sel;
        if (d->sel >= d->scroll + DLG_LIST_ROWS) d->scroll = d->sel - DLG_LIST_ROWS + 1;
    }
    repaint_client(draw);
    return 1;
}

/* ===================================================================
 *  NOTEPAD
 * =================================================================== */
#define NP_MAX   65536
#define NP_DIR   "/data/Documents"
#define NP_SB_W  16             /* scrollbar width */

static char np_buf[NP_MAX + 1];
static int  np_len, np_caret, np_top, np_modified;
static char np_file[40];                       /* "" = untitled */
static char np_dir[256] = NP_DIR;              /* folder the file lives in */
static char np_title[64] = "Untitled - Notepad";
static int  *np_lines, np_nlines;              /* start of every (wrapped) line */
static int  np_cols = 60, np_rows = 20;
static int  np_goal_col = -1;                  /* column kept while moving up/down */
static int  np_anchor = -1;                    /* other end of the selection (-1 = none) */
static int  np_dragging;                       /* selecting with the mouse */
static long long np_last_click; static int np_last_pos = -1;
static int  np_ctx_open, np_ctx_x, np_ctx_y;   /* right-click menu */
#define ID_NPCTX 700
static Dialog np_dlg;
static int  np_dlg_mode, np_pending;           /* what the open dialog is for / what to do after */
enum { NPD_PROMPT = 1, NPD_SAVEAS, NPD_OPEN, NPD_INFO };
enum { NPA_NONE, NPA_NEW, NPA_OPEN, NPA_CLOSE };

static const DropItem np_file_items[] = {
    { "New", "Ctrl+N" }, { "Open...", "Ctrl+O" }, { "Save", "Ctrl+S" }, { "Save As...", "" },
    { "-", "" }, { "Exit", "" },
};
static const DropItem np_edit_items[] = {
    { "Cut", "Ctrl+X" }, { "Copy", "Ctrl+C" }, { "Paste", "Ctrl+V" }, { "Delete", "Del" },
    { "-", "" }, { "Select All", "Ctrl+A" },
};
static const MenuDef np_menus[] = { { "File", np_file_items, 6 }, { "Edit", np_edit_items, 6 } };
static MenuBar np_mb = { np_menus, 2, -1 };
#define NP_CTX_N 6                             /* right-click menu uses the Edit items */

static void notepad_draw(int cx, int cy, int cw, int ch);
void np_dlg_done(int b);

static void np_set_title(void) {
    snprintf(np_title, sizeof np_title, "%s - Notepad", np_file[0] ? np_file : "Untitled");
    int i = win_of(notepad_draw);
    if (i >= 0) { repaint_window(i); repaint_taskbar(); }
}

/* split the text into lines that fit the window (word wrap) */
static void np_layout(void) {
    int n = 0, s = 0;
    for (;;) {
        np_lines[n++] = s;
        int e = s, cols = 0;                            /* characters, not bytes (UTF-8) */
        while (e < np_len && np_buf[e] != '\n' && cols < np_cols) { e = u8_next(np_buf, np_len, e); cols++; }
        int lim = e;
        if (e >= np_len) break;
        if (np_buf[e] == '\n') { s = e + 1; continue; }
        int b = lim;                                    /* wrap after the last space */
        for (int k = lim - 1; k > s; k--) if (np_buf[k] == ' ') { b = k + 1; break; }
        s = b;
    }
    np_nlines = n;
}

static int np_line_of(int pos) {
    int lo = 0, hi = np_nlines - 1;                     /* last line starting <= pos */
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (np_lines[mid] <= pos) lo = mid; else hi = mid - 1; }
    return lo;
}
static int np_line_len(int i) {                         /* visible characters on line i */
    int end = i + 1 < np_nlines ? np_lines[i + 1] : np_len;
    if (end > np_lines[i] && end <= np_len && np_buf[end - 1] == '\n') end--;
    return end - np_lines[i];
}
static void np_scroll_to_caret(void) {
    int l = np_line_of(np_caret);
    if (l < np_top) np_top = l;
    if (l >= np_top + np_rows) np_top = l - np_rows + 1;
    np_top = clampi(np_top, 0, imax(0, np_nlines - 1));
}

static void np_text_geom(int cw, int ch, int *tx, int *ty, int *tw, int *th) {
    *tx = 0; *ty = MENUBAR_H; *tw = cw - NP_SB_W; *th = ch - MENUBAR_H;
}

/* the selected range [a, b); 0 if nothing is selected */
static int np_sel(int *a, int *b) {
    if (np_anchor < 0 || np_anchor == np_caret) return 0;
    *a = imin(np_anchor, np_caret); *b = imax(np_anchor, np_caret);
    return 1;
}

/* text position under a point in the client area (clamped to the text) */
static int np_pos_at(int lx, int ly, int cw, int ch) {
    int tx, ty, tw, th; np_text_geom(cw, ch, &tx, &ty, &tw, &th);
    int line = clampi(np_top + (int)floor((ly - ty - 2) / 16.0), 0, np_nlines - 1);
    int col = imax(0, (lx - tx - 4 + 4) / 8);
    return u8_skip_cols(np_buf, np_lines[line], np_lines[line] + np_line_len(line), col);
}

/* the right-click menu: position of item i, and which items can be used */
static int np_ctx_item_y(int i) {
    int y = np_ctx_y + 3;
    for (int k = 0; k < i; k++) y += np_edit_items[k].label[0] == '-' ? 8 : 24;
    return y;
}
static int np_ctx_h(void) { return np_ctx_item_y(NP_CTX_N) + 3 - np_ctx_y; }
static int np_edit_enabled(int i) {
    int a, b, has = np_sel(&a, &b);
    if (i == 0 || i == 1 || i == 3) return has;
    if (i == 2) return clipboard && clip_len > 0;
    return np_len > 0;
}

static void notepad_draw(int cx, int cy, int cw, int ch) {
    int tx, ty, tw, th; np_text_geom(cw, ch, &tx, &ty, &tw, &th);
    int cols = imax(1, (tw - 8) / 8), rows = imax(1, (th - 4) / 16);
    if (cols != np_cols) { np_cols = cols; np_layout(); }
    if (rows != np_rows) { np_rows = rows; np_scroll_to_caret(); }
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == notepad_draw ? hot.b : -1;

    /* text */
    for (int r = 0; r < np_rows && np_top + r < np_nlines; r++) {
        int li = np_top + r, st = np_lines[li], n = np_line_len(li);
        char line[512];
        n = imin(n, (int)sizeof line - 1);
        memcpy(line, np_buf + st, n); line[n] = 0;
        for (int k = 0; k < n; k++) if (line[k] == '\t') line[k] = ' ';
        int x0 = cx + tx + 4, y0 = cy + ty + 2 + r * 16;
        draw_text(x0, y0, line, RGB(0, 0, 0), 255);
        int a, b;
        if (np_sel(&a, &b)) {                             /* selected part: white on blue */
            int s0 = imax(a, st) - st, s1 = imin(b, st + n) - st;
            int nl = b > st + n && st + n < np_len && np_buf[st + n] == '\n';   /* covers the line break */
            if (s1 > s0 || (nl && a <= st + n)) {
                s0 = imax(0, s0); s1 = imax(s0, s1);
                int c0 = u8_cols(line, 0, s0), c1 = c0 + u8_cols(line, s0, s1);
                fill_rect(x0 + 8 * c0, y0, 8 * (c1 - c0) + (nl ? 4 : 0), 16, RGB(0x33, 0x99, 0xff), 255);
                char seg[512];
                memcpy(seg, line + s0, s1 - s0); seg[s1 - s0] = 0;
                draw_text(x0 + 8 * c0, y0, seg, RGB(255, 255, 255), 255);
            }
        }
    }
    /* caret */
    int cl = np_line_of(np_caret);
    if (cl >= np_top && cl < np_top + np_rows && !np_dlg.active)
        fill_rect(cx + tx + 4 + 8 * u8_cols(np_buf, np_lines[cl], np_caret), cy + ty + 2 + (cl - np_top) * 16, 1, 16,
                  RGB(0, 0, 0), 255);

    /* scrollbar */
    int sx = cx + cw - NP_SB_W, sy = cy + ty;
    fill_rect(sx, sy, NP_SB_W, th, RGB(0xe8, 0xe8, 0xec), 255);
    fill_rect(sx, sy, 1, th, RGB(0xd0, 0xd0, 0xd8), 255);
    if (np_nlines > np_rows) {
        int thumb = imax(24, th * np_rows / np_nlines);
        int pos = (th - thumb) * np_top / imax(1, np_nlines - np_rows);
        fill_round_grad(sx + 2, sy + pos + 1, NP_SB_W - 4, thumb - 2, 3,
                        RGB(0xf0, 0xf2, 0xf6), RGB(0xc8, 0xd0, 0xdc), 255);
        stroke_round(sx + 2, sy + pos + 1, NP_SB_W - 4, thumb - 2, 3, RGB(0x90, 0x98, 0xa8), 255);
    }

    mb_draw_bar(&np_mb, cx, cy, cw, hotid);
    mb_draw_drop(&np_mb, cx, cy, hotid);

    if (np_ctx_open) {                                   /* right-click menu */
        int x = cx + np_ctx_x, y = cy + np_ctx_y, h = np_ctx_h();
        fill_rect(x + 3, y + 3, DROP_W, h, RGB(0, 0, 0), 50);
        fill_rect(x, y, DROP_W, h, RGB(0xf5, 0xf5, 0xf5), 255);
        fill_rect(x + 28, y + 2, 1, h - 4, RGB(0xe2, 0xe3, 0xe3), 255);
        fill_rect(x + 29, y + 2, 1, h - 4, RGB(0xff, 0xff, 0xff), 255);
        stroke_round(x, y, DROP_W, h, 1, RGB(0x97, 0x97, 0x97), 255);
        for (int i = 0; i < NP_CTX_N; i++) {
            int iy = cy + np_ctx_item_y(i);
            if (np_edit_items[i].label[0] == '-') {
                fill_rect(x + 32, iy + 3, DROP_W - 36, 1, RGB(0xe0, 0xe0, 0xe0), 255);
                continue;
            }
            int en = np_edit_enabled(i);
            if (en && hotid == ID_NPCTX + i) {
                fill_round(x + 3, iy, DROP_W - 6, 24, 3, RGB(0xe6, 0xf0, 0xfb), 255);
                stroke_round(x + 3, iy, DROP_W - 6, 24, 3, RGB(0xae, 0xcf, 0xf7), 255);
            }
            uint32_t tc = en ? RGB(0x10, 0x10, 0x10) : RGB(0xa0, 0xa0, 0xa0);
            draw_text(x + 36, iy + 4, np_edit_items[i].label, tc, 255);
            const char *ac = np_edit_items[i].accel;
            draw_text(x + DROP_W - 10 - text_w(ac), iy + 4, ac,
                      en ? RGB(0x6d, 0x6d, 0x6d) : RGB(0xb8, 0xb8, 0xb8), 255);
        }
    }
    dlg_draw(&np_dlg, cx, cy, cw, ch, hotid);
}

static int notepad_hit(int lx, int ly, int cw, int ch) {
    if (np_dlg.active) return dlg_hit(&np_dlg, lx, ly, cw, ch);
    if (np_ctx_open) {
        for (int i = 0; i < NP_CTX_N; i++) {
            if (np_edit_items[i].label[0] == '-') continue;
            int y = np_ctx_item_y(i);
            if (lx >= np_ctx_x && lx < np_ctx_x + DROP_W && ly >= y && ly < y + 24) return ID_NPCTX + i;
        }
        return -1;
    }
    return mb_hit(&np_mb, lx, ly);
}

static void notepad_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(notepad_draw); if (i < 0) { *x = *y = *w = *h = 0; return; }
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (dlg_elem_rect(&np_dlg, id, cw, ch, x, y, w, h)) return;
    if (mb_elem_rect(&np_mb, id, x, y, w, h)) return;
    if (id >= ID_NPCTX && id < ID_NPCTX + NP_CTX_N) {
        *x = np_ctx_x; *y = np_ctx_item_y(id - ID_NPCTX); *w = DROP_W; *h = 24; return;
    }
    *x = *y = *w = *h = 0;
}

static void np_changed(void) {
    np_layout(); np_scroll_to_caret();
    repaint_client(notepad_draw);
}

static void np_reset(void) {
    np_len = 0; np_buf[0] = 0; np_caret = 0; np_top = 0; np_modified = 0; np_file[0] = 0;
    snprintf(np_dir, sizeof np_dir, "%s", NP_DIR);
    np_goal_col = -1; np_anchor = -1; np_dragging = 0; np_ctx_open = 0;
    np_layout(); np_set_title();
}

static void np_info(const char *a, const char *b) {
    np_dlg_mode = NPD_INFO;
    dlg_open(&np_dlg, DLG_MSG, "Notepad", a, b, "OK", NULL, NULL, np_dlg_done);
    repaint_client(notepad_draw);
}

static int np_write(void) {                     /* save to np_file; 1 on success */
    char path[320];
    mkdir("/data", 0755); mkdir(NP_DIR, 0755);
    if (strncmp(np_dir, "/data", 5)) snprintf(np_dir, sizeof np_dir, "%s", NP_DIR);   /* system files are read-only */
    snprintf(path, sizeof path, "%s/%s", np_dir, np_file);
    FILE *f = fopen(path, "w");
    if (!f) { np_info("Could not save the file.", "Is the save disk working?"); return 0; }
    fwrite(np_buf, 1, np_len, f);
    fflush(f); fsync(fileno(f)); fclose(f); sync();
    np_modified = 0;
    np_set_title();
    return 1;
}

static void np_load_from(const char *dir, const char *name) {
    char path[320], d[256];
    snprintf(d, sizeof d, "%s", dir);
    if (!strcmp(d, "/")) d[0] = 0;
    snprintf(path, sizeof path, "%s/%s", d, name);
    FILE *f = fopen(path, "r");
    if (!f) { np_info("Could not open the file.", NULL); return; }
    np_reset();
    snprintf(np_dir, sizeof np_dir, "%s", d[0] ? d : "/");
    int c;
    while ((c = fgetc(f)) != EOF && np_len < NP_MAX) if (c != '\r') np_buf[np_len++] = (char)c;
    np_buf[np_len] = 0;
    fclose(f);
    snprintf(np_file, sizeof np_file, "%s", name);
    np_set_title();
    np_changed();
}
static void np_load(const char *name) { np_load_from(NP_DIR, name); }

static void np_saveas_dialog(void) {
    np_dlg_mode = NPD_SAVEAS;
    dlg_open(&np_dlg, DLG_INPUT, "Save As", "File name:", NULL, "Save", "Cancel", NULL, np_dlg_done);
    snprintf(np_dlg.field, sizeof np_dlg.field, "%s", np_file[0] ? np_file : "untitled.txt");
    repaint_client(notepad_draw);
}

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void np_open_dialog(void) {
    np_dlg_mode = NPD_OPEN;
    dlg_open(&np_dlg, DLG_LIST, "Open", "Your documents:", NULL, "Open", "Cancel", NULL, np_dlg_done);
    DIR *dir = opendir(NP_DIR);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) && np_dlg.nitems < DLG_MAXITEMS)
            if (e->d_name[0] != '.')
                snprintf(np_dlg.items[np_dlg.nitems++], sizeof np_dlg.items[0], "%s", e->d_name);
        closedir(dir);
    }
    qsort(np_dlg.items, np_dlg.nitems, sizeof np_dlg.items[0], cmp_str);
    if (np_dlg.nitems) np_dlg.sel = 0;
    repaint_client(notepad_draw);
}

static void np_do(int action) {                 /* carry out an action (no questions asked) */
    np_pending = NPA_NONE;
    if (action == NPA_NEW) { np_reset(); np_changed(); }
    else if (action == NPA_OPEN) np_open_dialog();
    else if (action == NPA_CLOSE) {
        int i = win_of(notepad_draw);
        np_modified = 0;
        if (i >= 0) close_win(i);
    }
}

static void np_request(int action) {            /* ask to save first if needed */
    if (!np_modified) { np_do(action); return; }
    np_pending = action;
    np_dlg_mode = NPD_PROMPT;
    char m[80];
    snprintf(m, sizeof m, "Do you want to save changes to %s?", np_file[0] ? np_file : "Untitled");
    if (strlen(m) > 36) dlg_open(&np_dlg, DLG_MSG, "Notepad", "Do you want to save changes to",
                                 np_file[0] ? np_file : "Untitled", "Save", "Don't Save", "Cancel", np_dlg_done);
    else dlg_open(&np_dlg, DLG_MSG, "Notepad", m, NULL, "Save", "Don't Save", "Cancel", np_dlg_done);
    repaint_client(notepad_draw);
}

static void np_save(void) {
    if (!np_file[0]) { np_saveas_dialog(); return; }
    if (np_write() && np_pending) np_do(np_pending);
}

void np_dlg_done(int b) {
    switch (np_dlg_mode) {
    case NPD_PROMPT:
        if (b == 0) np_save();
        else if (b == 1) np_do(np_pending);
        else np_pending = NPA_NONE;
        break;
    case NPD_SAVEAS:
        if (b != 0) { np_pending = NPA_NONE; break; }
        {
            char name[40];
            snprintf(name, sizeof name, "%s", np_dlg.field);
            while (name[0] == ' ' || name[0] == '.') memmove(name, name + 1, strlen(name));
            if (!name[0]) { np_saveas_dialog(); break; }
            if (!strchr(name, '.') && strlen(name) < sizeof name - 5) strcat(name, ".txt");
            snprintf(np_file, sizeof np_file, "%s", name);
            if (np_write() && np_pending) np_do(np_pending);
        }
        break;
    case NPD_OPEN:
        if (b == 0 && np_dlg.sel >= 0 && np_dlg.sel < np_dlg.nitems) {
            char name[40];
            snprintf(name, sizeof name, "%s", np_dlg.items[np_dlg.sel]);
            np_load(name);
        }
        break;
    }
}

static void np_delete_sel(void) {                 /* remove the selected text */
    int a, b;
    if (!np_sel(&a, &b)) { np_anchor = -1; return; }
    memmove(np_buf + a, np_buf + b, np_len - b + 1);
    np_len -= b - a; np_caret = a; np_anchor = -1; np_modified = 1;
}

static void np_insert(const char *s, int n) {
    np_delete_sel();                                  /* typing replaces the selection */
    if (np_len + n > NP_MAX) n = NP_MAX - np_len;
    if (n <= 0) return;
    memmove(np_buf + np_caret + n, np_buf + np_caret, np_len - np_caret + 1);
    memcpy(np_buf + np_caret, s, n);
    np_len += n; np_caret += n; np_modified = 1;
}

/* Edit actions: 0 cut, 1 copy, 2 paste, 3 delete, 5 select all */
static void np_edit(int which) {
    int a, b, has = np_sel(&a, &b);
    switch (which) {
    case 0: if (has) { clip_set(np_buf + a, b - a); np_delete_sel(); } break;
    case 1: if (has) clip_set(np_buf + a, b - a); return;          /* nothing changes on screen */
    case 2:
        if (clipboard && clip_len) {
            char *t = malloc((size_t)clip_len); int n = 0;
            if (!t) return;
            for (int i = 0; i < clip_len; i++) if (clipboard[i] != '\r') t[n++] = clipboard[i];
            np_insert(t, n);
            free(t);
        }
        break;
    case 3: np_delete_sel(); break;
    case 5: np_anchor = 0; np_caret = np_len; break;
    }
    np_goal_col = -1;
    np_changed();
}

static void np_menu_action(int menu, int item) {
    if (menu == 1) { np_edit(item); return; }
    switch (item) {
    case 0: np_request(NPA_NEW); break;
    case 1: np_request(NPA_OPEN); break;
    case 2: np_save(); break;
    case 3: np_saveas_dialog(); break;
    case 5: np_request(NPA_CLOSE); break;
    }
}

static int np_word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '\''; }

static void notepad_click(int lx, int ly, int cw, int ch) {
    if (dlg_click(&np_dlg, lx, ly, cw, ch, notepad_draw)) return;
    if (np_ctx_open) {                                /* right-click menu is open */
        int id = notepad_hit(lx, ly, cw, ch);
        np_ctx_open = 0;
        repaint_client(notepad_draw);
        if (id >= ID_NPCTX && np_edit_enabled(id - ID_NPCTX)) np_edit(id - ID_NPCTX);
        return;
    }
    int menu, r = mb_click(&np_mb, lx, ly, &menu, notepad_draw);
    if (r >= 0) { np_menu_action(menu, r); return; }
    if (r == -2) return;
    int tx, ty, tw, th; np_text_geom(cw, ch, &tx, &ty, &tw, &th);
    if (lx >= tw || ly < ty) return;                 /* scrollbar / menu bar */
    int pos = np_pos_at(lx, ly, cw, ch);
    long long t = ui_ms();
    if (pos == np_last_pos && t - np_last_click < 450) {        /* double-click: select the word */
        int a = pos, b = pos;
        while (a > 0 && np_word_char(np_buf[a - 1])) a--;
        while (b < np_len && np_word_char(np_buf[b])) b++;
        np_anchor = a; np_caret = b;
        np_last_pos = -1; np_dragging = 0;
    } else {
        if (shift) { if (np_anchor < 0) np_anchor = np_caret; }  /* Shift+click extends */
        else np_anchor = pos;
        np_caret = pos;
        np_dragging = 1;
        np_last_pos = pos; np_last_click = t;
    }
    np_goal_col = -1;
    repaint_client(notepad_draw);
}

/* mouse moved with the button held: stretch the selection */
static void notepad_drag(int lx, int ly, int cw, int ch) {
    if (!np_dragging || np_dlg.active) return;
    int tx, ty, tw, th; np_text_geom(cw, ch, &tx, &ty, &tw, &th);
    if (ly < ty && np_top > 0) np_top--;                         /* scroll while dragging */
    if (ly >= ty + th && np_top + np_rows < np_nlines) np_top++;
    int pos = np_pos_at(clampi(lx, 0, tw - 1), clampi(ly, ty, ty + th - 1), cw, ch);
    if (pos == np_caret) return;
    np_caret = pos;
    repaint_client(notepad_draw);
}

static void notepad_release(void) { np_dragging = 0; }

static void notepad_rclick(int lx, int ly, int cw, int ch) {
    if (np_dlg.active) return;
    np_mb.open = -1;
    int tx, ty, tw, th; np_text_geom(cw, ch, &tx, &ty, &tw, &th);
    if (lx >= tw || ly < ty) return;
    int a, b, pos = np_pos_at(lx, ly, cw, ch);
    if (!np_sel(&a, &b) || pos < a || pos > b) { np_caret = pos; np_anchor = -1; }   /* outside: move there */
    np_ctx_open = 1;
    np_ctx_x = clampi(lx, 0, cw - DROP_W - 4);
    np_ctx_y = ly;
    if (np_ctx_y + np_ctx_h() > ch - 4) np_ctx_y = imax(MENUBAR_H, ly - np_ctx_h());
    repaint_client(notepad_draw);
}

static void notepad_key(int code, int mods) {
    if (dlg_key(&np_dlg, code, mods, notepad_draw)) return;
    if (np_mb.open >= 0 || np_ctx_open) {
        if (code == KEY_ESC) { np_mb.open = -1; np_ctx_open = 0; repaint_client(notepad_draw); }
        return;
    }
    int moving = code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN ||
                 code == KEY_HOME || code == KEY_END || code == KEY_PAGEUP || code == KEY_PAGEDOWN;
    int a, b, has = np_sel(&a, &b);
    if (moving) {                                    /* Shift + move = select */
        if (mods & MOD_SHIFT) { if (np_anchor < 0) np_anchor = np_caret; }
        else {
            np_anchor = -1;
            if (has && (code == KEY_LEFT || code == KEY_RIGHT) && !(mods & MOD_CTRL)) {
                np_caret = code == KEY_LEFT ? a : b;  /* collapse to that end, like Windows */
                np_goal_col = -1; np_changed(); return;
            }
        }
    }
    if (mods & MOD_CTRL) {
        if (code == KEY_S) np_save();
        else if (code == KEY_N) np_request(NPA_NEW);
        else if (code == KEY_O) np_request(NPA_OPEN);
        else if (code == KEY_A) np_edit(5);
        else if (code == KEY_C) np_edit(1);
        else if (code == KEY_X) np_edit(0);
        else if (code == KEY_V) np_edit(2);
        else if (code == KEY_HOME || code == KEY_END) {      /* start / end of document */
            np_caret = code == KEY_HOME ? 0 : np_len;
            np_goal_col = -1;
            np_changed();
        }
        return;
    }
    if (moving && np_anchor == np_caret && !(mods & MOD_SHIFT)) np_anchor = -1;
    int l = np_line_of(np_caret), col = u8_cols(np_buf, np_lines[l], np_caret);
    int vertical = 0;
    switch (code) {
    case KEY_LEFT:  np_caret = u8_prev(np_buf, np_caret); break;
    case KEY_RIGHT: np_caret = u8_next(np_buf, np_len, np_caret); break;
    case KEY_HOME:  np_caret = np_lines[l]; break;
    case KEY_END:   np_caret = np_lines[l] + np_line_len(l); break;
    case KEY_UP: case KEY_DOWN: case KEY_PAGEUP: case KEY_PAGEDOWN: {
        int step = code == KEY_UP ? -1 : code == KEY_DOWN ? 1 : code == KEY_PAGEUP ? -np_rows : np_rows;
        if (np_goal_col < 0) np_goal_col = col;
        int nl = clampi(l + step, 0, np_nlines - 1);
        np_caret = u8_skip_cols(np_buf, np_lines[nl], np_lines[nl] + np_line_len(nl), np_goal_col);
        vertical = 1;
        break;
    }
    case KEY_BACKSPACE:
        if (has) np_delete_sel();
        else if (np_caret > 0) {                      /* the whole letter, even a 2-byte one */
            int p = u8_prev(np_buf, np_caret), d = np_caret - p;
            memmove(np_buf + p, np_buf + np_caret, np_len - np_caret + 1);
            np_caret = p; np_len -= d; np_modified = 1;
        }
        break;
    case KEY_DELETE:
        if (has) np_delete_sel();
        else if (np_caret < np_len) {
            int q = u8_next(np_buf, np_len, np_caret), d = q - np_caret;
            memmove(np_buf + np_caret, np_buf + q, np_len - q + 1);
            np_len -= d; np_modified = 1;
        }
        break;
    case KEY_ENTER: case KEY_KPENTER: np_insert("\n", 1); break;
    case KEY_TAB: np_insert("    ", 4); break;
    default: {
        char s[8]; int n = key_text(code, mods, s);
        if (!n) return;
        np_insert(s, n);
    }
    }
    if (!vertical) np_goal_col = -1;
    np_changed();
}

static void notepad_wheel(int amount) {
    np_top = clampi(np_top - amount * 3, 0, imax(0, np_nlines - np_rows));
    repaint_client(notepad_draw);
}

static int notepad_on_close(void) {
    if (!np_modified) return 1;
    np_request(NPA_CLOSE);
    return 0;
}

/* ===================================================================
 *  MINESWEEPER
 * =================================================================== */
#define MS_T     24             /* tile size */
#define MS_MAXW  30
#define MS_MAXH  16
#define MS_PANEL 34
#define ID_MSTIME 900

static int ms_w = 9, ms_h = 9, ms_mines = 10, ms_level;
static uint8_t ms_mine[MS_MAXH][MS_MAXW], ms_state[MS_MAXH][MS_MAXW], ms_num[MS_MAXH][MS_MAXW];
enum { MS_HIDDEN, MS_OPEN, MS_FLAG };
static int ms_started, ms_over, ms_flags, ms_opened, ms_time, ms_boom_x = -1, ms_boom_y = -1;
static time_t ms_t0;
static Dialog ms_dlg;

static DropItem ms_items[] = {
    { "New game", "F2" }, { "-", "" },
    { "Beginner", "*" }, { "Intermediate", "" }, { "Expert", "" },
    { "-", "" }, { "Exit", "" },
};
static const MenuDef ms_menus[] = { { "Game", ms_items, 7 } };
static MenuBar ms_mb = { ms_menus, 1, -1 };

static void mines_draw(int cx, int cy, int cw, int ch);

static void ms_client_size(int *cw, int *ch) {
    *cw = imax(262, ms_w * MS_T + 24);
    *ch = MENUBAR_H + 10 + ms_h * MS_T + 10 + MS_PANEL;
}
static void ms_board_origin(int cw, int *bx, int *by) { *bx = (cw - ms_w * MS_T) / 2; *by = MENUBAR_H + 10; }

static void ms_new_game(void) {
    memset(ms_mine, 0, sizeof ms_mine); memset(ms_state, 0, sizeof ms_state); memset(ms_num, 0, sizeof ms_num);
    ms_started = ms_over = ms_flags = ms_opened = ms_time = 0;
    ms_boom_x = ms_boom_y = -1;
    ms_dlg.active = 0;
    repaint_client(mines_draw);
}

/* mines are placed on the first click, never on or next to that tile */
static void ms_place_mines(int sx, int sy) {
    int placed = 0;
    while (placed < ms_mines) {
        int x = rand() % ms_w, y = rand() % ms_h;
        if (ms_mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;
        ms_mine[y][x] = 1; placed++;
    }
    for (int y = 0; y < ms_h; y++)
        for (int x = 0; x < ms_w; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < ms_w && ny < ms_h) n += ms_mine[ny][nx];
                }
            ms_num[y][x] = n;
        }
    ms_started = 1; ms_t0 = time(NULL);
}

void ms_dlg_done(int b);

static void ms_lose(int x, int y) {
    ms_over = 1; ms_boom_x = x; ms_boom_y = y;
    for (int j = 0; j < ms_h; j++)
        for (int i = 0; i < ms_w; i++)
            if (ms_mine[j][i] && ms_state[j][i] != MS_FLAG) ms_state[j][i] = MS_OPEN;
    repaint_client(mines_draw);
    dlg_open(&ms_dlg, DLG_MSG, "Game lost", "Sorry, you hit a mine!", "Better luck next time.",
             "Play again", "Exit", NULL, ms_dlg_done);
    repaint_client(mines_draw);
}

static void ms_check_win(void) {
    if (ms_opened != ms_w * ms_h - ms_mines) return;
    ms_over = 2;
    for (int j = 0; j < ms_h; j++)
        for (int i = 0; i < ms_w; i++)
            if (ms_mine[j][i]) ms_state[j][i] = MS_FLAG;
    ms_flags = ms_mines;
    char m[48];
    snprintf(m, sizeof m, "Your time: %d second%s", ms_time, ms_time == 1 ? "" : "s");
    dlg_open(&ms_dlg, DLG_MSG, "Game won", "Congratulations, you won!", m, "Play again", "Exit", NULL,
             ms_dlg_done);
    repaint_client(mines_draw);
}

/* open a tile; empty tiles open their neighbours too (flood fill) */
static void ms_open(int x, int y) {
    static int stack[MS_MAXW * MS_MAXH * 2];
    int sp = 0;
    stack[sp++] = y * MS_MAXW + x;
    while (sp) {
        int v = stack[--sp], cx = v % MS_MAXW, cy = v / MS_MAXW;
        if (ms_state[cy][cx] != MS_HIDDEN) continue;
        ms_state[cy][cx] = MS_OPEN; ms_opened++;
        if (ms_mine[cy][cx]) { ms_lose(cx, cy); return; }
        if (ms_num[cy][cx]) continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (nx >= 0 && ny >= 0 && nx < ms_w && ny < ms_h && ms_state[ny][nx] == MS_HIDDEN)
                    stack[sp++] = ny * MS_MAXW + nx;
            }
    }
}

static void ms_tile_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(mines_draw), cw = 262;
    if (i >= 0) { int cx, cy, chh; client_rect(&wins[i], &cx, &cy, &cw, &chh); }
    int bx, by; ms_board_origin(cw, &bx, &by);
    *x = bx + (id % MS_MAXW) * MS_T; *y = by + (id / MS_MAXW) * MS_T; *w = MS_T; *h = MS_T;
}

static void ms_draw_flag(int x, int y) {
    fill_rect(x + 12, y + 4, 2, 14, RGB(0x30, 0x30, 0x30), 255);           /* pole */
    for (int j = 0; j <= 8; j++) {                                          /* red pennant */
        int len = 8 - abs(j - 4) * 2;
        if (len > 0) fill_rect(x + 12 - len, y + 4 + j, len, 1, RGB(0xe0, 0x20, 0x18), 255);
    }
    fill_rect(x + 7, y + 17, 12, 2, RGB(0x30, 0x30, 0x30), 255);           /* base */
}

static const uint32_t ms_numcol[9] = {
    0, RGB(0x2c, 0x40, 0xc4), RGB(0x1c, 0x78, 0x1c), RGB(0xc0, 0x20, 0x20), RGB(0x10, 0x10, 0x80),
    RGB(0x80, 0x10, 0x10), RGB(0x10, 0x80, 0x80), RGB(0, 0, 0), RGB(0x60, 0x60, 0x60),
};

static void mines_draw(int cx, int cy, int cw, int ch) {
    fill_rect(cx, cy, cw, ch, RGB(0xdc, 0xe6, 0xf2), 255);
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == mines_draw ? hot.b : -1;
    int bx, by; ms_board_origin(cw, &bx, &by);
    bx += cx; by += cy;
    fill_rect(bx - 2, by - 2, ms_w * MS_T + 4, ms_h * MS_T + 4, RGB(0x6c, 0x84, 0xa4), 255);
    for (int y = 0; y < ms_h; y++)
        for (int x = 0; x < ms_w; x++) {
            int tx = bx + x * MS_T, ty = by + y * MS_T;
            if (tx + MS_T <= clip_x0 || tx >= clip_x1 || ty + MS_T <= clip_y0 || ty >= clip_y1) continue;
            int st = ms_state[y][x], hv = hotid == y * MS_MAXW + x && !ms_over && !ms_dlg.active;
            if (st == MS_OPEN) {
                uint32_t bg = (x == ms_boom_x && y == ms_boom_y) ? RGB(0xf0, 0x50, 0x40) : RGB(0xee, 0xf2, 0xf8);
                fill_rect(tx, ty, MS_T, MS_T, bg, 255);
                fill_rect(tx, ty, MS_T, 1, RGB(0xc4, 0xce, 0xdc), 255);
                fill_rect(tx, ty, 1, MS_T, RGB(0xc4, 0xce, 0xdc), 255);
                if (ms_mine[y][x]) draw_mine(tx + MS_T / 2, ty + MS_T / 2, 8);
                else if (ms_num[y][x]) {
                    char t[2] = { (char)('0' + ms_num[y][x]), 0 };
                    draw_text_bold(tx + 8, ty + 4, t, ms_numcol[ms_num[y][x]]);
                }
            } else {                                               /* glossy blue tile */
                uint32_t top = hv ? RGB(0x9c, 0xd2, 0xff) : RGB(0x6c, 0xa4, 0xec);
                uint32_t bot = hv ? RGB(0x3a, 0x8c, 0xe8) : RGB(0x26, 0x5c, 0xc0);
                fill_round_grad(tx + 1, ty + 1, MS_T - 2, MS_T - 2, 3, top, bot, 255);
                fill_rect(tx + 3, ty + 2, MS_T - 6, MS_T / 2 - 3, RGB(255, 255, 255), 55);
                stroke_round(tx + 1, ty + 1, MS_T - 2, MS_T - 2, 3, RGB(0x18, 0x3c, 0x80), 200);
                if (st == MS_FLAG) {
                    int wrong = ms_over == 1 && !ms_mine[y][x];
                    ms_draw_flag(tx, ty);
                    if (wrong) {                                    /* misplaced flag */
                        for (int k = 3; k < MS_T - 3; k++) {
                            fill_rect(tx + k, ty + k, 2, 1, RGB(0, 0, 0), 255);
                            fill_rect(tx + MS_T - k - 2, ty + k, 2, 1, RGB(0, 0, 0), 255);
                        }
                    }
                }
            }
        }

    /* bottom panel: time and mines left */
    int py = cy + ch - MS_PANEL + 2;
    char t[16];
    fill_round_grad(cx + 10, py, 92, 26, 4, RGB(0xfa, 0xfc, 0xff), RGB(0xdc, 0xe6, 0xf4), 255);
    stroke_round(cx + 10, py, 92, 26, 4, RGB(0x8c, 0xa4, 0xc4), 255);
    fill_circle(cx + 26, py + 13, 8, RGB(0xff, 0xff, 0xff), 255);           /* clock */
    stroke_round(cx + 18, py + 5, 17, 17, 8, RGB(0x30, 0x40, 0x58), 255);
    fill_rect(cx + 26, py + 7, 1, 7, RGB(0x30, 0x40, 0x58), 255);
    fill_rect(cx + 26, py + 13, 5, 1, RGB(0x30, 0x40, 0x58), 255);
    snprintf(t, sizeof t, "%d", ms_time);
    draw_text_bold(cx + 44, py + 5, t, RGB(0x10, 0x20, 0x40));
    fill_round_grad(cx + cw - 102, py, 92, 26, 4, RGB(0xfa, 0xfc, 0xff), RGB(0xdc, 0xe6, 0xf4), 255);
    stroke_round(cx + cw - 102, py, 92, 26, 4, RGB(0x8c, 0xa4, 0xc4), 255);
    snprintf(t, sizeof t, "%d", ms_mines - ms_flags);
    draw_text_bold(cx + cw - 94, py + 5, t, RGB(0x10, 0x20, 0x40));
    draw_mine(cx + cw - 26, py + 13, 8);

    mb_draw_bar(&ms_mb, cx, cy, cw, hotid);
    mb_draw_drop(&ms_mb, cx, cy, hotid);
    dlg_draw(&ms_dlg, cx, cy, cw, ch, hotid);
}

static int ms_tile_at(int lx, int ly, int cw, int *tx, int *ty) {
    int bx, by; ms_board_origin(cw, &bx, &by);
    if (lx < bx || ly < by) return 0;
    *tx = (lx - bx) / MS_T; *ty = (ly - by) / MS_T;
    return *tx < ms_w && *ty < ms_h;
}

static int mines_hit(int lx, int ly, int cw, int ch) {
    if (ms_dlg.active) return dlg_hit(&ms_dlg, lx, ly, cw, ch);
    int id = mb_hit(&ms_mb, lx, ly);
    if (id >= 0 || ms_mb.open >= 0) return id;
    int tx, ty;
    if (ms_tile_at(lx, ly, cw, &tx, &ty)) return ty * MS_MAXW + tx;
    return -1;
}

static void mines_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(mines_draw); if (i < 0) { *x = *y = *w = *h = 0; return; }
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (dlg_elem_rect(&ms_dlg, id, cw, ch, x, y, w, h)) return;
    if (mb_elem_rect(&ms_mb, id, x, y, w, h)) return;
    if (id >= 0 && id < MS_MAXW * MS_MAXH) { ms_tile_rect(id, x, y, w, h); return; }
    *x = *y = *w = *h = 0;
}

static void ms_set_level(int level) {
    static const int lv[3][3] = { { 9, 9, 10 }, { 16, 16, 40 }, { 30, 16, 99 } };
    ms_level = level;
    ms_w = lv[level][0]; ms_h = lv[level][1]; ms_mines = lv[level][2];
    for (int k = 0; k < 3; k++) ms_items[2 + k].accel = k == level ? "*" : "";
    ms_new_game();
    int i = win_of(mines_draw);
    if (i >= 0) { int cw, ch; ms_client_size(&cw, &ch); resize_win(i, cw, ch); }
}

void ms_dlg_done(int b) {
    if (b == 0) ms_new_game();
    else { int i = win_of(mines_draw); if (i >= 0) close_win(i); }
}

static void mines_click(int lx, int ly, int cw, int ch) {
    if (dlg_click(&ms_dlg, lx, ly, cw, ch, mines_draw)) return;
    int menu, r = mb_click(&ms_mb, lx, ly, &menu, mines_draw);
    if (r >= 0) {
        if (r == 0) ms_new_game();
        else if (r >= 2 && r <= 4) ms_set_level(r - 2);
        else if (r == 6) { int i = win_of(mines_draw); if (i >= 0) close_win(i); }
        return;
    }
    if (r == -2 || ms_over) return;
    int x, y;
    if (!ms_tile_at(lx, ly, cw, &x, &y)) return;
    if (ms_state[y][x] == MS_FLAG) return;
    if (!ms_started) ms_place_mines(x, y);
    if (ms_state[y][x] == MS_HIDDEN) ms_open(x, y);
    else if (ms_num[y][x]) {                     /* "chord": open around a satisfied number */
        int f = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < ms_w && ny < ms_h) f += ms_state[ny][nx] == MS_FLAG;
            }
        if (f == ms_num[y][x])
            for (int dy = -1; dy <= 1 && !ms_over; dy++)
                for (int dx = -1; dx <= 1 && !ms_over; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < ms_w && ny < ms_h && ms_state[ny][nx] == MS_HIDDEN)
                        ms_open(nx, ny);
                }
    }
    if (!ms_over) ms_check_win();
    repaint_client(mines_draw);
}

static void mines_rclick(int lx, int ly, int cw, int ch) {
    if (ms_dlg.active || ms_over || ms_mb.open >= 0) return;
    int x, y;
    if (!ms_tile_at(lx, ly, cw, &x, &y)) return;
    if (ms_state[y][x] == MS_HIDDEN) { ms_state[y][x] = MS_FLAG; ms_flags++; }
    else if (ms_state[y][x] == MS_FLAG) { ms_state[y][x] = MS_HIDDEN; ms_flags--; }
    else return;
    repaint_client(mines_draw);
}

static void mines_key(int code, int mods) {
    if (dlg_key(&ms_dlg, code, mods, mines_draw)) return;
    if (code == KEY_ESC && ms_mb.open >= 0) { ms_mb.open = -1; repaint_client(mines_draw); }
    if (code == KEY_F2) ms_new_game();
}

static void mines_tick(void) {
    if (!ms_started || ms_over) return;
    int t = (int)imin(999, (int)(time(NULL) - ms_t0));
    if (t != ms_time) {
        ms_time = t;
        int i = win_of(mines_draw); if (i < 0) return;
        int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
        repaint_client_rect(mines_draw, 10, ch - MS_PANEL, 100, MS_PANEL);
    }
}

#include "solitaire.c"
#include "store.c"                /* Software Manager: apps from Flathub */
static void print_open(const char *file);
#include "media_apps.c"           /* Photos, Videos, Music */           /* Klondike Solitaire */
#include "print_ui.c"             /* Print: Photos > Print */
static int media_video_playing(void) { return vd.mp.started && !vd.mp.paused && !vd.mp.ended && win_of(videos_draw) >= 0; }
static int media_music_playing(void) { return mu.mp.started && !mu.mp.paused && !mu.mp.ended && win_of(music_draw) >= 0; }

static void run(const char *cmd);
/* ===================================================================
 *  FILE EXPLORER
 *  Path "" is the "Computer" view. C: is "/" (the system, in memory),
 *  D: is "/data" (the save disk). Changes are only allowed on D:.
 * =================================================================== */
#define EX_NAV_H    40          /* back / up / address bar */
#define EX_CMD_H    30          /* command bar */
#define EX_LEFT_W   180         /* navigation pane */
#define EX_STATUS_H 26
#define EX_HDR_H    24          /* column headers */
#define EX_ROW_H    22
#define EX_MAX      1024
#define EX_SB_W     16

/* element ids */
#define ID_EXBACK  3300
#define ID_EXUP    3301
#define ID_EXCMD   3000         /* + 0 new folder, 1 rename, 2 delete */
#define ID_EXNAV   3100
#define ID_EXCRUMB 3200
#define ID_EXDRIVE 3400

typedef struct { char name[64]; int dir, link; long long size; time_t mtime; } ExItem;
static ExItem ex_items[EX_MAX];
static int  ex_n, ex_sel = -1, ex_scroll;
static unsigned char ex_mark[EX_MAX];          /* selected rows, when several are (ex_multi) */
static int ex_multi, ex_anchor = -1;
static char ex_path[256];                       /* "" = Computer */
static char ex_hist[16][256]; static int ex_nhist;
static char ex_title[64] = "Computer";
static long long ex_sig;                        /* to notice changes on disk */
static long long ex_last_click; static int ex_last_idx = -1;
static Dialog ex_dlg;
static int ex_dlg_mode;
enum { EXD_NEWFOLDER = 1, EXD_RENAME, EXD_DELETE, EXD_INFO, EXD_NEWFILE, EXD_EMPTYBIN };
/* right-click > New: the kinds of empty file ccos can make */
static const struct { const char *label, *name, *body; int len, icon, exec; } ex_newt[] = {
    { "Text Document", "New Text Document.txt", "", 0, ICON_NOTE, 0 },
    { "Web Page", "New Web Page.html",
      "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>New page</title>\n</head>\n<body>\n\n</body>\n</html>\n", -1, ICON_FILE, 0 },
    { "Shell Script", "New Script.sh", "#!/bin/sh\n\n", -1, ICON_TERMINAL, 1 },
    { "Markdown Notes", "New Notes.md", "# Notes\n\n", -1, ICON_NOTE, 0 },
    { "CSV Spreadsheet", "New Table.csv", "", 0, ICON_FILE, 0 },
    { "Zip File", "New Zip File.zip", "PK\x05\x06\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 22, ICON_ZIP, 0 },
};
#define EX_NNEWT (int)(sizeof ex_newt / sizeof ex_newt[0])
static int ex_new_kind;

static void explorer_draw(int cx, int cy, int cw, int ch);
void ex_dlg_done(int b);

#define MAXRD 8
static struct { char dev[16], name[40], mnt[128], fs[12]; int ours, ro; } rdrv[MAXRD];
static int nrd;
static struct { const char *label, *path; int icon, indent; } ex_nav[8 + MAXRD] = {
    { "Documents", "/data/Documents", ICON_FOLDER, 0 },
    { "Images", "/data/Images", ICON_FOLDER_IMG, 0 },
    { "Downloads", "/apps/home/Downloads", ICON_FOLDER_DL, 0 },
    { "Videos", "/data/Videos", ICON_FOLDER_VID, 0 },
    { "Recycle Bin", "/data/.bin", ICON_BIN, 0 },
    { "Computer", "", ICON_COMPUTER, 0 },
    { "ccos System (C:)", "/", ICON_DRIVE, 1 },
    { "ccos Data (D:)", "/data", ICON_DRIVE, 1 },
};
static int ex_nnav = 8;
#define EX_NNAV ex_nnav
static void ex_nav_rebuild(void) {               /* the USB drives under Computer */
    ex_nnav = 8;
    for (int i = 0; i < nrd && ex_nnav < 8 + MAXRD; i++) {
        ex_nav[ex_nnav].label = rdrv[i].name; ex_nav[ex_nnav].path = rdrv[i].mnt;
        ex_nav[ex_nnav].icon = ICON_USB; ex_nav[ex_nnav].indent = 1; ex_nnav++;
    }
}
static int ex_nav_y(int i) { return EX_NAV_H + EX_CMD_H + 30 + i * 24 + (i >= 4 ? 22 : 0); }

/* ---- USB drives ------------------------------------------------------
 * Every couple of seconds we look for new disks (sda, sdb...). Each
 * partition we can read (FAT, exFAT, NTFS, ext4, CD/ISO) is mounted at
 * /media/NAME. Ejecting writes everything, unmounts, and powers it off. */
static void ex_nav_rebuild(void);
static long long ex_read(void);
static int rd_of_path(const char *p);
static void ex_go(const char *path, int remember);
static int rd_mounted_at(const char *dev, char *mnt, size_t n) {      /* already mounted somewhere? */
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char d[256], m[256], ty[32]; int found = 0;
    char want[32]; snprintf(want, sizeof want, "/dev/%s", dev);
    while (fscanf(f, "%255s %255s %31s %*[^\n]", d, m, ty) == 3)
        if (!strcmp(d, want)) { snprintf(mnt, n, "%s", m); found = 1; break; }
    fclose(f);
    return found;
}
static void rd_label(const char *dev, char *out, size_t n) {        /* the drive's name, from blkid */
    out[0] = 0;
    char cmd[64]; snprintf(cmd, sizeof cmd, "blkid /dev/%s 2>/dev/null", dev);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char line[512];
    if (fgets(line, sizeof line, p)) {
        char *l = strstr(line, " LABEL=\"");
        if (l) { l += 8; char *e = strchr(l, '"'); if (e) { *e = 0; snprintf(out, n, "%s", l); } }
    }
    pclose(p);
}
static int rd_find(const char *dev) { for (int i = 0; i < nrd; i++) if (!strcmp(rdrv[i].dev, dev)) return i; return -1; }
static void drives_poll(void) {
    /* which partitions are there now? */
    char now[24][16]; int nn = 0;
    DIR *d = opendir("/sys/class/block");
    struct dirent *e;
    while (d && (e = readdir(d)) && nn < 24) {
        if (strncmp(e->d_name, "sd", 2) || strlen(e->d_name) > 6) continue;
        char p[128];
        int part = isdigit((unsigned char)e->d_name[strlen(e->d_name) - 1]);
        if (!part) {                                     /* a whole disk: only if it has no partitions */
            snprintf(p, sizeof p, "/sys/class/block/%s/%s1", e->d_name, e->d_name);
            if (access(p, F_OK) == 0) continue;
            snprintf(p, sizeof p, "/sys/class/block/%s/size", e->d_name);
            FILE *f = fopen(p, "r"); long long sz = 0; if (f) { if (fscanf(f, "%lld", &sz) != 1) sz = 0; fclose(f); }
            if (sz == 0) continue;                       /* a card reader with no card */
        }
        snprintf(now[nn++], 16, "%s", e->d_name);
    }
    if (d) closedir(d);
    int changed = 0;
    /* gone: pulled out without ejecting */
    for (int i = nrd - 1; i >= 0; i--) {
        int there = 0;
        for (int k = 0; k < nn; k++) if (!strcmp(now[k], rdrv[i].dev)) there = 1;
        if (there) continue;
        if (rdrv[i].ours) { umount2(rdrv[i].mnt, MNT_DETACH); rmdir(rdrv[i].mnt); }
        if (rd_of_path(ex_path) == i) ex_path[0] = 0, ex_title[0] = 0;
        memmove(rdrv + i, rdrv + i + 1, sizeof rdrv[0] * (size_t)(nrd - i - 1)); nrd--;
        changed = 1;
    }
    /* new ones */
    for (int k = 0; k < nn && nrd < MAXRD; k++) {
        if (rd_find(now[k]) >= 0) continue;
        static char tried[24][16]; static int ntried;                 /* unreadable: don't try every 2 s */
        int skip = 0; for (int j = 0; j < ntried; j++) if (!strcmp(tried[j], now[k])) skip = 1;
        if (skip) continue;
        char mnt[128] = "", lab[40];
        rd_label(now[k], lab, sizeof lab);
        if (!strcmp(lab, "ccos-store") && !rd_mounted_at(now[k], mnt, sizeof mnt)) {   /* ccos's own files: ccos-savedisk mounts it */
            if (ntried < 24) snprintf(tried[ntried++], 16, "%s", now[k]);
            continue;
        }
        int ours = 0, ro = 0;
        char fs[12] = "";
        if (rd_mounted_at(now[k], mnt, sizeof mnt)) {
            if (!strcmp(mnt, "/data") || !strcmp(mnt, "/apps") || !strcmp(mnt, "/mnt/store") ||
                !strcmp(mnt, "/mnt/usb")) continue;                    /* ccos's own stick: that's C: and D: */
        } else {
            char base[40]; snprintf(base, sizeof base, "%s", lab[0] ? lab : "USB Drive");
            for (char *c = base; *c; c++) if (*c == '/') *c = '_';
            mkdir("/media", 0755);
            snprintf(mnt, sizeof mnt, "/media/%s", base);
            for (int n = 2; access(mnt, F_OK) == 0 && n < 20; n++) snprintf(mnt, sizeof mnt, "/media/%s %d", base, n);
            mkdir(mnt, 0755);
            char dev[32]; snprintf(dev, sizeof dev, "/dev/%s", now[k]);
            static const char *types[] = { "vfat", "exfat", "ntfs3", "ext4", "iso9660", "udf", NULL };
            static const char *opts[] = { "utf8=1,shortname=mixed,flush", "iocharset=utf8", "iocharset=utf8,prealloc", "", "", "", NULL };
            int ok = 0;
            for (int i = 0; types[i] && !ok; i++) {
                if (mount(dev, mnt, types[i], MS_NOATIME, opts[i]) == 0) { ok = 1; snprintf(fs, sizeof fs, "%s", types[i]); }
                else if (errno == EROFS || errno == EACCES) {
                    if (mount(dev, mnt, types[i], MS_RDONLY, opts[i]) == 0) { ok = 1; ro = 1; snprintf(fs, sizeof fs, "%s", types[i]); }
                }
            }
            if (!ok) { rmdir(mnt); if (ntried < 24) snprintf(tried[ntried++], 16, "%s", now[k]); continue; }
            ours = 1;
            /* a ccos stick while the save disk isn't found yet: leave it for ccos-savedisk (10 s) */
            char ct[200]; snprintf(ct, sizeof ct, "%s/ccos/ccos.txt", mnt);
            /* the stick ccos started from: never ours to mount, or ccos-savedisk can't use it
               (Linux won't mount it twice). It keeps looking and takes it as soon as it can. */
            static int live = -1;
            if (live < 0) { FILE *cf = fopen("/proc/cmdline", "r"); char cl[1024] = "";
                            if (cf) { if (!fgets(cl, sizeof cl, cf)) cl[0] = 0; fclose(cf); } live = strstr(cl, "ccos.live") != NULL; }
            if (!conf_persistent && access(ct, F_OK) == 0 && live) { umount2(mnt, MNT_DETACH); rmdir(mnt); continue; }
        }
        int i = nrd++;
        snprintf(rdrv[i].dev, sizeof rdrv[i].dev, "%s", now[k]);
        snprintf(rdrv[i].mnt, sizeof rdrv[i].mnt, "%s", mnt);
        snprintf(rdrv[i].fs, sizeof rdrv[i].fs, "%s", fs);
        char ccos_txt[200]; snprintf(ccos_txt, sizeof ccos_txt, "%s/ccos/ccos.txt", mnt);
        if (access(ccos_txt, F_OK) == 0) snprintf(rdrv[i].name, sizeof rdrv[i].name, "ccos USB");
        else snprintf(rdrv[i].name, sizeof rdrv[i].name, "%s", lab[0] ? lab : "USB Drive");
        rdrv[i].ours = ours; rdrv[i].ro = ro;
        changed = 1;
        if (strcmp(rdrv[i].name, "ccos USB")) {
            char b[120]; snprintf(b, sizeof b, "\"%s\" is ready. Open it in File Explorer.", rdrv[i].name);
            toast(ICON_USB, "USB drive connected", b);
        }
    }
    if (changed) {
        ex_nav_rebuild();
        if (win_of(explorer_draw) >= 0) {
            if (!ex_path[0] && strcmp(ex_title, "Computer")) ex_go("", 0);     /* we were on a drive that's gone */
            else { if (!ex_path[0]) ex_sel = -1; ex_sig = ex_read(); repaint_client(explorer_draw); }
        }
    }
}
/* eject: write everything, unmount, power it off */
static int drive_eject(int i) {
    if (i < 0 || i >= nrd) return 0;
    if (!strcmp(rdrv[i].mnt, "/mnt/usb") && conf_persistent) {
        toast(ICON_USB, "Can't eject this drive", "ccos is running from it and keeps your files on it.");
        return 0;
    }
    sync();
    if (umount2(rdrv[i].mnt, 0) != 0) {
        if (errno == EBUSY) { toast(ICON_USB, "Can't eject yet", "A program is still using the drive. Close it and try again."); return 0; }
    }
    if (rdrv[i].ours) rmdir(rdrv[i].mnt);
    char disk[16]; snprintf(disk, sizeof disk, "%s", rdrv[i].dev);
    for (char *c = disk + 2; *c; c++) if (isdigit((unsigned char)*c)) { *c = 0; break; }
    /* other partitions of the same stick go too */
    for (int k = nrd - 1; k >= 0; k--)
        if (k != i && !strncmp(rdrv[k].dev, disk, strlen(disk))) { sync(); umount2(rdrv[k].mnt, MNT_DETACH); if (rdrv[k].ours) rmdir(rdrv[k].mnt); }
    char p[96]; snprintf(p, sizeof p, "/sys/block/%s/device/delete", disk);
    FILE *f = fopen(p, "w"); if (f) { fputs("1", f); fclose(f); }
    char b[120]; snprintf(b, sizeof b, "You can now safely remove \"%s\".", rdrv[i].name);
    toast(ICON_USB, "Safe to remove", b);
    if (rd_of_path(ex_path) == i) ex_path[0] = 0, ex_title[0] = 0;
    drives_poll();
    return 1;
}
static int rd_of_path(const char *p) {          /* which USB drive a path is on, or -1 */
    for (int i = 0; i < nrd; i++) {
        size_t L = strlen(rdrv[i].mnt);
        if (!strncmp(p, rdrv[i].mnt, L) && (p[L] == 0 || p[L] == '/')) return i;
    }
    return -1;
}

/* the special folders always exist (made when needed) */
#include "flathub.c"             /* Downloads > Flathub: a launcher for each installed app */
static void ex_make_special(void) {
    mkdir("/data", 0755); mkdir("/data/Documents", 0755); mkdir("/data/Images", 0755); mkdir("/data/Videos", 0755);
    mkdir(dl_dir(), 0755);
    ex_nav[2].path = dl_dir();
}
static int ex_folder_icon(const char *path) {          /* which folder picture for this path */
    if (!strcmp(path, "/data/Images")) return ICON_FOLDER_IMG;
    if (!strcmp(path, "/data/Videos")) return ICON_FOLDER_VID;
    if (!strcmp(path, "/apps/home/Downloads") || !strcmp(path, "/data/Downloads") || !strcmp(path, "/data/home/Downloads") || !strcmp(path, "/root/Downloads")) return ICON_FOLDER_DL;
    return ICON_FOLDER;
}
static int rd_of_path(const char *p);
static int ex_on_d(const char *p) {     /* where changes are allowed: D:, Downloads, USB drives */
    { int r = rd_of_path(p); if (r >= 0) return !rdrv[r].ro; }
    return !strcmp(p, "/data") || !strncmp(p, "/data/", 6) || !strcmp(p, "/apps/home/Downloads") || !strncmp(p, "/apps/home/Downloads/", 21);
}
static int ex_writable(void) { return ex_path[0] && ex_on_d(ex_path); }

static int ex_is_text(const char *path, long long size) {
    if (size > NP_MAX) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char b[512];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) if (b[i] == 0) return 0;
    return 1;
}

static int ex_is_zip(const char *name);
static int ex_is_archive(const char *name);
static const char *ex_type(const ExItem *it) {
    if (!it->dir && ex_is_zip(it->name)) return "Zip archive";
    if (!it->dir && ex_is_archive(it->name)) return "Archive";
    if (!it->dir && is_launcher(it->name)) return "Application";
    if (!it->dir && is_ccupd(it->name)) return "ccos Update";
    static char t[32];
    if (it->dir) return "File folder";
    const char *dot = strrchr(it->name, '.');
    if (dot && !strcasecmp(dot, ".txt")) return "Text Document";
    if (dot && !strcasecmp(dot, ".conf")) return "Settings File";
    { int mk = media_kind(it->name); if (mk == MK_IMAGE) return "Picture"; if (mk == MK_VIDEO) return "Video"; if (mk == MK_AUDIO) return "Sound"; }
    if (dot && dot[1] && strlen(dot) < 8) {
        snprintf(t, sizeof t, "%s File", dot + 1);
        for (char *p = t; *p && *p != ' '; p++) *p = toupper((unsigned char)*p);
        return t;
    }
    return it->link ? "Shortcut" : "File";
}

static int ex_cmp(const void *a, const void *b) {
    const ExItem *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;          /* folders first */
    return strcasecmp(x->name, y->name);
}

static void ex_child(char *out, size_t n, const char *name) {
    if (!strcmp(ex_path, "/")) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", ex_path, name);
}

/* read the folder; returns a signature so changes can be noticed */
static long long ex_read(void) {
    ex_n = 0;
    long long sig = 0;
    if (!ex_path[0]) return 0;
    DIR *d = opendir(ex_path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) && ex_n < EX_MAX) {
        if (e->d_name[0] == '.') continue;
        if (!strcmp(ex_path, "/data") && !strcmp(e->d_name, "lost+found")) continue;
        ExItem *it = &ex_items[ex_n];
        snprintf(it->name, sizeof it->name, "%s", e->d_name);
        char p[512]; ex_child(p, sizeof p, e->d_name);
        struct stat st, lst;
        if (lstat(p, &lst)) continue;
        it->link = S_ISLNK(lst.st_mode);
        if (stat(p, &st)) st = lst;
        it->dir = S_ISDIR(st.st_mode);
        it->size = st.st_size; it->mtime = lst.st_mtime;
        sig = sig * 31 + it->size + it->mtime + (unsigned char)it->name[0] * 7 + (long long)strlen(it->name);
        ex_n++;
    }
    closedir(d);
    qsort(ex_items, ex_n, sizeof *ex_items, ex_cmp);
    return sig + ex_n;
}

static void ex_set_title(void) {
    if (!ex_path[0]) snprintf(ex_title, sizeof ex_title, "Computer");
    else if (!strcmp(ex_path, "/")) snprintf(ex_title, sizeof ex_title, "ccos System (C:)");
    else if (!strcmp(ex_path, "/data")) snprintf(ex_title, sizeof ex_title, "ccos Data (D:)");
    else if (!strcmp(ex_path, BIN_DIR)) snprintf(ex_title, sizeof ex_title, "Recycle Bin");
    else if (rd_of_path(ex_path) >= 0 && !strcmp(ex_path, rdrv[rd_of_path(ex_path)].mnt)) snprintf(ex_title, sizeof ex_title, "%s", rdrv[rd_of_path(ex_path)].name);
    else snprintf(ex_title, sizeof ex_title, "%s", strrchr(ex_path, '/') + 1);
    int i = win_of(explorer_draw);
    if (i >= 0) { repaint_window(i); repaint_taskbar(); }
}

static void ex_go(const char *path, int remember) {
    if (remember && ex_nhist < 16 && strcmp(path, ex_path)) {
        snprintf(ex_hist[ex_nhist++], sizeof ex_hist[0], "%s", ex_path);
    }
    snprintf(ex_path, sizeof ex_path, "%s", path);
    if (!strcmp(ex_path, "/data/Documents")) { mkdir("/data", 0755); mkdir(NP_DIR, 0755); }
    ex_make_special();
    ex_sig = ex_read();
    ex_sel = -1; ex_multi = 0; ex_scroll = 0; ex_last_idx = -1;
    ex_set_title();
    repaint_client(explorer_draw);
}

static void ex_parent(char *out, size_t n) {
    if (!strcmp(ex_path, "/") || !strcmp(ex_path, "/data") || !ex_path[0] || !strncmp(ex_path, BIN_DIR, strlen(BIN_DIR))) { out[0] = 0; return; }
    { int r = rd_of_path(ex_path); if (r >= 0 && !strcmp(ex_path, rdrv[r].mnt)) { out[0] = 0; return; } }
    snprintf(out, n, "%s", ex_path);
    char *s = strrchr(out, '/');
    if (s == out) out[1] = 0; else *s = 0;
}

static void ex_info(const char *a, const char *b);
static void ex_need_writable(void);
static void ex_select_name(const char *name);
static void ex_fmt_size(double b, char *out, size_t n);
static void explorer_draw(int cx, int cy, int cw, int ch);
#include "fileops.c"             /* several selected, Cut/Copy/Paste, zip files */

/* ---- geometry ---------------------------------------------------- */
static void ex_main(int cw, int ch, int *x, int *y, int *w, int *h) {
    *x = EX_LEFT_W; *y = EX_NAV_H + EX_CMD_H; *w = cw - EX_LEFT_W; *h = ch - *y - EX_STATUS_H;
}
static int ex_rows(int cw, int ch) { int x, y, w, h; ex_main(cw, ch, &x, &y, &w, &h); return imax(1, (h - EX_HDR_H) / EX_ROW_H); }
#define EX_NCMD 6
static int ex_in_bin(void) { return !strcmp(ex_path, BIN_DIR); }
static const char *ex_cmd_labels[2][6] = {
    { "New folder", "Cut", "Copy", "Paste", "Rename", "Delete" },
    { "Restore", "Restore all", "Empty Recycle Bin", "", "", "Delete" },      /* in the Recycle Bin */
};
#define ex_cmd_label ex_cmd_labels[ex_in_bin()]
static void ex_cmd_rect(int i, int *x, int *y, int *w, int *h) {
    int xx = 8;
    for (int k = 0; k < i; k++) if (ex_cmd_label[k][0]) xx += text_w(ex_cmd_label[k]) + 22 + 4;
    *x = xx; *y = EX_NAV_H + 3; *w = ex_cmd_label[i][0] ? text_w(ex_cmd_label[i]) + 22 : 0; *h = EX_CMD_H - 6;
}
static int ex_cmd_enabled(int i) {
    int ns = ex_path[0] ? ex_nselected() : 0;
    if (ex_in_bin()) return i == 0 || i == 5 ? ns > 0 : (i == 1 || i == 2) ? ex_n > 0 : 0;
    switch (i) {
    case 0: return ex_writable();
    case 1: return ex_writable() && ns > 0;
    case 2: return ns > 0;
    case 3: return ex_writable() && fclip.n > 0;
    case 4: return ex_writable() && ns == 1;
    default: return ex_writable() && ns > 0;
    }
}
#define EX_NDRIVE (2 + nrd)            /* C:, D:, then USB drives */
static void ex_drive_rect(int i, int cw, int *x, int *y, int *w, int *h) {
    int per = imax(1, (cw - EX_LEFT_W - 16) / 270);
    int j = i < 2 ? i : i - 2, top = EX_NAV_H + EX_CMD_H + 40;
    if (i >= 2) top += ((1 + per) / per) * 76 + 30;    /* "Removable Drives" below the hard disks */
    *x = EX_LEFT_W + 16 + (j % per) * 270; *y = top + (j / per) * 76; *w = 256; *h = 64;
}

/* address bar pieces: Computer > drive > folders */
#define EX_MAXCRUMB 10
static int ex_crumbs(char labels[][40], char paths[][256]) {
    int n = 0;
    snprintf(labels[n], 40, "Computer"); paths[n][0] = 0; n++;
    if (!ex_path[0]) return n;
    if (!strncmp(ex_path, BIN_DIR, strlen(BIN_DIR))) {                 /* the Bin is its own place */
        n = 0; snprintf(labels[n], 40, "Recycle Bin"); snprintf(paths[n], 256, BIN_DIR); n++;
        return n;
    }
    int u = rd_of_path(ex_path);
    int d = u < 0 && ex_on_d(ex_path);
    if (!strncmp(ex_path, "/apps/home/Downloads", 20)) d = 0;   /* that one lives on C: */
    const char *base = u >= 0 ? rdrv[u].mnt : d ? "/data" : "";
    snprintf(labels[n], 40, "%s", u >= 0 ? rdrv[u].name : d ? "ccos Data (D:)" : "ccos System (C:)");
    snprintf(paths[n], 256, "%s", base[0] ? base : "/"); n++;
    const char *rest = ex_path + strlen(base);
    char built[256]; snprintf(built, sizeof built, "%s", base);
    while (*rest && n < EX_MAXCRUMB) {
        if (*rest == '/') rest++;
        const char *e = strchr(rest, '/');
        int len = e ? (int)(e - rest) : (int)strlen(rest);
        if (!len) break;
        snprintf(labels[n], 40, "%.*s", len, rest);
        size_t bl = strlen(built);
        snprintf(built + bl, sizeof built - bl, "/%.*s", len, rest);
        snprintf(paths[n], 256, "%s", built);
        n++;
        rest += len;
    }
    return n;
}
static int ex_crumb_x(char labels[][40], int i) {
    int x = 104 + 24;
    for (int k = 0; k < i; k++) x += text_w(labels[k]) + 12 + 14;
    return x;
}

/* ---- icons (also used by the start menu) --------------------------- */
static void draw_folder_icon(int cx, int cy, int r) {
    int w = r * 2, h = r * 3 / 2, x = cx - r, y = cy - h / 2;
    fill_round(x, y - 2, w / 2, 5, 2, RGB(0xd8, 0xa8, 0x3c), 255);                     /* tab */
    fill_round_grad(x, y, w, h, 2, RGB(0xfc, 0xe4, 0x98), RGB(0xe8, 0xb0, 0x40), 255);
    fill_rect(x + 1, y + 2, w - 2, 2, RGB(255, 255, 255), 80);
    stroke_round(x, y, w, h, 2, RGB(0xb0, 0x80, 0x24), 230);
}
/* the special folders: blue Images, green Downloads, purple Videos, each with a little sign */
static void draw_folder_tinted(int cx, int cy, int r, int kind) {
    uint32_t tab, top, bot, edge;
    if (kind == ICON_FOLDER_IMG)     { tab = RGB(0x3a, 0x7c, 0xd0); top = RGB(0xa8, 0xd4, 0xff); bot = RGB(0x4a, 0x90, 0xe8); edge = RGB(0x1e, 0x56, 0xa8); }
    else if (kind == ICON_FOLDER_DL) { tab = RGB(0x3a, 0xa0, 0x48); top = RGB(0xb4, 0xf0, 0xa8); bot = RGB(0x4c, 0xb8, 0x50); edge = RGB(0x24, 0x7c, 0x30); }
    else                             { tab = RGB(0x84, 0x48, 0xc8); top = RGB(0xdc, 0xbc, 0xff); bot = RGB(0x98, 0x5c, 0xe0); edge = RGB(0x60, 0x2c, 0xa0); }
    int w = r * 2, h = r * 3 / 2, x = cx - r, y = cy - h / 2;
    fill_round(x, y - 2, w / 2, 5, 2, tab, 255);
    fill_round_grad(x, y, w, h, 2, top, bot, 255);
    fill_rect(x + 1, y + 2, w - 2, 2, RGB(255, 255, 255), 80);
    stroke_round(x, y, w, h, 2, edge, 230);
    /* the sign, white, in the middle of the folder */
    uint32_t wc = RGB(255, 255, 255);
    int s = imax(2, r * 2 / 5), mx = cx, my = cy + h / 10;
    if (kind == ICON_FOLDER_IMG) {                      /* a mountain and a sun */
        for (int k = 0; k <= s; k++) fill_rect(mx - s + k / 2 - 1, my + s / 2 - k + s / 2, (s - k) + 1 + k / 3, 1, wc, 230);
        fill_circle(mx + s * 2 / 3, my - s / 2, imax(1, s / 3), wc, 230);
        if (r >= 10) stroke_round(mx - s - 2, my - s, 2 * s + 4, 2 * s - 1, 1, wc, 200);
    } else if (kind == ICON_FOLDER_DL) {                /* an arrow pointing down */
        int st = imax(1, s / 3);
        fill_rect(mx - st / 2, my - s, st + (r >= 10), s, wc, 240);
        for (int k = 0; k <= s * 2 / 3; k++) fill_rect(mx - s * 2 / 3 + k, my + k - s / 6, 2 * (s * 2 / 3 - k) + 1, 1, wc, 240);
        if (r >= 10) fill_rect(mx - s, my + s * 2 / 3 + 1, 2 * s + 1, imax(1, st / 2 + 1), wc, 220);
    } else {                                            /* a play triangle */
        for (int k = -s * 3 / 4; k <= s * 3 / 4; k++) fill_rect(mx - s / 2, my + k, (s * 3 / 4 - abs(k)) * 5 / 4 + 1, 1, wc, 240);
    }
}
static void draw_file_icon(int cx, int cy, int r) {
    int w = r * 3 / 2, h = r * 2, x = cx - w / 2, y = cy - r, f = imax(3, r / 2);
    fill_rect(x, y, w - f, h, RGB(0xfc, 0xfc, 0xfc), 255);
    fill_rect(x + w - f, y + f, f, h - f, RGB(0xfc, 0xfc, 0xfc), 255);
    fill_rect(x + w - f, y, 1, f, RGB(0x8c, 0x96, 0xa4), 255);                         /* folded corner */
    fill_rect(x + w - f, y + f, f, 1, RGB(0x8c, 0x96, 0xa4), 255);
    fill_rect(x, y, w - f, 1, RGB(0x8c, 0x96, 0xa4), 255); fill_rect(x, y, 1, h, RGB(0x8c, 0x96, 0xa4), 255);
    fill_rect(x, y + h - 1, w, 1, RGB(0x8c, 0x96, 0xa4), 255);
    fill_rect(x + w - 1, y + f, 1, h - f, RGB(0x8c, 0x96, 0xa4), 255);
    for (int k = 0; k < f; k++) fill_rect(x + w - f + k, y + k, 1, 1, RGB(0x8c, 0x96, 0xa4), 255);
}
static void draw_drive_icon(int cx, int cy, int r) {
    int w = r * 2, h = r, x = cx - r, y = cy - h / 2;
    fill_round_grad(x, y, w, h, 3, RGB(0xe8, 0xec, 0xf0), RGB(0x90, 0x9a, 0xa8), 255);
    stroke_round(x, y, w, h, 3, RGB(0x40, 0x48, 0x54), 230);
    fill_rect(x + 3, y + h - 5, w / 2, 2, RGB(0x50, 0x58, 0x64), 255);
    fill_circle(x + w - 5, y + h - 4, 1.6f, RGB(0x30, 0xd0, 0x40), 255);                /* activity light */
}
/* a battery standing up, green */
static void draw_battery_icon(int cx, int cy, int r) {
    int w = r, h = r * 2 - 2, x = cx - w / 2, y = cy - h / 2 + 1;
    fill_round(cx - r / 4, y - 3, r / 2, 4, 1, RGB(0x50, 0x58, 0x64), 255);
    fill_round_grad(x, y, w, h, 3, RGB(0xf4, 0xf6, 0xf8), RGB(0xb8, 0xc0, 0xc8), 255);
    stroke_round(x, y, w, h, 3, RGB(0x40, 0x48, 0x54), 255);
    fill_round_grad(x + 2, y + h / 3, w - 4, h - h / 3 - 2, 2, RGB(0x70, 0xe0, 0x70), RGB(0x20, 0x98, 0x30), 255);
}
/* a crescent moon: sleep */
static void draw_moon_icon(int cx, int cy, int r) {
    float R = r * 0.9f;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            float d1 = sqrtf((float)(x * x + y * y)), dx = x - R * 0.45f, dy = y + R * 0.3f, d2 = sqrtf(dx * dx + dy * dy);
            float a1 = clampf(R - d1 + 0.5f, 0, 1), a2 = clampf(d2 - R * 0.78f + 0.5f, 0, 1);
            int a = (int)(255 * a1 * a2);
            if (a > 0) blend(cx + x, cy + y, mix(RGB(0xff, 0xe8, 0x80), RGB(0xe0, 0xa8, 0x20), (y + r) / (2.0f * r)), a);
        }
}
/* a zip file: a folder with a zipper down the middle */
/* a glass waste basket; full: crumpled paper in it */
static void draw_bin_icon(int cx, int cy, int r, int full) {
    float top = cy - r * 0.62f, bot = cy + r * 0.92f, wt = r * 0.78f, wb = r * 0.56f;
    int x0 = cx - r - 1, y0 = cy - r - 2, x1 = cx + r + 2, y1 = cy + r + 2;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    if (full) {                                                      /* paper first, behind the front glass */
        fill_circle(cx - r * 0.28f, top - r * 0.08f, r * 0.34f, RGB(0xf4, 0xf4, 0xf0), 255);
        fill_circle(cx + r * 0.22f, top - r * 0.16f, r * 0.30f, RGB(0xe4, 0xec, 0xf6), 255);
        fill_circle(cx + r * 0.02f, top + r * 0.10f, r * 0.30f, RGB(0xfa, 0xfa, 0xf6), 255);
        aa_line(cx - r * 0.45f, top - r * 0.05f, cx - r * 0.12f, top - r * 0.22f, 1.0f, RGB(0xb8, 0xb8, 0xb0));
        aa_line(cx + r * 0.05f, top - r * 0.3f, cx + r * 0.38f, top - r * 0.05f, 1.0f, RGB(0xa8, 0xb4, 0xc4));
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f - cx, py = y + 0.5f;
            if (py < top || py > bot) continue;
            float k = (py - top) / (bot - top), hw = wt + (wb - wt) * k;
            float cov = clampf(hw + 0.5f - fabsf(px), 0, 1) * clampf(bot + 0.5f - py, 0, 1);
            if (cov <= 0) continue;
            float u = (px / hw + 1) / 2;                              /* 0 left .. 1 right */
            uint32_t c = mix(RGB(0xd4, 0xe4, 0xf2), RGB(0x6c, 0x88, 0xa8), u * 0.8f + k * 0.2f);
            int a = 150;
            float rib = fmodf(u * 7.0f, 1.0f);
            if (rib < 0.12f) { c = mix(c, RGB(0x50, 0x68, 0x88), 0.5f); a = 200; }
            if (u < 0.28f && u > 0.14f) { c = mix(c, RGB(255, 255, 255), 0.5f); a = 190; }   /* shine */
            if (hw - fabsf(px) < 1.2f || bot - py < 1.4f) { c = RGB(0x4c, 0x60, 0x7c); a = 230; }
            blend(x, y, c, (int)(a * cov));
        }
    /* the rim */
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (x + 0.5f - cx) / (wt + 1), dy = (y + 0.5f - top) / (r * 0.16f);
            float d = sqrtf(dx * dx + dy * dy);
            if (d > 1.05f || d < 0.78f) continue;
            blend(x, y, d > 0.9f ? RGB(0x4c, 0x60, 0x7c) : RGB(0xee, 0xf4, 0xfa), 220);
        }
}
/* a webcam: dark body, glass lens */
static void draw_camera_icon(int cx, int cy, int r) {
    fill_round(cx - r, cy - r * 2 / 3, 2 * r, r * 4 / 3 + 1, imax(2, r / 4), RGB(0x2c, 0x30, 0x38), 255);
    fill_round(cx - r * 2 / 3, cy - r * 2 / 3 - imax(2, r / 5), r / 2, imax(2, r / 5) + 1, 1, RGB(0x2c, 0x30, 0x38), 255);
    fill_circle(cx, cy + 1, r * 0.56f, RGB(0x9c, 0xa4, 0xb0), 255);
    fill_circle(cx, cy + 1, r * 0.44f, RGB(0x10, 0x18, 0x30), 255);
    fill_circle(cx, cy + 1, r * 0.28f, RGB(0x24, 0x48, 0x90), 255);
    fill_circle(cx - r * 0.12f, cy - r * 0.1f, r * 0.1f, RGB(0xd0, 0xe0, 0xff), 230);
    fill_circle(cx + r * 0.7f, cy - r * 0.4f, r * 0.1f, RGB(0xff, 0xc8, 0x40), 255);
}
/* screen recording: a screen with a red dot */
static void draw_record_icon(int cx, int cy, int r) {
    fill_round(cx - r, cy - r * 3 / 4, 2 * r, r * 5 / 4, imax(1, r / 6), RGB(0x30, 0x3c, 0x50), 255);
    fill_round(cx - r + 2, cy - r * 3 / 4 + 2, 2 * r - 4, r * 5 / 4 - 4, 1, RGB(0x1c, 0x60, 0xa8), 255);
    fill_rect(cx - r / 4, cy + r / 2, r / 2, r / 3, RGB(0x30, 0x3c, 0x50), 255);
    fill_rect(cx - r / 2, cy + r * 3 / 4, r, imax(2, r / 6), RGB(0x30, 0x3c, 0x50), 255);
    fill_circle(cx, cy - r / 8, r * 0.36f, RGB(0xff, 0xff, 0xff), 255);
    fill_circle(cx, cy - r / 8, r * 0.28f, RGB(0xe0, 0x24, 0x24), 255);
}
/* a magnifying glass */
static void draw_search_icon(int cx, int cy, int r) {
    float rr = r * 0.55f, ox = cx - r * 0.2f, oy = cy - r * 0.2f;
    for (int y = (int)(oy - rr - 2); y <= (int)(oy + rr + 2); y++)
        for (int x = (int)(ox - rr - 2); x <= (int)(ox + rr + 2); x++) {
            float d = sqrtf((x + 0.5f - ox) * (x + 0.5f - ox) + (y + 0.5f - oy) * (y + 0.5f - oy));
            float cov = clampf(1.0f - fabsf(d - rr) / 1.2f, 0, 1);
            if (cov > 0 && x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) blend(x, y, RGB(0x50, 0x5c, 0x70), (int)(255 * cov));
        }
    aa_line(ox + rr * 0.7f, oy + rr * 0.7f, cx + r * 0.85f, cy + r * 0.85f, imax(2, r / 5), RGB(0x50, 0x5c, 0x70));
}
static void draw_zip_icon(int cx, int cy, int r) {
    draw_folder_icon(cx, cy, r);
    int h = r * 3 / 2, y = cy - h / 2, zw = imax(2, r / 3);
    fill_rect(cx - zw / 2, y, zw, h, RGB(0x50, 0x58, 0x64), 255);
    for (int k = 0; k + 1 < h; k += 2) fill_rect(cx - zw / 2 + (k / 2 % 2 ? zw / 2 : 0), y + k, imax(1, zw / 2), 1, RGB(0xd8, 0xdc, 0xe2), 255);
    fill_rect(cx - zw / 2 - 1, cy + h / 6, zw + 2, imax(2, r / 3), RGB(0xb8, 0xc0, 0xcc), 255);
}
/* a printer with a sheet coming out */
static void draw_printer_icon(int cx, int cy, int r) {
    int w = r * 2, x = cx - r;
    fill_rect(cx - r * 3 / 5, cy - r, r * 6 / 5, r * 3 / 4, RGB(0xfa, 0xfa, 0xfa), 255);           /* paper in */
    stroke_round(cx - r * 3 / 5, cy - r, r * 6 / 5, r * 3 / 4, 0, RGB(0x90, 0x98, 0xa4), 255);
    fill_round_grad(x, cy - r / 3, w, r, 3, RGB(0x8c, 0x96, 0xa4), RGB(0x46, 0x4e, 0x5a), 255);   /* the body */
    stroke_round(x, cy - r / 3, w, r, 3, RGB(0x2c, 0x32, 0x3a), 255);
    fill_circle(x + w - 5, cy, 1.5f, RGB(0x40, 0xe0, 0x50), 255);
    fill_rect(cx - r * 3 / 5, cy + r / 3, r * 6 / 5, r * 2 / 3, RGB(255, 255, 255), 255);        /* the print */
    stroke_round(cx - r * 3 / 5, cy + r / 3, r * 6 / 5, r * 2 / 3, 0, RGB(0x90, 0x98, 0xa4), 255);
    fill_rect(cx - r / 3, cy + r / 2 + 1, r * 2 / 3, imax(1, r / 8), RGB(0x40, 0x80, 0xd0), 255);
}
/* a sun: brightness */
static void draw_sun_icon(int cx, int cy, int r) {
    uint32_t c = RGB(0xf8, 0xc0, 0x30);
    fill_circle(cx, cy, r * 0.45f, c, 255);
    for (int k = 0; k < 8; k++) {
        float a = k * 3.14159f / 4;
        for (float t = r * 0.62f; t <= r * 0.95f; t += 0.5f) fill_circle(cx + cosf(a) * t, cy + sinf(a) * t, imax(1, r / 8) * 0.8f, c, 255);
    }
}
/* a USB stick lying on its side: metal plug on the left, blue body */
static void draw_usb_icon(int cx, int cy, int r) {
    int h = r * 4 / 5 + 2, pw = r * 3 / 5, bw = r * 2 - pw, x = cx - r, y = cy - h / 2;
    int ph = h * 2 / 3, py = cy - ph / 2;
    fill_round_grad(x, py, pw + 2, ph, 1, RGB(0xf0, 0xf2, 0xf4), RGB(0x9c, 0xa4, 0xac), 255);
    stroke_round(x, py, pw + 2, ph, 1, RGB(0x50, 0x58, 0x60), 220);
    fill_rect(x + pw / 4, py + ph / 4, imax(1, pw / 4), imax(1, ph / 5), RGB(0x40, 0x44, 0x4c), 255);
    fill_rect(x + pw / 4, py + ph - ph / 4 - imax(1, ph / 5), imax(1, pw / 4), imax(1, ph / 5), RGB(0x40, 0x44, 0x4c), 255);
    fill_round_grad(x + pw, y, bw, h, 3, RGB(0x5c, 0xa8, 0xf0), RGB(0x1c, 0x4c, 0xa0), 255);
    stroke_round(x + pw, y, bw, h, 3, RGB(0x14, 0x30, 0x68), 230);
    fill_circle(x + pw + bw - 5, cy, 1.6f, RGB(0x30, 0xd0, 0x40), 255);
}
static void draw_computer_icon(int cx, int cy, int r) {
    int w = r * 2, h = r * 3 / 2, x = cx - r, y = cy - r;
    fill_round(x, y, w, h, 2, RGB(0x30, 0x34, 0x3c), 255);
    fill_round_grad(x + 2, y + 2, w - 4, h - 4, 1, RGB(0x6c, 0xc4, 0xf4), RGB(0x1c, 0x5c, 0xa8), 255);
    fill_rect(cx - 1, y + h, 3, r / 3 + 1, RGB(0x50, 0x54, 0x5c), 255);
    fill_rect(cx - r / 2, y + h + r / 3, r, 2, RGB(0x50, 0x54, 0x5c), 255);
}

/* a dark screen with a prompt ">_" */
static void draw_terminal_icon(int cx, int cy, int r) {
    int w = r * 2, h = r * 3 / 2 + 2, x = cx - r, y = cy - h / 2;
    fill_round(x, y, w, h, 3, RGB(0x50, 0x54, 0x5c), 255);
    fill_round(x + 1, y + 1, w - 2, h - 2, 2, RGB(0x16, 0x18, 0x1c), 255);
    int s = imax(1, r / 6), px = x + r / 3, py = y + h / 2;
    for (int i = 0; i <= r / 3; i++) {                     /* ">" */
        fill_rect(px + i, py - r / 3 + i, s + 1, s, RGB(0x6c, 0xe0, 0x7c), 255);
        fill_rect(px + i, py + r / 3 - i, s + 1, s, RGB(0x6c, 0xe0, 0x7c), 255);
    }
    fill_rect(px + r / 2 + 2, py + r / 3, r / 2, s + 1, RGB(0xe8, 0xe8, 0xe8), 255);   /* "_" */
}

/* any other app: a small window */
static void draw_app_icon(int cx, int cy, int r) {
    int w = r * 2, h = r * 3 / 2 + 2, x = cx - r, y = cy - h / 2;
    fill_round(x, y, w, h, 3, RGB(0x2c, 0x5c, 0x9c), 255);
    fill_rect(x + 2, y + r / 3 + 2, w - 4, h - r / 3 - 4, RGB(0xf4, 0xf6, 0xf8), 255);
}

/* ---- drawing ------------------------------------------------------- */
static void ex_storage(const char *path, double *total, double *freeb) {
    struct statvfs sv;
    *total = *freeb = 0;
    if (statvfs(path, &sv) == 0) {
        *total = (double)sv.f_blocks * sv.f_frsize;
        *freeb = (double)sv.f_bavail * sv.f_frsize;
    }
}
static void ex_fmt_size(double b, char *out, size_t n) {
    if (b >= 1024.0 * 1024 * 1024) snprintf(out, n, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024.0 * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
    else snprintf(out, n, "%.0f KB", b / 1024.0);
}

static void ex_draw_drive_tile(int i, int cx, int cy, int cw, int hotid) {
    int x, y, w, h; ex_drive_rect(i, cw, &x, &y, &w, &h);
    x += cx; y += cy;
    int sel = ex_sel == i, hv = hotid == ID_EXDRIVE + i;
    if (sel || hv) {
        fill_round(x, y, w, h, 3, sel ? RGB(0xcc, 0xe8, 0xff) : RGB(0xe5, 0xf3, 0xfb), 255);
        stroke_round(x, y, w, h, 3, sel ? RGB(0x84, 0xac, 0xdd) : RGB(0xb8, 0xd6, 0xfb), 255);
    }
    int usb = i >= 2;
    if (usb) draw_icon(ICON_USB, x + 26, y + 30, 18); else draw_icon(ICON_DRIVE, x + 26, y + 30, 18);
    const char *name = usb ? rdrv[i - 2].name : i ? "ccos Data (D:)" : "ccos System (C:)";
    draw_text(x + 54, y + 6, name, RGB(0x10, 0x10, 0x10), 255);
    double total, freeb; ex_storage(usb ? rdrv[i - 2].mnt : i ? "/data" : "/", &total, &freeb);
    int bw = w - 64, bx = x + 54, by = y + 26;
    fill_rect(bx, by, bw, 14, RGB(0xe6, 0xe6, 0xe6), 255);
    stroke_round(bx, by, bw, 14, 1, RGB(0xbc, 0xbc, 0xbc), 255);
    if (total > 0) {
        double used = (total - freeb) / total;
        int uw = (int)((bw - 2) * used);
        int full = used > 0.9;
        if (uw > 0) fill_round_grad(bx + 1, by + 1, uw, 12, 1,
                                    full ? RGB(0xf0, 0x6c, 0x6c) : RGB(0x4c, 0xc0, 0xf0),
                                    full ? RGB(0xc0, 0x18, 0x18) : RGB(0x1c, 0x84, 0xc8), 255);
        char a[24], b[24], t[64];
        ex_fmt_size(freeb, a, sizeof a); ex_fmt_size(total, b, sizeof b);
        snprintf(t, sizeof t, "%s free of %s", a, b);
        draw_text(bx, by + 18, t, RGB(0x6d, 0x6d, 0x6d), 255);
    } else draw_text(bx, by + 18, i ? "Not connected" : "In memory", RGB(0x6d, 0x6d, 0x6d), 255);
}

#include "exdrag.c"              /* drag a box to select, drag files onto folders */
static void explorer_draw(int cx, int cy, int cw, int ch) {
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == explorer_draw ? hot.b : -1;
    fill_rect(cx, cy, cw, ch, RGB(255, 255, 255), 255);

    /* navigation bar: back, up, address */
    fill_rect(cx, cy, cw, EX_NAV_H, RGB(0xdc, 0xe8, 0xf4), 255);
    for (int j = 0; j < EX_NAV_H; j++) fill_rect(cx, cy + j, cw, 1, RGB(0xc4, 0xd8, 0xee), j * 200 / EX_NAV_H);
    for (int b = 0; b < 2; b++) {
        int en = b == 0 ? ex_nhist > 0 : ex_path[0] != 0;
        float bx = cx + 22 + b * 30, by = cy + EX_NAV_H / 2.0f;
        int hv = hotid == ID_EXBACK + b && en;
        uint32_t top = en ? (hv ? RGB(0x8c, 0xd8, 0xff) : RGB(0x5c, 0xb0, 0xf0)) : RGB(0xd0, 0xd8, 0xe0);
        uint32_t bot = en ? (hv ? RGB(0x1c, 0x7c, 0xd8) : RGB(0x14, 0x5c, 0xb0)) : RGB(0xa8, 0xb0, 0xbc);
        fill_round_grad((int)bx - 12, (int)by - 12, 24, 24, 12, top, bot, 255);
        stroke_round((int)bx - 12, (int)by - 12, 24, 24, 12, en ? RGB(0x10, 0x3c, 0x78) : RGB(0x90, 0x98, 0xa4), 255);
        draw_text_bold((int)bx - 4, (int)by - 8, b == 0 ? "\x11" : "\x18", RGB(255, 255, 255));
    }
    int ax = cx + 84, aw = cw - 92;
    fill_rect(ax, cy + 7, aw, 26, RGB(255, 255, 255), 255);
    stroke_round(ax, cy + 7, aw, 26, 2, RGB(0x8c, 0xa4, 0xc4), 255);
    char labels[EX_MAXCRUMB][40], paths[EX_MAXCRUMB][256];
    int nc = ex_crumbs(labels, paths);
    draw_icon(ex_path[0] ? ICON_FOLDER : ICON_COMPUTER, ax + 14, cy + 20, 8);
    for (int i = 0; i < nc; i++) {
        int x = cx + ex_crumb_x(labels, i) - 104 + 84 + 20;
        int w = text_w(labels[i]) + 12;
        if (x + w > ax + aw - 4) break;
        if (hotid == ID_EXCRUMB + i) {
            fill_round(x - 2, cy + 9, w, 22, 2, RGB(0xe5, 0xf3, 0xfb), 255);
            stroke_round(x - 2, cy + 9, w, 22, 2, RGB(0xb8, 0xd6, 0xfb), 255);
        }
        draw_text(x + 4, cy + 12, labels[i], RGB(0x10, 0x10, 0x10), 255);
        if (i < nc - 1) draw_text(x + w + 1, cy + 12, "\x10", RGB(0x50, 0x50, 0x50), 255);
    }

    /* command bar */
    int cby = cy + EX_NAV_H;
    fill_rect(cx, cby, cw, EX_CMD_H, RGB(0xf4, 0xf8, 0xfc), 255);
    for (int j = 0; j < EX_CMD_H; j++) fill_rect(cx, cby + j, cw, 1, RGB(0xd8, 0xe6, 0xf4), j * 255 / EX_CMD_H);
    fill_rect(cx, cby + EX_CMD_H - 1, cw, 1, RGB(0xa8, 0xbc, 0xd4), 255);
    for (int i = 0; i < EX_NCMD; i++) {
        int en = ex_cmd_enabled(i);
        int x, y, w, h; ex_cmd_rect(i, &x, &y, &w, &h);
        if (!w) continue;
        if (en && hotid == ID_EXCMD + i) {
            fill_round_grad(cx + x, cy + y, w, h, 3, RGB(0xfc, 0xfe, 0xff), RGB(0xd8, 0xec, 0xfc), 255);
            stroke_round(cx + x, cy + y, w, h, 3, RGB(0x84, 0xb4, 0xe4), 255);
        }
        draw_text(cx + x + (w - text_w(ex_cmd_label[i])) / 2, cy + y + (h - 16) / 2, ex_cmd_label[i],
                  en ? RGB(0x1e, 0x39, 0x5b) : RGB(0xa0, 0xa8, 0xb4), 255);
    }

    /* navigation pane */
    int ny = cy + EX_NAV_H + EX_CMD_H;
    fill_rect(cx, ny, EX_LEFT_W, ch - EX_NAV_H - EX_CMD_H - EX_STATUS_H, RGB(0xf1, 0xf5, 0xfb), 255);
    fill_rect(cx + EX_LEFT_W - 1, ny, 1, ch - EX_NAV_H - EX_CMD_H - EX_STATUS_H, RGB(0xd6, 0xe0, 0xec), 255);
    draw_text(cx + 12, ny + 8, "Favorites", RGB(0x1e, 0x39, 0x87), 255);
    for (int i = 0; i < EX_NNAV; i++) {
        int y = cy + ex_nav_y(i), x = cx + 8 + ex_nav[i].indent * 16;
        int cur = !strcmp(ex_path, ex_nav[i].path);
        if (cur || hotid == ID_EXNAV + i) {
            fill_round(cx + 4, y, EX_LEFT_W - 10, 22, 2, cur ? RGB(0xcc, 0xe8, 0xff) : RGB(0xe5, 0xf3, 0xfb), 255);
            stroke_round(cx + 4, y, EX_LEFT_W - 10, 22, 2, cur ? RGB(0x84, 0xac, 0xdd) : RGB(0xb8, 0xd6, 0xfb), 255);
        }
        draw_icon(ex_nav[i].icon, x + 10, y + 11, 7);
        if (i >= 7) {                                    /* USB drive: name, then an eject button */
            char nm[24]; snprintf(nm, sizeof nm, "%.13s", ex_nav[i].label);
            draw_text(x + 24, y + 3, nm, RGB(0x10, 0x10, 0x10), 255);
            int ex = cx + EX_LEFT_W - 26, ey = y + 5;
            uint32_t ec = hotid == ID_EXNAV + i ? RGB(0x10, 0x50, 0xb0) : RGB(0x60, 0x6c, 0x7c);
            for (int k = 0; k < 6; k++) fill_rect(ex + 6 - k, ey + k, 1 + 2 * k, 1, ec, 255);
            fill_rect(ex, ey + 8, 13, 2, ec, 255);
        } else draw_text(x + 24, y + 3, ex_nav[i].label, RGB(0x10, 0x10, 0x10), 255);
    }

    /* main area */
    int mx0, my0, mw, mh; ex_main(cw, ch, &mx0, &my0, &mw, &mh);
    mx0 += cx; my0 += cy;
    if (!ex_path[0]) {
        draw_text(mx0 + 16, my0 + 12, "Hard Disk Drives (2)", RGB(0x1e, 0x39, 0x87), 255);
        fill_rect(mx0 + 180, my0 + 20, mw - 200, 1, RGB(0xd6, 0xe0, 0xec), 255);
        if (nrd) {
            int x, y, w, h; ex_drive_rect(2, cw, &x, &y, &w, &h);
            char hd[48]; snprintf(hd, sizeof hd, "Removable Drives (%d)", nrd);
            draw_text(mx0 + 16, cy + y - 28, hd, RGB(0x1e, 0x39, 0x87), 255);
            fill_rect(mx0 + 196, cy + y - 20, mw - 216, 1, RGB(0xd6, 0xe0, 0xec), 255);
        }
        for (int i = 0; i < EX_NDRIVE; i++) ex_draw_drive_tile(i, cx, cy, cw, hotid);
    } else {
        int rows = ex_rows(cw, ch);
        int lw = mw - (ex_n > rows ? EX_SB_W : 0);
        int col_size = mx0 + lw - 12, col_type = col_size - 80 - 110, col_date = col_type - 150;
        fill_rect(mx0, my0, lw, EX_HDR_H, RGB(0xfc, 0xfc, 0xfc), 255);
        fill_rect(mx0, my0 + EX_HDR_H - 1, lw, 1, RGB(0xe2, 0xe6, 0xec), 255);
        draw_text(mx0 + 30, my0 + 4, "Name", RGB(0x4c, 0x60, 0x7a), 255);
        draw_text(col_date, my0 + 4, "Date modified", RGB(0x4c, 0x60, 0x7a), 255);
        draw_text(col_type, my0 + 4, "Type", RGB(0x4c, 0x60, 0x7a), 255);
        draw_text(col_size - 32, my0 + 4, "Size", RGB(0x4c, 0x60, 0x7a), 255);
        for (int c = 0; c < 3; c++)
            fill_rect((c == 0 ? col_date : c == 1 ? col_type : col_size - 72) - 8, my0 + 3, 1, EX_HDR_H - 6,
                      RGB(0xe2, 0xe6, 0xec), 255);
        if (!ex_n) draw_text(mx0 + (lw - 8 * 22) / 2, my0 + EX_HDR_H + 16, "This folder is empty.",
                             RGB(0x6d, 0x6d, 0x6d), 255);
        for (int r = 0; r < rows && ex_scroll + r < ex_n; r++) {
            int i = ex_scroll + r, y = my0 + EX_HDR_H + 2 + r * EX_ROW_H;
            if (y + EX_ROW_H <= clip_y0 || y >= clip_y1) continue;
            ExItem *it = &ex_items[i];
            int sel = ex_selected(i), hv = hotid == i;
            if (sel || hv) {
                fill_round_grad(mx0 + 2, y, lw - 4, EX_ROW_H, 2,
                                sel ? RGB(0xdc, 0xeb, 0xfc) : RGB(0xf2, 0xf8, 0xff),
                                sel ? RGB(0xc1, 0xdb, 0xfc) : RGB(0xe6, 0xf2, 0xfe), 255);
                stroke_round(mx0 + 2, y, lw - 4, EX_ROW_H, 2, sel ? RGB(0x7d, 0xa2, 0xce) : RGB(0xb8, 0xd6, 0xfb), 255);
            }
            const char *ty = ex_type(it);
            char fp_[512]; ex_child(fp_, sizeof fp_, it->name);
            int faded = ex_clip_cut_has(fp_);
            draw_icon(it->dir ? ex_folder_icon(fp_) : !strcmp(ty, "Text Document") ? ICON_NOTE : !strcmp(ty, "Picture") ? ICON_PHOTO :
                      !strcmp(ty, "Video") ? ICON_VIDEO : !strcmp(ty, "Sound") ? ICON_MUSIC : !strcmp(ty, "Zip archive") || !strcmp(ty, "Archive") ? ICON_ZIP :
                      !strcmp(ty, "Application") ? launcher_icon(it->name) : ICON_FILE,
                      mx0 + 16, y + EX_ROW_H / 2, 7);
            char nm[64]; int maxc = imax(4, (col_date - mx0 - 40) / 8);
            snprintf(nm, sizeof nm, "%s", it->name);
            if (text_len(nm) > maxc) { u8_trunc(nm, maxc - 2); strcat(nm, ".."); }
            draw_text(mx0 + 30, y + 3, nm, faded ? RGB(0x90, 0x90, 0x90) : RGB(0x10, 0x10, 0x10), 255);
            char dt[32]; struct tm tm; localtime_r(&it->mtime, &tm);
            strftime(dt, sizeof dt, set_clock24 ? "%d.%m.%Y %H:%M" : "%d.%m.%Y %I:%M%p", &tm);
            draw_text(col_date, y + 3, dt, RGB(0x6d, 0x6d, 0x6d), 255);
            draw_text(col_type, y + 3, ty, RGB(0x6d, 0x6d, 0x6d), 255);
            if (!it->dir) {
                char sz[24]; snprintf(sz, sizeof sz, "%lld KB", (it->size + 1023) / 1024);
                draw_text(col_size - text_w(sz), y + 3, sz, RGB(0x6d, 0x6d, 0x6d), 255);
            }
        }
        if (ex_n > rows) {                                  /* scrollbar */
            int sx = mx0 + mw - EX_SB_W, sh = mh;
            fill_rect(sx, my0, EX_SB_W, sh, RGB(0xe8, 0xe8, 0xec), 255);
            int thumb = imax(24, sh * rows / ex_n), pos = (sh - thumb) * ex_scroll / imax(1, ex_n - rows);
            fill_round_grad(sx + 2, my0 + pos + 1, EX_SB_W - 4, thumb - 2, 3,
                            RGB(0xf0, 0xf2, 0xf6), RGB(0xc8, 0xd0, 0xdc), 255);
            stroke_round(sx + 2, my0 + pos + 1, EX_SB_W - 4, thumb - 2, 3, RGB(0x90, 0x98, 0xa8), 255);
        }
    }

    /* status bar */
    int sy = cy + ch - EX_STATUS_H;
    fill_rect(cx, sy, cw, EX_STATUS_H, RGB(0xf0, 0xf4, 0xfa), 255);
    fill_rect(cx, sy, cw, 1, RGB(0xd0, 0xdc, 0xea), 255);
    char st[160];
    if (!ex_path[0]) {
        if (ex_sel >= 2 && ex_sel - 2 < nrd) snprintf(st, sizeof st, "%s - USB drive%s. Right-click it to eject.", rdrv[ex_sel - 2].name, rdrv[ex_sel - 2].ro ? " (read-only)" : "");
        else if (ex_sel >= 0) snprintf(st, sizeof st, "%s", ex_sel ? "ccos Data (D:) - your files and settings, kept on the save disk"
                                                                  : "ccos System (C:) - the operating system, kept in memory");
        else snprintf(st, sizeof st, "%d items", EX_NDRIVE);
    }
    else if (ex_note[0] && ui_ms() < ex_note_until) snprintf(st, sizeof st, "%s", ex_note);
    else if (ex_in_bin() && ex_sel >= 0 && ex_nselected() == 1) {
        char o[600]; long long w = 0;
        if (!bin_lookup(ex_items[ex_sel].name, o, sizeof o, &w)) {
            char *s = strrchr(o, '/'); if (s && s != o) *s = 0;
            struct tm tm; time_t tt = (time_t)w; localtime_r(&tt, &tm);
            char d[32]; strftime(d, sizeof d, "%d.%m.%Y %H:%M", &tm);
            char pp[200]; pretty_path(o, pp, sizeof pp);
            snprintf(st, sizeof st, "From %.60s   deleted %s", pp, d);
        } else snprintf(st, sizeof st, "%s", ex_items[ex_sel].name);
    }
    else if (ex_in_bin() && !ex_n) snprintf(st, sizeof st, "The Recycle Bin is empty.");
    else if (ex_nselected() > 1) {
        long long sum = 0; int n = 0;
        for (int i = 0; i < ex_n; i++) if (ex_mark[i]) { n++; if (!ex_items[i].dir) sum += ex_items[i].size; }
        char sz[24]; ex_fmt_size((double)sum, sz, sizeof sz);
        snprintf(st, sizeof st, "%d items selected   %s", n, sz);
    }
    else if (ex_sel >= 0) {
        ExItem *it = &ex_items[ex_sel];
        if (it->dir) snprintf(st, sizeof st, "%s   File folder", it->name);
        else snprintf(st, sizeof st, "%s   %s   %lld KB", it->name, ex_type(it), (it->size + 1023) / 1024);
    } else snprintf(st, sizeof st, "%d item%s%s", ex_n, ex_n == 1 ? "" : "s",
                    ex_on_d(ex_path) ? "" : "   (system files are read-only)");
    draw_text(cx + 12, sy + 5, st, RGB(0x1e, 0x39, 0x5b), 255);

    ex_drag_draw(cx, cy, cw, ch);
    dlg_draw(&ex_dlg, cx, cy, cw, ch, hotid);
}

/* ---- hit testing --------------------------------------------------- */
static int explorer_hit(int lx, int ly, int cw, int ch) {
    if (ex_dlg.active) return dlg_hit(&ex_dlg, lx, ly, cw, ch);
    for (int b = 0; b < 2; b++) {
        int bx = 22 + b * 30, by = EX_NAV_H / 2;
        if ((lx - bx) * (lx - bx) + (ly - by) * (ly - by) <= 144) return ID_EXBACK + b;
    }
    if (ly < EX_NAV_H) {
        char labels[EX_MAXCRUMB][40], paths[EX_MAXCRUMB][256];
        int nc = ex_crumbs(labels, paths);
        for (int i = 0; i < nc; i++) {
            int x = ex_crumb_x(labels, i) - 104 + 84 + 20, w = text_w(labels[i]) + 12;
            if (lx >= x - 2 && lx < x - 2 + w && ly >= 9 && ly < 31) return ID_EXCRUMB + i;
        }
        return -1;
    }
    for (int i = 0; i < EX_NCMD; i++) {
        int x, y, w, h; ex_cmd_rect(i, &x, &y, &w, &h);
        if (lx >= x && lx < x + w && ly >= y && ly < y + h) return ID_EXCMD + i;
    }
    if (lx < EX_LEFT_W) {
        for (int i = 0; i < EX_NNAV; i++) {
            int y = ex_nav_y(i);
            if (ly >= y && ly < y + 22) return ID_EXNAV + i;
        }
        return -1;
    }
    int mx0, my0, mw, mh; ex_main(cw, ch, &mx0, &my0, &mw, &mh);
    if (!ex_path[0]) {
        for (int i = 0; i < EX_NDRIVE; i++) {
            int x, y, w, h; ex_drive_rect(i, cw, &x, &y, &w, &h);
            if (lx >= x && lx < x + w && ly >= y && ly < y + h) return ID_EXDRIVE + i;
        }
        return -1;
    }
    int rows = ex_rows(cw, ch), lw = mw - (ex_n > rows ? EX_SB_W : 0);
    if (lx >= mx0 + lw || ly < my0 + EX_HDR_H + 2 || ly >= my0 + mh) return -1;
    int r = (ly - my0 - EX_HDR_H - 2) / EX_ROW_H;
    if (r < rows && ex_scroll + r < ex_n) return ex_scroll + r;
    return -1;
}

static void explorer_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(explorer_draw); *x = *y = *w = *h = 0;
    if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (dlg_elem_rect(&ex_dlg, id, cw, ch, x, y, w, h)) return;
    if (id == ID_EXBACK || id == ID_EXUP) { *x = 8 + (id - ID_EXBACK) * 30; *y = 6; *w = 30; *h = 30; return; }
    if (id >= ID_EXCRUMB && id < ID_EXCRUMB + EX_MAXCRUMB) { *x = 84; *y = 7; *w = cw - 92; *h = 26; return; }
    if (id >= ID_EXCMD && id < ID_EXCMD + EX_NCMD) { ex_cmd_rect(id - ID_EXCMD, x, y, w, h); return; }
    if (id >= ID_EXNAV && id < ID_EXNAV + EX_NNAV) { *x = 0; *y = ex_nav_y(id - ID_EXNAV); *w = EX_LEFT_W; *h = 22; return; }
    if (id >= ID_EXDRIVE && id < ID_EXDRIVE + EX_NDRIVE) { ex_drive_rect(id - ID_EXDRIVE, cw, x, y, w, h); return; }
    if (id >= 0 && id < EX_MAX) {
        int mx0, my0, mw, mh; ex_main(cw, ch, &mx0, &my0, &mw, &mh);
        *x = mx0; *y = my0 + EX_HDR_H + 2 + (id - ex_scroll) * EX_ROW_H; *w = mw; *h = EX_ROW_H;
    }
}

/* ---- actions -------------------------------------------------------- */
static void ex_info(const char *a, const char *b) {
    ex_dlg_mode = EXD_INFO;
    dlg_open(&ex_dlg, DLG_MSG, "File Explorer", a, b, "OK", NULL, NULL, ex_dlg_done);
    repaint_client(explorer_draw);
}

static void ex_open_item(int i) {
    ExItem *it = &ex_items[i];
    char p[512]; ex_child(p, sizeof p, it->name);
    if (it->dir) { ex_go(p, 1); return; }
    if (ex_is_archive(it->name)) { ex_extract(p); return; }         /* a zip: unpack it into a folder */
    if (is_launcher(it->name)) { launcher_run(p); return; }     /* Downloads > Flathub > an app */
    if (is_ccupd(it->name)) { up_install_file(p); return; }     /* a ccos update file */
    const char *app = media_app_for(it->name);
    if (app) { char c[600]; snprintf(c, sizeof c, "%s %s", app, p); run(c); return; }
    if (!ex_is_text(p, it->size)) { ex_info("ccos doesn't have an app that", "can open this file yet."); return; }
    if (win_of(notepad_draw) >= 0 && np_modified) {
        ex_info("Notepad has unsaved changes.", "Save or close them first.");
        return;
    }
    run("@notepad");
    np_load_from(ex_path, it->name);
}

/* open any file with the right app (pictures, videos, sounds, text) */
static void open_file(const char *path) {
    if (is_launcher(path)) { launcher_run(path); return; }
    if (is_ccupd(path)) { up_install_file(path); return; }
    if (ex_is_archive(path)) { struct stat as; if (stat(path, &as) == 0 && S_ISREG(as.st_mode)) { ex_extract(path); return; } }
    { struct stat ds; if (stat(path, &ds) == 0 && S_ISDIR(ds.st_mode)) { run("@explorer"); ex_go(path, 1); return; } }
    const char *app = media_app_for(path);
    if (app) { char c[600]; snprintf(c, sizeof c, "%s %s", app, path); run(c); return; }
    char dir[512]; snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (!sl) return;
    *sl = 0;
    struct stat st;
    if (stat(path, &st) != 0 || !ex_is_text(path, st.st_size)) { ex_info("ccos doesn't have an app that", "can open this file yet."); return; }
    run("@notepad");
    np_load_from(dir[0] ? dir : "/", sl + 1);
}
/* right-click on a file: Open, and for pictures: Set as desktop background */
static void explorer_rclick(int lx, int ly, int cw, int ch) {
    if (ex_dlg.active) return;
    int id = explorer_hit(lx, ly, cw, ch);
    {   /* a USB drive (tile or left pane): Open / Eject */
        int u = -1;
        if (id >= ID_EXDRIVE + 2 && id < ID_EXDRIVE + EX_NDRIVE && !ex_path[0]) u = id - ID_EXDRIVE - 2;
        if (id >= ID_EXNAV + 8 && id < ID_EXNAV + EX_NNAV) u = id - ID_EXNAV - 8;
        if (u >= 0) {
            const char *lb[2] = { "Open", "Eject" };
            int ac[2] = { PA_OPEN_FILE, PA_EJECT };
            pop_show_list(mx, my, ICON_USB, rdrv[u].mnt, 2, lb, ac);
            return;
        }
    }
    if (!ex_path[0]) return;
    const char *lb[MAXPOP]; int ac[MAXPOP], n = 0;
    #define ADD(l, a) do { lb[n] = (l); ac[n] = (a); n++; } while (0)
    if (ex_in_bin()) {                                  /* the Recycle Bin's own menu */
        if (id < 0 || id >= ex_n) {
            if (ex_multi || ex_sel >= 0) { ex_single(-1); repaint_client(explorer_draw); }
            if (!ex_n) return;
            ADD("Restore all", PA_RESTOREALL); ADD("Empty Recycle Bin", PA_EMPTYBIN); ADD("Select all", PA_SELALL);
            pop_show_list(mx, my, ICON_BIN_FULL, ex_path, n, lb, ac);
            return;
        }
        if (!ex_selected(id)) ex_single(id);
        repaint_client(explorer_draw);
        char bp[512]; ex_child(bp, sizeof bp, ex_items[id].name);
        ADD("Restore", PA_RESTORE); ADD("Delete for good", PA_DELETE);
        pop_show_list(mx, my, ex_items[id].dir ? ICON_FOLDER : ICON_FILE, bp, n, lb, ac);
        return;
    }
    if (id < 0 || id >= ex_n) {                         /* empty space: Paste, New folder */
        if (ex_multi || ex_sel >= 0) { ex_single(-1); repaint_client(explorer_draw); }
        if (fclip.n) ADD("Paste", PA_PASTE);
        int newi = n;
        ADD("New", PA_NEWMENU);
        ADD("Select all", PA_SELALL);
        pop_show_list(mx, my, ICON_FOLDER, ex_path, n, lb, ac);
        const char *sl[POP_SUBMAX]; int sa[POP_SUBMAX], si[POP_SUBMAX], sn = 0;
        sl[sn] = "Folder"; sa[sn] = PA_NEWFOLDER; si[sn] = ICON_FOLDER; sn++;
        for (int k = 0; k < EX_NNEWT && sn < POP_SUBMAX; k++) { sl[sn] = ex_newt[k].label; sa[sn] = PA_NEWFILE + k; si[sn] = ex_newt[k].icon; sn++; }
        pop_set_sub(newi, sn, sl, sa, si);
        return;
    }
    if (!ex_selected(id)) ex_single(id);
    repaint_client(explorer_draw);
    char p[512]; ex_child(p, sizeof p, ex_items[id].name);
    int ns = ex_nselected(), mk = ex_items[id].dir ? 0 : media_kind(ex_items[id].name);
    int img = ns == 1 && mk == MK_IMAGE && media_kind(p) == MK_IMAGE && !strstr(p, ".svg");
    if (ns == 1) ADD("Open", PA_OPEN_FILE);
    if (ns == 1 && ex_is_archive(ex_items[id].name)) ADD("Extract all", PA_UNZIP);
    ADD("Cut", PA_CUT);
    ADD("Copy", PA_COPY);
    if (ns == 1) ADD("Rename", PA_RENAME);
    ADD("Delete", PA_DELETE);
    if (!(ns == 1 && ex_is_archive(ex_items[id].name))) ADD("Compress to zip", PA_ZIP);
    if (img) { ADD("Set as desktop background", PA_SET_WALL); ADD("Print...", PA_PRINT); }
    #undef ADD
    pop_show_list(mx, my, ex_items[id].dir ? ICON_FOLDER : mk == MK_IMAGE ? ICON_PHOTO : mk == MK_VIDEO ? ICON_VIDEO : mk == MK_AUDIO ? ICON_MUSIC :
                  ex_is_archive(ex_items[id].name) ? ICON_ZIP : ICON_FILE, p, n, lb, ac);
}
/* ---- the Recycle Bin's buttons ---- */
static void desk_bin_changed(void);
/* "/data/Documents/x" -> "D:/Documents/x", Downloads by name */
static void pretty_path(const char *p, char *out, size_t n) {
    const char *dl = dl_dir(); size_t dn = strlen(dl);
    if (!strncmp(p, dl, dn) && (p[dn] == '/' || !p[dn])) snprintf(out, n, "Downloads%s", p + dn);
    else if (!strncmp(p, "/data", 5) && (p[5] == '/' || !p[5])) snprintf(out, n, "D:%s", p[5] ? p + 5 : "/");
    else snprintf(out, n, "%s", p);
}
static void ex_bin_restore(int all) {
    int done = 0, bad = 0; char last[1100] = "";
    for (int i = 0; i < ex_n; i++) {
        if (!all && !ex_selected(i)) continue;
        if (bin_restore(ex_items[i].name, last, sizeof last)) bad++; else done++;
    }
    bin_tidy_index(); sync();
    char m[120];
    if (bad) snprintf(m, sizeof m, "%d item%s couldn't be put back.", bad, bad == 1 ? "" : "s");
    else if (done == 1) { char *s = strrchr(last, '/'); if (s) *s = 0; char pp[200]; pretty_path(last, pp, sizeof pp); snprintf(m, sizeof m, "Put back in %.80s", pp); }
    else snprintf(m, sizeof m, "Put back %d items where they were.", done);
    ex_note_set(m);
    ex_sig = ex_read(); ex_single(-1);
    desk_bin_changed();
    repaint_client(explorer_draw);
}
static void ex_bin_empty_ask(void) {
    if (!bin_count()) return;
    ex_dlg_mode = EXD_EMPTYBIN;
    char m[80]; int n = bin_count();
    snprintf(m, sizeof m, n == 1 ? "Delete the item in the Recycle Bin for good?" : "Delete all %d items in the Recycle Bin for good?", n);
    dlg_open(&ex_dlg, DLG_MSG, "Empty Recycle Bin", m, "This can't be undone.", "Empty", "Cancel", NULL, ex_dlg_done);
    repaint_client(explorer_draw);
}
static void ex_need_writable(void) { ex_info("System files can't be changed.", "You can make changes on D: only."); }

static void ex_new_folder(void) {
    if (!ex_writable()) { ex_need_writable(); return; }
    ex_dlg_mode = EXD_NEWFOLDER;
    dlg_open(&ex_dlg, DLG_INPUT, "New folder", "Folder name:", NULL, "Create", "Cancel", NULL, ex_dlg_done);
    snprintf(ex_dlg.field, sizeof ex_dlg.field, "New folder");
    repaint_client(explorer_draw);
}

static void ex_new_file(int k) {
    if (!ex_writable()) { ex_need_writable(); return; }
    ex_new_kind = k;
    ex_dlg_mode = EXD_NEWFILE;
    char t[48]; snprintf(t, sizeof t, "New %s", ex_newt[k].label);
    dlg_open(&ex_dlg, DLG_INPUT, t, "Name:", NULL, "Create", "Cancel", NULL, ex_dlg_done);
    snprintf(ex_dlg.field, sizeof ex_dlg.field, "%s", ex_newt[k].name);
    repaint_client(explorer_draw);
}

static void ex_rename(void) {
    if (ex_sel < 0 || ex_nselected() != 1) return;
    if (!ex_writable()) { ex_need_writable(); return; }
    ex_dlg_mode = EXD_RENAME;
    dlg_open(&ex_dlg, DLG_INPUT, "Rename", "New name:", NULL, "Rename", "Cancel", NULL, ex_dlg_done);
    snprintf(ex_dlg.field, sizeof ex_dlg.field, "%s", ex_items[ex_sel].name);
    repaint_client(explorer_draw);
}

static void ex_delete(void) {
    int ns = ex_nselected();
    if (!ns) return;
    if (!ex_writable()) { ex_need_writable(); return; }
    if (!ex_in_bin() && !shift) {                       /* to the Recycle Bin, if they can go there */
        int n, all = 1; char **v = ex_sel_paths(&n);
        for (int i = 0; v && i < n; i++) if (!bin_can(v[i])) all = 0;
        if (v && all) {
            int done = 0;
            for (int i = 0; i < n; i++) if (!bin_put(v[i])) done++;
            ex_free_list(v, n);
            sync();
            char m[80];
            if (done == n) snprintf(m, sizeof m, done == 1 ? "Moved to the Recycle Bin." : "Moved %d items to the Recycle Bin.", done);
            else snprintf(m, sizeof m, "%d of %d items couldn't be moved to the Recycle Bin.", n - done, n);
            ex_note_set(m);
            ex_sig = ex_read(); ex_single(-1);
            ex_scroll = imin(ex_scroll, imax(0, ex_n - 1));
            desk_bin_changed();
            repaint_client(explorer_draw);
            return;
        }
        if (v) ex_free_list(v, n);
    }
    ex_dlg_mode = EXD_DELETE;
    if (ns > 1) {
        char m[80]; snprintf(m, sizeof m, "these %d items, and everything in them?", ns);
        dlg_open(&ex_dlg, DLG_MSG, "Delete Multiple Items", "Permanently delete", m, "Yes", "No", NULL, ex_dlg_done);
        repaint_client(explorer_draw);
        return;
    }
    if (ex_multi) for (int i = 0; i < ex_n; i++) if (ex_mark[i]) ex_sel = i;
    char m[80];
    snprintf(m, sizeof m, "\"%.28s\"?", ex_items[ex_sel].name);
    dlg_open(&ex_dlg, DLG_MSG, ex_items[ex_sel].dir ? "Delete Folder" : "Delete File",
             ex_items[ex_sel].dir ? "Permanently delete this folder and" : "Permanently delete this file",
             ex_items[ex_sel].dir ? "everything in it?" : m, "Yes", "No", NULL, ex_dlg_done);
    repaint_client(explorer_draw);
}

static int rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char c[512]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
                rm_rf(c);
            }
            closedir(d);
        }
        return rmdir(p);
    }
    return unlink(p);
}

static void ex_select_name(const char *name) {
    ex_multi = 0;
    for (int i = 0; i < ex_n; i++) if (!strcmp(ex_items[i].name, name)) { ex_sel = i; break; }
    int i = win_of(explorer_draw);
    if (i >= 0 && ex_sel >= 0) {
        int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
        int rows = ex_rows(cw, ch);
        if (ex_sel < ex_scroll) ex_scroll = ex_sel;
        if (ex_sel >= ex_scroll + rows) ex_scroll = ex_sel - rows + 1;
    }
}

void ex_dlg_done(int b) {
    char name[40], p[512], q[512];
    if (ex_dlg_mode == EXD_NEWFOLDER && b == 0) {
        snprintf(name, sizeof name, "%s", ex_dlg.field);
        if (!name[0] || name[0] == '.') { ex_info("Please type a folder name.", NULL); return; }
        ex_child(p, sizeof p, name);
        for (int k = 2; access(p, F_OK) == 0 && k < 100; k++) {
            char nn[48]; snprintf(nn, sizeof nn, "%.32s (%d)", name, k);
            ex_child(p, sizeof p, nn);
            snprintf(name, sizeof name, "%s", nn);
        }
        if (mkdir(p, 0755)) { ex_info("Could not create the folder.", NULL); return; }
        sync();
        ex_sig = ex_read(); ex_select_name(name);
        repaint_client(explorer_draw);
    } else if (ex_dlg_mode == EXD_NEWFILE && b == 0) {
        char nm[200]; snprintf(nm, sizeof nm, "%s", ex_dlg.field);
        if (!nm[0] || nm[0] == '.' || strchr(nm, '/')) { ex_info("Please type a file name.", NULL); return; }
        const char *want = strrchr(ex_newt[ex_new_kind].name, '.');      /* keep the right ending */
        const char *have = strrchr(nm, '.');
        if (want && (!have || strcasecmp(have, want)) && strlen(nm) + strlen(want) < sizeof nm) strcat(nm, want);
        char base[200]; snprintf(base, sizeof base, "%s", nm);
        char *dot = strrchr(base, '.'); char ext[24] = "";
        if (dot && dot != base) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
        ex_child(p, sizeof p, nm);
        for (int k = 2; access(p, F_OK) == 0 && k < 100; k++) {
            snprintf(nm, sizeof nm, "%.150s (%d)%s", base, k, ext);
            ex_child(p, sizeof p, nm);
        }
        FILE *f = fopen(p, "wbx");
        if (!f) { ex_info("Could not create the file.", NULL); return; }
        int len = ex_newt[ex_new_kind].len < 0 ? (int)strlen(ex_newt[ex_new_kind].body) : ex_newt[ex_new_kind].len;
        if (len) fwrite(ex_newt[ex_new_kind].body, 1, (size_t)len, f);
        fclose(f);
        if (ex_newt[ex_new_kind].exec) chmod(p, 0755);
        sync();
        ex_sig = ex_read(); ex_select_name(nm);
        repaint_client(explorer_draw);
    } else if (ex_dlg_mode == EXD_RENAME && b == 0 && ex_sel >= 0) {
        snprintf(name, sizeof name, "%s", ex_dlg.field);
        if (!name[0] || name[0] == '.') { ex_info("Please type a name.", NULL); return; }
        ex_child(p, sizeof p, ex_items[ex_sel].name);
        ex_child(q, sizeof q, name);
        if (strcmp(p, q)) {
            if (access(q, F_OK) == 0) { ex_info("There is already a file", "with that name."); return; }
            if (rename(p, q)) { ex_info("Could not rename it.", NULL); return; }
            sync();
            if (win_of(notepad_draw) >= 0 && !strcmp(np_dir, ex_path) && !strcmp(np_file, ex_items[ex_sel].name)) {
                snprintf(np_file, sizeof np_file, "%s", name);           /* keep Notepad in step */
                np_set_title();
            }
        }
        ex_sig = ex_read(); ex_select_name(name);
        repaint_client(explorer_draw);
    } else if (ex_dlg_mode == EXD_EMPTYBIN && b == 0) {
        if (bin_empty()) ex_info("Some items couldn't be deleted.", NULL);
        ex_sig = ex_read(); ex_single(-1);
        desk_bin_changed();
        repaint_client(explorer_draw);
    } else if (ex_dlg_mode == EXD_DELETE && b == 0 && ex_nselected()) {
        int n, bad = 0; char **v = ex_sel_paths(&n);
        for (int i = 0; v && i < n; i++) if (rm_rf(v[i])) bad++;
        if (v) ex_free_list(v, n);
        if (bad) ex_info(bad == 1 ? "Something couldn't be deleted." : "Some items couldn't be deleted.", NULL);
        if (ex_in_bin()) { bin_tidy_index(); desk_bin_changed(); }
        sync();
        ex_sig = ex_read(); ex_single(-1);
        ex_scroll = imin(ex_scroll, imax(0, ex_n - 1));
        repaint_client(explorer_draw);
    }
}

static void explorer_click(int lx, int ly, int cw, int ch) {
    exd.mode = EXD_NONE;
    if (dlg_click(&ex_dlg, lx, ly, cw, ch, explorer_draw)) return;
    int id = explorer_hit(lx, ly, cw, ch);
    explorer_press(lx, ly, cw, ch, id);
    char p[256];
    if (id == ID_EXBACK) {
        if (ex_nhist) { ex_nhist--; snprintf(p, sizeof p, "%s", ex_hist[ex_nhist]); ex_go(p, 0); }
    } else if (id == ID_EXUP) {
        if (ex_path[0]) { ex_parent(p, sizeof p); ex_go(p, 1); }
    } else if (id >= ID_EXCRUMB && id < ID_EXCRUMB + EX_MAXCRUMB) {
        char labels[EX_MAXCRUMB][40], paths[EX_MAXCRUMB][256];
        ex_crumbs(labels, paths);
        ex_go(paths[id - ID_EXCRUMB], 1);
    } else if (id >= ID_EXCMD && id < ID_EXCMD + EX_NCMD && ex_in_bin()) {
        if (!ex_cmd_enabled(id - ID_EXCMD)) return;
        if (id == ID_EXCMD) ex_bin_restore(0);
        else if (id == ID_EXCMD + 1) ex_bin_restore(1);
        else if (id == ID_EXCMD + 2) ex_bin_empty_ask();
        else if (id == ID_EXCMD + 5) ex_delete();
    } else if (id == ID_EXCMD) ex_new_folder();
    else if (id == ID_EXCMD + 1) ex_clip(1);
    else if (id == ID_EXCMD + 2) ex_clip(0);
    else if (id == ID_EXCMD + 3) ex_paste();
    else if (id == ID_EXCMD + 4) ex_rename();
    else if (id == ID_EXCMD + 5) ex_delete();
    else if (id >= ID_EXNAV && id < ID_EXNAV + EX_NNAV) {
        if (id - ID_EXNAV >= 7 && lx >= EX_LEFT_W - 30) { drive_eject(id - ID_EXNAV - 7); return; }   /* the eject button */
        ex_go(ex_nav[id - ID_EXNAV].path, 1);
    }
    else if (id >= ID_EXDRIVE && id < ID_EXDRIVE + EX_NDRIVE) {
        int i = id - ID_EXDRIVE;
        long long t = ui_ms();
        if (ex_last_idx == 1000 + i && t - ex_last_click < 450) { ex_go(i >= 2 ? rdrv[i - 2].mnt : i ? "/data" : "/", 1); return; }
        ex_sel = i; ex_last_idx = 1000 + i; ex_last_click = t;
        repaint_client(explorer_draw);
    } else if (id >= 0 && id < EX_MAX) {
        long long t = ui_ms();
        if (!ctrl && !shift && ex_last_idx == id && t - ex_last_click < 450) { ex_last_idx = -1; ex_open_item(id); return; }
        if (!ctrl && !shift && ex_multi && ex_selected(id)) exd.collapse = id;   /* keep the group: it may get dragged */
        else ex_click_row(id);
        ex_last_idx = id; ex_last_click = t;
        repaint_client(explorer_draw);
    } else if ((ex_sel >= 0 || ex_multi) && !ctrl) {       /* click on empty space */
        ex_single(-1); repaint_client(explorer_draw);
    }
}

static void explorer_key(int code, int mods) {
    if (dlg_key(&ex_dlg, code, mods, explorer_draw)) return;
    if (mods & MOD_CTRL) {
        if (code == KEY_C) ex_clip(0);
        else if (code == KEY_X) ex_clip(1);
        else if (code == KEY_V) ex_paste();
        else if (code == KEY_A) { ex_select_all(); repaint_client(explorer_draw); }
        return;
    }
    int i = win_of(explorer_draw); if (i < 0) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    int n = ex_path[0] ? ex_n : 2, rows = ex_rows(cw, ch);
    char p[256];
    switch (code) {
    case KEY_DOWN: case KEY_RIGHT: if (n) ex_single(imin(ex_sel + 1, n - 1)); break;
    case KEY_UP: case KEY_LEFT: if (n) ex_single(imax(ex_sel - 1, 0)); break;
    case KEY_HOME: if (n) ex_single(0); break;
    case KEY_END: if (n) ex_single(n - 1); break;
    case KEY_ENTER: case KEY_KPENTER:
        if (ex_sel >= 0) { if (!ex_path[0]) ex_go(ex_sel ? "/data" : "/", 1); else ex_open_item(ex_sel); }
        return;
    case KEY_BACKSPACE: if (ex_path[0]) { ex_parent(p, sizeof p); ex_go(p, 1); } return;
    case KEY_DELETE: ex_delete(); return;
    case KEY_F2: ex_rename(); return;
    default: return;
    }
    if (ex_path[0] && ex_sel >= 0) {
        if (ex_sel < ex_scroll) ex_scroll = ex_sel;
        if (ex_sel >= ex_scroll + rows) ex_scroll = ex_sel - rows + 1;
    }
    repaint_client(explorer_draw);
}

static void explorer_wheel(int amount) {
    int i = win_of(explorer_draw); if (i < 0 || !ex_path[0]) return;
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    ex_scroll = clampi(ex_scroll - amount * 3, 0, imax(0, ex_n - ex_rows(cw, ch)));
    repaint_client(explorer_draw);
}

static void explorer_tick(void) {               /* notice files saved by other apps */
    static int every;
    if (++every % 2 || ex_dlg.active) return;
    if (!ex_path[0]) { repaint_client(explorer_draw); return; }   /* storage numbers */
    char keep[64] = "";
    if (ex_sel >= 0 && ex_sel < ex_n) snprintf(keep, sizeof keep, "%s", ex_items[ex_sel].name);
    long long sig = ex_read();
    if (sig == ex_sig) return;
    ex_sig = sig; ex_sel = -1; ex_multi = 0;
    if (keep[0]) ex_select_name(keep);
    repaint_client(explorer_draw);
}

#include "cp437map.h"                 /* letters for Wi-Fi names */
static int wake_pipe[2] = { -1, -1 };
#include "wayland.c"            /* real Linux apps (Wayland) in ccos windows */

/* ---- resizing windows by dragging an edge or corner ----------------- */
#define RZ_L 1
#define RZ_R 2
#define RZ_T 4
#define RZ_B 8
#define RZ_OUT 5                     /* how far outside the window the edge can still be grabbed */
static int rz_win = -1, rz_edges, rz_mx, rz_my, rz_x, rz_y, rz_w, rz_h;

static int can_resize(const Win *w) { return w->open && !w->minimized && !w->maximized && !w->no_max && !w->fullscreen; }

/* the window whose edge is under the mouse (-1 none); *edges says which edges */
static int resize_hit(int *edges) {
    if (ON_TASKBAR) return -1;
    for (int k = nz - 1; k >= 0; k--) {
        int i = zorder[k]; Win *w = &wins[i];
        if (w->minimized) continue;
        int in_win = in_rect(w->x, w->y, w->w, w->h);
        if (!in_rect(w->x - RZ_OUT, w->y - RZ_OUT, w->w + 2 * RZ_OUT, w->h + 2 * RZ_OUT)) continue;
        if (!can_resize(w)) { if (in_win) return -1; continue; }
        int e = 0;
        if (mx < w->x + 4) e |= RZ_L;
        if (mx >= w->x + w->w - 5) e |= RZ_R;
        if (my < w->y + 3) e |= RZ_T;
        if (my >= w->y + w->h - 5) e |= RZ_B;
        if (e & (RZ_L | RZ_R)) { if (my < w->y + 16) e |= RZ_T; if (my >= w->y + w->h - 16) e |= RZ_B; }
        if (e & (RZ_T | RZ_B)) { if (mx < w->x + 16) e |= RZ_L; if (mx >= w->x + w->w - 16) e |= RZ_R; }
        if (e) { *edges = e; return i; }
        if (in_win) return -1;                 /* the window is in front of anything below */
    }
    return -1;
}

static int cursor_for_edges(int e) {
    if ((e & RZ_L && e & RZ_T) || (e & RZ_R && e & RZ_B)) return CUR_NWSE;
    if ((e & RZ_R && e & RZ_T) || (e & RZ_L && e & RZ_B)) return CUR_NESW;
    if (e & (RZ_L | RZ_R)) return CUR_EW;
    return CUR_NS;
}

static void resize_start(int i, int edges) {
    raise_win(i);
    rz_win = i; rz_edges = edges; rz_mx = mx; rz_my = my;
    rz_x = wins[i].x; rz_y = wins[i].y; rz_w = wins[i].w; rz_h = wins[i].h;
}
static void wl_resize_from_app(int win, int edges) { if (can_resize(&wins[win])) resize_start(win, edges); }

static void resize_follow(void) {
    if (rz_win < 0 || !wins[rz_win].open) { rz_win = -1; return; }
    Win *w = &wins[rz_win];
    int dx = mx - rz_mx, dy = my - rz_my;
    int minw = 2 * FRAME + 240, minh = TITLE_H + FRAME + 120;
    int nx = rz_x, ny = rz_y, nw = rz_w, nh = rz_h;
    if (rz_edges & RZ_R) nw = imax(minw, rz_w + dx);
    if (rz_edges & RZ_L) { nw = imax(minw, rz_w - dx); nx = rz_x + rz_w - nw; }
    if (rz_edges & RZ_B) nh = imax(minh, imin(rz_h + dy, H - TASKBAR_H - rz_y));
    if (rz_edges & RZ_T) { nh = imax(minh, rz_h - dy); ny = rz_y + rz_h - nh;
                           if (ny < 0) { nh += ny; ny = 0; } }
    if (nx == w->x && ny == w->y && nw == w->w && nh == w->h) return;
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    w->x = nx; w->y = ny; w->w = nw; w->h = nh;
    int x0 = imin(ox, nx) - 2, y0 = imin(oy, ny) - 1;            /* old + new area */
    int x1 = imax(ox + ow, nx + nw) + 5, y1 = imax(oy + oh, ny + nh) + 6;
    repaint(x0, y0, x1 - x0, y1 - y0);
    if (w->resized) w->resized();                                 /* Wayland apps redraw at the new size */
}

/* ---- cursor --------------------------------------------------------- */
static int cur_x = -100, cur_y = -100, cur_shape = CUR_ARROW, cur_col = -1, cur_size = -1;

static inline uint32_t to_fb(uint32_t c) { return swap_rb ? RGB(B(c), G(c), R(c)) : c; }

static void blit(int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) {
        uint32_t *dst = fbmem + (size_t)y * stride + x0, *src = back + (size_t)y * W + x0;
        if (!swap_rb) memcpy(dst, src, (size_t)(x1 - x0) * 4);
        else for (int x = 0; x < x1 - x0; x++) dst[x] = to_fb(src[x]);
    }
}

static void put_fb(int x, int y, uint32_t c, int a) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint32_t base = back[(size_t)y * W + x];
    fbmem[(size_t)y * stride + x] = to_fb(a >= 255 ? c : mix(base, c, a / 255.0f));
}

#define CUR_HIDDEN 99
static void draw_cursor_fb(void) {
    if (cur_shape == CUR_HIDDEN) return;              /* a game or video asked for no pointer */
    render_cursor(cur_shape, cur_col, cursor_scales[cur_size], cur_x, cur_y, put_fb);
}

/* which pointer to show right now */
static int pointer_shape(void) {
    if (rz_win >= 0) return cursor_for_edges(rz_edges);
    if (wl_pointer_locked() && !menu_open && !pop.open) return CUR_HIDDEN;
    if (mouse_down || menu_open || pop.open || net_panel || clk_panel) return CUR_ARROW;
    if (wl_cursor_hidden()) return CUR_HIDDEN;
    int e; if (resize_hit(&e) >= 0) return cursor_for_edges(e);
    return CUR_ARROW;
}

static void mark_cursor(void) {
    if (cur_size < 0) return;
    int x0, y0, x1, y1; cursor_box(cursor_scales[cur_size], &x0, &y0, &x1, &y1);
    mark(cur_x + x0, cur_y + y0, x1 - x0, y1 - y0);
}

static void rec_changed(void);
static void present(void) {
    int shape = pointer_shape();
    if (mx != cur_x || my != cur_y || shape != cur_shape || set_cursor_col != cur_col || set_cursor_size != cur_size) {
        rec_changed();
        mark_cursor();                                   /* where it was */
        cur_x = mx; cur_y = my; cur_shape = shape; cur_col = set_cursor_col; cur_size = set_cursor_size;
        mark_cursor();                                   /* where it is now */
    }
    if (!ndirty) return;
    rec_changed();
    for (int i = 0; i < ndirty; i++) blit(dirty[i].x0, dirty[i].y0, dirty[i].x1, dirty[i].y1);
    ndirty = 0;
    draw_cursor_fb();
}

static void update_hover(void) {
    Hot h = find_hot();
    if (h.type != hot.type || h.a != hot.a || h.b != hot.b) {
        Hot old = hot;
        hot = h;
        repaint_hot(old);
        repaint_hot(h);
    }
}

static void net_panel_set(int open);
static void ms_reset(int open);
static void set_menu(int open) {
    if (open && net_panel) net_panel_set(0);
    if (open) fp_scan();                              /* apps installed since last time */
    if (open == menu_open) return;
    ms_reset(open);
    menu_open = open;
    repaint_menu();
    hot.type = HOT_NONE;
    repaint_hot((Hot){ HOT_ORB, 0, 0 });
    update_hover();
}

/* ---- input devices -------------------------------------------------- */
#define MAXDEV 32
static struct {
    int fd, abs, xmin, xmax, ymin, ymax;
    int tp, xres, yres;                 /* laptop touchpad: moves the pointer like a mouse */
    int pad;                            /* a game controller: left alone, for games */
} dev[MAXDEV];

static void scan_devices(void) {
    char path[64];
    for (int i = 0; i < MAXDEV; i++) {
        if (dev[i].fd > 0) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (dev[i].pad) { if (access(path, F_OK) != 0) dev[i].pad = 0; continue; }   /* unplugged: forget */
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);      /* write: keyboard lamps */
        if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        {   /* game controllers (Xbox, PlayStation, Switch...): games read them directly */
            unsigned char kb[KEY_MAX / 8 + 1] = { 0 };
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb);
            #define HASK(k) (kb[(k) / 8] & (1 << ((k) % 8)))
            if (HASK(BTN_GAMEPAD) || HASK(BTN_JOYSTICK) || HASK(BTN_TRIGGER_HAPPY1)) { close(fd); dev[i].pad = 1; continue; }
            #undef HASK
        }
        ioctl(fd, EVIOCGRAB, 1);
        {   /* a new keyboard: show Caps Lock on its lamp too */
            unsigned long leds = 0;
            if (ioctl(fd, EVIOCGBIT(EV_LED, sizeof leds), &leds) >= 0 && (leds & (1UL << LED_CAPSL))) {
                struct input_event le[2] = { { .type = EV_LED, .code = LED_CAPSL, .value = capslock_state },
                                             { .type = EV_SYN, .code = SYN_REPORT } };
                if (write(fd, le, sizeof le) < 0) {}
            }
        }
        dev[i].fd = fd;
        struct input_absinfo ax, ay;
        dev[i].abs = dev[i].tp = 0;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 &&
            ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
            dev[i].xmin = ax.minimum; dev[i].xmax = ax.maximum;
            dev[i].ymin = ay.minimum; dev[i].ymax = ay.maximum;
            /* a touchpad says "I'm a pointer" and reports fingers; a tablet
               (like QEMU's) maps straight onto the screen */
            unsigned char props[8] = { 0 }, keys[KEY_MAX / 8 + 1] = { 0 };
            ioctl(fd, EVIOCGPROP(sizeof props), props);
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
            int pointer = props[INPUT_PROP_POINTER / 8] & (1 << (INPUT_PROP_POINTER % 8));
            int finger = keys[BTN_TOOL_FINGER / 8] & (1 << (BTN_TOOL_FINGER % 8));
            if (pointer && finger) {
                dev[i].tp = 1;
                /* units per millimetre; guess a 100 x 60 mm pad if the driver doesn't say */
                dev[i].xres = ax.resolution > 0 ? ax.resolution : imax(1, (ax.maximum - ax.minimum) / 100);
                dev[i].yres = ay.resolution > 0 ? ay.resolution : imax(1, (ay.maximum - ay.minimum) / 60);
            } else dev[i].abs = 1;
        }
    }
}

static void caps_leds(void) {           /* the Caps Lock lamp on every keyboard */
    struct input_event le[2] = { { .type = EV_LED, .code = LED_CAPSL, .value = capslock_state },
                                 { .type = EV_SYN, .code = SYN_REPORT } };
    for (int i = 0; i < MAXDEV; i++)
        if (dev[i].fd > 0 && write(dev[i].fd, le, sizeof le) < 0) {}
}

static void restore_console(void) {
    if (tty >= 0) ioctl(tty, KDSETMODE, KD_TEXT);
}

static void run(const char *cmd) {
    if (!cmd) return;
    if (!strncmp(cmd, "flatpak:", 8)) {               /* an app from Flathub */
        char id[96]; snprintf(id, sizeof id, "%s", cmd + 8);
        char *const argv[] = { "ccos-flatpak-run", id, NULL };
        wl_spawn("/usr/bin/ccos-flatpak-run", argv);
        return;
    }
    if (cmd[0] == '@') {                              /* built-in apps */
        app_base(cmd, launch_cmd, sizeof launch_cmd);
        if (!strcmp(cmd, "@terminal")) {              /* foot, a real Wayland terminal */
            char *const argv[] = { "foot", "--login-shell", NULL };   /* login shell: the usual root@ccos prompt */
            wl_spawn("foot", argv);
        }
        if (!strcmp(cmd, "@fileop")) {
            static const Win t = { .title = "Copying", .icon = ICON_FOLDER, .no_max = 1, .draw = fileop_draw, .click = fileop_click,
                                   .key = fileop_key, .on_close = fileop_on_close };
            open_win(&t, FO_W, FO_H);
            return;
        }
        if (!strcmp(cmd, "@camera")) {
            static const Win t = { .title = "Camera", .icon = ICON_CAMERA, .draw = cam_draw, .click = cam_click, .hit = cam_hit,
                                   .key = cam_key, .tick = cam_tick, .on_close = cam_on_close, .fd = cam_fd, .wake = cam_wake, .no_dark = 1 };
            open_win(&t, CAM_W, CAM_H);
            cam_start();
            return;
        }
        if (!strcmp(cmd, "@record")) {
            static const Win t = { .title = "Record", .icon = ICON_RECORD, .no_max = 1, .draw = rec_draw, .click = rec_click,
                                   .hit = rec_hit, .tick = rec_app_tick };
            open_win(&t, REC_W, REC_H);
            return;
        }
        if (!strcmp(cmd, "@print")) {
            static const Win t = { .title = "Print", .icon = ICON_PRINTER, .no_max = 1, .draw = print_draw, .click = print_click,
                                   .key = print_key, .tick = print_tick, .on_close = print_on_close };
            open_win(&t, PR_W, PR_H);
            return;
        }
        if (!strcmp(cmd, "@settings")) {
            static const Win t = { .title = "Settings", .icon = ICON_GEAR, .no_max = 1,
                                   .draw = settings_draw, .click = settings_click, .drag = settings_drag,
                                   .key = settings_key, .wheel = settings_wheel,
                                   .tick = settings_tick };
            open_win(&t, ST_W, ST_H);
        }
        if (!strncmp(cmd, "@explorer", 9)) {
            static const Win t = { .title = ex_title, .icon = ICON_FOLDER,
                                   .draw = explorer_draw, .click = explorer_click, .hit = explorer_hit,
                                   .elem_rect = explorer_elem_rect, .key = explorer_key,
                                   .wheel = explorer_wheel, .tick = explorer_tick, .rclick = explorer_rclick,
                                   .drag = explorer_drag, .release = explorer_release };
            const char *where = cmd[9] == ' ' ? cmd + 10 : "";
            if (win_of(explorer_draw) < 0) {               /* fresh window: start at "where" */
                ex_nhist = 0; ex_dlg.active = 0; ex_sel = -1; ex_multi = 0; ex_scroll = 0;
                snprintf(ex_path, sizeof ex_path, "%s", where);
                if (!strcmp(ex_path, NP_DIR)) { mkdir("/data", 0755); mkdir(NP_DIR, 0755); }
                ex_sig = ex_read();
                ex_set_title();
                open_win(&t, 760, 460);
            } else {
                open_win(&t, 760, 460);                    /* bring it to the front */
                if (strcmp(where, ex_path)) ex_go(where, 1);
            }
        }
        if (!strcmp(cmd, "@notepad")) {
            static const Win t = { .title = np_title, .icon = ICON_NOTE,
                                   .draw = notepad_draw, .click = notepad_click, .hit = notepad_hit,
                                   .elem_rect = notepad_elem_rect, .key = notepad_key,
                                   .wheel = notepad_wheel, .on_close = notepad_on_close,
                                   .drag = notepad_drag, .release = notepad_release,
                                   .rclick = notepad_rclick };
            if (win_of(notepad_draw) < 0) { np_reset(); np_mb.open = -1; np_dlg.active = 0; }
            open_win(&t, 600, 380);
        }
        if (!strcmp(cmd, "@mines")) {
            static const Win t = { .title = "Minesweeper", .icon = ICON_MINE, .no_max = 1,
                                   .draw = mines_draw, .click = mines_click, .hit = mines_hit,
                                   .elem_rect = mines_elem_rect, .key = mines_key,
                                   .rclick = mines_rclick, .tick = mines_tick };
            if (win_of(mines_draw) < 0) { ms_mb.open = -1; ms_new_game(); }
            int cw, ch; ms_client_size(&cw, &ch);
            open_win(&t, cw, ch);
        }
        if (!strncmp(cmd, "@photos", 7) || !strncmp(cmd, "@videos", 7) || !strncmp(cmd, "@music", 6)) {
            const char *arg = strchr(cmd, ' '); arg = arg ? arg + 1 : NULL;
            if (cmd[1] == 'p') {
                static const Win t = { .title = "Photos", .icon = ICON_PHOTO, .draw = photos_draw, .click = photos_click,
                                       .hit = photos_hit, .elem_rect = photos_elem_rect, .drag = photos_drag, .release = photos_release,
                                       .wheel = photos_wheel, .key = photos_key, .resized = photos_resized, .on_close = photos_on_close };
                photos_open(arg);
                open_win(&t, imin(W - 80, 960), imin(H - TASKBAR_H - 80, 680));
                ph_title(); repaint_client(photos_draw);
            } else if (cmd[1] == 'v') {
                static const Win t = { .title = "Videos", .icon = ICON_VIDEO, .no_dark = 1, .draw = videos_draw, .click = videos_click,
                                       .hit = videos_hit, .elem_rect = videos_elem_rect, .drag = videos_drag, .release = videos_release,
                                       .wheel = videos_wheel, .key = videos_key, .motion = videos_motion, .fd = videos_fd,
                                       .wake = videos_wake, .tick = videos_tick, .on_close = videos_on_close };
                videos_open(arg);
                open_win(&t, imin(W - 80, 960), imin(H - TASKBAR_H - 80, 600));
                vd_title(); repaint_client(videos_draw);
            } else {
                static const Win t = { .title = "Music", .icon = ICON_MUSIC, .draw = music_draw, .click = music_click,
                                       .hit = music_hit, .elem_rect = music_elem_rect, .drag = music_drag, .release = music_release,
                                       .wheel = music_wheel, .key = music_key, .fd = music_fd, .wake = music_wake,
                                       .on_close = music_on_close };
                music_open(arg);
                open_win(&t, 560, imin(H - TASKBAR_H - 80, 560));
                mu_title(); repaint_client(music_draw);
            }
        }
        if (!strcmp(cmd, "@store")) {
            static const Win t = { .title = "Software Manager", .icon = ICON_BAG,
                                   .draw = store_draw, .click = store_click, .hit = store_hit,
                                   .elem_rect = store_elem_rect, .key = store_key, .wheel = store_wheel,
                                   .tick = store_tick, .fd = sm_fd, .wake = sm_wake, .on_close = store_on_close,
                                   .resized = store_resized };
            store_open();
            open_win(&t, imin(W - 60, 980), imin(H - TASKBAR_H - 70, 700));
        }
        if (!strcmp(cmd, "@solitaire")) {
            static const Win t = { .title = "Solitaire", .icon = ICON_CARDS, .no_max = 1, .no_dark = 1,
                                   .draw = solitaire_draw, .click = solitaire_click, .drag = solitaire_drag,
                                   .release = solitaire_release, .hit = solitaire_hit,
                                   .elem_rect = solitaire_elem_rect, .key = solitaire_key, .tick = solitaire_tick };
            if (win_of(solitaire_draw) < 0) { so_mb.open = -1; so_new_game(); }
            open_win(&t, SO_W, SO_H);
        }
        if (!strcmp(cmd, "@calc")) {
            static const Win t = { .title = "Calculator", .icon = ICON_CALC, .no_max = 1,
                                   .draw = calc_draw, .click = calc_click, .hit = calc_hit,
                                   .elem_rect = calc_elem_rect, .key = calc_key,
                                   .release = calc_release };
            open_win(&t, CALC_CW, CALC_CH);
        }
        return;
    }
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* hand the screen to the shutdown script: "poweroff", "reboot" or "shell" */
static void power_action(const char *mode) {
    execl("/sbin/ccos-shutdown", "ccos-shutdown", mode, (char *)NULL);
    restore_console();                       /* only if the script is missing */
    execl("/bin/busybox", "poweroff", "-f", (char *)NULL);
}
static void shutdown_now(void) { power_action("poweroff"); }

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void popm_repaint(void) {
    repaint(pop.x, pop.y, POP_W + 4, pop_h() + 4);
    if (pop.sub_item >= 0) { int x, y, w, h; pop_sub_rect(&x, &y, &w, &h); repaint(x, y, w + 4, h + 4); }
}
static void pop_close(void) {
    if (!pop.open) return;
    pop.open = 0;
    popm_repaint();
}
/* give the menu just shown a submenu, opening beside item 'item' */
static void pop_set_sub(int item, int n, const char *const *labels, const int *acts, const int *icons) {
    pop.sub_item = item; pop.sub_open = 0; pop.sub_n = 0;
    for (int i = 0; i < n && i < POP_SUBMAX; i++) {
        snprintf(pop.sub_label[i], sizeof pop.sub_label[0], "%s", labels[i]);
        pop.sub_act[i] = acts[i]; pop.sub_icon[i] = icons[i]; pop.sub_n++;
    }
}

/* build a right-click menu for an app; win = its window or -1 */
/* a menu that isn't about an app: the desktop's own, or a file's */
static void pop_show_list(int x, int y, int icon, const char *path, int n, const char *const *labels, const int *acts) {
    pop_close();
    memset(&pop, 0, sizeof pop);
    pop.win = -1; pop.desk = -1; pop.icon = icon; pop.sub_item = -1;
    if (path) snprintf(pop.path, sizeof pop.path, "%s", path);
    for (int i = 0; i < n && i < MAXPOP; i++) { snprintf(pop.label[i], sizeof pop.label[0], "%s", labels[i]); pop.act[i] = acts[i]; pop.n++; }
    pop.x = clampi(x, 0, W - POP_W - 4);
    pop.y = clampi(y, 0, H - TASKBAR_H - pop_h() - 2);
    pop.open = 1;
    repaint(pop.x, pop.y, POP_W + 4, pop_h() + 4);
}
static int pop_desk_next = -1;          /* set just before pop_show() for a desktop icon */
static void pop_show(const char *cmd, int win, int x, int y, int above_taskbar, int from_menu) {
    const AppInfo *ai = app_info(cmd);
    if (!ai && win < 0) return;
    pop_close();
    memset(&pop, 0, sizeof pop); pop.sub_item = -1;
    app_base(cmd, pop.cmd, sizeof pop.cmd);
    pop.win = win; pop.from_menu = from_menu; pop.desk = pop_desk_next; pop_desk_next = -1;
    pop.icon = pop.desk >= 0 ? desk_icons[pop.desk].icon : ai ? ai->icon : (win >= 0 ? wins[win].icon : 0);
    snprintf(pop.name, sizeof pop.name, "%s", pop.desk >= 0 ? desk_icons[pop.desk].label : ai ? ai->label : "");
    snprintf(pop.cmd, sizeof pop.cmd, "%s", pop.desk >= 0 ? desk_icons[pop.desk].cmd : cmd);   /* "@explorer /data/Documents" stays whole */
    if (from_menu) {                                       /* the start menu's own name for it */
        for (int k = 0; left_items[k].label; k++) if (!strcmp(left_items[k].cmd, cmd)) snprintf(pop.name, sizeof pop.name, "%s", left_items[k].label);
        for (int k = 0; right_items[k].label; k++) if (!strcmp(right_items[k].cmd, cmd)) { snprintf(pop.name, sizeof pop.name, "%s", right_items[k].label); pop.icon = right_items[k].icon; }
    }
    snprintf(pop.label[pop.n], sizeof pop.label[0], "%s", pop.name[0] ? pop.name : wins[win].title); pop.act[pop.n++] = PA_OPEN;
    if (ai) {
        int pinned = pin_index(cmd) >= 0;
        snprintf(pop.label[pop.n], sizeof pop.label[0], "%s", pinned ? "Unpin from taskbar" : "Pin to taskbar");
        pop.act[pop.n++] = pinned ? PA_UNPIN : PA_PIN;
    }
    if (from_menu && ai && desk_find(cmd) < 0) { snprintf(pop.label[pop.n], sizeof pop.label[0], "Add to Desktop"); pop.act[pop.n++] = PA_DESK_ADD; }
    if (pop.desk >= 0) { snprintf(pop.label[pop.n], sizeof pop.label[0], "Delete"); pop.act[pop.n++] = PA_DESK_DEL; }
    if (win >= 0) { snprintf(pop.label[pop.n], sizeof pop.label[0], "Close window"); pop.act[pop.n++] = PA_CLOSE; }
    pop.x = clampi(x, 0, W - POP_W - 4);
    pop.y = above_taskbar ? H - TASKBAR_H - pop_h() - 2 : clampi(y, 0, H - TASKBAR_H - pop_h() - 2);
    pop.open = 1;
    repaint(pop.x, pop.y, POP_W + 4, pop_h() + 4);
}

/* File Explorer's right-click menu items */
static void ex_pop_action(int act, const char *path) {
    if (win_of(explorer_draw) < 0) return;
    switch (act) {
    case PA_CUT: ex_clip(1); break;
    case PA_COPY: ex_clip(0); break;
    case PA_PASTE: ex_paste(); break;
    case PA_RENAME: ex_rename(); break;
    case PA_DELETE: ex_delete(); break;
    case PA_ZIP: ex_compress(); break;
    case PA_UNZIP: ex_extract(path); break;
    case PA_NEWFOLDER: ex_new_folder(); break;
    case PA_SELALL: ex_select_all(); repaint_client(explorer_draw); break;
    case PA_RESTORE: ex_bin_restore(0); break;
    case PA_RESTOREALL: ex_bin_restore(1); break;
    case PA_EMPTYBIN: ex_bin_empty_ask(); break;
    default: if (act >= PA_NEWFILE && act < PA_NEWFILE + EX_NNEWT) ex_new_file(act - PA_NEWFILE); break;
    }
}
static void pop_do(int i) {
    if (i >= 100) {                                      /* a submenu item */
        int sa = pop.sub_act[i - 100];
        char sp[512]; snprintf(sp, sizeof sp, "%s", pop.path);
        pop_close();
        ex_pop_action(sa, sp); update_hover(); return;
    }
    if (pop.act[i] == PA_NEWMENU) { pop.sub_open = 1; popm_repaint(); return; }
    int act = pop.act[i], win = pop.win, from_menu = pop.from_menu, desk = pop.desk;
    char cmd[112], name[40]; snprintf(cmd, sizeof cmd, "%s", pop.cmd); snprintf(name, sizeof name, "%s", pop.name);
    char path[512]; snprintf(path, sizeof path, "%s", pop.path);
    pop_close();
    if (act == PA_OPEN_FILE) { open_file(path); update_hover(); return; }
    if (act == PA_SET_WALL) {
        if (set_wallpaper_file(path)) { make_wallpaper(); repaint_all(); if (win_of(settings_draw) >= 0) st_repaint(); }
        update_hover(); return;
    }
    if (act == PA_PRINT) { print_open(path); update_hover(); return; }
    if (act >= PA_CUT && act < PA_NEWFILE + 8) { ex_pop_action(act, path); update_hover(); return; }
    if (act == PA_EJECT) { drive_eject(rd_of_path(path)); update_hover(); return; }
    if (act == PA_CUSTOMIZE) { st_tab = TAB_LOOK; run("@settings"); st_repaint(); update_hover(); return; }
    if (act == PA_OPEN) {
        if (from_menu) set_menu(0);
        if (win >= 0 && wins[win].open) restore_win(win); else run(cmd);
    } else if (act == PA_PIN && npins < MAXPINS && pin_index(cmd) < 0) {
        app_base(cmd, pins[npins++], sizeof pins[0]);
        save_settings(); repaint_taskbar();
    } else if (act == PA_UNPIN) {
        int k = pin_index(cmd);
        if (k >= 0) {
            memmove(pins[k], pins[k + 1], sizeof pins[0] * (size_t)(npins - k - 1));
            npins--;
            save_settings(); repaint_taskbar();
        }
    } else if (act == PA_DESK_ADD) {
        if (from_menu) set_menu(0);
        if (desk_add(cmd, name)) { save_settings(); repaint_all(); }
    } else if (act == PA_DESK_DEL && desk >= 0 && desk < ndesk && !strcmp(desk_icons[desk].cmd, cmd)) {
        int x, y, w, h; di_rect(desk, &x, &y, &w, &h);
        if (di_sel == desk) di_sel = -1; else if (di_sel > desk) di_sel--;
        desk_remove(desk);
        save_settings(); repaint(x, y, w, h);
    } else if (act == PA_CLOSE && win >= 0 && wins[win].open) {
        if (!wins[win].on_close || wins[win].on_close()) close_win(win);
    }
    update_hover();
}

static void net_panel_set(int open) {
    if (open == net_panel) return;
    if (open) clk_panel_set(0);
    net_panel = open;
    if (open) {
        net_poll();
        if (wifi.present && set_net && set_wifi && wifi.ctl) { wpa_ok("SCAN"); wifi_last_scan = time(NULL); }
        wifi_poll(1); net_fast = 4;
    } else { wifi_sel = -1; wifi_sel_esc[0] = 0; wifi_pass[0] = 0; if (pend_id < 0) wifi_msg[0] = 0; }
    netp_place();
    netp_repaint(); tray_repaint();
}

static void press(void) {
    update_hover();
    if (hot.type == HOT_CLOCK) { pop_close(); set_menu(0); net_panel_set(0); clk_panel_set(!clk_panel); return; }
    if (clk_panel) {
        if (hot.type == HOT_CLKP) { if (hot.a) clkp_click(hot.a); return; }
        clk_panel_set(0);                        /* clicked elsewhere: close it */
        if (hot.type != HOT_TRAY) return;
    }
    if (hot.type == HOT_TRAY) { pop_close(); set_menu(0); net_panel_set(!net_panel); return; }
    if (hot.type == HOT_BAT) { pop_close(); set_menu(0); st_tab = TAB_POWER; run("@settings"); st_repaint(); return; }
    if (hot.type == HOT_REC) { rec_stop(); return; }
    if (net_panel) {
        if (hot.type == HOT_NETP) { net_do(hot.a); update_hover(); return; }
        if (in_rect(netp_x0, netp_y0, NETP_W, netp_h)) return;
        net_panel_set(0);                        /* clicked elsewhere: close it */
        return;
    }
    if (pop.open) {                                    /* a click closes the popup (or picks from it) */
        int a = hot.type == HOT_POPUP ? hot.a : -1;
        if (a >= 0) pop_do(a); else pop_close();
        return;
    }
    if (!menu_open) {                                  /* grabbing a window's edge: resize it */
        int e, i = resize_hit(&e);
        if (i >= 0) { resize_start(i, e); return; }
    }
    switch (hot.type) {
    case HOT_ORB:   set_menu(!menu_open); return;
    case HOT_SD:
        if (hot.a == 2) { set_menu(0); power_sleep(); return; }
        power_action(hot.a == 0 ? "poweroff" : "reboot"); return;
    case HOT_LITEM: { const char *c = left_items[hot.a].cmd; set_menu(0); run(c); return; }
    case HOT_SRES: msr_open(hot.a); return;
    case HOT_SBOX: return;
    case HOT_RITEM: { const char *c = right_items[hot.a].cmd; set_menu(0); run(c); return; }
    case HOT_TASK: {
        TbItem it[MAXPINS + MAXWIN];
        int n = tb_items(it), k = hot.a;
        set_menu(0);
        if (k >= n) return;
        if (it[k].win < 0) { run(pins[it[k].pin]); update_hover(); return; }   /* pinned: launch */
        int i = it[k].win;
        if (wins[i].minimized) restore_win(i);
        else if (i == active_win()) minimize_win(i);
        else restore_win(i);
        update_hover();
        return;
    }
    case HOT_CAP: {
        int i = hot.a, b = hot.b;
        if (b == CAP_CLOSE) { if (!wins[i].on_close || wins[i].on_close()) close_win(i); }
        else if (b == CAP_MAX) { raise_win(i); if (!wins[i].no_max) toggle_max(i); }
        else minimize_win(i);
        hot.type = HOT_NONE;
        update_hover();
        return;
    }
    }
    if (in_menu()) return;
    if (menu_open) { set_menu(0); }

    for (int k = nz - 1; k >= 0; k--) {              /* click on a window */
        int i = zorder[k];
        Win *w = &wins[i];
        if (w->minimized || !in_rect(w->x, w->y, w->w, w->h)) continue;
        raise_win(i);
        int cx, cy, cw, ch; client_rect(w, &cx, &cy, &cw, &ch);
        if (in_rect(cx, cy, cw, ch)) {
            if (w->drag) content_drag = i;
            if (w->click) w->click(mx - cx, my - cy, cw, ch);
        } else if (my < w->y + TITLE_H) {               /* title bar */
            long long t = now_ms();
            if (last_click_win == i && t - last_click_ms < 400) {
                if (!w->no_max) toggle_max(i);
                last_click_win = -1; return;
            }
            last_click_win = i; last_click_ms = t;
            if (!w->maximized) { drag_win = i; drag_dx = mx - w->x; drag_dy = my - w->y; }
        }
        return;
    }

    /* the desktop itself: icons */
    int di = di_at_mouse();
    int old = di_sel;
    if (di >= 0) {
        long long t = now_ms();
        if (di == di_last && t - di_last_click < 450) {          /* double-click: open it */
            di_last = -1; di_sel = di;
            if (old >= 0 && old != di) repaint_hot((Hot){ HOT_ICON, old, 0 });
            repaint_hot((Hot){ HOT_ICON, di, 0 });
            run(desk_icons[di].cmd);
            return;
        }
        di_last = di; di_last_click = t;
        di_sel = di;
        int x, y, w, h; di_rect(di, &x, &y, &w, &h);
        di_drag = di; di_dx = mx - x; di_dy = my - y; di_moved = 0;
    } else di_sel = -1;
    if (old >= 0 && old != di_sel) repaint_hot((Hot){ HOT_ICON, old, 0 });
    if (di_sel >= 0) repaint_hot((Hot){ HOT_ICON, di_sel, 0 });
}

/* drop a dragged icon onto the nearest grid spot (swap if it's taken) */
static void di_drop(void) {
    int i = di_drag;
    di_drag = -1;
    if (i < 0 || !di_moved) return;
    int ox = di_x, oy = di_y;
    int maxc = (W - DI_X0) / DI_CW - 1, maxr = (H - TASKBAR_H - DI_Y0) / DI_CH - 1;
    int c = clampi((ox - DI_X0 + DI_CW / 2) / DI_CW, 0, maxc);
    int r = clampi((oy - DI_Y0 + DI_CH / 2) / DI_CH, 0, maxr);
    for (int k = 0; k < NDESK; k++)
        if (k != i && desk_icons[k].col == c && desk_icons[k].row == r) {
            desk_icons[k].col = desk_icons[i].col; desk_icons[k].row = desk_icons[i].row;
            int x, y, w, h; di_rect(k, &x, &y, &w, &h); repaint(x, y, w, h);
        }
    int sx = DI_X0 + desk_icons[i].col * DI_CW, sy = DI_Y0 + desk_icons[i].row * DI_CH;
    desk_icons[i].col = c; desk_icons[i].row = r;
    di_moved = 0;
    repaint(ox, oy, DI_W, DI_H);                         /* where it was dropped */
    repaint(sx, sy, DI_W, DI_H);                         /* where it came from */
    int x, y, w, h; di_rect(i, &x, &y, &w, &h); repaint(x, y, w, h);
    save_settings();
}

static void di_follow_mouse(void) {
    if (di_drag < 0 || !mouse_down) return;
    int nx = mx - di_dx, ny = my - di_dy;
    if (!di_moved) {
        int x, y, w, h; di_rect(di_drag, &x, &y, &w, &h);
        if (abs(nx - x) < 5 && abs(ny - y) < 5) return;          /* not a drag yet */
        di_moved = 1; di_x = x; di_y = y;
    }
    int ox = di_x, oy = di_y;
    di_x = clampi(nx, 0, W - DI_W); di_y = clampi(ny, 0, H - TASKBAR_H - DI_H);
    int x0 = imin(ox, di_x), y0 = imin(oy, di_y);
    repaint(x0, y0, abs(di_x - ox) + DI_W, abs(di_y - oy) + DI_H);
}

/* the top window under the mouse, or -1 */
static int win_under_mouse(void) {
    for (int k = nz - 1; k >= 0; k--) {
        Win *w = &wins[zorder[k]];
        if (!w->minimized && in_rect(w->x, w->y, w->w, w->h)) return zorder[k];
    }
    return -1;
}

static void rpress(void) {                            /* right mouse button */
    if (stick_wait || snip_on) return;
    if (wl_popup_button(0x111, 1)) { wl_popup_button(0x111, 0); return; }   /* a Wayland app's menu */
    if (pop.open) { pop_close(); return; }
    update_hover();
    if (menu_open) {                                    /* start menu: pin a program */
        if (hot.type == HOT_LITEM) pop_show(left_items[hot.a].cmd, -1, mx, my, 0, 1);
        else if (hot.type == HOT_RITEM) pop_show(right_items[hot.a].cmd, -1, mx, my, 0, 1);
        else if (!in_menu()) set_menu(0);
        return;
    }
    if (ON_TASKBAR) {                          /* taskbar: the jump menu */
        if (hot.type != HOT_TASK) return;
        TbItem it[MAXPINS + MAXWIN];
        int n = tb_items(it), k = hot.a;
        if (k >= n) return;
        const char *cmd = it[k].win >= 0 ? wins[it[k].win].cmd : pins[it[k].pin];
        pop_show(cmd, it[k].win, mx - 40, 0, 1, 0);
        return;
    }
    int i = win_under_mouse();
    if (i < 0) {                                        /* desktop icon */
        int di = di_at_mouse();
        if (di >= 0) { pop_desk_next = di; pop_show(desk_icons[di].cmd, -1, mx, my, 0, 0); }
        else {                                          /* the background itself */
            static const char *const lb[] = { "Customize" };
            static const int ac[] = { PA_CUSTOMIZE };
            pop_show_list(mx, my, ICON_GEAR, NULL, 1, lb, ac);
        }
        return;
    }
    raise_win(i);
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (wins[i].rclick && in_rect(cx, cy, cw, ch)) wins[i].rclick(mx - cx, my - cy, cw, ch);
}

static void wheel(int amount) {
    if (stick_wait || snip_on) return;
    int i = win_under_mouse();
    if (i >= 0 && wins[i].wheel) wins[i].wheel(amount);
}

static void on_signal(int s) { (void)s; restore_console(); _exit(0); }

/* ---- laptop touchpad --------------------------------------------------
 * One finger moves the pointer, two fingers scroll, a quick tap clicks
 * (two-finger tap = right click). Pressing the pad clicks; pressing in the
 * bottom-right corner, or with two fingers down, is a right click. */
static void left_button(int down) {
    if (stick_wait) { if (down) stick_wait_click(); return; }
    if (snip_on) { snip_button(down); return; }
    if ((down ? !mouse_down : 1) && wl_popup_button(0x110 /* BTN_LEFT */, down)) {   /* a Wayland app's menu */
        if (!down) { drag_win = -1; content_drag = -1; rz_win = -1; }
        mouse_down = down; return;
    }
    if (down && !mouse_down) {
        int ti = toast_at(mx, my);                       /* a click closes a notification */
        if (ti >= 0) { toast_clicked(ti); memmove(toasts + ti, toasts + ti + 1, sizeof toasts[0] * (size_t)(ntoast - ti - 1)); ntoast--; repaint_toasts(); mouse_down = 1; return; }
        press();
    }
    if (!down) {
        drag_win = -1; content_drag = -1; rz_win = -1;
        di_drop();
        for (int i = 0; i < MAXWIN; i++)
            if (wins[i].open && wins[i].release) wins[i].release();
    }
    mouse_down = down;
}

static struct {
    int touching, fingers, maxfingers;
    int x, y, lx, ly, have_last;        /* current and previous finger position (pad units) */
    int sx, sy, moved_far;               /* where the touch started */
    long long t0;                        /* when it started (ms) */
    double fx, fy, scroll;               /* leftover fractions */
    int pressed_right;                   /* the physical click became a right click */
} tp;

/* returns 1 when the pointer moved */
static int touchpad_event(int d, const struct input_event *v) {
    if (v->type == EV_ABS) {
        if (v->code == ABS_X) tp.x = v->value;
        if (v->code == ABS_Y) tp.y = v->value;
        return 0;
    }
    if (v->type == EV_KEY) {
        int f = 0;
        switch (v->code) {
        case BTN_TOUCH:
            if (v->value) {
                tp.touching = 1; tp.have_last = 0; tp.maxfingers = 1;
                tp.t0 = now_ms(); tp.moved_far = 0; tp.sx = tp.x; tp.sy = tp.y; tp.scroll = 0;
            } else {
                tp.touching = 0; tp.have_last = 0;
                /* a short, still touch is a tap */
                if (!tp.moved_far && !mouse_down && now_ms() - tp.t0 < 220) {
                    if (tp.maxfingers >= 2) { rpress(); wl_raw_button(BTN_RIGHT, 0); }
                    else { left_button(1); left_button(0); }
                }
            }
            return 0;
        case BTN_TOOL_FINGER:    f = 1; break;
        case BTN_TOOL_DOUBLETAP: f = 2; break;
        case BTN_TOOL_TRIPLETAP: f = 3; break;
        case BTN_TOOL_QUADTAP:   f = 4; break;
        case BTN_LEFT: case BTN_RIGHT: {
            mx = clampi(mx, 0, W - 1); my = clampi(my, 0, H - 1);
            if (v->value) {
                int corner = tp.x > dev[d].xmin + (dev[d].xmax - dev[d].xmin) / 2 &&
                             tp.y > dev[d].ymin + (dev[d].ymax - dev[d].ymin) * 4 / 5;
                tp.pressed_right = v->code == BTN_RIGHT || tp.fingers >= 2 || corner;
                if (tp.pressed_right) rpress(); else left_button(1);
                tp.moved_far = 1;                      /* not also a tap */
            } else if (!tp.pressed_right) left_button(0);
            else wl_raw_button(BTN_RIGHT, 0);
            return 0;
        }
        default: return 0;
        }
        if (v->value) {
            tp.fingers = f; tp.have_last = 0;          /* finger count changed: don't jump */
            if (f > tp.maxfingers) tp.maxfingers = f;
        }
        return 0;
    }
    if (v->type != EV_SYN || v->code != SYN_REPORT || !tp.touching) return 0;
    if (!tp.have_last) { tp.lx = tp.x; tp.ly = tp.y; tp.have_last = 1; tp.fx = tp.fy = 0; return 0; }
    double dxmm = (double)(tp.x - tp.lx) / dev[d].xres, dymm = (double)(tp.y - tp.ly) / dev[d].yres;
    tp.lx = tp.x; tp.ly = tp.y;
    if (abs(tp.x - tp.sx) > 2 * dev[d].xres || abs(tp.y - tp.sy) > 2 * dev[d].yres) tp.moved_far = 1;
    if (tp.fingers >= 2) {                             /* two fingers: scroll, the page follows */
        tp.scroll += dymm;
        while (tp.scroll >= 4)  { wheel(1);  tp.scroll -= 4; }
        while (tp.scroll <= -4) { wheel(-1); tp.scroll += 4; }
        return 0;
    }
    /* one finger: about 9 screen pixels per mm, faster when moving quickly */
    double speed = sqrt(dxmm * dxmm + dymm * dymm);
    static const double tp_speeds[3] = { 0.65, 1.0, 1.5 };            /* Settings > Mouse */
    double gain = (W / 1280.0) * 9.0 * tp_speeds[set_tp_speed] * (speed > 1.5 ? fmin(2.2, 1 + (speed - 1.5) * 0.5) : 1);
    tp.fx += dxmm * gain; tp.fy += dymm * gain;
    int ix = (int)tp.fx, iy = (int)tp.fy;
    tp.fx -= ix; tp.fy -= iy;
    if (!ix && !iy) return 0;
    mx += ix; my += iy;
    return 1;
}

#include "record.c"               /* Record: the screen into a video */
#include "camera.c"               /* Camera: the webcam */
#include "menusearch.c"           /* type in the Start menu */
#include "screenshot.c"           /* Print Screen and Win+Shift+S */
#include "stickwait.c"            /* waiting for the USB boot stick before loading settings */
int main(void) {
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { perror("desktop: /dev/fb0"); return 1; }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &vi) || ioctl(fb, FBIOGET_FSCREENINFO, &fi)) {
        perror("desktop: fb info"); return 1;
    }
    if (vi.bits_per_pixel != 32) {
        fprintf(stderr, "desktop: need 32-bit color, got %d\n", vi.bits_per_pixel); return 1;
    }
    W = vi.xres; H = vi.yres; stride = fi.line_length / 4;
    fb_fd = fb;
    swap_rb = vi.red.offset == 0;
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbmem == MAP_FAILED) { perror("desktop: mmap"); return 1; }
    back = malloc((size_t)W * H * 4);
    wall = malloc((size_t)W * H * 4);
    glowmap = malloc((size_t)W * H);
    np_lines = malloc(sizeof(int) * (NP_MAX + 2));
    if (pipe(wake_pipe) == 0) {                       /* background threads wake us up through this */
        fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK); fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC); fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);
    }
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0) ioctl(tty, KDSETMODE, KD_GRAPHICS);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    {   /* finished child programs are cleaned up by the kernel; but programs we start must
           see the normal setting (an "ignored" SIGCHLD is passed on and breaks waiting for
           helpers - e.g. Xwayland's keyboard compiler) */
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL; sa.sa_flags = SA_NOCLDWAIT;
        sigaction(SIGCHLD, &sa, NULL);
    }

    font_init();
    scan_devices();                               /* the mouse, for the stick screen */
    stick_wait_screen();                          /* settings are on the stick: wait for it */
    check_persistent();
    fp_scan();                                    /* Flatpak apps (pins may point at them) */
    load_settings();
    if (!set_bin_icon) { desk_add(BIN_CMD, "Recycle Bin"); set_bin_icon = 1; }
    desk_bin_changed();
    tz_init();
    power_init();
    toast_fifo_open();
    if (!store_icon_done) { store_icon_done = 1; if (desk_add("@store", "Software Manager")) save_settings(); }
    fp_scan();                                    /* new Flathub apps -> desktop shortcuts */
    volume_apply();
    ex_make_special();                            /* Documents, Images, Downloads, Videos */
    net_poll();
    wifi_poll(0);
    net_fast = 20;                                     /* the network is still starting up */
    if (npins < 0) {                                   /* first run: pin File Explorer and Terminal */
        npins = 0;
        snprintf(pins[npins++], sizeof pins[0], "@explorer");
        snprintf(pins[npins++], sizeof pins[0], "@terminal");
        snprintf(pins[npins++], sizeof pins[0], "@store");
    }
    make_glowmap();
    make_wallpaper();
    mx = W / 2; my = H / 2;
    scan_devices();
    int last_min;
    { time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); last_min = tm.tm_min; }
    repaint_all();

    /* startup: play the chime and fade from the loading screen to the desktop */
    if (set_sound && fork() == 0) {
        execl("/bin/playsound", "playsound", "/usr/share/sounds/startup.wav", (char *)NULL);
        _exit(0);
    }
    {
        uint32_t *from = malloc((size_t)W * H * 4);
        if (from) {
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) from[y * W + x] = to_fb(fbmem[(size_t)y * stride + x]);
            long long t0 = now_ms();
            for (;;) {
                long long el = now_ms() - t0;
                int a = (int)clampi((int)(el * 256 / 1500), 0, 256);    /* 1.5 seconds */
                for (int y = 0; y < H; y++) {
                    uint32_t *d = fbmem + (size_t)y * stride, *o = from + (size_t)y * W, *nw = back + (size_t)y * W;
                    for (int x = 0; x < W; x++) {
                        uint32_t A = o[x], Bc = nw[x];
                        uint32_t rb = (((A & 0xff00ff) * (256 - a) + (Bc & 0xff00ff) * a) >> 8) & 0xff00ff;
                        uint32_t g  = (((A & 0x00ff00) * (256 - a) + (Bc & 0x00ff00) * a) >> 8) & 0x00ff00;
                        d[x] = to_fb(rb | g);
                    }
                }
                if (a >= 256) break;
                usleep(16000);
            }
            free(from);
        }
    }
    present();
    int wl_fd = wl_start();                                /* Wayland apps can connect from now on */
    wl_resize_hook = wl_resize_from_app;

    for (;;) {
        wl_update_focus();                                 /* the active window gets the keyboard */
        wl_send_pending(rz_win >= 0);                      /* new window sizes for Wayland apps */
        wl_flush();                                        /* send Wayland apps what happened */
        struct pollfd pfd[MAXDEV + 6 + MAXWIN];
        int map[MAXDEV + 6 + MAXWIN], n = 0;
        for (int i = 0; i < MAXDEV; i++)
            if (dev[i].fd > 0) { pfd[n].fd = dev[i].fd; pfd[n].events = POLLIN; map[n++] = i; }
        int nin = n;                                       /* input devices come first */
        if (wake_pipe[0] >= 0) { pfd[n].fd = wake_pipe[0]; pfd[n].events = POLLIN; map[n++] = -1; }
        if (wl_fd >= 0) { pfd[n].fd = wl_fd; pfd[n].events = POLLIN; map[n++] = -2; }
        { int xf = xw_poll_fd(); if (xf >= 0) { pfd[n].fd = xf; pfd[n].events = POLLIN; map[n++] = -3; } }
        if (toast_fd >= 0) { pfd[n].fd = toast_fd; pfd[n].events = POLLIN; map[n++] = -4; }
        int napp = n;                                      /* then connections apps asked us to watch */
        for (int i = 0; i < MAXWIN && n < MAXDEV + 3 + MAXWIN; i++)
            if (wins[i].open && wins[i].fd) { int f = wins[i].fd(); if (f >= 0) { pfd[n].fd = f; pfd[n].events = POLLIN; map[n++] = -10 - i; } }

        struct timeval tv;
        gettimeofday(&tv, NULL);
        int wait_ms = 1000 - tv.tv_usec / 1000;
        { int o = osd_wait(); if (o >= 0 && o < wait_ms) wait_ms = o + 1; }
        { int o = toast_wait(); if (o >= 0 && o < wait_ms) wait_ms = o + 1; }
        { int o = rec_wait(); if (o >= 0 && o < wait_ms) wait_ms = o; }

        int polled = poll(pfd, n, wait_ms);
        rec_tick();                                        /* Record: a picture of the screen */
        osd_wait();                                        /* hide the popup when its time is up */
        toast_wait();
        if (polled <= 0) {
            scan_devices();
            time_t now = time(NULL);
            struct tm tm; localtime_r(&now, &tm);
            if (tm.tm_min != last_min) { last_min = tm.tm_min; repaint_clock(); if (clk_panel) clkp_repaint(); }
        }

        int woke = 0;
        for (int k = napp; k < n; k++)
            if (pfd[k].revents & (POLLIN | POLLHUP | POLLERR)) { int i = -10 - map[k]; if (wins[i].open && wins[i].wake) wins[i].wake(); }
        for (int k = nin; k < napp; k++) {
            if (map[k] == -1 && (pfd[k].revents & POLLIN)) {
                char junk[64];
                while (read(wake_pipe[0], junk, sizeof junk) > 0) {}
                woke = 1;
            }
            if (map[k] == -2 && pfd[k].revents) wl_dispatch();   /* Wayland apps sent something */
            if (map[k] == -3 && (pfd[k].revents & POLLIN)) xw_start();   /* an X11 app wants a screen */
            if (map[k] == -4 && (pfd[k].revents & POLLIN)) toast_read();  /* a notification from an app */
        }
        if (woke)
            for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].wake) wins[i].wake();
        n = nin;

        int moved = 0, omx = mx, omy = my;
        for (int k = 0; k < n; k++) {
            if (!(pfd[k].revents & POLLIN)) continue;
            int d = map[k];
            struct input_event ev[64];
            ssize_t got;
            while ((got = read(pfd[k].fd, ev, sizeof ev)) > 0) {
                for (int e = 0; e < got / (ssize_t)sizeof *ev; e++) {
                    struct input_event *v = &ev[e];
                    if (v->type == EV_SW) { if (v->code == SW_LID) power_lid(v->value); continue; }
                    if (v->type == EV_KEY || v->type == EV_REL || v->type == EV_ABS) {
                        if (idle_state == 2) { power_activity(); if (v->type == EV_KEY) continue; }   /* waking the screen: that key does nothing else */
                        idle_last = ui_ms();
                        if (idle_state) power_wake_screen();
                    }
                    if (dev[d].tp) { if (touchpad_event(d, v)) moved = 1; continue; }
                    if (v->type == EV_REL) {
                        if (v->code == REL_X) { mx += v->value; moved = 1; }
                        if (v->code == REL_Y) { my += v->value; moved = 1; }
                        if (v->code == REL_WHEEL) wheel(v->value);
                    } else if (v->type == EV_ABS && dev[d].abs) {
                        if (v->code == ABS_X) {
                            mx = (long)(v->value - dev[d].xmin) * (W - 1) / (dev[d].xmax - dev[d].xmin);
                            moved = 1;
                        }
                        if (v->code == ABS_Y) {
                            my = (long)(v->value - dev[d].ymin) * (H - 1) / (dev[d].ymax - dev[d].ymin);
                            moved = 1;
                        }
                    } else if (v->type == EV_KEY) {
                        int down = v->value != 0;
                        if (v->code == KEY_CAPSLOCK) {         /* ccos keeps Caps Lock itself */
                            if (v->value == 1) {
                                capslock = capslock_state = !capslock_state;
                                caps_leds(); wl_set_capslock(capslock); osd_show(OSD_CAPS);
                            }
                            continue;
                        }
                        if (v->code == KEY_BRIGHTNESSUP || v->code == KEY_BRIGHTNESSDOWN) {
                            if (v->value) bright_set((set_bright > 0 ? set_bright : 100) + (v->code == KEY_BRIGHTNESSUP ? 10 : -10), 1);
                            continue;
                        }
                        if (v->code == KEY_SLEEP) { if (v->value == 1) power_sleep(); continue; }
                        if (v->code == KEY_VOLUMEUP || v->code == KEY_VOLUMEDOWN || v->code == KEY_MUTE) {
                            if (v->value) {                    /* pressed or held */
                                if (v->code == KEY_MUTE) { if (v->value == 1) volume_set(set_volume, !set_muted, 1); }
                                else volume_set(set_volume + (v->code == KEY_VOLUMEUP ? 2 : -2), 0, 1);
                            }
                            continue;
                        }
                        mx = clampi(mx, 0, W - 1); my = clampi(my, 0, H - 1);
                        /* ccos's own keys, whatever app is active: Windows key, Print Screen, Win+Shift+S */
                        if (v->code == KEY_LEFTMETA || v->code == KEY_RIGHTMETA) {
                            if (v->value == 1) { meta_down = 1; meta_used = 0; }
                            else if (v->value == 0) { meta_down = 0; if (!meta_used && !snip_on) set_menu(!menu_open); }
                            continue;
                        }
                        if (v->code == KEY_SYSRQ) { if (v->value == 1 && !snip_on) shot_full(); continue; }
                        {
                            int mod = v->code == KEY_LEFTSHIFT || v->code == KEY_RIGHTSHIFT || v->code == KEY_LEFTCTRL ||
                                      v->code == KEY_RIGHTCTRL || v->code == KEY_LEFTALT || v->code == KEY_RIGHTALT;
                            if (mod && (meta_down || snip_on)) {
                                if (v->code == KEY_LEFTSHIFT || v->code == KEY_RIGHTSHIFT) shift = down;
                                if (v->code == KEY_LEFTCTRL || v->code == KEY_RIGHTCTRL) ctrl = down;
                                if (v->code == KEY_LEFTALT) alt = down;
                                continue;
                            }
                            if (snip_on && v->code < BTN_MISC) { if (v->value == 1 && v->code == KEY_ESC) snip_end(0); continue; }
                            if (meta_down && v->code < BTN_MISC) {
                                meta_used = 1;
                                if (v->value == 1 && v->code == KEY_S && shift) snip_start();
                                if (v->value == 1 && v->code == KEY_R && alt) rec_toggle();
                                continue;
                            }
                        }
                        {   /* a Wayland app is active: it gets the keyboard as it is */
                            int a = active_win();
                            int is_key = v->code < BTN_MISC || v->code >= KEY_OK;
                            int ours = v->code == KEY_LEFTMETA || v->code == KEY_RIGHTMETA || v->code == KEY_POWER ||
                                       (v->code == KEY_BACKSPACE && ctrl && alt);
                            if (v->code == KEY_F11 && a >= 0 && wins[a].rawkey && wl_is_xwayland(a) && !menu_open) {
                                if (v->value == 1) set_fullscreen(a, !wins[a].fullscreen);   /* X11 apps: ccos does F11 */
                                continue;
                            }
                            if (is_key && !ours && a >= 0 && wins[a].rawkey && !menu_open && !net_panel && !pop.open) {
                                if (v->code == KEY_LEFTSHIFT || v->code == KEY_RIGHTSHIFT) shift = down;
                                if (v->code == KEY_LEFTCTRL || v->code == KEY_RIGHTCTRL) ctrl = down;
                                if (v->code == KEY_LEFTALT || (v->code == KEY_RIGHTALT && set_kbd != KBD_PL)) alt = down;
                                if (v->code == KEY_RIGHTALT && set_kbd == KBD_PL) altgr = down;
                                wins[a].rawkey(v->code, v->value);
                                continue;
                            }
                        }
                        switch (v->code) {
                        case BTN_LEFT:
                            left_button(down);
                            break;
                        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift = down; break;
                        case BTN_RIGHT:
                            if (v->value == 1) rpress();
                            else if (v->value == 0) wl_raw_button(BTN_RIGHT, 0);
                            break;
                        case BTN_MIDDLE: case BTN_SIDE: case BTN_EXTRA:      /* for apps and games */
                            if (v->value != 2) wl_raw_button(v->code, down);
                            break;
                        case KEY_LEFTCTRL: case KEY_RIGHTCTRL: ctrl = down; break;
                        case KEY_LEFTALT:  alt = down; break;
                        case KEY_RIGHTALT: if (set_kbd == KBD_PL) altgr = down; else alt = down; break;
                        case KEY_LEFTMETA: case KEY_RIGHTMETA:
                            if (v->value == 1) set_menu(!menu_open);
                            break;
                        case KEY_POWER:
                            if (v->value == 1) shutdown_now();
                            break;
                        case KEY_BACKSPACE:
                            if (down && ctrl && alt) { restore_console(); return 0; }
                            /* fall through: otherwise it goes to the active app */
                        default:
                            if (!down) break;
                            if (net_panel) { net_key(v->code, (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0)); break; }
                            if (clk_panel && v->code == KEY_ESC) { clk_panel_set(0); break; }
                            if (v->code == KEY_ESC && pop.open) { pop_close(); break; }
                            if (menu_open) { menu_search_key(v->code, (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (altgr ? MOD_ALTGR : 0)); break; }
                            if (!menu_open) {                 /* send keys to the active app */
                                int a = active_win();
                                if (v->code == KEY_F11 && a >= 0 && !wins[a].no_max) { set_fullscreen(a, !wins[a].fullscreen); break; }
                                if (a >= 0 && wins[a].key)
                                    wins[a].key(v->code, (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0) | (altgr ? MOD_ALTGR : 0));
                            }
                            break;
                        }
                    }
                }
            }
            if (got == 0 || (pfd[k].revents & (POLLERR | POLLHUP))) {
                close(dev[d].fd); dev[d].fd = 0;
            }
        }
        if (moved) {                                       /* games: how far the mouse moved; a locked pointer stays */
            wl_rel_motion(mx - omx, my - omy);
            if (wl_pointer_locked()) { mx = omx; my = omy; moved = 0; }
            int cx0, cy0, cx1, cy1;
            if (wl_pointer_confine(&cx0, &cy0, &cx1, &cy1)) { mx = clampi(mx, cx0, cx1); my = clampi(my, cy0, cy1); }
        }
        mx = clampi(mx, 0, W - 1);
        my = clampi(my, 0, H - 1);
        if (moved && snip_on) snip_motion();
        else if (moved) {
            if (drag_win >= 0) move_win(drag_win, mx - drag_dx, my - drag_dy);
            if (rz_win >= 0 && mouse_down) resize_follow();
            di_follow_mouse();
            wl_popup_motion();                              /* over a Wayland app's menu? */
            {                                               /* tell apps that track the mouse */
                int top = win_under_mouse();
                for (int i = 0; i < MAXWIN; i++) {
                    if (!wins[i].open || !wins[i].motion || wins[i].minimized) continue;
                    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
                    wins[i].motion(mx - cx, my - cy, top == i && !menu_open && !pop.open && in_rect(cx, cy, cw, ch));
                }
            }
            if (content_drag >= 0 && mouse_down && wins[content_drag].open && wins[content_drag].drag) {
                int cx, cy, cw, ch; client_rect(&wins[content_drag], &cx, &cy, &cw, &ch);
                wins[content_drag].drag(mx - cx, my - cy, cw, ch);
            }
            update_hover();
        }
        static time_t last_tick;                          /* once-a-second app timers */
        time_t nowt = time(NULL);
        if (nowt != last_tick) {
            last_tick = nowt;
            for (int i = 0; i < MAXWIN; i++) if (wins[i].open && wins[i].tick) wins[i].tick();
            {   /* the USB stick's save file showed up after the desktop started */
                static int was = -1;
                check_persistent();
                if (was == 0 && conf_persistent) {
                    if (access(CONF_PATH, F_OK) == 0) {   /* saved settings: use them */
                        load_settings(); make_wallpaper(); repaint_all();
                    } else save_settings();                /* new save file: keep what you set */
                }
                was = conf_persistent;
            }
            {   /* apps installed or removed (Terminal, the store): refresh menu and desktop */
                static struct timespec fp_mt; struct stat st;
                if (stat(FP_DIR, &st) == 0 && (st.st_mtim.tv_sec != fp_mt.tv_sec || st.st_mtim.tv_nsec != fp_mt.tv_nsec)) {
                    int first = fp_mt.tv_sec == 0 && fp_mt.tv_nsec == 0;
                    fp_mt = st.st_mtim;
                    if (!first) { fp_scan(); if (menu_open) repaint_menu(); repaint_taskbar(); }
                }
            }
            power_tick();
            tz_tick();
            print_job_poll();
            stick_late_check();
            update_poll();
            fileop_poll();
            static int drv_tick;
            if (++drv_tick % 2 == 0) drives_poll();            /* USB drives plugged in or pulled out */
            static int net_tick;
            if (net_fast > 0 || ++net_tick % 2 == 0) {
                if (net_fast > 0) net_fast--;
                int changed = net_poll();
                changed |= wifi_poll(net_panel);
                if (changed) {
                    tray_repaint();
                    if (net_panel) { netp_place(); netp_repaint(); update_hover(); }
                }
            }
        }
        present();
    }
}
