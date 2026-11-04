# ogles2 on the Raspberry Pi 3 (VC4): plan

One `ogles2.library` (OpenGL ES 2.0 for AmigaOS 3 on PiStorm) for both the Raspberry Pi 4 (V3D
4.2, as now) and the Raspberry Pi 3, whose GPU is the VideoCore IV 3D core ("VC4", V3D 2.1), with
one shader compiler for both (PD1, PD3). Started 2026-11-03 at the developer's request in the
repository `ladislawv/ogles2pi3`, a copy of `ogles2` at release 1.3.3 with its history. The work
lives here until it is finished and tested; then it goes into `ogles2`, and the release for both
Pis comes from there (PD8).
[`DEVELOPMENT_PLAN.md`](DEVELOPMENT_PLAN.md) stays the plan of the library (its §13 the Pi 4
status); this one covers what the Pi 3 adds. Its §10 is the status and the work queue of the Pi 3
work.

**How facts are marked.** Facts without a mark were checked on 2026-11-03 in one of: this
repository; Mesa at the pinned commit `74b6971` (`~/mesa-build/mesa` in WSL, with
`src/gallium/drivers/vc4/`); Emu68 `v1.1.0-beta.1` (`~/emu68/src-v1.1.0-beta.1`); Raspberry Pi's
Linux (`raspberrypi/linux`, branch `rpi-6.12.y`: device trees, `drivers/gpu/drm/vc4/`,
`raspberrypi-firmware.h`, `bcm2711_defconfig`); Raspberry Pi's userland (`hello_fft`); Broadcom's
reference guide (`VideoCoreIV-AG100-R.pdf`, 2013-09-16, §2.2). The source is named where it
matters. Facts marked **(Pi 3)** were measured on the test Pi 3 on 2026-11-04 (§6, §10).
**(verify)** marks what is still inferred; it is checked in the phase named, and the mark
replaced by the result.

Names: phases are P0 to P7 (§8); decisions about the Pi 3 work are PD1, PD2, ... (§9), not to be
confused with the phases or with DEVELOPMENT_PLAN's D1, D2, ...

## 1. Summary

- **Only OpenGL ES 2.0 on the Pi 3.** VC4 is ES 2.0 hardware (Mesa's vc4 driver stops there too):
  no ES 3 features, no compute. ogles3 stays Pi 4 only.
- **One library, two backends (PD1).** The front end (API, objects, state, the programs' tables,
  `agl*`, display, the program cache) is shared; behind a backend interface sit `v3dcore` (V3D
  4.2, as now) and a new `vc4core` (V3D 2.1). The library picks the backend at start from the
  device tree and `IDENT0`. Programs, headers, the import library and the API do not change: every
  existing program runs on a Pi 3 as it is.
- **One compiler, two drivers (PD3).** Today's `v3dglslc` is Mesa's GL core and gallium v3d driver
  over a fake DRM device of our own (`tools/v3dglslc/compat/fake_drm.c`; DEVELOPMENT_PLAN §8.2,
  §8.4). It also links Mesa's gallium vc4 driver, over a VC4 twin of that device (Mesa's drm-shim
  has one to copy from, `src/broadcom/drm-shim/vc4_noop.c`); each request names the GPU.
- **The Pi 4 does not change.** The backend split comes first and is accepted only when the Pi 4
  gives the same results and speed as release 1.3.3 (P2); the compiler's V3D 4.2 output stays
  byte for byte the same (P4).
- **The biggest risk is shader variants on the Pi 3.** VC4 has no fixed-function blending, texture
  swizzle or vertex format conversion: Mesa's vc4 compiles them into the shaders. So the fragment
  key holds the blend state, the colour mask, the target's format and each sampler's format,
  swizzle and wrap modes, and the vertex key holds each attribute's format (§4.2). Every new
  combination is a compile (a draw that needs one waits about a second on the A1200 with the Pi 4,
  `program.c`; longer on the Pi 3). The count for real programs is measured on the Pi 4 before any
  VC4 code exists (P2).
- **Everything that can run on the Pi 3 under Linux runs there first** (the developer,
  2026-11-03): the hardware's facts, the bare GPU, all of `vc4core`, the compiler's output on the
  GPU, the emulations' GPU side and the hardware's own baseline (dEQP-GLES2 with Mesa's vc4). The
  A1200 is for what only it has: Emu68, big-endian code, AmigaOS, the RTG display, the 68k
  compiler and the front end (PD4).
- **The extensions VC4 lacks are emulated (PD6):** S3TC by decoding, multiple render targets by
  one pass per target, half-float targets by packing into the tile buffer, float textures through
  the half-float texture type, and the texture-parameter extensions in the shader (§2.1).
- **Licence (PD9):** `v3dcore` and the Pi 4 backend code adapted from PiStorm3D stay under the
  Hyperion MiniGL licence; everything else (the shared front end, `vc4core`, the VC4 backend, the
  compiler, the import library) is MIT, with only permissive third-party notices. The built
  library contains `v3dcore`, so as a whole it stays under Hyperion's terms, as today (§5.3).

## 2. The hardware: what differs from the Pi 4

| | Pi 4 (`v3dcore`) | Pi 3 (BCM2837, BCM2837B0; Zero 2 W: RP3A0) |
|---|---|---|
| 3D core | V3D 4.2, `IDENT0` = `0x04443356` | VideoCore IV 3D, V3D 2.1: `IDENT0` = `0x02443356` (Linux's `V3D_EXPECTED_IDENT0`: "V3D", tech version 2); `IDENT1` holds VPM size, semaphores, TMUs and QPUs per slice, slices, revision (`vc4_regs.h`). **(Pi 3)** `IDENT0` `0x02443356`, `IDENT1` `0xc1102431` (revision 1, 3 slices, 4 QPUs and 2 TMUs per slice, 16 semaphores, VPM 12 KB), `IDENT2` `0x00000121` |
| Device-tree node | `/v3dbus/v3d@7ec04000`, compatible `brcm,2711-v3d` (`bcm2711-rpi-ds.dtsi`) | `/soc/v3d@7ec00000`, compatible `brcm,vc4-v3d` in Raspberry Pi's tree (`bcm270x.dtsi`, `status = "disabled"` until the KMS overlay enables it; upstream: `brcm,bcm2835-v3d`, `reg <0x7ec00000 0x1000>`). **(Pi 3)** as said: `reg` `0x7ec00000`, size `0x1000`; `okay` with the overlay, `disabled` without it, and the 3D core works either way, so the backend's choice (§5.1) does not look at `status` |
| Registers (ARM) | `0xFEC00000` | `0x3FC00000`; `/soc` `ranges` = `<0x7e000000 0x3f000000 0x1000000>` (`bcm2837.dtsi`; **(Pi 3)** plus `<0x40000000 0x40000000 0x1000>`, the ARM-local block) |
| Registers (68k, Emu68) | `0xF2C00000` (`v3d_regs.h`: Emu68 maps `/soc` from `0xF2000000`) | the same `0xF2C00000` by the same rule (`map_peripheral_ranges` maps `/soc` and, on BCM2711 only, `/scb`) (verify P5) |
| Mailbox | `0xFE00B880`, found through `/aliases/mailbox` and `/soc`'s `ranges` (`v3d_device.c`) | `0x3F00B880` (`bcm283x.dtsi`); the alias exists (`bcm270x-rpi.dtsi`; **(Pi 3)** `/soc/mailbox@7e00b880`) |
| GPU addresses | identity (bus = physical) | **bus = `0xC0000000` + physical** (`/soc` `dma-ranges` = `<0xc0000000 0x00000000 0x3f000000>`, `bcm2837.dtsi`; **(Pi 3)** plus `<0x7e000000 0x3f000000 0x1000000>` for the peripherals): the uncached alias, which the ARM side must use because it cannot see the VideoCore's L2 (`hello_fft` uses the same alias on the Pi 2 and 3). Every address the GPU or the firmware reads gets it, **the mailbox's message buffer too** (`v3d_device.c` passes the 68k address as is). **(Pi 3)** `ALLOCATE_MEMORY` with flag `0x4` and `LOCK_MEMORY` gave bus `0xFE001000` (physical `0x3E001000`, in `gpu_mem`); the GPU used it as is |
| Power | `power_on_V3D`: PM_GRAFX and the ASB bridges at BCM2711's addresses (ASB `0x7EC11000`) | **(Pi 3) the mailbox's `SET_ENABLE_QPU` (`0x00030012`, value 1), as `hello_fft`**: the registers answer at once. `SET_DOMAIN_STATE` (`0x00038030`, domain 10, `RPI_POWER_DOMAIN_V3D`) alone reports the domain on, but the registers read `0xdeadbeef` (Linux also enables the V3D clock, `vc4_v3d.c`, `clk_prepare_enable`); `GET_DOMAIN_STATE` keeps answering 0 after `SET_ENABLE_QPU`. Direct PM/ASB writes (Linux's `bcm2835-power`) not needed. **The firmware counts `SET_ENABLE_QPU`:** after two enables one disable leaves the core on with its state; only the last disable powers it off (registers `0xdeadbeef`), and the next enable brings it up reset (`VPMBASE` `0x10`). A user that does not disable (a crash, a second program) leaves the next one a core that was never reset, and **such a core loses a whole tile's primitives about once in 80 draws, with VPM errors in `ERRSTAT`** (bits 6 to 8: read, write and free of unallocated VPM); a core reset first did not, in over 1,600 draws, nor did Mesa in 3,000 (`ERRSTAT` stays `0x1000`). So `vc4core` takes the core over by disabling until the registers read `0xdeadbeef`, enabling once and checking `VPMBASE` = `0x10`, and checks `ERRSTAT` after jobs (P3: why the state goes bad, and a long soak) |
| RAM | 1 to 8 GB | 3B, 3B+: 1 GB; 3A+, Zero 2 W: 512 MB. Shared with the firmware's `gpu_mem`, Emu68's JIT cache and its RTG memory (`vc4-mem`). **(Pi 3)** Linux sees 905 MB with `gpu_mem` 76 MB (the KMS default), 855 MB with `gpu_mem=128` |
| CPU for Emu68 | Cortex-A72, 1.5 to 1.8 GHz | Cortex-A53: 1.2 GHz (3B), 1.4 GHz (3B+, 3A+), 1.0 GHz (Zero 2 W) |
| Shader cores | 8 QPUs, QPU v4 | 12 QPUs (3 slices of 4), QPU v2; **(Pi 3, 3B)** V3D clock 300 MHz (`vcgencmd measure_clock v3d`, both boots; core 400 MHz, ARM 1200 MHz) |
| Vertex input | VPM; formats converted by the hardware | VPM through the vertex DMA; **formats converted in the vertex shader**: `vc4_vs_key.attr_formats[8]`; at most 8 attributes (`max_inputs = 8`) |
| Blending | fixed-function | **in the fragment shader**, which reads the tile buffer (`vc4_nir_lower_blend.c`): the whole blend state (`pipe_rt_blend_state`: enable, equations, factors, colour mask), the logic op and the target's format (`color_format`: RGBA, BGRA, 565) are in the fragment key |
| Depth, stencil | per-draw packets | the fragment key has `depth_enabled`, `stencil_enabled`, `stencil_twoside`, `stencil_full_writemasks`; the stencil set-up words come in the uniform stream (`QUNIFORM_STENCIL`) |
| Command lists | V3D 4.x packets; the render list loops over supertiles by itself | VC4 packets, `src/broadcom/cle/vc4_packet.xml` (packers `v3d_packet_v21_pack.h`); the same scheme (binner, tile lists, renderer, semaphores), other encoding; **the render list names every tile** (coordinates, loads, branch to its bin list, stores). Linux builds it in the kernel; Mesa keeps an MIT copy of that code (`src/gallium/drivers/vc4/kernel/vc4_render_cl.c`, for the simulator). **(Pi 3) Mesa's copy is older than Linux's and wrong on the hardware:** it puts `CLEAR_COLORS` and the None-mode store after `TILE_RENDERING_MODE_CONFIG`, and then every job stores the previous job's clear colour (all tiles); Linux puts them first, which works (`tests/pi3_probe`, `clearseq`, `--variant=mesa`). The clear values are taken when the rendering-mode packet is processed. `vc4core` follows Linux's order |
| Binner memory | tile state, tile allocation, overflow | tile state 48 bytes and allocation 32 bytes per tile, the allocation 4 KB-aligned after the state (`kernel/vc4_validate.c`); **the tile state's pointers have 28 bits: state, allocation and overflow must lie in one 256 MB-aligned segment** (Linux `vc4_v3d.c`, `bin_bo_alloc`); overflow through `BPOA`/`BPOS`, `INTCTL`'s `OUTOMEM` bit |
| Shader state | GL shader state record, v4 | GL shader state record, v2; three shaders per program as now (fragment, vertex, coordinate); the vertex shader is compiled for the fragment shader it feeds (`vc4_vs_key.fs_inputs`) |
| Textures | UIF, UBLINEAR, LT; many formats; TFU | T and LT tilings (LT for small levels), sizes up to 2048 (`max_texture_2d_size`); types (guide, table 18): RGBA8888, RGBX8888, RGBA4444, RGBA5551, RGB565, LUMINANCE, ALPHA, LUMALPHA, ETC1, S16F, S8, S16, BW1, A4, A1, **RGBA64 (16-bit float per channel, filtered, read as two 32-bit results)**, RGBA32R and YUYV422R (raster). Mesa uses RGBA8888 (also for RGBX, BGRA and depth Z24S8, by swizzle and scaling), RGBA4444, RGBA5551, RGB565, ALPHA (A8, L8, I8, R8 by swizzle), LUMALPHA (LA8, RG8) and ETC1 (`vc4_formats.c`). Base addresses in 4 KB units (`VC4_TEX_P0_OFFSET`); mip levels stored before level 0, levels above 0 padded to powers of two. Raster textures: the guide supports them, Mesa says the Pi cannot sample them (`vc4_state.c`, it copies them to a tiled shadow). **(Pi 3)** both partly right: a raster RGBA32R texture is sampled right **when its width is a power of two** (16, 32, 64, 128, 256 wide, any height tried: 7 to 256); other widths are read from wrong rows (rows in groups of 4, spaced inside a group by the width rounded up to a power of two, the groups by 4 widths rounded up to a granule that changes with the height; no stride the CPU could choose fits, `tests/pi3_probe` `rastermap`). So an RTG bitmap or other raster image is sampled directly only at power-of-two widths, else through a copy (T/LT). An LT RGBA8888 texture gives Mesa's picture exactly. **No TFU** |
| Wrap modes | repeat, clamp, mirror, mirror-clamp | repeat, clamp to edge, mirror, clamp to border (`translate_wrap`) |
| Texture LOD | bias, min/max LOD in the sampler | none in the sampler: a mip-level count only (`MIPLVLS`); a first level other than 0 by an explicit LOD in the shader (`force_first_level`) or a copy (`vc4_state.c`) |
| Render targets | RGBA8, RGB565, RGBA4, RGB5A1, RGBA16F; up to 4 (MRT) | **one** colour buffer, RGBA8888 (BGRA by the shader's swizzle) or RGB565 (`vc4_formats.c`); 64×64 tiles, 32×32 with 4× MSAA (`vc4_job.c`); stored as raster, T or LT; up to 4096 (the shaded vertex format's 12.4 fixed point, `kernel/vc4_validate.c`), textures up to 2048; **(Pi 3)** Mesa reports 2048 for the viewport and renderbuffers |
| Raster stores | stride field | no stride field: a raster image's stride is its width rounded up to 4 pixels × bytes per pixel (`kernel/vc4_validate.c`). **(Pi 3)** confirmed: frames 129 and 130 wide are stored with a stride of 132 pixels, the padding pixels written too (with the clear colour), nothing beyond the frame's height. So an RTG bitmap is rendered into directly, with the frame's width set to `BytesPerRow` / 4 and the clip window to the visible width, when `BytesPerRow` is a multiple of 16; otherwise into a buffer of our own and copied (the padding columns of the bitmap get the clear colour) |
| Indices | 8, 16, 32 bit | 8 and 16 bit; Mesa makes a 16-bit shadow copy of 32-bit indices, and caps the index at 65535 (`vc4_draw.c`); non-indexed draws are split into pieces of at most 65535 vertices (GFXH-515) |
| Limits | DEVELOPMENT_PLAN §7.10 | from `vc4_screen.c`: 8 vertex attributes, 8 varyings, 16 samplers, textures 2048, points 1 to 512, lines 1 to 32, 4× MSAA (`VC4_MAX_SAMPLES`). **(Pi 3)** Mesa vc4 (26.2.2, `tests/pi3/ref/mesa_limits.c`): vertex attributes 8, uniform vectors 4088 (vertex) and 4095 (fragment) as on the Pi 4, **varyings 8** (the Pi 4 front end says 16: the backend gives it), texture units 16 per stage and 32 combined, texture, cube map and renderbuffer 2048, viewport 2048×2048, subpixel bits 4, points 1 to 512, lines 1 to 32, `MAX_SAMPLES` 4, draw buffers 1, read format RGBA/UNSIGNED_BYTE; every precision (vertex and fragment, low to high float) range 127 and 23 bits, int range 31/30 |
| Errata | — | those Mesa works around: HW-2116 (flush before `0x1ef0` draws in a job), GFXH-515/SW-5891 (16-bit binner indices), two-vertex line loops (drawn as lines), HW-2905 (early Z with a full-resolution load), HW-2726 (zero-size points) (`vc4_draw.c`, `vc4_emit.c`, `vc4_state.c`) |
| Speed | — | unknown until measured (P6). On paper the shader throughput is close (12 QPUs at the Pi 3's V3D clock against 8 at 500 MHz); against the Pi 3: blending in the shader, CPU tiling without the TFU, Emu68 on an A53 |

**Consequences for the library:**
- **Shader variants multiply** on the Pi 3 (§4.2): the main risk.
- **CPU work grows without the TFU.** The paths that use it on the Pi 4 (mipmaps, large uploads,
  `glCopyTex*` from the screen; DEVELOPMENT_PLAN §7.6) take their CPU fallbacks on the Pi 3. They
  exist already: probes `M`, `U`, `K` select them on the Pi 4. Mipmaps can also be rendered (each
  level drawn, as Mesa's vc4 does through its blitter).
- **GPU memory** is reached through the bus alias and kept coherent with the 68k's view: Emu68 maps
  RAM cached and identity-mapped on every Pi (`src/aarch64/start.c`), so the flush rules of
  DEVELOPMENT_PLAN §7.3 still apply; the VC4's own L2 and slice caches are cleared through
  `L2CACTL` and `SLCACTL` (Linux's `vc4_flush_caches`).
- **Same machine, same byte-order rules:** Emu68 runs the ARM big-endian and the GPU reads
  little-endian (`LE32`). Completion is polled as now (no interrupts under Emu68): `CT0CS`/`CT1CS`,
  `BFC`/`RFC` and `INTCTL` exist on VC4 (`vc4_regs.h`).
- **No other user of the VC4 3D core:** MiniGL (PiStorm3D) drives V3D 4.2 only, and Emu68's RTG
  uses the display (HVS) and its own `vc4-mem` (verify P5). Several programs share the GPU: PD5.

### 2.1 Extensions on VC4

The extension string (`ogles2/context.c`) has 33 names. The Pi 4 keeps its list as it is; on the
Pi 3 the library keeps every one too (PD6), as below. Mesa vc4's own string on the Pi 3 (P0) is
the reference for what the hardware does natively.

**(Pi 3) Mesa vc4's ES 2.0 string** (Mesa 26.2.2, `tests/pi3/ref/mesa_limits.c`): of our 33 it
has `OES_compressed_ETC1_RGB8_texture`, `OES_packed_depth_stencil`, `EXT_blend_minmax`,
`EXT_texture_format_BGRA8888`, `OES_texture_npot`, `OES_fbo_render_mipmap`, `OES_depth_texture`,
`OES_element_index_uint`, `OES_mapbuffer`, `EXT_discard_framebuffer`, `EXT_read_format_bgra`,
`OES_get_program_binary` (with no binary format), `OES_required_internalformat`,
`OES_rgb8_rgba8`, `OES_draw_elements_base_vertex`, the four S3TC/DXT names, and
`EXT_draw_buffers`/`NV_fbo_color_attachments` with one draw buffer. The S3TC names (and ASTC)
come from Mesa decompressing on the CPU at upload, as PD6 does. It lacks
`OES_standard_derivatives` (below), the half-float and float texture names,
`EXT_color_buffer_half_float`, `EXT_texture_lod_bias`, `EXT_texture_filter_anisotropic`,
`SGIS_texture_lod`, `ARB_texture_mirror_clamp_to_edge`, `EXT_clip_control` and
`EXT_debug_marker` (it has `KHR_debug`). `ARB_texture_rectangle` is a desktop name (Mesa's GL
2.1 string has it). Beyond ours it has `OES_vertex_half_float`, `EXT_frag_depth`,
`EXT_shadow_samplers`, `EXT_texture_border_clamp`, `APPLE_texture_max_level`, `EXT_sRGB`,
`OES_vertex_array_object` and others not planned here.

| Kind | Extensions | How |
|---|---|---|
| Front end only | `OES_mapbuffer`, `EXT_discard_framebuffer`, `EXT_read_format_bgra`, `OES_get_program_binary`, `EXT_debug_marker`, `OES_required_internalformat`, `OES_rgb8_rgba8`, `OES_draw_elements_base_vertex` | as on the Pi 4 (base vertex: the attribute addresses offset per draw, as `vc4_draw.c` does) |
| Native | `OES_compressed_ETC1_RGB8_texture`, `OES_packed_depth_stencil`, `EXT_blend_minmax`, `EXT_texture_format_BGRA8888`, `OES_texture_npot`, `OES_fbo_render_mipmap`, `OES_standard_derivatives`, `OES_depth_texture`, `ARB_texture_rectangle` | ETC1 texture type; depth is always 24 + 8 bits; min/max in the shader's blending; BGRA by swizzle; NPOT with padded mip levels; derivatives by QPU rotations within each 2×2 quad (`ntq_fddx`, `ntq_fddy`: complete, used by Mesa's GL 2.1, but the driver has never reported the capability, once `PIPE_CAP_SM3`, now `fragment_shader_derivatives`, so Mesa's ES string lacks the extension; dEQP-GLES2 has no derivative cases, so `tests/m6_ext` checks it, P3); depth sampled as Z24S8 through RGBA8888 with the compare in the shader; `QUNIFORM_TEXRECT_SCALE_X/Y` |
| Near native | `OES_element_index_uint` | the 16-bit copy works while a draw's indices span at most 65536 (rebased by the minimum index); a wider draw is split by the CPU into pieces whose vertices are copied and re-indexed (Mesa does not do this: indices above 65535 break there) |
| Emulated, texture data | `EXT_texture_compression_s3tc`, `EXT_texture_compression_dxt1`, `ANGLE_texture_compression_dxt3`, `ANGLE_texture_compression_dxt5` | decoded on the CPU at upload (Mesa's `util/format` S3TC decoders, MIT) into RGBA8888, or RGB565/RGBA5551 for DXT1; 4 to 8 times the memory |
| Emulated, half-float textures | `OES_texture_half_float`, `OES_texture_half_float_linear` | the RGBA64 texture type (filtered by the hardware); the shader reads two results (RG, BA) and unpacks halves: a vc4 compiler change (key bit per sampler). RGB, LUMINANCE, ALPHA, LUMINANCE_ALPHA expanded to RGBA at upload |
| Emulated, float textures | `OES_texture_float` | stored as RGBA64 half floats (range ±65504, 11-bit precision): a documented deviation (accepted by the developer, 2026-11-03). Not planned: exact storage as raw 32-bit values in RGBA8888 texels decoded in the shader |
| Emulated, in the shader | `ARB_texture_mirror_clamp_to_edge` (the coordinate's absolute value, then the hardware's clamp to edge); `EXT_texture_lod_bias` (a uniform added through the TMU's bias input); `SGIS_texture_lod` (base and max level through `MIPLVLS` and a first-level copy as Mesa makes; min/max LOD by an LOD computed from derivatives and an explicit-LOD lookup); `EXT_texture_filter_anisotropic` (extra lookups along the major axis from the derivatives, up to the maximum reported, at least 2) | key bits per sampler and vc4 compiler changes (§4.1); the cost falls only on programs that use them |
| Emulated, render targets | `EXT_draw_buffers`, `NV_fbo_color_attachments` | **one pass per colour attachment:** the framebuffer object's job is binned and rendered once per attachment, each pass with a fragment variant that writes that attachment's output (key: output index) and loads the depth and stencil the job started with; only the last pass stores them. Costs N times the work; needs a vc4 compiler change (Mesa's vc4 has one colour output, `max_outputs` 1 for fragment shaders: the compiler's screen reports four draw buffers, and the key picks one output). **Compiler side done (2026-11-04):** patch 0016 adds the fragment key's `draw_buffer` (`gl_FragData[draw_buffer]` is the colour, `gl_FragColor` in every pass, the other outputs dropped), `v3dglslc -g 21` reports four draw buffers and compiles one fragment variant per `gl_FragData[k]` written; the format's key has the byte (FORMAT.md 1.1) |
| Emulated, render targets | `EXT_color_buffer_half_float` | the fragment shader packs two half floats into each 32-bit tile-buffer pixel and blends them itself (VC4 blends in the shader anyway): RGBA16F as two packed images (RG, BA) through the passes above, sampled as packed textures and unpacked in the shader (filtering done in the shader). The largest change of all: a new packed colour format in `vc4_nir_lower_blend.c` |
| Emulated, depth | `EXT_clip_control` | origin by the front end's flips as now; `ZERO_TO_ONE` by a twin of patch 0013 (z lerped towards w by a uniform) in the vc4 vertex and coordinate shaders (verify P4: how VC4 clips z) |

Order of work (P3, P4): the native and near-native ones first, then S3TC, half-float and float
textures, the texture-parameter ones, then multiple render targets, then half-float targets.

### 2.2 References

- Broadcom's "VideoCore IV 3D Architecture Reference Guide" (`VideoCoreIV-AG100-R.pdf`, 2013-09-16,
  111 pages; in `external/Warp3DPiStorm4/`, DEVELOPMENT_PLAN §3; on this PC also in
  `C:\- CPP\pistorm3d\external\Warp3DPiStorm4\`). Its errata are in Mesa's and Linux's comments
  (HW-, GFXH-, SW- numbers, §2).
- Mesa (MIT), at the pinned commit: `src/gallium/drivers/vc4/` (driver and compiler: keys in
  `vc4_qir.h`, key set-up in `vc4_program.c`, uniforms in `vc4_uniforms.c`, draws in
  `vc4_draw.c`, state in `vc4_emit.c`/`vc4_state.c`, jobs in `vc4_job.c`, layout in
  `vc4_resource.c`, tiling in `vc4_tiling*.c`), its `kernel/` copy of the kernel's render list
  and validation (MIT; older than Linux's: its render list clears late on the hardware, §2
  "Command lists"), `src/broadcom/cle/vc4_packet.xml`, `src/broadcom/drm-shim/vc4_noop.c`.
  `dev/fetch_external.sh` fetches them into `external/reference/mesa/` (with
  `include/drm-uapi/vc4_drm.h` and `docs/drivers/vc4.rst`; done 2026-11-04).
- Linux's `drivers/gpu/drm/vc4/` (binner memory, overflow, power, caches). **Licences differ per
  file:** `vc4_v3d.c` and `vc4_regs.h` are GPL-2.0-only, `vc4_gem.c`, `vc4_render_cl.c` and
  `vc4_irq.c` MIT. GPL files are read for behaviour, never copied (PD9). Copied code comes from
  Mesa.
- Raspberry Pi's userland, `host_applications/linux/apps/hello_pi/hello_fft/` (BSD-3, Broadcom):
  `mailbox.c` (mailbox through `/dev/vcio`, memory tags `0x3000c` to `0x3000f`, `0x30012`,
  `/dev/mem` mapping) and `gpu_fft_base.c` (memory flag `0x4` on the Pi 2 and 3, `BUS_TO_PHYS`)
  show the calls the Linux probe makes; read, not copied (PD9).
- Emu68 (MPL-2.0) is read for behaviour only; nothing of it goes into this repository's code
  (PD9).

## 3. Architecture

```
 Application (GLES2 + agl*), libogles2.a                                   (unchanged)
        |
 ogles2.library front end: API, objects, state, programs' tables,          (shared, MIT)
 jobs, display, program cache
        |  backend interface (§5.1), chosen at start
   +----+-------------------------------+
   v                                    v
 V3D 4.2 backend + v3dcore            VC4 backend + vc4core                (Hyperion | new, MIT)
   |                                    |
   v                                    v
 .v3dp, device 42                     .v3dp, device 21
        \                              /
         v3dglslc: Mesa's GL core, v3d and vc4 drivers                     (MIT; S1 on the PC,
                                                                            S2 on the Amiga)
```

- `vc4core/` is new code under the MIT licence, written from the reference guide and Mesa's vc4
  driver (MIT, its notices kept where code follows it). It is organised like `v3dcore`, but none of
  `v3dcore`'s or PiStorm3D's code is copied into it (PD9). Its parts: device (registers, mailbox,
  power, identity), memory (bus alias, 4 KB alignment, the binner's 256 MB segment), control-list
  buffers and packets (packers generated from `vc4_packet.xml` as Mesa does), the render list per
  tile (after `kernel/vc4_render_cl.c`), binner memory and overflow, submission and waits, the
  errata of §2, texture layout and tiling (T and LT, after `vc4_resource.c` and `vc4_tiling.c`),
  the GPU lock.
- The same `vc4core` sources build for **m68k (AmigaOS)** and for **aarch64 Linux** (§6), with a
  thin OS layer: map the registers, allocate GPU memory, talk to the mailbox, wait, log.
- **Internal programs** per backend: on the Pi 4 as now (`builtin_shaders.h`, PiStorm3D's
  hand-written V3D 4.2 QPU code; `copy_tex_v3dp.h`; `clear_mrt_v3dp.h`); on the Pi 3, VC4 builds
  of the same GLSL programs from `v3dglslc` (the built-in program, the copy, and the clear of
  several targets, one pass per target).

## 4. The compiler

### 4.1 Host compiler (S1): `v3dglslc` with both drivers
- **Mesa build:** `dev/host-setup/build_mesa.sh` gets `-Dgallium-drivers=v3d,vc4,llvmpipe` (now
  `v3d,llvmpipe`). The pinned commit has the vc4 driver, its `kernel/` copy and `vc4_noop.c`.
- **Build lists:** `tools/v3dglslc/replay.py` (`LIBS`, `PART1`, `PART2`) and `build_host.sh` add the
  vc4 driver's and winsys' Meson targets (`src/gallium/drivers/vc4/`,
  `src/gallium/winsys/vc4/drm/`, the target names read from `compile_commands.json`) to the v3d
  ones. The vc4 compiler is inside the gallium driver directory (`vc4_program.c`, `vc4_qir*.c`,
  `vc4_qpu*.c`, `vc4_register_allocate.c`), not in `src/broadcom/compiler`.
- **The fake devices:** `compat/fake_drm.c` (V3D 4.2) stays; a VC4 twin beside it, after
  `vc4_noop.c`, answers `GET_PARAM` (`V3D_IDENT0`, `V3D_IDENT1`, `SUPPORTS_BRANCHES`,
  `SUPPORTS_ETC1`, `SUPPORTS_THREADED_FS`, `SUPPORTS_FIXED_RCL_ORDER`, `SUPPORTS_MADVISE`,
  `SUPPORTS_PERFMON`), `CREATE_BO`, `CREATE_SHADER_BO` (vc4 keeps shader code in BOs of their
  own), `MMAP_BO`, `GET_TILING`, `LABEL_BO`. **Its answers decide the code:** without branches the
  screen reports no control flow (`max_control_flow_depth`) and loops must unroll; without
  threaded FS the fragment shaders are single-threaded. It answers what `vc4core` supports
  (branches and threaded FS: yes, as on Linux), and `IDENT0`/`IDENT1` as the Pi 3 reads them
  (P0).
- **One process, a screen per GPU:** the program creates the screen and context of the GPU a
  request names when it first needs it (`v3d_screen_create` or `vc4_screen_create`, each on its
  fake device); a machine only ever asks for its own.
- **Patches:** the v3d ones (0001 to 0014) stay as they are. New, numbered on from 0015:
  - done: 0015 (the hook, below) and 0016 (the fragment key's `draw_buffer` for
    `EXT_draw_buffers`, §2.1);
  - the variant hook (twin of 0009) in `vc4_get_compiled_shader` (`vc4_program.c`), called before
    `vc4_bo_alloc_shader` with stage, key, prog data and QPU code; the driver's in-memory cache
    (`fs_cache`, `vs_cache`) bypassed while it is set (vc4 has no disk cache);
  - twins of 0012 (point size clamped to the range reported), 0013 (`EXT_clip_control`'s
    `ZERO_TO_ONE`) and 0014 (alpha-to-coverage variants), where vc4 needs them (P4: each checked);
  - the emulations' key bits (§2.1): half-float textures (two reads), mirror-clamp, LOD bias,
    LOD clamp, anisotropic lookups, output index (multiple render targets), packed half-float
    colour;
  - big-endian fixes found by the S1/S2 byte comparison (`compare_v3dp.sh`), as 0008 was for v3d.
- **The Pi 4's output does not change:** with both drivers linked, every V3D 4.2 `.v3dp` of the
  corpus and of the release's examples is byte for byte the one 1.3.3's compiler writes (apart from
  the compiler id, `compiler.c`, which makes caches compile again).
- Output: the `.v3dp` container with device version 42 or 21 (PD2), §4.3.
- **Done (2026-11-04), S1's first steps:**
  - the WSL Mesa build reconfigured with `-Dgallium-drivers=v3d,vc4,llvmpipe` (and
    `build_mesa.sh`); every file now gets `-DHAVE_VC4`, which `replay.py` drops, so the m68k
    build's commands are unchanged;
  - `tools/v3dglslc/test/v42_unchanged.sh save|check`: the Pi 4's output of every shader pair at
    hand (the test shaders, the corpus, the release's examples, Boing 2026: 54 files) against a
    baseline taken before; after the reconfiguration, with vc4 linked and with patch 0015:
    **54 of 54 identical**;
  - `compat/fake_drm.c` serves both GPUs (a second descriptor for VC4: the Pi 3's `IDENT0..2`,
    branches, ETC1, threaded fragment shaders and fixed render order yes, madvise and perfmon
    no; `CREATE_SHADER_BO` copies the code in as the kernel does; `drmGetCap` answers "no sync
    objects", which the m68k build needed);
  - `v3dglslc -g 21 [-b attr0,attr1,...]` links Mesa's vc4 driver (`replay.py` `VC4`, host build
    only until S2) and draws each program once in the reference state of `tests/pi3/ref/`
    (`run_pairs_vc4`); **its QPU code is identical to what the Pi's own Mesa (26.2.2) compiles
    for the triangle and the textured quad, all six stages** (`tests/pi3/ref/shaders/`,
    `test/compare_vc4_qpu.py`);
  - patch `0015-vc4-compiled-variant-hook.patch` (the twin of 0009) and `vc4_variants.c`, which
    prints each variant: stage, threading, the fragment inputs, the attribute layout, the
    uniform recipe and the code. It confirms the recipes `tests/pi3_probe` was built with by
    hand. Until the format (§4.3) is settled, this listing is what `tests/pi3` takes its shaders
    from;
  - the VC4 path is compiled into the host build only (`-DV3DGLSLC_VC4`, `build_host.sh`); the
    m68k build still compiles and links as it was (checked in a separate output directory:
    0 undefined symbols; the release's `v3dglslc.stripped` was not rebuilt).

### 4.2 Keys and variants (the main risk)
- **Mesa vc4's keys** (`vc4_qir.h`, filled in `vc4_update_compiled_fs`/`_vs` and
  `vc4_setup_shared_key`):
  - every stage, per sampler: `format`, `swizzle[4]`, and for single-sampled textures
    `compare_mode`, `compare_func`, `wrap_s`, `wrap_t`, `force_first_level`; for multisampled ones
    `msaa_width`, `msaa_height`;
  - vertex and coordinate: `fs_inputs` (the fragment variant's inputs), `attr_formats[8]`,
    `is_coord`, `per_vertex_point_size`;
  - fragment: `color_format`, `depth_enabled`, `stencil_enabled`, `stencil_twoside`,
    `stencil_full_writemasks`, `is_points`, `is_lines`, `point_coord_upper_left`,
    `point_sprite_mask`, `msaa`, `sample_coverage`, `sample_alpha_to_coverage`,
    `sample_alpha_to_one`, `logicop_func`, `ubo_1_size`, `ucp_enables`, and `blend` (the whole
    `pipe_rt_blend_state`).
- **Canonical keys (runtime, no Mesa change).** The runtime builds the key, so it fills only what
  changes the code: wrap modes only as "clamp/border or not" (only `PIPE_TEX_WRAP_CLAMP` and
  `CLAMP_TO_BORDER` change code, `vc4_program.c`: GLES2 has neither); `compare_func` only with
  `compare_mode`; per sampler the composed swizzle (format swizzle × sampler swizzle, which is all
  that `nir_lower_tex` receives) with a representative format, plus "depth" and "sRGB"; blend
  factors and equations only when blending is on; `logicop_func` always `COPY`; `ucp_enables`,
  `ubo_1_size` 0. This removes most texture-format and wrap-mode variants; the blend state, the
  colour mask, the target format, the attribute formats and the depth/stencil enables remain.
- **Measured first, on the Pi 4 (P2):** a probe of the library (`ENV:OGLES2/Probe`) logs per program
  every distinct canonical VC4 key its draws would use. Quake III's timedemo, gl4es gears, the
  examples, SDL's `testgles2` and dEQP's shader groups give the numbers: variants per program and
  per run, and how many appear after the first frames (each is a stall while S2 compiles, or a
  wrong draw without it).
- **Measured (2026-11-04), probe V** (`ogles2/vc4keys.c`: per draw the canonical keys of the
  list above; a sampler's format class and swizzle are taken from the Pi 4's stored texture type
  and swizzle, which may count a few formats apart that VC4 shares; `tests/a1200/vc4keys.py`
  summarises the log; packages `p2v-*` made by `tests/a1200/p2_vc4keys.sh` from the 1.3.3
  packages with this repository's library; results `tests/a1200/results/p2v-*-20261104-*`):

  | Run | Programs | Fragment keys | Vertex keys | Most per program | New after frame 100 |
  |---|---|---|---|---|---|
  | corpus (es2gears, 10 book samples, 3 of ours; `m2-5`) | 22 | 32 | 23 | 3 | 0 |
  | SDL2's tests (`m6-3`) | 8 | 13 | 10 | 3 | 1 |
  | SDL3's tests (`m6-4`) | 10 | 12 | 12 | 2 | 0 |
  | Quake III, renderergl2, the whole demo (`m6-5`) | 18 | 39 | 25 | 6 | 2 (frames 100-489) |
  | Quake III on gl4es, the whole demo (`m6-2`) | 5 | 17 | 5 | 9 | 1 (frame ~470) |

  What makes the keys differ: in Quake III blending (on/off and its factors, by shader stage)
  and the sampler's format (RGB against RGBA textures), in a few programs the attribute formats;
  in the corpus and SDL the render target (the display against an RGBA8 texture: the
  default framebuffer's format, BGRA on the RTG screen, against RGBA8888 is a key difference on
  VC4) and points against triangles. Quake III's loading takes about the first 35 frames; nearly
  all keys appear by frame 100, i.e. in the first second of play. dEQP's shader groups were not
  measured (one draw state per program: one or two keys each).
  **Conclusion for PD7:** the counts are small (a game needs about 40 to 60 fragment and vertex
  variants, at most 9 for one program, 1 or 2 first needed in play), so option 1 (accept: the
  offline compiler's key manifests list the common states, the rest compile into the cache
  once) is enough; options 2 and 3 are not needed for these programs. Decided so (PD7,
  2026-11-04).
- **Then decided (PD7), with the numbers:**
  1. accept them: the offline compiler's key manifests (DEVELOPMENT_PLAN §8.2) list the common
     states; the rest compile at draw time into the cache (a stall once per installation);
  2. reduce them in the compiler: patch vc4 so that cheap dimensions come from uniforms instead of
     the key (blend factors and equations, colour mask, attribute normalisation), at a few
     instructions per fragment or vertex;
  3. splice: compile the fragment shader once up to its colour output and append a blending tail
     per blend state at run time (QPU code patching; hardest, last resort).

### 4.3 The format
- `v3dp/FORMAT.md` 1.0 is frozen for V3D 4.2, and its reader rejects a device version other than
  42. VC4 needs its own key, prog-data and recipe sections (vc4's `QUNIFORM_*` enum differs from
  v3d's). Proposal: format 1.1, which adds VC4 section types and device 21 and leaves the shared
  sections (`PROGRAM`, `STRINGS`, `STAGE_PARAMS`, `UNIFORMS`, `ATTRIBUTES`) and every V3D 4.2 file
  unchanged; the loader takes the device of the backend in use. Settled in P4 after
  `vc4_program.c` and `vc4_uniforms.c` are read for the prog data the draw needs.
- **Settled (2026-11-04): format 1.1** (`v3dp/FORMAT.md`, "Version 1.1"): the same container;
  header minor 1 and device 21; one new section type, 7 `VC4_VARIANT` (stage 0 vertex,
  1 coordinate, 4 fragment; vc4's key field by field, the sampler entries as their bitfields'
  values, the MSAA view of that union left out since GLES2 samples no multisampled textures;
  the vertex key's `fs_inputs` by content; prog data: threading, inputs, VPM attribute offsets,
  colour inputs, early-Z, texture lookups, the fragment inputs; vc4's recipe enum; the code). The
  shared sections as in 1.0: vc4 does not pack uniforms either, and `QUNIFORM_UNIFORM`'s data is
  the constant-buffer index as with v3d. A V3D 4.2 file is still written as 1.0, byte for byte
  (54 of 54 unchanged). Written by `v3dp_writer.c` (`v3dp_set_device`, `v3dp_add_variant`) from
  `tools/v3dglslc/vc4_variants.c`'s serializer: `v3dglslc -g 21 -o DIR` writes
  `<vs>-<fs>.vc4p`; `v3dp_dump.py`, extended from the document, decodes them, and the bytes are
  the same on a second run. Two notes for the runtime: `UBO0_ADDR` (indirectly indexed uniform
  arrays) reads the stage's constant buffer from GPU memory through the TMU, so `vc4core` keeps
  a GPU copy of it; the vertex variant is chosen by the fragment variant's inputs.
- **The corpus for VC4 (2026-11-04, `tools/v3dglslc/test/vc4_corpus.sh`):** of the 36 shader
  pairs at hand (`v42_unchanged.sh`'s), 35 compile and are written. `OES_standard_derivatives`
  is enabled for VC4 in `v3dglslc` (§2.1: compiled, never reported by the driver); without it
  Boing 2026's `ball` and `room` (`fwidth`) failed; `ball`'s fragment shader is 944 instructions
  on VC4. `mrt.frag` (`gl_FragData[0..3]`: the driver has one draw buffer) failed until patch
  0016 (§2.1): now **all 36 compile**, `mrt` with a fragment variant per draw buffer (13
  instructions each). No register-allocation failure in the set.
- The Pi 4's extra variant files (`.v3dw`: 32-bit samplers, `.v3da`: alpha to coverage) stay; on
  VC4 the extra files are whatever PD7 decides.

### 4.4 The draw path's uniform recipes (VC4)
`vc4_uniforms.c` (`vc4_write_uniforms`) is the reference; the types (`enum quniform_contents`,
`vc4_qir.h`): `CONSTANT`, `UNIFORM`, `VIEWPORT_X_SCALE`, `VIEWPORT_Y_SCALE`, `VIEWPORT_Z_OFFSET`,
`VIEWPORT_Z_SCALE`, `TEXTURE_CONFIG_P0`, `P1`, `P2`, `TEXTURE_FIRST_LEVEL`, `TEXTURE_MSAA_ADDR`,
`UBO0_ADDR`, `UBO1_ADDR`, `TEXRECT_SCALE_X`, `TEXRECT_SCALE_Y`, `TEXTURE_BORDER_COLOR`,
`BLEND_CONST_COLOR_X/Y/Z/W`, `BLEND_CONST_COLOR_RGBA`, `BLEND_CONST_COLOR_AAAA`, `STENCIL`,
`SAMPLE_MASK`, `UNIFORMS_ADDRESS` (filled by the kernel on Linux: by `vc4core` here). The
emulations add their own (LOD bias, LOD clamp, clip z).

### 4.5 On-target compiler (S2) and the program cache
- **One `LIBS:v3dglslc`** (PD3), the 68k build of S1, started by the library as now
  (`v3dglslc SERVER`, port `"v3dglsl compiler"`). The request (`v3dp/v3dglsl_port.h`) gets a device
  field: request version 2 (`V3DGLSL_VERSION`), the field after `bindings`. The compiler takes
  version 1 as device 42, so an older `ogles2.library` (one a program ships in `PROGDIR:libs/`)
  still works with it. A 1.3.x compiler answers 3 ("bad request") to version 2: the library then
  sends version 1 on the Pi 4, and on the Pi 3 goes on as without a compiler.
- **The cache is shared by both cards.** The Amiga's disk stays when the Pi 3 card replaces the
  Pi 4 card, and so do `OGLES2Cache:` and an application's `PROGDIR:shaders/`. Today's file name is
  the source hash, `<24 hex digits>.v3dp` (29 characters; FFS allows 30). VC4 files are named
  `<24 hex digits>.vc4p`, their extra variant files likewise (`.vc4w`, ...), so the Pi 4's names
  and files stay as they are. The `.v3ds` shader records (compile status and info log from the
  GLSL front end, which both share) stay shared.
- Measured on the A1200 with the Pi 3: start-up, per-program and per-variant time (the Pi 4: about
  1 s and 0.2 s, DEVELOPMENT_PLAN §8.4), and memory (the m68k compiler is about 19 MB with the v3d
  driver; the vc4 driver's share is measured in P4; a 3A+ has 512 MB in all). On the Pi 4, start-up
  time and memory are checked against 1.3.3's.

## 5. The library

### 5.1 The backend interface
- The front end calls V3D code directly today: 519 lines in 11 files match
  `grep -c -E "V3D_|v3d_|cl_emit|QPU" ogles2/*.c` (2026-11-03): `job.c` 186, `texture.c` 125,
  `draw.c` 122, `device.c` 31, `fbo.c` 27, `v3dp.c` 9, `context.c` 6, `buffer.c` 5, `share.c` 4,
  `state.c` 3, `program.c` 1; and 15 in `ogles2_internal.h`.
- **P2 moves them behind a table of backend functions:** device open/close and information
  (limits, extension string, `GL_RENDERER`, formats); memory (the bus address of a block); job
  begin/submit/wait; clear; draw (state, shader state, vertices, indices); texture
  alloc/store/load/tiling; framebuffer targets; the TFU and blit paths (absent on VC4: the caller
  takes its CPU path); the program variant choice and the uniform recipe writer (keys and recipes
  are backend-specific); the internal programs (§3); the probes' letters.
- **Layout (proposal, settled in P2):** the shared front end stays in `ogles2/` (MIT); the
  interface in `ogles2/backend.h`; the V3D 4.2 backend's code, moved out of `job.c`, `draw.c`,
  `texture.c`, `fbo.c` and `device.c`, in `ogles2/v3d/` (Hyperion where it adapts PiStorm3D, PD9);
  the VC4 backend in `ogles2/vc4/` (MIT); `v3dcore/` and `vc4core/` beside them.
- **Choice at start** (`ogles2_device_open`, at the first context, as the device is opened now):
  `/v3dbus` with a `brcm,2711-v3d` node takes V3D 4.2; `/soc/v3d@7ec00000` (`brcm,vc4-v3d` or
  `brcm,bcm2835-v3d`, whatever its `status`: `disabled` without the KMS overlay, §2) takes VC4;
  then `IDENT0` must match (read after power-on: before it, the Pi 3's registers read
  `0xdeadbeef`). Neither: the context fails, as on a
  machine without a GPU today. Emu68 hands AmigaOS the firmware's device tree (§7) (verify P5).
- **The coupling, surveyed (2026-11-04):** the V3D-specific code is in `job.c` (render lists,
  submission: `build_render`, `build_render_fbo`, `grow_binning`, `zs_get`, the V3D half of
  `ogles2_job_begin`/`_flush`/`after_render`/`take_gpu`), `draw.c` (`emit_state` with its
  dedup, `shader_record`, `draw_packets`, `write_uniforms`, `code_address`, `spill_area`,
  `default_attribs`, the attribute encoding), the storage half of `texture.c` (`storage_of`,
  packing, `store_box`/`load_box`, `upload_all`'s allocation, the TMU records of
  `ogles2_texture_state`, the TFU paths), `fbo.c` (`rb_zs_storage`, `rb_surface`, `pools`, the
  tile-size part of `ogles2_fbo_target`), `device.c` (all) and `v3dp.c` (the VARIANT section,
  the device check, the code upload, `ogles2_image_variant`); `program.c` holds the built-in
  image (PiStorm3D's shaders), `draw.c` the internal programs `clear_mrt_v3dp.h` and
  `copy_tex_v3dp.h`. Elsewhere `ctx->v3d` is mostly a "has a GPU" test (frame.c, fbo.c, share.c,
  copy.c, state.c, context.c), plus `frame_active` reads and deferred frees on the context's
  frame. **Shared state to retire:** `g_v3d_cl_generation` (a cache key in draw.c, texture.c and
  `OGLES2Program.us_gen`), the `g_v3d_requested_*` handshake before `v3d_context_init`,
  `g_v3d_uif_fast`; and **a block's CPU address is its GPU address** in about 20 places
  (draw.c, buffer.c, job.c, fbo.c, texture.c), true under Emu68 on the Pi 4, not on the Pi 3
  (the `0xC0000000` alias). Hard-coded V3D limits: `state.c` (lines, points, uniform vectors,
  16 varyings, samples), `texture.c` (size 4096, anisotropy, LOD bias), `OGLES2_MAX_VIEWPORT`,
  the extension string (`context.c`).
- **The interface (settled 2026-11-04):** `ogles2/backend.h`, one table
  (`const struct OGLES2Backend *ogles2_backend`, chosen at device open) with opaque per-context,
  per-texture and per-variant objects; groups in the order the steps below take them:
  1. **device and capabilities:** open, reset, renderer, bytes in use, the GPU lock; a
     capabilities struct the front end reads instead of its constants (limits, extension
     string, formats, MSAA, TFU present);
  2. **memory:** blocks with a CPU pointer *and* a bus address (`struct v3d_mem`'s `hostptr`
     and `busaddr`), alloc/free through the backend; the front end keeps CPU addresses and a
     backend turns them into bus addresses where it writes its own GPU records; the front end
     owns the job generation (`ogles2_job_gen`), the cache key that was `g_v3d_cl_generation`
     (process-global and never reset, not per context: a re-created context would repeat keys);
  3. **jobs:** begin, clear snapshot, flush (lists and submission), wait, discard; the front end
     keeps the serials, the overlap and discard policy, the statistics; "can the tile buffer
     clear this" becomes a backend query;
  4. **draw:** one call with the resolved variants, vertex sources, indices and the state
     version; `emit_state` and the rest move behind it;
  5. **textures:** storage format, packing, allocation and tiling, load/store, sampler records;
     the front end keeps completeness, the fallback texture and the CPU copies;
  6. **framebuffer targets** (surfaces, tile pools) and the optional **TFU/blit** paths;
  7. **programs:** the VARIANT section's parsing and the code upload, variant choice, the
     uniform writer, the internal programs, the extra variant files (`.v3dw`, `.v3da`); a
     variant becomes a few shared fields plus the backend's own data.
  Each step moves code without changing it, builds, and runs the Pi 4 checks below the same
  day against a baseline taken that day: m5-1 (255 PASS), m7-6 (36 of 36), m2-5's 14 pictures
  byte for byte, the speed of m6-1 (gears) and Quake III's timedemo. Then, step 8, the MIT
  rewrite of the shared parts that still adapt PiStorm3D (§5.3: `display.c`'s RTG paths and
  what remains of `job.c`, `draw.c`, `texture.c`, `program.c` in the front end), checked the
  same way; then the acceptance below. **Baseline of 2026-11-04** (Pi 4 card, this
  repository's library before the split): m5-1 255 PASS and no FAIL, m7-6 36 of 36, m2-5 14 of
  14 pictures identical to 2026-10-14's, m6-1 window 469 fps, screen 1114, bitmap arrays 821
  (TEETH=1), Quake III renderergl2's whole demo 89.0 and 88.9 fps
  (`tests/a1200/results/*-20261104-1502*` to `-1505*`). `tests/a1200/p2_checks.sh` builds and
  runs the checks and prints what to compare.
- **Step 1 done (2026-11-04):** `ogles2/backend.h` (the table, `struct OGLES2Caps`) and
  `ogles2/v3d/backend_v3d.c` (the V3D 4.2 table with the values 1.3.3 hard-coded); the front
  end reads the extension string, `GL_RENDERER`, texture and viewport sizes, draw buffers,
  varyings, uniform vectors, line and point limits, anisotropy and LOD bias from
  `ogles2_backend->caps` (`context.c`, `state.c`, `texture.c`, `fbo.c`, `frame.c`); the table
  starts as V3D's, so a context without a GPU reports the same. Pi 4 checks the same day: m5-1
  255 PASS, no FAIL; m7-6 36 of 36; m2-5 14 of 14 identical; m6-1 window 467, screen 1115,
  bitmap arrays 829 fps; Quake III 88.8 fps: as the baseline.
- **Step 2 done (2026-11-04):** GPU memory through the table (`ogles2_mem_alloc`/`_free`,
  `backend.h`; V3D's are `v3d_mem_alloc`/`_free` on AllocVec'd fast RAM, all 28 calls of the
  front end); `ogles2_job_gen` (`job.c`) in place of v3dcore's `g_v3d_cl_generation` in
  `draw.c`, `texture.c` and `job.c`. The address rule settles the survey's "about 20 places":
  all of them write V3D records (shader records, uniforms, TMU records, tile lists, submission)
  and move with steps 3 to 7 into the V3D backend, where the bus address is the CPU address; a
  front end holding bus addresses would mix them with CPU ones (`draw.c`'s attribute addresses
  come from a buffer's GPU copy or from the control list's own memory, the display's bitmap is
  read by the CPU too). Pi 4 checks the same day: m5-1 255 PASS, no FAIL; m7-6 36 of 36;
  m2-5 14 of 14 identical; m6-1 window 466, screen 1102, bitmap arrays 818 fps; Quake III
  89.3 fps: as the baseline (gears within 1 %, its run-to-run spread; no draw path changed).
- **Step 3 done (2026-11-04):** jobs. `ogles2/v3d/job_v3d.c` (Hyperion, it adapts PiStorm3D)
  has the V3D half of `job.c`: the binning list and its growth, the render lists of the
  default framebuffer and of an object, depth and stencil kept across a mid-frame flush,
  submission, the waits, the retire list; twelve entries in the table (`job_begin` …
  `job_free`, `backend.h`) and the wait results `OGLES2_WAIT_*`. `job.c` keeps the serials,
  the clear snapshot (`ctx->job_clear_*`, the colour as GL gave it: the backend converts it),
  `ctx->job_open` (it replaces `v3d->frame_active` in `fbo.c`, `share.c`, `v3dp.c`), the
  locks, the overlap with the CPU, the statistics and the drop reasons. Still V3D's in
  `job.c`: the GPU lock and the reset (`v3d_gpu_lock`, `ogles2_device_reset`; the device
  group). Differences only under probes: probe S's render may be left running like any
  other; the flush's "cache before" time no longer holds the GPU's cache invalidation (in the
  binning time now). The frame.c test "can the tile buffer clear this" stays in the front
  end: VC4's tile buffer also clears depth and stencil together. Pi 4 checks the same day:
  m5-1 255 PASS, no FAIL; m7-6 36 of 36; m2-5 14 of 14 identical; speed below.
- **Step 4 done (2026-11-04):** draw. `ogles2/v3d/draw_v3d.c` (Hyperion) has what a draw puts
  into the job: the uniform streams, texture and sampler records, UBO copies, the spill area,
  vertex copies and attribute records, the shader record, the state packets with their dedup,
  the draw packets; one table entry `draw` takes a `struct OGLES2DrawCall` (the program, its
  three variants and their code addresses, mode, the vertex range, the indices as
  `struct OGLES2Elements`), and `draw_free`. The backend's per-context draw state is
  `ctx->be_draw`. `draw.c` keeps the GL entry points and their checks, the index range scan,
  the variant choice (with `wants_wide`), the program's state variables, the vertex input
  slots, probe V, the clears drawn as rectangles and the copies on the GPU (their internal
  programs move with step 7). Helpers both use are inline in `ogles2/draw_private.h`.
  Probe P's stage 1 now holds the slot mapping, and probe V counts a draw after it is made
  (it reads the same textures' storage). Pi 4 checks the same day: m5-1 255 PASS, no FAIL;
  m7-6 36 of 36; m2-5 14 of 14 identical; speed against step 3 (`tests/a1200/p2_speed.sh`):
  Quake III 89.4, 89.7, 88.7 fps against 89.4, 89.5, 88.4; gears bitmap arrays 823, 778,
  807 against 826, 807, 808, screen 1083, 1084, 1078 against 1106, 1084, 1098 (the same
  step 3 library ran bitmap arrays at 772 to 779 in the afternoon and 807 to 826 in the
  evening: the station's drift is larger than any step's cost so far).
- **Measuring speed between steps (learnt at step 3):** runs of one library minutes apart are
  not enough. Quake III's timedemo drifts by about 1.5 % within an hour (step 1's library:
  87.9 to 89.3 fps the same afternoon), and where the code lies moves gears: step 3 with 64
  or 160 bytes of unused code added early in the library ran the screen mode between 1077
  and 1097 fps, bitmap arrays between 769 and 779. So each step is compared with the last
  commit's library built in a tree of its own (`git archive HEAD`, its own `B`), the two
  alternating on the station, three runs each (`tests/a1200/p2_speed.sh ref`, then
  `tests/a1200/p2_speed.sh`). Step 3 against step 1 that way: Quake III
  89.7, 89.1, 89.1 against 88.8, 88.2, 88.2 fps; gears bitmap arrays 772 to 780 against 781
  to 789, screen 1070 to 1103 against 1089 to 1118. Gears, at about 1.3 ms a frame and one
  job each, is about 1 % (some 20 us a frame) slower in every pair, the padded builds too;
  Quake III is not. Step 2 alone sat between them (bitmap arrays 778 to 780); two variants of
  it (memory through the table only; the job generation moved only) did not separate in
  Quake III. To watch at the next steps (the draw step puts the table into the per-draw
  path): if gears keeps losing, a direct call when the backend is V3D's is the remedy. The
  acceptance (below) compares the whole of P2 against 1.3.3 the same way.
- **Acceptance of P2:** the Pi 4 build behaves exactly as 1.3.3: the A1200 regression packages,
  dEQP-GLES2's must-pass list and the benchmarks (gears, Quake III) give the same results and the
  same speed (the 1.3.3 checks of DEVELOPMENT_PLAN §13 are the list). The function table costs
  one indirect call per backend call on the 68k; the benchmarks show whether that matters.
- **Program binaries:** `OES_get_program_binary`'s binary is the `.v3dp` file
  (`GL_PROGRAM_BINARY_V3DP_PISTORM`, `program.c`). The token stays one for both GPUs: a binary for
  the other GPU fails to load (device field), which GLES2 allows, and the program then compiles
  from source.

### 5.2 Names
- Unchanged: `ogles2.library`, the import library and its search (`client/ogles2_open.c`), the
  headers, `T:ogles2.log`, `T:ogles2_missing_shaders.txt`, `OGLES2Cache:`, `ENV:OGLES2/LogFile`,
  `ENV:OGLES2/MSAA`, `ENV:OGLES2/WindowCopy`, `LIBS:v3dglslc` and its port, the installer and the
  archives. `ENV:OGLES2/Probe`'s letters are per backend (the V3D 4.2 ones as now).
- The GPU lock: `"V3D 4.2 submit"` on the Pi 4 as now (MiniGL's coexistence, DEVELOPMENT_PLAN
  §7.1), `"VC4 submit"` on the Pi 3 (PD5).
- The library's version: a new revision of version 1 (the API and the dispatch table only grow);
  the number is set at the release (P7).

### 5.3 Licence (PD9)
Checked 2026-11-03 against `LICENSE`, the file headers and the sources the compiler is built from.
- **Today.** `ogles2.library` as built is under the Hyperion MiniGL Open Source License (AmigaOS
  only; modifications published), decision D6, `LICENSE` section 1. The reasons are `v3dcore/`
  (PiStorm3D's backend: no notices in its files, licensed through `v3dcore/Licence.txt`, apart
  from the MIT parts by Macoy Madson and from Mesa); the front-end files that adapt PiStorm3D code,
  `ogles2/job.c`, `display.c`, `draw.c`, `program.c`, `texture.c` and `builtin_shaders.h`; and
  `tools/builtin_shaders/`. Everything else is MIT.
- **Decided (the developer, 2026-11-03):** `v3dcore` stays under Hyperion's licence; the rest is
  MIT. The line follows the backend line:
  - **Hyperion:** `v3dcore/`; the V3D 4.2 backend (`ogles2/v3d/`, §5.1), to which the parts of
    `job.c`, `draw.c` and `texture.c` that adapt PiStorm3D's V3D code move in P2;
    `builtin_shaders.h` and `tools/builtin_shaders/` (PiStorm3D's shaders, Pi 4 only);
  - **MIT:** the shared front end. The parts of it that still adapt PiStorm3D code after the move
    (display.c's RTG paths, what of `program.c`, `job.c`, `draw.c`, `texture.c` is not V3D-specific;
    the comments naming PiStorm3D mark them) are rewritten without its code, in P2, so the Pi 4
    regressions check the rewrite. Also MIT: `vc4core/`, the VC4 backend, the compiler, the import
    library and headers;
  - **the built `ogles2.library` contains `v3dcore`, so as a whole it stays under Hyperion's
    terms, as today;** its MIT files stay MIT on their own (`LICENSE` section 1 already says so).
    `LICENSE` lists the files anew after P2.
- **Third-party code: permissive, notices shipped, no copyleft:**
  - in the library: Mesa-derived code (MIT; Mesa's notices in those files); BLAKE3 (CC0 or
    Apache-2.0, used under CC0); the Khronos headers `gl2.h`, `gl2ext.h`, `khrplatform.h` (MIT).
    `gl2platform.h` is Apache-2.0, but Khronos meant that header to be the implementation's own,
    so it is replaced by an MIT one; clib2 (BSD-3, Olaf Barthel); libgcc (GPL-3 with the GCC
    Runtime Library Exception: no obligation for the binary);
  - in the compiler (909 Mesa source files scanned, the v3d build; the vc4 driver's files are
    scanned in P4): 874 MIT; 23 generated with no notice (by Mesa's Python scripts, MIT, and flex);
    three Bison parsers (GPL-3 with Bison's exception for generated parsers: no obligation);
    `softfloat.c` (BSD-3, Berkeley SoftFloat: its notice ships with the binary);
    `threads_posix.c`, `time.c` (BSL-1.0); `crc32.c` (zlib); two public-domain algorithms. The
    runtime: libnix (public domain), libstdc++ and libgcc (the GCC Runtime Library Exception);
  - the release ships a notices file with Mesa's MIT notice, SoftFloat's and clib2's BSD
    notices, the V3D third-party notices (`licences/License_V3D_ThirdParty.txt`) and the others.
- **Not part of the library or compiler:** the ports keep their own licences (Quake III GPL-2,
  glmark2 GPL-3, dEQP Apache-2.0, SDL zlib, gl4es MIT; `LICENSE` section 4). They are separate
  programs that open the library at run time; their binaries go out only on their own terms.
- **Rules from here on:** nothing is copied from GPL, MPL or BSD-licensed code into the library or
  the compiler, nor from PiStorm3D into MIT files; each new third-party source is checked before it
  is used (the scan above is repeated whenever the pinned Mesa commit changes).

## 6. The Linux test bench (Pi 3, Raspberry Pi OS)

Everything that can run here runs here first (the developer, 2026-11-03).

0. **Set-up.** Raspberry Pi OS Lite, 64-bit, current release; ssh with a key from WSL. Programs for
   the Pi are cross-compiled in WSL with Ubuntu's `aarch64-linux-gnu` GCC (installed already for
   Emu68, DEVELOPMENT_PLAN §2.4) and copied over; the Pi runs and debugs them (`gdb`). Two boot
   set-ups in `config.txt`, switched by a script and a reboot of the Pi (not the Amiga):
   - **reference:** `dtoverlay=vc4-kms-v3d` (it enables the V3D node; Mesa's vc4 works);
   - **bare:** the overlay commented out (the V3D node stays `disabled`, no kernel driver owns the
     3D core), `gpu_mem=128` for the firmware's allocator.

   **The test Pi (2026-11-04):** a Raspberry Pi 3 Model B Rev 1.2 (revision `a02082`, BCM2837,
   1 GB) at `192.168.137.191`, user `pi`; Raspberry Pi OS 64-bit with the desktop (Debian 13
   "trixie", labwc), kernel `6.18.50+rpt-rpi-v8`, firmware `a8698392` (2026-09-14), Mesa
   `26.2.2-1~bpo13+0~rpt1`. From WSL `ssh pi3` (`~/.ssh/config`, key `~/.ssh/id_ed25519`); `sudo`
   asks for the password (the scripts take it in `PI_PASS`; it is not written in this
   repository). Installed for the tests: `mesa-utils`, `kmscube`, `libegl-dev`, `libgles-dev`;
   the work lives in `~/pi3/` on the Pi. `tests/pi3/boot.sh reference|bare|status` switches the
   boot set-up (two lines of `config.txt`, the original kept as `config.txt.ogles2pi3-orig`) and
   reboots the Pi; the Pi is left in the reference boot, as it was. **Power (2026-11-04):** with
   the first supplies `vcgencmd get_throttled` read `0x50000`, then `0x50005` (under-voltage and
   throttling now, the ARM held at 600 MHz) a minute after boot, idle too, and the Pi rebooted
   once in the middle of dEQP (the run went on, §6.1); a short thick cable and a supply soldered
   to the board did not help. **Solved by another supply, 5.2 V at the board** (the developer's
   meter): `0x0` and 1200 MHz, also after a minute of all four cores busy with the GPU drawing
   (61 to 74 °C; the Pi 3 throttles at 80 °C, so a heatsink before long speed runs). Mesa's
   pictures had stayed exact through the low voltage (400 runs). Results taken before this
   (dEQP's baseline, the probe's timings) ran at the low voltage; their pass/fail results stand,
   their times are not speed figures.
1. **References from Mesa** (reference boot):
   - Mesa vc4's extension string and limits (`eglinfo`, `glGetIntegerv`), `IDENT0..2` (§6.2's
     probe can read them in the bare boot);
   - dEQP-GLES2's must-pass list with Mesa vc4, cross-built in WSL (VK-GL-CTS as `ports/deqp`
     uses, built for Linux): the hardware's own baseline, which P6 is compared against;
   - the test programs and examples built for Linux against the Pi's Mesa: their pictures;
   - shaders and command lists with `VC4_DEBUG=cl,qpu,qir,nir,shaderdb` (the flags of
     `vc4_screen.c`; `dump` exists only in simulator builds). Mesa at the pinned commit,
     cross-built for aarch64 and run with `LD_LIBRARY_PATH`, gives the same QPU code as our
     compiler; the Pi's own Mesa may differ. Mesa's binning lists carry relocations (filled in by
     the kernel) and the render list is the kernel's, so the references are shaders, list contents
     and pictures, not lists ready to run.

   **Done (2026-11-04):**
   - the string and limits: `tests/pi3/ref/mesa_limits.c` (surfaceless EGL, built on the Pi),
     results in §2 ("Limits") and §2.1;
   - one reference picture and its dump: `tests/pi3/ref/mesa_triangle.c` (a triangle in a
     128×128 renderbuffer, `VC4_DEBUG=qpu,cl,qir,shaderdb`): the QPU code as hex, uniforms named
     in the QIR, the binning list decoded; the probe (§6.2) uses them;
   - **the dEQP-GLES2 baseline:** `tests/pi3/deqp/build.sh` builds `deqp-gles2` in WSL from the
     sources `ports/deqp` uses, with the same lists of sources and a small platform of our own
     (`tcuPi3Platform.cpp`: an EGL pbuffer on Mesa's surfaceless platform, so the desktop is left
     alone), because the CTS's CMake wants SPIRV-Tools, glslang and Vulkan's layers present just
     to configure; `tests/pi3/deqp/run.sh` runs it on the Pi with the A1200 runs' options, in
     parts that resume after a reboot (`remaining.py`). The must-pass list (17,165 cases) took
     about 15 minutes: **16,906 pass, 234 not supported, 20 fail, 5 quality warnings**
     (`rasterization`). The failures: `texture.mipmap.2d.basic.*_non_square` 6,
     `texture.filtering.2d.nearest_mipmap_*_linear_{mirror,repeat}_rgba8888` 4,
     `texture.wrap.clamp_clamp_nearest_npot_etc1`, `clipping.line.wide_line_clip_viewport_{center,corner}`,
     `clipping.triangle_vertex.clip_three.clip_neg_x_and_pos_x_and_neg_x_neg_y_neg_z`,
     `depth_stencil_clear.depth_stencil_masked`, `fragment_ops.scissor.outside_render_line`,
     `multisample.depth` and `multisample.sample_coverage_invert` (in 4× MSAA),
     `uniform_api.random.3` and `.79`. Not supported: `fbo` 91, `shaders` 90, `texture` 41,
     `clip_control` 8, `debug_marker` 3, `multisampled_render_to_texture` 1. These are the
     hardware's (and Mesa's) baseline for P6: the Pi 3 backend is to fail no case beyond them,
     except documented deviations.
2. **The bare GPU from user space** (bare boot, root): the 3D core's registers mapped through
   `/dev/mem` (Raspberry Pi's arm64 kernel has `CONFIG_STRICT_DEVMEM` unset); power and GPU memory
   from the firmware mailbox (`/dev/vcio`, `CONFIG_BCM_VCIO=y`): `SET_DOMAIN_STATE` or
   `SET_ENABLE_QPU`, then `ALLOCATE_MEMORY`, `LOCK_MEMORY`, `UNLOCK_MEMORY`, `RELEASE_MEMORY`
   (`0x3000c` to `0x3000f`) with flag `0x4` (the `0xC0000000` alias), mapped through `/dev/mem` at
   the physical address (`BUS_TO_PHYS`), as `hello_fft` does. The firmware's allocator rather than a
   CMA buffer, whose physical address user space cannot learn without a driver. Registers are read
   only after power-on. This is the position `vc4core` is in under Emu68, with gdb and fast
   rebuilds.

   **Done (2026-11-04), `tests/pi3_probe/`** (`pi3_probe.c`, the lists and checks; `plat.h` with
   `plat_linux.c`, the platform, so that the m68k build swaps one file; `build.sh` builds it in
   WSL, statically linked). In the bare boot, as root, all pass: power with `SET_ENABLE_QPU`
   (`SET_DOMAIN_STATE` alone leaves the registers reading `0xdeadbeef`); `IDENT0..2`; a
   render-only clear of a 128×128 RGBA8888 raster frame (4 tiles; about 10 µs); **a triangle
   from Mesa's QPU code in our own binning and render lists, identical to Mesa's picture (0 of
   16,384 pixels differ)**, binning about 3 µs and rendering 25 µs; raster stores 128, 129 and
   130 pixels wide (§2, "Raster stores"). Found on the way: the clear values must come before
   `TILE_RENDERING_MODE_CONFIG` (§2, "Command lists"). The GPU memory's CPU mapping through
   `/dev/mem` is a device mapping (memory Linux does not own): aligned 32-bit accesses only, so
   the probe copies word by word (a `memcpy` or `memset` there may fault). A write past the end of
   that mapping lands in whatever the next pages map: one such probe bug rebooted the Pi; the
   probe now checks every access against the allocation.

   **Then (2026-11-04):** texturing, with Mesa's shaders for `tests/pi3/ref/mesa_texture.c` (a
   16×16 texture on a quad; its fragment shader is threaded, and TMU0 takes P0 and P1 from the
   uniform stream): LT gives Mesa's picture exactly, raster too at power-of-two widths (§2,
   "Textures"). Stress (`texrepeat`, the quad and the triangle 40 times each per run) found the
   lost tile of a core that was not reset, and with it the counted `SET_ENABLE_QPU` (§2,
   "Power"); the probe's platform layer now power-cycles the core into its reset state at open
   (`reset_core`), after which over 1,600 draws were all right with `ERRSTAT` clean. Tried and
   ruled out on the way: a texture-cache flush before the render job (Linux does it, the probe
   now too), a pause between binning and rendering, binner and renderer started together.
3. **`vc4core` on Linux with its own tests** (`tests/pi3/`, Linux programs calling `vc4core`
   directly, shaders from S1 in WSL): clears, each primitive type, attributes in every format,
   indices (8, 16, 32-bit with splitting), textures (each type, T and LT, NPOT, mipmaps by
   rendering, cube maps, ETC1), render to texture and renderbuffers, depth, stencil, blending,
   readback, MSAA, the errata cases, the raster store into a padded image, the emulations' GPU
   side (§2.1); pictures compared with Mesa's (exact where the hardware is deterministic,
   otherwise with dEQP's tolerance). GPU-only speed (fill rate, triangles, uploads) measured here.
   The Linux build is little-endian, so byte-order mistakes of the Amiga build stay hidden: the
   Amiga runs (§7) have the last word.
4. **Not on Linux: the front end** (PD4). It calls AmigaOS in 15 of its files (about 200 exec,
   DOS, graphics and RTG calls), 34 of the 38 test sources under `tests/*/` are Amiga programs,
   and S2 is an Amiga process; dEQP-GLES2 runs on the A1200 already (the must-pass list in about
   2.5 hours on the Pi 4, DEVELOPMENT_PLAN §13).

## 7. The A1200 with PiStorm, Pi 4 and Pi 3

- **The machine:** the test A1200's PiStorm32-lite takes a Pi 3 (the developer, 2026-11-03) as well
  as its Pi 4. Swapping the cards means rebooting the Amiga, and installing a library in its
  `LIBS:` changes its system files: both are asked each time (CLAUDE.md). Which Pi 3 model is used
  is recorded in P0 (512 MB on a 3A+, 1 GB on a 3B or 3B+).
- **Both cards, every release:** from P2 on, every change to the shared front end is checked on
  the Pi 4 card (the regression packages) and, from P5 on, on the Pi 3 card too. Tests are batched
  per card to keep the swaps few.
- **Emu68 on the Pi 3:** the same release as on the Pi 4 card (v1.1.0-beta.1), its `config.txt`;
  its memory map and device tree as seen from AmigaOS (`/memory@0`, `/soc` with `ranges` and
  `dma-ranges`, the V3D node, the mailbox alias, `/emu68`'s `vc4-mem`), the fast RAM left, and its
  RTG driver. Read with `Emu68DeviceTree` (PiStorm3D's tool) as in DEVELOPMENT_PLAN §13 (run 1),
  and from Emu68's source (`src/aarch64/start.c`, `src/raspi/start_rpi64.c`).
- **The probe again under AmigaOS** (`tests/pi3_probe/`'s m68k build): power-on, identity, a clear,
  a triangle; then the library.
- **GPU reachability:** `ogles2_device_gpu_reachable` (DEVELOPMENT_PLAN §7.3) checks the RAM
  ranges on both; `vc4core` turns a 68k address into a bus address through the alias.
- **The test station:** `tools/amictl` as now; the same `tests/a1200/` packages on both cards, run
  with `runpkg`.
- The Pi Zero 2 W (RP3A0, the same VC4) is used on PiStorm too: once the Pi 3 works it may run
  unchanged (verify, not planned).

## 8. Phases

| Phase | What | Done when |
|---|---|---|
| **P0** | The Pi 3 with Raspberry Pi OS reachable from WSL, cross tools, both boot set-ups; Mesa vc4's extension string, limits and dEQP-GLES2 baseline recorded; the (verify) facts of §2 checked on the Pi; `fetch_external.sh` with the vc4 sources | §2 and §2.1 carry results instead of marks; the baseline is in §10 |
| **P1** | The bare-GPU probe on Linux (`tests/pi3_probe/`): power-on (which mailbox call), `IDENT0..2`, one hand-built job (one tile cleared and stored), then a triangle with a Mesa-compiled shader in our own lists; raster sampling and the padded raster store tried | a picture read back matches Mesa's |
| **P2** | On the Pi 4, on the A1200: the backend interface (§5.1), the V3D code moved behind it, the shared front end's PiStorm3D parts rewritten as MIT (§5.3), the variant-count probe (§4.2). Can start at once, beside P0 and P1 | the Pi 4 regressions, dEQP-GLES2 and the benchmarks as 1.3.3; the variant counts recorded, PD7 decided |
| **P3** | `vc4core` on Linux, every feature of §6.3 including the emulations' GPU side | `tests/pi3/` all pass against Mesa's pictures |
| **P4** | `v3dglslc` with the vc4 driver: S1 in WSL (`.v3dp` device 21), the format (§4.3), the patches (§4.1), PD7 applied, the request's device field (§4.5), then S2 for m68k. S1 needs no Pi: it starts right after P0 | the Pi 4's `.v3dp` files unchanged; the VC4 corpus compiles; S1 and S2 bit-identical; P3's tests take their shaders from it |
| **P5** | The VC4 backend behind the interface (`ogles2/vc4/` on `vc4core`); on the A1200 with the Pi 3: Emu68's facts (§7), the probe under AmigaOS, then the test packages | the M1 to M7 packages pass on the Pi 3 card, and still on the Pi 4 card |
| **P6** | Conformance and speed on both cards: dEQP-GLES2's must-pass list on the A1200; gears, the examples, Quake III with its GLES2 renderer, SDL | the Pi 3 no worse than Mesa vc4's baseline (P0) except documented deviations; the Pi 4 as 1.3.3; benchmarks recorded for both |
| **P7** | Into `ogles2` (PD8): this repository's work merged there (the build root back to `~/ogles2-build`), checked again on both cards; then the release: one `ogles2.library`, one `v3dglslc`, the installer, the archives | a release from `ogles2`, tested on both cards |

## 9. Decisions

- **PD1 (2026-11-03): one `ogles2.library` with two backends,** chosen at start from the hardware
  (§5.1). Programs, the import library and the API stay as they are. (Earlier the same day: a
  separate `ogles2vc4.library`; changed after comparing. One library leaves programs unchanged,
  needs one installation and lets fixes reach both GPUs; its cost is the backend split with the
  Pi 4 as acceptance, and tests on both cards.)
- **PD2 (2026-11-03): `.v3dp` device version 21** for VC4 binaries; the loader already rejects a
  device version it does not support (`v3dp/FORMAT.md`, header). The format's VC4 sections are
  settled in P4 (§4.3).
- **PD3 (2026-11-03): one `v3dglslc` with both Mesa drivers,** the GPU named in each request
  (request version 2, §4.5); VC4 cache files `<24 hex digits>.vc4p`.
- **PD4 (2026-11-03): no Linux build of the front end** (§6.4); everything else runs on Linux
  first (§6).
- **PD5 (2026-11-03): the GPU lock** on the Pi 3 is the same public-semaphore code under its own
  name, `"VC4 submit"`: nothing else drives the VC4 3D core under AmigaOS, but several programs
  do.
- **PD6 (2026-11-03): every extension of 1.3 is kept on the Pi 3, the missing ones emulated**
  (§2.1); float textures stored as half floats, a documented deviation.
- **PD7 (2026-11-04): shader variants on the Pi 3: option 1, accept** (§4.2): the canonical
  keys; the offline compiler's key manifests list the common states, the other variants compile
  into the cache once (a stall the first time). Decided by the developer with P2's counts (a game
  about 40 to 60 variants, at most 9 for one program, 1 or 2 first needed in play); reducing
  the keys in the compiler (option 2) or splicing (option 3) are not planned.
- **PD8 (2026-11-03): the work lives in `ogles2pi3` until it is finished and tested** (P6), then
  goes into `ogles2`, from which the release for both Pis comes (P7). Meanwhile:
  - `ogles2` stays the Pi 4's: a fix needed there before P7 is made there and released from
    there as a 1.3.x;
  - such fixes are merged into this repository at once (`ogles2` as a second remote), so that
    the two stay close and P7's merge is a fast-forward or a small merge;
  - nothing is pushed to `ogles2` from here before P7. How the work goes in (a merge of this
    history, or a branch pushed there) is settled at P7.
- **PD9 (2026-11-03): licence.** `v3dcore` (and the V3D 4.2 backend code adapted from PiStorm3D,
  §5.3) stays under Hyperion's licence; everything else is MIT, with only permissive third-party
  notices. The built library, which contains `v3dcore`, stays under Hyperion's terms as a whole.

## 10. Status and next steps

**Done (2026-11-03):**
- Repository `ladislawv/ogles2pi3` (private) made from `ogles2` (`main` at release 1.3.3, its
  history and tags); cloned on the build machine at `C:\- CPP\ogles2pi3`.
- The plan: written, reviewed against this repository, then checked against the sources named at
  the top (Mesa's vc4 driver at the pinned commit, Raspberry Pi's Linux and userland, Emu68,
  Broadcom's guide). Corrections from that check: the packet XML is `vc4_packet.xml`; there are
  only three texture configuration words (P0 to P2); the keys live in `vc4_qir.h`; the wrap modes
  are in the key too; the raster store has no stride field; the RGBA64 half-float texture type
  exists and is filtered; Raspberry Pi's arm64 kernel has no `STRICT_DEVMEM`; the V3D node is
  disabled without the KMS overlay; the 3A+ and Zero 2 W have 512 MB.
- The licences checked (§5.3): today's `ogles2.library` is Hyperion-licensed through `v3dcore/` and
  five front-end files; the compiler has no copyleft obligations.
- Decisions PD1 to PD6, PD8 and PD9 taken by the developer (PD1, PD3 and PD8 after comparing one
  library with two; PD8: the work lives here until finished and tested, then goes into `ogles2`);
  PD7 waits for P2's counts. The A1200's PiStorm32-lite takes a Pi 3.

**Done (2026-11-04), on the test Pi 3 (§6.0):**
- P0: the Pi reachable from WSL (`ssh pi3`); its model, OS, kernel, firmware, Mesa, `gpu_mem` and
  clocks recorded (§6.0, §2); Mesa vc4 renders (`tests/pi3/ref/`); its ES 2.0 string and limits
  (§2.1, §2 "Limits"); `IDENT0..2` and the device tree (`/soc` `ranges` and `dma-ranges`, the
  V3D node, the mailbox alias) read (§2); both boot set-ups made and switched by
  `tests/pi3/boot.sh`; `fetch_external.sh` fetches the vc4 sources; **the dEQP-GLES2 baseline**:
  16,906 of 17,165 pass, 20 fail (§6.1).
- P1, its goal reached: `tests/pi3_probe/` powers the bare 3D core (`SET_ENABLE_QPU`), clears,
  and draws a triangle with Mesa's shaders in our own lists, identical to Mesa's picture (§6.2).
  Found: Mesa's `kernel/` render list clears a job late on the hardware (Linux's order works,
  §2 "Command lists"); `SET_DOMAIN_STATE` alone does not power the registers (§2 "Power");
  raster strides round up to 4 pixels and the padding is written (§2 "Raster stores"); the V3D
  node's `status` cannot be relied on (§5.1); Mesa's vc4 lacks `OES_standard_derivatives` by
  policy, not hardware (§2.1).
- The Pi's power supply sags (under-voltage flagged after every boot, one reboot under load):
  to be replaced before P3's speed measurements (§6.0).
- P1 finished: texturing from LT gives Mesa's picture exactly; raster textures sample right only
  at power-of-two widths (§2 "Textures"); the firmware counts `SET_ENABLE_QPU`, and a core not
  reset between users loses a tile's primitives now and then (VPM errors in `ERRSTAT`), so
  `vc4core` power-cycles the core into its reset state when it takes it and watches `ERRSTAT`
  (§2 "Power", §6.2). The second power supply still flags under-voltage (§6.0).

- P2's variant count measured on the A1200 with the Pi 4 card (§4.2): probe V
  (`ogles2/vc4keys.c`); the corpus, SDL2, SDL3 and Quake III (both renderers) need 1 to 9
  fragment keys per program, 39 + 25 for Quake III's renderergl2, 1 or 2 first needed in play.
  **This repository builds into its own root, `~/ogles2-build-pi3`** (every script's and the
  Makefile's default, 110 paths in 68 files; `~/ogles2-build-pi3/m68k`, `.../m68k-sync`,
  `.../v3dglslc`, `.../release`, `.../pi3`), never into `~/ogles2-build`, which the `ogles2`
  checkout builds the Pi 4's releases into: make compares timestamps and would mix the two
  trees' objects (the package scripts use `B` for the log-sync build in some packages and the
  plain one in others). The `ogles2` checkout's host compiler, which had been rebuilt from this
  repository's sources earlier the same day, was rebuilt from its own (V3D output identical
  either way). P7 (the merge into `ogles2`) returns the defaults to `~/ogles2-build`. Shared
  still: the toolchains, the ports' source clones, and the WSL Mesa tree (`~/mesa-build/mesa`),
  which now has vc4 configured and patches 0015 and 0016 applied; neither changes the V3D build
  (54 of 54 files unchanged).
- P4's S1 begun (§4.1): `v3dglslc -g 21` compiles with Mesa's vc4 driver, its code identical to
  the Pi's Mesa for the reference programs; the variant hook (patch 0015) reports code and
  uniform recipes; the Pi 4's output unchanged (54 files). Then the format (§4.3): 1.1 with a
  `VC4_VARIANT` section, written as `.vc4p` by `v3dglslc -g 21 -o`, decoded by `v3dp_dump.py`. The third power supply still flags
  under-voltage one minute after boot, idle (`0x50005`, ARM at 600 MHz): the cable is the next
  suspect (§6.0).

**Next:**
1. Done: the test Pi holds 5 V (another supply, 5.2 V; §6.0). A heatsink before long speed
   runs on it.
2. P2's backend interface and the MIT rewrite of the shared front end's PiStorm3D parts, on
   the Pi 4 card (PD7 decided: option 1): steps 1 to 4 done (§5.1), step 5 (textures) next; each
   step's speed compared with the last commit's library, alternating runs (§5.1).
3. P4's S1, next: the key states `-g 21 -o` compiles (today the one reference draw; the
   canonical keys of §4.2 and PD7 decide the rest); the library's limits for the Pi 3 in the
   compiler (8 varyings, draw buffers per the emulation); the corpus compiled for VC4 as a
   whole is done (36 of 36, §4.3, with the draw-buffers variants of patch 0016).
4. P3: `vc4core` on Linux, starting from the probe's lists (Linux's render-list order, the reset
   at take-over, `ERRSTAT` checks), with its tests in `tests/pi3/` against Mesa's pictures; early
   in it a long soak (hours of mixed draws) and the cause of the not-reset core's lost tiles.
