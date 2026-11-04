/*
 * The GPU backend interface (docs/PI3_PLAN.md §5.1): one ogles2.library for
 * the Raspberry Pi 4 (V3D 4.2, ogles2/v3d/) and the Raspberry Pi 3 (VC4,
 * ogles2/vc4/). The front end calls the GPU only through the table the
 * device open chooses. Built up step by step in P2, each step checked on the
 * Pi 4 against 1.3.3. Step 1: the device's identity and its capabilities,
 * which the front end used to hard-code. Step 2: GPU memory. Step 3: jobs
 * (job.c keeps the serials, the clear snapshot, the locks, the overlap and
 * the statistics; the backend builds, submits and waits).
 *
 * Addresses. GPU memory is a struct v3d_mem: `hostptr` is where the CPU
 * reads and writes it, `busaddr` where the GPU does. The front end keeps
 * CPU addresses everywhere (a texture's level, a renderbuffer, a program's
 * code, a buffer's GPU copy, the display's bitmap) and a backend turns them
 * into bus addresses where it writes its own GPU records: on the Pi 4 the
 * two are the same (Emu68 maps memory 1:1 and the V3D reads physical
 * addresses), on the Pi 3 the VC4 reads them through its 0xC0000000 alias.
 *
 * Job generation. ogles2_job_gen (job.c) counts the restarts of the
 * backend's control lists; the front end's caches of addresses in them
 * (draw.c, texture.c) are keyed on it. Process-global and never reset (the
 * reasons are in job.c).
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_BACKEND_H
#define OGLES2_BACKEND_H

#include <exec/types.h>

struct v3d_mem;
struct OGLES2Context;
struct OGLES2Target;

/* A wait's result (the V3D 4.2 backend's v3d_wait_result has these values). */
enum { OGLES2_WAIT_OK, OGLES2_WAIT_ERROR, OGLES2_WAIT_TIMED_OUT };

/* What the GPU can do, as GL reports it (glGetString, glGet*) and as the
 * front end checks arguments against. */
struct OGLES2Caps
{
    const char *extensions;      /* GL_EXTENSIONS */
    UWORD max_texture_size;      /* 2D, cube map and rectangle textures */
    UWORD max_viewport;          /* GL_MAX_VIEWPORT_DIMS, GL_MAX_RENDERBUFFER_SIZE */
    UBYTE max_draw_buffers;      /* GL_MAX_DRAW_BUFFERS_EXT, colour attachments (at most OGLES2_MAX_DRAW_BUFFERS) */
    UBYTE max_varyings;          /* GL_MAX_VARYING_VECTORS */
    UWORD max_vs_uniform_vectors, max_fs_uniform_vectors; /* the compiler's gl_Max*UniformVectors */
    UBYTE subpixel_bits;         /* GL_SUBPIXEL_BITS */
    float line_width_max;        /* GL_ALIASED_LINE_WIDTH_RANGE, from 1 */
    float point_size_max;        /* GL_ALIASED_POINT_SIZE_RANGE, from 1 */
    float max_anisotropy;        /* EXT_texture_filter_anisotropic */
    float max_lod_bias;          /* EXT_texture_lod_bias */
};

struct OGLES2Backend
{
    const char *name;            /* for the log */
    UWORD device;                /* the .v3dp device version of its programs (v3dp/FORMAT.md): 42, 21 */
    const struct OGLES2Caps *caps;
    const char *(*renderer)(void); /* GL_RENDERER, once the device is open */
    /* GPU memory, zeroed, in a block the GPU reaches; 0 or -1 (out of
     * memory). Freeing a block a job may still read waits for the job
     * (ogles2_share_defer_free, the frame's own list); this frees now. */
    int (*mem_alloc)(struct v3d_mem *m, ULONG size);
    void (*mem_free)(struct v3d_mem *m);
    /* Jobs (job.c's order): begin the lists (tg: a framebuffer object's
     * target, or NULL for the default framebuffer and for an object
     * without tile pools, bad); has a draw; memory for depth and stencil
     * kept past the job (before the locks); why it cannot render, or 0;
     * build the render lists (GPU and bitmap locked; FALSE: overflow);
     * submit the binning and wait (OGLES2_WAIT_*); submit the render (its
     * frame number); wait for a render; after a finished render; the job
     * is over (submitted or dropped); log where a binning that failed
     * stopped; free the context's job memory. */
    void (*job_begin)(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL bad);
    BOOL (*job_has_draws)(struct OGLES2Context *ctx);
    BOOL (*job_keep_zs)(struct OGLES2Context *ctx);
    const char *(*job_check)(struct OGLES2Context *ctx);
    BOOL (*job_build)(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL store_zs);
    int (*job_bin)(struct OGLES2Context *ctx, const struct OGLES2Target *tg);
    UBYTE (*job_render)(struct OGLES2Context *ctx);
    int (*wait_render)(UBYTE frame);
    void (*job_retire)(struct OGLES2Context *ctx);
    void (*job_end)(struct OGLES2Context *ctx, BOOL submitted);
    void (*job_report_stop)(struct OGLES2Context *ctx);
    void (*job_free)(struct OGLES2Context *ctx);
};

#define ogles2_mem_alloc(m, size) (ogles2_backend->mem_alloc((m), (size)))
#define ogles2_mem_free(m) (ogles2_backend->mem_free(m))

/* The control-list generation (see the top). */
extern ULONG ogles2_job_gen;

/* The backend in use: the V3D 4.2 one until a device open chooses (a
 * context without a GPU, the ABI tests' state-only ones, reports its limits). */
extern const struct OGLES2Backend *ogles2_backend;

extern const struct OGLES2Backend ogles2_backend_v3d; /* ogles2/v3d/backend_v3d.c */

#endif
