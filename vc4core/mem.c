/*
 * vc4core's GPU memory (docs/PI3_PLAN.md §3.1): blocks from the OS layer,
 * and the binner's pools.
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"
#include "vc4_os.h"

#include <string.h>

int vc4_mem_alloc(struct vc4_mem *m, uint32_t size)
{
    if (vc4_os_alloc(size, m) < 0) {
        vc4_os_log("out of GPU memory (%u bytes)", size);
        return -1;
    }
    return 0;
}

void vc4_mem_free(struct vc4_mem *m)
{
    if (m->size)
        vc4_os_free(m);
    memset(m, 0, sizeof(*m));
}

void vc4_mem_fill(struct vc4_mem *m, uint32_t off, uint32_t len, uint32_t pattern)
{
    uint32_t k;
    for (k = 0; k < len; k += 4)
        vc4_put32(m->cpu + off + k, pattern);
}

void vc4_mem_write(struct vc4_mem *m, uint32_t off, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    uint32_t k;
    for (k = 0; k < len; k += 4)
        *(volatile uint32_t *)(m->cpu + off + k) =
            (uint32_t)s[k] | (uint32_t)s[k + 1] << 8 | (uint32_t)s[k + 2] << 16 | (uint32_t)s[k + 3] << 24;
}

/* The binner's memory for tiles_x x tiles_y tiles: the tile state data
 * array (48 bytes a tile), the tile allocation (each tile's first 32-byte
 * block at 32 x its index, the render list branches there; then room for
 * the lists to grow) and overflow memory for when that is used up (BPOA,
 * BPOS). One block, which must lie in one 256 MB segment (the binner keeps
 * the top four address bits: Linux's vc4_validate.c). */
#define TALLOC_EXTRA (1u << 20)
#define OVERFLOW_CHUNKS 16 /* each binner out-of-memory request gets the next: 8 MB in all */
#ifndef OVERFLOW_SIZE
#define OVERFLOW_SIZE (512u << 10)
#endif

int vc4_pools_init(struct vc4_pools *p, uint16_t tiles_x, uint16_t tiles_y)
{
    uint32_t tiles = (uint32_t)tiles_x * tiles_y;
    uint32_t tsda_size = (tiles * 48 + 4095) & ~4095u;
    uint32_t talloc_size = (tiles * 32 + TALLOC_EXTRA + 4095) & ~4095u;
    memset(p, 0, sizeof(*p));
    if (vc4_mem_alloc(&p->mem, tsda_size + talloc_size + OVERFLOW_CHUNKS * OVERFLOW_SIZE) < 0)
        return -1;
    if ((p->mem.bus & 0xf0000000u) != ((p->mem.bus + p->mem.size - 1) & 0xf0000000u)) {
        vc4_os_log("the binner's pools cross a 256 MB boundary (0x%08x + %u)", p->mem.bus, p->mem.size);
        vc4_mem_free(&p->mem);
        return -1;
    }
    p->tiles_x = tiles_x;
    p->tiles_y = tiles_y;
    p->tsda = p->mem.bus;
    p->talloc = p->mem.bus + tsda_size;
    p->talloc_size = talloc_size;
    p->overflow = p->talloc + talloc_size;
    p->overflow_size = OVERFLOW_SIZE;
    p->overflow_chunks = OVERFLOW_CHUNKS;
    return 0;
}

#ifdef VC4_SEPARATE_OVERFLOW
/* A test build's (PI3_PLAN §3.1, open): chunks 1 and up each in a block of
 * its own, wherever the allocator puts it, instead of in the pools' one. */
static struct vc4_mem separate[OVERFLOW_CHUNKS];
#endif

uint32_t vc4_pools_overflow(struct vc4_pools *p, uint32_t n, uint32_t *size)
{
    if (n >= p->overflow_chunks)
        return 0;
    *size = p->overflow_size;
#ifdef VC4_SEPARATE_OVERFLOW
    if (n) {
        if (!separate[n].size && vc4_mem_alloc(&separate[n], p->overflow_size) < 0)
            return 0;
        return separate[n].bus;
    }
#endif
    return p->overflow + n * p->overflow_size;
}

void vc4_pools_free(struct vc4_pools *p)
{
#ifdef VC4_SEPARATE_OVERFLOW
    int k;
    for (k = 0; k < OVERFLOW_CHUNKS; k++)
        if (separate[k].size)
            vc4_mem_free(&separate[k]);
#endif
    vc4_mem_free(&p->mem);
    memset(p, 0, sizeof(*p));
}
