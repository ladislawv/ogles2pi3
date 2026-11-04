/*
 * vc4core's texture test (docs/PI3_PLAN.md §3.1, P3): tiling.c's layouts
 * under the TMU. The probe's textured quad (Mesa vc4's shaders for
 * tests/pi3/ref/mesa_texture.c) with
 *   lt  its 16x16 texture, LT by the TMU's rule: Mesa's picture pixel for
 *       pixel;
 *   t   a 64x64 texture, T by the rule, each texel holding its coordinates
 *       (R = 4x, G = 4y): every pixel of the quad must decode to the texel
 *       NEAREST sampling puts there, within one.
 *   sudo ./vc4_texture [--ref=mesa_texture.ppm] [--out=DIR]
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "vc4core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mesa vc4's code for mesa_texture.c, as tests/pi3_probe has it: the
 * fragment shader is threaded and reads 2 varyings and its TMU0
 * configuration (P0, P1) from the uniform stream. */
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

#define W 128
#define H 128
#define TEX_TYPE_RGBA8888 0

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

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

/* The quad, NDC -half..half, sampling the texture whose P0, P1 are given. */
static int draw(struct vc4_job *job, uint32_t p0, uint32_t p1, float half)
{
    const float quad[] = {-half, -half, 0.0f, 0.0f, half, -half, 1.0f, 0.0f,
                          -half, half,  0.0f, 1.0f, half, half,  1.0f, 1.0f};
    uint32_t vp[4], fs_unif[2], v[16], code_fs, code_vs, code_cs, u_fs, u_vs, u_cs, verts, rec, k;
    uint8_t *r;
    vp[0] = fbits(1.0f), vp[1] = fbits(64.0f * 16.0f), vp[2] = fbits(64.0f * 16.0f), vp[3] = fbits(0.5f);
    fs_unif[0] = p0;
    fs_unif[1] = p1;
    for (k = 0; k < 16; k++)
        v[k] = fbits(quad[k]);
    code_fs = put_code(job, tex_fs, sizeof tex_fs);
    code_vs = put_code(job, tex_vs, sizeof tex_vs);
    code_cs = put_code(job, tex_cs, sizeof tex_cs);
    u_fs = put_words(job, fs_unif, 2);
    u_vs = put_words(job, vp, 4);
    u_cs = put_words(job, vp, 4);
    verts = put_words(job, v, 16);
    r = vc4_buf_alloc(&job->state, 36 + 16, 16, &rec);
    if (!r)
        return -1;
    vc4_put16(r + 0, 1u << 2); /* clipping; the fragment shader threaded */
    r[2] = 0;
    r[3] = 2;                  /* varyings */
    vc4_put32(r + 4, code_fs);
    vc4_put32(r + 8, u_fs);
    vc4_put16(r + 12, 0);
    r[14] = 3;                 /* vertex shader: attributes 0 and 1 */
    r[15] = 16;
    vc4_put32(r + 16, code_vs);
    vc4_put32(r + 20, u_vs);
    vc4_put16(r + 24, 0);
    r[26] = 1;                 /* coordinate shader: position only */
    r[27] = 8;
    vc4_put32(r + 28, code_cs);
    vc4_put32(r + 32, u_cs);
    vc4_put32(r + 36, verts);  /* position: 8 bytes at 0, stride 16 */
    r[40] = 7;
    r[41] = 16;
    r[42] = 0;
    r[43] = 0;
    vc4_put32(r + 44, verts + 8); /* texture coordinate: 8 bytes at 8 */
    r[48] = 7;
    r[49] = 16;
    r[50] = 8;                 /* its VPM offset in the vertex shader */
    r[51] = 0;

    VC4_EMIT(&job->bcl, CLIP_WINDOW, .clip_window_width_in_pixels = W, .clip_window_height_in_pixels = H);
    VC4_EMIT(&job->bcl, CONFIGURATION_BITS, .enable_forward_facing_primitive = 1,
             .enable_reverse_facing_primitive = 1, .depth_test_function = VC4_COMPARE_FUNC_ALWAYS,
             .early_z_updates_enable = 1);
    VC4_EMIT(&job->bcl, DEPTH_OFFSET, .depth_offset_units = 0);
    VC4_EMIT(&job->bcl, POINT_SIZE, .point_size = 1.0f);
    VC4_EMIT(&job->bcl, LINE_WIDTH, .line_width = 1.0f);
    VC4_EMIT(&job->bcl, CLIPPER_XY_SCALING, .viewport_half_width_in_1_16th_of_pixel = 64.0f * 16.0f,
             .viewport_half_height_in_1_16th_of_pixel = 64.0f * 16.0f);
    VC4_EMIT(&job->bcl, CLIPPER_Z_SCALE_AND_OFFSET, .viewport_z_scale_zc_to_zs = 0.5f,
             .viewport_z_offset_zc_to_zs = 0.5f);
    VC4_EMIT(&job->bcl, VIEWPORT_OFFSET, .viewport_centre_x_coordinate = 64.0f,
             .viewport_centre_y_coordinate = 64.0f);
    VC4_EMIT(&job->bcl, FLAT_SHADE_FLAGS, .flat_shading_flags = 0);
    vc4_gl_shader_state(&job->bcl, rec, 2);
    VC4_EMIT(&job->bcl, VERTEX_ARRAY_PRIMITIVES, .primitive_mode = VC4_PRIM_TRIANGLE_STRIP, .length = 4);
    return 0;
}

/* P0: level 0's address (4 KB units), the type's low 4 bits, no further
 * levels; P1: the type's bit 4, height, width, NEAREST both ways, CLAMP. */
static uint32_t p0_of(uint32_t bus)
{
    return bus | (uint32_t)(TEX_TYPE_RGBA8888 & 15) << 4;
}

static uint32_t p1_of(uint32_t w, uint32_t h)
{
    return (uint32_t)(TEX_TYPE_RGBA8888 >> 4) << 31 | (h & 2047) << 20 | (w & 2047) << 8 | 1u << 7 |
           1u << 4 | 1u << 2 | 1u;
}

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

static int run(struct vc4_job *job, struct vc4_pools *pools, struct vc4_mem *fb, uint32_t p0, uint32_t p1,
               float half)
{
    struct vc4_target t;
    memset(&t, 0, sizeof t);
    t.width = W;
    t.height = H;
    t.color.bus = fb->bus;
    t.color.tiling = VC4_TILING_RASTER;
    t.color.format = VC4_COLOR_RGBA8888;
    t.store = VC4_COLOR;
    t.clear = VC4_COLOR;
    t.clear_color = 0xff400000u;
    vc4_mem_fill(fb, 0, W * H * 4, 0x55555555u);
    if (vc4_job_begin(job, &t, pools) < 0 || draw(job, p0, p1, half) < 0 || vc4_job_run(job, 1000000) < 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *ref = 0;
    struct vc4_pools pools;
    struct vc4_job job;
    struct vc4_mem fb, tex;
    struct vc4_texture_layout lay;
    static uint8_t texels[64 * 64 * 4];
    int k, x, y;

    for (k = 1; k < argc; k++)
        if (!strncmp(argv[k], "--ref=", 6))
            ref = argv[k] + 6;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("vc4core texture: tiling.c's layouts under the TMU\n");
    if (vc4_open() < 0)
        return 2;
    if (vc4_pools_init(&pools, 2, 2) < 0 || vc4_job_init(&job) < 0 || vc4_mem_alloc(&fb, W * H * 4) < 0 ||
        vc4_mem_alloc(&tex, 64u << 10) < 0)
        return 2;

    /* LT: mesa_texture's 16x16 texels. */
    vc4_texture_layout(&lay, 16, 16, 4, 1, 1);
    check(lay.level[0].tiling == VC4_TILING_LT, "16x16 RGBA8 is LT");
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            uint8_t *t = texels + (y * 16 + x) * 4;
            t[0] = (uint8_t)(x * 16);
            t[1] = (uint8_t)(y * 16);
            t[2] = (x + y) & 1 ? 0xff : 0;
            t[3] = 0xff;
        }
    vc4_texture_store(tex.cpu, &lay, 0, 0, texels, 16 * 4);
    if (run(&job, &pools, &fb, p0_of(tex.bus + lay.level[0].offset), p1_of(16, 16), 0.75f) < 0)
        check(0, "the LT job ran");
    else if (ref)
        check(compare(&fb, ref) == 0, "LT 16x16: Mesa's picture, pixel for pixel");

    /* T: 64x64, R = 4x, G = 4y. */
    vc4_texture_layout(&lay, 64, 64, 4, 1, 1);
    check(lay.level[0].tiling == VC4_TILING_T, "64x64 RGBA8 is T");
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            uint8_t *t = texels + (y * 64 + x) * 4;
            t[0] = (uint8_t)(x * 4);
            t[1] = (uint8_t)(y * 4);
            t[2] = 0x80;
            t[3] = 0xff;
        }
    vc4_texture_store(tex.cpu, &lay, 0, 0, texels, 64 * 4);
    if (run(&job, &pools, &fb, p0_of(tex.bus + lay.level[0].offset), p1_of(64, 64), 0.75f) < 0) {
        check(0, "the T job ran");
    } else {
        long bad = 0, n = 0;
        /* The quad covers pixels 16..111 both ways (NDC -0.75..0.75); away
         * from its edges each pixel shows texel ((p + 0.5 - 16) * 64 / 96). */
        for (y = 18; y < 110; y++)
            for (x = 18; x < 110; x++) {
                uint32_t p = vc4_get32(fb.cpu + (uint32_t)(y * W + x) * 4);
                int tx = (int)(p & 0xff) / 4, ty = (int)((p >> 8) & 0xff) / 4;
                int ex = (int)((x + 0.5f - 16) * 64 / 96), ey = (int)((y + 0.5f - 16) * 64 / 96);
                n++;
                if (((p >> 16) & 0xff) != 0x80 || abs(tx - ex) > 1 || abs(ty - ey) > 1) {
                    if (bad < 5)
                        printf("  pixel (%d,%d): %08x, texel (%d,%d), expected about (%d,%d)\n", x, y, p, tx, ty,
                               ex, ey);
                    bad++;
                }
            }
        printf("  T 64x64: %ld of %ld pixels inside the quad wrong\n", bad, n);
        check(bad == 0, "T 64x64: every pixel the texel NEAREST puts there");
    }
    /* Mipmaps: 64x64, all 7 levels (T, T, then LT), each one colour; quads
     * of 64, 32, 16 and 8 pixels take levels 0 to 3 (NEAREST_MIPMAP_NEAREST:
     * the level of the scale the TMU sees, 64 texels over the quad's width). */
    vc4_texture_layout(&lay, 64, 64, 4, 7, 1);
    check(lay.size <= tex.size && lay.level[0].tiling == VC4_TILING_T && lay.level[1].tiling == VC4_TILING_T &&
              lay.level[2].tiling == VC4_TILING_LT,
          "64x64 miptree: levels 0 and 1 T, 2 to 6 LT, in the block");
    {
        int l, lod, bad = 0;
        for (l = 0; l < 7; l++) {
            uint32_t n = lay.level[l].width * lay.level[l].height;
            for (k = 0; k < (int)n; k++) {
                texels[k * 4] = (uint8_t)(l * 32 + 16);
                texels[k * 4 + 1] = (uint8_t)(255 - l * 32);
                texels[k * 4 + 2] = 0x40;
                texels[k * 4 + 3] = 0xff;
            }
            vc4_texture_store(tex.cpu, &lay, (uint32_t)l, 0, texels, lay.level[l].width * 4);
        }
        for (lod = 0; lod < 4; lod++) {
            float half = 0.5f / (float)(1 << lod); /* 64 >> lod pixels wide */
            uint32_t want = 0xff400000u | (uint32_t)(255 - lod * 32) << 8 | (uint32_t)(lod * 32 + 16);
            uint32_t p1 = vc4_tex_p1(VC4_TEX_RGBA8888, 64, 64, VC4_MAG_NEAREST, VC4_MIN_NEAR_MIP_NEAR,
                                     VC4_WRAP_CLAMP, VC4_WRAP_CLAMP);
            uint32_t p0 = vc4_tex_p0(tex.bus + lay.level[0].offset, VC4_TEX_RGBA8888, 6, 0);
            if (run(&job, &pools, &fb, p0, p1, half) < 0) {
                bad++;
                continue;
            }
            for (y = 62; y < 66; y++)
                for (x = 62; x < 66; x++) {
                    uint32_t p = vc4_get32(fb.cpu + (uint32_t)(y * W + x) * 4);
                    if (p != want) {
                        if (bad < 5)
                            printf("  %d pixels wide: (%d,%d) %08x, level %d's colour %08x\n", 64 >> lod, x, y,
                                   p, lod, want);
                        bad++;
                    }
                }
        }
        check(bad == 0, "mipmaps: quads of 64, 32, 16, 8 pixels show levels 0, 1, 2, 3");
    }
    vc4_mem_free(&tex);
    vc4_mem_free(&fb);
    vc4_job_free(&job);
    vc4_pools_free(&pools);
    vc4_close();
    printf("%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
