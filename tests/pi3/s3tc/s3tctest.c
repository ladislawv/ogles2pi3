/*
 * The VC4 backend's S3TC decoder (ogles2/vc4/s3tc.c) against Mesa's
 * (util/format's dxt*_unpack_rgba_8unorm, linked from the host Mesa build):
 * random blocks of each type, and the blocks where DXT1's two modes and
 * DXT5's alpha modes meet, decoded by both, texel for texel.
 *   bash tests/pi3/s3tc/build.sh   (in WSL)
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "s3tc.h"

/* Mesa's (src/util/format/u_format_s3tc.h at the validated commit) */
void util_format_dxt1_rgb_unpack_rgba_8unorm(uint8_t *dst_row, unsigned dst_stride, const uint8_t *src_row,
                                             unsigned src_stride, unsigned width, unsigned height);
void util_format_dxt1_rgba_unpack_rgba_8unorm(uint8_t *dst_row, unsigned dst_stride, const uint8_t *src_row,
                                              unsigned src_stride, unsigned width, unsigned height);
void util_format_dxt3_rgba_unpack_rgba_8unorm(uint8_t *dst_row, unsigned dst_stride, const uint8_t *src_row,
                                              unsigned src_stride, unsigned width, unsigned height);
void util_format_dxt5_rgba_unpack_rgba_8unorm(uint8_t *dst_row, unsigned dst_stride, const uint8_t *src_row,
                                              unsigned src_stride, unsigned width, unsigned height);

static uint32_t seed = 12345;
static unsigned rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

int main(void)
{
    static const char *const names[4] = {"DXT1 RGB", "DXT1 RGBA", "DXT3", "DXT5"};
    int type, n, bad = 0, total = 0;
    for (type = 0; type < 4; type++) {
        int bytes = type < 2 ? 8 : 16, wrong = 0;
        for (n = 0; n < 20000; n++) {
            unsigned char blk[16], ours[16][4], mesa[4 * 4 * 4];
            int i;
            for (i = 0; i < bytes; i++)
                blk[i] = (unsigned char)rnd();
            /* the edges: equal colours, equal alphas, each order */
            if (n % 7 == 0) {
                int c = type < 2 ? 0 : 8;
                blk[c + 2] = blk[c + 0];
                blk[c + 3] = blk[c + 1];
            }
            if (type == 3 && n % 5 == 0)
                blk[1] = blk[0];
            vc4_s3tc_block(blk, type, ours);
            memset(mesa, 0x77, sizeof mesa);
            switch (type) {
            case 0: util_format_dxt1_rgb_unpack_rgba_8unorm(mesa, 16, blk, bytes, 4, 4); break;
            case 1: util_format_dxt1_rgba_unpack_rgba_8unorm(mesa, 16, blk, bytes, 4, 4); break;
            case 2: util_format_dxt3_rgba_unpack_rgba_8unorm(mesa, 16, blk, bytes, 4, 4); break;
            default: util_format_dxt5_rgba_unpack_rgba_8unorm(mesa, 16, blk, bytes, 4, 4); break;
            }
            for (i = 0; i < 16; i++)
                if (memcmp(ours[i], mesa + 4 * i, 4)) {
                    if (wrong < 3)
                        printf("  %s block %d texel %d: ours %02x%02x%02x%02x, Mesa's %02x%02x%02x%02x\n", names[type],
                               n, i, ours[i][0], ours[i][1], ours[i][2], ours[i][3], mesa[4 * i], mesa[4 * i + 1],
                               mesa[4 * i + 2], mesa[4 * i + 3]);
                    wrong++;
                    break;
                }
        }
        printf("%-10s 20000 blocks: %s\n", names[type], wrong ? "DIFFERENT" : "Mesa's texels");
        bad += wrong != 0;
        total++;
    }
    printf("%s (%d of %d types)\n", bad ? "FAILED" : "all equal to Mesa's", total - bad, total);
    return bad ? 1 : 0;
}
