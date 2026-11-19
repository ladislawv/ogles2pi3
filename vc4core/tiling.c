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

/* A long read at any alignment, its bytes as they are in memory (the
 * compiler makes the load the target allows: one move on the 68020 and x86,
 * bytes on an ARM that needs them). */
struct __attribute__((packed, may_alias)) vc4_u32_any
{
    uint32_t v;
};

/* One texel's bytes from s to d (d naturally aligned), as they are. */
static void put_texel(uint8_t *d, const uint8_t *s, uint32_t cpp)
{
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

/* Texels [x0, x1) of row y of the box, a texel at a time. */
static void put_texels(uint8_t *img, const struct vc4_level *l, uint32_t cpp, const uint8_t *row, uint32_t x,
                       uint32_t x0, uint32_t x1, uint32_t y)
{
    uint32_t i;
    for (i = x0; i < x1; i++)
        put_texel(img + vc4_texel_offset(l, cpp, i, y), row + (i - x) * cpp, cpp);
}

/* A box of a level, a utile at a time (2026-11-19, ogles3's 16-bit upload
 * report: a texel at a time, through vc4_texel_offset, every upload on a GPU
 * without a TFU): a utile's address once, each of its rows (8 or 16 bytes,
 * the utile's width) moved with naturally aligned 32-bit stores of the
 * source's bytes as they are (GPU memory may be device memory: no memcpy
 * into it). The utiles the box only partly covers, a texel at a time. */
void vc4_texture_store_box(uint8_t *base, const struct vc4_texture_layout *t, uint32_t level, uint32_t face,
                           const uint8_t *src, uint32_t src_stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const struct vc4_level *l = &t->level[level];
    uint32_t cpp = t->cpp, uw = vc4_utile_width(cpp), uh = vc4_utile_height(cpp), rb = uw * cpp;
    uint32_t ux0 = (x + uw - 1) / uw, ux1 = (x + w) / uw, uy0 = (y + uh - 1) / uh, uy1 = (y + h) / uh;
    uint32_t ux, uy, r, j, b;
    uint8_t *img = base + face * t->face_stride + l->offset;
    if (l->tiling == VC4_TILING_RASTER || ux0 >= ux1 || uy0 >= uy1) {
        for (j = 0; j < h; j++)
            put_texels(img, l, cpp, src + j * src_stride, x, x, x + w, y + j);
        return;
    }
    /* the rows above and below the whole utiles */
    for (j = y; j < uy0 * uh; j++)
        put_texels(img, l, cpp, src + (j - y) * src_stride, x, x, x + w, j);
    for (j = uy1 * uh; j < y + h; j++)
        put_texels(img, l, cpp, src + (j - y) * src_stride, x, x, x + w, j);
    /* their rows' texels left and right of them */
    for (j = uy0 * uh; j < uy1 * uh; j++) {
        const uint8_t *row = src + (j - y) * src_stride;
        put_texels(img, l, cpp, row, x, x, ux0 * uw, j);
        put_texels(img, l, cpp, row, x, ux1 * uw, x + w, j);
    }
    /* the whole utiles */
    for (uy = uy0; uy < uy1; uy++)
        for (ux = ux0; ux < ux1; ux++) {
            uint8_t *d = img + vc4_texel_offset(l, cpp, ux * uw, uy * uh);
            const uint8_t *s = src + (uy * uh - y) * src_stride + (ux * uw - x) * cpp;
            for (r = 0; r < uh; r++, d += rb, s += src_stride)
                for (b = 0; b < rb; b += 4) /* from RAM, any alignment; into the GPU's, aligned */
                    *(volatile uint32_t *)(d + b) = ((const struct vc4_u32_any *)(s + b))->v;
        }
}

void vc4_texture_store(uint8_t *base, const struct vc4_texture_layout *t, uint32_t level, uint32_t face,
                       const uint8_t *src, uint32_t src_stride)
{
    vc4_texture_store_box(base, t, level, face, src, src_stride, 0, 0, t->level[level].width,
                          t->level[level].height);
}
