/*
 * What the draw path's two halves share (docs/PI3_PLAN.md §5.1, P2 step 4):
 * draw.c, the front end, and a backend's draw (ogles2/v3d/draw_v3d.c).
 * Small helpers, inline in both.
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_DRAW_PRIVATE_H
#define OGLES2_DRAW_PRIVATE_H

static inline int type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_FLOAT: case GL_FIXED:          return 4;
    default:                               return 0;
    }
}

static inline ULONG float_bits(GLfloat f)
{
    union { GLfloat f; ULONG u; } c;
    c.f = f;
    return c.u;
}

/* The image's sampler uniforms, listed at the program's first draw (most of
 * a fixed-function emulation's uniforms are not samplers): how many to look
 * at, p->samplers[k] or (0xfe, too many to list) every uniform. */
static inline UWORD list_samplers(struct OGLES2Program *p)
{
    const struct OGLES2Image *im = p->image;
    UWORD u, n;
    if (p->nsamplers == 0xff) {
        for (u = 0, n = 0; u < im->nuniforms && n <= 16; u++)
            if (im->uniforms[u].flags & OGLES2_UNIFORM_SAMPLER) {
                if (n < 16)
                    p->samplers[n] = u;
                n++;
            }
        p->nsamplers = n <= 16 ? (UBYTE)n : 0xfe;
    }
    return p->nsamplers == 0xfe ? im->nuniforms : p->nsamplers;
}

/* The texture unit of element e of sampler uniform u (glUniform1i). */
static inline UBYTE unit_of(const struct OGLES2Program *p, UWORD u, UWORD e)
{
    return p->uniform_loc[u] == 0xFFFF ? 0 : p->sampler_unit[p->uniform_loc[u] + e];
}

/* Probe P: the time since the last mark goes to stage k (frame.c reports
 * them): 0 checks and the index range, 1 shaders, 2 textures, 3 uniforms,
 * 4 vertex data, 5 the record, 6 indices, 7 the binning list. */
static inline void mark(struct OGLES2Context *ctx, int k)
{
    if (ctx->probe & OGLES2_PROBE_PERIODIC) {
        ULONG t = ogles2_clock_us();
        if (k >= 0)
            ctx->stage[k] += (double)(ULONG)(t - ctx->t_stage);
        ctx->t_stage = t;
    }
}

/* Bytes per index. */
static inline ULONG index_size(GLenum type)
{
    return type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
}

#endif
