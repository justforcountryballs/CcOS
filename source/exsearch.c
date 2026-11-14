/* ===================================================================
 *  File Explorer search: the box right of the address. Finds files and
 *  folders in the open folder and everything inside it, by name. For
 *  D: and Downloads it uses the Start menu search's list (read in the
 *  background); elsewhere it looks through the folder itself.
 * =================================================================== */
static int exs_add(const char *full, int isdir, long long mt) {
    size_t pl = strlen(ex_path);
    if (ex_n >= EX_MAX) return 0;
    const char *rel = !strcmp(ex_path, "/") ? full + 1 : full + pl + 1;
    const char *nm = strrchr(full, '/'); nm = nm ? nm + 1 : full;
    if (!strcasestr(nm, ex_q)) return 1;
    ExItem *it = &ex_items[ex_n];
    snprintf(it->name, sizeof it->name, "%s", rel);
    it->dir = isdir; it->link = 0; it->mtime = (time_t)mt; it->size = 0;
    if (!isdir) { struct stat st; if (stat(full, &st) == 0) { it->size = st.st_size; it->mtime = st.st_mtime; } }
    ex_n++;
    return 1;
}
static void exs_walk(const char *dir, int depth, int *seen) {
    if (depth > 6 || *seen > 5000 || ex_n >= EX_MAX) return;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *seen <= 5000 && ex_n < EX_MAX) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lost+found")) continue;
        char p[600]; snprintf(p, sizeof p, "%s/%s", !strcmp(dir, "/") ? "" : dir, e->d_name);
        struct stat st; if (lstat(p, &st)) continue;
        (*seen)++;
        exs_add(p, S_ISDIR(st.st_mode), st.st_mtime);
        if (S_ISDIR(st.st_mode) && strcmp(p, "/proc") && strcmp(p, "/sys") && strcmp(p, "/dev")) exs_walk(p, depth + 1, seen);
    }
    closedir(d);
}
static int exs_cmp(const void *a, const void *b) {
    const ExItem *x = a, *y = b;
    const char *nx = strrchr(x->name, '/'), *ny = strrchr(y->name, '/');
    nx = nx ? nx + 1 : x->name; ny = ny ? ny + 1 : y->name;
    int px = !strncasecmp(nx, ex_q, strlen(ex_q)), py = !strncasecmp(ny, ex_q, strlen(ex_q));   /* starts with it: first */
    if (px != py) return py - px;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(nx, ny);
}
static long long ex_search_fill(void) {
    ex_n = 0;
    size_t pl = strlen(ex_path);
    const char *dl = dl_dir();
    int listed = (!strncmp(ex_path, "/data", 5) && (ex_path[5] == 0 || ex_path[5] == '/')) || !strncmp(ex_path, dl, strlen(dl));
    if (listed && ms_nfiles) {
        for (int i = 0; i < ms_nfiles && ex_n < EX_MAX; i++) {
            const char *p = ms_files[i] + 1;
            if (strncmp(p, ex_path, pl) || p[pl] != '/') continue;
            exs_add(p, ms_files[i][0] == 'd', ms_mtime ? ms_mtime[i] : 0);
        }
    } else { int seen = 0; exs_walk(ex_path, 0, &seen); }
    qsort(ex_items, (size_t)ex_n, sizeof *ex_items, exs_cmp);
    long long sig = 7;
    for (int i = 0; i < ex_n; i++) sig = sig * 31 + (long long)strlen(ex_items[i].name) + ex_items[i].mtime;
    return sig + ex_n;
}
static void ex_search_changed(void) {
    ex_sig = ex_read(); ex_sel = -1; ex_multi = 0; ex_scroll = 0; ex_last_idx = -1;
    repaint_client(explorer_draw);
}
static void ex_search_key(int code, int mods) {
    if (code == KEY_ESC) { ex_sfocus = 0; if (ex_q[0]) { ex_q[0] = 0; ex_search_changed(); } else repaint_client(explorer_draw); return; }
    if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_DOWN || code == KEY_TAB) {   /* to the results */
        ex_sfocus = 0; if (ex_n) ex_single(0); repaint_client(explorer_draw); return;
    }
    if (code == KEY_BACKSPACE) {
        int l = (int)strlen(ex_q);
        if (l) { ex_q[u8_prev(ex_q, l)] = 0; ex_search_changed(); }
        return;
    }
    char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t);
    if (k <= 0 || (mods & MOD_CTRL) || t[0] == '\t' || t[0] == '\n') return;
    if (!ex_path[0]) ex_go("/data", 1);                       /* Computer: search D: */
    size_t l = strlen(ex_q);
    if (l + (size_t)k < sizeof ex_q) { memcpy(ex_q + l, t, (size_t)k); ex_q[l + (size_t)k] = 0; }
    ex_sfocus = 1;
    ex_search_changed();
}
/* the Start menu search read the files again: a search running in File Explorer gets the new list */
static void ex_search_files_changed(void) { if (ex_q[0] && win_of(explorer_draw) >= 0) ex_search_changed(); }
