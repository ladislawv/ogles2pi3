/*
    The 16-bit upload report's test (2026-11-19, a user's, as sent; ogles3's
    tests/perf/upload_test.c, its USE_OGLES2 build here: package
    tests/a1200/upload, docs/PI3_PLAN.md's "Plan: ogles3's 16-bit upload fixes").
    UploadTest (BorderDown section 18): does a 16-bit texture upload cost more than a 32-bit one in ogles3?
    For each format a GL_TEXTURE_2D_ARRAY is made with glTexStorage3D (timed apart), then REPS tiles of
    SIZE x SIZE are uploaded with glTexSubImage3D (the calls timed, then glFinish timed).
    The same texel count for every format: a 16-bit tile is half the bytes of a 32-bit one.

    UploadTest [SIZE=n] [REPS=n] [ATLAS=n] [LAYERS=n] [IMAGE]
      SIZE    tile side in texels (default 256)
      REPS    tiles uploaded per format (default 64)
      ATLAS   array side (default 1024)
      LAYERS  array layers (default 4)
      IMAGE   (added to the report's test) each tile a whole SIZE x SIZE
              GL_TEXTURE_2D given with glTexImage2D and drawn once (a small
              triangle): a level 0 reaches the GPU's copy at the next draw.
*/
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#ifdef USE_OGLES2   /* GLES2: no arrays, a 2D texture (glTexImage2D + glTexSubImage2D), internal = format */
#include <proto/ogles2.h>
#include <GLES2/gl2.h>
#define OGLES3Open OGLES2Open
#define OGLES3Close OGLES2Close
#define OGLES3_CCT_MODEID OGLES2_CCT_MODEID
#define OGLES3_CCT_VSYNC OGLES2_CCT_VSYNC
#define OGLES3_CCT_DEPTH OGLES2_CCT_DEPTH
#define GL_RGBA8 GL_RGBA
#define GL_RGB565 GL_RGB
#define GL_RGB5_A1 GL_RGBA
#define GL_RGBA4 GL_RGBA
#define LIBNAME "ogles2"
#else
#include <proto/ogles3.h>
#include <GLES3/gl3.h>
#define LIBNAME "ogles3"
#endif

struct GfxBase *GfxBase;
struct Device *TimerBase;
static struct timerequest tr;
static ULONG eclock_hz;

static unsigned long long now_us(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((unsigned long long)ev.ev_hi << 32 | ev.ev_lo) * 1000000ull / eclock_hz;
}

static const struct {
    const char *name;
    GLenum internal, format, type;
    ULONG bytes;
} fmts[] = {
    { "RGBA8888 (GL_RGBA8,   RGBA/UNSIGNED_BYTE)         ", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4 },
    { "RGB565   (GL_RGB565,  RGB/UNSIGNED_SHORT_5_6_5)   ", GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2 },
    { "RGBA5551 (GL_RGB5_A1, RGBA/UNSIGNED_SHORT_5_5_5_1)", GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2 },
    { "RGBA4444 (GL_RGBA4,   RGBA/UNSIGNED_SHORT_4_4_4_4)", GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2 },
#ifndef USE_OGLES2   /* 8-bit channels into 16-bit textures (GLES3 allows it) */
    { "RGB565   (GL_RGB565,  RGB/UNSIGNED_BYTE)          ", GL_RGB565, GL_RGB, GL_UNSIGNED_BYTE, 3 },
    { "RGBA5551 (GL_RGB5_A1, RGBA/UNSIGNED_BYTE)         ", GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_BYTE, 4 },
    { "RGBA4444 (GL_RGBA4,   RGBA/UNSIGNED_BYTE)         ", GL_RGBA4, GL_RGBA, GL_UNSIGNED_BYTE, 4 },
#endif
};

/* microseconds -> "ms.cc" */
static void put_ms(const char *label, unsigned long long us)
{
    Printf((CONST_STRPTR)"  %s %4lu.%02lu ms", (ULONG)label, (ULONG)(us / 1000), (ULONG)(us % 1000 / 10));
}

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"SIZE/K/N,REPS/K/N,ATLAS/K/N,LAYERS/K/N,IMAGE/S", args, NULL);
    BOOL image = args[4] != 0;
    static const GLfloat tri[] = { -0.1f, -0.1f, 0.1f, -0.1f, -0.1f, 0.1f };
    static const char *vs = "attribute vec2 p; varying vec2 uv; "
                            "void main() { uv = p * 5.0 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
    static const char *fs = "precision mediump float; varying vec2 uv; uniform sampler2D s; "
                            "void main() { gl_FragColor = texture2D(s, uv); }";
    GLuint prog = 0;
    ULONG size = args[0] ? *(LONG *)args[0] : 256, reps = args[1] ? *(LONG *)args[1] : 64;
    ULONG atlas = args[2] ? *(LONG *)args[2] : 1024, layers = args[3] ? *(LONG *)args[3] : 4;
    ULONG err = 0, id = INVALID_ID, f, r, i, per_row;
    void *ctx = NULL;
    UBYTE *buf;
    struct EClockVal ev;

    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"UploadTest");
        return 20;
    }
    if (size > atlas)
        size = atlas;
    per_row = atlas / size;
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!GfxBase || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) {
        PutStr((CONST_STRPTR)"UploadTest: no graphics.library / timer.device\n");
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    eclock_hz = ReadEClock(&ev);
    buf = AllocVec(size * size * 4, MEMF_ANY);
    if (!buf || !OGLES3Open()) {
        PutStr((CONST_STRPTR)"UploadTest: no memory or no ogles3.library\n");
        return 20;
    }
    for (i = 0; i < size * size * 4; i++)
        buf[i] = (UBYTE)(i * 7 + (i >> 9));
    while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        struct DimensionInfo dims;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, id) || dims.MaxDepth < 24 ||
            dims.Nominal.MaxX - dims.Nominal.MinX + 1 != 640 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != 480)
            continue;
        ctx = aglCreateContextTags2(&err, OGLES3_CCT_MODEID, id, OGLES3_CCT_VSYNC, FALSE, OGLES3_CCT_DEPTH, 0,
                                    TAG_DONE);
    }
    if (!ctx) {
        Printf((CONST_STRPTR)"UploadTest: no 640x480 32-bit screen (error %lu)\n", err);
        OGLES3Close();
        return 20;
    }
    aglMakeCurrent(ctx);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    if (image) {
        GLuint v = glCreateShader(GL_VERTEX_SHADER), g = glCreateShader(GL_FRAGMENT_SHADER);
        GLint ok = 0;
        glShaderSource(v, 1, &vs, NULL);
        glCompileShader(v);
        glShaderSource(g, 1, &fs, NULL);
        glCompileShader(g);
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, g);
        glBindAttribLocation(prog, 0, "p");
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            PutStr((CONST_STRPTR)"UploadTest: the IMAGE mode's program did not link\n");
            image = FALSE;
        } else {
            glUseProgram(prog);
            glUniform1i(glGetUniformLocation(prog, "s"), 0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
            glEnableVertexAttribArray(0);
        }
    }
    if (image)
        Printf((CONST_STRPTR)"UploadTest " LIBNAME ": %lu glTexImage2D of %lux%lu per format, each drawn once\n", reps,
               size, size);
    else
    Printf((CONST_STRPTR)"UploadTest " LIBNAME ": %lu tiles of %lux%lu per format, array %lux%lux%lu\n", reps, size, size,
           atlas, atlas, layers);
    for (r = 0; r < 2; r++) {   /* pass 0 warms up, pass 1 is the measure */
        Printf((CONST_STRPTR)"pass %lu%s\n", r, (ULONG)(r ? " (measure)" : " (warm-up)"));
        for (f = 0; f < sizeof fmts / sizeof fmts[0]; f++) {
            GLuint tex;
            unsigned long long t0, t1, t2, t3, up;
            ULONG kb = (ULONG)((unsigned long long)reps * size * size * fmts[f].bytes >> 10);
            glFinish();
            t0 = now_us();
            glGenTextures(1, &tex);
            if (image) {
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            } else {
#ifdef USE_OGLES2
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, fmts[f].internal, atlas, atlas, 0, fmts[f].format, fmts[f].type, NULL);
#else
                glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, fmts[f].internal, atlas, atlas, layers);
#endif
            }
            glFinish();
            t1 = now_us();
            for (i = 0; i < reps; i++) {
                if (image) {
                    glTexImage2D(GL_TEXTURE_2D, 0, fmts[f].internal, size, size, 0, fmts[f].format, fmts[f].type, buf);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    continue;
                }
#ifdef USE_OGLES2
                ULONG cell = i % (per_row * per_row);
                glTexSubImage2D(GL_TEXTURE_2D, 0, cell % per_row * size, cell / per_row * size, size, size,
                                fmts[f].format, fmts[f].type, buf);
#else
                ULONG cell = i % (per_row * per_row * layers);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, cell % per_row * size, cell / per_row % per_row * size,
                                cell / (per_row * per_row), size, size, 1, fmts[f].format, fmts[f].type, buf);
#endif
            }
            t2 = now_us();
            glFinish();
            t3 = now_us();
            up = t3 - t1;
            Printf((CONST_STRPTR)"%s %6lu KB", (ULONG)fmts[f].name, kb);
            put_ms("create", t1 - t0);
            put_ms("calls", t2 - t1);
            put_ms("finish", t3 - t2);
            Printf((CONST_STRPTR)"  -> %lu KB/s, %lu Ktexel/s, err 0x%lx\n",
                   (ULONG)(up ? (unsigned long long)kb * 1000000ull / up : 0),
                   (ULONG)(up ? (unsigned long long)reps * size * size * 1000ull / up : 0), (ULONG)glGetError());
            glDeleteTextures(1, &tex);
        }
    }
    aglDestroyContext(ctx);
    OGLES3Close();
    FreeVec(buf);
    CloseDevice((struct IORequest *)&tr);
    CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return 0;
}
