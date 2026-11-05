/*
 * S3TC (DXT1, DXT3, DXT5) blocks decoded on the CPU, for VC4, whose TMU
 * has no BC types (docs/PI3_PLAN.md §2.1: "decoded on the CPU at upload").
 * Plain C, no AmigaOS types: tests/pi3/s3tc checks it against Mesa's
 * decoders on the PC.
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_S3TC_H
#define OGLES2_S3TC_H

#define VC4_S3TC_DXT1_RGB 0  /* GL_COMPRESSED_RGB_S3TC_DXT1_EXT: alpha one */
#define VC4_S3TC_DXT1_RGBA 1 /* GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: index 3 of a 3-colour block clear */
#define VC4_S3TC_DXT3 2
#define VC4_S3TC_DXT5 3

/* One 4x4 block (8 bytes for DXT1, 16 for DXT3 and DXT5) into 16 texels,
 * RGBA bytes, row by row. */
void vc4_s3tc_block(const unsigned char *block, int type, unsigned char out[16][4]);

#endif
