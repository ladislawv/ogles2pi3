/*
 * The bare-GPU probe for the Raspberry Pi 3's VideoCore IV 3D core (V3D 2.1):
 * PI3_PLAN P1 (§6.2, §8). No driver: the 3D core's registers, power and
 * memory from the firmware's mailbox (plat.h), command lists built here.
 *   pi3_probe [OPTIONS] [--ref=FILE.ppm] [--texref=FILE.ppm] [--out=DIR] [TEST...]
 *   sudo ./pi3_probe --ref=mesa_triangle.ppm --texref=mesa_texture.ppm
 * Tests (all by default, in this order):
 *   ident     IDENT0..2 decoded; IDENT0 must be 0x02443356.
 *   clear     a render list alone: a 128x128 RGBA8888 raster frame cleared
 *             to one colour (4 tiles of 64x64), every pixel checked.
 *   triangle  binner and renderer: one triangle of constant colour over the
 *             same frame, with the shaders Mesa's vc4 compiles for
 *             tests/pi3/ref/mesa_triangle.c (VC4_DEBUG=qpu) and our own
 *             lists; written as DIR/triangle.ppm and compared with --ref
 *             (that program's picture), pixel for pixel.
 *   stride    raster stores into frames 128, 130 and 129 pixels wide: how
 *             the rows are laid out (§2, "Raster stores": no stride field).
 *   texture   a quad sampling a 16x16 texture with Mesa's shaders for
 *             tests/pi3/ref/mesa_texture.c, the texture stored LT (as Mesa
 *             stores it) and raster (type RGBA32R); each compared with
 *             --texref (that program's picture). Then raster textures of
 *             other sizes, checked by the CPU: power-of-two widths must be
 *             right, the others are reported (their rows are misread).
 * Only when named:
 *   clearseq  five render-only clears with changing colours: which colour
 *             each job stores (with --variant=mesa: the previous one's).
 *   texrepeat the textured quad and the triangle 40 times each (the lost
 *             tile of a core not reset, see plat_linux.c's reset_core).
 *   rastermap where the TMU reads raster textures of various sizes.
 *   power     whether the firmware counts SET_ENABLE_QPU (it does).
 * Options: --power=qpu (the default: the mailbox's SET_ENABLE_QPU, the core
 * power-cycled at open into its reset state) or --power=domain
 * (SET_DOMAIN_STATE: the registers then read 0xdeadbeef); --no-reset (no
 * power cycle at open); --keep-power (leave the core on at exit);
 * --errstat (ERRSTAT around each draw); --concurrent (binner and renderer
 * started together) or --bin-delay=US; --no-render-flush (no texture-cache
 * flush before the render job); --rotate (each render list at another
 * address).
 * Lists, shaders and buffers follow Mesa's vc4 (MIT): the binning list as
 * vc4_job.c and the kernel's validation leave it (kernel/vc4_validate.c),
 * packets from src/broadcom/cle/vc4_packet.xml; the render list as Mesa's
 * kernel/vc4_render_cl.c builds it, but with the clear first, as Linux has
 * it now (see mesa_order). Little-endian hosts only for now (the m68k build
 * adds the byte swaps, P5).
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Registers (byte offsets; Broadcom's guide, VideoCoreIV-AG100-R, §10). */
#define V3D_IDENT0 0x000
#define V3D_IDENT1 0x004
#define V3D_IDENT2 0x008
#define V3D_L2CACTL 0x020
#define V3D_SLCACTL 0x024
#define V3D_INTCTL 0x030
#define V3D_INTENA 0x034
#define V3D_INTDIS 0x038
#define V3D_CT0CS 0x100
#define V3D_CT1CS 0x104
#define V3D_CT0EA 0x108
#define V3D_CT1EA 0x10c
#define V3D_CT0CA 0x110
#define V3D_CT1CA 0x114
#define V3D_PCS 0x130
#define V3D_BFC 0x134
#define V3D_RFC 0x138
#define V3D_BPCA 0x300
#define V3D_BPCS 0x304
#define V3D_BPOA 0x308
#define V3D_BPOS 0x30c
#define V3D_VPMBASE 0x504
#define V3D_DBGE 0xf00
#define V3D_FDBGO 0xf04
#define V3D_FDBGB 0xf08
#define V3D_FDBGR 0xf0c
#define V3D_FDBGS 0xf10
#define V3D_ERRSTAT 0xf20

/* Packets (kernel/vc4_packet.h). */
#define PKT_HALT 0
#define PKT_FLUSH 4
#define PKT_START_TILE_BINNING 6
#define PKT_INCREMENT_SEMAPHORE 7
#define PKT_WAIT_ON_SEMAPHORE 8
#define PKT_BRANCH_TO_SUB_LIST 17
#define PKT_STORE_MS_TILE_BUFFER 24
#define PKT_STORE_MS_TILE_BUFFER_AND_EOF 25
#define PKT_STORE_TILE_BUFFER_GENERAL 28
#define PKT_GL_ARRAY_PRIMITIVE 33
#define PKT_PRIMITIVE_LIST_FORMAT 56
#define PKT_GL_SHADER_STATE 64
#define PKT_CONFIGURATION_BITS 96
#define PKT_FLAT_SHADE_FLAGS 97
#define PKT_POINT_SIZE 98
#define PKT_LINE_WIDTH 99
#define PKT_DEPTH_OFFSET 101
#define PKT_CLIP_WINDOW 102
#define PKT_VIEWPORT_OFFSET 103
#define PKT_CLIPPER_XY_SCALING 105
#define PKT_CLIPPER_Z_SCALING 106
#define PKT_TILE_BINNING_MODE_CONFIG 112
#define PKT_TILE_RENDERING_MODE_CONFIG 113
#define PKT_CLEAR_COLORS 114
#define PKT_TILE_COORDINATES 115

#define RENDER_FORMAT_RGBA8888 (1 << 2)
#define BIN_AUTO_INIT_TSDA (1 << 2)
#define BIN_BLOCK_SIZE_128 (2 << 5)
#define SHADER_FS_SINGLE_THREAD (1 << 0)
#define SHADER_ENABLE_CLIPPING (1 << 2)

/* Layout of the one GPU allocation (byte offsets). The tile state, tile
 * allocation and overflow lie in one 256 MB-aligned segment (checked). */
#define MEM_SIZE (4u << 20)
#define OFF_FB 0x000000      /* frames, up to 1 MB */
#define OFF_TSDA 0x100000    /* tile state: 48 bytes per tile */
#define OFF_TALLOC 0x101000  /* tile allocation: 32 per tile, to 256, + 1 MB */
#define TALLOC_SIZE (256u + (1u << 20))
#define OFF_OVERFLOW 0x280000 /* binner overflow (BPOA/BPOS) */
#define OVERFLOW_SIZE 0x40000
#define OFF_BCL 0x300000
#define OFF_RCL 0x301000
#define OFF_SHADER_REC 0x302000
#define OFF_VERTICES 0x303000
#define OFF_CODE 0x304000
#define OFF_UNIFORMS 0x305000

static struct plat_mem mem;
static int failures;
/* Where the next render list goes: OFF_RCL, or (--rotate) one of four slots in
 * turn (a test of stale list data). */
static int rotate, rcl_slot;
static uint32_t rcl_off = OFF_RCL;

static uint32_t rd(uint32_t off)
{
    return plat_regs()[off / 4];
}

static void wr(uint32_t off, uint32_t v)
{
    plat_regs()[off / 4] = v;
}

/* The CPU's view of the allocation ends at mem.size: a write past it lands in
 * whatever the next pages map (one such bug rebooted the Pi). */
static void in_bounds(uint32_t off, uint32_t bytes)
{
    if (off > mem.size || bytes > mem.size - off) {
        fprintf(stderr, "probe bug: GPU memory access %#x+%#x beyond %#x\n", off, bytes, mem.size);
        abort();
    }
}

static void gpu_write(uint32_t off, const void *src, uint32_t bytes)
{
    const uint8_t *s = src;
    uint32_t k;
    in_bounds(off, bytes);
    for (k = 0; k < bytes; k += 4) {
        uint32_t w = 0;
        memcpy(&w, s + k, bytes - k < 4 ? bytes - k : 4);
        mem.cpu[(off + k) / 4] = w;
    }
}

static void gpu_fill(uint32_t off, uint32_t value, uint32_t words)
{
    uint32_t k;
    in_bounds(off, words * 4);
    for (k = 0; k < words; k++)
        mem.cpu[off / 4 + k] = value;
}

static uint32_t gpu_word(uint32_t off)
{
    in_bounds(off, 4);
    return mem.cpu[off / 4];
}

/* A control list being built in CPU memory, little-endian. */
struct cl {
    uint8_t b[4096];
    uint32_t n;
};

static void u8_(struct cl *c, uint32_t v)
{
    c->b[c->n++] = (uint8_t)v;
}
static void u16_(struct cl *c, uint32_t v)
{
    u8_(c, v);
    u8_(c, v >> 8);
}
static void u32_(struct cl *c, uint32_t v)
{
    u16_(c, v);
    u16_(c, v >> 16);
}
static void f32_(struct cl *c, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    u32_(c, v);
}

static void dump_state(const char *why)
{
    printf("  %s: CT0CS %08x CT1CS %08x CT0CA %08x CT0EA %08x CT1CA %08x CT1EA %08x\n", why, rd(V3D_CT0CS),
           rd(V3D_CT1CS), rd(V3D_CT0CA), rd(V3D_CT0EA), rd(V3D_CT1CA), rd(V3D_CT1EA));
    printf("  PCS %08x BFC %08x RFC %08x INTCTL %08x BPCA %08x BPCS %08x BPOA %08x BPOS %08x\n", rd(V3D_PCS),
           rd(V3D_BFC), rd(V3D_RFC), rd(V3D_INTCTL), rd(V3D_BPCA), rd(V3D_BPCS), rd(V3D_BPOA), rd(V3D_BPOS));
    printf("  ERRSTAT %08x DBGE %08x FDBGO %08x FDBGB %08x FDBGR %08x FDBGS %08x\n", rd(V3D_ERRSTAT), rd(V3D_DBGE),
           rd(V3D_FDBGO), rd(V3D_FDBGB), rd(V3D_FDBGR), rd(V3D_FDBGS));
}

/* Waits until the count register (BFC or RFC, 8 bits) reaches want. */
static int wait_count(uint32_t reg, uint32_t want, const char *what)
{
    uint64_t t0 = plat_time_us(), t;
    for (;;) {
        t = plat_time_us();
        if ((rd(reg) & 0xff) >= want) {
            printf("  %s done in %llu us\n", what, (unsigned long long)(t - t0));
            return 0;
        }
        if (t - t0 > 1000000) {
            printf("  %s: TIMEOUT after 1 s\n", what);
            dump_state("state");
            return -1;
        }
    }
}

/* Stops both control threads (after a timeout). */
static void reset_threads(void)
{
    wr(V3D_CT0CS, 1u << 15);
    wr(V3D_CT1CS, 1u << 15);
}

static void flush_caches(void)
{
    wr(V3D_L2CACTL, 1u << 2);    /* L2CCLR */
    wr(V3D_SLCACTL, 0x0f0f0f0f); /* TMU1, TMU0, uniform and instruction caches of every slice */
}

/* Before a render job, as Linux's vc4_submit_next_render_job: the L2 and the
 * TMU caches only (--no-render-flush leaves it out). */
static int no_render_flush;
/* --concurrent: start the binner and the renderer together (bin_mode 1);
 * --bin-delay=US: wait that long between the binner's end and the
 * renderer's start. By default the renderer starts when BFC counts the
 * binning done. */
static int bin_mode, bin_delay_us;

static void flush_texture_caches(void)
{
    if (no_render_flush)
        return;
    wr(V3D_L2CACTL, 1u << 2);
    wr(V3D_SLCACTL, 0x0f0f0000);
}

static void run_list(int thread, uint32_t off, uint32_t len)
{
    uint32_t ca = thread ? V3D_CT1CA : V3D_CT0CA, ea = thread ? V3D_CT1EA : V3D_CT0EA;
    wr(ca, mem.bus + off);
    wr(ea, mem.bus + off + len); /* writing the end address starts the thread */
}

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

/* QPU scheduler, VPM and cache registers (guide, section 10). */
static void show_sched(const char *when)
{
    printf("%s: SQRSV0 %08x SQRSV1 %08x SQCNTL %08x VPACNTL %08x VPMBASE %08x L2CACTL %08x BPOA %08x BPOS %08x "
           "BPCA %08x BPCS %08x ERRSTAT %08x DBGE %08x\n",
           when, rd(0x410), rd(0x414), rd(0x418), rd(0x500), rd(V3D_VPMBASE), rd(V3D_L2CACTL), rd(V3D_BPOA),
           rd(V3D_BPOS), rd(V3D_BPCA), rd(V3D_BPCS), rd(V3D_ERRSTAT), rd(V3D_DBGE));
}

/* --- power --------------------------------------------------------------- */

/* Is SET_ENABLE_QPU counted? Marks VPMBASE (reset value 0x10), enables a
 * second time, disables once and looks; disables again, looks; enables, and
 * the mark is gone if the core was reset. (Only when named.) */
static void test_power(void)
{
    uint32_t v[1];
    printf("power: SET_ENABLE_QPU counting\n");
    wr(V3D_VPMBASE, 5);
    printf("  marked: VPMBASE %08x\n", rd(V3D_VPMBASE));
    v[0] = 1;
    plat_mbox(0x00030012, v, 1);
    v[0] = 0;
    plat_mbox(0x00030012, v, 1);
    printf("  enable, disable: IDENT0 %08x VPMBASE %08x\n", rd(V3D_IDENT0), rd(V3D_VPMBASE));
    v[0] = 0;
    plat_mbox(0x00030012, v, 1);
    printf("  disable again: IDENT0 %08x VPMBASE %08x\n", rd(V3D_IDENT0), rd(V3D_VPMBASE));
    v[0] = 1;
    plat_mbox(0x00030012, v, 1);
    printf("  enable: IDENT0 %08x VPMBASE %08x (0x10: reset)\n", rd(V3D_IDENT0), rd(V3D_VPMBASE));
    check(rd(V3D_IDENT0) == 0x02443356, "powered again");
}

/* --- ident --------------------------------------------------------------- */

static void test_ident(void)
{
    uint32_t i0 = rd(V3D_IDENT0), i1 = rd(V3D_IDENT1), i2 = rd(V3D_IDENT2);
    printf("ident: IDENT0 %08x IDENT1 %08x IDENT2 %08x\n", i0, i1, i2);
    printf("  IDENT0: \"%c%c%c\", tech version %u\n", i0 & 0xff, (i0 >> 8) & 0xff, (i0 >> 16) & 0xff, i0 >> 24);
    printf("  IDENT1: revision %u, slices %u, QPUs per slice %u, TMUs per slice %u, semaphores %u, "
           "HDR %u, VPM %u KB\n",
           i1 & 15, (i1 >> 4) & 15, (i1 >> 8) & 15, (i1 >> 12) & 15, (i1 >> 16) & 0xff, (i1 >> 24) & 15, i1 >> 28);
    printf("  IDENT2: VRI memory %u, TLB memory %u, TLB double buffer %u\n", i2 & 15, (i2 >> 4) & 15,
           (i2 >> 8) & 15);
    check(i0 == 0x02443356, "IDENT0 is 0x02443356");
}

/* --- render list ------------------------------------------------------- */

/* --variant=mesa: the order of Mesa's kernel/vc4_render_cl.c (the clear
 * colours after TILE_RENDERING_MODE_CONFIG), with which each job stores the
 * previous job's clear colour (2026-11-04); by default the order of current
 * Linux (drivers/gpu/drm/vc4/vc4_render_cl.c): clear colours and the
 * None-mode store first. */
static int mesa_order;

static void emit_clear(struct cl *c, uint32_t clear_color)
{
    u8_(c, PKT_CLEAR_COLORS);
    u32_(c, clear_color);
    u32_(c, clear_color);
    u32_(c, 0); /* clear Z 0, VG mask 0 */
    u8_(c, 0);  /* clear stencil */
    /* A store in None mode, so that the new clear values reach the tile
     * buffer. */
    u8_(c, PKT_TILE_COORDINATES);
    u8_(c, 0);
    u8_(c, 0);
    u8_(c, PKT_STORE_TILE_BUFFER_GENERAL);
    u16_(c, 0);
    u32_(c, 0);
}

/* The render list for a frame of width x height at fb_off (raster RGBA8888),
 * cleared to clear_color; with binning, each tile branches to its list in
 * the tile allocation (after waiting for the binner once). */
static uint32_t build_rcl(uint32_t fb_off, int width, int height, uint32_t clear_color, int with_bin)
{
    static struct cl c;
    int tx = (width + 63) / 64, ty = (height + 63) / 64, x, y;

    c.n = 0;
    if (!mesa_order)
        emit_clear(&c, clear_color);
    u8_(&c, PKT_TILE_RENDERING_MODE_CONFIG);
    u32_(&c, mem.bus + fb_off);
    u16_(&c, (uint32_t)width);
    u16_(&c, (uint32_t)height);
    u16_(&c, RENDER_FORMAT_RGBA8888); /* raster, 1x, no MSAA */
    if (mesa_order)
        emit_clear(&c, clear_color);
    for (y = 0; y < ty; y++)
        for (x = 0; x < tx; x++) {
            int last = x == tx - 1 && y == ty - 1;
            u8_(&c, PKT_TILE_COORDINATES);
            u8_(&c, (uint32_t)x);
            u8_(&c, (uint32_t)y);
            if (with_bin) {
                if (x == 0 && y == 0)
                    u8_(&c, PKT_WAIT_ON_SEMAPHORE);
                u8_(&c, PKT_BRANCH_TO_SUB_LIST);
                u32_(&c, mem.bus + OFF_TALLOC + (uint32_t)(y * tx + x) * 32);
            }
            u8_(&c, last ? PKT_STORE_MS_TILE_BUFFER_AND_EOF : PKT_STORE_MS_TILE_BUFFER);
        }
    rcl_off = OFF_RCL + (rotate ? (uint32_t)(rcl_slot++ & 3) * 0x400 : 0);
    gpu_write(rcl_off, c.b, c.n);
    return c.n;
}

static int render_only(uint32_t fb_off, int width, int height, uint32_t color)
{
    uint32_t len = build_rcl(fb_off, width, height, color, 0);
    wr(V3D_RFC, 1);
    flush_caches();
    run_list(1, rcl_off, len);
    if (wait_count(V3D_RFC, 1, "render")) {
        reset_threads();
        return -1;
    }
    return 0;
}

/* --- clear --------------------------------------------------------------- */

static void test_clear(void)
{
    const uint32_t color = 0xff400000; /* R 0, G 0, B 0x40, A 0xff in memory order */
    uint32_t k, bad = 0, first_bad = 0;

    printf("clear: 128x128 RGBA8888 raster, colour %08x\n", color);
    gpu_fill(OFF_FB, 0x55555555, 128 * 128);
    if (render_only(OFF_FB, 128, 128, color)) {
        failures++;
        return;
    }
    for (k = 0; k < 128 * 128; k++)
        if (gpu_word(OFF_FB + k * 4) != color) {
            if (!bad++)
                first_bad = k;
        }
    if (bad)
        printf("  %u pixels differ, the first at (%u,%u): %08x\n", bad, first_bad % 128, first_bad / 128,
               gpu_word(OFF_FB + first_bad * 4));
    check(bad == 0, "every pixel cleared");
}

/* Render-only clears in a row with changing colours: which colour each job
 * stores (the first runs found the previous job's colour). */
static void test_clearseq(void)
{
    static const uint32_t colors[] = {0xff0000ff, 0xff00ff00, 0xff00ff00, 0xffff0000, 0xff0000ff};
    unsigned i;

    printf("clearseq: 128x128 render-only clears with colours");
    for (i = 0; i < sizeof colors / sizeof colors[0]; i++)
        printf(" %08x", colors[i]);
    printf("\n");
    for (i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        uint32_t k, same = 0;
        gpu_fill(OFF_FB, 0x55555555, 128 * 128);
        if (render_only(OFF_FB, 128, 128, colors[i]))
            return;
        for (k = 0; k < 128 * 128; k++)
            same += gpu_word(OFF_FB + k * 4) == colors[i];
        printf("  job %u: asked %08x, pixel (0,0) %08x, (127,0) %08x, (0,127) %08x, (127,127) %08x; %u of 16384 right\n",
               i, colors[i], gpu_word(OFF_FB), gpu_word(OFF_FB + 127 * 4), gpu_word(OFF_FB + 127 * 128 * 4),
               gpu_word(OFF_FB + (127 * 128 + 127) * 4), same);
    }
}

/* --- pictures ----------------------------------------------------------- */

static void write_ppm(const char *path, uint32_t fb_off, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) {
        perror(path);
        return;
    }
    /* Rows top first, as mesa_triangle writes them (its row 0 is GL's
     * bottom row, which is memory row 0 of the frame here too). */
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) {
            uint32_t p = gpu_word(fb_off + (uint32_t)(y * w + x) * 4);
            fputc(p & 0xff, f);
            fputc((p >> 8) & 0xff, f);
            fputc((p >> 16) & 0xff, f);
        }
    fclose(f);
    printf("  written: %s\n", path);
}

/* Compares the frame with a binary PPM of the same size; returns the number of
 * differing pixels, or -1. */
static long compare_ppm(const char *path, uint32_t fb_off, int w, int h)
{
    FILE *f = fopen(path, "rb");
    int pw, ph, maxv, x, y;
    long diff = 0, tile_diff[4] = {0, 0, 0, 0}, clear_diff = 0;
    if (!f) {
        perror(path);
        return -1;
    }
    if (fscanf(f, "P6 %d %d %d", &pw, &ph, &maxv) != 3 || pw != w || ph != h || maxv != 255) {
        fclose(f);
        printf("  %s: not a %dx%d PPM\n", path, w, h);
        return -1;
    }
    fgetc(f);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) {
            int r = fgetc(f), g = fgetc(f), b = fgetc(f);
            uint32_t p = gpu_word(fb_off + (uint32_t)(y * w + x) * 4);
            if ((uint32_t)r != (p & 0xff) || (uint32_t)g != ((p >> 8) & 0xff) || (uint32_t)b != ((p >> 16) & 0xff)) {
                if (diff < 5)
                    printf("  differs at memory (%d,%d): ours %08x, Mesa's %02x %02x %02x\n", x, y, p, r, g, b);
                diff++;
                tile_diff[((y / 64) * 2 + x / 64) & 3]++;
                if (p == 0xff400000)
                    clear_diff++;
            }
        }
    fclose(f);
    if (diff)
        printf("  differing pixels by tile (x,y in tiles; memory rows): (0,0) %ld (1,0) %ld (0,1) %ld (1,1) %ld; "
               "%ld of them hold the clear colour\n",
               tile_diff[0], tile_diff[1], tile_diff[2], tile_diff[3], clear_diff);
    return diff;
}

/* --- drawing ------------------------------------------------------------- */

/* One draw into the 128x128 frame at OFF_FB, cleared first: Mesa's shaders,
 * their uniform streams, vertex attributes, and our binning and render
 * lists. */
struct attr {
    uint32_t off;    /* in the vertex buffer */
    uint32_t bytes;  /* per vertex */
    uint32_t stride;
    uint32_t vs_vpm; /* VPM offsets (bytes) for the vertex and coordinate shader */
    uint32_t cs_vpm;
};

struct draw {
    const char *name;
    const uint64_t *fs, *vs, *cs;
    uint32_t fs_size, vs_size, cs_size; /* bytes */
    const uint32_t *fs_unif, *vs_unif, *cs_unif;
    uint32_t fs_unifs, vs_unifs, cs_unifs; /* words */
    int fs_threaded, fs_varyings;
    const struct attr *attr;
    int attrs;
    uint32_t vs_select, vs_size_attr, cs_select, cs_size_attr;
    const void *vertices;
    uint32_t vertices_size;
    int mode, count; /* GL primitive mode, vertices */
    uint32_t clear;
};

/* The tile allocation's first 1 KB, the tile state array and the pool
 * registers after the last draw. */
static uint32_t bin_snap[256], tsda_snap[48], bpca_snap, bpcs_snap, good_snap[2][256];
static int have_good[2]; /* per draw: [0] triangle, [1] the others */

static void dump_words(const char *what, const uint32_t *w, int n)
{
    int k;
    printf("  %s:", what);
    for (k = 0; k < n; k++)
        printf("%s%08x", k % 8 ? " " : "\n    ", w[k]);
    printf("\n");
}

/* --errstat: ERRSTAT around each draw. */
static int show_err;
static uint32_t err_before, err_binned;

static int run_draw(const struct draw *d)
{
    uint32_t code_fs, code_vs, code_cs, u_fs, u_vs, u_cs, rlen;
    struct cl c;
    int k;

    code_fs = OFF_CODE;
    code_vs = code_fs + d->fs_size;
    code_cs = code_vs + d->vs_size;
    gpu_write(code_fs, d->fs, d->fs_size);
    gpu_write(code_vs, d->vs, d->vs_size);
    gpu_write(code_cs, d->cs, d->cs_size);
    u_fs = OFF_UNIFORMS;
    u_vs = u_fs + 64;
    u_cs = u_vs + 64;
    gpu_write(u_fs, d->fs_unif, d->fs_unifs * 4);
    gpu_write(u_vs, d->vs_unif, d->vs_unifs * 4);
    gpu_write(u_cs, d->cs_unif, d->cs_unifs * 4);
    gpu_write(OFF_VERTICES, d->vertices, d->vertices_size);

    /* GL shader state record (36 bytes) and the attribute records (8 each). */
    c.n = 0;
    u16_(&c, SHADER_ENABLE_CLIPPING | (d->fs_threaded ? 0 : SHADER_FS_SINGLE_THREAD));
    u8_(&c, 0); /* fragment shader: uniforms (unused), varyings */
    u8_(&c, (uint32_t)d->fs_varyings);
    u32_(&c, mem.bus + code_fs);
    u32_(&c, mem.bus + u_fs);
    u16_(&c, 0); /* vertex shader: uniforms (unused), attribute select, total attribute size */
    u8_(&c, d->vs_select);
    u8_(&c, d->vs_size_attr);
    u32_(&c, mem.bus + code_vs);
    u32_(&c, mem.bus + u_vs);
    u16_(&c, 0); /* coordinate shader */
    u8_(&c, d->cs_select);
    u8_(&c, d->cs_size_attr);
    u32_(&c, mem.bus + code_cs);
    u32_(&c, mem.bus + u_cs);
    for (k = 0; k < d->attrs; k++) {
        u32_(&c, mem.bus + OFF_VERTICES + d->attr[k].off);
        u8_(&c, d->attr[k].bytes - 1);
        u8_(&c, d->attr[k].stride);
        u8_(&c, d->attr[k].vs_vpm);
        u8_(&c, d->attr[k].cs_vpm);
    }
    gpu_write(OFF_SHADER_REC, c.b, c.n);

    /* The binning list (Mesa's for such a draw, with the kernel's binner set-up). */
    c.n = 0;
    u8_(&c, PKT_TILE_BINNING_MODE_CONFIG);
    u32_(&c, mem.bus + OFF_TALLOC);
    u32_(&c, TALLOC_SIZE);
    u32_(&c, mem.bus + OFF_TSDA);
    u8_(&c, 2); /* tiles */
    u8_(&c, 2);
    u8_(&c, BIN_AUTO_INIT_TSDA | BIN_BLOCK_SIZE_128); /* initial block 32 */
    u8_(&c, PKT_START_TILE_BINNING);
    u8_(&c, PKT_PRIMITIVE_LIST_FORMAT);
    u8_(&c, 0x12); /* 16-bit index, triangles */
    u8_(&c, PKT_CLIP_WINDOW);
    u16_(&c, 0);
    u16_(&c, 0);
    u16_(&c, 128);
    u16_(&c, 128);
    u8_(&c, PKT_CONFIGURATION_BITS); /* both faces, depth ALWAYS, early Z updates */
    u8_(&c, 0x03);
    u8_(&c, 0x70);
    u8_(&c, 0x02);
    u8_(&c, PKT_DEPTH_OFFSET);
    u16_(&c, 0);
    u16_(&c, 0);
    u8_(&c, PKT_POINT_SIZE);
    f32_(&c, 1.0f);
    u8_(&c, PKT_LINE_WIDTH);
    f32_(&c, 1.0f);
    u8_(&c, PKT_CLIPPER_XY_SCALING);
    f32_(&c, 64.0f * 16.0f);
    f32_(&c, 64.0f * 16.0f);
    u8_(&c, PKT_CLIPPER_Z_SCALING);
    f32_(&c, 0.5f);
    f32_(&c, 0.5f);
    u8_(&c, PKT_VIEWPORT_OFFSET);
    u16_(&c, 64 * 16);
    u16_(&c, 64 * 16);
    u8_(&c, PKT_FLAT_SHADE_FLAGS);
    u32_(&c, 0);
    u8_(&c, PKT_GL_SHADER_STATE);
    u32_(&c, (mem.bus + OFF_SHADER_REC) | (uint32_t)(d->attrs & 7)); /* 0 means 8 */
    u8_(&c, PKT_GL_ARRAY_PRIMITIVE);
    u8_(&c, (uint32_t)d->mode);
    u32_(&c, (uint32_t)d->count);
    u32_(&c, 0);
    u8_(&c, PKT_INCREMENT_SEMAPHORE);
    u8_(&c, PKT_FLUSH);
    gpu_write(OFF_BCL, c.b, c.n);

    gpu_fill(OFF_FB, 0x55555555, 128 * 128);
    rlen = build_rcl(OFF_FB, 128, 128, d->clear, 1);

    wr(V3D_BPOA, mem.bus + OFF_OVERFLOW);
    wr(V3D_BPOS, OVERFLOW_SIZE);
    wr(V3D_BFC, 1);
    wr(V3D_RFC, 1);
    flush_caches();
    err_before = rd(V3D_ERRSTAT);
    if (bin_mode == 1) {
        /* Both threads at once: the render list's WAIT_ON_SEMAPHORE waits
         * for the binning list's INCREMENT_SEMAPHORE. */
        run_list(0, OFF_BCL, c.n);
        run_list(1, rcl_off, rlen);
        if (wait_count(V3D_BFC, 1, "binning")) {
            reset_threads();
            return -1;
        }
    } else {
        run_list(0, OFF_BCL, c.n);
        if (wait_count(V3D_BFC, 1, "binning")) {
            reset_threads();
            return -1;
        }
        if (bin_delay_us) {
            uint64_t t0 = plat_time_us();
            while (plat_time_us() - t0 < (uint64_t)bin_delay_us)
                ;
        }
        err_binned = rd(V3D_ERRSTAT);
        flush_texture_caches();
        run_list(1, rcl_off, rlen);
    }
    if (wait_count(V3D_RFC, 1, "render")) {
        reset_threads();
        return -1;
    }
    if (show_err)
        printf("  ERRSTAT before %08x, after binning %08x, after rendering %08x\n", err_before, err_binned,
               rd(V3D_ERRSTAT));
    {   /* the binner's results, for picture() when the picture is wrong */
        uint32_t k;
        for (k = 0; k < 256; k++)
            bin_snap[k] = gpu_word(OFF_TALLOC + k * 4);
        for (k = 0; k < 48; k++)
            tsda_snap[k] = gpu_word(OFF_TSDA + k * 4);
        bpca_snap = rd(V3D_BPCA) - mem.bus;
        bpcs_snap = rd(V3D_BPCS);
    }
    return 0;
}

/* Writes DIR/NAME.ppm and compares the frame with ref (when given). */
static void picture(const struct draw *d, const char *ref, const char *outdir)
{
    char path[512];
    printf("  pixel (64,64) %08x, pixel (2,2) %08x\n", gpu_word(OFF_FB + (64 * 128 + 64) * 4),
           gpu_word(OFF_FB + (2 * 128 + 2) * 4));
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, d->name);
    write_ppm(path, OFF_FB, 128, 128);
    if (ref) {
        int g = strcmp(d->name, "triangle") != 0;
        long n = compare_ppm(ref, OFF_FB, 128, 128);
        if (n >= 0)
            printf("  %ld of %d pixels differ from %s\n", n, 128 * 128, ref);
        check(n == 0, "the same picture as Mesa's");
        if (n == 0 && !have_good[g]) {
            memcpy(good_snap[g], bin_snap, sizeof bin_snap);
            have_good[g] = 1;
        }
        if (n > 0) {
            printf("  pool after binning: BPCA offset %#x, BPCS %#x\n", bpca_snap, bpcs_snap);
            dump_words("tile state array (48 bytes per tile)", tsda_snap, 48);
            dump_words("tile allocation, first 1 KB, this run", bin_snap, 128);
            if (have_good[g])
                dump_words("tile allocation, first 1 KB, a good run", good_snap[g], 128);
        }
    }
}

/* The vertex and coordinate shaders' uniforms in both Mesa programs: 1.0 (w),
 * the viewport's x and y scale (1/16 pixel), its z offset. */
static void viewport_uniforms(uint32_t *u)
{
    const float f[4] = {1.0f, 64.0f * 16.0f, 64.0f * 16.0f, 0.5f};
    memcpy(u, f, sizeof f);
}

/* --- triangle ------------------------------------------------------------ */

/* Mesa vc4's code for tests/pi3/ref/mesa_triangle.c (Mesa 26.2.2 on the Pi,
 * VC4_DEBUG=qpu): fragment "prog 1/4", vertex "prog 0/3", coordinate
 * "prog 0/4". Uniforms, in the order the code reads them: vertex and
 * coordinate shader as viewport_uniforms; fragment shader the packed colour.
 * The fragment shader is not threaded. */
static const uint64_t tri_fs[] = {
    0x100009e7009e7000ull, 0x100009e7009e7000ull, 0x10020ba715827d80ull, /* mov tlb_color_all, uni */
    0x300009e7009e7000ull, 0x100009e7009e7000ull, 0x500009e7009e7000ull,
};
static const uint64_t tri_vs[] = {
    0xd002102702821f80ull, 0xe0024c6700201a00ull, 0x100049e020c20037ull, 0x100049e1209c0007ull,
    0x1012402227c20277ull, 0x100049e3209c0017ull, 0x10220027079e76c0ull, 0xe0025c6700001a00ull,
    0x10020c2715027d80ull, 0x10020c2715827d80ull, 0x10020c27159c0fc0ull, 0x300009e7009e7000ull,
    0x100009e7009e7000ull, 0x100009e7009e7000ull,
};
static const uint64_t tri_cs[] = {
    0xe0024c6700201a00ull, 0xe0025c6700001a00ull, 0xd002102702821f80ull, 0x1002086715c27d80ull,
    0x10024c233582724eull, 0x100248a135c00d9full, 0x1012402027827256ull, 0x10024c20359c0487ull,
    0xd0020c27159c0fc0ull, 0x10220027079e7000ull, 0xd0020c27159e0fc0ull, 0x10020c2715027d80ull,
    0x10020c2715827d80ull, 0x10020c27159c0fc0ull, 0x300009e7009e7000ull, 0x100009e7009e7000ull,
    0x100009e7009e7000ull,
};

static void test_triangle(const char *ref, const char *outdir)
{
    static const float tri[] = {-0.75f, -0.75f, 0.75f, -0.5f, -0.25f, 0.75f};
    static const struct attr a[] = {{0, 8, 8, 0, 0}};
    static const uint32_t fs_unif[] = {0xff4080ff}; /* (1, .5, .25, 1) */
    uint32_t vp[4];
    struct draw d;
    uint32_t seg_lo = (mem.bus + OFF_TSDA) & 0xf0000000u,
             seg_hi = (mem.bus + OFF_OVERFLOW + OVERFLOW_SIZE - 1) & 0xf0000000u;

    printf("triangle: 128x128, Mesa's shaders, our lists\n");
    check(seg_lo == seg_hi, "binner memory in one 256 MB segment");
    viewport_uniforms(vp);
    memset(&d, 0, sizeof d);
    d.name = "triangle";
    d.fs = tri_fs, d.fs_size = sizeof tri_fs;
    d.vs = tri_vs, d.vs_size = sizeof tri_vs;
    d.cs = tri_cs, d.cs_size = sizeof tri_cs;
    d.fs_unif = fs_unif, d.fs_unifs = 1;
    d.vs_unif = d.cs_unif = vp, d.vs_unifs = d.cs_unifs = 4;
    d.attr = a, d.attrs = 1;
    d.vs_select = d.cs_select = 1, d.vs_size_attr = d.cs_size_attr = 8;
    d.vertices = tri, d.vertices_size = sizeof tri;
    d.mode = 4, d.count = 3; /* GL_TRIANGLES */
    d.clear = 0xff400000;    /* (0, 0, .25, 1) */
    if (run_draw(&d)) {
        failures++;
        return;
    }
    picture(&d, ref, outdir);
}

/* --- texture ------------------------------------------------------------- */

/* Mesa vc4's code for tests/pi3/ref/mesa_texture.c (VC4_DEBUG=qpu,qir):
 * fragment "prog 1/1" (threaded; 2 varyings; texture2D through TMU0, which
 * takes its configuration words P0 and P1 from the uniform stream: the stream
 * is P0, P1), vertex "prog 0/1" (position and texture coordinate, 16 bytes),
 * coordinate "prog 0/2" (position only); their uniforms as
 * viewport_uniforms. */
static const uint64_t tex_fs[] = {
    0x100049e0203e303eull, 0x100248e1213e317eull, 0x600208a7019e7340ull, 0x10021e67159e7480ull,
    0x10021e27159e76c0ull, 0xa00009e7009e7000ull, 0x190208e7049e7900ull, 0x1b424823849e791bull,
    0x1d524863849e7900ull, 0x1f6248a3849e7909ull, 0x117049e3809e7012ull, 0x10020ba7159e76c0ull,
    0x300009e7009e7000ull, 0x100009e7009e7000ull, 0x500009e7009e7000ull,
};
static const uint64_t tex_vs[] = {
    0xd002102702821f80ull, 0xe0024c6700401a00ull, 0x100049e220c20037ull, 0x100049e3209c0017ull,
    0x1012402027c206f7ull, 0x1002482135c00d87ull, 0x1022402187c27276ull, 0xe0025c6700001a00ull,
    0x10020c2715027d80ull, 0x10020c2715827d80ull, 0x10020c27159c0fc0ull, 0x10020c27159e7000ull,
    0x10020c27159e7240ull, 0x300009e7009e7000ull, 0x100009e7009e7000ull, 0x100009e7009e7000ull,
};
static const uint64_t tex_cs[] = {
    0xe0024c6700201a00ull, 0xe0025c6700001a00ull, 0xd002102702821f80ull, 0x1002086715c27d80ull,
    0x10024c233582724eull, 0x100248a135c00d9full, 0x1012402027827256ull, 0x10024c20359c0487ull,
    0xd0020c27159c0fc0ull, 0x10220027079e7000ull, 0xd0020c27159e0fc0ull, 0x10020c2715027d80ull,
    0x10020c2715827d80ull, 0x10020c27159c0fc0ull, 0x300009e7009e7000ull, 0x100009e7009e7000ull,
    0x100009e7009e7000ull,
};

#define TEX_TYPE_RGBA8888 0
#define TEX_TYPE_RGBA32R 16
#define OFF_TEXTURE 0x306000 /* 4 KB aligned: P0 holds the address in 4 KB units */

/* mesa_texture's 16x16 texel (x, y), as a little-endian RGBA8888 word. */
static uint32_t texel(int x, int y)
{
    return (uint32_t)(x * 16) | (uint32_t)(y * 16) << 8 | ((x + y) & 1 ? 0xffu : 0) << 16 | 0xffu << 24;
}

/* Draws mesa_texture's quad with its 16x16 texture stored as layout: "lt"
 * (4x4-pixel utiles of 64 bytes in raster order, type RGBA8888: what Mesa
 * uses for a texture this small; the TMU picks LT by the size) or "raster"
 * (rows of 64 bytes, type RGBA32R). */
static void texture_case(const char *layout, const char *ref, const char *outdir)
{
    static const float quad[] = {-0.75f, -0.75f, 0.0f, 0.0f, 0.75f, -0.75f, 1.0f, 0.0f,
                                 -0.75f, 0.75f,  0.0f, 1.0f, 0.75f, 0.75f,  1.0f, 1.0f};
    static const struct attr a[] = {{0, 8, 16, 0, 0}, {8, 8, 16, 8, 0}};
    int lt = !strcmp(layout, "lt"), type = lt ? TEX_TYPE_RGBA8888 : TEX_TYPE_RGBA32R, x, y;
    uint32_t fs_unif[2], vp[4];
    struct draw d;
    char name[64];

    printf("texture %s: 16x16 RGBA8, NEAREST, CLAMP_TO_EDGE, type %d\n", layout, type);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            uint32_t off = lt ? (uint32_t)(((y / 4) * 4 + x / 4) * 64 + (y % 4) * 16 + (x % 4) * 4)
                              : (uint32_t)(y * 64 + x * 4);
            in_bounds(OFF_TEXTURE + off, 4);
            mem.cpu[(OFF_TEXTURE + off) / 4] = texel(x, y);
        }
    /* P0: address (4 KB units), the type's low 4 bits, no mip levels beyond
     * 0. P1: the type's bit 4, height, width, NEAREST magnification (1) and
     * minification (1), CLAMP wrap (1) in t and s. */
    fs_unif[0] = (mem.bus + OFF_TEXTURE) | (uint32_t)(type & 15) << 4;
    fs_unif[1] = (uint32_t)(type >> 4) << 31 | 16u << 20 | 16u << 8 | 1u << 7 | 1u << 4 | 1u << 2 | 1u;
    viewport_uniforms(vp);

    memset(&d, 0, sizeof d);
    snprintf(name, sizeof name, "texture_%s", layout);
    d.name = name;
    d.fs = tex_fs, d.fs_size = sizeof tex_fs;
    d.vs = tex_vs, d.vs_size = sizeof tex_vs;
    d.cs = tex_cs, d.cs_size = sizeof tex_cs;
    d.fs_unif = fs_unif, d.fs_unifs = 2;
    d.vs_unif = d.cs_unif = vp, d.vs_unifs = d.cs_unifs = 4;
    d.fs_threaded = 1, d.fs_varyings = 2;
    d.attr = a, d.attrs = 2;
    d.vs_select = 3, d.vs_size_attr = 16, d.cs_select = 1, d.cs_size_attr = 8;
    d.vertices = quad, d.vertices_size = sizeof quad;
    d.mode = 5, d.count = 4; /* GL_TRIANGLE_STRIP */
    d.clear = 0xff400000;
    if (run_draw(&d)) {
        failures++;
        return;
    }
    picture(&d, ref, outdir);
}

/* A raster (RGBA32R) texture of w x h, rows of w * 4 bytes, NEAREST: the
 * same quad, checked by the CPU. Texel (x, y) holds R = x, G = y, B = x ^ y
 * (mod 256); every pixel inside the quad (away from its edges) must decode
 * to a texel within one of where NEAREST sampling puts it. Mesa copies
 * raster textures to a tiled shadow ("Raspberry Pi doesn't support sampling
 * from raster textures", vc4_state.c); this measures what the TMU does. */
static int raster_size(int w, int h, int stride, int quiet)
{
    static const float quad[] = {-0.75f, -0.75f, 0.0f, 0.0f, 0.75f, -0.75f, 1.0f, 0.0f,
                                 -0.75f, 0.75f,  0.0f, 1.0f, 0.75f, 0.75f,  1.0f, 1.0f};
    static const struct attr a[] = {{0, 8, 16, 0, 0}, {8, 8, 16, 8, 0}};
    uint32_t fs_unif[2], vp[4];
    struct draw d;
    int x, y, bad = 0, checked = 0;

    if (!quiet)
        printf("texture raster %dx%d: NEAREST, CLAMP_TO_EDGE, rows of %d texels\n", w, h, stride);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t off = OFF_TEXTURE + (uint32_t)(y * stride + x) * 4;
            in_bounds(off, 4);
            mem.cpu[off / 4] =
                (uint32_t)(x & 255) | (uint32_t)(y & 255) << 8 | (uint32_t)((x ^ y) & 255) << 16 | 0xffu << 24;
        }
    fs_unif[0] = (mem.bus + OFF_TEXTURE) | (uint32_t)(TEX_TYPE_RGBA32R & 15) << 4;
    fs_unif[1] = 1u << 31 | (uint32_t)(h & 2047) << 20 | (uint32_t)(w & 2047) << 8 | 1u << 7 | 1u << 4 | 1u << 2 | 1u;
    viewport_uniforms(vp);
    memset(&d, 0, sizeof d);
    d.name = "texture_raster_size";
    d.fs = tex_fs, d.fs_size = sizeof tex_fs;
    d.vs = tex_vs, d.vs_size = sizeof tex_vs;
    d.cs = tex_cs, d.cs_size = sizeof tex_cs;
    d.fs_unif = fs_unif, d.fs_unifs = 2;
    d.vs_unif = d.cs_unif = vp, d.vs_unifs = d.cs_unifs = 4;
    d.fs_threaded = 1, d.fs_varyings = 2;
    d.attr = a, d.attrs = 2;
    d.vs_select = 3, d.vs_size_attr = 16, d.cs_select = 1, d.cs_size_attr = 8;
    d.vertices = quad, d.vertices_size = sizeof quad;
    d.mode = 5, d.count = 4;
    d.clear = 0xff400000;
    if (run_draw(&d)) {
        failures++;
        return -1;
    }
    /* The quad covers pixels 16 to 111 in x and y (memory row = GL row). */
    for (y = 18; y < 110; y++)
        for (x = 18; x < 110; x++) {
            uint32_t p = gpu_word(OFF_FB + (uint32_t)(y * 128 + x) * 4);
            int tx = (int)(((x + 0.5) - 16) / 96.0 * w), ty = (int)(((y + 0.5) - 16) / 96.0 * h);
            int rx = (int)(p & 255), ry = (int)((p >> 8) & 255), rb = (int)((p >> 16) & 255);
            int dx = ((rx - (tx & 255)) + 256) % 256, dy = ((ry - (ty & 255)) + 256) % 256;
            checked++;
            if (rb != ((rx ^ ry) & 255) || (dx > 1 && dx < 255) || (dy > 1 && dy < 255)) {
                if (bad < 4 && !quiet)
                    printf("  pixel (%d,%d): %08x, want texel about (%d,%d)\n", x, y, p, tx, ty);
                bad++;
            }
        }
    if (!quiet) {
        printf("  %d of %d pixels wrong\n", bad, checked);
        check(bad == 0, "raster texture sampled");
    }
    return bad;
}

/* Which row strides (in texels) work for a raster texture w wide: every
 * multiple of 4 texels from w up to the next power of two. */
static void raster_stride(int w, int h)
{
    int s, p2 = 1, found = 0;
    while (p2 < w)
        p2 <<= 1;
    printf("raster stride of a %dx%d texture:", w, h);
    for (s = (w + 3) & ~3; s <= p2; s += 4)
        if (raster_size(w, h, s, 1) == 0) {
            printf(" %d works", s);
            found = 1;
        }
    printf("%s\n", found ? "" : " none of them");
}

/* Where the TMU reads a raster texture's texels: the memory holds each word's
 * own index (R, G, B = bits 0-7, 8-15, 16-23), and for some texels the word
 * index fetched is printed (from the pixel at the centre of the texel's
 * footprint in the quad). */
static void raster_map(int w, int h)
{
    static const float quad[] = {-0.75f, -0.75f, 0.0f, 0.0f, 0.75f, -0.75f, 1.0f, 0.0f,
                                 -0.75f, 0.75f,  0.0f, 1.0f, 0.75f, 0.75f,  1.0f, 1.0f};
    static const struct attr a[] = {{0, 8, 16, 0, 0}, {8, 8, 16, 8, 0}};
    uint32_t fs_unif[2], vp[4], k;
    struct draw d;
    int ty, tx;

    for (k = 0; k < (MEM_SIZE - OFF_TEXTURE) / 4; k++) /* to the end of the allocation, no further */
        mem.cpu[OFF_TEXTURE / 4 + k] = k | 0xffu << 24;
    fs_unif[0] = (mem.bus + OFF_TEXTURE) | (uint32_t)(TEX_TYPE_RGBA32R & 15) << 4;
    fs_unif[1] = 1u << 31 | (uint32_t)(h & 2047) << 20 | (uint32_t)(w & 2047) << 8 | 1u << 7 | 1u << 4 | 1u << 2 | 1u;
    viewport_uniforms(vp);
    memset(&d, 0, sizeof d);
    d.name = "texture_raster_map";
    d.fs = tex_fs, d.fs_size = sizeof tex_fs;
    d.vs = tex_vs, d.vs_size = sizeof tex_vs;
    d.cs = tex_cs, d.cs_size = sizeof tex_cs;
    d.fs_unif = fs_unif, d.fs_unifs = 2;
    d.vs_unif = d.cs_unif = vp, d.vs_unifs = d.cs_unifs = 4;
    d.fs_threaded = 1, d.fs_varyings = 2;
    d.attr = a, d.attrs = 2;
    d.vs_select = 3, d.vs_size_attr = 16, d.cs_select = 1, d.cs_size_attr = 8;
    d.vertices = quad, d.vertices_size = sizeof quad;
    d.mode = 5, d.count = 4;
    d.clear = 0xff400000;
    if (run_draw(&d)) {
        failures++;
        return;
    }
    /* Each row's first texel (w <= 96: every texel has its own pixel). */
    printf("raster map %dx%d, word fetched for texel (0, y), y = 0, 1, ...:", w, h);
    for (ty = 0; ty < h; ty++) {
        int py = 16 + (int)((ty + 0.5) * 96.0 / h);
        int px = 16 + (int)(0.5 * 96.0 / w);
        (void)tx;
        printf(" %u", gpu_word(OFF_FB + (uint32_t)(py * 128 + px) * 4) & 0xffffffu);
    }
    printf("\n");
}

static void test_rastermap(void)
{
    static const int widths[] = {16, 20, 24, 32, 36, 48, 64, 72, 80, 88, 96};
    /* w, h, row stride in texels */
    static const int sizes[][3] = {{64, 60, 64}, {64, 37, 64}, {32, 100, 32}, {128, 7, 128},
                                   {72, 64, 128}, {100, 64, 128}, {72, 32, 128}, {72, 16, 128},
                                   {72, 8, 128}, {72, 4, 128}, {100, 60, 128}, {72, 60, 128}};
    unsigned i;
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++)
        raster_map(widths[i], 16);
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int bad = raster_size(sizes[i][0], sizes[i][1], sizes[i][2], 1);
        printf("raster %dx%d, rows of %d texels: %s\n", sizes[i][0], sizes[i][1], sizes[i][2],
               bad ? "WRONG" : "right");
    }
    raster_stride(100, 60);
    raster_stride(72, 60);
    raster_stride(200, 40);
    raster_stride(36, 20);
}

static void test_texture(const char *ref, const char *outdir)
{
    /* Raster textures read right when the width is a power of two (any
     * height). Other widths are read with a row addressing no stride
     * matches ("rastermap"): rows in groups of 4, spaced inside a group by
     * the width rounded up to a power of two, the groups spaced by 4 times
     * the width rounded up to a granule that depends on the height (4
     * texels at height 16, 32 at heights 40 and 60). So they are only
     * reported here. */
    static const int good[][2] = {{64, 64}, {256, 16}, {16, 256}, {64, 37}, {32, 100}, {128, 7}};
    static const int bad[][3] = {{72, 60, 72}, {72, 60, 128}, {100, 60, 100}, {200, 40, 200}};
    unsigned i;

    texture_case("lt", ref, outdir);
    texture_case("raster", ref, outdir);
    for (i = 0; i < sizeof good / sizeof good[0]; i++)
        raster_size(good[i][0], good[i][1], good[i][0], 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int n = raster_size(bad[i][0], bad[i][1], bad[i][2], 1);
        printf("texture raster %dx%d, rows of %d texels: %d of 8464 pixels wrong (expected: the width is not a "
               "power of two)\n",
               bad[i][0], bad[i][1], bad[i][2], n);
    }
}

/* --- stride -------------------------------------------------------------- */

/* Runs of cleared and untouched words from the frame's start: shows the
 * stride the raster store uses and which pixels it writes. */
static void test_stride(void)
{
    static const int widths[] = {128, 130, 129};
    const uint32_t color = 0xff00ff00, pattern = 0xaaaaaaaa;
    const int h = 70, span = 160 * 80;
    unsigned i;

    printf("stride: raster stores of frames 128, 130 and 129 pixels wide, %d high\n", h);
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        int w = widths[i], k, runs = 0, last = -1;
        uint32_t prev = 0;
        int start = 0;
        char line[512];
        int pos = 0;

        gpu_fill(OFF_FB, pattern, (uint32_t)span);
        if (render_only(OFF_FB, w, h, color)) {
            failures++;
            continue;
        }
        line[0] = 0;
        for (k = 0; k <= span; k++) {
            uint32_t v = k < span ? gpu_word(OFF_FB + (uint32_t)k * 4) : ~0u;
            if (k < span && v == color)
                last = k;
            if (k == 0) {
                prev = v;
                continue;
            }
            if (v != prev) {
                if (runs < 8 && pos < 400)
                    pos += snprintf(line + pos, sizeof line - (size_t)pos, " %s%d", prev == color ? "C" : "-",
                                    k - start);
                runs++;
                start = k;
                prev = v;
            }
        }
        printf("  width %d: runs (C cleared, - untouched, in pixels):%s ...; last cleared pixel %d "
               "(row %d col %d at stride %d)\n",
               w, line, last, last / w, last % w, w);
    }
}

int main(int argc, char **argv)
{
    enum plat_power power = PLAT_POWER_QPU;
    const char *ref = NULL, *texref = NULL, *outdir = ".";
    int all = 1, k, want[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const char *names[] = {"ident", "clear", "triangle", "stride", "clearseq", "texture", "rastermap", "texrepeat", "power"};

    for (k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--power=qpu"))
            power = PLAT_POWER_QPU;
        else if (!strcmp(argv[k], "--variant=mesa"))
            mesa_order = 1;
        else if (!strcmp(argv[k], "--no-render-flush"))
            no_render_flush = 1;
        else if (!strcmp(argv[k], "--concurrent"))
            bin_mode = 1;
        else if (!strncmp(argv[k], "--bin-delay=", 12))
            bin_delay_us = atoi(argv[k] + 12);
        else if (!strcmp(argv[k], "--no-reset"))
            plat_no_reset = 1;
        else if (!strcmp(argv[k], "--keep-power"))
            plat_keep_power = 1;
        else if (!strcmp(argv[k], "--errstat"))
            show_err = 1;
        else if (!strcmp(argv[k], "--rotate"))
            rotate = 1;
        else if (!strcmp(argv[k], "--power=domain"))
            power = PLAT_POWER_DOMAIN;
        else if (!strncmp(argv[k], "--ref=", 6))
            ref = argv[k] + 6;
        else if (!strncmp(argv[k], "--texref=", 9))
            texref = argv[k] + 9;
        else if (!strncmp(argv[k], "--out=", 6))
            outdir = argv[k] + 6;
        else {
            int t;
            for (t = 0; t < 9; t++)
                if (!strcmp(argv[k], names[t]))
                    break;
            if (t == 9) {
                fprintf(stderr, "usage: %s [--power=domain|qpu] [--variant=mesa] [--rotate] [--ref=FILE.ppm] [--out=DIR] [--texref=FILE.ppm] [ident|clear|triangle|stride|clearseq|texture|rastermap|texrepeat|power]...\n",
                        argv[0]);
                return 2;
            }
            want[t] = 1;
            all = 0;
        }
    }

    if (plat_open(power)) {
        plat_close();
        return 1;
    }
    if (rd(V3D_IDENT0) != 0x02443356) {
        printf("IDENT0 is %08x, not 0x02443356: the 3D core is not powered or not a VC4\n", rd(V3D_IDENT0));
        plat_close();
        return 1;
    }
    if (plat_alloc(MEM_SIZE, &mem)) {
        plat_close();
        return 1;
    }
    printf("memory: %u KB at bus %08x\n", mem.size >> 10, mem.bus);
    printf("state at start: CT0CS %08x CT1CS %08x PCS %08x VPMBASE %08x INTENA %08x\n", rd(V3D_CT0CS), rd(V3D_CT1CS),
           rd(V3D_PCS), rd(V3D_VPMBASE), rd(V3D_INTENA));
    show_sched("state at start");

    /* As Linux's vc4_v3d_init_hw: no VPM kept for user programs; interrupts
     * off (completion is polled, as under Emu68) and cleared. */
    wr(V3D_VPMBASE, 0);
    wr(V3D_INTDIS, 0xf);
    wr(V3D_INTCTL, 0xf);

    if (want[8])
        test_power();
    if (all || want[0])
        test_ident();
    if (all || want[1])
        test_clear();
    if (all || want[2])
        test_triangle(ref, outdir);
    if (all || want[3])
        test_stride();
    if (want[4])
        test_clearseq();
    if (all || want[5])
        test_texture(texref, outdir);
    if (want[6])
        test_rastermap();
    if (want[7])
        for (k = 0; k < 40; k++) {
            texture_case("lt", texref, outdir);
            test_triangle(ref, outdir);
        }

    show_sched("state at end");
    plat_free(&mem);
    plat_close();
    printf("%s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
