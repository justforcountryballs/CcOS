/* ===================================================================
 *  SOLITAIRE (Klondike, like the classic Windows game)
 *  Included from desktop.c.
 *
 *  Click the deck to turn cards over. Drag cards (or a run of cards) onto
 *  another column: one lower and the other colour, kings on empty spaces.
 *  Build the four piles at the top up from Ace to King in one suit.
 *  Double-click a card to send it up to its pile.
 * =================================================================== */
#define SO_CW    72               /* card size */
#define SO_CH    100
#define SO_GAP   12
#define SO_X0    14
#define SO_Y0    (MENUBAR_H + 12)
#define SO_TY    (SO_Y0 + SO_CH + 18)   /* top of the seven columns */
#define SO_DOWN  8                /* how much of a face-down card shows in a column */
#define SO_UP    24               /* ... and of a face-up card */
#define SO_STATUS 26
#define SO_W     (2 * SO_X0 + 7 * SO_CW + 6 * SO_GAP)
#define SO_H     600

enum { P_STOCK, P_WASTE, P_FOUND, P_TAB };
static int so_stock[52], so_nstock, so_waste[52], so_nwaste;
static int so_found[4][13], so_nfound[4];
static int so_tab[7][20], so_ntab[7], so_down[7];       /* so_down[i]: how many are face down */
static int so_draw3, so_score, so_moves, so_won, so_started;
static time_t so_t0; static int so_time;
static Dialog so_dlg;

/* what's being dragged */
static struct { int on, from, idx, n, cards[20], ox, oy, x, y; } so_dr;
static long long so_last_click; static int so_last_lx, so_last_ly;

static DropItem so_items[] = {
    { "New game", "F2" }, { "-", "" }, { "Draw one", "*" }, { "Draw three", "" }, { "-", "" }, { "Exit", "" },
};
static const MenuDef so_menus[] = { { "Game", so_items, 6 } };
static MenuBar so_mb = { so_menus, 1, -1 };

static void solitaire_draw(int cx, int cy, int cw, int ch);

static int so_rank(int c) { return c % 13; }             /* 0 = Ace ... 12 = King */
static int so_suit(int c) { return c / 13; }             /* 0 spades, 1 hearts, 2 diamonds, 3 clubs */
static int so_red(int c) { return so_suit(c) == 1 || so_suit(c) == 2; }

/* ---- the deal ---- */
static void so_new_game(void) {
    int d[52];
    for (int i = 0; i < 52; i++) d[i] = i;
    for (int i = 51; i > 0; i--) { int j = rand() % (i + 1), t = d[i]; d[i] = d[j]; d[j] = t; }
    int k = 0;
    for (int c = 0; c < 7; c++) {
        so_ntab[c] = c + 1; so_down[c] = c;
        for (int j = 0; j <= c; j++) so_tab[c][j] = d[k++];
    }
    so_nstock = 0;
    while (k < 52) so_stock[so_nstock++] = d[k++];
    so_nwaste = 0;
    for (int f = 0; f < 4; f++) so_nfound[f] = 0;
    so_score = 0; so_moves = 0; so_won = 0; so_started = 0; so_time = 0;
    so_dr.on = 0; so_dlg.active = 0;
    repaint_client(solitaire_draw);
}

/* ---- where things are (client coordinates) ---- */
static int so_pile_x(int col) { return SO_X0 + col * (SO_CW + SO_GAP); }
static int so_up_step(int col) {                         /* squeeze long columns so they fit */
    int up = so_ntab[col] - so_down[col];
    int room = SO_H - SO_STATUS - 8 - SO_TY - SO_CH - so_down[col] * SO_DOWN;
    if (up <= 1) return SO_UP;
    return imax(12, imin(SO_UP, room / (up - 1)));
}
static int so_card_y(int col, int j) {
    int y = SO_TY, step = so_up_step(col);
    for (int k = 0; k < j; k++) y += k < so_down[col] ? SO_DOWN : step;
    return y;
}
static int so_waste_show(void) { return so_draw3 ? imin(3, so_nwaste) : imin(1, so_nwaste); }
static int so_waste_x(int i) { return so_pile_x(1) + i * 16; }   /* i-th of the shown waste cards */

/* ---- drawing ---- */
/* the four suits, from simple shapes (u, v from -1 to 1, v downwards) */
static int so_in_suit(int s, float u, float v) {
    #define IN_C(cx, cy, r) (((u) - (cx)) * ((u) - (cx)) + ((v) - (cy)) * ((v) - (cy)) < (r) * (r))
    switch (s) {
    case 1: /* heart */
        if (IN_C(-0.48f, -0.34f, 0.5f) || IN_C(0.48f, -0.34f, 0.5f)) return 1;
        return v > -0.3f && v < 0.95f && fabsf(u) < 0.97f * (0.95f - v) / 1.25f;
    case 2: /* diamond */
        return fabsf(u) / 0.72f + fabsf(v) < 1;
    case 0: /* spade */
        if (IN_C(-0.46f, 0.22f, 0.44f) || IN_C(0.46f, 0.22f, 0.44f)) return 1;
        if (v < 0.3f && v > -1 && fabsf(u) < 0.9f * (v + 1) / 1.3f) return 1;
        return v > 0.35f && v < 1 && fabsf(u) < 0.08f + (v - 0.35f) * 0.5f;
    default: /* club */
        if (IN_C(0, -0.48f, 0.36f) || IN_C(-0.45f, 0.1f, 0.36f) || IN_C(0.45f, 0.1f, 0.36f) || IN_C(0, -0.05f, 0.22f)) return 1;
        return v > 0.1f && v < 1 && fabsf(u) < 0.07f + (v - 0.1f) * 0.42f;
    }
    #undef IN_C
}
static void so_draw_suit(int cx, int cy, int r, int s, uint32_t col) {
    int x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 2, y1 = cy + r + 2;
    if (!clip_range(&x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int n = 0;                                   /* 3x3 samples: smooth edges */
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++)
                    n += so_in_suit(s, (x + (sx + 0.5f) / 3 - cx) / r, (y + (sy + 0.5f) / 3 - cy) / r);
            if (n) put_back(x, y, col, n * 255 / 9);
        }
}

static const char *so_rank_name[13] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };

static void so_draw_back(int x, int y) {
    fill_round(x, y, SO_CW, SO_CH, 6, RGB(0x30, 0x34, 0x40), 255);
    fill_round(x + 1, y + 1, SO_CW - 2, SO_CH - 2, 5, RGB(255, 255, 255), 255);
    fill_round_grad(x + 5, y + 5, SO_CW - 10, SO_CH - 10, 3, RGB(0x3a, 0x8c, 0xe8), RGB(0x0c, 0x3a, 0x86), 255);
    for (int k = -SO_CH; k < SO_CW; k += 9)              /* fine diagonal lines */
        for (int t = 0; t < SO_CH - 12; t++) {
            int px = x + 6 + k + t, py = y + 6 + t;
            if (px >= x + 6 && px < x + SO_CW - 6) put_back(px, py, RGB(255, 255, 255), 40);
        }
    draw_text_bold(x + SO_CW / 2 - 12, y + SO_CH / 2 - 8, "/o\\", RGB(255, 255, 255));   /* the ccos logo */
}

static void so_draw_card(int x, int y, int c) {
    if (x + SO_CW + 2 <= clip_x0 || x >= clip_x1 || y + SO_CH + 2 <= clip_y0 || y >= clip_y1) return;
    fill_round(x + 1, y + 2, SO_CW, SO_CH, 6, RGB(0, 0, 0), 40);          /* shadow */
    if (c < 0) { so_draw_back(x, y); return; }
    uint32_t col = so_red(c) ? RGB(0xd0, 0x18, 0x20) : RGB(0x10, 0x10, 0x14);
    fill_round(x, y, SO_CW, SO_CH, 6, RGB(0x80, 0x84, 0x8c), 255);
    fill_round(x + 1, y + 1, SO_CW - 2, SO_CH - 2, 5, RGB(255, 255, 255), 255);
    int r = so_rank(c), s = so_suit(c);
    const char *rn = so_rank_name[r];
    draw_text_bold(x + 5, y + 4, rn, col);
    so_draw_suit(x + 5 + 4 * (int)strlen(rn), y + 28, 6, s, col);
    if (r >= 10) {                                        /* J, Q, K: a framed letter */
        fill_round(x + 16, y + 22, SO_CW - 32, SO_CH - 40, 4, so_red(c) ? RGB(0xfc, 0xe8, 0xe8) : RGB(0xe8, 0xec, 0xf4), 255);
        stroke_round(x + 16, y + 22, SO_CW - 32, SO_CH - 40, 4, col, 160);
        draw_text_scaled(x + SO_CW / 2 - 8, y + 30, rn, col, 2);
        so_draw_suit(x + SO_CW / 2, y + SO_CH - 32, 9, s, col);
    } else {
        so_draw_suit(x + SO_CW / 2, y + SO_CH / 2 + 4, r == 0 ? 20 : 15, s, col);
    }
    int bw = text_w(rn);                         /* small rank in the other corner */
    draw_text(x + SO_CW - 5 - bw, y + SO_CH - 20, rn, col, 255);
}

static void so_draw_slot(int x, int y, const char *label, int suit) {
    stroke_round(x, y, SO_CW, SO_CH, 6, RGB(0x9c, 0xd0, 0xa0), 150);
    fill_round(x + 1, y + 1, SO_CW - 2, SO_CH - 2, 5, RGB(0, 0x30, 0x10), 50);
    if (suit >= 0) so_draw_suit(x + SO_CW / 2, y + SO_CH / 2, 18, suit, RGB(0x4c, 0xa4, 0x60));   /* which suit goes here */
    if (label) draw_text_bold(x + (SO_CW - text_w(label)) / 2, y + SO_CH / 2 - 8, label, RGB(0x9c, 0xd0, 0xa0));
}

static void solitaire_draw(int cx, int cy, int cw, int ch) {
    int hotid = hot.type == HOT_CONTENT && wins[hot.a].draw == solitaire_draw ? hot.b : -1;
    /* green felt */
    for (int y = imax(cy + MENUBAR_H, clip_y0); y < imin(cy + ch - SO_STATUS, clip_y1); y++) {
        uint32_t c = mix(RGB(0x1e, 0x86, 0x3c), RGB(0x0e, 0x5a, 0x26), (float)(y - cy) / ch);
        for (int x = imax(cx, clip_x0); x < imin(cx + cw, clip_x1); x++) back[(size_t)y * W + x] = c;
    }
    /* deck, turned cards, the four piles */
    if (so_nstock) so_draw_card(cx + so_pile_x(0), cy + SO_Y0, -1);
    else so_draw_slot(cx + so_pile_x(0), cy + SO_Y0, so_nwaste ? "\x09" : NULL, -1);  /* round: deal again */
    int show = so_waste_show(), skip = (so_dr.on && so_dr.from == P_WASTE) ? 1 : 0;
    for (int i = 0; i < show; i++) {
        int k = so_nwaste - show + i;
        if (skip && k == so_nwaste - 1) continue;
        so_draw_card(cx + so_waste_x(i), cy + SO_Y0, so_waste[k]);
    }
    for (int f = 0; f < 4; f++) {
        int x = cx + so_pile_x(3 + f), n = so_nfound[f];
        if (so_dr.on && so_dr.from == P_FOUND && so_dr.idx == f) n--;
        if (n > 0) so_draw_card(x, cy + SO_Y0, so_found[f][n - 1]);
        else so_draw_slot(x, cy + SO_Y0, NULL, f);
    }
    /* the seven columns */
    for (int c = 0; c < 7; c++) {
        int x = cx + so_pile_x(c), n = so_ntab[c];
        if (so_dr.on && so_dr.from == P_TAB && so_dr.idx == c) n -= so_dr.n;
        if (n == 0) so_draw_slot(x, cy + SO_TY, NULL, -1);
        for (int j = 0; j < n; j++) so_draw_card(x, cy + so_card_y(c, j), j < so_down[c] ? -1 : so_tab[c][j]);
    }
    /* cards in your hand */
    if (so_dr.on)
        for (int k = 0; k < so_dr.n; k++) so_draw_card(cx + so_dr.x, cy + so_dr.y + k * SO_UP, so_dr.cards[k]);
    /* status bar */
    int sy = cy + ch - SO_STATUS;
    fill_rect(cx, sy, cw, SO_STATUS, RGB(0xe8, 0xec, 0xf2), 255);
    fill_rect(cx, sy, cw, 1, RGB(0xb8, 0xc0, 0xcc), 255);
    char b[80];
    snprintf(b, sizeof b, "Score: %d    Moves: %d    Time: %d:%02d", so_score, so_moves, so_time / 60, so_time % 60);
    draw_text(cx + 10, sy + 5, b, RGB(0x30, 0x30, 0x30), 255);
    mb_draw_bar(&so_mb, cx, cy, cw, hotid);
    mb_draw_drop(&so_mb, cx, cy, hotid);
    dlg_draw(&so_dlg, cx, cy, cw, ch, hotid);
}

/* ---- rules ---- */
static int so_fits_found(int c, int f) {
    if (so_suit(c) != f) return 0;                       /* pile f is for suit f */
    return so_rank(c) == so_nfound[f];
}
static int so_fits_tab(int c, int col) {
    if (so_ntab[col] == 0) return so_rank(c) == 12;      /* only a King on an empty column */
    if (so_ntab[col] == so_down[col]) return 0;
    int t = so_tab[col][so_ntab[col] - 1];
    return so_red(t) != so_red(c) && so_rank(t) == so_rank(c) + 1;
}

static void so_check_win(void);
static void so_flip_tops(void) {                         /* turn over a column's new top card */
    for (int c = 0; c < 7; c++)
        if (so_ntab[c] && so_down[c] == so_ntab[c]) { so_down[c]--; so_score += 5; }
}

/* take n cards off a pile (they are already copied into so_dr.cards) */
static void so_take(int from, int idx, int n) {
    if (from == P_WASTE) so_nwaste -= n;
    else if (from == P_FOUND) so_nfound[idx] -= n;
    else so_ntab[idx] -= n;
}

static void so_start_clock(void) { if (!so_started) { so_started = 1; so_t0 = time(NULL); } }

/* move the cards in hand onto a pile; 1 if allowed */
static int so_put(int to, int idx) {
    int c = so_dr.cards[0];
    if (to == P_FOUND) {
        if (so_dr.n != 1 || !so_fits_found(c, idx)) return 0;
        so_take(so_dr.from, so_dr.idx, 1);
        so_found[idx][so_nfound[idx]++] = c;
        so_score += so_dr.from == P_FOUND ? 0 : 10;
    } else {
        if (so_dr.from == P_TAB && so_dr.idx == idx) return 0;
        if (!so_fits_tab(c, idx)) return 0;
        so_take(so_dr.from, so_dr.idx, so_dr.n);
        for (int k = 0; k < so_dr.n; k++) so_tab[idx][so_ntab[idx]++] = so_dr.cards[k];
        if (so_dr.from == P_WASTE) so_score += 5;
        if (so_dr.from == P_FOUND) so_score = imax(0, so_score - 15);
    }
    so_moves++;
    so_start_clock();
    so_flip_tops();
    so_check_win();
    return 1;
}

/* the card under the mouse: which pile, which pile number, how many cards from there to the top */
static int so_pick(int lx, int ly, int *from, int *idx, int *n, int *cardx, int *cardy) {
    #define IN(x, y) (lx >= (x) && lx < (x) + SO_CW && ly >= (y) && ly < (y) + SO_CH)
    int show = so_waste_show();
    if (show && IN(so_waste_x(show - 1), SO_Y0)) { *from = P_WASTE; *idx = 0; *n = 1; *cardx = so_waste_x(show - 1); *cardy = SO_Y0; return 1; }
    for (int f = 0; f < 4; f++)
        if (so_nfound[f] && IN(so_pile_x(3 + f), SO_Y0)) { *from = P_FOUND; *idx = f; *n = 1; *cardx = so_pile_x(3 + f); *cardy = SO_Y0; return 1; }
    for (int c = 0; c < 7; c++)
        for (int j = so_ntab[c] - 1; j >= so_down[c]; j--)
            if (IN(so_pile_x(c), so_card_y(c, j)) && (j == so_ntab[c] - 1 || ly < so_card_y(c, j + 1))) {
                *from = P_TAB; *idx = c; *n = so_ntab[c] - j; *cardx = so_pile_x(c); *cardy = so_card_y(c, j); return 1;
            }
    return 0;
    #undef IN
}

/* send a card up to its pile, if it can go (double-click) */
static int so_auto_up(int from, int idx) {
    int c;
    if (from == P_WASTE) { if (!so_nwaste) return 0; c = so_waste[so_nwaste - 1]; }
    else if (from == P_TAB) { if (so_ntab[idx] == so_down[idx] || !so_ntab[idx]) return 0; c = so_tab[idx][so_ntab[idx] - 1]; }
    else return 0;
    int f = so_suit(c);
    if (!so_fits_found(c, f)) return 0;
    so_dr.on = 0; so_dr.from = from; so_dr.idx = idx; so_dr.n = 1; so_dr.cards[0] = c;
    return so_put(P_FOUND, f);
}

static void so_dlg_done(int b) {
    if (b == 0) so_new_game();
    else { int i = win_of(solitaire_draw); if (i >= 0) close_win(i); }
}
static void so_check_win(void) {
    for (int f = 0; f < 4; f++) if (so_nfound[f] != 13) return;
    so_won = 1;
    static char m[64];
    snprintf(m, sizeof m, "Score %d in %d:%02d.", so_score, so_time / 60, so_time % 60);
    dlg_open(&so_dlg, DLG_MSG, "Game won", "Congratulations, you won!", m, "Play again", "Exit", NULL, so_dlg_done);
}

static void so_turn_deck(void) {                         /* click on the deck */
    if (so_nstock == 0) {                                /* deal the turned cards again */
        if (!so_nwaste) return;
        while (so_nwaste) so_stock[so_nstock++] = so_waste[--so_nwaste];
        so_score = imax(0, so_score - (so_draw3 ? 0 : 100));
    } else {
        int n = so_draw3 ? 3 : 1;
        while (n-- && so_nstock) so_waste[so_nwaste++] = so_stock[--so_nstock];
    }
    so_moves++;
    so_start_clock();
}

/* ---- mouse and keys ---- */
static void solitaire_click(int lx, int ly, int cw, int ch) {
    if (dlg_click(&so_dlg, lx, ly, cw, ch, solitaire_draw)) return;
    int menu, r = mb_click(&so_mb, lx, ly, &menu, solitaire_draw);
    if (r >= 0) {
        if (r == 0) so_new_game();
        else if (r == 2 || r == 3) {
            so_draw3 = r == 3;
            so_items[2].accel = so_draw3 ? "" : "*"; so_items[3].accel = so_draw3 ? "*" : "";
            so_new_game();
        } else if (r == 5) { int i = win_of(solitaire_draw); if (i >= 0) close_win(i); }
        return;
    }
    if (r == -2 || so_won) return;
    long long t = ui_ms();
    int dbl = t - so_last_click < 400 && abs(lx - so_last_lx) < 6 && abs(ly - so_last_ly) < 6;
    so_last_click = dbl ? 0 : t; so_last_lx = lx; so_last_ly = ly;

    if (lx >= so_pile_x(0) && lx < so_pile_x(0) + SO_CW && ly >= SO_Y0 && ly < SO_Y0 + SO_CH) {
        so_turn_deck(); repaint_client(solitaire_draw); return;
    }
    int from, idx, n, x, y;
    if (!so_pick(lx, ly, &from, &idx, &n, &x, &y)) return;
    if (dbl && n == 1 && so_auto_up(from, idx)) { repaint_client(solitaire_draw); return; }
    /* pick the cards up */
    so_dr.on = 1; so_dr.from = from; so_dr.idx = idx; so_dr.n = n;
    so_dr.ox = lx - x; so_dr.oy = ly - y; so_dr.x = x; so_dr.y = y;
    for (int k = 0; k < n; k++)
        so_dr.cards[k] = from == P_WASTE ? so_waste[so_nwaste - 1] : from == P_FOUND ? so_found[idx][so_nfound[idx] - 1]
                                          : so_tab[idx][so_ntab[idx] - n + k];
    repaint_client(solitaire_draw);
}

static void so_hand_rect(int *x, int *y, int *w, int *h) {
    *x = so_dr.x - 2; *y = so_dr.y - 2; *w = SO_CW + 6; *h = SO_CH + (so_dr.n - 1) * SO_UP + 6;
}

static void solitaire_drag(int lx, int ly, int cw, int ch) {
    (void)cw; (void)ch;
    if (!so_dr.on) return;
    int x0, y0, w0, h0; so_hand_rect(&x0, &y0, &w0, &h0);
    so_dr.x = lx - so_dr.ox; so_dr.y = ly - so_dr.oy;
    int x1, y1, w1, h1; so_hand_rect(&x1, &y1, &w1, &h1);
    int ax = imin(x0, x1), ay = imin(y0, y1);
    repaint_client_rect(solitaire_draw, ax, ay, imax(x0 + w0, x1 + w1) - ax, imax(y0 + h0, y1 + h1) - ay);
}

static void solitaire_release(void) {
    if (!so_dr.on) return;
    /* drop on the pile the card overlaps most */
    int best = 0, bt = -1, bi = -1;
    #define OVL(px, py, pw, ph) (imax(0, imin(so_dr.x + SO_CW, (px) + (pw)) - imax(so_dr.x, (px))) * \
                                 imax(0, imin(so_dr.y + SO_CH, (py) + (ph)) - imax(so_dr.y, (py))))
    for (int f = 0; f < 4 && so_dr.n == 1; f++) {
        int o = OVL(so_pile_x(3 + f), SO_Y0, SO_CW, SO_CH);
        if (o > best && so_fits_found(so_dr.cards[0], f)) { best = o; bt = P_FOUND; bi = f; }
    }
    for (int c = 0; c < 7; c++) {
        int top = so_ntab[c] ? so_card_y(c, so_ntab[c] - 1) : SO_TY;
        int o = OVL(so_pile_x(c), top, SO_CW, SO_CH);
        if (o > best && !(so_dr.from == P_TAB && so_dr.idx == c) && so_fits_tab(so_dr.cards[0], c)) { best = o; bt = P_TAB; bi = c; }
    }
    #undef OVL
    so_dr.on = 0;
    if (bt >= 0) { so_dr.on = 0; so_put(bt, bi); }
    repaint_client(solitaire_draw);
}

static int solitaire_hit(int lx, int ly, int cw, int ch) {
    if (so_dlg.active) return dlg_hit(&so_dlg, lx, ly, cw, ch);
    return mb_hit(&so_mb, lx, ly);
}
static void solitaire_elem_rect(int id, int *x, int *y, int *w, int *h) {
    int i = win_of(solitaire_draw); if (i < 0) { *x = *y = *w = *h = 0; return; }
    int cx, cy, cw, ch; client_rect(&wins[i], &cx, &cy, &cw, &ch);
    if (dlg_elem_rect(&so_dlg, id, cw, ch, x, y, w, h)) return;
    if (mb_elem_rect(&so_mb, id, x, y, w, h)) return;
    *x = *y = *w = *h = 0;
}
static void solitaire_key(int code, int mods) {
    if (dlg_key(&so_dlg, code, mods, solitaire_draw)) return;
    if (code == KEY_ESC && so_mb.open >= 0) { so_mb.open = -1; repaint_client(solitaire_draw); }
    if (code == KEY_F2) so_new_game();
}
static void solitaire_tick(void) {
    if (!so_started || so_won) return;
    int t = (int)(time(NULL) - so_t0);
    if (t != so_time) { so_time = t; repaint_client_rect(solitaire_draw, 0, SO_H - SO_STATUS, SO_W, SO_STATUS); }
}

/* a small card with a heart: the Solitaire icon */
static void draw_cards_icon(int cx, int cy, int r) {
    int w = r * 3 / 2, h = r * 2, x = cx - w / 2 + r / 4, y = cy - r;
    fill_round(x - r / 2, y + 1, w, h - 1, 3, RGB(0x1c, 0x5c, 0xc0), 255);      /* a card behind */
    stroke_round(x - r / 2, y + 1, w, h - 1, 3, RGB(255, 255, 255), 200);
    fill_round(x, y, w, h, 3, RGB(0x70, 0x74, 0x7c), 255);
    fill_round(x + 1, y + 1, w - 2, h - 2, 2, RGB(255, 255, 255), 255);
    so_draw_suit(x + w / 2, y + h / 2, imax(3, r * 2 / 3), 1, RGB(0xd0, 0x18, 0x20));
}
