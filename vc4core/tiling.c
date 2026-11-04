/*
 * vc4core's texture layouts (docs/PI3_PLAN.md §3.1), after Mesa's vc4
 * (MIT: vc4_tiling.c's description and t_utile_address, vc4_resource.c's
 * vc4_setup_slices), written for vc4core.
 *
 * A level is made of utiles of 64 bytes (4x4 pixels at 4 bytes a texel; 8x8
 * at 1, 8x4 at 2, 2x4 at 8), its pixels in raster order. LT: the utiles in
 * raster order. T: 4 KB tiles of 2x2 subtiles of 1 KB (4x4 utiles each, in
 * raster order); on even rows of tiles the subtiles go (BL, TL, TR, BR), on
 * odd rows (TR, BR, BL, TL), and odd rows of tiles run right to left. The
 * TMU takes a level as LT when it is at most 4 utiles wide or high, as T
 * otherwise; so the miptree is laid out by that rule, from the smallest
 * level up, then shifted so that level 0 starts on a page (P0 holds its
 * address in 4 KB units).
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"

uint32_t vc4_utile_width(uint32_t cpp)
{
    return cpp == 1 || cpp == 2 ? 8 : cpp == 4 ? 4 : 2;
}

uint32_t vc4_utile_height(uint32_t cpp)
{
    return cpp == 1 ? 8 : 4;
}

int vc4_size_is_lt(uint32_t w, uint32_t h, uint32_t cpp)
{
    return w <= 4 * vc4_utile_width(cpp) || h <= 4 * vc4_utile_height(cpp);
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static uint32_t pot(uint32_t v)
{
    uint32_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

void vc4_texture_layout(struct vc4_texture_layout *t, uint32_t w, uint32_t h, uint32_t cpp, uint32_t levels,
                        int faces)
{
    uint32_t uw = vc4_utile_width(cpp), uh = vc4_utile_height(cpp);
    uint32_t pw = pot(w), ph = pot(h), offset = 0, shift;
    int i;
    t->width = w;
    t->height = h;
    t->cpp = cpp;
    t->levels = levels;
    for (i = (int)levels - 1; i >= 0; i--) {
        struct vc4_level *l = &t->level[i];
        uint32_t lw = i ? (pw >> i ? pw >> i : 1) : w, lh = i ? (ph >> i ? ph >> i : 1) : h;
        l->width = lw;
        l->height = lh;
        if (vc4_size_is_lt(lw, lh, cpp)) {
            l->tiling = VC4_TILING_LT;
            lw = align_up(lw, uw);
            lh = align_up(lh, uh);
        } else {
            l->tiling = VC4_TILING_T;
            lw = align_up(lw, 8 * uw);
            lh = align_up(lh, 8 * uh);
        }
        l->offset = offset;
        l->stride = lw * cpp;
        l->padded_height = lh;
        offset += lh * l->stride;
    }
    shift = align_up(t->level[0].offset, 4096) - t->level[0].offset;
    for (i = 0; i < (int)levels; i++)
        t->level[i].offset += shift;
    /* each face a whole miptree at a page-aligned offset from the first */
    t->face_stride = align_up(offset + shift, 4096);
    t->size = t->face_stride * (uint32_t)(faces > 1 ? faces : 1);
}

/* Mesa's t_utile_address: the 1 KB subtile of utile (ux, uy) (both multiples
 * of 4) in a T image utile_stride utiles wide. */
static uint32_t t_subtile(uint32_t ux, uint32_t uy, uint32_t utile_stride)
{
    static const uint32_t odd_map[4] = {2, 1, 3, 0}, even_map[4] = {0, 3, 1, 2};
    uint32_t tile_stride = utile_stride >> 3, tx = ux >> 3, ty = uy >> 3;
    uint32_t s = (((uy >> 2) & 1) << 1) + ((ux >> 2) & 1);
    if (ty & 1)
        tx = tile_stride - tx - 1;
    return 4096 * (ty * tile_stride + tx) + 1024 * ((ty & 1) ? odd_map[s] : even_map[s]);
}

uint32_t vc4_texel_offset(const struct vc4_level *l, uint32_t cpp, uint32_t x, uint32_t y)
{
    uint32_t uw = vc4_utile_width(cpp), uh = vc4_utile_height(cpp);
    uint32_t ux = x / uw, uy = y / uh, in = (y % uh) * uw * cpp + (x % uw) * cpp;
    uint32_t utile_stride = l->stride / cpp / uw;
    if (l->tiling == VC4_TILING_RASTER)
        return y * l->stride + x * cpp;
    if (l->tiling == VC4_TILING_LT)
        return (uy * utile_stride + ux) * 64 + in;
    return t_subtile(ux & ~3u, uy & ~3u, utile_stride) + ((uy & 3) * 4 + (ux & 3)) * 64 + in;
}

void vc4_texture_store(uint8_t *base, const struct vc4_texture_layout *t, uint32_t level, uint32_t face,
                       const uint8_t *src, uint32_t src_stride)
{
    const struct vc4_level *l = &t->level[level];
    uint32_t cpp = t->cpp, x, y;
    uint8_t *img = base + face * t->face_stride + l->offset;
    for (y = 0; y < l->height; y++) {
        const uint8_t *s = src + y * src_stride;
        for (x = 0; x < l->width; x++, s += cpp) {
            uint8_t *d = img + vc4_texel_offset(l, cpp, x, y);
            switch (cpp) {
            case 1:
                *d = s[0];
                break;
            case 2:
                vc4_put16(d, (uint32_t)s[0] | (uint32_t)s[1] << 8);
                break;
            case 8:
                vc4_put32(d + 4, (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24);
                /* fall through */
            default:
                vc4_put32(d, (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
                break;
            }
        }
    }
}
