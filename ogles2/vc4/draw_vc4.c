/*
 * The VC4 backend's draw (ogles2/backend.h, docs/PI3_PLAN.md §5.1, P5), as
 * Mesa's vc4 builds one (vc4_draw.c, vc4_emit.c, vc4_uniforms.c) and as
 * tests/pi3/vc4core/vc4p.c rehearsed it on the Pi 3:
 *
 *  - the draw's shader keys (key_vc4.c) from the GL state, and the
 *    program's variants with those keys (wraps and the comparison function
 *    aside: §4.2): from its file, else from its keyed image (program.c:
 *    the program's .vc4k, compiled anew by the on-target compiler with the
 *    draw's keys added, which the draw waits for); a key no compile gives
 *    draws with the front end's choice, logged once a program;
 *  - each stage's uniform stream from its recipe (vc4's quniform types:
 *    blending, the colour mask and the stencil test are the fragment
 *    shader's on VC4, the TMU's configuration words too);
 *  - the vertex data: each enabled array's [first, first+count) in the
 *    GPU's byte order (VC4 converts the types in the shader: the key holds
 *    them), a disabled one's current value (16 bytes, stride 0), then the
 *    GL shader record and one attribute record per vertex input slot;
 *  - into the binning list: the clip window, the configuration bits, depth
 *    offset, point size, line width, the viewport, the flat-shade flags,
 *    GL_SHADER_STATE and the primitives. VC4 counts vertices in 16 bits: a
 *    longer array draw is split as Mesa does (a new shader record for each
 *    65535-vertex part, the attributes moved on); indices must stay below
 *    65536 after the front end's rebasing.
 *
 * With several colour attachments (EXT_draw_buffers, job_vc4.c) all but the
 * vertex data and indices is done once a pass, into its binning list: each
 * pass's fragment variant writes its own output, and may read other varyings,
 * so its vertex and coordinate variants and its uniform streams are its own.
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "../draw_private.h"
#include "vc4core.h"
#include "vc4_os.h"
#include "v3d_types.h"
#include "key_vc4.h"
#include "backend_vc4.h"

#define MAX_VERTS 65535u

/* The context's draw state (ctx->be_draw). */
struct vc4_draw
{
    const struct OGLES2Image *missed[1]; /* a draw's keys missed: logged once a program */
    const struct OGLES2Image *told_big;  /* a draw VC4 cannot do whole: logged once a program */
};

static ULONG told_emu; /* samplers whose emulation was logged (sampler_states) */

/* Vertex j of a draw as the emitted vertex data numbers it: the element
 * rebased by the front end's minimum, or the array's j (glPolygonMode's
 * edges, ogles2_vc4_draw). */
static ULONG poly_index(const struct OGLES2Elements *el, ULONG j)
{
    if (!el)
        return j;
    return el->type == GL_UNSIGNED_INT     ? ((const ULONG *)el->indices)[j] - el->min
         : el->type == GL_UNSIGNED_SHORT ? (ULONG)(UWORD)(((const UWORD *)el->indices)[j] - el->min)
                                          : (ULONG)(UBYTE)(((const UBYTE *)el->indices)[j] - el->min);
}

static struct vc4_draw *draw_state(struct OGLES2Context *ctx)
{
    struct vc4_draw *d = ctx->be_draw;
    if (!d && (d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR)) != 0)
        ctx->be_draw = d;
    return d;
}

void ogles2_vc4_draw_free(struct OGLES2Context *ctx)
{
    if (ctx->be_draw)
        FreeVec(ctx->be_draw);
    ctx->be_draw = 0;
}

static ULONG fbits(GLfloat f)
{
    union { GLfloat f; ULONG u; } x;
    x.f = f;
    return x.u;
}

/* State buffer room: its CPU address and bus address, or NULL. */
static UBYTE *claim(struct vc4_ctx *c, ULONG size, ULONG align, ULONG *bus)
{
    uint32_t b;
    UBYTE *p = vc4_buf_alloc(&c->job[c->build].state, size, align, &b);
    *bus = b;
    return p;
}

static BOOL y_down(const struct OGLES2Context *ctx)
{
    return ctx->fb_flip ^ (ctx->state.clip_origin == GL_UPPER_LEFT_EXT);
}

/* ---- the keys ---- */

/* What the key and the stream need of each sampler, per stage ([0] vertex,
 * [1] fragment) and texture index. */
struct samplers
{
    struct vc4_key_sampler key[2][OGLES2_MAX_STAGE_SAMPLERS];
    ULONG p[2][OGLES2_MAX_STAGE_SAMPLERS][5]; /* P0, P1, P2; the S16F planes' stride (patch 0018);
                                               * the negative planes' P0 (patch 0022) */
    ULONG first[2][OGLES2_MAX_STAGE_SAMPLERS];
    GLfloat emu[2][OGLES2_MAX_STAGE_SAMPLERS][5]; /* patch 0019's uniforms (ogles2_vc4_sampler) */
    UBYTE n[2];
};

static BOOL sampler_states(struct OGLES2Context *ctx, struct OGLES2Program *p, struct samplers *sm)
{
    const struct OGLES2Image *im = p->image;
    UWORD u, k, n = list_samplers(p);
    int st;
    for (k = 0; k < n; k++) {
        const struct OGLES2ImageUniform *un;
        u = p->nsamplers == 0xfe ? k : p->samplers[k];
        un = &im->uniforms[u];
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER))
            continue;
        for (st = 0; st < 2; st++) {
            UWORD e, ne = un->array ? un->array : 1;
            for (e = 0; e < ne; e++) {
                ULONG idx = un->sampler[st] + e;
                struct vc4_key_sampler *ks;
                if (un->sampler[st] >= OGLES2_MAX_STAGE_SAMPLERS || idx >= OGLES2_MAX_STAGE_SAMPLERS)
                    break;
                ks = &sm->key[st][idx];
                if (!ogles2_vc4_sampler(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type), sm->p[st][idx], ks,
                                        &sm->first[st][idx], sm->emu[st][idx]))
                    return FALSE;
                if ((ks->force_first || ks->lod_bias || ks->lod_clamp || ks->aniso) && told_emu < 24) {
                    /* P5 run 1's emulations, as drawn: the first few */
                    const GLfloat *e = sm->emu[st][idx];
                    told_emu++;
                    ogles2_log("draw: sampler %ld emulated: force_first %ld bias %ld clamp %ld aniso %ld, first %ld, "
                               "P0 0x%08lx; bias %ld/100, LOD %ld/100..%ld/100, %ldx%ld",
                               (ULONG)idx, (ULONG)ks->force_first, (ULONG)ks->lod_bias, (ULONG)ks->lod_clamp,
                               (ULONG)ks->aniso, sm->first[st][idx], sm->p[st][idx][0], (LONG)(e[0] * 100.0f),
                               (LONG)(e[1] * 100.0f), (LONG)(e[2] * 100.0f), (LONG)e[3], (LONG)e[4]);
                }
                if (idx + 1 > sm->n[st])
                    sm->n[st] = (UBYTE)(idx + 1);
            }
        }
    }
    return TRUE;
}

/* ---- uniform streams ---- */

struct draw_info
{
    const struct OGLES2Program *p;
    const struct samplers *sm;
    ULONG stencil[3];
    ULONG ubo[2];          /* each stage's constant buffer copy (bus), if read */
    UBYTE color_format;
    float vp_xs, vp_ys, vp_zs, vp_zo;
};

/* One stage's stream (one word more: the QPU reads ahead); its bus address, 0. */
static ULONG write_uniforms(struct OGLES2Context *ctx, struct vc4_ctx *c, const struct draw_info *di,
                            const struct OGLES2Variant *v)
{
    int si = v->stage == OGLES2_VARIANT_FS ? 1 : 0;
    const struct OGLES2State *s = &ctx->state;
    const ULONG *cb = di->p->cbuf[si];
    UWORD ncb = di->p->image->cbuf_words[si];
    ULONG bus, k;
    UBYTE *u = claim(c, (v->nrecipe + 1) * 4UL, 4, &bus);
    if (!u)
        return 0;
    ctx->n_uwords += v->nrecipe;
    for (k = 0; k < v->nrecipe; k++) {
        ULONG d = v->rdata[k], w, x = d & (OGLES2_MAX_STAGE_SAMPLERS - 1);
        switch (v->rtype[k]) {
        case VC4_U_CONSTANT: w = d; break;
        case VC4_U_UNIFORM: w = d < ncb ? cb[d] : 0; break;
        case VC4_U_VIEWPORT_X_SCALE: w = fbits(di->vp_xs); break;
        case VC4_U_VIEWPORT_Y_SCALE: w = fbits(di->vp_ys); break;
        case VC4_U_VIEWPORT_Z_OFFSET: w = fbits(di->vp_zo); break;
        case VC4_U_VIEWPORT_Z_SCALE: w = fbits(di->vp_zs); break;
        case VC4_U_TEXTURE_CONFIG_P0: { /* patch 0018: a plane in bits 16 and up; 0022: 4 to 7 the negatives' */
            const ULONG *tp = di->sm->p[si][x];
            ULONG pl = (d >> 16) & 7;
            w = pl < 4 ? tp[0] + pl * tp[3] : tp[4] + (pl - 4) * tp[3];
            break;
        }
        case VC4_U_TEXTURE_CONFIG_P1: w = di->sm->p[si][x][1]; break;
        case VC4_U_TEXTURE_CONFIG_P2: /* BSLOD: an explicit LOD (Mesa's: the recipe's bit 16) */
            w = di->sm->p[si][x][2] | ((d >> 16) & 1);
            break;
        case VC4_U_TEXTURE_FIRST_LEVEL: w = fbits((GLfloat)di->sm->first[si][x]); break;
        case VC4_U_UBO0_ADDR: /* the array's offset in the buffer (Mesa's reloc: + data) */
            w = di->ubo[si] + d;
            break;
        case VC4_U_BLEND_CONST_COLOR_X:
        case VC4_U_BLEND_CONST_COLOR_Y:
        case VC4_U_BLEND_CONST_COLOR_Z:
        case VC4_U_BLEND_CONST_COLOR_W:
            w = fbits(s->blend_color[v->rtype[k] - VC4_U_BLEND_CONST_COLOR_X]);
            break;
        case VC4_U_BLEND_CONST_COLOR_RGBA: { /* in the target's byte order (Mesa: its format swizzle) */
            ULONG b[4], j;
            for (j = 0; j < 4; j++)
                b[j] = (ULONG)(s->blend_color[j] * 255.0f + 0.5f);
            w = di->color_format == 1 ? b[2] | b[1] << 8 | b[0] << 16 | b[3] << 24
              : di->color_format == 2 ? b[0] | b[1] << 8 | b[2] << 16
                                      : b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
            break;
        }
        case VC4_U_BLEND_CONST_COLOR_AAAA:
            w = 0x01010101UL * (ULONG)(s->blend_color[3] * 255.0f + 0.5f);
            break;
        case VC4_U_STENCIL: w = d < 3 ? di->stencil[d] : 0; break;
        case VC4_U_SAMPLE_MASK: {
            ULONG m = 0xf;
            if (ctx->job_msaa && (s->caps & OGLES2_CAP_SAMPLE_COVERAGE)) {
                int n = (int)(s->sample_coverage_value * 4.0f + 0.5f);
                m = (1UL << n) - 1;
                if (s->sample_coverage_invert)
                    m = ~m & 0xf;
            }
            w = m;
            break;
        }
        case VC4_U_UNIFORMS_ADDRESS: w = bus; break;
        case VC4_U_TEXRECT_SCALE_X: w = fbits(1.0f / di->sm->emu[si][x][3]); break;
        case VC4_U_TEXRECT_SCALE_Y: w = fbits(1.0f / di->sm->emu[si][x][4]); break;
        case VC4_U_TEXTURE_LOD_BIAS:
        case VC4_U_TEXTURE_MIN_LOD:
        case VC4_U_TEXTURE_MAX_LOD:
        case VC4_U_TEXTURE_WIDTH:
        case VC4_U_TEXTURE_HEIGHT:
            w = fbits(di->sm->emu[si][x][v->rtype[k] - VC4_U_TEXTURE_LOD_BIAS]);
            break;
        default: w = 0; break; /* MSAA textures, UBO 1, border colour: not in GLES2 */
        }
        vc4_put32(u + 4 * k, w);
    }
    vc4_put32(u + 4 * k, 0);
    return bus;
}

/* A stage's constant buffer as a UBO (indirectly indexed uniforms): its
 * little-endian copy. */
static BOOL ubo_copy(struct vc4_ctx *c, const struct OGLES2Program *p, int si, ULONG *bus)
{
    UWORD n = p->image->cbuf_words[si];
    UBYTE *d = claim(c, n * 4UL + 16, 16, bus);
    if (!d)
        return FALSE;
    memcpy(d, p->cbuf_le[si], n * 4UL);
    return TRUE;
}

/* ---- vertex data ---- */

/* count elements of n components of type into the GPU's byte order. */
static void convert(UBYTE *dst, ULONG dst_stride, const UBYTE *src, ULONG src_stride, ULONG count, GLint n,
                    GLenum type)
{
    ULONG i;
    int k, es = type_size(type);
    /* Whole words a vertex (the 68k takes them unaligned), the type decided
     * once: 5 ms of Quake III's frame went on this byte by byte (P5). */
    if (es == 4) {
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++)
                ((ULONG *)dst)[k] = vc4_le32(((const ULONG *)src)[k]);
    } else if (es == 2) {
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++)
                ((UWORD *)dst)[k] = vc4_le16(((const UWORD *)src)[k]);
    } else if (n == 4) { /* colours */
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            *(ULONG *)dst = *(const ULONG *)src;
    } else {
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++)
                dst[k] = src[k];
    }
}

struct attr_src
{
    ULONG bus, stride, bytes;  /* bytes a vertex the GPU reads */
};

static BOOL copy_array(struct OGLES2Context *ctx, struct vc4_ctx *c, const struct OGLES2Array *a, GLint first,
                       GLsizei count, struct attr_src *o)
{
    int es = type_size(a->type), n = a->size;
    ULONG src_stride = a->stride ? (ULONG)a->stride : (ULONG)(n * es);
    ULONG dst_stride = ((ULONG)(n * es) + 3) & ~3UL;
    const UBYTE *src = (const UBYTE *)a->pointer;
    UBYTE *dst;
    o->bytes = (ULONG)(n * es);
    if (a->buffer && (src = ogles2_buffer_stream(a->buffer)) != 0) {
        src += (ULONG)a->pointer;
    } else if (a->buffer) {
        ULONG vertices, base = ogles2_buffer_copy(ctx, a->buffer, (ULONG)a->pointer, n, a->type, a->normalized,
                                                  a->stride, (ULONG)(n * es), &o->stride, &vertices, convert);
        if (!base || (ULONG)first + (ULONG)count > vertices || o->stride > 255)
            return FALSE;
        o->bus = ogles2_vc4_bus(base) + (ULONG)first * o->stride;
        return TRUE;
    }
    if (!(dst = claim(c, dst_stride * (ULONG)count + 4, 4, &o->bus)))
        return FALSE;
    o->stride = dst_stride;
    ctx->n_vbytes += dst_stride * (ULONG)count;
    convert(dst, dst_stride, src + (ULONG)first * src_stride, src_stride, (ULONG)count, n, a->type);
    return TRUE;
}

static BOOL constant_attrib(struct OGLES2Context *ctx, struct vc4_ctx *c, GLuint g, struct attr_src *o)
{
    UBYTE *p;
    int k;
    if (!(p = claim(c, 16, 16, &o->bus)))
        return FALSE;
    for (k = 0; k < 4; k++)
        vc4_putf(p + 4 * k, g < OGLES2_MAX_VERTEX_ATTRIBS ? ctx->state.attrib[g][k] : (k == 3 ? 1.0f : 0.0f));
    o->stride = 0;
    o->bytes = 16;
    return TRUE;
}

/* The GL shader record (Mesa's vc4_emit_gl_shader_state) with the vertex
 * data moved on by `skip` vertices; its bus address, 0. */
static ULONG shader_record(struct vc4_ctx *c, const ULONG code[3], const ULONG unif[3],
                           const struct OGLES2Variant *cs, const struct OGLES2Variant *vs,
                           const struct OGLES2Variant *fs, const struct attr_src *src, int nslots, ULONG skip)
{
    const struct vc4_prog *cp = VC4_PROG_C(cs), *vp = VC4_PROG_C(vs), *fp = VC4_PROG_C(fs);
    int n = nslots ? nslots : 1, k;
    ULONG bus;
    UBYTE *r = claim(c, 36 + 8UL * (ULONG)n, 16, &bus);
    if (!r)
        return 0;
    vc4_put16(r, (fp->fs_threaded ? 0 : 1u) | (vs->per_vertex_point_size ? 2u : 0) | 4u /* clipping */);
    r[2] = 0;
    r[3] = fp->num_inputs;
    vc4_put32(r + 4, code[2]);
    vc4_put32(r + 8, unif[2]);
    vc4_put16(r + 12, 0);
    r[14] = vp->vattrs_live;
    r[15] = vp->vattr_offsets[8];
    vc4_put32(r + 16, code[1]);
    vc4_put32(r + 20, unif[1]);
    vc4_put16(r + 24, 0);
    r[26] = cp->vattrs_live;
    r[27] = cp->vattr_offsets[8];
    vc4_put32(r + 28, code[0]);
    vc4_put32(r + 32, unif[0]);
    for (k = 0; k < n; k++) {
        UBYTE *a = r + 36 + 8 * k;
        if (nslots) {
            vc4_put32(a, src[k].bus + skip * src[k].stride);
            a[4] = (UBYTE)(src[k].bytes - 1);
            a[5] = (UBYTE)src[k].stride;
        } else { /* no inputs: one record the shaders do not read */
            vc4_put32(a, unif[1]);
            a[4] = 3;
            a[5] = 0;
        }
        a[6] = vp->vattr_offsets[k < 8 ? k : 8];
        a[7] = cp->vattr_offsets[k < 8 ? k : 8];
    }
    return bus;
}

/* ---- state packets ---- */

static void emit_state(struct OGLES2Context *ctx, struct vc4_cl *cl, struct vc4_emitted *em,
                       const struct OGLES2Variant *fs, const struct draw_info *di)
{
    const struct OGLES2State *s = &ctx->state;
    ULONG word[2], cfgw;
    BOOL first = !em->valid;
    BOOL flip = ctx->fb_flip, depth_on = (s->caps & OGLES2_CAP_DEPTH_TEST) && ctx->fb_depth_bits;
    BOOL stencil_on = (s->caps & OGLES2_CAP_STENCIL_TEST) && ctx->fb_stencil_bits;
    LONG fh = (LONG)ctx->fb_h, vx = ctx->viewport[0], vy = ctx->viewport[1], vw = ctx->viewport[2],
         vh = ctx->viewport[3], l, t, r, b;
    struct VC4_CONFIGURATION_BITS cfg;
    UBYTE *p;
    BOOL ez;

    /* the clip window: viewport and scissor box, top-down, in the framebuffer */
    l = vx;
    r = vx + vw;
    t = flip ? fh - (vy + vh) : vy;
    b = flip ? fh - vy : vy + vh;
    if (s->caps & OGLES2_CAP_SCISSOR_TEST) {
        LONG sl = ctx->scissor[0], sr = sl + ctx->scissor[2];
        LONG st = flip ? fh - (ctx->scissor[1] + ctx->scissor[3]) : ctx->scissor[1];
        LONG sb = flip ? fh - ctx->scissor[1] : ctx->scissor[1] + ctx->scissor[3];
        if (sl > l) l = sl;
        if (sr < r) r = sr;
        if (st > t) t = st;
        if (sb < b) b = sb;
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)ctx->fb_w) r = (LONG)ctx->fb_w;
    if (b > fh) b = fh;
    if (r < l) r = l;
    if (b < t) b = t;
    /* Each packet only when it says something new in this list (the state
     * lasts as long as the list: 4 ms of Quake III's frame, P5). */
    if (first || em->clip[0] != (ULONG)l || em->clip[1] != (ULONG)t || em->clip[2] != (ULONG)r || em->clip[3] != (ULONG)b) {
        em->clip[0] = (ULONG)l;
        em->clip[1] = (ULONG)t;
        em->clip[2] = (ULONG)r;
        em->clip[3] = (ULONG)b;
        VC4_EMIT(cl, CLIP_WINDOW, .clip_window_left_pixel_coordinate = (uint32_t)l,
                 .clip_window_bottom_pixel_coordinate = (uint32_t)t, .clip_window_width_in_pixels = (uint32_t)(r - l),
                 .clip_window_height_in_pixels = (uint32_t)(b - t));
    }

    /* configuration bits (Mesa's vc4_create_rasterizer_state and _zsa_state):
     * culling, the winding as the V3D backend sets it, early Z in the < direction */
    memset(&cfg, 0, sizeof(cfg));
    cfg.enable_forward_facing_primitive = !(s->caps & OGLES2_CAP_CULL_FACE) || s->cull_face_mode == GL_BACK;
    cfg.enable_reverse_facing_primitive = !(s->caps & OGLES2_CAP_CULL_FACE) || s->cull_face_mode == GL_FRONT;
    cfg.clockwise_primitives = y_down(ctx) ? s->front_face == GL_CCW : s->front_face == GL_CW;
    cfg.enable_depth_offset = (s->caps & OGLES2_CAP_POLYGON_OFFSET_FILL) ? 1 : 0;
    cfg.rasteriser_oversample_mode = ctx->job_msaa ? 1 : 0;
    cfg.early_z_updates_enable = 1;
    if (depth_on) {
        cfg.depth_test_function = (uint32_t)(s->depth_func - GL_NEVER);
        cfg.z_updates_enable = s->depth_mask ? 1 : 0;
        ez = (s->depth_func == GL_LESS || s->depth_func == GL_LEQUAL) &&
             (!stencil_on || (s->stencil_zfail[0] == GL_KEEP && s->stencil_zfail[1] == GL_KEEP));
        /* HW-2905: no early Z with 4x MSAA (Mesa); nor for a shader that turns it off */
        cfg.early_z_enable = ez && !ctx->job_msaa && !VC4_PROG_C(fs)->disable_early_z;
    } else {
        cfg.depth_test_function = 7; /* ALWAYS */
    }
    if (ctx->job_msaa)
        cfg.early_z_updates_enable = 0;
    VC4_CONFIGURATION_BITS_pack((UBYTE *)&cfgw, &cfg);
    if ((first || em->cfg != cfgw) && (p = vc4_cl_room(cl, VC4_CONFIGURATION_BITS_length)) != 0) {
        em->cfg = cfgw;
        *(ULONG *)p = cfgw; /* the packet's four bytes as packed (the list is in memory the CPU caches) */
    }
    /* depth offset: Mesa's float_to_187_half (the top 16 bits); units in
     * the 24-bit buffer's steps, which VC4's always is */
    word[0] = fbits(s->polygon_offset_factor) >> 16;
    word[1] = fbits(s->polygon_offset_units) >> 16;
    if (first || em->offset[0] != word[0] || em->offset[1] != word[1]) {
        em->offset[0] = word[0];
        em->offset[1] = word[1];
        VC4_EMIT(cl, DEPTH_OFFSET, .depth_offset_factor = word[0], .depth_offset_units = word[1]);
    }
    if (first)
        VC4_EMIT(cl, POINT_SIZE, .point_size = 1.0f);
    word[0] = fbits(s->line_width);
    if (first || em->line != word[0]) {
        em->line = word[0];
        VC4_EMIT(cl, LINE_WIDTH, .line_width = s->line_width);
    }
    word[0] = fbits(di->vp_xs);
    word[1] = fbits(di->vp_ys);
    if (first || em->xy[0] != word[0] || em->xy[1] != word[1]) {
        em->xy[0] = word[0];
        em->xy[1] = word[1];
        VC4_EMIT(cl, CLIPPER_XY_SCALING, .viewport_half_width_in_1_16th_of_pixel = di->vp_xs,
                 .viewport_half_height_in_1_16th_of_pixel = di->vp_ys);
    }
    word[0] = fbits(di->vp_zs);
    word[1] = fbits(di->vp_zo);
    if (first || em->z[0] != word[0] || em->z[1] != word[1]) {
        em->z[0] = word[0];
        em->z[1] = word[1];
        VC4_EMIT(cl, CLIPPER_Z_SCALE_AND_OFFSET, .viewport_z_scale_zc_to_zs = di->vp_zs,
                 .viewport_z_offset_zc_to_zs = di->vp_zo);
    }
    {
        float cx = (float)vx + (float)vw * 0.5f;
        float cy = flip ? (float)fh - ((float)vy + (float)vh * 0.5f) : (float)vy + (float)vh * 0.5f;
        word[0] = fbits(cx);
        word[1] = fbits(cy);
        if (first || em->centre[0] != word[0] || em->centre[1] != word[1]) {
            em->centre[0] = word[0];
            em->centre[1] = word[1];
            VC4_EMIT(cl, VIEWPORT_OFFSET, .viewport_centre_x_coordinate = cx, .viewport_centre_y_coordinate = cy);
        }
    }
    if (first)
        VC4_EMIT(cl, FLAT_SHADE_FLAGS, .flat_shading_flags = 0);
    em->valid = TRUE;
}

/* ---- one draw ---- */

/* The draw's three variants and the images they are in ([0] coordinate,
 * [1] vertex, [2] fragment): from the program's image or its keyed one. */
struct chosen
{
    const struct OGLES2Variant *v[3];
    const struct OGLES2Image *in[3];
};

static BOOL find_all(const struct OGLES2Image *a, const struct OGLES2Image *b, const UBYTE *kfs, const UBYTE *kvs,
                     BOOL points, struct chosen *ch)
{
    static const UBYTE stages[3] = {OGLES2_VARIANT_CS, OGLES2_VARIANT_VS, OGLES2_VARIANT_FS};
    int k, j;
    for (k = 2; k >= 0; k--) { /* the fragment variant first: the vertex one pairs with it */
        const struct OGLES2Image *ims[2];
        ims[0] = a;
        ims[1] = b;
        ch->v[k] = 0;
        for (j = 0; j < 2 && !ch->v[k]; j++)
            if (ims[j] && (ch->v[k] = ogles2_vc4_variant_find(ims[j], stages[k], (k == 2 ? kfs : kvs) + 4,
                                                                 ch->v[2], points)) != 0)
                ch->in[k] = ims[j];
        if (!ch->v[k])
            return FALSE;
    }
    return TRUE;
}

/* By the keys; else the keyed image, loaded, then compiled with them; else
 * the front end's choice (logged once a program). */
static void choose(struct OGLES2Context *ctx, struct vc4_draw *d, const struct OGLES2DrawCall *dc,
                   const UBYTE *kfs, const UBYTE *kvs, BOOL points, struct chosen *ch)
{
    struct OGLES2Image *im = (struct OGLES2Image *)dc->p->image;
    const struct OGLES2Image *k;
    UBYTE recs[VC4_KEY_FS_BYTES + VC4_KEY_VS_BYTES];
    int j;
    if (find_all(im, 0, kfs, kvs, points, ch))
        return;
    if ((k = ogles2_image_keyed(ctx, im, 0, 0)) != 0 && find_all(im, k, kfs, kvs, points, ch))
        return;
    memcpy(recs, kfs, VC4_KEY_FS_BYTES);
    memcpy(recs + VC4_KEY_FS_BYTES, kvs, VC4_KEY_VS_BYTES);
    if ((k = ogles2_image_keyed(ctx, im, recs, sizeof(recs))) != 0 && find_all(im, k, kfs, kvs, points, ch))
        return;
    /* points: a variant without the per-vertex size before the closest */
    if (points && (find_all(im, k, kfs, kvs, 2, ch) || find_all(im, 0, kfs, kvs, 2, ch)))
        return;
    if (d->missed[0] != im) {
        d->missed[0] = im;
        ogles2_log("draw: the program has no shader variants for this draw's state, and none were compiled; "
                   "the closest are used (logged once a program)");
    }
    for (j = 0; j < 3; j++) {
        ch->v[j] = dc->var[j];
        ch->in[j] = im;
    }
}

/* The image's last choices (its be_cache). The keys are 596 bytes, slow to
 * build and hash for every draw (6 ms of Quake III's frame, P5), and the search
 * compares them variant by variant: a draw whose signature equals an earlier
 * one's takes its variants without either. The signature holds all that
 * ogles2_vc4_draw_keys reads, in the form the key does (what the key leaves out
 * left out too, as a wrap mode but mirror-clamp), and must stay in step with it. */
#define CHOICES 8
#define SIG_SAMPLERS 8
struct draw_sig
{
    ULONG flags, counts;
    UWORD blend[6];
    UWORD attr[8];
    ULONG samp[2 * SIG_SAMPLERS]; /* per sampler in use, both stages: its format, its bits */
};
#define SIG_WORDS_HEAD ((sizeof(struct draw_sig) - sizeof(((struct draw_sig *)0)->samp)) / 4)

struct choice
{
    ULONG words; /* 0: unused */
    struct draw_sig sig;
    struct chosen ch;
};
struct choices
{
    UBYTE next, last;
    struct choice e[CHOICES];
};

/* The draw's signature; its length in words, 0 with too many samplers for one. */
static ULONG draw_signature(const struct OGLES2Context *ctx, const struct OGLES2Program *p, const struct samplers *sm,
                            GLenum mode, BOOL points, BOOL twoside, BOOL full, UBYTE color_format, UBYTE att,
                            struct draw_sig *g)
{
    const struct OGLES2State *s = &ctx->state;
    ULONG caps = s->caps, f, n = 0;
    int nslots = p->nslots == 0xff ? 0 : p->nslots, st, k;
    BOOL msaa = ctx->job_msaa != 0;
    if (sm->n[0] + sm->n[1] > SIG_SAMPLERS)
        return 0;
    if (nslots > 8)
        nslots = 8;
    f = (ULONG)color_format | (ULONG)att << 2;
    f |= (mode == GL_POINTS ? 2UL : (mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP) ? 1UL : 0UL) << 4;
    if (points)
        f |= 1UL << 6;
    if ((caps & OGLES2_CAP_DEPTH_TEST) && ctx->fb_depth_bits)
        f |= 1UL << 7;
    if ((caps & OGLES2_CAP_STENCIL_TEST) && ctx->fb_stencil_bits)
        f |= 1UL << 8 | (twoside ? 1UL << 9 : 0) | (full ? 1UL << 10 : 0);
    if (mode == GL_POINTS && y_down(ctx))
        f |= 1UL << 11;
    if (msaa)
        f |= 1UL << 12 | ((caps & OGLES2_CAP_SAMPLE_COVERAGE) ? 1UL << 13 : 0) |
             ((caps & OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE) ? 1UL << 14 : 0);
    f |= (s->color_mask[0] ? 1UL << 16 : 0) | (s->color_mask[1] ? 1UL << 17 : 0) | (s->color_mask[2] ? 1UL << 18 : 0) |
         (s->color_mask[3] ? 1UL << 19 : 0);
    if (s->clip_depth == GL_ZERO_TO_ONE_EXT)
        f |= 1UL << 20;
    f |= (ULONG)nslots << 21;
    if (caps & OGLES2_CAP_BLEND) {
        f |= 1UL << 15;
        g->blend[0] = (UWORD)s->blend_eq_rgb;
        g->blend[1] = (UWORD)s->blend_eq_alpha;
        g->blend[2] = (UWORD)s->blend_src_rgb;
        g->blend[3] = (UWORD)s->blend_dst_rgb;
        g->blend[4] = (UWORD)s->blend_src_alpha;
        g->blend[5] = (UWORD)s->blend_dst_alpha;
    } else {
        g->blend[0] = g->blend[1] = g->blend[2] = g->blend[3] = g->blend[4] = g->blend[5] = 0;
    }
    g->flags = f;
    g->counts = (ULONG)sm->n[0] | (ULONG)sm->n[1] << 8;
    for (k = 0; k < 8; k++) {
        GLuint a = k < nslots ? p->slot_attrib[k] : 0xff;
        const struct OGLES2Array *arr = a < OGLES2_MAX_VERTEX_ATTRIBS ? &ctx->array[a] : 0;
        g->attr[k] = arr && arr->enabled ? (UWORD)(0x8000u | (arr->type & 15) << 8 | arr->size << 4 | (arr->normalized ? 1 : 0)) : 0;
    }
    for (st = 0; st < 2; st++)
        for (k = 0; k < sm->n[st]; k++) {
            const struct vc4_key_sampler *ks = &sm->key[st][k];
            g->samp[n++] = (ULONG)ks->f;
            g->samp[n++] = (ks->wrap_s == GL_MIRROR_CLAMP_TO_EDGE ? 1UL : 0) | (ks->wrap_t == GL_MIRROR_CLAMP_TO_EDGE ? 2UL : 0) |
                           (ks->force_first ? 4UL : 0) | (ks->lod_bias ? 8UL : 0) | (ks->lod_clamp ? 16UL : 0) |
                           (ks->half_signed ? 32UL : 0) | (ULONG)ks->aniso << 8;
        }
    return SIG_WORDS_HEAD + n;
}

static void choose_cached(struct OGLES2Context *ctx, struct vc4_draw *d, const struct OGLES2DrawCall *dc,
                          const struct samplers *sm, GLenum mode, BOOL points, BOOL twoside, BOOL full,
                          UBYTE color_format, UBYTE att, UBYTE draw_buffer, struct chosen *ch)
{
    struct OGLES2Image *im = (struct OGLES2Image *)dc->p->image;
    struct choices *cache = im->be_cache;
    struct draw_sig sig;
    struct choice *e;
    UBYTE kfs[VC4_KEY_FS_BYTES], kvs[VC4_KEY_VS_BYTES];
    ULONG words = draw_signature(ctx, dc->p, sm, mode, points, twoside, full, color_format, att, &sig), i, j;
    if (cache && words) {
        /* the last one first: runs of draws in one state */
        for (i = 0; i < CHOICES; i++) {
            ULONG at = (i + cache->last) % CHOICES;
            e = &cache->e[at];
            if (e->words != words || e->sig.flags != sig.flags)
                continue;
            for (j = 1; j < words && ((const ULONG *)&e->sig)[j] == ((const ULONG *)&sig)[j]; j++)
                ;
            if (j == words) {
                cache->last = (UBYTE)at;
                *ch = e->ch;
                return;
            }
        }
    }
    ogles2_vc4_draw_keys(ctx, dc->p, mode, color_format, draw_buffer,
                         (const struct vc4_key_sampler(*)[OGLES2_MAX_STAGE_SAMPLERS])sm->key, sm->n, points, kfs, kvs);
    choose(ctx, d, dc, kfs, kvs, points, ch);
    if (!words)
        return;
    /* the search may have replaced the keyed image, and with it the cache */
    if (!(cache = im->be_cache) && !(cache = im->be_cache = AllocVec(sizeof(*cache), MEMF_ANY | MEMF_CLEAR)))
        return;
    cache->last = cache->next;
    e = &cache->e[cache->next];
    cache->next = (UBYTE)((cache->next + 1) % CHOICES);
    e->words = words;
    for (j = 0; j < words; j++)
        ((ULONG *)&e->sig)[j] = ((const ULONG *)&sig)[j];
    e->ch = *ch;
}

static ULONG code_bus(const struct OGLES2Image *im, const struct OGLES2Variant *v)
{
    return im->code_mem ? im->code_mem->busaddr + v->code_offset : 0;
}

BOOL ogles2_vc4_draw(struct OGLES2Context *ctx, const struct OGLES2DrawCall *dc)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct OGLES2Program *p = dc->p;
    const struct OGLES2Image *im = p->image;
    const struct OGLES2State *s = &ctx->state;
    const struct OGLES2Elements *el = dc->el;
    struct vc4_draw *d;
    struct samplers sm;
    struct draw_info di;
    struct attr_src src[16];
    const struct OGLES2Variant *cs, *vs, *fs;
    struct chosen ch;
    ULONG code[3], unif[3], rec;
    ULONG ib = 0, ilen = 0, imax = 0; /* the draw's 16-bit (or 8-bit) indices, shared by its passes */
    GLenum mode = dc->mode, poly = 0;
    BOOL twoside = FALSE, full = FALSE, points = mode == GL_POINTS;
    int nslots = p->nslots == 0xff ? 0 : p->nslots, i, k, pass;

    if (c->bad)
        return FALSE;
    /* glPolygonMode, which VC4 has no hardware for (P5 run 1, m6_ext): a
     * triangle draw's vertices drawn as points, or its triangles' edges as a
     * line list built at the emission below; face culling cannot come first
     * (the CPU has no transformed vertices): a culled face's edges are drawn */
    if (mode >= GL_TRIANGLES && s->polygon_mode != GL_FILL) {
        poly = mode;
        mode = s->polygon_mode == GL_POINT ? GL_POINTS : GL_LINES;
        points = mode == GL_POINTS;
    }
    {
        /* A job near the end of its blocks (VC4_CL_BLOCKS of state and of
         * binning list: a frame of many draws, m4_stress's 100,000, P5) is
         * rendered now, mid-frame, and this draw goes into the next job
         * (depth and stencil kept, the colour loaded), as the Pi 4's lists
         * grow instead */
        const struct vc4_job *j = &c->job[c->build];
        if (c->has_draws && (j->state.cur >= VC4_CL_BLOCKS - 2 || j->bcl.cur >= VC4_CL_BLOCKS - 2)) {
            ogles2_job_flush(ctx, FALSE);
            ogles2_job_begin(ctx);
            if (c->bad)
                return FALSE;
        }
    }
    if (!(d = draw_state(ctx)) || !im->code_mem)
        goto nomem;
    memset(&sm, 0, sizeof(sm));
    memset(&di, 0, sizeof(di));
    if (!sampler_states(ctx, p, &sm))
        goto nomem;
    mark(ctx, 2);

    /* gl_PointSize's flag in the keys: points (the variant search tries both) */
    if ((s->caps & OGLES2_CAP_STENCIL_TEST) && ctx->fb_stencil_bits)
        ogles2_vc4_stencil_words(s, di.stencil, &twoside, &full);
    di.p = p;
    di.sm = &sm;
    di.vp_xs = (float)ctx->viewport[2] * 0.5f * 16.0f;
    di.vp_ys = (float)ctx->viewport[3] * (y_down(ctx) ? -0.5f : 0.5f) * 16.0f;
    di.vp_zs = (s->depth_range[1] - s->depth_range[0]) * 0.5f;
    di.vp_zo = (s->depth_range[1] + s->depth_range[0]) * 0.5f;
    if (nslots > 8)
        nslots = 8; /* VC4's 8 attributes (caps) */

    for (pass = 0; pass < c->npass; pass++) {
        struct vc4_cl *cl = &ogles2_vc4_pass_job(c, pass)->bcl;

        /* the variants, by the keys (drawkey_vc4.c) */
        choose_cached(ctx, d, dc, &sm, mode, points, twoside, full, c->pass_format[pass], c->pass_att[pass],
                      c->npass > 1 ? ogles2_vc4_output_for(im, c->pass_att[pass]) : 0, &ch);
        cs = ch.v[0];
        vs = ch.v[1];
        fs = ch.v[2];
        code[0] = code_bus(ch.in[0], cs);
        code[1] = code_bus(ch.in[1], vs);
        code[2] = code_bus(ch.in[2], fs);
        if (!code[0] || !code[1] || !code[2])
            goto nomem;
        mark(ctx, 1);

        /* the uniform streams */
        di.color_format = c->pass_format[pass];
        for (k = 0; k < 2; k++)
            if (!di.ubo[k] && ((k ? fs : vs)->uses_ubo || (!k && cs->uses_ubo)))
                if (!ubo_copy(c, p, k, &di.ubo[k]))
                    goto nomem;
        unif[0] = write_uniforms(ctx, c, &di, cs);
        unif[1] = write_uniforms(ctx, c, &di, vs);
        unif[2] = write_uniforms(ctx, c, &di, fs);
        if (!unif[0] || !unif[1] || !unif[2])
            goto nomem;
        mark(ctx, 3);

        /* the vertex data, once a draw */
        if (!pass)
            for (i = 0; i < nslots; i++) {
                GLuint g = p->slot_attrib[i];
                BOOL ok = g < OGLES2_MAX_VERTEX_ATTRIBS && ctx->array[g].enabled
                              ? copy_array(ctx, c, &ctx->array[g], dc->first, dc->count, &src[i])
                              : constant_attrib(ctx, c, g, &src[i]);
                if (!ok)
                    goto nomem;
            }
        mark(ctx, 4);
        if (!(rec = shader_record(c, code, unif, cs, vs, fs, src, nslots, 0)))
            goto nomem;
        mark(ctx, 5);

        emit_state(ctx, cl, &c->emitted[pass], fs, &di);
        if (poly && mode == GL_LINES) {
            /* each triangle's three edges, as 16-bit indices */
            if (!pass) {
                ULONG n = el ? (ULONG)el->count : (ULONG)dc->count, ntri, t;
                UBYTE *x;
                ntri = poly == GL_TRIANGLES ? n / 3 : n >= 3 ? n - 2 : 0;
                if (ntri) {
                    if (!(x = claim(c, ntri * 12 + 4, 16, &ib)))
                        goto nomem;
                    for (t = 0; t < ntri; t++) {
                        ULONG v[3], q;
                        v[0] = poly_index(el, poly == GL_TRIANGLES ? 3 * t : poly == GL_TRIANGLE_FAN ? 0 : t);
                        v[1] = poly_index(el, poly == GL_TRIANGLES ? 3 * t + 1 : t + 1);
                        v[2] = poly_index(el, poly == GL_TRIANGLES ? 3 * t + 2 : t + 2);
                        for (q = 0; q < 6; q++, ilen++) {
                            ULONG e = v[((q + 1) >> 1) % 3]; /* 0 1, 1 2, 2 0 */
                            if (e > imax)
                                imax = e;
                            x[2 * ilen] = (UBYTE)e;
                            x[2 * ilen + 1] = (UBYTE)(e >> 8);
                        }
                    }
                    if (imax > MAX_VERTS)
                        return TRUE; /* nothing drawn (as the indexed path) */
                }
            }
            if (ilen) {
                vc4_gl_shader_state(cl, rec, nslots ? nslots : 1);
                VC4_EMIT(cl, INDEXED_PRIMITIVE_LIST, .primitive_mode = GL_LINES, .index_type = 1u, .length = ilen,
                         .address_of_indices_list = ib, .maximum_index = imax);
            }
        } else if (el) {
            /* indices rebased by the front end: 8-bit as they are, 16-bit
             * little-endian, 32-bit as 16-bit while they fit */
            ULONG n = (ULONG)el->count, j;
            BOOL bytes = el->type == GL_UNSIGNED_BYTE;
            if (!pass) {
                UBYTE *x;
                /* The largest is the front end's range (it copied that many
                 * vertices). No branch in these loops: Emu68 runs a loop with
                 * branches at a third of the speed (draw.c). */
                imax = (ULONG)dc->count - 1;
                if (imax > MAX_VERTS) {
                    if (d->told_big != im) {
                        d->told_big = im;
                        ogles2_log("draw: indices span more than 65536 vertices: VC4 cannot draw them (logged once a program)");
                    }
                    return TRUE; /* nothing drawn */
                }
                if (!(x = claim(c, (bytes ? n : 2 * n) + 4, 16, &ib)))
                    goto nomem;
                if (el->type == GL_UNSIGNED_SHORT) {
                    const UWORD *in = el->indices;
                    UWORD *o = (UWORD *)x, m = (UWORD)el->min;
                    for (j = 0; j < n; j++)
                        o[j] = vc4_le16((UWORD)(in[j] - m));
                } else if (el->type == GL_UNSIGNED_INT) {
                    const ULONG *in = el->indices;
                    UWORD *o = (UWORD *)x;
                    ULONG m = el->min;
                    for (j = 0; j < n; j++)
                        o[j] = vc4_le16((UWORD)(in[j] - m));
                } else {
                    const UBYTE *in = el->indices;
                    UBYTE m = (UBYTE)el->min;
                    for (j = 0; j < n; j++)
                        x[j] = (UBYTE)(in[j] - m);
                }
                ctx->n_indices += el->count;
            }
            vc4_gl_shader_state(cl, rec, nslots ? nslots : 1);
            VC4_EMIT(cl, INDEXED_PRIMITIVE_LIST, .primitive_mode = mode, .index_type = bytes ? 0u : 1u, .length = n,
                     .address_of_indices_list = ib, .maximum_index = imax);
        } else {
            /* parts of up to 65535 vertices (Mesa's vc4_draw_vbo); strips go on
             * from their last vertices; loops and fans are drawn whole up to the limit */
            ULONG left = (ULONG)dc->count, start = 0, part, step;
            while (left) {
                part = step = left;
                if (left > MAX_VERTS) {
                    switch (mode) {
                    case GL_POINTS: part = step = MAX_VERTS; break;
                    case GL_LINES: part = step = MAX_VERTS - MAX_VERTS % 2; break;
                    case GL_TRIANGLES: part = step = MAX_VERTS - MAX_VERTS % 3; break;
                    case GL_LINE_STRIP: part = MAX_VERTS; step = part - 1; break;
                    case GL_TRIANGLE_STRIP: part = MAX_VERTS - 1; step = part - 2; break; /* even: the winding stays */
                    default:
                        part = step = left = MAX_VERTS;
                        if (d->told_big != im) {
                            d->told_big = im;
                            ogles2_log("draw: a loop or fan of more than 65535 vertices: drawn up to there (logged once a program)");
                        }
                        break;
                    }
                }
                if (start && !(rec = shader_record(c, code, unif, cs, vs, fs, src, nslots, start)))
                    goto nomem;
                vc4_gl_shader_state(cl, rec, nslots ? nslots : 1);
                VC4_EMIT(cl, VERTEX_ARRAY_PRIMITIVES, .primitive_mode = mode, .length = part, .index_of_first_vertex = 0);
                if (part >= left)
                    break;
                left -= step;
                start += step;
            }
        }
        mark(ctx, 7);
        if (cl->failed || c->job[c->build].state.failed)
            goto nomem;
    }
    c->has_draws = TRUE;
    return TRUE;

nomem:
    c->bad = TRUE;
    c->why_bad = "a draw ran out of memory";
    return FALSE;
}
