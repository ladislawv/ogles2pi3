/*
 * The VC4 backend's shared definitions (ogles2/vc4/, docs/PI3_PLAN.md §5.1,
 * P5): a context's object (ctx->v3d holds it), the groups' functions.
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_BACKEND_VC4_H
#define OGLES2_BACKEND_VC4_H

#include <exec/types.h>
#include <GLES2/gl2.h>
#include "vc4core.h"
#include "key_vc4.h"

struct OGLES2Variant;
struct ogles2_rd;
struct OGLES2Image;
struct OGLES2Context;
struct OGLES2Target;
struct OGLES2Texture;
struct OGLES2Surface;
struct OGLES2DrawCall;
struct v3d_mem;

/* A VC4 variant's prog data (v3dp/FORMAT.md 1.1, VC4_VARIANT), kept in
 * struct OGLES2Variant's be[] (static_assert'd to fit). */
struct vc4_prog
{
    UBYTE fs_threaded, num_inputs, vattrs_live, disable_early_z;
    UBYTE vattr_offsets[9];      /* per attribute, and [8] the total: the VPM layout */
    UBYTE nfs_inputs;            /* the fragment shader's inputs (slot, swizzle pairs): a fragment
                                  * variant's own; a vertex variant's key's (those it writes) */
    UWORD pad;
    ULONG color_inputs, num_texture_samples;
    const UBYTE *fs_inputs;      /* nfs_inputs pairs, in the file */
};

#define VC4_PROG(v) ((struct vc4_prog *)(v)->be)
#define VC4_PROG_C(v) ((const struct vc4_prog *)(v)->be)

/* vc4's enum quniform_contents at the validated commit (FORMAT.md 1.1). */
enum {
    VC4_U_CONSTANT, VC4_U_UNIFORM, VC4_U_VIEWPORT_X_SCALE, VC4_U_VIEWPORT_Y_SCALE, VC4_U_VIEWPORT_Z_OFFSET,
    VC4_U_VIEWPORT_Z_SCALE, VC4_U_TEXTURE_CONFIG_P0, VC4_U_TEXTURE_CONFIG_P1, VC4_U_TEXTURE_CONFIG_P2,
    VC4_U_TEXTURE_FIRST_LEVEL, VC4_U_TEXTURE_MSAA_ADDR, VC4_U_UBO0_ADDR, VC4_U_UBO1_ADDR,
    VC4_U_TEXRECT_SCALE_X, VC4_U_TEXRECT_SCALE_Y, VC4_U_TEXTURE_BORDER_COLOR, VC4_U_BLEND_CONST_COLOR_X,
    VC4_U_BLEND_CONST_COLOR_Y, VC4_U_BLEND_CONST_COLOR_Z, VC4_U_BLEND_CONST_COLOR_W,
    VC4_U_BLEND_CONST_COLOR_RGBA, VC4_U_BLEND_CONST_COLOR_AAAA, VC4_U_STENCIL, VC4_U_SAMPLE_MASK,
    VC4_U_UNIFORMS_ADDRESS
};

/* Blocks freed once the job that may read them has rendered. */
struct vc4_defer
{
    struct vc4_defer *next;
    ULONG size, handle, busaddr;
    APTR hostptr;              /* struct v3d_mem's fields */
};

#define VC4_SLOTS 2 /* a job built while the last one renders */

/* A context's object (ctx->v3d): the binner's pools for its default
 * framebuffer, two jobs (one built while the other renders), the blocks
 * each frees after its render, the open job's target and draw state. */
struct vc4_ctx
{
    struct vc4_pools pools;
    struct vc4_job job[VC4_SLOTS];
    struct vc4_defer *defer[VC4_SLOTS];
    UBYTE build;               /* the slot the open job builds in */
    UBYTE rendering;           /* the slot last submitted */
    BOOL has_draws, bad;       /* the open job: drew; cannot render (a draw ran out of memory) */
    const char *why_bad;
    UWORD w, h;                /* the default framebuffer */
    int depth_bits;
    BOOL msaa;
    UWORD frame_w;             /* the open job's frame width: the bitmap's row in pixels */
    struct vc4_pools fbo_pools; /* a framebuffer object's own pools, as the job uses them */
    struct vc4_target target;  /* the open job's */
    BOOL fbo;                  /* the open job draws into a framebuffer object */
    UBYTE color_format;        /* the key's target format: 0 RGBA8 (R first in memory), 1 BGRA8, 2 RGB565 */
    /* depth and stencil kept across a mid-frame flush (job_keep_zs) */
    struct vc4_mem zs;
    UWORD zs_w, zs_h;
    /* the draw's */
    struct vc4_draw *draw;
};

#define VC4_CTX(ctx) ((struct vc4_ctx *)(ctx)->v3d)

/* backend_vc4.c (ogles2_vc4_present: backend.h) */
void ogles2_vc4_mem_free_after(struct OGLES2Context *user, struct v3d_mem *m);
ULONG ogles2_vc4_bus(ULONG cpu); /* a CPU address in memory the GPU reaches, as its bus address */

/* job_vc4.c */
void ogles2_vc4_job_begin(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL bad);
BOOL ogles2_vc4_job_has_draws(struct OGLES2Context *ctx);
BOOL ogles2_vc4_job_keep_zs(struct OGLES2Context *ctx);
const char *ogles2_vc4_job_check(struct OGLES2Context *ctx);
BOOL ogles2_vc4_job_build(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL store_zs);
int ogles2_vc4_job_bin(struct OGLES2Context *ctx, const struct OGLES2Target *tg);
UBYTE ogles2_vc4_job_render(struct OGLES2Context *ctx);
int ogles2_vc4_wait_render(UBYTE frame);
void ogles2_vc4_job_retire(struct OGLES2Context *ctx);
void ogles2_vc4_job_end(struct OGLES2Context *ctx, BOOL submitted);
void ogles2_vc4_job_report_stop(struct OGLES2Context *ctx);
void ogles2_vc4_job_free(struct OGLES2Context *ctx);
void ogles2_vc4_defer_free_all(struct vc4_ctx *c, int slot);

/* draw_vc4.c */
BOOL ogles2_vc4_draw(struct OGLES2Context *ctx, const struct OGLES2DrawCall *dc);
void ogles2_vc4_draw_free(struct OGLES2Context *ctx);

/* texture_vc4.c */
BOOL ogles2_vc4_tex_upload(struct OGLES2Texture *t);
void ogles2_vc4_tex_store(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w, int h);
BOOL ogles2_vc4_tex_load(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *dst,
                         ULONG stride);
void ogles2_vc4_tex_free(struct OGLES2Texture *t);
BOOL ogles2_vc4_tex_rename(struct OGLES2Context *user, struct OGLES2Texture *t);
int ogles2_vc4_tex_levels(const struct OGLES2Texture *t);
BOOL ogles2_vc4_tex_holds(const struct OGLES2Texture *t, int k, int l);
BOOL ogles2_vc4_tex_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s);
BOOL ogles2_vc4_tex_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l, struct OGLES2Surface *s);
/* A draw's sampler: the texture unit's texture of its kind (OGLES2_TEX_*),
 * complete or the fallback, its configuration words P0, P1, P2, its
 * storage for the key (FALSE: none at all). */
BOOL ogles2_vc4_sampler(struct OGLES2Context *ctx, int unit, int kind, ULONG p[3],
                        const struct vc4_tex_format **f, GLenum *wrap_s, GLenum *wrap_t, ULONG *first);

/* drawkey_vc4.c: VC4's storage of a texture (its level 0's format and
 * type; 0: none yet); the stencil test's uniform words, two-sidedness and
 * whether the third word is needed; a draw's key records (key_vc4.h) for a
 * target of colour class color_class (0 RGBA8, 1 BGRA8, 2 RGB565) and the
 * samplers' entries per stage. */
const struct vc4_tex_format *ogles2_vc4_storage(const struct OGLES2Texture *t);
struct OGLES2State;
struct OGLES2Program;
void ogles2_vc4_stencil_words(const struct OGLES2State *s, ULONG w[3], BOOL *twoside, BOOL *full);
void ogles2_vc4_draw_keys(struct OGLES2Context *ctx, const struct OGLES2Program *p, GLenum mode, UBYTE color_class,
                          const struct vc4_key_sampler key[2][16], const UBYTE n[2], int psize, UBYTE *kfs, UBYTE *kvs);

/* fbo_vc4.c */
BOOL ogles2_vc4_rb_zs_storage(GLenum format, UWORD w, UWORD h, struct v3d_mem *mem, ULONG *base, ULONG *ub,
                              ULONG *bytes, UBYTE *cpp);
void ogles2_vc4_rb_surface(GLenum format, ULONG base, ULONG ub, ULONG bytes, BOOL stencil_only,
                           struct OGLES2Surface *s);
void ogles2_vc4_target_layout(struct OGLES2Target *t, UBYTE depth_format, BOOL wide);
BOOL ogles2_vc4_target_pools(struct OGLES2Context *ctx, struct v3d_mem *alloc, struct v3d_mem *state,
                             ULONG *tiles, ULONG name, struct OGLES2Target *t);

/* program_vc4.c */
const char *ogles2_vc4_variant_read(struct ogles2_rd *s, struct OGLES2Variant *v);
const char *ogles2_vc4_variant_recipe(struct OGLES2Variant *v, BOOL *clip_z, ULONG *bad);
const struct OGLES2Variant *ogles2_vc4_variant_choose(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                      GLenum mode);
/* The variant of the stage whose key is `key` (a key record's key, after its
 * stage: key_vc4.h), wraps and the comparison function aside (they do not
 * change the code: §4.2); a vertex or coordinate one also feeding fs's
 * inputs (vertex) and taking either gl_PointSize flag when points (exact
 * first). 0: none in the image. */
const struct OGLES2Variant *ogles2_vc4_variant_find(const struct OGLES2Image *im, UBYTE stage, const UBYTE *key,
                                                    const struct OGLES2Variant *fs, BOOL points);

#endif
