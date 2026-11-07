/*
 * The VC4 backend's shader keys and texture storage (key_vc4.h).
 *
 * The sampler entries follow what Mesa's state tracker and vc4 driver make
 * of the same texture: the format vc4_formats.c stores it as, and the view
 * swizzle the state tracker gives its base format (RGB: alpha one;
 * LUMINANCE: XXX1; LUMINANCE_ALPHA: XXXW; ALPHA: 000W). RGBA4 is kept in
 * the RGBA4444 texture type, which reads GL's packing in the RGBA8 order, so
 * its key is RGBA8's (Mesa itself stores GL_RGBA4 as RGBA8: the same code).
 * Depth textures (OES_depth_texture, OES_packed_depth_stencil) are Z24S8
 * words read as RGBA8888, the depth in the top 24 bits, which the shader
 * scales (Mesa's ntq_scale_depth_texture); X8Z24 for DEPTH_COMPONENT of
 * either type, S8_UINT_Z24 for DEPTH_STENCIL, sampled as luminance (XXX1).
 * Half-float and float textures (OES_texture_half_float, OES_texture_float,
 * docs/PI3_PLAN.md §2.1) are four planes of the S16F type, one a channel
 * (expanded to RGBA at upload), keyed R16G16B16A16_FLOAT (the compiler's four
 * lookups, tools/v3dglslc/patches/0018); Mesa's vc4 has no such textures.
 * Checked against the keys Mesa builds by tests/pi3/keys.
 *
 * MIT licence (LICENSE).
 */
#include <string.h>
#include "key_vc4.h"
#include "vc4core.h"

#define SW(a, b, c, d) {a, b, c, d}
#define X 0
#define Y 1
#define Z 2
#define W 3
#define S0 4
#define S1 5

static const struct vc4_tex_format
    rgba8 = {VC4_TEX_RGBA8888, 4, 0, VC4K_R8G8B8A8_UNORM, SW(X, Y, Z, W)},
    rgbx8 = {VC4_TEX_RGBX8888, 4, 0, VC4K_R8G8B8X8_UNORM, SW(X, Y, Z, S1)},
    bgra8 = {VC4_TEX_RGBA8888, 4, 0, VC4K_B8G8R8A8_UNORM, SW(X, Y, Z, W)},
    rgb565 = {VC4_TEX_RGB565, 2, 0, VC4K_B5G6R5_UNORM, SW(X, Y, Z, S1)},
    rgba4444 = {VC4_TEX_RGBA4444, 2, 0, VC4K_R8G8B8A8_UNORM, SW(X, Y, Z, W)},
    rgba5551 = {VC4_TEX_RGBA5551, 2, 0, VC4K_A1B5G5R5_UNORM, SW(X, Y, Z, W)},
    lum = {VC4_TEX_ALPHA, 1, 0, VC4K_L8_UNORM, SW(X, X, X, S1)},
    alpha = {VC4_TEX_ALPHA, 1, 0, VC4K_A8_UNORM, SW(S0, S0, S0, W)},
    lumalpha = {VC4_TEX_LUMALPHA, 2, 0, VC4K_L8A8_UNORM, SW(X, X, X, W)},
    etc1 = {VC4_TEX_ETC1, 8, 1, VC4K_ETC1_RGB8, SW(X, Y, Z, S1)},
    depth = {VC4_TEX_RGBA8888, 4, 0, VC4K_X8Z24_UNORM, SW(X, X, X, S1)},
    depth_stencil = {VC4_TEX_RGBA8888, 4, 0, VC4K_S8_UINT_Z24_UNORM, SW(X, X, X, S1)},
    half = {VC4_TEX_S16F, 2, 0, VC4K_R16G16B16A16_FLOAT, SW(X, Y, Z, W)}; /* a plane's texel */

const struct vc4_tex_format *vc4_tex_format(GLenum format, GLenum type)
{
    if (type == GL_HALF_FLOAT_OES || type == GL_FLOAT)
        return format == GL_RGBA || format == GL_RGB || format == GL_LUMINANCE || format == GL_ALPHA ||
                       format == GL_LUMINANCE_ALPHA
                   ? &half
                   : 0;
    switch (format) {
    case GL_RGBA:
        return type == GL_UNSIGNED_BYTE ? &rgba8
             : type == GL_UNSIGNED_SHORT_4_4_4_4 ? &rgba4444
             : type == GL_UNSIGNED_SHORT_5_5_5_1 ? &rgba5551 : 0;
    case GL_RGB:
        return type == GL_UNSIGNED_BYTE ? &rgbx8 : type == GL_UNSIGNED_SHORT_5_6_5 ? &rgb565 : 0;
    case GL_BGRA_EXT:
        return type == GL_UNSIGNED_BYTE ? &bgra8 : 0;
    case GL_LUMINANCE:
        return type == GL_UNSIGNED_BYTE ? &lum : 0;
    case GL_ALPHA:
        return type == GL_UNSIGNED_BYTE ? &alpha : 0;
    case GL_LUMINANCE_ALPHA:
        return type == GL_UNSIGNED_BYTE ? &lumalpha : 0;
    case GL_ETC1_RGB8_OES:
        return &etc1;
    case GL_DEPTH_COMPONENT:
        return type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT ? &depth : 0;
    case GL_DEPTH_STENCIL_OES:
        return type == GL_UNSIGNED_INT_24_8_OES ? &depth_stencil : 0;
    default:
        return 0;
    }
}

unsigned vc4_attr_format(GLenum type, int size, int normalized)
{
    /* pipe_format: each family's 1-component value, then +1 per component */
    unsigned base;
    if (size < 1 || size > 4)
        return 0;
    switch (type) {
    case GL_FLOAT: base = 21; break;                          /* R32_FLOAT */
    case GL_FIXED: base = 21; break;                          /* floats: draw_vc4.c's convert */
    case GL_UNSIGNED_BYTE: base = normalized ? 81 : 89; break;  /* R8_UNORM, R8_USCALED */
    case GL_BYTE: base = normalized ? 85 : 93; break;           /* R8_SNORM, R8_SSCALED */
    case GL_UNSIGNED_SHORT: base = normalized ? 57 : 65; break; /* R16_UNORM, R16_USCALED */
    case GL_SHORT: base = normalized ? 61 : 69; break;          /* R16_SNORM, R16_SSCALED */
    case GL_HALF_FLOAT_OES: base = 53; break;                   /* R16_FLOAT */
    default: return 0;
    }
    return base + (unsigned)size - 1;
}

/* GL's blend equations and factors as PIPE_BLEND_* and PIPE_BLENDFACTOR_*. */
static unsigned char pipe_eq(GLenum e)
{
    switch (e) {
    case GL_FUNC_SUBTRACT: return 1;
    case GL_FUNC_REVERSE_SUBTRACT: return 2;
    case GL_MIN_EXT: return 3;
    case GL_MAX_EXT: return 4;
    default: return 0; /* GL_FUNC_ADD */
    }
}

static unsigned char pipe_factor(GLenum f)
{
    switch (f) {
    case GL_ONE: return 1;
    case GL_SRC_COLOR: return 2;
    case GL_SRC_ALPHA: return 3;
    case GL_DST_ALPHA: return 4;
    case GL_DST_COLOR: return 5;
    case GL_SRC_ALPHA_SATURATE: return 6;
    case GL_CONSTANT_COLOR: return 7;
    case GL_CONSTANT_ALPHA: return 8;
    case GL_ZERO: return 17;
    case GL_ONE_MINUS_SRC_COLOR: return 18;
    case GL_ONE_MINUS_SRC_ALPHA: return 19;
    case GL_ONE_MINUS_DST_ALPHA: return 20;
    case GL_ONE_MINUS_DST_COLOR: return 21;
    case GL_ONE_MINUS_CONSTANT_COLOR: return 23;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return 24;
    default: return 1;
    }
}

static unsigned char pipe_wrap(GLenum w, int canonical)
{
    if (w == GL_MIRROR_CLAMP_TO_EDGE)
        return 6; /* PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE: the shader's absolute value (patch 0019) */
    if (canonical)
        return 0; /* only CLAMP and CLAMP_TO_BORDER change vc4's code: GLES2 has neither */
    return w == GL_CLAMP_TO_EDGE ? 2 : w == GL_MIRRORED_REPEAT ? 4 : 0;
}

static unsigned char *put32(unsigned char *o, unsigned v)
{
    o[0] = (unsigned char)v;
    o[1] = (unsigned char)(v >> 8);
    o[2] = (unsigned char)(v >> 16);
    o[3] = (unsigned char)(v >> 24);
    return o + 4;
}

/* The samplers: 16 entries of 16 bytes (FORMAT.md 1.1). */
static unsigned char *samplers(unsigned char *o, int n, const struct vc4_key_sampler *s, int canonical)
{
    int i;
    memset(o, 0, 16 * 16);
    for (i = 0; i < n && i < 16; i++, o += 16) {
        if (!s[i].f)
            continue;
        put32(o, s[i].f->key_format);
        memcpy(o + 4, s[i].f->swizzle, 4);
        /* o[8], o[9]: compare mode (GLES2 samples no shadow) and function: the
         * sampler's, GL's LEQUAL (PIPE_FUNC_LEQUAL), as Mesa fills it */
        o[9] = canonical ? 0 : 3;
        o[10] = pipe_wrap(s[i].wrap_s, canonical);
        o[11] = pipe_wrap(s[i].wrap_t, canonical);
        o[12] = s[i].force_first;
        /* patch 0019's emulations (FORMAT.md 1.1) */
        o[13] = (unsigned char)((s[i].lod_bias ? 1 : 0) | (s[i].lod_clamp ? 2 : 0));
        o[14] = s[i].aniso;
        o[15] = s[i].half_signed; /* patch 0022 */
    }
    return o + 16 * (16 - (n < 16 ? n : 16));
}

void vc4_key_hint(unsigned char *out, unsigned color_format, int depth)
{
    unsigned char *o = put32(out, 5); /* the stage of a hint */
    o = put32(o, color_format);
    *o++ = (unsigned char)(depth ? 1 : 0);
    *o++ = 0;
    *o++ = 0;
    *o = 0;
}

void vc4_key_fs(unsigned char *out, const struct vc4_fs_state *s, int canonical)
{
    unsigned char *o = put32(out, 4);
    o = samplers(o, s->nsamplers, s->samp, canonical);
    o = put32(o, s->color_format);
    /* Mesa: the depth test's flag is on whenever the stencil test is */
    *o++ = s->depth_test || s->stencil_test;
    *o++ = s->stencil_test;
    *o++ = s->stencil_test && s->stencil_twoside;
    *o++ = s->stencil_test && s->stencil_full_writemasks;
    *o++ = s->prim == 2;
    *o++ = s->prim == 1;
    *o++ = s->prim == 2 && s->point_coord_upper_left;
    *o++ = s->msaa;
    *o++ = s->msaa && s->sample_coverage;
    *o++ = s->msaa && s->alpha_to_coverage;
    *o++ = s->msaa && s->alpha_to_one;
    *o++ = 12;                    /* logicop_func: PIPE_LOGICOP_COPY */
    o = put32(o, 0);              /* point_sprite_mask */
    o = put32(o, 0);              /* ubo_1_size */
    *o++ = 0;                     /* ucp_enables */
    if (s->blend) {
        unsigned char eq = pipe_eq(s->eq_rgb), eqa = pipe_eq(s->eq_alpha);
        *o++ = 1;
        *o++ = eq;
        /* MIN and MAX ignore the factors: the state tracker makes them ONE */
        *o++ = eq >= 3 ? 1 : pipe_factor(s->src_rgb);
        *o++ = eq >= 3 ? 1 : pipe_factor(s->dst_rgb);
        *o++ = eqa;
        *o++ = eqa >= 3 ? 1 : pipe_factor(s->src_alpha);
        *o++ = eqa >= 3 ? 1 : pipe_factor(s->dst_alpha);
    } else {
        memset(o, 0, 7);
        o += 7;
    }
    *o++ = s->colormask & 15;
    *o++ = s->draw_buffer;
    *o++ = 0;
    *o++ = 0;
}

void vc4_key_vs(unsigned char *out, const struct vc4_vs_state *s, int canonical)
{
    unsigned char *o = put32(out, 0);
    int i;
    o = samplers(o, s->nsamplers, s->samp, canonical);
    for (i = 0; i < 8; i++)
        o = put32(o, i < s->nattrs ? s->attr_format[i] : 0);
    *o++ = 0;                     /* is_coord: the compiler makes the coordinate twin */
    *o++ = s->per_vertex_point_size;
    *o++ = s->clip_halfz;         /* patch 0020 */
    *o++ = 0;
    o = put32(o, 0);              /* fs_inputs: the compiler takes the fragment variant's */
}
