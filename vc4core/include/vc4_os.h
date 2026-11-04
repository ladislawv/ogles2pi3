/*
 * vc4core's OS layer (docs/PI3_PLAN.md §3.1): what differs between Linux on
 * the Pi 3 (os_linux.c, P3) and AmigaOS under Emu68 (os_amiga.c, P5).
 *
 * MIT licence (LICENSE).
 */
#ifndef VC4_OS_H
#define VC4_OS_H

#include <stdint.h>

struct vc4_mem;

/* The registers mapped and the firmware's mailbox open; the core untouched. */
int vc4_os_open(void);
void vc4_os_close(void);
/* The 3D core's registers (4 KB, little-endian words). */
volatile uint32_t *vc4_os_regs(void);
/* One firmware property tag: its request in val (n words, at least the
 * response's length), the response written over them. The response's length
 * in bytes, or -1. */
int vc4_os_mbox(uint32_t tag, uint32_t *val, int n);
/* GPU memory, 4 KB aligned; 0 or -1. */
int vc4_os_alloc(uint32_t size, struct vc4_mem *m);
/* 1 if the GPU reaches the CPU's [addr, addr+size) (RAM the device tree
 * lists: not the Amiga's own chip RAM, for one), else 0. */
int vc4_os_gpu_reachable(uint32_t addr, uint32_t size);
void vc4_os_free(struct vc4_mem *m);
/* The CPU's writes to [p, p+n) made visible to the GPU, and the GPU's to the
 * CPU (nothing to do where the memory is mapped uncached). */
void vc4_os_flush(const void *p, uint32_t n);
void vc4_os_invalidate(const void *p, uint32_t n);
uint64_t vc4_os_time_us(void);
void vc4_os_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* os_amiga.c: each log line goes to the program (ogles2.library, a test):
 * it defines this. */
void vc4_os_log_line(const char *line);

#endif
