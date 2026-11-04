/*
 * vc4core's jobs (docs/PI3_PLAN.md §3.1): a binning list and a render list
 * for one target, run and waited for.
 *
 * The render list names every tile, as Linux's vc4_render_cl.c builds it
 * (Mesa's kernel/vc4_render_cl.c is an older copy, MIT): the clear colours
 * and a store in None mode come first, then TILE_RENDERING_MODE_CONFIG (the
 * other order, Mesa's copy's, stores the previous job's clear colour: the
 * probe, PI3_PLAN §2 "Command lists"); per tile its loads, its coordinates,
 * the wait for the binner before the first branch, the branch to the tile's
 * list in the tile allocation, its stores, the last with the end of frame.
 * Before the render the L2 and TMU caches are flushed, as Linux does.
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"
#include "vc4_os.h"
#include "vc4_regs.h"

#include <string.h>

#define CL_BLOCK (256u << 10)
#define STATE_BLOCK (1u << 20)

int vc4_job_init(struct vc4_job *job)
{
    memset(job, 0, sizeof(*job));
    if (vc4_cl_init(&job->bcl, CL_BLOCK) < 0 || vc4_cl_init(&job->rcl, CL_BLOCK) < 0 ||
        vc4_buf_init(&job->state, STATE_BLOCK) < 0) {
        vc4_job_free(job);
        return -1;
    }
    return 0;
}

void vc4_job_free(struct vc4_job *job)
{
    vc4_cl_free(&job->bcl);
    vc4_cl_free(&job->rcl);
    vc4_buf_free(&job->state);
}

int vc4_job_begin(struct vc4_job *job, const struct vc4_target *t, struct vc4_pools *pools)
{
    job->target = *t;
    job->pools = pools;
    /* 64x64 tiles, 32x32 with 4x MSAA (the tile buffer holds four samples
     * a pixel: Mesa's vc4_job.c) */
    job->tiles_x = (uint16_t)(t->msaa ? (t->width + 31) / 32 : (t->width + 63) / 64);
    job->tiles_y = (uint16_t)(t->msaa ? (t->height + 31) / 32 : (t->height + 63) / 64);
    if (job->tiles_x > pools->tiles_x || job->tiles_y > pools->tiles_y) {
        vc4_os_log("job: %ux%u (%s) does not fit pools of %ux%u tiles", t->width, t->height,
                   t->msaa ? "4x MSAA" : "1x", pools->tiles_x, pools->tiles_y);
        return -1;
    }
    vc4_cl_reset(&job->bcl);
    vc4_cl_reset(&job->rcl);
    vc4_buf_reset(&job->state);
    VC4_EMIT(&job->bcl, TILE_BINNING_MODE_CONFIGURATION,
             .tile_allocation_memory_address = pools->talloc,
             .tile_allocation_memory_size = pools->talloc_size,
             .tile_state_data_array_address = pools->tsda,
             .width_in_tiles = job->tiles_x,
             .height_in_tiles = job->tiles_y,
             .multisample_mode_4x = t->msaa ? 1u : 0u,
             .auto_initialise_tile_state_data_array = 1,
             .tile_allocation_initial_block_size = VC4_TILE_ALLOCATION_INITIAL_BLOCK_SIZE_BLOCK_SIZE_32,
             .tile_allocation_block_size = VC4_TILE_ALLOCATION_BLOCK_SIZE_BLOCK_SIZE_128);
    VC4_EMIT0(&job->bcl, START_TILE_BINNING);
    /* As Mesa's vc4_start_draw: once a job (the hardware takes each draw's
     * primitive from its draw packet). */
    VC4_EMIT(&job->bcl, PRIMITIVE_LIST_FORMAT, .data_type = VC4_DATA_TYPE__16_BIT_INDEX,
             .primitive_type = VC4_PRIMITIVE_TYPE_TRIANGLES_LIST);
    return job->bcl.failed ? -1 : 0;
}

/* The tile coordinates (they also start a pending load, and place loads,
 * stores and the clipping). */
static void coords(struct vc4_cl *cl, int x, int y)
{
    VC4_EMIT(cl, TILE_COORDINATES, .tile_column_number = (uint32_t)x, .tile_row_number = (uint32_t)y);
}

/* A store in None mode: runs a pending load (or makes new clear values
 * take: then without the disable-clear bits). */
static void store_none(struct vc4_cl *cl, int keep)
{
    VC4_EMIT(cl, STORE_TILE_BUFFER_GENERAL,
             .buffer_to_store = VC4_BUFFER_TO_STORE_NONE,
             .disable_color_buffer_clear_on_store_dump = (uint32_t)keep,
             .disable_z_stencil_buffer_clear_on_store_dump = (uint32_t)keep,
             .disable_vg_mask_buffer_clear_on_store_dump = (uint32_t)keep);
}

/* VC4_COLOR_* (the rendering mode's encoding) as the loads' and stores'. */
static uint32_t pixel_format(uint8_t f)
{
    return f == VC4_COLOR_RGBA8888 ? VC4_PIXEL_COLOR_FORMAT_RGBA8888
         : f == VC4_COLOR_BGR565_DITHERED ? VC4_PIXEL_COLOR_FORMAT_BGR565_DITHERED
                                          : VC4_PIXEL_COLOR_FORMAT_BGR565_NO_DITHER;
}

static void build_rcl(struct vc4_job *job)
{
    const struct vc4_target *t = &job->target;
    struct vc4_cl *cl = &job->rcl;
    int load_c = (t->load & VC4_COLOR) && !(t->clear & VC4_COLOR) && t->color.bus;
    int load_zs = (t->load & VC4_ZS) && !(t->clear & VC4_ZS) && t->zs.bus;
    int store_c = (t->store & VC4_COLOR) && t->color.bus;
    int store_zs = (t->store & VC4_ZS) && t->zs.bus;
    int x, y;

    if (t->clear) {
        VC4_EMIT(cl, CLEAR_COLORS,
                 .clear_color = (uint64_t)t->clear_color << 32 | t->clear_color,
                 .clear_zs = t->clear_z & 0xffffffu,
                 .clear_vg_mask = 0,
                 .clear_stencil = t->clear_s);
        coords(cl, 0, 0);
        store_none(cl, 0);
    }
    VC4_EMIT(cl, TILE_RENDERING_MODE_CONFIGURATION,
             .memory_address = store_c ? t->color.bus : 0,
             .width_pixels = t->width,
             .height_pixels = t->height,
             .non_hdr_frame_buffer_color_format = t->color.format,
             .memory_format = t->color.tiling,
             /* 4x MSAA: the general loads and stores go over the pixels
              * (a load fills each pixel's four samples), and the resolved
              * store averages the four (Mesa's vc4_job.c) */
             .multisample_mode_4x = t->msaa ? 1u : 0u,
             .decimate_mode = t->msaa ? 1u : 0u);
    for (y = 0; y < job->tiles_y; y++)
        for (x = 0; x < job->tiles_x; x++) {
            int first = !x && !y, last = x == job->tiles_x - 1 && y == job->tiles_y - 1;
            if (load_c)
                VC4_EMIT(cl, LOAD_TILE_BUFFER_GENERAL,
                         .memory_base_address_of_frame_tile_dump_buffer = t->color.bus,
                         .buffer_to_store = VC4_BUFFER_TO_STORE_COLOR,
                         .format = t->color.tiling,
                         .pixel_color_format = pixel_format(t->color.format));
            if (load_zs) {
                if (load_c) { /* one load at a time: run the colour one first */
                    coords(cl, x, y);
                    store_none(cl, 1);
                }
                VC4_EMIT(cl, LOAD_TILE_BUFFER_GENERAL,
                         .memory_base_address_of_frame_tile_dump_buffer = t->zs.bus,
                         .buffer_to_store = VC4_BUFFER_TO_STORE_Z_STENCIL,
                         .format = t->zs.tiling);
            }
            coords(cl, x, y);
            if (first)
                VC4_EMIT0(cl, WAIT_ON_SEMAPHORE);
            VC4_EMIT(cl, BRANCH_TO_SUB_LIST, .address = job->pools->talloc + (uint32_t)(y * job->tiles_x + x) * 32);
            if (store_zs) {
                VC4_EMIT(cl, STORE_TILE_BUFFER_GENERAL,
                         .memory_base_address_of_frame_tile_dump_buffer = t->zs.bus,
                         .buffer_to_store = VC4_BUFFER_TO_STORE_Z_STENCIL,
                         .format = t->zs.tiling,
                         .disable_color_buffer_clear_on_store_dump = (uint32_t)store_c,
                         .last_tile_of_frame = (uint32_t)(last && !store_c));
                if (store_c)
                    coords(cl, x, y);
            }
            if (store_c) {
                if (last)
                    VC4_EMIT0(cl, STORE_MULTI_SAMPLE_RESOLVED_TILE_COLOR_BUFFER_AND_EOF);
                else
                    VC4_EMIT0(cl, STORE_MULTI_SAMPLE_RESOLVED_TILE_COLOR_BUFFER);
            } else if (!store_zs) { /* nothing to store: the frame still ends */
                VC4_EMIT(cl, STORE_TILE_BUFFER_GENERAL, .buffer_to_store = VC4_BUFFER_TO_STORE_NONE,
                         .last_tile_of_frame = (uint32_t)last);
            }
        }
}

/* Waits until count register reg (BFC, RFC: 8 bits) reaches 1; the binner's
 * out-of-memory requests are served meanwhile with the pools' overflow
 * memory. */
static int wait_count(struct vc4_job *job, uint32_t reg, uint32_t timeout_us, uint32_t *took)
{
    uint64_t t0 = vc4_os_time_us(), t;
    for (;;) {
        t = vc4_os_time_us();
        if (vc4_read(reg) & 0xff) {
            *took = (uint32_t)(t - t0);
            return 0;
        }
        if (reg == V3D_BFC && (vc4_read(V3D_INTCTL) & V3D_INT_OUTOMEM)) {
            /* The binner used up the overflow memory it had: the next chunk
             * (Linux's vc4_overflow_mem_work gives a new bin slot). */
            uint32_t size, at;
            if ((at = vc4_pools_overflow(job->pools, ++job->overflows, &size)) == 0) {
                vc4_os_log("job: the binner needs more than the pools' %u overflow chunks",
                           job->pools->overflow_chunks);
                return -1; /* it would wait for ever */
            }
            /* the new memory first, then the request cleared (Linux's
             * vc4_overflow_mem_work: else it is raised again at once) */
            vc4_write(V3D_BPOA, at);
            vc4_write(V3D_BPOS, size);
            vc4_write(V3D_INTCTL, V3D_INT_OUTOMEM);
        }
        if (t - t0 > timeout_us)
            return -1;
    }
}

int vc4_job_run(struct vc4_job *job, uint32_t timeout_us)
{
    uint32_t err;
    VC4_EMIT0(&job->bcl, INCREMENT_SEMAPHORE);
    VC4_EMIT0(&job->bcl, FLUSH);
    build_rcl(job);
    if (job->bcl.failed || job->rcl.failed || job->state.failed) {
        vc4_os_log("job: out of GPU memory for its lists");
        return -1;
    }
    vc4_cl_flush(&job->bcl);
    vc4_cl_flush(&job->rcl);
    vc4_buf_flush(&job->state);
    job->overflows = 0;
    {
        uint32_t size, at = vc4_pools_overflow(job->pools, 0, &size);
        vc4_write(V3D_BPOA, at);
        vc4_write(V3D_BPOS, size);
    }
    vc4_write(V3D_INTCTL, V3D_INT_OUTOMEM);
    vc4_write(V3D_BFC, 1); /* writing 1 clears the counts */
    vc4_write(V3D_RFC, 1);
    vc4_write(V3D_L2CACTL, V3D_L2CACTL_L2CCLR);
    vc4_write(V3D_SLCACTL, 0x0f0f0f0fu); /* TMU1, TMU0, uniform, instruction caches of each slice */
    vc4_write(V3D_CT0CA, vc4_cl_start(&job->bcl));
    vc4_write(V3D_CT0EA, vc4_cl_end(&job->bcl)); /* starts the binner */
    if (wait_count(job, V3D_BFC, timeout_us, &job->bin_us) < 0) {
        vc4_reset("a binning job it did not finish");
        return -2;
    }
    job->errstat_bin = vc4_read(V3D_ERRSTAT);
    /* Before the render, as Linux's vc4_submit_next_render_job. */
    vc4_write(V3D_L2CACTL, V3D_L2CACTL_L2CCLR);
    vc4_write(V3D_SLCACTL, 0x0f0f0000u);
    vc4_write(V3D_CT1CA, vc4_cl_start(&job->rcl));
    vc4_write(V3D_CT1EA, vc4_cl_end(&job->rcl));
    if (wait_count(job, V3D_RFC, timeout_us, &job->render_us) < 0) {
        vc4_reset("a render job it did not finish");
        return -3;
    }
    err = vc4_read(V3D_ERRSTAT);
    job->errstat = err;
    if (err != V3D_ERRSTAT_IDLE) {
        /* The error bits stay set until written back (1 clears: found
         * 2026-11-04; else every later job reports them) */
        vc4_write(V3D_ERRSTAT, err & ~V3D_ERRSTAT_IDLE);
        vc4_os_log("job: ERRSTAT 0x%08x after it, 0x%08x after its binning (0x%08x when all is well)", err,
                   job->errstat_bin, V3D_ERRSTAT_IDLE);
    }

    return 0;
}
