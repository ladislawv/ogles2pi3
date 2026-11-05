/*
 * The .v3dp reader (v3dp/FORMAT.md, versions 1.0 and 1.1): a program binary from the
 * offline compiler (v3dglslc, S1) or the on-target one (S2) becomes a
 * struct OGLES2Image, with its code copied once into GPU memory.
 *
 * Every field is read explicitly, little-endian, with bounds checks; a file
 * the runtime cannot honour is refused with a reason rather than drawn
 * wrongly: an unknown major version, V3D version or Mesa commit, a bad CRC,
 * a uniform recipe type or state variable the runtime does not implement
 * (plan sections 8.3, 8.6, 7.11). A variant's key, prog data and recipe
 * types are the backend's to read and judge (backend.h, P2 step 7).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "v3dp_private.h"
#include "v3d_types.h" /* struct v3d_mem: the code's GPU memory */

/* Mesa commits whose recipe and state numbering the runtime implements. */
static const char *const validated_commits[] = {"74b69717c52b"};

/* ---- allocations: one list per image ---- */

static APTR alloc(struct OGLES2Image *im, ULONG size)
{
    APTR *b = AllocVec(size + 8, MEMF_ANY | MEMF_CLEAR);
    if (!b)
        return 0;
    b[0] = im->blocks; /* the list runs through the first word */
    im->blocks = b;
    return (UBYTE *)b + 8;
}

static void free_blocks(struct OGLES2Image *im)
{
    APTR *b = im->blocks, *next;
    while (b) {
        next = b[0];
        FreeVec(b);
        b = next;
    }
    im->blocks = 0;
}

/* ---- the reader ---- */


static ULONG crc32(const UBYTE *p, ULONG n)
{
    ULONG c = 0xffffffffUL;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320UL & (0UL - (c & 1)));
    }
    return ~c;
}

static void say(char *why, ULONG len, const char *a, const char *b, ULONG n)
{
    ULONG i = 0;
    const char *s;
    char num[12];
    int k = 0;
    for (s = a; *s && i + 1 < len; s++)
        why[i++] = *s;
    for (s = b; s && *s && i + 1 < len; s++)
        why[i++] = *s;
    if (n != ~0UL) {
        do
            num[k++] = (char)('0' + n % 10);
        while ((n /= 10) && k < 11);
        if (i + 1 < len)
            why[i++] = ' ';
        while (k && i + 1 < len)
            why[i++] = num[--k];
    }
    why[i] = 0;
}

/* A string table entry; "" for an offset outside the table. */
static const char *str_at(const char *strings, ULONG len, ULONG off)
{
    return off < len ? strings + off : "";
}

#define FAIL(a, b, n) do { say(why, whylen, (a), (b), (n)); goto fail; } while (0)



struct OGLES2Image *ogles2_v3dp_load(const UBYTE *file, ULONG len, char *why, ULONG whylen)
{
    struct OGLES2Image *im = 0;
    struct ogles2_rd r, s;
    ULONG nsections, i, k, stages = 0;
    const char *strings = 0;
    ULONG strings_len = 0;
    struct OGLES2Variant *vars = 0;
    ULONG nvars = 0;
    const UBYTE *sec_start[64];
    ULONG sec_type[64], sec_len[64];
    BOOL commit_ok = FALSE;

    why[0] = 0;
    if (len < 32 || memcmp(file, "V3DP", 4))
        FAIL("not a .v3dp file", 0, ~0UL);
    r.p = file + 4;
    r.end = file + len;
    r.bad = FALSE;
    if (u16(&r) != 1)
        FAIL("unknown format major version", 0, ~0UL);
    (void)u16(&r);
    if (u32(&r) != ogles2_backend->device) /* the GPU's (FORMAT.md 1.1: 42 V3D 4.2, 21 VC4) */
        FAIL("not compiled for this GPU", 0, ~0UL);
    for (i = 0; i < sizeof(validated_commits) / sizeof(validated_commits[0]); i++)
        if (!memcmp(r.p, validated_commits[i], 12))
            commit_ok = TRUE;
    if (!commit_ok)
        FAIL("compiled by a Mesa commit this library does not know", 0, ~0UL);
    r.p += 12;
    nsections = u32(&r);
    if (u32(&r) != crc32(file + 32, len - 32))
        FAIL("CRC mismatch (damaged file)", 0, ~0UL);
    if (nsections > 64)
        FAIL("too many sections:", 0, nsections);

    /* Section index. */
    for (i = 0; i < nsections; i++) {
        sec_type[i] = u32(&r);
        sec_len[i] = u32(&r);
        sec_start[i] = r.p;
        skip(&r, (sec_len[i] + 3) & ~3UL);
        if (r.bad)
            FAIL("truncated section", 0, i);
        if (sec_type[i] == ogles2_backend->variant_section)
            nvars++;
    }
    if (nsections < 2 || sec_type[0] != 1 || sec_type[1] != 2)
        FAIL("PROGRAM and STRINGS must come first", 0, ~0UL);

    im = AllocVec(sizeof(*im), MEMF_ANY | MEMF_CLEAR);
    if (!im)
        FAIL("out of memory", 0, ~0UL);
    vars = alloc(im, (nvars ? nvars : 1) * sizeof(*vars));
    if (!vars)
        FAIL("out of memory", 0, ~0UL);
    strings = (const char *)sec_start[1];
    strings_len = sec_len[1];
    if (!strings_len || strings[strings_len - 1])
        FAIL("STRINGS not NUL-terminated", 0, ~0UL);

#define STR(off) str_at(strings, strings_len, (off))

    for (i = 0; i < nsections; i++) {
        s.p = sec_start[i];
        s.end = sec_start[i] + sec_len[i];
        s.bad = FALSE;
        /* The backend's variant section (FORMAT.md: 6 V3D 4.2, 7 VC4) as 6; another GPU's as unknown. */
        switch (sec_type[i] == ogles2_backend->variant_section ? 6UL : sec_type[i] == 6 ? 0UL : sec_type[i]) {
        case 1: /* PROGRAM */
            if (sec_len[i] < 36)
                FAIL("short PROGRAM section", 0, ~0UL);
            memcpy(im->hash, s.p, 32);
            s.p += 32;
            stages = u32(&s);
            break;
        case 3: { /* STAGE_PARAMS */
            ULONG stage = u32(&s), nvalues = u32(&s), nparams = u32(&s), si;
            ULONG *tmpl;
            struct OGLES2StateVar *sv;
            if (stage != 0 && stage != 4)
                FAIL("STAGE_PARAMS for an unknown stage", 0, stage);
            si = stage == 4;
            if (nvalues > 4096)
                FAIL("constant buffer too large:", 0, nvalues);
            /* Parameters: only state variables matter (the template holds the rest). */
            {
                const UBYTE *save = s.p;
                ULONG nstate = 0;
                for (k = 0; k < nparams; k++) {
                    if (u32(&s) == 2)
                        nstate++;
                    skip(&s, 4 * 4 + 8);
                }
                s.p = save;
                sv = alloc(im, (im->nstatevars + nstate + 1) * sizeof(*sv));
                if (!sv)
                    FAIL("out of memory", 0, ~0UL);
                if (im->nstatevars)
                    memcpy(sv, im->statevars, im->nstatevars * sizeof(*sv));
            }
            for (k = 0; k < nparams; k++) {
                ULONG kind = u32(&s), name = u32(&s), off = u32(&s), size = u32(&s);
                UWORD st;
                (void)u32(&s); /* GL type */
                st = u16(&s);
                skip(&s, 6);
                (void)name;
                (void)size;
                if (kind == 2) {
                    if (st != OGLES2_STATE_DEPTH_RANGE && st != OGLES2_STATE_FB_WPOS_Y_TRANSFORM &&
                        st != OGLES2_STATE_FB_PNTC_Y_TRANSFORM)
                        FAIL("state variable not implemented: ", STR(name), st);
                    if (off + 4 > nvalues)
                        FAIL("state variable outside the constant buffer", 0, ~0UL);
                    sv[im->nstatevars].stage = (UBYTE)si;
                    sv[im->nstatevars].offset = (UWORD)off;
                    sv[im->nstatevars].state = st;
                    im->nstatevars++;
                }
            }
            im->statevars = sv;
            tmpl = alloc(im, nvalues * 4 + 4);
            if (!tmpl)
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < nvalues; k++)
                tmpl[k] = u32(&s);
            im->cbuf_words[si] = (UWORD)nvalues;
            im->cbuf_template[si] = tmpl;
            break;
        }
        case 4: { /* UNIFORMS */
            ULONG n = u32(&s);
            struct OGLES2ImageUniform *u;
            if (n > 1024)
                FAIL("too many uniforms:", 0, n);
            u = alloc(im, (n + 1) * sizeof(*u));
            if (!u)
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < n; k++) {
                ULONG np, j;
                struct OGLES2Place *pl;
                u[k].name = STR(u32(&s));
                u[k].type = u32(&s);
                u[k].array = (UWORD)u32(&s);
                u[k].flags = (UBYTE)u32(&s);
                np = u32(&s);
                if (np > 2)
                    FAIL("uniform with more than two storage places: ", u[k].name, np);
                pl = alloc(im, (np + 1) * sizeof(*pl));
                if (!pl)
                    FAIL("out of memory", 0, ~0UL);
                for (j = 0; j < np; j++) {
                    ULONG stage = u32(&s);
                    pl[j].stage = stage == 4;
                    pl[j].offset = (UWORD)u32(&s);
                    pl[j].elem_stride = (UBYTE)(u8(&s) / 4);
                    pl[j].vec_stride = (UBYTE)(u8(&s) / 4);
                    pl[j].int_as_float = u8(&s);
                    (void)u8(&s);
                }
                u[k].nplaces = (UBYTE)np;
                u[k].places = pl;
                u[k].sampler[0] = u32(&s);
                u[k].sampler[1] = u32(&s);
            }
            im->nuniforms = (UWORD)n;
            im->uniforms = u;
            break;
        }
        case 5: { /* ATTRIBUTES */
            ULONG n = u32(&s);
            struct OGLES2ImageAttrib *a;
            if (n > OGLES2_MAX_IMAGE_ATTRIBS)
                FAIL("too many attributes:", 0, n);
            a = alloc(im, (n + 1) * sizeof(*a));
            if (!a)
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < n; k++) {
                ULONG slot, loc;
                a[k].name = STR(u32(&s));
                a[k].type = u32(&s);
                loc = u32(&s); /* the linker's location (informational); its columns read */
                slot = u32(&s);
                a[k].slot = slot > 15 ? 0xff : (UBYTE)slot;
                a[k].columns = a[k].type == GL_FLOAT_MAT2 ? 2 : a[k].type == GL_FLOAT_MAT3 ? 3
                             : a[k].type == GL_FLOAT_MAT4 ? 4 : 1;
                a[k].colmask = (loc & 0x80000000UL) ? (UBYTE)((loc >> 8) & 15) : 0;
            }
            im->nattribs = (UBYTE)n;
            im->attribs = a;
            break;
        }
        case 6: { /* VARIANT */
            struct OGLES2Variant *v = &vars[im->nvariants];
            ULONG n, j, codelen, bad;
            const char *e;
            BOOL clip_z = FALSE;
            ULONG *rt, *rd2;
            v->stage = (UBYTE)u32(&s);
            if ((e = ogles2_backend->variant_read(&s, v)) != 0) /* its key and prog data */
                FAIL(e, 0, v->stage);
            n = u32(&s);
            if (n > 4096)
                FAIL("uniform recipe too long:", 0, n);
            rt = alloc(im, n * 8 + 8);
            if (!rt)
                FAIL("out of memory", 0, ~0UL);
            rd2 = rt + n;
            for (j = 0; j < n; j++)
                rt[j] = u32(&s);
            for (j = 0; j < n; j++)
                rd2[j] = u32(&s);
            v->nrecipe = (UWORD)n;
            v->rtype = rt;
            v->rdata = rd2;
            /* The recipe's types are the GPU's (the backend judges them; 0 a
             * constant and 1 a constant buffer word on both GPUs). */
            bad = ~0UL;
            if ((e = ogles2_backend->variant_recipe(v, &clip_z, &bad)) != 0)
                FAIL(e, 0, bad);
            if (clip_z)
                im->clip_z_coeff = TRUE;
            {
                UWORD *ui = alloc(im, n * 4 + 4), *sp = ui ? ui + n : 0, ns = 0;
                if (!ui)
                    FAIL("out of memory", 0, ~0UL);
                for (j = 0; j < n; j++) {
                    if (rt[j] == OGLES2_U_UNIFORM && rd2[j] < 65536) {
                        ui[j] = (UWORD)rd2[j];
                    } else {
                        ui[j] = 0;
                        sp[ns++] = (UWORD)j;
                    }
                }
                v->uidx = ui;
                v->special = sp;
                v->nspecial = ns;
            }
            codelen = u32(&s);
            if ((codelen & 7) || !need(&s, codelen))
                FAIL("bad code size", 0, codelen);
            v->code = s.p;
            v->code_bytes = codelen;
            s.p += codelen;
            if (s.bad)
                FAIL("truncated VARIANT section", 0, ~0UL);
            im->nvariants++;
            break;
        }
        default:
            break; /* STRINGS, and sections from newer minor versions */
        }
        if (s.bad)
            FAIL("truncated section", 0, i);
    }
    if (stages != 0x11)
        FAIL("not a vertex and fragment program", 0, ~0UL);
    im->variants = vars;

    /* The code: every variant, 8-byte aligned, in one block of GPU memory;
     * without a GPU (a state-only context: tests) in ordinary memory, never
     * drawn with. */
    if (!ogles2_device_ready()) {
        ULONG off = 0;
        UBYTE *c;
        for (k = 0; k < im->nvariants; k++) {
            vars[k].code_offset = off;
            off += vars[k].code_bytes;
        }
        if (!(c = alloc(im, off + 8)))
            FAIL("out of memory", 0, ~0UL);
        for (k = 0; k < im->nvariants; k++) {
            memcpy(c + vars[k].code_offset, vars[k].code, vars[k].code_bytes);
            vars[k].code = c + vars[k].code_offset;
        }
        im->code_total = off;
    } else {
        struct v3d_mem *m = alloc(im, sizeof(*m));
        ULONG off = 0, clen;
        if (!m)
            FAIL("out of memory", 0, ~0UL);
        for (k = 0; k < im->nvariants; k++) {
            vars[k].code_offset = off;
            off += vars[k].code_bytes;
        }
        if (!off || ogles2_mem_alloc(m, off) < 0)
            FAIL("out of GPU memory for the code", 0, ~0UL);
        im->code_mem = m;
        im->code_total = off;
        for (k = 0; k < im->nvariants; k++) {
            memcpy((UBYTE *)m->hostptr + vars[k].code_offset, vars[k].code, vars[k].code_bytes);
            vars[k].code = (const UBYTE *)m->hostptr + vars[k].code_offset;
        }
        clen = m->size;
        CachePreDMA(m->hostptr, &clen, 0);
    }
    /* Names: STRINGS points into `file`, which goes; keep a copy. */
    {
        char *copy = alloc(im, strings_len);
        ULONG d = (ULONG)copy - (ULONG)strings;
        if (!copy)
            FAIL("out of memory", 0, ~0UL);
        memcpy(copy, strings, strings_len);
        for (k = 0; k < im->nattribs; k++)
            ((struct OGLES2ImageAttrib *)im->attribs)[k].name += d;
        for (k = 0; k < im->nuniforms; k++)
            ((struct OGLES2ImageUniform *)im->uniforms)[k].name += d;
    }
    im->name = "program binary";
    return im;

fail:
    if (im) {
        if (im->code_mem && im->code_mem->hostptr)
            ogles2_mem_free(im->code_mem);
        free_blocks(im);
        FreeVec(im);
    }
    return 0;
}

void ogles2_image_free(struct OGLES2Image *im, struct OGLES2Context *ctx)
{
    if (!im)
        return;
    if (im->code_mem && im->code_mem->hostptr) {
        /* The GPU may still run this code in the open job: free it after
         * that job (the backend's mem_free_after), or now. */
        if (ctx && ctx->job_open)
            ogles2_backend->mem_free_after(ctx, im->code_mem);
        else
            ogles2_mem_free(im->code_mem);
    }
    if (im->binary)
        FreeVec(im->binary);
    ogles2_image_free(im->wide, ctx); /* its code: deferred the same way */
    ogles2_image_free(im->a2c, ctx);
    ogles2_image_free(im->keyed, ctx);
    if (im->be_cache)
        FreeVec(im->be_cache);
    if (im->bind_src)
        FreeVec(im->bind_src);
    if (im->vs_src)
        FreeVec(im->vs_src);
    if (im->fs_src)
        FreeVec(im->fs_src);
    free_blocks(im);
    FreeVec(im);
}


/* The variant for a draw: the backend's choice (its keys). */
const struct OGLES2Variant *ogles2_image_variant(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                 GLenum mode)
{
    return ogles2_backend->variant_choose(im, stage, cbufs, mode);
}
