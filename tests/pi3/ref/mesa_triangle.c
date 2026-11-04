/*
 * Mesa vc4's picture of one triangle (PI3_PLAN P0/P1, §6.1): the reference the
 * bare-GPU probe (tests/pi3_probe/) is compared against, and with
 * VC4_DEBUG=qpu,cl,qir,nir,shaderdb the QPU code and command lists Mesa makes
 * for it. Linux only, built and run on the Pi (reference boot):
 *   gcc -O2 -o mesa_triangle mesa_triangle.c -lEGL -lGLESv2
 *   VC4_DEBUG=qpu,cl ./mesa_triangle out.ppm 2> dump.txt
 * Into a 128x128 RGBA8 renderbuffer (2x2 tiles of 64x64): cleared to dark
 * blue, then one triangle of constant colour across all four tiles; read back
 * and written as a binary PPM. Surfaceless EGL; nothing is shown.
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

static const char *vs_src =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0); }\n";

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
    static const GLfloat tri[] = { -0.75f, -0.75f, 0.75f, -0.5f, -0.25f, 0.75f };
    static unsigned char px[W * H * 4];
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint fb, rb, prog;
    GLint ok = 0;
    FILE *f;
    int i;

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

    prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "pos");
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
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "GL error\n");
        return 1;
    }

    /* PPM rows top first; GL's are bottom first. */
    f = fopen(argc > 1 ? argv[1] : "mesa_triangle.ppm", "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = H - 1; i >= 0; i--) {
        int x;
        for (x = 0; x < W; x++)
            fwrite(&px[(i * W + x) * 4], 1, 3, f);
    }
    fclose(f);
    printf("pixel (64,64) = %02x %02x %02x %02x, pixel (2,2) = %02x %02x %02x %02x\n",
           px[(64 * W + 64) * 4], px[(64 * W + 64) * 4 + 1], px[(64 * W + 64) * 4 + 2],
           px[(64 * W + 64) * 4 + 3], px[(2 * W + 2) * 4], px[(2 * W + 2) * 4 + 1],
           px[(2 * W + 2) * 4 + 2], px[(2 * W + 2) * 4 + 3]);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return 0;
}
