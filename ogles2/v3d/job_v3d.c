/*
 * The V3D 4.2 backend's jobs (ogles2/backend.h, docs/PI3_PLAN.md §5.1, P2
 * step 3): the control lists, the render lists of the default framebuffer
 * and of a framebuffer object, depth and stencil kept across a mid-frame
 * flush, submission and the waits. Moved from ogles2/job.c, which keeps
 * what is the same on every GPU: the job's serial, the clear snapshot, the
 * locks, the overlap with the CPU (M6), the statistics.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/execbase.h>
#include <proto/exec.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "backend_v3d.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_submit_timeout.h"
#include "v3d_regs.h"

static void supertiles(V3DContext *v, ULONG w, ULONG h, ULONG sw, ULONG sh, ULONG tw, ULONG th);

/* ---- the binning list grows (plan section 9, M4) ----
 * Its block is fixed (V3D_INITIAL_CL_BUFFER_SIZE, 4 MB), and a frame with
 * enough draws outgrows it. Instead of dropping the frame, a claim that
 * does not fit ends the block with a BRANCH to a new one, as Mesa's
 * v3d_cl_ensure_space does; the list is then submitted from the first
 * block to the end of the last. BRANCH_ROOM bytes past `capacity` are kept
 * free in every block for that packet. The new blocks are freed when the
 * frame's render has finished (v3d_frame_defer_free). */
#define BRANCH_ROOM 8                         /* BRANCH: opcode + 32-bit address */
#define CHAIN_BLOCK (1024UL * 1024)           /* the least for a further block */

static void *grow_binning(v3d_static_buffer *buf, v3d_uintptr size)
{
    struct OGLES2Context *ctx = buf->grow_data;
    V3DContext *v = ctx->v3d;
    v3d_mem m;
    UBYTE *b;
    /* Each block twice the last: few blocks even for huge frames (the
     * frame's retire list holds V3D_RETIRE_MAX). */
    ULONG addr, len, want = (ULONG)buf->capacity * 2;
    if (want < CHAIN_BLOCK)
        want = CHAIN_BLOCK;
    if (want < (ULONG)size + BRANCH_ROOM + 256)
        want = (ULONG)size + BRANCH_ROOM + 256;
    if (ogles2_mem_alloc(&m, want) < 0)
        return 0; /* out of memory: the frame is dropped as before */
    /* BRANCH, little-endian, in the room kept free after `used`. */
    b = buf->start + buf->used;
    addr = (ULONG)m.hostptr;
    b[0] = v3d_OP_BRANCH;
    b[1] = (UBYTE)addr;
    b[2] = (UBYTE)(addr >> 8);
    b[3] = (UBYTE)(addr >> 16);
    b[4] = (UBYTE)(addr >> 24);
    /* The old block is finished: make it visible to the GPU now
     * (v3d_context_flush_for_dma flushes only the current block). */
    len = (ULONG)buf->used + 5;
    CachePreDMA(buf->start, &len, 0);
    if (ctx->chained_blocks++ < 8)
        ogles2_log("job: binning list block full at 0x%08lx + %ld: BRANCH to a new block at 0x%08lx (%ld bytes)",
                   (ULONG)buf->start, (LONG)buf->used, addr, (LONG)m.size);
    v3d_frame_defer_free(&v->frame, &m); /* freed after this frame's render */
    buf->start = (v3d_u8 *)m.hostptr;
    buf->used = 0;
    /* 256 bytes of slack for the control list reader's prefetch. */
    buf->capacity = (int)m.size - BRANCH_ROOM - 256;
    return v3d_buffer_claim_memory(buf, size);
}

/* ---- depth and stencil across a mid-frame flush (plan 7.2) ----
 * A job ends before its frame when the program reads pixels, copies them,
 * or calls glFlush/glFinish. Such a job stores depth and stencil (D24S8 as
 * one buffer) to memory of its own, and the next job of the frame loads
 * them instead of clearing early, as Mesa does for any surface outliving a
 * job. Only the GPU reads this memory, so its layout is the GPU's: UIF
 * without XOR, padded as Mesa pads a UIF level (v3d_setup_slices). At
 * aglSwapBuffers they are not kept (EGL: undefined after a swap). */
struct zs_save
{
    v3d_mem mem;
    ULONG base;            /* 4096-aligned within mem */
    ULONG w, h;            /* the framebuffer it was made for */
    BOOL msaa;             /* all four samples: twice the width and height (Mesa's msaa_scale) */
    ULONG height_in_ub;    /* padded height in UIF blocks */
    UBYTE buffer, format;  /* v3d_ZSTENCIL/v3d_Z, V3D_OUTPUT_IMAGE_FORMAT_* */
};

static struct zs_save *zs_get(struct OGLES2Context *ctx)
{
    struct zs_save *z = ctx->zs_save;
    V3DContext *v = ctx->v3d;
    ULONG cpp, ub_w, ub_h, pw, ph;
    if (!ctx->depth_bits && !ctx->stencil_bits)
        return 0;
    if (z && z->w == v->width && z->h == v->height && z->msaa == ctx->msaa)
        return z;
    if (z) {
        ogles2_mem_free(&z->mem);
        FreeVec(z);
        ctx->zs_save = 0;
    }
    if (!(z = AllocVec(sizeof(*z), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    cpp = ctx->depth_format == OGLES2_DEPTH_16 ? 2 : 4;
    ub_w = 2 * v3d_utile_width((int)cpp);
    ub_h = 2 * v3d_utile_height((int)cpp);
    pw = ((ULONG)v->width * (ctx->msaa ? 2 : 1) + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ph = ((ULONG)v->height * (ctx->msaa ? 2 : 1) + ub_h - 1) / ub_h * ub_h;
    if (ogles2_mem_alloc(&z->mem, pw * ph * cpp + 4096) < 0) {
        FreeVec(z);
        return 0;
    }
    z->base = ((ULONG)z->mem.hostptr + 4095) & ~4095UL;
    z->w = v->width;
    z->h = v->height;
    z->msaa = ctx->msaa;
    z->height_in_ub = ph / ub_h;
    z->buffer = ctx->depth_format == OGLES2_DEPTH_24 ? v3d_ZSTENCIL : v3d_Z;
    z->format = ctx->depth_format == OGLES2_DEPTH_16 ? V3D_OUTPUT_IMAGE_FORMAT_D16
              : ctx->depth_format == OGLES2_DEPTH_24 ? V3D_OUTPUT_IMAGE_FORMAT_D24S8
                                                     : V3D_OUTPUT_IMAGE_FORMAT_D32F;
    ctx->zs_save = z;
    return z;
}

/* aglDestroyContext: the depth and stencil memory. */
void ogles2_v3d_job_free(struct OGLES2Context *ctx)
{
    struct zs_save *z = ctx->zs_save;
    if (z) {
        ogles2_mem_free(&z->mem);
        FreeVec(z);
        ctx->zs_save = 0;
    }
}

/* The job's lists (ogles2_job_begin): tg the framebuffer object's target,
 * NULL for the default framebuffer and for an object without tile pools
 * (bad: the job is dropped at the flush). */
void ogles2_v3d_job_begin(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL bad)
{
    V3DContext *v = ctx->v3d;
    UWORD w = v->width, h = v->height;
    UBYTE nrt = 1, bpp = V3D_INTERNAL_BPP_32, tw, th;
    v3d_u8 s;

    /* An object's job with several render targets (EXT_draw_buffers) or a
     * 64-bit one (EXT_color_buffer_half_float) has smaller tiles as well:
     * ogles2_fbo_target's. 4x MSAA on the default framebuffer only (plan
     * 7.2): 32x32 tiles, Mesa's v3d_choose_tile_size for one 32-bpp target;
     * the hardware derives the same from the configuration packets. */
    if (tg) {
        w = tg->w;
        h = tg->h;
        nrt = tg->nrt;
        bpp = tg->bpp;
        tw = tg->tile_w;
        th = tg->tile_h;
    } else {
        tw = ctx->job_msaa ? 32 : 64;
        th = ctx->job_msaa ? 32 : 64;
    }
    v->frame.tilesX = (w + tw - 1) / tw;
    v->frame.tilesY = (h + th - 1) / th;
    s = v->build_slot;
    v3d_cl_reset(&v->tile_list_mem[s], &v->tile_list_buf[s]);
    v3d_cl_reset(&v->binning_mem[s], &v->binning_buf[s]);
    /* Back to the first block (a chained frame left buf->start in its last). */
    v->binning_buf[s].start = (v3d_u8 *)v->binning_mem[s].hostptr;
    v->binning_buf[s].capacity -= BRANCH_ROOM;
    v->binning_buf[s].grow = grow_binning;
    v->binning_buf[s].grow_data = ctx;
    v3d_cl_reset(&v->render_mem[s], &v->render_buf[s]);
    v3d_cl_reset(&v->state_mem[s], &v->state_buf[s]);

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    /* As Mesa's v3d_start_binning (v3dx_draw.c). */
    NumberOfLayers(v, 1);
    TileBinningModeCfg(v, v3d_TILE_ALLOCATION_INITIAL_BLOCK_SIZE_64B,
                       v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, nrt, bpp, ctx->job_msaa, FALSE, w, h);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);
    OcclusionQueryCounter(v, 0);
    DoCommand(v, v3d_OP_START_TILE_BINNING);

    v->frame_active = TRUE;
    v->ez_state = V3D_EZ_UNDECIDED;
    v->first_ez_state = V3D_EZ_UNDECIDED;
    v->frame_corrupted = bad;
    v->draw_state_configured = FALSE;
}

static v3d_u8 to_byte(GLfloat f)
{
    return (v3d_u8)(f * 255.0f + 0.5f); /* f is clamped to [0,1] by glClearColor */
}

/* The colour clear value as the tile buffer's 8-bit ARGB: the glClear's
 * (ogles2_job_clear keeps it as GL gave it). */
static ULONG clear_argb(const struct OGLES2Context *ctx)
{
    const GLfloat *c = ctx->job_clear_rgba;
    return ((ULONG)to_byte(c[3]) << 24) | ((ULONG)to_byte(c[0]) << 16) | ((ULONG)to_byte(c[1]) << 8) |
           to_byte(c[2]);
}

/* Builds the tile list and the render list. The display must be locked. */
static void build_render(struct OGLES2Context *ctx, struct zs_save *store_zs)
{
    V3DContext *v = ctx->v3d;
    struct OGLES2Display *d = &ctx->display;
    v3d_u8 s = v->build_slot;
    UWORD width = v->width, height = v->height;
    ULONG tilesX = v->frame.tilesX, tilesY = v->frame.tilesY;
    ULONG start, end, sw = 1, sh = 1, fw, fh;
    UBYTE clear_color = ctx->job_clear_color;
    UBYTE ez = v->first_ez_state;
    UBYTE ez_off = (ez == V3D_EZ_DISABLED) || (width & 1) || (height & 1); /* GFXH-1918 */
    UBYTE depth_type = ctx->depth_format == OGLES2_DEPTH_16 ? V3D_INTERNAL_TYPE_DEPTH16
                     : ctx->depth_format == OGLES2_DEPTH_24 ? V3D_INTERNAL_TYPE_DEPTH24
                                                            : V3D_INTERNAL_TYPE_DEPTH32F;
    ULONG argb, abgr;
    UBYTE flip_y = (ctx->probe & OGLES2_PROBE_FLIP_Y) ? TRUE : FALSE;
    /* 4x MSAA: the colour stored resolved (the four samples averaged), the
     * colour loaded into every sample; depth kept across a flush with all
     * four (Mesa v3dx_rcl.c's decimate modes). */
    UBYTE msaa = ctx->msaa;
    UBYTE zs_decimate = msaa ? V3D_DECIMATE_MODE_ALL_SAMPLES : V3D_DECIMATE_MODE_SAMPLE_0;
    /* Depth and stencil from the frame's previous job, unless this one
     * clears them anyway. */
    struct zs_save *load_zs = ctx->zs_saved && !ctx->job_clear_zs ? zs_get(ctx) : 0;
    /* Mesa v3dx_rcl.c: the early Z/S clear only without a Z/S load or store;
     * otherwise the clear packets clear Z/S as well. */
    UBYTE early_zs_clear = !(load_zs || store_zs);
    if (store_zs && !ctx->zs_logged) {
        ctx->zs_logged = TRUE;
        ogles2_log("job: depth and stencil kept across a mid-frame flush (logged once)");
    }

    /* Finish binning. Probe S: the binner signals the render thread, which
     * waits for it (VC4's protocol; packets 7 and 8 exist on V3D 4.2 too). */
    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_INCREMENT_SEMAPHORE);
    DoCommand(v, v3d_OP_FLUSH);

    /* Generic tile list: load colour unless it is cleared, store it to the
     * render bitmap. Depth: early clear, no load, no store (see the top). */
    v->current_buf = &v->tile_list_buf[s];
    SetBuffer(v, &v->tile_list_buf[s]);
    start = CurrentBufferAddress(v);
    DoCommand(v, v3d_OP_TILE_COORDINATES_IMPLICIT);
    /* Probe Y: Flip Y on both, with the height it needs (Mesa's XML: "used
     * for y flip"). */
    if (!clear_color)
        LoadTileBufferGeneral(v, v3d_RENDER_TARGET_0, V3D_MEMORY_FORMAT_RASTER, flip_y,
                              V3D_DECIMATE_MODE_SAMPLE_0, V3D_OUTPUT_IMAGE_FORMAT_RGBA8, FALSE, FALSE,
                              d->rb_swap, d->bprow, flip_y ? height : 0, d->base);
    if (load_zs)
        LoadTileBufferGeneral(v, load_zs->buffer, V3D_MEMORY_FORMAT_UIF_NO_XOR, FALSE,
                              zs_decimate, load_zs->format, FALSE, FALSE, FALSE,
                              load_zs->height_in_ub, 0, load_zs->base);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    PrimListFormat(v, v3d_LIST_TRIANGLES, FALSE);
    SetInstanceid(v, 0);
    BranchToImplicitTileList(v, 0);
    /* No dithering into the RGBA8 framebuffer, whatever GL_DITHER says (GLES2
     * leaves dithering to the implementation): the tile buffer holds 8 bits
     * per channel, so there is nothing finer to dither from, and Mesa never
     * sets the store's dither mode. PiStorm3D dithered when GL_DITHER was on;
     * on the A1200 that store turned 0xff into 0x00 and 0xbf into 0xc0 (M1
     * run 2). Probe D (ENV:OGLES2/Probe) brings it back, for that probe only.
     * Dithering matters for 16-bit targets (plan 7.7), which come later. */
    StoreTileBufferGeneral(v, v3d_RENDER_TARGET_0, V3D_MEMORY_FORMAT_RASTER, flip_y,
                           (ctx->probe & OGLES2_PROBE_STORE_DITHER) ? V3D_DITHER_MODE_RGBA : V3D_DITHER_MODE_NONE,
                           msaa ? V3D_DECIMATE_MODE_4X : V3D_DECIMATE_MODE_SAMPLE_0,
                           V3D_OUTPUT_IMAGE_FORMAT_RGBA8, FALSE, FALSE,
                           d->rb_swap, d->bprow, flip_y ? height : 0, d->base);
    if (store_zs)
        StoreTileBufferGeneral(v, store_zs->buffer, V3D_MEMORY_FORMAT_UIF_NO_XOR, FALSE,
                               V3D_DITHER_MODE_NONE, zs_decimate, store_zs->format,
                               FALSE, FALSE, FALSE, store_zs->height_in_ub, 0, store_zs->base);
    /* Mesa v3dx_rcl.c, GFXH-1461/GFXH-1689: on 4.2 the per-store clear bit is
     * broken for Z/S, so clear with this packet, RTs bit set; Z/S bit clear
     * because the early Z/S clear does that. */
    if (clear_color || ctx->job_clear_zs)
        ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_RETURN_FROM_SUB_LIST);
    end = CurrentBufferAddress(v);

    /* Render list. */
    v->current_buf = &v->render_buf[s];
    SetBuffer(v, &v->render_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_WAIT_ON_SEMAPHORE);
    TileRenderingModeCFGCommon(v, 1, width, height, v3d_RENDER_TARGET_MAXIMUM_32BPP, msaa, FALSE,
                               ez == V3D_EZ_GT_GE ? v3d_EARLY_Z_DIRECTION_GT_GE
                                                  : v3d_EARLY_Z_DIRECTION_LT_LE,
                               ez_off, depth_type,
                               /* Mesa: the early Z/S clear only without a Z/S load or
                                * store (with it, the stored depth was the clear value,
                                * A1200 run ogles2-m4-1); otherwise the dummy tile and
                                * the end-of-tile clear below clear the tile buffer */
                               early_zs_clear);
    argb = clear_argb(ctx);
    abgr = (argb & 0xFF00FF00UL) | ((argb & 0x00FF0000UL) >> 16) | ((argb & 0x000000FFUL) << 16);
    TileRenderingModeCFGClearColorsPart1(v, 0, abgr);
    if (ctx->probe & OGLES2_PROBE_LOG_CLEAR)
        ogles2_log("job: frame %lu, clear %s, ARGB %08lx, packet ABGR %08lx", ctx->frames,
                   clear_color ? "colour" : "no colour", argb, abgr);
    TileRenderingModeCFGColor(v, 1, V3D_INTERNAL_BPP_32, V3D_INTERNAL_TYPE_8, 0);
    TileRenderingModeCFGZSClearValues(v, ctx->job_clear_zs ? ctx->job_clear_stencil : 0,
                                      ctx->job_clear_zs ? ctx->job_clear_depth : 1.0f);
    TileListInitialBlockSize(v, v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, TRUE);

    /* Supertiles: fewer than 256 in the frame (Mesa v3dx_rcl.c). */
    for (;;) {
        fw = (tilesX + sw - 1) / sw;
        fh = (tilesY + sh - 1) / sh;
        if (fw * fh < 256)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }
    Multicore_Rendering_Tile_List_Set_Base(v, (ULONG)v->tile_alloc_mem.hostptr, 0);
    MulticoreRenderingSupertileCFG(v, sw, sh, fw, fh, 1, FALSE, FALSE, tilesY, tilesX);

    /* Mesa's two dummy tiles before the real ones (v3dx_rcl.c). */
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);

    StartAddressOfGenericTileList(v, start, end);
    supertiles(v, width, height, sw, sh, msaa ? 32 : 64, msaa ? 32 : 64);
    DoCommand(v, v3d_OP_END_OF_RENDERING);
}

/* SUPERTILE_COORDINATES for every supertile of a w x h job, sw x sh tiles
 * of tw x th pixels each, row by row: three bytes each (opcode, column,
 * row), written in one claim (up to 255 packets; v3dcore's writer took one
 * claim and clear loop a packet). */
static void supertiles(V3DContext *v, ULONG w, ULONG h, ULONG sw, ULONG sh, ULONG tw, ULONG th)
{
    ULONG nx = (w - 1) / (tw * sw) + 1, ny = (h - 1) / (th * sh) + 1, sx, sy;
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, 3 * nx * ny);
    if (!b)
        return; /* the list overflowed: the flush drops the job (any_overflow) */
    for (sy = 0; sy < ny; sy++)
        for (sx = 0; sx < nx; sx++, b += 3) {
            b[0] = v3d_OP_SUPERTILE_COORDINATES;
            b[1] = (UBYTE)sx;
            b[2] = (UBYTE)sy;
        }
}

/* TILE_RENDERING_MODE_CFG (Clear Colors Part3), render target rt: only
 * the UIF padding, as raw little-endian bytes. Mesa (v3dx_rcl.c) sends it
 * when the image's padded height exceeds the job's by 15 UIF blocks or
 * more. */
static void clear_colors_part3(V3DContext *v, UBYTE rt, ULONG pad)
{
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, 9);
    if (!b)
        return;
    b[0] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3;
    b[1] = (UBYTE)(5 | rt << 4);    /* sub-id 5, the render target */
    b[2] = b[3] = 0;                /* clear colour, high 16 bits */
    b[4] = b[5] = 0;                /* raster stride or image height */
    b[6] = (UBYTE)pad;              /* bits 40..52: the padded height */
    b[7] = (UBYTE)((pad >> 8) & 0x1f);
    b[8] = 0;
}

/* TILE_RENDERING_MODE_CFG (Clear Colors Part1, and Part2 for a 64-bit
 * target), render target rt, as raw little-endian bytes: the clear colour
 * in the target's internal type, lo and hi its first and second 32 bits
 * (Mesa's v3dx_rcl.c: Part1 the low 32 bits and the next 24, Part2 the 32
 * after them; the rest is 0 up to 64 bits). */
static void clear_colors(V3DContext *v, UBYTE rt, ULONG lo, ULONG hi, BOOL wide)
{
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, wide ? 18 : 9);
    int i;
    if (!b)
        return;
    b[0] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1;
    b[1] = (UBYTE)(3 | rt << 4); /* sub-id 3, the render target */
    for (i = 0; i < 4; i++)
        b[2 + i] = (UBYTE)(lo >> (8 * i));
    for (i = 0; i < 3; i++)
        b[6 + i] = (UBYTE)(hi >> (8 * i));
    if (!wide)
        return;
    b[9] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2;
    b[10] = (UBYTE)(4 | rt << 4); /* sub-id 4 */
    b[11] = (UBYTE)(hi >> 24);
    for (i = 12; i < 18; i++)
        b[i] = 0;
}

/* Bytes per pixel of a colour target's image (V3D_OUTPUT_IMAGE_FORMAT_*). */
static ULONG target_cpp(UBYTE format)
{
    return format == V3D_OUTPUT_IMAGE_FORMAT_RGBA16F ? 8 : format == V3D_OUTPUT_IMAGE_FORMAT_RGBA8 ? 4 : 2;
}

/* The render list of a framebuffer object's job (Mesa's v3dx_rcl.c): colour
 * loaded unless cleared, depth and stencil loaded unless cleared, all
 * stored, in the images' own layouts; no Y flip. Each colour image drawn
 * into is a render target of its own (EXT_draw_buffers: output i, target
 * i), all cleared alike. */
static void build_render_fbo(struct OGLES2Context *ctx, const struct OGLES2Target *tg, ULONG ta)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s = v->build_slot;
    UWORD width = tg->w, height = tg->h;
    ULONG tilesX = (width + tg->tile_w - 1) / tg->tile_w, tilesY = (height + tg->tile_h - 1) / tg->tile_h;
    ULONG start, end, sw = 1, sh = 1, fw, fh, argb, abgr;
    UBYTE rt_bpp[4], rt_type[4], rt_clamp[4];
    UBYTE rt;
    UBYTE clear_color = ctx->job_clear_color, clear_zs = ctx->job_clear_zs;
    UBYTE ez = v->first_ez_state;
    UBYTE ez_off = (ez == V3D_EZ_DISABLED) || (width & 1) || (height & 1); /* GFXH-1918 */
    /* EXT_discard_framebuffer: not stored by this job, not loaded from before it. */
    UBYTE no_store = ctx->job_discard, no_load = ctx->job_noload;
    BOOL store_c = tg->colors && !(no_store & OGLES2_DISCARD_COLOR);
    BOOL store_z = tg->has_z && !(no_store & OGLES2_DISCARD_DEPTH &&
                                  (!tg->packed_zs || (no_store & OGLES2_DISCARD_STENCIL) || !ctx->fb_stencil_bits));
    BOOL store_s = tg->has_s && !(no_store & OGLES2_DISCARD_STENCIL);
    /* Mesa: the early Z/S clear only without a Z/S load or store. */
    UBYTE early_zs_clear = !(tg->has_z || tg->has_s);
    const struct OGLES2Surface *c, *z = &tg->z, *st = &tg->s;

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_INCREMENT_SEMAPHORE);
    DoCommand(v, v3d_OP_FLUSH);

    /* Generic tile list. */
    v->current_buf = &v->tile_list_buf[s];
    SetBuffer(v, &v->tile_list_buf[s]);
    start = CurrentBufferAddress(v);
    DoCommand(v, v3d_OP_TILE_COORDINATES_IMPLICIT);
    if (!clear_color && !(no_load & OGLES2_DISCARD_COLOR))
        for (rt = 0; rt < tg->nrt; rt++)
            if (tg->colors & (1 << rt)) {
                c = &tg->color[rt];
                LoadTileBufferGeneral(v, v3d_RENDER_TARGET_0 + rt, c->mem_format, FALSE,
                                      V3D_DECIMATE_MODE_SAMPLE_0, c->format, c->force_alpha1, FALSE, FALSE,
                                      c->ub, 0, c->addr);
            }
    if (!clear_zs) {
        if (tg->has_z && !((no_load & OGLES2_DISCARD_DEPTH) &&
                           (!tg->packed_zs || (no_load & OGLES2_DISCARD_STENCIL) || !ctx->fb_stencil_bits)))
            LoadTileBufferGeneral(v, tg->packed_zs ? v3d_ZSTENCIL : v3d_Z, z->mem_format, FALSE,
                                  V3D_DECIMATE_MODE_SAMPLE_0, z->format, FALSE, FALSE, FALSE, z->ub, 0, z->addr);
        if (tg->has_s && !(no_load & OGLES2_DISCARD_STENCIL))
            LoadTileBufferGeneral(v, v3d_STENCIL, st->mem_format, FALSE, V3D_DECIMATE_MODE_SAMPLE_0,
                                  st->format, FALSE, FALSE, FALSE, st->ub, 0, st->addr);
    }
    DoCommand(v, v3d_OP_END_OF_LOADS);
    PrimListFormat(v, v3d_LIST_TRIANGLES, FALSE);
    SetInstanceid(v, 0);
    BranchToImplicitTileList(v, 0);
    if (store_c)
        for (rt = 0; rt < tg->nrt; rt++)
            if (tg->colors & (1 << rt)) {
                c = &tg->color[rt];
                StoreTileBufferGeneral(v, v3d_RENDER_TARGET_0 + rt, c->mem_format, FALSE, V3D_DITHER_MODE_NONE,
                                       V3D_DECIMATE_MODE_SAMPLE_0, c->format, FALSE, FALSE, FALSE, c->ub, 0,
                                       c->addr);
            }
    if (store_z)
        StoreTileBufferGeneral(v, tg->packed_zs ? v3d_ZSTENCIL : v3d_Z, z->mem_format, FALSE,
                               V3D_DITHER_MODE_NONE, V3D_DECIMATE_MODE_SAMPLE_0, z->format, FALSE, FALSE,
                               FALSE, z->ub, 0, z->addr);
    if (store_s)
        StoreTileBufferGeneral(v, v3d_STENCIL, st->mem_format, FALSE, V3D_DITHER_MODE_NONE,
                               V3D_DECIMATE_MODE_SAMPLE_0, st->format, FALSE, FALSE, FALSE, st->ub, 0, st->addr);
    if (!store_c && !store_z && !store_s) /* Mesa: a tile stores something */
        StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (clear_color || clear_zs)
        ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_RETURN_FROM_SUB_LIST);
    end = CurrentBufferAddress(v);

    /* Render list. */
    v->current_buf = &v->render_buf[s];
    SetBuffer(v, &v->render_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_WAIT_ON_SEMAPHORE);
    TileRenderingModeCFGCommon(v, tg->nrt, width, height, tg->bpp, FALSE, FALSE,
                               ez == V3D_EZ_GT_GE ? v3d_EARLY_Z_DIRECTION_GT_GE
                                                  : v3d_EARLY_Z_DIRECTION_LT_LE,
                               ez_off, tg->depth_type, early_zs_clear);
    argb = clear_argb(ctx);
    abgr = (argb & 0xFF00FF00UL) | ((argb & 0x00FF0000UL) >> 16) | ((argb & 0x000000FFUL) << 16);
    /* Each target's internal type: 8 bits a channel, or 16F for a half-float
     * image (64 bits; EXT_color_buffer_half_float), not clamped (Mesa: clamp
     * none but for sRGB and integer formats). The fragment shader writes both
     * alike, as 16-bit floats (Mesa's key changes only for 32-bit, 16-bit
     * normalized and integer targets), so its variants are the same. */
    for (rt = 0; rt < tg->nrt; rt++) {
        BOOL wide;
        c = &tg->color[rt];
        wide = (tg->colors & (1 << rt)) && c->format == V3D_OUTPUT_IMAGE_FORMAT_RGBA16F;
        rt_bpp[rt] = wide ? V3D_INTERNAL_BPP_64 : V3D_INTERNAL_BPP_32;
        rt_type[rt] = wide ? V3D_INTERNAL_TYPE_16F : V3D_INTERNAL_TYPE_8;
        rt_clamp[rt] = 0;
        if (wide) {
            const GLfloat *f = ctx->job_clear_rgba;
            clear_colors(v, rt, ogles2_float_to_half(f[0]) | (ULONG)ogles2_float_to_half(f[1]) << 16,
                         ogles2_float_to_half(f[2]) | (ULONG)ogles2_float_to_half(f[3]) << 16, TRUE);
        } else {
            clear_colors(v, rt, abgr, 0, FALSE);
        }
        if ((tg->colors & (1 << rt)) && c->ub) {
            ULONG ubh = 2 * v3d_utile_height((int)target_cpp(c->format));
            if (c->ub - (height + ubh - 1) / ubh >= 15)
                clear_colors_part3(v, rt, c->ub);
        }
    }
    TileRenderingModeCFGColorEach(v, tg->nrt, rt_bpp, rt_type, rt_clamp);
    TileRenderingModeCFGZSClearValues(v, clear_zs ? ctx->job_clear_stencil : 0,
                                      clear_zs ? ctx->job_clear_depth : 1.0f);
    TileListInitialBlockSize(v, v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, TRUE);

    for (;;) {
        fw = (tilesX + sw - 1) / sw;
        fh = (tilesY + sh - 1) / sh;
        if (fw * fh < 256)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }
    Multicore_Rendering_Tile_List_Set_Base(v, ta, 0);
    MulticoreRenderingSupertileCFG(v, sw, sh, fw, fh, 1, FALSE, FALSE, tilesY, tilesX);

    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);

    StartAddressOfGenericTileList(v, start, end);
    supertiles(v, width, height, sw, sh, tg->tile_w, tg->tile_h);
    DoCommand(v, v3d_OP_END_OF_RENDERING);
}

static BOOL any_overflow(V3DContext *v)
{
    v3d_u8 s = v->build_slot;
    return v->binning_buf[s].overflowed || v->state_buf[s].overflowed ||
           v->render_buf[s].overflowed || v->tile_list_buf[s].overflowed;
}

BOOL ogles2_v3d_job_has_draws(struct OGLES2Context *ctx)
{
    return ctx->v3d->draw_state_configured;
}

/* Depth and stencil to be kept past this job: their memory, before the
 * locks (no allocation under the bitmap lock). */
BOOL ogles2_v3d_job_keep_zs(struct OGLES2Context *ctx)
{
    return zs_get(ctx) != 0;
}

/* Why the job cannot render (a draw found its memory short), or 0. */
const char *ogles2_v3d_job_check(struct OGLES2Context *ctx)
{
    if (!ctx->v3d->frame_corrupted)
        return 0;
    return ctx->job_fb ? "no memory for the framebuffer object's tile pools, or a draw's uniform stream went stale"
                       : "a draw's uniform stream went stale (state buffer grew mid-draw)";
}

/* The tile pools: the object's own (tg->ta_addr) or the context's. */
static void pools(V3DContext *v, const struct OGLES2Target *tg, ULONG *ta, ULONG *ta_size, ULONG *ts)
{
    if (tg && tg->ta_addr) {
        *ta = tg->ta_addr;
        *ta_size = tg->ta_size;
        *ts = tg->ts_addr;
    } else {
        *ta = (ULONG)v->tile_alloc_mem.hostptr;
        *ta_size = (ULONG)v->tile_alloc_mem.size;
        *ts = (ULONG)v->tile_state_mem.hostptr;
    }
}

/* The render lists, with the GPU and (default framebuffer) the bitmap
 * locked. FALSE: a list overflowed. */
BOOL ogles2_v3d_job_build(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL store_zs)
{
    V3DContext *v = ctx->v3d;
    ULONG ta, ta_size, ts;
    if (tg) {
        pools(v, tg, &ta, &ta_size, &ts);
        build_render_fbo(ctx, tg, ta);
    } else {
        build_render(ctx, store_zs ? zs_get(ctx) : 0);
    }
    return !any_overflow(v);
}

/* Probe S: the render list was submitted with the binning list (the GPU
 * lock is held from the one to the other, so one is enough). */
static BOOL both_submitted;
static v3d_u8 both_frame;

/* The binning list: submitted and waited for (OGLES2_WAIT_*). */
int ogles2_v3d_job_bin(struct OGLES2Context *ctx, const struct OGLES2Target *tg)
{
    V3DContext *v = ctx->v3d;
    V3DDevice *dev = ogles2_device();
    v3d_u8 s = v->build_slot, last;
    ULONG ta, ta_size, ts;
    v3d_wait_result wb;
    pools(v, tg, &ta, &ta_size, &ts);
    both_submitted = FALSE;
    v3d_invalidate_caches();
    last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_mem[s].hostptr,
                              (v3d_address)((ULONG)v->binning_buf[s].start + v->binning_buf[s].used),
                              (v3d_address)ta, (v3d_u32)ta_size, (v3d_address)ts);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE) {
        /* Probe S: both lists go to the GPU at once; the render list waits
         * for the binner's semaphore. Out-of-memory service stays in the
         * binning wait. */
        both_frame = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                       (v3d_address)((ULONG)v->render_buf[s].start + v->render_buf[s].used));
        both_submitted = TRUE;
    }
    wb = v3d_wait_binning_timeout(dev, &v->frame, last);
    if (both_submitted)
        v3d_frame_render_submitted(&v->frame);
    return (int)wb;
}

/* The render list, after its binning: submitted; its frame number for
 * the wait. */
UBYTE ogles2_v3d_job_render(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s = v->build_slot, frame_no;
    if (both_submitted) {
        both_submitted = FALSE;
        return both_frame;
    }
    v3d_invalidate_caches();
    frame_no = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                 (v3d_address)((ULONG)v->render_buf[s].start + v->render_buf[s].used));
    v3d_frame_render_submitted(&v->frame);
    return frame_no;
}

int ogles2_v3d_wait_render(UBYTE frame)
{
    return (int)v3d_wait_render_timeout(frame);
}

/* After a render that finished: the blocks the job no longer needs. */
void ogles2_v3d_job_retire(struct OGLES2Context *ctx)
{
    v3d_frame_retire(ogles2_device(), &ctx->v3d->frame);
}

/* The job is over; submitted: the next one builds in another slot. Not the
 * running render's either when this one was queued behind it (job.c sets
 * q_active first): then the third slot. Otherwise [0] and [1] in turn (from
 * [2], [0]: everything earlier has finished). */
void ogles2_v3d_job_end(struct OGLES2Context *ctx, BOOL submitted)
{
    V3DContext *v = ctx->v3d;
    if (submitted) {
        v3d_u8 s = v->build_slot;
        if (ctx->q_active && ctx->q_serial == ctx->job_serial)
            v->build_slot = (v3d_u8)(3 - s - v->last_slot); /* neither queued nor running */
        else
            v->build_slot = s == 0 ? 1 : 0;
        v->last_slot = s;
    }
    v->frame_active = FALSE;
}

/* job.c's queue (backend.h; job.c has checked the target's own tile
 * pools): a third slot for the next job. Not with probe S (job_bin submits
 * the render with the binning). */
BOOL ogles2_v3d_job_can_queue(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        return FALSE;
    if (v->build_slot == v->last_slot)
        return FALSE; /* never: the running render's slot is not the one built in */
    if (!v->slot2_ready && v3d_context_alloc_slot2(ogles2_device(), v) < 0) {
        ogles2_log("job: no memory for a third control-list slot: framebuffer-object jobs wait as before");
        return FALSE;
    }
    return TRUE;
}

/* The queued job, binned: its blocks belong to it from now on (the next
 * job's are tagged after it, so retiring up to it never frees those); its
 * render list stays in its slot until job_render_queued. */
void ogles2_v3d_job_queue(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    v->queued_slot = v->build_slot;
    v3d_frame_render_submitted(&v->frame);
}

/* Its render, now that nothing else renders (job.c holds the GPU lock and
 * has waited for the running render): submitted, as job_render does. */
UBYTE ogles2_v3d_job_render_queued(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s = v->queued_slot;
    v3d_invalidate_caches();
    return v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                             (v3d_address)((ULONG)v->render_buf[s].start + v->render_buf[s].used));
}

/* A binning job that did not finish: where the control list executor
 * stopped (raw little-endian words), logged once the locks are given up. */
void ogles2_v3d_job_report_stop(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s = v->build_slot;
    ogles2_log("job: binning stopped: CT0CA 0x%08lx CT0EA 0x%08lx CT0CS 0x%08lx; list 0x%08lx..0x%08lx "
               "(last block 0x%08lx), %ld spill blocks",
               (ULONG)LE32(V3D_CT0CA), (ULONG)LE32(V3D_CT0EA), (ULONG)LE32(V3D_CT0CS),
               (ULONG)v->binning_mem[s].hostptr,
               (ULONG)v->binning_buf[s].start + v->binning_buf[s].used, (ULONG)v->binning_buf[s].start,
               (LONG)v->frame.job_spills);
}
