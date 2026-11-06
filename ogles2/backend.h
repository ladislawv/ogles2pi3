/*
 * The GPU backend interface (docs/PI3_PLAN.md §5.1): one ogles2.library for
 * the Raspberry Pi 4 (V3D 4.2, ogles2/v3d/) and the Raspberry Pi 3 (VC4,
 * ogles2/vc4/). The front end calls the GPU only through the table the
 * device open chooses. Built up step by step in P2, each step checked on the
 * Pi 4 against 1.3.3. Step 1: the device's identity and its capabilities,
 * which the front end used to hard-code. Step 2: GPU memory. Step 3: jobs
 * (job.c keeps the serials, the clear snapshot, the locks, the overlap and
 * the statistics; the backend builds, submits and waits). Step 4: draw
 * (draw.c keeps the entry points, their checks, the variant choice). Step 5:
 * texture storage (texture.c keeps the objects, the RAM copies, completeness
 * and the fallback texture). Step 6: framebuffer targets (fbo.c keeps the
 * objects, attachments and completeness) and memory freed after a job.
 * Step 7: the device (device.c keeps the clock and the calls) and programs
 * (v3dp.c reads the file, the backend its variants' keys and prog data).
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
#include <GLES2/gl2.h>

struct v3d_mem;
struct OGLES2Context;
struct OGLES2Target;

struct OGLES2Program;
struct OGLES2Texture;
struct OGLES2Surface;
struct SignalSemaphore;
struct V3DContext;
struct OGLES2Image;
struct ogles2_rd;

/* The internal programs (draw.c): a clear into several render targets
 * (EXT_draw_buffers), and glCopyTex*'s copy on the GPU. */
#define OGLES2_PROG_CLEAR_MRT 0
#define OGLES2_PROG_COPY_TEX  1
struct OGLES2Variant;

/* An indexed draw's indices: the application's, rebased to the copied
 * vertex range by subtracting min. */
struct OGLES2Elements
{
    const void *indices;
    GLenum type;          /* GL_UNSIGNED_BYTE, _SHORT or _INT (OES_element_index_uint) */
    GLsizei count;
    ULONG min;            /* the smallest index: subtracted from each */
};

/* One draw, its arguments checked: program p with its variants (binning,
 * vertex, fragment) and their code's addresses, the vertices
 * [first, first+count) of the arrays read through p's vertex input slots,
 * in order or (el) through indices. */
struct OGLES2DrawCall
{
    struct OGLES2Program *p;
    const struct OGLES2Variant *var[3];
    const ULONG *code;
    GLenum mode;
    GLint first;
    GLsizei count;
    const struct OGLES2Elements *el;
};

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
    UBYTE max_vertex_attribs;    /* GL_MAX_VERTEX_ATTRIBS (at most OGLES2_MAX_VERTEX_ATTRIBS) */
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
    UBYTE variant_section;       /* its programs' VARIANT section type (FORMAT.md): 6, 7 VC4 */
    const char *program_ext;     /* their file names' extension: ".v3dp", ".vc4p" */
    const char *(*renderer)(void); /* GL_RENDERER, once the device is open */
    /* The device: opened at the first context (FALSE: no GPU); open and
     * working; reset after a job it did not finish (the GPU lock held);
     * bytes of GPU memory in use; RAM the GPU reaches; the GPU submission
     * lock (shared with other GPU clients); a context's pools and control
     * lists for a w x h default framebuffer (the V3D one's type for now:
     * ctx->v3d), resized, freed. */
    BOOL (*open)(void);
    BOOL (*ready)(void);
    void (*reset)(const char *why);
    ULONG (*bytes)(void);
    BOOL (*gpu_reachable)(ULONG addr, ULONG size);
    struct SignalSemaphore *(*gpu_lock)(void);
    struct V3DContext *(*context_new)(UWORD w, UWORD h, int depth_bits, BOOL msaa);
    BOOL (*context_resize)(struct V3DContext *v, UWORD w, UWORD h);
    void (*context_free)(struct V3DContext *v);
    /* GPU memory, zeroed, in a block the GPU reaches; 0 or -1 (out of
     * memory). Freeing a block a job may still read waits for the job
     * (ogles2_share_defer_free, the frame's own list); this frees now. */
    int (*mem_alloc)(struct v3d_mem *m, ULONG size);
    void (*mem_free)(struct v3d_mem *m);
    void (*mem_free_after)(struct OGLES2Context *user, struct v3d_mem *m); /* after user's job renders */
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
    /* Draw (FALSE: out of memory; the job is dropped at its flush); the
     * context's draw state freed (aglDestroyContext, after the last job). */
    BOOL (*draw)(struct OGLES2Context *ctx, const struct OGLES2DrawCall *dc);
    void (*draw_free)(struct OGLES2Context *ctx);
    /* Texture storage: the GPU copy made (again) from the RAM copies; a box
     * of face k, level l stored from a RAM copy, or loaded into one (FALSE:
     * no copy); the copy freed; given to the job `user` has open (FALSE: not
     * done, the job is rendered instead); its levels (0: none); whether it
     * takes level l of face k in place; glGenerateMipmap on the GPU (can,
     * do: 0 or an error, -3 busy); level 0 filled from a raster image
     * (glCopyTex*: 0, an error, -3 busy). */
    BOOL (*tex_upload)(struct OGLES2Texture *t);
    void (*tex_store)(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w, int h);
    BOOL (*tex_load)(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *dst, ULONG stride);
    void (*tex_free)(struct OGLES2Texture *t);
    BOOL (*tex_rename)(struct OGLES2Context *user, struct OGLES2Texture *t);
    int (*tex_levels)(const struct OGLES2Texture *t);
    BOOL (*tex_holds)(const struct OGLES2Texture *t, int k, int l);
    BOOL (*tex_can_generate)(const struct OGLES2Texture *t, int n);
    int (*tex_generate)(struct OGLES2Texture *t, int n);
    int (*tex_from_raster)(struct OGLES2Context *ctx, struct OGLES2Texture *t, ULONG src, ULONG stride, UWORD w,
                           UWORD h, BOOL bgra);
    /* Framebuffer targets: a depth or stencil renderbuffer's storage (mem,
     * and its image's base, padded height, bytes and bytes a pixel) and that
     * image as a surface; a target's tile size, bpp and depth type (wide: a
     * half-float target drawn); its tile pools when it has more tiles than
     * the context's (kept in the framebuffer's *alloc, *state, *tiles; t's
     * ta_addr 0 when the context's do); a texture level as a colour or a
     * depth (and stencil) surface, its GPU copy made first. */
    BOOL (*rb_zs_storage)(GLenum format, UWORD w, UWORD h, struct v3d_mem *mem, ULONG *base, ULONG *ub,
                          ULONG *bytes, UBYTE *cpp);
    void (*rb_surface)(GLenum format, ULONG base, ULONG ub, ULONG bytes, BOOL stencil_only, struct OGLES2Surface *s);
    /* Depth and stencil in one image (VC4): the stencil values from one such
     * image to another of the same size; 0: they have images of their own. */
    void (*rb_stencil_copy)(ULONG from, ULONG to, ULONG bytes);
    void (*target_layout)(struct OGLES2Target *t, UBYTE depth_format, BOOL wide);
    BOOL (*target_pools)(struct OGLES2Context *ctx, struct v3d_mem *alloc, struct v3d_mem *state, ULONG *tiles,
                         ULONG name, struct OGLES2Target *t);
    BOOL (*tex_surface)(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l, struct OGLES2Surface *s);
    BOOL (*tex_zs_surface)(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l, struct OGLES2Surface *s);
    /* Programs: a variant section's key and prog data, after its stage (0,
     * or why the file is refused); its recipe's types judged, uses_ubo and
     * no_textures set, *clip_z for OGLES2_U_CLIP_Z_COEFF (0, or why, *bad
     * the value); the variant a draw uses (stage, render targets, primitive
     * mode); the built-in image (clears drawn as rectangles); an internal
     * program's file (OGLES2_PROG_*; 0: none). */
    const char *(*variant_read)(struct ogles2_rd *s, struct OGLES2Variant *v);
    const char *(*variant_recipe)(struct OGLES2Variant *v, BOOL *clip_z, ULONG *bad);
    const struct OGLES2Variant *(*variant_choose)(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs, GLenum mode);
    const struct OGLES2Image *(*builtin_image)(void);
    const UBYTE *(*internal_program)(int which, ULONG *len);
    /* What such a program's image needs once loaded (VC4: its sources, for the
     * variants of states it does not hold); 0: nothing. */
    void (*internal_loaded)(int which, struct OGLES2Image *im);
};

/* The generic vertex attributes GL offers on this GPU: indices from it on
 * are GL_INVALID_VALUE (the context's arrays are OGLES2_MAX_VERTEX_ATTRIBS
 * long, the most any backend has: the internal programs use the last two). */
#define OGLES2_GL_VERTEX_ATTRIBS ((GLuint)ogles2_backend->caps->max_vertex_attribs)

#define ogles2_mem_alloc(m, size) (ogles2_backend->mem_alloc((m), (size)))
#define ogles2_mem_free(m) (ogles2_backend->mem_free(m))

/* The control-list generation (see the top). */
extern ULONG ogles2_job_gen;

/* The backend in use: the V3D 4.2 one until a device open chooses (a
 * context without a GPU, the ABI tests' state-only ones, reports its limits). */
extern const struct OGLES2Backend *ogles2_backend;

extern const struct OGLES2Backend ogles2_backend_v3d; /* ogles2/v3d/backend_v3d.c */
extern const struct OGLES2Backend ogles2_backend_vc4; /* ogles2/vc4/backend_vc4.c */
/* The machine's GPU is a VideoCore IV (the device tree says so): device.c
 * chooses the VC4 backend at the first device open. */
BOOL ogles2_vc4_present(void);

#endif
