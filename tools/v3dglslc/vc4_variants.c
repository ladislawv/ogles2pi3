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

/* patch 0017: a variant for a key the caller built */
extern struct vc4_context *vc4_compiled_variant_context;
struct vc4_compiled_shader *vc4_compile_variant(struct vc4_context *vc4, enum qstage stage, struct vc4_key *key);

void vc4_variants_install(FILE *f);
void vc4_variants_capture(int on);
int vc4_variants_compile_keys(const char *path);
int vc4_variants_compile_key_buffer(const void *keys, size_t size);

static FILE *out;  /* the listing, or NULL */
static int capture; /* serialize for the writer */
/* From the variants the reference draw compiled: the program's shaders (a
 * key's shader_state) and the last fragment variant's inputs (a vertex
 * key's fs_inputs), for vc4_variants_compile_keys. */
static struct vc4_uncompiled_shader *last_fs_state, *last_vs_state;
static const struct vc4_fs_inputs *last_fs_inputs;

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
        /* patch 0019: the texture-parameter emulations (FORMAT.md 1.1) */
        s_u8(&b, key->tex[i].lod_bias | key->tex[i].lod_clamp << 1);
        s_u8(&b, key->tex[i].aniso);
        s_u8(&b, key->tex[i].half_signed); /* patch 0022 */
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
        s_u8(&b, k->clip_halfz); /* patch 0020 */
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
    if (stage == QSTAGE_FRAG) {
        last_fs_state = key->shader_state;
        last_fs_inputs = sh->fs_inputs;
    } else
        last_vs_state = key->shader_state;
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

/* ---- keys from a file (docs/PI3_PLAN.md §4.2, PD7) ----
 *
 * A key file is a sequence of records, each a u32 stage (4 fragment, 0
 * vertex) and that stage's key as a VC4_VARIANT section holds it
 * (v3dp/FORMAT.md 1.1: the samplers, then the stage's fields; a vertex key's
 * fs_inputs are read and replaced by those of the fragment variant compiled
 * last). A fragment record compiles that fragment variant; a vertex record
 * the vertex and coordinate variants for it. All little-endian. They go
 * through the hook like the reference draw's, after it, for its program. */

struct rd {
    const unsigned char *p;
    size_t len, at;
    int bad;
};

static unsigned r_u8(struct rd *r)
{
    if (r->at + 1 > r->len) {
        r->bad = 1;
        return 0;
    }
    return r->p[r->at++];
}

static uint32_t r_u32(struct rd *r)
{
    uint32_t v = r_u8(r);
    v |= r_u8(r) << 8;
    v |= r_u8(r) << 16;
    v |= (uint32_t)r_u8(r) << 24;
    return v;
}

static void r_key_common(struct rd *r, struct vc4_key *k)
{
    int i;
    for (i = 0; i < VC4_MAX_TEXTURE_SAMPLERS; i++) {
        k->tex[i].format = r_u32(r);
        k->tex[i].swizzle[0] = r_u8(r);
        k->tex[i].swizzle[1] = r_u8(r);
        k->tex[i].swizzle[2] = r_u8(r);
        k->tex[i].swizzle[3] = r_u8(r);
        k->tex[i].compare_mode = r_u8(r);
        k->tex[i].compare_func = r_u8(r);
        k->tex[i].wrap_s = r_u8(r);
        k->tex[i].wrap_t = r_u8(r);
        k->tex[i].force_first_level = r_u8(r);
        {
            uint8_t f = r_u8(r); /* patch 0019 */
            k->tex[i].lod_bias = f & 1;
            k->tex[i].lod_clamp = (f >> 1) & 1;
            k->tex[i].aniso = r_u8(r) & 7;
        }
        k->tex[i].half_signed = r_u8(r) & 1; /* patch 0022 */
    }
}

static void r_fs_key(struct rd *r, struct vc4_fs_key *k)
{
    r_key_common(r, &k->base);
    k->color_format = r_u32(r);
    k->depth_enabled = r_u8(r);
    k->stencil_enabled = r_u8(r);
    k->stencil_twoside = r_u8(r);
    k->stencil_full_writemasks = r_u8(r);
    k->is_points = r_u8(r);
    k->is_lines = r_u8(r);
    k->point_coord_upper_left = r_u8(r);
    k->msaa = r_u8(r);
    k->sample_coverage = r_u8(r);
    k->sample_alpha_to_coverage = r_u8(r);
    k->sample_alpha_to_one = r_u8(r);
    k->logicop_func = r_u8(r);
    k->point_sprite_mask = r_u32(r);
    k->ubo_1_size = r_u32(r);
    k->ucp_enables = r_u8(r);
    k->blend.blend_enable = r_u8(r);
    k->blend.rgb_func = r_u8(r);
    k->blend.rgb_src_factor = r_u8(r);
    k->blend.rgb_dst_factor = r_u8(r);
    k->blend.alpha_func = r_u8(r);
    k->blend.alpha_src_factor = r_u8(r);
    k->blend.alpha_dst_factor = r_u8(r);
    k->blend.colormask = r_u8(r);
    k->draw_buffer = r_u8(r); /* patch 0016 */
    r_u8(r);
    r_u8(r);
}

static void r_vs_key(struct rd *r, struct vc4_vs_key *k)
{
    uint32_t i, n;
    r_key_common(r, &k->base);
    for (i = 0; i < 8; i++)
        k->attr_formats[i] = r_u32(r);
    k->is_coord = r_u8(r);
    k->per_vertex_point_size = r_u8(r);
    k->clip_halfz = r_u8(r); /* patch 0020 */
    r_u8(r);
    n = r_u32(r); /* fs_inputs: replaced */
    for (i = 0; i < n * 2 && !r->bad; i++)
        r_u8(r);
    while (r->at % 4 && !r->bad)
        r_u8(r);
}

/* The records of keys[0, size): the number compiled, or -1 (the reason
 * printed). */
int vc4_variants_compile_key_buffer(const void *keys, size_t size)
{
    struct rd r = { 0 };
    int records = 0;
    if (!vc4_compiled_variant_context || !last_fs_state || !last_vs_state) {
        fprintf(stderr, "v3dglslc: vc4 keys: the reference draw compiled no variants to build on\n");
        return -1;
    }
    r.p = keys;
    r.len = size;
    while (r.at < r.len) {
        uint32_t stage = r_u32(&r);
        if (stage == 4) {
            struct vc4_fs_key k;
            struct vc4_compiled_shader *sh;
            memset(&k, 0, sizeof(k));
            r_fs_key(&r, &k);
            if (r.bad)
                break;
            k.base.shader_state = last_fs_state;
            sh = vc4_compile_variant(vc4_compiled_variant_context, QSTAGE_FRAG, &k.base);
            if (!sh || sh->failed) {
                fprintf(stderr, "v3dglslc: vc4 keys: record %d (fragment) did not compile\n", records + 1);
                return -1;
            }
            last_fs_inputs = sh->fs_inputs;
        } else if (stage == 0) {
            struct vc4_vs_key k;
            int coord;
            memset(&k, 0, sizeof(k));
            r_vs_key(&r, &k);
            if (r.bad)
                break;
            for (coord = 0; coord < 2; coord++) {
                struct vc4_compiled_shader *sh;
                k.base.shader_state = last_vs_state;
                k.fs_inputs = coord ? NULL : last_fs_inputs; /* as vc4_update_compiled_vs */
                k.is_coord = coord;
                sh = vc4_compile_variant(vc4_compiled_variant_context, coord ? QSTAGE_COORD : QSTAGE_VERT, &k.base);
                if (!sh || sh->failed) {
                    fprintf(stderr, "v3dglslc: vc4 keys: record %d (%s) did not compile\n", records + 1,
                            coord ? "coordinate" : "vertex");
                    return -1;
                }
            }
        } else {
            fprintf(stderr, "v3dglslc: vc4 keys: record %d: stage %u is neither 4 nor 0\n", records + 1,
                    (unsigned)stage);
            return -1;
        }
        records++;
    }
    if (r.bad) {
        fprintf(stderr, "v3dglslc: vc4 keys: record %d is cut short\n", records + 1);
        return -1;
    }
    return records;
}

/* The same from a file (-k). */
int vc4_variants_compile_keys(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long size;
    int records;
    if (!f) {
        fprintf(stderr, "v3dglslc: %s: cannot open\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        fprintf(stderr, "v3dglslc: %s: cannot read\n", path);
        return -1;
    }
    fclose(f);
    records = vc4_variants_compile_key_buffer(buf, (size_t)size);
    free(buf);
    return records;
}
