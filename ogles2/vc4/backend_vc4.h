/*
 * The VC4 backend's shared definitions (ogles2/vc4/, docs/PI3_PLAN.md §5.1,
 * P5).
 *
 * MIT licence (LICENSE).
 */
#ifndef OGLES2_BACKEND_VC4_H
#define OGLES2_BACKEND_VC4_H

#include <exec/types.h>

struct OGLES2Variant;
struct ogles2_rd;
struct OGLES2Image;

/* A VC4 variant's prog data (v3dp/FORMAT.md 1.1, VC4_VARIANT), kept in
 * struct OGLES2Variant's be[] (static_assert'd to fit). */
struct vc4_prog
{
    UBYTE fs_threaded, num_inputs, vattrs_live, disable_early_z;
    UBYTE vattr_offsets[9];      /* per attribute, and [8] the total: the VPM layout */
    UBYTE nfs_inputs;            /* a fragment variant's inputs (slot, swizzle pairs) */
    UWORD pad;
    ULONG color_inputs, num_texture_samples;
    const UBYTE *fs_inputs;      /* nfs_inputs pairs, in the file */
};

#define VC4_PROG(v) ((struct vc4_prog *)(v)->be)
#define VC4_PROG_C(v) ((const struct vc4_prog *)(v)->be)

/* program_vc4.c */
const char *ogles2_vc4_variant_read(struct ogles2_rd *s, struct OGLES2Variant *v);
const char *ogles2_vc4_variant_recipe(struct OGLES2Variant *v, BOOL *clip_z, ULONG *bad);
const struct OGLES2Variant *ogles2_vc4_variant_choose(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                      GLenum mode);

#endif
