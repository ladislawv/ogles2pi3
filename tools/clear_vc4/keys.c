/*
 * The key records (v3dglslc -k) of the clears the VC4 backend draws as
 * rectangles (ogles2/draw.c, ogles2_draw_clear): built by the backend's own
 * key builder (ogles2/vc4/key_vc4.c) for the states such a clear has, so
 * the built-in image (tools/clear_vc4/make_header.sh) holds their variants
 * and a clear needs no compile. A target RGBA8 (a texture) or BGRA8 (most
 * displays), the depth and stencil tests on or off with the colour written
 * or not, and every colour mask with the tests off; triangles, no blending,
 * no MSAA; position three or four floats, colour the current value (four
 * floats). Other states compile at the first clear (the image keeps its
 * sources).
 *   keys OUTFILE
 *
 * MIT licence (LICENSE).
 */
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "key_vc4.h"

int main(int argc, char **argv)
{
    static const unsigned formats[2] = {VC4K_R8G8B8A8_UNORM, VC4K_B8G8R8A8_UNORM};
    unsigned char rec[VC4_KEY_FS_BYTES > VC4_KEY_VS_BYTES ? VC4_KEY_FS_BYTES : VC4_KEY_VS_BYTES];
    struct vc4_fs_state fs;
    struct vc4_vs_state vs;
    int f, depth, stencil, n = 0;
    FILE *out;
    if (argc != 2 || !(out = fopen(argv[1], "wb"))) {
        fprintf(stderr, "usage: keys OUTFILE\n");
        return 1;
    }
    int pos, mask;
    /* (P5 run 1, 2026-11-05: masked clears (glColorMask's partial masks)
     * and a four-float position compiled at a context's first such clear,
     * and the image kept them: the packages' memory checks counted them;
     * every mask with the depth and stencil tests off now, and both
     * position sizes) */
    for (pos = 3; pos <= 4; pos++) {
        memset(&vs, 0, sizeof(vs));
        vs.nattrs = 2;
        vs.attr_format[0] = (unsigned short)vc4_attr_format(GL_FLOAT, pos, 0);
        vs.attr_format[1] = VC4K_R32G32B32A32_FLOAT;
        for (f = 0; f < 2; f++)
            for (depth = 0; depth < 2; depth++)
                for (stencil = 0; stencil < 2; stencil++)
                    for (mask = 0; mask < 16; mask++) {
                        if ((depth || stencil) && mask != 0 && mask != 15)
                            continue; /* a partial mask with the tests on: at run time */
                        memset(&fs, 0, sizeof(fs));
                        fs.color_format = formats[f];
                        fs.depth_test = (unsigned char)depth;
                        fs.stencil_test = (unsigned char)stencil;
                        fs.colormask = (unsigned char)mask;
                        vc4_key_fs(rec, &fs, 1);
                        fwrite(rec, 1, VC4_KEY_FS_BYTES, out);
                        vc4_key_vs(rec, &vs, 1); /* its vertex variant pairs with this fragment one */
                        fwrite(rec, 1, VC4_KEY_VS_BYTES, out);
                        n++;
                    }
    }
    fclose(out);
    printf("%s: %d fragment and vertex key pairs\n", argv[1], n);
    return 0;
}
