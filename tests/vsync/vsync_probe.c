/*
 * vsync_probe: how the swap's wait for the refresh behaves on the PiStorm's
 * RTG (the report "aglSwapBuffers with OGLES3_CCT_VSYNC sometimes waits two
 * vblanks"), without GL. A 640x480 32-bit screen of its own; the Pi's
 * display scaler (HVS, channel 1: HDMI0) gives the truth: SCALER_DISPSTAT1
 * holds the line it outputs (bits 11:0, VideoCore.card's GetVBeamPos) and
 * a frame counter (bits 17:12, what VideoCore.card measures the refresh
 * with). Only reads of the HVS; nothing is written to any register.
 *
 *   vsync_probe HVS                       the HVS's lines and frame counter, 5 frames
 *   vsync_probe BOVP|TOF|CNT [FRAMES=n] [PRI=n] [WORK=us] [DPRI=n DSPIN=us DPERIOD=ms] [OUT=file]
 *     BOVP: WaitBOVP on the screen, TOF: WaitTOF, CNT: the HVS's frame
 *     counter (a timer sleep to just before the next frame, then polling);
 *     each frame WORK us of CPU work first, then the wait. DPRI: a task at
 *     that priority wakes every DPERIOD ms and spins DSPIN us (its phase
 *     against the refresh drifts). A task at priority -1 counts what the
 *     CPU leaves idle. OUT: every wait and every spin as text lines.
 *   vsync_probe IDLE [FRAMES=s]           the idle counter alone for 5 s (or s), the baseline
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;

static inline ULONG le32(ULONG v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
#define PERI(off) le32(*(volatile ULONG *)(0xF2000000 + (off)))
#define HVS 0x400000
#define DISPSTAT1() PERI(HVS + 0x58)
#define LINE(s) ((s)&0xfff)
#define FC(s) (((s) >> 12) & 63)
static inline ULONG now_us(void) /* the Pi's system timer, 1 MHz */
{
    return PERI(0x3004);
}
static void spin_us(ULONG us)
{
    ULONG t = now_us();
    while (now_us() - t < us)
        ;
}

/* ---- the idle counter (priority -1) and the disturber ---- */
static volatile LONG quit_tasks;
static volatile ULONG idle_n;
static volatile int idle_done, dist_done;
static LONG dpri, dspin, dperiod;
#define MAXD 4096
static ULONG d_t0[MAXD], d_t1[MAXD];
static volatile ULONG nd;

static void idle_main(void)
{
    while (!quit_tasks)
        idle_n++;
    Forbid();
    idle_done = 1;
}

static void dist_main(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : 0;
    if (tr && !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
        while (!quit_tasks) {
            tr->tr_node.io_Command = TR_ADDREQUEST;
            tr->tr_time.tv_secs = (ULONG)dperiod / 1000;
            tr->tr_time.tv_micro = (ULONG)dperiod % 1000 * 1000;
            DoIO((struct IORequest *)tr);
            if (nd < MAXD)
                d_t0[nd] = now_us();
            spin_us((ULONG)dspin);
            if (nd < MAXD) {
                d_t1[nd] = now_us();
                nd++;
            }
        }
        CloseDevice((struct IORequest *)tr);
    }
    if (tr)
        DeleteIORequest((struct IORequest *)tr);
    if (port)
        DeleteMsgPort(port);
    Forbid();
    dist_done = 1;
}

static void tasks_start(BOOL dist)
{
    quit_tasks = 0;
    idle_n = 0;
    idle_done = 0;
    dist_done = !dist;
    nd = 0;
    if (!CreateNewProcTags(NP_Entry, (ULONG)idle_main, NP_Name, (ULONG) "vsync_probe idle", NP_Priority, -1,
                           NP_StackSize, 4096, TAG_DONE))
        idle_done = 1;
    if (dist && !CreateNewProcTags(NP_Entry, (ULONG)dist_main, NP_Name, (ULONG) "vsync_probe disturber",
                                   NP_Priority, dpri, NP_StackSize, 8192, TAG_DONE))
        dist_done = 1;
}

static void tasks_stop(void)
{
    quit_tasks = 1;
    while (!idle_done || !dist_done)
        Delay(2);
}

/* ---- HVS: 5 frames of the line and the frame counter ---- */
static void hvs(void)
{
    int k;
    ULONG s, prev, t, t0, tstart, maxline = 0, nfc = 0, tfc[8];
    ULONG fc_line_before[8], fc_line_after[8], wrap_t[8], wrap_before[8], wrap_after[8], nwrap = 0;
    ULONG run_line = 0, run_t = 0, longest_run = 0, longest_line = 0, nsamples = 0;
    for (k = 0; k < 0x80; k += 16)
        printf("HVS +0x%02x: %08lx %08lx %08lx %08lx\n", k, (unsigned long)PERI(HVS + k),
               (unsigned long)PERI(HVS + k + 4), (unsigned long)PERI(HVS + k + 8), (unsigned long)PERI(HVS + k + 12));
    Forbid(); /* about 85 ms */
    prev = DISPSTAT1();
    t0 = tstart = now_us();
    run_line = LINE(prev);
    run_t = t0;
    while (nfc < 6 && (t = now_us()) - tstart < 200000) {
        s = DISPSTAT1();
        nsamples++;
        if (LINE(s) > maxline)
            maxline = LINE(s);
        if (FC(s) != FC(prev)) {
            tfc[nfc] = t;
            fc_line_before[nfc] = LINE(prev);
            fc_line_after[nfc] = LINE(s);
            nfc++;
        }
        if (LINE(s) < LINE(prev) && nwrap < 8) {
            wrap_t[nwrap] = t;
            wrap_before[nwrap] = LINE(prev);
            wrap_after[nwrap] = LINE(s);
            nwrap++;
        }
        if (LINE(s) != run_line) {
            if (t - run_t > longest_run) {
                longest_run = t - run_t;
                longest_line = run_line;
            }
            run_line = LINE(s);
            run_t = t;
        }
        prev = s;
    }
    Permit();
    printf("HVS: %lu samples in %lu us, highest line %lu\n", (unsigned long)nsamples,
           (unsigned long)(now_us() - t0), (unsigned long)maxline);
    for (k = 0; k < (int)nfc; k++)
        printf("  frame counter changed at +%lu us (%lu us after the one before), line %lu -> %lu\n",
               (unsigned long)(tfc[k] - t0), (unsigned long)(k ? tfc[k] - tfc[k - 1] : 0),
               (unsigned long)fc_line_before[k], (unsigned long)fc_line_after[k]);
    for (k = 0; k < (int)nwrap; k++)
        printf("  line went back at +%lu us: %lu -> %lu\n", (unsigned long)(wrap_t[k] - t0),
               (unsigned long)wrap_before[k], (unsigned long)wrap_after[k]);
    printf("  the longest a line stayed: line %lu, %lu us\n", (unsigned long)longest_line,
           (unsigned long)longest_run);
    /* How long each line value lasts around the end of a frame: one frame's
     * lines from the counter's change on, as (line, us) runs. */
    {
        ULONG lines[64], durs[64], n = 0, f0;
        Forbid();
        f0 = FC(DISPSTAT1());
        while (FC(DISPSTAT1()) == f0)
            ;
        prev = DISPSTAT1();
        run_t = now_us();
        tstart = run_t;
        while (n < 64 && now_us() - tstart < 40000) {
            s = DISPSTAT1();
            if (LINE(s) != LINE(prev) && (LINE(prev) < 8 || LINE(prev) + 8 > maxline || n < 4)) {
                t = now_us();
                lines[n] = LINE(prev);
                durs[n] = t - run_t;
                n++;
                run_t = t;
            } else if (LINE(s) != LINE(prev)) {
                run_t = now_us();
            }
            prev = s;
        }
        Permit();
        printf("  lines after a frame change (line: us): ");
        for (k = 0; k < (int)n; k++)
            printf("%lu:%lu ", (unsigned long)lines[k], (unsigned long)durs[k]);
        printf("\n");
    }
}

/* ---- the waits ---- */
#define MAXF 8192
static ULONG f_t0[MAXF], f_s0[MAXF], f_t1[MAXF], f_s1[MAXF];
static struct timerequest *ctr;
static struct MsgPort *cport;
static ULONG frame_us = 16683, height_lines = 480;

/* CNT: the next change of the HVS's frame counter. */
static void wait_cnt(void)
{
    ULONG s = DISPSTAT1(), f0 = FC(s);
    for (;;) {
        ULONG line;
        s = DISPSTAT1();
        if (FC(s) != f0)
            return;
        line = LINE(s);
        if (ctr && line + 40 < height_lines) {
            ULONG left = (height_lines - line) * (frame_us / (height_lines + 45)), sleep = left - 1000;
            if (left > 1500) {
                ctr->tr_node.io_Command = TR_ADDREQUEST;
                ctr->tr_time.tv_secs = 0;
                ctr->tr_time.tv_micro = sleep;
                DoIO((struct IORequest *)ctr);
                continue;
            }
        }
    }
}

static void waits(int mode, struct Screen *scr, ULONG frames, LONG pri, LONG work, const char *out)
{
    ULONG i, hist[64] = {0}, dfc[4] = {0}, lmin = ~0UL, lmax = 0, t_start, t_end, nmiss = 0;
    BYTE old = SetTaskPri(FindTask(0), pri);
    static const char *names[] = {"WaitBOVP", "WaitTOF", "HVS frame counter"};
    if (frames > MAXF)
        frames = MAXF;
    tasks_start(dpri != 0 && dspin > 0 && dperiod > 0);
    Delay(10);
    t_start = now_us();
    for (i = 0; i < frames; i++) {
        if (work)
            spin_us((ULONG)work);
        f_t0[i] = now_us();
        f_s0[i] = DISPSTAT1();
        if (mode == 0)
            WaitBOVP(&scr->ViewPort);
        else if (mode == 1)
            WaitTOF();
        else
            wait_cnt();
        f_s1[i] = DISPSTAT1();
        f_t1[i] = now_us();
    }
    t_end = now_us();
    tasks_stop();
    SetTaskPri(FindTask(0), old);
    for (i = 0; i < frames; i++) {
        ULONG d = (FC(f_s1[i]) - FC(f_s0[i])) & 63, l = LINE(f_s1[i]);
        dfc[d < 3 ? d : 3]++;
        if (l < lmin)
            lmin = l;
        if (l > lmax)
            lmax = l;
        if (i) {
            ULONG ms = (f_t1[i] - f_t1[i - 1] + 500) / 1000;
            hist[ms < 63 ? ms : 63]++;
        }
    }
    printf("%s, priority %ld, work %ld us", names[mode], (long)pri, (long)work);
    if (dpri && dspin && dperiod)
        printf(", a task at %ld spinning %ld us every %ld ms (%lu spins)", (long)dpri, (long)dspin, (long)dperiod,
               (unsigned long)nd);
    printf(": %lu waits in %lu ms\n", (unsigned long)frames, (unsigned long)((t_end - t_start) / 1000));
    printf("  idle counter: %lu rounds a second\n",
           (unsigned long)((double)idle_n * 1e6 / (double)(t_end - t_start + 1)));
    printf("  frames the counter moved during a wait: 0: %lu, 1: %lu, 2: %lu, 3+: %lu\n", (unsigned long)dfc[0],
           (unsigned long)dfc[1], (unsigned long)dfc[2], (unsigned long)dfc[3]);
    printf("  line at the return: %lu-%lu; ms between returns:", (unsigned long)lmin, (unsigned long)lmax);
    for (i = 0; i < 64; i++)
        if (hist[i])
            printf(" %lu:%lu", (unsigned long)i, (unsigned long)hist[i]);
    printf("\n");
    /* The waits that slept through a refresh: called before the counter's
     * change, returned two or more changes later. */
    for (i = 0; i < frames; i++) {
        ULONG d = (FC(f_s1[i]) - FC(f_s0[i])) & 63, k;
        int hit = 0;
        if (d < 2)
            continue;
        for (k = 0; k < nd; k++)
            if (d_t1[k] > f_t0[i] && d_t0[k] < f_t1[i])
                hit++;
        if (nmiss++ < 24)
            printf("  wait %lu: %lu us, line %lu -> %lu, counter +%lu%s\n", (unsigned long)i,
                   (unsigned long)(f_t1[i] - f_t0[i]), (unsigned long)LINE(f_s0[i]), (unsigned long)LINE(f_s1[i]),
                   (unsigned long)d, hit ? ", a spin of the disturber inside it" : "");
    }
    if (nd) {
        /* Of the spins: how many lay over a refresh (the counter's change
         * inside the spin, seen by the waits around it) and how many of those
         * a wait slept through. */
        ULONG over = 0, missed = 0, k;
        for (k = 0; k < nd; k++) {
            for (i = 0; i < frames; i++)
                if (d_t1[k] > f_t0[i] && d_t0[k] < f_t1[i])
                    break;
            if (i == frames)
                continue;
            over++;
            if (((FC(f_s1[i]) - FC(f_s0[i])) & 63) >= 2)
                missed++;
        }
        printf("  spins inside a wait: %lu, of them in a wait that slept through a refresh: %lu\n",
               (unsigned long)over, (unsigned long)missed);
    }
    if (out) {
        FILE *f = fopen(out, "w");
        if (f) {
            for (i = 0; i < frames; i++)
                fprintf(f, "W %lu %lu %lu %lu %lu %lu\n", (unsigned long)f_t0[i], (unsigned long)LINE(f_s0[i]),
                        (unsigned long)FC(f_s0[i]), (unsigned long)f_t1[i], (unsigned long)LINE(f_s1[i]),
                        (unsigned long)FC(f_s1[i]));
            for (i = 0; i < nd; i++)
                fprintf(f, "D %lu %lu\n", (unsigned long)d_t0[i], (unsigned long)d_t1[i]);
            fclose(f);
        }
    }
}

int main(void)
{
    LONG args[8] = {0};
    struct RDArgs *rda = ReadArgs((CONST_STRPTR) "MODE/A,FRAMES/K/N,PRI/K/N,WORK/K/N,DPRI/K/N,DSPIN/K/N,DPERIOD/K/N,OUT/K",
                                  args, NULL);
    const char *mode;
    ULONG frames, err = 0, id;
    LONG pri, work;
    struct Screen *s = 0;
    int rc = 0;
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR) "vsync_probe");
        return 20;
    }
    mode = (const char *)args[0];
    frames = args[1] ? *(LONG *)args[1] : 1200;
    pri = args[2] ? *(LONG *)args[2] : 0;
    work = args[3] ? *(LONG *)args[3] : 5000;
    dpri = args[4] ? *(LONG *)args[4] : 0;
    dspin = args[5] ? *(LONG *)args[5] : 0;
    dperiod = args[6] ? *(LONG *)args[6] : 0;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((CONST_STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase) {
        printf("FAIL: no intuition/graphics/cybergraphics\n");
        return 20;
    }
    if (!stricmp(mode, "IDLE")) {
        ULONG t0 = now_us(), t1;
        tasks_start(FALSE);
        Delay(args[1] ? 50 * frames : 250); /* FRAMES: seconds here */
        t1 = now_us();
        tasks_stop();
        printf("idle: %lu rounds a second\n", (unsigned long)((double)idle_n * 1e6 / (double)(t1 - t0)));
        goto done;
    }
    id = BestCModeIDTags(CYBRBIDTG_NominalWidth, 640, CYBRBIDTG_NominalHeight, 480, CYBRBIDTG_Depth, 32, TAG_DONE);
    if (id != (ULONG)INVALID_ID)
        s = OpenScreenTags(0, SA_Depth, 24, SA_DisplayID, id, SA_Width, 640, SA_Height, 480, SA_ErrorCode,
                           (ULONG)&err, SA_ShowTitle, FALSE, SA_Draggable, FALSE, SA_Quiet, TRUE, TAG_DONE);
    if (!s) {
        printf("FAIL: no 640x480 32-bit screen (mode 0x%08lx, error %lu)\n", (unsigned long)id, (unsigned long)err);
        rc = 20;
        goto done;
    }
    FillPixelArray(&s->RastPort, 0, 0, 640, 480, 0x00203040);
    Delay(25);
    if (!stricmp(mode, "HVS")) {
        hvs();
    } else {
        int m = !stricmp(mode, "BOVP") ? 0 : !stricmp(mode, "TOF") ? 1 : !stricmp(mode, "CNT") ? 2 : -1;
        if (m < 0) {
            printf("FAIL: MODE is HVS, BOVP, TOF, CNT or IDLE\n");
            rc = 20;
        } else {
            if (m == 2) {
                /* The frame's length and lines from the HVS: the counter's
                 * change to the next, the highest line seen. */
                ULONG f0, t0, maxl = 0, st;
                Forbid();
                f0 = FC(DISPSTAT1());
                while (FC(DISPSTAT1()) == f0)
                    ;
                t0 = now_us();
                f0 = FC(DISPSTAT1());
                while (FC(st = DISPSTAT1()) == f0)
                    if (LINE(st) > maxl)
                        maxl = LINE(st);
                frame_us = now_us() - t0;
                Permit();
                height_lines = maxl;
                printf("CNT: a frame %lu us, lines to %lu\n", (unsigned long)frame_us, (unsigned long)maxl);
                cport = CreateMsgPort();
                ctr = cport ? (struct timerequest *)CreateIORequest(cport, sizeof(*ctr)) : 0;
                if (ctr && OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)ctr, 0)) {
                    DeleteIORequest((struct IORequest *)ctr);
                    ctr = 0;
                }
            }
            waits(m, s, frames, pri, work, (const char *)args[7]);
            if (ctr) {
                CloseDevice((struct IORequest *)ctr);
                DeleteIORequest((struct IORequest *)ctr);
            }
            if (cport)
                DeleteMsgPort(cport);
        }
    }
    CloseScreen(s);
done:
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return rc;
}
