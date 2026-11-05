/*
 * vc4core: the Raspberry Pi 3's VideoCore IV 3D core (V3D 2.1) without a
 * driver (docs/PI3_PLAN.md §3.1). What the VC4 backend of ogles2.library
 * (ogles2/vc4/, P5) and the Linux tests (tests/pi3/, P3) call: the device,
 * GPU memory, control lists, jobs (binning list, render list per tile),
 * submission and waits. The OS below it is vc4_os.h's.
 *
 * GPU memory may be device memory (Linux maps the firmware's memory so):
 * write it with naturally aligned stores of 1, 2 or 4 bytes only, never
 * memcpy; words for the GPU through vc4_put32 and friends (little-endian on
 * every host, the 68k included).
 *
 * MIT licence (LICENSE).
 */
#ifndef VC4CORE_H
#define VC4CORE_H

#include <stdint.h>
#include "vc4_pack.h"

#if defined(__m68k__) || defined(__mc68000__) || defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define VC4_BIG_ENDIAN 1
#else
#define VC4_BIG_ENDIAN 0
#endif

static inline uint32_t vc4_le32(uint32_t v)
{
#if VC4_BIG_ENDIAN
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
#else
    return v;
#endif
}

/* Stores into GPU memory: p naturally aligned. */
static inline void vc4_put32(uint8_t *p, uint32_t v)
{
    *(volatile uint32_t *)p = vc4_le32(v);
}

static inline void vc4_put16(uint8_t *p, uint32_t v)
{
#if VC4_BIG_ENDIAN
    *(volatile uint16_t *)p = (uint16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
#else
    *(volatile uint16_t *)p = (uint16_t)v;
#endif
}

static inline void vc4_putf(uint8_t *p, float f)
{
    vc4_put32(p, vc4_fbits(f));
}

static inline uint32_t vc4_get32(const uint8_t *p)
{
    return vc4_le32(*(const volatile uint32_t *)p);
}

/* ---- the device (device.c) ---- */

/* Takes the 3D core over: powered off until its registers stop answering,
 * powered on once (its reset state: PI3_PLAN §2, "Power"), its identity
 * checked. 0, or < 0 with the reason logged. */
int vc4_open(void);
void vc4_close(void);
uint32_t vc4_read(uint32_t reg);
void vc4_write(uint32_t reg, uint32_t v);
/* After a job that did not finish: both control threads stopped and the
 * core taken over again (its state is unknown). */
int vc4_reset(const char *why);

/* ---- memory (mem.c) ---- */

/* A block of GPU memory, 4 KB aligned: the CPU's view at cpu, the GPU's bus
 * address at bus (the Pi 3: 0xC0000000 + physical). */
struct vc4_mem
{
    uint8_t *cpu;
    uint32_t bus;
    uint32_t size;
    uint32_t handle; /* the OS layer's */
};

int vc4_mem_alloc(struct vc4_mem *m, uint32_t size);
void vc4_mem_free(struct vc4_mem *m);
/* Fills with a 32-bit pattern (len a multiple of 4). */
void vc4_mem_fill(struct vc4_mem *m, uint32_t off, uint32_t len, uint32_t pattern);
/* Copies host memory in (len a multiple of 4, offsets 4-aligned). */
void vc4_mem_write(struct vc4_mem *m, uint32_t off, const void *src, uint32_t len);

/* ---- control lists (cl.c) ---- */

#define VC4_CL_BLOCKS 16

/* A list in blocks of GPU memory: a block that fills up ends with a BRANCH
 * to the next (the binning and the render lists), and the list runs from
 * the first block's start to `next` in the last. */
struct vc4_cl
{
    struct vc4_mem block[VC4_CL_BLOCKS];
    int nblocks, cur;
    uint8_t *next, *end; /* in block[cur]; end leaves room for the BRANCH */
    uint32_t block_size;
    int failed;          /* out of memory: the job is dropped */
};

int vc4_cl_init(struct vc4_cl *cl, uint32_t block_size);
void vc4_cl_free(struct vc4_cl *cl);
void vc4_cl_reset(struct vc4_cl *cl);
/* Room for n bytes (a packet); NULL when out of memory (cl->failed set). */
uint8_t *vc4_cl_room(struct vc4_cl *cl, uint32_t n);
uint32_t vc4_cl_start(const struct vc4_cl *cl); /* bus address of the first byte */
uint32_t vc4_cl_end(const struct vc4_cl *cl);   /* and after the last */
/* What the CPU wrote made visible to the GPU (vc4_os_flush: under Emu68 GPU
 * memory is cached fast RAM); vc4_job_run does it for its lists and state. */
void vc4_cl_flush(const struct vc4_cl *cl);

/* VC4_EMIT(cl, CLEAR_COLORS, .clear_color = c, .clear_zs = z): one packet. */
#define VC4_EMIT(cl, NAME, ...)                                      \
    do {                                                             \
        uint8_t *vc4_p_ = vc4_cl_room((cl), VC4_##NAME##_length);    \
        if (vc4_p_) {                                                \
            struct VC4_##NAME vc4_v_ = {__VA_ARGS__};                \
            VC4_##NAME##_pack(vc4_p_, &vc4_v_);                      \
        }                                                            \
    } while (0)
/* A packet without fields (FLUSH, START_TILE_BINNING, ...). */
#define VC4_EMIT0(cl, NAME)                                          \
    do {                                                             \
        uint8_t *vc4_p_ = vc4_cl_room((cl), VC4_##NAME##_length);    \
        if (vc4_p_)                                                  \
            VC4_##NAME##_pack(vc4_p_);                               \
    } while (0)

/* The state a job's draws read (shader records, uniforms, vertices): blocks
 * of GPU memory, filled in turn. */
struct vc4_buf
{
    struct vc4_mem block[VC4_CL_BLOCKS];
    int nblocks, cur;
    uint32_t used, block_size;
    int failed;
};

int vc4_buf_init(struct vc4_buf *b, uint32_t block_size);
void vc4_buf_free(struct vc4_buf *b);
void vc4_buf_reset(struct vc4_buf *b);
/* size bytes at a multiple of align (a power of two up to 4096); NULL when
 * out of memory; *bus their bus address. */
uint8_t *vc4_buf_alloc(struct vc4_buf *b, uint32_t size, uint32_t align, uint32_t *bus);
void vc4_buf_flush(const struct vc4_buf *b);

/* ---- jobs (job.c) ---- */

#define VC4_TILING_RASTER 0
#define VC4_TILING_T 1
#define VC4_TILING_LT 2

/* The tile buffer's colour format in memory (Tile Rendering Mode's
 * "Non-HDR Frame Buffer Color Format", the stores' "Pixel Color Format"). */
#define VC4_COLOR_RGBA8888 1
#define VC4_COLOR_BGR565_DITHERED 0
#define VC4_COLOR_BGR565 2

/* An image a job loads or stores. */
struct vc4_surface
{
    uint32_t bus;   /* 0: none */
    uint8_t tiling; /* VC4_TILING_* */
    uint8_t format; /* VC4_COLOR_* (colour) */
};

#define VC4_COLOR 1
#define VC4_ZS 2

/* What a job renders into. A raster colour image is width pixels a row,
 * rounded up to 4 (PI3_PLAN §2, "Raster stores"). */
struct vc4_target
{
    uint16_t width, height;
    uint8_t msaa;          /* 4x: 32x32 tiles, the colour stored resolved */
    struct vc4_surface color, zs;
    uint8_t load;          /* VC4_COLOR | VC4_ZS: loaded at each tile's start */
    uint8_t store;         /* and stored at its end */
    uint8_t clear;         /* cleared (the clear values below), instead of loaded */
    uint32_t clear_color;  /* RGBA8888 as the tile buffer holds it: A in the top byte */
    uint32_t clear_z;      /* 24 bits */
    uint8_t clear_s;
};

/* The binner's memory for frames of up to tiles_x x tiles_y tiles: tile
 * state, tile allocation and overflow, in one 256 MB segment. */
struct vc4_pools
{
    struct vc4_mem mem;
    uint16_t tiles_x, tiles_y;
    uint32_t tsda, talloc, talloc_size, overflow, overflow_size; /* bus addresses, sizes */
    uint32_t overflow_chunks; /* overflow_size each, one after the other */
};

int vc4_pools_init(struct vc4_pools *p, uint16_t tiles_x, uint16_t tiles_y);
/* The pools' layout for `tiles` tiles, for a caller with its own block (a
 * framebuffer object's): the tile state's bytes (first), the tile
 * allocation's (next), all of them with the overflow memory (after). */
void vc4_pools_layout(uint32_t tiles, uint32_t *tsda_size, uint32_t *talloc_size, uint32_t *total);
void vc4_pools_free(struct vc4_pools *p);
/* Overflow chunk n of a job (0 first): its bus address and size; 0 when the
 * pools have no more. All of them lie in the pools' one block: overflow
 * memory in blocks of their own (in the same 256 MB segment) gave the
 * binner VCM errors (ERRSTAT bit 14) from the second such block on, where
 * the same requests served from one block were clean (2026-11-04); Linux
 * too gives out slots of one bin buffer. */
uint32_t vc4_pools_overflow(struct vc4_pools *p, uint32_t n, uint32_t *size);

struct vc4_job
{
    struct vc4_cl bcl, rcl;
    struct vc4_buf state;
    struct vc4_target target;
    struct vc4_pools *pools;
    uint16_t tiles_x, tiles_y;
    uint32_t errstat;      /* after the last run */
    uint32_t errstat_bin;  /* after its binning */
    uint32_t bin_us, render_us; /* how long the last run's binning and rendering took */
    uint32_t overflows;    /* the binner asked for more memory this many times (each got a chunk) */
    int out_of_overflow;   /* the last binning stopped: the pools had no more chunks */
    int caller_flushes;    /* the caller makes the CPU's writes visible itself, after
                            * vc4_job_end_lists (ogles2.library: one clean of the whole cache) */
    uint64_t t_render;     /* when the render was started */
};

int vc4_job_init(struct vc4_job *job);
void vc4_job_free(struct vc4_job *job);
/* Starts the job's binning list for target t (the context's pools, sized
 * for at least its tiles): the binning mode, the start, the primitive list
 * format. The caller then appends its state and draws to job->bcl, their
 * records to job->state. */
int vc4_job_begin(struct vc4_job *job, const struct vc4_target *t, struct vc4_pools *pools);
/* Ends the binning list, builds the render list, runs both and waits for
 * them (timeout_us each). 0; -1 out of memory; -2 the binning, -3 the
 * rendering did not finish (the core was reset). */
int vc4_job_run(struct vc4_job *job, uint32_t timeout_us);
/* vc4_job_run in its steps, for a caller that lets the render run on while
 * the CPU builds the next job (ogles2.library), and that logs and resets
 * itself (these log nothing: a bitmap may be locked): the lists ended and
 * made visible to the GPU (0, -1 out of memory); the binning run and waited
 * for (0, -2: did not finish, out_of_overflow says if for want of memory);
 * the render started; waited for (0, -3: did not finish; errstat set and
 * its error bits cleared). Only one job on the core at a time: a binning
 * starts after the last render finished. */
int vc4_job_end_lists(struct vc4_job *job);
int vc4_job_bin(struct vc4_job *job, uint32_t timeout_us);
void vc4_job_render_start(struct vc4_job *job);
int vc4_job_render_wait(struct vc4_job *job, uint32_t timeout_us);

/* ---- textures (tiling.c) ---- */

#define VC4_MAX_LEVELS 12 /* 2048 */

struct vc4_level
{
    uint32_t offset;        /* from the miptree's start (level 0: page aligned) */
    uint32_t stride;        /* bytes a row of pixels (padded) */
    uint32_t width, height; /* the level's own */
    uint32_t padded_height;
    uint8_t tiling;         /* VC4_TILING_LT or _T, as the TMU takes the level */
};

struct vc4_texture_layout
{
    uint32_t width, height, cpp, levels;
    struct vc4_level level[VC4_MAX_LEVELS];
    uint32_t face_stride;   /* a cube map's faces, each a whole miptree */
    uint32_t size;          /* bytes, every face */
};

uint32_t vc4_utile_width(uint32_t cpp);
uint32_t vc4_utile_height(uint32_t cpp);
int vc4_size_is_lt(uint32_t w, uint32_t h, uint32_t cpp);
/* The layout of a w x h texture of cpp bytes a texel, levels levels (the
 * smaller ones from the next power of two down), faces 1 or 6. */
void vc4_texture_layout(struct vc4_texture_layout *t, uint32_t w, uint32_t h, uint32_t cpp, uint32_t levels,
                        int faces);
/* Where texel (x, y) of a level lies, from the level's start. */
uint32_t vc4_texel_offset(const struct vc4_level *l, uint32_t cpp, uint32_t x, uint32_t y);
/* A whole level from src (cpp-byte texels as the GPU holds them, rows
 * src_stride bytes apart) into the miptree at base. */
void vc4_texture_store(uint8_t *base, const struct vc4_texture_layout *t, uint32_t level, uint32_t face,
                       const uint8_t *src, uint32_t src_stride);

/* The TMU's texture configuration words, P0 and P1 (Linux's vc4_packet.h:
 * VC4_TEX_P0_*, VC4_TEX_P1_*), as a fragment shader's uniform stream gives
 * them. */
#define VC4_TEX_RGBA8888 0
#define VC4_TEX_RGBX8888 1
#define VC4_TEX_RGBA4444 2
#define VC4_TEX_RGBA5551 3
#define VC4_TEX_RGB565 4
#define VC4_TEX_LUMINANCE 5
#define VC4_TEX_ALPHA 6
#define VC4_TEX_LUMALPHA 7
#define VC4_TEX_ETC1 8
#define VC4_TEX_S16F 9
#define VC4_TEX_S8 10
#define VC4_TEX_S16 11
#define VC4_TEX_BW1 12
#define VC4_TEX_A4 13
#define VC4_TEX_A1 14
#define VC4_TEX_RGBA64 15
#define VC4_TEX_RGBA32R 16
#define VC4_TEX_YUV422R 17

#define VC4_MAG_LINEAR 0
#define VC4_MAG_NEAREST 1
#define VC4_MIN_LINEAR 0
#define VC4_MIN_NEAREST 1
#define VC4_MIN_NEAR_MIP_NEAR 2
#define VC4_MIN_NEAR_MIP_LIN 3
#define VC4_MIN_LIN_MIP_NEAR 4
#define VC4_MIN_LIN_MIP_LIN 5
#define VC4_WRAP_REPEAT 0
#define VC4_WRAP_CLAMP 1
#define VC4_WRAP_MIRROR 2
#define VC4_WRAP_BORDER 3

/* P0: level 0's address (page aligned), a cube map, the type's low 4 bits,
 * the last level (Mesa: last - first). */
static inline uint32_t vc4_tex_p0(uint32_t level0_bus, uint32_t type, uint32_t last_level, int cube)
{
    return (level0_bus & 0xfffff000u) | (cube ? 1u << 9 : 0) | (type & 15) << 4 | (last_level & 15);
}

/* P1: the type's bit 4, level 0's size (2048 as 0), the filters, the wraps. */
static inline uint32_t vc4_tex_p1(uint32_t type, uint32_t w, uint32_t h, uint32_t mag, uint32_t min,
                                  uint32_t wrap_s, uint32_t wrap_t)
{
    return (type >> 4) << 31 | (h & 2047) << 20 | (type == VC4_TEX_ETC1 ? 1u << 19 : 0) /* ETCFLIP, as Mesa */ |
           (w & 2047) << 8 | (mag & 1) << 7 | (min & 7) << 4 | (wrap_t & 3) << 2 | (wrap_s & 3);
}

/* GL_SHADER_STATE: the record at rec_bus (16-byte aligned) with nattr
 * attribute records (1 to 8). The XML gives its address as a 28-bit number
 * at bit 0, which would drop a bus address's top bits: the word is the
 * address with the count (8 as 0) in its low three bits, as Mesa writes it. */
static inline void vc4_gl_shader_state(struct vc4_cl *cl, uint32_t rec_bus, int nattr)
{
    uint8_t *p = vc4_cl_room(cl, VC4_GL_SHADER_STATE_length);
    uint32_t w = rec_bus | ((uint32_t)nattr & 7);
    if (!p)
        return;
    p[0] = VC4_GL_SHADER_STATE_opcode;
    p[1] = (uint8_t)w;
    p[2] = (uint8_t)(w >> 8);
    p[3] = (uint8_t)(w >> 16);
    p[4] = (uint8_t)(w >> 24);
}

#endif
