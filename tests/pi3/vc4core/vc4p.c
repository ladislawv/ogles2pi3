/*
 * The Linux tests' .vc4p loader and draw builder (vc4p.h).
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "vc4p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vc4's enum quniform_contents at the validated commit (FORMAT.md 1.1). */
enum {
    U_CONSTANT, U_UNIFORM, U_VIEWPORT_X_SCALE, U_VIEWPORT_Y_SCALE, U_VIEWPORT_Z_OFFSET, U_VIEWPORT_Z_SCALE,
    U_TEXTURE_CONFIG_P0, U_TEXTURE_CONFIG_P1, U_TEXTURE_CONFIG_P2, U_TEXTURE_FIRST_LEVEL, U_TEXTURE_MSAA_ADDR,
    U_UBO0_ADDR, U_UBO1_ADDR, U_TEXRECT_SCALE_X, U_TEXRECT_SCALE_Y, U_TEXTURE_BORDER_COLOR,
    U_BLEND_CONST_COLOR_X, U_BLEND_CONST_COLOR_Y, U_BLEND_CONST_COLOR_Z, U_BLEND_CONST_COLOR_W,
    U_BLEND_CONST_COLOR_RGBA, U_BLEND_CONST_COLOR_AAAA, U_STENCIL, U_SAMPLE_MASK, U_UNIFORMS_ADDRESS
};

struct rd
{
    const uint8_t *p, *end;
    int bad;
};

static uint32_t u8(struct rd *r)
{
    if (r->p + 1 > r->end) {
        r->bad = 1;
        return 0;
    }
    return *r->p++;
}

static uint32_t u32(struct rd *r)
{
    uint32_t v;
    if (r->p + 4 > r->end) {
        r->bad = 1;
        return 0;
    }
    v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    r->p += 4;
    return v;
}

static void skip(struct rd *r, uint32_t n)
{
    if (r->p + n > r->end)
        r->bad = 1;
    else
        r->p += n;
}

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void str(char *dst, const uint8_t *strings, uint32_t len, uint32_t off)
{
    snprintf(dst, 64, "%s", off < len ? (const char *)strings + off : "");
}

/* The fs_inputs list: a count, two bytes each, padded to 4. */
/* A list of fragment inputs (slot, swizzle pairs): its count. */
static uint32_t inputs(struct rd *r)
{
    uint32_t n = u32(r);
    skip(r, 2 * n);
    skip(r, (4 - (2 * n) % 4) % 4);
    return n;
}

static int variant(struct vc4p *p, struct rd *r)
{
    struct vc4p_variant *v;
    uint32_t k;
    if (p->nvariants >= 32)
        return -1;
    v = &p->v[p->nvariants];
    memset(v, 0, sizeof(*v));
    v->stage = u32(r);
    for (k = 0; k < 16; k++) {
        v->tex_format[k] = u32(r);
        skip(r, 12); /* swizzle, compare, wrap, first level: the tests set the matching textures */
    }
    if (v->stage == VC4P_STAGE_FS) {
        v->color_format = u32(r);
        v->depth_enabled = (uint8_t)u8(r);
        v->stencil_enabled = (uint8_t)u8(r);
        skip(r, 2); /* stencil_twoside, stencil_full_writemasks */
        v->is_points = (uint8_t)u8(r);
        v->is_lines = (uint8_t)u8(r);
        skip(r, 6); /* point_coord_upper_left .. logicop_func */
        skip(r, 8); /* point_sprite_mask, ubo_1_size */
        skip(r, 1); /* ucp_enables */
        v->blend_enable = (uint8_t)u8(r);
        skip(r, 6);
        v->colormask = (uint8_t)u8(r);
        v->draw_buffer = (uint8_t)u8(r);
        skip(r, 2);
    } else {
        for (k = 0; k < 8; k++)
            v->attr_formats[k] = u32(r);
        v->is_coord = (uint8_t)u8(r);
        v->per_vertex_point_size = (uint8_t)u8(r);
        skip(r, 2);
        v->nfs_inputs = inputs(r); /* the fragment inputs it writes */
    }
    v->fs_threaded = (uint8_t)u8(r);
    v->num_inputs = (uint8_t)u8(r);
    v->vattrs_live = (uint8_t)u8(r);
    v->disable_early_z = (uint8_t)u8(r);
    for (k = 0; k < 9; k++)
        v->vattr_offsets[k] = (uint8_t)u8(r);
    skip(r, 3);
    v->color_inputs = u32(r);
    v->num_texture_samples = u32(r);
    if (v->stage == VC4P_STAGE_FS)
        v->nfs_inputs = inputs(r); /* the inputs it reads */
    else
        inputs(r);
    v->nrecipe = u32(r);
    if (v->nrecipe > 256)
        return -1;
    for (k = 0; k < v->nrecipe; k++)
        v->rtype[k] = u32(r);
    for (k = 0; k < v->nrecipe; k++)
        v->rdata[k] = u32(r);
    v->code_bytes = u32(r);
    if (r->bad || r->p + v->code_bytes > r->end || !(v->code = malloc(v->code_bytes ? v->code_bytes : 1)))
        return -1;
    memcpy(v->code, r->p, v->code_bytes);
    r->p += v->code_bytes;
    p->nvariants++;
    return r->bad ? -1 : 0;
}

int vc4p_load(struct vc4p *p, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long len;
    struct rd r;
    const uint8_t *strings = 0;
    uint32_t nsec, k, strings_len = 0;

    memset(p, 0, sizeof(*p));
    if (!f) {
        printf("  %s: cannot open\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, f) != (size_t)len || len < 32 || memcmp(data, "V3DP", 4)) {
        printf("  %s: not a program binary\n", path);
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    r.p = data + 4;
    r.end = data + len;
    r.bad = 0;
    if ((u32(&r) & 0xffff) != 1 || u32(&r) != 21) {
        printf("  %s: not format 1.x for VC4 (device 21)\n", path);
        free(data);
        return -1;
    }
    skip(&r, 12);
    nsec = u32(&r);
    if (u32(&r) != crc32_of(data + 32, (uint32_t)len - 32)) {
        printf("  %s: CRC mismatch\n", path);
        free(data);
        return -1;
    }
    for (k = 0; k < nsec && !r.bad; k++) {
        uint32_t type = u32(&r), n = u32(&r), j;
        struct rd s;
        s.p = r.p;
        s.end = r.p + n;
        s.bad = s.end > r.end;
        skip(&r, (n + 3) & ~3u);
        if (type == 2) {
            strings = s.p;
            strings_len = n;
        } else if (type == 3) { /* STAGE_PARAMS: the constant buffer's template */
            uint32_t stage = u32(&s), nvalues = u32(&s), nparams = u32(&s), si = stage == 4;
            skip(&s, nparams * 24);
            if (nvalues > 1024)
                return -1;
            p->cbuf_words[si] = nvalues;
            for (j = 0; j < nvalues; j++)
                p->cbuf[si][j] = u32(&s);
        } else if (type == 4) { /* UNIFORMS */
            uint32_t nu = u32(&s);
            for (j = 0; j < nu && j < 64; j++) {
                struct vc4p_uniform *u = &p->u[j];
                uint32_t q;
                str(u->name, strings, strings_len, u32(&s));
                u->type = u32(&s);
                u->array = u32(&s);
                u->flags = u32(&s);
                u->nplaces = u32(&s);
                for (q = 0; q < u->nplaces && q < 2; q++) {
                    u->place[q].stage = u32(&s) == 4;
                    u->place[q].offset = u32(&s);
                    u->place[q].elem_stride = (uint8_t)(u8(&s) / 4);
                    u->place[q].vec_stride = (uint8_t)(u8(&s) / 4);
                    u->place[q].int_as_float = (uint8_t)u8(&s);
                    u8(&s);
                }
                u->sampler[0] = u32(&s);
                u->sampler[1] = u32(&s);
            }
            p->nuniforms = (int)(nu < 64 ? nu : 64);
        } else if (type == 5) { /* ATTRIBUTES */
            uint32_t na = u32(&s);
            for (j = 0; j < na && j < 16; j++) {
                str(p->a[j].name, strings, strings_len, u32(&s));
                p->a[j].type = u32(&s);
                u32(&s); /* location */
                p->a[j].slot = u32(&s);
            }
            p->nattribs = (int)(na < 16 ? na : 16);
        } else if (type == 7) {
            if (variant(p, &s) < 0) {
                printf("  %s: a VC4_VARIANT section the loader cannot read\n", path);
                free(data);
                return -1;
            }
        }
        if (s.bad)
            r.bad = 1;
    }
    free(data);
    if (r.bad) {
        printf("  %s: truncated\n", path);
        return -1;
    }
    return 0;
}

void vc4p_free(struct vc4p *p)
{
    int k;
    for (k = 0; k < p->nvariants; k++)
        free(p->v[k].code);
    p->nvariants = 0;
}

const struct vc4p_variant *vc4p_find(const struct vc4p *p, uint32_t stage, int points)
{
    int k;
    for (k = 0; k < p->nvariants; k++) {
        const struct vc4p_variant *v = &p->v[k];
        if (v->stage == stage && (stage == VC4P_STAGE_FS ? v->is_points : v->per_vertex_point_size) == (points != 0))
            return v;
    }
    return 0;
}

const struct vc4p_variant *vc4p_find_attr(const struct vc4p *p, uint32_t stage, uint32_t slot, uint32_t format)
{
    int k;
    for (k = 0; k < p->nvariants; k++) {
        const struct vc4p_variant *v = &p->v[k];
        if (v->stage == stage && !v->per_vertex_point_size && slot < 8 && v->attr_formats[slot] == format)
            return v;
    }
    return 0;
}

const struct vc4p_variant *vc4p_find_attr_for(const struct vc4p *p, uint32_t slot, uint32_t format,
                                              const struct vc4p_variant *fs)
{
    int k;
    for (k = 0; k < p->nvariants; k++) {
        const struct vc4p_variant *v = &p->v[k];
        if (v->stage == VC4P_STAGE_VS && !v->per_vertex_point_size && slot < 8 && v->attr_formats[slot] == format &&
            v->nfs_inputs == fs->nfs_inputs)
            return v;
    }
    return 0;
}

const struct vc4p_variant *vc4p_find_tex(const struct vc4p *p, uint32_t unit, uint32_t format)
{
    int k;
    for (k = 0; k < p->nvariants; k++) {
        const struct vc4p_variant *v = &p->v[k];
        if (v->stage == VC4P_STAGE_FS && !v->is_points && !v->is_lines && !v->depth_enabled && !v->blend_enable &&
            !v->stencil_enabled && v->color_format == VC4P_FORMAT_R8G8B8A8_UNORM && unit < 16 &&
            v->tex_format[unit] == format)
            return v;
    }
    return 0;
}

const struct vc4p_variant *vc4p_find_fs_rt(const struct vc4p *p, int prim, int depth, int blend, int stencil,
                                           uint32_t color_format)
{
    int k;
    for (k = 0; k < p->nvariants; k++) {
        const struct vc4p_variant *v = &p->v[k];
        if (v->stage == VC4P_STAGE_FS && v->is_points == (prim == 2) && v->is_lines == (prim == 1) &&
            v->depth_enabled == (depth != 0) && v->blend_enable == (blend != 0) &&
            v->stencil_enabled == (stencil != 0) && v->color_format == color_format)
            return v;
    }
    return 0;
}

const struct vc4p_variant *vc4p_find_fs(const struct vc4p *p, int prim, int depth, int blend, int stencil)
{
    return vc4p_find_fs_rt(p, prim, depth, blend, stencil, VC4P_FORMAT_R8G8B8A8_UNORM);
}

uint32_t vc4p_stencil_word(uint32_t gl_func, uint32_t ref, uint32_t valuemask, uint32_t fail, uint32_t zfail,
                           uint32_t zpass, uint32_t writemask)
{
    uint32_t wm = writemask == 0x1 ? 0 : writemask == 0x3 ? 1 : writemask == 0xf ? 2 : 3;
    return 3u << 30 /* front and back alike */ | wm << 28 | zfail << 25 | zpass << 22 | fail << 19 |
           (gl_func - 0x0200) << 16 /* GL_NEVER.. as PIPE_FUNC_ */ | (ref & 0xff) << 8 | (valuemask & 0xff);
}

int vc4p_set(struct vc4p *p, const char *name, const uint32_t *words, uint32_t n)
{
    int k;
    uint32_t q, i;
    for (k = 0; k < p->nuniforms; k++) {
        const struct vc4p_uniform *u = &p->u[k];
        if (strcmp(u->name, name))
            continue;
        /* the values in order, into each place (a vector's components one
         * word apart: the places' layout for plain vectors and scalars) */
        for (q = 0; q < u->nplaces; q++)
            for (i = 0; i < n && u->place[q].offset + i < 1024; i++)
                p->cbuf[u->place[q].stage][u->place[q].offset + i] = words[i];
        return 0;
    }
    return -1;
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

/* A stage's uniform stream from its recipe (Mesa's vc4_write_uniforms). */
static int stream(struct vc4_job *job, const struct vc4p_draw *d, const struct vc4p_variant *v, uint32_t *bus)
{
    int si = v->stage == VC4P_STAGE_FS;
    uint8_t *p = vc4_buf_alloc(&job->state, v->nrecipe * 4 + 4, 4, bus);
    uint32_t k, w;
    const uint8_t *bc = (const uint8_t *)&d->blend_color;
    if (!p)
        return -1;
    for (k = 0; k < v->nrecipe; k++) {
        uint32_t t = v->rtype[k], x = v->rdata[k];
        switch (t) {
        case U_CONSTANT: w = x; break;
        case U_UNIFORM: w = x < 1024 ? d->prog->cbuf[si][x] : 0; break;
        case U_VIEWPORT_X_SCALE: w = fbits(d->vp_w * 0.5f * 16.0f); break;
        case U_VIEWPORT_Y_SCALE: w = fbits(d->vp_h * 0.5f * 16.0f); break;
        case U_VIEWPORT_Z_OFFSET: w = fbits((d->depth_near + d->depth_far) * 0.5f); break;
        case U_VIEWPORT_Z_SCALE: w = fbits((d->depth_far - d->depth_near) * 0.5f); break;
        case U_TEXTURE_CONFIG_P0: w = d->tex[x & 15][0]; break;
        case U_TEXTURE_CONFIG_P1: w = d->tex[x & 15][1]; break;
        case U_TEXTURE_CONFIG_P2: w = d->tex[x & 15][2]; break;
        case U_BLEND_CONST_COLOR_X: case U_BLEND_CONST_COLOR_Y: case U_BLEND_CONST_COLOR_Z:
        case U_BLEND_CONST_COLOR_W:
            w = fbits(bc[t - U_BLEND_CONST_COLOR_X] / 255.0f); /* the colour's bytes R, G, B, A */
            break;
        case U_BLEND_CONST_COLOR_RGBA: w = d->blend_color; break;
        case U_BLEND_CONST_COLOR_AAAA: w = 0x01010101u * bc[3]; break;
        case U_STENCIL: w = x < 3 ? d->stencil[x] : 0; break;
        default:
            printf("  recipe type %u: not in the tests' builder yet\n", t);
            return -1;
        }
        vc4_put32(p + 4 * k, w);
    }
    vc4_put32(p + 4 * k, 0); /* the QPU reads one word ahead */
    return 0;
}

int vc4p_emit(struct vc4_job *job, const struct vc4p_draw *d, uint16_t width, uint16_t height)
{
    const struct vc4p_variant *vs = d->vs, *cs = d->cs, *fs = d->fs;
    uint32_t code[3], unif[3], attr[8], rec, k, n = d->nattrs ? d->nattrs : 1;
    const struct vc4p_variant *st[3];
    uint8_t *r;

    st[0] = fs;
    st[1] = vs;
    st[2] = cs;
    for (k = 0; k < 3; k++) {
        uint32_t i;
        uint8_t *c = vc4_buf_alloc(&job->state, st[k]->code_bytes, 8, &code[k]);
        if (!c)
            return -1;
        for (i = 0; i < st[k]->code_bytes; i += 4)
            *(volatile uint32_t *)(c + i) = (uint32_t)st[k]->code[i] | (uint32_t)st[k]->code[i + 1] << 8 |
                                            (uint32_t)st[k]->code[i + 2] << 16 |
                                            (uint32_t)st[k]->code[i + 3] << 24;
        if (stream(job, d, st[k], &unif[k]) < 0)
            return -1;
    }
    for (k = 0; k < d->nattrs; k++) {
        uint32_t i;
        uint8_t *a = vc4_buf_alloc(&job->state, (d->attr[k].size + 3) & ~3u, 16, &attr[k]);
        const uint8_t *s = d->attr[k].data;
        if (!a)
            return -1;
        for (i = 0; i < d->attr[k].size; i += 4)
            vc4_put32(a + i, (uint32_t)s[i] | (uint32_t)s[i + 1] << 8 | (uint32_t)s[i + 2] << 16 |
                                 (uint32_t)s[i + 3] << 24);
    }
    /* The shader record, Mesa's vc4_emit_gl_shader_state. */
    r = vc4_buf_alloc(&job->state, 36 + 8 * n, 16, &rec);
    if (!r)
        return -1;
    vc4_put16(r, (fs->fs_threaded ? 0 : 1u) | (vs->per_vertex_point_size ? 2u : 0) | 4u /* clipping */);
    r[2] = 0;
    r[3] = fs->num_inputs;
    vc4_put32(r + 4, code[0]);
    vc4_put32(r + 8, unif[0]);
    vc4_put16(r + 12, 0);
    r[14] = vs->vattrs_live;
    r[15] = vs->vattr_offsets[8];
    vc4_put32(r + 16, code[1]);
    vc4_put32(r + 20, unif[1]);
    vc4_put16(r + 24, 0);
    r[26] = cs->vattrs_live;
    r[27] = cs->vattr_offsets[8];
    vc4_put32(r + 28, code[2]);
    vc4_put32(r + 32, unif[2]);
    for (k = 0; k < n; k++) {
        uint8_t *a = r + 36 + 8 * k;
        vc4_put32(a, d->nattrs ? attr[k] : unif[1]); /* no attributes: one record the shaders do not read */
        a[4] = (uint8_t)((d->nattrs ? d->attr[k].bytes : 4) - 1);
        a[5] = (uint8_t)(d->nattrs ? d->attr[k].stride : 0);
        a[6] = vs->vattr_offsets[k];
        a[7] = cs->vattr_offsets[k];
    }

    VC4_EMIT(&job->bcl, CLIP_WINDOW, .clip_window_left_pixel_coordinate = d->clip[2] ? d->clip[0] : 0,
             .clip_window_bottom_pixel_coordinate = d->clip[2] ? d->clip[1] : 0,
             .clip_window_width_in_pixels = d->clip[2] ? d->clip[2] : width,
             .clip_window_height_in_pixels = d->clip[2] ? d->clip[3] : height);
    {
        uint8_t *p = vc4_cl_room(&job->bcl, VC4_CONFIGURATION_BITS_length);
        if (p)
            VC4_CONFIGURATION_BITS_pack(p, &d->cfg);
    }
    VC4_EMIT(&job->bcl, DEPTH_OFFSET, .depth_offset_units = 0, .depth_offset_factor = 0);
    VC4_EMIT(&job->bcl, POINT_SIZE, .point_size = 1.0f);
    VC4_EMIT(&job->bcl, LINE_WIDTH, .line_width = 1.0f);
    VC4_EMIT(&job->bcl, CLIPPER_XY_SCALING, .viewport_half_width_in_1_16th_of_pixel = d->vp_w * 0.5f * 16.0f,
             .viewport_half_height_in_1_16th_of_pixel = d->vp_h * 0.5f * 16.0f);
    VC4_EMIT(&job->bcl, CLIPPER_Z_SCALE_AND_OFFSET,
             .viewport_z_scale_zc_to_zs = (d->depth_far - d->depth_near) * 0.5f,
             .viewport_z_offset_zc_to_zs = (d->depth_near + d->depth_far) * 0.5f);
    VC4_EMIT(&job->bcl, VIEWPORT_OFFSET, .viewport_centre_x_coordinate = d->vp_x + d->vp_w * 0.5f,
             .viewport_centre_y_coordinate = d->vp_y + d->vp_h * 0.5f);
    VC4_EMIT(&job->bcl, FLAT_SHADE_FLAGS, .flat_shading_flags = 0);
    vc4_gl_shader_state(&job->bcl, rec, (int)n);
    if (d->indices8) {
        uint32_t ib, i, max = 0;
        uint8_t *x = vc4_buf_alloc(&job->state, (d->count + 3) & ~3u, 16, &ib);
        if (!x)
            return -1;
        for (i = 0; i < d->count; i++) {
            x[i] = d->indices8[i];
            if (d->indices8[i] > max)
                max = d->indices8[i];
        }
        VC4_EMIT(&job->bcl, INDEXED_PRIMITIVE_LIST, .primitive_mode = d->mode, .index_type = 0 /* 8-bit */,
                 .length = d->count, .address_of_indices_list = ib, .maximum_index = max);
    } else if (d->indices) {
        uint32_t ib, i, max = 0;
        uint8_t *x = vc4_buf_alloc(&job->state, (d->count * 2 + 3) & ~3u, 16, &ib);
        if (!x)
            return -1;
        for (i = 0; i < d->count; i += 2) {
            uint32_t a = d->indices[i], b = i + 1 < d->count ? d->indices[i + 1] : 0;
            vc4_put32(x + 2 * i, a | b << 16);
            if (a > max)
                max = a;
            if (i + 1 < d->count && b > max)
                max = b;
        }
        VC4_EMIT(&job->bcl, INDEXED_PRIMITIVE_LIST, .primitive_mode = d->mode, .index_type = 1 /* 16-bit */,
                 .length = d->count, .address_of_indices_list = ib, .maximum_index = max);
    } else {
        VC4_EMIT(&job->bcl, VERTEX_ARRAY_PRIMITIVES, .primitive_mode = d->mode, .length = d->count,
                 .index_of_first_vertex = 0);
    }
    return job->bcl.failed || job->state.failed ? -1 : 0;
}
