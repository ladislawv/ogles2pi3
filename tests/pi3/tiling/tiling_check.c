/*
 * vc4core's texture store a utile at a time (tiling.c's vc4_texture_store_box,
 * 2026-11-19) checked against the texel-at-a-time store it replaced, byte for
 * byte: T and LT levels (the layout's own choice by size), 1, 2, 4 and 8 bytes
 * a texel, sizes on and off the utile and tile bounds, every mip level, one
 * face and six, whole levels and boxes at odd offsets. Plain C, no GPU:
 *   bash tests/pi3/tiling/build.sh && ~/ogles2-build-pi3/pi3/tiling_check
 * MIT licence (LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vc4core.h"

/* The store before 2026-11-19: every texel through vc4_texel_offset. */
static void ref_store_box(uint8_t *base, const struct vc4_texture_layout *t, uint32_t level, uint32_t face,
                          const uint8_t *src, uint32_t src_stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const struct vc4_level *l = &t->level[level];
    uint32_t cpp = t->cpp, i, j;
    uint8_t *img = base + face * t->face_stride + l->offset;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            uint8_t *d = img + vc4_texel_offset(l, cpp, x + i, y + j);
            const uint8_t *s = src + j * src_stride + i * cpp;
            memcpy(d, s, cpp);
        }
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 8;
}

int main(void)
{
    static const uint32_t sizes[] = {1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 256, 300};
    static const uint32_t cpps[] = {1, 2, 4, 8};
    uint32_t nsz = sizeof sizes / sizeof sizes[0], ci, wi, hi, cases = 0, boxes = 0, fails = 0;
    for (ci = 0; ci < 4; ci++)
        for (wi = 0; wi < nsz; wi++)
            for (hi = 0; hi < nsz; hi += (wi % 3) + 1) {
                uint32_t cpp = cpps[ci], w = sizes[wi], h = sizes[hi], levels = 1, faces, lv, k, f, m;
                struct vc4_texture_layout lay;
                uint8_t *a, *b, *src;
                for (m = w > h ? w : h; m > 1; m >>= 1)
                    levels++;
                if (levels > VC4_MAX_LEVELS)
                    levels = VC4_MAX_LEVELS;
                faces = (w == h && (wi & 1)) ? 6 : 1;
                vc4_texture_layout(&lay, w, h, cpp, levels, (int)faces);
                a = malloc(lay.size);
                b = malloc(lay.size);
                src = malloc((size_t)(w + 8) * (h + 8) * cpp + 64);
                for (m = 0; m < lay.size; m++)
                    a[m] = b[m] = (uint8_t)(m * 13 + 7);
                for (m = 0; m < (w + 8) * (h + 8) * cpp + 64; m++)
                    src[m] = (uint8_t)rnd();
                for (f = 0; f < faces; f++)
                    for (lv = 0; lv < levels; lv++) {
                        const struct vc4_level *l = &lay.level[lv];
                        uint32_t stride = (l->width + 3) * cpp + (rnd() & 1); /* odd strides too: any alignment */
                        /* the whole level, then boxes over it */
                        vc4_texture_store(a, &lay, lv, f, src + 1, stride);
                        ref_store_box(b, &lay, lv, f, src + 1, stride, 0, 0, l->width, l->height);
                        for (k = 0; k < 6; k++) {
                            uint32_t bx = rnd() % l->width, by = rnd() % l->height;
                            uint32_t bw = 1 + rnd() % (l->width - bx), bh = 1 + rnd() % (l->height - by);
                            uint32_t off = rnd() % 5;
                            vc4_texture_store_box(a, &lay, lv, f, src + off, stride, bx, by, bw, bh);
                            ref_store_box(b, &lay, lv, f, src + off, stride, bx, by, bw, bh);
                            boxes++;
                        }
                    }
                cases++;
                if (memcmp(a, b, lay.size)) {
                    for (m = 0; m < lay.size && a[m] == b[m]; m++)
                        ;
                    printf("FAIL cpp %u %ux%u (%u levels, %u faces, level 0 %s): first difference at byte %u\n", cpp, w,
                           h, levels, faces, lay.level[0].tiling == VC4_TILING_LT ? "LT" : "T", m);
                    fails++;
                }
                free(a);
                free(b);
                free(src);
            }
    printf("%u textures, %u boxes and their whole levels: %u differ\n", cases, boxes, fails);
    return fails != 0;
}
