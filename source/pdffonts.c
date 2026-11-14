/*
 * pdffonts.c - the fonts ccos-pdf keeps from MuPDF: only the 14 standard PDF fonts
 * (Times, Helvetica, Courier, Symbol, Dingbats). MuPDF's own font table also pulls
 * in large Chinese/Japanese/Korean and other-script fonts (about 40 MB); defining
 * the lookup functions here leaves those out. PDFs with their own fonts (nearly
 * all) are unaffected. Licence: AGPL-3.0 (part of ccos-pdf).
 */
#include <mupdf/fitz.h>
#include <string.h>
#define F(n) extern const unsigned char _binary_resources_fonts_urw_##n##_cff_start[], _binary_resources_fonts_urw_##n##_cff_end[];
F(NimbusMonoPS_Regular) F(NimbusMonoPS_Italic) F(NimbusMonoPS_Bold) F(NimbusMonoPS_BoldItalic)
F(NimbusSans_Regular) F(NimbusSans_Italic) F(NimbusSans_Bold) F(NimbusSans_BoldItalic)
F(NimbusRoman_Regular) F(NimbusRoman_Italic) F(NimbusRoman_Bold) F(NimbusRoman_BoldItalic)
F(StandardSymbolsPS) F(Dingbats)
#define RET(n) do { *len = (int)(_binary_resources_fonts_urw_##n##_cff_end - _binary_resources_fonts_urw_##n##_cff_start); return _binary_resources_fonts_urw_##n##_cff_start; } while (0)
const unsigned char *fz_lookup_base14_font(fz_context *ctx, const char *name, int *len) {
    (void)ctx;
    if (!strcmp(name, "Courier")) RET(NimbusMonoPS_Regular);
    if (!strcmp(name, "Courier-Oblique")) RET(NimbusMonoPS_Italic);
    if (!strcmp(name, "Courier-Bold")) RET(NimbusMonoPS_Bold);
    if (!strcmp(name, "Courier-BoldOblique")) RET(NimbusMonoPS_BoldItalic);
    if (!strcmp(name, "Helvetica")) RET(NimbusSans_Regular);
    if (!strcmp(name, "Helvetica-Oblique")) RET(NimbusSans_Italic);
    if (!strcmp(name, "Helvetica-Bold")) RET(NimbusSans_Bold);
    if (!strcmp(name, "Helvetica-BoldOblique")) RET(NimbusSans_BoldItalic);
    if (!strcmp(name, "Times-Roman")) RET(NimbusRoman_Regular);
    if (!strcmp(name, "Times-Italic")) RET(NimbusRoman_Italic);
    if (!strcmp(name, "Times-Bold")) RET(NimbusRoman_Bold);
    if (!strcmp(name, "Times-BoldItalic")) RET(NimbusRoman_BoldItalic);
    if (!strcmp(name, "Symbol")) RET(StandardSymbolsPS);
    if (!strcmp(name, "ZapfDingbats")) RET(Dingbats);
    *len = 0; return NULL;
}
const unsigned char *fz_lookup_builtin_font(fz_context *ctx, const char *name, int bold, int italic, int *len) {
    (void)ctx;
    if (!strcmp(name, "Courier")) { if (bold) { if (italic) RET(NimbusMonoPS_BoldItalic); RET(NimbusMonoPS_Bold); } if (italic) RET(NimbusMonoPS_Italic); RET(NimbusMonoPS_Regular); }
    if (!strcmp(name, "Helvetica") || !strcmp(name, "Arial")) { if (bold) { if (italic) RET(NimbusSans_BoldItalic); RET(NimbusSans_Bold); } if (italic) RET(NimbusSans_Italic); RET(NimbusSans_Regular); }
    if (!strcmp(name, "Times") || !strcmp(name, "Times Roman") || !strcmp(name, "Times New Roman")) { if (bold) { if (italic) RET(NimbusRoman_BoldItalic); RET(NimbusRoman_Bold); } if (italic) RET(NimbusRoman_Italic); RET(NimbusRoman_Regular); }
    if (!strcmp(name, "Symbol")) RET(StandardSymbolsPS);
    if (!strcmp(name, "Dingbats") || !strcmp(name, "ZapfDingbats")) RET(Dingbats);
    *len = 0; return NULL;
}
const unsigned char *fz_lookup_cjk_font(fz_context *ctx, int ordering, int *len, int *index) { (void)ctx; (void)ordering; *len = 0; if (index) *index = 0; return NULL; }
const unsigned char *fz_lookup_cjk_font_by_language(fz_context *ctx, const char *lang, int *len, int *subfont) { (void)ctx; (void)lang; *len = 0; if (subfont) *subfont = 0; return NULL; }
int fz_lookup_cjk_ordering_by_language(const char *name) { (void)name; return -1; }
const unsigned char *fz_lookup_noto_font(fz_context *ctx, int script, int lang, int *len, int *subfont) { (void)ctx; (void)script; (void)lang; *len = 0; if (subfont) *subfont = 0; return NULL; }
const unsigned char *fz_lookup_noto_math_font(fz_context *ctx, int *len) { (void)ctx; *len = 0; return NULL; }
const unsigned char *fz_lookup_noto_music_font(fz_context *ctx, int *len) { (void)ctx; *len = 0; return NULL; }
const unsigned char *fz_lookup_noto_symbol1_font(fz_context *ctx, int *len) { (void)ctx; *len = 0; return NULL; }
const unsigned char *fz_lookup_noto_symbol2_font(fz_context *ctx, int *len) { (void)ctx; *len = 0; return NULL; }
const unsigned char *fz_lookup_noto_emoji_font(fz_context *ctx, int *len) { (void)ctx; *len = 0; return NULL; }
const unsigned char *fz_lookup_noto_boxes_font(fz_context *ctx, int *len) { (void)ctx; *len = 0; return NULL; }
