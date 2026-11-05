/*
 * The VC4 backend's program variants (ogles2/backend.h, docs/PI3_PLAN.md
 * §4.3, §5.1, P5): a .vc4p VC4_VARIANT's key and prog data (v3dp/FORMAT.md
 * 1.1), its uniform recipe's types (Mesa vc4's enum quniform_contents at the
 * validated commit), the front end's variant by stage and primitive, and
 * the draw's choice by the whole key (ogles2/vc4/key_vc4.c builds it).
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
    const UBYTE *key = s->p, *key_inputs = 0;
    UBYTE n = 0, j;
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
        key_inputs = inputs(s, &n); /* the fragment shader's inputs it writes (FORMAT.md 1.1) */
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
    if (v->stage != OGLES2_VARIANT_FS) { /* its own list is empty: the key's is what pairs it */
        pd->fs_inputs = key_inputs;
        pd->nfs_inputs = n;
    }
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
/* patch 0019's five after UNIFORMS_ADDRESS (24): LOD bias, min and max LOD,
 * width, height (backend_vc4.h; draw_vc4.c fills them). At 24, every
 * variant with an emulated texture parameter was refused at load, and its
 * draws took the closest variant (P5 run 1, m6-6). */
#define VC4_U_LAST 29 /* TEXTURE_HEIGHT */

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

/* The first variant of the stage for the primitive class, else the
 * stage's first (the draw chooses by the whole key: ogles2_vc4_variant_find). */
const struct OGLES2Variant *ogles2_vc4_variant_choose(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                      GLenum mode)
{
    BOOL points = mode == GL_POINTS, lines = mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP;
    const struct OGLES2Variant *any = 0;
    UWORD i;
    (void)cbufs;
    for (i = 0; i < im->nvariants; i++) {
        const struct OGLES2Variant *v = &im->variants[i];
        if (v->stage != stage)
            continue;
        if (!any)
            any = v;
        if (stage == OGLES2_VARIANT_FS ? v->is_points == points && v->is_lines == lines
                                       : v->per_vertex_point_size == points)
            return v;
    }
    return any;
}

/* Key bytes alike, each sampler entry's comparison function and wraps
 * (bytes 9 to 11) aside, except that a wrap of 6 (MIRROR_CLAMP_TO_EDGE: the
 * shader's absolute value, patch 0019) must be in both or neither (P5 run
 * 1: a mirror-clamp draw took the plain variant). */
static BOOL key_same(const UBYTE *a, const UBYTE *b, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++) {
        if (i < SAMPLER_KEY_BYTES && (i & 15) >= 9 && (i & 15) <= 11) {
            if ((i & 15) != 9 && (a[i] == 6) != (b[i] == 6))
                return FALSE;
            continue;
        }
        if (a[i] != b[i])
            return FALSE;
    }
    return TRUE;
}

static BOOL inputs_same(const struct vc4_prog *a, const struct vc4_prog *b)
{
    UBYTE k;
    if (a->nfs_inputs != b->nfs_inputs)
        return FALSE;
    for (k = 0; k < 2 * a->nfs_inputs; k++)
        if (a->fs_inputs[k] != b->fs_inputs[k])
            return FALSE;
    return TRUE;
}

#define VS_KEY_PSIZE (SAMPLER_KEY_BYTES + 32 + 1)

static const struct OGLES2Variant *find(const struct OGLES2Image *im, UBYTE stage, const UBYTE *key,
                                        const struct OGLES2Variant *fs, int psize)
{
    UWORD i;
    for (i = 0; i < im->nvariants; i++) {
        const struct OGLES2Variant *v = &im->variants[i];
        if (v->stage != stage || !v->key)
            continue;
        if (stage == OGLES2_VARIANT_FS) {
            if (v->key_len >= FS_KEY_BYTES && key_same(v->key, key, FS_KEY_BYTES))
                return v;
            continue;
        }
        /* attribute formats and the samplers; is_coord is the stage's */
        /* and clip_halfz (patch 0020, the byte after gl_PointSize's: P5 run 1,
         * EXT_clip_control's ZERO_TO_ONE took the plain variant) */
        if (v->key_len < VS_KEY_PSIZE + 2 || !key_same(v->key, key, SAMPLER_KEY_BYTES + 32) ||
            v->key[VS_KEY_PSIZE] != psize || v->key[VS_KEY_PSIZE + 1] != key[VS_KEY_PSIZE + 1])
            continue;
        if (stage == OGLES2_VARIANT_VS && fs && !inputs_same(VC4_PROG_C(v), VC4_PROG_C(fs)))
            continue;
        return v;
    }
    return 0;
}

const struct OGLES2Variant *ogles2_vc4_variant_find(const struct OGLES2Image *im, UBYTE stage, const UBYTE *key,
                                                    const struct OGLES2Variant *fs, BOOL points)
{
    const struct OGLES2Variant *v;
    if (stage == OGLES2_VARIANT_FS)
        return find(im, stage, key, 0, 0);
    /* gl_PointSize: a points draw's key asks for the per-vertex size (GLES2
     * points take it from the shader, as Mesa's key: without it the variant
     * drops the shader's size, P5 run 1's 1-pixel particles); points 2: the
     * other one too, the draw's last resort (draw_vc4.c, choose) */
    if ((v = find(im, stage, key, fs, key[VS_KEY_PSIZE])) != 0 || points != 2)
        return v;
    return find(im, stage, key, fs, !key[VS_KEY_PSIZE]);
}
