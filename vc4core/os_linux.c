/*
 * vc4core's OS layer on Linux (Raspberry Pi OS on the Pi 3, the bare boot of
 * docs/PI3_PLAN.md §6.0: no kernel driver owns the 3D core). Run as root. The
 * firmware's mailbox through /dev/vcio; the registers and the firmware's GPU
 * memory through /dev/mem (Raspberry Pi's arm64 kernel has
 * CONFIG_STRICT_DEVMEM unset), as tests/pi3_probe's plat_linux.c.
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"
#include "vc4_os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define V3D_PHYS 0x3FC00000u  /* BCM2837: /soc ranges 0x7e000000 -> 0x3f000000 */
#define BUS_ALIAS 0xC0000000u /* /soc dma-ranges: bus = 0xC0000000 + physical */
#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)
#define TAG_ALLOCATE_MEMORY 0x0003000cu
#define TAG_LOCK_MEMORY 0x0003000du
#define TAG_UNLOCK_MEMORY 0x0003000eu
#define TAG_RELEASE_MEMORY 0x0003000fu
#define MEM_FLAG_DIRECT 0x4 /* the 0xC0000000 (uncached) alias */

static int vcio = -1, devmem = -1;
static volatile uint32_t *regs;

void vc4_os_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("vc4core: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}

int vc4_os_mbox(uint32_t tag, uint32_t *val, int n)
{
    uint32_t buf[32] __attribute__((aligned(16)));
    int k;
    if (n > 24)
        return -1;
    buf[0] = (uint32_t)(6 + n) * 4; /* size, code, tag, value size, request size, values, end */
    buf[1] = 0;
    buf[2] = tag;
    buf[3] = (uint32_t)n * 4;
    buf[4] = 0;
    for (k = 0; k < n; k++)
        buf[5 + k] = val[k];
    buf[5 + n] = 0;
    if (ioctl(vcio, IOCTL_MBOX_PROPERTY, buf) < 0) {
        vc4_os_log("mailbox tag 0x%08x: %s", tag, strerror(errno));
        return -1;
    }
    if (buf[1] != 0x80000000u || !(buf[4] & 0x80000000u)) {
        vc4_os_log("mailbox tag 0x%08x: response 0x%08x 0x%08x", tag, buf[1], buf[4]);
        return -1;
    }
    for (k = 0; k < n; k++)
        val[k] = buf[5 + k];
    return (int)(buf[4] & 0x7fffffffu);
}

int vc4_os_open(void)
{
    if ((vcio = open("/dev/vcio", O_RDWR)) < 0) {
        vc4_os_log("/dev/vcio: %s", strerror(errno));
        return -1;
    }
    if ((devmem = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
        vc4_os_log("/dev/mem: %s (run as root)", strerror(errno));
        return -1;
    }
    regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, devmem, V3D_PHYS);
    if (regs == MAP_FAILED) {
        vc4_os_log("mapping the registers: %s", strerror(errno));
        regs = NULL;
        return -1;
    }
    return 0;
}

void vc4_os_close(void)
{
    if (regs)
        munmap((void *)regs, 4096);
    regs = NULL;
    if (devmem >= 0)
        close(devmem);
    if (vcio >= 0)
        close(vcio);
    vcio = devmem = -1;
}

volatile uint32_t *vc4_os_regs(void)
{
    return regs;
}

int vc4_os_alloc(uint32_t size, struct vc4_mem *m)
{
    uint32_t v[3];
    void *p;
    memset(m, 0, sizeof(*m));
    size = (size + 4095) & ~4095u;
    v[0] = size;
    v[1] = 4096;
    v[2] = MEM_FLAG_DIRECT;
    if (vc4_os_mbox(TAG_ALLOCATE_MEMORY, v, 3) < 0 || !v[0])
        return -1;
    m->handle = v[0];
    v[0] = m->handle;
    if (vc4_os_mbox(TAG_LOCK_MEMORY, v, 1) < 0 || !v[0]) {
        vc4_os_free(m);
        return -1;
    }
    m->bus = v[0];
    m->size = size;
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, devmem, m->bus & ~BUS_ALIAS);
    if (p == MAP_FAILED) {
        vc4_os_log("mapping %u bytes of GPU memory: %s", size, strerror(errno));
        vc4_os_free(m);
        return -1;
    }
    m->cpu = p;
    return 0;
}

void vc4_os_free(struct vc4_mem *m)
{
    uint32_t v[1];
    if (m->cpu)
        munmap(m->cpu, m->size);
    if (m->handle) {
        v[0] = m->handle;
        vc4_os_mbox(TAG_UNLOCK_MEMORY, v, 1);
        v[0] = m->handle;
        vc4_os_mbox(TAG_RELEASE_MEMORY, v, 1);
    }
    memset(m, 0, sizeof(*m));
}

/* Every block is the firmware's: the GPU reaches it. */
int vc4_os_gpu_reachable(uint32_t addr, uint32_t size)
{
    (void)addr;
    (void)size;
    return 1;
}

int vc4_os_present(void)
{
    return 1; /* the tests run on the Pi 3 */
}

uint32_t vc4_os_bus(const void *p)
{
    (void)p;
    return 0;
}

/* /dev/mem maps the firmware's memory uncached (O_SYNC): nothing to do. */
void vc4_os_flush(const void *p, uint32_t n)
{
    (void)p;
    (void)n;
}

void vc4_os_invalidate(const void *p, uint32_t n)
{
    (void)p;
    (void)n;
}

uint64_t vc4_os_time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
