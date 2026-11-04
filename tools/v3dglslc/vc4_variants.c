/*
 * v3dglslc -g 21 (docs/PI3_PLAN.md §4.1, P4): receives every variant Mesa's
 * vc4 driver compiles (patches/0015-vc4-compiled-variant-hook.patch) and
 * prints what a VC4 backend needs to run it: the stage, threading, the
 * fragment shader's inputs, the vertex attribute layout, the uniform recipe
 * (enum quniform_contents and its data, in the order the code reads them)
 * and the QPU code. Kept apart from v3dglslc.c, whose V3D 4.2 headers would
 * meet vc4's here. The .v3dp writer for VC4 (format 1.1, §4.3) comes later;
 * until then this listing is what tests/pi3 takes its shaders from.
 *
 * MIT licence (LICENSE: tools/v3dglslc).
 */
#include <stdio.h>
#include "vc4_context.h"
#include "vc4_qir.h"

extern void (*vc4_compiled_variant_hook)(enum qstage stage, const struct vc4_key *key, uint32_t key_size,
                                         const struct vc4_compiled_shader *shader,
                                         const uint64_t *qpu_insts, uint32_t qpu_inst_count);

void vc4_variants_install(FILE *f);

static FILE *out;

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
    (void)key;
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
