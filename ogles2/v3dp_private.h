/*
 * The .v3dp reader's bounds-checked little-endian reads (v3dp.c), shared
 * with a backend's reader of its variants' keys and prog data
 * (docs/PI3_PLAN.md §5.1, P2 step 7).
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_V3DP_PRIVATE_H
#define OGLES2_V3DP_PRIVATE_H

struct ogles2_rd
{
    const UBYTE *p, *end;
    BOOL bad;
};

static inline ULONG need(struct ogles2_rd *r, ULONG n)
{
    if (r->bad || (ULONG)(r->end - r->p) < n) {
        r->bad = TRUE;
        return 0;
    }
    return 1;
}

static inline UBYTE u8(struct ogles2_rd *r)
{
    if (!need(r, 1))
        return 0;
    return *r->p++;
}

static inline UWORD u16(struct ogles2_rd *r)
{
    UWORD v;
    if (!need(r, 2))
        return 0;
    v = (UWORD)(r->p[0] | (r->p[1] << 8));
    r->p += 2;
    return v;
}

static inline ULONG u32(struct ogles2_rd *r)
{
    ULONG v;
    if (!need(r, 4))
        return 0;
    v = (ULONG)r->p[0] | ((ULONG)r->p[1] << 8) | ((ULONG)r->p[2] << 16) | ((ULONG)r->p[3] << 24);
    r->p += 4;
    return v;
}

static inline void skip(struct ogles2_rd *r, ULONG n)
{
    if (need(r, n))
        r->p += n;
}

#endif
