/* ===================================================================
 *  Python: write Python code and run it.
 *  Top: the code (colours, line numbers, auto-indent, Tab = 4 spaces,
 *  Ctrl+Z undo, Ctrl+/ comment). Bottom: what the program prints, and a
 *  box to answer input(). F5 runs, the program's errors point at the line.
 *  Files live in D:\Documents\Python. Programs that open windows (turtle,
 *  tkinter) open them on the desktop like any app.
 * =================================================================== */
#define PY_MAX (256 * 1024)
#define PY_DIR "/data/Documents/Python"
#define PY_BAR 44
#define PY_LH 18                                 /* line height */
#define PY_GUT 52                                /* line numbers */
#define PY_OUTMAX 2000                           /* lines of output kept */
#define PY_UNDO 60
static struct {
    char buf[PY_MAX + 1]; int len, caret, anchor, top, left, goal, modified;
    int *ls, nl;                                 /* where every line starts */
    unsigned char *st;                           /* each line starts inside a ''' / """ string: 1 / 2 */
    char file[64];                               /* "" = not saved yet */
    char *undo[PY_UNDO]; int ucaret[PY_UNDO], nundo, uredo; long long last_edit; int last_kind;
    int dragging; long long last_click; int last_pos;
    /* output */
    struct { char *s; unsigned char kind; } out[PY_OUTMAX]; int nout, oscroll;   /* kind: 0 out, 1 err, 2 info, 3 input */
    char partial[2][1024]; int plen[2];
    pid_t pid; int fo, fe, fi; long long t0;     /* the running program: its stdout, stderr, stdin */
    char in[200]; int focus_out;                 /* typing an answer for input() */
    int err_line;                                /* the line the last error was on (0 = none) */
    char msg[96];
    int had_err, stopped;                        /* (the exit code may already be gone: the desktop collects finished programs) */
    int split;                                   /* where code ends and output starts (y in the window) */
} py = { .fo = -1, .fe = -1, .fi = -1, .last_pos = -1 };
static Dialog py_dlg; static int py_dlg_mode, py_pending, py_closing;
enum { PYD_SAVEAS = 1, PYD_OPEN, PYD_PROMPT, PYD_INFO, PYD_EXAMPLE };
static void pyapp_draw(int cx, int cy, int cw, int ch);

/* ---- the text ---- */
static void py_lines(void) {
    int n = 1; for (int i = 0; i < py.len; i++) if (py.buf[i] == '\n') n++;
    free(py.ls); free(py.st);
    py.ls = malloc(sizeof(int) * (size_t)(n + 1)); py.st = calloc((size_t)n + 1, 1);
    if (!py.ls || !py.st) { py.nl = 0; return; }
    py.nl = 0; py.ls[py.nl++] = 0;
    for (int i = 0; i < py.len; i++) if (py.buf[i] == '\n') py.ls[py.nl++] = i + 1;
    py.ls[py.nl] = py.len + 1;
    /* which lines start inside a triple-quoted string (for the colours) */
    int in = 0;
    for (int l = 0; l < py.nl; l++) {
        py.st[l] = (unsigned char)in;
        int e = py.ls[l + 1] - 1;
        for (int i = py.ls[l]; i < e; i++) {
            char c = py.buf[i];
            if (in) { if (i + 2 < e + 1 && c == (in == 1 ? '\'' : '"') && py.buf[i + 1] == c && py.buf[i + 2] == c) { in = 0; i += 2; } continue; }
            if (c == '#') break;
            if (c == '\'' || c == '"') {
                if (i + 2 < e && py.buf[i + 1] == c && py.buf[i + 2] == c) { in = c == '\'' ? 1 : 2; i += 2; continue; }
                for (i++; i < e && py.buf[i] != c; i++) if (py.buf[i] == '\\') i++;
            }
        }
    }
}
static int py_line_of(int pos) { int lo = 0, hi = py.nl - 1; while (lo < hi) { int m = (lo + hi + 1) / 2; if (py.ls[m] <= pos) lo = m; else hi = m - 1; } return lo; }
static int py_line_end(int l) { return py.ls[l + 1] - 1; }
static int py_col(int pos) { int l = py_line_of(pos); return u8_cols(py.buf, py.ls[l], pos); }
static int py_pos_at_col(int l, int col) { int p = py.ls[l], e = py_line_end(l); while (p < e && col > 0) { p = u8_next(py.buf, e, p); col--; } return p; }
static int py_sel(int *a, int *b) {
    if (py.anchor < 0 || py.anchor == py.caret) return 0;
    *a = imin(py.anchor, py.caret); *b = imax(py.anchor, py.caret); return 1;
}
static void py_title(void) {
    static char t[96];
    snprintf(t, sizeof t, "%s%s - Python", py.modified ? "*" : "", py.file[0] ? py.file : "untitled.py");
    int w = win_of(pyapp_draw); if (w >= 0 && wins[w].title != t) { wins[w].title = t; repaint_window(w); }
    else if (w >= 0) repaint_window(w);
}
/* undo: a copy of the whole text before each group of changes */
static void py_snapshot(int kind) {
    long long now = ui_ms();
    if (kind && kind == py.last_kind && now - py.last_edit < 1200) { py.last_edit = now; return; }   /* (typing: one undo step) */
    py.last_kind = kind; py.last_edit = now;
    if (py.nundo == PY_UNDO) { free(py.undo[0]); memmove(py.undo, py.undo + 1, sizeof py.undo[0] * (PY_UNDO - 1)); memmove(py.ucaret, py.ucaret + 1, sizeof py.ucaret[0] * (PY_UNDO - 1)); py.nundo--; }
    for (int i = py.nundo; i < PY_UNDO; i++) { free(py.undo[i]); py.undo[i] = NULL; }
    py.undo[py.nundo] = strndup(py.buf, (size_t)py.len); py.ucaret[py.nundo] = py.caret; py.nundo++; py.uredo = py.nundo;
}
static void py_changed(void) { py.modified = 1; py.err_line = 0; py_lines(); py_title(); }
static void py_set_text(const char *s, int n) {
    n = imin(n, PY_MAX); memcpy(py.buf, s, (size_t)n); py.buf[n] = 0; py.len = n;
    py.caret = 0; py.anchor = -1; py.top = 0; py.left = 0; py.err_line = 0;
    for (int i = 0; i < PY_UNDO; i++) { free(py.undo[i]); py.undo[i] = NULL; }
    py.nundo = py.uredo = 0; py.last_kind = 0;
    py_lines();
}
static void py_delete_range(int a, int b) { memmove(py.buf + a, py.buf + b, (size_t)(py.len - b) + 1); py.len -= b - a; }
static void py_insert_raw(int at, const char *s, int n) {
    if (py.len + n > PY_MAX) n = PY_MAX - py.len;
    if (n <= 0) return;
    memmove(py.buf + at + n, py.buf + at, (size_t)(py.len - at) + 1);
    memcpy(py.buf + at, s, (size_t)n); py.len += n;
}
static void py_insert(const char *s, int n, int kind) {    /* replaces the selection */
    py_snapshot(kind);
    int a, b; if (py_sel(&a, &b)) { py_delete_range(a, b); py.caret = a; }
    py.anchor = -1;
    py_insert_raw(py.caret, s, n); py.caret += imin(n, PY_MAX);
    py.caret = imin(py.caret, py.len);
    py_changed();
}

/* ---- running ---- */
static void py_out_add(const char *s, int n, int kind) {
    if (py.nout == PY_OUTMAX) { free(py.out[0].s); memmove(py.out, py.out + 1, sizeof py.out[0] * (PY_OUTMAX - 1)); py.nout--; }
    py.out[py.nout].s = strndup(s, (size_t)n); py.out[py.nout].kind = (unsigned char)kind; py.nout++;
    py.oscroll = 0;                                /* (0 = following the end) */
}
static void py_out_clear(void) { for (int i = 0; i < py.nout; i++) free(py.out[i].s); py.nout = 0; py.oscroll = 0; py.plen[0] = py.plen[1] = 0; }
static void py_feed(int which, const char *s, int n) {     /* split into lines */
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (c == '\n' || py.plen[which] >= (int)sizeof py.partial[0] - 1) {
            py_out_add(py.partial[which], py.plen[which], which); py.plen[which] = 0;
            if (c == '\n') continue;
        }
        py.partial[which][py.plen[which]++] = c;
    }
}
static void py_flush_partial(void) {
    for (int k = 0; k < 2; k++) if (py.plen[k]) { py_out_add(py.partial[k], py.plen[k], k); py.plen[k] = 0; }
}
static int py_running(void) { return py.pid > 0; }
static void py_finished(int status) {
    py_flush_partial();
    if (py.fo >= 0) close(py.fo);
    if (py.fe >= 0) close(py.fe);
    if (py.fi >= 0) close(py.fi);
    py.fo = py.fe = py.fi = -1; py.pid = 0;
    /* an error? find the last "line N" in our file */
    for (int i = py.nout - 1; i >= 0 && i >= py.nout - 60; i--) {
        if (py.out[i].kind != 1) continue;
        const char *f = strstr(py.out[i].s, "File \""), *l = strstr(py.out[i].s, "\", line ");
        if (f && l && (strstr(py.out[i].s, py.file[0] ? py.file : "untitled.py"))) { py.err_line = atoi(l + 8); break; }
    }
    char b[96];
    double s = (ui_ms() - py.t0) / 1000.0;
    int failed = (WIFEXITED(status) && WEXITSTATUS(status)) || (py.had_err && py.err_line);
    if (py.stopped || WIFSIGNALED(status)) snprintf(b, sizeof b, "Stopped after %.1f s.", s);
    else if (failed && py.err_line) snprintf(b, sizeof b, "The program had an error on line %d (it's marked). (%.1f s)", py.err_line, s);
    else if (failed) snprintf(b, sizeof b, "The program ended with an error. (%.1f s)", s);
    else snprintf(b, sizeof b, "Finished in %.1f s.", s);
    py_out_add(b, (int)strlen(b), 2);
    if (py.err_line > 0 && py.err_line <= py.nl) {                 /* show the line */
        int l = py.err_line - 1;
        py.caret = py.ls[l]; py.anchor = -1;
        int w = win_of(pyapp_draw);
        if (w >= 0) { int cx, cy, cw, ch; client_rect(&wins[w], &cx, &cy, &cw, &ch); int rows = (py.split - PY_BAR - 8) / PY_LH; if (l < py.top || l >= py.top + rows) py.top = imax(0, l - rows / 2); }
    }
    py.focus_out = 0;
}
static int py_fd(void) { return py.fo >= 0 ? py.fo : py.fe; }
static void py_reap(void) {
    if (py.pid <= 0) return;
    int st; pid_t r = waitpid(py.pid, &st, WNOHANG);
    if (r == py.pid) py_finished(st);
}
static void py_wake(void) {                        /* the program printed something */
    char b[4096]; ssize_t n; int any = 0;
    for (int k = 0; k < 2; k++) {
        int *fd = k ? &py.fe : &py.fo;
        while (*fd >= 0 && (n = read(*fd, b, sizeof b)) > 0) { py_feed(k, b, (int)n); any = 1; if (k) py.had_err = 1; }
        if (*fd >= 0 && n == 0) { close(*fd); *fd = -1; any = 1; }
    }
    if (py.fo < 0 && py.fe < 0) {                  /* both closed: it ended (or soon will) */
        int st; if (waitpid(py.pid, &st, 0) == py.pid) py_finished(st); else py_finished(0);
    }
    if (any) repaint_client(pyapp_draw);
}
static void py_stop(void) { if (py.pid > 0) { py.stopped = 1; kill(-py.pid, SIGKILL); kill(py.pid, SIGKILL); } }
static int py_write_file(const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return 0;
    size_t w = fwrite(py.buf, 1, (size_t)py.len, f);
    if (py.len && py.buf[py.len - 1] != '\n') fputc('\n', f);
    return fclose(f) == 0 && w == (size_t)py.len;
}
static void py_run(void) {
    if (py_running()) { py_stop(); return; }
    py_out_clear();
    /* the program runs from a copy in memory (so running never waits for the USB stick),
       in its own folder (so it finds files next to it) */
    char dir[300], path[400];
    snprintf(dir, sizeof dir, "/tmp/ccos-python");
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s", dir, py.file[0] ? py.file : "untitled.py");
    if (!py_write_file(path)) { py_out_add("Couldn't save the program to run it.", 36, 1); repaint_client(pyapp_draw); return; }
    int o[2], e[2], i[2];
    if (pipe(o) < 0) return;
    if (pipe(e) < 0) { close(o[0]); close(o[1]); return; }
    if (pipe(i) < 0) { close(o[0]); close(o[1]); close(e[0]); close(e[1]); return; }
    const char *cwd = PY_DIR; struct stat sb; if (stat(cwd, &sb) != 0) cwd = "/tmp/ccos-python";
    pid_t p = fork();
    if (p == 0) {
        setsid();
        dup2(i[0], 0); dup2(o[1], 1); dup2(e[1], 2);
        for (int fd = 3; fd < 256; fd++) close(fd);
        if (chdir(cwd) < 0) {}
        setenv("PYTHONUNBUFFERED", "1", 1); setenv("PYTHONIOENCODING", "utf-8", 1); setenv("TERM", "dumb", 1);
        setenv("DISPLAY", ":0", 1); setenv("HOME", "/root", 1);
        struct stat ds; setenv("PYTHONUSERBASE", stat("/data/.python", &ds) == 0 || mkdir("/data/.python", 0755) == 0 ? "/data/.python" : "/root/.local", 1);
        setenv("PYTHONPATH", cwd, 1);
        execl("/usr/bin/python3", "python3", "-u", path, (char *)NULL);
        _exit(127);
    }
    close(o[1]); close(e[1]); close(i[0]);
    if (p < 0) { close(o[0]); close(e[0]); close(i[1]); return; }
    py.pid = p; py.fo = o[0]; py.fe = e[0]; py.fi = i[1];
    fcntl(py.fo, F_SETFL, O_NONBLOCK); fcntl(py.fe, F_SETFL, O_NONBLOCK);
    fcntl(py.fo, F_SETFD, FD_CLOEXEC); fcntl(py.fe, F_SETFD, FD_CLOEXEC); fcntl(py.fi, F_SETFD, FD_CLOEXEC);
    py.t0 = ui_ms(); py.err_line = 0; py.in[0] = 0; py.had_err = 0; py.stopped = 0;
    char b[120]; snprintf(b, sizeof b, "Running %s...", py.file[0] ? py.file : "untitled.py");
    py_out_add(b, (int)strlen(b), 2);
    repaint_client(pyapp_draw);
}
static void py_send_input(void) {
    if (py.fi < 0) return;
    size_t n = strlen(py.in);
    if (write(py.fi, py.in, n) < 0 || write(py.fi, "\n", 1) < 0) {}
    py_flush_partial();                            /* the question ("What's your name? ") then the answer */
    if (py.nout && py.out[py.nout - 1].kind == 0) {
        char *j = malloc(strlen(py.out[py.nout - 1].s) + n + 1);
        if (j) { sprintf(j, "%s%s", py.out[py.nout - 1].s, py.in); free(py.out[py.nout - 1].s); py.out[py.nout - 1].s = j; py.out[py.nout - 1].kind = 3; }
    } else py_out_add(py.in, (int)n, 3);
    py.in[0] = 0;
}

/* ---- files ---- */
static void py_dir(void) { mkdir("/data/Documents", 0755); mkdir(PY_DIR, 0755); }
static int py_save_to(const char *name) {
    py_dir();
    char p[300]; snprintf(p, sizeof p, PY_DIR "/%s", name);
    char *mb = NULL; size_t ml = 0; FILE *f = open_memstream(&mb, &ml); if (!f) return 0;
    fwrite(py.buf, 1, (size_t)py.len, f); if (py.len && py.buf[py.len - 1] != '\n') fputc('\n', f);
    fclose(f);
    if (conf_persistent) aw_save(p, mb, ml, 1, name);
    else { FILE *g = fopen(p, "w"); if (g) { fwrite(mb, 1, ml, g); fclose(g); } }
    free(mb);
    snprintf(py.file, sizeof py.file, "%s", name);
    py.modified = 0; py_title();
    return 1;
}
static void py_load(const char *name) {
    char p[300]; snprintf(p, sizeof p, PY_DIR "/%s", name);
    FILE *f = fopen(p, "r");
    if (!f) { snprintf(py.msg, sizeof py.msg, "Couldn't open %s", name); return; }
    char *t = malloc(PY_MAX + 1); size_t n = t ? fread(t, 1, PY_MAX, f) : 0; fclose(f);
    if (!t) return;
    int k = 0; for (size_t i = 0; i < n; i++) if (t[i] != '\r') t[k++] = t[i];     /* (Windows line ends) */
    py_set_text(t, k); free(t);
    snprintf(py.file, sizeof py.file, "%s", name); py.modified = 0; py_title();
}
void py_dlg_done(int b);
static void py_saveas(void) {
    py_dlg_mode = PYD_SAVEAS;
    dlg_open(&py_dlg, DLG_INPUT, "Save As", "Name (saved in D:\\Documents\\Python):", NULL, "Save", "Cancel", NULL, py_dlg_done);
    snprintf(py_dlg.field, sizeof py_dlg.field, "%s", py.file[0] ? py.file : "my_program.py");
    repaint_client(pyapp_draw);
}
static void py_save(void) { if (py.file[0]) py_save_to(py.file); else py_saveas(); }
static void py_open_dlg(void) {
    py_dlg_mode = PYD_OPEN;
    dlg_open(&py_dlg, DLG_LIST, "Open", "Your Python programs:", NULL, "Open", "Cancel", NULL, py_dlg_done);
    DIR *d = opendir(PY_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && py_dlg.nitems < DLG_MAXITEMS) {
            const char *x = strrchr(e->d_name, '.');
            if (e->d_name[0] != '.' && x && !strcmp(x, ".py")) snprintf(py_dlg.items[py_dlg.nitems++], sizeof py_dlg.items[0], "%s", e->d_name);
        }
        closedir(d);
    }
    qsort(py_dlg.items, py_dlg.nitems, sizeof py_dlg.items[0], cmp_str);
    if (py_dlg.nitems) py_dlg.sel = 0;
    else snprintf(py_dlg.msg[1], sizeof py_dlg.msg[1], "(none yet - save one first)");
    repaint_client(pyapp_draw);
}
static const struct { const char *name, *code; } py_examples[] = {
    { "Hello", "# Your first program: press F5 (or Run) to run it.\nname = input(\"What's your name? \")\nprint(\"Hello,\", name + \"!\")\nprint(\"Your name has\", len(name), \"letters.\")\n" },
    { "Guess the number", "import random\n\nsecret = random.randint(1, 100)\nprint(\"I'm thinking of a number from 1 to 100.\")\ntries = 0\nwhile True:\n    guess = int(input(\"Your guess: \"))\n    tries += 1\n    if guess < secret:\n        print(\"Higher!\")\n    elif guess > secret:\n        print(\"Lower!\")\n    else:\n        print(\"Yes! It was\", secret, \"- you got it in\", tries, \"tries.\")\n        break\n" },
    { "Turtle drawing", "# Draws in its own window. Close it when you're done.\nimport turtle\n\nt = turtle.Turtle()\nt.speed(0)\ncolors = [\"red\", \"orange\", \"gold\", \"green\", \"blue\", \"purple\"]\nfor i in range(180):\n    t.pencolor(colors[i % len(colors)])\n    t.forward(i * 2)\n    t.left(59)\n\nturtle.done()\n" },
    { "Times tables", "for a in range(1, 11):\n    row = \"\"\n    for b in range(1, 11):\n        row += str(a * b).rjust(4)\n    print(row)\n" },
    { "A window with a button", "import tkinter as tk\n\nclicks = 0\n\ndef clicked():\n    global clicks\n    clicks += 1\n    label.config(text=f\"Clicked {clicks} times\")\n\nwin = tk.Tk()\nwin.title(\"My app\")\nlabel = tk.Label(win, text=\"Hello from Python!\", font=(\"DejaVu Sans\", 16))\nlabel.pack(padx=30, pady=20)\ntk.Button(win, text=\"Click me\", command=clicked).pack(pady=10)\nwin.mainloop()\n" },
    { "Fibonacci", "def fib(n):\n    a, b = 0, 1\n    for _ in range(n):\n        a, b = b, a + b\n    return a\n\nfor i in range(1, 31):\n    print(i, fib(i))\n" },
};
#define PY_NEX (int)(sizeof py_examples / sizeof py_examples[0])
static void py_examples_dlg(void) {
    py_dlg_mode = PYD_EXAMPLE;
    dlg_open(&py_dlg, DLG_LIST, "Examples", "Open an example program:", NULL, "Open", "Cancel", NULL, py_dlg_done);
    for (int i = 0; i < PY_NEX; i++) snprintf(py_dlg.items[py_dlg.nitems++], sizeof py_dlg.items[0], "%s", py_examples[i].name);
    py_dlg.sel = 0;
    repaint_client(pyapp_draw);
}
static void py_do(int what) {                      /* 1 new, 2 open, 3 example, 9 close */
    if (what == 9) { int w = win_of(pyapp_draw); py_stop(); if (w >= 0) close_win(w); return; }
    if (what == 1) { py_set_text("", 0); py.file[0] = 0; py.modified = 0; py_title(); }
    else if (what == 2) py_open_dlg();
    else if (what == 3) py_examples_dlg();
}
static void py_request(int what) {                 /* unsaved changes? ask first */
    if (!py.modified || !py.len) { py_do(what); return; }
    py_pending = what; py_dlg_mode = PYD_PROMPT;
    dlg_open(&py_dlg, DLG_MSG, "Python", "Save your changes first?", NULL, "Save", "Don't save", "Cancel", py_dlg_done);
    repaint_client(pyapp_draw);
}
void py_dlg_done(int b) {
    if (py_dlg_mode == PYD_SAVEAS && b == 0) {
        char n[48]; snprintf(n, sizeof n, "%s", py_dlg.field);
        if (!n[0]) return;
        if (!strchr(n, '.')) strncat(n, ".py", sizeof n - strlen(n) - 1);
        py_save_to(n);
        if (py_pending) { int w = py_pending; py_pending = 0; py_do(w); }
    } else if (py_dlg_mode == PYD_OPEN && b == 0 && py_dlg.sel >= 0) py_load(py_dlg.items[py_dlg.sel]);
    else if (py_dlg_mode == PYD_EXAMPLE && b == 0 && py_dlg.sel >= 0) {
        int k = py_dlg.sel;
        py_set_text(py_examples[k].code, (int)strlen(py_examples[k].code));
        py.file[0] = 0; py.modified = 0; py_title();
    } else if (py_dlg_mode == PYD_PROMPT) {
        if (b == 0) { if (py.file[0]) { py_save_to(py.file); int w = py_pending; py_pending = 0; py_do(w); } else py_saveas(); }
        else if (b == 1) { int w = py_pending; py_pending = 0; py_do(w); }
        else py_pending = 0;
    }
    repaint_client(pyapp_draw);
}

/* ---- drawing ---- */
static int py_is_kw(const char *s, int n) {
    static const char *kw[] = { "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else",
        "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
        "while", "with", "yield", "match", "case", NULL };
    for (int i = 0; kw[i]; i++) if ((int)strlen(kw[i]) == n && !memcmp(kw[i], s, (size_t)n)) return 1;
    return 0;
}
static int py_is_builtin(const char *s, int n) {
    static const char *bi[] = { "print", "input", "len", "range", "int", "str", "float", "list", "dict", "set", "tuple", "bool", "open", "abs", "min", "max",
        "sum", "round", "sorted", "reversed", "enumerate", "zip", "map", "filter", "type", "isinstance", "chr", "ord", "self", "super", "any", "all", "pow", NULL };
    for (int i = 0; bi[i]; i++) if ((int)strlen(bi[i]) == n && !memcmp(bi[i], s, (size_t)n)) return 1;
    return 0;
}
#define PYC_TEXT RGB(0x1e, 0x22, 0x28)
#define PYC_KW   RGB(0x8a, 0x2b, 0xc2)
#define PYC_STR  RGB(0x1a, 0x80, 0x3a)
#define PYC_COM  RGB(0x80, 0x88, 0x94)
#define PYC_NUM  RGB(0xc0, 0x5a, 0x10)
#define PYC_BI   RGB(0x10, 0x6a, 0xb8)
#define PYC_DEF  RGB(0xb0, 0x60, 0x00)
static void py_span(int x, int y, const char *s, int n, uint32_t c, int *col, int left, int maxcols) {
    /* n bytes, at text column *col (the view starts at column left) */
    const char *p = s, *e = s + n;
    while (p < e) {
        int cl = 1; unsigned char u = (unsigned char)*p;
        if (u >= 0xf0) cl = 4; else if (u >= 0xe0) cl = 3; else if (u >= 0xc0) cl = 2;
        if (*col >= left + maxcols) return;
        if (*col >= left && *p != ' ') { char t[5]; memcpy(t, p, (size_t)cl); t[cl] = 0; draw_text(x + (*col - left) * 8, y, t, c, 255); }
        p += cl; (*col)++;
    }
}
static void py_draw_line(int l, int x, int y, int maxcols) {
    const char *s = py.buf + py.ls[l]; int n = py_line_end(l) - py.ls[l];
    int col = 0, i = 0, in = py.st[l], prev_def = 0;
    while (i < n) {
        int j = i; uint32_t c = PYC_TEXT;
        if (in) {                                   /* inside ''' or """ */
            char q = in == 1 ? '\'' : '"';
            while (j < n && !(s[j] == q && j + 2 < n + 1 && j + 2 <= n - 1 + 1 && s[j + 1] == q && s[j + 2] == q)) j++;
            if (j < n) { j += 3; in = 0; } c = PYC_STR;
        } else if (s[i] == '#') { j = n; c = PYC_COM; }
        else if (s[i] == '\'' || s[i] == '"') {
            char q = s[i];
            if (i + 2 < n && s[i + 1] == q && s[i + 2] == q) {
                j = i + 3; in = q == '\'' ? 1 : 2;
                while (j < n && !(s[j] == q && j + 2 < n && s[j + 1] == q && s[j + 2] == q)) j++;
                if (j < n) { j += 3; in = 0; }
            } else { j = i + 1; while (j < n && s[j] != q) { if (s[j] == '\\') j++; j++; } if (j < n) j++; }
            c = PYC_STR;
        } else if (isdigit((unsigned char)s[i])) { while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++; c = PYC_NUM; }
        else if (isalpha((unsigned char)s[i]) || s[i] == '_' || (unsigned char)s[i] >= 0x80) {
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || (unsigned char)s[j] >= 0x80)) j++;
            if ((s[j < n ? j : n - 1] == '\'' || s[j < n ? j : n - 1] == '"') && j - i <= 2 && strchr("fFrRbBuU", s[i])) { /* f"..." */ c = PYC_STR; int q = j; char qq = s[q]; q++; while (q < n && s[q] != qq) q++; if (q < n) q++; j = q; }
            else if (prev_def) c = PYC_DEF;
            else if (py_is_kw(s + i, j - i)) { c = PYC_KW; prev_def = (j - i == 3 && !memcmp(s + i, "def", 3)) || (j - i == 5 && !memcmp(s + i, "class", 5)); py_span(x, y, s + i, j - i, c, &col, py.left, maxcols); i = j; continue; }
            else if (py_is_builtin(s + i, j - i)) c = PYC_BI;
            prev_def = 0;
        } else if (s[i] == '@' && i == (int)strspn(s, " ")) { while (j < n && s[j] != '(' ) j++; c = PYC_DEF; }
        else j = i + 1;
        if (j <= i) j = i + 1;
        py_span(x, y, s + i, j - i, c, &col, py.left, maxcols);
        if (c != PYC_TEXT || !isspace((unsigned char)s[i])) { if (c != PYC_DEF) prev_def = prev_def && c == PYC_TEXT && isspace((unsigned char)s[i]); }
        i = j;
    }
}
enum { YPB_RUN = 1, YPB_NEW, YPB_OPEN, YPB_SAVE, YPB_EX, YPB_CLEAR, YPB_TERM, YPB_EDIT = 100, YPB_OUT, YPB_IN };
static int py_btn_x[10], py_btn_w[10];
static void py_toolbar_btn(int cx, int cy, int id, int *x, const char *l, int primary, int hv) {
    int w = text_w(l) + 26;
    py_btn_x[id] = *x - cx; py_btn_w[id] = w;
    if (primary) fill_round(*x, cy + 7, w, 30, 15, hv ? mix(TH()->accent, RGB(0, 0, 0), 0.12f) : TH()->accent, 255);
    else if (hv) fill_round(*x, cy + 7, w, 30, 15, RGB(0xe6, 0xec, 0xf4), 255);
    draw_text(*x + 13, cy + 14, l, primary ? RGB(255, 255, 255) : PYC_TEXT, 255);
    *x += w + 6;
}
static void pyapp_draw(int cx, int cy, int cw, int ch) {
    int hv = hot.type == HOT_CONTENT && wins[hot.a].draw == pyapp_draw ? hot.b : -1;
    py.split = imax(PY_BAR + 120, ch * 62 / 100);
    /* toolbar */
    fill_rect(cx, cy, cw, PY_BAR, RGB(0xf6, 0xf7, 0xf9), 255);
    fill_rect(cx, cy + PY_BAR - 1, cw, 1, RGB(0xdc, 0xe0, 0xe6), 255);
    int x = cx + 10;
    py_toolbar_btn(cx, cy, YPB_RUN, &x, py_running() ? "Stop" : "Run  (F5)", 1, hv == YPB_RUN);
    x += 8;
    py_toolbar_btn(cx, cy, YPB_NEW, &x, "New", 0, hv == YPB_NEW);
    py_toolbar_btn(cx, cy, YPB_OPEN, &x, "Open", 0, hv == YPB_OPEN);
    py_toolbar_btn(cx, cy, YPB_SAVE, &x, "Save", 0, hv == YPB_SAVE);
    py_toolbar_btn(cx, cy, YPB_EX, &x, "Examples", 0, hv == YPB_EX);
    if (py.msg[0]) draw_text(x + 10, cy + 14, py.msg, RGB(0xc0, 0x40, 0x30), 255);
    /* code */
    int ey = cy + PY_BAR, eh = py.split - PY_BAR;
    fill_rect(cx, ey, cw, eh, RGB(255, 255, 255), 255);
    fill_rect(cx, ey, PY_GUT, eh, RGB(0xf3, 0xf5, 0xf8), 255);
    int rows = (eh - 8) / PY_LH, maxcols = (cw - PY_GUT - 16) / 8;
    int sa = 0, sb = 0, has = py_sel(&sa, &sb);
    int ocx0 = clip_x0, ocx1 = clip_x1, ocy0 = clip_y0, ocy1 = clip_y1;
    clip_y0 = imax(clip_y0, ey); clip_y1 = imin(clip_y1, ey + eh);
    int cl = py_line_of(py.caret);
    for (int r = 0; r < rows && py.top + r < py.nl; r++) {
        int l = py.top + r, y = ey + 4 + r * PY_LH;
        if (l + 1 == py.err_line) fill_rect(cx + PY_GUT, y, cw - PY_GUT, PY_LH, RGB(0xfd, 0xe4, 0xe4), 255);
        else if (l == cl && !has) fill_rect(cx + PY_GUT, y, cw - PY_GUT, PY_LH, RGB(0xf2, 0xf6, 0xfc), 255);
        char num[12]; snprintf(num, sizeof num, "%d", l + 1);
        draw_text(cx + PY_GUT - 10 - text_w(num), y + 1, num, l == cl ? PYC_TEXT : RGB(0xa8, 0xb0, 0xbc), 255);
        if (has) {                                  /* the selection */
            int a = imax(sa, py.ls[l]), b = imin(sb, py_line_end(l) + 1);
            if (a < b || (sa <= py.ls[l] && sb > py_line_end(l))) {
                int c0 = u8_cols(py.buf, py.ls[l], imax(a, py.ls[l])) - py.left, c1 = u8_cols(py.buf, py.ls[l], imin(b, py_line_end(l))) - py.left + (sb > py_line_end(l) ? 1 : 0);
                c0 = imax(0, c0); c1 = imin(maxcols, c1);
                if (c1 > c0) fill_rect(cx + PY_GUT + 8 + c0 * 8, y, (c1 - c0) * 8, PY_LH, RGB(0xc8, 0xdc, 0xf8), 255);
            }
        }
        clip_x0 = imax(ocx0, cx + PY_GUT + 8);
        py_draw_line(l, cx + PY_GUT + 8, y + 1, maxcols);
        clip_x0 = ocx0;
        if (l == cl && win_of(pyapp_draw) == active_win() && !py.focus_out) {
            int c = py_col(py.caret) - py.left;
            if (c >= 0 && c <= maxcols) fill_rect(cx + PY_GUT + 8 + c * 8, y, 2, PY_LH, PYC_TEXT, 255);
        }
    }
    clip_x0 = ocx0; clip_x1 = ocx1; clip_y0 = ocy0; clip_y1 = ocy1;
    if (!py.len) draw_text(cx + PY_GUT + 16, ey + 4 + PY_LH + 2, "Type Python code here, or open one of the Examples.", RGB(0xa8, 0xb0, 0xbc), 255);
    /* output */
    int oy = cy + py.split, oh = ch - py.split;
    fill_rect(cx, oy, cw, oh, RGB(0x1d, 0x20, 0x26), 255);
    fill_rect(cx, oy, cw, 26, RGB(0x2a, 0x2e, 0x36), 255);
    draw_text_bold(cx + 12, oy + 5, "Output", RGB(0xe6, 0xe8, 0xec));
    if (py.nout) { int bx = cx + cw - 70; if (hv == YPB_CLEAR) fill_round(bx - 6, oy + 2, 64, 22, 6, RGB(0x3a, 0x40, 0x4a), 255); draw_text(bx + 4, oy + 5, "Clear", RGB(0xc8, 0xcc, 0xd4), 255); }
    int inh = py_running() ? 34 : 0;
    int orows = (oh - 30 - inh) / PY_LH;
    int npart = (py.plen[0] > 0) + (py.plen[1] > 0), total = py.nout + npart;   /* (+ a line still being printed, like an input() question) */
    int first = imax(0, total - orows - py.oscroll);
    for (int r = 0; r < orows && first + r < total; r++) {
        int k = first + r;
        char t[400]; int kind;
        if (k < py.nout) { snprintf(t, sizeof t, "%s", py.out[k].s); kind = py.out[k].kind; }
        else { int w = k - py.nout == 0 && py.plen[0] ? 0 : 1; snprintf(t, sizeof t, "%.*s", py.plen[w], py.partial[w]); kind = w; }
        uint32_t c = kind == 1 ? RGB(0xff, 0x8a, 0x80) : kind == 2 ? RGB(0x8a, 0x94, 0xa4) : kind == 3 ? RGB(0x9c, 0xd6, 0xff) : RGB(0xe6, 0xe8, 0xec);
        while (t[0] && text_w(t) > cw - 24) t[u8_prev(t, (int)strlen(t))] = 0;
        draw_text(cx + 12, oy + 30 + r * PY_LH, t, c, 255);
    }
    if (!py.nout) draw_text(cx + 12, oy + 32, "Press Run (F5): what your program prints shows up here.", RGB(0x8a, 0x94, 0xa4), 255);
    if (py_running()) {                             /* answering input() */
        int iy = cy + ch - 32;
        fill_round(cx + 8, iy, cw - 16, 26, 6, py.focus_out ? RGB(0x34, 0x3a, 0x46) : RGB(0x2a, 0x2e, 0x36), 255);
        stroke_round(cx + 8, iy, cw - 16, 26, 6, py.focus_out ? TH()->accent : RGB(0x44, 0x4a, 0x56), 255);
        if (py.in[0]) draw_text(cx + 18, iy + 5, py.in, RGB(0x9c, 0xd6, 0xff), 255);
        else if (!py.focus_out) draw_text(cx + 18, iy + 5, "Click here to type an answer for input(), then press Enter", RGB(0x7a, 0x84, 0x94), 255);
        if (py.focus_out) fill_rect(cx + 18 + text_w(py.in) + 1, iy + 5, 1, 16, RGB(0xe6, 0xe8, 0xec), 255);
    }
    dlg_draw(&py_dlg, cx, cy, cw, ch, hv);
}
static int pyapp_hit(int lx, int ly, int cw, int ch) {
    if (py_dlg.active) return dlg_hit(&py_dlg, lx, ly, cw, ch);
    if (ly < PY_BAR) { for (int id = YPB_RUN; id <= YPB_EX; id++) if (lx >= py_btn_x[id] && lx < py_btn_x[id] + py_btn_w[id] && ly >= 7 && ly < 37) return id; return -1; }
    if (ly < py.split) return YPB_EDIT;
    if (py.nout && ly < py.split + 26 && lx >= cw - 76) return YPB_CLEAR;
    if (py_running() && ly >= ch - 32) return YPB_IN;
    return YPB_OUT;
}
static void pyapp_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(pyapp_draw), cx = 0, cy = 0, cw = 900, ch = 620;
    if (i >= 0) client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (dlg_elem_rect(&py_dlg, id, cw, ch, x, y, w, h)) { *x += cx; *y += cy; return; }
    *x = cx; *y = cy; *w = cw; *h = ch;
}
static int py_pos_at(int lx, int ly, int cw) {
    (void)cw;
    int l = clampi(py.top + (ly - PY_BAR - 4) / PY_LH, 0, py.nl - 1);
    int col = imax(0, (lx - PY_GUT - 8 + 4) / 8) + py.left;
    return py_pos_at_col(l, col);
}
static void py_show_caret(int cw) {
    int rows = (py.split - PY_BAR - 8) / PY_LH, maxcols = (cw - PY_GUT - 16) / 8;
    int l = py_line_of(py.caret), c = py_col(py.caret);
    if (l < py.top) py.top = l;
    if (l >= py.top + rows) py.top = l - rows + 1;
    if (c < py.left) py.left = imax(0, c - 8);
    if (c >= py.left + maxcols) py.left = c - maxcols + 8;
}
static int py_cw(void) { int w = win_of(pyapp_draw), cx, cy, cw = 900, ch; if (w >= 0) client_rect(&wins[w], &cx, &cy, &cw, &ch); return cw; }
static void pyapp_click(int lx, int ly, int cw, int ch) {
    if (dlg_click(&py_dlg, lx, ly, cw, ch, pyapp_draw)) return;
    int id = pyapp_hit(lx, ly, cw, ch);
    py.msg[0] = 0;
    py.focus_out = id == YPB_IN;
    if (id == YPB_RUN) py_run();
    else if (id == YPB_NEW) py_request(1);
    else if (id == YPB_OPEN) py_request(2);
    else if (id == YPB_SAVE) py_save();
    else if (id == YPB_EX) py_request(3);
    else if (id == YPB_CLEAR) py_out_clear();
    else if (id == YPB_EDIT) {
        int p = py_pos_at(lx, ly, cw);
        long long t = ui_ms();
        if (p == py.last_pos && t - py.last_click < 400) {        /* double-click: the word */
            int a = p, b = p;
            while (a > 0 && (isalnum((unsigned char)py.buf[a - 1]) || py.buf[a - 1] == '_')) a--;
            while (b < py.len && (isalnum((unsigned char)py.buf[b]) || py.buf[b] == '_')) b++;
            py.anchor = a; py.caret = b;
        } else {
            if (!shift_held) py.anchor = p;
            else if (py.anchor < 0) py.anchor = py.caret;
            py.caret = p; py.dragging = 1;
        }
        py.last_click = t; py.last_pos = p; py.goal = -1;
    }
    repaint_client(pyapp_draw);
}
static void pyapp_drag(int lx, int ly, int cw, int ch) {
    (void)ch;
    if (!py.dragging) return;
    int p = py_pos_at(lx, clampi(ly, PY_BAR, py.split - 4), cw);
    if (ly < PY_BAR + 4 && py.top > 0) py.top--;
    if (ly > py.split - 6 && py.top < py.nl - 1) py.top++;
    if (p != py.caret) { py.caret = p; repaint_client(pyapp_draw); }
}
static void pyapp_release(void) { py.dragging = 0; if (py.anchor == py.caret) py.anchor = -1; }
static void pyapp_wheel(int amount) {
    if (my >= 0) {
        int w = win_of(pyapp_draw); int cx, cy, cw, ch; if (w < 0) return; client_rect(&wins[w], &cx, &cy, &cw, &ch);
        if (my - cy >= py.split) {                    /* the output */
            int orows = (ch - py.split - 30 - (py_running() ? 34 : 0)) / PY_LH;
            py.oscroll = clampi(py.oscroll + amount * 3, 0, imax(0, py.nout - orows));
        } else py.top = clampi(py.top - amount * 3, 0, imax(0, py.nl - 3));
        repaint_client(pyapp_draw);
    }
}
static int py_indent_of(int l) { int i = py.ls[l], e = py_line_end(l), n = 0; while (i < e && py.buf[i] == ' ') { i++; n++; } return n; }
static void py_lines_sel(int *l0, int *l1) {
    int a, b;
    if (py_sel(&a, &b)) { *l0 = py_line_of(a); *l1 = py_line_of(b > a && b == py.ls[py_line_of(b)] ? b - 1 : b); }
    else *l0 = *l1 = py_line_of(py.caret);
}
static void py_indent_lines(int dir) {             /* Tab / Shift+Tab on several lines */
    int l0, l1; py_lines_sel(&l0, &l1);
    py_snapshot(0);
    for (int l = l1; l >= l0; l--) {
        int s = py.ls[l];
        if (dir > 0) { py_insert_raw(s, "    ", 4); }
        else { int n = 0; while (n < 4 && py.buf[s + n] == ' ') n++; if (n) py_delete_range(s, s + n); }
        py_lines();
    }
    py.anchor = py.ls[l0]; py.caret = py_line_end(l1);
    py_changed();
}
static void py_toggle_comment(void) {
    int l0, l1; py_lines_sel(&l0, &l1);
    int all = 1;
    for (int l = l0; l <= l1; l++) { int s = py.ls[l] + py_indent_of(l); if (s < py_line_end(l) && py.buf[s] != '#') all = 0; }
    py_snapshot(0);
    for (int l = l1; l >= l0; l--) {
        int s = py.ls[l] + py_indent_of(l);
        if (s >= py_line_end(l) && !all) continue;
        if (all) { int n = py.buf[s] == '#' ? (py.buf[s + 1] == ' ' ? 2 : 1) : 0; if (n) py_delete_range(s, s + n); }
        else py_insert_raw(s, "# ", 2);
        py_lines();
    }
    py_changed();
}
static void py_undo(int redo) {
    if (!redo) {
        if (py.nundo <= 0) return;
        if (py.uredo == py.nundo) { py_snapshot(-1); py.nundo--; }   /* (keep the latest to redo to) */
        py.nundo--;
    } else { if (py.nundo + 1 >= py.uredo) return; py.nundo++; }
    const char *t = py.undo[py.nundo]; if (!t) return;
    int n = (int)strlen(t); memcpy(py.buf, t, (size_t)n + 1); py.len = n;
    py.caret = imin(py.ucaret[py.nundo], py.len); py.anchor = -1; py.last_kind = 0;
    if (redo && py.nundo + 1 < py.uredo) {}
    py.modified = 1; py_lines(); py_title();
}
static void pyapp_key(int code, int mods) {
    if (dlg_key(&py_dlg, code, mods, pyapp_draw)) return;
    int ctrl = mods & MOD_CTRL, shift = mods & MOD_SHIFT, cw = py_cw();
    if (code == KEY_F5) { py_run(); return; }
    if (ctrl && code == KEY_S) { if (shift) py_saveas(); else py_save(); return; }
    if (ctrl && code == KEY_O) { py_request(2); return; }
    if (ctrl && code == KEY_N) { py_request(1); return; }
    if (py.focus_out && py_running()) {             /* typing an answer */
        if (code == KEY_ENTER || code == KEY_KPENTER) py_send_input();
        else if (code == KEY_BACKSPACE) { int l = (int)strlen(py.in); if (l) py.in[u8_prev(py.in, l)] = 0; }
        else if (code == KEY_ESC) py.focus_out = 0;
        else if (ctrl && code == KEY_C) py_stop();
        else if (ctrl && code == KEY_V && clipboard) { size_t l = strlen(py.in); for (int i = 0; i < clip_len && l < sizeof py.in - 1 && clipboard[i] != '\n'; i++) py.in[l++] = clipboard[i]; py.in[l] = 0; }
        else { char t[8]; int k = key_text(code, mods & ~MOD_CTRL, t); size_t l = strlen(py.in); if (k > 0 && !ctrl && t[0] != '\n' && t[0] != '\t' && l + (size_t)k < sizeof py.in) { memcpy(py.in + l, t, (size_t)k); py.in[l + (size_t)k] = 0; } }
        repaint_client(pyapp_draw); return;
    }
    int a = 0, b = 0, has = py_sel(&a, &b);
    int l = py_line_of(py.caret);
    int rows = (py.split - PY_BAR - 8) / PY_LH;
    int move = -1;                                    /* where the caret goes (keys that move it) */
    switch (code) {
    case KEY_LEFT: move = ctrl ? ({ int p = py.caret; while (p > 0 && !isalnum((unsigned char)py.buf[p - 1])) p--; while (p > 0 && (isalnum((unsigned char)py.buf[p - 1]) || py.buf[p - 1] == '_')) p--; p; })
                         : (has && !shift ? a : u8_prev(py.buf, py.caret)); break;
    case KEY_RIGHT: move = ctrl ? ({ int p = py.caret; while (p < py.len && (isalnum((unsigned char)py.buf[p]) || py.buf[p] == '_')) p++; while (p < py.len && !isalnum((unsigned char)py.buf[p]) && py.buf[p] != '\n') p++; p; })
                          : (has && !shift ? b : u8_next(py.buf, py.len, py.caret)); break;
    case KEY_UP: case KEY_DOWN: case KEY_PAGEUP: case KEY_PAGEDOWN: {
        int d = code == KEY_UP ? -1 : code == KEY_DOWN ? 1 : code == KEY_PAGEUP ? -rows : rows;
        if (py.goal < 0) py.goal = py_col(py.caret);
        int nl2 = clampi(l + d, 0, py.nl - 1);
        move = nl2 == l ? (d < 0 ? 0 : py.len) : py_pos_at_col(nl2, py.goal);
        if (code == KEY_PAGEUP || code == KEY_PAGEDOWN) py.top = clampi(py.top + d, 0, imax(0, py.nl - 1));
        break; }
    case KEY_HOME: { int ind = py.ls[l] + py_indent_of(l); move = ctrl ? 0 : (py.caret == ind ? py.ls[l] : ind); break; }
    case KEY_END: move = ctrl ? py.len : py_line_end(l); break;
    }
    if (move >= 0) {
        if (shift) { if (py.anchor < 0) py.anchor = py.caret; } else py.anchor = -1;
        py.caret = move;
        if (code != KEY_UP && code != KEY_DOWN && code != KEY_PAGEUP && code != KEY_PAGEDOWN) py.goal = -1;
        py_show_caret(cw); repaint_client(pyapp_draw); return;
    }
    py.goal = -1;
    if (ctrl) {
        switch (code) {
        case KEY_A: py.anchor = 0; py.caret = py.len; break;
        case KEY_C: if (has) clip_set(py.buf + a, b - a); else if (py_running()) py_stop(); return;
        case KEY_X: if (has) { clip_set(py.buf + a, b - a); py_snapshot(0); py_delete_range(a, b); py.caret = a; py.anchor = -1; py_changed(); } break;
        case KEY_V: if (clipboard && clip_len) {
                char *t = malloc((size_t)clip_len + 1); int n = 0;
                if (t) { for (int i = 0; i < clip_len; i++) { char c = clipboard[i]; if (c == '\r') continue; if (c == '\t') { t[n++] = ' '; continue; } t[n++] = c; } py_insert(t, n, 0); free(t); }
            } break;
        case KEY_Z: py_undo(shift); break;
        case KEY_Y: py_undo(1); break;
        case KEY_SLASH: py_toggle_comment(); break;
        case KEY_D: {                                  /* duplicate the line */
            int s = py.ls[l], e = py_line_end(l); char *t = strndup(py.buf + s, (size_t)(e - s));
            if (t) { py_snapshot(0); py_insert_raw(e, "\n", 1); py_insert_raw(e + 1, t, e - s); py.caret += e - s + 1; free(t); py_changed(); }
            break; }
        default: return;
        }
        py_show_caret(cw); repaint_client(pyapp_draw); return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) {      /* keep the indent; one more after ':' */
        int ind = py_indent_of(l);
        int p = py.caret - 1; while (p >= py.ls[l] && py.buf[p] == ' ') p--;
        if (p >= py.ls[l] && py.buf[p] == ':' ) ind += 4;
        int s = py.ls[l] + py_indent_of(l);
        if (s < py_line_end(l) && (!strncmp(py.buf + s, "return", 6) || !strncmp(py.buf + s, "break", 5) || !strncmp(py.buf + s, "pass", 4) || !strncmp(py.buf + s, "continue", 8) || !strncmp(py.buf + s, "raise", 5)) && py.caret == py_line_end(l))
            ind = imax(0, ind - 4);
        char t[130]; t[0] = '\n'; int n = imin(ind, 120); memset(t + 1, ' ', (size_t)n);
        py_insert(t, n + 1, 0);
    } else if (code == KEY_TAB) {
        if (has && py_line_of(a) != py_line_of(b)) py_indent_lines(shift ? -1 : 1);
        else if (shift) py_indent_lines(-1);
        else { int c = py_col(py.caret), n = 4 - c % 4; py_insert("    ", n, 2); }
    } else if (code == KEY_BACKSPACE) {
        if (has) { py_snapshot(0); py_delete_range(a, b); py.caret = a; py.anchor = -1; py_changed(); }
        else if (py.caret > 0) {
            int c = py_col(py.caret), only = 1;
            for (int i = py.ls[l]; i < py.caret; i++) if (py.buf[i] != ' ') only = 0;
            int n = only && c > 0 ? (c % 4 ? c % 4 : 4) : py.caret - u8_prev(py.buf, py.caret);
            py_snapshot(3); py_delete_range(py.caret - n, py.caret); py.caret -= n; py_changed();
        }
    } else if (code == KEY_DELETE) {
        if (has) { py_snapshot(0); py_delete_range(a, b); py.caret = a; py.anchor = -1; py_changed(); }
        else if (py.caret < py.len) { py_snapshot(4); py_delete_range(py.caret, u8_next(py.buf, py.len, py.caret)); py_changed(); }
    } else if (code == KEY_ESC) { py.anchor = -1; }
    else {
        char t[8]; int k = key_text(code, mods, t);
        if (k <= 0 || t[0] == '\n' || t[0] == '\t') return;
        /* typing a closing bracket / quote that's already there: step over it */
        if (k == 1 && !has && py.caret < py.len && py.buf[py.caret] == t[0] && strchr(")]}", t[0])) { py.caret++; }
        else py_insert(t, k, 1);
    }
    py_show_caret(cw); repaint_client(pyapp_draw);
}
static void pyapp_tick(void) { py_reap(); }
static int pyapp_on_close(void) {
    if (py_closing || !py.modified || !py.len) { py_closing = 0; py_stop(); return 1; }
    py_pending = 9; py_dlg_mode = PYD_PROMPT;
    dlg_open(&py_dlg, DLG_MSG, "Python", "Save your changes first?", NULL, "Save", "Don't save", "Cancel", py_dlg_done);
    repaint_client(pyapp_draw);
    return 0;
}
static void pyapp_open(void);
static void pyapp_open_file(const char *path) {     /* a .py from File Explorer */
    pyapp_open();
    FILE *f = fopen(path, "r"); if (!f) return;
    char *t = malloc(PY_MAX + 1); size_t n = t ? fread(t, 1, PY_MAX, f) : 0; fclose(f);
    if (!t) return;
    int k = 0; for (size_t i = 0; i < n; i++) if (t[i] != '\r') t[k++] = t[i];
    py_set_text(t, k); free(t);
    const char *s = strrchr(path, '/'); snprintf(py.file, sizeof py.file, "%s", s ? s + 1 : path);
    py.modified = 0;
    /* (it's saved back into D:\Documents\Python) */
}
static void pyapp_open(void) {
    static int first = 1;
    if (first) { first = 0; py_set_text(py_examples[0].code, (int)strlen(py_examples[0].code)); }
    static const Win t = { .title = "untitled.py - Python", .icon = ICON_PYTHON, .draw = pyapp_draw, .click = pyapp_click, .hit = pyapp_hit,
                           .elem_rect = pyapp_elem_rect, .key = pyapp_key, .wheel = pyapp_wheel, .drag = pyapp_drag, .release = pyapp_release,
                           .fd = py_fd, .wake = py_wake, .tick = pyapp_tick, .on_close = pyapp_on_close };
    int w = win_of(pyapp_draw);
    if (w >= 0) { restore_win(w); return; }
    open_win(&t, imin(W - 60, 940), imin(H - TASKBAR_H - 40, 660));
    py_title();
}
static void draw_python_icon(int cx, int cy, int r) {   /* a code page with a green snake-ish curve */
    int w = r * 2, h = r * 2, x = cx - r, y = cy - r;
    fill_round(x, y, w, h, imax(3, r / 3), RGB(0x2b, 0x31, 0x3a), 255);
    float s = r / 16.0f;
    for (float t = 0; t < 1; t += 0.01f) {
        float px = cx - 10 * s + 20 * s * t, py2 = cy + 2 * s + sinf(t * 6.2831853f) * 5 * s;
        fill_circle(px, py2, imax(1.5f, 2.6f * s), RGB(0x4c, 0xc4, 0x6a), 255);
    }
    fill_circle(cx + 10 * s, cy + 2 * s, imax(1.5f, 3.2f * s), RGB(0x4c, 0xc4, 0x6a), 255);
    if (r >= 12) { draw_text(cx - 12, y + (int)(2 * s), ">>>", RGB(0xf0, 0xc8, 0x40), 255); }
}
