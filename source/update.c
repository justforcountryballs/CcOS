/* ccos-update - updates ccos on its USB stick, keeping your files and apps
 *
 *   ccos-update check URL     ask GitHub (URL = .../releases/latest) what the newest ccos is
 *   ccos-update install SRC   SRC = a .ccupd file or its https:// address: download,
 *                             check the signature, put it in the other slot, and
 *                             make the next start try it
 *   ccos-update confirm       run once ccos has started: the new version works
 *   ccos-update status        version, slot, and whether this is a stick
 *
 * Output: one line per fact, for the desktop to read:
 *   check:   CURRENT v / LATEST v / PAGE url / URL url / SIZE n / NEWER | UPTODATE | ERR msg
 *            (release notes go to /run/ccos/update-notes.txt)
 *   install: T total / P done / STEP text / OK version | ERR msg | CANCELED
 *   confirm: CONFIRMED v | ROLLEDBACK | NOTHING
 *
 * The stick: a FAT part with ccos twice, in two "slots":
 *   slot a  /boot/ccos/      bzImage, initramfs.cpio.gz
 *   slot b  /boot/ccos-b/
 * and /boot/grub/grubenv (GRUB's 1 KB settings block) saying which one to start:
 *   slot=a|b   prev=the other one   trying=1 after an install
 * GRUB: trying=1 -> sets trying=2 and starts the new slot. If ccos gets to the
 * desktop, "confirm" sets trying=0. If it doesn't (crash, power cut), the next
 * start finds trying=2 still there and goes back to prev: nothing is lost.
 * Files, settings and apps live on the other part of the stick (ccos-store);
 * updates never touch it.
 *
 * Update file (.ccupd):
 *   "CCUPD1\n"
 *   "SIG <128 hex>\n"         ed25519 signature of the header below
 *   header: "version 1.1\n", "file NAME SIZE SHA256\n"..., "end\n"
 *   then the files' bytes, one after another, in header order
 * Only updates signed with the ccos release key are installed.
 *
 * build: gcc -O2 -o ccos-update update.c -lcurl-gnutls -lsodium
 */
#include <curl/curl.h>
#include <sodium.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define PUBKEY "6c48d13d610a19940f6a196a4a1348de1626a8cf2dc668915b649c0d7e531c7f"
static char STICK_[256] = "/mnt/usb";              /* CCOS_STICK=dir overrides (for testing) */
#define STICK STICK_
static char ENVF[300];
#define NOTES "/run/ccos/update-notes.txt"

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }
static void say(const char *fmt, const char *a) { printf(fmt, a); putchar('\n'); fflush(stdout); }
static void die(const char *m) { say("ERR %s", m); exit(1); }

static void my_version(char *out, size_t n) {
    snprintf(out, n, "1.0");
    FILE *f = fopen("/etc/ccos-version", "r");
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0; fclose(f); }
}
/* "1.10" > "1.9"; a leading v is ignored */
static int vcmp(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    while (*a || *b) {
        char *ea, *eb; long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        if ((*ea && *ea != '.') || (*eb && *eb != '.')) return strcmp(ea, eb);
        a = *ea ? ea + 1 : ea; b = *eb ? eb + 1 : eb;
    }
    return 0;
}
static char cur_slot(void) {
    char c[2048] = ""; FILE *f = fopen("/proc/cmdline", "r");
    if (f) { if (!fgets(c, sizeof c, f)) c[0] = 0; fclose(f); }
    char *s = strstr(c, "ccos.slot=");
    return s && s[10] == 'b' ? 'b' : 'a';
}
static int sp_ok(const char *rel, int mode) { char p[400]; snprintf(p, sizeof p, "%s%s", STICK, rel); return access(p, mode) == 0; }
static int on_stick(void) {
    return sp_ok("/ccos/ccos.txt", F_OK) && sp_ok("/boot", W_OK);
}

/* ---- GRUB's environment block: "# GRUB Environment Block\n" + key=value lines, '#' to 1024 bytes ---- */
static char env_k[16][32], env_v[16][96]; static int env_n;
static void env_load(void) {
    env_n = 0;
    FILE *f = fopen(ENVF, "r"); if (!f) return;
    char l[256];
    while (fgets(l, sizeof l, f) && env_n < 16) {
        l[strcspn(l, "\n")] = 0;
        char *eq = strchr(l, '=');
        if (l[0] == '#' || !eq) continue;
        *eq = 0;
        snprintf(env_k[env_n], 32, "%s", l); snprintf(env_v[env_n], 96, "%s", eq + 1); env_n++;
    }
    fclose(f);
}
static const char *env_get(const char *k) { for (int i = 0; i < env_n; i++) if (!strcmp(env_k[i], k)) return env_v[i]; return ""; }
static void env_set(const char *k, const char *v) {
    for (int i = 0; i < env_n; i++) if (!strcmp(env_k[i], k)) { snprintf(env_v[i], 96, "%s", v); return; }
    if (env_n < 16) { snprintf(env_k[env_n], 32, "%s", k); snprintf(env_v[env_n], 96, "%s", v); env_n++; }
}
static int env_save(void) {
    char b[1024]; memset(b, '#', sizeof b);
    int p = snprintf(b, sizeof b, "# GRUB Environment Block\n");
    for (int i = 0; i < env_n; i++) {
        if (!env_v[i][0]) continue;
        int k = snprintf(b + p, sizeof b - (size_t)p, "%s=%s\n", env_k[i], env_v[i]);
        if (k < 0 || p + k >= (int)sizeof b) return -1;
        p += k;
    }
    b[p] = '#';                                          /* snprintf's 0 */
    int fd = open(ENVF, O_WRONLY | O_CREAT, 0644);       /* in place: GRUB writes to the same spot */
    if (fd < 0) return -1;
    int ok = pwrite(fd, b, sizeof b, 0) == (ssize_t)sizeof b && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

/* ---- downloads ---- */
struct mem { char *p; size_t n; };
static size_t to_mem(void *d, size_t s, size_t n, void *u) {
    struct mem *m = u; size_t k = s * n;
    if (m->n + k > (8u << 20)) return 0;
    char *q = realloc(m->p, m->n + k + 1); if (!q) return 0;
    memcpy(q + m->n, d, k); m->p = q; m->n += k; m->p[m->n] = 0; return k;
}
static CURL *web(const char *url) {
    CURL *c = curl_easy_init(); if (!c) die("Couldn't start downloading");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ccos-updater");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) curl_easy_setopt(c, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    return c;
}
static const char *web_err(CURLcode r, CURL *c) {
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (code == 404) return "No ccos release was found on GitHub yet";
    if (code == 403) return "GitHub said 'too many requests' - try again in an hour";
    if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT) return "No internet connection";
    if (r == CURLE_OPERATION_TIMEDOUT) return "The connection is too slow or stopped";
    if (r == CURLE_WRITE_ERROR) return "Not enough room for the download";
    return curl_easy_strerror(r);
}

/* a "key": "string" value in GitHub's JSON, unescaped (from p on) */
static char *json_str(const char *p, const char *key, char *out, size_t n) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *s = strstr(p, pat); out[0] = 0;
    if (!s) return NULL;
    s += strlen(pat);
    while (*s == ' ' || *s == ':' || *s == '\n' || *s == '\t' || *s == '\r') s++;
    if (*s != '"') return (char *)s;
    s++;
    size_t k = 0;
    while (*s && *s != '"') {
        unsigned c = (unsigned char)*s++;
        if (c == '\\' && *s) {
            char e = *s++;
            if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'r') c = '\r';
            else if (e == 'u') {
                unsigned v = 0; for (int i = 0; i < 4 && *s; i++, s++) v = v * 16 + (unsigned)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
                char u[4]; int m = 0;
                if (v < 0x80) u[m++] = (char)v;
                else if (v < 0x800) { u[m++] = (char)(0xC0 | v >> 6); u[m++] = (char)(0x80 | (v & 63)); }
                else { u[m++] = (char)(0xE0 | v >> 12); u[m++] = (char)(0x80 | ((v >> 6) & 63)); u[m++] = (char)(0x80 | (v & 63)); }
                for (int i = 0; i < m && k + 1 < n; i++) out[k++] = u[i];
                continue;
            } else c = (unsigned char)e;
        }
        if (c == '\r') continue;
        if (k + 1 < n) out[k++] = (char)c;
    }
    out[k] = 0;
    return (char *)s;
}

static int cmd_check(const char *url) {
    char me[32]; my_version(me, sizeof me);
    say("CURRENT %s", me);
    struct mem m = { 0 };
    CURL *c = web(url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
    struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    CURLcode r = curl_easy_perform(c);
    if (r != CURLE_OK) die(web_err(r, c));
    char tag[64], page[300], body[16384], dl[600] = "", nm[200];
    json_str(m.p, "tag_name", tag, sizeof tag);
    if (!tag[0]) die("GitHub's answer didn't make sense");
    json_str(m.p, "html_url", page, sizeof page);
    json_str(m.p, "body", body, sizeof body);
    long size = 0;
    for (const char *a = strstr(m.p, "\"assets\""); a && (a = strstr(a, "\"browser_download_url\"")); a++) {
        char u[600]; json_str(a, "browser_download_url", u, sizeof u);
        size_t l = strlen(u);
        if (l > 6 && !strcmp(u + l - 6, ".ccupd")) {
            snprintf(dl, sizeof dl, "%s", u);
            /* its size: the "size" before this url, in the same asset */
            const char *st = a; while (st > m.p && strncmp(st, "\"size\"", 6)) st--;
            if (st > m.p) size = strtol(st + 7, NULL, 10);
            break;
        }
    }
    (void)nm;
    mkdir("/run/ccos", 0755);
    FILE *f = fopen(NOTES, "w"); if (f) { fputs(body, f); fclose(f); }
    const char *v = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
    say("LATEST %s", v);
    say("PAGE %s", page);
    if (dl[0]) { say("URL %s", dl); printf("SIZE %ld\n", size); }
    say("%s", vcmp(v, me) > 0 ? "NEWER" : "UPTODATE");
    return 0;
}

/* ---- install ---- */
static char partial[600];
static void cleanup(void) { if (partial[0]) unlink(partial); }
static int dl_progress(void *u, curl_off_t tot, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)u; (void)ut; (void)un;
    static curl_off_t last = -1; static int said;
    if (tot > 0 && !said) { printf("T %lld\n", (long long)tot); said = 1; }
    if (now - last >= (1 << 20) || now == tot) { printf("P %lld\n", (long long)now); fflush(stdout); last = now; }
    return stop ? 1 : 0;
}
static void download(const char *url, const char *to) {
    say("STEP %s", "Downloading");
    snprintf(partial, sizeof partial, "%s", to);
    FILE *f = fopen(to, "wb"); if (!f) die("Couldn't save the download");
    CURL *c = web(url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
    CURLcode r = curl_easy_perform(c);
    if (fclose(f) != 0 && r == CURLE_OK) die("Not enough room for the download");
    if (stop) { say("%s", "CANCELED"); exit(2); }
    if (r != CURLE_OK) die(web_err(r, c));
    curl_easy_cleanup(c);
}

struct ufile { char name[64]; long long size; char sha[65]; };
/* copy n bytes of in to path (via path.new), checking the hash; 0 = ok */
static int put_file(FILE *in, const struct ufile *u, const char *path, long long *done, long long total) {
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s.new", path);
    snprintf(partial, sizeof partial, "%s", tmp);
    FILE *o = fopen(tmp, "wb");
    if (!o) die("Couldn't write to the USB stick");
    crypto_hash_sha256_state h; crypto_hash_sha256_init(&h);
    static unsigned char b[1 << 18];
    long long left = u->size;
    while (left > 0) {
        if (stop) { say("%s", "CANCELED"); exit(2); }
        size_t k = fread(b, 1, left < (long long)sizeof b ? (size_t)left : sizeof b, in);
        if (!k) die("The update file is cut short (download it again)");
        if (fwrite(b, 1, k, o) != k) die("Not enough room on the USB stick's ccos part");
        crypto_hash_sha256_update(&h, b, k);
        left -= (long long)k; *done += (long long)k;
        printf("P %lld\n", *done * 1000 / (total ? total : 1)); fflush(stdout);
    }
    if (fflush(o) || fsync(fileno(o)) || fclose(o)) die("Couldn't write to the USB stick");
    unsigned char d[32]; char hx[65]; crypto_hash_sha256_final(&h, d);
    sodium_bin2hex(hx, sizeof hx, d, 32);
    if (strcmp(hx, u->sha)) die("The update file is damaged (download it again)");
    return 0;
}
static int same_file(const char *path, const char *sha) {     /* already has this content? */
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    crypto_hash_sha256_state h; crypto_hash_sha256_init(&h);
    static unsigned char b[1 << 16]; size_t k;
    while ((k = fread(b, 1, sizeof b, f))) crypto_hash_sha256_update(&h, b, k);
    fclose(f);
    unsigned char d[32]; char hx[65]; crypto_hash_sha256_final(&h, d); sodium_bin2hex(hx, sizeof hx, d, 32);
    return !strcmp(hx, sha);
}

static int cmd_install(const char *src) {
    if (!on_stick()) die("Updates install on a ccos USB stick (in QEMU, get the new ccos.zip instead)");
    char pkg[600];
    int downloaded = 0;
    if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8)) {
        const char *room = access("/mnt/store", W_OK) == 0 ? "/mnt/store" : access("/data", W_OK) == 0 ? "/data" : "/tmp";
        snprintf(pkg, sizeof pkg, "%s/ccos-update.ccupd", room);
        download(src, pkg);
        downloaded = 1;
    } else snprintf(pkg, sizeof pkg, "%s", src);
    partial[0] = 0;
    say("STEP %s", "Checking the update");
    FILE *in = fopen(pkg, "rb");
    if (!in) die("Couldn't open the update file");
    char l[512], sighex[200] = "";
    if (!fgets(l, sizeof l, in) || strcmp(l, "CCUPD1\n")) die("This isn't a ccos update file");
    if (!fgets(l, sizeof l, in) || strncmp(l, "SIG ", 4)) die("This update file isn't signed");
    snprintf(sighex, sizeof sighex, "%.128s", l + 4);
    char hdr[8192]; size_t hn = 0;
    struct ufile uf[8]; int nf = 0; char ver[64] = "";
    for (;;) {
        if (!fgets(l, sizeof l, in)) die("This update file is damaged");
        size_t k = strlen(l);
        if (hn + k >= sizeof hdr) die("This update file is damaged");
        memcpy(hdr + hn, l, k); hn += k;
        if (!strcmp(l, "end\n")) break;
        if (!strncmp(l, "version ", 8)) { snprintf(ver, sizeof ver, "%s", l + 8); ver[strcspn(ver, "\n")] = 0; }
        else if (!strncmp(l, "file ", 5) && nf < 8) {
            if (sscanf(l + 5, "%63s %lld %64s", uf[nf].name, &uf[nf].size, uf[nf].sha) == 3) nf++;
        }
    }
    unsigned char pk[32], sig[64];
    if (sodium_hex2bin(pk, 32, PUBKEY, 64, NULL, NULL, NULL) || sodium_hex2bin(sig, 64, sighex, 128, NULL, NULL, NULL) ||
        crypto_sign_verify_detached(sig, (unsigned char *)hdr, hn, pk) != 0)
        die("This update isn't signed by ccos: it was not installed");
    int need[2] = { 0, 0 };
    for (int i = 0; i < nf; i++) {
        if (strchr(uf[i].name, '/')) die("This update file is damaged");
        if (!strcmp(uf[i].name, "bzImage")) need[0] = 1;
        if (!strcmp(uf[i].name, "initramfs.cpio.gz")) need[1] = 1;
    }
    if (!need[0] || !need[1] || !ver[0]) die("This update file is missing parts");
    char me[32]; my_version(me, sizeof me);
    /* room on the stick's ccos part */
    long long total = 0; for (int i = 0; i < nf; i++) total += uf[i].size;
    struct statvfs sv;
    char from = cur_slot(), to = from == 'a' ? 'b' : 'a';
    char dir[400]; snprintf(dir, sizeof dir, "%s/boot/%s", STICK, to == 'a' ? "ccos" : "ccos-b");
    if (statvfs(STICK, &sv) == 0 && (long long)sv.f_bavail * (long long)sv.f_frsize < total + (8 << 20)) {
        /* the other slot's old files make room when they go */
        char p[400]; snprintf(p, sizeof p, "%s/initramfs.cpio.gz", dir); unlink(p);
        snprintf(p, sizeof p, "%s/bzImage", dir); unlink(p);
        if (statvfs(STICK, &sv) == 0 && (long long)sv.f_bavail * (long long)sv.f_frsize < total + (4 << 20))
            die("Not enough room on the USB stick's ccos part");
    }
    { char bd[300]; snprintf(bd, sizeof bd, "%s/boot", STICK); mkdir(bd, 0755); } mkdir(dir, 0755);
    say("STEP %s", "Installing");
    printf("T 1000\n");
    long long done = 0;
    /* GRUB stops trusting the other slot before its files change */
    env_load();
    if (!strcmp(env_get("prev"), (char[]){ to, 0 })) { env_set("prev", ""); env_save(); }
    for (int i = 0; i < nf; i++) {
        char path[400];
        if (!strcmp(uf[i].name, "BOOTX64.EFI")) snprintf(path, sizeof path, "%s/EFI/BOOT/BOOTX64.EFI", STICK);
        else if (!strcmp(uf[i].name, "grub.cfg")) snprintf(path, sizeof path, "%s/boot/grub/grub.cfg", STICK);
        else snprintf(path, sizeof path, "%s/%s", dir, uf[i].name);
        uf[i].name[63] = 0;
        int shared = !strcmp(uf[i].name, "BOOTX64.EFI") || !strcmp(uf[i].name, "grub.cfg");
        if (shared && same_file(path, uf[i].sha)) {             /* unchanged: leave it alone */
            if (fseek(in, uf[i].size, SEEK_CUR)) die("This update file is damaged");
            done += uf[i].size;
            continue;
        }
        put_file(in, &uf[i], path, &done, total);
        char tmp[700]; snprintf(tmp, sizeof tmp, "%s.new", path);
        if (!shared) { if (rename(tmp, path)) die("Couldn't write to the USB stick"); }
        uf[i].size = -1;                                    /* placed; shared files go in last */
    }
    fclose(in);
    /* GRUB's own files last, each swapped in one step */
    for (int i = 0; i < nf; i++) {
        if (uf[i].size != -1) continue;
        char path[400];
        if (!strcmp(uf[i].name, "BOOTX64.EFI")) snprintf(path, sizeof path, "%s/EFI/BOOT/BOOTX64.EFI", STICK);
        else if (!strcmp(uf[i].name, "grub.cfg")) snprintf(path, sizeof path, "%s/boot/grub/grub.cfg", STICK);
        else continue;
        char tmp[700]; snprintf(tmp, sizeof tmp, "%s.new", path);
        if (rename(tmp, path)) die("Couldn't write to the USB stick");
    }
    partial[0] = 0;
    /* the next start tries the new one */
    env_load();
    char a[2] = { to, 0 }, b[2] = { from, 0 };
    env_set("slot", a); env_set("prev", b); env_set("trying", "1"); env_set("rolledback", "");
    char vv[96]; snprintf(vv, sizeof vv, "%s", ver); env_set("version_new", vv);
    snprintf(vv, sizeof vv, "%s", me); env_set("version_old", vv);
    if (env_save()) die("Couldn't write GRUB's settings on the stick");
    sync();
    if (downloaded) unlink(pkg);
    say("OK %s", ver);
    return 0;
}

static int cmd_confirm(void) {
    if (!on_stick()) { say("%s", "NOTHING"); return 0; }
    env_load();
    char me = cur_slot(), s[2] = { me, 0 };
    const char *tr = env_get("trying");
    if (env_get("rolledback")[0]) {
        env_set("rolledback", ""); env_set("trying", ""); env_set("slot", s);
        env_save(); sync();
        say("ROLLEDBACK %s", env_get("version_new")[0] ? env_get("version_new") : "the update");
        return 0;
    }
    if (tr[0] && strcmp(tr, "0")) {
        if (strcmp(env_get("slot"), s)) {                /* started the previous version by hand: keep it */
            env_set("prev", env_get("slot")); env_set("slot", s);
        }
        env_set("trying", "");
        env_save(); sync();
        char v[32]; my_version(v, sizeof v);
        say("CONFIRMED %s", v);
        return 0;
    }
    if (strcmp(env_get("slot"), s) && env_get("slot")[0]) {   /* picked "previous version" in the menu */
        env_set("prev", env_get("slot")); env_set("slot", s); env_save(); sync();
    }
    say("%s", "NOTHING");
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    atexit(cleanup);
    if (getenv("CCOS_STICK")) snprintf(STICK_, sizeof STICK_, "%s", getenv("CCOS_STICK"));
    snprintf(ENVF, sizeof ENVF, "%s/boot/grub/grubenv", STICK);
    if (sodium_init() < 0) die("Couldn't start");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc == 3 && !strcmp(argv[1], "check")) return cmd_check(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "install")) return cmd_install(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "confirm")) return cmd_confirm();
    if (argc == 2 && !strcmp(argv[1], "status")) {
        char v[32]; my_version(v, sizeof v);
        say("VERSION %s", v);
        printf("SLOT %c\nSTICK %s\n", cur_slot(), on_stick() ? "yes" : "no");
        return 0;
    }
    fprintf(stderr, "usage: ccos-update check URL | install FILE|URL | confirm | status\n");
    return 1;
}
