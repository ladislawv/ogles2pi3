/*
 * vc4core's draws from .vc4p files (docs/PI3_PLAN.md §3.1, P3): programs
 * compiled on the PC by v3dglslc -g 21, loaded and drawn through vc4p.c
 * (the uniform streams from their recipes, the shader record from their
 * prog data). Cases:
 *   triangle  tests/pi3/ref/shaders/triangle.*: Mesa's picture (the code is
 *             what the Pi's Mesa compiles: §4.1)
 *   texture   tests/pi3/ref/shaders/texture.*, the 16x16 LT texture: Mesa's
 *             picture
 *   depth, blend, indices, indices8, scissor, lines, points, stencil, attrib,
 *   textures (565, LUMINANCE, 4444, 5551, LUMINANCE_ALPHA, ETC1, NPOT),
 *   rgb565 (an RGB565 frame), cube, msaa (4x, against Mesa's 4x pbuffer),
 *   padded (a 130-pixel-wide frame: the depth picture in its left part):
 *             tests/pi3/ref/cases.h, against mesa_cases.c's pictures
 *   jobs, rtt the depth case in two jobs (stores and loads), and through a
 *             T-format image drawn as a texture: Mesa's depth picture
 *   overflow  the binner's overflow memory (--passes=N draws of 163,840
 *             triangles)
 *   speed     GPU-only numbers on a 1024x768 frame (named only: not in the
 *             default list or a soak's)
 *   sudo ./vc4_draw --dir=VC4P_DIR --refdir=PPM_DIR [--soak=ROUNDS [--loud]] [--passes=N] [CASE...]
 * The .vc4p files are named as v3dglslc -o names them
 * (<vertex>-<fragment>.vc4p for these).
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "vc4p.h"
#include "vc4_os.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 128
#define H 128

static int failures;
static const char *dir = ".", *refdir = ".";
static struct vc4_pools pools;
static struct vc4_job job;
static struct vc4_mem fb, tex;

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

/* The frame's format: RGBA8888, or BGR565 (the rgb565 case). */
static int fb565;
/* The msaa case's: drawn upside down (as Mesa draws a window system's
 * buffer, its pbuffer: the samples' positions mirrored), compared with the
 * reference's rows reversed, channels within cmp_tol (Mesa's resolve
 * rounds, the hardware's truncates). */
static int flip_y, cmp_tol;
/* The padded case's: the frame's width and its rows' (the raster store
 * rounds a row up to 4 pixels: PI3_PLAN §2, "Raster stores"). */
static int fb_w = W, fb_stride = W;

/* Pixel (x, y) of the frame as 8-bit R, G, B (565 expanded as Mesa's
 * read-back does: rounded). */
static void pixel(int x, int y, int *r, int *g, int *b)
{
    if (fb565) {
        uint32_t p = vc4_get32(fb.cpu + (uint32_t)(y * fb_stride + (x & ~1)) * 2);
        if (x & 1)
            p >>= 16;
        *r = (int)(((p >> 11) & 31) * 255 + 15) / 31;
        *g = (int)(((p >> 5) & 63) * 255 + 31) / 63;
        *b = (int)((p & 31) * 255 + 15) / 31;
    } else {
        uint32_t p = vc4_get32(fb.cpu + (uint32_t)(y * fb_stride + x) * 4);
        *r = (int)(p & 0xff);
        *g = (int)((p >> 8) & 0xff);
        *b = (int)((p >> 16) & 0xff);
    }
}

static long compare(const char *name)
{
    char path[512];
    FILE *f;
    int w, h, max, x, y;
    long diff = 0;
    snprintf(path, sizeof path, "%s/%s", refdir, name);
    f = fopen(path, "rb");
    if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || w != W || h != H) {
        printf("  %s: not a %dx%d PPM\n", path, W, H);
        if (f)
            fclose(f);
        return -1;
    }
    fgetc(f);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) {
            int r = fgetc(f), g = fgetc(f), b = fgetc(f), pr, pg, pb;
            pixel(x, flip_y ? H - 1 - y : y, &pr, &pg, &pb);
            if (abs(r - pr) > cmp_tol || abs(g - pg) > cmp_tol || abs(b - pb) > cmp_tol) {
                if (diff < 5)
                    printf("  differs at (%d,%d): ours %02x %02x %02x, Mesa's %02x %02x %02x\n", x, y, pr, pg, pb, r,
                           g, b);
                diff++;
            }
        }
    fclose(f);
    return diff;
}

static int load(struct vc4p *p, const char *file, struct vc4p_draw *d)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (vc4p_load(p, path) < 0)
        return -1;
    memset(d, 0, sizeof(*d));
    d->prog = p;
    d->fs = vc4p_find(p, VC4P_STAGE_FS, 0);
    d->vs = vc4p_find(p, VC4P_STAGE_VS, 0);
    d->cs = vc4p_find(p, VC4P_STAGE_CS, 0);
    if (!d->fs || !d->vs || !d->cs) {
        printf("  %s: no triangle variants\n", path);
        return -1;
    }
    d->vp_w = W;
    d->vp_h = H;
    d->depth_far = 1.0f;
    d->cfg.enable_forward_facing_primitive = 1;
    d->cfg.enable_reverse_facing_primitive = 1;
    d->cfg.depth_test_function = VC4_COMPARE_FUNC_ALWAYS;
    d->cfg.early_z_updates_enable = 1;
    return 0;
}

/* One job into the 128x128 RGBA8 frame, cleared to (0, 0, .25, 1). */
static int run(const struct vc4p_draw *d)
{
    struct vc4_target t;
    memset(&t, 0, sizeof t);
    t.width = W;
    t.height = H;
    t.color.bus = fb.bus;
    t.color.tiling = VC4_TILING_RASTER;
    t.color.format = VC4_COLOR_RGBA8888;
    t.store = VC4_COLOR;
    t.clear = VC4_COLOR;
    t.clear_color = 0xff400000u;
    vc4_mem_fill(&fb, 0, W * H * 4, 0x55555555u);
    if (vc4_job_begin(&job, &t, &pools) < 0 || vc4p_emit(&job, d, W, H) < 0 || vc4_job_run(&job, 1000000) < 0)
        return -1;
    if (job.errstat != 0x1000u)
        printf("  ERRSTAT %08x\n", job.errstat);
    return 0;
}

static void case_triangle(void)
{
    static const float tri[] = {-0.75f, -0.75f, 0.75f, -0.5f, -0.25f, 0.75f};
    struct vc4p p;
    struct vc4p_draw d;
    printf("triangle (S1's triangle.vert + triangle.frag)\n");
    if (load(&p, "triangle-triangle.vc4p", &d) < 0) {
        failures++;
        return;
    }
    d.nattrs = 1;
    d.attr[0].data = tri, d.attr[0].bytes = 8, d.attr[0].stride = 8, d.attr[0].size = sizeof tri;
    d.mode = VC4_PRIM_TRIANGLES;
    d.count = 3;
    if (run(&d) < 0)
        check(0, "the job ran");
    else
        check(compare("mesa_triangle.ppm") == 0, "Mesa's picture, pixel for pixel");
    vc4p_free(&p);
}

static void case_texture(void)
{
    static const float pos[] = {-0.75f, -0.75f, 0.75f, -0.75f, -0.75f, 0.75f, 0.75f, 0.75f};
    static const float uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
    static uint8_t texels[16 * 16 * 4];
    struct vc4_texture_layout lay;
    struct vc4p p;
    struct vc4p_draw d;
    int x, y;
    printf("texture (S1's texture.vert + texture.frag, 16x16 LT)\n");
    if (load(&p, "texture-texture.vc4p", &d) < 0) {
        failures++;
        return;
    }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            uint8_t *t = texels + (y * 16 + x) * 4;
            t[0] = (uint8_t)(x * 16);
            t[1] = (uint8_t)(y * 16);
            t[2] = (x + y) & 1 ? 0xff : 0;
            t[3] = 0xff;
        }
    vc4_texture_layout(&lay, 16, 16, 4, 1, 1);
    vc4_texture_store(tex.cpu, &lay, 0, 0, texels, 16 * 4);
    d.tex[0][0] = vc4_tex_p0(tex.bus + lay.level[0].offset, VC4_TEX_RGBA8888, 0, 0);
    d.tex[0][1] = vc4_tex_p1(VC4_TEX_RGBA8888, 16, 16, VC4_MAG_NEAREST, VC4_MIN_NEAREST, VC4_WRAP_CLAMP,
                             VC4_WRAP_CLAMP);
    d.nattrs = 2;
    d.attr[0].data = pos, d.attr[0].bytes = 8, d.attr[0].stride = 8, d.attr[0].size = sizeof pos;
    d.attr[1].data = uv, d.attr[1].bytes = 8, d.attr[1].stride = 8, d.attr[1].size = sizeof uv;
    d.mode = VC4_PRIM_TRIANGLE_STRIP;
    d.count = 4;
    if (run(&d) < 0)
        check(0, "the job ran");
    else
        check(compare("mesa_texture.ppm") == 0, "Mesa's picture, pixel for pixel");
    vc4p_free(&p);
}

/* ---- the cases of tests/pi3/ref/cases.h, against mesa_cases.c's pictures ---- */

#include "../ref/cases.h"

static uint32_t bits(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

/* One draw with flat.vert or point.vert + flat.frag in colour col: the
 * fragment variant for the primitive class, the depth test and blending. */
static uint32_t stencil_word; /* the stencil cases': nonzero, the stencil test on */
static int msaa_on;           /* the msaa case's: 4x MSAA target, the rasteriser oversampling */
static const uint8_t *flat_indices8; /* the indices8 case's: 8-bit indices instead */
static const int *flat_clip;         /* the scissor case's box */

static int flat(struct vc4p *p, int prim, int depth, int blend, const float *col, const float *v, uint32_t nv,
                uint32_t mode, const unsigned short *idx, uint32_t count)
{
    struct vc4p_draw d;
    uint32_t c[4];
    int k;
    memset(&d, 0, sizeof d);
    for (k = 0; k < 4; k++)
        c[k] = bits(col[k]);
    vc4p_set(p, "color", c, 4);
    d.prog = p;
    /* vc4's key: the depth "enabled" with the stencil test too (vc4_program.c) */
    d.fs = vc4p_find_fs_rt(p, prim, depth || stencil_word, blend, stencil_word != 0,
                           fb565 ? VC4P_FORMAT_B5G6R5_UNORM : VC4P_FORMAT_R8G8B8A8_UNORM);
    d.stencil[0] = stencil_word;
    d.vs = vc4p_find(p, VC4P_STAGE_VS, prim == 2);
    d.cs = vc4p_find(p, VC4P_STAGE_CS, prim == 2);
    if (!d.fs || !d.vs || !d.cs) {
        printf("  no variant for primitive class %d, depth %d, blend %d (compiled with -s?)\n", prim, depth, blend);
        return -1;
    }
    d.vp_w = W;
    d.vp_h = flip_y ? -H : H;
    d.vp_y = flip_y ? H : 0;
    d.depth_far = 1.0f;
    d.cfg.enable_forward_facing_primitive = 1;
    d.cfg.enable_reverse_facing_primitive = 1;
    d.cfg.depth_test_function = depth ? VC4_COMPARE_FUNC_LESS : VC4_COMPARE_FUNC_ALWAYS;
    d.cfg.z_updates_enable = depth ? 1 : 0;
    d.cfg.early_z_updates_enable = 1;
    d.cfg.rasteriser_oversample_mode = msaa_on ? 1 : 0; /* early Z stays off (Mesa: HW-2905) */
    d.nattrs = 1;
    d.attr[0].data = v, d.attr[0].bytes = 12, d.attr[0].stride = 12, d.attr[0].size = nv * 12;
    d.mode = mode;
    d.count = count;
    d.indices = idx;
    d.indices8 = flat_indices8;
    if (flat_clip) {
        d.clip[0] = (uint16_t)flat_clip[0];
        d.clip[1] = (uint16_t)flat_clip[1];
        d.clip[2] = (uint16_t)flat_clip[2];
        d.clip[3] = (uint16_t)flat_clip[3];
    }
    return vc4p_emit(&job, &d, W, H);
}

static int begin_case(int depth)
{
    struct vc4_target t;
    memset(&t, 0, sizeof t);
    t.width = (uint16_t)fb_w;
    t.height = H;
    t.color.bus = fb.bus;
    t.color.tiling = VC4_TILING_RASTER;
    t.color.format = fb565 ? VC4_COLOR_BGR565 : VC4_COLOR_RGBA8888; /* Mesa's: not dithered */
    t.store = VC4_COLOR;
    t.clear = VC4_COLOR | (depth ? VC4_ZS : 0);
    t.clear_color = 0xff400000u;
    t.clear_z = 0xffffffu; /* 1.0 */
    t.msaa = (uint8_t)msaa_on;
    vc4_mem_fill(&fb, 0, W * H * 4, 0x55555555u);
    return vc4_job_begin(&job, &t, &pools);
}

static void end_case(const char *name, int ok)
{
    char ref[64], path[512];
    if (!ok || vc4_job_run(&job, 1000000) < 0) {
        check(0, "the job ran");
        return;
    }
    if (job.errstat != 0x1000u)
        printf("  ERRSTAT %08x\n", job.errstat);
    snprintf(ref, sizeof ref, "mesa_%s.ppm", name);
    snprintf(path, sizeof path, "vc4core_%s.ppm", name);
    {   /* ours too, for a look when they differ */
        FILE *f = fopen(path, "wb");
        int x, y;
        if (f) {
            fprintf(f, "P6 %d %d 255\n", W, H);
            for (y = H - 1; y >= 0; y--)
                for (x = 0; x < W; x++) {
                    int r, g, b;
                    pixel(x, y, &r, &g, &b);
                    fputc(r, f);
                    fputc(g, f);
                    fputc(b, f);
                }
            fclose(f);
        }
    }
    check(compare(ref) == 0, cmp_tol ? "Mesa's picture, each channel within its resolve's rounding (1)"
                                     : "Mesa's picture, pixel for pixel");
}

static int load_flat(struct vc4p *p, const char *file)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    return vc4p_load(p, path);
}

static void case_depth(void)
{
    struct vc4p p;
    int k, ok = 1;
    printf("depth (three triangles, LESS)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(1) < 0) {
        failures++;
        return;
    }
    for (k = 0; k < 3 && ok; k++)
        ok = flat(&p, 0, 1, 0, depth_col[k], depth_tri[k], 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    end_case("depth", ok);
    vc4p_free(&p);
}

static void case_blend(void)
{
    struct vc4p p;
    int ok;
    printf("blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    ok = flat(&p, 0, 0, 0, blend_col[0], blend_quad, 4, VC4_PRIM_TRIANGLE_STRIP, 0, 4) == 0 &&
         flat(&p, 0, 0, 1, blend_col[1], blend_tri, 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    end_case("blend", ok);
    vc4p_free(&p);
}

static void case_indices(void)
{
    struct vc4p p;
    int ok;
    printf("indices (16-bit, a triangle list)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    ok = flat(&p, 0, 0, 0, idx_col, idx_vtx, 6, VC4_PRIM_TRIANGLES, idx_list, 9) == 0;
    end_case("indices", ok);
    vc4p_free(&p);
}

/* indices8: the same with 8-bit indices: the same picture. */
static void case_indices8(void)
{
    static uint8_t idx8[9];
    struct vc4p p;
    int k, ok;
    printf("indices8 (8-bit, a triangle list)\n");
    for (k = 0; k < 9; k++)
        idx8[k] = (uint8_t)idx_list[k];
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    flat_indices8 = idx8;
    ok = flat(&p, 0, 0, 0, idx_col, idx_vtx, 6, VC4_PRIM_TRIANGLES, 0, 9) == 0;
    flat_indices8 = 0;
    end_case("indices", ok);
    vc4p_free(&p);
}

/* scissor: the blend case's quad inside the scissor box (the clip window). */
static void case_scissor(void)
{
    struct vc4p p;
    int ok;
    printf("scissor (the clip window)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    flat_clip = sciss_box;
    ok = flat(&p, 0, 0, 0, blend_col[0], blend_quad, 4, VC4_PRIM_TRIANGLE_STRIP, 0, 4) == 0;
    flat_clip = 0;
    end_case("scissor", ok);
    vc4p_free(&p);
}

static void case_lines(void)
{
    struct vc4p p;
    int ok;
    printf("lines (three, width 1)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    ok = flat(&p, 1, 0, 0, line_col, line_vtx, 6, VC4_PRIM_LINES, 0, 6) == 0;
    end_case("lines", ok);
    vc4p_free(&p);
}

static void case_points(void)
{
    struct vc4p p;
    int ok;
    printf("points (five, 8 pixels)\n");
    if (load_flat(&p, "point-flat.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    ok = flat(&p, 2, 0, 0, point_col, point_vtx, 5, VC4_PRIM_POINTS, 0, 5) == 0;
    end_case("points", ok);
    vc4p_free(&p);
}

/* stencil: the triangle writes 1 (ALWAYS, REPLACE), the quad is drawn where
 * the stencil is 1 (EQUAL, KEEP); the tile buffer's stencil cleared to 0. */
static void case_stencil(void)
{
    struct vc4p p;
    int ok;
    printf("stencil (ALWAYS/REPLACE, then EQUAL 1)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0 || begin_case(1) < 0) {
        failures++;
        return;
    }
    stencil_word = vc4p_stencil_word(0x0207 /* GL_ALWAYS */, 1, 0xff, VC4P_STENCIL_KEEP, VC4P_STENCIL_KEEP,
                                     VC4P_STENCIL_REPLACE, 0xff);
    ok = flat(&p, 0, 0, 0, sten_col[0], sten_tri, 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    stencil_word = vc4p_stencil_word(0x0202 /* GL_EQUAL */, 1, 0xff, VC4P_STENCIL_KEEP, VC4P_STENCIL_KEEP,
                                     VC4P_STENCIL_KEEP, 0xff);
    ok = ok && flat(&p, 0, 0, 0, sten_col[1], sten_quad, 4, VC4_PRIM_TRIANGLE_STRIP, 0, 4) == 0;
    stencil_word = 0;
    end_case("stencil", ok);
    vc4p_free(&p);
}

/* A draw of a two-input program (pos at slot 0, the other at slot 1) whose
 * vertex variants are the ones for attribute 1's format and whose fragment
 * variant is the one for sampler 0's format (0: the plain triangle one). */
static int two_inputs(struct vc4p *p, uint32_t attr1_format, uint32_t tex0_format, const void *a0, uint32_t b0,
                      uint32_t s0, const void *a1, uint32_t b1, uint32_t s1, uint32_t size, uint32_t mode,
                      uint32_t count, struct vc4p_draw *d)
{
    memset(d, 0, sizeof(*d));
    d->prog = p;
    d->vs = vc4p_find_attr(p, VC4P_STAGE_VS, 1, attr1_format);
    d->cs = vc4p_find_attr(p, VC4P_STAGE_CS, 1, attr1_format);
    d->fs = tex0_format ? vc4p_find_tex(p, 0, tex0_format) : vc4p_find_fs(p, 0, 0, 0, 0);
    if (!d->fs || !d->vs || !d->cs) {
        int k;
        printf("  no variant for attribute 1 format %u, sampler 0 format %u; the file has:\n", attr1_format,
               tex0_format);
        for (k = 0; k < p->nvariants; k++)
            printf("    stage %u: attribute 1 %u, sampler 0 %u\n", p->v[k].stage, p->v[k].attr_formats[1],
                   p->v[k].tex_format[0]);
        return -1;
    }
    d->vp_w = W;
    d->vp_h = H;
    d->depth_far = 1.0f;
    d->cfg.enable_forward_facing_primitive = 1;
    d->cfg.enable_reverse_facing_primitive = 1;
    d->cfg.depth_test_function = VC4_COMPARE_FUNC_ALWAYS;
    d->cfg.early_z_updates_enable = 1;
    d->nattrs = 2;
    d->attr[0].data = a0, d->attr[0].bytes = b0, d->attr[0].stride = s0, d->attr[0].size = size;
    d->attr[1].data = a1, d->attr[1].bytes = b1, d->attr[1].stride = s1, d->attr[1].size = size;
    d->mode = mode;
    d->count = count;
    return 0;
}

/* attrib: colours as four normalized unsigned bytes (VC4 converts them in
 * the vertex shader: its own variant). */
static void case_attrib(void)
{
    struct vc4p p;
    struct vc4p_draw d;
    int ok;
    printf("attrib (colours as normalized unsigned bytes)\n");
    if (load_flat(&p, "color-color.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    ok = two_inputs(&p, VC4P_FORMAT_R8G8B8A8_UNORM, 0, attr_pos, 12, 12, attr_col, 4, 4, sizeof attr_pos,
                    VC4_PRIM_TRIANGLES, 3, &d) == 0;
    d.attr[1].size = sizeof attr_col;
    ok = ok && vc4p_emit(&job, &d, W, H) == 0;
    end_case("attrib", ok);
    vc4p_free(&p);
}

/* tex565, texlum: the textured quad with a 16x16 texture of that format (LT:
 * two utiles wide), NEAREST, CLAMP. */
/* The texture cases: name, the pipe_format the variant is keyed on, the
 * hardware type, bytes a texel (a block for ETC1), the size in texels. The
 * stored texels are GL's: L8 as ALPHA (the variant swizzles WWW1), RGBA4444
 * read by the hardware in GL's packing (R in the top bits; this Mesa keeps
 * GL_RGBA4 as RGBA8, so its variant is the RGBA8 one), RGBA5551 as GL's
 * (A1B5G5R5), LUMINANCE_ALPHA as LUMALPHA, ETC1 as 4x4 blocks of 8 bytes
 * (Mesa stores them unchanged, sets P1's ETC flip), NPOT 13x7 RGBA8. */
struct texfmt
{
    const char *name;
    uint32_t key, type, cpp, w, h;
};
static const struct texfmt texfmts[] = {
    {"tex565", VC4P_FORMAT_B5G6R5_UNORM, VC4_TEX_RGB565, 2, 16, 16},
    {"texlum", VC4P_FORMAT_L8_UNORM, VC4_TEX_ALPHA, 1, 16, 16},
    {"tex4444", VC4P_FORMAT_R8G8B8A8_UNORM, VC4_TEX_RGBA4444, 2, 16, 16},
    {"tex5551", VC4P_FORMAT_A1B5G5R5_UNORM, VC4_TEX_RGBA5551, 2, 16, 16},
    {"texla", VC4P_FORMAT_L8A8_UNORM, VC4_TEX_LUMALPHA, 2, 16, 16},
    {"etc1", VC4P_FORMAT_ETC1_RGB8, VC4_TEX_ETC1, 8, 16, 16},
    {"npot", VC4P_FORMAT_R8G8B8A8_UNORM, VC4_TEX_RGBA8888, 4, NPOT_W, NPOT_H},
};
#define NTEXFMTS (int)(sizeof texfmts / sizeof texfmts[0])

static void tex_case(const struct texfmt *t)
{
    static uint8_t texels[16 * 16 * 4];
    struct vc4_texture_layout lay;
    struct vc4p p;
    struct vc4p_draw d;
    int etc = t->type == VC4_TEX_ETC1;
    uint32_t sw = etc ? t->w / 4 : t->w, sh = etc ? t->h / 4 : t->h; /* stored: texels or blocks */
    int x, y, ok;
    printf("%s (%ux%u)\n", t->name, t->w, t->h);
    for (y = 0; y < (int)sh; y++)
        for (x = 0; x < (int)sw; x++) {
            uint8_t *o = texels + ((uint32_t)y * sw + (uint32_t)x) * t->cpp;
            int packed16 = t->type == VC4_TEX_RGB565 || t->type == VC4_TEX_RGBA4444 || t->type == VC4_TEX_RGBA5551;
            if (packed16) { /* GL's 16-bit texels, little-endian as the GPU reads them */
                unsigned short v = t->type == VC4_TEX_RGB565     ? tex565_texel(x, y)
                                 : t->type == VC4_TEX_RGBA4444 ? tex4444_texel(x, y)
                                                               : tex5551_texel(x, y);
                o[0] = (uint8_t)v;
                o[1] = (uint8_t)(v >> 8);
            } else if (!strcmp(t->name, "texlum"))
                o[0] = texlum_texel(x, y);
            else if (!strcmp(t->name, "texla"))
                texla_texel(x, y, o);
            else if (etc)
                etc1_block(x, y, o);
            else
                npot_texel(x, y, o);
        }
    if (load_flat(&p, "texture-texture.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    vc4_texture_layout(&lay, sw, sh, t->cpp, 1, 1);
    vc4_texture_store(tex.cpu, &lay, 0, 0, texels, sw * t->cpp);
    ok = two_inputs(&p, VC4P_FORMAT_R32G32_FLOAT, t->key, tq_pos, 8, 8, tq_uv, 8, 8, sizeof tq_pos,
                    VC4_PRIM_TRIANGLE_STRIP, 4, &d) == 0;
    d.tex[0][0] = vc4_tex_p0(tex.bus + lay.level[0].offset, t->type, 0, 0);
    d.tex[0][1] = vc4_tex_p1(t->type, t->w, t->h, VC4_MAG_NEAREST, VC4_MIN_NEAREST, VC4_WRAP_CLAMP, VC4_WRAP_CLAMP);
    ok = ok && vc4p_emit(&job, &d, W, H) == 0;
    end_case(t->name, ok);
    vc4p_free(&p);
}

/* rgb565: the blend case into an RGB565 frame (stored by the tile buffer
 * without dithering, as Mesa's), its own fragment variants. */
static void case_rgb565(void)
{
    struct vc4p p;
    int ok;
    printf("rgb565 (the blend case into an RGB565 frame)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0) {
        failures++;
        return;
    }
    fb565 = 1;
    ok = begin_case(0) == 0 &&
         flat(&p, 0, 0, 0, blend_col[0], blend_quad, 4, VC4_PRIM_TRIANGLE_STRIP, 0, 4) == 0 &&
         flat(&p, 0, 0, 1, blend_col[1], blend_tri, 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    end_case("rgb565", ok);
    fb565 = 0;
    vc4p_free(&p);
}

/* cube: an 8x8 cube map (six faces, each a whole miptree a page apart), six
 * quads each sampling one face; position and direction interleaved. */
static void case_cube(void)
{
    static float cv[36 * 6];
    static uint8_t face[8 * 8 * 4];
    struct vc4_texture_layout lay;
    struct vc4p p;
    struct vc4p_draw d;
    int f, k, ok;
    printf("cube (an 8x8 cube map, a face a quad)\n");
    cube_vertices(cv);
    if (load_flat(&p, "cube-cube.vc4p") < 0 || begin_case(0) < 0) {
        failures++;
        return;
    }
    vc4_texture_layout(&lay, 8, 8, 4, 1, 6);
    for (f = 0; f < 6; f++) {
        for (k = 0; k < 64; k++)
            memcpy(face + k * 4, cube_face_col[f], 4);
        vc4_texture_store(tex.cpu, &lay, 0, (uint32_t)f, face, 8 * 4);
    }
    ok = two_inputs(&p, VC4P_FORMAT_R32G32B32_FLOAT, 0, cv, 12, 24, cv + 3, 12, 24, 35 * 24 + 12,
                    VC4_PRIM_TRIANGLES, 36, &d) == 0;
    d.tex[0][0] = vc4_tex_p0(tex.bus + lay.level[0].offset, VC4_TEX_RGBA8888, 0, 1);
    d.tex[0][1] = vc4_tex_p1(VC4_TEX_RGBA8888, 8, 8, VC4_MAG_NEAREST, VC4_MIN_NEAREST, VC4_WRAP_CLAMP, VC4_WRAP_CLAMP);
    d.tex[0][2] = 1u << 30 | lay.face_stride;
    ok = ok && vc4p_emit(&job, &d, W, H) == 0;
    end_case("cube", ok);
    vc4p_free(&p);
}

/* padded: the depth case in a frame 130 pixels wide (its rows 132 pixels
 * apart), drawn into its left 128 columns: those must be Mesa's depth
 * picture, columns 128 and 129 the clear colour; what the store writes into
 * the rows' padding (columns 130, 131) is reported. */
static void case_padded(void)
{
    struct vc4p p;
    int k, ok, x, y;
    long wrong = 0, pad_written = 0;
    printf("padded (the depth case in a 130-pixel-wide raster frame, rows of 132)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0) {
        failures++;
        return;
    }
    fb_w = 130;
    fb_stride = 132;
    vc4_mem_fill(&fb, 0, 132 * H * 4, 0x55555555u);
    ok = begin_case(1) == 0;
    vc4_mem_fill(&fb, 0, 132 * H * 4, 0x55555555u); /* begin_case filled 128 x 128 only */
    for (k = 0; k < 3 && ok; k++)
        ok = flat(&p, 0, 1, 0, depth_col[k], depth_tri[k], 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    if (ok && vc4_job_run(&job, 1000000) == 0) {
        for (y = 0; y < H; y++) {
            for (x = 128; x < 130; x++)
                if (vc4_get32(fb.cpu + (uint32_t)(y * 132 + x) * 4) != 0xff400000u)
                    wrong++;
            for (x = 130; x < 132; x++)
                if (vc4_get32(fb.cpu + (uint32_t)(y * 132 + x) * 4) != 0x55555555u)
                    pad_written++;
        }
        printf("  %ld of %d padding pixels written by the store\n", pad_written, 2 * H);
        check(wrong == 0, "columns 128 and 129 the clear colour");
        check(compare("mesa_depth.ppm") == 0, "columns 0 to 127: Mesa's depth picture, pixel for pixel");
    } else
        check(0, "the job ran");
    fb_w = W;
    fb_stride = W;
    vc4p_free(&p);
}

/* msaa: the depth case in 4x MSAA (32x32 tiles), stored resolved; Mesa's
 * picture from a 4x multisampled pbuffer. The fragment variant is the
 * single-sample one: without blending vc4's MSAA key changes no code (only
 * blending that reads the destination writes per sample). */
static void case_msaa(void)
{
    struct vc4p p;
    int k, ok = 1;
    printf("msaa (the depth case, 4x)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0) {
        failures++;
        return;
    }
    msaa_on = 1;
    flip_y = 1;
    cmp_tol = 1;
    ok = begin_case(1) == 0;
    for (k = 0; k < 3 && ok; k++)
        ok = flat(&p, 0, 1, 0, depth_col[k], depth_tri[k], 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    msaa_on = 0;
    end_case("msaa", ok);
    flip_y = 0;
    cmp_tol = 0;
    vc4p_free(&p);
}

/* ---- across jobs: stores and loads (no picture of their own: Mesa's
 * depth picture, drawn in two jobs or through a texture) ---- */

static struct vc4_mem zsbuf, rtt; /* a T-format depth/stencil buffer, a T-format colour image */

static int begin_target(const struct vc4_target *t)
{
    return vc4_job_begin(&job, t, &pools);
}

/* jobs: the depth case's first triangle in one job (colour and depth
 * stored), the other two in a second that loads both. */
static void case_jobs(void)
{
    struct vc4p p;
    struct vc4_target t;
    int k, ok;
    printf("jobs (the depth case in two jobs: colour (raster) and depth (T) stored, then loaded)\n");
    if (load_flat(&p, "flat-flat.vc4p") < 0) {
        failures++;
        return;
    }
    memset(&t, 0, sizeof t);
    t.width = W;
    t.height = H;
    t.color.bus = fb.bus;
    t.color.tiling = VC4_TILING_RASTER;
    t.color.format = VC4_COLOR_RGBA8888;
    t.zs.bus = zsbuf.bus;
    t.zs.tiling = VC4_TILING_T;
    t.store = VC4_COLOR | VC4_ZS;
    t.clear = VC4_COLOR | VC4_ZS;
    t.clear_color = 0xff400000u;
    t.clear_z = 0xffffffu;
    vc4_mem_fill(&fb, 0, W * H * 4, 0x55555555u);
    vc4_mem_fill(&zsbuf, 0, W * H * 4, 0x12345678u);
    ok = begin_target(&t) == 0 && flat(&p, 0, 1, 0, depth_col[0], depth_tri[0], 3, VC4_PRIM_TRIANGLES, 0, 3) == 0 &&
         vc4_job_run(&job, 1000000) == 0;
    if (ok && job.errstat != 0x1000u)
        printf("  first job: ERRSTAT %08x\n", job.errstat);
    t.clear = 0;
    t.load = VC4_COLOR | VC4_ZS;
    t.store = VC4_COLOR;
    ok = ok && begin_target(&t) == 0;
    for (k = 1; k < 3 && ok; k++)
        ok = flat(&p, 0, 1, 0, depth_col[k], depth_tri[k], 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    end_case("depth", ok);
    vc4p_free(&p);
}

/* rtt: the depth case rendered into a 128x128 T-format colour image, then
 * that image drawn as a texture over the whole frame, a texel a pixel. */
static void case_rtt(void)
{
    static const float full_pos[4 * 2] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const float full_uv[4 * 2] = {0, 0, 1, 0, 0, 1, 1, 1};
    struct vc4_texture_layout lay;
    struct vc4p p, tp;
    struct vc4_target t;
    struct vc4p_draw d;
    int k, ok;
    printf("rtt (the depth case into a T-format image, then drawn from it as a texture)\n");
    vc4_texture_layout(&lay, W, H, 4, 1, 1);
    if (lay.level[0].tiling != VC4_TILING_T || lay.size > rtt.size) {
        printf("  a %dx%d RGBA8 texture is not T, or larger than %u bytes\n", W, H, rtt.size);
        failures++;
        return;
    }
    if (load_flat(&p, "flat-flat.vc4p") < 0 || load_flat(&tp, "texture-texture.vc4p") < 0) {
        failures++;
        return;
    }
    memset(&t, 0, sizeof t);
    t.width = W;
    t.height = H;
    t.color.bus = rtt.bus + lay.level[0].offset;
    t.color.tiling = VC4_TILING_T;
    t.color.format = VC4_COLOR_RGBA8888;
    t.store = VC4_COLOR;
    t.clear = VC4_COLOR | VC4_ZS;
    t.clear_color = 0xff400000u;
    t.clear_z = 0xffffffu;
    vc4_mem_fill(&rtt, 0, lay.size, 0x55555555u);
    ok = begin_target(&t) == 0;
    for (k = 0; k < 3 && ok; k++)
        ok = flat(&p, 0, 1, 0, depth_col[k], depth_tri[k], 3, VC4_PRIM_TRIANGLES, 0, 3) == 0;
    ok = ok && vc4_job_run(&job, 1000000) == 0;
    if (ok && job.errstat != 0x1000u)
        printf("  first job: ERRSTAT %08x\n", job.errstat);
    ok = ok && begin_case(0) == 0 &&
         two_inputs(&tp, VC4P_FORMAT_R32G32_FLOAT, VC4P_FORMAT_R8G8B8A8_UNORM, full_pos, 8, 8, full_uv, 8, 8,
                    sizeof full_pos, VC4_PRIM_TRIANGLE_STRIP, 4, &d) == 0;
    d.tex[0][0] = vc4_tex_p0(rtt.bus + lay.level[0].offset, VC4_TEX_RGBA8888, 0, 0);
    d.tex[0][1] = vc4_tex_p1(VC4_TEX_RGBA8888, W, H, VC4_MAG_NEAREST, VC4_MIN_NEAREST, VC4_WRAP_CLAMP, VC4_WRAP_CLAMP);
    ok = ok && vc4p_emit(&job, &d, W, H) == 0;
    end_case("depth", ok);
    vc4p_free(&tp);
    vc4p_free(&p);
}

/* ---- speed: GPU-only numbers (PI3_PLAN §6.3), not a pass/fail case ---- */

#define SW 1024
#define SH 768

/* A draw into the SW x SH frame: flat (col) or textured (tp, the bound
 * texture in tex_words), its own job. */
static int speed_draw(struct vc4_job *j, struct vc4p *p, const struct vc4p_variant *fs, const float *col,
                      const void *pos, uint32_t pos_bytes, uint32_t nv, const void *uv, uint32_t mode,
                      const uint16_t *idx, uint32_t count, const uint32_t *tex_words)
{
    struct vc4p_draw d;
    memset(&d, 0, sizeof d);
    if (col) {
        uint32_t c[4];
        int k;
        for (k = 0; k < 4; k++)
            c[k] = bits(col[k]);
        vc4p_set(p, "color", c, 4);
    }
    d.prog = p;
    d.fs = fs;
    d.vs = uv ? vc4p_find_attr(p, VC4P_STAGE_VS, 1, VC4P_FORMAT_R32G32_FLOAT) : vc4p_find(p, VC4P_STAGE_VS, 0);
    d.cs = uv ? vc4p_find_attr(p, VC4P_STAGE_CS, 1, VC4P_FORMAT_R32G32_FLOAT) : vc4p_find(p, VC4P_STAGE_CS, 0);
    if (!d.fs || !d.vs || !d.cs)
        return -1;
    d.vp_w = SW;
    d.vp_h = SH;
    d.depth_far = 1.0f;
    d.cfg.enable_forward_facing_primitive = 1;
    d.cfg.enable_reverse_facing_primitive = 1;
    d.cfg.depth_test_function = VC4_COMPARE_FUNC_ALWAYS;
    d.cfg.early_z_updates_enable = 1;
    d.nattrs = uv ? 2 : 1;
    d.attr[0].data = pos, d.attr[0].bytes = pos_bytes, d.attr[0].stride = pos_bytes, d.attr[0].size = nv * pos_bytes;
    if (uv)
        d.attr[1].data = uv, d.attr[1].bytes = 8, d.attr[1].stride = 8, d.attr[1].size = nv * 8;
    if (tex_words)
        memcpy(d.tex[0], tex_words, 3 * sizeof(uint32_t));
    d.mode = mode;
    d.count = count;
    d.indices = idx;
    return vc4p_emit(j, &d, SW, SH);
}

static void case_speed(void)
{
    static const float full3[4 * 3] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    static const float full2[4 * 2] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const float uv[4 * 2] = {0, 0, 4, 0, 0, 3, 4, 3}; /* the 256x256 texture repeated */
    static const float col[4] = {0.2f, 0.6f, 0.9f, 0.5f};
    enum { GRID = 100, REPEAT = 10, QUADS = 20 };
    static float grid[(GRID + 1) * (GRID + 1) * 3];
    static uint16_t gidx[GRID * GRID * 6];
    struct vc4_pools sp;
    struct vc4_job sj;
    struct vc4_mem sfb, stex;
    struct vc4_target t;
    struct vc4p p, tp;
    struct vc4_texture_layout lay;
    uint8_t *texels;
    uint32_t tw[3], i;
    int k, x, y, ok;
    uint64_t t0;
    printf("speed (a %dx%d RGBA8 frame; GPU times from the job)\n", SW, SH);
    if (vc4_pools_init(&sp, SW / 64, SH / 64) < 0 || vc4_job_init(&sj) < 0 || vc4_mem_alloc(&sfb, SW * SH * 4) < 0 ||
        load_flat(&p, "flat-flat.vc4p") < 0 || load_flat(&tp, "texture-texture.vc4p") < 0) {
        failures++;
        return;
    }
    memset(&t, 0, sizeof t);
    t.width = SW;
    t.height = SH;
    t.color.bus = sfb.bus;
    t.color.tiling = VC4_TILING_RASTER;
    t.color.format = VC4_COLOR_RGBA8888;
    t.store = VC4_COLOR;
    t.clear = VC4_COLOR;
    t.clear_color = 0xff400000u;

    /* the clear and store alone */
    ok = vc4_job_begin(&sj, &t, &sp) == 0 && vc4_job_run(&sj, 2000000) == 0;
    printf("  clear and store only: render %u us (%.0f Mpixel/s stored)\n", sj.render_us,
           ok ? (double)SW * SH / sj.render_us : 0.0);

    /* fill: QUADS full-frame quads, flat, then blended */
    for (k = 0; k < 2; k++) {
        ok = vc4_job_begin(&sj, &t, &sp) == 0;
        for (i = 0; i < QUADS && ok; i++)
            ok = speed_draw(&sj, &p, vc4p_find_fs(&p, 0, 0, k, 0), col, full3, 12, 4, 0, VC4_PRIM_TRIANGLE_STRIP, 0, 4,
                            0) == 0;
        ok = ok && vc4_job_run(&sj, 5000000) == 0;
        printf("  %d full-frame quads, %s: binning %u us, render %u us: %.0f Mpixel/s\n", QUADS,
               k ? "blended" : "flat", sj.bin_us, sj.render_us, ok ? (double)QUADS * SW * SH / sj.render_us : 0.0);
    }

    /* textured fill: a 256x256 RGBA8 T texture, NEAREST, repeated */
    if (vc4_mem_alloc(&stex, 512u << 10) == 0) {
        texels = malloc(256 * 256 * 4);
        for (y = 0; y < 256; y++)
            for (x = 0; x < 256; x++) {
                uint8_t *o = texels + (y * 256 + x) * 4;
                o[0] = (uint8_t)x, o[1] = (uint8_t)y, o[2] = (uint8_t)(x ^ y), o[3] = 255;
            }
        vc4_texture_layout(&lay, 256, 256, 4, 1, 1);
        t0 = vc4_os_time_us();
        for (k = 0; k < 10; k++)
            vc4_texture_store(stex.cpu, &lay, 0, 0, texels, 256 * 4);
        printf("  upload: a 256x256 RGBA8 texture tiled (T) into GPU memory by the CPU: %.1f MB/s\n",
               10.0 * 256 * 256 * 4 / (double)(vc4_os_time_us() - t0));
        tw[0] = vc4_tex_p0(stex.bus + lay.level[0].offset, VC4_TEX_RGBA8888, 0, 0);
        tw[1] = vc4_tex_p1(VC4_TEX_RGBA8888, 256, 256, VC4_MAG_NEAREST, VC4_MIN_NEAREST, VC4_WRAP_REPEAT,
                           VC4_WRAP_REPEAT);
        tw[2] = 0;
        ok = vc4_job_begin(&sj, &t, &sp) == 0;
        for (i = 0; i < QUADS && ok; i++)
            ok = speed_draw(&sj, &tp, vc4p_find_tex(&tp, 0, VC4P_FORMAT_R8G8B8A8_UNORM), 0, full2, 8, 4, uv,
                            VC4_PRIM_TRIANGLE_STRIP, 0, 4, tw) == 0;
        ok = ok && vc4_job_run(&sj, 5000000) == 0;
        printf("  %d full-frame quads, textured: render %u us: %.0f Mpixel/s\n", QUADS, sj.render_us,
               ok ? (double)QUADS * SW * SH / sj.render_us : 0.0);
        free(texels);
        vc4_mem_free(&stex);
    }

    /* triangles: a GRID x GRID mesh (20,000 triangles, about 10x8 pixels
     * each) over the frame, REPEAT times */
    for (y = 0; y <= GRID; y++)
        for (x = 0; x <= GRID; x++) {
            float *v = grid + (y * (GRID + 1) + x) * 3;
            v[0] = -1.0f + 2.0f * (float)x / GRID, v[1] = -1.0f + 2.0f * (float)y / GRID, v[2] = 0;
        }
    for (i = 0, y = 0; y < GRID; y++)
        for (x = 0; x < GRID; x++) {
            uint16_t a = (uint16_t)(y * (GRID + 1) + x), b = (uint16_t)(a + 1), c = (uint16_t)(a + GRID + 1),
                     e = (uint16_t)(c + 1);
            gidx[i++] = a, gidx[i++] = b, gidx[i++] = c, gidx[i++] = b, gidx[i++] = e, gidx[i++] = c;
        }
    ok = vc4_job_begin(&sj, &t, &sp) == 0;
    for (k = 0; k < REPEAT && ok; k++)
        ok = speed_draw(&sj, &p, vc4p_find_fs(&p, 0, 0, 0, 0), col, grid, 12, (GRID + 1) * (GRID + 1), 0,
                        VC4_PRIM_TRIANGLES, gidx, GRID * GRID * 6, 0) == 0;
    ok = ok && vc4_job_run(&sj, 5000000) == 0;
    printf("  %d small triangles (%d draws of %d, 16-bit indices): binning %u us (%.2f Mtri/s), render %u us (%.2f "
           "Mtri/s), %u overflow chunk(s)\n",
           REPEAT * GRID * GRID * 2, REPEAT, GRID * GRID * 2, sj.bin_us,
           ok ? (double)REPEAT * GRID * GRID * 2 / sj.bin_us : 0.0, sj.render_us,
           ok ? (double)REPEAT * GRID * GRID * 2 / sj.render_us : 0.0, sj.overflows);
    if (!ok)
        check(0, "the speed jobs ran");
    vc4p_free(&tp);
    vc4p_free(&p);
    vc4_mem_free(&sfb);
    vc4_job_free(&sj);
    vc4_pools_free(&sp);
}

/* The binner's overflow: a ring of 1024 tiny triangles around the frame's
 * centre (each in all four tiles), drawn through 16-bit indices again and
 * again until the tile lists outgrow the tile allocation and ask for
 * overflow memory, chunk after chunk. The picture must be the ring's, drawn
 * once; the job must have asked for overflow memory. */
static int overflow_passes = 6;
static int vcmbe; /* overflow jobs with ERRSTAT's VCMBE */

static void case_overflow(void)
{
    static float ring[1024 * 9];
    static unsigned short idx[3 * 160 * 1024 / 2 * 2];
    static uint8_t once[W * H * 4];
    const uint32_t per_draw = 3 * 1024 * 160; /* 160 times round the ring: 163,840 triangles */
    struct vc4p p;
    int k, pass, ok = 1;
    uint32_t i, ovf;
    long diff = 0;
    printf("overflow (%d draws of 163,840 tiny triangles, every one in all four tiles)\n", overflow_passes);
    for (k = 0; k < 1024; k++) {
        float a = (float)k * 0.0061359f, cx = 0.004f * (float)(k % 7) - 0.012f, cy = 0.004f * (float)(k % 5) - 0.008f;
        float *v = ring + 9 * k;
        (void)a;
        v[0] = cx - 0.02f, v[1] = cy - 0.02f, v[2] = 0;
        v[3] = cx + 0.02f, v[4] = cy - 0.02f, v[5] = 0;
        v[6] = cx, v[7] = cy + 0.02f, v[8] = 0;
    }
    for (i = 0; i < per_draw; i++)
        idx[i] = (unsigned short)(i % (3 * 1024));
    if (load_flat(&p, "flat-flat.vc4p") < 0) {
        failures++;
        return;
    }
    /* the ring once: the picture to get */
    if (begin_case(0) < 0 || flat(&p, 0, 0, 0, idx_col, ring, 3 * 1024, VC4_PRIM_TRIANGLES, idx, 3 * 1024) < 0 ||
        vc4_job_run(&job, 1000000) < 0) {
        check(0, "the ring once");
        vc4p_free(&p);
        return;
    }
    for (i = 0; i < W * H * 4; i += 4)
        vc4_put32(once + i, vc4_get32(fb.cpu + i)); /* host memory: any store will do */
    if (begin_case(0) < 0) {
        failures++;
        vc4p_free(&p);
        return;
    }
    for (pass = 0; pass < overflow_passes && ok; pass++)
        ok = flat(&p, 0, 0, 0, idx_col, ring, 3 * 1024, VC4_PRIM_TRIANGLES, idx, per_draw) == 0;
    if (!ok || vc4_job_run(&job, 10000000) < 0) {
        check(0, "the job ran");
        vc4p_free(&p);
        return;
    }
    ovf = job.overflows;
    for (i = 0; i < W * H * 4; i += 4)
        if (vc4_get32(fb.cpu + i) != vc4_get32(once + i))
            diff++;
    printf("  binning %u us, rendering %u us, %u overflow chunk(s) asked for, ERRSTAT %08x\n", job.bin_us,
           job.render_us, ovf, job.errstat);
    check(ovf > 0, "the binner asked for overflow memory");
    printf("  %ld pixels differ from the ring drawn once\n", diff);
    check(diff == 0, "the ring's picture");
    /* VCMBE (bit 14, the binner's vertex cache manager) comes in about 1 of
     * 200 jobs whose binner ran out of memory, never in the others; the
     * picture is right (PI3_PLAN §3.1, 2026-11-04): counted, not failed */
    if (job.errstat == 0x5000u) {
        vcmbe++;
        printf("  ERRSTAT VCMBE (the binner ran out of memory: seen before, picture right), %d so far\n", vcmbe);
    } else
        check(job.errstat == 0x1000u, "ERRSTAT clean");
    vc4p_free(&p);
}

static const char *const all_cases[] = {"triangle", "texture", "depth",  "blend", "indices", "indices8",
                                        "scissor",  "lines",    "points",  "stencil", "attrib", "textures", "rgb565",
                                        "cube",     "padded",  "msaa",    "jobs",    "rtt",    "overflow"};
#define NCASES (int)(sizeof all_cases / sizeof all_cases[0])

/* One case by name; the jobs it ran (0: unknown). */
static int run_case(const char *name)
{
    int f;
    if (!strcmp(name, "triangle"))
        case_triangle();
    else if (!strcmp(name, "texture"))
        case_texture();
    else if (!strcmp(name, "depth"))
        case_depth();
    else if (!strcmp(name, "blend"))
        case_blend();
    else if (!strcmp(name, "indices"))
        case_indices();
    else if (!strcmp(name, "indices8"))
        case_indices8();
    else if (!strcmp(name, "scissor"))
        case_scissor();
    else if (!strcmp(name, "lines"))
        case_lines();
    else if (!strcmp(name, "points"))
        case_points();
    else if (!strcmp(name, "stencil"))
        case_stencil();
    else if (!strcmp(name, "attrib"))
        case_attrib();
    else if (!strcmp(name, "textures")) {
        for (f = 0; f < NTEXFMTS; f++)
            tex_case(&texfmts[f]);
        return NTEXFMTS;
    } else if (!strcmp(name, "rgb565"))
        case_rgb565();
    else if (!strcmp(name, "cube"))
        case_cube();
    else if (!strcmp(name, "padded"))
        case_padded();
    else if (!strcmp(name, "msaa"))
        case_msaa();
    else if (!strcmp(name, "jobs")) {
        case_jobs();
        return 2;
    } else if (!strcmp(name, "rtt")) {
        case_rtt();
        return 2;
    } else if (!strcmp(name, "speed")) {
        case_speed();
    } else if (!strcmp(name, "overflow")) {
        case_overflow();
        return 2;
    } else {
        printf("unknown case %s\n", name);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *names[64];
    int k, n = 0, soak = 0, loud = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "--dir=", 6))
            dir = argv[k] + 6;
        else if (!strncmp(argv[k], "--refdir=", 9))
            refdir = argv[k] + 9;
        else if (!strncmp(argv[k], "--soak=", 7))
            soak = atoi(argv[k] + 7);
        else if (!strncmp(argv[k], "--passes=", 9))
            overflow_passes = atoi(argv[k] + 9);
        else if (!strcmp(argv[k], "--loud"))
            loud = 1;
        else if (argv[k][0] != '-' && n < 64)
            names[n++] = argv[k];
    }
    if (!n)
        for (k = 0; k < NCASES; k++)
            names[n++] = all_cases[k];
    if (vc4_open() < 0)
        return 2;
    if (vc4_pools_init(&pools, 4, 4) < 0 /* 128x128 in 32x32 tiles (MSAA) */ || vc4_job_init(&job) < 0 ||
        vc4_mem_alloc(&fb, 132 * H * 4) < 0 || vc4_mem_alloc(&tex, 64u << 10) < 0 ||
        vc4_mem_alloc(&zsbuf, W * H * 4) < 0 || vc4_mem_alloc(&rtt, W * H * 4) < 0)
        return 2;
    if (!soak) {
        for (k = 0; k < n; k++)
            run_case(names[k]);
    } else {
        /* --soak=N: the cases N times (all, or the ones named; the overflow
         * one every tenth round with 2 draws unless --passes says), each
         * picture checked; only the rounds with failures reported, unless
         * --loud. */
        int round, quiet = open("/dev/null", O_WRONLY), out = dup(1), before;
        long jobs = 0;
        uint64_t t0 = vc4_os_time_us();
        int passes_given = overflow_passes != 6;
        if (!passes_given)
            overflow_passes = 2;
        for (round = 1; round <= soak; round++) {
            before = failures;
            fflush(stdout);
            if (!loud)
                dup2(quiet, 1);
            for (k = 0; k < n; k++) {
                if (!strcmp(names[k], "overflow") && n > 1 && round % 10)
                    continue;
                if (loud)
                    printf("[round %d] ", round);
                jobs += run_case(names[k]);
            }
            fflush(stdout);
            if (!loud)
                dup2(out, 1);
            if (failures != before)
                printf("round %d: %d failure(s)\n", round, failures - before);
            if (round % 100 == 0)
                printf("round %d of %d, %llu s, %d failure(s) so far\n", round, soak,
                       (unsigned long long)((vc4_os_time_us() - t0) / 1000000u), failures);
        }
        printf("soak: %d rounds (%ld jobs), %d failure(s), %d overflow job(s) with VCMBE\n", soak, jobs, failures,
               vcmbe);
    }
    vc4_mem_free(&rtt);
    vc4_mem_free(&zsbuf);
    vc4_mem_free(&tex);
    vc4_mem_free(&fb);
    vc4_job_free(&job);
    vc4_pools_free(&pools);
    vc4_close();
    printf("%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
