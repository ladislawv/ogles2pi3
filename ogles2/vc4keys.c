/*
 * Probe V (ENV:OGLES2/Probe): the shader variants the Raspberry Pi 3 would
 * need (docs/PI3_PLAN.md §4.2, P2), measured on the Pi 4. VC4 has no
 * fixed-function blending, texture swizzle or vertex format conversion, so
 * Mesa's vc4 driver compiles them into the shaders and its keys hold that
 * state. For every draw this builds the canonical VC4 keys the draw would
 * use and counts the distinct ones per program; each new key is logged with
 * the frame it first appeared in (a key first needed after the opening
 * frames is a stall on the Pi 3 while the on-target compiler runs), and a
 * summary per program when it is freed, and for the context at its end.
 *
 * Canonical (§4.2): only what changes the code. Fragment key: the primitive
 * class; per render target drawn (EXT_draw_buffers: one variant per target
 * on VC4) the target's format class; blending (equations and factors only
 * when on); the colour mask; depth and stencil enables (two-sided, full
 * write masks); MSAA, sample coverage, alpha to coverage; per sampler the
 * texture's format class and swizzle. Vertex key: per vertex input its type,
 * size and normalisation; points with gl_PointSize. The texture's format
 * class and swizzle are taken from what the Pi 4 stores (its hardware type
 * and swizzle): a stand-in for VC4's format and composed swizzle, which may
 * count a few formats apart that the Pi 3 would share (RGBA4 and RGBA8, say).
 *
 * MIT licence (LICENSE).
 */
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "ogles2_internal.h"

#define MAX_KEYS 64     /* per program and stage; more are counted, not kept */
#define LATE_FRAME 30   /* a key first seen at or after this frame is "late" */
#define KEY_BYTES 160

struct vc4keys {
    ULONG draws;
    UWORD nfs, nvs;     /* distinct keys seen (also past MAX_KEYS) */
    UWORD late_fs, late_vs;
    ULONG fs[MAX_KEYS], vs[MAX_KEYS]; /* their hashes */
};

/* FNV-1a, 32 bits. */
static ULONG hash(const UBYTE *b, int n)
{
    ULONG h = 2166136261UL;
    int i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619UL;
    }
    return h;
}

/* Adds h to the set; TRUE if it is new. */
static BOOL add(ULONG *set, UWORD *n, ULONG h)
{
    UWORD i, m = *n < MAX_KEYS ? *n : MAX_KEYS;
    for (i = 0; i < m; i++)
        if (set[i] == h)
            return FALSE;
    if (*n < MAX_KEYS)
        set[*n] = h;
    (*n)++;
    return TRUE;
}

static UBYTE blend_code(GLenum e)
{
    /* equations and factors as small numbers (their low bits differ) */
    return (UBYTE)((e & 0xff) ^ ((e >> 8) & 0xff));
}

static void log_key(struct OGLES2Context *ctx, const struct OGLES2Program *p, const char *stage, UWORD n,
                    const UBYTE *k, int len)
{
    char hex[2 * KEY_BYTES + 1];
    static const char digits[] = "0123456789abcdef";
    int i, j = 0;
    /* trailing zeros left out */
    while (len > 0 && !k[len - 1])
        len--;
    for (i = 0; i < len && j + 2 < (int)sizeof(hex); i++) {
        hex[j++] = digits[k[i] >> 4];
        hex[j++] = digits[k[i] & 15];
    }
    hex[j] = 0;
    ogles2_log("vc4keys: program %ld %s key %ld at frame %ld: %s", (ULONG)p->name, (ULONG)stage, (ULONG)n,
               ctx->frames, (ULONG)hex);
}

void ogles2_vc4keys_draw(struct OGLES2Context *ctx, struct OGLES2Program *p, const struct OGLES2Vc4KeyDraw *in)
{
    struct vc4keys *v = p->vc4keys;
    const struct OGLES2State *s = &ctx->state;
    UBYTE k[KEY_BYTES];
    int n, i, rt, logged = 0;

    if (!v) {
        v = AllocVec(sizeof(*v), MEMF_ANY | MEMF_CLEAR);
        if (!v)
            return;
        p->vc4keys = v;
        ctx->vc4k_programs++;
    }
    v->draws++;
    ctx->vc4k_draws++;

    /* Vertex key (the coordinate shader has the same one with is_coord). */
    memset(k, 0, sizeof(k));
    n = 0;
    k[n++] = in->psiz;
    k[n++] = in->nslots;
    for (i = 0; i < in->nslots && i < 16; i++) {
        k[n++] = in->attr[i][0];
        k[n++] = in->attr[i][1];
        k[n++] = in->attr[i][2];
    }
    for (i = 0; i < in->nsamp[0] && i < 16; i++) { /* vertex shader samplers */
        memcpy(&k[n], in->samp[0][i], 5);
        n += 5;
    }
    if (add(v->vs, &v->nvs, hash(k, n))) {
        ctx->vc4k_vs++;
        if (ctx->frames >= LATE_FRAME) {
            v->late_vs++;
            ctx->vc4k_late_vs++;
        }
        log_key(ctx, p, "vertex", v->nvs, k, n);
        logged = 1;
    }

    /* Fragment keys: one per render target drawn. */
    for (rt = 0; rt < OGLES2_MAX_DRAW_BUFFERS; rt++) {
        UBYTE hw = 0xfe, swz[4] = {0, 0, 0, 0};
        if (ctx->draw_fb) {
            if (!(ctx->fb_draw_mask & (1 << rt)))
                continue;
            if (!ogles2_texture_stored_of(ogles2_fbo_color_texture(ctx->draw_fb, rt), &hw, swz))
                hw = 0xfd;
        } else if (rt) {
            break; /* the default framebuffer: one target, the display's format (0xfe) */
        }
        memset(k, 0, sizeof(k));
        n = 0;
        k[n++] = in->prim;
        k[n++] = (UBYTE)rt;
        k[n++] = hw;
        memcpy(&k[n], swz, 4);
        n += 4;
        if (s->caps & OGLES2_CAP_BLEND) {
            k[n++] = 1;
            k[n++] = blend_code(s->blend_eq_rgb);
            k[n++] = blend_code(s->blend_eq_alpha);
            k[n++] = blend_code(s->blend_src_rgb);
            k[n++] = blend_code(s->blend_dst_rgb);
            k[n++] = blend_code(s->blend_src_alpha);
            k[n++] = blend_code(s->blend_dst_alpha);
        } else {
            n += 7;
        }
        k[n++] = (UBYTE)((s->color_mask[0] ? 1 : 0) | (s->color_mask[1] ? 2 : 0) | (s->color_mask[2] ? 4 : 0) |
                         (s->color_mask[3] ? 8 : 0));
        {
            BOOL stencil = (s->caps & OGLES2_CAP_STENCIL_TEST) && ctx->fb_stencil_bits;
            BOOL depth = ((s->caps & OGLES2_CAP_DEPTH_TEST) && ctx->fb_depth_bits) || stencil;
            k[n++] = (UBYTE)((depth ? 1 : 0) | (stencil ? 2 : 0));
            if (stencil) {
                BOOL twoside = s->stencil_func[0] != s->stencil_func[1] || s->stencil_fail[0] != s->stencil_fail[1] ||
                               s->stencil_zfail[0] != s->stencil_zfail[1] || s->stencil_zpass[0] != s->stencil_zpass[1] ||
                               s->stencil_ref[0] != s->stencil_ref[1] ||
                               s->stencil_value_mask[0] != s->stencil_value_mask[1] ||
                               s->stencil_write_mask[0] != s->stencil_write_mask[1];
                BOOL full = (s->stencil_write_mask[0] & 0xff) == 0xff && (s->stencil_write_mask[1] & 0xff) == 0xff;
                k[n - 1] |= (UBYTE)((twoside ? 4 : 0) | (full ? 8 : 0));
            }
        }
        {
            BOOL msaa = ctx->msaa && !ctx->draw_fb;
            k[n++] = (UBYTE)((msaa ? 1 : 0) | (msaa && (s->caps & OGLES2_CAP_SAMPLE_COVERAGE) ? 2 : 0) |
                             (msaa && (s->caps & OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE) ? 4 : 0));
        }
        for (i = 0; i < in->nsamp[1] && i < 16; i++) { /* fragment shader samplers */
            memcpy(&k[n], in->samp[1][i], 5);
            n += 5;
        }
        if (add(v->fs, &v->nfs, hash(k, n))) {
            ctx->vc4k_fs++;
            if (ctx->frames >= LATE_FRAME) {
                v->late_fs++;
                ctx->vc4k_late_fs++;
            }
            log_key(ctx, p, "fragment", v->nfs, k, n);
            logged = 1;
        }
    }
    /* New keys are rare: put them in the file now (the draw is the
     * application's call, as at context creation), so a full ring later
     * does not lose them. */
    if (logged)
        ogles2_log_flush();
}

void ogles2_vc4keys_free(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    struct vc4keys *v = p->vc4keys;
    (void)ctx;
    if (!v)
        return;
    ogles2_log("vc4keys: program %ld (%s): %ld draws, %ld fragment keys (%ld late), %ld vertex keys (%ld late)",
               (ULONG)p->name, (ULONG)(p->image && p->image->name ? p->image->name : "?"), v->draws, (ULONG)v->nfs,
               (ULONG)v->late_fs, (ULONG)v->nvs, (ULONG)v->late_vs);
    FreeVec(v);
    p->vc4keys = NULL;
}

void ogles2_vc4keys_report(struct OGLES2Context *ctx)
{
    if (!(ctx->probe & OGLES2_PROBE_VC4KEYS))
        return;
    ogles2_log("vc4keys: context: %ld frames, %ld draws, %ld programs drawn, %ld fragment keys (%ld first seen "
               "from frame %ld on), %ld vertex keys (%ld from frame %ld on)",
               ctx->frames, ctx->vc4k_draws, ctx->vc4k_programs, ctx->vc4k_fs, ctx->vc4k_late_fs, (ULONG)LATE_FRAME,
               ctx->vc4k_vs, ctx->vc4k_late_vs, (ULONG)LATE_FRAME);
}
