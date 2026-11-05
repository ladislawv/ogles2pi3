/*
 * The VC4 backend's jobs (ogles2/backend.h, docs/PI3_PLAN.md §5.1, P5):
 * vc4core's job (one binning list, one render list naming every tile) for
 * the default framebuffer or a framebuffer object, in the steps job.c takes
 * (lists, binning, render left running while the CPU builds the next job,
 * its wait). Two jobs a context: one builds while the other renders.
 *
 * The default framebuffer is rendered straight into the display's bitmap.
 * VC4's raster stores have no stride field: a row is the frame's width
 * rounded up to 4 pixels (PI3_PLAN §2, "Raster stores"), so the frame is as
 * wide as the bitmap's row (BytesPerRow / 4, which must be a multiple of
 * 16 bytes), the draws clipped to the visible width; the columns past it
 * are the bitmap's padding and get the clear colour. A bitmap with other
 * rows is not drawn into (the copy from a buffer of our own: left, §10).
 *
 * Depth and stencil: the tile buffer's (Z24S8), cleared at a job's start;
 * kept across a mid-frame flush in a T-format image of the context's (4x
 * MSAA: not kept, left). A framebuffer object's depth and stencil images
 * are one Z24S8 image on VC4 (fbo_vc4.c): its depth's, else its stencil's.
 *
 * Nothing is logged while a bitmap is locked (job.c): what goes wrong in
 * job_build is logged at job_end, the GPU's errors at job_retire.
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "vc4core.h"
#include "vc4_os.h"
#include "vc4_regs.h"
#include "v3d_types.h"
#include "backend_vc4.h"

#define BIN_TIMEOUT_US 2000000u
#define RENDER_TIMEOUT_US 2000000u

/* The job on the GPU (one at a time: job.c's take_gpu), and its number. */
static struct vc4_job *running;
static UBYTE running_frame, frames;
static ULONG errors_logged;

/* ---- blocks freed after a job ---- */

void ogles2_vc4_defer_free_all(struct vc4_ctx *c, int slot)
{
    struct vc4_defer *d = c->defer[slot], *n;
    c->defer[slot] = 0;
    for (; d; d = n) {
        struct v3d_mem m;
        n = d->next;
        m.size = d->size;
        m.handle = d->handle;
        m.busaddr = d->busaddr;
        m.hostptr = d->hostptr;
        ogles2_mem_free(&m);
        FreeVec(d);
    }
}

/* Into the open job's slot: it renders after any job that may read the
 * block (the one left running, the other slot's, renders first). */
void ogles2_vc4_mem_free_after(struct OGLES2Context *user, struct v3d_mem *m)
{
    struct vc4_ctx *c = VC4_CTX(user);
    struct vc4_defer *d;
    if (!m->size)
        return;
    if (!c || !(d = AllocVec(sizeof(*d), MEMF_ANY))) {
        /* No list: the render left running finishes first, and the open
         * job (which may name the block) is dropped. */
        if (running)
            ogles2_vc4_wait_render(running_frame);
        if (c) {
            c->bad = TRUE;
            c->why_bad = "out of memory for the list of blocks to free after it";
        }
        ogles2_mem_free(m);
        return;
    }
    d->size = m->size;
    d->handle = m->handle;
    d->busaddr = m->busaddr;
    d->hostptr = m->hostptr;
    d->next = c->defer[c->build];
    c->defer[c->build] = d;
}

/* ---- the job's target ---- */

/* The key's target format and the tile buffer's clear colour for it (the
 * colour packed as the image holds it: Mesa's vc4_clear; RGB565 from the
 * RGBA8 order, the store packs it). */
static ULONG clear_word(const struct OGLES2Context *ctx, UBYTE color_format)
{
    const GLfloat *f = ctx->job_clear_rgba;
    ULONG c[4];
    int k;
    for (k = 0; k < 4; k++)
        c[k] = (ULONG)(f[k] * 255.0f + 0.5f); /* clamped to [0,1] by glClearColor */
    if (color_format == 1) /* B, G, R, A in memory */
        return c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
    return c[3] << 24 | c[2] << 16 | c[1] << 8 | c[0];
}

static void surface(const struct OGLES2Surface *s, struct vc4_surface *o)
{
    o->bus = s->addr ? ogles2_vc4_bus(s->addr) : 0;
    o->tiling = s->mem_format;
    o->format = s->format;
}

void ogles2_vc4_job_begin(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL bad)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct vc4_target *t = &c->target;
    struct vc4_pools *pools = &c->pools;
    memset(t, 0, sizeof(*t));
    c->has_draws = FALSE;
    c->bad = bad;
    c->why_bad = bad ? "no memory for the framebuffer object's tile pools" : 0;
    c->fbo = ctx->draw_fb != 0;
    if (c->fbo) {
        if (!tg) { /* no pools: dropped at the flush; lists for the context's frame meanwhile */
            t->width = c->w;
            t->height = c->h;
        } else {
            t->width = tg->w;
            t->height = tg->h;
            if (tg->ta_addr) { /* its own pools (fbo_vc4.c's layout) */
                memset(&c->fbo_pools, 0, sizeof(c->fbo_pools));
                c->fbo_pools.tiles_x = (UWORD)((tg->w + 63) / 64);
                c->fbo_pools.tiles_y = (UWORD)((tg->h + 63) / 64);
                c->fbo_pools.tsda = ogles2_vc4_bus(tg->ts_addr);
                c->fbo_pools.talloc = ogles2_vc4_bus(tg->ta_addr);
                c->fbo_pools.talloc_size = tg->ta_size;
                c->fbo_pools.overflow = c->fbo_pools.talloc + tg->ta_size;
                c->fbo_pools.overflow_size = c->pools.overflow_size;
                c->fbo_pools.overflow_chunks = c->pools.overflow_chunks;
                pools = &c->fbo_pools;
            }
        }
        /* the target's format for the draws' keys: job.c's target has no
         * surfaces yet, so they are looked up here (the flush does again) */
        c->color_format = 0;
        if (tg) {
            struct OGLES2Target full;
            if (ogles2_fbo_target(ctx, ctx->draw_fb, TRUE, &full) && (full.colors & 1))
                c->color_format = full.color[0].format == VC4_COLOR_BGR565 ? 2 : 0;
        }
    } else {
        /* the bitmap's row as last locked (the binning needs the tile count
         * before the lock; job_build checks it) */
        ULONG fw = ctx->display.bprow ? ctx->display.bprow / 4 : ((ULONG)ctx->width + 3) & ~3UL;
        if (fw < ctx->width)
            fw = ctx->width;
        c->frame_w = (UWORD)fw;
        t->width = (UWORD)fw;
        t->height = (UWORD)ctx->height;
        t->msaa = ctx->job_msaa ? 1 : 0;
        c->color_format = ctx->display.rb_swap ? 1 : 0;
    }
    if (vc4_job_begin(&c->job[c->build], t, pools) < 0) {
        c->bad = TRUE;
        if (!c->why_bad)
            c->why_bad = "its frame is larger than its tile pools, or no memory for its lists";
    }
}

BOOL ogles2_vc4_job_has_draws(struct OGLES2Context *ctx)
{
    return VC4_CTX(ctx)->has_draws;
}

/* The default framebuffer's depth and stencil kept past this job: a T (or
 * LT) Z24S8 image of the frame's size, before the locks. */
BOOL ogles2_vc4_job_keep_zs(struct OGLES2Context *ctx)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct vc4_texture_layout lay;
    if ((!ctx->depth_bits && !ctx->stencil_bits) || ctx->job_msaa)
        return FALSE; /* 4x MSAA: the general store keeps one sample a pixel (left, §10) */
    if (c->zs.size && c->zs_w == c->frame_w && c->zs_h == ctx->height)
        return TRUE;
    if (c->zs.size)
        vc4_mem_free(&c->zs);
    vc4_texture_layout(&lay, c->frame_w, ctx->height, 4, 1, 1);
    if (vc4_mem_alloc(&c->zs, lay.size) < 0)
        return FALSE;
    c->zs_w = c->frame_w;
    c->zs_h = (UWORD)ctx->height;
    return TRUE;
}

const char *ogles2_vc4_job_check(struct OGLES2Context *ctx)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    return c->bad ? c->why_bad : 0;
}

BOOL ogles2_vc4_job_build(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL store_zs)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct vc4_job *job = &c->job[c->build];
    struct vc4_target *t = &job->target;
    UBYTE noload = ctx->job_noload, nostore = ctx->job_discard;
    BOOL has_zs;
    t->load = t->store = t->clear = 0;
    t->clear_color = clear_word(ctx, c->color_format);
    t->clear_z = 0xffffffu;
    t->clear_s = 0;
    if (ctx->job_clear_zs) {
        t->clear_z = (ULONG)(ctx->job_clear_depth * 16777215.0f + 0.5f) & 0xffffffu;
        t->clear_s = ctx->job_clear_stencil;
    }
    if (!tg) {
        ULONG bprow = ctx->display.bprow, fw = bprow / 4;
        UWORD tiles = (UWORD)((fw + (t->msaa ? 31 : 63)) / (t->msaa ? 32 : 64));
        if (bprow & 15) {
            c->why_bad = "the display's rows are not a multiple of 16 bytes (VC4 stores rows of 4 pixels)";
            return FALSE;
        }
        if (tiles != job->tiles_x) {
            c->why_bad = "the display's row length changed since the job began";
            return FALSE;
        }
        t->width = (UWORD)fw;
        t->color.bus = ogles2_vc4_bus(ctx->display.base);
        t->color.tiling = VC4_TILING_RASTER;
        t->color.format = VC4_COLOR_RGBA8888;
        t->store = VC4_COLOR;
        if (ctx->job_clear_color)
            t->clear |= VC4_COLOR;
        else
            t->load |= VC4_COLOR;
        has_zs = (ctx->depth_bits || ctx->stencil_bits) && c->zs.size && c->zs_w == fw;
        t->zs.bus = 0;
        if (has_zs && (store_zs || (ctx->zs_saved && !ctx->job_clear_zs))) {
            t->zs.bus = c->zs.bus;
            t->zs.tiling = vc4_size_is_lt(fw, ctx->height, 4) ? VC4_TILING_LT : VC4_TILING_T;
            if (store_zs)
                t->store |= VC4_ZS;
            if (ctx->zs_saved && !ctx->job_clear_zs)
                t->load |= VC4_ZS;
        }
        if (!(t->load & VC4_ZS))
            t->clear |= VC4_ZS; /* EGL: undefined at a frame's start; the tile buffer's is cleared */
    } else {
        memset(&t->color, 0, sizeof(t->color));
        memset(&t->zs, 0, sizeof(t->zs));
        if (tg->colors & 1) {
            surface(&tg->color[0], &t->color);
            if (!(nostore & OGLES2_DISCARD_COLOR))
                t->store |= VC4_COLOR;
            if (ctx->job_clear_color)
                t->clear |= VC4_COLOR;
            else if (!(noload & OGLES2_DISCARD_COLOR))
                t->load |= VC4_COLOR;
        }
        if (tg->has_z || tg->has_s) {
            surface(tg->has_z ? &tg->z : &tg->s, &t->zs);
            if ((nostore & (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)) !=
                (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL))
                t->store |= VC4_ZS;
            if (!ctx->job_clear_zs && (noload & (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)) !=
                                          (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL))
                t->load |= VC4_ZS;
        }
        if (!(t->load & VC4_ZS))
            t->clear |= VC4_ZS;
        if (!(t->load & VC4_COLOR) && !(t->clear & VC4_COLOR))
            t->clear |= VC4_COLOR; /* discarded: undefined, the tile buffer's is cleared */
    }
    if (vc4_job_end_lists(job) < 0) {
        c->why_bad = "out of GPU memory for its lists";
        return FALSE;
    }
    c->why_bad = 0;
    return TRUE;
}

int ogles2_vc4_job_bin(struct OGLES2Context *ctx, const struct OGLES2Target *tg)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    (void)tg;
    c->rendering = c->build;
    if (vc4_job_bin(&c->job[c->build], BIN_TIMEOUT_US) < 0)
        return c->job[c->build].out_of_overflow ? OGLES2_WAIT_ERROR : OGLES2_WAIT_TIMED_OUT;
    return OGLES2_WAIT_OK;
}

UBYTE ogles2_vc4_job_render(struct OGLES2Context *ctx)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    running = &c->job[c->build];
    running_frame = ++frames;
    vc4_job_render_start(running);
    return running_frame;
}

int ogles2_vc4_wait_render(UBYTE frame)
{
    struct vc4_job *j = running;
    if (!j || frame != running_frame)
        return OGLES2_WAIT_OK; /* waited for already */
    running = 0;
    return vc4_job_render_wait(j, RENDER_TIMEOUT_US) < 0 ? OGLES2_WAIT_TIMED_OUT : OGLES2_WAIT_OK;
}

/* After a render that finished (the locks given up): the blocks its job
 * no longer needs, and the GPU's errors, the first few. */
void ogles2_vc4_job_retire(struct OGLES2Context *ctx)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct vc4_job *j = &c->job[c->rendering];
    ogles2_vc4_defer_free_all(c, c->rendering);
    /* ERRSTAT 0x1000 is the idle state; VCMBE (bit 14) comes with about one
     * job in 200 whose binner ran out of memory, the picture right
     * (PI3_PLAN §3.1): not an error */
    if ((j->errstat & ~(V3D_ERRSTAT_IDLE | 0x4000UL /* VCMBE */)) && errors_logged < 8) {
        errors_logged++;
        ogles2_log("job: the GPU reports ERRSTAT 0x%08lx after it, 0x%08lx after its binning (logged 8 times)",
                   (ULONG)j->errstat, (ULONG)j->errstat_bin);
    }
}

void ogles2_vc4_job_end(struct OGLES2Context *ctx, BOOL submitted)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    if (c->why_bad && !c->bad) /* job_build's reason (the frame was dropped: logged as an overflow) */
        ogles2_log("job: %s", (ULONG)c->why_bad);
    c->why_bad = 0;
    if (submitted) {
        c->build = (UBYTE)(1 - c->build);
    } else {
        /* Never submitted, and nothing of this context's runs (the flush
         * waited for its render first): what it would have freed goes now. */
        ogles2_vc4_defer_free_all(c, c->build);
    }
    c->has_draws = FALSE;
    c->bad = FALSE;
}

void ogles2_vc4_job_report_stop(struct OGLES2Context *ctx)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    struct vc4_job *j = &c->job[c->rendering];
    if (j->out_of_overflow)
        ogles2_log("job: the binner needed more than its pools' %ld overflow chunks", (ULONG)j->pools->overflow_chunks);
    else
        ogles2_log("job: binning stopped: CT0CA 0x%08lx CT0EA 0x%08lx; list 0x%08lx..0x%08lx",
                   (ULONG)vc4_read(V3D_CT0CA), (ULONG)vc4_read(V3D_CT0EA),
                   (ULONG)vc4_cl_start(&j->bcl), (ULONG)vc4_cl_end(&j->bcl));
}

void ogles2_vc4_job_free(struct OGLES2Context *ctx)
{
    struct vc4_ctx *c = VC4_CTX(ctx);
    if (c && c->zs.size)
        vc4_mem_free(&c->zs);
}
