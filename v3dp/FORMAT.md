# `.v3dp` program binary container, version 1.0 (and 1.1: VC4, at the end)

**Status: frozen (2026-10-10).** Any change follows the versioning rules at the end. Verified:
the x86-64 and m68k writers produce identical bytes (`dev/spike-c/harness/compare_v3dp.sh`, five
test programs; the A1200 matched for the first four), and `v3dp_dump.py`, written from this
document, decodes every file.

A `.v3dp` file holds one linked GLES2 program compiled for V3D 4.2: every compiled shader
variant (QPU code, uniform recipe, the shader properties the draw path needs) and the program
tables the runtime needs to implement `glUniform*`, `glGetUniformLocation`, samplers and
attributes. It is written by the offline compiler (S1, `v3dglslc` on the PC) and by the
on-target compiler module (S2), and read by `ogles2.library` (plan sections 8.2 to 8.4).

**Rule zero: both compilers must produce the same bytes.** The writer serializes every field
explicitly; nothing is a memory image of a host struct (the hosts differ in byte order and in
struct alignment).

## Encoding

- All integers are **little-endian**. `u8`/`u16`/`u32`/`i16`/`i32`/`u64`.
- QPU instructions are `u64`, little-endian, exactly as the GPU reads them.
- Strings live in the string table (section `STRINGS`) and are referenced by `str`, a `u32`
  byte offset into it. Offset 0 is the empty string.
- Every section starts 4-byte aligned; padding bytes are 0.
- Floats are stored as their IEEE-754 bit patterns in `u32`.

## File layout

```
header
section 0
section 1
...
```

### Header (32 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | magic `"V3DP"` |
| 4 | `u16` | format major version (1) |
| 6 | `u16` | format minor version (0) |
| 8 | `u32` | V3D version × 10 + revision, as Mesa's `devinfo.ver` (42) |
| 12 | `char[12]` | Mesa commit, first 12 hex digits, NUL-padded |
| 24 | `u32` | number of sections |
| 28 | `u32` | CRC-32 (zlib's `crc32()`: reflected, polynomial 0xedb88320) of all bytes after the header |

A reader rejects a major version it does not know, a V3D version other than 42, and a Mesa
commit not on its list of validated commits (the runtime may then recompile with S2, or
report the miss for S1).

### Section

| Type | Field |
|---|---|
| `u32` | section type |
| `u32` | payload size in bytes (excluding padding) |
| bytes | payload, then zero padding to a multiple of 4 |

Unknown section types are skipped (forward compatibility within a major version).

| Type | Name | Count |
|---|---|---|
| 1 | `PROGRAM` | exactly 1, first |
| 2 | `STRINGS` | exactly 1, second |
| 3 | `STAGE_PARAMS` | one per linked stage (vertex, fragment) |
| 4 | `UNIFORMS` | exactly 1 |
| 5 | `ATTRIBUTES` | exactly 1 |
| 6 | `VARIANT` | one per compiled variant |

## `PROGRAM`

| Type | Field |
|---|---|
| `u8[32]` | BLAKE3 of the program's sources: for each shader in attach order, its stage (`u32`), its length in bytes (`u32`) and its text. A program linked with its attribute bindings (aliased attributes, `V3DGLSL_LINK_BOUND`) hashes them after the shaders the same way, as the stage `0x42494E44` ("BIND") with the text `<index> <name>
` per binding, sorted by name. The runtime's disk-cache file name is the hash's first 24 hex digits plus `.v3dp` (29 characters: FFS file names are at most 30); the runtime checks the full hash in the file |
| `u32` | stages present: bit 0 vertex, bit 4 fragment (Mesa's `mesa_shader_stage` bit numbers) |

## `STRINGS`

Concatenated NUL-terminated strings. The first byte is NUL (offset 0 = empty string).

## `STAGE_PARAMS`: one stage's constant buffer

The driver reads uniforms (`QUNIFORM_UNIFORM`) from a per-stage constant buffer of 32-bit
values: Mesa's `gl_program_parameter_list::ParameterValues`. The runtime keeps one per stage,
initialised from the template below, updates it from `glUniform*` (table `UNIFORMS`) and from
GL state before each draw (state entries below).

| Type | Field |
|---|---|
| `u32` | stage (0 vertex, 4 fragment) |
| `u32` | number of 32-bit values in the constant buffer (`NumParameterValues`) |
| `u32` | number of parameters (`NumParameters`) |
| per parameter: | |
| `u32` | kind: 0 uniform, 1 constant, 2 state variable (Mesa `PROGRAM_UNIFORM`, `PROGRAM_CONSTANT`, `PROGRAM_STATE_VAR`) |
| `str` | name |
| `u32` | value offset in the constant buffer, in 32-bit units (`ValueOffset`) |
| `u32` | size in components (`Size`) |
| `u32` | GL data type (`DataType`, e.g. `GL_FLOAT_VEC4`) |
| `i16[4]` | state indexes (`StateIndexes`, Mesa `gl_state_index` values); zeros unless kind 2 |
| `u32[n]` | template: the constant buffer's initial contents. Constants are filled in, uniforms hold the linker's initial values (0 in GLES2), state variables hold 0 |

The runtime must compute every state variable it is given before a draw, and must reject a
program containing a state index it does not implement (plan section 7.11), rather than leave
it at zero. State index numbers are Mesa's enum values at the validated commit, like the
recipe types below.

## `UNIFORMS`: the program's active uniforms

| Type | Field |
|---|---|
| `u32` | number of uniforms (Mesa's `UniformStorage` entries, in its order) |
| per uniform: | |
| `str` | name as Mesa stores it (an array of a basic type without `[0]`, struct members as `s.m`, `s[1].m`) |
| `u32` | GL type (`GL_FLOAT_MAT4`, `GL_SAMPLER_2D`, ...) of one element |
| `u32` | array elements (0 if not an array) |
| `u32` | flags: bit 0 sampler, bit 1 built-in (`gl_*`; a state variable, no storage places), bit 2 hidden (not visible to the application) |
| `u32` | number of storage places |
| per storage place: | where `glUniform*` writes the value; one per stage that uses the uniform |
| `u32` | stage |
| `u32` | offset in that stage's constant buffer, 32-bit units |
| `u8` | element stride in **bytes** (between array elements) |
| `u8` | vector stride in **bytes** (between matrix columns) |
| `u8` | format: 0 native, 1 integer stored as float (Mesa `gl_uniform_driver_format`) |
| `u8` | pad |
| `u32[2]` | sampler index for the vertex and the fragment stage (the texture index in that stage's `TMU_CONFIG` recipe data); 0xffffffff where not a sampler or unused there |

Samplers have no storage places: the texture unit a sampler reads (`glUniform1i`) is runtime
state, not part of a constant buffer. Built-in uniforms (`gl_DepthRange.near`, ...) have none
either; the shader reads them through state variables in `STAGE_PARAMS`.

Uniform locations are the runtime's own numbering; the file does not fix them. The strides
are the layout; with the V3D driver at the validated commit every vector, array element and
matrix column starts on 4 values (16 bytes), e.g. a `mat3` takes 12 values and a `vec3[4]` 16.

## `ATTRIBUTES`: vertex shader inputs

| Type | Field |
|---|---|
| `u32` | number of attributes |
| per attribute: | |
| `str` | name |
| `u32` | GL type |
| `u32` | GL location the linker assigned in bits 0-7 (informational). With bit 31 set, bits 8-11 say which of its columns the shader reads (bit k: column k; a vector or scalar: bit 0), each taking the next slot; a compiler older than that wrote the location alone, and then every column takes a slot |
| `u32` | input slot of its first column read: the number of vertex inputs the shader reads below it (`inputs_read` bit order, as `nir_recompute_io_bases` numbers them); 0xffffffff if the shader reads none of it |

The runtime maps an application's attribute arrays to input slots **by name**, so a binary
does not depend on `glBindAttribLocation`. A matrix attribute takes one slot per column.
The exception is aliasing (GLES 2.0 2.10.4: two names bound to one location). Mesa's linker
must know of it, or it gives every active attribute a location of its own and fails past
16. Such a program is linked with its bindings, and both names read the same slot.

## `VARIANT`: one compiled shader

| Type | Field |
|---|---|
| `u32` | stage: 0 vertex, 1 vertex binning (coordinate) shader, 4 fragment |
| key | the variant key, field by field (below) |
| prog data | the shader's properties, field by field (below) |
| `u32` | uniform recipe entries |
| `u32[n]`, `u32[n]` | recipe types (`enum quniform_contents` values), then their data words |
| `u32` | QPU code size in bytes |
| `u64[size/8]` | QPU instructions, little-endian |

Recipe type numbers are Mesa's enum values at the validated commit (0 `QUNIFORM_CONSTANT`
to 45 `QUNIFORM_BLEND_CONSTANT_A` at 74b69717c52b, and 46 `QUNIFORM_CLIP_Z_COEFF` from
`tools/v3dglslc/patches/0013`: the vertex position's z lerped towards w, -1 for
`EXT_clip_control`'s `ZERO_TO_ONE`, else 0; `v3dp_dump.py` lists them); the reader
checks the commit (header) and rejects types it does not implement (plan section 8.6). For
`QUNIFORM_UNIFORM` the data word is an index, in 32-bit units, into the stage's constant
buffer (`STAGE_PARAMS`); for `QUNIFORM_CONSTANT` it is the value itself.

Variants appear in the order the driver compiled them, without duplicates. The runtime uses
a variant only when its key equals the key the draw computes, field by field; anything else
is a miss (compile with S2, or report it).

What a file contains is the writer's choice, not part of the format. `v3dglslc` draws once
per default key state and stores what the driver compiles for those draws: triangles, lines
and points into an RGBA8 framebuffer object with a depth buffer, each sampler on its own unit
with a complete RGBA8 texture. That gives one vertex and one binning shader (two more if the
vertex shader writes `gl_PointSize`, for points) and a fragment shader per primitive class.
Variants the driver compiles at link time (`V3D_DEBUG=precompile`) are not stored: their keys
never match a draw (all eight colour buffers for `gl_FragColor`, every vertex output kept).

**`.v3dw`: the 32-bit-sampler variants** (2026-10-13; the format is unchanged). A program
with samplers has a second file of the same format, `<24 hex digits>.v3dw` beside its
`.v3dp`: the same draws with every sampler bound to an RGBA32F texture, so its fragment
variants have every sampler's bit set in `sampler_is_32b` (the TMU returns 32 bits a
channel, which float and depth textures need, and which reads 8-bit textures correctly too).
`ogles2.library` uses its fragment variants for draws that sample such a texture; its other
variants may be missing (`v3dglslc` captures what the driver compiles for those draws). The
offline compiler writes it next to the `.v3dp`; the on-target compiler writes it on request
(`V3DGLSL_LINK32`, `v3dp/v3dglsl_port.h`), when a draw first needs it.

**`.v3da`: the alpha-to-coverage variants** (2026-10-15; the format is unchanged). Every
program can have a third file of the same format, `<24 hex digits>.v3da`: the same draws with
the fragment key's `msaa` and `sample_alpha_to_coverage` set (`tools/v3dglslc/patches/0014`;
V3D has no fixed-function alpha to coverage, so the fragment shader turns alpha into the
sample mask). `ogles2.library` uses its fragment variants for draws into a 4x MSAA default
framebuffer with `GL_SAMPLE_ALPHA_TO_COVERAGE` enabled. Only the on-target compiler writes it,
on request (`V3DGLSL_LINK_A2C`), when a draw first needs it; it is looked for in `PROGDIR:`
and the cache first, as the `.v3dw` is.

### Common key and prog data (all stages)

Key (`struct v3d_key`): `u32 sampler_is_32b`, then `u8` each: `is_last_geometry_stage`,
`robust_uniform_access`, `robust_storage_access`, `robust_image_access`,
`robust_image_access_2`, `null_descriptor`, `u8 pad[2]`.

Prog data (`struct v3d_prog_data`): `u32` each: `spill_size`, `tmu_spills`, `tmu_fills`,
`tmu_count`, `qpu_read_stalls`; `u8` each: `compile_strategy_idx`, `threads`, `single_seg`,
`tmu_dirty_rcl`, `has_control_barrier`, `has_global_address`, `u8 pad[2]`.

### Vertex and binning shaders

Key (`struct v3d_vs_key`), after the common key: `u8 num_used_outputs`, `u8 pad[3]`,
`u8 used_outputs[64]` (`slot_and_component`), `u32 va_swap_rb_mask`, `u8` each: `is_coord`,
`per_vertex_point_size`, `clamp_color`, `u8 pad`.

Prog data (`struct v3d_vs_prog_data`), after the common part: `u8` each: `uses_iid`,
`uses_biid`, `uses_vid`, `separate_segments`, `vcm_cache_size`, `writes_psiz`, `u8 pad[2]`,
`u8 vattr_sizes[16]`, `u32 vpm_input_size`, `u32 vpm_output_size`,
`i32 driver_location_map[64]`.

### Fragment shader

Key (`struct v3d_fs_key`), after the common key: `u8` each: `is_points`, `is_lines`,
`line_smoothing`, `point_coord_upper_left`, `msaa`, `sample_alpha_to_coverage`,
`sample_alpha_to_one`, `can_earlyz_with_discard`, `software_blend`, `ignore_sample_mask`,
`cbufs`, `swap_color_rb`, `f32_color_rb`, `norm_16`, `snorm`, `ucp_enables`, `has_gs`,
`u8 pad[3]`; per draw buffer (8): `u32 format` (`enum pipe_format`), `u8 swizzle[4]`; per draw
buffer (8): `u32` each `rgb_func`, `rgb_src_factor`, `rgb_dst_factor`, `alpha_func`,
`alpha_src_factor`, `alpha_dst_factor`; `u32 logicop_func`, `u32 point_sprite_mask`.

Prog data (`struct v3d_fs_prog_data`), after the common part: `u8` each: `uses_pid`,
`num_inputs`, `writes_z`, `writes_z_from_fep`, `disable_ez`, `uses_center_w`,
`uses_implicit_point_line_varyings`, `lock_scoreboard_on_first_thrsw`,
`force_per_sample_msaa`, `u8 pad[3]`, `u8 input_slots[64]`, `u32 flat_shade_flags[3]`,
`u32 noperspective_flags[3]`, `u32 centroid_flags[3]`.

## Version 1.1: the Raspberry Pi 3's VideoCore IV (2026-11-04)

Adds VC4 programs (`docs/PI3_PLAN.md` §4.3); nothing of 1.0 changes, and a V3D 4.2 file is
still written as 1.0, byte for byte as before (it uses nothing of 1.1).

- **Header:** format minor version 1; device version **21** (VC4, V3D 2.1: Mesa vc4's
  `screen->v3d_ver`). A reader takes files of its own GPU only: ogles2.library the device of the
  backend in use. A 1.0 reader rejects device 21 at the header.
- **Sections:** `PROGRAM`, `STRINGS`, `STAGE_PARAMS`, `UNIFORMS` and `ATTRIBUTES` as in 1.0 (they
  come from Mesa's GL linker and state tracker; with the vc4 driver the constant buffer has the
  same layout, a vector or array element or matrix column per 4 values). The variants are
  sections of a new type, **7 `VC4_VARIANT`**; a VC4 file has no type 6.
- **File names:** the runtime's cache and `PROGDIR:shaders/` name a VC4 program
  `<24 hex digits>.vc4p` (the same hash), so the two GPUs' files lie side by side.

### `VC4_VARIANT`: one shader compiled by Mesa's vc4 driver

| Type | Field |
|---|---|
| `u32` | stage: 0 vertex, 1 coordinate (VC4's binning vertex shader), 4 fragment |
| key | the variant key, field by field (below) |
| prog data | the shader's properties (below) |
| `u32` | uniform recipe entries |
| `u32[n]`, `u32[n]` | recipe types (vc4's `enum quniform_contents`), then their data words |
| `u32` | QPU code size in bytes |
| `u64[size/8]` | QPU instructions, little-endian |

Recipe types are vc4's own enum (`vc4_qir.h`), not v3d's: at 74b69717c52b, 0 `CONSTANT`,
1 `UNIFORM`, 2 `VIEWPORT_X_SCALE`, 3 `VIEWPORT_Y_SCALE`, 4 `VIEWPORT_Z_OFFSET`,
5 `VIEWPORT_Z_SCALE`, 6 `TEXTURE_CONFIG_P0`, 7 `TEXTURE_CONFIG_P1`, 8 `TEXTURE_CONFIG_P2`,
9 `TEXTURE_FIRST_LEVEL`, 10 `TEXTURE_MSAA_ADDR`, 11 `UBO0_ADDR`, 12 `UBO1_ADDR`,
13 `TEXRECT_SCALE_X`, 14 `TEXRECT_SCALE_Y`, 15 `TEXTURE_BORDER_COLOR`,
16 to 19 `BLEND_CONST_COLOR_X`, `_Y`, `_Z`, `_W`, 20 `BLEND_CONST_COLOR_RGBA`,
21 `BLEND_CONST_COLOR_AAAA`, 22 `STENCIL`, 23 `SAMPLE_MASK`, 24 `UNIFORMS_ADDRESS`. As in 1.0,
`UNIFORM`'s data word is an index (32-bit units) into the stage's constant buffer and
`CONSTANT`'s the value; the texture types' data word is the sampler (texture unit index of
that stage). `UBO0_ADDR` (indirectly indexed uniform arrays) reads the constant buffer through
the TMU: the runtime keeps the stage's constant buffer in GPU memory too. The fragment shader's
texture lookups take their configuration words from the stream (`TEXTURE_CONFIG_P0`, `P1`
before each lookup, as the probe found, `tests/pi3_probe`).

Enum values in keys (`pipe_format`, `PIPE_SWIZZLE_*`, `PIPE_TEX_WRAP_*`, `PIPE_FUNC_*`,
`PIPE_BLEND_*`, `PIPE_BLENDFACTOR_*`, `PIPE_LOGICOP_*`) are Mesa's at the validated commit,
like the recipe types.

**Key, every stage** (`struct vc4_key`; its `shader_state` pointer is the program itself and is
not stored): for each of the 16 samplers (`VC4_MAX_TEXTURE_SAMPLERS`): `u32 format`,
`u8 swizzle[4]`, then `u8` each `compare_mode`, `compare_func`, `wrap_s`, `wrap_t`,
`force_first_level`, `u8 pad[3]` (16 bytes). The key's other view of these bits, the
multisampled texture's `msaa_width`/`msaa_height`, is not stored: GLES2 samples no
multisampled textures, and the two views of one union depend on the host's bitfield layout.

**Key, vertex and coordinate shaders** (`struct vc4_vs_key`), after the common key:
`u32 attr_formats[8]` (each attribute's `pipe_format`: VC4 converts formats in the shader),
`u8` each `is_coord`, `per_vertex_point_size`, `u8 pad[2]`, then the fragment shader's inputs
the variant writes (`fs_inputs`, compiled for the fragment variant it feeds): `u32` count,
per input `u8 slot`, `u8 swizzle`, zero padding to 4 bytes.

**Key, fragment shader** (`struct vc4_fs_key`), after the common key: `u32 color_format`,
`u8` each `depth_enabled`, `stencil_enabled`, `stencil_twoside`, `stencil_full_writemasks`,
`is_points`, `is_lines`, `point_coord_upper_left`, `msaa`, `sample_coverage`,
`sample_alpha_to_coverage`, `sample_alpha_to_one`, `logicop_func`; `u32 point_sprite_mask`,
`u32 ubo_1_size`; `u8 ucp_enables`, then the blend state (`pipe_rt_blend_state`) as `u8` each
`blend_enable`, `rgb_func`, `rgb_src_factor`, `rgb_dst_factor`, `alpha_func`,
`alpha_src_factor`, `alpha_dst_factor`, `colormask`; `u8 draw_buffer` (`tools/v3dglslc/patches/0016`:
the `gl_FragData[draw_buffer]` that is the colour, for `EXT_draw_buffers` drawn one attachment
per pass; `gl_FragColor` is the colour in every pass), `u8 pad[2]`.

**Prog data, every stage** (`struct vc4_compiled_shader`, what the draw needs): `u8` each
`fs_threaded` (the shader record's single-thread flag is its inverse), `num_inputs`,
`vattrs_live`, `disable_early_z`; `u8 vattr_offsets[9]` (byte offsets of attributes 0 to 7 in
the VPM, and the total as the ninth), `u8 pad[3]`; `u32 color_inputs`;
`u32 num_texture_samples`; then the fragment shader's inputs (`fs_inputs`, empty in vertex and
coordinate shaders): `u32` count, per input `u8 slot`, `u8 swizzle`, zero padding to 4 bytes.
The runtime matches a vertex variant to a fragment variant by these inputs.

The writer's choice of variants (which draws `v3dglslc -g 21` makes) is not part of the
format; it is settled with PD7 (`docs/PI3_PLAN.md` §4.2).

## Versioning

Adding a section type or appending fields at the end of a section is a minor version change.
Anything else is a major version change. The reader skips unknown sections and ignores
trailing bytes it does not know.
