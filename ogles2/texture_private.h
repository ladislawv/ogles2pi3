/*
 * Texture objects as texture.c (the front end) and a backend's texture
 * storage (ogles2/v3d/texture_v3d.c) both see them (docs/PI3_PLAN.md §5.1,
 * P2 step 5), and the small helpers both use, inline.
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_TEXTURE_PRIVATE_H
#define OGLES2_TEXTURE_PRIVATE_H

#include "v3d_context.h"
#include "v3d_texture.h"

#define MAX_LEVELS 13 /* 4096 = 2^12 */

/* The images of one face (a 2D texture has one). */
struct OGLES2Face
{
    UWORD w[MAX_LEVELS], h[MAX_LEVELS];
    ULONG defined;                  /* levels given (bit per level) */
    GLenum format[MAX_LEVELS];      /* each level's internal format (glTexSubImage2D must match it) */
    GLenum type[MAX_LEVELS];        /* and the type it was given in (picks the stored format) */
    UBYTE *shadow[MAX_LEVELS];      /* RGBA8 copy of each level */
    ULONG rendered;                 /* levels the GPU drew into since: the copy is stale */
    ULONG blank;                    /* levels never given texels (glTexImage2D without pixels):
                                     * the copy is zeros, as a new GPU copy is (MEMF_CLEAR) */
};

struct OGLES2Texture
{
    GLuint name;
    GLenum target;                  /* 0 until first bound */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    GLfloat max_aniso;              /* EXT_texture_filter_anisotropic: 1 = off */
    GLfloat lod_bias;               /* EXT_texture_lod_bias (a texture parameter, as AmigaOS 4's) */
    GLfloat min_lod, max_lod;       /* SGIS_texture_lod */
    GLint base_level, max_level;
    ULONG pver;                     /* bumped by every parameter change: the records' check */
    struct OGLES2Face f[6];         /* 2D: f[0]; a cube map: +X, -X, +Y, -Y, +Z, -Z */
    /* The GPU copy, the backend's (backend.h, P2 step 5). These fields are
     * the V3D 4.2 backend's (ogles2/v3d/texture_v3d.c); a second backend's
     * storage replaces them with its own (PI3_PLAN §5.1). */
    V3DTexture *gpu;                /* NULL until level 0 exists (a cube map: of all faces) */
    UBYTE hw_type, cpp, swizzle[4]; /* the stored format (storage_of), set with gpu */
    ULONG face_stride;              /* a cube map: bytes from one face's levels to the next */
    ULONG used_serial;              /* the job that last drew with it */
    BOOL warned_incomplete;         /* sampled while incomplete: logged once */
    BOOL warned_filter;             /* sampled nearest though linear was asked for: logged once */
    /* The state records last made for a draw (ogles2_texture_state), reused
     * while what they hold is the same: by every later job when they are in
     * the texture's own block (rec), else by the job's later draws (in its
     * state buffer: a job still used the block when they changed). */
    ULONG st_gen;                   /* the control list they are in (ogles2_job_gen) */
    APTR st_mem;                    /* the GPU copy they point at */
    ULONG st_pver;                  /* the parameters they were made from */
    ULONG st_tex, st_smp;
    struct OGLES2Context *st_ctx;   /* whose job's state buffer they are in (not st_own) */
    BOOL st_own;                    /* they are in rec */
    v3d_mem rec;                    /* 64 bytes: texture state at 0, sampler state at 32; goes
                                     * with the GPU copy (freed, or deferred when renamed) */
    /* GLES 2.0 4.4.3: deleting a texture detaches it from the deleting
     * context's bound framebuffer only; attached to another, it lives on
     * without its name (orphan) until the last attachment goes. */
    UWORD fbo_refs;                 /* framebuffer attachment points that refer to it */
    BOOL orphan;                    /* deleted while attached: freed with the last of them */
};

struct OGLES2TexState
{
    struct OGLES2Share *sh;         /* the named textures: sh->tex, sh->ntex (share.c) */
    struct OGLES2Texture default2d, defaultcube, defaultrect; /* name 0: the context's own */
    struct OGLES2Texture *unit2d[OGLES2_MAX_TEXTURE_UNITS], *unitcube[OGLES2_MAX_TEXTURE_UNITS];
    struct OGLES2Texture *unitrect[OGLES2_MAX_TEXTURE_UNITS]; /* ARB_texture_rectangle */
    struct OGLES2Texture fallback;  /* 1x1 (0,0,0,1): what an incomplete texture samples */
    UBYTE active;
    BOOL warned_flush;
    BOOL logged_tfu;                /* glGenerateMipmap's first TFU job logged */
    struct OGLES2Texture *scratch;  /* glCopyTex* from the default framebuffer (copy.c) */
};

/* GL's conversion of a float to an integer parameter: the nearest. */
static inline GLint round_param(GLfloat f)
{
    return f < 0.0f ? -(GLint)(0.5f - f) : (GLint)(f + 0.5f);
}

/* What a level's RAM copy holds, by its format and type:
 *  - RGBA8: RGBA bytes, GL's conversions (the 16-bit packings expanded);
 *  - FLOAT: RGBA floats, the 68k's own (OES_texture_float, _half_float);
 *  - DEPTH16, DEPTH24: the stored depth word as a ULONG (OES_depth_texture:
 *    16 bits; or 24 bits << 8 with the stencil in the low byte, which is
 *    also OES_packed_depth_stencil's UNSIGNED_INT_24_8 layout);
 *  - BLOCKS: S3TC's 4x4 blocks as given, rows of ceil(w/4) blocks (M7,
 *    docs/M7_EXTENSIONS_PLAN.md step 3), stored as they are: the TMU
 *    decodes them.
 * The levels of a texture must be of level 0's class to be sampled. */
#define CLASS_RGBA8   0
#define CLASS_FLOAT   1
#define CLASS_DEPTH16 2
#define CLASS_DEPTH24 3
#define CLASS_BLOCKS  4

/* EXT_texture_compression_dxt1, EXT_texture_compression_s3tc and ANGLE's
 * dxt3/dxt5 (the same enums): a block's bytes, 0 for another format. */
static inline ULONG dxt_block_bytes(GLenum format)
{
    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return 16;
    default:
        return 0;
    }
}

/* Texels in blocks of 4, rounded up (a level smaller than 4x4 is one block). */
static inline ULONG blocks4(ULONG texels)
{
    return (texels + 3) / 4;
}

static inline int class_of(GLenum format, GLenum type)
{
    if (dxt_block_bytes(format))
        return CLASS_BLOCKS;
    if (type == GL_FLOAT || type == GL_HALF_FLOAT_OES)
        return CLASS_FLOAT;
    if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT)
        return CLASS_DEPTH16;
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES)
        return CLASS_DEPTH24;
    return CLASS_RGBA8;
}

/* Bytes per texel of a RAM copy of that class (BLOCKS is never asked: its
 * copies are only stored and replaced, never converted, filtered or read). */
static inline ULONG copy_bytes(int cls)
{
    return cls == CLASS_FLOAT ? 16 : 4;
}

static inline int level_class(const struct OGLES2Face *f, int l)
{
    return class_of(f->format[l], f->type[l]);
}

/* A float's bits and back. */
static inline ULONG f2u(GLfloat f)
{
    union { GLfloat f; ULONG u; } x;
    x.f = f;
    return x.u;
}

static inline GLfloat u2f(ULONG u)
{
    union { GLfloat f; ULONG u; } x;
    x.u = u;
    return x.f;
}

/* IEEE half precision (OES_texture_half_float) to single and back, by
 * their bits (nearest, ties away from zero). */
static inline GLfloat half_to_float(UWORD h)
{
    ULONG s = (ULONG)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023;
    if (e == 31)
        return u2f(s | 0x7f800000UL | (m << 13)); /* infinity, NaN */
    if (!e) { /* zero, or a subnormal: m * 2^-24 */
        GLfloat v = (GLfloat)m * (1.0f / 16777216.0f);
        return s ? -v : v;
    }
    return u2f(s | ((e + 112) << 23) | (m << 13));
}

static inline int level_count(int w, int h)
{
    int n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

/* Level `l` of a face has the size the mip chain of its level 0 gives it,
 * and its RAM copy is of level 0's class (the stored format is level 0's). */
static inline BOOL consistent(const struct OGLES2Face *f, int l)
{
    int w = f->w[0] >> l, h = f->h[0] >> l;
    /* The same format and type as level 0 (GLES 2.0 3.7.10), not just the
     * same storage: RGB and RGBA levels are both kept as RGBA8. */
    if (!(f->defined & (1UL << l)) || !(f->defined & 1) || level_class(f, l) != level_class(f, 0) ||
        f->format[l] != f->format[0] || f->type[l] != f->type[0])
        return FALSE;
    return f->w[l] == (w ? w : 1) && f->h[l] == (h ? h : 1);
}

static inline int faces(const struct OGLES2Texture *t)
{
    return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
}

/* Level 0 exists; a cube map's on all six faces, square and of one size
 * (GLES 2.0 3.7.10, "cube complete"). */
static inline BOOL base_ok(const struct OGLES2Texture *t)
{
    const struct OGLES2Face *f0 = &t->f[0];
    int k;
    if (!(f0->defined & 1) || !f0->w[0] || !f0->h[0])
        return FALSE;
    for (k = 1; k < faces(t); k++)
        if (!(t->f[k].defined & 1) || t->f[k].w[0] != f0->w[0] || t->f[k].h[0] != f0->h[0] ||
            level_class(&t->f[k], 0) != level_class(f0, 0) || t->f[k].format[0] != f0->format[0] ||
            t->f[k].type[0] != f0->type[0])
            return FALSE;
    return faces(t) == 1 || f0->w[0] == f0->h[0];
}

static inline BOOL mipmapped(GLenum min_filter)
{
    return min_filter != GL_NEAREST && min_filter != GL_LINEAR;
}

/* texture.c, for the backend */
extern BOOL ogles2_tex_cpu_uploads; /* probe U of the context last seen (a test switch) */
struct OGLES2TexState *ogles2_texture_ts(struct OGLES2Context *ctx);
/* ogles2_texture_ts without a call once the state exists (a draw's every sampler) */
static inline struct OGLES2TexState *texture_ts_fast(struct OGLES2Context *ctx)
{
    ogles2_tex_cpu_uploads = (ctx->probe & OGLES2_PROBE_CPU_UPLOAD) != 0;
    return ctx->tex ? ctx->tex : ogles2_texture_ts(ctx);
}
struct OGLES2Texture *ogles2_texture_fallback(struct OGLES2TexState *ts);
BOOL ogles2_texture_complete(const struct OGLES2Texture *t, int *first, int *last);
BOOL ogles2_texture_upload(struct OGLES2Texture *t); /* the GPU copy made again, rendered levels read back */

#endif
