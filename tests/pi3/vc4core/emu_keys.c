/*
 * The key records (v3dglslc -g 21 -k) of draw.c's cases for the extensions
 * VC4 emulates (docs/PI3_PLAN.md §2.1): built by the VC4 backend's own key
 * builder (ogles2/vc4/key_vc4.c), so the variants are the ones the library
 * would ask for. The textured quad (tests/pi3/ref/shaders/texture.*: pos
 * and uv two floats each) into an RGBA8 target, triangles:
 *   - sampler 0 a half-float texture (RGBA64: patch 0018's two reads);
 *   - an RGBA8 texture with patch 0019's emulations: MIRROR_CLAMP_TO_EDGE,
 *     the LOD bias, the LOD clamp, a forced first level, 4 anisotropic
 *     lookups.
 * And flat.vert + flat.frag (pos three floats): the depth test on, the
 * vertex variants keyed clip_halfz (EXT_clip_control's ZERO_TO_ONE, patch
 * 0020); into a half-float target (EXT_color_buffer_half_float, patch
 * 0021), the depth test on, or blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
 * And flat.vert + mrt.frag (gl_FragData[0] and [1]): the depth test on, a
 * fragment variant for each (EXT_draw_buffers, patch 0016).
 *   emu_keys TEXTURE_KEYS FLAT_KEYS MRT_KEYS
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "key_vc4.h"

static FILE *out;
static int n;

/* A fragment record for fs and the vertex record of the quad's arrays. */
static void pair(const struct vc4_fs_state *fs)
{
    unsigned char rec[VC4_KEY_FS_BYTES > VC4_KEY_VS_BYTES ? VC4_KEY_FS_BYTES : VC4_KEY_VS_BYTES];
    struct vc4_vs_state vs;
    memset(&vs, 0, sizeof(vs));
    vs.nattrs = 2;
    vs.attr_format[0] = (unsigned short)vc4_attr_format(GL_FLOAT, 2, 0);
    vs.attr_format[1] = (unsigned short)vc4_attr_format(GL_FLOAT, 2, 0);
    vc4_key_fs(rec, fs, 1);
    fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
    vc4_key_vs(rec, &vs, 1);
    fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
    n++;
}

static void base(struct vc4_fs_state *fs, const struct vc4_tex_format *f)
{
    memset(fs, 0, sizeof(*fs));
    fs->color_format = VC4K_R8G8B8A8_UNORM;
    fs->colormask = 15;
    fs->nsamplers = 1;
    fs->samp[0].f = f;
    fs->samp[0].wrap_s = fs->samp[0].wrap_t = GL_CLAMP_TO_EDGE;
}

int main(int argc, char **argv)
{
    struct vc4_fs_state fs;
    if (argc != 4 || !(out = fopen(argv[1], "wb"))) {
        fprintf(stderr, "usage: emu_keys TEXTURE_KEYS FLAT_KEYS MRT_KEYS\n");
        return 1;
    }
    base(&fs, vc4_tex_format(GL_RGBA, GL_HALF_FLOAT_OES)); /* texhalf */
    pair(&fs);
    base(&fs, vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE)); /* mirror */
    fs.samp[0].wrap_s = fs.samp[0].wrap_t = GL_MIRROR_CLAMP_TO_EDGE;
    pair(&fs);
    base(&fs, vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE)); /* lodbias */
    fs.samp[0].lod_bias = 1;
    pair(&fs);
    base(&fs, vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE)); /* minlod, maxlod, baselevel */
    fs.samp[0].lod_clamp = 1;
    pair(&fs);
    fs.samp[0].lod_bias = 1; /* lodboth */
    pair(&fs);
    base(&fs, vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE)); /* forcefirst */
    fs.samp[0].force_first = 1;
    pair(&fs);
    base(&fs, vc4_tex_format(GL_RGBA, GL_UNSIGNED_BYTE)); /* aniso, anisoquad */
    fs.samp[0].aniso = 2;
    pair(&fs);
    fclose(out);
    printf("%s: %d fragment and vertex key pairs\n", argv[1], n);
    if (!(out = fopen(argv[2], "wb")))
        return 1;
    n = 0;
    {
        unsigned char rec[VC4_KEY_FS_BYTES > VC4_KEY_VS_BYTES ? VC4_KEY_FS_BYTES : VC4_KEY_VS_BYTES];
        struct vc4_vs_state vs;
        memset(&fs, 0, sizeof(fs));
        fs.color_format = VC4K_R8G8B8A8_UNORM;
        fs.colormask = 15;
        fs.depth_test = 1;
        memset(&vs, 0, sizeof(vs));
        vs.nattrs = 1;
        vs.attr_format[0] = (unsigned short)vc4_attr_format(GL_FLOAT, 3, 0);
        vs.clip_halfz = 1;
        vc4_key_fs(rec, &fs, 1);
        fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
        vc4_key_vs(rec, &vs, 1);
        fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
        n++;
        vs.clip_halfz = 0;
        fs.color_format = VC4K_R16G16B16A16_FLOAT; /* hdr */
        vc4_key_fs(rec, &fs, 1);
        fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
        vc4_key_vs(rec, &vs, 1);
        fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
        n++;
        fs.depth_test = 0; /* hdrblend: the blend case's quad, then its blended triangle */
        vc4_key_fs(rec, &fs, 1);
        fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
        vc4_key_vs(rec, &vs, 1);
        fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
        n++;
        fs.blend = 1;
        fs.eq_rgb = fs.eq_alpha = GL_FUNC_ADD;
        fs.src_rgb = fs.src_alpha = GL_SRC_ALPHA;
        fs.dst_rgb = fs.dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
        vc4_key_fs(rec, &fs, 1);
        fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
        vc4_key_vs(rec, &vs, 1);
        fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
        n++;
    }
    fclose(out);
    printf("%s: %d fragment and vertex key pairs\n", argv[2], n);
    if (!(out = fopen(argv[3], "wb")))
        return 1;
    n = 0;
    {
        unsigned char rec[VC4_KEY_FS_BYTES > VC4_KEY_VS_BYTES ? VC4_KEY_FS_BYTES : VC4_KEY_VS_BYTES];
        struct vc4_vs_state vs;
        int db;
        memset(&vs, 0, sizeof(vs));
        vs.nattrs = 1;
        vs.attr_format[0] = (unsigned short)vc4_attr_format(GL_FLOAT, 3, 0);
        for (db = 0; db < 2; db++) {
            memset(&fs, 0, sizeof(fs));
            fs.color_format = VC4K_R8G8B8A8_UNORM;
            fs.colormask = 15;
            fs.depth_test = 1;
            fs.draw_buffer = (unsigned char)db;
            vc4_key_fs(rec, &fs, 1);
            fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
            vc4_key_vs(rec, &vs, 1);
            fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
            n++;
        }
    }
    fclose(out);
    printf("%s: %d fragment and vertex key pairs\n", argv[3], n);
    return 0;
}
