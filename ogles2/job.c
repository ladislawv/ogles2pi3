/*
 * Jobs (plan section 7.7): drawing as the GPU does it, one binning pass and
 * one render pass, into the default framebuffer or a framebuffer object
 * (ctx->job_fb). Here what is the same on every GPU (docs/PI3_PLAN.md
 * §5.1): when a job begins and ends, its serial (shared objects keep the
 * last job that used them, share.c), the glClear values it clears with, the
 * GPU submission lock, a render left running while the CPU builds the next
 * job (M6), the statistics and the reasons a job is dropped. The backend
 * builds, submits and waits (backend.h; V3D 4.2: ogles2/v3d/job_v3d.c).
 *
 *  - Locks: the GPU submission lock (plan 7.1) before the bitmap's, so a
 *    program waiting for the GPU never holds a bitmap; nothing is logged
 *    while a bitmap is locked (cybergraphics: no library calls then; exec's
 *    cache and semaphore calls are the exception).
 *  - The default framebuffer's depth and stencil are cleared at the start of
 *    every tile and neither loaded nor stored (plan 7.2: EGL leaves them
 *    undefined after a swap), except across a flush in the middle of a
 *    frame (glReadPixels, glFlush), which keeps them for the frame's next job.
 *  - A flush may return once the render is submitted (M6): a framebuffer
 *    object's job, and one on a screen of our own or on the screen a window
 *    is all of. ogles2_job_finish waits for it, at the next flush or before
 *    the CPU reads what it writes or changes what it reads (texture.c,
 *    buffer.c, fbo.c, frame.c). The submission lock is given up once the
 *    render is submitted: the next submitter, any context of any program,
 *    waits for that render on the GPU first (take_gpu), so programs take
 *    turns and none holds the lock across its CPU work.
 *  - A framebuffer object's job flushed while this context's default
 *    framebuffer render still runs is queued behind it instead of waiting
 *    for it: built and binned now (the binner works beside the running
 *    render, into the target's own tile pools), its render submitted by
 *    ogles2_job_finish once the running one has ended (the GPU is never
 *    given a render while another runs). Without it a program that
 *    draws into a texture at the start of its frame (SA68's
 *    caustics) waited there for the whole previous frame's render, and the
 *    CPU and GPU no longer overlapped. ogles2_job_finish waits for both.
 *    The backend says when it can (job_can_queue: the target's own tile
 *    pools, a third slot of lists); ENV:OGLES2/NoQueue (anything but 0)
 *    turns it off.
 *  - Drawing into another framebuffer renders the open job first (jobs v1).
 *    An object's job loads and stores its images in their own layouts,
 *    without the Y flip.
 *
 * Written for ogles2; PI3_PLAN P2 step 8 rewrote what had come from
 * PiStorm3D's frame handling.
 *
 * MIT licence (LICENSE).
 */
#include <exec/execbase.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "v3d_gpu_lock.h"
/* Probe H's wait (V3D only: never set on VC4); v3dcore/hw/v3d_hw.c. */
unsigned char v3d_get_render_frame_count(void);

#define GPU_LOCK_TIMEOUT_MS 5000

/* The job generation (backend.h): changes whenever a backend starts a
 * control list afresh, so that a cache of addresses into one (draw.c's
 * state packets and uniform streams, texture.c's records) sees that they
 * are gone. One number for all contexts that only goes up: a cache that
 * outlives its context, or one context's cache looked at by another, can
 * never see a value again that it saw before. */
ULONG ogles2_job_gen = 0;

/* Starts the job if none is open: the backend's lists, then the job's
 * serial and an empty clear snapshot. */
void ogles2_job_begin(struct OGLES2Context *ctx)
{
    struct OGLES2Target tg;
    BOOL bad = FALSE;

    if (ctx->job_open) {
        if (ctx->job_fb == ctx->draw_fb)
            return;
        /* Drawing into another framebuffer: the open job is rendered first
         * (jobs v1). The default framebuffer's depth and stencil are kept
         * for its next job, as at any mid-frame flush. */
        ogles2_job_flush(ctx, FALSE);
    }
    if (ctx->draw_fb && !ogles2_fbo_target(ctx, ctx->draw_fb, FALSE, &tg))
        bad = TRUE; /* no memory for its tile pools: the job is dropped at the flush */
    ctx->job_own_pools = ctx->draw_fb && !bad && tg.ta_addr;
    ctx->job_fb = ctx->draw_fb;
    /* 4x MSAA on the default framebuffer only (plan 7.2). */
    ctx->job_msaa = !ctx->draw_fb && ctx->msaa;
    ctx->t_job_begin = ogles2_clock_us();
    ctx->job_discard = 0;
    ctx->job_noload = ctx->draw_fb ? ogles2_fbo_take_discarded(ctx->draw_fb) : 0;
    ogles2_backend->job_begin(ctx, ctx->draw_fb && !bad ? &tg : 0, bad);
    ogles2_job_gen++;
    ctx->job_open = TRUE;
    /* Unique across contexts: a shared object's "last used by job N" names
     * one job of one member (share.c). */
    ObtainSemaphore(&OGLES2LibBase->lock);
    if (!++OGLES2LibBase->serial)
        ++OGLES2LibBase->serial; /* 0: never used */
    ctx->job_serial = OGLES2LibBase->serial;
    ReleaseSemaphore(&OGLES2LibBase->lock);
    ctx->job_clear_color = FALSE;
    ctx->job_clear_zs = FALSE;
}

/* glClear's whole-framebuffer path: the tile buffer is cleared when the job
 * renders. The values are those current at the glClear (Mesa snapshots them
 * the same way), not at the flush. */
void ogles2_job_clear(struct OGLES2Context *ctx, GLbitfield mask)
{
    const GLfloat *c = ctx->state.clear_color;
    int k;

    ogles2_job_begin(ctx);
    if (mask & GL_COLOR_BUFFER_BIT) {
        ctx->job_clear_color = TRUE;
        for (k = 0; k < 4; k++) /* clamped to [0,1] by glClearColor */
            ctx->job_clear_rgba[k] = c[k];
    }
    /* The tile buffer clears depth and stencil together; each keeps the
     * value of the last glClear that named it (a stencil clear after a full
     * one must not take the newer glClearDepthf, dEQP depth_stencil_clear).
     * Before the job's first such clear neither holds anything yet. */
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        if (!ctx->job_clear_zs) {
            ctx->job_clear_depth = 1.0f;
            ctx->job_clear_stencil = 0;
        }
        ctx->job_clear_zs = TRUE;
        if (mask & GL_DEPTH_BUFFER_BIT)
            ctx->job_clear_depth = ctx->state.clear_depth;
        if (mask & GL_STENCIL_BUFFER_BIT)
            ctx->job_clear_stencil = (UBYTE)ctx->state.clear_stencil; /* the low 8 bits (GLES 2.0 4.2.3) */
    }
}

void ogles2_job_free(struct OGLES2Context *ctx)
{
    ogles2_backend->job_free(ctx);
    ctx->zs_saved = FALSE;
}

/* The colour image after the GPU's write: nothing here. The CPU reads it
 * only through v3dcore's v3d_texture_load (texture.c's sync, glReadPixels,
 * glCopyTex*), which invalidates the level first; it was cleaned before
 * the render with everything else (clean_all). An exec cache call costs
 * about 41 us (M6): one less per framebuffer-object job. Depth and stencil
 * images are never read by the CPU. */
static void target_dma(const struct OGLES2Target *tg, BOOL after)
{
    (void)tg;
    (void)after;
}

/* Before the GPU reads anything the CPU wrote since the last flush (the
 * control lists, the state buffer, buffer copies, the render target): one
 * clean of the whole data cache. On Emu68 every exec cache call cleans the
 * whole cache whatever its range, about 41 microseconds each
 * (tests/perf/cachetime.c), and the flush made five of them (M6). Data
 * only: clearing the instruction cache would make Emu68 drop its
 * translated code. */
static void clean_all(void)
{
    CacheClearE(0, ~0UL, CACRF_ClearD); /* length ~0: the whole cache */
}

/* After a render: a GPU that did not finish reset, the CPU cache over what
 * it wrote, the locks released, the retire list, the colour image marked. */
static void after_render(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, const struct OGLES2Target *tg,
                         int wr, BOOL holds_lock)
{
    ULONG t0;
    if (wr != OGLES2_WAIT_OK) {
        /* The reset needs the submission lock; a render left running gave
         * it up when it was submitted. */
        if (holds_lock || v3d_gpu_lock(ogles2_device_gpu_lock(), GPU_LOCK_TIMEOUT_MS)) {
            ogles2_device_reset("a render job it did not finish");
            if (!holds_lock)
                v3d_gpu_unlock(ogles2_device_gpu_lock());
        }
    }
    t0 = ogles2_clock_us();
    if (fb) {
        target_dma(tg, TRUE);
    } else {
        ogles2_display_dma(&ctx->display, TRUE);
        ogles2_display_unlock(&ctx->display);
    }
    ctx->sum_post += (double)(ULONG)(ogles2_clock_us() - t0);
    if (holds_lock)
        v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (wr == OGLES2_WAIT_OK) {
        ogles2_backend->job_retire(ctx);
        if (fb)
            ogles2_fbo_rendered(ctx, fb);
    } else {
        ogles2_log("job: render %s", (ULONG)(wr == OGLES2_WAIT_TIMED_OUT ? "timed out" : "failed"));
        if (!fb)
            ctx->zs_saved = FALSE;
    }
}

static void take_gpu(struct OGLES2Context *ctx);

/* The queued job's render (ogles2_job_finish; its lists were built and
 * binned at its flush): submitted as any render, with the submission lock
 * and nobody else's render on the GPU, then waited for; meanwhile another
 * submitter waits for it (pend_ctx). Returns OGLES2_WAIT_*. */
static int render_queued(struct OGLES2Context *ctx)
{
    struct SignalSemaphore *gpu = ogles2_device_gpu_lock();
    UBYTE frame;
    int wr;
    ULONG t0;
    if (!v3d_gpu_lock(gpu, GPU_LOCK_TIMEOUT_MS)) {
        /* Not on the GPU: nothing to reset; the target keeps its old picture. */
        ctx->frames_dropped++;
        ogles2_log("job: frame dropped: %s", (ULONG) "the GPU stayed busy (another program holds the submission lock)");
        return OGLES2_WAIT_OK;
    }
    take_gpu(ctx);
    frame = ogles2_backend->job_render_queued(ctx);
    ObtainSemaphore(&OGLES2LibBase->lock);
    ctx->pend_done = FALSE;
    OGLES2LibBase->pend_ctx = ctx;
    OGLES2LibBase->pend_task = FindTask(0);
    OGLES2LibBase->pend_frame = frame;
    ReleaseSemaphore(&OGLES2LibBase->lock);
    v3d_gpu_unlock(gpu);
    t0 = ogles2_clock_us();
    wr = ogles2_backend->wait_render(frame);
    ctx->sum_render += (double)(ULONG)(ogles2_clock_us() - t0);
    ObtainSemaphore(&OGLES2LibBase->lock);
    if (OGLES2LibBase->pend_ctx == ctx)
        OGLES2LibBase->pend_ctx = 0;
    ctx->pend_done = FALSE;
    ReleaseSemaphore(&OGLES2LibBase->lock);
    if (wr != OGLES2_WAIT_OK)
        ogles2_log("job: queued render %s", (ULONG)(wr == OGLES2_WAIT_TIMED_OUT ? "timed out" : "failed"));
    return wr;
}

/* The render this context left running: waited for (unless another
 * submitter already saw it finish: take_gpu), then what follows a render. */
void ogles2_job_finish(struct OGLES2Context *ctx)
{
    int wr = OGLES2_WAIT_OK;
    BOOL done;
    ULONG t0;
    if (!ctx->pend)
        return;
    ctx->pend = FALSE;
    ObtainSemaphore(&OGLES2LibBase->lock);
    done = ctx->pend_done;
    ctx->pend_done = FALSE;
    if (OGLES2LibBase->pend_ctx == ctx && !(ctx->q_active && ctx->q_hw))
        OGLES2LibBase->pend_ctx = 0;
    ReleaseSemaphore(&OGLES2LibBase->lock);
    if (ctx->q_active && ctx->q_hw) {
        /* Probe H: both renders on the hardware, in order: the count reaching
         * pend_frame + 2 (modulo 256: it may be pend_frame, + 1 or + 2) is
         * both done. Another submitter that waited for them marked done. */
        ctx->q_active = FALSE;
        ctx->q_hw = FALSE;
        if (!done) {
            UBYTE target = (UBYTE)(ctx->pend_frame + 2);
            t0 = ogles2_clock_us();
            for (;;) {
                if ((UBYTE)(v3d_get_render_frame_count() - target) < 128)
                    break;
                if ((ULONG)(ogles2_clock_us() - t0) > 2000000UL) {
                    wr = OGLES2_WAIT_TIMED_OUT;
                    ogles2_log("job: queued render (probe H) timed out", 0);
                    break;
                }
            }
            ctx->sum_render += (double)(ULONG)(ogles2_clock_us() - t0);
        }
        ObtainSemaphore(&OGLES2LibBase->lock);
        if (OGLES2LibBase->pend_ctx == ctx)
            OGLES2LibBase->pend_ctx = 0;
        ctx->pend_done = FALSE;
        ReleaseSemaphore(&OGLES2LibBase->lock);
        if (wr == OGLES2_WAIT_OK)
            ogles2_fbo_rendered(ctx, ctx->q_fb);
        after_render(ctx, ctx->pend_fb, &ctx->pend_tg, wr, FALSE);
        return;
    }
    if (!done) {
        /* Nothing was submitted after it (take_gpu would have marked it
         * done), so the count is one past ours at most: no wrap-around
         * case the wait cannot tell. */
        t0 = ogles2_clock_us();
        wr = ogles2_backend->wait_render(ctx->pend_frame);
        ctx->sum_render += (double)(ULONG)(ogles2_clock_us() - t0);
    }
    /* A job queued behind it: its render now, and waited for, before what
     * follows a render (the retire list goes up to the queued job's
     * blocks). */
    if (ctx->q_active) {
        ctx->q_active = FALSE;
        if (wr == OGLES2_WAIT_OK) {
            wr = render_queued(ctx);
            if (wr == OGLES2_WAIT_OK)
                ogles2_fbo_rendered(ctx, ctx->q_fb);
        }
    }
    after_render(ctx, ctx->pend_fb, &ctx->pend_tg, wr, FALSE);
}

/* ENV:OGLES2/NoQueue: set and not "0" (read at the context's first chance
 * to queue: the library stays loaded between programs). */
static BOOL no_queue(struct OGLES2Context *ctx)
{
    if (!ctx->no_queue) {
        char b[8];
        BOOL off = GetVar((STRPTR) "OGLES2/NoQueue", (STRPTR)b, sizeof(b), GVF_GLOBAL_ONLY) > 0 && b[0] != '0';
        ctx->no_queue = off ? 2 : 1;
        if (off)
            ogles2_log("job: ENV:OGLES2/NoQueue: framebuffer-object jobs wait for the running render");
    }
    return ctx->no_queue == 2;
}

/* A flush may leave its render running (see the top): a framebuffer
 * object's job, or one on a screen of our own or on the screen a window is
 * all of (display.c, window_is_screen). Never one whose bitmap lock
 * someone else may need: an application's bitmap can be drawn into by
 * another task, and the window mode's copy needs the finished frame. With
 * any number of contexts and programs (M6, plan 7.1 phase 2 within ogles2):
 * the submission lock is given up once the render is submitted, and the
 * next submitter waits for that render on the GPU (take_gpu). */
static BOOL may_defer(struct OGLES2Context *ctx, BOOL object)
{
    return object || ctx->display.own_screen || ctx->display.direct;
}

/* Before a submission, with the submission lock held: a render some
 * context left running finishes on the GPU first (one job on the GPU at a
 * time: the waits read the GPU's frame counts). This task's own other
 * context: waited for whole (job_finish). Another task's: only on the
 * GPU, and marked done, so its owner does not wait again (two renders past
 * its count could look like none at the count's wrap-around); the owner
 * does the rest at its next flush. Nobody holds the lock while it builds
 * its next frame, so programs take turns at the GPU and none waits for
 * another's CPU work (programs whose tasks wait for each other included). */
static void take_gpu(struct OGLES2Context *ctx)
{
    struct OGLES2Context *p;
    struct Task *t;
    UBYTE frame;
    int wr;
    ObtainSemaphore(&OGLES2LibBase->lock);
    p = OGLES2LibBase->pend_ctx;
    t = OGLES2LibBase->pend_task;
    frame = OGLES2LibBase->pend_frame;
    ReleaseSemaphore(&OGLES2LibBase->lock);
    if (!p || p == ctx)
        return;
    if (t == FindTask(0)) {
        ogles2_job_finish(p);
        return;
    }
    wr = ogles2_backend->wait_render(frame);
    if (wr != OGLES2_WAIT_OK)
        ogles2_device_reset("another program's render it did not finish");
    ObtainSemaphore(&OGLES2LibBase->lock);
    if (OGLES2LibBase->pend_ctx == p) { /* still not finished by its owner */
        p->pend_done = TRUE;
        OGLES2LibBase->pend_ctx = 0;
    }
    ReleaseSemaphore(&OGLES2LibBase->lock);
}

/* A job of the backend's own, not this file's (VC4's copy from the display): no
 * other program's render left on the GPU; the GPU's lock held. */
void ogles2_job_idle_gpu(struct OGLES2Context *ctx)
{
    take_gpu(ctx);
}

/* Renders the open job and, unless it may run on (may_defer), waits for
 * it. Returns FALSE if the frame was dropped (the reason is logged). */
BOOL ogles2_job_flush(struct OGLES2Context *ctx, BOOL frame_end)
{
    const struct OGLES2Backend *be = ogles2_backend;
    struct SignalSemaphore *gpu = ogles2_device_gpu_lock();
    int wb = OGLES2_WAIT_OK, wr = OGLES2_WAIT_OK;
    const char *drop = 0;
    struct OGLES2Framebuffer *fb = ctx->job_fb;
    struct OGLES2Target tg;
    BOOL store_zs = FALSE;
    ULONG t0, t1, t2, t3;
    BOOL defer, queue = FALSE;

    /* A framebuffer object's job behind the default framebuffer's running
     * render: queued (see the top), not waited for. Decided from what the
     * job's start found (its tile pools), nothing computed here: the
     * target's surfaces are set up below, as for any job. A target on the
     * context's pools (the running render may use them) gets its own from
     * its next job on. */
    if (ctx->job_open && fb && !frame_end && ctx->pend && !ctx->pend_fb && !ctx->q_active &&
        be->job_can_queue && !no_queue(ctx)) {
        if (!ctx->job_own_pools) {
            ogles2_fbo_want_own_pools(fb);
        } else if (be->job_can_queue(ctx)) {
            queue = TRUE;
        }
    }
    if (!queue)
        ogles2_job_finish(ctx); /* the previous render, if it still runs */
    if (!ctx->job_open) {
        if (frame_end)
            ctx->zs_saved = FALSE;
        return TRUE;
    }
    t0 = t1 = t2 = t3 = ogles2_clock_us();
    ctx->sum_build += (double)(ULONG)(t0 - ctx->t_job_begin);
    ctx->n_jobs++;
    /* Depth and stencil outlive a mid-frame flush when the job drew, and
     * also when it only loaded them or only cleared them: a job with nothing
     * but a colour clear between two read-backs must hand them on (dEQP-GLES2
     * multisample.stencil lost its stencil there). */
    if (!fb && !frame_end && (be->job_has_draws(ctx) || ctx->zs_saved || ctx->job_clear_zs) &&
        !(ctx->job_discard & (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)))
        store_zs = be->job_keep_zs(ctx); /* FALSE without depth and stencil, or memory */
    if ((drop = be->job_check(ctx)) != 0)
        goto out;
    if (fb && !ogles2_fbo_target(ctx, fb, TRUE, &tg)) {
        drop = "the framebuffer object's images have no GPU memory";
        goto out;
    }
    if (queue && (!ctx->pend || !tg.ta_addr)) {
        /* (Its pools were made anew, waiting for the running render, or
         * are the context's after all: an ordinary job.) */
        queue = FALSE;
        ogles2_job_finish(ctx);
    }
    defer = may_defer(ctx, fb != 0);
    if (!v3d_gpu_lock(gpu, GPU_LOCK_TIMEOUT_MS)) {
        drop = "the GPU stayed busy (another program holds the submission lock)";
        goto out;
    }
    take_gpu(ctx); /* with the lock: a render left running finishes first */
    if (!fb && !ogles2_display_lock(&ctx->display)) {
        v3d_gpu_unlock(gpu);
        drop = "the render bitmap could not be locked";
        goto out;
    }

    if (!be->job_build(ctx, fb ? &tg : 0, store_zs)) {
        if (!fb)
            ogles2_display_unlock(&ctx->display);
        v3d_gpu_unlock(gpu);
        drop = "a control list overflowed";
        goto out;
    }
    t1 = ogles2_clock_us();
    ctx->sum_list += (double)(ULONG)(t1 - t0);
    clean_all(); /* the target's lines and the lists the CPU wrote */
    t2 = ogles2_clock_us();
    ctx->sum_pre += (double)(ULONG)(t2 - t1);
    t1 = t2;
    wb = be->job_bin(ctx, fb ? &tg : 0);
    t2 = ogles2_clock_us();
    ctx->sum_bin += (double)(ULONG)(t2 - t1);
    if (wb == OGLES2_WAIT_OK && queue && ctx->pend) {
        /* Binned while the running render goes on; its own render starts
         * in ogles2_job_finish, after that one (render_queued). Nothing of
         * it is on the GPU now, so no other submitter has to wait for it. */
        be->job_queue(ctx);
        ctx->q_hw = FALSE;
        if ((ctx->probe & OGLES2_PROBE_HWQUEUE) && be->job_render_queued) {
            /* Probe H: its render written to the render thread's queue now,
             * behind pend's (CT1QBA/QEA while thread 1 runs); it ends one
             * render after pend's. Another submitter waits for it. */
            if (v3d_get_render_frame_count() == ctx->pend_frame)
                ctx->n_hw_busy++;
            be->job_render_queued(ctx);
            ctx->q_hw = TRUE;
            ctx->q_frame = (UBYTE)(ctx->pend_frame + 1);
            ObtainSemaphore(&OGLES2LibBase->lock);
            ctx->pend_done = FALSE;
            OGLES2LibBase->pend_ctx = ctx;
            OGLES2LibBase->pend_task = FindTask(0);
            OGLES2LibBase->pend_frame = ctx->q_frame;
            ReleaseSemaphore(&OGLES2LibBase->lock);
        }
        ctx->q_active = TRUE;
        ctx->q_serial = ctx->job_serial;
        ctx->q_fb = fb;
        ctx->q_tg = tg;
        ctx->n_queued++;
        v3d_gpu_unlock(gpu);
        ctx->frames++;
        be->job_end(ctx, TRUE);
        ogles2_job_gen++;
        ctx->job_open = FALSE;
        if (frame_end)
            ctx->zs_saved = FALSE;
        return TRUE;
    }
    if (wb == OGLES2_WAIT_OK) {
        /* (queue without pend: the render it was to follow ended meanwhile;
         * an ordinary job now.) */
        UBYTE frame_no = be->job_render(ctx);
        if (defer) {
            /* The render runs on; ogles2_job_finish waits for it. The
             * screen's bitmap is unlocked now: it is on display, so it does
             * not move, and the lock held across the program's next frame
             * blocked input.device (P96's pointer is drawn into the screen
             * bitmap): ioquake3's input handler removal hung on it at exit. */
            if (!fb)
                ogles2_display_unlock(&ctx->display);
            ctx->pend = TRUE;
            ctx->pend_frame = frame_no;
            ctx->pend_serial = ctx->job_serial;
            ctx->pend_fb = fb;
            ctx->pend_done = FALSE;
            ObtainSemaphore(&OGLES2LibBase->lock);
            OGLES2LibBase->pend_ctx = ctx;
            OGLES2LibBase->pend_task = FindTask(0);
            OGLES2LibBase->pend_frame = frame_no;
            ReleaseSemaphore(&OGLES2LibBase->lock);
            /* The lock goes now, not at the program's next flush: the next
             * submitter, of any program, waits for this render on the GPU
             * (take_gpu). */
            v3d_gpu_unlock(gpu);
            if (fb)
                ctx->pend_tg = tg;
            ctx->frames++;
            be->job_end(ctx, TRUE);
            ogles2_job_gen++;
            ctx->job_open = FALSE;
            if (fb) {
                if (frame_end)
                    ctx->zs_saved = FALSE;
            } else {
                ctx->zs_saved = store_zs; /* undone if the render fails */
            }
            return TRUE;
        }
        wr = be->wait_render(frame_no);
        t3 = ogles2_clock_us();
        ctx->sum_render += (double)(ULONG)(t3 - t2);
        after_render(ctx, fb, &tg, wr, TRUE);
    } else {
        /* A job the GPU did not finish leaves it busy or stopped: reset it
         * before anyone submits again (plan 9 M4); a render this one was to
         * queue behind is waited for first (the reset would cut it short). */
        if (queue)
            ogles2_job_finish(ctx);
        ogles2_device_reset("a binning job it did not finish");
        if (fb) {
            target_dma(&tg, TRUE);
        } else {
            ogles2_display_dma(&ctx->display, TRUE);
            ogles2_display_unlock(&ctx->display);
        }
        v3d_gpu_unlock(gpu);
        drop = wb == OGLES2_WAIT_TIMED_OUT ? "binning timed out" : "binning failed";
        be->job_report_stop(ctx);
    }
    ctx->frames++;
    be->job_end(ctx, TRUE);
    ogles2_job_gen++;
    ctx->job_open = FALSE;

out:
    if (ctx->job_open) { /* dropped before it was submitted */
        be->job_end(ctx, FALSE);
        ctx->job_open = FALSE;
    }
    if (fb) {
        /* The default framebuffer's saved depth and stencil stay for its
         * next job, unless the frame ends here. */
        if (frame_end)
            ctx->zs_saved = FALSE;
    } else {
        /* The next job loads depth and stencil only if this one stored them
         * and rendered (a frame end stores nothing). */
        ctx->zs_saved = !drop && wr == OGLES2_WAIT_OK && store_zs;
    }
    if (drop) {
        ctx->frames_dropped++;
        ogles2_log("job: frame dropped: %s", (ULONG)drop);
        return FALSE;
    }
    return wr == OGLES2_WAIT_OK;
}

BOOL ogles2_job_has_draws(struct OGLES2Context *ctx)
{
    return ctx->job_open && ogles2_backend->job_has_draws(ctx);
}

/* aglDestroyContext: forget an open job (nothing was submitted). */
void ogles2_job_discard(struct OGLES2Context *ctx)
{
    ogles2_job_finish(ctx);
    if (ctx->job_open) {
        ogles2_backend->job_end(ctx, FALSE);
        ctx->job_open = FALSE;
    }
}
