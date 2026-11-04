/*
 * The V3D 4.2 backend's program variants (ogles2/backend.h, docs/PI3_PLAN.md
 * §5.1, P2 step 7): a .v3dp VARIANT's key and prog data (v3dp/FORMAT.md
 * 1.0), its uniform recipe's types (Mesa v3d's enum quniform_contents at the
 * validated commit), and the variant a draw uses. Moved from ogles2/v3dp.c,
 * which reads the rest of the file.
 *
 * MIT licence (LICENSE).
 */
#include <exec/types.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "backend_v3d.h"
#include "v3dp_private.h"

/* The common prog data (FORMAT.md "Common key and prog data"), 28 bytes. */
static void read_common_prog_data(struct ogles2_rd *r, struct OGLES2Variant *v)
{
    v->spill_size = u32(r);
    skip(r, 4 * 4);              /* tmu_spills, tmu_fills, tmu_count, qpu_read_stalls */
    skip(r, 1);                  /* compile_strategy_idx */
    v->threads = u8(r);
    v->single_seg = u8(r);
    skip(r, 3 + 2);              /* tmu_dirty_rcl, has_control_barrier, has_global_address, pad */
}

#define COMMON_KEY_BYTES (4 + 6 + 2) /* sampler_is_32b, six bools, pad */

/* A VARIANT section after its stage: the key and prog data (FORMAT.md
 * "Common key and prog data", "Vertex and binning shaders", "Fragment
 * shader"). 0, or why the file is refused. */
const char *ogles2_v3d_variant_read(struct ogles2_rd *s, struct OGLES2Variant *v)
{
    ULONG j;
    if (v->stage == OGLES2_VARIANT_VS || v->stage == OGLES2_VARIANT_CS) {
        skip(s, COMMON_KEY_BYTES);
        skip(s, 4 + 64 + 4);             /* num_used_outputs+pad, used_outputs, va_swap_rb_mask */
        (void)u8(s);                     /* is_coord (the stage says it) */
        v->per_vertex_point_size = u8(s);
        skip(s, 2);                      /* clamp_color, pad */
        read_common_prog_data(s, v);
        v->uses_iid = u8(s);
        (void)u8(s);                     /* uses_biid */
        v->uses_vid = u8(s);
        v->separate_segments = u8(s);
        v->vcm_cache_size = u8(s);
        v->writes_psiz = u8(s);
        skip(s, 2);
        for (j = 0; j < 16; j++)
            v->vattr_sizes[j] = u8(s);
        v->vpm_input_size = u32(s);
        v->vpm_output_size = u32(s);
        skip(s, 64 * 4);                 /* driver_location_map */
    } else if (v->stage == OGLES2_VARIANT_FS) {
        skip(s, COMMON_KEY_BYTES);
        v->is_points = u8(s);
        v->is_lines = u8(s);
        skip(s, 8);                      /* line_smoothing .. ignore_sample_mask */
        v->cbufs = u8(s);
        skip(s, 6 + 3);                  /* the other key bytes, pad */
        skip(s, 8 * 8 + 8 * 24 + 4 + 4); /* colour formats, blend, logicop, sprite mask */
        read_common_prog_data(s, v);
        v->uses_pid = u8(s);
        v->num_inputs = u8(s);
        v->writes_z = u8(s);
        v->writes_z_from_fep = u8(s);
        v->disable_ez = u8(s);
        v->uses_center_w = u8(s);
        v->uses_implicit_point_line_varyings = u8(s);
        v->lock_scoreboard_on_first_thrsw = u8(s);
        skip(s, 1 + 3 + 64);             /* force_per_sample_msaa, pad, input_slots */
        for (j = 0; j < 3; j++)
            v->flat_shade_flags[j] = u32(s);
        for (j = 0; j < 3; j++)
            v->noperspective_flags[j] = u32(s);
        for (j = 0; j < 3; j++)
            v->centroid_flags[j] = u32(s);
    } else {
        return "variant of an unknown stage";
    }
    return 0;
}

static BOOL recipe_ok(ULONG t)
{
    return t <= OGLES2_U_VIEWPORT_Z_SCALE || t == OGLES2_U_TMU_CONFIG_P0 || t == OGLES2_U_TMU_CONFIG_P1 ||
           t == OGLES2_U_UBO_ADDR ||
           t == OGLES2_U_LINE_WIDTH || t == OGLES2_U_AA_LINE_WIDTH ||
           t == OGLES2_U_SPILL_OFFSET || t == OGLES2_U_SPILL_SIZE_PER_THREAD ||
           (t >= OGLES2_U_BLEND_CONSTANT_R && t <= OGLES2_U_BLEND_CONSTANT_R + 3) || t == OGLES2_U_CLIP_Z_COEFF;
}

/* The recipe's types: each one implemented (plan 8.6), UBO 0 only (the
 * stage's own constant buffer, for indirectly indexed uniform arrays; others
 * are GLES3 uniform blocks); what the draw path wants to know of it. 0, or
 * why the file is refused (*bad: the value). */
const char *ogles2_v3d_variant_recipe(struct OGLES2Variant *v, BOOL *clip_z, ULONG *bad)
{
    const ULONG *rt = v->rtype, *rd = v->rdata;
    ULONG j;
    for (j = 0; j < v->nrecipe; j++)
        if (!recipe_ok(rt[j])) {
            *bad = rt[j];
            return "uniform recipe type not implemented yet:";
        }
    for (j = 0; j < v->nrecipe; j++) {
        if (rt[j] == OGLES2_U_UBO_ADDR && (rd[j] >> 24) != 0) {
            *bad = rd[j] >> 24;
            return "a uniform block other than the default one";
        }
        if (rt[j] == OGLES2_U_UBO_ADDR)
            v->uses_ubo = 1;
        if (rt[j] == OGLES2_U_CLIP_Z_COEFF)
            *clip_z = TRUE;
    }
    v->no_textures = 1;
    for (j = 0; j < v->nrecipe; j++)
        if (rt[j] == OGLES2_U_TMU_CONFIG_P0 || rt[j] == OGLES2_U_TMU_CONFIG_P1 ||
            rt[j] == OGLES2_U_UBO_ADDR || rt[j] == OGLES2_U_SPILL_OFFSET ||
            (rt[j] >= OGLES2_U_BLEND_CONSTANT_R && rt[j] <= OGLES2_U_BLEND_CONSTANT_R + 3))
            v->no_textures = 0; /* nor the blend colour or the spill area: not in draw.c's key */
    return 0;
}

/* The variant for a draw: the stage's variant whose primitive key matches
 * (fragment: points, lines or triangles, and the render targets it writes,
 * `cbufs`; vertex and binning: a point size written per vertex, for points,
 * when the program has such a variant). A fragment shader with no variant
 * for those targets: the one for target 0 (EXT_draw_buffers: v3dglslc
 * makes the others only for programs that write gl_FragData[1] or up). */
const struct OGLES2Variant *ogles2_v3d_variant_choose(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                      GLenum mode)
{
    const struct OGLES2Variant *v, *any = 0, *rt0 = 0;
    BOOL points = mode == GL_POINTS;
    BOOL lines = mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP;
    UWORD i;
    for (i = 0; i < im->nvariants; i++) {
        v = &im->variants[i];
        if (v->stage != stage)
            continue;
        if (stage == OGLES2_VARIANT_FS) {
            if (v->is_points == points && v->is_lines == lines) {
                UBYTE vc = v->cbufs ? v->cbufs : 1;
                if (vc == cbufs)
                    return v;
                if (vc == 1 && !rt0)
                    rt0 = v;
            }
        } else {
            if (v->per_vertex_point_size == (points ? 1 : 0))
                return v;
        }
        if (!any)
            any = v;
    }
    return rt0 ? rt0 : any; /* a miss: the closest there is (plan 7.8); draw.c logs it */
}
