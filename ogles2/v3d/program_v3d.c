/*
 * The V3D 4.2 backend's built-in programs (ogles2/backend.h,
 * docs/PI3_PLAN.md §5.1, P2 step 7): the built-in image (PiStorm3D's
 * smooth-shaded shaders, builtin_shaders.h, with a hand-written
 * description; clears drawn as rectangles), and the internal programs as
 * .v3dp files (tools/clear_mrt, tools/copy_tex). Moved from ogles2/program.c
 * and ogles2/draw.c.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/types.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "backend_v3d.h"
#include "builtin_shaders.h"
#include "clear_mrt_v3dp.h"
#include "copy_tex_v3dp.h"

/* ---- the built-in image ---- */

static const struct OGLES2ImageAttrib smooth_attribs[] = {
    {"a_position", GL_FLOAT_VEC3, 0, 1, 0}, /* VPM inputs 0-2, both shaders */
    {"a_color", GL_FLOAT_VEC4, 1, 1, 0},    /* VPM inputs 3-6, vertex shader only */
};

static const struct OGLES2Place smooth_mvp_place[] = {{0, 16, 4, 0, 0}};

static const struct OGLES2ImageUniform smooth_uniforms[] = {
    {"u_mvp", GL_FLOAT_MAT4, 0, 0, 1, smooth_mvp_place, {~0UL, ~0UL}},
};

/* The vertex and coordinate shaders read the same stream (v3d_assembler.c:
 * scale_p, scale_p_y, the matrix, then the depth range's scale and offset,
 * which the coordinate shader does not reach). The matrix goes row by row:
 * the shader forms x' = x M00 + y M10 + z M20 + w M30 from the first four
 * words, so they are row 0 of the column-major GL matrix (words 0, 4, 8, 12). */
static const ULONG smooth_rtype[20] = {
    OGLES2_U_PISTORM_VP_WIDTH, OGLES2_U_PISTORM_VP_HEIGHT,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    OGLES2_U_VIEWPORT_Z_SCALE, OGLES2_U_VIEWPORT_Z_OFFSET,
};
static const ULONG smooth_rdata[20] = {0, 0, 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15, 0, 0};

/* The record values PiStorm3D's d_BuildShaderRecord uses for this pairing,
 * expressed as prog_data: 4 threads, the vertex and coordinate shaders start
 * in their final thread section, VPM output segments of 1 (coordinate) and 2
 * (vertex), a VCM cache of 4 batches, four varyings using the pixel centre w. */
static const struct OGLES2Variant smooth_variants[3] = {
    {OGLES2_VARIANT_CS, 4, 1, 0, 0, 4, 0, 0, 0, {3}, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     {0}, {0}, {0}, 0, builtin_coordinate, sizeof(builtin_coordinate), 0, 20, smooth_rtype,
     smooth_rdata, 0, 0, 0, 0, 0, 0, 0, 0, {0}},
    {OGLES2_VARIANT_VS, 4, 1, 0, 0, 4, 0, 0, 0, {3, 4}, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     {0}, {0}, {0}, 0, builtin_vertex_smooth, sizeof(builtin_vertex_smooth),
     sizeof(builtin_coordinate), 20, smooth_rtype, smooth_rdata, 0, 0, 0, 0, 0, 0, 0, 0, {0}},
    {OGLES2_VARIANT_FS, 4, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0,
     {0}, {0}, {0}, 0, builtin_fragment_smooth, sizeof(builtin_fragment_smooth),
     sizeof(builtin_coordinate) + sizeof(builtin_vertex_smooth), 0, 0, 0, 0, 0, 0, 0, 0,
     1 /* render target 0 */, 0, 0, {0}},
};

static const struct OGLES2Image builtin_smooth = {
    "built-in smooth colour (PiStorm3D VERTEX_SMOOTH, FRAGMENT_UNTEXTURED_SMOOTH)",
    {0},
    3, smooth_variants,
    2, smooth_attribs,
    1, smooth_uniforms,
    {16, 0}, {0, 0},
    0, 0,
    sizeof(builtin_coordinate) + sizeof(builtin_vertex_smooth) + sizeof(builtin_fragment_smooth),
    0,          /* no OGLES2_U_CLIP_Z_COEFF: clears are drawn NEGATIVE_ONE_TO_ONE */
    0, 0,       /* code memory (draw.c uploads it per context), allocations */
    0, 0,       /* no binary */
    0, 0,       /* no 32-bit-sampler variants */
    0, 0,       /* no alpha-to-coverage variants */
    0, 0,       /* no sources */
    0,          /* in no program cache */
};

const struct OGLES2Image *ogles2_v3d_builtin_image(void)
{
    return &builtin_smooth;
}

/* An internal program's .v3dp (OGLES2_PROG_*), its length in *len. */
const UBYTE *ogles2_v3d_internal_program(int which, ULONG *len)
{
    switch (which) {
    case OGLES2_PROG_CLEAR_MRT:
        *len = sizeof(clear_mrt_v3dp);
        return clear_mrt_v3dp;
    case OGLES2_PROG_COPY_TEX:
        *len = sizeof(copy_tex_v3dp);
        return copy_tex_v3dp;
    default:
        *len = 0;
        return 0;
    }
}
