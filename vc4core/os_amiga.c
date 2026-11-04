/*
 * vc4core's OS layer on AmigaOS under Emu68 (docs/PI3_PLAN.md §3.1, §7, P5):
 * the Raspberry Pi 3's peripherals, its firmware's mailbox and fast RAM as
 * GPU memory, as the 68k sees them.
 *
 * Emu68 maps the ARM's physical address space one to one into the 68k's, so
 * a peripheral is reached at its ARM physical address: the device tree's
 * /soc "ranges" turns the bus addresses the peripherals are known by
 * (0x7e......) into those. The GPU reaches RAM through the bus alias /soc
 * "dma-ranges" gives (on the Pi 3, bus 0xC0000000 + physical: the uncached
 * "direct" alias Linux uses). GPU memory is fast RAM (AllocVec), cached on
 * the ARM: written by the CPU, it is cleaned to RAM before the GPU reads it
 * (vc4_os_flush: CachePreDMA), and lines the GPU wrote are dropped before the
 * CPU reads them (vc4_os_invalidate: CachePostDMA). A new block is cleaned at
 * once, so no dirty line of the allocator's can later land on what the GPU
 * wrote there. The mailbox request itself lives in GPU-visible memory too.
 *
 * Everything the device tree gives is read once, at vc4_os_open. Written for
 * vc4core; the devicetree.resource offsets are Emu68's public interface.
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"
#include "vc4_os.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdarg.h>
#include <string.h>

/* ---- devicetree.resource (Emu68): its calls by offset ---- */

static APTR dtbase;

#define DT_CALL1(off, v0)                                                                            \
    ({                                                                                               \
        register APTR dt_r_ __asm("d0");                                                             \
        register APTR dt_b_ __asm("a6") = dtbase;                                                    \
        register APTR dt_a0_ __asm("a0") = (APTR)(v0);                                               \
        __asm volatile("jsr -" #off "(%%a6)" : "=r"(dt_r_), "+r"(dt_a0_) : "r"(dt_b_)                \
                       : "d1", "a1", "cc", "memory");                                                \
        dt_r_;                                                                                       \
    })
#define DT_CALL2(off, v0, v1)                                                                        \
    ({                                                                                               \
        register APTR dt_r_ __asm("d0");                                                             \
        register APTR dt_b_ __asm("a6") = dtbase;                                                    \
        register APTR dt_a0_ __asm("a0") = (APTR)(v0);                                               \
        register APTR dt_a1_ __asm("a1") = (APTR)(v1);                                               \
        __asm volatile("jsr -" #off "(%%a6)" : "=r"(dt_r_), "+r"(dt_a0_), "+r"(dt_a1_) : "r"(dt_b_)  \
                       : "d1", "cc", "memory");                                                      \
        dt_r_;                                                                                       \
    })

#define DT_OpenKey(name) DT_CALL1(6, name)
#define DT_CloseKey(key) ((void)DT_CALL1(12, key))
#define DT_FindProperty(key, name) DT_CALL2(24, key, name)
#define DT_GetPropLen(prop) ((ULONG)DT_CALL1(36, prop))
#define DT_GetPropValue(prop) ((const ULONG *)DT_CALL1(48, prop))
#define DT_GetParent(key) DT_CALL1(54, key)

/* A property's value, looked up in the key and then its parents (cells
 * counts are inherited so). */
static const ULONG *prop_up(APTR key, const char *name, ULONG *len)
{
    while (key) {
        APTR p = DT_FindProperty(key, name);
        if (p) {
            if (len)
                *len = DT_GetPropLen(p);
            return DT_GetPropValue(p);
        }
        key = DT_GetParent(key);
    }
    return 0;
}

/* ---- what the device tree gives ---- */

static ULONG soc_bus, soc_cpu;        /* /soc ranges: bus base -> ARM physical */
static ULONG dma_bus = 0xC0000000u;   /* /soc dma-ranges: bus = physical - dma_cpu + dma_bus */
static ULONG dma_cpu;
static volatile ULONG *mbox;          /* the ARM's mailbox 0 */
static volatile ULONG *regs;          /* the 3D core */
static volatile ULONG *systimer;      /* the free-running 1 MHz counter */
static uint8_t *mbreq;                /* the request buffer: 16-byte aligned, fast RAM */
#define MBW(k) (mbreq + 4 * (k))
static APTR mbreq_block;

#define V3D_BUS 0x7ec00000u
#define SYSTIMER_BUS 0x7e003000u
#define MBOX_READ 0x00    /* in 32-bit words: 0x00 */
#define MBOX_STATUS 0x06  /* 0x18 */
#define MBOX_WRITE 0x08   /* 0x20 */
#define MBOX_FULL 0x80000000u
#define MBOX_EMPTY 0x40000000u
#define MBOX_PROPERTY 8

static volatile ULONG *periph(ULONG bus)
{
    return (volatile ULONG *)(bus - soc_bus + soc_cpu);
}

static ULONG bus_of(const void *cpu)
{
    return (ULONG)cpu - dma_cpu + dma_bus;
}

static int read_tree(void)
{
    APTR soc, aliases, key;
    const ULONG *v, *cells, *root_cells, *alias_cells;
    const char *path;
    ULONG len = 0, sc = 1, pc = 1;

    if (!(dtbase = OpenResource((CONST_STRPTR) "devicetree.resource"))) {
        vc4_os_log("no devicetree.resource (not running on Emu68?)");
        return -1;
    }
    if (!(soc = DT_OpenKey("/soc"))) {
        vc4_os_log("no /soc in the device tree");
        return -1;
    }
    /* ranges: <child bus (soc's #address-cells)> <parent (root's)> <size> */
    cells = prop_up(soc, "#address-cells", 0);
    key = DT_OpenKey("/");
    root_cells = key ? prop_up(key, "#address-cells", 0) : 0;
    if (key)
        DT_CloseKey(key);
    sc = cells ? cells[0] : 1;
    pc = root_cells ? root_cells[0] : 1;
    v = (const ULONG *)DT_GetPropValue(DT_FindProperty(soc, "ranges"));
    if (!v) {
        DT_CloseKey(soc);
        vc4_os_log("no /soc ranges");
        return -1;
    }
    soc_bus = v[sc - 1];
    soc_cpu = v[sc + pc - 1];
    {
        APTR p = DT_FindProperty(soc, "dma-ranges");
        if (p && DT_GetPropLen(p) >= (sc + pc) * 4) {
            v = DT_GetPropValue(p);
            dma_bus = v[sc - 1];
            dma_cpu = v[sc + pc - 1];
        }
    }
    DT_CloseKey(soc);

    /* the mailbox: /aliases' "mailbox" names its node; reg in bus addresses */
    if (!(aliases = DT_OpenKey("/aliases"))) {
        vc4_os_log("no /aliases in the device tree");
        return -1;
    }
    {
        APTR p = DT_FindProperty(aliases, "mailbox");
        path = p ? (const char *)DT_GetPropValue(p) : 0;
    }
    key = path ? DT_OpenKey(path) : 0;
    DT_CloseKey(aliases);
    if (!key) {
        vc4_os_log("no mailbox alias in the device tree");
        return -1;
    }
    alias_cells = prop_up(DT_GetParent(key), "#address-cells", 0);
    v = prop_up(key, "reg", &len);
    if (!v) {
        DT_CloseKey(key);
        vc4_os_log("the mailbox has no reg");
        return -1;
    }
    mbox = periph(v[(alias_cells ? alias_cells[0] : 1) - 1]);
    DT_CloseKey(key);
    regs = periph(V3D_BUS);
    systimer = periph(SYSTIMER_BUS);
    return 0;
}

/* ---- the mailbox ---- */

int vc4_os_mbox(uint32_t tag, uint32_t *val, int n)
{
    ULONG size, k, rsp;
    int len;
    if (n > 24 || !mbreq)
        return -1;
    size = (ULONG)(6 + n) * 4;
    vc4_put32(MBW(0), size);
    vc4_put32(MBW(1), 0);
    vc4_put32(MBW(2), tag);
    vc4_put32(MBW(3), (uint32_t)n * 4);
    vc4_put32(MBW(4), 0);
    for (k = 0; k < (ULONG)n; k++)
        vc4_put32(MBW(5 + k), val[k]);
    vc4_put32(MBW(5 + n), 0);
    vc4_os_flush(mbreq, size);
    Forbid();
    while (vc4_le32(mbox[MBOX_STATUS]) & MBOX_FULL)
        ;
    mbox[MBOX_WRITE] = vc4_le32((bus_of(mbreq) & ~15u) | MBOX_PROPERTY);
    do {
        while (vc4_le32(mbox[MBOX_STATUS]) & MBOX_EMPTY)
            ;
        rsp = vc4_le32(mbox[MBOX_READ]);
    } while ((rsp & 15) != MBOX_PROPERTY);
    Permit();
    vc4_os_invalidate(mbreq, size);
    if (vc4_get32(MBW(1)) != 0x80000000u || !(vc4_get32(MBW(4)) & 0x80000000u)) {
        vc4_os_log("mailbox tag 0x%08x: response 0x%08x 0x%08x", tag, vc4_get32(MBW(1)), vc4_get32(MBW(4)));
        return -1;
    }
    for (k = 0; k < (ULONG)n; k++)
        val[k] = vc4_get32(MBW(5 + k));
    len = (int)(vc4_get32(MBW(4)) & 0x7fffffffu);
    return len;
}

/* ---- open, close ---- */

int vc4_os_open(void)
{
    if (read_tree() < 0)
        return -1;
    if (!mbreq_block) {
        if (!(mbreq_block = AllocVec(256 + 16, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR)))
            return -1;
        mbreq = (uint8_t *)(((ULONG)mbreq_block + 15) & ~15u);
    }
    return 0;
}

void vc4_os_close(void)
{
    if (mbreq_block)
        FreeVec(mbreq_block);
    mbreq_block = 0;
    mbreq = 0;
}

volatile uint32_t *vc4_os_regs(void)
{
    return (volatile uint32_t *)regs;
}

/* ---- GPU memory: fast RAM, 4 KB aligned ---- */

int vc4_os_alloc(uint32_t size, struct vc4_mem *m)
{
    APTR block;
    ULONG at;
    memset(m, 0, sizeof(*m));
    size = (size + 4095) & ~4095u;
    if (!(block = AllocVec(size + 4096, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR)))
        return -1;
    at = ((ULONG)block + 4095) & ~4095u;
    m->cpu = (uint8_t *)at;
    m->bus = bus_of((void *)at);
    m->size = size;
    m->handle = (uint32_t)block;
    vc4_os_flush(m->cpu, size); /* the allocator's zeros out of the cache now */
    return 0;
}

void vc4_os_free(struct vc4_mem *m)
{
    if (m->handle)
        FreeVec((APTR)m->handle);
    memset(m, 0, sizeof(*m));
}

void vc4_os_flush(const void *p, uint32_t n)
{
    ULONG len = n;
    if (n)
        CachePreDMA((APTR)p, &len, 0);
}

void vc4_os_invalidate(const void *p, uint32_t n)
{
    ULONG len = n;
    if (n)
        CachePostDMA((APTR)p, &len, 0);
}

/* ---- time: the system timer's 64-bit microsecond count ---- */

uint64_t vc4_os_time_us(void)
{
    ULONG hi, lo, hi2;
    if (!systimer)
        return 0;
    do {
        hi = vc4_le32(systimer[2]);
        lo = vc4_le32(systimer[1]);
        hi2 = vc4_le32(systimer[2]);
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

/* ---- the log: vc4core's C formats (%d %u %x %s, width and 0), each line
 * handed to the program's vc4_os_log_line ---- */

static char *put_num(char *o, char *end, ULONG v, int base, int width, int zero, int neg)
{
    char d[12];
    int n = 0;
    do {
        d[n++] = "0123456789abcdef"[v % (ULONG)base];
        v /= (ULONG)base;
    } while (v);
    if (neg)
        d[n++] = '-';
    while (width > n && o < end) {
        *o++ = zero ? '0' : ' ';
        width--;
    }
    while (n && o < end)
        *o++ = d[--n];
    return o;
}

void vc4_os_log(const char *fmt, ...)
{
    char line[256], *o = line, *end = line + sizeof(line) - 1;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && o < end) {
        int width = 0, zero = 0, lng = 0;
        if (*fmt != '%') {
            *o++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '0')
            zero = 1;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        switch (*fmt) {
        case 'd': {
            long v = lng > 1 ? (long)va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            o = put_num(o, end, (ULONG)(v < 0 ? -v : v), 10, width, zero, v < 0);
            break;
        }
        case 'u':
            o = put_num(o, end, lng > 1 ? (ULONG)va_arg(ap, unsigned long long)
                                : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, width, zero, 0);
            break;
        case 'x':
            o = put_num(o, end, lng > 1 ? (ULONG)va_arg(ap, unsigned long long)
                                : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, width, zero, 0);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (s && *s && o < end)
                *o++ = *s++;
            break;
        }
        case '%':
            *o++ = '%';
            break;
        default:
            *o++ = '?';
            break;
        }
        if (*fmt)
            fmt++;
    }
    *o = 0;
    va_end(ap);
    vc4_os_log_line(line);
}
