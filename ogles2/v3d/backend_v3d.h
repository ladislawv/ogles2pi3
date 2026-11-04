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
int ogles2_v3d_tex_generate(struct OGLES2Texture *t, int n);
int ogles2_v3d_tex_from_raster(struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w, UWORD h, BOOL bgra);

#endif
