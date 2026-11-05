/*
 * A draw's VC4 shader keys from the GL state (docs/PI3_PLAN.md §4.2, P4 and
 * P5): what the VC4 backend's draw chooses its variants by (draw_vc4.c),
 * and probe R, which writes the same keys on either GPU so the offline
 * corpus can be compiled with them (the key manifests: `v3dglslc -g 21 -k`).
 *
 * The keys are key_vc4.c's canonical records: the target's format class,
 * the primitive class, depth and stencil enables (two-sided, full write
 * masks), MSAA's flags, blending in GL's terms, the colour mask; per sampler
 * the texture's storage (ogles2_vc4_storage) and wraps; per vertex input
 * slot the array's pipe format (a disabled one's current value: four
 * floats); points with gl_PointSize.
 *
 * Probe R (ENV:OGLES2/Probe): each program's distinct key pairs, a fragment
 * record and its vertex record, appended to T:ogles2-vc4keys/<hash>.keys
 * (the program's file name's 24 digits) as they first appear. On the Pi 4
 * the textures' storage is worked out from their formats as the Pi 3 would
 * store them, and gl_PointSize from the Pi 4 variant; the Pi 3 would build
 * the same keys. Each file is a key file for `v3dglslc -g 21 -k` with the
 * program's sources.
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "../draw_private.h"
#include "../texture_private.h"
#include "key_vc4.h"
#include "backend_vc4.h"

/* ---- textures: VC4's storage of a texture's level 0 ---- */

const struct vc4_tex_format *ogles2_vc4_storage(const struct OGLES2Texture *t)
{
    GLenum f = t->f[0].format[0], ty = t->f[0].type[0];
    const struct vc4_tex_format *s;
    if (class_of(f, ty) == CLASS_DEPTH16 || class_of(f, ty) == CLASS_DEPTH24)
        return vc4_tex_format(f == GL_DEPTH_STENCIL_OES ? GL_DEPTH_STENCIL_OES : GL_DEPTH_COMPONENT,
                              f == GL_DEPTH_STENCIL_OES ? GL_UNSIGNED_INT_24_8_OES : GL_UNSIGNED_INT);
    if (class_of(f, ty) == CLASS_BLOCKS)
        return vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE); /* S3TC: decoded at upload (texture_vc4.c) */
    if (class_of(f, ty) == CLASS_FLOAT)
        return vc4_tex_format(GL_RGBA, GL_HALF_FLOAT_OES); /* RGBA64 (texture_vc4.c) */
    if (f == GL_ETC1_RGB8_OES)
        return vc4_tex_format(GL_RGB, GL_UNSIGNED_BYTE); /* decoded by texture.c */
    if (f == GL_BGRA_EXT)
        return vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE); /* converted by texture.c */
    s = vc4_tex_format(f, ty);
    return s ? s : vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE);
}

/* The emulated texture parameters (patch 0019, key_vc4.h): one level above
 * level 0 by an explicit LOD; the bias whenever it is set; with a
 * mipmapping filter (not on cube maps: the LOD there would need the
 * major face's derivatives), the LOD computed and clamped when a base level
 * or the LOD range limits it, and 2 or 4 lookups for anisotropy (the
 * backend reports 4). */
void ogles2_vc4_sampler_emu(const struct OGLES2Texture *t, int kind, int first, int last, struct vc4_key_sampler *ks)
{
    ks->force_first = (unsigned char)(first > 0 && first == last);
    ks->lod_bias = (unsigned char)(t->lod_bias != 0.0f && !ks->force_first);
    ks->lod_clamp = 0;
    ks->aniso = 0;
    if (kind == OGLES2_TEX_CUBE || ks->force_first || !mipmapped(t->min_filter))
        return;
    ks->lod_clamp = (unsigned char)(first > 0 || t->min_lod > 0.0f || t->max_lod < (GLfloat)(last - first));
    if (t->max_aniso >= 2.0f)
        ks->aniso = (unsigned char)(t->max_aniso >= 4.0f ? 2 : 1);
}

/* ---- the stencil test (Mesa's vc4_state.c, tlb_stencil_setup_*) ---- */

static ULONG stencil_wm_bits(GLuint m)
{
    m &= 0xff;
    return m == 0x1 ? 0 : m == 0x3 ? 1 : m == 0xf ? 2 : m == 0xff ? 3 : 0xff;
}

static ULONG stencil_op(GLenum op)
{
    switch (op) {
    case GL_ZERO: return 0;
    case GL_REPLACE: return 2;
    case GL_INCR: return 3;
    case GL_DECR: return 4;
    case GL_INVERT: return 5;
    case GL_INCR_WRAP: return 6;
    case GL_DECR_WRAP: return 7;
    default: return 1; /* GL_KEEP */
    }
}

static ULONG stencil_bits(const struct OGLES2State *s, int f, ULONG wm)
{
    ULONG b = 0;
    if (wm != 0xff)
        b |= wm << 28;
    /* A write mask of 0 writes nothing: KEEP throughout. Its full-mask word
     * is 0, which the key takes for "no full masks", and the 2-bit field's 0
     * is mask 0x1 (P5 run 1: the corpus's Stencil_Test, glStencilMask(0)
     * with INVERT left over, flipped bit 0 between its passes) */
    if (!(s->stencil_write_mask[f] & 0xff))
        return b | 1UL << 25 | 1UL << 22 | 1UL << 19 | (ULONG)(s->stencil_func[f] - GL_NEVER) << 16 |
               (s->stencil_value_mask[f] & 0xff);
    return b | stencil_op(s->stencil_zfail[f]) << 25 | stencil_op(s->stencil_zpass[f]) << 22 |
           stencil_op(s->stencil_fail[f]) << 19 | (ULONG)(s->stencil_func[f] - GL_NEVER) << 16 |
           (s->stencil_value_mask[f] & 0xff);
}

void ogles2_vc4_stencil_words(const struct OGLES2State *s, ULONG w[3], BOOL *twoside, BOOL *full)
{
    ULONG fwm = stencil_wm_bits(s->stencil_write_mask[0]), bwm;
    BOOL back = s->stencil_func[1] != s->stencil_func[0] || s->stencil_fail[1] != s->stencil_fail[0] ||
                s->stencil_zfail[1] != s->stencil_zfail[0] || s->stencil_zpass[1] != s->stencil_zpass[0] ||
                s->stencil_ref[1] != s->stencil_ref[0] ||
                (s->stencil_value_mask[1] & 0xff) != (s->stencil_value_mask[0] & 0xff) ||
                (s->stencil_write_mask[1] & 0xff) != (s->stencil_write_mask[0] & 0xff);
    GLuint back_mask = s->stencil_write_mask[0];
    bwm = fwm;
    w[0] = stencil_bits(s, 0, fwm) | ((ULONG)s->stencil_ref[0] & 0xff) << 8;
    w[1] = 0;
    w[2] = 0;
    if (back) {
        bwm = stencil_wm_bits(s->stencil_write_mask[1]);
        back_mask = s->stencil_write_mask[1];
        w[0] |= 1UL << 30;
        w[1] = stencil_bits(s, 1, bwm) | 2UL << 30 | ((ULONG)s->stencil_ref[1] & 0xff) << 8;
    } else {
        w[0] |= 3UL << 30;
    }
    if (fwm == 0xff || bwm == 0xff)
        w[2] = (s->stencil_write_mask[0] & 0xff) | (back_mask & 0xff) << 8;
    *twoside = back;
    *full = w[2] != 0;
}

/* ---- the keys ---- */

static const unsigned short target_format[3] = {VC4K_R8G8B8A8_UNORM, VC4K_B8G8R8A8_UNORM, VC4K_B5G6R5_UNORM};

static BOOL y_down(const struct OGLES2Context *ctx)
{
    return ctx->fb_flip ^ (ctx->state.clip_origin == GL_UPPER_LEFT_EXT);
}

void ogles2_vc4_draw_keys(struct OGLES2Context *ctx, const struct OGLES2Program *p, GLenum mode, UBYTE color_class,
                          const struct vc4_key_sampler key[2][OGLES2_MAX_STAGE_SAMPLERS], const UBYTE n[2], int psize,
                          UBYTE *kfs, UBYTE *kvs)
{
    const struct OGLES2State *s = &ctx->state;
    struct vc4_fs_state fss;
    struct vc4_vs_state vss;
    GLenum fsmode;
    int nslots = p->nslots == 0xff ? 0 : p->nslots, i, k;
    memset(&fss, 0, sizeof(fss));
    memset(&vss, 0, sizeof(vss));
    /* the class the rasteriser sees: glPolygonMode's lines and points */
    fsmode = mode >= GL_TRIANGLES && s->polygon_mode != GL_FILL ? (s->polygon_mode == GL_POINT ? GL_POINTS : GL_LINES)
                                                                 : mode;
    fss.color_format = target_format[color_class < 3 ? color_class : 0];
    fss.prim = fsmode == GL_POINTS ? 2 : (fsmode == GL_LINES || fsmode == GL_LINE_LOOP || fsmode == GL_LINE_STRIP) ? 1 : 0;
    fss.depth_test = (s->caps & OGLES2_CAP_DEPTH_TEST) && ctx->fb_depth_bits;
    fss.stencil_test = (s->caps & OGLES2_CAP_STENCIL_TEST) && ctx->fb_stencil_bits;
    if (fss.stencil_test) {
        ULONG w[3];
        BOOL twoside, full;
        ogles2_vc4_stencil_words(s, w, &twoside, &full);
        fss.stencil_twoside = (unsigned char)twoside;
        fss.stencil_full_writemasks = (unsigned char)full;
    }
    fss.point_coord_upper_left = (unsigned char)y_down(ctx);
    fss.msaa = ctx->job_msaa ? 1 : 0;
    fss.sample_coverage = (s->caps & OGLES2_CAP_SAMPLE_COVERAGE) ? 1 : 0;
    fss.alpha_to_coverage = (s->caps & OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE) ? 1 : 0;
    fss.blend = (s->caps & OGLES2_CAP_BLEND) ? 1 : 0;
    fss.eq_rgb = s->blend_eq_rgb;
    fss.eq_alpha = s->blend_eq_alpha;
    fss.src_rgb = s->blend_src_rgb;
    fss.dst_rgb = s->blend_dst_rgb;
    fss.src_alpha = s->blend_src_alpha;
    fss.dst_alpha = s->blend_dst_alpha;
    fss.colormask = (unsigned char)((s->color_mask[0] ? 1 : 0) | (s->color_mask[1] ? 2 : 0) |
                                    (s->color_mask[2] ? 4 : 0) | (s->color_mask[3] ? 8 : 0));
    fss.nsamplers = n[1];
    for (k = 0; k < n[1]; k++)
        fss.samp[k] = key[1][k];
    vss.nattrs = (unsigned char)(nslots < 8 ? nslots : 8);
    for (i = 0; i < vss.nattrs; i++) {
        GLuint g = p->slot_attrib[i];
        const struct OGLES2Array *a = g < OGLES2_MAX_VERTEX_ATTRIBS ? &ctx->array[g] : 0;
        vss.attr_format[i] = (unsigned short)(a && a->enabled ? vc4_attr_format(a->type, a->size, a->normalized)
                                                              : VC4K_R32G32B32A32_FLOAT);
    }
    vss.per_vertex_point_size = (unsigned char)(psize ? 1 : 0);
    vss.clip_halfz = (unsigned char)(s->clip_depth == GL_ZERO_TO_ONE_EXT); /* patch 0020 */
    vss.nsamplers = n[0];
    for (k = 0; k < n[0]; k++)
        vss.samp[k] = key[0][k];
    vc4_key_fs(kfs, &fss, 1);
    vc4_key_vs(kvs, &vss, 1);
}

/* ---- probe R: the key manifests ---- */

#define MAX_PROGRAMS 48
#define MAX_PAIRS 128

struct seen
{
    UBYTE hash[32];
    UWORD n;
    ULONG pair[MAX_PAIRS];
};

static struct seen *seen;
static BOOL told, dir_made;

/* The sampler entries from the textures as they are bound (the Pi 4's
 * draw has no VC4 storage: worked out from the formats). */
static void probe_samplers(struct OGLES2Context *ctx, struct OGLES2Program *p,
                           struct vc4_key_sampler key[2][OGLES2_MAX_STAGE_SAMPLERS], UBYTE n[2])
{
    struct OGLES2TexState *ts = texture_ts_fast(ctx);
    const struct OGLES2Image *im = p->image;
    UWORD u, k, m = list_samplers(p);
    int st;
    if (!ts)
        return;
    for (k = 0; k < m; k++) {
        const struct OGLES2ImageUniform *un;
        u = p->nsamplers == 0xfe ? k : p->samplers[k];
        un = &im->uniforms[u];
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER))
            continue;
        for (st = 0; st < 2; st++) {
            UWORD e, ne = un->array ? un->array : 1;
            for (e = 0; e < ne; e++) {
                ULONG idx = un->sampler[st] + e;
                int kind = ogles2_sampler_kind(un->type), unit = unit_of(p, u, e), first, last;
                const struct OGLES2Texture *t;
                const struct vc4_tex_format *f = 0;
                if (un->sampler[st] >= OGLES2_MAX_STAGE_SAMPLERS || idx >= OGLES2_MAX_STAGE_SAMPLERS)
                    break;
                t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit]
                                                                                      : ts->unit2d[unit];
                if (t && ogles2_texture_complete(t, &first, &last))
                    f = ogles2_vc4_storage(t);
                /* incomplete or not stored: the fallback, RGBA8 */
                key[st][idx].f = f ? f : vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE);
                key[st][idx].wrap_s = t ? t->wrap_s : GL_REPEAT;
                key[st][idx].wrap_t = t ? t->wrap_t : GL_REPEAT;
                if (f)
                    ogles2_vc4_sampler_emu(t, kind, first, last, &key[st][idx]);
                if (idx + 1 > n[st])
                    n[st] = (UBYTE)(idx + 1);
            }
        }
    }
}

void ogles2_vc4_record_draw(struct OGLES2Context *ctx, struct OGLES2Program *p, GLenum mode, int psize)
{
    static const char digits[] = "0123456789abcdef";
    struct vc4_key_sampler key[2][OGLES2_MAX_STAGE_SAMPLERS];
    UBYTE n[2] = {0, 0}, rec[VC4_KEY_FS_BYTES + VC4_KEY_VS_BYTES], cls = 0;
    const struct OGLES2Image *im = p->image;
    struct seen *e = 0;
    char path[64];
    ULONG h = 2166136261UL, i;
    BPTR fh;
    if (!im || im == ogles2_builtin_image())
        return;
    memset(key, 0, sizeof(key));
    probe_samplers(ctx, p, key, n);
    if (ctx->draw_fb) {
        const struct OGLES2Texture *t = ogles2_fbo_color_texture(ctx->draw_fb, 0);
        const struct vc4_tex_format *f = t ? ogles2_vc4_storage(t) : 0;
        cls = f && f->hw_type == VC4_TEX_RGB565 ? 2 : 0;
    } else {
        cls = ctx->display.rb_swap ? 1 : 0;
    }
    ogles2_vc4_draw_keys(ctx, p, mode, cls, (const struct vc4_key_sampler(*)[OGLES2_MAX_STAGE_SAMPLERS])key, n,
                         psize, rec, rec + VC4_KEY_FS_BYTES);
    for (i = 0; i < sizeof(rec); i++)
        h = (h ^ rec[i]) * 16777619UL;
    /* the program's set, by its hash */
    if (!seen && !(seen = AllocVec(sizeof(*seen) * MAX_PROGRAMS, MEMF_ANY | MEMF_CLEAR)))
        return;
    for (i = 0; i < MAX_PROGRAMS && !e; i++)
        if (seen[i].n && !memcmp(seen[i].hash, im->hash, 32))
            e = &seen[i];
    for (i = 0; i < MAX_PROGRAMS && !e; i++)
        if (!seen[i].n) {
            e = &seen[i];
            memcpy(e->hash, im->hash, 32);
        }
    if (!e)
        return; /* more programs than the probe keeps: the rest unrecorded */
    for (i = 0; i < e->n && i < MAX_PAIRS; i++)
        if (e->pair[i] == h)
            return;
    if (e->n < MAX_PAIRS)
        e->pair[e->n] = h;
    e->n++;
    if (!dir_made) {
        BPTR l = CreateDir((STRPTR) "T:ogles2-vc4keys");
        if (l)
            UnLock(l);
        dir_made = TRUE;
    }
    strcpy(path, "T:ogles2-vc4keys/");
    for (i = 0; i < 12; i++) {
        path[17 + 2 * i] = digits[im->hash[i] >> 4];
        path[18 + 2 * i] = digits[im->hash[i] & 15];
    }
    strcpy(path + 41, ".keys");
    if ((fh = Open((STRPTR)path, MODE_READWRITE)) != 0) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, rec, sizeof(rec));
        Close(fh);
    }
    if (!told) {
        told = TRUE;
        ogles2_log("probe R: the draws' VC4 key records go to T:ogles2-vc4keys/ (a file a program)");
    }
}
