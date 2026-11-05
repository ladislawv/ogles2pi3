/*
 * The VC4 backend's table (ogles2/backend.h, docs/PI3_PLAN.md §5.1, P5):
 * the Raspberry Pi 3's VideoCore IV on vc4core. Here the capabilities, the
 * device, GPU memory and the contexts; the groups in job_vc4.c, draw_vc4.c,
 * texture_vc4.c, fbo_vc4.c and program_vc4.c; the built-in image (clears of
 * part of a framebuffer: tools/clear_vc4). Not written yet, and refused:
 * glGenerateMipmap and glCopyTex* on the GPU (the front end's CPU paths run
 * instead), the internal programs (none needed with one draw buffer: the
 * copy program comes with glCopyTex* on the GPU). The device open does not choose this backend yet: the
 * Pi 4 runs as before until the A1200 has the Pi 3 card for the first time.
 *
 * Contexts: the front end keeps a context's backend object in ctx->v3d, the
 * V3D backend's type; this backend keeps its own struct vc4_ctx there
 * (backend_vc4.h).
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <string.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "vc4core.h"
#include "vc4_os.h"
#include "v3d_types.h" /* struct v3d_mem: the front end's GPU block */
#include "backend_vc4.h"
#include "builtin_vc4p.h"

/* ---- capabilities: §2's limits, §2.1's extensions as far as done ---- */

static const struct OGLES2Caps caps = {
    /* the front end's own and the native ones (§2.1); the emulated ones as
     * they are written */
    "GL_OES_mapbuffer GL_EXT_discard_framebuffer GL_EXT_read_format_bgra GL_OES_get_program_binary "
    "GL_EXT_debug_marker GL_OES_required_internalformat GL_OES_rgb8_rgba8 "
    "GL_OES_draw_elements_base_vertex GL_OES_compressed_ETC1_RGB8_texture GL_OES_packed_depth_stencil "
    "GL_EXT_blend_minmax GL_EXT_texture_format_BGRA8888 GL_OES_texture_npot GL_OES_fbo_render_mipmap "
    "GL_OES_standard_derivatives",
    2048,  /* textures (vc4_screen.c) */
    2048,  /* viewport and renderbuffers: Mesa's on the Pi 3 (§2 "Render targets") */
    1,     /* draw buffers: one colour buffer (EXT_draw_buffers comes emulated, §2.1) */
    8,     /* varyings */
    8,     /* vertex attributes */
    4088, 4095, /* uniform vectors, as the compiler's gl_Max* (§4.1) */
    4,     /* subpixel bits */
    32.0f, /* lines */
    512.0f, /* points */
    1.0f,  /* no anisotropy yet (emulated, §2.1) */
    0.0f,  /* no LOD bias yet */
};

/* ---- the device ---- */

static LONG state;          /* 0 not tried, 1 ready, -1 failed */
static ULONG bytes_in_use;
static struct SignalSemaphore *gpu_lock;
#define VC4_GPU_LOCK_NAME "VC4 submit"

/* vc4core's log lines (os_amiga.c) */
void vc4_os_log_line(const char *line)
{
    ogles2_log("vc4core: %s", (ULONG)line);
}

/* The public submission lock other GPU clients of the Pi 3 share: found,
 * or made and added once. */
static struct SignalSemaphore *lock_get(void)
{
    static const char name[] = VC4_GPU_LOCK_NAME;
    struct SignalSemaphore *s;
    Forbid();
    s = FindSemaphore((STRPTR)name);
    if (!s && (s = AllocVec(sizeof(*s) + sizeof(name), MEMF_PUBLIC | MEMF_CLEAR))) {
        char *n = (char *)(s + 1);
        ULONG k;
        for (k = 0; k < sizeof(name); k++)
            n[k] = name[k];
        s->ss_Link.ln_Name = n;
        s->ss_Link.ln_Pri = 0;
        AddSemaphore(s);
    }
    Permit();
    return s;
}

static const char *renderer(void)
{
    return "VideoCore IV V3D 2.1 (PiStorm)";
}

BOOL ogles2_vc4_present(void)
{
    return vc4_os_present() ? TRUE : FALSE;
}

static BOOL dev_open(void)
{
    ObtainSemaphore(&OGLES2LibBase->lock);
    if (!gpu_lock)
        gpu_lock = lock_get();
    if (state == 0) {
        state = vc4_open() == 0 ? 1 : -1;
        ogles2_log("device: VC4 %s", (ULONG)(state == 1 ? "taken over" : "not found or not answering"));
    }
    ReleaseSemaphore(&OGLES2LibBase->lock);
    return state == 1;
}

static BOOL ready(void)
{
    return state == 1;
}

static void reset(const char *why)
{
    int r = vc4_reset(why);
    ogles2_log("device: GPU reset after %s: %s", (ULONG)why, (ULONG)(r == 0 ? "the core answers" : "NO ANSWER"));
}

static ULONG bytes(void)
{
    return bytes_in_use;
}

ULONG ogles2_vc4_bus(ULONG cpu)
{
    return vc4_os_bus((const void *)cpu);
}

static BOOL gpu_reachable(ULONG addr, ULONG size)
{
    return vc4_os_gpu_reachable(addr, size) ? TRUE : FALSE;
}

static struct SignalSemaphore *lock(void)
{
    return gpu_lock;
}

/* ---- GPU memory: vc4core's blocks as the front end's struct v3d_mem ---- */

static int mem_alloc(struct v3d_mem *m, ULONG size)
{
    struct vc4_mem b;
    if (vc4_mem_alloc(&b, size) < 0)
        return -1;
    m->size = b.size;
    m->handle = b.handle;
    m->busaddr = b.bus;
    m->hostptr = b.cpu;
    bytes_in_use += b.size;
    return 0;
}

static void mem_free(struct v3d_mem *m)
{
    struct vc4_mem b;
    if (!m->size)
        return;
    b.cpu = m->hostptr;
    b.bus = m->busaddr;
    b.size = m->size;
    b.handle = m->handle;
    bytes_in_use -= m->size;
    vc4_mem_free(&b);
    m->size = 0;
    m->handle = 0;
    m->busaddr = 0;
    m->hostptr = 0;
}

/* ---- a context's pools and jobs ---- */

/* Pools for a w x h frame in the tile size of its mode (64, 32 with MSAA),
 * a tile column more: the frame is as wide as the bitmap's row, which may
 * be longer than the display (job_vc4.c). */
static BOOL pools_for(struct vc4_ctx *c, UWORD w, UWORD h)
{
    UWORD t = c->msaa ? 32 : 64;
    vc4_pools_free(&c->pools);
    return vc4_pools_init(&c->pools, (UWORD)((w + t - 1) / t + 1), (UWORD)((h + t - 1) / t)) == 0;
}

static struct V3DContext *context_new(UWORD w, UWORD h, int depth_bits, BOOL msaa)
{
    struct vc4_ctx *c = AllocVec(sizeof(*c), MEMF_ANY | MEMF_CLEAR);
    if (!c)
        return 0;
    c->w = w;
    c->h = h;
    c->depth_bits = depth_bits;
    c->msaa = msaa;
    if (!pools_for(c, w, h) || vc4_job_init(&c->job[0]) < 0 || vc4_job_init(&c->job[1]) < 0) {
        vc4_pools_free(&c->pools);
        vc4_job_free(&c->job[0]);
        vc4_job_free(&c->job[1]);
        FreeVec(c);
        return 0;
    }
    return (struct V3DContext *)c; /* the front end's handle; this backend's object */
}

static BOOL context_resize(struct V3DContext *v, UWORD w, UWORD h)
{
    struct vc4_ctx *c = (struct vc4_ctx *)v;
    UWORD t = c->msaa ? 32 : 64;
    if ((UWORD)((w + t - 1) / t + 1) <= c->pools.tiles_x && (UWORD)((h + t - 1) / t) <= c->pools.tiles_y) {
        c->w = w;
        c->h = h;
        return TRUE;
    }
    if (!pools_for(c, w, h))
        return FALSE;
    c->w = w;
    c->h = h;
    return TRUE;
}

static void context_free(struct V3DContext *v)
{
    struct vc4_ctx *c = (struct vc4_ctx *)v;
    int k;
    if (!c)
        return;
    for (k = 0; k < VC4_SLOTS; k++) {
        ogles2_vc4_defer_free_all(c, k);
        vc4_job_free(&c->job[k]);
    }
    if (c->zs.size)
        vc4_mem_free(&c->zs);
    vc4_pools_free(&c->pools);
    FreeVec(c);
}

/* ---- not written yet: refused (the front end's CPU paths run instead) ---- */

static BOOL tex_can_generate(const struct OGLES2Texture *t, int n) { (void)t; (void)n; return FALSE; }
static int tex_generate(struct OGLES2Texture *t, int n) { (void)t; (void)n; return GL_INVALID_OPERATION; }
/* -3 (texture.c: "not now"): the CPU copies, without the TFU's log line each time */
static int tex_from_raster(struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w, UWORD h, BOOL bgra) { (void)t; (void)src; (void)stride; (void)w; (void)h; (void)bgra; return -3; }
static const UBYTE *internal_program(int which, ULONG *len) { (void)which; *len = 0; return 0; }

/* ---- the built-in image: tools/clear_vc4's program, loaded at its first
 * use (its code in GPU memory, kept while the library is), with its sources
 * for the variants of clear states it does not hold (ogles2_image_keyed) ---- */

static struct OGLES2Image *builtin;
static BOOL builtin_failed;

static char *dup_str(const char *s)
{
    ULONG n = 0;
    char *d;
    while (s[n])
        n++;
    if ((d = AllocVec(n + 1, MEMF_ANY)) != 0)
        memcpy(d, s, n + 1);
    return d;
}

static const struct OGLES2Image *builtin_image(void)
{
    char why[96];
    if (!builtin && !builtin_failed && state == 1) {
        if (!(builtin = ogles2_v3dp_load(builtin_vc4p, sizeof(builtin_vc4p), why, sizeof(why)))) {
            builtin_failed = TRUE;
            ogles2_log("device: the built-in clear program was refused: %s", (ULONG)why);
        } else {
            builtin->name = "built-in clear (tools/clear_vc4)";
            builtin->vs_src = dup_str(builtin_vc4_vs);
            builtin->fs_src = dup_str(builtin_vc4_fs);
        }
    }
    return builtin;
}

const struct OGLES2Backend ogles2_backend_vc4 = {
    "VC4 (V3D 2.1)",
    21,
    &caps,
    7,       /* VC4_VARIANT */
    ".vc4p",
    renderer,
    dev_open,
    ready,
    reset,
    bytes,
    gpu_reachable,
    lock,
    context_new,
    context_resize,
    context_free,
    mem_alloc,
    mem_free,
    ogles2_vc4_mem_free_after,
    ogles2_vc4_job_begin,
    ogles2_vc4_job_has_draws,
    ogles2_vc4_job_keep_zs,
    ogles2_vc4_job_check,
    ogles2_vc4_job_build,
    ogles2_vc4_job_bin,
    ogles2_vc4_job_render,
    ogles2_vc4_wait_render,
    ogles2_vc4_job_retire,
    ogles2_vc4_job_end,
    ogles2_vc4_job_report_stop,
    ogles2_vc4_job_free,
    ogles2_vc4_draw,
    ogles2_vc4_draw_free,
    ogles2_vc4_tex_upload,
    ogles2_vc4_tex_store,
    ogles2_vc4_tex_load,
    ogles2_vc4_tex_free,
    ogles2_vc4_tex_rename,
    ogles2_vc4_tex_levels,
    ogles2_vc4_tex_holds,
    tex_can_generate,
    tex_generate,
    tex_from_raster,
    ogles2_vc4_rb_zs_storage,
    ogles2_vc4_rb_surface,
    ogles2_vc4_target_layout,
    ogles2_vc4_target_pools,
    ogles2_vc4_tex_surface,
    ogles2_vc4_tex_zs_surface,
    ogles2_vc4_variant_read,
    ogles2_vc4_variant_recipe,
    ogles2_vc4_variant_choose,
    builtin_image,
    internal_program,
};
