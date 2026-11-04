/*
 * Mesa vc4's OpenGL ES 2.0 strings and limits on the Raspberry Pi 3 (PI3_PLAN
 * P0, §6.1): the reference the VC4 backend is compared against. Linux only,
 * built and run on the Pi (reference boot, Mesa's vc4 driver):
 *   gcc -O2 -o mesa_limits mesa_limits.c -lEGL -lGLESv2 && ./mesa_limits
 * A surfaceless EGL context; nothing is shown.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef GL_MAX_TEXTURE_LOD_BIAS_EXT
#define GL_MAX_TEXTURE_LOD_BIAS_EXT 0x84FD /* EXT_texture_lod_bias */
#endif

static void ints(const char *name, GLenum pname, int n)
{
    GLint v[16];
    int k;
    memset(v, 0, sizeof v);
    glGetIntegerv(pname, v);
    printf("%-40s", name);
    for (k = 0; k < n; k++)
        printf(" %d", v[k]);
    printf("%s\n", glGetError() ? "  (error)" : "");
}

static void floats(const char *name, GLenum pname, int n)
{
    GLfloat v[4] = { 0, 0, 0, 0 };
    int k;
    glGetFloatv(pname, v);
    printf("%-40s", name);
    for (k = 0; k < n; k++)
        printf(" %g", v[k]);
    printf("%s\n", glGetError() ? "  (error)" : "");
}

static void precision(const char *name, GLenum shader, GLenum type)
{
    GLint range[2] = { 0, 0 }, prec = 0;
    glGetShaderPrecisionFormat(shader, type, range, &prec);
    printf("%-40s range %d %d precision %d\n", name, range[0], range[1], prec);
}

#define I(p, n) ints(#p, p, n)
#define F(p, n) floats(#p, p, n)

int main(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLDisplay dpy;
    EGLContext ctx;
    EGLint major, minor;
    GLint n = 0, k, fmts[64];
    const char *ext;

    dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                      : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "no surfaceless EGL display\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "no ES2 context (0x%x)\n", eglGetError());
        return 1;
    }

    printf("GL_VENDOR                                %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER                              %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION                               %s\n", glGetString(GL_VERSION));
    printf("GL_SHADING_LANGUAGE_VERSION              %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    I(GL_MAX_VERTEX_ATTRIBS, 1);
    I(GL_MAX_VERTEX_UNIFORM_VECTORS, 1);
    I(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 1);
    I(GL_MAX_VARYING_VECTORS, 1);
    I(GL_MAX_TEXTURE_IMAGE_UNITS, 1);
    I(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 1);
    I(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 1);
    I(GL_MAX_TEXTURE_SIZE, 1);
    I(GL_MAX_CUBE_MAP_TEXTURE_SIZE, 1);
    I(GL_MAX_RENDERBUFFER_SIZE, 1);
    I(GL_MAX_VIEWPORT_DIMS, 2);
    I(GL_SUBPIXEL_BITS, 1);
    F(GL_ALIASED_POINT_SIZE_RANGE, 2);
    F(GL_ALIASED_LINE_WIDTH_RANGE, 2);
    {   /* the read format needs a framebuffer: an RGBA8 renderbuffer's */
        GLuint fb, rb;
        glGenFramebuffers(1, &fb);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, 64, 64);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        I(GL_IMPLEMENTATION_COLOR_READ_FORMAT, 1);
        I(GL_IMPLEMENTATION_COLOR_READ_TYPE, 1);
        I(GL_RED_BITS, 1);
        I(GL_ALPHA_BITS, 1);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fb);
        glDeleteRenderbuffers(1, &rb);
    }
    I(GL_NUM_SHADER_BINARY_FORMATS, 1);
    I(GL_NUM_PROGRAM_BINARY_FORMATS_OES, 1);
    I(GL_SHADER_COMPILER, 1);
    I(GL_MAX_SAMPLES_EXT, 1);
    I(GL_MAX_DRAW_BUFFERS_EXT, 1);
    I(GL_MAX_COLOR_ATTACHMENTS_EXT, 1);
    F(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 1);
    F(GL_MAX_TEXTURE_LOD_BIAS_EXT, 1);
    precision("VS LOW_FLOAT", GL_VERTEX_SHADER, GL_LOW_FLOAT);
    precision("VS MEDIUM_FLOAT", GL_VERTEX_SHADER, GL_MEDIUM_FLOAT);
    precision("VS HIGH_FLOAT", GL_VERTEX_SHADER, GL_HIGH_FLOAT);
    precision("VS HIGH_INT", GL_VERTEX_SHADER, GL_HIGH_INT);
    precision("FS LOW_FLOAT", GL_FRAGMENT_SHADER, GL_LOW_FLOAT);
    precision("FS MEDIUM_FLOAT", GL_FRAGMENT_SHADER, GL_MEDIUM_FLOAT);
    precision("FS HIGH_FLOAT", GL_FRAGMENT_SHADER, GL_HIGH_FLOAT);
    precision("FS HIGH_INT", GL_FRAGMENT_SHADER, GL_HIGH_INT);
    while (glGetError())
        ;

    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    if (n > 64)
        n = 64;
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, fmts);
    printf("GL_COMPRESSED_TEXTURE_FORMATS            %d:", n);
    for (k = 0; k < n; k++)
        printf(" 0x%04X", fmts[k]);
    printf("\n");

    ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("GL_EXTENSIONS\n");
    for (; ext && *ext; ) {
        const char *e = strchr(ext, ' ');
        int len = e ? (int)(e - ext) : (int)strlen(ext);
        if (len)
            printf("  %.*s\n", len, ext);
        ext += len + (e ? 1 : 0);
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return 0;
}
