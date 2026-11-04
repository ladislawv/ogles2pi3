/*
 * The GPU backend interface (docs/PI3_PLAN.md §5.1): one ogles2.library for
 * the Raspberry Pi 4 (V3D 4.2, ogles2/v3d/) and the Raspberry Pi 3 (VC4,
 * ogles2/vc4/). The front end calls the GPU only through the table the
 * device open chooses. Built up step by step in P2, each step checked on the
 * Pi 4 against 1.3.3; this is step 1: the device's identity and its
 * capabilities, which the front end used to hard-code.
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_BACKEND_H
#define OGLES2_BACKEND_H

#include <exec/types.h>

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
};

/* The backend in use: the V3D 4.2 one until a device open chooses (a
 * context without a GPU, the ABI tests' state-only ones, reports its limits). */
extern const struct OGLES2Backend *ogles2_backend;

extern const struct OGLES2Backend ogles2_backend_v3d; /* ogles2/v3d/backend_v3d.c */

#endif
