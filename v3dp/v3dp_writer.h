/*
 * .v3dp writer (FORMAT.md): serializes a linked GLES2 program and every V3D
 * variant the driver compiled for it, field by field in little-endian, so the
 * m68k compiler module and the PC compiler produce the same bytes.
 *
 * Needs Mesa patch 0009 (v3d_compiled_variant_hook).
 */
#ifndef V3DP_WRITER_H
#define V3DP_WRITER_H

#include <stddef.h>

struct gl_shader_program;

/* The GPU of the files written next: 42 (V3D 4.2, the default: format 1.0,
 * VARIANT sections from the v3d hook) or 21 (VC4: format 1.1, VC4_VARIANT
 * sections that v3dp_add_variant receives already serialized, from
 * tools/v3dglslc/vc4_variants.c). */
void v3dp_set_device(unsigned ver);

/* Start capturing variants: forgets earlier ones and, for V3D 4.2, installs
 * the driver hook. */
void v3dp_capture_begin(void);
/* A variant section's payload, serialized by the caller (VC4); duplicates
 * are dropped. */
void v3dp_add_variant(const void *payload, size_t size);
/* Stop capturing (removes the hook); the captured variants stay until the next begin. */
void v3dp_capture_end(void);

/* The "stage" of the program's attribute bindings in the hash (v3dp/FORMAT.md,
 * PROGRAM): a program linked with them (V3DGLSL_LINK_BOUND) hashes them as a
 * third source. "BIND". */
#define V3DP_HASH_BINDINGS 0x42494E44u

/* Writes the program and the captured variants to path. sources[i] is the
 * source of the shader attached i-th, stages[i] its mesa_shader_stage (or
 * V3DP_HASH_BINDINGS: hashed only).
 * Returns 0, or -1 after printing the reason to stderr. */
int v3dp_write(const char *path, struct gl_shader_program *prog,
               unsigned num_sources, const unsigned *stages,
               const char *const *sources);

#endif
