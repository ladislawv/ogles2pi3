/*
 * The cases Mesa's vc4 (mesa_cases.c, reference boot) and vc4core
 * (tests/pi3/vc4core/draw.c, bare boot) both draw, with the same data, into
 * a 128x128 RGBA8 target cleared to (0, 0, .25, 1) and depth 1.0
 * (docs/PI3_PLAN.md §3.1, §6.3). Shaders: tests/pi3/ref/shaders/flat.vert,
 * point.vert (vec3 position), flat.frag (a uniform colour).
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#ifndef PI3_CASES_H
#define PI3_CASES_H

/* depth: three triangles drawn in this order with GL_LESS; the last one,
 * at z 0, lies behind the second (z -0.5) and in front of the first (0.5). */
static const float depth_tri[3][9] = {
    {-0.9f, -0.9f, 0.5f, 0.6f, -0.8f, 0.5f, -0.2f, 0.8f, 0.5f},
    {-0.3f, -0.6f, -0.5f, 0.9f, -0.2f, -0.5f, 0.1f, 0.9f, -0.5f},
    {-0.8f, 0.1f, 0.0f, 0.7f, -0.7f, 0.0f, 0.5f, 0.6f, 0.0f},
};
static const float depth_col[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};

/* blend: an opaque quad (a strip), then a triangle at alpha 0.5 with
 * SRC_ALPHA, ONE_MINUS_SRC_ALPHA. */
static const float blend_quad[4 * 3] = {-0.7f, -0.7f, 0, 0.5f, -0.7f, 0, -0.7f, 0.5f, 0, 0.5f, 0.5f, 0};
static const float blend_tri[3 * 3] = {-0.2f, -0.9f, 0, 0.9f, -0.1f, 0, -0.1f, 0.9f, 0};
static const float blend_col[2][4] = {{1, 0, 0, 1}, {0, 0, 1, 0.5f}};

/* indices: a triangle list through 16-bit indices, vertices shared. */
static const float idx_vtx[6 * 3] = {-0.8f, -0.8f, 0, 0.0f, -0.9f, 0, 0.8f, -0.8f, 0,
                                     -0.6f, 0.3f,  0, 0.1f, 0.8f,  0, 0.7f, 0.2f,  0};
static const unsigned short idx_list[9] = {0, 1, 3, 1, 4, 3, 1, 2, 5};
static const float idx_col[4] = {1, 0.5f, 0, 1};

/* lines: three lines, width 1. */
static const float line_vtx[6 * 3] = {-0.9f, -0.8f, 0, 0.8f, 0.7f, 0, -0.7f, 0.6f, 0,
                                      0.6f, -0.9f, 0, -0.95f, 0.0f, 0, 0.95f, 0.05f, 0};
static const float line_col[4] = {1, 1, 0, 1};

/* points: five points, 8 pixels (point.vert's gl_PointSize). */
static const float point_vtx[5 * 3] = {-0.6f, -0.6f, 0, 0.5f, -0.4f, 0, 0.0f, 0.0f, 0, -0.4f, 0.6f, 0, 0.7f, 0.7f, 0};
static const float point_col[4] = {0, 1, 1, 1};

/* scissor: the blend case's quad with the scissor box (x, y, w, h; GL's
 * window coordinates, y up). */
static const int sciss_box[4] = {20, 30, 70, 50};

/* stencil (flat program): the triangle writes stencil 1 where it covers
 * (ALWAYS, REPLACE), then the quad is drawn where stencil EQUAL 1 (KEEP). */
static const float sten_tri[3 * 3] = {-0.8f, -0.7f, 0, 0.7f, -0.5f, 0, -0.1f, 0.8f, 0};
static const float sten_quad[4 * 3] = {-0.9f, -0.9f, 0, 0.9f, -0.9f, 0, -0.9f, 0.9f, 0, 0.9f, 0.9f, 0};
static const float sten_col[2][4] = {{1, 0, 0, 1}, {0, 1, 0.5f, 1}};

/* pclear: clears of part of the frame (the scissor box: x, y, w, h, y
 * up): box 0's colour cleared to pclr_col, box 1's depth to 0; then the
 * blend case's quad (z 0) drawn in depth_col[1] with LESS, so not in box 1.
 * ogles2 draws such clears as rectangles (VC4: tools/clear_vc4's program). */
static const int pclr_box[2][4] = {{10, 10, 60, 40}, {50, 40, 50, 60}};
static const float pclr_col[4] = {1, 0.6f, 0, 1};

/* depthtex: the depth case's triangles drawn into a 128x128 depth texture
 * (OES_depth_texture: DEPTH_COMPONENT, UNSIGNED_INT; an FBO with only it),
 * then that texture drawn as the textured quad (tq_pos, tq_uv below), NEAREST,
 * CLAMP: its depths as greys. */

/* mipmap (texture.vert, texture.frag): a 32x32 RGBA8 texture with its six
 * levels, each a solid colour (mip_col), NEAREST_MIPMAP_NEAREST, CLAMP; four
 * quads of 32, 16, 8 and 4 pixels (mip_quad: 4 vertices of x, y each; uv 0
 * to 1 over each), so each shows the level the TMU picks for it. */
#define MIP_SIZE 32
#define MIP_LEVELS 6
static const unsigned char mip_col[MIP_LEVELS][4] = {
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}};
static const float mip_quad[4][8] = {
    {-0.9f, -0.5f, -0.4f, -0.5f, -0.9f, 0.0f, -0.4f, 0.0f},       /* 32 pixels */
    {-0.25f, -0.5f, 0.0f, -0.5f, -0.25f, -0.25f, 0.0f, -0.25f},   /* 16 */
    {0.25f, -0.5f, 0.375f, -0.5f, 0.25f, -0.375f, 0.375f, -0.375f}, /* 8 */
    {0.6f, -0.5f, 0.6625f, -0.5f, 0.6f, -0.4375f, 0.6625f, -0.4375f}, /* 4 */
};
static const float mip_uv[4 * 2] = {0, 0, 1, 0, 0, 1, 1, 1};

/* attrib (color.vert, color.frag): a triangle, its colours unsigned bytes,
 * normalized (attribute 1), smooth shaded. */
static const float attr_pos[3 * 3] = {-0.8f, -0.8f, 0, 0.8f, -0.6f, 0, 0.0f, 0.85f, 0};
static const unsigned char attr_col[3 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 32, 64, 255, 255};

/* tex565, texlum (texture.vert, texture.frag): the textured quad of
 * mesa_texture.c with a 16x16 texture of that format, NEAREST, CLAMP. */
static const float tq_pos[4 * 2] = {-0.75f, -0.75f, 0.75f, -0.75f, -0.75f, 0.75f, 0.75f, 0.75f};
static const float tq_uv[4 * 2] = {0, 0, 1, 0, 0, 1, 1, 1};
static unsigned short tex565_texel(int x, int y)
{
    return (unsigned short)((x * 2) << 11 | (y * 4) << 5 | ((x + y) & 1 ? 31 : 0));
}
static unsigned char texlum_texel(int x, int y)
{
    return (unsigned char)(x * 16 + y);
}

/* tex4444, tex5551, texla, etc1 (16x16), npot (13x7 RGBA8): more textures
 * on the same quad, as GL packs them (4444: R in the top bits, A in the
 * bottom; 5551: R at 11, A at 0; LUMINANCE_ALPHA: L then A; ETC1: 4x4 blocks
 * of 8 bytes in raster order, any 64 bits being a block). */
static unsigned short tex4444_texel(int x, int y)
{
    return (unsigned short)(x << 12 | y << 8 | ((x ^ y) & 15) << 4 | (15 - x));
}
static unsigned short tex5551_texel(int x, int y)
{
    return (unsigned short)((x * 2) << 11 | (y * 2) << 6 | ((x + y) & 31) << 1 | (x & 1));
}
static void texla_texel(int x, int y, unsigned char *la)
{
    la[0] = (unsigned char)(x * 16 + y);
    la[1] = (unsigned char)(255 - y * 16);
}
#define NPOT_W 13
#define NPOT_H 7
static void npot_texel(int x, int y, unsigned char *c)
{
    c[0] = (unsigned char)(x * 19);
    c[1] = (unsigned char)(y * 36);
    c[2] = (unsigned char)((x * y) & 1 ? 255 : 64);
    c[3] = 255;
}
static void etc1_block(int bx, int by, unsigned char *b)
{
    unsigned int v = (unsigned int)(by * 4 + bx + 1) * 2654435761u;
    int k;
    for (k = 0; k < 8; k++) {
        v = v * 1103515245u + 12345u;
        b[k] = (unsigned char)(v >> 16);
    }
}

/* rgb565: the blend case into an RGB565 renderbuffer. */

/* cube (cube.vert, cube.frag): an 8x8 RGBA cube map, each face one colour
 * (+X, -X, +Y, -Y, +Z, -Z in GL's order), six quads each sampling one face
 * straight on (36 vertices: position, direction). */
static const unsigned char cube_face_col[6][4] = {
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}};
static void cube_vertices(float *v) /* 36 x (x, y, z, dx, dy, dz) */
{
    static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float corner[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    int f, k;
    for (f = 0; f < 6; f++) {
        float x0 = -0.9f + 0.6f * (float)(f % 3), y0 = -0.8f + 0.9f * (float)(f / 3);
        for (k = 0; k < 6; k++) {
            float *o = v + (f * 6 + k) * 6;
            o[0] = x0 + 0.5f * corner[k][0];
            o[1] = y0 + 0.6f * corner[k][1];
            o[2] = 0;
            o[3] = dirs[f][0];
            o[4] = dirs[f][1];
            o[5] = dirs[f][2];
        }
    }
}

#endif
