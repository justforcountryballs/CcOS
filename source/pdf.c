/*
 * ccos-pdf - draws PDF pages for ccos's PDF viewer (with MuPDF). Licence: AGPL-3.0 (as MuPDF).
 *
 * Runs next to the desktop and takes requests on stdin, one per line:
 *   O <file>                    open a PDF  ->  "P <pages>" then one "S <w> <h>" per page (points)
 *                                               or "E <why>"
 *   R <page> <width> <out>      draw page (from 0) <width> pixels wide into <out>
 *                               (a raw file: "W H\n" then W*H*3 bytes RGB)  ->  "D <page> <width>"
 *   T <page>                    the page's text  ->  "X <page> <bytes>" then the text
 * Build: gcc -O2 -o ccos-pdf pdf.c pdffonts.c -lmupdf -lmupdf-third -lmujs -lgumbo -lopenjp2 -ljbig2dec -ljpeg -lfreetype -lharfbuzz -lz -lm
 */
#include <mupdf/fitz.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    fz_context *ctx = fz_new_context(NULL, NULL, 64 << 20);
    if (!ctx) { printf("E MuPDF didn't start\n"); return 1; }
    fz_register_document_handlers(ctx);
    fz_document *doc = NULL;
    char line[4200];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 'O' && line[1] == ' ') {
            if (doc) { fz_drop_document(ctx, doc); doc = NULL; }
            fz_try(ctx) {
                doc = fz_open_document(ctx, line + 2);
                if (fz_needs_password(ctx, doc)) { fz_drop_document(ctx, doc); doc = NULL; printf("E This PDF is locked with a password.\n"); }
                else {
                    int n = fz_count_pages(ctx, doc);
                    printf("P %d\n", n);
                    for (int i = 0; i < n; i++) {
                        fz_rect b = fz_empty_rect;
                        fz_try(ctx) { fz_page *p = fz_load_page(ctx, doc, i); b = fz_bound_page(ctx, p); fz_drop_page(ctx, p); }
                        fz_catch(ctx) { b = fz_make_rect(0, 0, 612, 792); }
                        printf("S %.1f %.1f\n", b.x1 - b.x0, b.y1 - b.y0);
                    }
                }
            }
            fz_catch(ctx) { doc = NULL; printf("E Couldn't open it: %s\n", fz_caught_message(ctx)); }
            fflush(stdout);
        } else if (line[0] == 'R' && doc) {
            int pg, w; char out[4096];
            if (sscanf(line, "R %d %d %4095s", &pg, &w, out) != 3) continue;
            fz_pixmap *pix = NULL;
            fz_try(ctx) {
                fz_page *p = fz_load_page(ctx, doc, pg);
                fz_rect b = fz_bound_page(ctx, p);
                float z = w / fmaxf(1, b.x1 - b.x0);
                pix = fz_new_pixmap_from_page(ctx, p, fz_scale(z, z), fz_device_rgb(ctx), 0);
                fz_drop_page(ctx, p);
                FILE *f = fopen(out, "wb");
                if (f) {
                    int W = fz_pixmap_width(ctx, pix), H = fz_pixmap_height(ctx, pix), st = fz_pixmap_stride(ctx, pix), nc = fz_pixmap_components(ctx, pix);
                    unsigned char *s = fz_pixmap_samples(ctx, pix);
                    fprintf(f, "%d %d\n", W, H);
                    unsigned char *row = malloc((size_t)W * 3);
                    for (int y = 0; y < H; y++) {
                        for (int x = 0; x < W; x++) memcpy(row + x * 3, s + (size_t)y * st + x * nc, 3);
                        fwrite(row, 3, (size_t)W, f);
                    }
                    free(row); fclose(f);
                }
                fz_drop_pixmap(ctx, pix);
                printf("D %d %d\n", pg, w);
            }
            fz_catch(ctx) { printf("D %d %d\n", pg, -1); }
            fflush(stdout);
        } else if (line[0] == 'T' && doc) {
            int pg; if (sscanf(line, "T %d", &pg) != 1) continue;
            fz_buffer *buf = NULL; const char *txt = "";
            fz_try(ctx) {
                fz_page *p = fz_load_page(ctx, doc, pg);
                fz_stext_page *sp = fz_new_stext_page_from_page(ctx, p, NULL);
                buf = fz_new_buffer_from_stext_page(ctx, sp);
                fz_drop_stext_page(ctx, sp); fz_drop_page(ctx, p);
                txt = fz_string_from_buffer(ctx, buf);
            }
            fz_catch(ctx) { txt = ""; }
            printf("X %d %zu\n", pg, strlen(txt)); fwrite(txt, 1, strlen(txt), stdout); fflush(stdout);
            if (buf) fz_drop_buffer(ctx, buf);
        }
    }
    if (doc) fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 0;
}
