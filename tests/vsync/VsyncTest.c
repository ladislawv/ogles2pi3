/*
    VsyncTest (Ikaruga §37d): is a vsync swap ever missed although the frame is ready early?
    A 640x480 32-bit screen with VSYNC, each frame a clear and aglSwapBuffers(); at the end the
    time between two swaps (histogram, ms) and the frames of the long ones.
    Built twice: -DUSE_OGLES2 (ogles2.library) or ogles3.library.

    VsyncTest [FRAMES=n] [PRI=n] [BUSY] [WORK=ms]
      FRAMES  swaps to time (default 3000)
      PRI     this task's priority while it runs (default 0)
      BUSY    a task at priority 15 that keeps the 68k busy 2 ms once a second (the repro)
      WORK    ms of CPU work before each frame (as a program would have; without it a clear-only
              frame lets every other swap return at once)
*/
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <graphics/displayinfo.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#ifdef USE_OGLES2
#include <proto/ogles2.h>
#include <GLES2/gl2.h>
#define LIBOPEN OGLES2Open
#define LIBCLOSE OGLES2Close
#define T_MODEID OGLES2_CCT_MODEID
#define T_VSYNC OGLES2_CCT_VSYNC
#define T_DEPTH OGLES2_CCT_DEPTH
#define LIBNAME "ogles2"
#else
#include <proto/ogles3.h>
#include <GLES3/gl3.h>
#define LIBOPEN OGLES3Open
#define LIBCLOSE OGLES3Close
#define T_MODEID OGLES3_CCT_MODEID
#define T_VSYNC OGLES3_CCT_VSYNC
#define T_DEPTH OGLES3_CCT_DEPTH
#define LIBNAME "ogles3"
#endif

struct GfxBase *GfxBase;
struct Device *TimerBase;
static struct timerequest tr;
static ULONG eclock_hz;
static volatile int busy_quit, busy_done;

static unsigned long long now_us(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((unsigned long long)ev.ev_hi << 32 | ev.ev_lo) * 1000000ull / eclock_hz;
}

static void busy_task(void)
{
    while (!busy_quit) {
        unsigned long long t;
        Delay(50);
        t = now_us();
        while (now_us() - t < 2000)
            ;
    }
    Forbid();
    busy_done = 1;
}

int main(void)
{
    LONG args[4] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"FRAMES/K/N,PRI/K/N,BUSY/S,WORK/K/N", args, NULL);
    ULONG frames = args[0] ? *(LONG *)args[0] : 3000, hist[64] = { 0 }, i, nlong = 0, longs[32];
    LONG pri = args[1] ? *(LONG *)args[1] : 0, work = args[3] ? *(LONG *)args[3] : 0;
    BYTE old_pri;
    void *ctx = NULL;
    ULONG err = 0, id = INVALID_ID;
    unsigned long long last = 0;
    struct Process *busy = NULL;
    struct EClockVal ev;

    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"VsyncTest");
        return 20;
    }
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!GfxBase || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) {
        PutStr((CONST_STRPTR)"VsyncTest: no graphics.library / timer.device\n");
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    eclock_hz = ReadEClock(&ev);
    if (!LIBOPEN()) {
        PutStr((CONST_STRPTR)"VsyncTest: needs " LIBNAME ".library\n");
        return 20;
    }
    while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        struct DimensionInfo dims;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, id) || dims.MaxDepth < 24 ||
            dims.Nominal.MaxX - dims.Nominal.MinX + 1 != 640 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != 480)
            continue;
        ctx = aglCreateContextTags2(&err, T_MODEID, id, T_VSYNC, TRUE, T_DEPTH, 0, TAG_DONE);
    }
    if (!ctx) {
        Printf((CONST_STRPTR)"VsyncTest: no 640x480 32-bit screen for " LIBNAME " (error %lu)\n", err);
        LIBCLOSE();
        return 20;
    }
    aglMakeCurrent(ctx);
    if (args[2])
        busy = CreateNewProcTags(NP_Entry, (ULONG)busy_task, NP_Name, (ULONG)"VsyncTest busy", NP_Priority, 15,
                                 TAG_DONE);
    old_pri = SetTaskPri(FindTask(NULL), pri);
    for (i = 0; i <= frames; i++) {
        unsigned long long t;
        if (work) {
            t = now_us();
            while (now_us() - t < (unsigned long long)work * 1000)
                ;
        }
        glClearColor((i & 63) / 63.0f, 0.2f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        aglSwapBuffers();
        t = now_us();
        if (i > 60) {   /* after the start */
            ULONG ms = (ULONG)((t - last + 500) / 1000);
            hist[ms < 63 ? ms : 63]++;
            if (ms >= 25 && nlong < 32)
                longs[nlong++] = i;
        }
        last = t;
    }
    SetTaskPri(FindTask(NULL), old_pri);
    if (busy) {
        busy_quit = 1;
        while (!busy_done)
            Delay(2);
    }
    aglDestroyContext(ctx);
    LIBCLOSE();
    Printf((CONST_STRPTR)LIBNAME ": %lu swaps, priority %ld, work %ld ms%s; time between two swaps, ms: frames",
           frames - 60, pri, work, (ULONG)(busy ? ", busy task" : ""));
    for (i = 0; i < 64; i++)
        if (hist[i])
            Printf((CONST_STRPTR)"  %lu: %lu", i, hist[i]);
    PutStr((CONST_STRPTR)"\nlong ones at frames:");
    for (i = 0; i < nlong; i++)
        Printf((CONST_STRPTR)" %lu", longs[i]);
    PutStr((CONST_STRPTR)"\n");
    CloseDevice((struct IORequest *)&tr);
    CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return 0;
}
