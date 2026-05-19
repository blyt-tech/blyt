/*
 * testcard.c — Philips PM5544-style test card renderer.
 *
 * Fills a 320×240 XRGB8888 buffer with the test card for a given frame
 * counter.  Called by the session on every frame until the cart issues a
 * drawing call, making the test card invisible to the frontend: it simply
 * appears as normal cart output.
 */

#include "testcard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- Palette indices ---- */
#define TC_BLACK 0
#define TC_DARK1 1
#define TC_DARK2 2
#define TC_BG 3
#define TC_DARK3 4
#define TC_LITE1 5
#define TC_WHITE 6
#define TC_YELLOW 7
#define TC_CYAN 8
#define TC_GREEN 9
#define TC_MAGENTA 10
#define TC_RED 11
#define TC_BLUE 12
#define TC_BLUESIDE 13
#define TC_GOLD 14
#define TC_TEAL 15
#define TC_PINK 16
#define TC_OLIVE 17
#define TC_PURPLE 18
#define TC_TEXT 20

#define TC_W 320
#define TC_H 240
#define TC_CX 160
#define TC_CY 120
#define TC_CR 110

/* TILE=18: 17×13 grey squares centred on screen (matches original PM5544). */
#define TILE 18
#define GS_L 7
#define GS_R 313
#define GS_T 3
#define GS_B 237

#define SB_T 21
#define SB_B 219
#define SH_T (SB_T + 2 * TILE)
#define SH_B (SB_B - 2 * TILE)

#define BSQ_LX1 24
#define BSQ_LX2 108
#define BSQ_RX1 213
#define BSQ_RX2 296
#define BSQ_Y1 (GS_T + 2 * TILE)
#define BSQ_Y2 (GS_T + 7 * TILE)

#define STRIPE_X 115
#define TBAR_X1 121
#define TBAR_X2 199
#define TBAR_Y1 (GS_T + 1 * TILE)
#define TBAR_Y2 (GS_T + 2 * TILE)

#define EVEN_Y1 (GS_T + 3 * TILE)
#define EVEN_Y2 (GS_T + 4 * TILE)

#define CB_Y1 (GS_T + 4 * TILE)
#define CB_Y2 (GS_T + 6 * TILE)
#define CB_X1 90
#define CB_X2 125
#define CB_X3 160
#define CB_X4 195
#define CB_X5 230

#define WAVE_Y2 (GS_T + 8 * TILE)
#define WV1_X1 73
#define WV1_X2 117
#define WV2_X1 108
#define WV2_X2 152
#define WV3_X1 143
#define WV3_X2 248

#define GS_Y1 (GS_T + 8 * TILE)
#define GS_Y2 (GS_T + 10 * TILE)

#define WBW_Y1 (GS_T + 10 * TILE)
#define WBW_Y2 (GS_T + 11 * TILE)
#define WBW_BX1 108
#define WBW_BX2 213
#define WSTRIPE_X 115

#define YR_Y1 (GS_T + 11 * TILE)
#define RED_X1 151
#define RED_X2 169

#define XHAIR_HY1 (GS_T + 6 * TILE)
#define XHAIR_HY2 (GS_T + 7 * TILE)
#define XHAIR_VX1 152
#define XHAIR_VX2 168
#define XHAIR_VY1 (GS_T + 5 * TILE)
#define XHAIR_VY2 (GS_T + 8 * TILE)

#define FC_Y (WBW_Y1 + (WBW_Y2 - WBW_Y1 - 7) / 2)

/* ---- Static state ---- */

/* Active pixel buffer for the current draw call (set in blyt_testcard_draw). */
static uint8_t *s_pixels;

/* 5-wide × 7-tall glyphs — each byte is one row, bit 4 = left, bit 0 = right */
static const uint8_t g_0[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
static const uint8_t g_1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
static const uint8_t g_2[7] = {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F};
static const uint8_t g_3[7] = {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E};
static const uint8_t g_4[7] = {0x11, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x01};
static const uint8_t g_5[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x11, 0x0E};
static const uint8_t g_6[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
static const uint8_t g_7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
static const uint8_t g_8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
static const uint8_t g_9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E};
static const uint8_t g_B[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
static const uint8_t g_L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
static const uint8_t g_T[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
static const uint8_t g_Y[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};

static const uint8_t *tc_glyph(char c) {
    switch (c) {
    case '0':
        return g_0;
    case '1':
        return g_1;
    case '2':
        return g_2;
    case '3':
        return g_3;
    case '4':
        return g_4;
    case '5':
        return g_5;
    case '6':
        return g_6;
    case '7':
        return g_7;
    case '8':
        return g_8;
    case '9':
        return g_9;
    case 'B':
        return g_B;
    case 'L':
        return g_L;
    case 'T':
        return g_T;
    case 'Y':
        return g_Y;
    default:
        return NULL;
    }
}

static void tc_pset(int x, int y, uint8_t color) {
    if ((unsigned)x < TC_W && (unsigned)y < TC_H)
        s_pixels[y * TC_W + x] = color;
}

static void tc_draw_char(int x0, int y0, const uint8_t *g, uint8_t color, int scale) {
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (g[row] & (0x10u >> col)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        tc_pset(x0 + col * scale + sx, y0 + row * scale + sy, color);
            }
        }
    }
}

static void tc_draw_text(int x0, int y0, const char *str, uint8_t color, int scale) {
    for (int x = x0; *str; str++, x += (5 + 1) * scale) {
        const uint8_t *g = tc_glyph(*str);
        if (g)
            tc_draw_char(x, y0, g, color, scale);
    }
}

static int tc_itoa(uint32_t n, char *buf) {
    if (n == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    char tmp[12];
    int i = 0;
    while (n) {
        tmp[i++] = '0' + (char)(n % 10);
        n /= 10;
    }
    for (int j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return i;
}

static void init_palette(uint32_t *p) {
    memset(p, 0, 256 * sizeof(uint32_t));
    p[TC_BLACK] = 0x00000000;
    p[TC_DARK1] = 0x00343434;
    p[TC_DARK2] = 0x00666666;
    p[TC_BG] = 0x00777777;
    p[TC_DARK3] = 0x00999999;
    p[TC_LITE1] = 0x00CCCCCC;
    p[TC_WHITE] = 0x00FFFFFF;
    p[TC_YELLOW] = 0x00CCCC00;
    p[TC_CYAN] = 0x0000CCCC;
    p[TC_GREEN] = 0x0000CC00;
    p[TC_MAGENTA] = 0x00CC00CC;
    p[TC_RED] = 0x00CC0000;
    p[TC_BLUE] = 0x000000CC;
    p[TC_BLUESIDE] = 0x00587FE6;
    p[TC_GOLD] = 0x00A78019;
    p[TC_TEAL] = 0x0026AD80;
    p[TC_PINK] = 0x00D95280;
    p[TC_OLIVE] = 0x0080960E;
    p[TC_PURPLE] = 0x008069F1;
    p[TC_TEXT] = 0x00D0D0D0;
}

static uint8_t bar_color(int x) {
    if (x < CB_X1)
        return TC_YELLOW;
    if (x < CB_X2)
        return TC_CYAN;
    if (x < CB_X3)
        return TC_GREEN;
    if (x < CB_X4)
        return TC_MAGENTA;
    if (x < CB_X5)
        return TC_RED;
    return TC_BLUE;
}

static uint8_t side_color(int x, int y) {
    if (y < SB_T + 1 || y > SB_B - 2)
        return 0;
    bool upper = (y < TC_CY);
    bool corner = (y <= SH_T - 2 || y >= SH_B + 1);
    if (x >= GS_L + TILE + 1 && x <= GS_L + 2 * TILE - 1) {
        if (x == GS_L + 2 * TILE - 1 && !corner)
            return 0;
        return upper ? TC_TEAL : TC_PINK;
    }
    if (x >= GS_L + 2 * TILE && x <= GS_L + 3 * TILE - 2 && corner)
        return upper ? TC_BLUESIDE : TC_GOLD;
    if (x >= GS_R - 3 * TILE + 1 && x <= GS_R - 2 * TILE - 1 && corner)
        return upper ? TC_BLUESIDE : TC_GOLD;
    if (x >= GS_R - 2 * TILE && x <= GS_R - TILE - 2) {
        if (x == GS_R - 2 * TILE && !corner)
            return 0;
        return upper ? TC_OLIVE : TC_PURPLE;
    }
    return 0;
}

/* ---- Public API ---- */

void blyt_testcard_init_palette(uint32_t *palette_out) {
    init_palette(palette_out);
}

void blyt_testcard_draw(uint32_t frame_count, uint8_t *pixels_out) {
    s_pixels = pixels_out;

    int r2 = TC_CR * TC_CR;

    for (int y = 0; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            int dx = x - TC_CX, dy = y - TC_CY;
            int d2 = dx * dx + dy * dy;
            bool in_circ = (d2 <= r2);
            uint8_t c;

            /* 1. Castellations aligned to the grey-square tile grid */
            int tc_col = (x - GS_L + 2 * TILE) / TILE;
            int tc_row = (y - GS_T + 2 * TILE) / TILE;
            int tc_tx = (x - GS_L + 2 * TILE) % TILE;
            int tc_ty = (y - GS_T + 2 * TILE) % TILE;
            bool tc_border = (tc_tx == 0 || tc_tx == TILE - 1 || tc_ty == 0 || tc_ty == TILE - 1);
            bool tc_dark = ((tc_col + tc_row) % 2 == 1);
            c = (tc_border || !tc_dark) ? TC_WHITE : TC_BLACK;

            /* 2. Grey-square background */
            if (x >= GS_L && x <= GS_R && y >= GS_T && y <= GS_B) {
                int tx = (x - GS_L) % TILE, ty = (y - GS_T) % TILE;
                c = (tx > 0 && tx < TILE - 1 && ty > 0 && ty < TILE - 1) ? TC_BG : TC_WHITE;
            }

            /* 3. Side colour bars */
            if (!in_circ && x >= GS_L && x <= GS_R) {
                uint8_t sc = side_color(x, y);
                if (sc)
                    c = sc;
            }

            /* 4. Circle interior */
            if (in_circ) {
                c = TC_WHITE;

                if (y >= BSQ_Y1 && y < BSQ_Y2)
                    if ((x >= BSQ_LX1 && x < BSQ_LX2) || (x >= BSQ_RX1 && x < BSQ_RX2))
                        c = TC_BLACK;

                if (y >= TBAR_Y1 && y < TBAR_Y2 && x >= TBAR_X1 && x < TBAR_X2)
                    c = TC_BLACK;

                if (x >= STRIPE_X && x < STRIPE_X + 2 && y >= BSQ_Y1)
                    c = TC_BLACK;

                if (y >= EVEN_Y1 && y < EVEN_Y2)
                    c = (x % 25 < 12) ? TC_LITE1 : TC_BLACK;

                if (y >= CB_Y1 && y < CB_Y2)
                    c = bar_color(x);

                if (y >= CB_Y2)
                    c = TC_BLACK;

                if (y >= CB_Y2 && y < WAVE_Y2) {
                    if (x >= WV1_X1 && x < WV1_X2)
                        c = ((x - WV1_X1) % 8 < 4) ? TC_BLACK : TC_DARK2;
                    else if (x >= WV2_X1 && x < WV2_X2)
                        c = ((x - WV2_X1) % 3 < 2) ? TC_BLACK : TC_DARK2;
                    else if (x >= WV3_X1 && x < WV3_X2)
                        c = TC_DARK2;
                }

                if (y >= GS_Y1 && y < GS_Y2) {
                    if (x < CB_X1)
                        c = TC_BLACK;
                    else if (x < CB_X2)
                        c = TC_DARK1;
                    else if (x < CB_X3)
                        c = TC_DARK2;
                    else if (x < CB_X4)
                        c = TC_DARK3;
                    else if (x < CB_X5)
                        c = TC_LITE1;
                    else
                        c = TC_WHITE;
                }

                if (y >= WBW_Y1 && y < WBW_Y2) {
                    c = (x >= WBW_BX1 && x < WBW_BX2) ? TC_BLACK : TC_WHITE;
                    if (x >= WSTRIPE_X && x < WSTRIPE_X + 2)
                        c = TC_WHITE;
                }

                if (y >= YR_Y1)
                    c = (x >= RED_X1 && x < RED_X2) ? TC_RED : TC_YELLOW;

                bool in_xh = (y >= XHAIR_HY1 && y < XHAIR_HY2) ||
                             (x >= XHAIR_VX1 && x < XHAIR_VX2 && y >= XHAIR_VY1 && y < XHAIR_VY2);
                if (in_xh) {
                    int tx = (x - GS_L) % TILE;
                    c = (tx == 0 || tx == TILE - 1) ? TC_WHITE : TC_BLACK;
                    if ((x == TC_CX || x == TC_CX - 1) || y == TC_CY)
                        c = TC_WHITE;
                }
            }

            s_pixels[y * TC_W + x] = c;
        }
    }

    tc_draw_text(TC_CX - 35, TBAR_Y1 + (TBAR_Y2 - TBAR_Y1 - 14) / 2, "BLYT32", TC_WHITE, 2);

    char numstr[12];
    int nlen = tc_itoa(frame_count, numstr);
    tc_draw_text(TC_CX - (nlen * 6 - 1) / 2, FC_Y, numstr, TC_TEXT, 1);
}
