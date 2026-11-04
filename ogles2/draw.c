/*
 * The draw path (plan section 7.9): client-side vertex arrays and
 * glDrawArrays with the linked program's image: a .v3dp program binary's
 * compiled variants (M2), or the built-in image for clears.
 *
 * Here: the GL entry points and their checks, the variants a draw uses, the
 * program's state variables and its vertex input slots, the clears drawn as
 * rectangles and the copies on the GPU. What a draw puts into the job is
 * the backend's (backend.h; V3D 4.2: ogles2/v3d/draw_v3d.c, P2 step 4).
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "draw_private.h"
#include "v3d_types.h"


struct OGLES2Draw
{
    v3d_mem builtin_code;          /* the built-in image's code, uploaded at first use */
    struct OGLES2Program quad;     /* clears drawn as rectangles (built-in image) */
    BOOL quad_ready;
    /* and into several render targets (EXT_draw_buffers): clear_mrt_v3dp.h */
    struct OGLES2Program mrt_quad;
    struct OGLES2Image *mrt_image;
    BOOL mrt_ready, mrt_failed;
    /* and glCopyTex* on the GPU (copy.c): copy_tex_v3dp.h */
    struct OGLES2Program copy_prog;
    struct OGLES2Image *copy_image;
    BOOL copy_ready, copy_failed;
    const struct OGLES2Image *missed; /* the last image a variant miss was logged for */
    const struct OGLES2Image *missed_rt; /* and a render-target miss (EXT_draw_buffers) */
};

/* ---- vertex arrays ---- */

void ogles2_glVertexAttribPointer(struct OGLES2Context *ctx, GLuint index, GLint size, GLenum type,
                                  GLboolean normalized, GLsizei stride, const void *pointer)
{
    struct OGLES2Array *a;
    if (!ogles2_context_valid(ctx))
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS || size < 1 || size > 4 || stride < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!type_size(type)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    a = &ctx->array[index];
    a->size = size;
    a->type = type;
    a->normalized = normalized ? GL_TRUE : GL_FALSE;
    a->stride = stride;
    a->pointer = pointer;
    a->buffer = ogles2_buffer_array(ctx); /* GLES 2.0 2.9: captured now */
}

static void enable_array(struct OGLES2Context *ctx, GLuint index, GLboolean on)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->array[index].enabled = on;
}

void ogles2_glEnableVertexAttribArray(struct OGLES2Context *ctx, GLuint index)
{
    enable_array(ctx, index, GL_TRUE);
}

void ogles2_glDisableVertexAttribArray(struct OGLES2Context *ctx, GLuint index)
{
    enable_array(ctx, index, GL_FALSE);
}

void ogles2_glGetVertexAttribiv(struct OGLES2Context *ctx, GLuint index, GLenum pname,
                                GLint *params)
{
    const struct OGLES2Array *a;
    int k;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    a = &ctx->array[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED:        *params = a->enabled; break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE:           *params = a->size; break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE:         *params = a->stride; break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE:           *params = (GLint)a->type; break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:     *params = a->normalized; break;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: *params = (GLint)ogles2_buffer_name(a->buffer); break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (k = 0; k < 4; k++)
            params[k] = (GLint)ctx->state.attrib[index][k];
        break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void ogles2_glGetVertexAttribPointerv(struct OGLES2Context *ctx, GLuint index, GLenum pname,
                                      void **pointer)
{
    if (!ogles2_context_valid(ctx) || !pointer)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    *pointer = (void *)ctx->array[index].pointer;
}
/* ---- per-context draw state ---- */

static struct OGLES2Draw *draw_state(struct OGLES2Context *ctx)
{
    struct OGLES2Draw *d = ctx->draw;
    if (!d) {
        if (!(d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR)))
            return 0;
        ctx->draw = d;
    }
    return d;
}

/* The GPU address of a variant's code. A loaded image carries its own; the
 * built-in one is uploaded once per context (it is static data). */
static ULONG code_address(struct OGLES2Draw *d, const struct OGLES2Image *im,
                          const struct OGLES2Variant *v)
{
    if (im->code_mem)
        return (ULONG)im->code_mem->hostptr + v->code_offset;
    if (im != ogles2_builtin_image())
        return 0; /* loaded without a GPU: never drawn */
    if (!d->builtin_code.hostptr) {
        ULONG len;
        UWORD i;
        if (ogles2_mem_alloc(&d->builtin_code, im->code_total) < 0) {
            d->builtin_code.hostptr = 0;
            return 0;
        }
        for (i = 0; i < im->nvariants; i++)
            memcpy((UBYTE *)d->builtin_code.hostptr + im->variants[i].code_offset,
                   im->variants[i].code, im->variants[i].code_bytes);
        len = d->builtin_code.size;
        CachePreDMA(d->builtin_code.hostptr, &len, 0);
    }
    return (ULONG)d->builtin_code.hostptr + v->code_offset;
}

void ogles2_draw_free(struct OGLES2Context *ctx)
{
    struct OGLES2Draw *d = ctx->draw;
    ogles2_backend->draw_free(ctx);
    if (!d)
        return;
    if (d->builtin_code.hostptr)
        ogles2_mem_free(&d->builtin_code);
    if (d->quad_ready)
        ogles2_program_builtin_free(&d->quad);
    if (d->mrt_ready)
        ogles2_program_builtin_free(&d->mrt_quad);
    if (d->mrt_image)
        ogles2_image_free(d->mrt_image, ctx);
    if (d->copy_ready)
        ogles2_program_builtin_free(&d->copy_prog);
    if (d->copy_image)
        ogles2_image_free(d->copy_image, ctx);
    FreeVec(d);
    ctx->draw = 0;
}

/* The program's state variables (FORMAT.md STAGE_PARAMS kind 2), computed
 * into its constant buffers before a draw, as Mesa's prog_statevars.c does
 * for a framebuffer drawn upside down (FlipY: the default framebuffer). */
static void update_state_vars(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    const struct OGLES2Image *im = p->image;
    const GLfloat *dr = ctx->state.depth_range;
    UWORD i;
    for (i = 0; i < im->nstatevars; i++) {
        const struct OGLES2StateVar *sv = &im->statevars[i];
        GLfloat v[4];
        int k;
        switch (sv->state) {
        case OGLES2_STATE_DEPTH_RANGE:
            v[0] = dr[0];
            v[1] = dr[1];
            v[2] = dr[1] - dr[0];
            v[3] = 1.0f;
            break;
        case OGLES2_STATE_FB_WPOS_Y_TRANSFORM:
            /* Mesa's prog_statevars.c: flipped (-1, height, 1, 0), else
             * (1, 0, -1, height) */
            v[0] = ctx->fb_flip ? -1.0f : 1.0f;
            v[1] = ctx->fb_flip ? (GLfloat)ctx->fb_h : 0.0f;
            v[2] = ctx->fb_flip ? 1.0f : -1.0f;
            v[3] = ctx->fb_flip ? 0.0f : (GLfloat)ctx->fb_h;
            break;
        case OGLES2_STATE_FB_PNTC_Y_TRANSFORM:
            /* upper-left sprite origin: inverted on a flipped buffer only */
            v[0] = ctx->fb_flip ? -1.0f : 1.0f;
            v[1] = ctx->fb_flip ? 1.0f : 0.0f;
            v[2] = 0.0f;
            v[3] = 0.0f;
            break;
        default:
            continue;
        }
        for (k = 0; k < 4; k++)
            if (p->cbuf[sv->stage][sv->offset + k] != float_bits(v[k]))
                ogles2_cbuf_set(p, sv->stage, sv->offset + k, float_bits(v[k]));
    }
}

/* A fragment-shader sampler of the program reads a texture the TMU returns
 * 32 bits for (a float or depth texture): the draw needs the 32-bit
 * variants (Mesa's sampler_is_32b key). */
static BOOL wants_wide(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    const struct OGLES2Image *im = p->image;
    UWORD k, n = list_samplers(p);
    for (k = 0; k < n; k++) {
        UWORD u = p->nsamplers == 0xfe ? k : p->samplers[k];
        const struct OGLES2ImageUniform *un = &im->uniforms[u];
        UWORD e, ne = un->array ? un->array : 1;
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER) || un->sampler[1] >= OGLES2_MAX_STAGE_SAMPLERS)
            continue;
        for (e = 0; e < ne; e++)
            if (ogles2_texture_wide(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type)))
                return TRUE;
    }
    return FALSE;
}

/* ---- glDrawArrays ---- */

static int column_slot(const struct OGLES2ImageAttrib *at, int k);

/* Probe V (vc4keys.c, docs/PI3_PLAN.md §4.2): the draw's vertex inputs and
 * the textures its samplers read, for the VC4 key count. */
static void vc4keys_glue(struct OGLES2Context *ctx, struct OGLES2Program *p, GLenum mode, GLenum fsmode,
                         const struct OGLES2Variant *vs, int nslots)
{
    const struct OGLES2Image *im = p->image;
    struct OGLES2Vc4KeyDraw in;
    UWORD k, n = list_samplers(p);
    int i, st;

    memset(&in, 0, sizeof(in));
    in.prim = (UBYTE)(fsmode == GL_POINTS ? 2
                      : fsmode == GL_LINES || fsmode == GL_LINE_LOOP || fsmode == GL_LINE_STRIP ? 1 : 0);
    in.psiz = (UBYTE)(mode == GL_POINTS && vs->writes_psiz);
    in.nslots = (UBYTE)nslots;
    for (i = 0; i < nslots && i < 16; i++) {
        GLuint g = p->slot_attrib[i];
        if (g < OGLES2_MAX_VERTEX_ATTRIBS && ctx->array[g].enabled) {
            GLenum t = ctx->array[g].type;
            in.attr[i][0] = (UBYTE)(t ^ (t >> 8));
            in.attr[i][1] = (UBYTE)ctx->array[g].size;
            in.attr[i][2] = (UBYTE)(ctx->array[g].normalized ? 1 : 0);
        } else {
            in.attr[i][0] = 0xff; /* the current value, given as floats */
        }
    }
    for (k = 0; k < n; k++) {
        UWORD u = p->nsamplers == 0xfe ? k : p->samplers[k];
        const struct OGLES2ImageUniform *un = &im->uniforms[u];
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER))
            continue;
        for (st = 0; st < 2; st++) {
            UWORD e, ne = un->array ? un->array : 1;
            for (e = 0; e < ne; e++) {
                ULONG idx = un->sampler[st] + e;
                UBYTE *d;
                if (un->sampler[st] >= OGLES2_MAX_STAGE_SAMPLERS || idx >= OGLES2_MAX_STAGE_SAMPLERS)
                    break;
                d = in.samp[st][idx];
                if (!ogles2_texture_stored(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type), &d[0], &d[1]))
                    d[0] = 0xfd;
                if (idx + 1 > in.nsamp[st])
                    in.nsamp[st] = (UBYTE)(idx + 1);
            }
        }
    }
    ogles2_vc4keys_draw(ctx, p, &in);
}

/* Draws with program p and the context's state; arguments are valid. The
 * vertices [first, first+count) are copied; without `el` they are drawn in
 * order, with it through its indices. */
static void draw_arrays(struct OGLES2Context *ctx, struct OGLES2Program *p, GLenum mode,
                        GLint first, GLsizei count, const struct OGLES2Elements *el)
{
    const struct OGLES2Image *im = p->image;
    const struct OGLES2Variant *cs, *vs, *fs;
    const struct OGLES2Image *fim; /* the fragment shader's image */
    GLenum fsmode;
    struct OGLES2Draw *d;
    struct OGLES2DrawCall dc;
    ULONG code[3];
    int a, nslots = 0, i;

    if (!(d = draw_state(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    mark(ctx, 0);
    /* The program's variants for this primitive mode, as at its last draw
     * (they and their code stay until it is linked again). The fragment
     * shader's is the class the rasterizer sees: triangles in glPolygonMode's
     * GL_LINE or GL_POINT are lines or points to it (Mesa's
     * v3d_rasterizes_lines/points). */
    fsmode = mode >= GL_TRIANGLES && ctx->state.polygon_mode != GL_FILL
                 ? (ctx->state.polygon_mode == GL_POINT ? GL_POINTS : GL_LINES) : mode;
    /* And from the program's 32-bit-sampler variants while a float or depth
     * texture is bound to one of its samplers (program.c, the .v3dw). */
    fim = im;
    if (ogles2_wide_seen && wants_wide(ctx, p)) {
        const struct OGLES2Image *w = ogles2_image_wide((struct OGLES2Image *)im);
        if (w)
            fim = w;
    }
    /* A 4x MSAA default framebuffer with GL_SAMPLE_ALPHA_TO_COVERAGE: the
     * program's alpha-to-coverage variants (the .v3da; Mesa turns alpha into
     * the sample mask in the fragment shader). Not with 32-bit samplers at
     * the same time: no file holds both, and those keep their variants. */
    if (ctx->msaa && !ctx->draw_fb && (ctx->state.caps & OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE) && fim == im) {
        const struct OGLES2Image *a = ogles2_image_a2c((struct OGLES2Image *)im);
        if (a)
            fim = a;
    }
    if (p->var_ok && p->var_mode == mode && p->var_fsmode == fsmode && p->var_fim == fim &&
        p->var_nrt == ctx->fb_nrt) {
        cs = p->var[0];
        vs = p->var[1];
        fs = p->var[2];
        code[0] = p->var_code[0];
        code[1] = p->var_code[1];
        code[2] = p->var_code[2];
    } else {
        UBYTE cbufs = (UBYTE)((1 << ctx->fb_nrt) - 1);
        cs = ogles2_image_variant(im, OGLES2_VARIANT_CS, 0, mode);
        vs = ogles2_image_variant(im, OGLES2_VARIANT_VS, 0, mode);
        fs = ogles2_image_variant(fim, OGLES2_VARIANT_FS, cbufs, fsmode);
        if (!cs || !vs || !fs) {
            ogles2_log("glDrawArrays: the program has no shader for this primitive; nothing drawn");
            return;
        }
        if (d->missed != im && (fs->is_points != (fsmode == GL_POINTS) ||
                                fs->is_lines != (fsmode == GL_LINES || fsmode == GL_LINE_LOOP ||
                                                 fsmode == GL_LINE_STRIP))) {
            d->missed = im; /* plan 7.8: the closest variant is used; logged once per program */
            ogles2_log("glDrawArrays: no fragment shader variant for primitive %ld; using the closest",
                       (ULONG)mode);
        }
        if (d->missed_rt != im && (fs->cbufs ? fs->cbufs : 1) != cbufs) {
            d->missed_rt = im; /* logged once per program */
            ogles2_log("glDrawArrays: no fragment shader variant for %ld render targets "
                       "(EXT_draw_buffers): the program draws into the first only", (ULONG)ctx->fb_nrt);
        }
        code[0] = code_address(d, im, cs);
        code[1] = code_address(d, im, vs);
        code[2] = code_address(d, fim, fs);
        if (!code[0] || !code[1] || !code[2]) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        p->var[0] = cs;
        p->var[1] = vs;
        p->var[2] = fs;
        p->var_code[0] = code[0];
        p->var_code[1] = code[1];
        p->var_code[2] = code[2];
        p->var_mode = mode;
        p->var_fsmode = fsmode;
        p->var_fim = fim;
        p->var_nrt = ctx->fb_nrt;
        p->var_ok = TRUE;
    }

    ogles2_job_begin(ctx);

    mark(ctx, 1);
    update_state_vars(ctx, p);
    /* One attribute record per vertex input slot, from the generic attribute
     * bound to it (a matrix: one slot and one generic attribute per column). */
    /* Mapped at the program's first draw (attribute locations are fixed
     * at link). */
    if (p->nslots == 0xff) {
        int k, cs;
        for (a = 0; a < im->nattribs; a++)
            for (k = 0; k < im->attribs[a].columns; k++)
                if ((cs = column_slot(&im->attribs[a], k)) >= 0 && cs + 1 > nslots)
                    nslots = cs + 1;
        if (nslots > 16)
            nslots = 16;
        for (i = 0; i < nslots; i++) {
            GLuint g = 0xff;
            for (a = 0; a < im->nattribs; a++)
                for (k = 0; k < im->attribs[a].columns; k++)
                    if (column_slot(&im->attribs[a], k) == (int)i)
                        g = p->attrib_index[a] + k;
            p->slot_attrib[i] = (UBYTE)(g < OGLES2_MAX_VERTEX_ATTRIBS ? g : 0xff);
        }
        p->nslots = (UBYTE)nslots;
    }
    nslots = p->nslots;
    dc.p = p;
    dc.var[0] = cs;
    dc.var[1] = vs;
    dc.var[2] = fs;
    dc.code = code;
    dc.mode = mode;
    dc.first = first;
    dc.count = count;
    dc.el = el;
    if (!ogles2_backend->draw(ctx, &dc)) {
        ogles2_log("glDrawArrays: out of memory for the draw's GPU data; frame dropped");
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (ctx->probe & OGLES2_PROBE_VC4KEYS)
        vc4keys_glue(ctx, p, mode, fsmode, vs, nslots);
}

/* The vertex input slot of column k of an attribute (a matrix's columns are
 * generic attributes of their own), -1 if the shader does not read it. */
static int column_slot(const struct OGLES2ImageAttrib *at, int k)
{
    int j, n = 0;
    if (at->slot == 0xff)
        return -1;
    if (!at->colmask) /* every column, from the slot on */
        return at->slot + k;
    if (!(at->colmask & (1 << k)))
        return -1;
    for (j = 0; j < k; j++)
        if (at->colmask & (1 << j))
            n++;
    return at->slot + n;
}

/* Every enabled array the program reads has data, and a buffer's holds
 * vertices [0, end) (GLES2 leaves reading past it undefined: not drawn). */
static BOOL arrays_ok(struct OGLES2Context *ctx, const struct OGLES2Program *p, ULONG end,
                      const char *fn)
{
    int a;
    for (a = 0; a < p->image->nattribs; a++) {
        const struct OGLES2Array *arr = &ctx->array[p->attrib_index[a]];
        if (p->image->attribs[a].slot == 0xff || !arr->enabled)
            continue;
        if (arr->buffer) {
            ULONG es = (ULONG)(type_size(arr->type) * arr->size);
            ULONG st = arr->stride ? (ULONG)arr->stride : es;
            ULONG off = (ULONG)arr->pointer, size = ogles2_buffer_size(arr->buffer);
            if (off + es > size || (size - off - es) / st + 1 < end) {
                if (fn)
                    ogles2_log("%s: attribute %ld reads past the end of buffer %ld; nothing drawn",
                               (ULONG)fn, (ULONG)p->attrib_index[a], (ULONG)ogles2_buffer_name(arr->buffer));
                return FALSE;
            }
        } else if (!arr->pointer) {
            if (fn)
                ogles2_log("%s: attribute %ld is enabled without an array; nothing drawn", (ULONG)fn,
                           (ULONG)p->attrib_index[a]);
            return FALSE;
        }
    }
    return TRUE;
}

void ogles2_glDrawArrays(struct OGLES2Context *ctx, GLenum mode, GLint first, GLsizei count)
{
    ULONG t0 = ogles2_clock_us();
    struct OGLES2Program *p;

    if (!ogles2_context_valid(ctx))
        return;
    mark(ctx, -1);
    if (mode > GL_TRIANGLE_FAN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (first < 0 || count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    p = ctx->program;
    if (!p || !ctx->v3d || count == 0) /* no program: undefined; nothing is drawn */
        return;
    if (!arrays_ok(ctx, p, (ULONG)first + (ULONG)count, "glDrawArrays"))
        return;
    draw_arrays(ctx, p, mode, first, count, 0);
    ctx->n_draws++;
    ctx->sum_draw += (double)(ULONG)(ogles2_clock_us() - t0);
}

/* glDrawElements, and with a base vertex added to every index
 * (OES_draw_elements_base_vertex): the copied range moves by basevertex,
 * the rebased indices stay as they are. With `range` (glDrawRangeElements)
 * the program has said which indices it uses, range[0] to range[1]: they
 * are not scanned. */
static void draw_elements(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLenum type,
                          const void *indices, GLint basevertex, const ULONG *range, const char *fn)
{
    struct OGLES2Program *p;
    struct OGLES2Elements el;
    ULONG lo = ~0UL, hi = 0, k, t0 = ogles2_clock_us();

    if (!ogles2_context_valid(ctx))
        return;
    mark(ctx, -1);
    if (mode > GL_TRIANGLE_FAN ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx))
        return;
    p = ctx->program;
    if (!p || !ctx->v3d || count == 0)
        return;
    {
        /* With an element buffer bound, `indices` is the offset into it. */
        BOOL bad;
        const void *in = ogles2_buffer_indices(ctx, (ULONG)indices,
                                               (ULONG)count * index_size(type), &bad);
        if (bad) {
            ogles2_log("%s: the indices lie outside the element buffer; nothing drawn", (ULONG)fn);
            return;
        }
        if (in)
            indices = in;
        else if (!indices)
            return;
    }
scan:
    /* Mesa's state tracker: copy only the range the indices use. Without
     * branches (Emu68 runs a loop with two of them a pass at a third of the
     * speed): d's sign bit, spread by the arithmetic shift, picks it. Indices
     * past 2^31 would not fit the vertex arrays anyway. */
    if (range) {
        /* About 25 ns an index saved (Quake III through gl4es: 3.5 us a
         * draw). An index outside the range is undefined (GLES 3.0 2.9.5):
         * the GPU reads other vertices. */
        if (range[1] > 0x7FFFFFFFUL) {
            ogles2_log("%s: an end of 2^31 or more; nothing drawn", (ULONG)fn);
            return;
        }
        lo = range[0];
        hi = range[1];
    } else if (type == GL_UNSIGNED_INT) {
        const ULONG *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        ULONG any = 0;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = (LONG)(in[k] & 0x7FFFFFFFUL);
            any |= in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        if (any & 0x80000000UL) {
            ogles2_log("%s: an index of 2^31 or more; nothing drawn", (ULONG)fn);
            return;
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    } else if (type == GL_UNSIGNED_SHORT) {
        const UWORD *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    } else {
        const UBYTE *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    }
    if ((LONG)lo + basevertex < 0) {
        ogles2_log("%s: index %ld + base vertex %ld is negative; nothing drawn", (ULONG)fn, (LONG)lo,
                   (LONG)basevertex);
        return;
    }
    lo += (ULONG)basevertex;
    hi += (ULONG)basevertex;
    if (!arrays_ok(ctx, p, hi + 1, range ? 0 : fn)) { /* 0: quiet */
        if (!range)
            return;
        /* A range wider than the arrays (it need only hold the indices,
         * GLES 3.0 2.9.3; dEQP-GLES31's draw_range_elements_base_vertex in
         * ogles3): the indices' own. */
        range = 0;
        goto scan;
    }
    el.indices = indices;
    el.type = type;
    el.count = count;
    el.min = lo - (ULONG)basevertex; /* the indices' own smallest */
    draw_arrays(ctx, p, mode, (GLint)lo, (GLsizei)(hi - lo + 1), &el);
    ctx->n_draws++;
    ctx->sum_draw += (double)(ULONG)(ogles2_clock_us() - t0);
}

void ogles2_glDrawElements(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLenum type,
                           const void *indices)
{
    draw_elements(ctx, mode, count, type, indices, 0, 0, "glDrawElements");
}

void ogles2_glDrawElementsBaseVertexOES(struct OGLES2Context *ctx, GLenum mode, GLsizei count,
                                        GLenum type, const void *indices, GLint basevertex)
{
    draw_elements(ctx, mode, count, type, indices, basevertex, 0, "glDrawElementsBaseVertexOES");
}

/* GLES 3.0's glDrawRangeElements, offered to GLES2 programs through
 * aglGetProcAddress (ports/gl4es passes a locked array range): the indices
 * lie in [start, end], so ogles2 does not scan them for their range. */
void ogles2_glDrawRangeElements(struct OGLES2Context *ctx, GLenum mode, GLuint start, GLuint end,
                                GLsizei count, GLenum type, const void *indices)
{
    ULONG range[2];
    if (!ogles2_context_valid(ctx))
        return;
    if (end < start) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    range[0] = start;
    range[1] = end;
    draw_elements(ctx, mode, count, type, indices, 0, range, "glDrawRangeElements");
}

#include "clear_mrt_v3dp.h"

/* The program a clear draws into several render targets with: the clear
 * colour (the last generic attribute, constant) in every output, position
 * from the last but one; tools/clear_mrt. 0: none (logged once), and the
 * built-in one clears the first target only. */
static struct OGLES2Program *mrt_program(struct OGLES2Draw *d)
{
    struct OGLES2Program *p = &d->mrt_quad;
    UWORD a;
    if (!d->mrt_ready && !d->mrt_failed) {
        char why[96];
        if (!(d->mrt_image = ogles2_v3dp_load(clear_mrt_v3dp, sizeof(clear_mrt_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->mrt_image)) {
            d->mrt_failed = TRUE;
            ogles2_log("glClear: no program for several render targets (%s): the first is cleared only",
                       (ULONG)(d->mrt_image ? "out of memory" : why));
            return 0;
        }
        d->mrt_ready = TRUE;
    }
    if (!d->mrt_ready)
        return 0;
    for (a = 0; a < d->mrt_image->nattribs; a++)
        p->attrib_index[a] = d->mrt_image->attribs[a].name[2] == 'p' /* a_position */
                                 ? OGLES2_MAX_VERTEX_ATTRIBS - 2 : OGLES2_MAX_VERTEX_ATTRIBS - 1;
    return p;
}

#include "copy_tex_v3dp.h"

/* The program glCopyTex* draws with on the GPU: unit 0's 2D texture over a
 * quad, position from the last but one generic attribute, texture
 * coordinates from the last; tools/copy_tex. 0: none (logged once), and the
 * copies go through the CPU. */
static struct OGLES2Program *copy_program(struct OGLES2Draw *d)
{
    struct OGLES2Program *p = &d->copy_prog;
    UWORD a;
    if (!d->copy_ready && !d->copy_failed) {
        char why[96];
        if (!(d->copy_image = ogles2_v3dp_load(copy_tex_v3dp, sizeof(copy_tex_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->copy_image)) {
            d->copy_failed = TRUE;
            ogles2_log("glCopyTexSubImage2D: no program for copies on the GPU (%s): the CPU copies",
                       (ULONG)(d->copy_image ? "out of memory" : why));
            return 0;
        }
        d->copy_ready = TRUE;
    }
    if (!d->copy_ready)
        return 0;
    for (a = 0; a < d->copy_image->nattribs; a++)
        p->attrib_index[a] = d->copy_image->attribs[a].name[2] == 'p' /* a_position */
                                 ? OGLES2_MAX_VERTEX_ATTRIBS - 2 : OGLES2_MAX_VERTEX_ATTRIBS - 1;
    return p;
}

/* glCopyTexImage2D and glCopyTexSubImage2D on the GPU (copy.c): texture
 * src, bound to unit 0 for the draw, over the window rectangle (x, y, w, h)
 * of the bound framebuffer (the copy's target), texture coordinates tc[0]
 * to tc[2] left to right and tc[1] to tc[3] bottom to top. None of the
 * application's state applies: no test, blending, dithering or mask, as a
 * copy writes. The application's state is restored afterwards. FALSE:
 * nothing drawn (no program). */
BOOL ogles2_draw_copy(struct OGLES2Context *ctx, struct OGLES2Texture *src, const GLfloat *tc, GLint x, GLint y,
                      GLsizei w, GLsizei h)
{
    struct OGLES2State saved = ctx->state, *s = &ctx->state;
    struct OGLES2Array saved_pos = ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2];
    struct OGLES2Array saved_tc = ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 1];
    struct OGLES2Program *saved_prog = ctx->program, *p;
    struct OGLES2Texture *saved_tex;
    struct OGLES2Draw *d;
    static const GLfloat pos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLfloat uv[8];
    GLint saved_vp[4];
    int k;

    if (!(d = draw_state(ctx)) || !(p = copy_program(d)))
        return FALSE;
    for (k = 0; k < 4; k++)
        saved_vp[k] = ctx->viewport[k];
    uv[0] = tc[0]; uv[1] = tc[1]; uv[2] = tc[2]; uv[3] = tc[1];
    uv[4] = tc[0]; uv[5] = tc[3]; uv[6] = tc[2]; uv[7] = tc[3];
    for (k = 0; k < 2; k++) {
        struct OGLES2Array *a = &ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2 + k];
        a->enabled = GL_TRUE;
        a->size = 2;
        a->type = GL_FLOAT;
        a->normalized = GL_FALSE;
        a->stride = 0;
        a->pointer = k ? (const void *)uv : (const void *)pos;
        a->buffer = 0;
    }
    s->caps = 0;
    s->polygon_mode = GL_FILL;
    s->color_mask[0] = s->color_mask[1] = s->color_mask[2] = s->color_mask[3] = GL_TRUE;
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->clip_origin = GL_LOWER_LEFT_EXT;
    s->clip_depth = GL_NEGATIVE_ONE_TO_ONE_EXT;
    ctx->viewport[0] = x;
    ctx->viewport[1] = y;
    ctx->viewport[2] = w;
    ctx->viewport[3] = h;
    saved_tex = ogles2_texture_unit0(ctx, src);

    ogles2_job_begin(ctx);
    ctx->state_ver++; /* the override above */
    mark(ctx, -1);
    draw_arrays(ctx, p, GL_TRIANGLE_STRIP, 0, 4, 0);

    ogles2_texture_unit0(ctx, saved_tex);
    ctx->state = saved;
    ctx->state_ver++;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 1] = saved_tc;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
    return TRUE;
}

/* ---- clears that the tile buffer cannot do ----
 * A clear confined by the scissor box or by write masks, or one after draws
 * in the same job (the tile buffer clears before any of them), is drawn as a
 * rectangle (plan 7.7; PiStorm3D's gl_ClearViaQuad, Mesa's blitter clear):
 * the built-in program, an identity matrix, the clear colour as a constant
 * attribute, z at the clear depth, and a state that writes exactly what
 * glClear writes. The application's state is restored afterwards. */
void ogles2_draw_clear(struct OGLES2Context *ctx, GLbitfield mask)
{
    static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    struct OGLES2State saved = ctx->state, *s = &ctx->state;
    struct OGLES2Array saved_pos = ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2];
    struct OGLES2Program *saved_prog = ctx->program, *quad;
    struct OGLES2Draw *d;
    GLint saved_vp[4];
    GLfloat pos[12], z;
    int k;

    if ((mask & GL_DEPTH_BUFFER_BIT) && (!ctx->fb_depth_bits || !s->depth_mask))
        mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    if ((mask & GL_STENCIL_BUFFER_BIT) && !ctx->fb_stencil_bits)
        mask &= ~(GLbitfield)GL_STENCIL_BUFFER_BIT;
    if (!mask)
        return;
    for (k = 0; k < 4; k++)
        saved_vp[k] = ctx->viewport[k];

    /* The program: the built-in image, an identity matrix, position from
     * the last-but-one generic attribute, colour from the last (constant). */
    if (!(d = draw_state(ctx)))
        return;
    if (!d->quad_ready) {
        if (!ogles2_program_builtin(&d->quad))
            return;
        d->quad_ready = TRUE;
        for (k = 0; k < 16; k++)
            ogles2_cbuf_set(&d->quad, 0, (ULONG)k, float_bits(identity[k]));
    }
    quad = &d->quad;
    quad->attrib_index[0] = OGLES2_MAX_VERTEX_ATTRIBS - 2;
    quad->attrib_index[1] = OGLES2_MAX_VERTEX_ATTRIBS - 1;
    /* EXT_draw_buffers: GLES2 clears every draw buffer. */
    if (ctx->fb_nrt > 1 && (mask & GL_COLOR_BUFFER_BIT)) {
        struct OGLES2Program *m = mrt_program(d);
        if (m)
            quad = m;
    }

    z = s->clear_depth * 2.0f - 1.0f; /* lands on clear_depth with the range [0, 1] */
    pos[0] = -1; pos[1] = -1;  pos[3] = 1; pos[4] = -1;
    pos[6] = -1; pos[7] = 1;   pos[9] = 1; pos[10] = 1;
    pos[2] = pos[5] = pos[8] = pos[11] = z;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].enabled = GL_TRUE;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].size = 3;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].type = GL_FLOAT;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].normalized = GL_FALSE;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].stride = 0;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].pointer = pos;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].buffer = 0;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 1].enabled = GL_FALSE;
    for (k = 0; k < 4; k++)
        s->attrib[OGLES2_MAX_VERTEX_ATTRIBS - 1][k] = s->clear_color[k];

    /* The state: only the scissor test stays the application's (the
     * rectangle is filled whatever glPolygonMode says). */
    s->caps &= OGLES2_CAP_SCISSOR_TEST;
    s->polygon_mode = GL_FILL;
    if (!(mask & GL_COLOR_BUFFER_BIT))
        s->color_mask[0] = s->color_mask[1] = s->color_mask[2] = s->color_mask[3] = GL_FALSE;
    if (mask & GL_DEPTH_BUFFER_BIT) {
        s->caps |= OGLES2_CAP_DEPTH_TEST;
        s->depth_func = GL_ALWAYS;
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
        s->caps |= OGLES2_CAP_STENCIL_TEST;
        for (k = 0; k < 2; k++) {
            s->stencil_func[k] = GL_ALWAYS;
            s->stencil_ref[k] = s->clear_stencil & 0xff;
            s->stencil_fail[k] = s->stencil_zfail[k] = s->stencil_zpass[k] = GL_REPLACE;
            s->stencil_write_mask[k] = saved.stencil_write_mask[0]; /* glClear uses the front mask */
        }
    }
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->clip_origin = GL_LOWER_LEFT_EXT;
    s->clip_depth = GL_NEGATIVE_ONE_TO_ONE_EXT;
    ctx->viewport[0] = ctx->viewport[1] = 0;
    ctx->viewport[2] = (GLint)ctx->fb_w;
    ctx->viewport[3] = (GLint)ctx->fb_h;

    ogles2_job_begin(ctx);
    ctx->state_ver++; /* the override above */
    mark(ctx, -1); /* probe P: the time before is not the draw's */
    draw_arrays(ctx, quad, GL_TRIANGLE_STRIP, 0, 4, 0);

    ctx->state = saved;
    ctx->state_ver++;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
}
