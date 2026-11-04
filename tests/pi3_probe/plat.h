/*
 * The bare-GPU probe's platform layer (PI3_PLAN P1, §6.2): what differs
 * between Linux on the Pi 3 (plat_linux.c) and AmigaOS under Emu68 (P5).
 * The core (pi3_probe.c) only builds lists and reads results.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#ifndef PI3_PROBE_PLAT_H
#define PI3_PROBE_PLAT_H

#include <stdint.h>

/* How the 3D core is powered on (PI3_PLAN §2, "Power"). */
enum plat_power {
    PLAT_POWER_DOMAIN, /* mailbox SET_DOMAIN_STATE (0x00038030), domain 10 (V3D) */
    PLAT_POWER_QPU     /* mailbox SET_ENABLE_QPU (0x00030012), as hello_fft */
};

/* GPU memory from the firmware's allocator, seen by the CPU at cpu and by the
 * GPU at bus (the 0xC0000000 alias on the Pi 3). The CPU's view may be a
 * device mapping: only aligned 32-bit accesses (plat_write/plat_read). */
struct plat_mem {
    volatile uint32_t *cpu;
    uint32_t bus;
    uint32_t size;
    uint32_t handle;
};

/* Opens the platform and powers the 3D core on; 0 on success. */
int plat_open(enum plat_power power);
void plat_close(void);
/* The 3D core's registers (V3D 2.1, 4 KB). */
volatile uint32_t *plat_regs(void);
/* The firmware's mailbox: one property tag, its request words in val (n
 * words, n >= the response's length), the response written over them.
 * Returns the tag's response length in bytes, or -1. */
int plat_mbox(uint32_t tag, uint32_t *val, int n);
int plat_alloc(uint32_t size, struct plat_mem *m);
void plat_free(struct plat_mem *m);
uint64_t plat_time_us(void);

#endif
