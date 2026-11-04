/*
 * Textures v1 (plan section 7.6): 2D and cube map texture objects, glTexImage2D and
 * glTexSubImage2D, parameters, glGenerateMipmap, glPixelStorei, and the
 * texture and sampler state the draw path hands to the TMU_CONFIG recipes.
 *
 * Each level keeps an RGBA8 copy in RAM, converted with GL's rules
 * (GL_LUMINANCE -> (L,L,L,1), GL_ALPHA -> (0,0,0,A), the 16-bit formats
 * expanded), for glGenerateMipmap and for re-uploads when the mip chain
 * grows. On the GPU the texture is stored in the hardware format level 0's
 * format and type map to (plan 7.6, Mesa's v3dx_format_table.c: RGB565,
 * RGBA4, RGB5_A1, R8 and RG8 with swizzles, else RGBA8), packed from those
 * copies at upload, with PiStorm3D's layout code (v3dcore's v3d_texture.c:
 * Mesa's level layout, UIF/UBLINEAR/LINEARTILE tiling).
 *
 * A texture changed after a draw of the open job used it is renamed (plan
 * 7.7 hazard 3): the job keeps the old GPU copy, the texture gets a new one.
 * Cube maps: six faces, each with its own levels; on the GPU six copies of
 * the 2D layout one after another (Mesa's cube_map_stride), sampled with
 * an image depth of 6 and that stride.
 *
 * Render to texture (plan 7.7, fbo.c): a level a framebuffer object's job
 * drew into is newer on the GPU than its RAM copy (OGLES2Face.rendered);
 * the copy is read back (sync) before anything works from it.
 *
 * Since P2 step 5 (docs/PI3_PLAN.md §5.1) the GPU copy is the backend's
 * (backend.h; V3D 4.2: ogles2/v3d/texture_v3d.c): its format, packing,
 * layout, loads, stores and the draw's records. Here: the objects, their
 * RAM copies, completeness, the fallback texture.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "texture_private.h"
#include "etc1.h"


#define MAX_SIZE   ((int)ogles2_backend->caps->max_texture_size) /* the backend's (backend.h) */



/* ---- the context's texture state ---- */


static void sync(struct OGLES2Texture *t);
static void rect_defaults(struct OGLES2Texture *t);

static void init_object(struct OGLES2Texture *t, GLuint name, GLenum target)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->target = target;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    t->max_aniso = 1.0f;
    t->min_lod = -1000.0f;
    t->max_lod = 1000.0f;
    t->max_level = 1000;
    rect_defaults(t);
}

/* ARB_texture_rectangle's defaults: a linear minification filter (no
 * mipmaps) and clamped wraps. Also when a new name is first bound there. */
static void rect_defaults(struct OGLES2Texture *t)
{
    if (t->target != GL_TEXTURE_RECTANGLE)
        return;
    t->min_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_CLAMP_TO_EDGE;
}

BOOL ogles2_tex_cpu_uploads; /* probe U of the context last seen here (a test switch): the backend's TFU uploads */

static struct OGLES2TexState *ts_get(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    int i;
    ogles2_tex_cpu_uploads = (ctx->probe & OGLES2_PROBE_CPU_UPLOAD) != 0;
    if (ts)
        return ts;
    if (!(ts = AllocVec(sizeof(*ts), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    ts->sh = ctx->share;
    init_object(&ts->default2d, 0, GL_TEXTURE_2D);
    init_object(&ts->defaultcube, 0, GL_TEXTURE_CUBE_MAP);
    init_object(&ts->defaultrect, 0, GL_TEXTURE_RECTANGLE);
    for (i = 0; i < OGLES2_MAX_TEXTURE_UNITS; i++) {
        ts->unit2d[i] = &ts->default2d;
        ts->unitcube[i] = &ts->defaultcube;
        ts->unitrect[i] = &ts->defaultrect;
    }
    ctx->tex = ts;
    return ts;
}

struct OGLES2TexState *ogles2_texture_ts(struct OGLES2Context *ctx)
{
    return ts_get(ctx);
}


static void free_levels(struct OGLES2Texture *t)
{
    int k, l;
    for (k = 0; k < 6; k++) {
        for (l = 0; l < MAX_LEVELS; l++) {
            if (t->f[k].shadow[l])
                FreeVec(t->f[k].shadow[l]);
            t->f[k].shadow[l] = 0;
        }
        t->f[k].defined = 0;
        t->f[k].blank = 0;
    }
    ogles2_backend->tex_free(t);
}

/* aglDestroyContext (its jobs finished): the context's own textures; the
 * named ones with the share group's last member. */
void ogles2_textures_free(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    struct OGLES2Share *sh = ctx->share;
    GLuint i;
    if (sh && sh->members == 1) {
        for (i = 1; i < sh->ntex; i++)
            if (sh->tex[i]) {
                free_levels(sh->tex[i]);
                FreeVec(sh->tex[i]);
            }
        if (sh->tex)
            FreeVec(sh->tex);
        sh->tex = 0;
        sh->ntex = 0;
        ogles2_names_free(&sh->far_tex);
    }
    if (!ts)
        return;
    free_levels(&ts->default2d);
    free_levels(&ts->defaultcube);
    free_levels(&ts->defaultrect);
    free_levels(&ts->fallback);
    ogles2_texture_hidden_free(ctx, ts->scratch);
    FreeVec(ts);
    ctx->tex = 0;
}

/* A texture is about to change while a draw of the open job uses it (plan
 * 7.7 hazard 3): that draw must still read the old contents. Renaming, as
 * Mesa does: the GPU copy goes to the job (freed when the frame has
 * rendered, v3d_frame_defer_free), and the texture gets a new one, rebuilt
 * from the RAM copies at its next upload or draw. Only when the job's
 * retire list is nearly full is the job rendered first instead, which
 * costs a wait and the frame's depth and stencil (plan 7.2). */
static void before_change(struct OGLES2Context *ctx, struct OGLES2TexState *ts,
                          struct OGLES2Texture *t)
{
    struct OGLES2Context *m, *user;
    /* Its records are made again at the next draw: a new level can change
     * the level count without a new GPU copy. */
    t->st_own = FALSE;
    t->st_gen = 0;
    /* An open job draws into it (a framebuffer object's colour image), any
     * member's of the share group: rendered first. Then the RAM copies are
     * made current. */
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fbo_texture_changing(m, t);
    sync(t);
    /* A job not rendered yet uses it: an open one, or one submitted and
     * still running (M6 overlap), of any member: the old copy goes with
     * that job (share.c: serials are unique across contexts). */
    if (!t->used_serial || !(user = ogles2_job_user(ctx, t->used_serial)) || !user->v3d)
        return;
    if (ogles2_backend->tex_rename(user, t)) {
        t->used_serial = 0;
        ctx->n_renames++;
        if (!ts->warned_flush) {
            ts->warned_flush = TRUE;
            ogles2_log("texture %ld changed after a draw used it in the same frame: renamed "
                       "(logged once)", (ULONG)t->name);
        }
        return;
    }
    ogles2_log("texture %ld changed after a draw used it: the frame is rendered first "
               "(too many renamed blocks in this frame)", (ULONG)t->name);
    ogles2_job_flush(user, FALSE);
    ogles2_job_finish(user);
}

/* ---- names and binding ---- */

/* Makes `name` a valid index of the name table. */
static BOOL table_room(struct OGLES2TexState *ts, GLuint name)
{
    GLuint nn, j;
    struct OGLES2Texture **tab;
    if (name < ts->sh->ntex)
        return TRUE;
    nn = ts->sh->ntex ? ts->sh->ntex : 32;
    while (nn <= name)
        nn *= 2;
    if (!(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < ts->sh->ntex; j++)
        tab[j] = ts->sh->tex[j];
    if (ts->sh->tex)
        FreeVec(ts->sh->tex);
    ts->sh->tex = tab;
    ts->sh->ntex = nn;
    return TRUE;
}

/* A new texture object named `name` (no object has it). */
static struct OGLES2Texture *create(struct OGLES2TexState *ts, GLuint name)
{
    struct OGLES2Texture *t;
    GLuint slot = ogles2_name_slot(&ts->sh->far_tex, name, TRUE);
    if (slot == OGLES2_NO_SLOT || !table_room(ts, slot) ||
        !(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_name_forget(&ts->sh->far_tex, name);
        return 0;
    }
    init_object(t, name, 0);
    ts->sh->tex[slot] = t;
    return t;
}

void ogles2_glGenTextures(struct OGLES2Context *ctx, GLsizei n, GLuint *textures)
{
    struct OGLES2TexState *ts;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(ts = ts_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        while (i < ts->sh->ntex && ts->sh->tex[i])
            i++;
        if (i >= OGLES2_NEAR_NAMES || !create(ts, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (textures)
            textures[k] = i;
    }
}

static struct OGLES2Texture *by_name(struct OGLES2TexState *ts, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&ts->sh->far_tex, name, FALSE) : OGLES2_NO_SLOT;
    return slot < ts->sh->ntex ? ts->sh->tex[slot] : 0;
}

/* A deleted texture's memory goes (its name already has). */
static void destroy(struct OGLES2Context *ctx, struct OGLES2TexState *ts, struct OGLES2Texture *t)
{
    int u;
    for (u = 0; u < 6; u++)
        t->f[u].rendered = 0; /* going: nothing to read back (SDL's render targets: 38 ms) */
    before_change(ctx, ts, t); /* a queued draw must not read it; a job drawing into it rendered */
    free_levels(t);
    FreeVec(t);
}

void ogles2_glDeleteTextures(struct OGLES2Context *ctx, GLsizei n, const GLuint *textures)
{
    struct OGLES2TexState *ts;
    struct OGLES2Context *m;
    GLsizei k;
    int u;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(ts = ts_get(ctx)) || !textures)
        return;
    ogles2_trace(ctx, "glDeleteTextures", n, n ? textures[0] : 0);
    for (k = 0; k < n; k++) {
        struct OGLES2Texture *t = by_name(ts, textures[k]);
        if (!t)
            continue;
        /* Detached from this context's bound framebuffer (its job drawing
         * into it rendered first); other framebuffers keep it (GLES 2.0
         * 4.4.3). Every member of the share group unbinds it. */
        ogles2_fbo_texture_deleted(ctx, t);
        for (m = ctx->share->first; m; m = m->share_next) {
            struct OGLES2TexState *mts = m->tex;
            if (!mts)
                continue;
            for (u = 0; u < OGLES2_MAX_TEXTURE_UNITS; u++) { /* deleting unbinds (GLES 2.0 3.7.13) */
                if (mts->unit2d[u] == t)
                    mts->unit2d[u] = &mts->default2d;
                if (mts->unitcube[u] == t)
                    mts->unitcube[u] = &mts->defaultcube;
                if (mts->unitrect[u] == t)
                    mts->unitrect[u] = &mts->defaultrect;
            }
        }
        ts->sh->tex[ogles2_name_slot(&ts->sh->far_tex, t->name, FALSE)] = 0;
        ogles2_name_forget(&ts->sh->far_tex, t->name);
        if (t->fbo_refs)
            t->orphan = TRUE; /* still attached: lives on, nameless */
        else
            destroy(ctx, ts, t);
    }
}

/* fbo.c: an attachment point refers to t, or no longer does. An orphan goes
 * with its last attachment; at aglDestroyContext (`now`: the jobs are
 * finished) without waiting for jobs. */
void ogles2_texture_attach(struct OGLES2Texture *t)
{
    t->fbo_refs++;
}

void ogles2_texture_detach(struct OGLES2Context *ctx, struct OGLES2Texture *t, BOOL now)
{
    struct OGLES2TexState *ts;
    if (t->fbo_refs)
        t->fbo_refs--;
    if (t->fbo_refs || !t->orphan)
        return;
    if (now || !(ts = ts_get(ctx))) {
        free_levels(t);
        FreeVec(t);
        return;
    }
    destroy(ctx, ts, t);
}

GLboolean ogles2_glIsTexture(struct OGLES2Context *ctx, GLuint texture)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return GL_FALSE;
    t = by_name(ts, texture);
    return t && t->target ? GL_TRUE : GL_FALSE;
}

void ogles2_glActiveTexture(struct OGLES2Context *ctx, GLenum texture)
{
    struct OGLES2TexState *ts;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + OGLES2_MAX_TEXTURE_UNITS) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ts->active = (UBYTE)(texture - GL_TEXTURE0);
}

void ogles2_glBindTexture(struct OGLES2Context *ctx, GLenum target, GLuint texture)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP && target != GL_TEXTURE_RECTANGLE) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!texture) {
        t = target == GL_TEXTURE_2D ? &ts->default2d
          : target == GL_TEXTURE_CUBE_MAP ? &ts->defaultcube : &ts->defaultrect;
    } else {
        t = by_name(ts, texture);
        /* GLES2 lets a program bind a name glGenTextures did not return. */
        if (!t && !(t = create(ts, texture))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (t->target && t->target != target) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (!t->target) {
            t->target = target;
            rect_defaults(t);
        }
    }
    if (target == GL_TEXTURE_2D)
        ts->unit2d[ts->active] = t;
    else if (target == GL_TEXTURE_CUBE_MAP)
        ts->unitcube[ts->active] = t;
    else
        ts->unitrect[ts->active] = t;
}

/* ---- parameters ---- */

static struct OGLES2Texture *bound(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_CUBE_MAP)
        return ts->unitcube[ts->active];
    if (target == GL_TEXTURE_RECTANGLE)
        return ts->unitrect[ts->active];
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

/* The largest anisotropy (the sampler's 2, 4, 8 or 16) and LOD bias (its
 * s8.8 field holds more; Mesa's v3d screen offers 15). */
#define MAX_ANISOTROPY (ogles2_backend->caps->max_anisotropy)
#define MAX_LOD_BIAS   (ogles2_backend->caps->max_lod_bias)

/* A parameter given as `i` (glTexParameteri*) or `f` (glTexParameterf*):
 * enums and levels read i, the float parameters f (GL converts either way). */
static void tex_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint i, GLfloat f)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum e = (GLenum)i;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return;
    /* ARB_texture_rectangle: no mipmapping, clamped coordinates only (of
     * desktop GL's three, the one GLES2 has), base level 0. */
    if (target == GL_TEXTURE_RECTANGLE) {
        if ((pname == GL_TEXTURE_MIN_FILTER && e != GL_NEAREST && e != GL_LINEAR) ||
            ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) && e != GL_CLAMP_TO_EDGE)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            return;
        }
        if (pname == GL_TEXTURE_BASE_LEVEL && i != 0) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST &&
            e != GL_LINEAR_MIPMAP_NEAREST && e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
            break;
        t->min_filter = e;
        t->pver++;
        return;
    case GL_TEXTURE_MAG_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR)
            break;
        t->mag_filter = e;
        t->pver++;
        return;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (e != GL_REPEAT && e != GL_CLAMP_TO_EDGE && e != GL_MIRRORED_REPEAT && e != GL_MIRROR_CLAMP_TO_EDGE)
            break;
        if (pname == GL_TEXTURE_WRAP_S)
            t->wrap_s = e;
        else
            t->wrap_t = e;
        t->pver++;
        return;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
        if (!(f >= 1.0f)) { /* EXT_texture_filter_anisotropic: below 1 (or NaN) */
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        t->max_aniso = f > MAX_ANISOTROPY ? MAX_ANISOTROPY : f;
        t->pver++;
        return;
    case GL_TEXTURE_LOD_BIAS:
        t->lod_bias = f; /* clamped where it is used (GL keeps the value) */
        t->pver++;
        return;
    case GL_TEXTURE_MIN_LOD:
        t->min_lod = f;
        t->pver++;
        return;
    case GL_TEXTURE_MAX_LOD:
        t->max_lod = f;
        t->pver++;
        return;
    case GL_TEXTURE_BASE_LEVEL:
    case GL_TEXTURE_MAX_LEVEL:
        if (i < 0) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        if (pname == GL_TEXTURE_BASE_LEVEL)
            t->base_level = i;
        else
            t->max_level = i;
        t->pver++;
        return;
    default:
        break;
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
}


void ogles2_glTexParameteri(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint param)
{
    tex_param(ctx, target, pname, param, (GLfloat)param);
}

void ogles2_glTexParameterf(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat param)
{
    tex_param(ctx, target, pname, round_param(param), param);
}

void ogles2_glTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLint *params)
{
    if (params)
        tex_param(ctx, target, pname, params[0], (GLfloat)params[0]);
}

void ogles2_glTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLfloat *params)
{
    if (params)
        tex_param(ctx, target, pname, round_param(params[0]), params[0]);
}

/* A parameter's value, as a float; FALSE (GL_INVALID_ENUM set) if pname is none. */
static BOOL get_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat *v)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return FALSE;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:          *v = (GLfloat)t->min_filter; return TRUE;
    case GL_TEXTURE_MAG_FILTER:          *v = (GLfloat)t->mag_filter; return TRUE;
    case GL_TEXTURE_WRAP_S:              *v = (GLfloat)t->wrap_s; return TRUE;
    case GL_TEXTURE_WRAP_T:              *v = (GLfloat)t->wrap_t; return TRUE;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:  *v = t->max_aniso; return TRUE;
    case GL_TEXTURE_LOD_BIAS:            *v = t->lod_bias; return TRUE;
    case GL_TEXTURE_MIN_LOD:             *v = t->min_lod; return TRUE;
    case GL_TEXTURE_MAX_LOD:             *v = t->max_lod; return TRUE;
    case GL_TEXTURE_BASE_LEVEL:          *v = (GLfloat)t->base_level; return TRUE;
    case GL_TEXTURE_MAX_LEVEL:           *v = (GLfloat)t->max_level; return TRUE;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        return FALSE;
    }
}

void ogles2_glGetTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLint *params)
{
    GLfloat v;
    if (params && get_param(ctx, target, pname, &v))
        *params = pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER ||
                  pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T ||
                  pname == GL_TEXTURE_BASE_LEVEL || pname == GL_TEXTURE_MAX_LEVEL
                      ? (GLint)(ULONG)v /* enums and levels: exact */
                      : round_param(v);
}

void ogles2_glGetTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLfloat *params)
{
    GLfloat v;
    if (params && get_param(ctx, target, pname, &v))
        *params = v;
}

void ogles2_glPixelStorei(struct OGLES2Context *ctx, GLenum pname, GLint param)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (param != 1 && param != 2 && param != 4 && param != 8) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname == GL_UNPACK_ALIGNMENT)
        ctx->unpack_alignment = (UBYTE)param;
    else if (pname == GL_PACK_ALIGNMENT)
        ctx->pack_alignment = (UBYTE)param;
    else
        ogles2_error(ctx, GL_INVALID_ENUM);
}

/* glGet* for the texture state (state.c asks). */
BOOL ogles2_texture_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    switch (pname) {
    case GL_ACTIVE_TEXTURE:       *v = (GLint)(GL_TEXTURE0 + (ts ? ts->active : 0)); return TRUE;
    case GL_TEXTURE_BINDING_2D:   *v = ts ? (GLint)ts->unit2d[ts->active]->name : 0; return TRUE;
    case GL_TEXTURE_BINDING_CUBE_MAP: *v = ts ? (GLint)ts->unitcube[ts->active]->name : 0; return TRUE;
    case GL_TEXTURE_BINDING_RECTANGLE: *v = ts ? (GLint)ts->unitrect[ts->active]->name : 0; return TRUE;
    case GL_MAX_RECTANGLE_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_UNPACK_ALIGNMENT:     *v = ctx->unpack_alignment; return TRUE;
    case GL_PACK_ALIGNMENT:       *v = ctx->pack_alignment; return TRUE;
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_MAX_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_STAGE_SAMPLERS; return TRUE;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_TEXTURE_UNITS; return TRUE;
    /* Vertex texture fetch: the draws bind the units of both stages' samplers
     * (draw.c texture_states); dEQP-GLES2's texture.vertex tests sample in
     * the vertex shader (M7). Units are shared between the stages. */
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_STAGE_SAMPLERS; return TRUE;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: *v = (GLint)MAX_ANISOTROPY; return TRUE;
    case GL_MAX_TEXTURE_LOD_BIAS: *v = (GLint)MAX_LOD_BIAS; return TRUE;
    default:                      return FALSE;
    }
}

/* ---- images ---- */


/* Set once an image whose texture the TMU returns 32 bits for (32-bit
 * floats, depth) was given: from then on draws look at their samplers'
 * textures to pick the program's 32-bit variants (draw.c). */
ULONG ogles2_wide_seen;

static ULONG wide_class(GLenum format, GLenum type)
{
    return type == GL_FLOAT || format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES;
}

/* Bytes per pixel of a valid format/type pair; 0 and the error to set if not. */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1 /* EXT_texture_format_BGRA8888 */
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

static int components(GLenum format)
{
    switch (format) {
    case GL_RGBA:            return 4;
    case GL_RGB:             return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_LUMINANCE:
    case GL_ALPHA:           return 1;
    default:                 return 0;
    }
}

static int pixel_size(GLenum format, GLenum type, GLenum *err)
{
    *err = GL_NO_ERROR;
    /* OES_depth_texture and OES_packed_depth_stencil's texture formats take
     * their types only, and their types no other format. */
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES ||
        type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_24_8_OES) {
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT) return 2;
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_INT) return 4;
        if (format == GL_DEPTH_STENCIL_OES && type == GL_UNSIGNED_INT_24_8_OES) return 4;
        *err = (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES || components(format) ||
                format == GL_BGRA_EXT) &&
               (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_24_8_OES ||
                type == GL_UNSIGNED_BYTE || type == GL_FLOAT || type == GL_HALF_FLOAT_OES ||
                type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                type == GL_UNSIGNED_SHORT_5_5_5_1)
                   ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    }
    switch (type) {
    case GL_FLOAT:          /* OES_texture_float */
    case GL_HALF_FLOAT_OES: /* OES_texture_half_float */
        if (components(format))
            return components(format) * (type == GL_FLOAT ? 4 : 2);
        *err = format == GL_BGRA_EXT ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE:
        switch (format) {
        case GL_RGBA:
        case GL_BGRA_EXT:        return 4;
        case GL_RGB:             return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE:
        case GL_ALPHA:           return 1;
        default:                 *err = GL_INVALID_ENUM; return 0;
        }
    case GL_UNSIGNED_SHORT_5_6_5:
        if (format == GL_RGB) return 2;
        *err = format == GL_RGBA || format == GL_LUMINANCE || format == GL_ALPHA ||
               format == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        if (format == GL_RGBA) return 2;
        *err = format == GL_RGB || format == GL_LUMINANCE || format == GL_ALPHA ||
               format == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    default:
        *err = GL_INVALID_ENUM;
        return 0;
    }
}

static UBYTE x5(UWORD v) { return (UBYTE)((v << 3) | (v >> 2)); }
static UBYTE x6(UWORD v) { return (UBYTE)((v << 2) | (v >> 4)); }
static UBYTE x4(UWORD v) { return (UBYTE)((v << 4) | v); }


UWORD ogles2_float_to_half(GLfloat f)
{
    ULONG u = f2u(f), s = (u >> 16) & 0x8000, m = u & 0x7fffff;
    LONG e = (LONG)((u >> 23) & 255) - 127 + 15;
    if (((u >> 23) & 255) == 255)
        return (UWORD)(s | 0x7c00 | (m ? 0x200 : 0));
    if (e >= 31)
        return (UWORD)(s | 0x7c00); /* too large: infinity */
    if (e <= 0) {                   /* a subnormal half, or zero */
        ULONG shift, h;
        if (e < -10)
            return (UWORD)s;
        m |= 0x800000;
        shift = (ULONG)(14 - e);
        h = (m >> shift) + ((m >> (shift - 1)) & 1);
        return (UWORD)(s | h);
    }
    return (UWORD)((s | ((ULONG)e << 10) | (m >> 13)) + ((m >> 12) & 1)); /* a carry rounds the exponent up */
}

/* Floats or half floats of the application's format into RGBA floats (the
 * conventions of the RGBA8 copy: LUMINANCE (L,L,L,1), ALPHA (0,0,0,A)). */
static void convert_float(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                          int w, int h, int align)
{
    int n = components(format), bpc = type == GL_FLOAT ? 4 : 2, x, y, c;
    ULONG src_stride = ((ULONG)(w * n * bpc) + align - 1) & ~(ULONG)(align - 1);
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        GLfloat *d = (GLfloat *)dst;
        for (x = 0; x < w; x++, d += 4) {
            GLfloat v[4];
            for (c = 0; c < n; c++, s += bpc) {
                if (bpc == 4) {
                    memcpy(&v[c], s, 4); /* the 68k's own byte order, as GL's types are */
                } else {
                    UWORD hv;
                    memcpy(&hv, s, 2);
                    v[c] = half_to_float(hv);
                }
            }
            switch (format) {
            case GL_RGBA:            d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3]; break;
            case GL_RGB:             d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = 1.0f; break;
            case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = v[0]; d[3] = v[1]; break;
            case GL_LUMINANCE:       d[0] = d[1] = d[2] = v[0]; d[3] = 1.0f; break;
            default: /* GL_ALPHA */  d[0] = d[1] = d[2] = 0.0f; d[3] = v[0]; break;
            }
        }
    }
}

/* Depth values into the stored depth word (CLASS_DEPTH16/24). */
static void convert_depth(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum type, int w, int h,
                          int align)
{
    int bpp = type == GL_UNSIGNED_SHORT ? 2 : 4, x, y;
    ULONG src_stride = ((ULONG)(w * bpp) + align - 1) & ~(ULONG)(align - 1);
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        ULONG *d = (ULONG *)dst;
        for (x = 0; x < w; x++, s += bpp) {
            if (bpp == 2) {
                UWORD v;
                memcpy(&v, s, 2);
                d[x] = v;
            } else {
                ULONG v;
                memcpy(&v, s, 4);
                /* 24 bits kept; UNSIGNED_INT_24_8's low byte is the stencil */
                d[x] = type == GL_UNSIGNED_INT ? v & 0xffffff00UL : v;
            }
        }
    }
}

/* The 16-bit packed types into an RGBA8 texel (R in the top byte: the copy's
 * byte order on the 68k) as two table lookups, one per byte, ORed: each
 * channel's expansion (x5, x6, x4) splits into bits from the high byte and
 * bits from the low byte that never overlap. [0] by the high byte, [1] by
 * the low. Made once, 6 KB. */
static ULONG t565[2][256], t4444[2][256], t5551[2][256];
static BOOL packed_tables;

static void make_packed_tables(void)
{
    int b;
    for (b = 0; b < 256; b++) {
        ULONG h = (ULONG)b, l = (ULONG)b;
        /* 5-6-5: R = h>>3; G = (h&7)<<3 | l>>5; B = l&31 */
        t565[0][b] = ((ULONG)x5((UWORD)(h >> 3)) << 24) | ((((h & 7) << 5) | ((h & 7) >> 1)) << 16) | 0xFF;
        t565[1][b] = (((l >> 5) << 2) << 16) | ((ULONG)x5((UWORD)(l & 31)) << 8);
        /* 4-4-4-4: R = h>>4; G = h&15; B = l>>4; A = l&15 */
        t4444[0][b] = ((ULONG)x4((UWORD)(h >> 4)) << 24) | ((ULONG)x4((UWORD)(h & 15)) << 16);
        t4444[1][b] = ((ULONG)x4((UWORD)(l >> 4)) << 8) | x4((UWORD)(l & 15));
        /* 5-5-5-1: R = h>>3; G = (h&7)<<2 | l>>6; B = (l>>1)&31; A = l&1 */
        t5551[0][b] = ((ULONG)x5((UWORD)(h >> 3)) << 24) | ((((h & 7) << 5) | (h & 7)) << 16);
        t5551[1][b] = (((l >> 6) << 3) << 16) | ((ULONG)x5((UWORD)((l >> 1) & 31)) << 8) | ((l & 1) ? 0xFF : 0);
    }
    packed_tables = TRUE;
}

/* One row of RGBA8-class texels from the application's (format, type), the
 * common cases each in a loop of its own (a switch per texel cost 50 ms a
 * megapixel on the A1200). FALSE for one convert's general loop does. */
static BOOL convert_row(ULONG *d, const UBYTE *s, GLenum format, GLenum type, int w)
{
    const ULONG (*tab)[256];
    int x;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:   tab = (const ULONG (*)[256])t565; break;
    case GL_UNSIGNED_SHORT_4_4_4_4: tab = (const ULONG (*)[256])t4444; break;
    case GL_UNSIGNED_SHORT_5_5_5_1: tab = (const ULONG (*)[256])t5551; break;
    case GL_UNSIGNED_BYTE:          tab = 0; break;
    default:                        return FALSE;
    }
    if (tab) {
        if (!packed_tables)
            make_packed_tables();
        for (x = 0; x < w; x++, s += 2) /* big-endian: the 68k's own order */
            d[x] = tab[0][s[0]] | tab[1][s[1]];
        return TRUE;
    }
    switch (format) {
    case GL_RGBA:
        memcpy(d, s, (ULONG)w * 4);
        return TRUE;
    case GL_BGRA_EXT:
        for (x = 0; x < w; x++, s += 4)
            d[x] = ((ULONG)s[2] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[0] << 8) | s[3];
        return TRUE;
    case GL_RGB:
        for (x = 0; x < w; x++, s += 3)
            d[x] = ((ULONG)s[0] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[2] << 8) | 0xFF;
        return TRUE;
    case GL_LUMINANCE_ALPHA:
        for (x = 0; x < w; x++, s += 2)
            d[x] = ((ULONG)s[0] * 0x01010100UL) | s[1];
        return TRUE;
    case GL_LUMINANCE:
        for (x = 0; x < w; x++)
            d[x] = ((ULONG)s[x] * 0x01010100UL) | 0xFF;
        return TRUE;
    case GL_ALPHA:
        for (x = 0; x < w; x++)
            d[x] = s[x];
        return TRUE;
    }
    return FALSE;
}

/* w x h pixels of the application's (format, type) at src, rows aligned to
 * the unpack alignment, into the RAM copy at dst (dst_stride bytes per row)
 * in the class of (format, type). */
static void convert(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                    int w, int h, int align)
{
    GLenum err;
    int bpp = pixel_size(format, type, &err), x, y;
    ULONG src_stride = ((ULONG)(w * bpp) + align - 1) & ~(ULONG)(align - 1);
    switch (class_of(format, type)) {
    case CLASS_FLOAT:
        convert_float(dst, dst_stride, src, format, type, w, h, align);
        return;
    case CLASS_DEPTH16:
    case CLASS_DEPTH24:
        convert_depth(dst, dst_stride, src, type, w, h, align);
        return;
    }
    if (!((ULONG)dst & 3) && !(dst_stride & 3) && convert_row((ULONG *)dst, src, format, type, 0)) {
        for (y = 0; y < h; y++, src += src_stride, dst += dst_stride)
            convert_row((ULONG *)dst, src, format, type, w);
        return;
    }
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        UBYTE *d = dst;
        for (x = 0; x < w; x++, d += 4) {
            UWORD v;
            switch (type) {
            case GL_UNSIGNED_SHORT_5_6_5:
                v = (UWORD)((s[0] << 8) | s[1]); /* big-endian: the 68k's own order */
                s += 2;
                d[0] = x5((UWORD)(v >> 11)); d[1] = x6((UWORD)((v >> 5) & 63)); d[2] = x5((UWORD)(v & 31)); d[3] = 255;
                continue;
            case GL_UNSIGNED_SHORT_4_4_4_4:
                v = (UWORD)((s[0] << 8) | s[1]);
                s += 2;
                d[0] = x4((UWORD)(v >> 12)); d[1] = x4((UWORD)((v >> 8) & 15));
                d[2] = x4((UWORD)((v >> 4) & 15)); d[3] = x4((UWORD)(v & 15));
                continue;
            case GL_UNSIGNED_SHORT_5_5_5_1:
                v = (UWORD)((s[0] << 8) | s[1]);
                s += 2;
                d[0] = x5((UWORD)(v >> 11)); d[1] = x5((UWORD)((v >> 6) & 31));
                d[2] = x5((UWORD)((v >> 1) & 31)); d[3] = (v & 1) ? 255 : 0;
                continue;
            }
            switch (format) {
            case GL_RGBA:            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; s += 4; break;
            case GL_BGRA_EXT:        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; s += 4; break;
            case GL_RGB:             d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; s += 3; break;
            case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; s += 2; break;
            case GL_LUMINANCE:       d[0] = d[1] = d[2] = s[0]; d[3] = 255; s += 1; break;
            default: /* GL_ALPHA */  d[0] = d[1] = d[2] = 0; d[3] = s[0]; s += 1; break;
            }
        }
    }
}



/* GLES 2.0 3.7.10 with SGIS_texture_lod's levels: level 0 (a cube map's
 * cube complete; the GPU copy is laid out from it, so it must exist even
 * when a later base level is sampled), and the levels sampled, base_level
 * up to max_level or the 1x1 level (a mipmapping filter) or base_level
 * alone, consistent on every face and in the GPU copy: *first and *last. */
BOOL ogles2_texture_complete(const struct OGLES2Texture *t, int *first, int *last)
{
    int n, l, k, b = t->base_level, e;
    if (!t->gpu || !base_ok(t))
        return FALSE;
    n = level_count(t->f[0].w[0], t->f[0].h[0]);
    if (b >= n)
        return FALSE;
    e = !mipmapped(t->min_filter) ? b : t->max_level < n - 1 ? t->max_level : n - 1;
    if (e < b || e >= ogles2_backend->tex_levels(t))
        return FALSE;
    for (k = 0; k < faces(t); k++)
        for (l = b; l <= e; l++)
            if (l && !consistent(&t->f[k], l))
                return FALSE;
    *first = b;
    *last = e;
    return TRUE;
}


/* The RAM copies of every level the GPU drew into, read back. */
static void sync(struct OGLES2Texture *t)
{
    int k, l;
    for (k = 0; k < 6; k++) {
        struct OGLES2Face *f = &t->f[k];
        if (!f->rendered)
            continue;
        for (l = 0; l < MAX_LEVELS; l++)
            if ((f->rendered & (1UL << l)) && f->shadow[l]) {
                ogles2_backend->tex_load(t, k, l, 0, 0, f->w[l], f->h[l], f->shadow[l],
                         (ULONG)f->w[l] * copy_bytes(level_class(f, l)));
                f->blank &= ~(1UL << l);
            }
        f->rendered = 0;
    }
}




/* A whole level from its RAM copy into the GPU copy. */
static void upload_level(struct OGLES2Texture *t, int k, int l)
{
    ogles2_backend->tex_store(t, k, l, t->f[k].shadow[l], 0, 0, t->f[k].w[l], t->f[k].h[l]);
}

/* (Re)makes the GPU copy for the levels given (the backend's), what the
 * GPU drew into read back first. */
static BOOL upload_all(struct OGLES2Texture *t)
{
    sync(t);
    return ogles2_backend->tex_upload(t);
}

BOOL ogles2_texture_upload(struct OGLES2Texture *t)
{
    return upload_all(t);
}

static BOOL face_target(GLenum target)
{
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

/* The bound texture an image call's target names, and the face (*k);
 * 0 with GL_INVALID_ENUM set for another target. A rectangle texture
 * (ARB_texture_rectangle) has level 0 only: GL_INVALID_VALUE otherwise. */
static struct OGLES2Texture *image_texture(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target,
                                           GLint level, int *k)
{
    *k = 0;
    if (face_target(target)) {
        *k = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        return ts->unitcube[ts->active];
    }
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_RECTANGLE) {
        if (level != 0) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return 0;
        }
        return ts->unitrect[ts->active];
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

/* ---- for framebuffer objects (fbo.c) ---- */

struct OGLES2Texture *ogles2_texture_get(struct OGLES2Context *ctx, GLuint name)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    return ts ? by_name(ts, name) : 0;
}

GLuint ogles2_texture_name(const struct OGLES2Texture *t)
{
    return t->name;
}

GLenum ogles2_texture_target(const struct OGLES2Texture *t)
{
    return t->target;
}

BOOL ogles2_texture_image(const struct OGLES2Texture *t, int k, int l, UWORD *w, UWORD *h,
                          GLenum *format, GLenum *type)
{
    const struct OGLES2Face *f = &t->f[k];
    if (k < 0 || k > 5 || l < 0 || l >= MAX_LEVELS || !(f->defined & (1UL << l)))
        return FALSE;
    *w = f->w[l];
    *h = f->h[l];
    *format = f->format[l];
    *type = f->type[l];
    return TRUE;
}

/* GLES2's colour-renderable texture images (plan 7.7): RGB and RGBA in the
 * formats the TLB stores; the GPU copy is in level 0's format, so the level
 * must have level 0's format and type. Half-float RGBA too
 * (EXT_color_buffer_half_float, which names RGBA only for textures, as
 * dEQP checks): stored as RGBA16F, which the TLB writes from a 16F render
 * target. */
BOOL ogles2_texture_renderable(const struct OGLES2Texture *t, int k, int l)
{
    const struct OGLES2Face *f = &t->f[k], *f0 = &t->f[0];
    GLenum fm, ty;
    if (!(f->defined & (1UL << l)) || !(f0->defined & 1))
        return FALSE;
    fm = f->format[l];
    ty = f->type[l];
    if (fm != f0->format[0] || ty != f0->type[0])
        return FALSE;
    /* A BGRA_EXT image is kept as RGBA8 (only its uploads swap), so it is
     * colour-renderable as RGBA is (EXT_texture_format_BGRA8888). */
    return (ty == GL_UNSIGNED_BYTE && (fm == GL_RGB || fm == GL_RGBA || fm == GL_BGRA_EXT)) ||
           (ty == GL_UNSIGNED_SHORT_5_6_5 && fm == GL_RGB) ||
           ((ty == GL_UNSIGNED_SHORT_4_4_4_4 || ty == GL_UNSIGNED_SHORT_5_5_5_1) && fm == GL_RGBA) ||
           (ty == GL_HALF_FLOAT_OES && fm == GL_RGBA);
}

BOOL ogles2_texture_half(const struct OGLES2Texture *t, int k, int l)
{
    return ogles2_texture_renderable(t, k, l) && t->f[k].type[l] == GL_HALF_FLOAT_OES;
}


/* OES_depth_texture: level l of a 2D texture is a depth image; its depth
 * bits (16 or 24) and whether it holds stencil too (DEPTH_STENCIL_OES). */
BOOL ogles2_texture_depth(const struct OGLES2Texture *t, int l, UBYTE *bits, BOOL *stencil)
{
    const struct OGLES2Face *f = &t->f[0];
    int c;
    if (t->target != GL_TEXTURE_2D || l < 0 || l >= MAX_LEVELS || !(f->defined & (1UL << l)))
        return FALSE;
    c = level_class(f, l);
    if (c != CLASS_DEPTH16 && c != CLASS_DEPTH24)
        return FALSE;
    *bits = c == CLASS_DEPTH16 ? 16 : 24;
    *stencil = f->format[l] == GL_DEPTH_STENCIL_OES;
    return TRUE;
}


/* draw.c: unit `unit`'s texture of that kind returns 32 bits (by its level
 * 0's format; an incomplete one samples the RGBA8 fallback, which the
 * 32-bit variants read correctly too). */
BOOL ogles2_texture_wide(struct OGLES2Context *ctx, int unit, int kind)
{
    struct OGLES2TexState *ts = ctx->tex;
    const struct OGLES2Texture *t;
    if (!ts)
        return FALSE;
    t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit] : ts->unit2d[unit];
    return (t->f[0].defined & 1) && wide_class(t->f[0].format[0], t->f[0].type[0]);
}

void ogles2_texture_rendered(struct OGLES2Texture *t, int k, int l)
{
    t->f[k].rendered |= 1UL << l;
    t->f[k].blank &= ~(1UL << l);
}

/* A half-float level read as RGBA bytes (glReadPixels' RGBA/UNSIGNED_BYTE):
 * each value clamped to [0,1] and rounded, as GLES 2.0 4.3.1 converts a
 * colour for an unsigned normalized type. */
static UBYTE float_byte(GLfloat v)
{
    return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (UBYTE)(v * 255.0f + 0.5f);
}

static BOOL read_half(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba,
                      ULONG stride)
{
    const struct OGLES2Face *f = &t->f[k];
    ULONG row = (ULONG)w * 16, i;
    GLfloat *box = AllocVec(row * h + 4, MEMF_ANY);
    int j;
    if (!box)
        return FALSE;
    if (!ogles2_backend->tex_load(t, k, l, x, y, w, h, (UBYTE *)box, row)) {
        if (!f->shadow[l]) { /* no GPU copy (a state-only context): the RAM copy */
            FreeVec(box);
            return FALSE;
        }
        for (j = 0; j < h; j++)
            memcpy((UBYTE *)box + (ULONG)j * row, f->shadow[l] + ((ULONG)(y + j) * f->w[l] + x) * 16, row);
    }
    for (j = 0; j < h; j++) {
        const GLfloat *s = (const GLfloat *)((UBYTE *)box + (ULONG)j * row);
        UBYTE *d = rgba + (ULONG)j * stride;
        for (i = 0; i < (ULONG)w * 4; i++)
            d[i] = float_byte(s[i]);
        if (f->format[l] == GL_RGB) /* the stored alpha is the shader's: an RGB image reads 1 */
            for (i = 3; i < (ULONG)w * 4; i += 4)
                d[i] = 255;
    }
    FreeVec(box);
    return TRUE;
}

BOOL ogles2_texture_read(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h,
                         UBYTE *rgba, ULONG stride)
{
    const struct OGLES2Face *f = &t->f[k];
    int j;
    if (ogles2_texture_half(t, k, l))
        return read_half(t, k, l, x, y, w, h, rgba, stride);
    if (level_class(f, l) != CLASS_RGBA8)
        return FALSE;
    if (ogles2_backend->tex_load(t, k, l, x, y, w, h, rgba, stride))
        return TRUE;
    if (!f->shadow[l]) /* no GPU copy (a state-only context): the RAM copy */
        return FALSE;
    for (j = 0; j < h; j++)
        memcpy(rgba + (ULONG)j * stride, f->shadow[l] + ((ULONG)(y + j) * f->w[l] + x) * 4, (ULONG)w * 4);
    return TRUE;
}

struct OGLES2Texture *ogles2_texture_hidden(struct OGLES2Context *ctx, UWORD w, UWORD h, GLenum format,
                                            GLenum type)
{
    struct OGLES2Texture *t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR);
    if (!t)
        return 0;
    init_object(t, 0, GL_TEXTURE_2D);
    t->min_filter = GL_NEAREST;
    t->f[0].shadow[0] = AllocVec((ULONG)w * h * 4 + 4, MEMF_ANY | MEMF_CLEAR);
    t->f[0].w[0] = w;
    t->f[0].h[0] = h;
    t->f[0].format[0] = format;
    t->f[0].type[0] = type;
    t->f[0].defined = 1;
    t->f[0].blank = 1;
    if (!t->f[0].shadow[0] || (ctx->v3d && (!upload_all(t) || !t->gpu))) {
        ogles2_texture_hidden_free(ctx, t);
        return 0;
    }
    return t;
}

void ogles2_texture_hidden_free(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    (void)ctx;
    if (!t)
        return;
    t->f[0].rendered = 0;
    free_levels(t);
    FreeVec(t);
}

/* ---- for glCopyTex* on the GPU (copy.c) ---- */

/* The default framebuffer's region as a texture: w x h RGBA8, filled by the
 * TFU from the raster image at `src` (rows `stride` bytes apart, the top row
 * first, in the render bitmap the GPU drew), red and blue swapped in the
 * sampler for a BGRA bitmap. One per context, made again for another size.
 * The caller has rendered and finished the context's jobs: none still reads
 * the scratch texture, none still writes the bitmap. 0: the TFU could not
 * (the CPU copies). */
struct OGLES2Texture *ogles2_texture_scratch(struct OGLES2Context *ctx, UWORD w, UWORD h, ULONG src, ULONG stride,
                                             BOOL bgra)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *t;
    int r;
    if (!ts || !ctx->v3d)
        return 0;
    if ((t = ts->scratch) && (t->f[0].w[0] != w || t->f[0].h[0] != h)) {
        ogles2_texture_hidden_free(ctx, t);
        ts->scratch = t = 0;
    }
    if (!t && !(t = ts->scratch = ogles2_texture_hidden(ctx, w, h, GL_RGBA, GL_UNSIGNED_BYTE)))
        return 0;
    r = ogles2_backend->tex_from_raster(t, src, stride, w, h, bgra);
    if (r == -3)
        return 0; /* the GPU stayed busy */
    if (r) {
        ogles2_log("glCopyTexSubImage2D: the TFU did not copy %ldx%ld from the framebuffer (%ld): the CPU copies",
                   (ULONG)w, (ULONG)h, (LONG)r);
        return 0;
    }
    t->f[0].rendered = 1; /* its RAM copy is not the picture (never read: nothing changes it) */
    t->f[0].blank = 0;
    return t;
}

/* Texture t on unit 0's 2D target for the copy's draw (draw.c); the one
 * there before is returned, to be put back. */
struct OGLES2Texture *ogles2_texture_unit0(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *old;
    if (!ts)
        return 0;
    old = ts->unit2d[0];
    ts->unit2d[0] = t;
    return old;
}

/* A framebuffer object's colour texture sampled by the copy: level 0,
 * nearest, clamped, whatever the application set (put back with
 * ogles2_texture_params_restore). */
void ogles2_texture_params_copy(struct OGLES2Texture *t, struct OGLES2TexParams *saved)
{
    saved->min_filter = t->min_filter;
    saved->mag_filter = t->mag_filter;
    saved->wrap_s = t->wrap_s;
    saved->wrap_t = t->wrap_t;
    saved->base_level = t->base_level;
    saved->max_level = t->max_level;
    saved->max_aniso = t->max_aniso;
    saved->lod_bias = t->lod_bias;
    t->min_filter = t->mag_filter = GL_NEAREST;
    t->wrap_s = t->wrap_t = GL_CLAMP_TO_EDGE;
    t->base_level = t->max_level = 0;
    t->max_aniso = 1.0f;
    t->lod_bias = 0.0f;
    t->pver++;
}

void ogles2_texture_params_restore(struct OGLES2Texture *t, const struct OGLES2TexParams *saved)
{
    t->min_filter = saved->min_filter;
    t->mag_filter = saved->mag_filter;
    t->wrap_s = saved->wrap_s;
    t->wrap_t = saved->wrap_t;
    t->base_level = saved->base_level;
    t->max_level = saved->max_level;
    t->max_aniso = saved->max_aniso;
    t->lod_bias = saved->lod_bias;
    t->pver++;
}

static void set_level(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, GLint level, UBYTE *img,
                      GLsizei width, GLsizei height, GLenum format, GLenum type, BOOL given);

/* OES_required_internalformat: a sized colour format's base format (0 for
 * any other value). */
static GLenum sized_color_base(GLenum ifmt)
{
    switch (ifmt) {
    case GL_RGBA8_OES: case GL_RGBA4: case GL_RGB5_A1: return GL_RGBA;
    case GL_RGB8_OES: case GL_RGB565:                  return GL_RGB;
    case GL_ALPHA8_OES:                                return GL_ALPHA;
    case GL_LUMINANCE8_OES:                            return GL_LUMINANCE;
    case GL_LUMINANCE8_ALPHA8_OES:                     return GL_LUMINANCE_ALPHA;
    default:                                           return 0;
    }
}

/* OES_required_internalformat in glTexImage2D: a sized internal format
 * stands for the base format, given a format and type that fill it with at
 * least its precision (the image keeps the type's, as an unsized one: GLES
 * lets an implementation store more). The value as given if it is not a
 * sized format; NOT_FILLED for one that format and type cannot fill
 * (GL_INVALID_OPERATION). */
#define NOT_FILLED 0xFFFFFFFFUL /* no GL value: 0 is an internal format to refuse too */
static GLenum sized_base(GLenum ifmt, GLenum format, GLenum type)
{
    GLenum base = sized_color_base(ifmt);
    BOOL ok;
    switch (ifmt) {
    case GL_RGBA4:   ok = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_4_4_4_4; break;
    case GL_RGB5_A1: ok = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_5_5_5_1; break;
    case GL_RGB565:  ok = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_5_6_5; break;
    case GL_DEPTH_COMPONENT16:
        return format == GL_DEPTH_COMPONENT && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)
                   ? GL_DEPTH_COMPONENT : NOT_FILLED;
    case GL_DEPTH24_STENCIL8_OES:
        return format == GL_DEPTH_STENCIL_OES && type == GL_UNSIGNED_INT_24_8_OES ? GL_DEPTH_STENCIL_OES : NOT_FILLED;
    default:
        if (!base)
            return ifmt;
        ok = type == GL_UNSIGNED_BYTE;
        break;
    }
    return ok && format == base ? base : NOT_FILLED;
}

static void tex_image(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum err;
    UBYTE *img;
    ULONG cb;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    if (level < 0 || level >= MAX_LEVELS || width < 0 || height < 0 || border != 0 ||
        width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) ||
        (face_target(target) && width != height)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* EXT_texture_format_BGRA8888 (revision 1.4 on): its sized format names
     * the same image as BGRA_EXT. */
    if ((GLenum)internalformat == GL_BGRA8_EXT && format == GL_BGRA_EXT)
        internalformat = GL_BGRA_EXT;
    /* OES_required_internalformat (GLES 3 programs and dEQP's clip_control
     * cases pass GL_RGBA8). */
    if (sized_base((GLenum)internalformat, format, type) == NOT_FILLED) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    internalformat = (GLint)sized_base((GLenum)internalformat, format, type);
    if ((GLenum)internalformat != format) {
        ogles2_error(ctx, internalformat == GL_RGBA || internalformat == GL_RGB ||
                          internalformat == GL_LUMINANCE || internalformat == GL_ALPHA ||
                          internalformat == GL_LUMINANCE_ALPHA || internalformat == GL_DEPTH_COMPONENT ||
                          internalformat == GL_DEPTH_STENCIL_OES ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    /* OES_depth_texture: 2D textures only (cube maps would need
     * OES_depth_texture_cube_map). */
    if ((format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES) && target != GL_TEXTURE_2D) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    before_change(ctx, ts, t);
    cb = copy_bytes(class_of(format, type));
    img = width && height ? AllocVec((ULONG)width * height * cb, MEMF_ANY | MEMF_CLEAR) : 0;
    if (width && height && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (class_of(format, type) != CLASS_RGBA8)
        ogles2_wide_seen |= wide_class(format, type);
    if (img && pixels)
        convert(img, (ULONG)width * cb, pixels, format, type, width, height, ctx->unpack_alignment);
    set_level(ctx, t, k, level, img, width, height, format, type, pixels != 0);
}

/* Level `level` of face k becomes `img`, its new RAM copy (0: empty);
 * `given`: it holds the image's texels, else zeros. The caller has dealt
 * with the jobs that use the texture (before_change). */
static void set_level(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, GLint level, UBYTE *img,
                      GLsizei width, GLsizei height, GLenum format, GLenum type, BOOL given)
{
    struct OGLES2Face *f = &t->f[k];
    if (f->shadow[level])
        FreeVec(f->shadow[level]);
    f->shadow[level] = img;
    f->w[level] = (UWORD)width;
    f->h[level] = (UWORD)height;
    f->format[level] = format;
    f->type[level] = type;
    f->defined |= 1UL << level;
    if (img && given)
        f->blank &= ~(1UL << level);
    else
        f->blank |= 1UL << level;
    if (!ctx->v3d)
        return; /* a state-only context keeps the images only */
    /* No GPU texture yet, a new level 0, or a level the chain has no room
     * for: rebuilt at its first use (a draw, ogles2_texture_state; a
     * framebuffer, ogles2_texture_surface). A program sends level 0 and
     * then each mip level: rebuilding at each stored every level before it
     * again (Quake III's load, M6). No job uses the old copy (before_change). */
    if (!t->gpu || (level == 0 && faces(t) == 1) || !ogles2_backend->tex_holds(t, k, level)) {
        ogles2_backend->tex_free(t);
    } else if (consistent(f, level) && img) {
        upload_level(t, k, level);
    }
}

/* Timed for probe P (frame.c). */
void ogles2_glTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    ULONG t0 = ogles2_clock_us();
    tex_image(ctx, target, level, internalformat, width, height, border, format, type, pixels);
    if (ogles2_context_valid(ctx))
        ctx->sum_teximage += (double)(ULONG)(ogles2_clock_us() - t0);
}

static void tex_sub_image(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    GLenum err;
    UBYTE *sub;
    ULONG cb;
    int y, k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    f = &t->f[k];
    if (level < 0 || level >= MAX_LEVELS || !(f->defined & (1UL << level))) {
        ogles2_error(ctx, level < 0 || level >= MAX_LEVELS ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return;
    }
    /* GLES 2.0 3.7.2: the level's own format; and the RAM copy's class
     * (float data into a float level, depth into a depth level). */
    if (format != f->format[level] || class_of(format, type) != level_class(f, level)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
        xoffset + width > f->w[level] || yoffset + height > f->h[level]) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!width || !height || !pixels)
        return;
    before_change(ctx, ts, t);
    cb = copy_bytes(level_class(f, level));
    if (!(sub = AllocVec((ULONG)width * height * cb, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    convert(sub, (ULONG)width * cb, pixels, format, type, width, height, ctx->unpack_alignment);
    for (y = 0; y < height; y++)
        memcpy(f->shadow[level] + ((ULONG)(yoffset + y) * f->w[level] + xoffset) * cb,
               sub + (ULONG)y * width * cb, (ULONG)width * cb);
    f->blank &= ~(1UL << level);
    if (t->gpu && level < ogles2_backend->tex_levels(t) && consistent(f, level))
        ogles2_backend->tex_store(t, k, level, sub, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    FreeVec(sub);
}

/* Timed for probe P (frame.c). */
void ogles2_glTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    ULONG t0 = ogles2_clock_us();
    tex_sub_image(ctx, target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (ogles2_context_valid(ctx))
        ctx->sum_teximage += (double)(ULONG)(ogles2_clock_us() - t0);
}

/* ---- copies from the framebuffer (glCopyTexImage2D, glCopyTexSubImage2D) ----
 * The region is read as glReadPixels reads it (the open job rendered first,
 * rows bottom-up, which is a texture's row order too), then given to
 * glTexImage2D/glTexSubImage2D, so targets, levels and errors are theirs.
 * The default framebuffer is RGBA8, from which every GLES2 format may be
 * copied (GLES 2.0 3.8.2, table 3.15). */

static int base_components(GLenum format)
{
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: return 1;
    case GL_LUMINANCE_ALPHA:          return 2;
    case GL_RGB:                      return 3;
    case GL_RGBA:                     return 4;
    default:                          return 0;
    }
}

/* width x height pixels from (x, y) as `format` bytes, tightly packed
 * (unpack alignment 1): LUMINANCE takes red, ALPHA alpha, RGB drops alpha
 * (table 3.15). Pixels outside the framebuffer read as 0. */
static UBYTE *read_region(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format)
{
    ULONG n = (ULONG)width * height, i;
    int c = base_components(format);
    UBYTE *rgba = AllocVec(n * 4 + 1, MEMF_ANY | MEMF_CLEAR), *out;
    UBYTE pack = ctx->pack_alignment;
    if (!rgba)
        return 0;
    ctx->pack_alignment = 1;
    ogles2_glReadPixels(ctx, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    ctx->pack_alignment = pack;
    out = rgba; /* converted in place: each pixel only shrinks */
    for (i = 0; i < n; i++) {
        const UBYTE *s = rgba + i * 4;
        UBYTE *d = out + i * c;
        UBYTE r = s[0], g = s[1], b = s[2], a = s[3];
        switch (format) {
        case GL_ALPHA:           d[0] = a; break;
        case GL_LUMINANCE:       d[0] = r; break;
        case GL_LUMINANCE_ALPHA: d[0] = r; d[1] = a; break;
        case GL_RGB:             d[0] = r; d[1] = g; d[2] = b; break;
        default:                 d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
        }
    }
    return out;
}

void ogles2_glCopyTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                             GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height,
                             GLint border)
{
    UBYTE *img, unpack;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (sized_color_base(internalformat)) /* OES_required_internalformat */
        internalformat = sized_color_base(internalformat);
    if (!base_components(internalformat)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0 || border != 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    /* RGB and RGBA on the GPU (copy.c): the level made empty, as
     * glTexImage2D makes it (its errors are glTexImage2D's), then the
     * region drawn into it. */
    if ((internalformat == GL_RGB || internalformat == GL_RGBA) && width > 0 && height > 0 &&
        ctx->error == GL_NO_ERROR && ctx->v3d && !(ctx->probe & OGLES2_PROBE_CPU_COPY)) {
        struct OGLES2TexState *ts;
        struct OGLES2Texture *t;
        int k;
        ogles2_glTexImage2D(ctx, target, level, (GLint)internalformat, width, height, 0, internalformat,
                            GL_UNSIGNED_BYTE, 0);
        if (ctx->error != GL_NO_ERROR)
            return;
        if ((ts = ts_get(ctx)) && (t = image_texture(ctx, ts, target, level, &k)) &&
            ogles2_copy_gpu(ctx, t, k, level, 0, 0, x, y, width, height))
            return;
    }
    if (!(img = read_region(ctx, x, y, width, height, internalformat))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    unpack = ctx->unpack_alignment;
    ctx->unpack_alignment = 1;
    ogles2_glTexImage2D(ctx, target, level, (GLint)internalformat, width, height, 0, internalformat,
                        GL_UNSIGNED_BYTE, img);
    ctx->unpack_alignment = unpack;
    FreeVec(img);
}

void ogles2_glCopyTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    UBYTE *img, unpack;
    GLenum fmt;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    /* In the level's own format, so the conversion is table 3.15's (a
     * LUMINANCE texture takes red); glTexSubImage2D reports a level that
     * does not exist. */
    {
        struct OGLES2TexState *ts = ts_get(ctx);
        struct OGLES2Texture *t;
        const struct OGLES2Face *f;
        int k;
        if (!ts || !(t = image_texture(ctx, ts, target, level, &k)))
            return;
        f = &t->f[k];
        fmt = level >= 0 && level < MAX_LEVELS ? f->format[level] : 0;
        if (!base_components(fmt))
            fmt = GL_RGBA; /* an undefined level: glTexSubImage2D refuses it */
        /* On the GPU (copy.c) when glTexSubImage2D would take the copy: a
         * level of its own, the region inside it, a format a render target
         * holds and RGBA bytes fill (not half-float: glTexSubImage2D takes
         * no bytes into a float level). */
        if (width > 0 && height > 0 && level >= 0 && level < MAX_LEVELS && (f->defined & (1UL << level)) &&
            xoffset >= 0 && yoffset >= 0 && xoffset + width <= f->w[level] && yoffset + height <= f->h[level] &&
            level_class(f, level) == CLASS_RGBA8 &&
            ogles2_copy_gpu(ctx, t, k, level, xoffset, yoffset, x, y, width, height))
            return;
    }
    if (!width || !height) { /* nothing to copy; the level and offsets are still checked */
        ogles2_glTexSubImage2D(ctx, target, level, xoffset, yoffset, width, height, fmt,
                               GL_UNSIGNED_BYTE, 0);
        return;
    }
    if (!(img = read_region(ctx, x, y, width, height, fmt))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    unpack = ctx->unpack_alignment;
    ctx->unpack_alignment = 1;
    ogles2_glTexSubImage2D(ctx, target, level, xoffset, yoffset, width, height, fmt,
                           GL_UNSIGNED_BYTE, img);
    ctx->unpack_alignment = unpack;
    FreeVec(img);
}

/* ---- compressed textures ----
 * OES_compressed_ETC1_RGB8_texture: decoded into the RGBA8 storage (etc1.c)
 * and stored as glTexImage2D does; the level then keeps the ETC1 format,
 * which no TexSubImage may change.
 * S3TC (DXT1, DXT3, DXT5; M7 step 3): kept as given, in RAM and on the GPU,
 * which samples BC1-BC3 itself. glCompressedTexSubImage2D replaces whole
 * blocks; glTexSubImage2D, glCopyTexSubImage2D (another format: refused by
 * glTexSubImage2D) and glGenerateMipmap give GL_INVALID_OPERATION, and a
 * framebuffer object cannot draw into one (not colour-renderable). */
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif

/* Level `level` of an S3TC texture: `bytes` of blocks (data 0: zeros). */
static void dxt_image(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target, GLint level,
                      GLenum format, GLsizei width, GLsizei height, ULONG bytes, const void *data)
{
    struct OGLES2Texture *t;
    UBYTE *img;
    int k;
    if (width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) || (face_target(target) && width != height)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    before_change(ctx, ts, t);
    img = bytes ? AllocVec(bytes, MEMF_ANY | MEMF_CLEAR) : 0;
    if (bytes && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (img && data)
        CopyMem((APTR)data, img, bytes);
    set_level(ctx, t, k, level, img, width, height, format, 0, data != 0);
}

void ogles2_glCompressedTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                   GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                   GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    ULONG bw, bh, bs;
    UBYTE *img, unpack;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bs = dxt_block_bytes(internalformat);
    if (internalformat != GL_ETC1_RGB8_OES && !bs) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    if (width < 0 || height < 0 || border != 0 || level < 0 || level >= MAX_LEVELS ||
        imageSize != (GLsizei)(bw * bh * (bs ? bs : 8))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (bs) {
        dxt_image(ctx, ts, target, level, internalformat, width, height, (ULONG)imageSize, data);
        return;
    }
    img = width && height ? AllocVec((ULONG)width * height * 4, MEMF_ANY | MEMF_CLEAR) : 0;
    if (width && height && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (img && data)
        ogles2_etc1_decode((const UBYTE *)data, (ULONG)width, (ULONG)height, img);
    unpack = ctx->unpack_alignment;
    ctx->unpack_alignment = 1;
    ogles2_glTexImage2D(ctx, target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    ctx->unpack_alignment = unpack;
    if (img)
        FreeVec(img);
    /* The level is ETC1: glTexSubImage2D (and ETC1's CompressedTexSubImage2D)
     * may not change it. */
    t = face_target(target) ? ts->unitcube[ts->active] : ts->unit2d[ts->active];
    t->f[face_target(target) ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0].format[level] =
        GL_ETC1_RGB8_OES;
}

void ogles2_glCompressedTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                      GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                                      GLenum format, GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    ULONG bs, bw, bh, row, j;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    bs = dxt_block_bytes(format);
    if ((target != GL_TEXTURE_2D && !face_target(target)) || (format != GL_ETC1_RGB8_OES && !bs)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* The order of the checks is dEQP's (negative_api.texture): values
     * first, then the level, its bounds and size; the format and the block
     * bounds last. */
    if (level < 0 || level >= MAX_LEVELS || xoffset < 0 || yoffset < 0 || width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    f = &t->f[k];
    if (!(f->defined & (1UL << level))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    if (xoffset + width > f->w[level] || yoffset + height > f->h[level] ||
        imageSize != (GLsizei)(bw * bh * (bs ? bs : 8))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* OES_compressed_ETC1_RGB8_texture: ETC1 images cannot be updated; S3TC
     * (EXT_texture_compression_s3tc): the level's own format, whole blocks
     * (a box may end at the level's edge). */
    if (!bs || format != f->format[level] || ((xoffset | yoffset) & 3) ||
        ((width & 3) && xoffset + width != f->w[level]) || ((height & 3) && yoffset + height != f->h[level])) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!width || !height || !data)
        return;
    before_change(ctx, ts, t);
    row = blocks4(f->w[level]) * bs;
    for (j = 0; j < bh; j++)
        CopyMem((UBYTE *)data + j * bw * bs, f->shadow[level] + ((ULONG)yoffset / 4 + j) * row + (ULONG)xoffset / 4 * bs,
                bw * bs);
    f->blank &= ~(1UL << level);
    if (t->gpu && level < ogles2_backend->tex_levels(t) && consistent(f, level))
        ogles2_backend->tex_store(t, k, level, (const UBYTE *)data, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* Level l+1 from level l: each texel the mean of a 2x2 box (a 1-texel edge
 * reuses its row or column). */
static void box_down(const UBYTE *src, int sw, int sh, UBYTE *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++) {
                ULONG sum = src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                            src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c];
                dst[(y * dw + x) * 4 + c] = (UBYTE)((sum + 2) / 4);
            }
        }
}

/* The same for an RGBA float copy. */
static void box_down_float(const GLfloat *src, int sw, int sh, GLfloat *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++)
                dst[(y * dw + x) * 4 + c] = (src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                                             src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c]) * 0.25f;
        }
}

/* glGenerateMipmap on the GPU (plan 7.6's "later"): the TFU filters levels
 * 1 .. n-1 from level 0 where it is, in the texture's GPU memory. The CPU
 * path below filters the RAM copies and uploads the whole chain, after a
 * read-back when the GPU drew level 0: glmark2's refract, a 1600x1200
 * render target every frame, spent 500 ms a frame there. Only for a 2D
 * texture whose GPU copy already has the chain, laid out by an earlier
 * glGenerateMipmap or the levels given (the first call takes the CPU path),
 * in a format the TFU filters. The levels it writes are newer on the GPU
 * than their RAM copies, as a render target's are (read back when the CPU
 * needs them, sync). Probe M keeps the CPU path. FALSE: the CPU path runs. */
static BOOL generate_tfu(struct OGLES2Context *ctx, struct OGLES2TexState *ts, struct OGLES2Texture *t, int n)
{
    struct OGLES2Face *f = &t->f[0];
    struct OGLES2Context *m, *user;
    ULONG made = ((1UL << n) - 1) & ~1UL; /* levels 1 .. n-1 */
    int l, r;
    if ((ctx->probe & OGLES2_PROBE_CPU_MIPMAP) || !ctx->v3d || faces(t) != 1 || n < 2 ||
        !ogles2_backend->tex_can_generate(t, n))
        return FALSE;
    for (l = 1; l < n; l++) {
        int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
        if (!(f->defined & (1UL << l)) || !f->shadow[l] || f->w[l] != w || f->h[l] != h ||
            f->format[l] != f->format[0] || f->type[l] != f->type[0])
            return FALSE;
    }
    /* The job that draws level 0 rendered first (any member's)... */
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fbo_texture_changing(m, t);
    /* ...and one that samples the texture and has not rendered yet: it must
     * see the old levels, which the TFU writes over in place (renaming, as
     * before_change does, would need the levels in RAM: the read-back this
     * path is for). Usually rendered already: the next job's start waits
     * for the one before. */
    if (t->used_serial && (user = ogles2_job_user(ctx, t->used_serial)) != 0 && user->v3d) {
        ogles2_job_flush(user, FALSE);
        ogles2_job_finish(user);
    }
    r = ogles2_backend->tex_generate(t, n);
    if (r) {
        if (r == -2)
            ogles2_log("glGenerateMipmap: the TFU did not finish (texture %ld, %ldx%ld): "
                       "the CPU makes the levels", (ULONG)t->name, (ULONG)f->w[0], (ULONG)f->h[0]);
        return FALSE;
    }
    f->rendered |= made;
    f->blank &= ~made;
    if (!ts->logged_tfu) {
        ts->logged_tfu = TRUE;
        ogles2_log("glGenerateMipmap: the TFU (texture %ld, %ldx%ld, %ld levels; logged once)",
                   (ULONG)t->name, (ULONG)f->w[0], (ULONG)f->h[0], (ULONG)n);
    }
    return TRUE;
}

void ogles2_glGenerateMipmap(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    int l, n, k, cls;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target == GL_TEXTURE_RECTANGLE) { /* ARB_texture_rectangle: no mip levels */
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(t = bound(ctx, ts, target)))
        return;
    /* A compressed level 0 (ETC1, S3TC), of any size: GLES 2.0 3.7.11. */
    if ((t->f[0].defined & 1) && (t->f[0].format[0] == GL_ETC1_RGB8_OES || dxt_block_bytes(t->f[0].format[0]))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!base_ok(t)) {
        /* GLES 2.0 3.7.11: a cube map that is not cube complete. */
        if (target == GL_TEXTURE_CUBE_MAP)
            ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* Depth textures: no mip chain to filter (GLES 3.0's rule for formats
     * that are not filterable; OES_depth_texture says nothing). */
    cls = level_class(&t->f[0], 0);
    if (cls == CLASS_DEPTH16 || cls == CLASS_DEPTH24) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    n = level_count(t->f[0].w[0], t->f[0].h[0]);
    if (generate_tfu(ctx, ts, t, n))
        return;
    before_change(ctx, ts, t);
    for (k = 0; k < faces(t); k++) {
        struct OGLES2Face *f = &t->f[k];
        for (l = 1; l < n; l++) {
            int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
            UBYTE *img = AllocVec((ULONG)w * h * copy_bytes(cls), MEMF_ANY);
            if (!img) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            if (cls == CLASS_FLOAT)
                box_down_float((const GLfloat *)f->shadow[l - 1], f->w[l - 1], f->h[l - 1], (GLfloat *)img, w, h);
            else
                box_down(f->shadow[l - 1], f->w[l - 1], f->h[l - 1], img, w, h);
            if (f->shadow[l])
                FreeVec(f->shadow[l]);
            f->shadow[l] = img;
            f->w[l] = (UWORD)w;
            f->h[l] = (UWORD)h;
            f->format[l] = f->format[l - 1];
            f->type[l] = f->type[l - 1];
            f->defined |= 1UL << l;
            f->blank &= ~(1UL << l);
        }
    }
    if (ctx->v3d && !upload_all(t))
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* ---- for the draw path ---- */

/* The fallback texture, 1x1 (0,0,0,1) (GLES 2.0 3.8.2: an incomplete
 * texture samples that). */
struct OGLES2Texture *ogles2_texture_fallback(struct OGLES2TexState *ts)
{
    struct OGLES2Texture *t = &ts->fallback;
    if (!t->gpu) {
        init_object(t, 0, GL_TEXTURE_2D);
        t->min_filter = t->mag_filter = GL_NEAREST;
        if (!(t->f[0].shadow[0] = AllocVec(4, MEMF_ANY | MEMF_CLEAR)))
            return 0;
        t->f[0].shadow[0][3] = 255;
        t->f[0].w[0] = t->f[0].h[0] = 1;
        t->f[0].format[0] = GL_RGBA;
        t->f[0].type[0] = GL_UNSIGNED_BYTE;
        t->f[0].defined = 1;
        if (!upload_all(t))
            return 0;
    }
    return t;
}


/* Probe V (vc4keys.c, docs/PI3_PLAN.md §4.2): the texture bound to a unit
 * for a sampler of `kind`, and its stored hardware type and swizzle
 * (storage_of); FALSE: none, or no image yet. */
BOOL ogles2_texture_stored(struct OGLES2Context *ctx, int unit, int kind, UBYTE *hw_type, UBYTE swizzle[4])
{
    struct OGLES2TexState *ts = ts_get(ctx);
    const struct OGLES2Texture *t;
    if (!ts || unit < 0 || unit >= OGLES2_MAX_TEXTURE_UNITS)
        return FALSE;
    t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit] : ts->unit2d[unit];
    return ogles2_texture_stored_of(t, hw_type, swizzle);
}

