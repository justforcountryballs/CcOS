/* ===================================================================
 *  PRINT (Photos > Print, or Ctrl+P): pick a printer and how the
 *  picture goes on the paper. The printing itself is /usr/bin/ccos-print:
 *  it finds Wi-Fi printers, asks them what they can do, and sends pages.
 *  The last printer used is remembered and picked automatically next time.
 * =================================================================== */
#define PR_W 720
#define PR_H 556
#define PR_MAXP 8
#define PR_MAXM 14
#define PR_SAVED "/data/printers.txt"
static struct {
    char file[300];
    struct { char name[64], uri[160], model[64]; int color; } p[PR_MAXP];
    int np, sel;
    /* what the chosen printer can do */
    int caps_ok;                      /* 0 asking, 1 ready, -1 offline */
    char state[16];
    char media[PR_MAXM][48]; int nmedia;
    int can_color, can_borderless;
    /* the choices */
    int mi, orient, fill, mono, quality, copies, borderless;
    pid_t list_pid, caps_pid;
    int searching;
    char addr[64]; int addr_focus;
    char msg[96];
    uint8_t *tp; int tw, th;          /* a small copy of the picture, for the preview (RGBA) */
} pr;
static pid_t prjob_pid; static char prjob_name[96]; static char prjob_printer[64];

static void print_draw(int cx, int cy, int cw, int ch);
static void pr_repaint(void) { repaint_client(print_draw); }
static int pr_alive(pid_t *p) { if (*p > 0 && kill(*p, 0) != 0) *p = 0; return *p > 0; }
/* run ccos-print with its output going to a file */
static pid_t pr_spawn(const char *out, char *const argv[]) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        int f = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { dup2(f, 1); close(f); }
        execv("/usr/bin/ccos-print", argv);
        _exit(127);
    }
    return p;
}
static int pr_add(const char *name, const char *uri, const char *model, int color) {
    for (int i = 0; i < pr.np; i++) if (!strcmp(pr.p[i].uri, uri)) return i;
    if (pr.np >= PR_MAXP) return -1;
    int i = pr.np++;
    snprintf(pr.p[i].name, sizeof pr.p[i].name, "%s", name);
    snprintf(pr.p[i].uri, sizeof pr.p[i].uri, "%s", uri);
    snprintf(pr.p[i].model, sizeof pr.p[i].model, "%s", model);
    pr.p[i].color = color;
    return i;
}
/* "P name|uri|model|color" lines (from ccos-print list, or our saved list) */
static void pr_read_list(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char l[512];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        if (strncmp(l, "P ", 2)) continue;
        char *fld[4] = { l + 2 }; int k = 1;
        for (char *s = l + 2; *s && k < 4; s++) if (*s == '|') { *s = 0; fld[k++] = s + 1; }
        if (k == 4) pr_add(fld[0], fld[1], fld[2], atoi(fld[3]));
    }
    fclose(f);
}
static void pr_save(void) {                    /* the chosen printer first: it's picked next time */
    if (pr.sel < 0 || pr.sel >= pr.np) return;
    FILE *f = fopen(PR_SAVED ".tmp", "w"); if (!f) return;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < pr.np; i++) if ((i == pr.sel) == !pass)
            fprintf(f, "P %s|%s|%s|%d\n", pr.p[i].name, pr.p[i].uri, pr.p[i].model, pr.p[i].color);
    fclose(f); rename(PR_SAVED ".tmp", PR_SAVED);
}
static void pr_ask_caps(void) {
    if (pr.sel < 0) return;
    pr.caps_ok = 0; pr.nmedia = 0; pr.state[0] = 0;
    pr.can_color = 1; pr.can_borderless = 0;          /* until it says otherwise */
    if (pr.caps_pid > 0) kill(pr.caps_pid, SIGKILL);
    char *const argv[] = { "ccos-print", "caps", pr.p[pr.sel].uri, NULL };
    pr.caps_pid = pr_spawn("/run/ccos/print-caps.txt", argv);
}
static void pr_read_caps(void) {
    FILE *f = fopen("/run/ccos/print-caps.txt", "r");
    pr.caps_ok = -1;
    if (!f) return;
    char l[2048], def[48] = "";
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        if (!strncmp(l, "state=", 6)) { snprintf(pr.state, sizeof pr.state, "%s", l + 6); pr.caps_ok = strcmp(l + 6, "offline") ? 1 : -1; }
        else if (!strncmp(l, "color=", 6)) pr.can_color = atoi(l + 6);
        else if (!strncmp(l, "borderless=", 11)) pr.can_borderless = atoi(l + 11);
        else if (!strncmp(l, "media_default=", 14)) snprintf(def, sizeof def, "%s", l + 14);
        else if (!strncmp(l, "media=", 6)) {
            pr.nmedia = 0;
            for (char *t = strtok(l + 6, ","); t && pr.nmedia < PR_MAXM; t = strtok(NULL, ",")) {
                if (strstr(t, "custom") || !strchr(t, 'x')) continue;
                snprintf(pr.media[pr.nmedia++], 48, "%s", t);
            }
        }
    }
    fclose(f);
    pr.mi = 0;
    for (int i = 0; i < pr.nmedia; i++) if (!strcmp(pr.media[i], def)) pr.mi = i;
    if (!pr.can_color) pr.mono = 1;
    if (!pr.can_borderless) pr.borderless = 0;
}
/* paper names people know */
static void pr_media_name(const char *m, char *out, size_t n) {
    static const struct { const char *k, *v; } names[] = {
        { "iso_a4_", "A4" }, { "iso_a5_", "A5" }, { "iso_a6_", "A6" }, { "iso_a3_", "A3" }, { "na_letter_", "Letter" },
        { "na_legal_", "Legal" }, { "na_index-4x6_", "Photo 10 x 15 cm" }, { "oe_photo-l_", "Photo 9 x 13 cm" },
        { "na_5x7_", "Photo 13 x 18 cm" }, { "jpn_hagaki_", "Postcard (Hagaki)" }, { "iso_c6_", "Envelope C6" },
        { "iso_dl_", "Envelope DL" }, { "om_small-photo_", "Photo 10 x 15 cm" } };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!strncmp(m, names[i].k, strlen(names[i].k))) { snprintf(out, n, "%s", names[i].v); return; }
    const char *u = strrchr(m, '_'); snprintf(out, n, "%s", u ? u + 1 : m);
}
static int pr_media_mm(const char *m, double *w, double *h) {
    const char *u = strrchr(m, '_'); double a, b; char unit[4] = "";
    if (!u || sscanf(u + 1, "%lfx%lf%3s", &a, &b, unit) < 3) { *w = 210; *h = 297; return 0; }
    double k = !strcmp(unit, "in") ? 25.4 : 1;
    *w = fmin(a, b) * k; *h = fmax(a, b) * k; return 1;
}

static void pr_thumb(const char *file) {
    free(pr.tp); pr.tp = NULL; pr.tw = pr.th = 0;
    int w, h, n;
    uint8_t *px = stbi_load(file, &w, &h, &n, 4);
    if (!px) {                                         /* SVG drawings */
        NSVGimage *sv = nsvgParseFromFile(file, "px", 96);
        if (!sv || sv->width <= 0 || sv->height <= 0) { if (sv) nsvgDelete(sv); return; }
        float sc = 400.0f / fmaxf(sv->width, sv->height);
        w = imax(1, (int)(sv->width * sc)); h = imax(1, (int)(sv->height * sc));
        px = calloc((size_t)w * h, 4);
        NSVGrasterizer *r = nsvgCreateRasterizer();
        if (px && r) nsvgRasterize(r, sv, 0, 0, sc, px, w, h, w * 4);
        if (r) nsvgDeleteRasterizer(r);
        nsvgDelete(sv);
        if (!px) return;
        pr.tp = px; pr.tw = w; pr.th = h; return;
    }
    int s = imax(1, (int)ceil(fmax(w, h) / 400.0));    /* about 400 pixels is plenty */
    pr.tw = imax(1, w / s); pr.th = imax(1, h / s);
    pr.tp = malloc((size_t)pr.tw * pr.th * 4);
    if (pr.tp) for (int y = 0; y < pr.th; y++) for (int x = 0; x < pr.tw; x++)
        memcpy(pr.tp + ((size_t)y * pr.tw + x) * 4, px + ((size_t)(y * s) * w + x * s) * 4, 4);
    if (!pr.tp) pr.tw = pr.th = 0;
    stbi_image_free(px);
}
static void print_open(const char *file) {
    if (!file || !*file) return;
    snprintf(pr.file, sizeof pr.file, "%s", file);
    pr_thumb(file);
    if (win_of(print_draw) < 0) {                 /* fresh: start from what we remember */
        pr.np = 0; pr.sel = -1; pr.copies = 1; pr.orient = 0; pr.fill = 0; pr.quality = 1; pr.borderless = 0; pr.mono = 0;
        pr.can_color = 1; pr.can_borderless = 0; pr.caps_ok = 0;
        pr.msg[0] = 0; pr.addr[0] = 0; pr.addr_focus = 0;
        pr_read_list(PR_SAVED);
        if (pr.np) { pr.sel = 0; pr_ask_caps(); }
    }
    mkdir("/run/ccos", 0755);
    if (!pr_alive(&pr.list_pid)) {                /* look for printers on the network */
        char *const argv[] = { "ccos-print", "list", NULL };
        pr.list_pid = pr_spawn("/run/ccos/print-list.txt", argv);
        pr.searching = 1;
    }
    run("@print");
    pr_repaint();
}
static void print_tick(void) {
    int ch = 0;
    if (pr.searching && !pr_alive(&pr.list_pid)) {
        pr.searching = 0; int before = pr.np;
        pr_read_list("/run/ccos/print-list.txt");
        if (pr.sel < 0 && pr.np) { pr.sel = 0; pr_ask_caps(); }
        ch = 1; (void)before;
    }
    if (pr.sel >= 0 && pr.caps_ok == 0 && !pr_alive(&pr.caps_pid)) { pr_read_caps(); ch = 1; }
    if (ch) pr_repaint();
}
/* the print job runs after the window is closed; a toast says how it went */
static void print_job_poll(void) {
    if (!prjob_pid || pr_alive(&prjob_pid)) return;
    char last[200] = "";
    FILE *f = fopen("/run/ccos/print-job.txt", "r");
    if (f) { char l[200]; while (fgets(l, sizeof l, f)) { l[strcspn(l, "\n")] = 0; if (l[0]) snprintf(last, sizeof last, "%s", l); } fclose(f); }
    char b[200];
    for (int k = ntoast - 1; k >= 0; k--)                 /* "Printing..." is done: replace it */
        if (toasts[k].icon == ICON_PRINTER) { memmove(toasts + k, toasts + k + 1, sizeof toasts[0] * (size_t)(ntoast - k - 1)); ntoast--; }
    repaint_toasts();
    if (!strcmp(last, "OK")) { snprintf(b, sizeof b, "\"%s\" was sent to %s.", prjob_name, prjob_printer); toast(ICON_PRINTER, "Printed", b); }
    else toast(ICON_PRINTER, "Couldn't print", !strncmp(last, "ERR ", 4) ? last + 4 : "The printer didn't answer.");
}

/* ---- the window ---- */
#define PR_X 300                     /* the right-hand column */
#define PR_LIST_Y 42
#define PR_ROW 30
#define PR_ADDR_Y 190
#define PR_OPT_Y 246
#define PR_OPT_H 34
#define PR_NOPT 7
#define PR_BTN_Y (PR_H - 46)
static const char *pr_opt_label[PR_NOPT] = { "Paper", "Orientation", "Picture", "Color", "Quality", "Copies", "Borders" };
static void pr_opt_value(int i, char *out, size_t n) {
    static const char *orients[] = { "Automatic", "Portrait", "Landscape" }, *quals[] = { "Draft", "Normal", "High" };
    switch (i) {
    case 0: if (pr.nmedia) pr_media_name(pr.media[pr.mi], out, n); else snprintf(out, n, "%s", pr.caps_ok == 0 && pr.sel >= 0 ? "..." : "A4"); break;
    case 1: snprintf(out, n, "%s", orients[pr.orient]); break;
    case 2: snprintf(out, n, "%s", pr.fill ? "Fill the page" : "Whole picture"); break;
    case 3: snprintf(out, n, "%s", pr.mono || !pr.can_color ? "Black & white" : "Color"); break;
    case 4: snprintf(out, n, "%s", quals[pr.quality]); break;
    case 5: snprintf(out, n, "%d", pr.copies); break;
    case 6: snprintf(out, n, "%s", pr.borderless ? "Borderless" : pr.can_borderless || pr.caps_ok <= 0 ? "White borders" : "White borders (only)"); break;
    }
}
static void pr_spinner(int x, int y, int w, const char *val, uint32_t text) {
    st_choice(x, y, w, 28, "", 0);
    draw_text(x + 10, y + 6, "<", RGB(0x50, 0x60, 0x74), 255);
    draw_text(x + w - 18, y + 6, ">", RGB(0x50, 0x60, 0x74), 255);
    char v[40]; snprintf(v, sizeof v, "%.24s", val);
    draw_text(x + (w - text_w(v)) / 2, y + 6, v, text, 255);
}
/* the paper with the picture on it, like it will come out */
static void pr_preview(int x0, int y0, int bw, int bh) {
    double wmm, hmm; pr_media_mm(pr.nmedia ? pr.media[pr.mi] : "iso_a4_210x297mm", &wmm, &hmm);
    double s = fmin((bw - 20) / wmm, (bh - 20) / hmm);
    int pw = (int)(wmm * s), ph_ = (int)(hmm * s), px = x0 + (bw - pw) / 2, py = y0 + (bh - ph_) / 2;
    fill_rect(px + 3, py + 4, pw, ph_, RGB(0, 0, 0), 40);
    fill_rect(px, py, pw, ph_, RGB(255, 255, 255), 255);
    stroke_round(px - 1, py - 1, pw + 2, ph_ + 2, 0, RGB(0xb0, 0xb8, 0xc4), 255);
    if (!pr.tp || !pr.tw) return;
    int rot = pr.orient == 2 ? 1 : pr.orient == 1 ? 0 : pr.tw > pr.th;
    double m = (pr.borderless && pr.can_borderless ? 0 : 5) * s;
    double sw = rot ? pr.th : pr.tw, sh = rot ? pr.tw : pr.th, aw = pw - 2 * m, ah = ph_ - 2 * m;
    double k = pr.fill ? fmax(aw / sw, ah / sh) : fmin(aw / sw, ah / sh);
    double w = sw * k, h = sh * k, ix = m + (aw - w) / 2, iy = m + (ah - h) / 2;
    int mono = pr.mono || !pr.can_color;
    dark_keep(px, py, pw, ph_);
    for (int yy = (int)fmax(iy, m); yy < (int)fmin(iy + h, ph_ - m); yy++)
        for (int xx = (int)fmax(ix, m); xx < (int)fmin(ix + w, pw - m); xx++) {
            double u = (xx + 0.5 - ix) / w, v = (yy + 0.5 - iy) / h;
            int sx, sy;
            if (rot) { sx = (int)(v * pr.tw); sy = (int)((1 - u) * pr.th); } else { sx = (int)(u * pr.tw); sy = (int)(v * pr.th); }
            sx = clampi(sx, 0, pr.tw - 1); sy = clampi(sy, 0, pr.th - 1);
            const uint8_t *c = pr.tp + ((size_t)sy * pr.tw + sx) * 4;
            int a = c[3], r = (c[0] * a + 255 * (255 - a)) / 255, g = (c[1] * a + 255 * (255 - a)) / 255, b = (c[2] * a + 255 * (255 - a)) / 255;
            if (mono) r = g = b = (r * 299 + g * 587 + b * 114) / 1000;
            blend(px + xx, py + yy, RGB(r, g, b), 255);
        }
}
static void print_draw(int cx, int cy, int cw, int ch) {
    uint32_t text = RGB(0x20, 0x20, 0x20), grey = RGB(0x70, 0x78, 0x84);
    fill_rect(cx, cy, cw, ch, RGB(0xf4, 0xf6, 0xf9), 255);
    /* left: the preview */
    fill_round(cx + 14, cy + 14, 270, ch - 76, 6, RGB(0xe2, 0xe7, 0xee), 255);
    pr_preview(cx + 14, cy + 14, 270, ch - 76);
    char nm[60]; mf_shown(mf_base(pr.file), nm, sizeof nm);
    char nm2[40]; snprintf(nm2, sizeof nm2, "%.33s", nm);
    draw_text(cx + 16, cy + ch - 54, nm2, grey, 255);
    /* right: printer */
    st_heading(cx + PR_X - 24, cy + 16, "Printer");
    if (pr.searching) draw_text(cx + PR_X + 200, cy + 16, "Searching...", grey, 255);
    if (!pr.np)
        draw_text(cx + PR_X, cy + PR_LIST_Y + 8, pr.searching ? "Looking for printers on your Wi-Fi..." : "No printers found on this network.", grey, 255);
    for (int i = 0; i < pr.np && i < 4; i++) {
        int y = cy + PR_LIST_Y + i * PR_ROW;
        if (i == pr.sel) { fill_round(cx + PR_X - 6, y, cw - PR_X - 10, PR_ROW - 2, 4, RGB(0xcc, 0xe8, 0xff), 255);
                           stroke_round(cx + PR_X - 6, y, cw - PR_X - 10, PR_ROW - 2, 4, RGB(0x84, 0xac, 0xdd), 255); }
        uint32_t dot = i != pr.sel ? RGB(0xa8, 0xb0, 0xb8) : pr.caps_ok > 0 ? RGB(0x2c, 0xb0, 0x40) : pr.caps_ok < 0 ? RGB(0xd8, 0x40, 0x30) : RGB(0xe0, 0xa8, 0x20);
        fill_circle(cx + PR_X + 6, y + PR_ROW / 2 - 1, 4.5f, dot, 255);
        char n[40]; snprintf(n, sizeof n, "%.36s", pr.p[i].name);
        draw_text(cx + PR_X + 20, y + 6, n, text, 255);
    }
    {   /* how the chosen printer is doing */
        const char *s = pr.sel < 0 ? "" : pr.caps_ok == 0 ? "Connecting..." : pr.caps_ok < 0 ? "Not answering: is it on and on this Wi-Fi?" :
                        !strcmp(pr.state, "printing") ? "Ready (busy printing)" : !strcmp(pr.state, "stopped") ? "Stopped: check paper and ink" : "Ready";
        draw_text(cx + PR_X, cy + PR_LIST_Y + 4 * PR_ROW + 4, s, pr.caps_ok < 0 ? RGB(0xc0, 0x30, 0x20) : grey, 255);
    }
    /* add one by its address */
    draw_text(cx + PR_X, cy + PR_ADDR_Y + 6, "Not listed? Address:", grey, 255);
    int fx = cx + PR_X + 168, fw = cw - PR_X - 168 - 92;
    fill_round(fx, cy + PR_ADDR_Y, fw, 28, 3, RGB(255, 255, 255), 255);
    stroke_round(fx, cy + PR_ADDR_Y, fw, 28, 3, pr.addr_focus ? RGB(0x33, 0x99, 0xff) : RGB(0xa8, 0xb4, 0xc4), 255);
    char a[40]; snprintf(a, sizeof a, "%s%s", pr.addr, pr.addr_focus ? "_" : "");
    draw_text(fx + 6, cy + PR_ADDR_Y + 6, pr.addr[0] || pr.addr_focus ? a : "192.168.1.20", pr.addr[0] || pr.addr_focus ? text : RGB(0xb0, 0xb8, 0xc0), 255);
    st_choice(cx + cw - 84, cy + PR_ADDR_Y, 70, 28, "Add", 0);
    fill_rect(cx + PR_X - 6, cy + PR_OPT_Y - 14, cw - PR_X - 10, 1, RGB(0xd8, 0xe0, 0xe8), 255);
    /* the choices */
    for (int i = 0; i < PR_NOPT; i++) {
        int y = cy + PR_OPT_Y + i * PR_OPT_H;
        draw_text(cx + PR_X, y + 6, pr_opt_label[i], text, 255);
        char v[48]; pr_opt_value(i, v, sizeof v);
        pr_spinner(cx + PR_X + 120, y, cw - PR_X - 134, v, text);
    }
    /* message, Print, Cancel */
    if (pr.msg[0]) draw_text(cx + 16, cy + PR_BTN_Y + 7, pr.msg, RGB(0xc0, 0x30, 0x20), 255);
    int ok = pr.sel >= 0 && pr.caps_ok > 0;
    st_choice(cx + cw - 14 - 110 - 8 - 110, cy + PR_BTN_Y, 110, 30, "Cancel", 0);
    st_choice(cx + cw - 14 - 110, cy + PR_BTN_Y, 110, 30, "Print", ok);
}
static void pr_close(void) { int i = win_of(print_draw); if (i >= 0) close_win(i); }
static void pr_go(void) {
    if (pr.sel < 0 || pr.caps_ok <= 0) { snprintf(pr.msg, sizeof pr.msg, "Pick a printer that's ready first."); pr_repaint(); return; }
    if (pr_alive(&prjob_pid)) { snprintf(pr.msg, sizeof pr.msg, "Wait: still sending the last picture."); pr_repaint(); return; }
    static const char *orients[] = { "auto", "portrait", "landscape" }, *quals[] = { "draft", "normal", "high" };
    char copies[8]; snprintf(copies, sizeof copies, "%d", pr.copies);
    char *argv[24]; int n = 0;
    static char file[300], uri[160], media[48];
    snprintf(file, sizeof file, "%s", pr.file); snprintf(uri, sizeof uri, "%s", pr.p[pr.sel].uri);
    snprintf(media, sizeof media, "%s", pr.nmedia ? pr.media[pr.mi] : "");
    argv[n++] = "ccos-print"; argv[n++] = "image"; argv[n++] = file; argv[n++] = uri;
    if (media[0]) { argv[n++] = "--media"; argv[n++] = media; }
    argv[n++] = "--orient"; argv[n++] = (char *)orients[pr.orient];
    argv[n++] = "--fit"; argv[n++] = pr.fill ? "fill" : "fit";
    if (pr.mono || !pr.can_color) argv[n++] = "--mono";
    argv[n++] = "--quality"; argv[n++] = (char *)quals[pr.quality];
    argv[n++] = "--copies"; argv[n++] = copies;
    if (pr.borderless && pr.can_borderless) argv[n++] = "--borderless";
    argv[n] = NULL;
    prjob_pid = pr_spawn("/run/ccos/print-job.txt", argv);
    mf_shown(mf_base(pr.file), prjob_name, sizeof prjob_name);
    snprintf(prjob_printer, sizeof prjob_printer, "%s", pr.p[pr.sel].name);
    pr_save();
    char b[200]; snprintf(b, sizeof b, "Sending \"%s\" to %s...", prjob_name, prjob_printer);
    toast(ICON_PRINTER, "Printing", b);
    pr_close();
}
static void pr_add_address(void) {
    if (!pr.addr[0]) { pr.addr_focus = 1; pr_repaint(); return; }
    char uri[160], name[64];
    if (strstr(pr.addr, "://")) snprintf(uri, sizeof uri, "%s", pr.addr);
    else snprintf(uri, sizeof uri, "ipp://%s%s/ipp/print", pr.addr, strchr(pr.addr, ':') ? "" : ":631");
    snprintf(name, sizeof name, "Printer at %s", pr.addr);
    int i = pr_add(name, uri, "", 1);
    if (i >= 0) { pr.sel = i; pr_ask_caps(); }
    pr.addr[0] = 0; pr.addr_focus = 0;
    pr_repaint();
}
static void print_click(int lx, int ly, int cw, int ch) {
    (void)ch;
    pr.msg[0] = 0;
    pr.addr_focus = 0;
    for (int i = 0; i < pr.np && i < 4; i++)                       /* a printer */
        if (lx >= PR_X - 6 && ly >= PR_LIST_Y + i * PR_ROW && ly < PR_LIST_Y + (i + 1) * PR_ROW) {
            if (i != pr.sel) { pr.sel = i; pr_ask_caps(); }
            pr_repaint(); return;
        }
    if (ly >= PR_ADDR_Y && ly < PR_ADDR_Y + 28) {
        if (lx >= cw - 84 && lx < cw - 14) { pr_add_address(); return; }
        if (lx >= PR_X + 168 && lx < cw - 92) { pr.addr_focus = 1; pr_repaint(); return; }
    }
    for (int i = 0; i < PR_NOPT; i++) {                            /* < value > */
        int y = PR_OPT_Y + i * PR_OPT_H, x = PR_X + 120, w = cw - PR_X - 134;
        if (ly < y || ly >= y + 28 || lx < x || lx >= x + w) continue;
        int d = lx < x + w / 2 ? -1 : 1;
        switch (i) {
        case 0: if (pr.nmedia) pr.mi = (pr.mi + d + pr.nmedia) % pr.nmedia; break;
        case 1: pr.orient = (pr.orient + d + 3) % 3; break;
        case 2: pr.fill = !pr.fill; break;
        case 3: if (pr.can_color) pr.mono = !pr.mono; break;
        case 4: pr.quality = clampi(pr.quality + d, 0, 2); break;
        case 5: pr.copies = clampi(pr.copies + d, 1, 99); break;
        case 6: if (pr.can_borderless) pr.borderless = !pr.borderless; break;
        }
        pr_repaint(); return;
    }
    if (ly >= PR_BTN_Y && ly < PR_BTN_Y + 30) {
        if (lx >= cw - 14 - 110) { pr_go(); return; }
        if (lx >= cw - 14 - 228 && lx < cw - 14 - 118) { pr_close(); return; }
    }
    pr_repaint();
}
static void print_key(int code, int mods) {
    if (code == KEY_ESC) { pr_close(); return; }
    if (pr.addr_focus) {
        if (code == KEY_ENTER || code == KEY_KPENTER) { pr_add_address(); return; }
        size_t l = strlen(pr.addr);
        if (code == KEY_BACKSPACE) { if (l) pr.addr[l - 1] = 0; pr_repaint(); return; }
        char c = key_char(code, mods);
        if (c > 32 && c < 127 && l + 1 < sizeof pr.addr) { pr.addr[l] = c; pr.addr[l + 1] = 0; pr_repaint(); }
        return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) pr_go();
}
static int print_on_close(void) { pr.addr_focus = 0; free(pr.tp); pr.tp = NULL; pr.tw = pr.th = 0; return 1; }
