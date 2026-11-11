/*
 * The V3D 4.2 backend's functions, for its table (backend_v3d.c).
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_BACKEND_V3D_H
#define OGLES2_BACKEND_V3D_H

#include <exec/types.h>

struct OGLES2Context;
struct OGLES2Target;
struct OGLES2DrawCall;
struct OGLES2Texture;
struct OGLES2Surface;
struct v3d_mem;
struct SignalSemaphore;
struct V3DContext;
struct OGLES2Image;
struct OGLES2Variant;
struct ogles2_rd;

/* device_v3d.c */
BOOL ogles2_v3d_open(void);
BOOL ogles2_v3d_ready(void);
void ogles2_v3d_reset(const char *why);
ULONG ogles2_v3d_bytes(void);
BOOL ogles2_v3d_gpu_reachable(ULONG addr, ULONG size);
struct SignalSemaphore *ogles2_v3d_gpu_lock(void);
struct V3DContext *ogles2_v3d_context_new(UWORD w, UWORD h, int depth_bits, BOOL msaa);
BOOL ogles2_v3d_context_resize(struct V3DContext *v, UWORD w, UWORD h);
void ogles2_v3d_context_free(struct V3DContext *v);
const char *ogles2_v3d_renderer(void);

/* job_v3d.c */
void ogles2_v3d_job_begin(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL bad);
BOOL ogles2_v3d_job_has_draws(struct OGLES2Context *ctx);
BOOL ogles2_v3d_job_keep_zs(struct OGLES2Context *ctx);
const char *ogles2_v3d_job_check(struct OGLES2Context *ctx);
BOOL ogles2_v3d_job_build(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL store_zs);
int ogles2_v3d_job_bin(struct OGLES2Context *ctx, const struct OGLES2Target *tg);
UBYTE ogles2_v3d_job_render(struct OGLES2Context *ctx);
int ogles2_v3d_wait_render(UBYTE frame);
void ogles2_v3d_job_retire(struct OGLES2Context *ctx);
void ogles2_v3d_job_end(struct OGLES2Context *ctx, BOOL submitted);
BOOL ogles2_v3d_job_can_queue(struct OGLES2Context *ctx);
void ogles2_v3d_job_queue(struct OGLES2Context *ctx);
UBYTE ogles2_v3d_job_render_queued(struct OGLES2Context *ctx);
void ogles2_v3d_job_report_stop(struct OGLES2Context *ctx);
void ogles2_v3d_job_free(struct OGLES2Context *ctx);

/* draw_v3d.c */
BOOL ogles2_v3d_draw(struct OGLES2Context *ctx, const struct OGLES2DrawCall *dc);
void ogles2_v3d_draw_free(struct OGLES2Context *ctx);

/* texture_v3d.c */
BOOL ogles2_v3d_tex_upload(struct OGLES2Texture *t);
void ogles2_v3d_tex_store(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w, int h);
BOOL ogles2_v3d_tex_load(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba, ULONG stride);
void ogles2_v3d_tex_free(struct OGLES2Texture *t);
BOOL ogles2_v3d_tex_rename(struct OGLES2Context *user, struct OGLES2Texture *t);
int ogles2_v3d_tex_levels(const struct OGLES2Texture *t);
BOOL ogles2_v3d_tex_holds(const struct OGLES2Texture *t, int k, int l);
BOOL ogles2_v3d_tex_can_generate(const struct OGLES2Texture *t, int n);
int ogles2_v3d_tex_generate(struct OGLES2Context *ctx, struct OGLES2Texture *t, int n);
int ogles2_v3d_tex_from_raster(struct OGLES2Context *ctx, struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w,
                               UWORD h, BOOL bgra);
BOOL ogles2_v3d_tex_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l, struct OGLES2Surface *s);
BOOL ogles2_v3d_tex_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l, struct OGLES2Surface *s);

/* fbo_v3d.c */
BOOL ogles2_v3d_rb_zs_storage(GLenum format, UWORD w, UWORD h, struct v3d_mem *mem, ULONG *base, ULONG *ub,
                              ULONG *bytes, UBYTE *cpp_out);
void ogles2_v3d_rb_surface(GLenum format, ULONG base, ULONG ub, ULONG bytes, BOOL stencil_only,
                           struct OGLES2Surface *s);
void ogles2_v3d_target_layout(struct OGLES2Target *t, UBYTE depth_format, BOOL wide);
BOOL ogles2_v3d_target_pools(struct OGLES2Context *ctx, struct v3d_mem *pool_alloc, struct v3d_mem *pool_state,
                             ULONG *pool_tiles, ULONG name, struct OGLES2Target *t);
void ogles2_v3d_mem_free_after(struct OGLES2Context *user, struct v3d_mem *m);

/* v3dp_v3d.c, program_v3d.c */
const char *ogles2_v3d_variant_read(struct ogles2_rd *s, struct OGLES2Variant *v);
const char *ogles2_v3d_variant_recipe(struct OGLES2Variant *v, BOOL *clip_z, ULONG *bad);
const struct OGLES2Variant *ogles2_v3d_variant_choose(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                      GLenum mode);
const struct OGLES2Image *ogles2_v3d_builtin_image(void);
const UBYTE *ogles2_v3d_internal_program(int which, ULONG *len);

#endif
