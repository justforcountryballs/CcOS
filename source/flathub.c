/* ===================================================================
 *  Downloads > Flathub: one launcher file for every app installed from
 *  Flathub.  Double-click one to start the app.  Kept in step with what
 *  is installed: new apps get one, removed apps lose theirs.
 *
 *  A launcher is a small text file, "Firefox.app":
 *      [ccos launcher]
 *      Name=Firefox
 *      Run=flatpak:org.mozilla.firefox
 * =================================================================== */
static void ex_info(const char *a, const char *b);
static int is_launcher(const char *name) {
    const char *e = strrchr(name, '.');
    return e && !strcasecmp(e, ".app");
}
/* the Run= line of a launcher ("" if it isn't one) */
static void launcher_cmd(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char l[300]; int ok = 0;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (!strcmp(l, "[ccos launcher]")) ok = 1;
        else if (ok && !strncmp(l, "Run=", 4)) snprintf(out, n, "%s", l + 4);
    }
    fclose(f);
}
static void launcher_run(const char *path) {
    char c[200]; launcher_cmd(path, c, sizeof c);
    if (!strncmp(c, "flatpak:", 8) || c[0] == '@') {
        for (int i = 0; i < nfp; i++) if (!strcmp(fpapps[i].cmd, c)) { run(c); return; }
        if (c[0] == '@') { run(c); return; }
        ex_info("This app isn't installed any more.", "Get it again in Software Manager.");
        return;
    }
    ex_info("This launcher doesn't say which", "app to start.");
}
/* the picture for "Name.app": the app's own icon when it's installed */
static int launcher_icon(const char *name) {
    size_t l = strlen(name) - 4;
    for (int i = 0; i < nfp; i++)
        if (strlen(fp_info[i].label) == l && !strncmp(fp_info[i].label, name, l)) return fp_info[i].icon;
    return ICON_APP;
}

static void flathub_sync(void) {
    static int calls;
    if (!calls++) return;              /* the very first scan runs before the clock is set right */
    char dir[300]; snprintf(dir, sizeof dir, "%s/Flathub", dl_dir());
    if (!nfp && access(dir, F_OK) != 0) return;            /* no apps yet: no folder yet */
    mkdir(dl_dir(), 0755);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return;
    /* launchers for apps that are gone */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!is_launcher(e->d_name)) continue;
            char p[600], c[200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            launcher_cmd(p, c, sizeof c);
            if (strncmp(c, "flatpak:", 8)) continue;            /* not one of ours */
            int have = 0;
            for (int i = 0; i < nfp; i++) if (!strcmp(fpapps[i].cmd, c)) have = 1;
            if (!have) unlink(p);
        }
        closedir(d);
    }
    /* and for new ones */
    for (int i = 0; i < nfp; i++) {
        char nm[64]; snprintf(nm, sizeof nm, "%s", fpapps[i].name);
        for (char *s = nm; *s; s++) if (*s == '/') *s = '-';
        char p[600]; snprintf(p, sizeof p, "%s/%s.app", dir, nm);
        char c[200]; launcher_cmd(p, c, sizeof c);
        if (!strcmp(c, fpapps[i].cmd)) continue;                /* already there */
        if (c[0]) snprintf(p, sizeof p, "%s/%s (%s).app", dir, nm, fpapps[i].id);   /* two apps, one name */
        if (access(p, F_OK) == 0) continue;
        FILE *f = fopen(p, "w");
        if (!f) continue;
        fprintf(f, "[ccos launcher]\nName=%s\nRun=%s\n", fpapps[i].name, fpapps[i].cmd);
        fclose(f);
    }
}
