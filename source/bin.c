/* ===================================================================
 *  Recycle Bin
 *
 *  Deleting in File Explorer moves things to /data/.bin (a real folder,
 *  shown as "Recycle Bin"), so they can be put back. /data/.bin/.index
 *  remembers where each came from:   name <TAB> original path <TAB> time
 *  Things on another disk (a USB drive) can't go there: those are still
 *  deleted for good, after asking - like Windows.
 *  On the stick, D: (/data) and Downloads (/apps/home/Downloads) are the
 *  same partition seen in two places; moving between them is a rename of
 *  the partition's real paths (found through /proc/self/mountinfo).
 * =================================================================== */
#define BIN_DIR "/data/.bin"
#define BIN_INDEX BIN_DIR "/.index"
static int rm_rf(const char *p);

/* where path really is: the mount that has it, as (device, path inside that device) */
static int mnt_real(const char *path, char *dev, size_t dn, char *inner, size_t in, char *mnt, size_t mn) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return -1;
    char l[1024]; size_t best = 0; int ok = -1;
    while (fgets(l, sizeof l, f)) {
        char d[32], root[400], mp[400];
        if (sscanf(l, "%*s %*s %31s %399s %399s", d, root, mp) != 3) continue;
        size_t k = strlen(mp);
        int match = !strcmp(mp, "/") ? 1 : (!strncmp(path, mp, k) && (path[k] == '/' || !path[k]));
        if (!match || k < best) continue;
        best = k; ok = 0;
        snprintf(dev, dn, "%s", d);
        snprintf(mnt, mn, "%s", mp);
        const char *rest = !strcmp(mp, "/") ? path : path + k;
        snprintf(inner, in, "%s%s", strcmp(root, "/") ? root : "", rest);
    }
    fclose(f);
    return ok;
}
/* a mount point that shows the whole device dev ("" if none) */
static void mnt_whole(const char *dev, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/proc/self/mountinfo", "r"); if (!f) return;
    char l[1024];
    while (fgets(l, sizeof l, f)) {
        char d[32], root[400], mp[400];
        if (sscanf(l, "%*s %*s %31s %399s %399s", d, root, mp) == 3 && !strcmp(d, dev) && !strcmp(root, "/")) {
            snprintf(out, n, "%s", mp); break;
        }
    }
    fclose(f);
}
/* rename that also works between two views (bind mounts) of one partition */
static int rename_any(const char *from, const char *to) {
    if (rename(from, to) == 0) return 0;
    if (errno != EXDEV) return -1;
    char d1[32], d2[32], i1[512], i2[512], m1[400], m2[400], w[400];
    if (mnt_real(from, d1, sizeof d1, i1, sizeof i1, m1, sizeof m1) || mnt_real(to, d2, sizeof d2, i2, sizeof i2, m2, sizeof m2)) return -1;
    if (strcmp(d1, d2)) { errno = EXDEV; return -1; }
    mnt_whole(d1, w, sizeof w);
    if (!w[0]) { errno = EXDEV; return -1; }
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s%s", strcmp(w, "/") ? w : "", i1);
    snprintf(b, sizeof b, "%s%s", strcmp(w, "/") ? w : "", i2);
    return rename(a, b);
}
/* could this go in the Bin? (same partition as D:) */
static int bin_can(const char *path) {
    if (!strncmp(path, BIN_DIR, strlen(BIN_DIR))) return 0;
    struct stat a, b;
    if (lstat(path, &a) || stat("/data", &b)) return 0;
    if (a.st_dev == b.st_dev) return 1;
    char d1[32], d2[32], i[512], m[400], w[400];
    if (mnt_real(path, d1, sizeof d1, i, sizeof i, m, sizeof m) || mnt_real("/data", d2, sizeof d2, i, sizeof i, m, sizeof m)) return 0;
    if (strcmp(d1, d2)) return 0;
    mnt_whole(d1, w, sizeof w);
    return w[0] != 0;
}
static int bin_count(void) {
    DIR *d = opendir(BIN_DIR); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
static void bin_name_free(const char *dir, const char *name, char *out, size_t n) {
    char base[256], ext[32] = "";
    snprintf(base, sizeof base, "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base && strlen(dot) < sizeof ext) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    char p[600]; snprintf(out, n, "%s", name);
    snprintf(p, sizeof p, "%s/%s", dir, out);
    for (int k = 2; access(p, F_OK) == 0 && k < 10000; k++) {
        snprintf(out, n, "%.200s (%d)%s", base, k, ext);
        snprintf(p, sizeof p, "%s/%s", dir, out);
    }
}
/* move path to the Bin; 0 = done */
static int bin_put(const char *path) {
    mkdir("/data", 0755); mkdir(BIN_DIR, 0700);
    const char *nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
    char inbin[256]; bin_name_free(BIN_DIR, nm, inbin, sizeof inbin);
    char to[600]; snprintf(to, sizeof to, BIN_DIR "/%s", inbin);
    if (rename_any(path, to)) return -1;
    FILE *f = fopen(BIN_INDEX, "a");
    if (f) { fprintf(f, "%s\t%s\t%lld\n", inbin, path, (long long)time(NULL)); fclose(f); }
    return 0;
}
/* the index line for a name in the Bin: its original path and time */
static int bin_lookup(const char *inbin, char *orig, size_t n, long long *when) {
    FILE *f = fopen(BIN_INDEX, "r"); if (!f) return -1;
    char l[1200]; int ok = -1;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        char *t1 = strchr(l, '\t'); if (!t1) continue; *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = 0;
        if (strcmp(l, inbin)) continue;
        snprintf(orig, n, "%s", t1 + 1); if (when) *when = atoll(t2 + 1); ok = 0;   /* the last one counts */
    }
    fclose(f);
    return ok;
}
/* forget names that are no longer in the Bin */
static void bin_tidy_index(void) {
    FILE *f = fopen(BIN_INDEX, "r"); if (!f) return;
    char tmp[] = BIN_INDEX ".new";
    FILE *o = fopen(tmp, "w"); if (!o) { fclose(f); return; }
    char l[1200];
    while (fgets(l, sizeof l, f)) {
        char nm[300]; snprintf(nm, sizeof nm, "%.*s", (int)strcspn(l, "\t"), l);
        char p[600]; snprintf(p, sizeof p, BIN_DIR "/%s", nm);
        if (nm[0] && access(p, F_OK) == 0) fputs(l, o);
    }
    fclose(f); fclose(o); rename(tmp, BIN_INDEX);
}
/* put one back where it came from; out = where it went */
static int bin_restore(const char *inbin, char *out, size_t on) {
    char orig[1024]; long long when;
    if (bin_lookup(inbin, orig, sizeof orig, &when)) return -1;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", orig);
    char *sl = strrchr(dir, '/'); if (!sl) return -1;
    *sl = 0;
    /* its folder may be gone too: make it again */
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    mkdir(dir, 0755);
    char nm[256]; bin_name_free(dir, sl + 1, nm, sizeof nm);
    char to[1300]; snprintf(to, sizeof to, "%s/%s", dir, nm);
    char from[600]; snprintf(from, sizeof from, BIN_DIR "/%s", inbin);
    if (rename_any(from, to)) return -1;
    if (out) snprintf(out, on, "%s", to);
    return 0;
}
static int bin_empty(void) {
    int bad = 0;
    DIR *d = opendir(BIN_DIR); if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[600]; snprintf(p, sizeof p, BIN_DIR "/%s", e->d_name);
        if (rm_rf(p)) bad++;
    }
    closedir(d);
    unlink(BIN_INDEX);
    bg_sync();
    return bad;
}
