/*
 * The VC4 backend's key builder (ogles2/vc4/key_vc4.c) against Mesa's own
 * keys (docs/PI3_PLAN.md §4.2, P5). tests/pi3/vc4core/build.sh compiles
 * tests/pi3/ref's programs with v3dglslc -g 21 -s: Mesa's state tracker and
 * vc4 driver build a key for each state drawn, and the .vc4p files hold them.
 * Here the same states are described as the backend will see them, our
 * records built, and each must be one of the file's keys byte for byte
 * (a vertex key without its fs_inputs, which the compiler fills); and every
 * key in the files must be one of ours.
 *   bash tests/pi3/keys/build.sh   (in WSL, after tests/pi3/vc4core/build.sh)
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "key_vc4.h"

static const char *dir;
static int failures;

struct file
{
    unsigned char *data;
    long len;
    int nsec, used[64];
    const unsigned char *sec[64]; /* VC4_VARIANT payloads */
    unsigned seclen[64];
};

static unsigned u32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static int load(struct file *f, const char *name)
{
    char path[512];
    FILE *fp;
    long o;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    memset(f, 0, sizeof(*f));
    if (!(fp = fopen(path, "rb"))) {
        printf("%s: cannot open\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    f->len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = malloc((size_t)f->len);
    if (fread(f->data, 1, (size_t)f->len, fp) != (size_t)f->len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    for (o = 32; o + 8 <= f->len;) {
        unsigned t = u32(f->data + o), n = u32(f->data + o + 4);
        if (t == 7 && f->nsec < 64) {
            f->sec[f->nsec] = f->data + o + 8;
            f->seclen[f->nsec++] = n;
        }
        o += 8 + ((n + 3) & ~3u);
    }
    return 0;
}

/* The record must be one of the file's keys: a fragment one whole, a vertex
 * one (stage 0 or its coordinate twin, stage 1) up to its fs_inputs. */
static void expect(struct file *f, const char *what, const unsigned char *rec, int vertex)
{
    int k, found = 0;
    unsigned n = vertex ? VC4_KEY_VS_BYTES - 4 - 4 /* up to fs_inputs */ : VC4_KEY_FS_BYTES - 4;
    for (k = 0; k < f->nsec; k++) {
        unsigned stage = u32(f->sec[k]);
        if (vertex ? stage == 4 : stage != 4)
            continue;
        if (vertex && stage == 1) {
            /* the coordinate key: is_coord set, the rest ours */
            unsigned char c[VC4_KEY_VS_BYTES];
            memcpy(c, rec, sizeof c);
            c[4 + 256 + 32] = 1;
            if (!memcmp(f->sec[k] + 4, c + 4, n)) {
                f->used[k] = 1;
                continue;
            }
        } else if (!memcmp(f->sec[k] + 4, rec + 4, n)) {
            f->used[k] = 1;
            found = 1;
        }
    }
    printf("  %-44s %s\n", what, found ? "Mesa's key" : "NOT AMONG MESA'S KEYS");
    if (!found) {
        /* the nearest key of the stage: where they part */
        int best = -1, best_at = -1;
        unsigned i;
        for (k = 0; k < f->nsec; k++) {
            unsigned stage = u32(f->sec[k]);
            if (vertex ? stage != 0 : stage != 4)
                continue;
            for (i = 0; i < n && f->sec[k][4 + i] == rec[4 + i]; i++)
                ;
            if ((int)i > best_at)
                best = k, best_at = (int)i;
        }
        if (best >= 0)
            printf("    nearest: variant %d, from key byte %d: Mesa's %02x %02x %02x %02x, ours %02x %02x %02x %02x\n",
                   best, best_at, f->sec[best][4 + best_at], f->sec[best][5 + best_at], f->sec[best][6 + best_at],
                   f->sec[best][7 + best_at], rec[4 + best_at], rec[5 + best_at], rec[6 + best_at], rec[7 + best_at]);
        failures++;
    }
}

static void unused(struct file *f, const char *name)
{
    int k;
    for (k = 0; k < f->nsec; k++)
        if (!f->used[k]) {
            printf("  %s: Mesa's variant %d (stage %u) is none of ours\n", name, k, u32(f->sec[k]));
            failures++;
        }
    free(f->data);
}

static struct vc4_fs_state fs_ref(void)
{
    struct vc4_fs_state s;
    memset(&s, 0, sizeof s);
    s.color_format = VC4K_R8G8B8A8_UNORM; /* v3dglslc draws into an RGBA8 renderbuffer */
    s.colormask = 15;
    return s;
}

static struct vc4_vs_state vs_of(int n, const unsigned *formats)
{
    struct vc4_vs_state s;
    int i;
    memset(&s, 0, sizeof s);
    s.nattrs = (unsigned char)n;
    for (i = 0; i < n; i++)
        s.attr_format[i] = (unsigned short)formats[i];
    return s;
}

/* --write DIR: the texture program's sampler states as key files, exact
 * (DIR/exact.keys) and canonical (DIR/canonical.keys), for build.sh to
 * compile with v3dglslc -k and compare the variants' code. */
static int write_keys(const char *out)
{
    static const GLenum f[][2] = {{GL_RGBA, GL_UNSIGNED_BYTE}, {GL_RGB, GL_UNSIGNED_SHORT_5_6_5},
                                  {GL_LUMINANCE, GL_UNSIGNED_BYTE}, {GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},
                                  {GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE}, {GL_ETC1_RGB8_OES, 0}};
    static const GLenum wraps[] = {GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT};
    unsigned char r[VC4_KEY_VS_BYTES];
    unsigned a[2] = {vc4_attr_format(GL_FLOAT, 2, 0), vc4_attr_format(GL_FLOAT, 2, 0)};
    int canonical, k, w;
    for (canonical = 0; canonical < 2; canonical++) {
        char path[512];
        FILE *fp;
        struct vc4_vs_state vs = vs_of(2, a);
        snprintf(path, sizeof path, "%s/%s.keys", out, canonical ? "canonical" : "exact");
        if (!(fp = fopen(path, "wb")))
            return 1;
        for (k = 0; k < (int)(sizeof f / sizeof f[0]); k++)
            for (w = 0; w < 3; w++) {
                struct vc4_fs_state fs = fs_ref();
                fs.nsamplers = 1;
                fs.samp[0].f = vc4_tex_format(f[k][0], f[k][1]);
                fs.samp[0].wrap_s = fs.samp[0].wrap_t = wraps[w];
                vc4_key_fs(r, &fs, canonical);
                fwrite(r, 1, VC4_KEY_FS_BYTES, fp);
            }
        vc4_key_vs(r, &vs, canonical);
        fwrite(r, 1, VC4_KEY_VS_BYTES, fp);
        fclose(fp);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--write"))
        return write_keys(argv[2]);
    unsigned char r[VC4_KEY_VS_BYTES];
    struct vc4_fs_state fs;
    struct vc4_vs_state vs;
    struct file f;
    unsigned vec3[1] = {vc4_attr_format(GL_FLOAT, 3, 0)};
    dir = argc > 1 ? argv[1] : ".";

    printf("flat-flat.vc4p (-s depth,blend,lines,stencil,rt565)\n");
    if (load(&f, "flat-flat.vc4p") == 0) {
        fs = fs_ref();
        vc4_key_fs(r, &fs, 0), expect(&f, "triangles", r, 0);
        fs.depth_test = 1;
        vc4_key_fs(r, &fs, 0), expect(&f, "depth test", r, 0);
        fs = fs_ref();
        fs.stencil_test = 1;
        vc4_key_fs(r, &fs, 0), expect(&f, "stencil test (one-sided, mask 0xff)", r, 0);
        fs = fs_ref();
        fs.blend = 1;
        fs.eq_rgb = fs.eq_alpha = GL_FUNC_ADD;
        fs.src_rgb = fs.src_alpha = GL_SRC_ALPHA;
        fs.dst_rgb = fs.dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
        vc4_key_fs(r, &fs, 0), expect(&f, "blending SRC_ALPHA, ONE_MINUS_SRC_ALPHA", r, 0);
        fs.color_format = VC4K_B5G6R5_UNORM;
        vc4_key_fs(r, &fs, 0), expect(&f, "the same into RGB565", r, 0);
        fs = fs_ref();
        fs.color_format = VC4K_B5G6R5_UNORM;
        vc4_key_fs(r, &fs, 0), expect(&f, "triangles into RGB565", r, 0);
        fs = fs_ref();
        fs.prim = 1;
        vc4_key_fs(r, &fs, 0), expect(&f, "lines", r, 0);
        vs = vs_of(1, vec3);
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec3 floats", r, 1);
        unused(&f, "flat-flat");
    } else
        failures++;

    printf("point-flat.vc4p (-s points)\n");
    if (load(&f, "point-flat.vc4p") == 0) {
        fs = fs_ref();
        vc4_key_fs(r, &fs, 0), expect(&f, "triangles", r, 0);
        fs.prim = 2;
        vc4_key_fs(r, &fs, 0), expect(&f, "points", r, 0);
        vs = vs_of(1, vec3);
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec3 floats", r, 1);
        vs.per_vertex_point_size = 1;
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec3 floats, points with gl_PointSize", r, 1);
        unused(&f, "point-flat");
    } else
        failures++;

    printf("color-color.vc4p (-b pos,col -s ubyte1)\n");
    if (load(&f, "color-color.vc4p") == 0) {
        unsigned a[2];
        fs = fs_ref();
        vc4_key_fs(r, &fs, 0), expect(&f, "triangles", r, 0);
        a[0] = vec3[0];
        a[1] = vc4_attr_format(GL_FLOAT, 4, 0);
        vs = vs_of(2, a);
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec3, vec4 floats", r, 1);
        a[1] = vc4_attr_format(GL_UNSIGNED_BYTE, 4, 1);
        vs = vs_of(2, a);
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec3 floats, 4 normalized bytes", r, 1);
        unused(&f, "color-color");
    } else
        failures++;

    printf("texture-texture.vc4p (-b pos,uv -s tex565,texlum,tex4444,tex5551,texla,etc1,texd24,texds,texd16)\n");
    if (load(&f, "texture-texture.vc4p") == 0) {
        static const struct { const char *what; GLenum format, type; } t[] = {
            {"sampler: RGBA, UNSIGNED_BYTE", GL_RGBA, GL_UNSIGNED_BYTE},
            {"sampler: RGB, UNSIGNED_SHORT_5_6_5", GL_RGB, GL_UNSIGNED_SHORT_5_6_5},
            {"sampler: LUMINANCE", GL_LUMINANCE, GL_UNSIGNED_BYTE},
            {"sampler: RGBA, UNSIGNED_SHORT_4_4_4_4", GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4},
            {"sampler: RGBA, UNSIGNED_SHORT_5_5_5_1", GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},
            {"sampler: LUMINANCE_ALPHA", GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE},
            {"sampler: ETC1", GL_ETC1_RGB8_OES, 0},
            {"sampler: DEPTH_COMPONENT, UNSIGNED_INT", GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},
            {"sampler: DEPTH_COMPONENT, UNSIGNED_SHORT", GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT},
            {"sampler: DEPTH_STENCIL, UNSIGNED_INT_24_8", GL_DEPTH_STENCIL_OES, GL_UNSIGNED_INT_24_8_OES},
        };
        unsigned a[2] = {vc4_attr_format(GL_FLOAT, 2, 0), vc4_attr_format(GL_FLOAT, 2, 0)};
        int k;
        for (k = 0; k < (int)(sizeof t / sizeof t[0]); k++) {
            fs = fs_ref();
            fs.nsamplers = 1;
            fs.samp[0].f = vc4_tex_format(t[k].format, t[k].type);
            fs.samp[0].wrap_s = fs.samp[0].wrap_t = GL_CLAMP_TO_EDGE;
            vc4_key_fs(r, &fs, 0), expect(&f, t[k].what, r, 0);
        }
        vs = vs_of(2, a);
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec2, vec2 floats", r, 1);
        unused(&f, "texture-texture");
    } else
        failures++;

    printf("cube-cube.vc4p (-b pos,dir)\n");
    if (load(&f, "cube-cube.vc4p") == 0) {
        unsigned a[2] = {vec3[0], vec3[0]};
        fs = fs_ref();
        fs.nsamplers = 1;
        fs.samp[0].f = vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE);
        fs.samp[0].wrap_s = fs.samp[0].wrap_t = GL_CLAMP_TO_EDGE;
        vc4_key_fs(r, &fs, 0), expect(&f, "samplerCube: RGBA, UNSIGNED_BYTE", r, 0);
        vs = vs_of(2, a);
        vc4_key_vs(r, &vs, 0), expect(&f, "vertex: vec3, vec3 floats", r, 1);
        unused(&f, "cube-cube");
    } else
        failures++;

    printf("%s (%d failure(s))\n", failures ? "FAILED" : "all of Mesa's keys built", failures);
    return failures ? 1 : 0;
}
