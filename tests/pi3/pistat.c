/*
 * pistat: the Pi's temperature, power and clock through the firmware's mailbox,
 * with the Amiga's free memory, a line every INTERVAL seconds into FILE, closed
 * after each (a reset loses nothing). For finding why the A1200's Wi-Fi link drops
 * under long runs (docs/PI3_PLAN.md §10): the Pi 3 card runs from the Amiga's 5 V.
 *   pistat FILE [INTERVAL [INTERFACE]]         Ctrl-C ends it
 * With INTERFACE: its counters (Roadshow's ShowNetStatus) and the free memory only.
 * The mailbox is then left alone: ogles2.library uses it too, without knowing of
 * this program, so a run under test should not have a second user of it.
 * Throttled's bits (Raspberry Pi's vcgencmd get_throttled): 0 under-voltage now,
 * 1 ARM capped now, 2 throttled now, 3 soft temperature limit now; 16 to 19 the same
 * since boot.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include <stdio.h>
#include <stdlib.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <utility/tagitem.h>
#include "vc4_os.h"

/* os_amiga.c's log lines (the library's go to its log): not wanted here */
void vc4_os_log_line(const char *line)
{
    (void)line;
}

int main(int argc, char **argv)
{
    long every = argc > 2 ? atol(argv[2]) : 30;
    ULONG minutes = 0;
    if (argc < 2 || every < 1) {
        printf("usage: pistat FILE [INTERVAL]\n");
        return 20;
    }
    if (argc <= 3 && vc4_os_open() < 0) {
        printf("pistat: no mailbox\n");
        return 20;
    }
    for (;;) {
        uint32_t t[2] = {0, 0}, th[1] = {0}, clk[2] = {3, 0}, v[2] = {1, 0};
        struct DateStamp now;
        FILE *f;
        long left;
        if (argc <= 3) {
            vc4_os_mbox(0x00030006u, t, 2);   /* the SoC's temperature, thousandths */
            vc4_os_mbox(0x00030046u, th, 1);  /* throttled */
            vc4_os_mbox(0x00030002u, clk, 2); /* the ARM's clock */
            vc4_os_mbox(0x00030003u, v, 2);   /* the core's voltage, millionths */
        }
        DateStamp(&now);
        if ((f = fopen(argv[1], "a")) != 0) {
            fprintf(f, "min %ld sec %ld: %lu.%lu C, throttled 0x%05lx, ARM %lu MHz, core %lu mV, fast free %lu, largest %lu\n",
                    (long)now.ds_Minute, (long)now.ds_Tick / TICKS_PER_SECOND, (unsigned long)t[1] / 1000,
                    (unsigned long)(t[1] % 1000) / 100, (unsigned long)th[0], (unsigned long)clk[1] / 1000000,
                    (unsigned long)v[1] / 1000, (unsigned long)AvailMem(MEMF_FAST),
                    (unsigned long)AvailMem(MEMF_FAST | MEMF_LARGEST));
            fclose(f);
        }
        if (argc > 3) { /* the interface's counters, as ShowNetStatus gives them */
            char cmd[200];
            snprintf(cmd, sizeof cmd, "ShowNetStatus %s >>%s", argv[3], argv[1]);
            SystemTags((STRPTR)cmd, TAG_DONE);
        }
        minutes++;
        for (left = every; left > 0; left--) {
            Delay(TICKS_PER_SECOND);
            if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
                if (argc <= 3)
                    vc4_os_close();
                return 0;
            }
        }
    }
}
