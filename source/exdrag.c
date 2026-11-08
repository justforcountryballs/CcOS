/* ===================================================================
 *  File Explorer: dragging with the mouse
 *
 *  - press on empty space and drag: a blue box selects every row it
 *    touches (Ctrl keeps what was already selected)
 *  - press on a file and drag: the selected files follow the pointer;
 *    drop them on a folder in the list, a place in the left pane or a
 *    folder in the address bar.  Same drive = move, another drive (a
 *    USB stick) = copy, like Windows; hold Ctrl to always copy.
 * =================================================================== */
static int explorer_hit(int lx, int ly, int cw, int ch);
enum { EXD_NONE, EXD_PRESS, EXD_BAND, EXD_FILES };
static struct {
    int mode, x0, y0, lx, ly;          /* y0: in "list" units (scroll included) */
    int collapse;                      /* pressed on a row of a bigger selection: shrink to it on release */
    int target;                        /* the hit id under the pointer while dragging files */
    unsigned char keep[EX_MAX];        /* the selection before the box (Ctrl) */
} exd = { .collapse = -1, .target = -1 };

static int ex_list_top(int cw, int ch) { int x, y, w, h; ex_main(cw, ch, &x, &y, &w, &h); return y + EX_HDR_H + 2; }

/* where a drop on hit id would go ("" = nowhere) */
static void exd_target_path(int id, char *out, size_t n) {
    out[0] = 0;
    if (id >= 0 && id < ex_n) {
        if (ex_items[id].dir && !ex_selected(id)) ex_child(out, n, ex_items[id].name);
    } else if (id >= ID_EXNAV && id < ID_EXNAV + EX_NNAV) {
        const char *p = ex_nav[id - ID_EXNAV].path;
        if (p[0] && strcmp(p, "/")) snprintf(out, n, "%s", p);
    } else if (id >= ID_EXCRUMB && id < ID_EXCRUMB + EX_MAXCRUMB) {
        char labels[EX_MAXCRUMB][40], paths[EX_MAXCRUMB][256];
        int nc = ex_crumbs(labels, paths);
        if (id - ID_EXCRUMB < nc && paths[id - ID_EXCRUMB][0] && strcmp(paths[id - ID_EXCRUMB], "/"))
            snprintf(out, n, "%s", paths[id - ID_EXCRUMB]);
    }
    if (out[0] && !strcmp(out, ex_path)) out[0] = 0;           /* where they already are */
}
static int exd_copy_to(const char *dest) {        /* copy (not move) when dropping here? */
    if (ctrl) return 1;
    struct stat a, b;
    if (stat(ex_path, &a) || stat(dest, &b)) return 1;
    return a.st_dev != b.st_dev;
}

static void explorer_press(int lx, int ly, int cw, int ch, int id) {
    exd.mode = EXD_NONE; exd.collapse = -1; exd.target = -1;
    if (!ex_path[0] || ex_dlg.active) return;
    exd.x0 = exd.lx = lx; exd.ly = ly;
    exd.y0 = ly - ex_list_top(cw, ch) + ex_scroll * EX_ROW_H;
    if (id >= 0 && id < ex_n) { exd.mode = EXD_PRESS; return; }
    int mx0, my0, mw, mh; ex_main(cw, ch, &mx0, &my0, &mw, &mh);
    if (id == -1 && lx >= mx0 && ly >= my0 + EX_HDR_H && ly < my0 + mh) {   /* empty space: start a box */
        exd.mode = EXD_BAND;
        memset(exd.keep, 0, sizeof exd.keep);
        if (ctrl) for (int i = 0; i < ex_n; i++) exd.keep[i] = (unsigned char)ex_selected(i);
    }
}

static void explorer_drag(int lx, int ly, int cw, int ch) {
    if (exd.mode == EXD_NONE || win_of(explorer_draw) < 0) return;
    exd.lx = lx; exd.ly = ly;
    if (exd.mode == EXD_PRESS) {
        int dy = ly - (exd.y0 + ex_list_top(cw, ch) - ex_scroll * EX_ROW_H);
        if (abs(lx - exd.x0) + abs(dy) < 6) return;
        exd.mode = EXD_FILES; exd.collapse = -1;
    }
    if (exd.mode == EXD_BAND) {
        int top = ex_list_top(cw, ch), rows = ex_rows(cw, ch);
        if (ly < top && ex_scroll > 0) ex_scroll--;                          /* scroll while dragging past the edge */
        else if (ly > top + rows * EX_ROW_H && ex_scroll + rows < ex_n) ex_scroll++;
        int y1 = ly - top + ex_scroll * EX_ROW_H;
        int a = imin(exd.y0, y1), b = imax(exd.y0, y1);
        int r0 = imax(0, a / EX_ROW_H), r1 = imin(ex_n - 1, b / EX_ROW_H);
        if (b < 0) r1 = -1;
        int any = 0, last = -1;
        for (int i = 0; i < ex_n; i++) {
            ex_mark[i] = (unsigned char)(exd.keep[i] || (i >= r0 && i <= r1 && abs(lx - exd.x0) + abs(y1 - exd.y0) > 3));
            if (ex_mark[i]) { any++; last = i; }
        }
        ex_multi = any > 0; ex_sel = last; if (!any) ex_sel = -1;
    } else if (exd.mode == EXD_FILES) {
        int id = explorer_hit(lx, ly, cw, ch);
        char p[512]; exd_target_path(id, p, sizeof p);
        exd.target = p[0] ? id : -1;
    }
    repaint_client(explorer_draw);
}

static void ex_drop_to(const char *dest) {
    int n; char **v = ex_sel_paths(&n);
    if (!v || !n) { free(v); return; }
    if (access(dest, W_OK) != 0) { ex_info("That place is read-only:", "files can't be put there."); ex_free_list(v, n); return; }
    int copy = exd_copy_to(dest);
    char d[512]; snprintf(d, sizeof d, "%s", dest);
    char **argv = calloc((size_t)n + 4, sizeof *argv);
    if (argv) {
        argv[0] = "ccos-files"; argv[1] = copy ? "copy" : "move"; argv[2] = d;
        for (int i = 0; i < n; i++) argv[3 + i] = v[i];
        fj_start(copy ? FJ_COPY : FJ_MOVE, argv, d, 0);
        free(argv);
    }
    ex_free_list(v, n);
}

static void explorer_release(void) {
    int mode = exd.mode, target = exd.target, collapse = exd.collapse;
    exd.mode = EXD_NONE; exd.target = -1; exd.collapse = -1;
    if (mode == EXD_NONE || win_of(explorer_draw) < 0) return;
    if (mode == EXD_FILES && target != -1) {
        char p[512]; exd_target_path(target, p, sizeof p);
        if (p[0]) ex_drop_to(p);
    } else if (mode == EXD_PRESS && collapse >= 0 && collapse < ex_n) ex_single(collapse);
    repaint_client(explorer_draw);
}

/* drawn last in explorer_draw: the blue box, the drop target, a label by the pointer */
static void ex_drag_draw(int cx, int cy, int cw, int ch) {
    if (exd.mode == EXD_BAND) {
        int mx0, my0, mw, mh; ex_main(cw, ch, &mx0, &my0, &mw, &mh);
        int top = ex_list_top(cw, ch);
        int y0 = exd.y0 + top - ex_scroll * EX_ROW_H;
        int x = imin(exd.x0, exd.lx), y = imin(y0, exd.ly), w = abs(exd.lx - exd.x0), h = abs(exd.ly - y0);
        int lo = my0 + EX_HDR_H, hi = my0 + mh;                    /* keep it inside the list */
        if (y < lo) { h -= lo - y; y = lo; }
        if (y + h > hi) h = hi - y;
        if (x < mx0) { w -= mx0 - x; x = mx0; }
        if (x + w > mx0 + mw) w = mx0 + mw - x;
        if (w > 1 && h > 1) {
            fill_rect(cx + x, cy + y, w, h, RGB(0x33, 0x99, 0xff), 50);
            stroke_round(cx + x, cy + y, w, h, 0, RGB(0x33, 0x88, 0xee), 255);
        }
    } else if (exd.mode == EXD_FILES) {
        int x, y, w, h; x = y = w = h = 0;
        if (exd.target >= 0 && exd.target < ex_n) {
            int mx0, my0, mw, mh; ex_main(cw, ch, &mx0, &my0, &mw, &mh);
            x = mx0 + 2; y = ex_list_top(cw, ch) + (exd.target - ex_scroll) * EX_ROW_H; w = mw - 4 - EX_SB_W; h = EX_ROW_H;
        } else if (exd.target >= ID_EXNAV && exd.target < ID_EXNAV + EX_NNAV) {
            x = 4; y = ex_nav_y(exd.target - ID_EXNAV); w = EX_LEFT_W - 10; h = 22;
        }
        if (w) { stroke_round(cx + x, cy + y, w, h, 2, RGB(0x26, 0x7c, 0xe0), 255); stroke_round(cx + x + 1, cy + y + 1, w - 2, h - 2, 2, RGB(0x26, 0x7c, 0xe0), 255); }
        /* the label that follows the pointer */
        char lab[120], p[512]; int n = ex_nselected();
        exd_target_path(exd.target, p, sizeof p);
        if (p[0]) {
            const char *b = strrchr(p, '/'); const char *nm = b && b[1] ? b + 1 : p;
            for (int i = 0; i < nrd; i++) if (!strcmp(rdrv[i].mnt, p)) nm = rdrv[i].name;
            snprintf(lab, sizeof lab, "%s %d item%s to %.24s", exd_copy_to(p) ? "Copy" : "Move", n, n == 1 ? "" : "s", nm);
        } else snprintf(lab, sizeof lab, "%d item%s", n, n == 1 ? "" : "s");
        int tw = text_w(lab) + 34, tx = imin(exd.lx + 16, cw - tw - 2), ty = imin(exd.ly + 18, ch - 26);
        fill_rect(cx + tx + 2, cy + ty + 2, tw, 22, RGB(0, 0, 0), 40);
        fill_round(cx + tx, cy + ty, tw, 22, 2, RGB(0xf8, 0xfb, 0xff), 255);
        stroke_round(cx + tx, cy + ty, tw, 22, 2, RGB(0x86, 0xa8, 0xd6), 255);
        draw_icon(n == 1 && ex_sel >= 0 && ex_sel < ex_n && ex_items[ex_sel].dir ? ICON_FOLDER : ICON_FILE, cx + tx + 13, cy + ty + 11, 6);
        draw_text(cx + tx + 26, cy + ty + 3, lab, RGB(0x1e, 0x39, 0x5b), 255);
    }
}
