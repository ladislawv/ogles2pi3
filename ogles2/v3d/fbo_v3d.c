/*
 * The V3D 4.2 backend's framebuffer targets (ogles2/backend.h,
 * docs/PI3_PLAN.md §5.1, P2 step 6): depth and stencil renderbuffers'
 * storage (UIF without XOR, Mesa's padding) and their surfaces, a
 * framebuffer object's tile size, depth type and tile pools. Moved from
 * ogles2/fbo.c, which keeps the objects, attachments and completeness.
 * Also the memory freed after a job (share.c, v3dp.c).
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "backend_v3d.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_hw.h"

#ifndef GL_STENCIL_INDEX8
#define GL_STENCIL_INDEX8 0x8D48
#endif

/* Depth and stencil: UIF without XOR, Mesa's padding for a UIF level. */
BOOL ogles2_v3d_rb_zs_storage(GLenum format, UWORD w, UWORD h, struct v3d_mem *mem, ULONG *base, ULONG *ub,
                              ULONG *bytes, UBYTE *cpp_out)
{
    ULONG cpp = format == GL_DEPTH_COMPONENT16 ? 2 : format == GL_STENCIL_INDEX8 ? 1 : 4;
    ULONG ub_w = 2 * v3d_utile_width((int)cpp), ub_h = 2 * v3d_utile_height((int)cpp);
    ULONG pw = (w + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ULONG ph = (h + ub_h - 1) / ub_h * ub_h;
    if (ogles2_mem_alloc(mem, pw * ph * cpp + 4096) < 0)
        return FALSE;
    *base = ((ULONG)mem->hostptr + 4095) & ~4095UL;
    *ub = ph / ub_h;
    *bytes = pw * ph * cpp;
    *cpp_out = (UBYTE)cpp;
    return TRUE;
}

/* A depth or stencil renderbuffer's image (ogles2_v3d_rb_zs_storage's) as a job's surface. */
void ogles2_v3d_rb_surface(GLenum format, ULONG base, ULONG ub, ULONG bytes, BOOL stencil_only,
                           struct OGLES2Surface *s)
{
    s->addr = base;
    s->ub = ub;
    s->bytes = bytes;
    s->mem_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
    s->format = stencil_only                         ? V3D_OUTPUT_IMAGE_FORMAT_S8
              : format == GL_DEPTH_COMPONENT16     ? V3D_OUTPUT_IMAGE_FORMAT_D16
                                                   : V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    s->force_alpha1 = FALSE;
}

/* A framebuffer object's job: the tile buffer's depth type, the widest
 * target (64 bits with a half-float one), and Mesa's v3d_choose_tile_size
 * for V3D 4.x: 64x64 for one 32-bit target, a step smaller for two, two
 * steps for three or four, and one more for 64-bit ones (64x32, 32x32,
 * 32x16); the hardware derives the same from the configuration packets. */
void ogles2_v3d_target_layout(struct OGLES2Target *t, UBYTE depth_format, BOOL wide)
{
    static const UBYTE sizes[] = {64, 64, 64, 32, 32, 32, 32, 16};
    int k;
    t->depth_type = depth_format == OGLES2_DEPTH_24 ? V3D_INTERNAL_TYPE_DEPTH24 : V3D_INTERNAL_TYPE_DEPTH16;
    t->bpp = wide ? V3D_INTERNAL_BPP_64 : V3D_INTERNAL_BPP_32;
    k = (t->nrt > 2 ? 2 : t->nrt > 1 ? 1 : 0) + (t->bpp == V3D_INTERNAL_BPP_64);
    t->tile_w = sizes[2 * k];
    t->tile_h = sizes[2 * k + 1];
}

/* Memory a job of `user` may still read (its open one, or the one left
 * running): freed when that job's frame has rendered. */
void ogles2_v3d_mem_free_after(struct OGLES2Context *user, struct v3d_mem *m)
{
    v3d_frame_defer_free(&user->v3d->frame, m);
}

/* Tile pools for w x h: the context's, if it has as many tiles; else fb's. */
BOOL ogles2_v3d_target_pools(struct OGLES2Context *ctx, struct v3d_mem *pool_alloc, struct v3d_mem *pool_state,
                             ULONG *pool_tiles, ULONG name, struct OGLES2Target *t)
{
    V3DContext *v = ctx->v3d;
    v3d_u32 zs, ts, ta, tx, ty;
    /* The job's tiles (smaller with several render targets), and as many
     * as the context's pools were made for (32x32 with 4x MSAA). */
    ULONG tiles = (ULONG)((t->w + t->tile_w - 1) / t->tile_w) * ((t->h + t->tile_h - 1) / t->tile_h);
    ULONG ct = ctx->msaa ? 32 : 64, mine = (ULONG)((v->width + ct - 1) / ct) * ((v->height + ct - 1) / ct);
    t->ta_addr = t->ta_size = t->ts_addr = 0;
    if (tiles <= mine && !t->want_own)
        return TRUE;
    if (*pool_tiles < tiles) {
        ogles2_job_finish(ctx); /* a running render may use the old pools */
        if (pool_alloc->hostptr)
            ogles2_mem_free(pool_alloc);
        if (pool_state->hostptr)
            ogles2_mem_free(pool_state);
        *pool_tiles = 0;
        /* sized for 32x32 tiles when the job's are smaller than 64x64; for
         * 32x16 ones (four 64-bit targets) as 32x32 over twice the height */
        v3d_frame_compute_pool_sizes(t->w, t->tile_h < 32 ? 2 * t->h : t->h, 16, t->tile_w < 64 || t->tile_h < 64,
                                     &zs, &ts, &ta, &tx, &ty);
        if (ogles2_mem_alloc(pool_alloc, ta + 4096) < 0)
            return FALSE;
        if (ogles2_mem_alloc(pool_state, ts + 4096) < 0) {
            ogles2_mem_free(pool_alloc);
            return FALSE;
        }
        {
            ULONG len = (ULONG)pool_alloc->size;
            CachePreDMA(pool_alloc->hostptr, &len, 0);
            len = (ULONG)pool_state->size;
            CachePreDMA(pool_state->hostptr, &len, 0);
        }
        *pool_tiles = tiles;
        ogles2_log("framebuffer %ld: tile pools of its own for %ldx%ld", name, (ULONG)t->w,
                   (ULONG)t->h);
    }
    t->ta_addr = ((ULONG)pool_alloc->hostptr + 4095) & ~4095UL;
    t->ta_size = (ULONG)pool_alloc->size - (t->ta_addr - (ULONG)pool_alloc->hostptr);
    t->ts_addr = ((ULONG)pool_state->hostptr + 4095) & ~4095UL;
    return TRUE;
}
