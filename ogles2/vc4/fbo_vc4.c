/*
 * The VC4 backend's framebuffer targets (ogles2/backend.h, docs/PI3_PLAN.md
 * §5.1, P5): depth and stencil renderbuffers' storage and surfaces, a
 * framebuffer object's tile size and tile pools.
 *
 * The tile buffer holds depth and stencil together (Z24S8), and its loads
 * and stores move them together: every depth or stencil renderbuffer is a
 * Z24S8 image in T (or, small, LT) tiling, whatever its format (Mesa's
 * state tracker picks Z24S8 for VC4 the same way). A framebuffer object's
 * job uses its depth attachment's image, else its stencil's; with separate
 * depth and stencil renderbuffers the stencil lives in the depth one's.
 * OGLES2Surface's V3D fields are this backend's: mem_format the tiling
 * (VC4_TILING_*), format the colour format (VC4_COLOR_*), ub unused; a
 * renderbuffer's ub holds its tiling.
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "vc4core.h"
#include "v3d_types.h"
#include "backend_vc4.h"

BOOL ogles2_vc4_rb_zs_storage(GLenum format, UWORD w, UWORD h, struct v3d_mem *mem, ULONG *base, ULONG *ub,
                              ULONG *bytes, UBYTE *cpp)
{
    struct vc4_texture_layout lay;
    (void)format;
    vc4_texture_layout(&lay, w, h, 4, 1, 1);
    if (ogles2_mem_alloc(mem, lay.size) < 0)
        return FALSE;
    *base = (ULONG)mem->hostptr; /* page aligned (vc4core's blocks) */
    *ub = lay.level[0].tiling;
    *bytes = lay.size;
    *cpp = 4;
    return TRUE;
}

void ogles2_vc4_rb_surface(GLenum format, ULONG base, ULONG ub, ULONG bytes, BOOL stencil_only,
                           struct OGLES2Surface *s)
{
    (void)format;
    (void)stencil_only;
    s->addr = base;
    s->bytes = bytes;
    s->mem_format = (UBYTE)ub;
    s->format = 0;
    s->force_alpha1 = FALSE;
    s->ub = 0;
}

/* 64x64 tiles, 64x32 with a half-float attachment (the tile buffer's 64-bit
 * mode, §2.1): bpp 1 on VC4 says so. Each pass takes the mode of its own
 * attachment (job_vc4.c); the pools are sized for the taller count. */
void ogles2_vc4_target_layout(struct OGLES2Target *t, UBYTE depth_format, BOOL wide)
{
    (void)depth_format;
    t->tile_w = 64;
    t->tile_h = wide ? 32 : 64;
    t->bpp = wide ? 1 : 0;
    t->depth_type = 0;
}

/* The context's pools when the object's frame has no more tiles across and
 * down; else pools of its own, in one block of GPU memory laid out as
 * vc4core's (vc4_pools_layout): the tile state at ts_addr, the tile
 * allocation at ta_addr (ta_size bytes), the overflow memory after it. */
BOOL ogles2_vc4_target_pools(struct OGLES2Context *ctx, struct v3d_mem *alloc, struct v3d_mem *state,
                             ULONG *tiles, ULONG name, struct OGLES2Target *t)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    UWORD tx = (UWORD)((t->w + 63) / 64), ty = (UWORD)((t->h + t->tile_h - 1) / t->tile_h);
    ULONG need = (ULONG)tx * ty, tsda, talloc, total;
    (void)state;
    t->ta_addr = t->ta_size = t->ts_addr = 0;
    if (tx <= c->pools.tiles_x && ty <= c->pools.tiles_y)
        return TRUE;
    vc4_pools_layout(need, &tsda, &talloc, &total);
    if (*tiles < need) {
        ogles2_job_finish(ctx); /* a running render may use the old pools */
        if (alloc->hostptr)
            ogles2_mem_free(alloc);
        *tiles = 0;
        if (ogles2_mem_alloc(alloc, total) < 0)
            return FALSE;
        if ((alloc->busaddr & 0xf0000000UL) != ((alloc->busaddr + alloc->size - 1) & 0xf0000000UL)) {
            ogles2_mem_free(alloc); /* the binner keeps the top four address bits */
            return FALSE;
        }
        *tiles = need;
        ogles2_log("framebuffer %ld: tile pools of its own for %ldx%ld", name, (ULONG)t->w, (ULONG)t->h);
    }
    t->ts_addr = (ULONG)alloc->hostptr;
    t->ta_addr = (ULONG)alloc->hostptr + tsda;
    t->ta_size = talloc;
    return TRUE;
}
