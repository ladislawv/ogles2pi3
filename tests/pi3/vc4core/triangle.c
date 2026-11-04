/*
 * vc4core's first test (docs/PI3_PLAN.md §3.1, P3): the probe's triangle
 * (tests/pi3_probe: Mesa vc4's shaders for tests/pi3/ref/mesa_triangle.c,
 * the same vertices, uniforms and clear) drawn through vc4core's API, its
 * 128x128 frame compared with Mesa's picture pixel for pixel; then the same
 * job N times (the soak's start: every picture checked, ERRSTAT clean).
 *   sudo ./vc4_triangle [--ref=mesa_triangle.ppm] [--out=DIR] [--repeat=N]
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "vc4core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mesa vc4's code for mesa_triangle.c (Mesa 26.2.2 on the Pi, VC4_DEBUG=qpu),
 * as tests/pi3_probe has it: fragment, vertex and coordinate shaders. */
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

#define W 128
#define H 128

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

/* Code into the state buffer (64-bit instructions, low word first). */
static uint32_t put_code(struct vc4_job *job, const uint64_t *code, uint32_t bytes)
{
    uint32_t bus, k;
    uint8_t *p = vc4_buf_alloc(&job->state, bytes, 8, &bus);
    for (k = 0; p && k < bytes / 8; k++) {
        vc4_put32(p + 8 * k, (uint32_t)code[k]);
        vc4_put32(p + 8 * k + 4, (uint32_t)(code[k] >> 32));
    }
    return bus;
}

static uint32_t put_words(struct vc4_job *job, const uint32_t *w, uint32_t n)
{
    uint32_t bus, k;
    uint8_t *p = vc4_buf_alloc(&job->state, n * 4, 4, &bus);
    for (k = 0; p && k < n; k++)
        vc4_put32(p + 4 * k, w[k]);
    return bus;
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

/* The draw: the probe's run_draw, through vc4core. */
static int draw(struct vc4_job *job)
{
    static const float tri[] = {-0.75f, -0.75f, 0.75f, -0.5f, -0.25f, 0.75f};
    static const uint32_t fs_unif[] = {0xff4080ff}; /* (1, .5, .25, 1) */
    uint32_t vp[4], code_fs, code_vs, code_cs, u_fs, u_vs, u_cs, verts, rec, k;
    uint8_t *r;

    vp[0] = fbits(1.0f), vp[1] = fbits(64.0f * 16.0f), vp[2] = fbits(64.0f * 16.0f), vp[3] = fbits(0.5f);
    code_fs = put_code(job, tri_fs, sizeof tri_fs);
    code_vs = put_code(job, tri_vs, sizeof tri_vs);
    code_cs = put_code(job, tri_cs, sizeof tri_cs);
    u_fs = put_words(job, fs_unif, 1);
    u_vs = put_words(job, vp, 4);
    u_cs = put_words(job, vp, 4);
    {
        uint32_t v[6];
        for (k = 0; k < 6; k++)
            v[k] = fbits(tri[k]);
        verts = put_words(job, v, 6);
    }
    /* The GL shader state record (36 bytes) and one attribute record. */
    r = vc4_buf_alloc(&job->state, 36 + 8, 16, &rec);
    if (!r)
        return -1;
    vc4_put16(r + 0, 1u << 2 | 1u << 0); /* clipping; the fragment shader single-threaded */
    r[2] = 0;
    r[3] = 0; /* varyings */
    vc4_put32(r + 4, code_fs);
    vc4_put32(r + 8, u_fs);
    vc4_put16(r + 12, 0);
    r[14] = 1; /* vertex shader: attribute 0 */
    r[15] = 8; /* its total size */
    vc4_put32(r + 16, code_vs);
    vc4_put32(r + 20, u_vs);
    vc4_put16(r + 24, 0);
    r[26] = 1;
    r[27] = 8;
    vc4_put32(r + 28, code_cs);
    vc4_put32(r + 32, u_cs);
    vc4_put32(r + 36, verts);
    r[40] = 8 - 1; /* bytes minus one */
    r[41] = 8;     /* stride */
    r[42] = 0;     /* VPM offsets */
    r[43] = 0;

    VC4_EMIT(&job->bcl, CLIP_WINDOW, .clip_window_width_in_pixels = W, .clip_window_height_in_pixels = H);
    VC4_EMIT(&job->bcl, CONFIGURATION_BITS, .enable_forward_facing_primitive = 1,
             .enable_reverse_facing_primitive = 1, .depth_test_function = VC4_COMPARE_FUNC_ALWAYS,
             .early_z_updates_enable = 1);
    VC4_EMIT(&job->bcl, DEPTH_OFFSET, .depth_offset_units = 0, .depth_offset_factor = 0);
    VC4_EMIT(&job->bcl, POINT_SIZE, .point_size = 1.0f);
    VC4_EMIT(&job->bcl, LINE_WIDTH, .line_width = 1.0f);
    VC4_EMIT(&job->bcl, CLIPPER_XY_SCALING, .viewport_half_width_in_1_16th_of_pixel = 64.0f * 16.0f,
             .viewport_half_height_in_1_16th_of_pixel = 64.0f * 16.0f);
    VC4_EMIT(&job->bcl, CLIPPER_Z_SCALE_AND_OFFSET, .viewport_z_scale_zc_to_zs = 0.5f,
             .viewport_z_offset_zc_to_zs = 0.5f);
    VC4_EMIT(&job->bcl, VIEWPORT_OFFSET, .viewport_centre_x_coordinate = 64.0f,
             .viewport_centre_y_coordinate = 64.0f);
    VC4_EMIT(&job->bcl, FLAT_SHADE_FLAGS, .flat_shading_flags = 0);
    vc4_gl_shader_state(&job->bcl, rec, 1);
    VC4_EMIT(&job->bcl, VERTEX_ARRAY_PRIMITIVES, .primitive_mode = VC4_PRIM_TRIANGLES, .length = 3,
             .index_of_first_vertex = 0);
    return 0;
}

/* The generated packers against the probe's bytes for the same state. */
static void check_packets(void)
{
    static const uint8_t cfg[] = {96, 0x03, 0x70, 0x02};
    static const uint8_t vpo[] = {103, 0x00, 0x04, 0x00, 0x04}; /* 64 x 16, twice */
    uint8_t b[16];
    struct VC4_CONFIGURATION_BITS c = {0};
    struct VC4_VIEWPORT_OFFSET v = {0};
    c.enable_forward_facing_primitive = 1;
    c.enable_reverse_facing_primitive = 1;
    c.depth_test_function = VC4_COMPARE_FUNC_ALWAYS;
    c.early_z_updates_enable = 1;
    VC4_CONFIGURATION_BITS_pack(b, &c);
    check(!memcmp(b, cfg, sizeof cfg), "CONFIGURATION_BITS packed as the probe's bytes");
    v.viewport_centre_x_coordinate = 64.0f;
    v.viewport_centre_y_coordinate = 64.0f;
    VC4_VIEWPORT_OFFSET_pack(b, &v);
    check(!memcmp(b, vpo, sizeof vpo), "VIEWPORT_OFFSET packed as the probe's bytes");
}

/* The frame against Mesa's picture (a PPM, rows top first: memory's last
 * row first, as the probe compares). The number of differing pixels. */
static long compare(struct vc4_mem *fb, const char *ref)
{
    FILE *f = fopen(ref, "rb");
    int w, h, max, x, y;
    long diff = 0;
    if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || w != W || h != H) {
        printf("  %s: not a %dx%d PPM\n", ref, W, H);
        if (f)
            fclose(f);
        return -1;
    }
    fgetc(f);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) {
            int r = fgetc(f), g = fgetc(f), b = fgetc(f);
            uint32_t p = vc4_get32(fb->cpu + (uint32_t)(y * w + x) * 4);
            if ((uint32_t)r != (p & 0xff) || (uint32_t)g != ((p >> 8) & 0xff) || (uint32_t)b != ((p >> 16) & 0xff)) {
                if (diff < 5)
                    printf("  differs at (%d,%d): ours %08x, Mesa's %02x %02x %02x\n", x, y, p, r, g, b);
                diff++;
            }
        }
    fclose(f);
    return diff;
}

static void write_ppm(struct vc4_mem *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f)
        return;
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (y = H - 1; y >= 0; y--)
        for (x = 0; x < W; x++) {
            uint32_t p = vc4_get32(fb->cpu + (uint32_t)(y * W + x) * 4);
            fputc((int)(p & 0xff), f);
            fputc((int)((p >> 8) & 0xff), f);
            fputc((int)((p >> 16) & 0xff), f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *ref = 0, *out = ".";
    int repeat = 0, k, bad = 0;
    struct vc4_pools pools;
    struct vc4_job job;
    struct vc4_mem fb;
    struct vc4_target t;
    char path[512];

    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "--ref=", 6))
            ref = argv[k] + 6;
        else if (!strncmp(argv[k], "--out=", 6))
            out = argv[k] + 6;
        else if (!strncmp(argv[k], "--repeat=", 9))
            repeat = atoi(argv[k] + 9);
    }
    printf("vc4core triangle: 128x128, Mesa's shaders, vc4core's lists\n");
    check_packets();
    if (vc4_open() < 0)
        return 2;
    if (vc4_pools_init(&pools, 2, 2) < 0 || vc4_job_init(&job) < 0 || vc4_mem_alloc(&fb, W * H * 4) < 0) {
        vc4_close();
        return 2;
    }
    memset(&t, 0, sizeof t);
    t.width = W;
    t.height = H;
    t.color.bus = fb.bus;
    t.color.tiling = VC4_TILING_RASTER;
    t.color.format = VC4_COLOR_RGBA8888;
    t.store = VC4_COLOR;
    t.clear = VC4_COLOR;
    t.clear_color = 0xff400000u; /* (0, 0, .25, 1) */
    for (k = 0; k <= repeat; k++) {
        long d;
        vc4_mem_fill(&fb, 0, W * H * 4, 0x55555555u);
        if (vc4_job_begin(&job, &t, &pools) < 0 || draw(&job) < 0 || vc4_job_run(&job, 1000000) < 0) {
            check(0, "the job ran");
            break;
        }
        if (k == 0) {
            printf("  binning %u us, rendering %u us, ERRSTAT %08x\n", job.bin_us, job.render_us, job.errstat);
            snprintf(path, sizeof path, "%s/vc4core_triangle.ppm", out);
            write_ppm(&fb, path);
        }
        d = ref ? compare(&fb, ref) : 0;
        if (d || job.errstat != 0x1000u) {
            bad++;
            if (bad <= 5)
                printf("  run %d: %ld pixels differ, ERRSTAT %08x\n", k, d, job.errstat);
        }
        if (k == 0)
            check(d == 0, ref ? "the picture is Mesa's, pixel for pixel" : "drawn (no --ref given)");
    }
    if (repeat)
        check(bad == 0, "every repeat the same picture, ERRSTAT clean");
    vc4_mem_free(&fb);
    vc4_job_free(&job);
    vc4_pools_free(&pools);
    vc4_close();
    printf("%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
