/*
 * Mesa's V3D driver on a fake DRM device, no window system.
 *   usage: v3dglslc [-d <V3D_DEBUG flags>] [-o <dir>] vs.vert fs.frag [vs2 fs2 ...]
 *          v3dglslc -g 21 [-d <VC4_DEBUG flags>] [-b attr0,attr1,...]
 *                         [-s depth,blend,points,lines,stencil,ubyte1,tex565,texlum,tex4444,
 *                             tex5551,texla,etc1,rt565,texd24,texds,texd16] [-k KEYFILE] [-o <dir>] vs.vert fs.frag ...
 *            (-k: variants for the vc4 keys in KEYFILE too: vc4_variants.c)
 *            (the Raspberry Pi 3's VC4: see run_pairs_vc4)
 *          v3dglslc SERVER   (AmigaOS: the on-target compiler, v3dp/v3dglsl_port.h)
 *          v3dglslc SERVE    (the host: the same requests as frames on stdin, serve_stdio;
 *                             tools/amictl/compile_bridge.py runs it for the A1200's
 *                             v3dglslc-remote)
 * Creates a V3D 4.2 screen (compat/fake_drm.c), a surfaceless GLES2 context
 * through the state tracker, and calls Mesa's GL entry points directly.
 * -o writes each program as <dir>/<vs>-<fs>.v3dp (dev/v3dp/FORMAT.md), with
 * the variants the driver compiles for draws in the default key states.
 *
 * Built for m68k-amigaos (build_m68k.sh; the seed of the S2 compiler module)
 * and natively on the PC (build_host.sh, the offline compiler), so the two can be compared
 * byte for byte.
 */
#include <stdlib.h>
#include "main/mtypes.h"
#include "main/context.h"
#include "main/shaderobj.h"
#include "api_exec_decl.h" /* generated: prototypes of the _mesa_* GL entry points */
#include "state_tracker/st_context.h"
#include "state_tracker/st_manager.h"
#include "frontend/api.h"
#include "pipe/p_screen.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/macros.h"
#include "v3d_screen.h"
#include "v3d_context.h" /* the driver's sampler views (bind_samplers) */
#include "xf86drm.h"
#include "v3dp_writer.h"

#define glCreateShader       _mesa_CreateShader
#define glShaderSource       _mesa_ShaderSource
#define glCompileShader      _mesa_CompileShader
#define glGetShaderiv        _mesa_GetShaderiv
#define glGetShaderInfoLog   _mesa_GetShaderInfoLog
#define glCreateProgram      _mesa_CreateProgram
#define glAttachShader       _mesa_AttachShader
#define glLinkProgram        _mesa_LinkProgram
#define glGetProgramiv       _mesa_GetProgramiv
#define glGetProgramInfoLog  _mesa_GetProgramInfoLog
#define glDeleteShader       _mesa_DeleteShader
#define glDeleteProgram      _mesa_DeleteProgram
#define glBindAttribLocation _mesa_BindAttribLocation
#define V3DC_WRITER
static void program_path(char *out, size_t size, const char *vs_path, const char *fs_path);
static int write_program(GLuint p, const char *path, const char *vs_src, const char *fs_src);
static int use_device(int dev);
#include "compile_common.c"
#ifdef __amigaos__
#include <proto/exec.h>
#else
#include <unistd.h>
#define V3DGLSL_OPS_ONLY /* the host's SERVE: the constants, no Exec */
#endif
#include "v3dglsl_port.h"

int fake_drm_open(void);
int fake_drm_vc4_open(void);

/* The Raspberry Pi 3's VideoCore IV (docs/PI3_PLAN.md §4.1, P4): -g 21 takes
 * Mesa's vc4 driver on its fake device (compat/fake_drm.c) instead of v3d.
 * Each program is drawn once in a reference state (draw_vc4_reference); its
 * variants are listed on stdout (vc4_variants.c) and with -o written as
 * <dir>/<vs>-<fs>.vc4p (v3dp/FORMAT.md 1.1). */
static int device = 42;
static int vc4_listing = 1; /* the command line lists VC4's variants; the server does not */
#ifdef V3DGLSLC_VC4 /* build_host.sh; the m68k build (S2) links vc4 in P4 */
struct renderonly;
struct pipe_screen *vc4_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro);
void vc4_variants_install(FILE *f); /* vc4_variants.c */
void vc4_variants_capture(int on);
int vc4_variants_compile_keys(const char *path);
int vc4_variants_compile_key_buffer(const void *keys, size_t size);
static const char *vc4_key_file; /* -k: variants for these keys too (vc4_variants.c) */
static const void *vc4_req_keys; /* a request's (V3DGLSL_VERSION_DEVICE) */
static size_t vc4_req_keys_size;
static int draw_vc4_reference(GLuint p);
extern uint8_t vc4_draw_buffer; /* Mesa patch 0016 */
static int run_pairs_vc4(int argc, char **argv, int first);
#endif

/* The build's stamp (build_m68k.sh: its time and the commits), which
 * ogles2.library reads from the file's tail to tell compilers apart
 * (ogles2_compiler_id: a size alone matched two different builds). Writable,
 * so it goes into the data hunk, which follows the 18 MB of code and its
 * relocations, a few kilobytes from the end of the file. */
#ifndef V3DGLSLC_STAMP
#define V3DGLSLC_STAMP "unstamped"
#endif
__attribute__((used)) char v3dglslc_stamp[] = "$V3DGLSLC-STAMP:" V3DGLSLC_STAMP "$";

#ifdef __amigaos__
void os_amiga_set_option(const char *name, const char *value);
#define set_option os_amiga_set_option
#else
static void set_option(const char *name, const char *value)
{
    setenv(name, value, 1);
}
#endif

/* The driver's driconf options, as the pipe loader passes them. */
static const driOptionDescription v3d_driconf[] = {
#include "driinfo_v3d.h"
};

/* File name part of a path, without its extension, at most 31 characters. */
static void base_name(char *out, const char *path)
{
    const char *b = path, *p, *dot = NULL;
    size_t n;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':')
            b = p + 1;
    for (p = b; *p; p++)
        if (*p == '.')
            dot = p;
    n = dot ? (size_t)(dot - b) : strlen(b);
    if (n > 31)
        n = 31;
    memcpy(out, b, n);
    out[n] = 0;
}

/* The render target of the key-state draws: an RGBA8 colour texture and a
 * 16-bit depth buffer, created once; and the textures the samplers get:
 * RGBA8, or RGBA32F for the variants that sample 32-bit returns. */
static GLuint draw_fbo, tex_2d, tex_cube, tex_2d32, tex_cube32, tex_rect, tex_rect32;
/* EXT_draw_buffers: targets with 2, 3 and 4 RGBA8 colour images, all drawn
 * into (the fragment key's cbufs 0x3, 0x7, 0xF). */
static GLuint mrt_fbo[3];

/* What write_program writes (v3dp/FORMAT.md): the variants of the default
 * key states (.v3dp); and, for a program with samplers, those of the same
 * draws with every sampler returning 32 bits (float and depth textures),
 * which ogles2.library loads only when a draw needs them (.v3dw, the same
 * file format). The command line writes both; a server request one. */
#define WRITE_BOTH 0
#define WRITE_PLAIN 1
#define WRITE_WIDE 2
#define WRITE_A2C 3 /* the alpha-to-coverage fragment variants: a .v3da (server only) */
static int write_mode = WRITE_BOTH;
extern bool v3d_force_alpha_to_coverage; /* Mesa patch 0014 */

static int make_target(void)
{
    GLuint tex, depth;
    GLenum status;

    _mesa_GenTextures(1, &tex);
    _mesa_BindTexture(GL_TEXTURE_2D, tex);
    _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    _mesa_BindTexture(GL_TEXTURE_2D, 0);
    _mesa_GenRenderbuffers(1, &depth);
    _mesa_BindRenderbuffer(GL_RENDERBUFFER, depth);
    _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
    _mesa_GenFramebuffers(1, &draw_fbo);
    _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    status = _mesa_CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "v3dglslc: draw target incomplete (0x%04x)\n", (unsigned)status);
        return 0;
    }
    {
        static const GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                                        GL_COLOR_ATTACHMENT3 };
        int n, i;
        for (n = 2; n <= 4; n++) {
            _mesa_GenFramebuffers(1, &mrt_fbo[n - 2]);
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, mrt_fbo[n - 2]);
            for (i = 0; i < n; i++) {
                GLuint t;
                _mesa_GenTextures(1, &t);
                _mesa_BindTexture(GL_TEXTURE_2D, t);
                _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, t, 0);
            }
            _mesa_BindTexture(GL_TEXTURE_2D, 0);
            _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            _mesa_DrawBuffers(n, bufs);
            status = _mesa_CheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "v3dglslc: %d-target draw target incomplete (0x%04x)\n", n, (unsigned)status);
                return 0;
            }
        }
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    }
    _mesa_Viewport(0, 0, 64, 64);
    _mesa_Enable(GL_DEPTH_TEST);

    /* Complete 1x1 RGBA8 textures for the samplers (bind_samplers). */
    {
        static const GLubyte texel[4] = { 255, 255, 255, 255 };
        GLenum face;
        _mesa_GenTextures(1, &tex_2d);
        _mesa_BindTexture(GL_TEXTURE_2D, tex_2d);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_2D, 0);
        _mesa_GenTextures(1, &tex_cube);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, tex_cube);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    /* The same as RGBA32F (OES_texture_float; nearest: not filterable), whose
     * 32-bit return sets the key's sampler_is_32b. */
    {
        static const GLfloat texel[4] = { 1, 1, 1, 1 };
        GLenum face;
        _mesa_GenTextures(1, &tex_2d32);
        _mesa_BindTexture(GL_TEXTURE_2D, tex_2d32);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_2D, 0);
        _mesa_GenTextures(1, &tex_cube32);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, tex_cube32);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    /* Rectangle textures (ARB_texture_rectangle, patches/0011), RGBA8 and
     * RGBA32F. */
    {
        static const GLubyte texel8[4] = { 255, 255, 255, 255 };
        static const GLfloat texel32[4] = { 1, 1, 1, 1 };
        _mesa_GenTextures(1, &tex_rect);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, tex_rect);
        _mesa_TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel8);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_GenTextures(1, &tex_rect32);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, tex_rect32);
        _mesa_TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel32);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, 0);
    }
    if (_mesa_GetError() != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: the key-state textures could not be made\n");
        return 0;
    }
    return 1;
}

/* The program has a sampler uniform. */
static int has_samplers(GLuint p)
{
    GLint n = 0, i, size;
    GLenum type;
    char name[256];
    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE || type == GL_SAMPLER_2D_RECT)
            return 1;
    }
    return 0;
}

/* The texture target and the key-state texture a sampler of `type` gets. */
static GLenum sampler_target(GLenum type, int wide, GLuint *tex)
{
    if (type == GL_SAMPLER_CUBE) {
        *tex = wide ? tex_cube32 : tex_cube;
        return GL_TEXTURE_CUBE_MAP;
    }
    if (type == GL_SAMPLER_2D_RECT) {
        *tex = wide ? tex_rect32 : tex_rect;
        return GL_TEXTURE_RECTANGLE;
    }
    *tex = wide ? tex_2d32 : tex_2d;
    return GL_TEXTURE_2D;
}

/* Give every sampler of the current program its own texture unit with a
 * complete RGBA8 texture of its kind bound, or (restore) put them all back
 * on unit 0, the value a freshly linked program has and the .v3dp template
 * records. Needed because:
 *  - samplers of different kinds on one unit make the draw an error (GLES2);
 *  - a sampler with nothing bound makes Mesa build its fallback texture,
 *    which waits on a fence (glFinish) the fake device cannot give;
 *  - the key's sampler_is_32b comes from the bound texture's format (wide:
 *    RGBA32F textures, for the variants with every sampler 32-bit). */
static struct pipe_context *the_pipe; /* the GL context's (main), for bind_samplers */

static void bind_samplers(GLuint p, int restore, int wide)
{
    GLint n = 0, i, j, unit = 0;

    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, units[32];
        GLenum type;
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_CUBE && type != GL_SAMPLER_2D_RECT)
            continue;
        loc = _mesa_GetUniformLocation(p, name);
        if (loc < 0 || size > 32)
            continue;
        for (j = 0; j < size; j++) {
            units[j] = restore ? 0 : unit;
            if (!restore) {
                GLuint tex;
                GLenum target = sampler_target(type, wide, &tex);
                _mesa_ActiveTexture(GL_TEXTURE0 + unit);
                _mesa_BindTexture(target, tex);
                unit++;
            }
        }
        _mesa_Uniform1iv(loc, size, units);
    }
    if (restore && the_pipe) {
        /* The driver's sampler views of every stage dropped, as in a fresh
         * process: the state tracker sends a stage's views only while its
         * program reads samplers, so a later program without any kept this
         * one's, and V3D's key takes sampler_is_32b from the views bound:
         * the program compiled before changed the variants (light after
         * tex: 7 bytes of light's .v3dp; found in ogles3). A later program
         * with samplers gets its own from the state tracker. */
        struct v3d_context *v3d = v3d_context(the_pipe);
        int s;
        for (s = 0; s < MESA_SHADER_STAGES; s++)
            if (v3d->tex[s].num_textures)
                the_pipe->set_sampler_views(the_pipe, (mesa_shader_stage)s, 0, 0, v3d->tex[s].num_textures, NULL);
    }
    _mesa_ActiveTexture(GL_TEXTURE0);
}

/* Draw once per default key state (plan section 8.2) so the driver compiles
 * the variants real draws use: triangles, lines and points into an RGBA8
 * target with depth on. The fake device runs nothing; the draws only make
 * the driver compute its keys and compile. */
static int draw_key_states(GLuint p, int wide)
{
    static const GLenum modes[] = { GL_TRIANGLES, GL_LINES, GL_POINTS };
    static const GLfloat zeros[4 * 4 * 3]; /* 3 vertices of a mat4 attribute */
    GLint n = 0, i, err;
    unsigned m;

    if (!draw_fbo && !make_target())
        return 0;
    _mesa_UseProgram(p);
    bind_samplers(p, 0, wide);
    _mesa_GetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, col, cols;
        GLenum type;
        _mesa_GetActiveAttrib(p, i, sizeof(name), NULL, &size, &type, name);
        loc = _mesa_GetAttribLocation(p, name);
        if (loc < 0)
            continue; /* built-in input such as gl_VertexID */
        cols = type == GL_FLOAT_MAT2 ? 2 : type == GL_FLOAT_MAT3 ? 3 : type == GL_FLOAT_MAT4 ? 4 : 1;
        for (col = 0; col < cols; col++) {
            _mesa_EnableVertexAttribArray(loc + col);
            _mesa_VertexAttribPointer(loc + col, 4, GL_FLOAT, GL_FALSE, 0, zeros);
        }
    }
    for (m = 0; m < ARRAY_SIZE(modes); m++)
        _mesa_DrawArrays(modes[m], 0, 3);
    /* EXT_draw_buffers: a fragment shader that writes gl_FragData[1] or up
     * is also drawn into 2, 3 and 4 targets. gl_FragColor would be broadcast
     * to them too, but every program would then compile nine more fragment
     * shaders: ogles2 draws such a program into the first target only. */
    {
        GET_CURRENT_CONTEXT(ctx);
        struct gl_shader_program *sp = _mesa_lookup_shader_program(ctx, p);
        struct gl_linked_shader *fsh = sp ? sp->_LinkedShaders[MESA_SHADER_FRAGMENT] : NULL;
        const uint64_t more = BITFIELD64_BIT(FRAG_RESULT_DATA1) | BITFIELD64_BIT(FRAG_RESULT_DATA2) |
                              BITFIELD64_BIT(FRAG_RESULT_DATA3);
        if (fsh && fsh->Program && (fsh->Program->info.outputs_written & more)) {
            int n;
            for (n = 0; n < 3; n++) {
                _mesa_BindFramebuffer(GL_FRAMEBUFFER, mrt_fbo[n]);
                for (m = 0; m < ARRAY_SIZE(modes); m++)
                    _mesa_DrawArrays(modes[m], 0, 3);
            }
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        }
    }
    /* Submit the job (a no-op on the fake device) so jobs do not pile up.
     * Not glFinish: its fence needs a sync file the fake device cannot give. */
    _mesa_Flush();
    for (i = 0; i < 16; i++)
        _mesa_DisableVertexAttribArray(i);
    bind_samplers(p, 1, 0);
    _mesa_UseProgram(0);
    err = _mesa_GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: GL error 0x%04x in the key-state draws\n", (unsigned)err);
        return 0;
    }
    return 1;
}

/* -o: <dir>/<vs>-<fs>.v3dp. */
static void program_path(char *out, size_t size, const char *vs_path, const char *fs_path)
{
    char vs_base[32], fs_base[32];
    size_t n = strlen(out_dir);
    base_name(vs_base, vs_path);
    base_name(fs_base, fs_path);
    snprintf(out, size, "%s%s%s-%s.%s", out_dir,
             n && out_dir[n - 1] != '/' && out_dir[n - 1] != ':' ? "/" : "", vs_base, fs_base,
             device == 21 ? "vc4p" : "v3dp");
}

static int write_program(GLuint p, const char *path, const char *vs_src, const char *fs_src)
{
    GET_CURRENT_CONTEXT(ctx);
    struct gl_shader_program *prog = _mesa_lookup_shader_program(ctx, p);
    /* The bindings of a V3DGLSL_LINK_BOUND request are hashed as a third
     * "source" (v3dp/FORMAT.md, PROGRAM). */
    static const unsigned stages[3] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, V3DP_HASH_BINDINGS };
    const char *sources[3];
    unsigned nsrc = link_bindings ? 3 : 2;
    int drawn;

    if (!prog)
        return 0;
    sources[0] = vs_src;
    sources[1] = fs_src;
    sources[2] = link_bindings;
#ifdef V3DGLSLC_VC4
    /* VC4 (docs/PI3_PLAN.md §4.5): one .vc4p, the reference draw's variants
     * and those of the keys given (-k, or a request's). */
    if (device == 21) {
        if (write_mode != WRITE_PLAIN && write_mode != WRITE_BOTH)
            return 0;
        v3dp_capture_begin();
        vc4_variants_capture(1);
        drawn = draw_vc4_reference(p);
        if (drawn && vc4_req_keys && vc4_variants_compile_key_buffer(vc4_req_keys, vc4_req_keys_size) < 0)
            drawn = 0;
        if (drawn && vc4_key_file && vc4_variants_compile_keys(vc4_key_file) < 0)
            drawn = 0;
        vc4_variants_capture(0);
        v3dp_capture_end();
        return drawn && v3dp_write(path, prog, nsrc, stages, sources) == 0;
    }
#endif
    /* The alpha-to-coverage variants: the same draws with the fragment key's
     * msaa and sample_alpha_to_coverage set (patch 0014). Only the fragment
     * shaders compile again, so the file holds fragment variants only; the
     * runtime takes only those from it. */
    if (write_mode == WRITE_A2C) {
        v3dp_capture_begin();
        v3d_force_alpha_to_coverage = true;
        drawn = draw_key_states(p, 0);
        v3d_force_alpha_to_coverage = false;
        v3dp_capture_end();
        return drawn && v3dp_write(path, prog, nsrc, stages, sources) == 0;
    }
    if (write_mode != WRITE_WIDE) {
        v3dp_capture_begin();
        drawn = draw_key_states(p, 0);
        v3dp_capture_end();
        if (!drawn || v3dp_write(path, prog, nsrc, stages, sources) != 0)
            return 0;
    }
    /* The 32-bit-sampler variants. After the plain draws the driver
     * compiles only the fragment shaders again (the vertex shaders' keys are
     * the same), so a .v3dw may hold fragment variants only: the runtime
     * takes only those from it. The command line's goes next to the .v3dp. */
    if (write_mode != WRITE_PLAIN && has_samplers(p)) {
        char wpath[512];
        size_t n = strlen(path);
        if (write_mode == WRITE_WIDE)
            snprintf(wpath, sizeof(wpath), "%s", path);
        else if (n > 5 && !strcmp(path + n - 5, ".v3dp") && n < sizeof(wpath))
            snprintf(wpath, sizeof(wpath), "%.*s.v3dw", (int)(n - 5), path);
        else
            return 0;
        v3dp_capture_begin();
        drawn = draw_key_states(p, 1);
        v3dp_capture_end();
        if (!drawn || v3dp_write(wpath, prog, nsrc, stages, sources) != 0)
            return 0;
    } else if (write_mode == WRITE_WIDE) {
        return 0; /* nothing samples: no such file */
    }
    return 1;
}

/* One request, the Amiga server's or the host's SERVE (version as struct
 * V3DGLSLRequest's: 1 the Pi 4's, 2 naming the GPU; dev its device field;
 * keys and bindings NULL when not sent): its result (0 done, 1 compile or
 * link failed, 2 not written, 3 not understood), the messages into log;
 * *quit for a QUIT the server must obey. */
static int handle(uint32_t version, uint32_t op, uint32_t dev, const char *vs, const char *fs, const char *out_path,
                  const char *bindings, const void *keys, size_t keys_size, char *log, size_t log_size, int *quit)
{
    int result;
    if (version != V3DGLSL_VERSION_DEVICE)
        dev = V3DGLSL_DEVICE_V3D42; /* version 1: the Pi 4, as since 1.0 */
    if (log && log_size)
        log[0] = 0;
    if ((version != V3DGLSL_VERSION && version != V3DGLSL_VERSION_DEVICE) ||
        (op != V3DGLSL_LINK && op != V3DGLSL_QUIT && op != V3DGLSL_COMPILE && op != V3DGLSL_LINK32 &&
         op != V3DGLSL_LINK_BOUND && op != V3DGLSL_LINK_A2C) ||
        ((op == V3DGLSL_LINK || op == V3DGLSL_LINK32 || op == V3DGLSL_LINK_BOUND || op == V3DGLSL_LINK_A2C) &&
         (!vs || !fs || !out_path)) ||
        (op == V3DGLSL_LINK_BOUND && !bindings) || (op == V3DGLSL_COMPILE && !vs == !fs) ||
#ifdef V3DGLSLC_VC4
        (dev != V3DGLSL_DEVICE_V3D42 && dev != V3DGLSL_DEVICE_VC4) ||
        (dev == V3DGLSL_DEVICE_VC4 && (op == V3DGLSL_LINK32 || op == V3DGLSL_LINK_A2C))
#else
        dev != V3DGLSL_DEVICE_V3D42
#endif
    ) {
        log_add(log, log_size, "the compiler does not understand this request", NULL);
        return 3;
    }
    if (op == V3DGLSL_QUIT) {
        *quit = 1;
        return 0;
    }
    if (use_device((int)dev) < 0) {
        log_add(log, log_size, "the compiler could not make a context for this GPU", NULL);
        return 2;
    }
    if (op == V3DGLSL_COMPILE) {
        /* The front end only: GL_COMPILE_STATUS and the shader's info log. */
        int ok;
        GLuint s = compile_src(vs ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, vs ? "vertex shader" : "fragment shader",
                               vs ? vs : fs, &ok, NULL, 0);
        char info[2048];
        info[0] = 0;
        glGetShaderInfoLog(s, sizeof(info), NULL, info);
        log_add(log, log_size, info, NULL);
        glDeleteShader(s);
        return ok ? 0 : 1;
    }
    write_mode = op == V3DGLSL_LINK32 ? WRITE_WIDE : op == V3DGLSL_LINK_A2C ? WRITE_A2C : WRITE_PLAIN;
    link_bindings = op == V3DGLSL_LINK_BOUND ? bindings : NULL;
#ifdef V3DGLSLC_VC4
    vc4_req_keys = dev == V3DGLSL_DEVICE_VC4 ? keys : NULL;
    vc4_req_keys_size = vc4_req_keys ? keys_size : 0;
#else
    (void)keys;
    (void)keys_size;
#endif
    result = link_pair("vertex shader", "fragment shader", vs, fs, out_path, log, log_size) ? 0 : 1;
    link_bindings = NULL;
#ifdef V3DGLSLC_VC4
    vc4_req_keys = NULL;
#endif
    return result;
}

#ifndef __amigaos__
/* SERVE on the host, the PC's side of the A1200's v3dglslc-remote
 * (tools/amictl/compile_bridge.py runs it): the Amiga server's requests as
 * frames on stdin, each answered on stdout, until stdin ends. Big-endian
 * u32s; a string is its length (0xffffffff: NULL) and its bytes.
 *   request: "V2Q1", version, op, device, vs, fs, bindings, keys, out_path
 *   answer:  "V2A1", result, log
 * ("V2": ogles2's, not ogles3's "V3" frames, which carry other fields.) A
 * QUIT is answered and the compiler goes on: the bridge ends it itself. The
 * compiler's own messages go to stderr: stdout is the frames'. */
static int read_u32(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return 1;
}

static void write_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

/* A string, NUL-terminated (malloc'd), or NULL; FALSE at the input's end. */
static int read_str(FILE *f, char **s, uint32_t *n)
{
    if (!read_u32(f, n))
        return 0;
    *s = NULL;
    if (*n == 0xffffffffu) {
        *n = 0;
        return 1;
    }
    if (!(*s = malloc((size_t)*n + 1)) || fread(*s, 1, *n, f) != *n)
        return 0;
    (*s)[*n] = 0;
    return 1;
}

static int serve_stdio(void)
{
    static char log[16384];
    FILE *out;
    int fd = dup(1), served = 0;
    dup2(2, 1);
    if (fd < 0 || !(out = fdopen(fd, "wb")))
        return 20;
    fprintf(stderr, "v3dglslc: serving on stdin (%s)\n", v3dglslc_stamp);
    for (;;) {
        char magic[4], *s[5] = { 0 };
        uint32_t version, op, dev, n[5];
        int k, ok, quit = 0;
        if (fread(magic, 1, 4, stdin) != 4)
            break;
        ok = !memcmp(magic, "V2Q1", 4) && read_u32(stdin, &version) && read_u32(stdin, &op) &&
             read_u32(stdin, &dev);
        for (k = 0; ok && k < 5; k++)
            ok = read_str(stdin, &s[k], &n[k]);
        if (!ok) {
            fprintf(stderr, "v3dglslc: unreadable request\n");
            return 20;
        }
        k = handle(version, op, dev, s[0], s[1], s[4], s[2], s[3], n[3], log, sizeof(log), &quit);
        fwrite("V2A1", 1, 4, out);
        write_u32(out, (uint32_t)k);
        write_u32(out, (uint32_t)strlen(log));
        fwrite(log, 1, strlen(log), out);
        fflush(out);
        for (k = 0; k < 5; k++)
            free(s[k]);
        served++;
    }
    fprintf(stderr, "v3dglslc: server done, %d request(s)\n", served);
    return 0;
}
#endif

#ifdef __amigaos__
/* A request goes back to its sender; one without a reply port (a QUIT sent
 * from ogles2.library's expunge, which cannot wait) is ours to free. */
static void done(struct V3DGLSLRequest *r)
{
    if (r->msg.mn_ReplyPort)
        ReplyMsg(&r->msg);
    else
        FreeVec(r);
}

/* The on-target compiler (plan 8.4): serve V3DGLSL_LINK requests on a
 * public port until V3DGLSL_QUIT. One context for all of them; each request
 * deletes its GL objects again. A second server started by a race finds the
 * port taken and leaves. */
static int serve(void)
{
    struct MsgPort *port;
    struct V3DGLSLRequest *r;
    int quit = 0, served = 0;

    Forbid();
    if (FindPort((CONST_STRPTR)V3DGLSL_PORT_NAME)) {
        Permit();
        fprintf(stderr, "v3dglslc: a compiler server is already running\n");
        return 0;
    }
    if (!(port = CreateMsgPort())) {
        Permit();
        return 20;
    }
    port->mp_Node.ln_Name = (char *)V3DGLSL_PORT_NAME;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();
    fprintf(stderr, "v3dglslc: serving on \"%s\" (%s)\n", V3DGLSL_PORT_NAME, v3dglslc_stamp);

    while (!quit) {
        WaitPort(port);
        while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
            /* Version 1: the Pi 4 (device 42), as since 1.0; version 2 names
             * the GPU and carries VC4's keys (docs/PI3_PLAN.md §4.5): read
             * only from a version-2 request (an older sender's ends before). */
            int v2 = r->version == V3DGLSL_VERSION_DEVICE;
            r->result = handle(r->version, r->op, v2 ? r->device : V3DGLSL_DEVICE_V3D42, r->vs, r->fs, r->out_path,
                               r->op == V3DGLSL_LINK_BOUND ? r->bindings : NULL, v2 ? r->vc4_keys : NULL,
                               v2 ? r->vc4_keys_size : 0, r->log, r->log_size, &quit);
            if (r->result != 3 && r->result != 2 && r->op != V3DGLSL_QUIT && r->op != V3DGLSL_COMPILE)
                served++;
            done(r);
        }
    }
    /* Gone from the list first, so nobody queues more; then fail what came. */
    Forbid();
    RemPort(port);
    Permit();
    while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
        log_add(r->log, r->log_size, "the compiler is shutting down", NULL);
        r->result = 2;
        done(r);
    }
    DeleteMsgPort(port);
    fprintf(stderr, "v3dglslc: server done, %d program(s)\n", served);
    return 0;
}
#endif

/* -b a,b,c: attribute a at location 0, b at 1, ... (link_bindings' "index
 * name" lines), as a program's glBindAttribLocation calls would. */
static const char *bindings_from_list(const char *list)
{
    static char buf[1024];
    size_t n = 0;
    int index = 0;
    while (*list && n + 40 < sizeof(buf)) {
        const char *end = strchr(list, ',');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        if (len > 31)
            len = 31;
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%d %.*s\n", index++, (int)len, list);
        list += len + (end ? 1 : 0);
    }
    buf[n] = 0;
    return buf;
}

#ifdef V3DGLSLC_VC4
/* -g 21: draw a linked program once so that the vc4 driver compiles its
 * variants for the reference state of tests/pi3/ref (mesa_triangle.c,
 * mesa_texture.c): a 128x128 RGBA8 target without depth, every attribute a
 * float array of its type's component count, every sampler on a 16x16 RGBA8
 * texture with NEAREST filtering and CLAMP_TO_EDGE, triangles. */
#define VC4_STATE_DEPTH 1
#define VC4_STATE_BLEND 2
#define VC4_STATE_POINTS 4
#define VC4_STATE_LINES 8
#define VC4_STATE_STENCIL 16
#define VC4_STATE_UBYTE1 32   /* attribute 1 as four normalized unsigned bytes */
#define VC4_STATE_TEX565 64   /* the 2D samplers on an RGB565 texture */
#define VC4_STATE_TEXLUM 128  /* and on a LUMINANCE one */
#define VC4_STATE_TEX4444 256 /* RGBA 4444 */
#define VC4_STATE_TEX5551 512 /* RGBA 5551 */
#define VC4_STATE_TEXLA 1024  /* LUMINANCE_ALPHA */
#define VC4_STATE_ETC1 2048   /* ETC1 */
#define VC4_STATE_RT565 4096  /* into an RGB565 target: plain, and blending as -s blend */
#define VC4_STATE_TEXD24 8192 /* a depth texture (OES_depth_texture: DEPTH_COMPONENT, UNSIGNED_INT) */
#define VC4_STATE_TEXDS 16384 /* a depth and stencil one (OES_packed_depth_stencil) */
#define VC4_STATE_TEXD16 32768 /* a 16-bit depth texture (DEPTH_COMPONENT, UNSIGNED_SHORT) */
#define VC4_STATE_TEXTURES (VC4_STATE_TEX565 | VC4_STATE_TEXLUM | VC4_STATE_TEX4444 | VC4_STATE_TEX5551 | \
                            VC4_STATE_TEXLA | VC4_STATE_ETC1 | VC4_STATE_TEXD24 | VC4_STATE_TEXDS | \
                            VC4_STATE_TEXD16)
/* -s depth,blend,points,lines,stencil,ubyte1,tex565,texlum,tex4444,tex5551,texla,etc1,rt565 */
static unsigned vc4_states;

static int draw_vc4_reference(GLuint p)
{
    static GLuint fbo, rb, tex2d, texcube;
    static const GLfloat zeros[4 * 4 * 3];
    static GLubyte texels[16 * 16 * 4];
    GLint n = 0, i, err, unit = 0;

    if (!fbo) {
        GLenum face;
        _mesa_GenRenderbuffers(1, &rb);
        _mesa_BindRenderbuffer(GL_RENDERBUFFER, rb);
        _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 128, 128);
        _mesa_GenFramebuffers(1, &fbo);
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, fbo);
        _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (_mesa_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "v3dglslc: the VC4 reference target is incomplete\n");
            return 0;
        }
        _mesa_Viewport(0, 0, 128, 128);
        _mesa_GenTextures(1, &tex2d);
        _mesa_BindTexture(GL_TEXTURE_2D, tex2d);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        _mesa_GenTextures(1, &texcube);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, texcube);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    _mesa_UseProgram(p);
    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, units[32], j;
        GLenum type;
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_CUBE)
            continue;
        loc = _mesa_GetUniformLocation(p, name);
        /* Every element of an array its own unit (bind_samplers): the rest left on
         * unit 0, beside a sampler of the other kind there, make the draw an error
         * and the link fail (dEQP uniform_api's sampler2D_samplerCube cases). */
        if (size > 32)
            size = 32;
        for (j = 0; j < size; j++) {
            units[j] = unit;
            _mesa_ActiveTexture(GL_TEXTURE0 + unit);
            _mesa_BindTexture(type == GL_SAMPLER_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D,
                              type == GL_SAMPLER_CUBE ? texcube : tex2d);
            unit++;
        }
        if (loc >= 0)
            _mesa_Uniform1iv(loc, size, units);
    }
    _mesa_ActiveTexture(GL_TEXTURE0);
    _mesa_GetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, col, cols, comps;
        GLenum type;
        _mesa_GetActiveAttrib(p, i, sizeof(name), NULL, &size, &type, name);
        loc = _mesa_GetAttribLocation(p, name);
        if (loc < 0)
            continue;
        cols = type == GL_FLOAT_MAT2 ? 2 : type == GL_FLOAT_MAT3 ? 3 : type == GL_FLOAT_MAT4 ? 4 : 1;
        comps = type == GL_FLOAT ? 1 : type == GL_FLOAT_VEC2 || type == GL_FLOAT_MAT2 ? 2
              : type == GL_FLOAT_VEC3 || type == GL_FLOAT_MAT3 ? 3 : 4;
        for (col = 0; col < cols; col++) {
            _mesa_EnableVertexAttribArray(loc + col);
            _mesa_VertexAttribPointer(loc + col, comps, GL_FLOAT, GL_FALSE, 0, zeros);
        }
    }
    _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
    /* A fragment shader that writes gl_FragData[1] or up: the variants that
     * take gl_FragData[k] as the colour (patch 0016), one per output it
     * writes; the library draws attachment k with variant k. */
    {
        GET_CURRENT_CONTEXT(ctx);
        struct gl_shader_program *sp = _mesa_lookup_shader_program(ctx, p);
        struct gl_linked_shader *fsh = sp ? sp->_LinkedShaders[MESA_SHADER_FRAGMENT] : NULL;
        int k;
        for (k = 1; k < 4; k++) {
            if (!fsh || !fsh->Program || !(fsh->Program->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA0 + k)))
                continue;
            /* A flush first: only a new job dirties the state that makes
             * vc4_update_compiled_fs look at the key again. Without it the
             * first such output kept the last draw's variant (gl_FragData[1]
             * was never compiled: found 2026-11-05, vc4_keys.py). */
            _mesa_Flush();
            vc4_draw_buffer = (uint8_t)k;
            _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
            _mesa_Flush();
        }
        vc4_draw_buffer = 0;
    }
    /* -s: the program drawn once more in each state named, for the Pi 3's
     * tests (tests/pi3/vc4core, P3); which key states the library's files
     * hold is settled with P5's key builder (PI3_PLAN 4.2). */
    if (vc4_states & (VC4_STATE_DEPTH | VC4_STATE_STENCIL)) {
        static GLuint zfbo, zcol, zrb;
        if (!zfbo) {
            _mesa_GenRenderbuffers(1, &zcol);
            _mesa_BindRenderbuffer(GL_RENDERBUFFER, zcol);
            _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 128, 128);
            _mesa_GenRenderbuffers(1, &zrb);
            _mesa_BindRenderbuffer(GL_RENDERBUFFER, zrb);
            _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 128, 128);
            _mesa_GenFramebuffers(1, &zfbo);
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, zfbo);
            _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, zcol);
            _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zrb);
            _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zrb);
        }
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, zfbo);
        if (vc4_states & VC4_STATE_DEPTH) {
            _mesa_Enable(GL_DEPTH_TEST);
            _mesa_DepthFunc(GL_LESS);
            _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
            _mesa_Flush();
            _mesa_Disable(GL_DEPTH_TEST);
        }
        if (vc4_states & VC4_STATE_STENCIL) { /* one-sided, mask 0xff */
            _mesa_Enable(GL_STENCIL_TEST);
            _mesa_StencilFunc(GL_ALWAYS, 1, 0xff);
            _mesa_StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
            _mesa_Flush();
            _mesa_Disable(GL_STENCIL_TEST);
        }
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    if (vc4_states & VC4_STATE_BLEND) {
        _mesa_Enable(GL_BLEND);
        _mesa_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
        _mesa_Flush();
        _mesa_Disable(GL_BLEND);
    }
    if (vc4_states & VC4_STATE_POINTS) {
        _mesa_DrawArrays(GL_POINTS, 0, 3);
        _mesa_Flush();
    }
    if (vc4_states & VC4_STATE_LINES) {
        _mesa_DrawArrays(GL_LINES, 0, 2);
        _mesa_Flush();
    }
    if (vc4_states & VC4_STATE_UBYTE1) {
        static const GLubyte bytes[64];
        _mesa_EnableVertexAttribArray(1);
        _mesa_VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, bytes);
        _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
        _mesa_Flush();
        _mesa_VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, zeros);
    }
    if (vc4_states & VC4_STATE_RT565) {
        static GLuint f565, c565;
        if (!f565) {
            _mesa_GenRenderbuffers(1, &c565);
            _mesa_BindRenderbuffer(GL_RENDERBUFFER, c565);
            _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, 128, 128);
            _mesa_GenFramebuffers(1, &f565);
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, f565);
            _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c565);
        }
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, f565);
        _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
        _mesa_Flush();
        if (vc4_states & VC4_STATE_BLEND) {
            _mesa_Enable(GL_BLEND);
            _mesa_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
            _mesa_Flush();
            _mesa_Disable(GL_BLEND);
        }
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    if (vc4_states & VC4_STATE_TEXTURES) {
        /* the formats tests/pi3/ref/mesa_cases.c uploads; RGBA4 makes no variant
         * of its own (this Mesa stores it as RGBA8: the RGBA4444 texture type
         * reads GL's packing with the RGBA8 swizzle) */
        static const struct { unsigned state; GLenum internal, format, type; GLsizei bytes; } fmts[] = {
            {VC4_STATE_TEX565, GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0},
            {VC4_STATE_TEXLUM, GL_LUMINANCE, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0},
            {VC4_STATE_TEX4444, GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 0},
            {VC4_STATE_TEX5551, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 0},
            {VC4_STATE_TEXLA, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 0},
            {VC4_STATE_ETC1, GL_ETC1_RGB8_OES, 0, 0, 16 * 16 / 2},
            {VC4_STATE_TEXD24, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0},
            {VC4_STATE_TEXDS, GL_DEPTH_STENCIL, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0},
            {VC4_STATE_TEXD16, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0},
        };
        static GLuint t[9];
        static GLubyte texels2[16 * 16 * 4];
        int u, which;
        for (which = 0; which < 9; which++) {
            if (!(vc4_states & fmts[which].state))
                continue;
            if (!t[which]) {
                _mesa_GenTextures(1, &t[which]);
                _mesa_BindTexture(GL_TEXTURE_2D, t[which]);
                if (fmts[which].bytes)
                    _mesa_CompressedTexImage2D(GL_TEXTURE_2D, 0, fmts[which].internal, 16, 16, 0,
                                               fmts[which].bytes, texels2);
                else
                    _mesa_TexImage2D(GL_TEXTURE_2D, 0, fmts[which].internal, 16, 16, 0, fmts[which].format,
                                     fmts[which].type, texels2);
                {
                    GLenum e = _mesa_GetError();
                    if (e != GL_NO_ERROR)
                        fprintf(stderr, "v3dglslc: GL error 0x%04x uploading texture state %u\n", (unsigned)e,
                                fmts[which].state);
                }
                _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            for (u = 0; u < unit; u++) { /* every unit: the 2D samplers' see it */
                _mesa_ActiveTexture(GL_TEXTURE0 + u);
                _mesa_BindTexture(GL_TEXTURE_2D, t[which]);
            }
            _mesa_DrawArrays(GL_TRIANGLES, 0, 3);
            _mesa_Flush();
        }
        for (u = 0; u < unit; u++) {
            _mesa_ActiveTexture(GL_TEXTURE0 + u);
            _mesa_BindTexture(GL_TEXTURE_2D, tex2d);
        }
        _mesa_ActiveTexture(GL_TEXTURE0);
    }
    _mesa_Flush(); /* submits the job to the fake device */
    for (i = 0; i < 8; i++)
        _mesa_DisableVertexAttribArray(i);
    _mesa_UseProgram(0);
    err = _mesa_GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: GL error 0x%04x in the VC4 reference draw\n", (unsigned)err);
        return 0;
    }
    return 1;
}

static int run_pairs_vc4(int argc, char **argv, int first)
{
    int i, failed = 0;
    for (i = first; i + 1 < argc; i += 2) {
        char *vs_src = load_text(argv[i]), *fs_src = load_text(argv[i + 1]);
        GLuint p, vs, fs;
        GLint ok = 0;
        int vs_ok = 0, fs_ok = 0;
        if (!vs_src || !fs_src) {
            fprintf(stderr, "v3dglslc: cannot read %s\n", !vs_src ? argv[i] : argv[i + 1]);
            failed++;
            free(vs_src);
            free(fs_src);
            continue;
        }
        fprintf(stderr, "v3dglslc: ==== program %s + %s (VC4)\n", argv[i], argv[i + 1]);
        vs = compile_src(GL_VERTEX_SHADER, argv[i], vs_src, &vs_ok, NULL, 0);
        fs = compile_src(GL_FRAGMENT_SHADER, argv[i + 1], fs_src, &fs_ok, NULL, 0);
        p = glCreateProgram();
        if (vs_ok && fs_ok) {
            glAttachShader(p, vs);
            glAttachShader(p, fs);
            apply_bindings(p, link_bindings);
            glLinkProgram(p);
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
        }
        fprintf(stderr, "v3dglslc: link: %s\n", ok ? "ok" : "FAILED");
        if (ok && out_dir) {
            char path[512];
            program_path(path, sizeof(path), argv[i], argv[i + 1]);
            ok = write_program(p, path, vs_src, fs_src);
            if (!ok)
                failed++;
        } else if (!ok || !draw_vc4_reference(p)) {
            failed++;
        }
        glDeleteProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        free(vs_src);
        free(fs_src);
    }
    fprintf(stderr, "v3dglslc: done, %d program(s) failed\n", failed);
    return failed ? 10 : 0;
}
#endif /* V3DGLSLC_VC4 */

/* -L: the limits programs are compiled with (GLSL's gl_Max* constants and
 * what links check), which ogles2.library must report on that GPU
 * (docs/PI3_PLAN.md §4.1; dEQP-GLES2 builtin_variable compares them). */
static int print_limits;

static int limits(void)
{
    static const struct { GLenum e; const char *name; } q[] = {
        {GL_MAX_VERTEX_ATTRIBS, "GL_MAX_VERTEX_ATTRIBS"},
        {GL_MAX_VERTEX_UNIFORM_VECTORS, "GL_MAX_VERTEX_UNIFORM_VECTORS"},
        {GL_MAX_FRAGMENT_UNIFORM_VECTORS, "GL_MAX_FRAGMENT_UNIFORM_VECTORS"},
        {GL_MAX_VARYING_VECTORS, "GL_MAX_VARYING_VECTORS"},
        {GL_MAX_TEXTURE_IMAGE_UNITS, "GL_MAX_TEXTURE_IMAGE_UNITS"},
        {GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, "GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS"},
        {GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, "GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS"},
        {GL_MAX_DRAW_BUFFERS, "GL_MAX_DRAW_BUFFERS"},
        {GL_MAX_TEXTURE_SIZE, "GL_MAX_TEXTURE_SIZE"},
        {GL_MAX_CUBE_MAP_TEXTURE_SIZE, "GL_MAX_CUBE_MAP_TEXTURE_SIZE"},
        {GL_MAX_RENDERBUFFER_SIZE, "GL_MAX_RENDERBUFFER_SIZE"},
    };
    unsigned i;
    for (i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        GLint v = -1;
        _mesa_GetIntegerv(q[i].e, &v);
        printf("%s %d\n", q[i].name, (int)v);
    }
    return 0;
}

/* One screen and context per GPU, made when first used (docs/PI3_PLAN.md
 * §4.1: the server serves both; a machine only asks for its own). Makes
 * dev's current: 0, or -1. */
static int use_device(int dev)
{
    static struct st_context *st_of[2];
    static driOptionCache info, cache;
    static struct pipe_screen_config config;
    int i = dev == 21;
    struct st_context *st = st_of[i];

    if (!st) {
        struct pipe_screen *screen;
        struct pipe_frontend_screen *fscreen = calloc(1, sizeof(*fscreen));
        struct st_context_attribs attribs;
        enum st_context_error err = ST_CONTEXT_SUCCESS;
        if (!fscreen)
            return -1;
        if (!config.options) {
            driParseOptionInfo(&info, v3d_driconf, ARRAY_SIZE(v3d_driconf));
            config.options = &cache;
            config.options_info = &info;
        }
#ifdef V3DGLSLC_VC4
        if (dev == 21)
            screen = vc4_screen_create(fake_drm_vc4_open(), &config, NULL);
        else
#endif
            screen = dev == 42 ? v3d_screen_create(fake_drm_open(), &config, NULL) : NULL;
        if (!screen) {
            fprintf(stderr, "v3dglslc: no screen for device %d\n", dev);
            free(fscreen);
            return -1;
        }
        fscreen->screen = screen;
        memset(&attribs, 0, sizeof(attribs));
        attribs.profile = API_OPENGLES2;
        attribs.major = 2;
        attribs.minor = 0;
        st = st_api_create_context(fscreen, &attribs, &err, NULL);
        if (!st) {
            fprintf(stderr, "v3dglslc: st_api_create_context failed (%d)\n", (int)err);
            return -1;
        }
        st_of[i] = st;
        if (!st_api_make_current(st, NULL, NULL)) {
            fprintf(stderr, "v3dglslc: st_api_make_current failed\n");
            return -1;
        }
        {
            GET_CURRENT_CONTEXT(ctx);
            if (dev == 21) {
                /* OES_standard_derivatives: vc4 compiles derivatives (ntq_fddx,
                 * ntq_fddy, per 2x2 quad) but never reports the capability, so
                 * Mesa leaves the extension off (docs/PI3_PLAN.md §2.1); the
                 * Pi 3's library offers it as the Pi 4's does. EXT_draw_buffers,
                 * emulated (§2.1): four draw buffers on the Pi 3 too, drawn one
                 * per pass (patch 0016's draw_buffer in the fragment key).
                 * Otherwise the driver's own limits (8 varyings, 8 vertex
                 * attributes, textures 2048): the library reports the same on
                 * the Pi 3 (GLSL's gl_Max* equal them; -L prints them). */
                ctx->Extensions.OES_standard_derivatives = GL_TRUE;
                ctx->Const.MaxDrawBuffers = 4;
            } else {
                /* ogles2.library's limits, which its GL queries report too
                 * (GLSL's gl_Max* constants must equal them: dEQP-GLES2
                 * builtin_variable): four draw buffers (EXT_draw_buffers, M7
                 * step 5; OGLES2_MAX_DRAW_BUFFERS), 16 texture units in each
                 * stage (OGLES2_MAX_STAGE_SAMPLERS; the driver offers 24) and
                 * 32 in all (OGLES2_MAX_TEXTURE_UNITS, M7 step 2; the driver
                 * offers 96). */
                ctx->Const.MaxDrawBuffers = 4;
                ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 16;
                ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 16;
                ctx->Const.MaxCombinedTextureImageUnits = 32;
            }
        }
#ifdef V3DGLSLC_VC4
        if (dev == 21)
            vc4_variants_install(vc4_listing ? stdout : NULL);
#endif
    } else if (!st_api_make_current(st, NULL, NULL)) {
        fprintf(stderr, "v3dglslc: st_api_make_current failed\n");
        return -1;
    }
    the_pipe = st->pipe;
    device = dev;
    v3dp_set_device(dev);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    const char *debug = NULL;
    static char debug_flags[256];

#ifdef __amigaos__
    /* libnix sends stderr to the console, not into the shell's ">file"
     * redirection (seen on the A1200; vamos merges them). Everything here and
     * Mesa's own log (the QPU listing) is written to stderr, so make it the
     * stdout stream: one ordered stream that the redirection captures. */
    __sF[2] = __sF[1];
    /* Line-buffered, so a failed assertion's message (abort() skips the
     * final flush) and everything before it reach the file. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#else
    /* Every compile must reach the driver (and the .v3dp hook). */
    set_option("MESA_SHADER_CACHE_DISABLE", "true");
#endif

#ifdef __amigaos__
    int server = argc == 2 && !strcmp(argv[1], "SERVER");
#else
    int server = argc == 2 && !strcmp(argv[1], "SERVE"); /* frames on stdin: serve_stdio */
#endif
    while (first + 1 < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "-d"))
            debug = argv[first + 1];
        else if (!strcmp(argv[first], "-o"))
            out_dir = argv[first + 1];
        else if (!strcmp(argv[first], "-g"))
            device = atoi(argv[first + 1]);
        else if (!strcmp(argv[first], "-b"))
            link_bindings = bindings_from_list(argv[first + 1]);
#ifdef V3DGLSLC_VC4
        else if (!strcmp(argv[first], "-k"))
            vc4_key_file = argv[first + 1];
        else if (!strcmp(argv[first], "-s")) {
            const char *a = argv[first + 1];
            vc4_states |= (strstr(a, "depth") ? VC4_STATE_DEPTH : 0) | (strstr(a, "blend") ? VC4_STATE_BLEND : 0) |
                          (strstr(a, "points") ? VC4_STATE_POINTS : 0) | (strstr(a, "lines") ? VC4_STATE_LINES : 0) |
                          (strstr(a, "stencil") ? VC4_STATE_STENCIL : 0) | (strstr(a, "ubyte1") ? VC4_STATE_UBYTE1 : 0) |
                          (strstr(a, "tex565") ? VC4_STATE_TEX565 : 0) | (strstr(a, "texlum") ? VC4_STATE_TEXLUM : 0) |
                          (strstr(a, "tex4444") ? VC4_STATE_TEX4444 : 0) | (strstr(a, "tex5551") ? VC4_STATE_TEX5551 : 0) |
                          (strstr(a, "texla") ? VC4_STATE_TEXLA : 0) | (strstr(a, "etc1") ? VC4_STATE_ETC1 : 0) |
                          (strstr(a, "rt565") ? VC4_STATE_RT565 : 0) | (strstr(a, "texd24") ? VC4_STATE_TEXD24 : 0) |
                          (strstr(a, "texds") ? VC4_STATE_TEXDS : 0) | (strstr(a, "texd16") ? VC4_STATE_TEXD16 : 0);
        }
#endif
        else if (!strcmp(argv[first], "-L")) {
            print_limits = 1; /* takes no value */
            first += 1;
            continue;
        } else
            break;
        first += 2;
    }
    if (device != 42 && device != 21) {
        fprintf(stderr, "v3dglslc: -g takes 42 (V3D 4.2, Raspberry Pi 4) or 21 (VC4, Raspberry Pi 3)\n");
        return 20;
    }
#ifndef V3DGLSLC_VC4
    if (device == 21) {
        fprintf(stderr, "v3dglslc: this build has no VC4 driver (-g 21)\n");
        return 20;
    }
#endif
    if (debug) {
        snprintf(debug_flags, sizeof(debug_flags), "%s", debug);
        set_option(device == 21 ? "VC4_DEBUG" : "V3D_DEBUG", debug_flags);
    }

    /* The server makes each GPU's context when a request first names it. */
    if (server) {
        vc4_listing = 0;
#ifdef __amigaos__
        return serve();
#else
        return serve_stdio();
#endif
    }
    if (use_device(device) < 0)
        return 20;
    fprintf(stderr, "v3dglslc: renderer %s\n", (const char *)_mesa_GetString(GL_RENDERER));
    if (print_limits)
        return limits();
#ifdef V3DGLSLC_VC4
    if (device == 21)
        return run_pairs_vc4(argc, argv, first);
#endif
    return run_pairs(argc, argv, first);
}
