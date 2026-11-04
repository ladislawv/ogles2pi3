/*
 * Mesa vc4's pictures of the cases of cases.h (docs/PI3_PLAN.md §3.1, §6.3):
 * the references tests/pi3/vc4core/draw.c compares vc4core's against. Linux,
 * built and run on the Pi in the reference boot:
 *   gcc -O2 -o mesa_cases mesa_cases.c -lEGL -lGLESv2
 *   ./mesa_cases SHADER_DIR OUT_DIR
 * Each case into a 128x128 RGBA8 renderbuffer with a D24S8 one, cleared to
 * (0, 0, .25, 1) and depth 1.0, read back as OUT_DIR/mesa_<case>.ppm. The
 * shaders are read from the files v3dglslc -g 21 compiles for vc4core
 * (flat.vert, point.vert, flat.frag), so the sources are the same.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "cases.h"

#define W 128
#define H 128

static const char *shader_dir, *out_dir;

static char *load(const char *name)
{
    char path[512];
    FILE *f;
    long n;
    char *s;
    snprintf(path, sizeof path, "%s/%s", shader_dir, name);
    if (!(f = fopen(path, "rb"))) {
        fprintf(stderr, "%s: cannot open\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = calloc(1, (size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n)
        exit(1);
    fclose(f);
    return s;
}

static GLuint shader(GLenum type, const char *name)
{
    GLuint s = glCreateShader(type);
    char *src = load(name);
    GLint ok = 0;
    glShaderSource(s, 1, (const char **)&src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s: %s\n", name, log);
        exit(1);
    }
    free(src);
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "col"); /* the second input, whichever it is (v3dglslc -b) */
    glBindAttribLocation(p, 1, "uv");
    glBindAttribLocation(p, 1, "dir");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "link %s + %s failed\n", vs, fs);
        exit(1);
    }
    return p;
}

static void begin(void)
{
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisableVertexAttribArray(1);
}

static void draw(GLuint p, const float *col, const float *v, GLenum mode, int n)
{
    glUseProgram(p);
    glUniform4fv(glGetUniformLocation(p, "color"), 1, col);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDrawArrays(mode, 0, n);
}

static void save(const char *name)
{
    static unsigned char px[W * H * 4];
    char path[512];
    FILE *f;
    int y;
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "%s: GL error\n", name);
        exit(1);
    }
    snprintf(path, sizeof path, "%s/mesa_%s.ppm", out_dir, name);
    if (!(f = fopen(path, "wb")))
        exit(1);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = H - 1; y >= 0; y--) { /* PPM rows top first; GL's bottom first */
        int x;
        for (x = 0; x < W; x++)
            fwrite(&px[(y * W + x) * 4], 1, 3, f);
    }
    fclose(f);
    printf("%s written\n", path);
}

int main(int argc, char **argv)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint fb, rb, zb, flat, point, color, texp, cube, tex;
    int k, x, y;

    if (argc < 3) {
        fprintf(stderr, "usage: %s SHADER_DIR OUT_DIR\n", argv[0]);
        return 2;
    }
    shader_dir = argv[1];
    out_dir = argv[2];
    dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
        return 1;
    eglBindAPI(EGL_OPENGL_ES_API);
    ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return 1;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
    glGenRenderbuffers(1, &zb);
    glBindRenderbuffer(GL_RENDERBUFFER, zb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, W, H);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    flat = program("flat.vert", "flat.frag");
    point = program("point.vert", "flat.frag");
    color = program("color.vert", "color.frag");
    texp = program("texture.vert", "texture.frag");
    cube = program("cube.vert", "cube.frag");

    begin();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    for (k = 0; k < 3; k++)
        draw(flat, depth_col[k], depth_tri[k], GL_TRIANGLES, 3);
    save("depth");

    begin();
    draw(flat, blend_col[0], blend_quad, GL_TRIANGLE_STRIP, 4);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw(flat, blend_col[1], blend_tri, GL_TRIANGLES, 3);
    save("blend");

    begin();
    glUseProgram(flat);
    glUniform4fv(glGetUniformLocation(flat, "color"), 1, idx_col);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, idx_vtx);
    glEnableVertexAttribArray(0);
    glDrawElements(GL_TRIANGLES, 9, GL_UNSIGNED_SHORT, idx_list);
    save("indices");

    begin();
    draw(flat, line_col, line_vtx, GL_LINES, 6);
    save("lines");

    begin();
    draw(point, point_col, point_vtx, GL_POINTS, 5);
    save("points");

    begin();
    glEnable(GL_SCISSOR_TEST);
    glScissor(sciss_box[0], sciss_box[1], sciss_box[2], sciss_box[3]);
    draw(flat, blend_col[0], blend_quad, GL_TRIANGLE_STRIP, 4);
    glDisable(GL_SCISSOR_TEST);
    save("scissor");

    begin();
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    draw(flat, sten_col[0], sten_tri, GL_TRIANGLES, 3);
    glStencilFunc(GL_EQUAL, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    draw(flat, sten_col[1], sten_quad, GL_TRIANGLE_STRIP, 4);
    save("stencil");

    begin();
    glUseProgram(color);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, attr_pos);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, attr_col);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    save("attrib");

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (k = 0; k < 7; k++) {
        static const char *const names[7] = {"tex565", "texlum", "tex4444", "tex5551", "texla", "etc1", "npot"};
        static unsigned short t16[16 * 16];
        static unsigned char t8[16 * 16 * 4];
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                t16[y * 16 + x] = k == 0 ? tex565_texel(x, y) : k == 2 ? tex4444_texel(x, y) : tex5551_texel(x, y);
                if (k == 1)
                    t8[y * 16 + x] = texlum_texel(x, y);
                else if (k == 4)
                    texla_texel(x, y, t8 + (y * 16 + x) * 2);
                else if (k == 5 && x < 4 && y < 4)
                    etc1_block(x, y, t8 + (y * 4 + x) * 8);
                else if (k == 6 && x < NPOT_W && y < NPOT_H)
                    npot_texel(x, y, t8 + (y * NPOT_W + x) * 4);
            }
        if (k == 0)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, t16);
        else if (k == 1)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 16, 16, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, t8);
        else if (k == 2)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, t16);
        else if (k == 3)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, t16);
        else if (k == 4)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 16, 16, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, t8);
        else if (k == 5)
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, 16, 16, 0, 16 * 16 / 2, t8);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NPOT_W, NPOT_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, t8);
        if (glGetError() != GL_NO_ERROR) {
            printf("%s: the upload failed: not written\n", names[k]);
            continue;
        }
        begin();
        glUseProgram(texp);
        glUniform1i(glGetUniformLocation(texp, "tex"), 0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tq_pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, tq_uv);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        save(names[k]);
    }

    /* rgb565: the blend case into an RGB565 renderbuffer (read back as
     * RGBA8, Mesa expanding the 5- and 6-bit values) */
    {
        GLuint f565, c565;
        glGenRenderbuffers(1, &c565);
        glBindRenderbuffer(GL_RENDERBUFFER, c565);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, W, H);
        glGenFramebuffers(1, &f565);
        glBindFramebuffer(GL_FRAMEBUFFER, f565);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c565);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            printf("rgb565: the framebuffer is not complete: not written\n");
        } else {
            begin();
            draw(flat, blend_col[0], blend_quad, GL_TRIANGLE_STRIP, 4);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw(flat, blend_col[1], blend_tri, GL_TRIANGLES, 3);
            save("rgb565");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
    }

    {
        static unsigned char face[8 * 8 * 4];
        static float cv[36 * 6];
        GLuint ct;
        glGenTextures(1, &ct);
        glBindTexture(GL_TEXTURE_CUBE_MAP, ct);
        for (k = 0; k < 6; k++) {
            for (x = 0; x < 64; x++)
                memcpy(face + x * 4, cube_face_col[k], 4);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + k, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, face);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cube_vertices(cv);
        begin();
        glUseProgram(cube);
        glUniform1i(glGetUniformLocation(cube, "tex"), 0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, cv);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, cv + 3);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        save("cube");
    }

    /* msaa: the depth case in a 4x multisampled 128x128 pbuffer (resolved
     * by the read), if the platform has such a configuration. */
    {
        static const EGLint want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_SAMPLE_BUFFERS, 1,
                                      EGL_SAMPLES, 4, EGL_NONE};
        static const EGLint pb_attr[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        EGLSurface pb;
        EGLContext mctx;
        if (!eglChooseConfig(dpy, want, &cfg, 1, &n) || n < 1) {
            printf("msaa: no 4x multisampled pbuffer configuration: not written\n");
        } else if ((pb = eglCreatePbufferSurface(dpy, cfg, pb_attr)) == EGL_NO_SURFACE ||
                   (mctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr)) == EGL_NO_CONTEXT ||
                   !eglMakeCurrent(dpy, pb, pb, mctx)) {
            printf("msaa: the pbuffer or its context failed: not written\n");
        } else {
            GLint samples = 0;
            glGetIntegerv(GL_SAMPLES, &samples);
            printf("msaa: %d samples\n", samples);
            flat = program("flat.vert", "flat.frag");
            begin();
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            for (k = 0; k < 3; k++)
                draw(flat, depth_col[k], depth_tri[k], GL_TRIANGLES, 3);
            save("msaa");
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(dpy, mctx);
            eglDestroySurface(dpy, pb);
        }
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return 0;
}
