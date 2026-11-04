/*
 * The V3D 4.2 backend's table (ogles2/backend.h, docs/PI3_PLAN.md §5.1):
 * the Raspberry Pi 4. Step 1 of P2: its capabilities, with the values the
 * front end hard-coded until then (state.c, texture.c, context.c, fbo.c,
 * frame.c; release 1.3.3).
 *
 * MIT licence (LICENSE).
 */
#include "../ogles2_internal.h"
#include "../backend.h"

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
    4088, 4095, /* Mesa's v3d: 4096 vec4 less what it keeps for itself */
    4,     /* GLES2's minimum, as Mesa's v3d reports */
    32.0f, /* V3D_MAX_LINE_WIDTH */
    512.0f, /* V3D_MAX_POINT_SIZE */
    16.0f, /* anisotropy */
    15.0f, /* LOD bias */
};

const struct OGLES2Backend ogles2_backend_v3d = {
    "V3D 4.2",
    42,
    &caps,
    ogles2_device_renderer,
};

const struct OGLES2Backend *ogles2_backend = &ogles2_backend_v3d;
