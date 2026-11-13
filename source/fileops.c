/* ===================================================================
 *  FILE EXPLORER: selecting several files, Cut / Copy / Paste, and
 *  zip files. The copying itself runs in /usr/bin/ccos-files, so the
 *  desktop never freezes; a progress window shows up when it takes
 *  more than a moment.
 * =================================================================== */
static struct { char **path; int n, cut; } fclip;   /* the files on the clipboard */
static char ex_note[96]; static long long ex_note_until;   /* a short message in the status bar */

static int ex_selected(int i) { return ex_multi ? ex_mark[i] : i == ex_sel; }
static int ex_nselected(void) {
    if (!ex_multi) return ex_sel >= 0 && ex_sel < ex_n;
    int n = 0; for (int i = 0; i < ex_n; i++) n += ex_mark[i];
    return n;
}
static void ex_single(int i) { ex_multi = 0; ex_sel = i; ex_anchor = i; }
static void ex_note_set(const char *s) { snprintf(ex_note, sizeof ex_note, "%s", s); ex_note_until = ui_ms() + 4000; }
/* the full paths of what's selected (malloc'd; free with ex_free_list) */
static char **ex_sel_paths(int *n) {
    *n = 0;
    char **v = calloc((size_t)ex_n + 1, sizeof *v);
    if (!v) return NULL;
    for (int i = 0; i < ex_n; i++) if (ex_selected(i)) { char p[512]; ex_child(p, sizeof p, ex_items[i].name); v[(*n)++] = strdup(p); }
    return v;
}
static void ex_free_list(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }

/* a click on a row, with Ctrl (add/remove) or Shift (a range) */
static void ex_click_row(int i) {
    if (ctrl) {
        if (!ex_multi) { memset(ex_mark, 0, sizeof ex_mark); if (ex_sel >= 0 && ex_sel < ex_n) ex_mark[ex_sel] = 1; ex_multi = 1; }
        ex_mark[i] = !ex_mark[i]; ex_sel = i; ex_anchor = i;
    } else if (shift && ex_anchor >= 0 && ex_anchor < ex_n) {
        memset(ex_mark, 0, sizeof ex_mark); ex_multi = 1;
        for (int k = imin(ex_anchor, i); k <= imax(ex_anchor, i); k++) ex_mark[k] = 1;
        ex_sel = i;
    } else ex_single(i);
}
static void ex_select_all(void) {
    if (!ex_path[0] || !ex_n) return;
    memset(ex_mark, 1, (size_t)ex_n); ex_multi = 1;
    if (ex_sel < 0) ex_sel = 0;
}

/* ---- the clipboard ---- */
static void ex_clip(int cut) {
    if (!ex_path[0] || !ex_nselected()) return;
    if (cut && !ex_writable()) { ex_need_writable(); return; }
    for (int i = 0; i < fclip.n; i++) free(fclip.path[i]);
    free(fclip.path);
    fclip.path = ex_sel_paths(&fclip.n); fclip.cut = cut;
    char b[96]; snprintf(b, sizeof b, "%d item%s %s. Go to a folder and press Ctrl+V to paste.", fclip.n, fclip.n == 1 ? "" : "s", cut ? "cut" : "copied");
    ex_note_set(b);
    repaint_client(explorer_draw);
}
static int ex_clip_cut_has(const char *path) {        /* cut files look faded until pasted */
    if (!fclip.cut) return 0;
    for (int i = 0; i < fclip.n; i++) if (!strcmp(fclip.path[i], path)) return 1;
    return 0;
}

/* ---- jobs: ccos-files in the background ---- */
enum { FJ_COPY, FJ_MOVE, FJ_ZIP, FJ_UNZIP };
static struct {
    pid_t pid; int kind, shown, open_after;
    long long total, done, started;
    int items;
    int nsrc;
    char cur[96], dest[300], dest_name[64], first_new[512], err[160], news[4096];
} fj;
static void fileop_draw(int cx, int cy, int cw, int ch);
static int fj_alive(void) { if (fj.pid > 0 && kill(fj.pid, 0) != 0) fj.pid = 0; return fj.pid > 0; }
static void fj_start(int kind, char **argv, const char *dest, int open_after) {
    if (fj_alive()) { ex_info("Wait a moment: the last copy", "isn't finished yet."); return; }
    mkdir("/run/ccos", 0755);
    memset(&fj, 0, sizeof fj);
    fj.kind = kind; fj.open_after = open_after; fj.started = ui_ms();
    for (int i = 3; argv[i]; i++) fj.nsrc++;               /* what was picked (not every file inside) */
    snprintf(fj.dest, sizeof fj.dest, "%s", dest);
    const char *b = strrchr(dest, '/');
    snprintf(fj.dest_name, sizeof fj.dest_name, "%s", b && b[1] ? b + 1 : dest);
    for (int i = 0; i < nrd; i++) if (!strcmp(rdrv[i].mnt, dest)) snprintf(fj.dest_name, sizeof fj.dest_name, "%s", rdrv[i].name);
    if (!strcmp(dest, "/data")) snprintf(fj.dest_name, sizeof fj.dest_name, "ccos Data (D:)");
    pid_t p = fork();
    if (p == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        int f = open("/run/ccos/fileop.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { dup2(f, 1); close(f); }
        execv("/usr/bin/ccos-files", argv);
        _exit(127);
    }
    fj.pid = p;
}
static void fj_read(int *finished, int *ok, int *canceled) {
    *finished = *ok = *canceled = 0;
    FILE *f = fopen("/run/ccos/fileop.txt", "r");
    if (!f) return;
    char l[700];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        if (l[0] == 'T' && l[1] == ' ') sscanf(l + 2, "%lld %d", &fj.total, &fj.items);
        else if (l[0] == 'P' && l[1] == ' ') { char *s = strchr(l + 2, ' '); fj.done = atoll(l + 2); if (s && s[1]) snprintf(fj.cur, sizeof fj.cur, "%s", s + 1); }
        else if (l[0] == 'N' && l[1] == ' ') {
            if (!fj.first_new[0]) snprintf(fj.first_new, sizeof fj.first_new, "%s", l + 2);
            const char *b = strrchr(l + 2, '/'); size_t k = strlen(fj.news);
            if (b && k + strlen(b) + 2 < sizeof fj.news) { strcat(fj.news, b + 1); strcat(fj.news, "\n"); }
        }
        else if (!strcmp(l, "OK")) { *finished = 1; *ok = 1; }
        else if (!strcmp(l, "CANCELED")) { *finished = 1; *canceled = 1; }
        else if (!strncmp(l, "ERR ", 4)) { *finished = 1; snprintf(fj.err, sizeof fj.err, "%s", l + 4); }
    }
    fclose(f);
}
static void fj_cancel(void) { if (fj_alive()) kill(fj.pid, SIGTERM); }
static const char *fj_verb(int ing) {
    static const char *a[] = { "Copying", "Moving", "Compressing", "Extracting" }, *b[] = { "Copied", "Moved", "Compressed", "Extracted" };
    return ing ? a[fj.kind] : b[fj.kind];
}
/* once a second */
static void fileop_poll(void) {
    if (!fj.pid && !fj.started) return;
    int alive = fj_alive(), fin, ok, can;
    fj_read(&fin, &ok, &can);
    if (alive) {                                       /* still going: show the window after a moment */
        if (!fj.shown && ui_ms() - fj.started > 900) { fj.shown = 1; run("@fileop"); }
        if (fj.shown) repaint_client(fileop_draw);
        return;
    }
    /* finished */
    fj.started = 0;
    int w = win_of(fileop_draw); if (w >= 0) close_win(w);
    if (win_of(explorer_draw) >= 0 && ex_path[0]) { ex_sig = ex_read(); ex_multi = 0; }
    char b[200];
    if (ok) {
        if (fj.kind == FJ_MOVE) {                          /* cut and pasted: the clipboard is used up */
            for (int i = 0; i < fclip.n; i++) free(fclip.path[i]);
            free(fclip.path); fclip.path = NULL; fclip.n = 0; fclip.cut = 0;
        }
        const char *nm = strrchr(fj.first_new, '/');
        if (fj.open_after && nm && win_of(explorer_draw) < 0) { run("@explorer"); ex_go(fj.first_new, 1); }   /* opened from Search */
        else if (win_of(explorer_draw) >= 0 && nm) {
            char dir[512]; snprintf(dir, sizeof dir, "%s", fj.first_new); dir[nm - fj.first_new] = 0;
            if (fj.open_after) ex_go(fj.first_new, 1);
            else if (!strcmp(dir, ex_path)) {
                ex_select_name(nm + 1);
                int cnt = 0;                               /* select everything that was pasted */
                for (int i = 0; i < ex_n; i++) {
                    char key[300]; snprintf(key, sizeof key, "\n%s\n", ex_items[i].name);
                    char hay[4100]; snprintf(hay, sizeof hay, "\n%s", fj.news);
                    ex_mark[i] = strstr(hay, key) != NULL; cnt += ex_mark[i];
                }
                if (cnt > 1) ex_multi = 1;
            }
        }
        if (fj.kind == FJ_ZIP) snprintf(b, sizeof b, "Made \"%s\".", nm ? nm + 1 : "the zip file");
        else if (fj.kind == FJ_UNZIP) snprintf(b, sizeof b, "Unpacked into \"%s\".", nm ? nm + 1 : fj.dest_name);
        else snprintf(b, sizeof b, "%s %d item%s to %s.", fj_verb(0), fj.nsrc, fj.nsrc == 1 ? "" : "s", fj.dest_name);
        if (fj.shown || fj.kind >= FJ_ZIP) toast(ICON_FOLDER, fj_verb(0), b);
        else ex_note_set(b);
    } else if (can) {
        toast(ICON_FOLDER, "Stopped", "Nothing half-finished was left behind.");
    } else {
        snprintf(b, sizeof b, "%s", fj.err[0] ? fj.err : "Something went wrong.");
        toast(ICON_FOLDER, fj.kind == FJ_UNZIP ? "Couldn't unpack it" : fj.kind == FJ_ZIP ? "Couldn't make the zip" : "Couldn't finish", b);
    }
    if (win_of(explorer_draw) >= 0) repaint_client(explorer_draw);
}

/* Paste: what's on the clipboard, into this folder */
static void ex_paste(void) {
    if (!ex_path[0]) return;
    if (!fclip.n) { ex_note_set("Nothing to paste: copy or cut some files first."); repaint_client(explorer_draw); return; }
    if (!ex_writable()) { ex_need_writable(); return; }
    char **argv = calloc((size_t)fclip.n + 4, sizeof *argv);
    if (!argv) return;
    argv[0] = "ccos-files"; argv[1] = fclip.cut ? "move" : "copy"; argv[2] = ex_path;
    for (int i = 0; i < fclip.n; i++) argv[3 + i] = fclip.path[i];
    fj_start(fclip.cut ? FJ_MOVE : FJ_COPY, argv, ex_path, 0);
    free(argv);
}
/* the folder for new files when this one is read-only: Downloads */
static void ex_out_dir(char *out, size_t n) {
    if (ex_writable()) snprintf(out, n, "%s", ex_path);
    else { snprintf(out, n, "%s", dl_dir()); mkdir(out, 0755); }
}
static int ex_is_zip(const char *name) { const char *e = strrchr(name, '.'); return e && !strcasecmp(e, ".zip"); }
/* other archives ccos-files can unpack (with libarchive): how long the ending is, or 0 */
static int ex_arc_ext(const char *name) {
    static const char *ends[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tgz", ".txz", ".tbz2", ".tar", ".rar", ".7z", ".zip", NULL };
    size_t l = strlen(name);
    for (int i = 0; ends[i]; i++) { size_t k = strlen(ends[i]); if (l > k && !strcasecmp(name + l - k, ends[i])) return (int)k; }
    return 0;
}
static int ex_is_archive(const char *name) { return ex_arc_ext(name) > 0; }
/* Compress: the selection into one zip file */
static void ex_compress(void) {
    int n; char **v = ex_sel_paths(&n);
    if (!v || !n) { free(v); return; }
    char dir[300], out[600];
    ex_out_dir(dir, sizeof dir);
    if (n == 1) {
        char nm[200]; snprintf(nm, sizeof nm, "%s", strrchr(v[0], '/') + 1);
        char *dot = strrchr(nm, '.'); struct stat st;
        if (dot && dot != nm && stat(v[0], &st) == 0 && !S_ISDIR(st.st_mode)) *dot = 0;     /* photo.jpg -> photo.zip */
        snprintf(out, sizeof out, "%s/%s.zip", dir, nm);
    } else {
        const char *f = strrchr(ex_path, '/');
        snprintf(out, sizeof out, "%s/%s.zip", dir, f && f[1] ? f + 1 : "Archive");
    }
    char **argv = calloc((size_t)n + 4, sizeof *argv);
    argv[0] = "ccos-files"; argv[1] = "zip"; argv[2] = out;
    for (int i = 0; i < n; i++) argv[3 + i] = v[i];
    fj_start(FJ_ZIP, argv, dir, 0);
    free(argv); ex_free_list(v, n);
}
/* Extract: a zip (or rar, 7z, tar...) into a new folder with its name, then open that folder */
static void ex_extract(const char *zip) {
    char dir[300]; snprintf(dir, sizeof dir, "%s", zip);       /* next to the archive, or in Downloads if that's read-only */
    { char *sl = strrchr(dir, '/'); if (sl && sl != dir) *sl = 0; else snprintf(dir, sizeof dir, "/"); }
    if (!ex_on_d(dir) || access(dir, W_OK)) { snprintf(dir, sizeof dir, "%s", dl_dir()); mkdir(dir, 0755); }
    char nm[200]; snprintf(nm, sizeof nm, "%s", strrchr(zip, '/') ? strrchr(zip, '/') + 1 : zip);
    int el = ex_arc_ext(nm);
    if (el && (size_t)el < strlen(nm)) nm[strlen(nm) - (size_t)el] = 0;   /* photos.tar.gz -> photos */
    else { char *dot = strrchr(nm, '.'); if (dot && dot != nm) *dot = 0; }
    char out[600]; snprintf(out, sizeof out, "%s/%s", dir, nm);
    for (int k = 2; access(out, F_OK) == 0 && k < 100; k++) snprintf(out, sizeof out, "%s/%s (%d)", dir, nm, k);
    char zp[600]; snprintf(zp, sizeof zp, "%s", zip);
    char *argv[] = { "ccos-files", ex_is_zip(zp) ? "unzip" : "extract", zp, out, NULL };
    fj_start(FJ_UNZIP, argv, dir, 1);
}

/* ---- the progress window ---- */
#define FO_W 470
#define FO_H 168
static void fileop_draw(int cx, int cy, int cw, int ch) {
    (void)ch;
    fill_rect(cx, cy, cw, ch, RGB(0xf6, 0xf8, 0xfb), 255);
    uint32_t text = RGB(0x1e, 0x39, 0x5b), grey = RGB(0x6d, 0x6d, 0x6d);
    char b[160];
    if (fj.kind == FJ_ZIP) snprintf(b, sizeof b, "Compressing %d item%s", fj.nsrc, fj.nsrc == 1 ? "" : "s");
    else if (fj.kind == FJ_UNZIP) snprintf(b, sizeof b, "Extracting %d file%s", fj.items, fj.items == 1 ? "" : "s");
    else snprintf(b, sizeof b, "%s %d item%s to %.30s", fj_verb(1), fj.nsrc, fj.nsrc == 1 ? "" : "s", fj.dest_name);
    draw_text_bold(cx + 18, cy + 16, b, text);
    char c[80]; snprintf(c, sizeof c, "%.52s", fj.cur);
    draw_text(cx + 18, cy + 42, c, grey, 255);
    int pct = fj.total > 0 ? (int)(fj.done * 100 / fj.total) : 0;
    if (pct > 100) pct = 100;
    int bx = cx + 18, bw = cw - 36, by = cy + 70;
    fill_round(bx, by, bw, 18, 3, RGB(0xe2, 0xe6, 0xec), 255);
    stroke_round(bx, by, bw, 18, 3, RGB(0xb8, 0xc0, 0xcc), 255);
    if (pct) fill_round_grad(bx + 1, by + 1, imax(4, (bw - 2) * pct / 100), 16, 2, RGB(0x6c, 0xd8, 0x6c), RGB(0x2c, 0xa8, 0x3c), 255);
    char sz1[24], sz2[24]; ex_fmt_size((double)fj.done, sz1, sizeof sz1); ex_fmt_size((double)fj.total, sz2, sizeof sz2);
    snprintf(b, sizeof b, "%d%%   %s of %s", pct, sz1, sz2);
    draw_text(cx + 18, cy + 98, b, grey, 255);
    st_choice(cx + cw - 18 - 100, cy + ch - 46, 100, 30, "Cancel", 0);
}
static void fileop_click(int lx, int ly, int cw, int ch) {
    if (lx >= cw - 118 && lx < cw - 18 && ly >= ch - 46 && ly < ch - 16) fj_cancel();
}
static void fileop_key(int code, int mods) { (void)mods; if (code == KEY_ESC) fj_cancel(); }
static int fileop_on_close(void) { if (fj_alive()) { fj_cancel(); return 0; } return 1; }   /* closing it = Cancel */
