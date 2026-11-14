/* ===================================================================
 *  Glass: window frames, the taskbar and the start menu show a blurred,
 *  frosted copy of the desktop background behind their colour.
 *  Settings > Appearance > Glass: 0% = solid colour, 100% = as see-through
 *  as it gets (still frosted and tinted, so it stays readable).
 * =================================================================== */
static int set_glass = 55;
static uint32_t *gb; static int gbw, gbh;        /* the blurred background, 8 times smaller */
#define GB_S 8
static void glass_make(void) {
    gbw = (BW + GB_S - 1) / GB_S; gbh = (H + GB_S - 1) / GB_S;
    uint32_t *a = realloc(gb, (size_t)gbw * gbh * 4), *t = malloc((size_t)gbw * gbh * 4);
    if (!a || !t) { free(t); if (a) gb = a; return; }
    gb = a;
    for (int y = 0; y < gbh; y++) for (int x = 0; x < gbw; x++) {          /* average each 8 x 8 block */
        unsigned r = 0, g = 0, b = 0, n = 0;
        for (int j = y * GB_S; j < imin(H, y * GB_S + GB_S); j += 2) for (int i = x * GB_S; i < imin(BW, x * GB_S + GB_S); i += 2) {
            uint32_t c = wall[(size_t)j * BW + i]; r += R(c); g += G(c); b += B(c); n++;
        }
        gb[y * gbw + x] = n ? RGB(r / n, g / n, b / n) : 0;
    }
    for (int pass = 0; pass < 3; pass++) {                                    /* box blur, both ways */
        for (int y = 0; y < gbh; y++) for (int x = 0; x < gbw; x++) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int k = -3; k <= 3; k++) { int xx = clampi(x + k, 0, gbw - 1); uint32_t c = gb[y * gbw + xx]; r += R(c); g += G(c); b += B(c); n++; }
            t[y * gbw + x] = RGB(r / n, g / n, b / n);
        }
        for (int y = 0; y < gbh; y++) for (int x = 0; x < gbw; x++) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int k = -3; k <= 3; k++) { int yy = clampi(y + k, 0, gbh - 1); uint32_t c = t[yy * gbw + x]; r += R(c); g += G(c); b += B(c); n++; }
            gb[y * gbw + x] = RGB(r / n, g / n, b / n);
        }
    }
    free(t);
}
static inline uint32_t glass_at(int x, int y) {               /* smooth (bilinear) */
    float fx = (x + 0.5f) / GB_S - 0.5f, fy = (y + 0.5f) / GB_S - 0.5f;
    int x0 = clampi((int)floorf(fx), 0, gbw - 1), y0 = clampi((int)floorf(fy), 0, gbh - 1);
    int x1 = imin(x0 + 1, gbw - 1), y1 = imin(y0 + 1, gbh - 1);
    float tx = clampf(fx - x0, 0, 1), ty = clampf(fy - y0, 0, 1);
    return mix(mix(gb[y0 * gbw + x0], gb[y0 * gbw + x1], tx), mix(gb[y1 * gbw + x0], gb[y1 * gbw + x1], tx), ty);
}
/* fill x,y,w,h (round corners r; 0 = square) with glass tinted by `tint`, leaving out the hole hx,hy,hw,hh */
static void glass_fill(int x, int y, int w, int h, int r, uint32_t tint, int hx, int hy, int hw, int hh) {
    if (!gb || set_glass <= 0) {
        if (r) fill_round(x, y, w, h, r, tint, 255); else fill_rect(x, y, w, h, tint, 255);
        return;
    }
    float g = set_glass / 100.0f * 0.80f;                       /* at 100%: 80% background, 20% colour */
    int lum = (R(tint) * 3 + G(tint) * 6 + B(tint)) / 10;
    uint32_t frost = lum > 128 ? RGB(255, 255, 255) : RGB(0x10, 0x12, 0x16);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) {
        uint32_t *d = &back[(size_t)j * BW];
        for (int i = x0; i < x1; i++) {
            if (hw > 0 && i >= hx && i < hx + hw && j >= hy && j < hy + hh) { i = hx + hw - 1; continue; }
            float cov = r ? round_cov(i, j, x, y, w, h, r) : 1;
            if (cov <= 0) continue;
            uint32_t bgc = mix(glass_at(i, j), frost, 0.38f);       /* frosted */
            uint32_t c = mix(tint, bgc, g);
            d[i] = cov >= 1 ? c : mix(d[i], c, cov);
        }
    }
}
