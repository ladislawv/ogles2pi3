/*
 * The Linux tests' program loader and draw builder (docs/PI3_PLAN.md §3.1,
 * P3): a .vc4p file (v3dp/FORMAT.md 1.1, written by v3dglslc -g 21) read,
 * and a draw built from it as Mesa's vc4 builds one (vc4_draw.c,
 * vc4_uniforms.c): each stage's uniform stream from its recipe, the shader
 * record from the variants' prog data, the binning list's state and draw
 * packets. A rehearsal of what the VC4 backend does in P5, kept to what the
 * tests need.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#ifndef VC4P_H
#define VC4P_H

#include "vc4core.h"

#define VC4P_STAGE_VS 0
#define VC4P_STAGE_CS 1
#define VC4P_STAGE_FS 4

struct vc4p_variant
{
    uint32_t stage;
    /* key */
    uint32_t tex_format[16];              /* each sampler's pipe_format */
    uint32_t attr_formats[8];
    uint8_t is_coord, per_vertex_point_size;
    uint32_t color_format;
    uint8_t depth_enabled, stencil_enabled, is_points, is_lines, blend_enable, colormask, draw_buffer;
    /* prog data */
    uint8_t fs_threaded, num_inputs, vattrs_live, disable_early_z;
    uint8_t vattr_offsets[9];
    uint32_t color_inputs, num_texture_samples;
    /* recipe and code */
    uint32_t nrecipe;
    uint32_t rtype[256], rdata[256];
    uint32_t code_bytes;
    uint8_t *code;
};

struct vc4p_place
{
    uint32_t stage, offset;
    uint8_t elem_stride, vec_stride, int_as_float;
};

struct vc4p_uniform
{
    char name[64];
    uint32_t type, array, flags, nplaces;
    struct vc4p_place place[2];
    uint32_t sampler[2];
};

struct vc4p_attrib
{
    char name[64];
    uint32_t type, slot;
};

struct vc4p
{
    uint32_t cbuf_words[2];       /* vertex, fragment */
    uint32_t cbuf[2][1024];
    int nuniforms, nattribs, nvariants;
    struct vc4p_uniform u[64];
    struct vc4p_attrib a[16];
    struct vc4p_variant v[32];
};

/* 0, or -1 with the reason printed. */
int vc4p_load(struct vc4p *p, const char *path);
void vc4p_free(struct vc4p *p);
/* The first variant of stage (points: is_points for the fragment shader,
 * per_vertex_point_size for the vertex ones). */
const struct vc4p_variant *vc4p_find(const struct vc4p *p, uint32_t stage, int points);
/* Mesa's pipe_format values at 74b69717c52b (u_formats.h is generated:
 * printed from the host's Mesa build), the ones the tests' keys hold. */
#define VC4P_FORMAT_R32G32_FLOAT 22
#define VC4P_FORMAT_R32G32B32_FLOAT 23
#define VC4P_FORMAT_R8G8B8A8_UNORM 84
#define VC4P_FORMAT_B5G6R5_UNORM 309
#define VC4P_FORMAT_L8_UNORM 315
#define VC4P_FORMAT_A1B5G5R5_UNORM 301
#define VC4P_FORMAT_B4G4R4A4_UNORM 304
#define VC4P_FORMAT_L8A8_UNORM 319
#define VC4P_FORMAT_ETC1_RGB8 186
/* The vertex or coordinate variant (not for points) whose attribute slot
 * has format; the fragment variant for triangles (no depth test, blending,
 * stencil test) whose sampler unit has format. */
const struct vc4p_variant *vc4p_find_attr(const struct vc4p *p, uint32_t stage, uint32_t slot, uint32_t format);
const struct vc4p_variant *vc4p_find_tex(const struct vc4p *p, uint32_t unit, uint32_t format);
/* The fragment variant for a primitive class (0 triangles, 1 lines, 2
 * points) with the depth test, blending and the stencil test on or off. */
const struct vc4p_variant *vc4p_find_fs(const struct vc4p *p, int prim, int depth, int blend, int stencil);
/* The same for a render target of color_format (a pipe_format: vc4_find_fs
 * takes R8G8B8A8). */
const struct vc4p_variant *vc4p_find_fs_rt(const struct vc4p *p, int prim, int depth, int blend, int stencil,
                                           uint32_t color_format);

/* The stencil test's uniform words for one-sided GL state (Mesa's
 * vc4_state.c: tlb_stencil_setup_bits; the reference in bits 8-15 as
 * vc4_uniforms.c adds it): func GL_NEVER..GL_ALWAYS, ops the hardware's
 * codes (VC4P_STENCIL_*), writemask 0x1, 0x3, 0xf or 0xff (others need
 * the third word: not here). */
#define VC4P_STENCIL_ZERO 0
#define VC4P_STENCIL_KEEP 1
#define VC4P_STENCIL_REPLACE 2
#define VC4P_STENCIL_INCR 3
#define VC4P_STENCIL_DECR 4
#define VC4P_STENCIL_INVERT 5
#define VC4P_STENCIL_INCR_WRAP 6
#define VC4P_STENCIL_DECR_WRAP 7
uint32_t vc4p_stencil_word(uint32_t gl_func, uint32_t ref, uint32_t valuemask, uint32_t fail, uint32_t zfail,
                           uint32_t zpass, uint32_t writemask);
/* A uniform's value(s) by name into the constant buffers (floats as their
 * bits; n words, its places' layout). -1 if there is no such uniform. */
int vc4p_set(struct vc4p *p, const char *name, const uint32_t *words, uint32_t n);

/* One draw. Attributes: the arrays of the vertex shader's inputs, in slot
 * order (bytes per vertex and stride as the GPU reads them: the variant's
 * attr_formats say the type). */
struct vc4p_draw
{
    const struct vc4p *prog;
    const struct vc4p_variant *vs, *cs, *fs;
    uint32_t nattrs;
    struct { const void *data; uint32_t bytes, stride, size; } attr[8];
    uint32_t mode, count;                 /* VC4_PRIM_*, vertices or indices */
    const uint16_t *indices;              /* NULL: arrays, in order */
    const uint8_t *indices8;              /* or 8-bit ones */
    uint32_t tex[16][3];                  /* per fragment sampler index: P0, P1, P2 */
    float vp_x, vp_y, vp_w, vp_h;         /* the viewport, pixels */
    uint16_t clip[4];                     /* the scissor box: x, y (from the bottom), w, h; w 0: the frame */
    float depth_near, depth_far;
    uint32_t blend_color;                 /* RGBA8888, for the BLEND_CONST recipes */
    uint32_t stencil[3];                  /* the STENCIL recipes' words (vc4p_stencil_word) */
    struct VC4_CONFIGURATION_BITS cfg;    /* the rasteriser's and depth test's state */
};

/* Into job: the records and streams (job->state), the packets (job->bcl).
 * 0, or -1 (out of memory, or a recipe type the builder does not have). */
int vc4p_emit(struct vc4_job *job, const struct vc4p_draw *d, uint16_t width, uint16_t height);

#endif
