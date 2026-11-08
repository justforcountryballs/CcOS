/* ccos-print - printing for ccos, straight to network printers (no CUPS)
 *
 * Almost every Wi-Fi printer made in the last ten years speaks IPP
 * ("IPP Everywhere" / AirPrint): you find it on the network with mDNS (the
 * same way phones do), ask it what it can do, and send it the pages over
 * HTTP. No driver needed. ccos makes the pages itself.
 *
 *   ccos-print list                       printers on the network (2-3 s):
 *                                           P name|uri|make and model|color
 *   ccos-print caps URI                   what a printer can do, one per line:
 *                                           state=idle|printing|stopped
 *                                           formats=..., media=..., color=1, ...
 *   ccos-print image FILE URI [options]   print a picture; options:
 *       --media iso_a4_210x297mm   paper size (a PWG media name)
 *       --orient auto|portrait|landscape
 *       --fit fit|fill             whole picture, or fill the page (crops)
 *       --mono                     black and white
 *       --quality draft|normal|high
 *       --copies N
 *       --borderless               no white margins (if the printer can)
 *     prints progress lines: "S sending 40", "S printing", "OK", "ERR message"
 *
 * Build: gcc -O2 -o ccos-print print.c -lz -lm
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "third/stb_image.h"
#define NANOSVG_IMPLEMENTATION
#include "third/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third/nanosvgrast.h"

static long long now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000LL + tv.tv_usec / 1000; }

/* =====================================================================
 *  mDNS: find printers (_ipp._tcp and _ipps._tcp)
 * ===================================================================== */
#define MAXREC 64
static struct { char inst[256]; char host[256]; int port; char rp[128], ty[128]; int color, ipps; } svc[MAXREC];
static int nsvc;
static struct { char host[256]; char ip[64]; } addrs[MAXREC];
static int naddr;
static char ptrs[MAXREC][256]; static int nptr; static int ptr_ipps[MAXREC];

/* read a (possibly compressed) DNS name at *off; returns 0 on error */
static int dns_name(const unsigned char *m, int len, int *off, char *out, int outn) {
    int o = *off, jumped = 0, n = 0, guard = 0;
    out[0] = 0;
    while (o < len && guard++ < 128) {
        int l = m[o];
        if (l == 0) { o++; break; }
        if ((l & 0xC0) == 0xC0) {
            if (o + 1 >= len) return 0;
            int p = ((l & 0x3F) << 8) | m[o + 1];
            if (!jumped) *off = o + 2;
            jumped = 1; o = p; continue;
        }
        o++;
        if (o + l > len) return 0;
        if (n && n < outn - 1) out[n++] = '.';
        for (int i = 0; i < l && n < outn - 1; i++) {
            char c = (char)m[o + i];
            if (c == '.' || c == '\\') { if (n < outn - 2) { out[n++] = '\\'; } }
            out[n++] = c;
        }
        o += l;
    }
    out[n] = 0;
    if (!jumped) *off = o;
    return 1;
}
static void dns_put_name(unsigned char *b, int *o, const char *name) {
    const char *s = name;
    while (*s) {
        const char *d = strchr(s, '.');
        int l = d ? (int)(d - s) : (int)strlen(s);
        b[(*o)++] = (unsigned char)l; memcpy(b + *o, s, (size_t)l); *o += l;
        s += l; if (*s == '.') s++;
    }
    b[(*o)++] = 0;
}
static int svc_find(const char *inst) {
    for (int i = 0; i < nsvc; i++) if (!strcasecmp(svc[i].inst, inst)) return i;
    if (nsvc >= MAXREC) return -1;
    memset(&svc[nsvc], 0, sizeof svc[0]);
    snprintf(svc[nsvc].inst, sizeof svc[0].inst, "%s", inst);
    svc[nsvc].port = 631;
    return nsvc++;
}
static void dns_parse(const unsigned char *m, int len) {
    if (len < 12) return;
    int qd = m[4] << 8 | m[5], an = m[6] << 8 | m[7], ns = m[8] << 8 | m[9], ar = m[10] << 8 | m[11];
    int o = 12; char name[256], t[256];
    for (int i = 0; i < qd; i++) { if (!dns_name(m, len, &o, name, sizeof name)) return; o += 4; }
    for (int i = 0; i < an + ns + ar; i++) {
        if (!dns_name(m, len, &o, name, sizeof name) || o + 10 > len) return;
        int type = m[o] << 8 | m[o + 1], rdl = m[o + 8] << 8 | m[o + 9];
        o += 10;
        if (o + rdl > len) return;
        int r = o;
        if (type == 12) {                                           /* PTR: a printer's name */
            if (dns_name(m, len, &r, t, sizeof t) && nptr < MAXREC) {
                int dup = 0; for (int k = 0; k < nptr; k++) if (!strcasecmp(ptrs[k], t)) dup = 1;
                if (!dup) { ptr_ipps[nptr] = strstr(name, "_ipps.") != NULL; snprintf(ptrs[nptr++], 256, "%s", t); }
            }
        } else if (type == 33 && rdl > 6) {                         /* SRV: host and port */
            int k = svc_find(name);
            if (k >= 0) { svc[k].port = m[o + 4] << 8 | m[o + 5]; r = o + 6; dns_name(m, len, &r, svc[k].host, sizeof svc[k].host); }
        } else if (type == 16) {                                    /* TXT: path, model, colour */
            int k = svc_find(name);
            for (int p = o; k >= 0 && p < o + rdl; ) {
                int l = m[p++]; if (p + l > o + rdl) break;
                char kv[300]; snprintf(kv, sizeof kv, "%.*s", l, (const char *)m + p); p += l;
                if (!strncasecmp(kv, "rp=", 3)) snprintf(svc[k].rp, sizeof svc[k].rp, "%s", kv + 3);
                else if (!strncasecmp(kv, "ty=", 3)) snprintf(svc[k].ty, sizeof svc[k].ty, "%s", kv + 3);
                else if (!strncasecmp(kv, "Color=", 6)) svc[k].color = toupper((unsigned char)kv[6]) == 'T';
            }
        } else if (type == 1 && rdl == 4 && naddr < MAXREC) {       /* A: the IPv4 address */
            snprintf(addrs[naddr].host, sizeof addrs[0].host, "%s", name);
            snprintf(addrs[naddr].ip, sizeof addrs[0].ip, "%u.%u.%u.%u", m[o], m[o + 1], m[o + 2], m[o + 3]);
            naddr++;
        }
        o += rdl;
    }
}
static void mdns_query(int s, const char **names, int n, int type) {
    unsigned char b[512]; int o = 12;
    memset(b, 0, 12); b[5] = (unsigned char)n;
    for (int i = 0; i < n; i++) {
        dns_put_name(b, &o, names[i]);
        b[o++] = 0; b[o++] = (unsigned char)type; b[o++] = 0x80; b[o++] = 1;   /* class IN, "answer me directly" */
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    if (sendto(s, b, (size_t)o, 0, (struct sockaddr *)&to, sizeof to) < 0) { /* no network */ }
}
/* the instance name as people read it: the first label, unescaped */
static void pretty_name(const char *inst, char *out, size_t n) {
    size_t k = 0;
    for (const char *p = inst; *p && k + 1 < n; p++) {
        if (*p == '\\' && p[1]) { p++; out[k++] = *p; continue; }
        if (*p == '.') break;
        out[k++] = *p;
    }
    out[k] = 0;
}
static int cmd_list(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    unsigned char ttl = 255; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0) {      /* 5353 busy: any port works for asking */
        me.sin_port = 0; bind(s, (struct sockaddr *)&me, sizeof me);
    }
    struct ip_mreq mr; inet_pton(AF_INET, "224.0.0.251", &mr.imr_multiaddr); mr.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
    const char *q[] = { "_ipp._tcp.local", "_ipps._tcp.local" };
    long long start = now_ms(), next = start; int sent = 0;
    while (now_ms() - start < 2600) {
        if (now_ms() >= next && sent < 3) { mdns_query(s, q, 2, 12); sent++; next = now_ms() + 700; }
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 100) > 0) {
            unsigned char m[9000]; ssize_t r = recv(s, m, sizeof m, 0);
            if (r > 0) dns_parse(m, (int)r);
        }
        /* names we know but without details: ask for them directly */
        static int asked[MAXREC];
        for (int i = 0; i < nptr; i++) {
            int k = -1; for (int j = 0; j < nsvc; j++) if (!strcasecmp(svc[j].inst, ptrs[i])) k = j;
            if ((k < 0 || !svc[k].host[0]) && !asked[i]) { const char *n1[] = { ptrs[i] }; mdns_query(s, n1, 1, 255); asked[i] = 1; }
        }
    }
    close(s);
    /* one line per printer; the plain-IPP one wins over the secure one */
    char done[MAXREC][128]; int nd = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nptr; i++) {
            if (ptr_ipps[i] != pass) continue;
            int k = -1; for (int j = 0; j < nsvc; j++) if (!strcasecmp(svc[j].inst, ptrs[i])) k = j;
            if (k < 0 || !svc[k].host[0]) continue;
            const char *ip = NULL;
            for (int a = 0; a < naddr; a++) if (!strcasecmp(addrs[a].host, svc[k].host)) ip = addrs[a].ip;
            if (!ip) continue;
            char nm[128]; pretty_name(ptrs[i], nm, sizeof nm);
            int dup = 0; for (int d = 0; d < nd; d++) if (!strcmp(done[d], nm)) dup = 1;
            if (dup) continue;
            snprintf(done[nd++], 128, "%s", nm);
            /* secure-only printers almost always take plain IPP on 631 too */
            int port = pass ? 631 : svc[k].port;
            printf("P %s|ipp://%s:%d/%s|%s|%d\n", nm, ip, port, svc[k].rp[0] ? svc[k].rp : "ipp/print",
                   svc[k].ty[0] ? svc[k].ty : nm, svc[k].color);
        }
    return 0;
}

/* =====================================================================
 *  IPP over HTTP
 * ===================================================================== */
typedef struct { unsigned char *b; size_t n, cap; } Buf;
static void bput(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 1024; b->b = realloc(b->b, b->cap); if (!b->b) { fputs("ERR out of memory\n", stdout); exit(1); } }
    memcpy(b->b + b->n, p, n); b->n += n;
}
static void bbyte(Buf *b, int v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
static void b16(Buf *b, int v) { bbyte(b, v >> 8); bbyte(b, v); }
static void b32(Buf *b, uint32_t v) { bbyte(b, v >> 24); bbyte(b, v >> 16); bbyte(b, v >> 8); bbyte(b, v); }
static void ipp_str(Buf *b, int tag, const char *name, const char *val) {
    bbyte(b, tag); b16(b, (int)strlen(name)); bput(b, name, strlen(name)); b16(b, (int)strlen(val)); bput(b, val, strlen(val));
}
static void ipp_int(Buf *b, int tag, const char *name, int v) {
    bbyte(b, tag); b16(b, (int)strlen(name)); bput(b, name, strlen(name)); b16(b, 4); b32(b, (uint32_t)v);
}
static void ipp_head(Buf *b, int op, int id, const char *uri) {
    bbyte(b, 2); bbyte(b, 0); b16(b, op); b32(b, (uint32_t)id);
    bbyte(b, 0x01);
    ipp_str(b, 0x47, "attributes-charset", "utf-8");
    ipp_str(b, 0x48, "attributes-natural-language", "en");
    ipp_str(b, 0x45, "printer-uri", uri);
    ipp_str(b, 0x42, "requesting-user-name", "ccos");
}

/* split ipp://host:port/path */
static int uri_split(const char *uri, char *host, size_t hn, int *port, char *path, size_t pn) {
    const char *p = strstr(uri, "://"); if (!p) return 0;
    p += 3;
    const char *sl = strchr(p, '/'), *co = strchr(p, ':');
    if (co && sl && co > sl) co = NULL;
    size_t hl = co ? (size_t)(co - p) : sl ? (size_t)(sl - p) : strlen(p);
    snprintf(host, hn, "%.*s", (int)hl, p);
    *port = co ? atoi(co + 1) : 631;
    snprintf(path, pn, "%s", sl ? sl : "/ipp/print");
    return 1;
}
/* POST an IPP request; the response body is returned (malloc), or NULL */
static unsigned char *ipp_post(const char *uri, const Buf *req, size_t *rlen, int progress) {
    char host[256], path[256]; int port;
    if (!uri_split(uri, host, sizeof host, &port, path, sizeof path)) return NULL;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0) return NULL;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 30, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0) { freeaddrinfo(ai); close(s); return NULL; }
    freeaddrinfo(ai);
    char h[512];
    int hl = snprintf(h, sizeof h, "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/ipp\r\nContent-Length: %zu\r\n"
                      "User-Agent: ccos-print/1.0\r\nConnection: close\r\n\r\n", path, host, port, req->n);
    if (write(s, h, (size_t)hl) != hl) { close(s); return NULL; }
    size_t off = 0; int last = -1;
    while (off < req->n) {
        ssize_t w = write(s, req->b + off, req->n - off > 65536 ? 65536 : req->n - off);
        if (w <= 0) { close(s); return NULL; }
        off += (size_t)w;
        int pc = (int)(off * 100 / req->n);
        if (progress && pc / 10 != last) { last = pc / 10; printf("S sending %d\n", pc); fflush(stdout); }
    }
    Buf r = { 0 };
    unsigned char tmp[16384]; ssize_t g;
    while ((g = read(s, tmp, sizeof tmp)) > 0) bput(&r, tmp, (size_t)g);
    close(s);
    if (!r.b) return NULL;
    bbyte(&r, 0);
    unsigned char *body = (unsigned char *)strstr((char *)r.b, "\r\n\r\n");
    if (!body || strncmp((char *)r.b, "HTTP/1.", 7) || atoi((char *)r.b + 9) != 200) { free(r.b); return NULL; }
    body += 4;
    size_t blen = r.n - 1 - (size_t)(body - r.b);
    if (strcasestr((char *)r.b, "Transfer-Encoding: chunked")) {   /* undo the chunks */
        Buf d = { 0 }; size_t p = 0;
        while (p < blen) {
            unsigned long cl = strtoul((char *)body + p, NULL, 16);
            unsigned char *e = (unsigned char *)strstr((char *)body + p, "\r\n"); if (!e) break;
            p = (size_t)(e - body) + 2;
            if (!cl || p + cl > blen) break;
            bput(&d, body + p, cl); p += cl + 2;
        }
        free(r.b); *rlen = d.n; return d.b;
    }
    unsigned char *out = malloc(blen + 1);
    memcpy(out, body, blen); *rlen = blen; free(r.b);
    return out;
}
/* walk the attributes of a response: cb(name, tag, value, len) */
static int ipp_status(const unsigned char *r, size_t n) { return n >= 4 ? (r[2] << 8 | r[3]) : -1; }
static void ipp_walk(const unsigned char *r, size_t n, void (*cb)(const char *, int, const unsigned char *, int)) {
    size_t i = 8; char name[128] = "";
    while (i < n) {
        int t = r[i++];
        if (t == 3) break;
        if (t < 0x10) continue;
        if (i + 2 > n) break;
        int nl = r[i] << 8 | r[i + 1]; i += 2;
        if (i + (size_t)nl + 2 > n) break;
        if (nl) snprintf(name, sizeof name, "%.*s", nl, (const char *)r + i);
        i += (size_t)nl;
        int vl = r[i] << 8 | r[i + 1]; i += 2;
        if (i + (size_t)vl > n) break;
        cb(name, t, r + i, vl);
        i += (size_t)vl;
    }
}

/* ---- what the printer can do ---- */
static struct {
    char formats[600], media[1500], media_default[80], state[16];
    int color, res[8], nres, borderless, copies_max, accepting;
} cap;
static void cat_list(char *dst, size_t n, const unsigned char *v, int l) {
    size_t k = strlen(dst);
    if (k + (size_t)l + 2 >= n) return;
    if (k) dst[k++] = ',';
    memcpy(dst + k, v, (size_t)l); dst[k + l] = 0;
}
static void caps_cb(const char *name, int tag, const unsigned char *v, int l) {
    int iv = l == 4 ? (int)((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]) : 0;
    if (!strcmp(name, "document-format-supported")) cat_list(cap.formats, sizeof cap.formats, v, l);
    else if (!strcmp(name, "media-supported")) cat_list(cap.media, sizeof cap.media, v, l);
    else if (!strcmp(name, "media-default")) snprintf(cap.media_default, sizeof cap.media_default, "%.*s", l, (const char *)v);
    else if (!strcmp(name, "print-color-mode-supported") && l == 5 && !memcmp(v, "color", 5)) cap.color = 1;
    else if (!strcmp(name, "pwg-raster-document-resolution-supported") && tag == 0x32 && l == 9 && cap.nres < 8) {
        cap.res[cap.nres++] = (int)((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
    } else if ((!strcmp(name, "media-bottom-margin-supported") || !strcmp(name, "media-left-margin-supported")) && l == 4 && iv == 0) cap.borderless = 1;
    else if (!strcmp(name, "copies-supported") && tag == 0x33 && l == 8) cap.copies_max = (int)((uint32_t)v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7]);
    else if (!strcmp(name, "printer-state") && l == 4) snprintf(cap.state, sizeof cap.state, "%s", iv == 3 ? "idle" : iv == 4 ? "printing" : "stopped");
    else if (!strcmp(name, "printer-is-accepting-jobs") && l == 1) cap.accepting = v[0];
}
static int get_caps(const char *uri) {
    memset(&cap, 0, sizeof cap); cap.copies_max = 1; cap.accepting = 1;
    Buf b = { 0 };
    ipp_head(&b, 0x000B, 1, uri);
    const char *want[] = { "document-format-supported", "media-supported", "media-default", "print-color-mode-supported",
                           "pwg-raster-document-resolution-supported", "media-bottom-margin-supported",
                           "media-left-margin-supported", "copies-supported", "printer-state", "printer-is-accepting-jobs" };
    for (int i = 0; i < 10; i++) {
        bbyte(&b, 0x44); b16(&b, i ? 0 : 20); if (!i) bput(&b, "requested-attributes", 20);
        b16(&b, (int)strlen(want[i])); bput(&b, want[i], strlen(want[i]));
    }
    bbyte(&b, 0x03);
    size_t n; unsigned char *r = ipp_post(uri, &b, &n, 0);
    free(b.b);
    if (!r) return 0;
    int st = ipp_status(r, n);
    if (st >= 0 && st < 0x100) ipp_walk(r, n, caps_cb);
    free(r);
    return st >= 0 && st < 0x100;
}
static int cmd_caps(const char *uri) {
    if (!get_caps(uri)) { printf("state=offline\n"); return 1; }
    printf("state=%s\naccepting=%d\nformats=%s\nmedia=%s\nmedia_default=%s\ncolor=%d\nborderless=%d\ncopies_max=%d\nres=",
           cap.state[0] ? cap.state : "idle", cap.accepting, cap.formats, cap.media, cap.media_default, cap.color, cap.borderless, cap.copies_max);
    for (int i = 0; i < cap.nres; i++) printf("%s%d", i ? "," : "", cap.res[i]);
    printf("\n");
    return 0;
}

/* =====================================================================
 *  the page: picture laid out on paper
 * ===================================================================== */
/* "iso_a4_210x297mm" -> 210 x 297 mm */
static int media_size(const char *m, double *wmm, double *hmm) {
    const char *u = strrchr(m, '_'); if (!u) return 0;
    double a, b; char unit[4] = "";
    if (sscanf(u + 1, "%lfx%lf%3s", &a, &b, unit) < 3) return 0;
    double k = !strcmp(unit, "in") ? 25.4 : 1.0;
    *wmm = a * k; *hmm = b * k;
    if (*wmm > *hmm) { double t = *wmm; *wmm = *hmm; *hmm = t; }
    return *wmm > 10 && *hmm > 10;
}
static unsigned char *img; static int iw, ih;          /* the picture, RGB */
static int load_image(const char *path) {
    int n;
    img = stbi_load(path, &iw, &ih, &n, 3);
    if (img) return 1;
    const char *e = strrchr(path, '.');
    if (e && !strcasecmp(e, ".svg")) {
        NSVGimage *sv = nsvgParseFromFile(path, "px", 96);
        if (!sv || sv->width <= 0) return 0;
        float sc = 2000.0f / fmaxf(sv->width, sv->height);
        iw = (int)(sv->width * sc); ih = (int)(sv->height * sc);
        unsigned char *rgba = malloc((size_t)iw * ih * 4);
        NSVGrasterizer *r = nsvgCreateRasterizer();
        nsvgRasterize(r, sv, 0, 0, sc, rgba, iw, ih, iw * 4);
        img = malloc((size_t)iw * ih * 3);
        for (int i = 0; i < iw * ih; i++) for (int c = 0; c < 3; c++)      /* on white paper */
            img[i * 3 + c] = (unsigned char)((rgba[i * 4 + c] * rgba[i * 4 + 3] + 255 * (255 - rgba[i * 4 + 3])) / 255);
        free(rgba); nsvgDeleteRasterizer(r); nsvgDelete(sv);
        return 1;
    }
    return 0;
}
typedef struct {
    double wmm, hmm;            /* paper, portrait */
    double margin;              /* mm */
    int rotate;                 /* picture turned 90 degrees */
    int fill, mono, dpi;
} Layout;
/* where the picture goes, in page pixels: x, y, w, h (may stick out when filling) */
static void place(const Layout *L, int pw, int ph, double *x, double *y, double *w, double *h) {
    double m = L->margin * L->dpi / 25.4;
    double bw = pw - 2 * m, bh = ph - 2 * m;
    double sw = L->rotate ? ih : iw, sh = L->rotate ? iw : ih;
    double s = L->fill ? fmax(bw / sw, bh / sh) : fmin(bw / sw, bh / sh);
    *w = sw * s; *h = sh * s; *x = m + (bw - *w) / 2; *y = m + (bh - *h) / 2;
}
/* draw the page row by row (RGB, or grey when mono) */
static void render_row(const Layout *L, int pw, int ph, int row, unsigned char *out) {
    int bpp = L->mono ? 1 : 3;
    memset(out, 255, (size_t)pw * bpp);
    double x, y, w, h; place(L, pw, ph, &x, &y, &w, &h);
    double m = L->margin * L->dpi / 25.4;
    if (row < m - 0.5 || row >= ph - m + 0.5) return;          /* margins stay white (fill crops at the margin) */
    if (row < y || row >= y + h) return;
    double v = (row + 0.5 - y) / h;
    int x0 = (int)fmax(fmax(x, m), 0), x1 = (int)fmin(fmin(x + w, pw - m), pw);
    for (int px = x0; px < x1; px++) {
        double u = (px + 0.5 - x) / w;
        double sx, sy;                                           /* in the picture */
        if (L->rotate) { sx = v * iw; sy = (1 - u) * ih; } else { sx = u * iw; sy = v * ih; }
        sx -= 0.5; sy -= 0.5;
        int ix = (int)floor(sx), iy = (int)floor(sy); double fx = sx - ix, fy = sy - iy;
        int ax = ix < 0 ? 0 : ix >= iw ? iw - 1 : ix, bx = ix + 1 >= iw ? iw - 1 : ix + 1 < 0 ? 0 : ix + 1;
        int ay = iy < 0 ? 0 : iy >= ih ? ih - 1 : iy, by = iy + 1 >= ih ? ih - 1 : iy + 1 < 0 ? 0 : iy + 1;
        double c[3];
        for (int k = 0; k < 3; k++) {
            double p00 = img[((size_t)ay * iw + ax) * 3 + k], p10 = img[((size_t)ay * iw + bx) * 3 + k];
            double p01 = img[((size_t)by * iw + ax) * 3 + k], p11 = img[((size_t)by * iw + bx) * 3 + k];
            c[k] = (p00 * (1 - fx) + p10 * fx) * (1 - fy) + (p01 * (1 - fx) + p11 * fx) * fy;
        }
        if (L->mono) out[px] = (unsigned char)(0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] + 0.5);
        else for (int k = 0; k < 3; k++) out[px * 3 + k] = (unsigned char)(c[k] + 0.5);
    }
}

/* ---- PWG raster (what IPP Everywhere printers all understand) ---- */
static void be32(unsigned char *h, int off, uint32_t v) { h[off] = v >> 24; h[off + 1] = v >> 16; h[off + 2] = v >> 8; h[off + 3] = v; }
static void pwg_line(Buf *b, const unsigned char *ln, int pw, int bpp) {
    int i = 0;
    while (i < pw) {
        int j = i + 1;                                           /* a run of the same pixel? */
        while (j < pw && j - i < 128 && !memcmp(ln + j * bpp, ln + i * bpp, (size_t)bpp)) j++;
        if (j - i >= 2) { bbyte(b, j - i - 1); bput(b, ln + i * bpp, (size_t)bpp); i = j; continue; }
        j = i + 1;                                               /* different pixels, as they are */
        while (j < pw && j - i < 128 && !(j + 1 < pw && !memcmp(ln + j * bpp, ln + (j + 1) * bpp, (size_t)bpp))) j++;
        bbyte(b, 257 - (j - i)); bput(b, ln + i * bpp, (size_t)(j - i) * bpp); i = j;
    }
}
static void make_pwg(Buf *doc, const Layout *L, const char *media, int copies) {
    int pw = (int)lround(L->wmm / 25.4 * L->dpi), ph = (int)lround(L->hmm / 25.4 * L->dpi), bpp = L->mono ? 1 : 3;
    Buf page = { 0 };
    unsigned char *a = malloc((size_t)pw * bpp), *bl = malloc((size_t)pw * bpp);
    int y = 0;
    render_row(L, pw, ph, 0, a);
    while (y < ph) {                                             /* repeat identical lines (white margins!) */
        int rep = 1, next = 0;
        while (y + rep < ph && rep < 256) {
            render_row(L, pw, ph, y + rep, bl);
            if (memcmp(a, bl, (size_t)pw * bpp)) { next = 1; break; }
            rep++;
        }
        bbyte(&page, rep - 1); pwg_line(&page, a, pw, bpp);
        y += rep;
        if (y < ph) { if (next) { unsigned char *t2 = a; a = bl; bl = t2; } else render_row(L, pw, ph, y, a); }
    }
    free(a); free(bl);
    unsigned char h[1796]; memset(h, 0, sizeof h);
    strcpy((char *)h, "PwgRaster");
    strcpy((char *)h + 192, "photo");                            /* print-content-optimize */
    be32(h, 276, L->dpi); be32(h, 280, L->dpi);
    be32(h, 340, 1);                                             /* copies: we send each page */
    be32(h, 352, (uint32_t)lround(L->wmm / 25.4 * 72)); be32(h, 356, (uint32_t)lround(L->hmm / 25.4 * 72));
    be32(h, 372, pw); be32(h, 376, ph);
    be32(h, 384, 8); be32(h, 388, bpp * 8); be32(h, 392, pw * bpp);
    be32(h, 400, L->mono ? 18 : 19);                             /* sGray / sRGB */
    be32(h, 420, bpp);
    be32(h, 452, copies);                                        /* total pages */
    be32(h, 456, 1); be32(h, 460, 1);                            /* cross-feed / feed transform */
    be32(h, 480, 0xffffff);                                      /* alternate primary: white */
    be32(h, 484, L->dpi >= 600 ? 5 : 4);                         /* print quality */
    snprintf((char *)h + 1732, 64, "%s", media);
    bput(doc, "RaS2", 4);
    for (int c = 0; c < copies; c++) { bput(doc, h, sizeof h); bput(doc, page.b, page.n); }
    free(page.b);
}

/* ---- PDF (for printers that take PDF but not PWG raster) ---- */
static void make_pdf(Buf *doc, const Layout *L, int copies) {
    int pw = (int)lround(L->wmm / 25.4 * L->dpi), ph = (int)lround(L->hmm / 25.4 * L->dpi), bpp = L->mono ? 1 : 3;
    double x, y, w, h; place(L, pw, ph, &x, &y, &w, &h);
    double m = L->margin * L->dpi / 25.4;
    /* only the part of the page with the picture on it */
    int x0 = (int)fmax(fmax(x, m), 0), x1 = (int)fmin(fmin(x + w, pw - m), pw);
    int y0 = (int)fmax(fmax(y, m), 0), y1 = (int)fmin(fmin(y + h, ph - m), ph);
    int cw = x1 - x0, ch = y1 - y0;
    unsigned char *raw = malloc((size_t)cw * ch * bpp), *row = malloc((size_t)pw * bpp);
    for (int r = 0; r < ch; r++) { render_row(L, pw, ph, y0 + r, row); memcpy(raw + (size_t)r * cw * bpp, row + (size_t)x0 * bpp, (size_t)cw * bpp); }
    free(row);
    uLongf zl = compressBound((uLong)cw * ch * bpp);
    unsigned char *z = malloc(zl);
    compress2(z, &zl, raw, (uLong)cw * ch * bpp, 6);
    free(raw);
    double Wpt = L->wmm / 25.4 * 72, Hpt = L->hmm / 25.4 * 72, k = 72.0 / L->dpi;
    long off[256]; int no = 0; char s[512];
    #define OBJ() (off[++no] = (long)doc->n)
    bput(doc, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n", 15);
    OBJ(); snprintf(s, sizeof s, "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"); bput(doc, s, strlen(s));
    OBJ(); { Buf kids = { 0 }; bput(&kids, "2 0 obj << /Type /Pages /Kids [", 31);
             for (int c = 0; c < copies; c++) { snprintf(s, sizeof s, "%d 0 R ", 5 + c * 2); bput(&kids, s, strlen(s)); }
             snprintf(s, sizeof s, "] /Count %d >> endobj\n", copies); bput(&kids, s, strlen(s)); bput(doc, kids.b, kids.n); free(kids.b); }
    OBJ(); snprintf(s, sizeof s, "3 0 obj << /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s /BitsPerComponent 8 "
                    "/Filter /FlateDecode /Length %lu >> stream\n", cw, ch, L->mono ? "DeviceGray" : "DeviceRGB", (unsigned long)zl);
    bput(doc, s, strlen(s)); bput(doc, z, zl); bput(doc, "\nendstream endobj\n", 18);
    free(z);
    char content[256];
    int cl = snprintf(content, sizeof content, "q %.3f 0 0 %.3f %.3f %.3f cm /Im0 Do Q\n", cw * k, ch * k, x0 * k, Hpt - (y0 + ch) * k);
    OBJ(); snprintf(s, sizeof s, "4 0 obj << /Length %d >> stream\n%sendstream endobj\n", cl, content); bput(doc, s, strlen(s));
    for (int c = 0; c < copies; c++) {
        OBJ(); snprintf(s, sizeof s, "%d 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents 4 0 R "
                        "/Resources << /XObject << /Im0 3 0 R >> >> >> endobj\n", no, Wpt, Hpt); bput(doc, s, strlen(s));
        OBJ(); snprintf(s, sizeof s, "%d 0 obj null endobj\n", no); bput(doc, s, strlen(s));   /* keeps numbering simple */
    }
    long xref = (long)doc->n;
    snprintf(s, sizeof s, "xref\n0 %d\n0000000000 65535 f \n", no + 1); bput(doc, s, strlen(s));
    for (int i = 1; i <= no; i++) { snprintf(s, sizeof s, "%010ld 00000 n \n", off[i]); bput(doc, s, strlen(s)); }
    snprintf(s, sizeof s, "trailer << /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", no + 1, xref); bput(doc, s, strlen(s));
}

static int has_fmt(const char *f) {
    const char *p = cap.formats; size_t l = strlen(f);
    while ((p = strstr(p, f))) { if ((p == cap.formats || p[-1] == ',') && (p[l] == ',' || !p[l])) return 1; p++; }
    return 0;
}
static int job_state_v;
static void job_cb(const char *name, int tag, const unsigned char *v, int l) {
    (void)tag;
    if ((!strcmp(name, "job-id") || !strcmp(name, "job-state")) && l == 4) {
        int iv = (int)((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
        if (!strcmp(name, "job-id")) job_state_v = (job_state_v & 0xffff) | iv << 16;
        else job_state_v = (job_state_v & ~0xffff) | iv;
    }
}
static int cmd_image(int argc, char **argv) {
    const char *file = argv[2], *uri = argv[3], *media = NULL, *orient = "auto", *quality = "normal";
    int fill = 0, mono = 0, copies = 1, borderless = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--media") && i + 1 < argc) media = argv[++i];
        else if (!strcmp(argv[i], "--orient") && i + 1 < argc) orient = argv[++i];
        else if (!strcmp(argv[i], "--fit") && i + 1 < argc) fill = !strcmp(argv[++i], "fill");
        else if (!strcmp(argv[i], "--mono")) mono = 1;
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = argv[++i];
        else if (!strcmp(argv[i], "--copies") && i + 1 < argc) copies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--borderless")) borderless = 1;
    }
    if (copies < 1) copies = 1; if (copies > 99) copies = 99;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("S connecting\n");
    if (!get_caps(uri)) { printf("ERR The printer didn't answer. Is it on and on the same Wi-Fi?\n"); return 1; }
    if (!cap.accepting) { printf("ERR The printer isn't taking jobs right now (out of paper or ink?).\n"); return 1; }
    if (!load_image(file)) { printf("ERR ccos couldn't open this picture.\n"); return 1; }
    if (!media || !*media) media = cap.media_default[0] ? cap.media_default : "iso_a4_210x297mm";
    Layout L = { .fill = fill, .mono = mono || !cap.color };
    if (!media_size(media, &L.wmm, &L.hmm)) { L.wmm = 210; L.hmm = 297; }
    L.margin = borderless && cap.borderless ? 0 : 5;
    int land = iw > ih;
    L.rotate = !strcmp(orient, "landscape") ? 1 : !strcmp(orient, "portrait") ? 0 : land;
    /* resolution: the printer's choice nearest to what was asked */
    int want = !strcmp(quality, "high") ? 600 : 300;
    L.dpi = 0;
    for (int i = 0; i < cap.nres; i++) if (!L.dpi || abs(cap.res[i] - want) < abs(L.dpi - want)) L.dpi = cap.res[i];
    if (!L.dpi) L.dpi = want;
    if (!strcmp(quality, "draft") && L.dpi > 300) L.dpi = 300;

    printf("S preparing\n");
    Buf doc = { 0 }; const char *fmt;
    if (has_fmt("image/pwg-raster")) { fmt = "image/pwg-raster"; make_pwg(&doc, &L, media, copies); }
    else if (has_fmt("application/pdf")) { fmt = "application/pdf"; if (L.dpi > 300) L.dpi = 300; make_pdf(&doc, &L, copies); }
    else { printf("ERR This printer needs a format ccos can't make yet (it doesn't take PWG raster or PDF).\n"); return 1; }

    Buf req = { 0 };
    ipp_head(&req, 0x0002, 2, uri);
    const char *base = strrchr(file, '/');
    ipp_str(&req, 0x42, "job-name", base ? base + 1 : file);
    ipp_str(&req, 0x49, "document-format", fmt);
    bbyte(&req, 0x02);                                          /* job attributes */
    ipp_str(&req, 0x44, "media", media);
    ipp_str(&req, 0x44, "print-color-mode", L.mono ? "monochrome" : "color");
    ipp_int(&req, 0x23, "print-quality", !strcmp(quality, "draft") ? 3 : !strcmp(quality, "high") ? 5 : 4);
    ipp_str(&req, 0x44, "print-scaling", "none");
    bbyte(&req, 0x03);
    bput(&req, doc.b, doc.n); free(doc.b);
    size_t n; unsigned char *r = ipp_post(uri, &req, &n, 1);
    free(req.b);
    if (!r) { printf("ERR Couldn't send the picture to the printer.\n"); return 1; }
    int st = ipp_status(r, n);
    job_state_v = 0; ipp_walk(r, n, job_cb); free(r);
    if (st < 0 || st >= 0x100) { printf("ERR The printer said no (IPP error 0x%04x).\n", st); return 1; }
    int job = job_state_v >> 16;
    printf("S printing\n");
    /* follow the job until it's done (at most 3 minutes) */
    for (int t = 0; t < 90 && job > 0; t++) {
        int s = job_state_v & 0xffff;
        if (s == 9) break;                                       /* completed */
        if (s == 7 || s == 8) { printf("ERR The printer stopped the job%s.\n", s == 7 ? " (canceled)" : ""); return 1; }
        sleep(2);
        Buf q = { 0 };
        ipp_head(&q, 0x0009, 3, uri); ipp_int(&q, 0x21, "job-id", job); bbyte(&q, 0x03);
        r = ipp_post(uri, &q, &n, 0); free(q.b);
        if (!r) break;                                           /* sent; can't follow it: that's fine */
        int keep = job_state_v & 0xffff0000; job_state_v = 0; ipp_walk(r, n, job_cb); job_state_v |= keep; free(r);
    }
    printf("OK\n");
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && !strcmp(argv[1], "list")) return cmd_list();
    if (argc >= 3 && !strcmp(argv[1], "caps")) return cmd_caps(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "image")) return cmd_image(argc, argv);
    fprintf(stderr, "usage: ccos-print list | caps URI | image FILE URI [--media M] [--orient auto|portrait|landscape]\n"
                    "                  [--fit fit|fill] [--mono] [--quality draft|normal|high] [--copies N] [--borderless]\n");
    return 1;
}
