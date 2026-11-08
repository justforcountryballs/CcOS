/*
 * ccos-store-index - turn Flathub's app catalog into a small list for Software Manager
 *
 * Flatpak downloads Flathub's catalog (AppStream XML, ~50 MB) with
 *   flatpak update --appstream flathub
 * This reads it once and writes one line per app, fields separated by tabs:
 *   id  name  summary  developer  categories(;)  icon-file  licence  version  homepage  description
 * (tabs/newlines inside fields become spaces / the two characters "\n").
 *
 * Usage: ccos-store-index appstream.xml[.gz] out-file
 * Build: gcc -O2 -o ccos-store-index store_index.c -lz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static char *buf; static size_t blen;

static int load(const char *path) {
    gzFile f = gzopen(path, "rb");
    if (!f) return -1;
    size_t cap = 64 << 20;
    buf = malloc(cap);
    int n;
    while (buf && (n = gzread(f, buf + blen, (unsigned)(cap - blen - 1))) > 0) {
        blen += (size_t)n;
        if (cap - blen < (8 << 20)) { cap *= 2; buf = realloc(buf, cap); }
    }
    gzclose(f);
    if (!buf) return -1;
    buf[blen] = 0;
    return 0;
}

/* growable string */
typedef struct { char *s; size_t n, cap; } str;
static void sadd(str *a, const char *p, size_t n) {
    if (a->n + n + 1 > a->cap) { a->cap = (a->n + n + 1) * 2 + 64; a->s = realloc(a->s, a->cap); }
    memcpy(a->s + a->n, p, n); a->n += n; a->s[a->n] = 0;
}
static void sc(str *a, const char *p) { sadd(a, p, strlen(p)); }
static void sclear(str *a) { a->n = 0; if (a->s) a->s[0] = 0; }

/* append text with XML entities decoded; spaces collapsed */
static void add_text(str *a, const char *p, const char *e) {
    while (p < e) {
        if (*p == '&') {
            const char *semi = memchr(p, ';', (size_t)(e - p));
            if (semi && semi - p < 10) {
                char ent[12]; size_t L = (size_t)(semi - p - 1); memcpy(ent, p + 1, L); ent[L] = 0;
                if (!strcmp(ent, "amp")) sc(a, "&");
                else if (!strcmp(ent, "lt")) sc(a, "<");
                else if (!strcmp(ent, "gt")) sc(a, ">");
                else if (!strcmp(ent, "quot")) sc(a, "\"");
                else if (!strcmp(ent, "apos")) sc(a, "'");
                else if (ent[0] == '#') {
                    unsigned long cp = ent[1] == 'x' ? strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10);
                    char u[5] = { 0 };
                    if (cp < 0x80) u[0] = (char)cp;
                    else if (cp < 0x800) { u[0] = (char)(0xc0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { u[0] = (char)(0xe0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63)); u[2] = (char)(0x80 | (cp & 63)); }
                    else { u[0] = (char)(0xf0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 63)); u[2] = (char)(0x80 | ((cp >> 6) & 63)); u[3] = (char)(0x80 | (cp & 63)); }
                    sc(a, u);
                }
                p = semi + 1;
                continue;
            }
        }
        if (*p == '\t' || *p == '\n' || *p == '\r' || *p == ' ') {
            if (a->n && a->s[a->n - 1] != ' ' && a->s[a->n - 1] != '\n') sadd(a, " ", 1);
            p++;
            continue;
        }
        sadd(a, p, 1); p++;
    }
}
static void trim_end(str *a) { while (a->n && (a->s[a->n - 1] == ' ')) a->s[--a->n] = 0; }

/* find the end of the element starting at p (just after its opening tag), honouring nesting */
static const char *elem_end(const char *p, const char *tag) {
    size_t L = strlen(tag); int depth = 1;
    while ((p = strchr(p, '<'))) {
        if (p[1] == '/' && !strncmp(p + 2, tag, L) && (p[2 + L] == '>' || p[2 + L] == ' ')) { if (--depth == 0) return p; }
        else if (!strncmp(p + 1, tag, L) && (p[1 + L] == '>' || p[1 + L] == ' ')) {
            const char *gt = strchr(p, '>');
            if (gt && gt[-1] != '/') depth++;
        }
        p++;
    }
    return NULL;
}

static int has_lang(const char *tag_start, const char *gt) {
    for (const char *q = tag_start; q < gt; q++) if (!strncmp(q, "xml:lang", 8)) return 1;
    return 0;
}
static int attr(const char *tag_start, const char *gt, const char *name, char *out, size_t n) {
    size_t L = strlen(name);
    for (const char *q = tag_start; q + L + 2 < gt; q++)
        if (!strncmp(q, name, L) && q[L] == '=' && q[L + 1] == '"' && (q[-1] == ' ')) {
            const char *v = q + L + 2, *e = strchr(v, '"');
            if (!e || e > gt) return 0;
            snprintf(out, n, "%.*s", (int)(e - v), v);
            return 1;
        }
    return 0;
}

/* the description: <p> paragraphs and <li> list items as plain text */
static void description(str *out, const char *p, const char *e) {
    while (p < e) {
        const char *lt = memchr(p, '<', (size_t)(e - p));
        if (!lt) { add_text(out, p, e); break; }
        add_text(out, p, lt);
        const char *gt = memchr(lt, '>', (size_t)(e - lt));
        if (!gt) break;
        if (!strncmp(lt, "<p", 2) && (lt[2] == '>' || lt[2] == ' ')) { trim_end(out); if (out->n) sc(out, "\n\n"); }
        else if (!strncmp(lt, "<li", 3)) { trim_end(out); if (out->n) sc(out, "\n"); sc(out, "\xe2\x80\xa2 "); }
        else if (!strncmp(lt, "</ul", 4) || !strncmp(lt, "</ol", 4)) { trim_end(out); }
        p = gt + 1;
    }
    trim_end(out);
}

static void put(FILE *o, const str *a) {          /* one field, tabs and newlines made safe */
    for (size_t i = 0; i < a->n; i++) {
        char ch = a->s[i];
        if (ch == '\t') fputc(' ', o);
        else if (ch == '\n') fputs("\\n", o);
        else if (ch == '\\') fputs("\\\\", o);
        else fputc(ch, o);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: ccos-store-index appstream.xml[.gz] out\n"); return 1; }
    if (load(argv[1]) < 0) { perror(argv[1]); return 1; }
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp", argv[2]);
    FILE *o = fopen(tmp, "w");
    if (!o) { perror(tmp); return 1; }
    str id = { 0 }, name = { 0 }, sum = { 0 }, dev = { 0 }, cats = { 0 }, icon = { 0 }, lic = { 0 }, ver = { 0 }, home = { 0 }, desc = { 0 }, ref = { 0 };
    int count = 0;
    const char *p = buf;
    while ((p = strstr(p, "<component"))) {
        if (p[10] != ' ' && p[10] != '>') { p += 10; continue; }   /* not <components> */
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char type[40] = "";
        attr(p, gt, "type", type, sizeof type);
        const char *end = elem_end(gt + 1, "component");
        if (!end) break;
        if (strcmp(type, "desktop") && strcmp(type, "desktop-application") && strcmp(type, "console-application")) { p = end; continue; }
        sclear(&id); sclear(&name); sclear(&sum); sclear(&dev); sclear(&cats); sclear(&icon);
        sclear(&lic); sclear(&ver); sclear(&home); sclear(&desc); sclear(&ref);
        int icon_size = 0;
        /* walk the component's direct children */
        const char *q = gt + 1;
        while (q < end) {
            const char *lt = strchr(q, '<');
            if (!lt || lt >= end) break;
            if (lt[1] == '/' || lt[1] == '!' || lt[1] == '?') { q = strchr(lt, '>') + 1; continue; }
            const char *tg = strchr(lt, '>');
            int self = tg[-1] == '/';
            char tag[32]; int tl = 0;
            for (const char *c = lt + 1; *c && *c != ' ' && *c != '>' && *c != '/' && tl < 31; c++) tag[tl++] = *c;
            tag[tl] = 0;
            const char *ce = self ? tg : elem_end(tg + 1, tag);
            if (!ce || ce > end) break;
            int lang = has_lang(lt, tg);
            if (!self && !lang) {
                if (!strcmp(tag, "id") && !id.n) add_text(&id, tg + 1, ce);
                else if (!strcmp(tag, "name") && !name.n) add_text(&name, tg + 1, ce);
                else if (!strcmp(tag, "summary") && !sum.n) add_text(&sum, tg + 1, ce);
                else if (!strcmp(tag, "developer_name") && !dev.n) add_text(&dev, tg + 1, ce);
                else if (!strcmp(tag, "developer") && !dev.n) {
                    const char *nm = strstr(tg, "<name>");
                    if (nm && nm < ce) { const char *ne = strstr(nm, "</name>"); if (ne && ne < ce) add_text(&dev, nm + 6, ne); }
                }
                else if (!strcmp(tag, "project_license") && !lic.n) add_text(&lic, tg + 1, ce);
                else if (!strcmp(tag, "description") && !desc.n) description(&desc, tg + 1, ce);
                else if (!strcmp(tag, "bundle") && !ref.n) add_text(&ref, tg + 1, ce);
                else if (!strcmp(tag, "url")) {
                    char ut[24] = ""; attr(lt, tg, "type", ut, sizeof ut);
                    if (!strcmp(ut, "homepage") && !home.n) add_text(&home, tg + 1, ce);
                }
                else if (!strcmp(tag, "icon")) {
                    char it[16] = "", h[8] = "";
                    attr(lt, tg, "type", it, sizeof it); attr(lt, tg, "height", h, sizeof h);
                    int hh = atoi(h);
                    if (!strcmp(it, "cached") && (hh == 64 || (hh == 128 && icon_size != 64))) {
                        sclear(&icon); add_text(&icon, tg + 1, ce); icon_size = hh;
                    }
                }
                else if (!strcmp(tag, "categories")) {
                    const char *c = tg + 1;
                    while ((c = strstr(c, "<category>")) && c < ce) {
                        const char *ee = strstr(c, "</category>");
                        if (!ee || ee > ce) break;
                        if (cats.n) sc(&cats, ";");
                        add_text(&cats, c + 10, ee);
                        c = ee;
                    }
                }
                else if (!strcmp(tag, "releases") && !ver.n) {
                    const char *r = strstr(tg, "<release");
                    if (r && r < ce) { const char *rg = strchr(r, '>'); char v[48] = ""; if (rg && attr(r, rg, "version", v, sizeof v)) sc(&ver, v); }
                }
            }
            q = self ? tg + 1 : strchr(ce, '>') + 1;
        }
        /* the app's Flatpak name: from <bundle> app/ID/arch/branch, else <id> without .desktop */
        str appid = { 0 };
        if (ref.n > 4 && !strncmp(ref.s, "app/", 4)) { char *sl = strchr(ref.s + 4, '/'); sadd(&appid, ref.s + 4, sl ? (size_t)(sl - ref.s - 4) : ref.n - 4); }
        else if (id.n) { sadd(&appid, id.s, id.n); if (appid.n > 8 && !strcmp(appid.s + appid.n - 8, ".desktop")) appid.s[appid.n -= 8] = 0; }
        if (appid.n && name.n && strncmp(ref.s ? ref.s : "", "runtime/", 8)) {
            const str *f[] = { &appid, &name, &sum, &dev, &cats, &icon, &lic, &ver, &home, &desc };
            for (int i = 0; i < 10; i++) { if (i) fputc('\t', o); put(o, f[i]); }
            fputc('\n', o);
            count++;
        }
        free(appid.s);
        p = end;
    }
    fclose(o);
    if (rename(tmp, argv[2]) != 0) { perror(argv[2]); return 1; }
    fprintf(stderr, "ccos-store-index: %d apps\n", count);
    return 0;
}
