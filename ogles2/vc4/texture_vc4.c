/*
 * The VC4 backend's texture storage (ogles2/backend.h, docs/PI3_PLAN.md
 * §5.1, P5): the stored formats (key_vc4.c's table), the miptree in
 * vc4core's layout (tiling.c: Mesa's, levels from the smallest, level 0
 * page aligned; T and LT tiling), stores from the RAM copies and loads back
 * (a texture a framebuffer object's job drew into), renaming, a level as a
 * render target, and a draw's sampler: the TMU's configuration words, which
 * VC4 takes from the fragment shader's uniform stream.
 *
 * The RAM copies are RGBA8 (texture.c); each format is packed from them:
 * RGBA8 and RGB8 as bytes R, G, B, A (RGBX8888); 16-bit formats in GL's
 * packing, little-endian; LUMINANCE and ALPHA one byte (the TMU's ALPHA type,
 * the key's swizzle tells them apart); LUMINANCE_ALPHA two. BGRA is
 * converted to RGBA by texture.c, so it is stored as RGBA8; ETC1 is decoded
 * there, so it is stored as RGB8. Depth textures (their RAM copies hold a
 * depth word a texel: 16 bits, or 24 bits over the stencil byte) are Z24S8
 * words, the depth in the top 24 bits, as the tile buffer stores them (a
 * job draws into one: tex_zs_surface) and the shader reads them (Mesa's);
 * sampled nearest (filtering would mix the word's bytes). S3TC levels (their
 * RAM copies hold the blocks) are decoded as they are stored (s3tc.c, Mesa's
 * texels) into RGBA8: VC4's TMU has no BC types. Half-float and float
 * textures (their RAM copies hold RGBA floats) are four planes of the S16F
 * type, one a channel, each a whole miptree (all faces), plane k at k plane
 * strides (4 KB multiples) from the first, halves little-endian
 * (OES_texture_float stored as halves: a documented deviation, §2.1),
 * filtered by the TMU; the shader makes a lookup a plane, the uniform
 * stream's P0 for plane k pointing at it (tools/v3dglslc/patches/0018; not
 * the RGBA64 type, whose second result word came back stale on the Pi 3,
 * §2 "Textures").
 *
 * The TMU returns an S16F texel without its sign (§2 "Textures", found
 * 2026-11-06: 0xbc00 reads as 0x3c00), so a texture holding negative values has
 * four more planes: the first four then hold max(v, 0), these max(-v, 0), and
 * the shader subtracts (patch 0022, the sampler key's half_signed: eight lookups
 * a sample). Filtering stays exact, being linear. They are a block of their
 * own, made the first time a negative value appears; until then the memory
 * they would hold is all zeros, and other textures pay nothing.
 *
 * A job draws into a half-float texture (EXT_color_buffer_half_float, §2.1)
 * through a second image: the tile buffer's 64-bit mode stores RGBA16F pixels,
 * which the planes are not, so each level drawn has them in an image of 8-byte
 * texels (rt, its own layout). Which of the two is current goes by level:
 * rt_fresh says the image is, planes_stale that the planes are behind it. The
 * planes become the image when a job first takes the level (compose); the image
 * becomes the planes when the texture is next sampled or stored into (split).
 * Both on the CPU: a GPU split would need an S16F store the tile buffer does
 * not have.
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "../texture_private.h"
#include "vc4core.h"
#include "vc4_os.h"
#include "v3d_types.h"
#include "v3d_gpu_lock.h"
#include "key_vc4.h"
#include "s3tc.h"
#include "backend_vc4.h"

/* The GPU copy (the front end's t->gpu, its type opaque here). */
struct vc4_tex
{
    struct v3d_mem mem;
    struct vc4_texture_layout lay;
    const struct vc4_tex_format *f;
    int cls;               /* the RAM copies' class (texture_private.h): RGBA8, FLOAT, DEPTH16, DEPTH24 */
    ULONG plane_stride;    /* FLOAT's S16F planes: bytes from one to the next; 0: one plane */
    struct v3d_mem neg;    /* the negative values' four planes (the top); size 0: none yet */
    /* a half-float texture drawn into (the top) */
    struct v3d_mem rt;
    struct vc4_texture_layout rt_lay;
    ULONG rt_fresh[6], planes_stale[6]; /* by face, a bit a level */
    BOOL any_stale;
};

#define VT(t) ((struct vc4_tex *)(t)->gpu)

static BOOL told_format, told_neg;
static ULONG told_levels; /* textures whose levels were logged (tex_alloc) */

static void le16(UBYTE *d, ULONG v)
{
    d[0] = (UBYTE)v;
    d[1] = (UBYTE)(v >> 8);
}

/* n RAM-copy texels (RGBA8, RGBA floats or depth words) into the stored
 * format, as the GPU reads it. */
static void pack(const struct vc4_tex_format *f, int cls, const UBYTE *s, UBYTE *d, ULONG n)
{
    ULONG i;
    if (cls == CLASS_FLOAT) { /* the channel's plane: store_box packs each; negatives: pack_neg's */
        const GLfloat *v = (const GLfloat *)s;
        for (i = 0; i < n; i++, d += 2)
            le16(d, v[i * 4] < 0.0f ? 0 : ogles2_float_to_half(v[i * 4]));
        return;
    }
    if (cls == CLASS_DEPTH24 || cls == CLASS_DEPTH16) {
        const ULONG *w = (const ULONG *)s;
        for (i = 0; i < n; i++, d += 4) {
            ULONG v = w[i];
            if (cls == CLASS_DEPTH16) /* 16 bits to 24, the top bits repeated */
                v = ((v & 0xffff) << 8 | (v & 0xffff) >> 8) << 8;
            d[0] = (UBYTE)v;
            d[1] = (UBYTE)(v >> 8);
            d[2] = (UBYTE)(v >> 16);
            d[3] = (UBYTE)(v >> 24);
        }
        return;
    }
    switch (f->hw_type) {
    case VC4_TEX_RGB565:
        for (i = 0; i < n; i++, s += 4, d += 2)
            le16(d, (ULONG)(s[0] >> 3) << 11 | (ULONG)(s[1] >> 2) << 5 | (ULONG)(s[2] >> 3));
        break;
    case VC4_TEX_RGBA4444:
        for (i = 0; i < n; i++, s += 4, d += 2)
            le16(d, (ULONG)(s[0] >> 4) << 12 | (ULONG)(s[1] >> 4) << 8 | (ULONG)(s[2] >> 4) << 4 | (ULONG)(s[3] >> 4));
        break;
    case VC4_TEX_RGBA5551:
        for (i = 0; i < n; i++, s += 4, d += 2)
            le16(d, (ULONG)(s[0] >> 3) << 11 | (ULONG)(s[1] >> 3) << 6 | (ULONG)(s[2] >> 3) << 1 | (ULONG)(s[3] >> 7));
        break;
    case VC4_TEX_ALPHA: /* LUMINANCE (red) or ALPHA */
        for (i = 0; i < n; i++, s += 4)
            *d++ = f->key_format == VC4K_A8_UNORM ? s[3] : s[0];
        break;
    case VC4_TEX_LUMALPHA:
        for (i = 0; i < n; i++, s += 4, d += 2) {
            d[0] = s[0];
            d[1] = s[3];
        }
        break;
    default: /* RGBA8888, RGBX8888: the bytes as they are */
        memcpy(d, s, n * 4);
        break;
    }
}

/* n RGBA floats' channel into its negative plane: max(-v, 0). */
static void pack_neg(const UBYTE *s, UBYTE *d, ULONG n)
{
    const GLfloat *v = (const GLfloat *)s;
    ULONG i;
    for (i = 0; i < n; i++, d += 2)
        le16(d, v[i * 4] < 0.0f ? ogles2_float_to_half(-v[i * 4]) : 0);
}

/* One stored texel at p back into the RAM copy's form: RGBA8 (8 bits from
 * fewer: the top bits repeated, as the TMU expands them), or a depth word. */
static void unpack1(const struct vc4_tex_format *f, int cls, const UBYTE *p, UBYTE *d)
{
    ULONG v = (ULONG)p[0] | (ULONG)p[1] << 8;
    if (cls == CLASS_DEPTH24 || cls == CLASS_DEPTH16) {
        ULONG w = v | (ULONG)p[2] << 16 | (ULONG)p[3] << 24;
        *(ULONG *)d = cls == CLASS_DEPTH16 ? w >> 16 : w;
        return;
    }
    switch (f->hw_type) {
    case VC4_TEX_RGB565:
        d[0] = (UBYTE)((v >> 8 & 0xf8) | v >> 13);
        d[1] = (UBYTE)((v >> 3 & 0xfc) | (v >> 9 & 3));
        d[2] = (UBYTE)((v << 3 & 0xf8) | (v >> 2 & 7));
        d[3] = 255;
        break;
    case VC4_TEX_RGBA4444:
        d[0] = (UBYTE)((v >> 12) * 17);
        d[1] = (UBYTE)((v >> 8 & 15) * 17);
        d[2] = (UBYTE)((v >> 4 & 15) * 17);
        d[3] = (UBYTE)((v & 15) * 17);
        break;
    case VC4_TEX_RGBA5551:
        d[0] = (UBYTE)((v >> 8 & 0xf8) | v >> 13);
        d[1] = (UBYTE)((v >> 3 & 0xf8) | (v >> 8 & 7));
        d[2] = (UBYTE)((v << 2 & 0xf8) | (v >> 3 & 7));
        d[3] = (UBYTE)(v & 1 ? 255 : 0);
        break;
    case VC4_TEX_ALPHA:
        if (f->key_format == VC4K_A8_UNORM) {
            d[0] = d[1] = d[2] = 0;
            d[3] = p[0];
        } else {
            d[0] = d[1] = d[2] = p[0];
            d[3] = 255;
        }
        break;
    case VC4_TEX_LUMALPHA:
        d[0] = d[1] = d[2] = p[0];
        d[3] = p[1];
        break;
    default:
        d[0] = p[0];
        d[1] = p[1];
        d[2] = p[2];
        d[3] = f->hw_type == VC4_TEX_RGBX8888 ? 255 : p[3];
        break;
    }
}

/* Face k's miptree, from the copy's start. */
static UBYTE *face_base(const struct vc4_tex *g, int k)
{
    return (UBYTE *)g->mem.hostptr + (ULONG)k * g->lay.face_stride;
}

void ogles2_vc4_tex_free(struct OGLES2Texture *t)
{
    struct vc4_tex *g = VT(t);
    if (!g)
        return;
    if (g->mem.size)
        ogles2_mem_free(&g->mem);
    if (g->rt.size)
        ogles2_mem_free(&g->rt);
    if (g->neg.size)
        ogles2_mem_free(&g->neg);
    FreeVec(g);
    t->gpu = 0;
}

/* The negative values' planes, made at the first one (cleared: the texels so
 * far have none). Without memory they read as their absolute values. */
static BOOL neg_planes(struct vc4_tex *g)
{
    if (g->neg.size)
        return TRUE;
    if (!g->plane_stride || ogles2_mem_alloc(&g->neg, 4 * g->plane_stride) < 0) {
        if (!told_neg) {
            told_neg = TRUE;
            ogles2_log("texture: no memory for its negative values' planes: they read as their absolute values "
                       "(logged once)");
        }
        return FALSE;
    }
    return TRUE;
}

/* Face k's miptree in the negative planes. */
static UBYTE *neg_base(const struct vc4_tex *g, int k)
{
    return (UBYTE *)g->neg.hostptr + (ULONG)k * g->lay.face_stride;
}

/* ---- a half-float texture's 64-bit image (the top) ---- */

/* A texel of the image holds R, G, B, A as little-endian halves in that order
 * (vc4_draw hdr), a plane's texel one of them: the bytes move as they are, but
 * for the sign (the negative planes hold a negative's magnitude). */

/* The image's level from the planes: before a job that may load it. */
static void rt_compose(struct vc4_tex *g, int k, int l)
{
    const struct vc4_level *pl = &g->lay.level[l], *rl = &g->rt_lay.level[l];
    UBYTE *rb = (UBYTE *)g->rt.hostptr + (ULONG)k * g->rt_lay.face_stride + rl->offset;
    const UBYTE *pb = face_base(g, k) + pl->offset;
    ULONG x, y, p;
    for (y = 0; y < pl->height; y++)
        for (x = 0; x < pl->width; x++) {
            ULONG po = vc4_texel_offset(pl, 2, x, y);
            UBYTE *d = rb + vc4_texel_offset(rl, 8, x, y);
            for (p = 0; p < 4; p++) {
                const UBYTE *s = pb + p * g->plane_stride + po;
                d[2 * p] = s[0];
                d[2 * p + 1] = s[1];
                if (g->neg.size) {
                    const UBYTE *n = neg_base(g, k) + pl->offset + p * g->plane_stride + po;
                    if (n[0] | n[1]) {
                        d[2 * p] = n[0];
                        d[2 * p + 1] = (UBYTE)(n[1] | 0x80);
                    }
                }
            }
        }
}

/* The planes' level from the image: after a job drew it (finished: the
 * callers), before they are sampled or partly stored into. */
static void rt_split(struct vc4_tex *g, int k, int l)
{
    const struct vc4_level *pl = &g->lay.level[l], *rl = &g->rt_lay.level[l];
    const UBYTE *rb = (const UBYTE *)g->rt.hostptr + (ULONG)k * g->rt_lay.face_stride + rl->offset;
    UBYTE *pb = face_base(g, k) + pl->offset;
    ULONG x, y, p;
    vc4_os_invalidate(rb, rl->stride * rl->padded_height);
    for (y = 0; y < pl->height; y++)
        for (x = 0; x < pl->width; x++) {
            ULONG po = vc4_texel_offset(pl, 2, x, y);
            const UBYTE *s = rb + vc4_texel_offset(rl, 8, x, y);
            for (p = 0; p < 4; p++) {
                UBYTE *d = pb + p * g->plane_stride + po;
                UBYTE lo = s[2 * p], hi = s[2 * p + 1];
                BOOL minus = (hi & 0x80) && ((hi & 0x7f) | lo); /* -0 is 0 */
                d[0] = minus ? 0 : lo;
                d[1] = minus ? 0 : hi;
                if (minus)
                    neg_planes(g);
                if (g->neg.size) {
                    UBYTE *n = neg_base(g, k) + pl->offset + p * g->plane_stride + po;
                    n[0] = minus ? lo : 0;
                    n[1] = minus ? (UBYTE)(hi & 0x7f) : 0;
                }
            }
        }
    g->planes_stale[k] &= ~(1UL << l);
}

/* Every level the planes are behind on. */
static void rt_split_stale(struct vc4_tex *g)
{
    int k, l;
    for (k = 0; k < 6; k++)
        for (l = 0; g->planes_stale[k] && l < (int)g->lay.levels; l++)
            if (g->planes_stale[k] & (1UL << l))
                rt_split(g, k, l);
    g->any_stale = FALSE;
}

static void store_box(struct vc4_tex *g, int cls, int k, int l, const UBYTE *src, int x, int y, int w, int h);

/* An S3TC box's blocks (rows of ceil(w/4) blocks) decoded into w x h RGBA8
 * texels; NULL out of memory. */
static UBYTE *decode_blocks(GLenum format, const UBYTE *src, int w, int h)
{
    int type = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT    ? VC4_S3TC_DXT1_RGB
             : format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT   ? VC4_S3TC_DXT1_RGBA
             : format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT   ? VC4_S3TC_DXT3 : VC4_S3TC_DXT5;
    ULONG bb = dxt_block_bytes(format), bw = blocks4((ULONG)w), bx, by;
    UBYTE *rgba = AllocVec((ULONG)w * (ULONG)h * 4 + 4, MEMF_ANY), texels[16][4];
    int i, j;
    if (!rgba)
        return 0;
    for (by = 0; by < blocks4((ULONG)h); by++)
        for (bx = 0; bx < bw; bx++) {
            vc4_s3tc_block(src + (by * bw + bx) * bb, type, texels);
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    ULONG px = bx * 4 + (ULONG)i, py = by * 4 + (ULONG)j;
                    if (px < (ULONG)w && py < (ULONG)h)
                        memcpy(rgba + (py * (ULONG)w + px) * 4, texels[j * 4 + i], 4);
                }
        }
    return rgba;
}

/* A box of level l of face k from the RAM copy src (w texels a row; S3TC:
 * the box on block bounds, src its blocks). */
void ogles2_vc4_tex_store(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w, int h)
{
    struct vc4_tex *g = VT(t);
    if (!g || l >= (int)g->lay.levels || w <= 0 || h <= 0)
        return;
    /* Into a level a job drew: the planes whole first (the box is part of it;
     * the job has rendered: texture.c's before_change), and the planes current
     * from here on. */
    if (g->rt.size) {
        if (g->planes_stale[k] & (1UL << l))
            rt_split(g, k, l);
        g->rt_fresh[k] &= ~(1UL << l);
    }
    if (g->cls == CLASS_BLOCKS) {
        UBYTE *rgba = decode_blocks(t->f[k].format[l], src, w, h);
        if (rgba) {
            store_box(g, CLASS_RGBA8, k, l, rgba, x, y, w, h);
            FreeVec(rgba);
        }
        return;
    }
    store_box(g, g->cls, k, l, src, x, y, w, h);
}

/* A packed box into the miptree at base (a face's, or a plane's). */
static void write_box(struct vc4_tex *g, UBYTE *base, int l, const UBYTE *packed, ULONG row, int x, int y, int w, int h)
{
    const struct vc4_level *lv = &g->lay.level[l];
    ULONG cpp = g->lay.cpp;
    int i, j;
    if (x == 0 && y == 0 && (ULONG)w == lv->width && (ULONG)h == lv->height) {
        vc4_texture_store(base, &g->lay, (uint32_t)l, 0, packed, row);
    } else {
        UBYTE *lb = base + lv->offset;
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                UBYTE *d = lb + vc4_texel_offset(lv, cpp, (ULONG)(x + i), (ULONG)(y + j));
                const UBYTE *s = packed + (ULONG)j * row + (ULONG)i * cpp;
                ULONG b;
                for (b = 0; b < cpp; b++)
                    d[b] = s[b];
            }
    }
}

static void store_box(struct vc4_tex *g, int cls, int k, int l, const UBYTE *src, int x, int y, int w, int h)
{
    ULONG row = (ULONG)w * g->lay.cpp, plane;
    UBYTE *packed;
    int j;
    if (!(packed = AllocVec(row * (ULONG)h + 4, MEMF_ANY)))
        return;
    if (cls == CLASS_FLOAT && !g->neg.size) {
        const GLfloat *v = (const GLfloat *)src;
        ULONG n = (ULONG)w * (ULONG)h * 4, i;
        for (i = 0; i < n; i++)
            if (v[i] < 0.0f) {
                neg_planes(g);
                break;
            }
    }
    for (plane = 0; plane < (g->plane_stride ? 4UL : 1UL); plane++) {
        /* FLOAT: each channel into its plane (pack takes the floats from it) */
        const UBYTE *s0 = src + (g->plane_stride ? plane * sizeof(GLfloat) : 0);
        for (j = 0; j < h; j++)
            pack(g->f, cls, s0 + (ULONG)j * (ULONG)w * copy_bytes(cls), packed + (ULONG)j * row, (ULONG)w);
        write_box(g, face_base(g, k) + plane * g->plane_stride, l, packed, row, x, y, w, h);
        if (cls == CLASS_FLOAT && g->neg.size) {
            for (j = 0; j < h; j++)
                pack_neg(s0 + (ULONG)j * (ULONG)w * copy_bytes(cls), packed + (ULONG)j * row, (ULONG)w);
            write_box(g, neg_base(g, k) + plane * g->plane_stride, l, packed, row, x, y, w, h);
        }
    }
    FreeVec(packed);
}

/* A box of face k, level l from the GPU's copy into RGBA8 rows (what a job
 * drew), the CPU's cache over it dropped first. */
BOOL ogles2_vc4_tex_load(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *dst,
                         ULONG stride)
{
    struct vc4_tex *g = VT(t);
    const struct vc4_level *lv;
    UBYTE *lb;
    int i, j;
    if (!g || l >= (int)g->lay.levels || w <= 0 || h <= 0)
        return FALSE;
    if (g->cls == CLASS_FLOAT) {
        /* What a job drew, as RGBA floats (texture.c: read_half, sync); a level
         * no job drew: the RAM copy has it. */
        if (!g->rt.size || !(g->rt_fresh[k] & (1UL << l)))
            return FALSE;
        lv = &g->rt_lay.level[l];
        lb = (UBYTE *)g->rt.hostptr + (ULONG)k * g->rt_lay.face_stride + lv->offset;
        vc4_os_invalidate(lb, lv->stride * lv->padded_height);
        for (j = 0; j < h; j++) {
            GLfloat *d = (GLfloat *)(dst + (ULONG)j * stride);
            for (i = 0; i < w; i++, d += 4) {
                const UBYTE *s = lb + vc4_texel_offset(lv, 8, (ULONG)(x + i), (ULONG)(y + j));
                int c;
                for (c = 0; c < 4; c++)
                    d[c] = half_to_float((UWORD)(s[2 * c] | s[2 * c + 1] << 8));
            }
        }
        return TRUE;
    }
    lv = &g->lay.level[l];
    lb = face_base(g, k) + lv->offset;
    vc4_os_invalidate(lb, lv->stride * lv->padded_height);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            unpack1(g->f, g->cls, lb + vc4_texel_offset(lv, g->lay.cpp, (ULONG)(x + i), (ULONG)(y + j)),
                    dst + (ULONG)j * stride + (ULONG)i * 4);
    return TRUE;
}

BOOL ogles2_vc4_tex_upload(struct OGLES2Texture *t)
{
    const struct OGLES2Face *f0 = &t->f[0];
    const struct vc4_tex_format *f;
    struct vc4_tex *g;
    int l, k, n = 1;
    ogles2_vc4_tex_free(t);
    if (!base_ok(t))
        return TRUE; /* nothing to sample yet (a cube map: until all faces agree) */
    if (!(f = ogles2_vc4_storage(t))) {
        if (!told_format) {
            told_format = TRUE;
            ogles2_log("texture %ld: format 0x%04lx, type 0x%04lx: not stored on VC4 yet (it reads black; "
                       "logged once)", (ULONG)t->name, (ULONG)f0->format[0], (ULONG)f0->type[0]);
        }
        return FALSE;
    }
    for (l = 1; l < MAX_LEVELS && l < VC4_MAX_LEVELS; l++)
        if (consistent(f0, l))
            n = l + 1;
    if (!(g = AllocVec(sizeof(*g), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    g->f = f;
    g->cls = level_class(f0, 0);
    vc4_texture_layout(&g->lay, f0->w[0], f0->h[0], f->cpp, (uint32_t)n, faces(t));
    if (f->key_format == VC4K_R16G16B16A16_FLOAT) /* four S16F planes, P0's 4 KB apart */
        g->plane_stride = (g->lay.size + 4095) & ~4095UL;
    if (ogles2_mem_alloc(&g->mem, g->plane_stride ? 4 * g->plane_stride : g->lay.size) < 0) {
        FreeVec(g);
        return FALSE;
    }
    if (n > 1 && told_levels < 16) { /* P5: the mip levels' layout, the first few textures */
        told_levels++;
        ogles2_log("texture %ld: %ldx%ld, %ld levels stored at +0x%lx +0x%lx +0x%lx ... (%s), bus 0x%08lx",
                   (ULONG)t->name, (ULONG)f0->w[0], (ULONG)f0->h[0], (ULONG)n, g->lay.level[0].offset,
                   g->lay.level[1].offset, n > 2 ? g->lay.level[2].offset : 0UL,
                   (ULONG)(g->lay.level[0].tiling == VC4_TILING_LT ? "LT" : "T"), g->mem.busaddr);
    }
    t->gpu = (V3DTexture *)g;
    t->hw_type = f->hw_type;
    t->cpp = f->cpp;
    memcpy(t->swizzle, f->swizzle, 4);
    t->face_stride = g->lay.face_stride;
    /* A blank level is in the new copy already: the memory starts cleared. */
    for (k = 0; k < faces(t); k++)
        for (l = 0; l < n; l++)
            if ((l == 0 || consistent(&t->f[k], l)) && !(t->f[k].blank & (1UL << l)) && t->f[k].shadow[l])
                ogles2_vc4_tex_store(t, k, l, t->f[k].shadow[l], 0, 0, t->f[k].w[l], t->f[k].h[l]);
    return TRUE;
}

int ogles2_vc4_tex_levels(const struct OGLES2Texture *t)
{
    return VT(t) ? (int)VT(t)->lay.levels : 0;
}

BOOL ogles2_vc4_tex_holds(const struct OGLES2Texture *t, int k, int l)
{
    const struct vc4_tex *g = VT(t);
    if (!g || (l == 0 && g->lay.width != t->f[k].w[0]) || l >= (int)g->lay.levels)
        return FALSE;
    return g->lay.width == t->f[0].w[0] && g->lay.height == t->f[0].h[0];
}

/* The GPU copy goes with the job that still reads it; a new one at the
 * texture's next use. */
BOOL ogles2_vc4_tex_rename(struct OGLES2Context *user, struct OGLES2Texture *t)
{
    struct vc4_tex *g = VT(t);
    if (!g || !user->v3d)
        return FALSE;
    ogles2_vc4_mem_free_after(user, &g->mem);
    if (g->rt.size)
        ogles2_vc4_mem_free_after(user, &g->rt);
    if (g->neg.size)
        ogles2_vc4_mem_free_after(user, &g->neg);
    FreeVec(g);
    t->gpu = 0;
    return TRUE;
}

/* A level as a job's colour image: RGBA8 and RGB8 (RGBA8888), RGB565; a
 * half-float one (ub 1: the tile buffer's 64-bit mode) in its 64-bit image,
 * made at the first such use. Asked for by a job about to draw it, so its
 * planes are behind from here on (at worst a split of what they hold). */
BOOL ogles2_vc4_tex_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s)
{
    struct vc4_tex *g;
    const struct vc4_level *lv;
    (void)ctx;
    if (!ogles2_texture_renderable(t, k, l))
        return FALSE;
    if ((!VT(t) || l >= (int)VT(t)->lay.levels) && !ogles2_texture_upload(t))
        return FALSE;
    if (!(g = VT(t)) || l >= (int)g->lay.levels)
        return FALSE;
    if (g->cls == CLASS_FLOAT) {
        if (!g->rt.size) {
            vc4_texture_layout(&g->rt_lay, g->lay.width, g->lay.height, 8, g->lay.levels, faces(t));
            if (ogles2_mem_alloc(&g->rt, g->rt_lay.size) < 0)
                return FALSE;
        }
        if (!(g->rt_fresh[k] & (1UL << l))) {
            rt_compose(g, k, l);
            g->rt_fresh[k] |= 1UL << l;
        }
        g->planes_stale[k] |= 1UL << l;
        g->any_stale = TRUE;
        lv = &g->rt_lay.level[l];
        s->addr = (ULONG)g->rt.hostptr + (ULONG)k * g->rt_lay.face_stride + lv->offset;
        s->bytes = lv->stride * lv->padded_height;
        s->mem_format = lv->tiling;
        s->format = VC4_COLOR_RGBA8888; /* the stores ignore it in the 64-bit mode (§2.1) */
        s->force_alpha1 = FALSE;
        s->ub = 1;
        return TRUE;
    }
    if (g->f->hw_type != VC4_TEX_RGBA8888 && g->f->hw_type != VC4_TEX_RGBX8888 && g->f->hw_type != VC4_TEX_RGB565)
        return FALSE; /* the tile buffer stores RGBA8888 and BGR565 only */
    lv = &g->lay.level[l];
    s->addr = (ULONG)face_base(g, k) + lv->offset;
    s->bytes = lv->stride * lv->padded_height;
    s->mem_format = lv->tiling;
    s->format = g->f->hw_type == VC4_TEX_RGB565 ? VC4_COLOR_BGR565 : VC4_COLOR_RGBA8888;
    s->force_alpha1 = FALSE;
    s->ub = 0;
    return TRUE;
}

/* glGenerateMipmap on the GPU (texture.c, generate_tfu; copy.c draws the levels):
 * the stores VC4's tile buffer has, a chain already laid out, and a power-of-two
 * level 0. The TMU takes every level above 0 at its power-of-two size (tiling.c),
 * so a level of 100x60 lies in 128x64 of memory, which a render of GL's size does
 * not fill (m6_tfu's 200x120, level 3 wrong by 192): those on the CPU. */
BOOL ogles2_vc4_tex_can_generate(const struct OGLES2Texture *t, int n)
{
    const struct vc4_tex *g = VT(t);
    return g && g->cls != CLASS_FLOAT && (int)g->lay.levels >= n && !(g->lay.width & (g->lay.width - 1)) &&
           !(g->lay.height & (g->lay.height - 1)) &&
           (g->f->hw_type == VC4_TEX_RGBA8888 || g->f->hw_type == VC4_TEX_RGBX8888 || g->f->hw_type == VC4_TEX_RGB565);
}

int ogles2_vc4_tex_generate(struct OGLES2Context *ctx, struct OGLES2Texture *t, int n)
{
    return ogles2_copy_levels(ctx, t, n) ? 0 : -1;
}

/* glCopyTex* from the display (copy.c, texture.c's scratch texture): level 0 of t
 * filled with the whole picture at src, a raster image of w pixels a row, by a job
 * with no draws that loads the raster and stores the level (T or LT); no TFU here
 * (the Pi 4's way), and the TMU samples raster images right only at power-of-two
 * widths (§2 "Textures"). A BGRA picture stays as it is and is sampled as BGRA.
 * The caller has rendered and finished this context's jobs. 0; -3: not this way
 * (the CPU copies). */
int ogles2_vc4_tex_from_raster(struct OGLES2Context *ctx, struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w,
                               UWORD h, BOOL bgra)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct vc4_tex *g = VT(t);
    struct vc4_target tg;
    const struct vc4_level *lv;
    const struct vc4_tex_format *f;
    ULONG len = stride * (ULONG)h, bus;
    int r = 0;
    if (!g || g->cls == CLASS_FLOAT || stride != (ULONG)w * 4 || (stride & 15) || c->bad)
        return -3;
    if ((UWORD)((w + 63) / 64) > c->pools.tiles_x || (UWORD)((h + 63) / 64) > c->pools.tiles_y)
        return -3;
    if (!(f = vc4_tex_format(bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE)))
        return -3;
    if (!c->blit_ready) {
        if (vc4_job_init(&c->blit) < 0)
            return -3;
        c->blit_ready = TRUE;
    }
    if (!(src & 15) && vc4_os_gpu_reachable(src, len)) {
        bus = ogles2_vc4_bus(src);
    } else {
        /* as job_vc4.c's draws into the bitmap: the context's buffer, its layout */
        if (c->shadow.size < len) {
            if (c->shadow.size)
                vc4_mem_free(&c->shadow);
            if (vc4_mem_alloc(&c->shadow, len) < 0) {
                c->shadow.size = 0;
                return -3;
            }
        }
        CopyMem((APTR)src, c->shadow.cpu, len);
        bus = c->shadow.bus;
    }
    lv = &g->lay.level[0];
    memset(&tg, 0, sizeof(tg));
    tg.width = w;
    tg.height = h;
    tg.color.bus = ogles2_vc4_bus((ULONG)face_base(g, 0) + lv->offset);
    tg.color.tiling = lv->tiling;
    tg.color.format = VC4_COLOR_RGBA8888;
    tg.color_load_bus = bus;
    tg.color_load_raster = 1;
    tg.load = VC4_COLOR;
    tg.store = VC4_COLOR;
    if (vc4_job_begin(&c->blit, &tg, &c->pools) < 0 || vc4_job_end_lists(&c->blit) < 0)
        return -3;
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the picture's copy; no dirty CPU line over the level */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), 5000))
        return -3;
    ogles2_job_idle_gpu(ctx);
    if (vc4_job_bin(&c->blit, 1000000) < 0) {
        ogles2_device_reset("a copy's job it did not finish");
        r = -3;
    } else {
        vc4_job_render_start(&c->blit);
        if (vc4_job_render_wait(&c->blit, 1000000) < 0) {
            ogles2_device_reset("a copy's job it did not finish");
            r = -3;
        }
    }
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (!r)
        g->f = f; /* the keys and sampler take it from here; the same stored type either way */
    return r;
}

/* A depth texture's level as a job's depth (and stencil) image, its GPU
 * copy made first: the tile buffer stores Z24S8 into the level's own
 * tiling, which the shader then samples (Mesa's depth textures). */
BOOL ogles2_vc4_tex_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l, struct OGLES2Surface *s)
{
    const struct vc4_tex *g;
    const struct vc4_level *lv;
    UBYTE bits;
    BOOL st;
    (void)ctx;
    if (!ogles2_texture_depth(t, l, &bits, &st) || !consistent(&t->f[0], l))
        return FALSE;
    if ((!VT(t) || l >= (int)VT(t)->lay.levels) && !ogles2_texture_upload(t))
        return FALSE;
    if (!(g = VT(t)) || l >= (int)g->lay.levels || g->lay.cpp != 4)
        return FALSE;
    lv = &g->lay.level[l];
    s->addr = (ULONG)face_base(g, 0) + lv->offset;
    s->bytes = lv->stride * lv->padded_height;
    s->mem_format = lv->tiling;
    s->format = 0;
    s->force_alpha1 = FALSE;
    s->ub = 0;
    return TRUE;
}

static ULONG min_filter(GLenum f)
{
    switch (f) {
    case GL_NEAREST: return VC4_MIN_NEAREST;
    case GL_NEAREST_MIPMAP_NEAREST: return VC4_MIN_NEAR_MIP_NEAR;
    case GL_NEAREST_MIPMAP_LINEAR: return VC4_MIN_NEAR_MIP_LIN;
    case GL_LINEAR_MIPMAP_NEAREST: return VC4_MIN_LIN_MIP_NEAR;
    case GL_LINEAR_MIPMAP_LINEAR: return VC4_MIN_LIN_MIP_LIN;
    default: return VC4_MIN_LINEAR;
    }
}

/* MIRROR_CLAMP_TO_EDGE: the shader takes the absolute value, the TMU clamps
 * (patch 0019). */
static ULONG wrap(GLenum w)
{
    return w == GL_CLAMP_TO_EDGE || w == GL_MIRROR_CLAMP_TO_EDGE ? VC4_WRAP_CLAMP
         : w == GL_MIRRORED_REPEAT                               ? VC4_WRAP_MIRROR
                                                                 : VC4_WRAP_REPEAT;
}

/* A sampler's TMU words and key entry, and the values of patch 0019's
 * uniforms (emu: the LOD bias, the LOD range in level 0's terms, level 0's
 * size). The TMU samples the miptree from level 0 (P0) with no level clamp
 * (Mesa: "There is no hardware level clamping"): a base level above 0
 * (SGIS_texture_lod, a texture complete from there) is sampled by an LOD the
 * shader computes and clamps to [base + min LOD, min(base + max LOD, last)],
 * or by an explicit LOD when it is the one level (Mesa's force_first_level);
 * magnifying such a base level filters with the minification filter (the
 * TMU's LOD is above 0 there: a documented deviation, §2.1). */
BOOL ogles2_vc4_sampler(struct OGLES2Context *ctx, int unit, int kind, ULONG p[5], struct vc4_key_sampler *ks,
                        ULONG *first_out, GLfloat emu[5])
{
    struct OGLES2TexState *ts = texture_ts_fast(ctx);
    struct OGLES2Texture *t;
    struct vc4_tex *g;
    int first = 0, last = 0;
    GLfloat lo, hi, maxb = ogles2_backend->caps->max_lod_bias;
    BOOL cube, depth;
    if (!ts)
        return FALSE;
    t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit] : ts->unit2d[unit];
    if (!t->gpu && t != &ts->fallback)
        ogles2_texture_upload(t);
    if (!ogles2_texture_complete(t, &first, &last)) {
        if (!t->warned_incomplete && t != &ts->fallback) {
            t->warned_incomplete = TRUE;
            ogles2_log("texture %ld sampled while incomplete or not stored: it reads black (logged once a texture)",
                       (ULONG)t->name);
        }
        if (!(t = ogles2_texture_fallback(ts)))
            return FALSE;
        first = last = 0;
    }
    if (!(g = VT(t)))
        return FALSE;
    if (g->any_stale) {
        /* a job drew it: finished (it may be running on), and its picture
         * into the planes the TMU reads */
        ogles2_job_finish(ctx);
        rt_split_stale(g);
    }
    t->used_serial = ctx->job_serial;
    cube = faces(t) > 1;
    p[0] = vc4_tex_p0(g->mem.busaddr + g->lay.level[0].offset, g->f->hw_type,
                      mipmapped(t->min_filter) || first ? (ULONG)last : 0, cube);
    /* a depth word is sampled whole: nearest (Mesa filters its bytes) */
    depth = g->cls == CLASS_DEPTH16 || g->cls == CLASS_DEPTH24;
    p[1] = vc4_tex_p1(g->f->hw_type, g->lay.width, g->lay.height,
                      t->mag_filter == GL_NEAREST || depth ? VC4_MAG_NEAREST : VC4_MAG_LINEAR,
                      depth ? (mipmapped(t->min_filter) || first ? VC4_MIN_NEAR_MIP_NEAR : VC4_MIN_NEAREST)
                      /* a base level above 0 unmipmapped: the explicit LOD
                       * (force_first) counts only with a mip filter (P5 run 1:
                       * NEAREST read level 0) */
                      : first && !mipmapped(t->min_filter)
                            ? (t->min_filter == GL_NEAREST ? VC4_MIN_NEAR_MIP_NEAR : VC4_MIN_LIN_MIP_NEAR)
                            : min_filter(t->min_filter),
                      wrap(t->wrap_s), wrap(t->wrap_t));
    /* P2 (Mesa's vc4_uniforms.c): PTYPE 1 (the cube map stride) for every
     * texture, as Mesa writes it: the TMU takes BSLOD (an explicit LOD, the
     * recipe's: draw_vc4.c) only with PTYPE 1 (§2 "Textures", found
     * 2026-11-05: with PTYPE 0 it added its own LOD to the explicit one) */
    p[2] = 1UL << 30 | (cube ? g->lay.face_stride & 0x3ffff000UL : 0);
    p[3] = g->plane_stride; /* P0 of plane k: p[0] + k p[3] (draw_vc4.c) */
    /* and of negative plane k: p[4] + k p[3] (level 0's offset and the low
     * bits as p[0]'s, both blocks on 4 KB) */
    p[4] = g->neg.size ? p[0] - g->mem.busaddr + g->neg.busaddr : 0;
    memset(ks, 0, sizeof(*ks));
    ks->half_signed = g->neg.size ? 1 : 0;
    ks->f = g->f;
    ks->wrap_s = t->wrap_s;
    ks->wrap_t = t->wrap_t;
    ogles2_vc4_sampler_emu(t, kind, first, last, ks);
    *first_out = (ULONG)first;
    emu[0] = t->lod_bias > maxb ? maxb : t->lod_bias < -maxb ? -maxb : t->lod_bias;
    lo = first ? (GLfloat)first + (t->min_lod > 0.0f ? t->min_lod : 0.0f) : t->min_lod;
    hi = (GLfloat)first + t->max_lod;
    if (hi > (GLfloat)last)
        hi = (GLfloat)last;
    emu[1] = lo;
    emu[2] = hi < lo ? lo : hi;
    emu[3] = (GLfloat)g->lay.width;
    emu[4] = (GLfloat)g->lay.height;
    return TRUE;
}
