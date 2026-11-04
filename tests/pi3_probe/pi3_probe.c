/*
 * The bare-GPU probe for the Raspberry Pi 3's VideoCore IV 3D core (V3D 2.1):
 * PI3_PLAN P1 (§6.2, §8). No driver: the 3D core's registers, power and
 * memory from the firmware's mailbox (plat.h), command lists built here.
 *   pi3_probe [--power=domain|qpu] [--variant=mesa] [--rotate] [--ref=FILE.ppm] [--out=DIR] [TEST...]
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
 *   clearseq  (only when named) five render-only clears with changing
 *             colours: which colour each job stores.
 * Power: --power=qpu (the default) is the mailbox's SET_ENABLE_QPU; with
 * --power=domain (SET_DOMAIN_STATE) the registers read 0xdeadbeef.
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

static void gpu_write(uint32_t off, const void *src, uint32_t bytes)
{
    const uint8_t *s = src;
    uint32_t k;
    for (k = 0; k < bytes; k += 4) {
        uint32_t w = 0;
        memcpy(&w, s + k, bytes - k < 4 ? bytes - k : 4);
        mem.cpu[(off + k) / 4] = w;
    }
}

static void gpu_fill(uint32_t off, uint32_t value, uint32_t words)
{
    uint32_t k;
    for (k = 0; k < words; k++)
        mem.cpu[off / 4 + k] = value;
}

static uint32_t gpu_word(uint32_t off)
{
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

/* --- triangle ------------------------------------------------------------ */

/* Mesa vc4's code for tests/pi3/ref/mesa_triangle.c (Mesa 26.2.2 on the Pi,
 * VC4_DEBUG=qpu): fragment "prog 1/4", vertex "prog 0/3", coordinate
 * "prog 0/4". Uniforms, in the order the code reads them: vertex and
 * coordinate shader 1.0 (w), viewport x scale, y scale (in 1/16 pixel), z
 * offset; fragment shader the packed colour. */
static const uint64_t fs_code[] = {
    0x100009e7009e7000ull, 0x100009e7009e7000ull, 0x10020ba715827d80ull, /* mov tlb_color_all, uni */
    0x300009e7009e7000ull, 0x100009e7009e7000ull, 0x500009e7009e7000ull,
};
static const uint64_t vs_code[] = {
    0xd002102702821f80ull, 0xe0024c6700201a00ull, 0x100049e020c20037ull, 0x100049e1209c0007ull,
    0x1012402227c20277ull, 0x100049e3209c0017ull, 0x10220027079e76c0ull, 0xe0025c6700001a00ull,
    0x10020c2715027d80ull, 0x10020c2715827d80ull, 0x10020c27159c0fc0ull, 0x300009e7009e7000ull,
    0x100009e7009e7000ull, 0x100009e7009e7000ull,
};
static const uint64_t cs_code[] = {
    0xe0024c6700201a00ull, 0xe0025c6700001a00ull, 0xd002102702821f80ull, 0x1002086715c27d80ull,
    0x10024c233582724eull, 0x100248a135c00d9full, 0x1012402027827256ull, 0x10024c20359c0487ull,
    0xd0020c27159c0fc0ull, 0x10220027079e7000ull, 0xd0020c27159e0fc0ull, 0x10020c2715027d80ull,
    0x10020c2715827d80ull, 0x10020c27159c0fc0ull, 0x300009e7009e7000ull, 0x100009e7009e7000ull,
    0x100009e7009e7000ull,
};

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
    long diff = 0;
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
                    printf("  differs at (%d,%d): ours %02x %02x %02x, Mesa's %02x %02x %02x\n", x, h - 1 - y,
                           p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff, r, g, b);
                diff++;
            }
        }
    fclose(f);
    return diff;
}

static void test_triangle(const char *ref, const char *outdir)
{
    static const float tri[] = {-0.75f, -0.75f, 0.75f, -0.5f, -0.25f, 0.75f};
    const uint32_t clear = 0xff400000, color = 0xff4080ff; /* (0,0,.25,1) and (1,.5,.25,1) */
    const float w_one = 1.0f, xs = 64.0f * 16.0f, ys = 64.0f * 16.0f, zoff = 0.5f;
    uint32_t vs_unif[4], fs_unif[1], code_fs, code_vs, code_cs, u_fs, u_vs, u_cs, rlen;
    struct cl c;
    char path[512];
    uint32_t seg_lo = (mem.bus + OFF_TSDA) & 0xf0000000u, seg_hi = (mem.bus + OFF_OVERFLOW + OVERFLOW_SIZE - 1) & 0xf0000000u;

    printf("triangle: 128x128, Mesa's shaders, our lists\n");
    check(seg_lo == seg_hi, "binner memory in one 256 MB segment");

    memcpy(&vs_unif[0], &w_one, 4);
    memcpy(&vs_unif[1], &xs, 4);
    memcpy(&vs_unif[2], &ys, 4);
    memcpy(&vs_unif[3], &zoff, 4);
    fs_unif[0] = color;

    code_fs = OFF_CODE;
    code_vs = code_fs + sizeof fs_code;
    code_cs = code_vs + sizeof vs_code;
    gpu_write(code_fs, fs_code, sizeof fs_code);
    gpu_write(code_vs, vs_code, sizeof vs_code);
    gpu_write(code_cs, cs_code, sizeof cs_code);
    u_fs = OFF_UNIFORMS;
    u_vs = u_fs + 16;
    u_cs = u_vs + 16;
    gpu_write(u_fs, fs_unif, sizeof fs_unif);
    gpu_write(u_vs, vs_unif, sizeof vs_unif);
    gpu_write(u_cs, vs_unif, sizeof vs_unif);
    gpu_write(OFF_VERTICES, tri, sizeof tri);

    /* GL shader state record (36 bytes) and one attribute record. */
    c.n = 0;
    u16_(&c, SHADER_ENABLE_CLIPPING | SHADER_FS_SINGLE_THREAD);
    u8_(&c, 0); /* fragment shader: uniforms (unused), varyings */
    u8_(&c, 0);
    u32_(&c, mem.bus + code_fs);
    u32_(&c, mem.bus + u_fs);
    u16_(&c, 0); /* vertex shader: uniforms (unused), attribute select, total attribute size */
    u8_(&c, 1);
    u8_(&c, 8);
    u32_(&c, mem.bus + code_vs);
    u32_(&c, mem.bus + u_vs);
    u16_(&c, 0); /* coordinate shader */
    u8_(&c, 1);
    u8_(&c, 8);
    u32_(&c, mem.bus + code_cs);
    u32_(&c, mem.bus + u_cs);
    u32_(&c, mem.bus + OFF_VERTICES); /* attribute 0: vec2 float */
    u8_(&c, 7);                       /* bytes - 1 */
    u8_(&c, 8);                       /* stride */
    u8_(&c, 0);                       /* VPM offset, vertex shader */
    u8_(&c, 0);                       /* VPM offset, coordinate shader */
    gpu_write(OFF_SHADER_REC, c.b, c.n);

    /* The binning list (Mesa's for this draw, with the kernel's binner set-up). */
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
    u32_(&c, (mem.bus + OFF_SHADER_REC) | 1); /* one attribute array */
    u8_(&c, PKT_GL_ARRAY_PRIMITIVE);
    u8_(&c, 4); /* triangles */
    u32_(&c, 3);
    u32_(&c, 0);
    u8_(&c, PKT_INCREMENT_SEMAPHORE);
    u8_(&c, PKT_FLUSH);
    gpu_write(OFF_BCL, c.b, c.n);

    gpu_fill(OFF_FB, 0x55555555, 128 * 128);
    rlen = build_rcl(OFF_FB, 128, 128, clear, 1);

    wr(V3D_BPOA, mem.bus + OFF_OVERFLOW);
    wr(V3D_BPOS, OVERFLOW_SIZE);
    wr(V3D_BFC, 1);
    wr(V3D_RFC, 1);
    flush_caches();
    run_list(0, OFF_BCL, c.n);
    if (wait_count(V3D_BFC, 1, "binning")) {
        reset_threads();
        failures++;
        return;
    }
    run_list(1, rcl_off, rlen);
    if (wait_count(V3D_RFC, 1, "render")) {
        reset_threads();
        failures++;
        return;
    }
    printf("  pixel (64,64) %08x, pixel (2,2) %08x\n", gpu_word(OFF_FB + (64 * 128 + 64) * 4),
           gpu_word(OFF_FB + (2 * 128 + 2) * 4));
    snprintf(path, sizeof path, "%s/triangle.ppm", outdir);
    write_ppm(path, OFF_FB, 128, 128);
    if (ref) {
        long d = compare_ppm(ref, OFF_FB, 128, 128);
        if (d >= 0)
            printf("  %ld of %d pixels differ from %s\n", d, 128 * 128, ref);
        check(d == 0, "the same picture as Mesa's");
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
    const char *ref = NULL, *outdir = ".";
    int all = 1, k, want[5] = {0, 0, 0, 0, 0};
    static const char *names[] = {"ident", "clear", "triangle", "stride", "clearseq"};

    for (k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--power=qpu"))
            power = PLAT_POWER_QPU;
        else if (!strcmp(argv[k], "--variant=mesa"))
            mesa_order = 1;
        else if (!strcmp(argv[k], "--rotate"))
            rotate = 1;
        else if (!strcmp(argv[k], "--power=domain"))
            power = PLAT_POWER_DOMAIN;
        else if (!strncmp(argv[k], "--ref=", 6))
            ref = argv[k] + 6;
        else if (!strncmp(argv[k], "--out=", 6))
            outdir = argv[k] + 6;
        else {
            int t;
            for (t = 0; t < 5; t++)
                if (!strcmp(argv[k], names[t]))
                    break;
            if (t == 5) {
                fprintf(stderr, "usage: %s [--power=domain|qpu] [--variant=mesa] [--rotate] [--ref=FILE.ppm] [--out=DIR] [ident|clear|triangle|stride|clearseq]...\n",
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

    /* As Linux's vc4_v3d_init_hw: no VPM kept for user programs; interrupts
     * off (completion is polled, as under Emu68) and cleared. */
    wr(V3D_VPMBASE, 0);
    wr(V3D_INTDIS, 0xf);
    wr(V3D_INTCTL, 0xf);

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

    plat_free(&mem);
    plat_close();
    printf("%s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
