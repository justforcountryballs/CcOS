/*
 *  watchdog.c - find out what makes the desktop freeze
 *
 *  The main loop says when it starts working (wd_busy) and when it goes
 *  back to waiting (wd_idle). A helper thread looks every 250 ms; if the
 *  desktop has been busy for more than a second, it notes what the main
 *  thread is stuck in (the kernel's view: /proc/self/task/ID/stack and
 *  wchan) and how long the freeze lasted, in /tmp/ccos-freeze.log and on
 *  the stick as D:\ccos-freeze-log.txt.
 */
#include <sys/syscall.h>
static volatile long long wd_since;            /* ms the main loop got busy; 0 = waiting */
static const char *volatile wd_where = "start";
static pid_t wd_tid;
static long long wd_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static void wd_busy(const char *where) { wd_where = where; __atomic_store_n(&wd_since, wd_now(), __ATOMIC_RELEASE); }
static void wd_idle(void) { __atomic_store_n(&wd_since, 0, __ATOMIC_RELEASE); }
static void wd_at(const char *where) { wd_where = where; }
static void wd_cat(FILE *o, const char *name) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/%s", (int)wd_tid, name);
    FILE *f = fopen(p, "r"); if (!f) return;
    char l[200]; int n = 0;
    while (fgets(l, sizeof l, f) && n++ < 24) fprintf(o, "    %s%s", l, strchr(l, '\n') ? "" : "\n");
    fclose(f);
}
static void *wd_thread(void *arg) {
    (void)arg;
    long long seen = 0;                          /* the stall already written down */
    for (;;) {
        usleep(250000);
        long long s = __atomic_load_n(&wd_since, __ATOMIC_ACQUIRE);
        if (s && seen != s && wd_now() - s > 1200) {
            seen = s;
            FILE *o = fopen("/tmp/ccos-freeze.log", "a");
            if (!o) continue;
            time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
            fprintf(o, "%s  desktop stuck for %lld ms in: %s\n", ts, wd_now() - s, wd_where);
            fprintf(o, "  wchan: "); wd_cat(o, "wchan"); fprintf(o, "\n  syscall: "); wd_cat(o, "syscall");
            fprintf(o, "  kernel stack:\n"); wd_cat(o, "stack");
            fclose(o);
            while (__atomic_load_n(&wd_since, __ATOMIC_ACQUIRE) == s) usleep(50000);
            o = fopen("/tmp/ccos-freeze.log", "a");
            if (o) { fprintf(o, "  ...it lasted %lld ms\n\n", wd_now() - s); fclose(o); }
            /* a copy on the stick (this thread may wait for the disk; the desktop doesn't) */
            struct stat a, b;
            if (stat("/data", &a) == 0 && stat("/", &b) == 0 && a.st_dev != b.st_dev) {
                FILE *in = fopen("/tmp/ccos-freeze.log", "r"), *out = fopen("/data/ccos-freeze-log.txt", "w");
                char buf[4096]; size_t k;
                while (in && out && (k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out);
                if (in) fclose(in);
                if (out) fclose(out);
            }
        }
    }
    return NULL;
}
static void wd_start(void) {
    wd_tid = (pid_t)syscall(SYS_gettid);
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, wd_thread, NULL);
}
