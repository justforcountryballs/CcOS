/* ===================================================================
 *  Saving without freezing: files the desktop writes (settings,
 *  Notepad, Calendar, Paint, screenshots) are handed to a helper
 *  thread. Writing to a USB stick can take seconds while it's busy;
 *  the desktop carries on meanwhile. If a write fails, a notification
 *  says so.
 * =================================================================== */
static int wake_pipe[2] = { -1, -1 };
static void toast(int icon, const char *title, const char *body);
typedef struct AwJob { struct AwJob *next; char path[320]; char *data; size_t n; int atomic, mkdirs; char what[64]; } AwJob;
static AwJob *aw_head, *aw_tail;
static pthread_mutex_t aw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aw_cv = PTHREAD_COND_INITIALIZER;
static int aw_started, aw_busy;
static char aw_fail[64]; static volatile int aw_failed;

static void aw_mkdirs(const char *path) {          /* the folders above path */
    char d[320]; snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
}
static void *aw_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&aw_mu);
        while (!aw_head) { aw_busy = 0; pthread_cond_wait(&aw_cv, &aw_mu); }
        AwJob *j = aw_head; aw_head = j->next; if (!aw_head) aw_tail = NULL;
        /* a newer save of the same file is waiting: skip this one */
        int newer = 0; for (AwJob *k = aw_head; k; k = k->next) if (!strcmp(k->path, j->path)) newer = 1;
        aw_busy = 1;
        pthread_mutex_unlock(&aw_mu);
        if (!newer) {
            if (j->mkdirs) aw_mkdirs(j->path);
            char tmp[340]; snprintf(tmp, sizeof tmp, "%s.ccos-tmp", j->path);
            const char *to = j->atomic ? tmp : j->path;
            FILE *f = fopen(to, "wb");
            int ok = f && fwrite(j->data, 1, j->n, f) == j->n;
            if (f && fclose(f)) ok = 0;
            if (ok && j->atomic && rename(tmp, j->path)) ok = 0;
            if (!ok) {
                if (j->atomic) unlink(tmp);
                snprintf(aw_fail, sizeof aw_fail, "%s", j->what);
                aw_failed = 1;
                if (wake_pipe[1] >= 0) { if (write(wake_pipe[1], "w", 1) < 0) {} }
            }
        }
        free(j->data); free(j);
    }
    return NULL;
}
/* save n bytes of data to path (copied: the caller may free it). atomic: never half-written */
static int aw_save(const char *path, const void *data, size_t n, int atomic, const char *what) {
    AwJob *j = calloc(1, sizeof *j);
    if (!j || !(j->data = malloc(n ? n : 1))) { free(j); return 0; }
    memcpy(j->data, data, n); j->n = n; j->atomic = atomic; j->mkdirs = 1;
    snprintf(j->path, sizeof j->path, "%s", path); snprintf(j->what, sizeof j->what, "%s", what ? what : "a file");
    pthread_mutex_lock(&aw_mu);
    if (!aw_started) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, aw_thread, NULL) == 0) aw_started = 1;
        pthread_attr_destroy(&at);
    }
    if (!aw_started) { pthread_mutex_unlock(&aw_mu); free(j->data); free(j); return 0; }
    if (aw_tail) aw_tail->next = j; else aw_head = j;
    aw_tail = j; aw_busy = 1;
    pthread_cond_signal(&aw_cv);
    pthread_mutex_unlock(&aw_mu);
    return 1;
}
/* before shutting down: wait (a while) for what's still being written */
static void aw_flush(int ms) {
    for (int t = 0; t < ms; t += 50) {
        pthread_mutex_lock(&aw_mu); int b = aw_busy || aw_head; pthread_mutex_unlock(&aw_mu);
        if (!b) return;
        usleep(50000);
    }
}
static void aw_tick(void) {
    if (!aw_failed) return;
    aw_failed = 0;
    char b[120]; snprintf(b, sizeof b, "Couldn't save %s. Is the USB stick full or unplugged?", aw_fail);
    toast(ICON_FILE, "Not saved", b);
}
