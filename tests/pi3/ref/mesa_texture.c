/*
 * Mesa vc4's picture of one textured quad (PI3_PLAN P1, §6.1): the reference
 * for the probe's texture tests (tests/pi3_probe, "texture"), and with
 * VC4_DEBUG=qpu,qir the texturing shader and the uniforms it reads. Linux
 * only, built and run on the Pi (reference boot):
 *   gcc -O2 -o mesa_texture mesa_texture.c -lEGL -lGLESv2
 *   VC4_DEBUG=qpu,qir ./mesa_texture out.ppm 2> dump.txt
 *   ./mesa_texture out.ppm 5000     (draws 5000 times more, keeping the GPU busy)
 * Into a 128x128 RGBA8 renderbuffer cleared to dark blue: a quad from
 * (-0.75,-0.75) to (0.75,0.75) as a triangle strip, texture coordinates 0 to
 * 1, sampling a 16x16 RGBA8 texture (texel (x,y) = (x*16, y*16, 255 if x+y is
 * odd else 0, 255)) with NEAREST filtering, CLAMP_TO_EDGE, no mipmaps. Read
 * back and written as a binary PPM. Surfaceless EGL; nothing is shown.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define W 128
#define H 128
#define T 16

static const char *vs_src =
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(tex, v_uv); }\n";

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "compile: %s\n", log);
        exit(1);
    }
    return s;
}

int main(int argc, char **argv)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    static const GLfloat quad[] = { /* x, y, u, v */
        -0.75f, -0.75f, 0.0f, 0.0f, 0.75f, -0.75f, 1.0f, 0.0f,
        -0.75f, 0.75f, 0.0f, 1.0f, 0.75f, 0.75f, 1.0f, 1.0f };
    static unsigned char px[W * H * 4], tex[T * T * 4];
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint fb, rb, prog, to;
    GLint ok = 0;
    FILE *f;
    int i, x, y;

    dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                      : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        fprintf(stderr, "no surfaceless EGL display\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "no ES2 context\n");
        return 1;
    }

    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "framebuffer incomplete\n");
        return 1;
    }

    for (y = 0; y < T; y++)
        for (x = 0; x < T; x++) {
            unsigned char *t = &tex[(y * T + x) * 4];
            t[0] = (unsigned char)(x * 16);
            t[1] = (unsigned char)(y * 16);
            t[2] = (x + y) & 1 ? 255 : 0;
            t[3] = 255;
        }
    glGenTextures(1, &to);
    glBindTexture(GL_TEXTURE_2D, to);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "uv");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "link failed\n");
        return 1;
    }

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* argv[2]: draw again that many times (keeps the GPU busy, for reading
     * its registers meanwhile); the picture is the first draw's. */
    for (i = argc > 2 ? atoi(argv[2]) : 0; i > 0; i--) {
        static unsigned char again[W * H * 4];
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, again);
    }
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "GL error\n");
        return 1;
    }

    f = fopen(argc > 1 ? argv[1] : "mesa_texture.ppm", "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = H - 1; i >= 0; i--)
        for (x = 0; x < W; x++)
            fwrite(&px[(i * W + x) * 4], 1, 3, f);
    fclose(f);
    printf("pixel (64,64) = %02x %02x %02x %02x\n", px[(64 * W + 64) * 4], px[(64 * W + 64) * 4 + 1],
           px[(64 * W + 64) * 4 + 2], px[(64 * W + 64) * 4 + 3]);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return 0;
}
