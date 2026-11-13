/* ccos-files - the work behind File Explorer's Copy, Cut/Paste and zip files
 *
 *   ccos-files copy DEST SRC...      copy files and folders into the folder DEST
 *   ccos-files move DEST SRC...      the same, then remove the originals
 *   ccos-files zip OUT.zip SRC...    pack files and folders into a zip file
 *   ccos-files unzip FILE.zip DEST   unpack a zip file into the folder DEST
 *
 * It prints what it's doing, for the progress window:
 *   T <total bytes> <items>       first: how much there is to do
 *   P <bytes done> <name>         now and then
 *   N <path>                      something new it made (a pasted file, the zip, ...)
 *   OK | ERR <message> | CANCELED at the end
 * Stop it with SIGTERM (the Cancel button): the half-done file is removed.
 * A name that's already taken gets " (2)" added, like Windows' "keep both".
 *
 * Build: gcc -O2 -o ccos-files files.c -lz -larchive
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <zlib.h>
#include <locale.h>
#include <archive.h>
#include <archive_entry.h>

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }
static long long total, done_bytes; static int nitems;
static long long last_report;
static char partial[PATH_MAX];             /* the file being written, removed if we stop */

static long long now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000LL + tv.tv_usec / 1000; }
static const char *base_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void report(const char *name, int force) {
    if (!force && now_ms() - last_report < 150) return;
    last_report = now_ms();
    printf("P %lld %s\n", done_bytes, base_of(name)); fflush(stdout);
}
static void fail(const char *msg, const char *path) {
    if (partial[0]) unlink(partial);
    if (path) printf("ERR %s: %s\n", msg, base_of(path)); else printf("ERR %s\n", msg);
    fflush(stdout); sync(); exit(1);
}
static void check_stop(void) {
    if (!stop) return;
    if (partial[0]) unlink(partial);
    printf("CANCELED\n"); fflush(stdout); sync(); exit(2);
}

/* ---- how much is there ---- */
static void measure(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return;
    nitems++;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[PATH_MAX]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            measure(c);
        }
        if (d) closedir(d);
    } else if (S_ISREG(st.st_mode)) total += st.st_size;
}
/* "name.txt" taken in dir: "name (2).txt" */
static void free_name(const char *dir, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, name);
    if (access(out, F_OK) != 0 && errno == ENOENT) return;
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int k = 2; k < 1000; k++) {
        snprintf(out, n, "%s/%.*s (%d)%s", dir, stem, name, k, dot && dot != name ? dot : "");
        if (access(out, F_OK) != 0) return;
    }
}

/* ---- copying ---- */
static void copy_file(const char *src, const char *dst, const struct stat *st) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) fail("Couldn't read", src);
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 0777);
    if (out < 0) { close(in); fail(errno == ENOSPC ? "Not enough room" : errno == EROFS ? "The drive is read-only" : "Couldn't write", dst); }
    snprintf(partial, sizeof partial, "%s", dst);
    static char buf[1 << 20];
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t o = 0; o < r; ) {
            ssize_t w = write(out, buf + o, (size_t)(r - o));
            if (w <= 0) { close(in); close(out); fail(errno == ENOSPC ? "Not enough room on the drive for" : "Couldn't write", dst); }
            o += w;
        }
        done_bytes += r;
        report(src, 0);
        check_stop();
    }
    if (r < 0) { close(in); close(out); fail("Couldn't read", src); }
    if (fsync(out) != 0 && errno != EINVAL) { close(in); close(out); fail("Couldn't finish writing", dst); }
    close(in); close(out);
    partial[0] = 0;
    struct utimbuf ut = { st->st_atime, st->st_mtime };
    utime(dst, &ut);                                   /* keep the date */
}
static void copy_tree(const char *src, const char *dst) {
    check_stop();
    struct stat st;
    if (lstat(src, &st) != 0) fail("Couldn't find", src);
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) fail(errno == ENOSPC ? "Not enough room" : "Couldn't make the folder", dst);
        DIR *d = opendir(src); struct dirent *e;
        if (!d) fail("Couldn't open the folder", src);
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char s[PATH_MAX], t[PATH_MAX];
            snprintf(s, sizeof s, "%s/%s", src, e->d_name); snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
            copy_tree(s, t);
        }
        closedir(d);
    } else if (S_ISLNK(st.st_mode)) {
        char l[PATH_MAX]; ssize_t n = readlink(src, l, sizeof l - 1);
        if (n > 0) { l[n] = 0; if (symlink(l, dst) != 0) { struct stat t; if (stat(src, &t) == 0 && S_ISREG(t.st_mode)) copy_file(src, dst, &t); } }
    } else if (S_ISREG(st.st_mode)) copy_file(src, dst, &st);
    report(src, 0);
}
static int rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[PATH_MAX]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            rm_tree(c);
        }
        if (d) closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}
static int inside(const char *dir, const char *src) {     /* is dir the same as src, or under it? */
    char a[PATH_MAX], b[PATH_MAX];
    if (!realpath(dir, a) || !realpath(src, b)) return 0;
    size_t l = strlen(b);
    return !strncmp(a, b, l) && (a[l] == 0 || a[l] == '/');
}
static int cmd_copy(int move, const char *dest, char **src, int n) {
    struct stat ds;
    if (stat(dest, &ds) != 0 || !S_ISDIR(ds.st_mode)) fail("The folder to paste into is gone", dest);
    for (int i = 0; i < n; i++) {
        if (inside(dest, src[i])) fail("A folder can't go inside itself", src[i]);
        measure(src[i]);
    }
    printf("T %lld %d\n", total, nitems); fflush(stdout);
    for (int i = 0; i < n; i++) {
        char dst[PATH_MAX], sdir[PATH_MAX];
        snprintf(sdir, sizeof sdir, "%s", src[i]);
        char *sl = strrchr(sdir, '/'); if (sl) *sl = 0;
        int same_dir = sl && !strcmp(sdir[0] ? sdir : "/", dest);
        if (move && same_dir) continue;                         /* cut and pasted in the same place: nothing to do */
        free_name(dest, base_of(src[i]), dst, sizeof dst);
        if (move && rename(src[i], dst) == 0) {                 /* same drive: just move it */
            struct stat st; if (lstat(dst, &st) == 0 && S_ISREG(st.st_mode)) done_bytes += st.st_size;
            printf("N %s\n", dst); report(dst, 1);
            continue;
        }
        copy_tree(src[i], dst);
        printf("N %s\n", dst); fflush(stdout);
        if (move) rm_tree(src[i]);
    }
    report("", 1);
    sync();
    printf("OK\n");
    return 0;
}

/* ---- zip ----------------------------------------------------------- */
typedef struct { char *name; uint32_t crc, csize, usize, off; uint16_t method, mtime, mdate; int dir; } ZEnt;
static ZEnt *zents; static int nz, zcap;
static FILE *zf;
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint16_t get16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void dos_time(time_t t, uint16_t *tm_, uint16_t *dt) {
    struct tm tm; localtime_r(&t, &tm);
    *tm_ = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *dt = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}
static void z_local(ZEnt *e) {
    unsigned char h[30]; memset(h, 0, sizeof h);
    put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 6, 0x0808);   /* sizes after the data; names are UTF-8 */
    put16(h + 8, e->method); put16(h + 10, e->mtime); put16(h + 12, e->mdate);
    put16(h + 26, (unsigned)strlen(e->name));
    e->off = (uint32_t)ftell(zf);
    if (fwrite(h, 1, 30, zf) != 30 || fwrite(e->name, 1, strlen(e->name), zf) != strlen(e->name)) fail("Couldn't write the zip file", NULL);
}
static void z_desc(ZEnt *e) {
    unsigned char d[16];
    put32(d, 0x08074b50); put32(d + 4, e->crc); put32(d + 8, e->csize); put32(d + 12, e->usize);
    if (fwrite(d, 1, 16, zf) != 16) fail("Couldn't write the zip file", NULL);
}
static ZEnt *z_new(const char *name, int dir, time_t mt) {
    if (nz == zcap) { zcap = zcap ? zcap * 2 : 64; zents = realloc(zents, (size_t)zcap * sizeof *zents); if (!zents) fail("Out of memory", NULL); }
    ZEnt *e = &zents[nz++]; memset(e, 0, sizeof *e);
    e->name = strdup(name); e->dir = dir;
    dos_time(mt, &e->mtime, &e->mdate);
    return e;
}
static void z_add_file(const char *path, const char *name, const struct stat *st) {
    if (st->st_size >= 0xFFFFFFFFLL) fail("Files over 4 GB can't go in a zip yet", path);
    ZEnt *e = z_new(name, 0, st->st_mtime);
    e->method = 8;
    z_local(e);
    FILE *in = fopen(path, "rb"); if (!in) fail("Couldn't read", path);
    z_stream zs; memset(&zs, 0, sizeof zs);
    deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    static unsigned char ib[1 << 18], ob[1 << 18];
    uLong crc = crc32(0, NULL, 0); size_t r; int flush;
    do {
        r = fread(ib, 1, sizeof ib, in);
        crc = crc32(crc, ib, (uInt)r); e->usize += (uint32_t)r;
        flush = feof(in) ? Z_FINISH : Z_NO_FLUSH;
        zs.next_in = ib; zs.avail_in = (uInt)r;
        do {
            zs.next_out = ob; zs.avail_out = sizeof ob;
            deflate(&zs, flush);
            size_t have = sizeof ob - zs.avail_out;
            if (fwrite(ob, 1, have, zf) != have) { fclose(in); fail("Not enough room for the zip file", NULL); }
            e->csize += (uint32_t)have;
        } while (zs.avail_out == 0);
        done_bytes += (long long)r; report(path, 0); check_stop();
    } while (flush != Z_FINISH);
    deflateEnd(&zs); fclose(in);
    e->crc = (uint32_t)crc;
    z_desc(e);
}
static void z_add_tree(const char *path, const char *name) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        char dn[PATH_MAX]; snprintf(dn, sizeof dn, "%s/", name);
        ZEnt *e = z_new(dn, 1, st.st_mtime); e->method = 0; z_local(e); z_desc(e);
        DIR *d = opendir(path); struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char c[PATH_MAX], cn[PATH_MAX];
            snprintf(c, sizeof c, "%s/%s", path, de->d_name); snprintf(cn, sizeof cn, "%s/%s", name, de->d_name);
            z_add_tree(c, cn);
        }
        if (d) closedir(d);
    } else if (S_ISREG(st.st_mode)) z_add_file(path, name, &st);
}
static int cmd_zip(const char *out, char **src, int n) {
    for (int i = 0; i < n; i++) measure(src[i]);
    if (total >= 0xFFFFFFFFLL) fail("Zips over 4 GB aren't supported yet", NULL);
    printf("T %lld %d\n", total, nitems); fflush(stdout);
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", out);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    char target[PATH_MAX]; free_name(dir[0] ? dir : "/", base_of(out), target, sizeof target);
    zf = fopen(target, "wb");
    if (!zf) fail(errno == EROFS ? "The drive is read-only" : "Couldn't make the zip file", target);
    snprintf(partial, sizeof partial, "%s", target);
    for (int i = 0; i < n; i++) z_add_tree(src[i], base_of(src[i]));
    long cd = ftell(zf);
    for (int i = 0; i < nz; i++) {                               /* the central directory */
        ZEnt *e = &zents[i]; unsigned char h[46]; memset(h, 0, sizeof h);
        put32(h, 0x02014b50); put16(h + 4, 0x031e); put16(h + 6, 20); put16(h + 8, 0x0808);
        put16(h + 10, e->method); put16(h + 12, e->mtime); put16(h + 14, e->mdate);
        put32(h + 16, e->crc); put32(h + 20, e->csize); put32(h + 24, e->usize);
        put16(h + 28, (unsigned)strlen(e->name));
        put32(h + 38, e->dir ? (040755u << 16 | 0x10) : (0100644u << 16));
        put32(h + 42, e->off);
        if (fwrite(h, 1, 46, zf) != 46 || fwrite(e->name, 1, strlen(e->name), zf) != strlen(e->name)) fail("Couldn't write the zip file", NULL);
    }
    long end = ftell(zf);
    unsigned char eo[22]; memset(eo, 0, sizeof eo);
    put32(eo, 0x06054b50); put16(eo + 8, (unsigned)nz); put16(eo + 10, (unsigned)nz);
    put32(eo + 12, (uint32_t)(end - cd)); put32(eo + 16, (uint32_t)cd);
    if (fwrite(eo, 1, 22, zf) != 22 || fflush(zf) != 0) fail("Not enough room for the zip file", NULL);
    fsync(fileno(zf)); fclose(zf);
    partial[0] = 0;
    printf("N %s\n", target); report("", 1);
    sync(); printf("OK\n");
    return 0;
}

/* ---- unzip --------------------------------------------------------- */
/* a name from inside a zip, made safe: no "..", no leading "/" or "\" */
static int safe_name(const char *in, char *out, size_t n) {
    size_t k = 0;
    while (*in == '/' || *in == '\\') in++;
    for (const char *p = in; *p && k + 1 < n; p++) out[k++] = *p == '\\' ? '/' : *p;
    out[k] = 0;
    for (char *s = out; s; ) {
        char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 2 && s[0] == '.' && s[1] == '.') return 0;
        s = e ? e + 1 : NULL;
    }
    return out[0] != 0;
}
static void mkdirs(const char *path) {                          /* every folder on the way to path */
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}
static int cmd_unzip(const char *zip, const char *dest) {
    FILE *f = fopen(zip, "rb");
    if (!f) fail("Couldn't open", zip);
    fseek(f, 0, SEEK_END); long size = ftell(f);
    long scan = size < 66000 ? size : 66000;                     /* find the end record */
    unsigned char *tail = malloc((size_t)scan);
    fseek(f, size - scan, SEEK_SET);
    if (!tail || fread(tail, 1, (size_t)scan, f) != (size_t)scan) fail("Couldn't read", zip);
    long eocd = -1;
    for (long i = scan - 22; i >= 0; i--) if (get32(tail + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) fail("This isn't a zip file (or it's damaged)", zip);
    unsigned count = get16(tail + eocd + 10); uint32_t cdsize = get32(tail + eocd + 12), cdoff = get32(tail + eocd + 16);
    free(tail);
    if (cdoff == 0xFFFFFFFFu || count == 0xFFFF) fail("Zips over 4 GB aren't supported yet", zip);
    unsigned char *cd = malloc(cdsize ? cdsize : 1);
    fseek(f, (long)cdoff, SEEK_SET);
    if (!cd || fread(cd, 1, cdsize, f) != cdsize) fail("This zip file is damaged", zip);
    /* first: how much is there, and is it all inside one folder (then don't nest it twice) */
    char top[256] = ""; int one_top = 1;
    for (uint32_t p = 0, i = 0; i < count && p + 46 <= cdsize; i++) {
        total += get32(cd + p + 24); nitems++;
        unsigned nl = get16(cd + p + 28);
        char raw[1024], name[1024];
        snprintf(raw, sizeof raw, "%.*s", (int)(nl < sizeof raw ? nl : sizeof raw - 1), (const char *)cd + p + 46);
        p += 46 + nl + get16(cd + p + 30) + get16(cd + p + 32);
        if (!safe_name(raw, name, sizeof name)) continue;
        char *sl = strchr(name, '/');
        if (!sl || (size_t)(sl - name) >= sizeof top - 1) { one_top = 0; continue; }
        sl[1] = 0;
        if (!top[0]) snprintf(top, sizeof top, "%.250s", name);
        else if (strcmp(top, name)) one_top = 0;
    }
    size_t strip = one_top && top[0] ? strlen(top) : 0;
    printf("T %lld %d\n", total, nitems); fflush(stdout);
    mkdir(dest, 0755);
    printf("N %s\n", dest); fflush(stdout);
    static unsigned char ib[1 << 18], ob[1 << 18];
    for (uint32_t p = 0, i = 0; i < count && p + 46 <= cdsize; i++) {
        check_stop();
        unsigned flags = get16(cd + p + 8), method = get16(cd + p + 10);
        uint32_t crc = get32(cd + p + 16), csize = get32(cd + p + 20), usize = get32(cd + p + 24), off = get32(cd + p + 42);
        unsigned nl = get16(cd + p + 28);
        char raw[1024], name[1024], out[PATH_MAX];
        snprintf(raw, sizeof raw, "%.*s", (int)(nl < sizeof raw ? nl : sizeof raw - 1), (const char *)cd + p + 46);
        p += 46 + nl + get16(cd + p + 30) + get16(cd + p + 32);
        if (!safe_name(raw, name, sizeof name)) continue;
        if (strip) { if (strlen(name) <= strip) continue; memmove(name, name + strip, strlen(name + strip) + 1); }
        if (flags & 1) fail("Password-protected zips aren't supported", zip);
        snprintf(out, sizeof out, "%s/%s", dest, name);
        size_t ol = strlen(out);
        if (out[ol - 1] == '/') { mkdirs(out); continue; }       /* a folder */
        mkdirs(out);
        unsigned char lh[30];
        fseek(f, (long)off, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || get32(lh) != 0x04034b50) fail("This zip file is damaged", zip);
        fseek(f, (long)off + 30 + get16(lh + 26) + get16(lh + 28), SEEK_SET);
        if (method != 0 && method != 8) fail("This zip uses a kind of packing ccos can't unpack yet", zip);
        FILE *o = fopen(out, "wb");
        if (!o) fail(errno == ENOSPC ? "Not enough room" : "Couldn't write", out);
        snprintf(partial, sizeof partial, "%s", out);
        uLong c = crc32(0, NULL, 0); uint32_t left = csize;
        if (method == 0) {
            while (left) {
                size_t want = left < sizeof ib ? left : sizeof ib, r = fread(ib, 1, want, f);
                if (!r) fail("This zip file is damaged", zip);
                if (fwrite(ib, 1, r, o) != r) fail("Not enough room on the drive for", out);
                c = crc32(c, ib, (uInt)r); left -= (uint32_t)r; done_bytes += (long long)r;
                report(name, 0); check_stop();
            }
        } else {
            z_stream zs; memset(&zs, 0, sizeof zs);
            inflateInit2(&zs, -15);
            int ret = Z_OK;
            while (ret != Z_STREAM_END) {
                if (zs.avail_in == 0) {
                    size_t want = left < sizeof ib ? left : sizeof ib;
                    size_t r = want ? fread(ib, 1, want, f) : 0;
                    if (!r) break;
                    left -= (uint32_t)r; zs.next_in = ib; zs.avail_in = (uInt)r;
                }
                zs.next_out = ob; zs.avail_out = sizeof ob;
                ret = inflate(&zs, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); fail("This zip file is damaged", zip); }
                size_t have = sizeof ob - zs.avail_out;
                if (fwrite(ob, 1, have, o) != have) fail("Not enough room on the drive for", out);
                c = crc32(c, ob, (uInt)have); done_bytes += (long long)have;
                report(name, 0); check_stop();
            }
            inflateEnd(&zs);
        }
        if (fflush(o) != 0) fail("Not enough room on the drive for", out);
        fclose(o); partial[0] = 0;
        if ((uint32_t)c != crc) fail("This zip file is damaged (a check failed) at", out);
        (void)usize;
    }
    fclose(f);
    report("", 1); sync();
    printf("OK\n");
    return 0;
}

/* .rar, .7z, .tar.gz/.xz/.bz2... (libarchive) */
static int cmd_extract(const char *arc, const char *dest) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a); archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, arc, 1 << 16) != ARCHIVE_OK) fail("This archive can't be opened (damaged, or an unknown kind)", arc);
    /* first pass: how much, and is it all in one folder */
    char top[256] = ""; int one_top = 1;
    struct archive_entry *e;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        char name[1024];
        const char *pn = archive_entry_pathname_utf8(e); if (!pn) pn = archive_entry_pathname(e);
        if (!pn || !safe_name(pn, name, sizeof name)) continue;
        total += archive_entry_size(e); nitems++;
        if (archive_entry_filetype(e) == AE_IFDIR && name[strlen(name) - 1] != '/') strncat(name, "/", sizeof name - strlen(name) - 1);
        char *sl = strchr(name, '/');
        if (!sl || (size_t)(sl - name) >= sizeof top - 1) { one_top = 0; continue; }
        sl[1] = 0;
        if (!top[0]) snprintf(top, sizeof top, "%.250s", name);
        else if (strcmp(top, name)) one_top = 0;
        check_stop();
    }
    archive_read_free(a);
    size_t strip = one_top && top[0] ? strlen(top) : 0;
    printf("T %lld %d\n", total, nitems); fflush(stdout);
    mkdir(dest, 0755);
    printf("N %s\n", dest); fflush(stdout);
    a = archive_read_new();
    archive_read_support_format_all(a); archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, arc, 1 << 16) != ARCHIVE_OK) fail("This archive can't be opened", arc);
    static char buf[1 << 18];
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        check_stop();
        char name[1024], out[PATH_MAX];
        const char *pn = archive_entry_pathname_utf8(e); if (!pn) pn = archive_entry_pathname(e);
        if (!pn || !safe_name(pn, name, sizeof name)) continue;
        int isdir = archive_entry_filetype(e) == AE_IFDIR;
        if (strip) { if (strlen(name) <= strip) continue; memmove(name, name + strip, strlen(name + strip) + 1); }
        snprintf(out, sizeof out, "%s/%s", dest, name);
        if (isdir || out[strlen(out) - 1] == '/') { strncat(out, "/", sizeof out - strlen(out) - 1); mkdirs(out); continue; }
        if (archive_entry_filetype(e) != AE_IFREG) continue;              /* links, devices: skipped (safer) */
        mkdirs(out);
        FILE *o = fopen(out, "wb");
        if (!o) fail(errno == ENOSPC ? "Not enough room" : "Couldn't write", out);
        snprintf(partial, sizeof partial, "%s", out);
        la_ssize_t k;
        while ((k = archive_read_data(a, buf, sizeof buf)) > 0) {
            if (fwrite(buf, 1, (size_t)k, o) != (size_t)k) fail("Not enough room on the drive for", out);
            done_bytes += k; report(name, 0); check_stop();
        }
        if (k < 0) {
            const char *m = archive_error_string(a);
            if (m && (strstr(m, "assword") || strstr(m, "ncrypt"))) fail("Password-protected archives aren't supported", arc);
            fail("This archive is damaged at", out);
        }
        if (fflush(o) != 0) fail("Not enough room on the drive for", out);
        fclose(o); partial[0] = 0;
        if (archive_entry_perm(e) & 0100) chmod(out, 0755);
    }
    if (r != ARCHIVE_EOF) {
        const char *m = archive_error_string(a);
        if (m && (strstr(m, "assword") || strstr(m, "ncrypt"))) fail("Password-protected archives aren't supported", arc);
        fail("This archive is damaged (or partly unsupported)", arc);
    }
    archive_read_free(a);
    report("", 1); sync();
    printf("OK\n");
    return 0;
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "C.UTF-8");                     /* archive names are UTF-8 */
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 4 && (!strcmp(argv[1], "copy") || !strcmp(argv[1], "move"))) return cmd_copy(argv[1][0] == 'm', argv[2], argv + 3, argc - 3);
    if (argc >= 4 && !strcmp(argv[1], "zip")) return cmd_zip(argv[2], argv + 3, argc - 3);
    if (argc == 4 && !strcmp(argv[1], "unzip")) return cmd_unzip(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "extract")) return cmd_extract(argv[2], argv[3]);
    fprintf(stderr, "usage: ccos-files copy|move DEST SRC... | zip OUT.zip SRC... | unzip FILE.zip DEST\n");
    return 1;
}
