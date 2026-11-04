/*
 * The bare-GPU probe's platform layer on Linux (Raspberry Pi OS on the Pi 3,
 * the bare boot of PI3_PLAN §6.0: no kernel driver owns the 3D core). Run as
 * root. The firmware's mailbox through /dev/vcio; the 3D core's registers and
 * the firmware's GPU memory through /dev/mem (Raspberry Pi's arm64 kernel has
 * CONFIG_STRICT_DEVMEM unset). The calls are those of Raspberry Pi's
 * hello_fft (mailbox.c, gpu_fft_base.c), written anew here.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define V3D_PHYS 0x3FC00000u /* BCM2837: /soc ranges 0x7e000000 -> 0x3f000000 */
#define BUS_ALIAS 0xC0000000u /* /soc dma-ranges: bus = 0xC0000000 + physical */
#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)

#define TAG_ALLOCATE_MEMORY 0x0003000c
#define TAG_LOCK_MEMORY 0x0003000d
#define TAG_UNLOCK_MEMORY 0x0003000e
#define TAG_RELEASE_MEMORY 0x0003000f
#define TAG_SET_ENABLE_QPU 0x00030012
#define TAG_GET_DOMAIN_STATE 0x00030030
#define TAG_SET_DOMAIN_STATE 0x00038030
#define DOMAIN_V3D 10
/* MEM_FLAG_DIRECT: the 0xC0000000 (uncached) alias, as hello_fft on the Pi 2/3. */
#define MEM_FLAG_DIRECT 0x4

static int vcio = -1, devmem = -1;
static volatile uint32_t *regs;
static enum plat_power power_used;
int plat_keep_power, plat_no_reset;

int plat_mbox(uint32_t tag, uint32_t *val, int n)
{
    uint32_t buf[32] __attribute__((aligned(16)));
    int k;

    if (n > 24)
        return -1;
    buf[0] = (uint32_t)(6 + n) * 4; /* size, code, tag, value size, request size, values, end */
    buf[1] = 0;                      /* a request */
    buf[2] = tag;
    buf[3] = (uint32_t)n * 4;
    buf[4] = 0;
    for (k = 0; k < n; k++)
        buf[5 + k] = val[k];
    buf[5 + n] = 0;
    if (ioctl(vcio, IOCTL_MBOX_PROPERTY, buf) < 0) {
        fprintf(stderr, "mailbox tag 0x%08x: ioctl: %s\n", tag, strerror(errno));
        return -1;
    }
    if (buf[1] != 0x80000000u || !(buf[4] & 0x80000000u)) {
        fprintf(stderr, "mailbox tag 0x%08x: response 0x%08x 0x%08x\n", tag, buf[1], buf[4]);
        return -1;
    }
    for (k = 0; k < n; k++)
        val[k] = buf[5 + k];
    return (int)(buf[4] & 0x7fffffffu);
}

static int power(enum plat_power how, int on)
{
    uint32_t v[2];
    if (how == PLAT_POWER_DOMAIN) {
        v[0] = DOMAIN_V3D;
        v[1] = on ? 1 : 0;
        if (plat_mbox(TAG_SET_DOMAIN_STATE, v, 2) < 0)
            return -1;
        if (on)
            printf("power: SET_DOMAIN_STATE(V3D, on) -> domain %u state %u\n", v[0], v[1]);
        return 0;
    }
    v[0] = on ? 1 : 0;
    if (plat_mbox(TAG_SET_ENABLE_QPU, v, 1) < 0)
        return -1;
    if (on)
        printf("power: SET_ENABLE_QPU(1) -> %u\n", v[0]);
    return 0;
}

/* Powers the 3D core off and on, so that it starts from its reset state.
 * The firmware counts SET_ENABLE_QPU: a user that did not switch it off (a
 * crash, a second process) leaves the count above zero, and the next enable
 * then finds the previous user's state (2026-11-04: such a core lost a tile's
 * primitives now and then, with VPM errors in ERRSTAT). So: disable until
 * the registers stop answering (0xdeadbeef), enable, and check VPMBASE's
 * reset value (0x10). Unpowered registers read 0xdeadbeef without harm. */
static int reset_core(void)
{
    uint32_t v[1];
    int k;

    for (k = 0; k < 32 && regs[0] != 0xdeadbeef; k++) {
        v[0] = 0;
        if (plat_mbox(TAG_SET_ENABLE_QPU, v, 1) < 0)
            return -1;
    }
    printf("power: %d SET_ENABLE_QPU(0) to power the core off%s\n", k, k > 1 ? " (it was enabled more than once)" : "");
    v[0] = 1;
    if (plat_mbox(TAG_SET_ENABLE_QPU, v, 1) < 0)
        return -1;
    if (regs[0x504 / 4] != 0x10) {
        printf("power: VPMBASE is %08x after power-on, not the reset value 0x10\n", regs[0x504 / 4]);
        return -1;
    }
    return 0;
}

int plat_open(enum plat_power how)
{
    uint32_t v[2];

    vcio = open("/dev/vcio", O_RDWR);
    if (vcio < 0) {
        perror("/dev/vcio");
        return -1;
    }
    devmem = open("/dev/mem", O_RDWR | O_SYNC);
    if (devmem < 0) {
        perror("/dev/mem (run as root)");
        return -1;
    }
    regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, devmem, V3D_PHYS);
    if (regs == MAP_FAILED) {
        perror("mmap V3D registers");
        regs = NULL;
        return -1;
    }
    v[0] = DOMAIN_V3D;
    v[1] = 0;
    if (plat_mbox(TAG_GET_DOMAIN_STATE, v, 2) >= 0)
        printf("power: GET_DOMAIN_STATE(V3D) before -> domain %u state %u\n", v[0], v[1]);
    if (how == PLAT_POWER_QPU && !plat_no_reset) {
        if (reset_core() < 0) /* which leaves it enabled once */
            return -1;
    } else if (power(how, 1) < 0) {
        return -1;
    }
    power_used = how;
    v[0] = DOMAIN_V3D;
    v[1] = 0;
    if (plat_mbox(TAG_GET_DOMAIN_STATE, v, 2) >= 0)
        printf("power: GET_DOMAIN_STATE(V3D) after -> domain %u state %u\n", v[0], v[1]);
    return 0;
}

void plat_close(void)
{
    if (regs)
        munmap((void *)regs, 4096);
    regs = NULL;
    if (vcio >= 0 && !plat_keep_power)
        power(power_used, 0);
    if (devmem >= 0)
        close(devmem);
    if (vcio >= 0)
        close(vcio);
    vcio = devmem = -1;
}

volatile uint32_t *plat_regs(void)
{
    return regs;
}

int plat_alloc(uint32_t size, struct plat_mem *m)
{
    uint32_t v[3];
    void *p;

    size = (size + 4095) & ~4095u;
    v[0] = size;
    v[1] = 4096;
    v[2] = MEM_FLAG_DIRECT;
    if (plat_mbox(TAG_ALLOCATE_MEMORY, v, 3) < 0 || !v[0]) {
        fprintf(stderr, "ALLOCATE_MEMORY(%u) failed\n", size);
        return -1;
    }
    m->handle = v[0];
    v[0] = m->handle;
    if (plat_mbox(TAG_LOCK_MEMORY, v, 1) < 0 || !v[0]) {
        fprintf(stderr, "LOCK_MEMORY failed\n");
        v[0] = m->handle;
        plat_mbox(TAG_RELEASE_MEMORY, v, 1);
        return -1;
    }
    m->bus = v[0];
    m->size = size;
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, devmem, m->bus & ~BUS_ALIAS);
    if (p == MAP_FAILED) {
        perror("mmap GPU memory");
        plat_free(m);
        return -1;
    }
    m->cpu = p;
    return 0;
}

void plat_free(struct plat_mem *m)
{
    uint32_t v[1];
    if (m->cpu)
        munmap((void *)m->cpu, m->size);
    m->cpu = NULL;
    if (m->handle) {
        v[0] = m->handle;
        plat_mbox(TAG_UNLOCK_MEMORY, v, 1);
        v[0] = m->handle;
        plat_mbox(TAG_RELEASE_MEMORY, v, 1);
    }
    m->handle = 0;
}

uint64_t plat_time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
