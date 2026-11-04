/*
 * The VC4 backend's table (ogles2/backend.h, docs/PI3_PLAN.md §5.1, P5):
 * the Raspberry Pi 3's VideoCore IV on vc4core. Written a group at a time;
 * so far the capabilities, the device, GPU memory and the contexts' pools.
 * The other groups refuse (a job is dropped, a draw fails, a program file is
 * refused) until written. The device open does not choose it yet: the Pi 4
 * runs as before until the A1200 has the Pi 3 card for the first time.
 *
 * Contexts: the front end keeps a context's backend object in ctx->v3d, the
 * V3D backend's type; this backend keeps its own struct vc4_ctx there.
 *
 * MIT licence (LICENSE).
 */
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include "../ogles2_internal.h"
#include "../backend.h"
#include "vc4core.h"
#include "vc4_os.h"
#include "v3d_types.h" /* struct v3d_mem: the front end's GPU block */
#include "backend_vc4.h"

/* ---- capabilities: §2's limits, §2.1's extensions as far as done ---- */

static const struct OGLES2Caps caps = {
    /* the front end's own and the native ones (§2.1); the emulated ones as
     * they are written */
    "GL_OES_mapbuffer GL_EXT_discard_framebuffer GL_EXT_read_format_bgra GL_OES_get_program_binary "
    "GL_EXT_debug_marker GL_OES_required_internalformat GL_OES_rgb8_rgba8 "
    "GL_OES_draw_elements_base_vertex GL_OES_compressed_ETC1_RGB8_texture GL_OES_packed_depth_stencil "
    "GL_EXT_blend_minmax GL_EXT_texture_format_BGRA8888 GL_OES_texture_npot GL_OES_fbo_render_mipmap "
    "GL_OES_standard_derivatives GL_OES_depth_texture",
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

/* Jobs run to their end before the next starts (vc4_job_run): nothing reads
 * a block after its job, so it is freed at once. */
static void mem_free_after(struct OGLES2Context *user, struct v3d_mem *m)
{
    (void)user;
    mem_free(m);
}

/* ---- a context's pools and job ---- */

struct vc4_ctx
{
    struct vc4_pools pools;
    struct vc4_job job;
    UWORD w, h;
    int depth_bits;
    BOOL msaa;
};

/* Pools for a w x h frame in the tile size of its mode (64, 32 with MSAA). */
static BOOL pools_for(struct vc4_ctx *c, UWORD w, UWORD h)
{
    UWORD t = c->msaa ? 32 : 64;
    vc4_pools_free(&c->pools);
    return vc4_pools_init(&c->pools, (UWORD)((w + t - 1) / t), (UWORD)((h + t - 1) / t)) == 0;
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
    if (!pools_for(c, w, h) || vc4_job_init(&c->job) < 0) {
        vc4_pools_free(&c->pools);
        vc4_job_free(&c->job);
        FreeVec(c);
        return 0;
    }
    return (struct V3DContext *)c; /* the front end's handle; this backend's object */
}

static BOOL context_resize(struct V3DContext *v, UWORD w, UWORD h)
{
    struct vc4_ctx *c = (struct vc4_ctx *)v;
    UWORD t = c->msaa ? 32 : 64;
    if ((UWORD)((w + t - 1) / t) <= c->pools.tiles_x && (UWORD)((h + t - 1) / t) <= c->pools.tiles_y) {
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
    if (!c)
        return;
    vc4_job_free(&c->job);
    vc4_pools_free(&c->pools);
    FreeVec(c);
}

/* ---- the groups still to write (P5): they refuse ---- */

static const char not_yet[] = "the VC4 backend does not do this yet (P5)";

static void job_begin(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL bad) { (void)ctx; (void)tg; (void)bad; }
static BOOL job_has_draws(struct OGLES2Context *ctx) { (void)ctx; return FALSE; }
static BOOL job_keep_zs(struct OGLES2Context *ctx) { (void)ctx; return TRUE; }
static const char *job_check(struct OGLES2Context *ctx) { (void)ctx; return not_yet; }
static BOOL job_build(struct OGLES2Context *ctx, const struct OGLES2Target *tg, BOOL store_zs) { (void)ctx; (void)tg; (void)store_zs; return FALSE; }
static int job_bin(struct OGLES2Context *ctx, const struct OGLES2Target *tg) { (void)ctx; (void)tg; return OGLES2_WAIT_ERROR; }
static UBYTE job_render(struct OGLES2Context *ctx) { (void)ctx; return 0; }
static int wait_render(UBYTE frame) { (void)frame; return OGLES2_WAIT_OK; }
static void job_retire(struct OGLES2Context *ctx) { (void)ctx; }
static void job_end(struct OGLES2Context *ctx, BOOL submitted) { (void)ctx; (void)submitted; }
static void job_report_stop(struct OGLES2Context *ctx) { (void)ctx; }
static void job_free(struct OGLES2Context *ctx) { (void)ctx; }
static BOOL draw(struct OGLES2Context *ctx, const struct OGLES2DrawCall *dc) { (void)ctx; (void)dc; return FALSE; }
static void draw_free(struct OGLES2Context *ctx) { (void)ctx; }
static BOOL tex_upload(struct OGLES2Texture *t) { (void)t; return FALSE; }
static void tex_store(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w, int h) { (void)t; (void)k; (void)l; (void)src; (void)x; (void)y; (void)w; (void)h; }
static BOOL tex_load(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *dst, ULONG stride) { (void)t; (void)k; (void)l; (void)x; (void)y; (void)w; (void)h; (void)dst; (void)stride; return FALSE; }
static void tex_free(struct OGLES2Texture *t) { (void)t; }
static BOOL tex_rename(struct OGLES2Context *user, struct OGLES2Texture *t) { (void)user; (void)t; return FALSE; }
static int tex_levels(const struct OGLES2Texture *t) { (void)t; return 0; }
static BOOL tex_holds(const struct OGLES2Texture *t, int k, int l) { (void)t; (void)k; (void)l; return FALSE; }
static BOOL tex_can_generate(const struct OGLES2Texture *t, int n) { (void)t; (void)n; return FALSE; }
static int tex_generate(struct OGLES2Texture *t, int n) { (void)t; (void)n; return GL_INVALID_OPERATION; }
static int tex_from_raster(struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w, UWORD h, BOOL bgra) { (void)t; (void)src; (void)stride; (void)w; (void)h; (void)bgra; return GL_INVALID_OPERATION; }
static BOOL rb_zs_storage(GLenum format, UWORD w, UWORD h, struct v3d_mem *mem, ULONG *base, ULONG *ub, ULONG *bytes_, UBYTE *cpp) { (void)format; (void)w; (void)h; (void)mem; (void)base; (void)ub; (void)bytes_; (void)cpp; return FALSE; }
static void rb_surface(GLenum format, ULONG base, ULONG ub, ULONG bytes_, BOOL stencil_only, struct OGLES2Surface *s) { (void)format; (void)base; (void)ub; (void)bytes_; (void)stencil_only; (void)s; }
static void target_layout(struct OGLES2Target *t, UBYTE depth_format, BOOL wide) { (void)t; (void)depth_format; (void)wide; }
static BOOL target_pools(struct OGLES2Context *ctx, struct v3d_mem *alloc, struct v3d_mem *st, ULONG *tiles, ULONG name, struct OGLES2Target *t) { (void)ctx; (void)alloc; (void)st; (void)tiles; (void)name; (void)t; return FALSE; }
static BOOL tex_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l, struct OGLES2Surface *s) { (void)ctx; (void)t; (void)k; (void)l; (void)s; return FALSE; }
static BOOL tex_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l, struct OGLES2Surface *s) { (void)ctx; (void)t; (void)l; (void)s; return FALSE; }
static const struct OGLES2Image *builtin_image(void) { return 0; }
static const UBYTE *internal_program(int which, ULONG *len) { (void)which; *len = 0; return 0; }

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
    mem_free_after,
    job_begin,
    job_has_draws,
    job_keep_zs,
    job_check,
    job_build,
    job_bin,
    job_render,
    wait_render,
    job_retire,
    job_end,
    job_report_stop,
    job_free,
    draw,
    draw_free,
    tex_upload,
    tex_store,
    tex_load,
    tex_free,
    tex_rename,
    tex_levels,
    tex_holds,
    tex_can_generate,
    tex_generate,
    tex_from_raster,
    rb_zs_storage,
    rb_surface,
    target_layout,
    target_pools,
    tex_surface,
    tex_zs_surface,
    ogles2_vc4_variant_read,
    ogles2_vc4_variant_recipe,
    ogles2_vc4_variant_choose,
    builtin_image,
    internal_program,
};
