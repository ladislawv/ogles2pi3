/*
 * S3TC block decoding (s3tc.h), from the formats' definition: two 5:6:5
 * colours and 2-bit indices (a third and fourth colour between them, or for
 * DXT1 with the first colour not above the second, their mean and black or
 * clear); DXT3 adds 4-bit alpha a texel, DXT5 two alpha values and 3-bit
 * indices (six or four steps between them, then 0 and 255). The steps are
 * integer divisions of the expanded 8-bit values, as Mesa's decoder rounds
 * them (tests/pi3/s3tc: equal to it texel for texel).
 *
 * MIT licence (LICENSE).
 */
#include "s3tc.h"

/* 5:6:5 to 8 bits a channel, the top bits repeated */
static void expand565(unsigned c, unsigned rgb[3])
{
    rgb[0] = ((c >> 8) & 0xf8) | ((c >> 13) & 7);
    rgb[1] = ((c >> 3) & 0xfc) | ((c >> 9) & 3);
    rgb[2] = ((c << 3) & 0xf8) | ((c >> 2) & 7);
}

/* The colour part (8 bytes): four colours (with alpha), each texel's index. */
static void colours(const unsigned char *b, int four, int clear3, unsigned char out[16][4])
{
    unsigned c0 = b[0] | (unsigned)b[1] << 8, c1 = b[2] | (unsigned)b[3] << 8;
    unsigned long bits = b[4] | (unsigned long)b[5] << 8 | (unsigned long)b[6] << 16 | (unsigned long)b[7] << 24;
    unsigned e0[3], e1[3], pal[4][4];
    int k, i;
    expand565(c0, e0);
    expand565(c1, e1);
    for (k = 0; k < 3; k++) {
        pal[0][k] = e0[k];
        pal[1][k] = e1[k];
        if (four || c0 > c1) {
            pal[2][k] = (2 * e0[k] + e1[k]) / 3;
            pal[3][k] = (e0[k] + 2 * e1[k]) / 3;
        } else {
            pal[2][k] = (e0[k] + e1[k]) / 2;
            pal[3][k] = 0;
        }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = !four && c0 <= c1 && clear3 ? 0 : 255;
    for (i = 0; i < 16; i++) {
        unsigned code = (unsigned)(bits >> (2 * i)) & 3;
        for (k = 0; k < 4; k++)
            out[i][k] = (unsigned char)pal[code][k];
    }
}

void vc4_s3tc_block(const unsigned char *b, int type, unsigned char out[16][4])
{
    int i;
    if (type == VC4_S3TC_DXT1_RGB || type == VC4_S3TC_DXT1_RGBA) {
        colours(b, 0, type == VC4_S3TC_DXT1_RGBA, out);
        return;
    }
    colours(b + 8, 1, 0, out);
    if (type == VC4_S3TC_DXT3) {
        for (i = 0; i < 16; i++) {
            unsigned a = (b[i / 2] >> (4 * (i & 1))) & 15;
            out[i][3] = (unsigned char)(a << 4 | a);
        }
        return;
    }
    {
        unsigned a0 = b[0], a1 = b[1];
        for (i = 0; i < 16; i++) {
            unsigned bit = 3 * (unsigned)i, code, a;
            /* 48 bits of 3-bit codes, little-endian, from byte 2 */
            unsigned v = b[2 + bit / 8] | (bit / 8 + 1 < 6 ? (unsigned)b[2 + bit / 8 + 1] << 8 : 0);
            code = (v >> (bit % 8)) & 7;
            if (code == 0)
                a = a0;
            else if (code == 1)
                a = a1;
            else if (a0 > a1)
                a = (a0 * (8 - code) + a1 * (code - 1)) / 7;
            else if (code < 6)
                a = (a0 * (6 - code) + a1 * (code - 1)) / 5;
            else
                a = code == 6 ? 0 : 255;
            out[i][3] = (unsigned char)a;
        }
    }
}
