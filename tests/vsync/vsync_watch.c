/*
 * vsync_watch: measures what a viewer of a vsync program sees, from the
 * Pi's display scaler (HVS, channel 1: SCALER_DISPSTAT1's frame counter and
 * line; only read), with any ogles2.library: a 640x480 32-bit screen,
 * VSYNC on, a white bar crossing it at 480 pixels a second (its place from
 * the time, as m6_vsync's), drawn with glClear and a scissor (no shaders).
 * After each swap the refresh the frame went out at (the counter; at the
 * last line, the one about to start) and the line. Reported:
 *   skipped   refreshes without a new frame (the previous one shown twice:
 *             the bar stops for a refresh, a stutter)
 *   doubled   frames that went out at the same refresh as the one before
 *             (one of them never shown)
 *   late      frames whose swap returned after the scan had begun (line >
 *             16: their render started after the picture's top was out)
 *   judder    refreshes whose bar moved by more than 2 ms of motion off one
 *             refresh's (the place is taken from the time when the frame
 *             is begun)
 *
 *   vsync_watch [SECONDS=n] [WORK=ms] [BUSY] [PRI=n]
 *     BUSY: a task at priority 15 that spins 2 ms once a second (the report)
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/ogles2.h>
#include <GLES2/gl2.h>

struct GfxBase *GfxBase;

static inline ULONG le32(ULONG v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
#define PERI(off) le32(*(volatile ULONG *)(0xF2000000 + (off)))
#define STAT() PERI(0x400000 + 0x58)
#define LINE(s) ((s)&0xfff)
#define FC(s) (((s) >> 12) & 63)
static inline ULONG now_us(void)
{
    return PERI(0x3004);
}

static volatile int busy_quit, busy_done;
static void busy_task(void)
{
    while (!busy_quit) {
        ULONG t;
        Delay(50);
        t = now_us();
        while (now_us() - t < 2000)
            ;
    }
    Forbid();
    busy_done = 1;
}

#define MAXF 4096
static ULONG f_pos[MAXF], f_ret[MAXF], f_stat[MAXF];

int main(void)
{
    LONG args[4] = {0};
    struct RDArgs *rda = ReadArgs((CONST_STRPTR) "SECONDS/K/N,WORK/K/N,BUSY/S,PRI/K/N", args, NULL);
    ULONG seconds, n = 0, i, err = 0, id = INVALID_ID, lines, period = 16667;
    LONG work, pri;
    void *ctx = NULL;
    struct Process *busy = NULL;
    BYTE old_pri;
    ULONG skipped = 0, doubled = 0, late = 0, very_late = 0, judder = 0, maxdev = 0, refreshes = 0, maxline = 0;
    ULONG skip_at[16], nskip = 0;
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR) "vsync_watch");
        return 20;
    }
    seconds = args[0] ? *(LONG *)args[0] : 30;
    work = args[1] ? *(LONG *)args[1] : 0;
    pri = args[3] ? *(LONG *)args[3] : 0;
    lines = le32(*(volatile ULONG *)(0xF2400000 + 0x50)) & 0xfff;
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        PutStr((CONST_STRPTR) "vsync_watch: needs graphics.library and ogles2.library\n");
        return 20;
    }
    while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        struct DimensionInfo dims;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, id) || dims.MaxDepth < 24 ||
            dims.Nominal.MaxX - dims.Nominal.MinX + 1 != 640 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != 480)
            continue;
        ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, id, OGLES2_CCT_VSYNC, TRUE, OGLES2_CCT_DEPTH, 0,
                                    TAG_DONE);
    }
    if (!ctx) {
        Printf((CONST_STRPTR) "vsync_watch: no 640x480 32-bit screen (error %lu)\n", err);
        OGLES2Close();
        return 20;
    }
    aglMakeCurrent(ctx);
    glViewport(0, 0, 640, 480);
    if (args[2])
        busy = CreateNewProcTags(NP_Entry, (ULONG)busy_task, NP_Name, (ULONG) "vsync_watch busy", NP_Priority, 15,
                                 TAG_DONE);
    old_pri = SetTaskPri(FindTask(NULL), pri);
    {
        ULONG t_start = now_us(), frames = 0;
        while (now_us() - t_start < seconds * 1000000UL) {
            ULONG t = now_us(), x;
            if (work)
                while (now_us() - t < (ULONG)work * 1000)
                    ;
            x = (ULONG)((unsigned long long)(t - t_start) * 480 / 1000000 % (640 - 48));
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.125f, 0.19f, 0.25f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLint)x, 0, 48, 400);
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            aglSwapBuffers();
            if (frames++ >= 60 && n < MAXF) { /* after the start */
                f_stat[n] = STAT();
                f_ret[n] = now_us();
                f_pos[n] = t;
                n++;
            }
        }
    }
    SetTaskPri(FindTask(NULL), old_pri);
    if (busy) {
        busy_quit = 1;
        while (!busy_done)
            Delay(2);
    }
    aglDestroyContext(ctx);
    OGLES2Close();
    /* The refresh each frame went out at: the counter, the next one at the
     * last line (the counter moves just after it). */
    for (i = 1; i < n; i++) {
        ULONG s0 = f_stat[i - 1], s1 = f_stat[i], r, line = LINE(s1);
        ULONG fc0 = (FC(s0) + (LINE(s0) >= lines ? 4 : 0)) & 63, fc1 = (FC(s1) + (line >= lines ? 4 : 0)) & 63;
        r = ((fc1 - fc0) & 63) / 4;
        if (f_ret[i] - f_ret[i - 1] > 200000) /* past the counter's 16 refreshes */
            r = (f_ret[i] - f_ret[i - 1] + period / 2) / period;
        refreshes += r;
        if (r == 0)
            doubled++;
        if (r > 1) {
            skipped += r - 1;
            if (nskip < 16)
                skip_at[nskip++] = i;
        }
        if (line > 16 && line < lines) {
            late++;
            if (line >= lines / 8)
                very_late++;
            if (line > maxline)
                maxline = line;
        }
        if (r) {
            /* The bar's motion between the two refreshes, against r refreshes' */
            ULONG moved = f_pos[i] - f_pos[i - 1], want = r * period, dev = moved > want ? moved - want : want - moved;
            if (dev > 2000)
                judder++;
            if (dev > maxdev)
                maxdev = dev;
        }
    }
    Printf((CONST_STRPTR) "%lu s, work %ld ms%s, priority %ld: %lu frames over %lu refreshes\n", seconds, work,
           (ULONG)(busy ? ", busy task" : ""), pri, n, refreshes);
    Printf((CONST_STRPTR) "  skipped refreshes %lu, doubled frames %lu, late %lu (past an eighth %lu, at most line %lu "
                          "of %lu), judder %lu (most %lu us)\n",
           skipped, doubled, late, very_late, maxline, lines, judder, maxdev);
    if (nskip) {
        PutStr((CONST_STRPTR) "  skips at frames:");
        for (i = 0; i < nskip; i++)
            Printf((CONST_STRPTR) " %lu", skip_at[i]);
        PutStr((CONST_STRPTR) "\n");
    }
    CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return 0;
}
