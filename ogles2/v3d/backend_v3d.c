/*
 * The V3D 4.2 backend's table (ogles2/backend.h, docs/PI3_PLAN.md §5.1):
 * the Raspberry Pi 4. Step 1 of P2: its capabilities, with the values the
 * front end hard-coded until then (state.c, texture.c, context.c, fbo.c,
 * frame.c; release 1.3.3). Step 2: GPU memory, AllocVec'd fast RAM
 * (v3dcore/hw/v3d_mem_allocvec.c), whose bus address is its CPU address.
 * Step 3: jobs (job_v3d.c). Step 4: draw (draw_v3d.c). Step 5: texture
 * storage (texture_v3d.c). Step 6: framebuffer targets (fbo_v3d.c). Step
 * 7: the device (device_v3d.c) and programs (v3dp_v3d.c, program_v3d.c).
 *
 * MIT licence (LICENSE).
 */
#include "../ogles2_internal.h"
#include "../backend.h"
#include "backend_v3d.h"
#include "v3d_device.h"

static const struct OGLES2Caps caps = {
    /* the 33 extensions of release 1.3 */
    "GL_OES_element_index_uint GL_OES_compressed_ETC1_RGB8_texture GL_EXT_texture_format_BGRA8888 "
    "GL_OES_mapbuffer GL_EXT_blend_minmax GL_OES_draw_elements_base_vertex "
    "GL_OES_rgb8_rgba8 GL_OES_packed_depth_stencil GL_OES_fbo_render_mipmap "
    "GL_EXT_discard_framebuffer GL_OES_texture_npot GL_EXT_read_format_bgra "
    "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias GL_SGIS_texture_lod "
    "GL_ARB_texture_mirror_clamp_to_edge GL_OES_standard_derivatives "
    "GL_OES_get_program_binary GL_OES_texture_float GL_OES_texture_half_float "
    "GL_OES_texture_half_float_linear GL_OES_depth_texture GL_ARB_texture_rectangle "
    "GL_EXT_debug_marker GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc "
    "GL_ANGLE_texture_compression_dxt3 GL_ANGLE_texture_compression_dxt5 GL_EXT_clip_control "
    "GL_EXT_draw_buffers GL_NV_fbo_color_attachments GL_OES_required_internalformat "
    "GL_EXT_color_buffer_half_float",
    4096,  /* textures */
    4096,  /* V3D_MAX_FRAMEBUFFER_SIZE on 4.2 */
    4,     /* draw buffers (EXT_draw_buffers, M7 step 5) */
    16,    /* varyings: V3D_MAX_FS_INPUTS / 4 */
    16,    /* vertex attributes (V3D_MAX_VS_INPUTS / 4: 16) */
    4088, 4095, /* Mesa's v3d: 4096 vec4 less what it keeps for itself */
    4,     /* GLES2's minimum, as Mesa's v3d reports */
    32.0f, /* V3D_MAX_LINE_WIDTH */
    512.0f, /* V3D_MAX_POINT_SIZE */
    16.0f, /* anisotropy */
    15.0f, /* LOD bias */
};

static int mem_alloc(struct v3d_mem *m, ULONG size)
{
    return v3d_mem_alloc(ogles2_device(), m, (unsigned)size);
}

static void mem_free(struct v3d_mem *m)
{
    v3d_mem_free(ogles2_device(), m);
}

const struct OGLES2Backend ogles2_backend_v3d = {
    "V3D 4.2",
    42,
    &caps,
    6,       /* VARIANT */
    ".v3dp",
    ogles2_v3d_renderer,
    ogles2_v3d_open,
    ogles2_v3d_ready,
    ogles2_v3d_reset,
    ogles2_v3d_bytes,
    ogles2_v3d_gpu_reachable,
    ogles2_v3d_gpu_lock,
    ogles2_v3d_context_new,
    ogles2_v3d_context_resize,
    ogles2_v3d_context_free,
    mem_alloc,
    mem_free,
    ogles2_v3d_mem_free_after,
    ogles2_v3d_job_begin,
    ogles2_v3d_job_has_draws,
    ogles2_v3d_job_keep_zs,
    ogles2_v3d_job_check,
    ogles2_v3d_job_build,
    ogles2_v3d_job_bin,
    ogles2_v3d_job_render,
    ogles2_v3d_wait_render,
    ogles2_v3d_job_retire,
    ogles2_v3d_job_end,
    ogles2_v3d_job_report_stop,
    ogles2_v3d_job_free,
    ogles2_v3d_draw,
    ogles2_v3d_draw_free,
    ogles2_v3d_tex_upload,
    ogles2_v3d_tex_store,
    ogles2_v3d_tex_load,
    ogles2_v3d_tex_free,
    ogles2_v3d_tex_rename,
    ogles2_v3d_tex_levels,
    ogles2_v3d_tex_holds,
    ogles2_v3d_tex_can_generate,
    ogles2_v3d_tex_generate,
    ogles2_v3d_tex_from_raster,
    ogles2_v3d_rb_zs_storage,
    ogles2_v3d_rb_surface,
    0, /* rb_stencil_copy */
    ogles2_v3d_target_layout,
    ogles2_v3d_target_pools,
    ogles2_v3d_tex_surface,
    ogles2_v3d_tex_zs_surface,
    ogles2_v3d_variant_read,
    ogles2_v3d_variant_recipe,
    ogles2_v3d_variant_choose,
    ogles2_v3d_builtin_image,
    0, /* link_hints */
    ogles2_v3d_internal_program,
    0, /* internal_loaded */
    ogles2_v3d_job_can_queue,
    ogles2_v3d_job_queue,
    ogles2_v3d_job_render_queued,
};

const struct OGLES2Backend *ogles2_backend = &ogles2_backend_v3d;
