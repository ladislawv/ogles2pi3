/*
 * The key records (v3dglslc -k) of the copies the VC4 backend draws
 * (ogles2/copy.c, glCopyTex* from a framebuffer object): built by the backend's
 * own key builder (ogles2/vc4/key_vc4.c) for the states such a copy has, so
 * tools/copy_tex/make_header_vc4.sh's image holds their variants and a copy
 * needs no compile, with or without a compiler installed. A source of any
 * format a colour attachment can be (a half-float one stays on the CPU: copy.c)
 * or the display's picture, a target RGBA8 or RGB565 (VC4's stores); triangles,
 * no test, no blending, every
 * channel written, no MSAA; position and texture coordinates two floats each.
 *   keys_vc4 OUTFILE
 *
 * MIT licence (LICENSE).
 */
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "key_vc4.h"
#include "vc4core.h"

int main(int argc, char **argv)
{
    static const unsigned targets[2] = {VC4K_R8G8B8A8_UNORM, VC4K_B5G6R5_UNORM};
    static const struct { GLenum format, type; } sources[] = {
        {GL_RGBA, GL_UNSIGNED_BYTE},
        {GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGB, GL_UNSIGNED_SHORT_5_6_5},
        {GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4},
        {GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},
        {GL_BGRA_EXT, GL_UNSIGNED_BYTE}, /* the picture of a BGRA display (ogles2_vc4_tex_from_raster) */
    };
    unsigned char rec[VC4_KEY_FS_BYTES > VC4_KEY_VS_BYTES ? VC4_KEY_FS_BYTES : VC4_KEY_VS_BYTES];
    struct vc4_fs_state fs;
    struct vc4_vs_state vs;
    unsigned s, t;
    int n = 0;
    FILE *out;
    if (argc != 2 || !(out = fopen(argv[1], "wb"))) {
        fprintf(stderr, "usage: keys_vc4 OUTFILE\n");
        return 1;
    }
    memset(&vs, 0, sizeof(vs));
    vs.nattrs = 2;
    vs.attr_format[0] = (unsigned short)vc4_attr_format(GL_FLOAT, 2, 0);
    vs.attr_format[1] = (unsigned short)vc4_attr_format(GL_FLOAT, 2, 0);
    for (t = 0; t < sizeof(targets) / sizeof(targets[0]); t++)
        for (s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
            const struct vc4_tex_format *f = vc4_tex_format(sources[s].format, sources[s].type);
            if (!f) {
                fprintf(stderr, "keys_vc4: no storage for source %u\n", s);
                return 1;
            }
            memset(&fs, 0, sizeof(fs));
            fs.color_format = targets[t];
            fs.colormask = 15;
            fs.nsamplers = 1;
            fs.samp[0].f = f;
            fs.samp[0].wrap_s = fs.samp[0].wrap_t = GL_CLAMP_TO_EDGE;
            vc4_key_fs(rec, &fs, 1);
            fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
            vc4_key_vs(rec, &vs, 1); /* its vertex variant pairs with this fragment one */
            fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
            n++;
            /* glGenerateMipmap's levels below the first: a texture into itself, one
             * level above 0 sampled by an explicit LOD (copy.c, ogles2_copy_levels) */
            if (f->key_format != VC4K_B8G8R8A8_UNORM && (f->hw_type == VC4_TEX_RGB565) == (targets[t] == VC4K_B5G6R5_UNORM) &&
                (f->hw_type == VC4_TEX_RGB565 || f->hw_type == VC4_TEX_RGBA8888 || f->hw_type == VC4_TEX_RGBX8888)) {
                fs.samp[0].force_first = 1;
                vc4_key_fs(rec, &fs, 1);
                fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
                vc4_key_vs(rec, &vs, 1);
                fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
                n++;
            }
        }
    fclose(out);
    printf("%s: %d fragment and vertex key pairs\n", argv[1], n);
    return 0;
}
