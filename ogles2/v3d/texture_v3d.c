/*
 * The V3D 4.2 backend's texture storage (ogles2/backend.h, docs/PI3_PLAN.md
 * §5.1, P2 step 5): the stored formats and their packing, the GPU copy's
 * allocation, layout and tiling (v3dcore's v3d_texture.c: Mesa's level
 * layout, UIF/UBLINEAR/LINEARTILE), its loads and stores (the TFU where it
 * beats the CPU), renaming, glGenerateMipmap and glCopyTex* on the TFU, the
 * texture and sampler records of a draw, and a level as a render target.
 * Moved from ogles2/texture.c, which keeps the objects, their RAM copies,
 * completeness and the fallback texture.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "backend_v3d.h"
#include "texture_private.h"
#include "v3d_context.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_texture.h"
#include "v3d_tfu.h"
#include "v3d_gpu_lock.h"

#define MAX_LOD_BIAS (ogles2_backend->caps->max_lod_bias)

/* Both records fit their halves of rec. */
typedef char ogles2_rec_fits[sizeof(v3d_texture_shader_state) <= 32 && sizeof(v3d_sampler_state) <= 32 ? 1 : -1];

/* No job uses the GPU copy any more (before_change renamed it or rendered
 * the job): the copy and the records' block. */
void ogles2_v3d_tex_free(struct OGLES2Texture *t)
{
    if (t->gpu) {
        if (t->gpu->texture_mem.hostptr)
            v3d_texture_free(ogles2_device(), t->gpu);
        FreeVec(t->gpu);
        t->gpu = 0;
    }
    if (t->rec.hostptr)
        ogles2_mem_free(&t->rec);
    t->st_own = FALSE;
}

/* A face's view of the GPU texture: face 0 is the texture itself; face k
 * the same layout `k` face strides further on (the upload functions only
 * write through texture_mem). */
static V3DTexture *face_gpu(struct OGLES2Texture *t, int k, V3DTexture *tmp)
{
    if (!k)
        return t->gpu;
    *tmp = *t->gpu;
    tmp->texture_mem.hostptr =
        (void *)(V3D_ALIGN_UP((v3d_uintptr)t->gpu->texture_mem.hostptr, 256) + (ULONG)k * t->face_stride);
    tmp->texture_mem.size = t->face_stride;
    return tmp;
}

/* The hardware format for level 0's format and type (Mesa's
 * v3dx_format_table.c, the formats GLES2's state tracker picks). */
static void storage_of(struct OGLES2Texture *t)
{
    GLenum f = t->f[0].format[0], ty = t->f[0].type[0];
    UBYTE R = v3d_SWIZZLE_RED, G = v3d_SWIZZLE_GREEN, B = v3d_SWIZZLE_BLUE, A = v3d_SWIZZLE_ALPHA;
    UBYTE O = v3d_SWIZZLE_ONE, Z = v3d_SWIZZLE_ZERO;
    UBYTE type = V3D_TEXTURE_DATA_FORMAT_RGBA8, cpp = 4, sw[4];
    sw[0] = R; sw[1] = G; sw[2] = B; sw[3] = A;
    if (f == GL_RGB)
        sw[3] = O; /* rendered into, the stored alpha is the shader's */
    if (dxt_block_bytes(f)) {
        /* S3TC: BC1-BC3, a block a "texel" (Mesa's table: DXT1 RGB is XYZ1) */
        type = f == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT   ? V3D_TEXTURE_DATA_FORMAT_BC2
             : f == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT   ? V3D_TEXTURE_DATA_FORMAT_BC3
                                                       : V3D_TEXTURE_DATA_FORMAT_BC1;
        cpp = (UBYTE)dxt_block_bytes(f);
        if (f == GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
            sw[3] = O;
    } else if (ty == GL_FLOAT || ty == GL_HALF_FLOAT_OES) {
        /* OES_texture_float (32-bit returns: the .v3dw variants) and
         * OES_texture_half_float (16-bit, filterable); RGB as RGBA. */
        BOOL h = ty == GL_HALF_FLOAT_OES;
        if (f == GL_LUMINANCE || f == GL_ALPHA) {
            type = h ? V3D_TEXTURE_DATA_FORMAT_R16F : V3D_TEXTURE_DATA_FORMAT_R32F;
            cpp = h ? 2 : 4;
            if (f == GL_LUMINANCE) {
                sw[0] = sw[1] = sw[2] = R; sw[3] = O;
            } else {
                sw[0] = sw[1] = sw[2] = Z; sw[3] = R;
            }
        } else if (f == GL_LUMINANCE_ALPHA) {
            type = h ? V3D_TEXTURE_DATA_FORMAT_RG16F : V3D_TEXTURE_DATA_FORMAT_RG32F;
            cpp = h ? 4 : 8;
            sw[0] = sw[1] = sw[2] = R; sw[3] = G;
        } else {
            type = h ? V3D_TEXTURE_DATA_FORMAT_RGBA16F : V3D_TEXTURE_DATA_FORMAT_RGBA32F;
            cpp = h ? 8 : 16;
        }
    } else if (f == GL_DEPTH_COMPONENT || f == GL_DEPTH_STENCIL_OES) {
        /* OES_depth_texture: sampled as luminance, (d, d, d, 1); 32-bit returns */
        type = ty == GL_UNSIGNED_SHORT ? V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16 : V3D_TEXTURE_DATA_FORMAT_DEPTH24X8;
        cpp = ty == GL_UNSIGNED_SHORT ? 2 : 4;
        sw[0] = sw[1] = sw[2] = R; sw[3] = O;
    } else if (f == GL_RGB && ty == GL_UNSIGNED_SHORT_5_6_5) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB565; cpp = 2; sw[3] = O;
    } else if (f == GL_RGBA && ty == GL_UNSIGNED_SHORT_4_4_4_4) {
        type = V3D_TEXTURE_DATA_FORMAT_RGBA4; cpp = 2;
    } else if (f == GL_RGBA && ty == GL_UNSIGNED_SHORT_5_5_5_1) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB5A1; cpp = 2;
    } else if (f == GL_LUMINANCE) {
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[0] = sw[1] = sw[2] = R; sw[3] = O;
    } else if (f == GL_ALPHA) {
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[0] = sw[1] = sw[2] = Z; sw[3] = R;
    } else if (f == GL_LUMINANCE_ALPHA) {
        type = V3D_TEXTURE_DATA_FORMAT_RG8; cpp = 2; sw[0] = sw[1] = sw[2] = R; sw[3] = G;
    }
    t->hw_type = type;
    t->cpp = cpp;
    memcpy(t->swizzle, sw, 4);
}

/* A 16-bit texel stored little-endian (the GPU's order) from the 68k. */
#define LE16_STORE(p, v) (*(UWORD *)(p) = (UWORD)(((v) << 8) | ((v) >> 8)))

/* n RGBA8 texels into the stored format, in the GPU's byte order (16-bit
 * texels little-endian, the GL packing: red in the top bits). Each format
 * in a loop of its own, a texel read as one long (R in the top byte): a
 * switch per texel cost 50 ms a megapixel on the A1200. The copy's rows and
 * `d` are word aligned (AllocVec, widths in whole texels). */
static void pack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    const ULONG *p = (const ULONG *)s;
    ULONG i, c;
    switch (t->hw_type) {
    case V3D_TEXTURE_DATA_FORMAT_RGB565:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF800) | ((c >> 13) & 0x07E0) | ((c >> 11) & 0x001F));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_RGBA4:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF800) | ((c >> 13) & 0x07C0) | ((c >> 10) & 0x003E) | ((c >> 7) & 1));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_R8: /* luminance (red of the copy) or alpha */
        if (t->swizzle[3] == v3d_SWIZZLE_RED)
            for (i = 0; i < n; i++)
                d[i] = (UBYTE)p[i];
        else
            for (i = 0; i < n; i++)
                d[i] = (UBYTE)(p[i] >> 24);
        return;
    case V3D_TEXTURE_DATA_FORMAT_RG8: /* luminance, alpha */
        for (i = 0; i < n; i++, d += 2) {
            c = p[i];
            d[0] = (UBYTE)(c >> 24);
            d[1] = (UBYTE)c;
        }
        return;
    default:
        memcpy(d, s, n * 4);
        return;
    }
}

static BOOL float_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R32F || hw == V3D_TEXTURE_DATA_FORMAT_RG32F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGBA32F || hw == V3D_TEXTURE_DATA_FORMAT_R16F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RG16F || hw == V3D_TEXTURE_DATA_FORMAT_RGBA16F;
}

static BOOL depth_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16 || hw == V3D_TEXTURE_DATA_FORMAT_DEPTH24X8;
}

/* Formats the TMU returns 32 bits a channel for (Mesa's v3dx_format_table.c):
 * a draw sampling one needs the program's .v3dp's 32-bit variants. */
static BOOL wide_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R32F || hw == V3D_TEXTURE_DATA_FORMAT_RG32F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGBA32F || depth_type(hw);
}

/* Formats the TMU cannot filter (Mesa's table): sampled nearest whatever the
 * filters say (OES_texture_float_linear is not offered; OES_depth_texture
 * leaves filtering to the implementation). */
static BOOL unfilterable_type(UBYTE hw)
{
    return wide_type(hw);
}

static void put_le32(UBYTE *d, ULONG v)
{
    d[0] = (UBYTE)v; d[1] = (UBYTE)(v >> 8); d[2] = (UBYTE)(v >> 16); d[3] = (UBYTE)(v >> 24);
}

/* n texels of a FLOAT or DEPTH RAM copy into the stored format, little-endian.
 * Luminance and alpha formats store red or alpha, as pack does. */
static void pack_wide(const struct OGLES2Texture *t, const UBYTE *src, UBYTE *d, ULONG n)
{
    ULONG i;
    BOOL alpha = t->swizzle[3] == v3d_SWIZZLE_RED; /* an ALPHA format: its one channel is alpha */
    if (depth_type(t->hw_type)) {
        const ULONG *s = (const ULONG *)src;
        for (i = 0; i < n; i++)
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16) {
                *d++ = (UBYTE)s[i];
                *d++ = (UBYTE)(s[i] >> 8);
            } else {
                put_le32(d, s[i]);
                d += 4;
            }
        return;
    }
    for (i = 0; i < n; i++, src += 16) {
        const GLfloat *s = (const GLfloat *)src;
        GLfloat v[4];
        int c, nc;
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_R32F:
        case V3D_TEXTURE_DATA_FORMAT_R16F:
            v[0] = alpha ? s[3] : s[0];
            nc = 1;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG32F:
        case V3D_TEXTURE_DATA_FORMAT_RG16F:
            v[0] = s[0];
            v[1] = s[3];
            nc = 2;
            break;
        default:
            v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; v[3] = s[3];
            nc = 4;
            break;
        }
        for (c = 0; c < nc; c++) {
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_R16F || t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG16F ||
                t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F) {
                UWORD hv = ogles2_float_to_half(v[c]);
                *d++ = (UBYTE)hv;
                *d++ = (UBYTE)(hv >> 8);
            } else {
                put_le32(d, f2u(v[c]));
                d += 4;
            }
        }
    }
}

/* The inverse of pack_wide for depth (what a framebuffer object's job drew). */
static void unpack_depth(const struct OGLES2Texture *t, const UBYTE *s, ULONG *d, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++)
        if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16) {
            d[i] = (ULONG)s[0] | ((ULONG)s[1] << 8);
            s += 2;
        } else {
            d[i] = (ULONG)s[0] | ((ULONG)s[1] << 8) | ((ULONG)s[2] << 16) | ((ULONG)s[3] << 24);
            s += 4;
        }
}

/* The inverse of pack: n texels of the stored format into RGBA8. */
static void unpack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    ULONG i;
    UWORD v;
    for (i = 0; i < n; i++, d += 4) {
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_RGB565:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 11) & 31) * 255 / 31);
            d[1] = (UBYTE)(((v >> 5) & 63) * 255 / 63);
            d[2] = (UBYTE)((v & 31) * 255 / 31);
            d[3] = 255;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGBA4:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 12) & 15) * 17);
            d[1] = (UBYTE)(((v >> 8) & 15) * 17);
            d[2] = (UBYTE)(((v >> 4) & 15) * 17);
            d[3] = (UBYTE)((v & 15) * 17);
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 11) & 31) * 255 / 31);
            d[1] = (UBYTE)(((v >> 6) & 31) * 255 / 31);
            d[2] = (UBYTE)(((v >> 1) & 31) * 255 / 31);
            d[3] = (v & 1) ? 255 : 0;
            break;
        case V3D_TEXTURE_DATA_FORMAT_R8:
            d[0] = d[1] = d[2] = t->swizzle[3] == v3d_SWIZZLE_RED ? 0 : s[0];
            d[3] = t->swizzle[3] == v3d_SWIZZLE_RED ? s[0] : 255;
            s++;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG8:
            d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; s += 2;
            break;
        default:
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            d[3] = t->swizzle[3] == v3d_SWIZZLE_ONE ? 255 : s[3];
            s += 4;
            break;
        }
    }
}

/* The inverse of pack_wide for RGBA16F (what a framebuffer object's job
 * drew into a half-float texture): n texels into RGBA floats. */
static void unpack_half(const UBYTE *s, GLfloat *d, ULONG n)
{
    ULONG i;
    for (i = 0; i < n * 4; i++, s += 2)
        d[i] = half_to_float((UWORD)(s[0] | (s[1] << 8)));
}

/* A box of face k, level l from the GPU's copy, as the RAM copy holds it:
 * RGBA8 rows, depth words (a depth texture a job drew into), or RGBA
 * floats (a half-float one). Other float textures are never drawn into. */
BOOL ogles2_v3d_tex_load(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba,
                     ULONG stride)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp;
    UBYTE *packed;
    int j;
    if (!t->gpu || l >= t->gpu->num_levels || w <= 0 || h <= 0 ||
        (float_type(t->hw_type) && t->hw_type != V3D_TEXTURE_DATA_FORMAT_RGBA16F))
        return FALSE;
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return FALSE;
    v3d_texture_load(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    for (j = 0; j < h; j++)
        if (depth_type(t->hw_type))
            unpack_depth(t, packed + (ULONG)j * row, (ULONG *)(rgba + (ULONG)j * stride), (ULONG)w);
        else if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F)
            unpack_half(packed + (ULONG)j * row, (GLfloat *)(rgba + (ULONG)j * stride), (ULONG)w);
        else
            unpack(t, packed + (ULONG)j * row, rgba + (ULONG)j * stride, (ULONG)w);
    FreeVec(packed);
    return TRUE;
}

/* A whole level stored by the TFU from a raster copy in the stored format
 * (`row` bytes a row), where it beats the CPU's tiling (v3d_texture_store),
 * the copy in memory the GPU reaches, a format the TFU takes. The job's
 * fixed cost is a cache clean, the lock and a few bridge writes, 0.3-0.5 ms
 * on the A1200, then about 1.9 ms a megapixel; the CPU tiles RGBA8 into UIF
 * directly at 5.6 ms a megapixel, other formats at about 40: so 512x512 and
 * up for RGBA8, 128x128 for the rest. The caller has dealt with the jobs
 * that use the texture (before_change), as for the CPU's store. Probe U
 * keeps the CPU's. FALSE: the CPU stores it. */
#define TFU_UPLOAD_MIN_TEXELS       (128UL * 128UL)
#define TFU_UPLOAD_MIN_TEXELS_RGBA8 (512UL * 512UL)
#define TFU_LOCK_TIMEOUT_MS 5000 /* job.c's GPU_LOCK_TIMEOUT_MS */
static BOOL store_tfu(struct OGLES2Texture *t, int k, int l, const UBYTE *raster, ULONG row, int w, int h)
{
    V3DTexture tmp, *g;
    ULONG len;
    int r;
    if (ogles2_tex_cpu_uploads ||
        (ULONG)w * (ULONG)h < (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA8 ? TFU_UPLOAD_MIN_TEXELS_RGBA8
                                                                          : TFU_UPLOAD_MIN_TEXELS) ||
        !v3d_tfu_supports_mipmaps(t->hw_type) || !ogles2_device_gpu_reachable((ULONG)raster, row * (ULONG)h))
        return FALSE;
    g = face_gpu(t, k, &tmp);
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the copy in memory, and no dirty line over the level */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return FALSE;
    r = v3d_tfu_store_raster(ogles2_device(), g, (v3d_u8)l, t->hw_type, (v3d_u16)w, (v3d_u16)h, raster, row);
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r == -2)
        ogles2_log("texture %ld: the TFU did not finish an upload (%ldx%ld): the CPU stores it",
                   (ULONG)t->name, (ULONG)w, (ULONG)h);
    if (r)
        return FALSE;
    /* The level's memory as the TFU left it: no CPU cache line from before
     * (a later glTexSubImage2D's partial store reads them). */
    len = g->texture_mem.size;
    CachePostDMA(g->texture_mem.hostptr, &len, 0);
    return TRUE;
}

/* A box of level l of face k, from the RAM copy `src` (`w` texels a row, of
 * level 0's class: complete() saw to it). S3TC: the box on block bounds,
 * `src` its blocks, stored as they are (the TFU takes no BC format). */
void ogles2_v3d_tex_store(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w,
                      int h)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp, in;
    UBYTE *packed;
    BOOL whole = x == 0 && y == 0 && w == t->f[k].w[l] && h == t->f[k].h[l];
    int j;
    if (class_of(t->f[0].format[0], t->f[0].type[0]) == CLASS_BLOCKS) {
        v3d_texture_store(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, src, blocks4((ULONG)w) * t->cpp,
                          x / 4, y / 4, (int)blocks4((ULONG)w), (int)blocks4((ULONG)h));
        return;
    }
    in = (ULONG)w * copy_bytes(class_of(t->f[0].format[0], t->f[0].type[0]));
    /* RGBA8 is stored as the RAM copy holds it: straight from there. */
    if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA8) {
        if (whole && store_tfu(t, k, l, src, (ULONG)w * 4, w, h))
            return;
        v3d_texture_store(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, src, (ULONG)w * 4, x, y, w, h);
        return;
    }
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return;
    for (j = 0; j < h; j++)
        if (float_type(t->hw_type) || depth_type(t->hw_type))
            pack_wide(t, src + (ULONG)j * in, packed + (ULONG)j * row, (ULONG)w);
        else
            pack(t, src + (ULONG)j * in, packed + (ULONG)j * row, (ULONG)w);
    if (!whole || !store_tfu(t, k, l, packed, row, w, h))
        v3d_texture_store(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    FreeVec(packed);
}

/* A cube map's memory: six of the 2D layout v3d_texture_alloc(_mipchain)
 * made, `face_stride` apart (Mesa: the tree's end aligned to 64 bytes; a
 * page here, so each face's level 0 keeps the alignment UIF XOR needs). */
static BOOL cube_memory(struct OGLES2Texture *t)
{
    V3DTexture *g = t->gpu;
    v3d_mem m;
    ULONG tree = g->levels[0].offset + (ULONG)g->levels[0].padded_h * g->levels[0].stride;
    t->face_stride = (tree + 4095) & ~4095UL;
    if (ogles2_mem_alloc(&m, t->face_stride * 6 + 256) < 0)
        return FALSE;
    ogles2_mem_free(&g->texture_mem);
    g->texture_mem = m;
    return TRUE;
}

/* (Re)creates the GPU texture for the levels given and uploads them (the
 * front end read back what the GPU drew first). */
BOOL ogles2_v3d_tex_upload(struct OGLES2Texture *t)
{
    V3DDevice *dev = ogles2_device();
    const struct OGLES2Face *f0 = &t->f[0];
    int l, k, n = 1;
    ogles2_v3d_tex_free(t);
    if (!base_ok(t))
        return TRUE; /* nothing to sample yet (a cube map: until all faces agree) */
    for (l = 1; l < MAX_LEVELS; l++)
        if (consistent(f0, l))
            n = l + 1;
    if (!(t->gpu = AllocVec(sizeof(*t->gpu), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    storage_of(t);
    if (v3d_texture_alloc_blocks(dev, t->gpu, f0->w[0], f0->h[0], t->cpp,
                                 level_class(f0, 0) == CLASS_BLOCKS ? 4 : 1) < 0 ||
        (n > 1 && v3d_texture_alloc_mipchain(dev, t->gpu, (v3d_u8)n) < 0)) {
        ogles2_v3d_tex_free(t);
        return FALSE;
    }
    /* Growing the chain parks the level-0 block it replaces on v3dcore's
     * park list for PiStorm3D's mid-frame renaming. Here nothing can use it
     * (the job that last drew with this texture was rendered before the
     * change, before_change), and ogles2 is the list's only user: free it. */
    if (n > 1)
        v3d_texture_drain_parked_all(dev);
    if (faces(t) > 1 && !cube_memory(t)) {
        ogles2_v3d_tex_free(t);
        return FALSE;
    }
    {
        extern int g_v3d_uif_fast; /* v3dcore's direct UIF store, checked at its first use */
        static BOOL told;
        if (g_v3d_uif_fast == 2 && !told) {
            told = TRUE;
            ogles2_log("texture: v3dcore's direct UIF store differed from its general one: not used");
        }
    }
    /* A blank level is in the new copy already: its memory starts cleared
     * (SDL's testrendercopyex makes a render target a frame: 1.2 MB of
     * zeros stored at 640x480). */
    for (k = 0; k < faces(t); k++)
        for (l = 0; l < n; l++)
            if ((l == 0 || consistent(&t->f[k], l)) && !(t->f[k].blank & (1UL << l)))
                ogles2_v3d_tex_store(t, k, l, t->f[k].shadow[l], 0, 0, t->f[k].w[l], t->f[k].h[l]);
    return TRUE;
}

/* The number of levels the GPU copy has; 0: none. */
int ogles2_v3d_tex_levels(const struct OGLES2Texture *t)
{
    return t->gpu ? t->gpu->num_levels : 0;
}

/* The GPU copy can take level l of face k in place: the chain has it, laid
 * out from level 0's size (a cube map's new level 0 of the same size too). */
BOOL ogles2_v3d_tex_holds(const struct OGLES2Texture *t, int k, int l)
{
    const V3DTexture *g = t->gpu;
    if (!g || (l == 0 && g->width != t->f[k].w[0]) || l >= g->num_levels)
        return FALSE;
    return g->width == t->f[0].w[0] && g->height == t->f[0].h[0];
}

/* A texture changes while a job not rendered yet uses it (texture.c,
 * before_change): its GPU copy and records go with that job (freed when the
 * job's frame has rendered) and it gets a new one at its next use. FALSE:
 * not done (no copy, or the job's retire list nearly full). */
BOOL ogles2_v3d_tex_rename(struct OGLES2Context *user, struct OGLES2Texture *t)
{
    V3DContext *v = user->v3d;
    if (!v || !t->gpu || v->frame.retire_count >= V3D_RETIRE_MAX - 8)
        return FALSE;
    if (t->gpu->texture_mem.hostptr)
        v3d_frame_defer_free(&v->frame, &t->gpu->texture_mem);
    FreeVec(t->gpu);
    t->gpu = 0;
    if (t->rec.hostptr) { /* the job's draws read their records there too */
        v3d_frame_defer_free(&v->frame, &t->rec);
        memset(&t->rec, 0, sizeof(t->rec));
    }
    t->st_own = FALSE;
    return TRUE;
}

/* glGenerateMipmap on the TFU (texture.c, generate_tfu): whether it can
 * make levels 1 to n-1 of this copy, and doing it (0, or v3d_tfu's error;
 * -3: the GPU stayed busy). */
BOOL ogles2_v3d_tex_can_generate(const struct OGLES2Texture *t, int n)
{
    return t->gpu && t->gpu->texture_mem.hostptr && t->gpu->num_levels >= n &&
           v3d_tfu_supports_mipmaps(t->hw_type);
}

int ogles2_v3d_tex_generate(struct OGLES2Texture *t, int n)
{
    int r;
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the CPU's writes to level 0 in memory */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return -3;
    r = v3d_tfu_generate_mipmaps(ogles2_device(), t->gpu, t->hw_type, (v3d_u8)(n - 1));
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    return r;
}

/* glCopyTex* on the GPU (texture.c, ogles2_texture_scratch): level 0 of t,
 * RGBA8, filled by the TFU from the raster image at src (rows `stride`
 * bytes apart), red and blue swapped in the sampler for a BGRA bitmap. 0,
 * or v3d_tfu's error; -3: the GPU stayed busy. */
int ogles2_v3d_tex_from_raster(struct OGLES2Context *ctx, struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w,
                               UWORD h, BOOL bgra)
{
    UBYTE sw0, sw2;
    int r;
    (void)ctx;
    CacheClearE(0, ~0UL, CACRF_ClearD); /* no dirty CPU line over the texture's memory */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return -3;
    r = v3d_tfu_store_raster(ogles2_device(), t->gpu, 0, t->hw_type, w, h, (const void *)src, stride);
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r)
        return r;
    sw0 = bgra ? v3d_SWIZZLE_BLUE : v3d_SWIZZLE_RED;
    sw2 = bgra ? v3d_SWIZZLE_RED : v3d_SWIZZLE_BLUE;
    if (t->swizzle[0] != sw0 || t->swizzle[2] != sw2) {
        t->swizzle[0] = sw0;
        t->swizzle[2] = sw2;
        t->pver++; /* the state records hold the swizzle */
    }
    return 0;
}

BOOL ogles2_v3d_tex_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s)
{
    const V3DTexture *g;
    ULONG cpp;
    if (!ogles2_texture_renderable(t, k, l))
        return FALSE;
    if ((!t->gpu || l >= t->gpu->num_levels) && !ogles2_texture_upload(t))
        return FALSE;
    if (!(g = t->gpu) || l >= g->num_levels)
        return FALSE; /* not in the chain (a level of another size), or a cube map not complete */
    cpp = t->cpp;
    s->addr = V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256) + (ULONG)k * t->face_stride +
              g->levels[l].offset;
    s->bytes = g->levels[l].stride * g->levels[l].padded_h;
    s->mem_format = g->levels[l].tiling_format;
    s->format = t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB565 ? V3D_OUTPUT_IMAGE_FORMAT_BGR565
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA4  ? V3D_OUTPUT_IMAGE_FORMAT_ABGR4444
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB5A1 ? V3D_OUTPUT_IMAGE_FORMAT_ABGR1555
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F ? V3D_OUTPUT_IMAGE_FORMAT_RGBA16F
                                                             : V3D_OUTPUT_IMAGE_FORMAT_RGBA8;
    s->force_alpha1 = t->f[k].format[l] == GL_RGB;
    s->ub = s->mem_format == V3D_MEMORY_FORMAT_UIF_NO_XOR || s->mem_format == V3D_MEMORY_FORMAT_UIF_XOR
                ? g->levels[l].padded_h / (2 * v3d_utile_height((int)cpp))
                : 0;
    (void)ctx;
    return TRUE;
}

/* A depth texture's level as a job's depth (and stencil) image, its GPU
 * storage made first: the tile buffer stores into the level's own layout
 * as D16 or D24S8, which the TMU then samples as DEPTH_COMP16 or
 * DEPTH24_X8 (Mesa's depth textures). FALSE: none. */
BOOL ogles2_v3d_tex_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l,
                               struct OGLES2Surface *s)
{
    const V3DTexture *g;
    UBYTE bits;
    BOOL st;
    (void)ctx;
    if (!ogles2_texture_depth(t, l, &bits, &st) || !consistent(&t->f[0], l))
        return FALSE;
    if ((!t->gpu || l >= t->gpu->num_levels) && !ogles2_texture_upload(t))
        return FALSE;
    if (!(g = t->gpu) || l >= g->num_levels)
        return FALSE;
    s->addr = V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256) + g->levels[l].offset;
    s->bytes = g->levels[l].stride * g->levels[l].padded_h;
    s->mem_format = g->levels[l].tiling_format;
    s->format = bits == 16 ? V3D_OUTPUT_IMAGE_FORMAT_D16 : V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    s->force_alpha1 = FALSE;
    s->ub = s->mem_format == V3D_MEMORY_FORMAT_UIF_NO_XOR || s->mem_format == V3D_MEMORY_FORMAT_UIF_XOR
                ? g->levels[l].padded_h / (2 * v3d_utile_height((int)t->cpp))
                : 0;
    return TRUE;
}

/* Claims `size` bytes of the state buffer at a 32-byte boundary (the
 * TMU_CONFIG words carry configuration in the address's low bits). */
static void *claim32(V3DContext *v, ULONG size)
{
    v3d_u8 s = v->build_slot;
    v3d_static_buffer *sb = &v->state_buf[s];
    sb->used = (sb->used + 31) & ~31;
    return v3d_cl_claim_fast(ogles2_device(), &v->state_mem[s], sb, size, &v->frame);
}

static UBYTE wrap_mode(GLenum w)
{
    return w == GL_CLAMP_TO_EDGE ? V3D_WRAP_MODE_CLAMP
         : w == GL_MIRRORED_REPEAT ? V3D_WRAP_MODE_MIRROR
         : w == GL_MIRROR_CLAMP_TO_EDGE ? V3D_WRAP_MODE_MIRRORONCE : V3D_WRAP_MODE_REPEAT;
}

/* The texture and sampler state for unit `unit`'s texture of `cube`
 * kind, written into the job's state buffer (packing as PiStorm3D's
 * v3d_texture_emit_state; values as Mesa's v3dx_state.c). FALSE: out of
 * memory. */
BOOL ogles2_texture_state(struct OGLES2Context *ctx, int unit, int kind, ULONG *tex_state,
                          ULONG *sampler_state)
{
    struct OGLES2TexState *ts = texture_ts_fast(ctx);
    struct OGLES2Texture *t;
    V3DContext *v = ctx->v3d;
    v3d_texture_shader_state *tsr;
    v3d_sampler_state *ss;
    V3DTexture *g;
    char *tiled;
    ULONG *swivel;
    UBYTE *swivel24, temp;
    UWORD *swivel16;
    int first = 0, last = 0;
    BOOL own;

    if (!ts)
        return FALSE;
    t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit] : ts->unit2d[unit];
    /* The same records as the last draw with it: a texture a draw of a job
     * not yet rendered used gets a new GPU copy (another address) when its
     * images change (before_change); its parameters are compared here.
     * Records in the texture's own block hold across jobs (Quake III: most
     * of a frame's textures are drawn once a frame, 2.5 us a record). */
    if (t->gpu && (t->st_own || (t->st_gen == ogles2_job_gen && t->st_ctx == ctx)) &&
        t->st_mem == t->gpu->texture_mem.hostptr && t->st_pver == t->pver) {
        t->used_serial = ctx->job_serial;
        *tex_state = t->st_tex;
        *sampler_state = t->st_smp;
        return TRUE;
    }
    /* A renamed texture gets its new copy at its first draw at the latest. */
    if (!t->gpu && t != &ts->fallback)
        ogles2_texture_upload(t);
    if (!ogles2_texture_complete(t, &first, &last)) {
        if (!t->warned_incomplete && t != &ts->fallback) {
            int l, n = mipmapped(t->min_filter) && t->f[0].w[0] ? level_count(t->f[0].w[0], t->f[0].h[0]) : 1;
            t->warned_incomplete = TRUE;
            for (l = 1; l < n && consistent(&t->f[0], l); l++)
                ;
            ogles2_log("texture %ld sampled while incomplete (it reads black): %s, level 0 %ldx%ld, "
                       "min filter 0x%04lx, level %ld of %ld missing or inconsistent (logged once a texture)",
                       (ULONG)t->name, (ULONG)(!t->gpu ? "no GPU copy" : base_ok(t) ? "mip chain" : "level 0"),
                       (ULONG)t->f[0].w[0], (ULONG)t->f[0].h[0], (ULONG)t->min_filter, (ULONG)l, (ULONG)n);
        }
        if (!(t = ogles2_texture_fallback(ts)))
            return FALSE;
        first = last = 0;
    }
    /* Into the texture's own block, unless a job not yet rendered may read
     * the records it holds (the last job that drew with the texture): then
     * into this job's state buffer, as before. */
    own = !t->used_serial || !ogles2_job_uses(ctx, t->used_serial);
    if (own && !t->rec.hostptr && ogles2_mem_alloc(&t->rec, 64) < 0)
        own = FALSE;
    t->used_serial = ctx->job_serial;
    g = t->gpu;

    if (own)
        tsr = (v3d_texture_shader_state *)t->rec.hostptr;
    else if (!(tsr = claim32(v, sizeof(*tsr))))
        return FALSE;
    memset(tsr, 0, sizeof(*tsr));
    tiled = (char *)V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256);
    tsr->texture_base_pointer_rshift_6 = ((ULONG)tiled + g->levels[0].offset) >> 6;
    tsr->flip_texture_y_axis = FALSE;
    tsr->base_level = (ULONG)first; /* SGIS_texture_lod */
    tsr->max_level = (ULONG)last;
    tsr->image_width_lo = g->width & 0x3f;
    tsr->image_width_hi = g->width >> 6;
    tsr->image_height = g->height;
    /* A cube map: six layers, a face stride apart (Mesa's v3dx_state.c). The
     * fallback is 2D: as a cube, every face reads its one texel. */
    tsr->image_depth_lo = faces(t);
    tsr->image_depth_hi = 0;
    tsr->array_stride_64_byte_aligned = faces(t) > 1 ? t->face_stride / 64 : 0;
    tsr->texture_type = t->hw_type;
    tsr->swizzle_r = t->swizzle[0];
    tsr->swizzle_g = t->swizzle[1];
    tsr->swizzle_b = t->swizzle[2];
    tsr->swizzle_a = t->swizzle[3];
    if (g->tiling_format == V3D_MEMORY_FORMAT_UIF_XOR) {
        tsr->level_0_is_strictly_uif = TRUE;
        tsr->level_0_xor_enable = TRUE;
        tsr->level_0_ubpad = g->ub_pad;
        tsr->extended = TRUE;
    }
    swivel = (ULONG *)tsr;
    swivel[0] = LE32(swivel[0]);
    swivel[1] = LE32(swivel[1]);
    swivel[2] = LE32(swivel[2]);
    swivel[3] = LE32(swivel[3]);
    swivel[4] = LE32(swivel[4]);
    *tex_state = (ULONG)tsr;

    if (own)
        ss = (v3d_sampler_state *)((UBYTE *)t->rec.hostptr + 32);
    else if (!(ss = claim32(v, sizeof(*ss))))
        return FALSE;
    memset(ss, 0, sizeof(*ss));
    ss->mag_filter_nearest = t->mag_filter == GL_NEAREST;
    ss->min_filter_nearest = t->min_filter == GL_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                             t->min_filter == GL_NEAREST_MIPMAP_LINEAR;
    ss->mip_filter_nearest = t->min_filter != GL_NEAREST_MIPMAP_LINEAR &&
                             t->min_filter != GL_LINEAR_MIPMAP_LINEAR;
    /* 32-bit floats and depth: the TMU does not filter them. */
    if (unfilterable_type(t->hw_type) &&
        !(ss->mag_filter_nearest && ss->min_filter_nearest && ss->mip_filter_nearest)) {
        ss->mag_filter_nearest = ss->min_filter_nearest = ss->mip_filter_nearest = TRUE;
        if (!t->warned_filter) {
            t->warned_filter = TRUE;
            ogles2_log("texture %ld: 32-bit float or depth texels are sampled nearest (no linear filtering "
                       "for them; logged once a texture)", (ULONG)t->name);
        }
    }
    ss->depth_compare_function = V3D_COMPARE_FUNC_NEVER;
    ss->wrap_s = wrap_mode(t->wrap_s);
    ss->wrap_t = wrap_mode(t->wrap_t);
    /* The LOD range, Mesa's v3dx_state.c: SGIS_texture_lod's limits within
     * the field's 0 to 15; without mipmapping clamped just above the base
     * level, so the hardware still chooses between the min and mag filters. */
    {
        GLfloat mn = t->min_lod < 0.0f ? 0.0f : t->min_lod > 15.0f ? 15.0f : t->min_lod;
        GLfloat mx = t->max_lod < mn ? mn : t->max_lod > 15.0f ? 15.0f : t->max_lod;
        GLfloat bias = t->lod_bias < -MAX_LOD_BIAS ? -MAX_LOD_BIAS
                     : t->lod_bias > MAX_LOD_BIAS ? MAX_LOD_BIAS : t->lod_bias;
        if (!mipmapped(t->min_filter)) {
            if (mn > 1.0f / 256.0f) mn = 1.0f / 256.0f;
            if (mx > 1.0f / 256.0f) mx = 1.0f / 256.0f;
        }
        ss->min_level_of_detail = V3D_FLOAT_TO_U4_8(mn);
        ss->max_level_of_detail = V3D_FLOAT_TO_U4_8(mx);
        /* EXT_texture_lod_bias: s8.8 */
        ss->fixed_bias = (UWORD)(WORD)round_param(bias * 256.0f);
    }
    /* EXT_texture_filter_anisotropic, Mesa's levels: 2, 4, 8, 16 */
    if (t->max_aniso > 1.0f && !unfilterable_type(t->hw_type)) {
        ss->anisotropy_enable = TRUE;
        ss->maximum_anisotropy = t->max_aniso > 8.0f ? 3 : t->max_aniso > 4.0f ? 2 : t->max_aniso > 2.0f ? 1 : 0;
    }
    swivel24 = (UBYTE *)ss;
    temp = swivel24[3];
    swivel24[3] = swivel24[1];
    swivel24[1] = temp;
    swivel16 = (UWORD *)ss;
    swivel16[2] = LE16(swivel16[2]); /* the bias */
    swivel16[3] = LE16(swivel16[3]);
    *sampler_state = (ULONG)ss;
    t->st_gen = ogles2_job_gen;
    t->st_ctx = ctx;
    t->st_own = own;
    t->st_mem = g->texture_mem.hostptr;
    t->st_pver = t->pver;
    t->st_tex = *tex_state;
    t->st_smp = *sampler_state;
    return TRUE;
}

BOOL ogles2_texture_stored_of(const struct OGLES2Texture *t, UBYTE *hw_type, UBYTE swizzle[4])
{
    if (!t || !t->gpu)
        return FALSE;
    *hw_type = t->hw_type;
    swizzle[0] = t->swizzle[0];
    swizzle[1] = t->swizzle[1];
    swizzle[2] = t->swizzle[2];
    swizzle[3] = t->swizzle[3];
    return TRUE;
}
