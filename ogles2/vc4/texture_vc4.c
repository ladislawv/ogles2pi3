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
 * §2 "Textures"). A job does not draw into them yet.
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
};

#define VT(t) ((struct vc4_tex *)(t)->gpu)

static BOOL told_format;

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
    if (cls == CLASS_FLOAT) { /* the channel's plane: store_box packs each */
        const GLfloat *v = (const GLfloat *)s;
        for (i = 0; i < n; i++, d += 2)
            le16(d, ogles2_float_to_half(v[i * 4]));
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
    FreeVec(g);
    t->gpu = 0;
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

static void store_box(struct vc4_tex *g, int cls, int k, int l, const UBYTE *src, int x, int y, int w, int h)
{
    const struct vc4_level *lv;
    ULONG cpp, row, plane;
    UBYTE *packed, *base;
    int i, j;
    cpp = g->lay.cpp;
    row = (ULONG)w * cpp;
    if (!(packed = AllocVec(row * (ULONG)h + 4, MEMF_ANY)))
        return;
    for (plane = 0; plane < (g->plane_stride ? 4UL : 1UL); plane++) {
    /* FLOAT: each channel into its plane (pack takes the floats from it) */
    const UBYTE *s0 = src + (g->plane_stride ? plane * sizeof(GLfloat) : 0);
    for (j = 0; j < h; j++)
        pack(g->f, cls, s0 + (ULONG)j * (ULONG)w * copy_bytes(cls), packed + (ULONG)j * row, (ULONG)w);
    lv = &g->lay.level[l];
    base = face_base(g, k) + plane * g->plane_stride;
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
    if (!g || l >= (int)g->lay.levels || w <= 0 || h <= 0 || g->cls == CLASS_FLOAT)
        return FALSE;
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
    FreeVec(g);
    t->gpu = 0;
    return TRUE;
}

/* A level as a job's colour image: RGBA8 and RGB8 (RGBA8888), RGB565. */
BOOL ogles2_vc4_tex_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s)
{
    const struct vc4_tex *g;
    const struct vc4_level *lv;
    (void)ctx;
    if (!ogles2_texture_renderable(t, k, l))
        return FALSE;
    if ((!VT(t) || l >= (int)VT(t)->lay.levels) && !ogles2_texture_upload(t))
        return FALSE;
    if (!(g = VT(t)) || l >= (int)g->lay.levels)
        return FALSE;
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
BOOL ogles2_vc4_sampler(struct OGLES2Context *ctx, int unit, int kind, ULONG p[4], struct vc4_key_sampler *ks,
                        ULONG *first_out, GLfloat emu[5])
{
    struct OGLES2TexState *ts = texture_ts_fast(ctx);
    struct OGLES2Texture *t;
    const struct vc4_tex *g;
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
    t->used_serial = ctx->job_serial;
    cube = faces(t) > 1;
    p[0] = vc4_tex_p0(g->mem.busaddr + g->lay.level[0].offset, g->f->hw_type,
                      mipmapped(t->min_filter) || first ? (ULONG)last : 0, cube);
    /* a depth word is sampled whole: nearest (Mesa filters its bytes) */
    depth = g->cls == CLASS_DEPTH16 || g->cls == CLASS_DEPTH24;
    p[1] = vc4_tex_p1(g->f->hw_type, g->lay.width, g->lay.height,
                      t->mag_filter == GL_NEAREST || depth ? VC4_MAG_NEAREST : VC4_MAG_LINEAR,
                      depth ? (mipmapped(t->min_filter) ? VC4_MIN_NEAR_MIP_NEAR : VC4_MIN_NEAREST)
                            : min_filter(t->min_filter),
                      wrap(t->wrap_s), wrap(t->wrap_t));
    /* P2 (Mesa's vc4_uniforms.c): PTYPE 1 (the cube map stride) for every
     * texture, as Mesa writes it: the TMU takes BSLOD (an explicit LOD, the
     * recipe's: draw_vc4.c) only with PTYPE 1 (§2 "Textures", found
     * 2026-11-05: with PTYPE 0 it added its own LOD to the explicit one) */
    p[2] = 1UL << 30 | (cube ? g->lay.face_stride & 0x3ffff000UL : 0);
    p[3] = g->plane_stride; /* P0 of plane k: p[0] + k p[3] (draw_vc4.c) */
    memset(ks, 0, sizeof(*ks));
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
