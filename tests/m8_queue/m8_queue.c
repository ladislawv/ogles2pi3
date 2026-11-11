/*
 * A framebuffer object's job queued behind the running render (job.c): on a
 * screen of its own (where the screen's render is left running), every
 * frame first clears a 64x64 texture to a colour of that frame's own, then
 * draws it over the screen. Every second frame reads the screen's middle
 * back: the frame before left its render running, so this frame's object
 * job was queued; the pixel must be this frame's colour (the queued render
 * ran before the screen's, and after the one before, which sampled the same
 * texture). Then the same with ENV:OGLES2/NoQueue's path as a control is
 * the package's second step. Ends with an object job queued and the context
 * destroyed at once (no swap), and checks that a second context's life
 * returns all memory.
 *   usage: m8_queue [FRAMES=60]
 * To see: a 640x480 screen changing colour every frame for about a second.
 * The shaders are tests/common's tex1 (their .v3dp: make test-v3dp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/ogles2.h>
#pragma GCC diagnostic ignored "-Wunused-function" /* perf_target.h's gears helpers */
#include "../perf/perf_target.h"

#include "../common/tex_shaders.h"

static int fails, passes;

static void check(int ok, const char *msg)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok)
        passes++;
    else
        fails++;
}

static GLuint link(void)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    const char *vs = tex1_vs, *fs = tex1_fs;
    glShaderSource(v, 1, &vs, 0);
    glCompileShader(v);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_texcoord");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* The frame's colour: each channel 0, 0x80 or 0xFF, never the same twice
 * running. */
static void colour(int i, GLfloat *c)
{
    static const GLfloat lv[3] = {0.0f, 128.0f / 255.0f, 1.0f};
    i = i % 26 + 1; /* 1..26: not black */
    c[0] = lv[i % 3];
    c[1] = lv[i / 3 % 3];
    c[2] = lv[i / 9 % 3];
}

static ULONG rgb(const GLfloat *c)
{
    return ((ULONG)(c[0] * 255.0f + 0.5f) << 16) | ((ULONG)(c[1] * 255.0f + 0.5f) << 8) |
           (ULONG)(c[2] * 255.0f + 0.5f);
}

static const GLfloat quad[] = { /* x y z u v */
    -1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1,
};

/* One context's life: frames as described above; last_queued: it ends with
 * an object's job queued, destroyed without a swap. */
static void run(ULONG mode, int frames, int last_queued)
{
    ULONG err = 0;
    void *ctx;
    GLuint prog, tex, fbo;
    GLfloat c[3];
    int i, bad = 0, reads = 0;
    char msg[160];
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, mode, OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
    if (!ctx) {
        sprintf(msg, "a screen of its own (aglCreateContext2 error %lu)", (unsigned long)err);
        check(0, msg);
        return;
    }
    aglMakeCurrent(ctx);
    if (!(prog = link())) {
        check(0, "the program links (its .v3dp beside the program)");
        aglDestroyContext(ctx);
        return;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, quad + 3);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "the texture's framebuffer is complete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (i = 0; i < frames; i++) {
        colour(i, c);
        /* the object's pass first: queued behind the frame before's render */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 64, 64);
        glClearColor(c[0], c[1], c[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, PG_W, PG_H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (i & 1) {
            UBYTE px[4];
            glReadPixels(PG_W / 2, PG_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            reads++;
            if ((((ULONG)px[0] << 16) | ((ULONG)px[1] << 8) | px[2]) != rgb(c)) {
                if (bad++ < 4) {
                    sprintf(msg, "frame %d: screen %02x%02x%02x, its colour %06lx", i, px[0], px[1], px[2],
                            (unsigned long)rgb(c));
                    check(0, msg);
                }
            }
        }
        aglSwapBuffers();
    }
    sprintf(msg, "%d frames, %d read back: each the frame's own colour (%d wrong)", frames, reads, bad);
    check(!bad, msg);
    if (last_queued) {
        /* An object's job queued, then the context destroyed with it. */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 64, 64);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT); /* starts the screen's job: the object's flushed (queued) */
    } else {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteProgram(prog);
    }
    aglDestroyContext(ctx);
}

int main(int argc, char **argv)
{
    struct pg_args a;
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    ULONG mode, before, after;
    char msg[120];

    memset(&a, 0, sizeof(a));
    a.fullscreen = 1;
    if (frames < 2)
        frames = 60;
    if (!pt_open(&a, "m8_queue") || !OGLES2Open()) {
        printf("needs %s\n", OGLES2NAME);
        pt_close();
        return 20;
    }
    if (!(mode = pt_find_mode())) {
        check(0, "a 640x480 32-bit screen mode");
    } else {
        run(mode, frames, 1); /* also the library's first-use memory */
        before = AvailMem(MEMF_ANY);
        run(mode, frames, 0);
        after = AvailMem(MEMF_ANY);
        /* within 4 KB: other tasks allocate meanwhile; a leaked slot of
         * lists or tile pools is megabytes */
        sprintf(msg, "a second context's life returns the memory (%ld bytes difference)", (long)(after - before));
        check((long)(after - before) > -4096 && (long)(after - before) < 4096, msg);
    }
    printf("%s: %d of %d checks passed\n", fails ? "FAILED" : "PASSED", passes, passes + fails);
    OGLES2Close();
    pt_close();
    return fails ? 10 : 0;
}
