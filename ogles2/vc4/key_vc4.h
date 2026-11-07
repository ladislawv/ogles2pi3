/*
 * The VC4 backend's shader keys (docs/PI3_PLAN.md §4.2, P5): the GL state a
 * draw has, turned into Mesa vc4's keys as `v3dglslc -k` and a version-2
 * compiler request read them (v3dp/FORMAT.md 1.1's VC4_VARIANT key layout,
 * after a u32 stage), and the texture storage the keys' sampler entries
 * describe. Plain C, no AmigaOS types: tests/pi3/keys builds it on the PC and
 * checks its keys against the ones Mesa builds for the same states.
 *
 * Mesa's enum values (pipe_format, PIPE_BLEND*, PIPE_TEX_WRAP_*,
 * PIPE_SWIZZLE_*) are the validated commit's, 74b69717c52b.
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_KEY_VC4_H
#define OGLES2_KEY_VC4_H

#include <GLES2/gl2.h>

/* the extension tokens it takes (gl2ext.h's values) */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_MIN_EXT
#define GL_MIN_EXT 0x8007
#define GL_MAX_EXT 0x8008
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_MIRROR_CLAMP_TO_EDGE
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif
#ifndef GL_DEPTH_STENCIL_OES
#define GL_DEPTH_STENCIL_OES 0x84F9
#define GL_UNSIGNED_INT_24_8_OES 0x84FA
#endif

/* pipe_format values the keys use */
#define VC4K_R32_FLOAT 21
#define VC4K_R32G32_FLOAT 22
#define VC4K_R32G32B32_FLOAT 23
#define VC4K_R32G32B32A32_FLOAT 24
#define VC4K_R8G8B8A8_UNORM 84
#define VC4K_B8G8R8A8_UNORM 105
#define VC4K_R8G8B8X8_UNORM 293
#define VC4K_B5G6R5_UNORM 309
#define VC4K_A1B5G5R5_UNORM 301
#define VC4K_L8_UNORM 315
#define VC4K_A8_UNORM 316
#define VC4K_L8A8_UNORM 319
#define VC4K_ETC1_RGB8 186
#define VC4K_S8_UINT_Z24_UNORM 274 /* depth and stencil textures: stencil in the low byte */
#define VC4K_X8Z24_UNORM 278       /* depth textures (16-bit ones too: VC4 samples no Z16) */
#define VC4K_R16G16B16A16_FLOAT 56 /* half-float and float textures: four S16F planes (patch 0018) */

/* How a GL texture format is stored on VC4 and sampled: the TMU's type
 * (VC4_TEX_*: vc4core.h), bytes a texel (a 4x4 block for ETC1), the key's
 * pipe format and view swizzle (PIPE_SWIZZLE_*: X 0 .. W 3, 0 4, 1 5). */
struct vc4_tex_format
{
    unsigned char hw_type, cpp, block;
    unsigned short key_format;
    unsigned char swizzle[4];
};

/* The storage of (format, type) as glTexImage2D gives them (internal format
 * = format in ES 2.0), or of a compressed format (type 0); 0 if VC4 has no
 * direct one (the caller converts, or refuses). */
const struct vc4_tex_format *vc4_tex_format(GLenum format, GLenum type);

/* A vertex array's pipe_format (VC4 converts in the shader: the vertex key
 * holds it); 0 if none. */
unsigned vc4_attr_format(GLenum type, int size, int normalized);

struct vc4_key_sampler
{
    const struct vc4_tex_format *f; /* NULL: no texture bound (zeros) */
    GLenum wrap_s, wrap_t;          /* GL_MIRROR_CLAMP_TO_EDGE: the shader's absolute value (patch 0019) */
    /* The texture parameters the shader emulates (docs/PI3_PLAN.md §2.1,
     * patch 0019): one level sampled above level 0 (Mesa's
     * force_first_level: an explicit LOD), EXT_texture_lod_bias's bias,
     * SGIS_texture_lod's levels and LOD range (an LOD computed in the
     * shader and clamped), EXT_texture_filter_anisotropic (2^aniso
     * lookups). */
    unsigned char force_first, lod_bias, lod_clamp, aniso;
    /* A half-float texture holding negative values: eight S16F planes, since
     * the TMU drops the sign (patch 0022, texture_vc4.c). */
    unsigned char half_signed;
};

/* What the fragment key takes from a draw. */
struct vc4_fs_state
{
    unsigned color_format;      /* the target's pipe_format: VC4K_B8G8R8A8_UNORM (the display),
                                 * VC4K_R8G8B8A8_UNORM (a texture), VC4K_B5G6R5_UNORM */
    unsigned char prim;         /* 0 triangles, 1 lines, 2 points */
    unsigned char depth_test, stencil_test, stencil_twoside, stencil_full_writemasks;
    unsigned char point_coord_upper_left, msaa, sample_coverage, alpha_to_coverage, alpha_to_one;
    unsigned char blend;        /* on: the equations and factors below (GL's) */
    GLenum eq_rgb, eq_alpha, src_rgb, dst_rgb, src_alpha, dst_alpha;
    unsigned char colormask;    /* bit 0 red .. bit 3 alpha */
    unsigned char draw_buffer;  /* EXT_draw_buffers: the pass's output (patch 0016) */
    unsigned char nsamplers;
    struct vc4_key_sampler samp[16];
};

/* What the vertex key takes (the coordinate shader's is the same). */
struct vc4_vs_state
{
    unsigned char nattrs;       /* the shader's inputs in slot order */
    unsigned short attr_format[8];
    unsigned char per_vertex_point_size; /* points, and the shader writes gl_PointSize */
    unsigned char clip_halfz;   /* EXT_clip_control's ZERO_TO_ONE: z from 0..w (patch 0020) */
    unsigned char nsamplers;
    struct vc4_key_sampler samp[16];
};

#define VC4_KEY_FS_BYTES (4 + 292) /* a record: the stage, the key */
#define VC4_KEY_VS_BYTES (4 + 296)

/* The records into out (VC4_KEY_*_BYTES); canonical: what does not change
 * the code left out (§4.2): the wrap modes (only CLAMP and CLAMP_TO_BORDER
 * would, GLES2 has neither) written as REPEAT, the comparison function when
 * comparison is off as 0 (Mesa fills in the sampler's, GL's LEQUAL); so
 * draws that differ only in them share a variant. */
void vc4_key_fs(unsigned char *out, const struct vc4_fs_state *s, int canonical);
void vc4_key_vs(unsigned char *out, const struct vc4_vs_state *s, int canonical);

/* Hint records for a link's request (tools/v3dglslc/vc4_variants.c), for the
 * program's first draw: 12 bytes each, a u32 stage of 5 to 15 and 8 bytes; a
 * compiler skips a kind it does not know. Stage 5: the reference draw's fragment
 * variant with this target format and depth test. Stage 6: its vertex variants with
 * the attributes at the slots of the mask that it declares vec4 (four floats) fed
 * three floats instead (glVertexAttribPointer's size 3 for a vec4 position). */
#define VC4_KEY_HINT_BYTES 12
void vc4_key_hint(unsigned char *out, unsigned color_format, int depth);
void vc4_key_vs_hint(unsigned char *out, unsigned slots);
/* A link's hints for a context (ogles2_vc4_link_hints): the vertex hint for slot 0;
 * the display's format without depth unless it is the reference's RGBA8; with depth
 * when the context has a depth buffer. The bytes written (0 if size is short). */
unsigned vc4_key_link_hints(unsigned char *buf, unsigned size, unsigned display_format, int depth_buffer);

#endif
