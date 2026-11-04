/*
 * vc4core's control lists and state buffers (docs/PI3_PLAN.md §3.1).
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"
#include "vc4_os.h"

#include <string.h>

#define BRANCH_ROOM VC4_BRANCH_length

int vc4_cl_init(struct vc4_cl *cl, uint32_t block_size)
{
    memset(cl, 0, sizeof(*cl));
    cl->block_size = (block_size + 4095) & ~4095u;
    if (vc4_mem_alloc(&cl->block[0], cl->block_size) < 0)
        return -1;
    cl->nblocks = 1;
    vc4_cl_reset(cl);
    return 0;
}

void vc4_cl_free(struct vc4_cl *cl)
{
    int k;
    for (k = 0; k < cl->nblocks; k++)
        vc4_mem_free(&cl->block[k]);
    memset(cl, 0, sizeof(*cl));
}

void vc4_cl_reset(struct vc4_cl *cl)
{
    cl->cur = 0;
    cl->next = cl->block[0].cpu;
    cl->end = cl->block[0].cpu + cl->block[0].size - BRANCH_ROOM;
    cl->failed = 0;
}

uint8_t *vc4_cl_room(struct vc4_cl *cl, uint32_t n)
{
    uint8_t *p;
    if (cl->next + n > cl->end) {
        /* The block is full: a BRANCH to the next (made at first use). */
        struct vc4_mem *nb;
        struct VC4_BRANCH br;
        if (cl->failed || cl->cur + 1 >= VC4_CL_BLOCKS) {
            cl->failed = 1;
            return 0;
        }
        nb = &cl->block[cl->cur + 1];
        if (cl->cur + 1 >= cl->nblocks) {
            if (vc4_mem_alloc(nb, cl->block_size) < 0) {
                cl->failed = 1;
                return 0;
            }
            cl->nblocks++;
        }
        br.address = nb->bus;
        VC4_BRANCH_pack(cl->next, &br);
        cl->cur++;
        cl->next = nb->cpu;
        cl->end = nb->cpu + nb->size - BRANCH_ROOM;
        if (cl->next + n > cl->end) {
            cl->failed = 1;
            return 0;
        }
    }
    p = cl->next;
    cl->next += n;
    return p;
}

uint32_t vc4_cl_start(const struct vc4_cl *cl)
{
    return cl->block[0].bus;
}

uint32_t vc4_cl_end(const struct vc4_cl *cl)
{
    return cl->block[cl->cur].bus + (uint32_t)(cl->next - cl->block[cl->cur].cpu);
}

int vc4_buf_init(struct vc4_buf *b, uint32_t block_size)
{
    memset(b, 0, sizeof(*b));
    b->block_size = (block_size + 4095) & ~4095u;
    if (vc4_mem_alloc(&b->block[0], b->block_size) < 0)
        return -1;
    b->nblocks = 1;
    return 0;
}

void vc4_buf_free(struct vc4_buf *b)
{
    int k;
    for (k = 0; k < b->nblocks; k++)
        vc4_mem_free(&b->block[k]);
    memset(b, 0, sizeof(*b));
}

void vc4_buf_reset(struct vc4_buf *b)
{
    b->cur = 0;
    b->used = 0;
    b->failed = 0;
}

uint8_t *vc4_buf_alloc(struct vc4_buf *b, uint32_t size, uint32_t align, uint32_t *bus)
{
    uint32_t at = (b->used + align - 1) & ~(align - 1);
    if (at + size > b->block[b->cur].size) {
        if (size > b->block_size || b->cur + 1 >= VC4_CL_BLOCKS) {
            b->failed = 1;
            return 0;
        }
        if (b->cur + 1 >= b->nblocks) {
            if (vc4_mem_alloc(&b->block[b->cur + 1], b->block_size) < 0) {
                b->failed = 1;
                return 0;
            }
            b->nblocks++;
        }
        b->cur++;
        at = 0;
    }
    b->used = at + size;
    *bus = b->block[b->cur].bus + at;
    return b->block[b->cur].cpu + at;
}
