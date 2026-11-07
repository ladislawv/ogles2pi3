/*
 * piclock: what the Raspberry Pi's firmware says about the clocks it runs, its
 * temperature and whether it throttles, read through its mailbox from AmigaOS
 * under Emu68 (vc4core's os_amiga.c: the device tree gives the mailbox). Pi 3
 * and Pi 4 alike. Read-only: property tags that report, none that set.
 *
 *   piclock                  once
 *   piclock <seconds> <step> every <step> seconds for <seconds> (beside a benchmark)
 *
 * Why (docs/PI3_PLAN.md §10, 2026-11-07): Quake III ran a quarter slower on the
 * Pi 4 card than two days before, with the same kernel, library and package; every
 * CPU stage alike, as a lower ARM clock would make it.
 *
 * MIT licence (LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <proto/dos.h>
#include "vc4_os.h"

void vc4_os_log_line(const char *line)
{
    printf("  (vc4core: %s)\n", line);
}

/* One tag with one id word in, the id and a value out: the value, or -1 */
static long tag2(uint32_t tag, uint32_t id)
{
    uint32_t v[2];
    v[0] = id;
    v[1] = 0;
    return vc4_os_mbox(tag, v, 2) < 0 ? -1 : (long)v[1];
}

static long tag1(uint32_t tag)
{
    uint32_t v[1] = {0};
    return vc4_os_mbox(tag, v, 1) < 0 ? -1 : (long)v[0];
}

static void mhz(const char *name, uint32_t id)
{
    long set = tag2(0x00030002u, id), measured = tag2(0x00030047u, id), max = tag2(0x00030004u, id);
    printf("  %-6s %4ld MHz set, %4ld measured, %4ld max\n", name, set < 0 ? -1 : set / 1000000,
           measured < 0 ? -1 : measured / 1000000, max < 0 ? -1 : max / 1000000);
}

static void report(void)
{
    long t = tag2(0x00030006u, 0), tmax = tag2(0x0003000au, 0), thr = tag1(0x00030046u);
    long turbo = tag2(0x00030009u, 0), volt = tag2(0x00030003u, 1);
    mhz("ARM", 3);
    mhz("core", 4);
    mhz("V3D", 5);
    mhz("SDRAM", 8);
    printf("  temperature %ld.%ld C (limit %ld C), turbo %ld, core voltage %ld.%03ld V\n", t / 1000, (t % 1000) / 100,
           tmax / 1000, turbo, volt / 1000000, (volt % 1000000) / 1000);
    if (thr >= 0)
        printf("  throttled 0x%05lx:%s%s%s%s;%s%s%s%s\n", thr, thr & 1 ? " under-voltage" : "", thr & 2 ? " ARM capped" : "",
               thr & 4 ? " throttled" : "", thr & 8 ? " soft temperature limit" : "", thr & 0xf0000 ? " since boot:" : " nothing since boot",
               thr & 0x10000 ? " under-voltage" : "", thr & 0x20000 ? " ARM capped" : "",
               thr & 0x40000 ? " throttled" : (thr & 0x80000 ? " soft temperature limit" : ""));
}

int main(int argc, char **argv)
{
    long secs = argc > 1 ? atol(argv[1]) : 0, step = argc > 2 ? atol(argv[2]) : 5, done = 0;
    if (vc4_os_open() < 0) {
        printf("piclock: no mailbox (not on Emu68, or no device tree)\n");
        return 20;
    }
    printf("piclock: board revision 0x%06lx, firmware %ld\n", tag1(0x00010002u), tag1(0x00000001u));
    for (;;) {
        printf("%lds:\n", done);
        report();
        if (done >= secs || step <= 0)
            break;
        Delay(50 * step);
        done += step;
    }
    vc4_os_close();
    return 0;
}
