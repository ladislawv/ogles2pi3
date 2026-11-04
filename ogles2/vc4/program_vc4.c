/*
 * The VC4 backend's program variants (ogles2/backend.h, docs/PI3_PLAN.md
 * §4.3, §5.1, P5): a .vc4p VC4_VARIANT's key and prog data (v3dp/FORMAT.md
 * 1.1), its uniform recipe's types (Mesa vc4's enum quniform_contents at the
 * validated commit) and, for now, the variant a draw uses by stage and
 * primitive alone; the choice by the whole key (ogles2/vc4/key_vc4.c) comes
 * with the draw.
 *
 * MIT licence (LICENSE).
 */
#include <exec/types.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "../v3dp_private.h"
#include "backend_vc4.h"

typedef char vc4_prog_fits[sizeof(struct vc4_prog) <= sizeof(((struct OGLES2Variant *)0)->be) ? 1 : -1];

#define SAMPLER_KEY_BYTES (16 * 16)
#define FS_KEY_BYTES (SAMPLER_KEY_BYTES + 36) /* FORMAT.md 1.1: 292 */

/* A pair list: u32 count, (slot, swizzle) bytes, zero padding to 4. */
static const UBYTE *inputs(struct ogles2_rd *s, UBYTE *n)
{
    ULONG c = u32(s);
    const UBYTE *p = s->p;
    if (c > 64 || !need(s, (2 * c + 3) & ~3UL)) {
        s->bad = TRUE;
        *n = 0;
        return 0;
    }
    s->p += (2 * c + 3) & ~3UL;
    *n = (UBYTE)c;
    return p;
}

/* The section after its stage. 0, or why the file is refused. */
const char *ogles2_vc4_variant_read(struct ogles2_rd *s, struct OGLES2Variant *v)
{
    struct vc4_prog *pd = VC4_PROG(v);
    const UBYTE *key = s->p;
    UBYTE n, j;
    if (v->stage == OGLES2_VARIANT_FS) {
        if (!need(s, FS_KEY_BYTES))
            return "truncated VC4_VARIANT key";
        v->is_points = key[SAMPLER_KEY_BYTES + 8];
        v->is_lines = key[SAMPLER_KEY_BYTES + 9];
        v->cbufs = (UBYTE)(1u << key[SAMPLER_KEY_BYTES + 33]); /* draw_buffer (patch 0016) */
        s->p += FS_KEY_BYTES;
    } else if (v->stage == OGLES2_VARIANT_VS || v->stage == OGLES2_VARIANT_CS) {
        if (!need(s, SAMPLER_KEY_BYTES + 32 + 4))
            return "truncated VC4_VARIANT key";
        v->per_vertex_point_size = key[SAMPLER_KEY_BYTES + 32 + 1];
        s->p += SAMPLER_KEY_BYTES + 32 + 4;
        (void)inputs(s, &n); /* the key's fs_inputs: the fragment variant's, which the file pairs */
    } else {
        return "unknown VC4_VARIANT stage";
    }
    v->key = key;
    v->key_len = (UWORD)(s->p - key);
    pd->fs_threaded = u8(s);
    pd->num_inputs = u8(s);
    pd->vattrs_live = u8(s);
    pd->disable_early_z = u8(s);
    for (j = 0; j < 9; j++)
        pd->vattr_offsets[j] = u8(s);
    skip(s, 3);
    pd->color_inputs = u32(s);
    pd->num_texture_samples = u32(s);
    pd->fs_inputs = inputs(s, &pd->nfs_inputs);
    v->threads = pd->fs_threaded ? 2 : 1;
    v->num_inputs = pd->num_inputs;
    return s->bad ? "truncated VC4_VARIANT prog data" : 0;
}

/* The recipe's types, vc4's (FORMAT.md 1.1): 0 CONSTANT and 1 UNIFORM as on
 * V3D; the others the draw fills. */
#define VC4_U_TEXTURE_CONFIG_P0 6
#define VC4_U_TEXTURE_MSAA_ADDR 10
#define VC4_U_UBO0_ADDR 11
#define VC4_U_UBO1_ADDR 12
#define VC4_U_LAST 24 /* UNIFORMS_ADDRESS */

const char *ogles2_vc4_variant_recipe(struct OGLES2Variant *v, BOOL *clip_z, ULONG *bad)
{
    UWORD j;
    BOOL tex = FALSE;
    *clip_z = FALSE;
    v->uses_ubo = 0;
    for (j = 0; j < v->nrecipe; j++) {
        ULONG t = v->rtype[j];
        if (t > VC4_U_LAST) {
            *bad = t;
            return "unknown uniform recipe type:";
        }
        if (t == VC4_U_UBO0_ADDR || t == VC4_U_UBO1_ADDR)
            v->uses_ubo = 1;
        if (t >= VC4_U_TEXTURE_CONFIG_P0 && t <= VC4_U_TEXTURE_MSAA_ADDR)
            tex = TRUE;
    }
    v->no_textures = !tex && !v->uses_ubo;
    return 0;
}

/* The first variant of the stage for the primitive class (the key's other
 * parts: with the draw). */
const struct OGLES2Variant *ogles2_vc4_variant_choose(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                      GLenum mode)
{
    BOOL points = mode == GL_POINTS, lines = mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP;
    UWORD i;
    (void)cbufs;
    for (i = 0; i < im->nvariants; i++) {
        const struct OGLES2Variant *v = &im->variants[i];
        if (v->stage != stage)
            continue;
        if (stage == OGLES2_VARIANT_FS ? v->is_points == points && v->is_lines == lines
                                       : v->per_vertex_point_size == points)
            return v;
    }
    return 0;
}
