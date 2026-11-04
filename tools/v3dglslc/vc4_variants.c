/*
 * v3dglslc -g 21 (docs/PI3_PLAN.md §4.1, P4): receives every variant Mesa's
 * vc4 driver compiles (patches/0015-vc4-compiled-variant-hook.patch) and
 * prints what a VC4 backend needs to run it: the stage, threading, the
 * fragment shader's inputs, the vertex attribute layout, the uniform recipe
 * (enum quniform_contents and its data, in the order the code reads them)
 * and the QPU code; and with a capture on (vc4_variants_capture), serializes
 * each as a VC4_VARIANT section for v3dp_writer.c (v3dp/FORMAT.md 1.1, field
 * by field, little-endian). Kept apart from v3dglslc.c, whose V3D 4.2 headers
 * would meet vc4's here.
 *
 * MIT licence (LICENSE: tools/v3dglslc).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vc4_context.h"
#include "vc4_qir.h"
#include "v3dp_writer.h"

/* v3dp/FORMAT.md 1.1 fixes these sizes. */
static_assert(VC4_MAX_TEXTURE_SAMPLERS == 16, "samplers in the key");
static_assert(QUNIFORM_UNIFORMS_ADDRESS == 24, "recipe types as FORMAT.md lists them");

extern void (*vc4_compiled_variant_hook)(enum qstage stage, const struct vc4_key *key, uint32_t key_size,
                                         const struct vc4_compiled_shader *shader,
                                         const uint64_t *qpu_insts, uint32_t qpu_inst_count);

void vc4_variants_install(FILE *f);
void vc4_variants_capture(int on);

static FILE *out;  /* the listing, or NULL */
static int capture; /* serialize for the writer */

/* ---- a growing little-endian byte buffer ---- */

struct sbuf {
    unsigned char *p;
    size_t len, cap;
    int failed;
};

static void s_u8(struct sbuf *b, unsigned v)
{
    if (b->failed)
        return;
    if (b->len == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        unsigned char *p = realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    b->p[b->len++] = (unsigned char)v;
}

static void s_u32(struct sbuf *b, uint32_t v)
{
    s_u8(b, v);
    s_u8(b, v >> 8);
    s_u8(b, v >> 16);
    s_u8(b, v >> 24);
}

static void s_u64(struct sbuf *b, uint64_t v)
{
    s_u32(b, (uint32_t)v);
    s_u32(b, (uint32_t)(v >> 32));
}

static void s_pad4(struct sbuf *b)
{
    while (b->len % 4)
        s_u8(b, 0);
}

static void s_inputs(struct sbuf *b, const struct vc4_fs_inputs *in)
{
    uint32_t i, n = in ? in->num_inputs : 0;
    s_u32(b, n);
    for (i = 0; i < n; i++) {
        s_u8(b, in->input_slots[i].slot);
        s_u8(b, in->input_slots[i].swizzle);
    }
    s_pad4(b);
}

/* FORMAT.md 1.1, VC4_VARIANT, in its order. */
static void serialize(enum qstage stage, const struct vc4_key *key, uint32_t key_size,
                      const struct vc4_compiled_shader *sh, const uint64_t *qpu, uint32_t n)
{
    struct sbuf b = { 0 };
    uint32_t i;

    if (stage == QSTAGE_FRAG ? key_size != sizeof(struct vc4_fs_key) : key_size != sizeof(struct vc4_vs_key)) {
        fprintf(stderr, "vc4_variants: unexpected key size %u\n", (unsigned)key_size);
        abort();
    }
    s_u32(&b, stage == QSTAGE_FRAG ? 4 : stage == QSTAGE_COORD ? 1 : 0);
    for (i = 0; i < VC4_MAX_TEXTURE_SAMPLERS; i++) {
        s_u32(&b, key->tex[i].format);
        s_u8(&b, key->tex[i].swizzle[0]);
        s_u8(&b, key->tex[i].swizzle[1]);
        s_u8(&b, key->tex[i].swizzle[2]);
        s_u8(&b, key->tex[i].swizzle[3]);
        s_u8(&b, key->tex[i].compare_mode);
        s_u8(&b, key->tex[i].compare_func);
        s_u8(&b, key->tex[i].wrap_s);
        s_u8(&b, key->tex[i].wrap_t);
        s_u8(&b, key->tex[i].force_first_level);
        s_u8(&b, 0);
        s_u8(&b, 0);
        s_u8(&b, 0);
    }
    if (stage == QSTAGE_FRAG) {
        const struct vc4_fs_key *k = (const struct vc4_fs_key *)key;
        s_u32(&b, k->color_format);
        s_u8(&b, k->depth_enabled);
        s_u8(&b, k->stencil_enabled);
        s_u8(&b, k->stencil_twoside);
        s_u8(&b, k->stencil_full_writemasks);
        s_u8(&b, k->is_points);
        s_u8(&b, k->is_lines);
        s_u8(&b, k->point_coord_upper_left);
        s_u8(&b, k->msaa);
        s_u8(&b, k->sample_coverage);
        s_u8(&b, k->sample_alpha_to_coverage);
        s_u8(&b, k->sample_alpha_to_one);
        s_u8(&b, k->logicop_func);
        s_u32(&b, k->point_sprite_mask);
        s_u32(&b, k->ubo_1_size);
        s_u8(&b, k->ucp_enables);
        s_u8(&b, k->blend.blend_enable);
        s_u8(&b, k->blend.rgb_func);
        s_u8(&b, k->blend.rgb_src_factor);
        s_u8(&b, k->blend.rgb_dst_factor);
        s_u8(&b, k->blend.alpha_func);
        s_u8(&b, k->blend.alpha_src_factor);
        s_u8(&b, k->blend.alpha_dst_factor);
        s_u8(&b, k->blend.colormask);
        s_u8(&b, k->draw_buffer); /* patch 0016 */
        s_u8(&b, 0);
        s_u8(&b, 0);
    } else {
        const struct vc4_vs_key *k = (const struct vc4_vs_key *)key;
        for (i = 0; i < 8; i++)
            s_u32(&b, k->attr_formats[i]);
        s_u8(&b, k->is_coord);
        s_u8(&b, k->per_vertex_point_size);
        s_u8(&b, 0);
        s_u8(&b, 0);
        s_inputs(&b, k->fs_inputs);
    }
    /* prog data */
    s_u8(&b, sh->fs_threaded);
    s_u8(&b, sh->num_inputs);
    s_u8(&b, sh->vattrs_live);
    s_u8(&b, sh->disable_early_z);
    for (i = 0; i < 9; i++)
        s_u8(&b, sh->vattr_offsets[i]);
    s_u8(&b, 0);
    s_u8(&b, 0);
    s_u8(&b, 0);
    s_u32(&b, sh->color_inputs);
    s_u32(&b, sh->uniforms.num_texture_samples);
    s_inputs(&b, stage == QSTAGE_FRAG ? sh->fs_inputs : NULL);
    /* recipe, code */
    s_u32(&b, sh->uniforms.count);
    for (i = 0; i < sh->uniforms.count; i++)
        s_u32(&b, sh->uniforms.contents[i]);
    for (i = 0; i < sh->uniforms.count; i++)
        s_u32(&b, sh->uniforms.data[i]);
    s_u32(&b, n * 8);
    for (i = 0; i < n; i++)
        s_u64(&b, qpu[i]);
    if (b.failed) {
        fprintf(stderr, "vc4_variants: out of memory\n");
        abort();
    }
    v3dp_add_variant(b.p, b.len);
    free(b.p);
}

static const char *uniform_name(enum quniform_contents c)
{
    static const char *const names[] = {
        [QUNIFORM_CONSTANT] = "CONSTANT",
        [QUNIFORM_UNIFORM] = "UNIFORM",
        [QUNIFORM_VIEWPORT_X_SCALE] = "VIEWPORT_X_SCALE",
        [QUNIFORM_VIEWPORT_Y_SCALE] = "VIEWPORT_Y_SCALE",
        [QUNIFORM_VIEWPORT_Z_OFFSET] = "VIEWPORT_Z_OFFSET",
        [QUNIFORM_VIEWPORT_Z_SCALE] = "VIEWPORT_Z_SCALE",
        [QUNIFORM_TEXTURE_CONFIG_P0] = "TEXTURE_CONFIG_P0",
        [QUNIFORM_TEXTURE_CONFIG_P1] = "TEXTURE_CONFIG_P1",
        [QUNIFORM_TEXTURE_CONFIG_P2] = "TEXTURE_CONFIG_P2",
        [QUNIFORM_TEXTURE_FIRST_LEVEL] = "TEXTURE_FIRST_LEVEL",
        [QUNIFORM_TEXTURE_MSAA_ADDR] = "TEXTURE_MSAA_ADDR",
        [QUNIFORM_UBO0_ADDR] = "UBO0_ADDR",
        [QUNIFORM_UBO1_ADDR] = "UBO1_ADDR",
        [QUNIFORM_TEXRECT_SCALE_X] = "TEXRECT_SCALE_X",
        [QUNIFORM_TEXRECT_SCALE_Y] = "TEXRECT_SCALE_Y",
        [QUNIFORM_TEXTURE_BORDER_COLOR] = "TEXTURE_BORDER_COLOR",
        [QUNIFORM_BLEND_CONST_COLOR_X] = "BLEND_CONST_COLOR_X",
        [QUNIFORM_BLEND_CONST_COLOR_Y] = "BLEND_CONST_COLOR_Y",
        [QUNIFORM_BLEND_CONST_COLOR_Z] = "BLEND_CONST_COLOR_Z",
        [QUNIFORM_BLEND_CONST_COLOR_W] = "BLEND_CONST_COLOR_W",
        [QUNIFORM_BLEND_CONST_COLOR_RGBA] = "BLEND_CONST_COLOR_RGBA",
        [QUNIFORM_BLEND_CONST_COLOR_AAAA] = "BLEND_CONST_COLOR_AAAA",
        [QUNIFORM_STENCIL] = "STENCIL",
        [QUNIFORM_SAMPLE_MASK] = "SAMPLE_MASK",
        [QUNIFORM_UNIFORMS_ADDRESS] = "UNIFORMS_ADDRESS",
    };
    return (unsigned)c < sizeof(names) / sizeof(names[0]) && names[c] ? names[c] : "?";
}

static void variant(enum qstage stage, const struct vc4_key *key, uint32_t key_size,
                    const struct vc4_compiled_shader *sh, const uint64_t *qpu, uint32_t n)
{
    uint32_t i;
    if (capture)
        serialize(stage, key, key_size, sh, qpu, n);
    if (!out)
        return;
    fprintf(out, "vc4-variant %s key-bytes %u threaded %d instructions %u\n",
            stage == QSTAGE_FRAG ? "fragment" : stage == QSTAGE_VERT ? "vertex" : "coordinate",
            (unsigned)key_size, sh->fs_threaded ? 1 : 0, (unsigned)n);
    if (stage == QSTAGE_FRAG) {
        fprintf(out, "  inputs %u (color mask 0x%x), disable-early-z %d:", sh->fs_inputs ? sh->fs_inputs->num_inputs : 0,
                (unsigned)sh->color_inputs, sh->disable_early_z ? 1 : 0);
        for (i = 0; sh->fs_inputs && i < sh->fs_inputs->num_inputs; i++)
            fprintf(out, " slot %u.%u", (unsigned)sh->fs_inputs->input_slots[i].slot,
                    (unsigned)sh->fs_inputs->input_slots[i].swizzle);
        fprintf(out, "\n");
    } else {
        fprintf(out, "  attributes live 0x%02x, offsets", (unsigned)sh->vattrs_live);
        for (i = 0; i < 9; i++)
            fprintf(out, " %u", (unsigned)sh->vattr_offsets[i]);
        fprintf(out, " (8: total), num_inputs %u\n", (unsigned)sh->num_inputs);
    }
    fprintf(out, "  uniforms %u, texture lookups %u:\n", (unsigned)sh->uniforms.count,
            (unsigned)sh->uniforms.num_texture_samples);
    for (i = 0; i < sh->uniforms.count; i++)
        fprintf(out, "    %2u %-22s 0x%08x\n", (unsigned)i, uniform_name(sh->uniforms.contents[i]),
                (unsigned)sh->uniforms.data[i]);
    fprintf(out, "  code:\n");
    for (i = 0; i < n; i++)
        fprintf(out, "    0x%016llx\n", (unsigned long long)qpu[i]);
}

void vc4_variants_install(FILE *f)
{
    out = f;
    vc4_compiled_variant_hook = variant;
}

void vc4_variants_capture(int on)
{
    capture = on;
}
