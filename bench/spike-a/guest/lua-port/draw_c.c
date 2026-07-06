/*
 * draw_c.c — native-C twin of draw_bench.lua (Doom R_DrawColumn analog).
 * SAME affine paletted texture-mapped columns + colormap, SAME fixed-point
 * arithmetic and i32 wrap (uint32 where Lua's int32 wraps), so it lands on the
 * Lua version's framebuffer checksum. Compiled C, no Lua VM — the "native
 * renderer under emulation vs host-Lua per-pixel" comparison point.
 * bench: draw_c <repeats>.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 320
#define H 240
#define TW 64
#define TH 128
#define FRACBITS 16
#define FRACUNIT (1 << FRACBITS)
#define TEXMASK (TH - 1)
#define FRACWRAP (FRACUNIT * TH - 1)

static int32_t tex[TW * TH];
static int32_t cm[256];
static int32_t fb[W * H];

int main(int argc, char **argv) {
    long repeats = (argc > 1) ? atol(argv[1]) : 1000;
    for (int i = 0; i < TW * TH; i++)
        tex[i] = (int32_t)(((uint32_t)i * 1103515245u + 12345u) & 0xff);
    for (int i = 0; i < 256; i++)
        cm[i] = (i * 7 + 13) & 0xff;

    for (long r = 0; r < repeats; r++) {
        for (int x = 0; x < W; x++) {
            int col = (x & (TW - 1)) * TH;
            int step = (FRACUNIT * TH) / H + ((x & 15) << (FRACBITS - 6));
            int frac = (x * 131) & FRACWRAP;
            int o = x;
            for (int y = 0; y < H; y++) {
                fb[o] = cm[tex[col + ((frac >> FRACBITS) & TEXMASK)]];
                o += W;
                frac += step;
            }
        }
    }
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < W * H; i++)
        h = (h ^ (uint32_t)fb[i]) * 0x01000193u;
    printf("[doom] repeats=%ld checksum=%ld\n", repeats, (long)(h & 0x7fffffff));
    return 0;
}
