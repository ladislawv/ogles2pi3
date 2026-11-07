/*
 * The disk cache's size (ogles2/compiler.c, cache_trim): the first compile of a
 * library's load removes the oldest of its own files when the cache is over
 * ENV:OGLES2/CacheKB, down to 3/4 of it, and touches nothing else.
 *   usage: m7_cache TRIM      100 files of 1 KB, the limit 64 KB (Run-M1 sets it):
 *                             the 52 oldest go, the rest and the others stay; files
 *                             added after the first compile stay this load
 *          m7_cache NOLIMIT   ENV:OGLES2/CacheKB 0: 100 more of them all stay
 *          m7_cache UNDER     30 files of 1 KB, under the limit: they all stay
 * The library must be freshly loaded (Avail FLUSH), and OGLES2Cache: assigned to an
 * empty drawer of this test's (Run-M1). Each compiles a program of its own: its
 * sources differ, so that no earlier run's file answers it.
 *
 * MIT licence (LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ogles2.h>

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

/* A file of the cache's kind, its date `age` days back (the larger, the older). */
static int make_fake(int series, int i, int age, const char *suffix)
{
    char path[64];
    static char kb[1024];
    struct DateStamp d;
    BPTR f;
    int ok;
    sprintf(path, "OGLES2Cache:%012x%012x%s", series, i, suffix);
    if (!(f = Open((STRPTR)path, MODE_NEWFILE)))
        return 0;
    ok = Write(f, kb, sizeof(kb)) == (LONG)sizeof(kb);
    Close(f);
    DateStamp(&d);
    d.ds_Days -= age;
    return ok && SetFileDate((STRPTR)path, &d);
}

static int fake_exists(int series, int i)
{
    char path[64];
    BPTR l;
    sprintf(path, "OGLES2Cache:%012x%012x.vc4p", series, i);
    if (!(l = Lock((STRPTR)path, ACCESS_READ)))
        return 0;
    UnLock(l);
    return 1;
}

static int plain_file(const char *name)
{
    char path[64];
    BPTR f;
    sprintf(path, "OGLES2Cache:%s", name);
    if (!(f = Open((STRPTR)path, MODE_NEWFILE)))
        return 0;
    Write(f, "keep", 4);
    Close(f);
    return 1;
}

static int plain_exists(const char *name)
{
    char path[64];
    BPTR l;
    sprintf(path, "OGLES2Cache:%s", name);
    if (!(l = Lock((STRPTR)path, ACCESS_READ)))
        return 0;
    UnLock(l);
    return 1;
}

/* Compiles a program whose sources no other run has: tag makes them differ. */
static int compile_one(const char *tag)
{
    static char vs[200], fs[200];
    const char *v = vs, *f = fs;
    GLuint sv, sf, p;
    GLint ok = 0;
    sprintf(vs, "attribute vec4 a; /* %s */\nvoid main() { gl_Position = a; }\n", tag);
    sprintf(fs, "precision mediump float; /* %s */\nvoid main() { gl_FragColor = vec4(1.0); }\n", tag);
    sv = glCreateShader(GL_VERTEX_SHADER);
    sf = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(sv, 1, &v, 0);
    glShaderSource(sf, 1, &f, 0);
    glCompileShader(sv);
    glCompileShader(sf);
    p = glCreateProgram();
    glAttachShader(p, sv);
    glAttachShader(p, sf);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(sv);
    glDeleteShader(sf);
    glDeleteProgram(p);
    return ok;
}

/* The oldest `gone` of series' n files removed, the rest there. */
static void expect_series(int series, int n, int gone, const char *what)
{
    char msg[120];
    int i, wrong_gone = 0, wrong_kept = 0;
    for (i = 0; i < n; i++) { /* file 0 is the oldest */
        if (i < gone ? fake_exists(series, i) : !fake_exists(series, i)) {
            if (i < gone)
                wrong_kept++;
            else
                wrong_gone++;
        }
    }
    sprintf(msg, "%s: of %d files the %d oldest gone (%d old ones left, %d new ones lost)", what, n, gone, wrong_kept, wrong_gone);
    check(!wrong_kept && !wrong_gone, msg);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "TRIM";
    void *ctx;
    ULONG e = 0;
    int i, ok = 1;
    printf("ogles2 M7: the disk cache's size, %s\n", mode);
    if (!OGLES2Open()) {
        printf("needs %s\n", OGLES2NAME);
        return 20;
    }
    ctx = aglCreateContextTags2(&e, OGLES2_CCT_PISTORM_WIDTH, 64, OGLES2_CCT_PISTORM_HEIGHT, 64, TAG_DONE);
    if (!ctx) {
        check(0, "a window context");
        OGLES2Close();
        return 20;
    }
    aglMakeCurrent(ctx);
    if (!strcmp(mode, "TRIM")) {
        /* File 0 the oldest; 100 KB against 64: down to 48 KB means 52 go. */
        for (i = 0; i < 100 && ok; i++)
            ok = make_fake(1, i, 100 - i, ".vc4p");
        ok = ok && plain_file("keepme.txt") && plain_file("short.vc4p") && plain_file("0123456789abcdef01234567.dat") &&
             plain_file("0123456789ABCDEF01234567.vc4p");
        check(ok, "the test's files made");
        check(compile_one("m7_cache trim 1"), "a program compiled");
        expect_series(1, 100, 52, "first compile, over the limit");
        check(plain_exists("keepme.txt"), "another program's file stays");
        check(plain_exists("short.vc4p"), "a shorter name stays");
        check(plain_exists("0123456789abcdef01234567.dat"), "another extension stays");
        check(plain_exists("0123456789ABCDEF01234567.vc4p"), "capital digits stay");
        /* Once a load: more now are not looked at until the next. */
        for (i = 0; i < 100 && ok; i++)
            ok = make_fake(2, i, 200 - i, ".vc4p");
        check(compile_one("m7_cache trim 2"), "a second program compiled");
        expect_series(2, 100, 0, "a second compile of the load");
    } else if (!strcmp(mode, "NOLIMIT")) {
        for (i = 0; i < 100 && ok; i++)
            ok = make_fake(3, i, 300 - i, ".vc4p");
        check(ok, "the test's files made");
        check(compile_one("m7_cache nolimit"), "a program compiled");
        expect_series(3, 100, 0, "with the limit 0");
    } else {
        for (i = 0; i < 30 && ok; i++)
            ok = make_fake(4, i, 30 - i, ".vc4p");
        check(ok, "the test's files made");
        check(compile_one("m7_cache under"), "a program compiled");
        expect_series(4, 30, 0, "under the limit");
    }
    aglDestroyContext(ctx);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "PASS", checks - failures, checks);
    return failures ? 10 : 0;
}
