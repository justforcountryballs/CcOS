#include <zlib.h>
/* ===================================================================
 *  Emoji picker: Windows + . (or Windows + ;)
 *  A panel by the mouse pointer: search by name, or pick a group.
 *  Clicking one types it into the app you were in (Firefox, Discord...).
 *  In ccos's own windows it goes on the clipboard instead.
 *  The pictures come from /usr/share/ccos/emoji.gz (Noto Color Emoji,
 *  32x32, made by emojigen/make_emoji.py).
 * =================================================================== */
#define EM_W 420
#define EM_H 452
#define EM_COLS 9
#define EM_CELL 44
#define EM_GRID_Y 98
#define EM_NG 10                        /* Recent + 9 groups */
#define EM_RECENT_FILE "/data/.emoji-recent"
static struct {
    int open, loaded, n, sz, x, y, group, scroll, sel, target;
    char q[48];
    unsigned char *px;                  /* n pictures, sz*sz RGBA each */
    struct { unsigned char g; char u[40]; char name[124]; } *e;
    int recent[32], nrecent;
    int vis[2000], nvis;                /* what the grid shows now */
} em;
static const char *em_group_names[EM_NG] = { "Recently used", "Smileys & emotion", "People & body", "Animals & nature", "Food & drink",
                                             "Travel & places", "Activities", "Objects", "Symbols", "Flags" };

static void em_load(void) {
    if (em.loaded) return;
    em.loaded = 1;
    gzFile f = gzopen("/usr/share/ccos/emoji.gz", "rb");
    if (!f) return;
    char hdr[16];
    if (gzread(f, hdr, 16) != 16 || memcmp(hdr, "CCEMOJI1", 8)) { gzclose(f); return; }
    uint32_t n, sz; memcpy(&n, hdr + 8, 4); memcpy(&sz, hdr + 12, 4);
    if (n > 2000 || sz != 32) { gzclose(f); return; }
    em.e = calloc(n, sizeof *em.e);
    em.px = malloc((size_t)n * sz * sz * 4);
    if (!em.e || !em.px) { gzclose(f); return; }
    for (uint32_t i = 0; i < n; i++) {
        unsigned char m[3];
        if (gzread(f, m, 3) != 3) { gzclose(f); return; }
        em.e[i].g = m[0];
        char tmp[256];
        if (gzread(f, tmp, m[1]) != m[1]) { gzclose(f); return; }
        memcpy(em.e[i].u, tmp, imin(m[1], 39)); em.e[i].u[imin(m[1], 39)] = 0;
        if (gzread(f, tmp, m[2]) != m[2]) { gzclose(f); return; }
        memcpy(em.e[i].name, tmp, imin(m[2], 123)); em.e[i].name[imin(m[2], 123)] = 0;
    }
    if (gzread(f, em.px, n * sz * sz * 4) != (int)(n * sz * sz * 4)) { gzclose(f); return; }
    gzclose(f);
    em.n = (int)n; em.sz = (int)sz;
    FILE *r = fopen(EM_RECENT_FILE, "r");          /* the ones used lately */
    if (r) {
        char l[64];
        while (em.nrecent < 32 && fgets(l, sizeof l, r)) {
            l[strcspn(l, "\n")] = 0;
            for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].u, l)) { em.recent[em.nrecent++] = i; break; }
        }
        fclose(r);
    }
}
static void em_used(int i) {                       /* to the front of Recently used */
    int k = 0;
    while (k < em.nrecent && em.recent[k] != i) k++;
    if (k == em.nrecent) k = imin(em.nrecent, 31); else em.nrecent--;
    memmove(em.recent + 1, em.recent, sizeof em.recent[0] * (size_t)k);
    em.recent[0] = i; em.nrecent = imin(em.nrecent + 1, 32);
    char *mb = NULL; size_t ml = 0; FILE *f = open_memstream(&mb, &ml);
    if (!f) return;
    for (int j = 0; j < em.nrecent; j++) fprintf(f, "%s\n", em.e[em.recent[j]].u);
    fclose(f);
    if (mb && conf_persistent) aw_save(EM_RECENT_FILE, mb, ml, 1, "recent emoji");
    free(mb);
}
static void em_fill(void) {                        /* the grid: a group, or what matches the search */
    em.nvis = 0;
    if (em.q[0]) {
        for (int i = 0; i < em.n && em.nvis < 2000; i++) if (strcasestr(em.e[i].name, em.q)) em.vis[em.nvis++] = i;
    } else if (em.group == 0) {
        for (int k = 0; k < em.nrecent; k++) em.vis[em.nvis++] = em.recent[k];
    } else {
        for (int i = 0; i < em.n && em.nvis < 2000; i++) if (em.e[i].g == em.group - 1) em.vis[em.nvis++] = i;
    }
    em.sel = em.nvis ? 0 : -1;
}
static int em_rows_shown(void) { return (EM_H - EM_GRID_Y - 34) / EM_CELL; }
static void em_repaint(void) { repaint(em.x - 6, em.y - 6, EM_W + 16, EM_H + 16); }

/* a picture, scaled to s x s, at x,y */
static void em_draw(int i, int x, int y, int s) {
    if (i < 0 || i >= em.n || !em.px) return;
    const unsigned char *src = em.px + (size_t)i * em.sz * em.sz * 4;
    for (int yy = imax(y, clip_y0); yy < imin(y + s, clip_y1); yy++)
        for (int xx = imax(x, clip_x0); xx < imin(x + s, clip_x1); xx++) {
            /* box filter from the 32x32 picture */
            float fx0 = (float)(xx - x) * em.sz / s, fy0 = (float)(yy - y) * em.sz / s, st = (float)em.sz / s;
            int x0 = (int)fx0, y0 = (int)fy0, x1 = imin(em.sz - 1, (int)(fx0 + st - 0.001f)), y1 = imin(em.sz - 1, (int)(fy0 + st - 0.001f));
            int r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = y0; sy <= y1; sy++) for (int sx = x0; sx <= x1; sx++) {
                const unsigned char *p = src + ((size_t)sy * em.sz + sx) * 4;
                r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; cnt++;
            }
            if (!a) continue;
            uint32_t c = RGB(r / a, g / a, b / a);
            uint32_t *d = &back[(size_t)yy * BW + xx];
            int al = a / cnt;
            *d = al >= 255 ? c : mix(*d, c, al / 255.0f);
        }
}
static void draw_emoji_picker(void) {
    if (!em.open) return;
    int x = em.x, y = em.y, w = EM_W, h = EM_H;
    if (x >= clip_x1 || x + w + 4 <= clip_x0 || y >= clip_y1 || y + h + 4 <= clip_y0) return;
    const Theme *t = TH();
    fill_round(x + 2, y + 5, w, h, 12, RGB(0, 0, 0), 70);
    glass_fill(x, y, w, h, 12, t->menu, 0, 0, 0, 0);
    stroke_round(x, y, w, h, 12, t->menu_line, 255);
    /* search */
    fill_round(x + 12, y + 12, w - 24, 34, 17, DARK ? RGB(0x2a, 0x2c, 0x32) : RGB(255, 255, 255), 255);
    stroke_round(x + 12, y + 12, w - 24, 34, 17, t->accent, 255);
    stroke_round(x + 24, y + 21, 12, 12, 6, t->sub, 255);
    fill_rect(x + 34, y + 31, 2, 6, t->sub, 255); fill_rect(x + 35, y + 34, 4, 2, t->sub, 255);
    if (em.q[0]) draw_text(x + 46, y + 20, em.q, t->text, 255);
    else draw_text(x + 46, y + 20, "Search emoji", t->sub, 255);
    fill_rect(x + 46 + (em.q[0] ? text_w(em.q) + 1 : 0), y + 20, 1, 18, t->text, 255);
    /* groups */
    static const int rep[EM_NG] = { -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    (void)rep;
    for (int g = 0; g < EM_NG; g++) {
        int gx = x + 12 + g * 39, gy = y + 54, on = !em.q[0] && em.group == g, hv = hot.type == HOT_EMO && hot.a == 5000 + g;
        if (on || hv) fill_round(gx, gy, 36, 34, 8, on ? t->hover : t->hover, on ? 255 : 160);
        if (on) fill_round(gx + 10, gy + 31, 16, 3, 1, t->accent, 255);
        if (g == 0) {                                /* a little clock: Recent */
            stroke_round(gx + 9, gy + 8, 18, 18, 9, t->text, 255);
            aa_line(gx + 18, gy + 11, gx + 18, gy + 17, 1.5f, t->text); aa_line(gx + 18, gy + 17, gx + 22, gy + 20, 1.5f, t->text);
        } else {
            int first = -1;
            for (int i = 0; i < em.n; i++) if (em.e[i].g == g - 1) { first = i; break; }
            if (g == 3) for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].name, "dog face")) { first = i; break; }
            if (g == 4) for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].name, "hamburger")) { first = i; break; }
            if (g == 6) for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].name, "soccer ball")) { first = i; break; }
            if (g == 7) for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].name, "light bulb")) { first = i; break; }
            if (g == 8) for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].name, "red heart")) { first = i; break; }
            if (g == 9) for (int i = 0; i < em.n; i++) if (!strcmp(em.e[i].name, "flag: Poland")) { first = i; break; }
            em_draw(first, gx + 7, gy + 6, 22);
        }
    }
    fill_rect(x + 12, y + 92, w - 24, 1, t->menu_line, 255);
    /* the grid */
    int gy0 = y + EM_GRID_Y, rows = em_rows_shown();
    int ox0 = clip_y0, ox1 = clip_y1;
    clip_y0 = imax(clip_y0, gy0); clip_y1 = imin(clip_y1, gy0 + rows * EM_CELL);
    for (int k = em.scroll * EM_COLS; k < em.nvis && k < (em.scroll + rows) * EM_COLS; k++) {
        int r = k / EM_COLS - em.scroll, c = k % EM_COLS;
        int cx = x + 12 + c * EM_CELL, cy = gy0 + r * EM_CELL;
        int hv = hot.type == HOT_EMO && hot.a == k;
        if (hv || k == em.sel) fill_round(cx, cy, EM_CELL - 2, EM_CELL - 2, 8, t->hover, hv ? 255 : 150);
        if (k == em.sel && !hv) stroke_round(cx, cy, EM_CELL - 2, EM_CELL - 2, 8, t->accent, 200);
        em_draw(em.vis[k], cx + 5, cy + 5, 32);
    }
    clip_y0 = ox0; clip_y1 = ox1;
    if (!em.nvis) {
        const char *m = !em.n ? "The emoji pictures are missing." : em.q[0] ? "No emoji with that name." : "Emoji you use show up here.";
        draw_text(x + (w - text_w(m)) / 2, gy0 + 60, m, t->sub, 255);
    }
    /* the name of the one under the mouse (or picked with the keys) */
    int show = hot.type == HOT_EMO && hot.a >= 0 && hot.a < em.nvis ? hot.a : em.sel;
    fill_rect(x + 12, y + h - 34, w - 24, 1, t->menu_line, 255);
    if (show >= 0 && show < em.nvis) {
        char b[124]; snprintf(b, sizeof b, "%s", em.e[em.vis[show]].name);
        if (b[0] >= 'a' && b[0] <= 'z') b[0] -= 32;
        while (b[0] && text_w(b) > w - 40) b[u8_prev(b, (int)strlen(b))] = 0;
        draw_text(x + 16, y + h - 26, b, t->text, 255);
    } else draw_text(x + 16, y + h - 26, em.q[0] ? "Search results" : em_group_names[em.group], t->sub, 255);
}
/* 0..nvis-1 an emoji, 5000+g a group, -2 the panel, -1 outside */
static int em_hit(void) {
    if (!em.open || !in_rect(em.x, em.y, EM_W, EM_H)) return -1;
    int lx = mx - em.x, ly = my - em.y;
    if (ly >= 54 && ly < 88 && lx >= 12) { int g = (lx - 12) / 39; if (g < EM_NG && (lx - 12) % 39 < 36) return 5000 + g; }
    if (ly >= EM_GRID_Y && ly < EM_GRID_Y + em_rows_shown() * EM_CELL && lx >= 12 && lx < 12 + EM_COLS * EM_CELL) {
        int k = ((ly - EM_GRID_Y) / EM_CELL + em.scroll) * EM_COLS + (lx - 12) / EM_CELL;
        if (k < em.nvis) return k;
    }
    return -2;
}
static void em_set(int open) {
    if (open == em.open) return;
    if (open) {
        em_load();
        em.target = active_win();
        em.q[0] = 0; em.scroll = 0;
        em.group = em.nrecent ? 0 : 1;
        em_fill();
        em.x = clampi(mx - EM_W / 2, 8, W - EM_W - 8);
        em.y = my + 24 + EM_H < H - TASKBAR_H ? my + 24 : imax(8, my - EM_H - 24);
        em.open = 1;
    } else { em.open = 0; }
    em_repaint();
}
static int em_open_(void) { return em.open; }
static void em_insert(int k) {
    if (k < 0 || k >= em.nvis) return;
    int i = em.vis[k];
    em_used(i);
    int a = em.target;
    if (a >= 0 && a < MAXWIN && wins[a].open && wins[a].rawkey && wl_type_text(em.e[i].u)) return;
    wl_clip_set_text(em.e[i].u, strlen(em.e[i].u));   /* a ccos window: on the clipboard */
    char b[160]; snprintf(b, sizeof b, "Copied \"%s\". Paste it with Ctrl+V.", em.e[i].name);
    toast(ICON_APP, "Emoji copied", b);
}
static void em_click(int id) {
    if (id >= 5000) { em.group = id - 5000; em.q[0] = 0; em.scroll = 0; em_fill(); em_repaint(); return; }
    if (id >= 0) { em.sel = id; em_insert(id); em_repaint(); }
}
static void em_wheel(int amount) {
    int rows = (em.nvis + EM_COLS - 1) / EM_COLS;
    int ns = clampi(em.scroll + amount, 0, imax(0, rows - em_rows_shown()));
    if (ns != em.scroll) { em.scroll = ns; em_repaint(); update_hover(); }
}
static void em_show_sel(void) {
    if (em.sel < 0) return;
    int r = em.sel / EM_COLS;
    if (r < em.scroll) em.scroll = r;
    if (r >= em.scroll + em_rows_shown()) em.scroll = r - em_rows_shown() + 1;
}
static void em_key(int code, int mods) {
    if (code == KEY_ESC) { if (em.q[0]) { em.q[0] = 0; em_fill(); em_repaint(); } else em_set(0); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER) { em_insert(em.sel); em_repaint(); return; }
    if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN) {
        if (em.nvis) {
            int d = code == KEY_LEFT ? -1 : code == KEY_RIGHT ? 1 : code == KEY_UP ? -EM_COLS : EM_COLS;
            em.sel = clampi(em.sel + d, 0, em.nvis - 1); em_show_sel(); em_repaint();
        }
        return;
    }
    if (code == KEY_TAB) {                              /* the next group */
        em.q[0] = 0; em.group = (em.group + ((mods & MOD_SHIFT) ? EM_NG - 1 : 1)) % EM_NG; em.scroll = 0; em_fill(); em_repaint(); return;
    }
    if (code == KEY_BACKSPACE) {
        int l = (int)strlen(em.q);
        if (l) { em.q[u8_prev(em.q, l)] = 0; em.scroll = 0; em_fill(); em_repaint(); }
        return;
    }
    char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
    if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
    size_t l = strlen(em.q);
    if (l + (size_t)k < sizeof em.q) { memcpy(em.q + l, t, (size_t)k); em.q[l + (size_t)k] = 0; }
    em.scroll = 0; em_fill(); em_repaint();
}
