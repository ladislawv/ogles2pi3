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
| Power | `power_on_V3D`: PM_GRAFX and the ASB bridges at BCM2711's addresses (ASB `0x7EC11000`) | **(Pi 3) the mailbox's `SET_ENABLE_QPU` (`0x00030012`, value 1), as `hello_fft`**: the registers answer at once. `SET_DOMAIN_STATE` (`0x00038030`, domain 10, `RPI_POWER_DOMAIN_V3D`) alone reports the domain on, but the registers read `0xdeadbeef` (Linux also enables the V3D clock, `vc4_v3d.c`, `clk_prepare_enable`); `GET_DOMAIN_STATE` keeps answering 0 after `SET_ENABLE_QPU`. Direct PM/ASB writes (Linux's `bcm2835-power`) not needed. **The firmware counts `SET_ENABLE_QPU`:** after two enables one disable leaves the core on with its state; only the last disable powers it off (registers `0xdeadbeef`), and the next enable brings it up reset (`VPMBASE` `0x10`). A user that does not disable (a crash, a second program) leaves the next one a core that was never reset, and **such a core loses a whole tile's primitives about once in 80 draws, with VPM errors in `ERRSTAT`** (bits 6 to 8: read, write and free of unallocated VPM); a core reset first did not, in over 1,600 draws, nor did Mesa in 3,000 (`ERRSTAT` stays `0x1000`). So `vc4core` takes the core over by disabling until the registers read `0xdeadbeef`, enabling once and checking `VPMBASE` = `0x10`, and checks `ERRSTAT` after jobs. **The cause (2026-11-05, `vc4_draw --keep`, a core taken as it is):** `VPMBASE` changed on a core that has run jobs. Raised from 0 to `0x10` (the VPM kept for user programs) on such a core, the next jobs lost a tile's primitives (2 of 3 tries, in the first round) and every change left the VPM errors (bits 6 to 8) in `ERRSTAT`; the same change on a core just reset, before any job, did nothing (480 jobs clean), nor did a job stopped mid-binning or mid-render (its control thread reset, the program ending with the core enabled: 480 jobs clean after each), nor a core left enabled by a clean user with `VPMBASE` unchanged. So the vertex cache manager's VPM allocator keeps state that a `VPMBASE` change puts out of step, until a power cycle; `vc4core` writes `VPMBASE` once, after the reset of its take-over, before any job |
| RAM | 1 to 8 GB | 3B, 3B+: 1 GB; 3A+, Zero 2 W: 512 MB. Shared with the firmware's `gpu_mem`, Emu68's JIT cache and its RTG memory (`vc4-mem`). **(Pi 3)** Linux sees 905 MB with `gpu_mem` 76 MB (the KMS default), 855 MB with `gpu_mem=128` |
| CPU for Emu68 | Cortex-A72, 1.5 to 1.8 GHz | Cortex-A53: 1.2 GHz (3B), 1.4 GHz (3B+, 3A+), 1.0 GHz (Zero 2 W) |
| Shader cores | 8 QPUs, QPU v4 | 12 QPUs (3 slices of 4), QPU v2; **(Pi 3, 3B)** V3D clock 300 MHz (`vcgencmd measure_clock v3d`, both boots; core 400 MHz, ARM 1200 MHz) |
| Vertex input | VPM; formats converted by the hardware | VPM through the vertex DMA; **formats converted in the vertex shader**: `vc4_vs_key.attr_formats[8]`; at most 8 attributes (`max_inputs = 8`) |
| Blending | fixed-function | **in the fragment shader**, which reads the tile buffer (`vc4_nir_lower_blend.c`): the whole blend state (`pipe_rt_blend_state`: enable, equations, factors, colour mask), the logic op and the target's format (`color_format`: RGBA, BGRA, 565) are in the fragment key |
| Depth, stencil | per-draw packets | the fragment key has `depth_enabled`, `stencil_enabled`, `stencil_twoside`, `stencil_full_writemasks`; the stencil set-up words come in the uniform stream (`QUNIFORM_STENCIL`) |
| Command lists | V3D 4.x packets; the render list loops over supertiles by itself | VC4 packets, `src/broadcom/cle/vc4_packet.xml` (packers `v3d_packet_v21_pack.h`); the same scheme (binner, tile lists, renderer, semaphores), other encoding; **the render list names every tile** (coordinates, loads, branch to its bin list, stores). Linux builds it in the kernel; Mesa keeps an MIT copy of that code (`src/gallium/drivers/vc4/kernel/vc4_render_cl.c`, for the simulator). **(Pi 3) Mesa's copy is older than Linux's and wrong on the hardware:** it puts `CLEAR_COLORS` and the None-mode store after `TILE_RENDERING_MODE_CONFIG`, and then every job stores the previous job's clear colour (all tiles); Linux puts them first, which works (`tests/pi3_probe`, `clearseq`, `--variant=mesa`). The clear values are taken when the rendering-mode packet is processed. `vc4core` follows Linux's order |
| Binner memory | tile state, tile allocation, overflow | tile state 48 bytes and allocation 32 bytes per tile, the allocation 4 KB-aligned after the state (`kernel/vc4_validate.c`); **the tile state's pointers have 28 bits: state, allocation and overflow must lie in one 256 MB-aligned segment** (Linux `vc4_v3d.c`, `bin_bo_alloc`); overflow through `BPOA`/`BPOS`, `INTCTL`'s `OUTOMEM` bit |
| Shader state | GL shader state record, v4 | GL shader state record, v2; three shaders per program as now (fragment, vertex, coordinate); the vertex shader is compiled for the fragment shader it feeds (`vc4_vs_key.fs_inputs`) |
| Textures | UIF, UBLINEAR, LT; many formats; TFU | T and LT tilings (LT for small levels), sizes up to 2048 (`max_texture_2d_size`); types (guide, table 18): RGBA8888, RGBX8888, RGBA4444, RGBA5551, RGB565, LUMINANCE, ALPHA, LUMALPHA, ETC1, S16F, S8, S16, BW1, A4, A1, **RGBA64 (16-bit float per channel, filtered, read as two 32-bit results)**, RGBA32R and YUYV422R (raster). Mesa uses RGBA8888 (also for RGBX, BGRA and depth Z24S8, by swizzle and scaling), RGBA4444, RGBA5551, RGB565, ALPHA (A8, L8, I8, R8 by swizzle), LUMALPHA (LA8, RG8) and ETC1 (`vc4_formats.c`). Base addresses in 4 KB units (`VC4_TEX_P0_OFFSET`); mip levels stored before level 0, levels above 0 padded to powers of two. Raster textures: the guide supports them, Mesa says the Pi cannot sample them (`vc4_state.c`, it copies them to a tiled shadow). **(Pi 3)** both partly right: a raster RGBA32R texture is sampled right **when its width is a power of two** (16, 32, 64, 128, 256 wide, any height tried: 7 to 256); other widths are read from wrong rows (rows in groups of 4, spaced inside a group by the width rounded up to a power of two, the groups by 4 widths rounded up to a granule that changes with the height; no stride the CPU could choose fits, `tests/pi3_probe` `rastermap`). So an RTG bitmap or other raster image is sampled directly only at power-of-two widths, else through a copy (T/LT). An LT RGBA8888 texture gives Mesa's picture exactly. **(Pi 3, 2026-11-05)** RGBA64 is read as the guide says, one request and two results (RG, then BA, two halves each), **but its second word comes back stale once a 32-bit texture has been sampled since the core's reset**: alone (and after RGBA64 or untextured jobs) every pixel is right; after any RGBA8 lookup (2D, mipmap, cube map, a copy) thousands of pixels get the BA word of other texels of the same texture, earlier lookups, the count varying run to run, until a power cycle. Not changed by a pause before the job, another buffer, waiting for the cache clears, P2 written, the cache swizzle, the L2 cache off; halved by `TMU_NOSWAP`. So half-float textures are not RGBA64 but four **S16F** planes (one 16-bit float channel, filtered; a lookup returns the half in the word's low 16 bits, `s16fdiag`): patch 0018, `texhalf`, `halfmip`, clean after every other case. **(Pi 3, 2026-11-06) The TMU returns an S16F texel without its sign:** 0xbc00 reads as 0x3c00, 0xb800 as 0x3800, 0xc000 as 0x4000, the word's upper half 0 (`vc4_draw s16fsign` on the A1200: the raw word through an RGBA8 variant, then through the 16a unpack; `hdrrange` had shown the 64-bit store keeps the sign). So a texture with negative values has four more planes, max(v, 0) and max(-v, 0) subtracted in the shader, patch 0022. **P2's `BSLOD` (an explicit LOD, "use bias only") counts only with `PTYPE` 1** (cube map stride), which Mesa writes for every texture: with `PTYPE` 0 the TMU added its own LOD to the explicit one (every derivative-LOD case one level off, `minlod`). The TMU's bilinear rounds down: a 2x2 average through it is up to 2 below the rounded box filter where neighbours differ by about 200 (`genmip`). The tile buffer's 64-bit colour mode stores RGBA16F pixels, which are the RGBA64 type's texels (`hdr`). **No TFU** |
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
| Emulated, half-float textures | `OES_texture_half_float`, `OES_texture_half_float_linear` | planned as the RGBA64 type (two results, RG and BA); RGB, LUMINANCE, ALPHA, LUMINANCE_ALPHA expanded to RGBA at upload. **Done (2026-11-05) as four S16F planes instead**, one a channel, each a whole miptree the TMU filters (RGBA64's second word comes back stale after 32-bit lookups, §2 "Textures"): patch 0018 (no key bit: the sampler's key format `R16G16B16A16_FLOAT` says it; four lookups, the P0 recipe's data word carrying the plane in bits 16 and up), the library's planes (`texture_vc4.c`: plane k at k plane strides, 4 KB multiples; `draw_vc4.c` adds the plane's offset to P0), `vc4_draw texhalf` and `halfmip` Mesa's pictures pixel for pixel. Four lookups a sample instead of one. **Negative values (2026-11-06):** the TMU drops an S16F texel's sign (§2 "Textures"), so a texture holding any has four more planes in a block of its own, made when the first appears, and the sampler key's `half_signed` (patch 0022) takes each channel as plane i minus plane i + 4: eight lookups a sample, for those textures only; filtering stays exact, being linear |
| Emulated, float textures | `OES_texture_float` | stored as RGBA64 half floats (range ±65504, 11-bit precision): a documented deviation (accepted by the developer, 2026-11-03); filtered as the half-float ones. Not planned: exact storage as raw 32-bit values in RGBA8888 texels decoded in the shader. **Done (2026-11-05)** with the half-float ones |
| Emulated, in the shader | `ARB_texture_mirror_clamp_to_edge` (the coordinate's absolute value, then the hardware's clamp to edge); `EXT_texture_lod_bias` (a uniform added through the TMU's bias input); `SGIS_texture_lod` (max level through `MIPLVLS`; min/max LOD by an LOD computed from derivatives and an explicit-LOD lookup); `EXT_texture_filter_anisotropic` (extra lookups along the major axis from the derivatives, up to the maximum reported, at least 2) | key bits per sampler and vc4 compiler changes (§4.1); the cost falls only on programs that use them. **Done (2026-11-05), patch 0019:** the sampler key's `lod_bias`, `lod_clamp`, `aniso` (2^aniso lookups; the backend reports 4), wrap mode 6 for mirror-clamp; five uniform types (the bias, the LOD range in level 0's terms, level 0's size). A base level above 0 is not a copy: the LOD computed in the shader, clamped to [base + min LOD, min(base + max LOD, max level)] over the whole miptree, or Mesa's `force_first_level` (an explicit LOD) when one level is sampled; **deviation:** magnifying a base level above 0 filters with the minification filter (the TMU's LOD is above 0 there). Not on cube maps (their LOD would need the major face's derivatives): only the bias there. `vc4_draw mirror` (against texels worked out), `mipemu` (bias, min LOD, max LOD, base level, both, max level, a forced level, anisotropy on square footprints: Mesa's mipmap picture with the levels GL picks), `anisoquad` (a 32x8 footprint: level 2 by the TMU, level 0 by 4 lookups), every pixel |
| Emulated, render targets | `EXT_draw_buffers`, `NV_fbo_color_attachments` | **one pass per colour attachment:** the framebuffer object's job is binned and rendered once per attachment, each pass with a fragment variant that writes that attachment's output (key: output index) and loads the depth and stencil the job started with; only the last pass stores them. Costs N times the work; needs a vc4 compiler change (Mesa's vc4 has one colour output, `max_outputs` 1 for fragment shaders: the compiler's screen reports four draw buffers, and the key picks one output). **Compiler side done (2026-11-04):** patch 0016 adds the fragment key's `draw_buffer` (`gl_FragData[draw_buffer]` is the colour, `gl_FragColor` in every pass, the other outputs dropped), `v3dglslc -g 21` reports four draw buffers and compiles one fragment variant per `gl_FragData[k]` written; the format's key has the byte (FORMAT.md 1.1). **GPU side checked (2026-11-05):** `vc4_draw drawbuf`, the depth case's draws in two passes with `gl_FragData[0]` and `[1]`'s variants (`tests/pi3/ref/shaders/mrt.frag`): Mesa's depth picture and its inverse, pixel for pixel. **The library's passes done (2026-11-05, P5; m7-4 and m7-5 pass on the Pi 3 card, 2026-11-06):** a job of its own per colour attachment but the last, lists only (`vc4_job_init_lists`), bound and rendered to the end inside `job_bin`; the last is the context's job, which stores depth and stencil and may render on. Each draw is recorded in every pass with its own keys, variants and uniform streams (a fragment variant may read other varyings, so its vertex variant is its own too), the vertex data and indices shared. `draw_buffer` stays 0 for an output the file holds no variant of (`ogles2_vc4_output_for`): `gl_FragColor`'s code is the same in every pass, so ordinary programs and the built-in clear compile nothing more, and an output the program does not write is undefined. Found on the way: `v3dglslc` never compiled `gl_FragData[1]`'s variant (no flush before its draw, so Mesa did not look at the key again), and patch 0016's store compared an output's location with its index, four times it, so a two-word (half-float) store of any output but 0 was dropped: both fixed, checked by `vc4_keys.py` |
| Native (64-bit tile buffer) | `EXT_color_buffer_half_float` | **Changed (2026-11-05):** not packing but the tile buffer's own 64-bit colour mode (the guide's "HDR" mode, which Mesa does not use: 64x32 tiles, RGBA16F pixels, the stores' colour format ignored), whose stored pixels are the RGBA64 texture type's texels, so a half-float target is sampled as a half-float texture with no conversion. Patch 0021: a fragment key with `color_format` `R16G16B16A16_FLOAT` writes two TLB words a pixel (RG, BA) through the per-sample colour writes, and blends in floats, unclamped, from two TLB reads; vc4core's `struct vc4_target` `hdr` (the tile-depth bits, the tile size, a 64-bit clear colour). No MSAA with it. A rendered image is RGBA16F pixels, so sampling it needs the S16F planes (the TMU's RGBA64 type is unreliable, §2 "Textures"): the CPU splits it (as `vc4_draw hdr` does; a GPU split would need an S16F store the tile buffer does not have). `vc4_draw hdr` (the depth case into an RGBA16F T-format image, split, drawn from the planes: Mesa's depth picture pixel for pixel), `hdrblend` (Mesa's blend picture within 1), `hdrrange` (2.0 and -0.5 stored as such). **The library's targets done (2026-11-05, P5; m7-5 passes on the Pi 3 card, 2026-11-06):** each half-float texture drawn into (renderbuffers are such textures) has a 64-bit image beside its planes, a level current in one or the other (`texture_vc4.c`: composed from the planes when a job first takes the level, split into them when it is next sampled or stored into, both on the CPU); the 64-bit mode is per pass, so an RGBA8 attachment beside a 16F one works; the pools sized for 64x32 tiles. A job **loading** a 64-bit image, which the `hdr` cases do not do, works (m7-5's blending and ping-pong); Z24S8 images with 64x32 tiles are not exercised by a package yet |
| Emulated, depth | `EXT_clip_control` | origin by the front end's flips as now; `ZERO_TO_ONE` by a twin of patch 0013 in the vc4 vertex and coordinate shaders. **Done (2026-11-05), patch 0020:** by a vertex key byte (`clip_halfz`) rather than a uniform, so the other mode costs nothing: z taken from 0..w to 2z - w before the clip coordinates and the scaled Z, VC4 clipping z to -w..w as the guide says; the viewport's Z scale and offset stay GL's. `vc4_draw clipz` (the depth case through it, a triangle before the near plane clipped away): Mesa's depth picture pixel for pixel |

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

### 3.1 `vc4core`: the design (P3, drafted 2026-11-04, for review)
What the VC4 backend (`ogles2/vc4/`, P5) calls, and the tests of P3 (`tests/pi3/`) before it.
From the probe (`tests/pi3_probe`, P1), which has every hardware sequence this needs, but in one
fixed 4 MB block with one-off lists; `vc4core` makes them general. MIT; follows Mesa's vc4 and
Linux's vc4 driver where named; nothing from `v3dcore` or PiStorm3D (PD9).
- **Files** (`vc4core/`):
  - `include/vc4core.h`: the API below; `include/vc4_os.h`: the OS layer;
  - `os_linux.c` (P3: `/dev/mem`, `/dev/vcio`, the firmware's allocator, as the probe's
    `plat_linux.c`) and `os_amiga.c` (P5: Emu68's peripheral window, AllocVec'd fast RAM, exec's
    cache calls, the GPU lock's semaphore);
  - `device.c`: take-over (disable `SET_ENABLE_QPU` until `IDENT0` reads `0xdeadbeef`, enable
    once, `VPMBASE` = `0x10`, §2 "Power"; then `VPMBASE` 0, as Linux), identity, registers,
    `ERRSTAT` (reported and cleared after each job), the reset after a job
    that does not finish (both control threads stopped, the core power-cycled as at take-over);
  - `mem.c`: blocks with a CPU pointer and a bus address (Pi 3: `0xC0000000 |` the physical
    address), alignment, and the binner's rule that tile allocation, tile state and overflow lie
    in one 256 MB segment;
  - `cl.c` and `vc4_pack.h`: control-list buffers that grow (a full block ends with `BRANCH` to
    the next, as v3dcore's binning list does on the Pi 4) and the packet packers, generated from
    `vc4_packet.xml` by Mesa's `gen_pack_header.py` (MIT; they write bytes, so they are right on
    the big-endian 68k too);
  - `job.c`: a job = binning list + render list for one target: begin (`TILE_BINNING_MODE_CONFIG`
    with the context's tile pools, `START_TILE_BINNING`; the backend then appends its state and
    draws), end (`INCREMENT_SEMAPHORE`, `FLUSH`), the render list per tile in Linux's order
    (clear colours and a None-mode store first, then `TILE_RENDERING_MODE_CONFIG`; per tile its
    coordinates, loads, `WAIT_ON_SEMAPHORE` before the first branch, `BRANCH_TO_SUB_LIST` to the
    tile's bin list, stores, the last with end of frame; MSAA and the raster/T/LT surfaces of
    Linux's `vc4_render_cl.c`), submit (L2 and TMU caches flushed before the render, as Linux;
    `BFC`/`RFC` counts; `BPOA`/`BPOS` overflow memory), waits with a timeout, the binner's
    out-of-memory interrupt served with more overflow memory, `ERRSTAT` checked after each job;
  - `tiling.c`: texture layouts (T, LT, raster) and mip levels, after Mesa's `vc4_tiling.c` and
    `vc4_resource.c` (raster textures only at power-of-two widths, §2 "Textures"); the raster store
    into a padded image (§2 "Raster stores");
  - `lock.c`: the GPU lock (one submitter at a time; on AmigaOS the semaphore other GPU clients
    share).
- **API** (`vc4core.h`, C, no OS types): `vc4_open`/`vc4_close` (take-over, identity);
  `vc4_mem_alloc`/`_free` (`struct vc4_mem`: `cpu`, `bus`, `size`); `struct vc4_cl` with
  `vc4_cl_room` and the packers; `struct vc4_surface` (bus address, format, tiling, stride,
  padded height, MSAA) and `struct vc4_target` (colour and depth/stencil surfaces, size, what to
  load, store and clear, the clear values); `struct vc4_pools` (tile allocation, tile state,
  overflow for a size in tiles); `vc4_job_begin(job, target, pools)`, the backend's packets into
  `job->bcl` and its shader records and uniforms into `job->state` (a growing buffer),
  `vc4_job_submit` (returns a frame number), `vc4_job_wait(frame, timeout)`; `vc4_tile_layout` and
  `vc4_tile_store`/`_load` for textures.
- **Writing GPU memory:** Linux maps the firmware's memory as device memory (the probe's
  `plat.h`): every write is a naturally aligned 1-, 2- or 4-byte store, never `memcpy`; the
  packers and the helpers keep to that. Words for the GPU are stored little-endian by helpers
  (`vc4_put32le`), never by a plain store, so the m68k build needs no other change (§6.3: the
  Amiga runs have the last word on byte order).
- **Tests first** (`tests/pi3/`, Linux, S1's shaders): the probe's triangle and textured quad
  rebuilt on `vc4core`, their pictures equal to the probe's and Mesa's; then §6.3's list, a
  feature at a time, each picture against Mesa's; the soak (hours of mixed jobs, `ERRSTAT`
  clean) early.
- **Changed while writing it (2026-11-04):** the packers come from a generator of our own,
  `tools/vc4pack/gen_vc4_pack.py`, reading the same `vc4_packet.xml` (Mesa's
  `gen_pack_header.py` shifts 64-bit values for every byte and copies 32-bit fields with
  `memcpy` in the host's byte order: slow on the 68k and wrong there). Same field names as
  Mesa's; `--test` packs every packet with 200 random field sets through both and compares:
  5,200 packets, none differ. The XML's two structs (shader and attribute records) have
  overlapping fields and `GL_SHADER_STATE`'s address is a 28-bit number at bit 0 (it would
  lose a bus address's top bits): written by hand (`vc4_gl_shader_state`), as Mesa's vc4 does.
  On Linux the tests are built with `-fno-store-merging -fno-tree-vectorize
  -fno-tree-slp-vectorize`: gcc otherwise turns the packers' byte stores into wider, unaligned
  stores (a NEON store of a byte run), and those fault on the device-mapped GPU memory (a bus
  error).
- **Done (2026-11-04):** `vc4core/` (`device.c`, `mem.c`, `cl.c`, `job.c`, `os_linux.c`, the
  headers; MIT) and its first test, `tests/pi3/vc4core/triangle.c` (`build.sh`: aarch64, and
  the core for the 68k as a compile check): the probe's triangle through the API, **Mesa's
  picture pixel for pixel**, then 200 repeats all the same with `ERRSTAT` clean; the take-over
  reads V3D 2.1, 3 slices of 4 QPUs, 12 KB VPM. Then `tiling.c` (Mesa's layouts: LT and T,
  the miptree from the smallest level up, level 0 page aligned, cube faces a page-aligned
  miptree each) and `tests/pi3/vc4core/texture.c`: the probe's 16x16 texture, LT by the TMU's
  rule, **Mesa's picture pixel for pixel**; a 64x64 one, T by the rule, its texels holding
  their coordinates: all 8,464 pixels inside the quad the texel NEAREST puts there; mipmaps:
  a 64x64 miptree of 7 levels (0 and 1 T, 2 to 6 LT, each one colour), quads of 64, 32, 16
  and 8 pixels show levels 0 to 3 with NEAREST_MIPMAP_NEAREST (`vc4_tex_p0`/`_p1` build the
  TMU's words, Linux's `VC4_TEX_P0_*`/`P1_*`). A miptree is larger than its texels (T levels
  pad to 8 utiles, level 0 is page aligned): the caller allocates `layout.size`. Then the
  tests' program loader and draw builder, `tests/pi3/vc4core/vc4p.c` (a rehearsal of the VC4
  backend's program handling, P5): a `.vc4p` read (header, CRC, the constant buffers'
  templates, uniforms, attributes, every `VC4_VARIANT`), each stage's uniform stream built
  from its recipe as Mesa's `vc4_uniforms.c` (constants, constant-buffer words, viewport,
  texture P0-P2, blend colour; the others refused by name until a test needs them), the
  shader record from the prog data as `vc4_emit_gl_shader_state` (selects, VPM offsets,
  varyings, threading), the state and draw packets (arrays or 16-bit indices).
  `PRIMITIVE_LIST_FORMAT` is now `vc4_job_begin`'s (once a job, as Mesa's `vc4_start_draw`).
  `tests/pi3/vc4core/draw.c`: the triangle and the textured quad compiled on the PC by
  `v3dglslc -g 21` (`build.sh` compiles them) and drawn through it: **Mesa's pictures pixel for
  pixel**. Next: §6.3's list with new programs and Mesa's pictures of them (depth, stencil,
  blending, indices, the attribute and texture formats, cube maps, MSAA: reference programs
  beside `tests/pi3/ref/`, their pictures made in one reference boot), the binner's overflow on
  demand, and the soak.
- **Then (2026-11-04): most of §6.3's list.** The reference programs and data are
  `tests/pi3/ref/cases.h` and `shaders/` (`flat`, `point`, `color`, `cube`, `texture`), drawn by
  Mesa in the reference boot by `tests/pi3/ref/mesa_cases.c` (`mesa_<case>.ppm`) and by `vc4core`
  in the bare boot by `tests/pi3/vc4core/draw.c` from `.vc4p` files compiled on the PC;
  `v3dglslc -s` draws the states the cases need after the reference draw (depth, blending,
  points, lines, stencil, an unsigned-byte attribute, the texture formats, an RGB565 target:
  §4.1), so the files hold those variants, and the tests' loader picks a variant by its key
  (primitive class, depth, blending, stencil, render target format, attribute and sampler
  formats). **Mesa's pictures pixel for pixel:** depth (three triangles, `LESS`), blending
  (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`), 8- and 16-bit indices, the scissor box (the clip
  window), lines, points (8 pixels), stencil (`ALWAYS`/`REPLACE`, then `EQUAL`), colours as
  normalized unsigned bytes (converted in the vertex shader: their own variant), textures
  RGB565, LUMINANCE (the ALPHA type, swizzled in the shader), RGBA4444 (the hardware reads GL's
  packing; this Mesa keeps `GL_RGBA4` as RGBA8, so the RGBA8 variant samples it), RGBA5551,
  LUMINANCE_ALPHA, ETC1 (blocks stored unchanged, P1's ETC flip set as Mesa does) and NPOT
  (13x7), a cube map (six faces, each a page-aligned miptree, P2's stride), an RGB565 frame
  (blending into it, not dithered as Mesa's), and two cases checked against Mesa's depth
  picture: the scene in two jobs (colour stored raster and depth/stencil stored T, both loaded
  by the second) and through a T-format image drawn back as a texture texel for pixel (render
  to texture). **4x MSAA** (32x32 tiles, the binner's and the renderer's 4x modes, the
  rasteriser oversampling, early Z off, the colour stored resolved) against Mesa's 4x pbuffer:
  equal once ours is drawn upside down as Mesa draws a window system's buffer (the samples'
  positions are mirrored with it), each channel within 1 (Mesa's resolve rounds, 63.75 to 64;
  the hardware's truncates, to 63). The binner's overflow on demand: 6 draws of 163,840 tiny
  triangles, each in all four tiles of the frame, ask for 12 chunks of overflow memory
  (binning 74 ms, rendering 258 ms) and give the picture of one draw. Served by polling
  `INTCTL`'s `OUTOMEM` bit while waiting for the binner (the bit is set with the interrupt
  disabled; **enabling it, as Linux does, hung the firmware's mailbox in the bare boot**: its
  line reaches the VideoCore); the new memory is written before the request is cleared, as
  Linux does. The pools have 16 chunks of 512 KB in their one block; a job that needs more is
  stopped and the core reset rather than left waiting. A frame 130 pixels wide (rows 132
  apart) with the depth scene in its left 128 columns: those are Mesa's picture, columns 128
  and 129 the clear colour, and the store writes the rows' padding too (all 256 pixels), as
  the probe found (§2 "Raster stores").
- **`ERRSTAT` (2026-11-04):** its error bits stay set until written back (1 clears), so one
  error showed in every later job: `vc4core` now reports them after a job and clears them.
  The 15,000-round soak (108,000 jobs) found the one error there is: **`VCMBE` (bit 14, the
  binner's vertex cache manager) in about 1 of 200 jobs whose binner ran out of memory**
  (15 in 3,000 such jobs; with chunks large enough that it never runs out, none in 3,000;
  never in jobs without overflow), with the picture right every time. Linux and Mesa never
  read `ERRSTAT`. The tests count it and do not fail on it; the backend (P5) should make the
  first chunk large enough that ordinary frames never run out. **Overflow chunks may lie in
  separate blocks:** the earlier finding that they give `VCMBE` was made before the bits were
  known to stick; retested (a test build, `-DVC4_SEPARATE_OVERFLOW`, chunks 1 and up each
  allocated on its own): 15 `VCMBE` in 3,000 jobs, the same rate as in one block, every picture
  right. So the backend can grow overflow memory on demand, as Linux does (the 256 MB segment
  rule still holds).
- **The soak (2026-11-04):** every case in a round, the overflow one every tenth; 5,000
  rounds, **126,000 jobs in about 30 minutes, no failure**, one `VCMBE` in the 500 overflow
  jobs, the Pi not throttled (53 °C). (The 15,000-round soak before it, 108,000 jobs, was
  clean but for the overflow jobs after the first `VCMBE`: the sticky bit.)
- **`VPMBASE` 0 (2026-11-04):** Linux's `vc4_v3d_init_hw` sets it to 0 ("take all the memory
  that would have been reserved for user QPU programs"), so Mesa's shaders always run with the
  whole 12 KB VPM for the vertex pipeline; `vc4core` now does the same after the take-over
  (which still checks the reset value 0x10 first). Every case passes as before; `VCMBE`'s rate
  is unchanged (16 in 3,000 overflow jobs); a mixed soak with it, 2,000 rounds (52,400 jobs), no
  failure.
- **First GPU-only numbers (2026-11-05, `vc4_draw speed`, a 1024x768 RGBA8 raster frame, the
  times the job measures; short runs, no heatsink yet, so a first look):** clear and store
  alone 0.96 ms (823 Mpixel/s stored); full-frame quads flat 1,032 Mpixel/s, blended 770,
  textured (256x256 RGBA8 T, NEAREST) 714; 200,000 small triangles (about 10x8 pixels, 16-bit
  indices) binned at 34 Mtri/s and rendered at 13.5 Mtri/s; the CPU's tiling of a texture
  into GPU memory (Linux, the A53) 79 MB/s. The Amiga's numbers (P5) are the ones that count:
  its CPU writes and tiles everything.
- **With the heatsink and fan (2026-11-05):** the speed case three times: the same numbers to
  0.5 % (flat 1,030 to 1,032 Mpixel/s, blended 769, textured 711 to 712, 13.45 Mtri/s
  rendered, the tiling 79 MB/s), so the first ones were not throttled either. **The burn:** a
  3,000-round soak of every case (171,600 jobs, the overflow case every tenth round) with the
  four CPU cores busy, 37 minutes: **no failure, never throttled** (`get_throttled` `0x0`,
  the ARM at 1.2 GHz and the V3D at 300 MHz in every 30 s sample), 31 °C at the start, 53 to
  55 °C after the first two minutes (55.3 at most). **`VCMBE` with the CPU busy:** 239 of the
  300 overflow jobs reported it (pictures right); in 500 rounds each, 0 of 50 with the cores
  idle against 32 of 50 with them busy. So the binner's vertex cache error when it runs out of
  memory is common when the CPU loads the memory bus, as Emu68 does on the A1200: the backend
  counting it, not failing a job on it (§5.1, `job_vc4.c`), is right.

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
    colour. **Done (2026-11-05)**, each checked on the Pi 3 (§6.3) and S1 and S2 identical on
    them (`compare_vc4p.sh`'s `emutex`, `emuflat`, `emumrt` runs): patch 0016 (output index,
    2026-11-04); **0018** half-float and float textures as four S16F planes (the key's format;
    not RGBA64: §2 "Textures"); **0019** the texture parameters (`lod_bias`, `lod_clamp`,
    `aniso` beside `force_first_level`, mirror-clamp by its wrap value, five uniform types,
    `ntq_emit_tex`'s lookup split out); **0020** `EXT_clip_control`'s `ZERO_TO_ONE` (the vertex
    key's `clip_halfz`); **0021** half-float targets in the tile buffer's 64-bit mode (no
    packing: the hardware has the mode). With none of their key bits set the code is as before
    (the VC4 corpus 36 of 36 unchanged);
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
- **Done (2026-11-04): the limits programs are compiled with.** `v3dglslc -L -g 21|42` prints
  them (GLSL's `gl_Max*` constants and what a link checks; dEQP's `builtin_variable` compares
  them with the library's queries). `-g 21`: vertex attributes 8, uniform vectors 4088 and
  4095, varyings 8, texture units 16, 16 and 32, draw buffers 4 (the emulation, §2.1),
  textures, cube maps and renderbuffers 2048: Mesa vc4's on the Pi 3 (§2 "Limits") but for the
  draw buffers. `-g 42`: 16, 4088, 4095, 16, 16, 16, 32, 4, 4096, 1.3.3's. So the VC4 backend's
  caps (P5) take these values. `struct OGLES2Caps` has the vertex attribute count since
  (`max_vertex_attribs`, V3D's 16; `OGLES2_GL_VERTEX_ATTRIBS`): `GL_MAX_VERTEX_ATTRIBS`, the
  index checks of the vertex array and attribute calls and of `glBindAttribLocation`, and the
  link's own attribute locations use it; the context's arrays stay 16 long (the internal
  programs use the last two, as front-end slots). The Pi 4's compiler output unchanged
  (`v42_unchanged.sh`: 54 of 54).
- **Done (2026-11-04): the states P3's tests need (`-s`).** `v3dglslc -g 21 -s a,b,...` draws
  the program again after the reference draw in each state named, so the `.vc4p` holds those
  variants too: `depth` (`LESS`, a D24S8 target), `stencil` (one-sided `ALWAYS`/`REPLACE`; vc4's
  key then has the depth flag too), `blend` (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`), `points`,
  `lines`, `ubyte1` (attribute 1 as four normalized unsigned bytes), the 2D samplers on a texture
  of another format (`tex565`, `texlum`, `tex4444`, `tex5551`, `texla`, `etc1`) and `rt565`
  (an RGB565 target, plain and with `blend`). A stop-gap for the tests until P5's key builder
  and PD7's canonical keys decide what a program file holds. The Pi 4's output unchanged
  (54 of 54).

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
- **Done (2026-11-05): variants from keys.** The compiler compiles a variant for a key it is
  given, not only for the states a draw reaches: patch 0017 exports vc4's own
  `vc4_get_compiled_shader` as `vc4_compile_variant` (and the context the hook's variants came
  from); `v3dglslc -g 21 -k FILE` reads keys in the layout a `VC4_VARIANT` section stores them
  (a u32 stage, 4 fragment or 0 vertex, then the key; a vertex record takes the fragment
  inputs of the fragment record before it, its coordinate twin gets none, as
  `vc4_update_compiled_vs`) and adds their variants to the program's file, after the reference
  draw's (which gives the shaders the keys point to). `tools/v3dglslc/test/vc4_keys.py`: for
  each of the five reference programs, the keys of its `-s` variants compiled with `-k` alone
  give the same variants byte for byte (5 of 5 programs). This is what both the offline
  compiler's key manifests and the run time's variant requests (§4.5) use; the canonical key
  builder (P5) writes these records. The Pi 4's output unchanged (54 of 54).
- **Done (2026-11-05): the key builder** (`ogles2/vc4/key_vc4.c`, the VC4 backend's, plain C):
  a draw's GL state (target format, primitive, depth and stencil enables, MSAA flags, blending
  in GL's terms, colour mask, draw buffer; per sampler the texture's storage and wraps; per
  vertex input GL's type, size and normalisation, gl_PointSize) turned into `-k`'s records,
  and the texture storage table the sampler entries describe (`vc4_tex_format`: the TMU's type,
  bytes a texel, the key's format and view swizzle: RGBA8, RGB8 as RGBX8888, RGB565, RGBA4444,
  RGBA5551, LUMINANCE and ALPHA in the ALPHA type, LUMINANCE_ALPHA, BGRA8, ETC1). Checked on
  the PC against the keys Mesa's state tracker and driver build for the same states
  (`tests/pi3/keys/`, against the `-s` files): **all 28 keys equal Mesa's byte for byte** (31
  with the depth textures' three, 2026-11-05), and
  every key in the files is one of them. Two of Mesa's habits found on the way: the sampler's
  comparison function is filled in (GL's `LEQUAL`) with comparison off, and blending's factors
  are zero when it is off. **Canonical keys** (wraps as `REPEAT`, the comparison function 0)
  compile to the same code: the texture program's six formats with three wrap modes each give
  20 exact variants and 9 canonical ones, every exact variant's code among them.

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
- **Done (2026-11-05): the request's device field and S2 with both drivers.**
  `v3dp/v3dglsl_port.h`: `V3DGLSL_VERSION` stays 1 (the Pi 4's requests, exactly as 1.3.3's
  library sends them, so its behaviour and an old compiler's answers stay as they were);
  `V3DGLSL_VERSION_DEVICE` (2) adds, after `bindings`, `device` (42 or 21) and the program's
  VC4 keys (`vc4_keys`, `vc4_keys_size`: `-k`'s records; the library sends every key it knows
  for the program and the compiler writes the `.vc4p` anew with all of them). VC4 takes
  `LINK`, `LINK_BOUND`, `COMPILE` and `QUIT`; `LINK32` and `LINK_A2C` answer 3. The compiler
  makes a GPU's screen and context when a request first names it (`use_device`), so the
  server on the Pi 4 never creates the VC4 one and its port appears before the first context
  is made (the first request takes that time instead of the start). The command line goes
  through the same `write_program` for both GPUs (the VC4 corpus's 36 files unchanged).
  `build_m68k.sh` links the vc4 driver (`replay.py --libs` TOOL + VC4): the m68k compiler is
  19,294,288 bytes stripped, 286 KB more than with v3d alone; S1 and S2 write the same files
  byte for byte, `tools/v3dglslc/test/compare_vc4p.sh` (6 of 6: the reference programs, `-s`
  states and a `-k` key file) and `compare_v3dp.sh` for the Pi 4 (8 of 8). The server's
  version-2 path waits for the A1200 (P5).
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
  programs move with step 7). Helpers both use are in `ogles2/draw_private.h`.
  Probe P's stage 1 now holds the slot mapping, and probe V counts a draw after it is made
  (it reads the same textures' storage). Pi 4 checks the same day: m5-1 255 PASS, no FAIL;
  m7-6 36 of 36; m2-5 14 of 14 identical; speed against step 3 (`tests/a1200/p2_speed.sh`):
  Quake III 89.4, 89.7, 88.7 fps against 89.4, 89.5, 88.4; gears bitmap arrays 823, 778,
  807 against 826, 807, 808, screen 1083, 1084, 1078 against 1106, 1084, 1098 (the same
  step 3 library ran bitmap arrays at 772 to 779 in the afternoon and 807 to 826 in the
  evening: the station's drift is larger than any step's cost so far).
- **Step 5 done (2026-11-04):** texture storage. `ogles2/v3d/texture_v3d.c` (Hyperion) has
  the stored formats and their packing, the GPU copy's allocation and tiling, its loads and
  stores (the TFU's uploads), renaming, glGenerateMipmap's and glCopyTex*'s TFU work, the
  draw's texture and sampler records (`ogles2_texture_state`, called by `draw_v3d.c`), a
  level as a render target (`ogles2_texture_surface`, `_zs_surface`: fbo.c still calls them
  directly, until step 6) and probe V's stored format; ten table entries (`tex_upload` …
  `tex_from_raster`). `texture.c` keeps the objects, names and parameters, the RAM copies
  and their conversions, completeness (`ogles2_texture_complete`), the fallback texture,
  when a GPU copy is made again or renamed, and the CPU's mipmaps. What both read is in
  `ogles2/texture_private.h`. Still to do for VC4: the texture object holds the V3D
  backend's storage fields (`gpu`, `hw_type`, `cpp`, `swizzle`, `face_stride`, the records'
  cache); VC4's storage replaces them with its own when it is written (P5). The shared
  helpers of `draw_private.h` and `texture_private.h` are `static inline` (the faster
  choice, whatever the size: plain static functions there ran Quake III at 87.4 fps against
  88.3 to 88.8 inline, 4 KB less), a draw's samplers reach the context's texture state
  without a call (`texture_ts_fast`), and the Makefile's library objects depend on every
  `ogles2/*.h`. Pi 4 checks the same day:
  m5-1 255 PASS, no FAIL; m7-6 36 of 36; m2-5 14 of 14 identical; and the texture packages
  as at their last runs: m2-4 146, m7-2 (S3TC) 66, m7-4 (draw buffers) 129, m7-5 (half-float
  targets) 162 PASS, none FAIL (`p2_checks.sh` counts them too now). Speed against step 4,
  alternating: gears as step 4 (bitmap arrays 816, 812, 811 against 815, 817, 813; screen
  1092, 1084, 1092 against 1104, 1098, 1097), Quake III about 2 % slower (88.2, 87.5, 87.6
  against 90.1, 89.6, 89.4 fps). Probe P puts the loss in stages this step did not change
  (per frame: glBufferData, in the untouched buffer.c, 443 against 410 us; vertex copies 812
  against 762; indices 220 against 193; the texture stage equal), the pattern of a placement
  effect on unchanged code (as gears at step 3), not of the moved code's cost. Open: P2's
  acceptance compares the whole against 1.3.3 and looks for the cause if Quake III stays
  below it (where the hot copies land, Emu68's translation of them).
- **Step 6 done (2026-11-04):** framebuffer targets. `ogles2/v3d/fbo_v3d.c` (Hyperion) has a
  depth or stencil renderbuffer's storage (UIF without XOR, Mesa's padding) and its surface,
  a framebuffer object's tile size, bpp and depth type (Mesa's v3d_choose_tile_size) and its
  tile pools; with the texture levels' surfaces (`texture_v3d.c`), eight table entries
  (`rb_zs_storage` … `tex_zs_surface`); fbo.c and copy.c call nothing of V3D's any more.
  Also the memory freed after a job (`mem_free_after`: share.c's and v3dp.c's deferred
  frees, step 2's "free after the job"). The TFU paths went behind the table with step 5
  (uploads, glGenerateMipmap, glCopyTex*'s raster copy); nothing else blits. Left for before
  the acceptance, with step 7: the device group of step 1 (open, reset, a context's pools
  and their resize, bytes in use, GPU reach, the GPU lock: `device.c`, called by name from
  context.c, frame.c, job.c, texture_v3d.c), so that a device open can choose VC4. Pi 4
  checks the same day: m5-1 255 PASS, no FAIL; m7-6 36 of 36; m2-5 14 of 14 identical;
  m2-4 146, m7-2 66, m7-4 129, m7-5 162 PASS, none FAIL; speed against step 5, alternating: Quake III
  88.3, 88.1, 88.9 fps against 89.2, 88.0, 88.0; gears bitmap arrays 812, 810, 809 against
  818, 816, 813, screen 1091, 1082, 1076 against 1094, 1098, 1090 (within 1 %).
- **Step 7 done (2026-11-04):** programs, and the device group left from step 1. Programs:
  `v3dp.c` reads the file's frame, checks the header's device against the backend's
  (`device`: 42, 21), takes the backend's variant section (`variant_section`: 6, VC4 7) and
  copies the code into GPU memory; the backend reads a variant's key and prog data
  (`variant_read`), judges its recipe's types (`variant_recipe`: its enum, UBO 0 only,
  `uses_ubo`, `no_textures`, the clip-z coefficient; 0 and 1, constant and constant buffer
  word, mean the same on both GPUs, so the uniform index tables stay shared) and chooses a
  draw's variant (`variant_choose`); `ogles2/v3d/v3dp_v3d.c` (MIT) with the reads in
  `ogles2/v3dp_private.h`. The built-in image (PiStorm3D's shaders) and the internal
  programs (`clear_mrt`, `copy_tex`) are the backend's (`builtin_image`,
  `internal_program`; `ogles2/v3d/program_v3d.c`, Hyperion); file names take its
  `program_ext` (`.v3dp`, VC4 `.vc4p`). Device: `ogles2/v3d/device_v3d.c` (v3d_init, the
  renderer, reset, the RAM the GPU reaches, the GPU lock, a context's pools); `device.c`
  keeps the clock and calls the table (nine entries `open` … `context_free`), and its open is
  where the VC4 backend will be chosen. Kept V3D's for now, to be generalised when VC4's are
  written (P5): `struct OGLES2Variant`'s prog-data fields, the texture object's storage
  fields (step 5), `ctx->v3d`'s type, the `.v3dw`/`.v3da` extra variant files (VC4 has no
  such variants: none are found), the on-target compiler's GPU (`compiler.c` runs
  `v3dglslc` for V3D; S2 for VC4 is P4's). Pi 4 checks the same day: m5-1 255 PASS, no FAIL;
  m7-6 36 of 36; m2-5 14 of 14 identical; m2-4 146, m7-2 66, m7-4 129, m7-5 162 PASS, none
  FAIL; the whole `make` (library, tests, fpcheck) builds; speed against step 6, alternating: Quake III 88.8,
  88.2, 87.3 fps against 88.5, 88.3, 88.1; gears bitmap arrays 817, 808, 809 against 817, 809,
  808, screen 1093, 1081, 1085 against 1106, 1086, 1081 (as step 6).
- **Step 8 done (2026-11-04):** the front end MIT (§5.3). An audit compared job.c, display.c,
  draw.c, program.c, texture.c and the two private headers with PiStorm3D's `gl/` and backend
  sources, v3dcore and the other MiniGL trees in `external/`. Copied or closely adapted, now
  rewritten (same behaviour): display.c's choice of a screen's depth (PiStorm3D's `tries[]`
  ladder: now `open_screen`, each of 24, the mode's report and 32 tried once), the tag lists
  of the fullscreen input window and of ogles2's own window (now tag arrays,
  `OpenWindowTagList`; the values are what the user sees, so they stay), the copy to a window
  (`ogles2_display_present`, one ClipBlit), and two comments: job.c's on the job generation
  (v3dcore's text on `g_v3d_cl_generation`, word for word) and display.c's quoting PiStorm3D
  on stale cache lines; job.c's header no longer calls the file a port. Judged independent or
  only alike in structure, where the task fixes the order of steps (Mesa's code has the
  same): job.c's flush and finish, `ogles2_draw_clear` (a clear drawn as a rectangle),
  texture.c's `before_change` (renaming), the CPU's glCopyTex* path, frame.c's choice of a
  drawn clear; one `AllocBitMap` call (the only way to write it). program.c, the
  conversions of texture.c and the private headers had nothing of PiStorm3D's. The seven
  files are MIT; `LICENSE` lists the Hyperion files anew: v3dcore/, the V3D backend's
  job_v3d.c, draw_v3d.c, texture_v3d.c, fbo_v3d.c, program_v3d.c, builtin_shaders.h and
  tools/builtin_shaders/. Found on the way: m1-5's smoke test had checked since M2 that
  glGenerateMipmap is a stub (two FAILs with step 7's library too, run to check; the call
  has not been a stub since M2, so earlier libraries fail them alike); it now
  checks glUniform1i without a program (GL_INVALID_OPERATION): 59 PASS, none FAIL, as in
  October. Pi 4 checks the same day: m1-5 (all four surfaces) 59 PASS; m5-1 255 PASS, no
  FAIL; m7-6 36 of 36; m2-5 14 of 14 identical; m2-4 146, m7-4 129, m7-5 162 PASS, none FAIL;
  speed against step 7, alternating: gears as step 7
  (bitmap arrays 775, 773, 778 against 780, 773, 771; window 447, 446, 452 against 451, 446,
  446), Quake III 88.3, 88.0, 87.4 fps against 89.0, 88.5, 88.2 (0.5 to 0.8 below in each
  pair; this step changed nothing a timedemo frame runs: comments, and the opening and the
  copy to a window, which a fullscreen timedemo does not use; for the acceptance to judge).
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
  **Run 2026-11-04** (library of bc44871, against 1.3.3's from `release/dist/ogles2-1.3.3.lha`,
  the same evening): speed, five runs each alternating (`REF_LIB=… p2_speed.sh 5`): Quake III
  89.1, 89.0, 88.5, 88.9, 88.4 fps against 89.2, 88.6, 89.0, 88.1, 88.5 (equal: step 5's 2 %
  was against a fast stretch of step 4's); gears within 1 % (bitmap immediate 432-434 against
  428-430, arrays 811-814 against 813-818, window 461-465 against 463-466, screen 1081-1095
  against 1089-1104); SDL (m6-4) 549-557 and 327 fps against 573 and 333 (testgles2,
  sprites), the others within 2 %, vsync 50 fps with either (the station's screen runs at
  50 Hz now: 60 in October); glmark2 (m6-7) 260 and 260 against 253. The packages, every one
  without a FAIL: m1-5 59, m2-4 146, m5-1 255, m6-6 389 (all its parts; October's run had
  only the last, 93, the same today), m6-8 121, m6-10 210, m7-2 66, m7-3 105, m7-4 129, m7-5
  162 PASS, m7-6 36 of 36, m2-5 14 of 14 pictures identical. dEQP-GLES2's must-pass list
  (`tests/a1200/p2_deqp.sh`: every group, and multisample in a 4x MSAA context), run
  2026-11-04 from 20:58, shaders compiled on the Amiga: **51 of the 59 groups done, each the
  same as October's** (passes, fails, not supported and warnings;
  `tests/a1200/results/p2-deqp-20261104-2058.txt`), through `shaders-return`. **Finished 2026-11-05, every group
  the same as October's: P2's acceptance is done.** The rest ran with the shaders compiled on
  the PC through the compile bridge (port 6803: `ogles3`'s bridge to the same station uses
  6802), `shaders-scoping` to `state_query` in minutes each (`p2-deqp-20261105-0905.txt`),
  then `texture`, `uniform_api`, `vertex_arrays` and the 4x MSAA `multisample` run under
  `deqp_watch.py` (`p2-deqp-20261105-1120.txt`, its `-watch.txt` the case-by-case log), ported
  from `ogles3` with amictld 1.4, dEQP's progress patch (`ports/deqp/patches/0005`) and
  `Run-M1 PROGRESS=1`: every 15 s the case running, the library's new log lines and the free
  memory, a hang reported with its case. **All 60 parts (59 groups and the MSAA run): 17,132
  of 17,181 pass, 25 fail, 18 not supported, 6 quality warnings, each group's counts equal to
  its last run before; `texture`'s 24 failures the same cases as on 2026-10-19.** The A1200
  stopped answering twice during the day, both times in a long group under the blind runner
  (`shaders-scoping` in the night, `texture` in the morning: no ping, then a reset); under the
  watcher `texture` ran its 807 cases in 44 minutes without it, so no case hangs it reliably.
  Found on the way: `amictl runpkg` without screenshots holds the station in one RUN for the
  whole job, so a killed client leaves the server blocked until the job ends (the watcher's
  START and short WAITs do not); it was freed by sending dEQP a Ctrl-C through the input
  channel (``Break `Status COM=deqp-gles2` C`` typed into Execute Command). The station's tools
  are now the shared repository `ladislawv/amictl` (`tools/amictl`, a git submodule; the
  compile bridge, this project's own, in `tools/compile_bridge/`).
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
  - **Done (2026-11-04, P2 step 8, §5.1):** the front end's PiStorm3D parts found by an audit
    rewritten, its files MIT, `LICENSE` updated. Still to make opaque when VC4's are written
    (P5): `texture_private.h` and `ogles2_internal.h` name v3dcore's types (`V3DTexture`,
    `v3d_mem`, `struct V3DContext`); a type, not code, but an MIT header should not need a
    Hyperion one.
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
   (61 to 74 °C; the Pi 3 throttles at 80 °C, so a heatsink before long speed runs: fitted
   2026-11-05 with a fan, 53 to 55 °C through a 37-minute burn, §3.1). Mesa's
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
  a triangle; then the library. **Prepared (2026-11-05) as the package `tests/a1200/p5-1`**, with
  vc4core's own cases instead of the probe: `tests/vc4_draw` (the Makefile: `tests/pi3/vc4core`'s
  `draw.c` and `vc4p.c` with vc4core and `os_amiga.c`) draws the triangle, then every case, against
  Mesa's pictures from the test Pi 3 (in the package), nothing shown; `os_amiga.c` logs what it read
  from the device tree (the `/soc` mapping, the RAM's bus address, the mailbox, the 3D core, the
  timer, the RAM ranges) for this section; then the sync-logging library's smoke test and
  `m1_clear BITMAP` through the VC4 backend. `vc4_draw` refuses to run without a VideoCore IV in
  the device tree. Two stores that assumed a little-endian host were found and fixed on the way
  (`vc4_mem_write`, the test loader's code copy).
- **Run 1 (2026-11-05), the A1200 with a Raspberry Pi 3 Model B Rev 1.2, Emu68 on CaffeineOS;**
  the steps run one at a time through the station (Wi-Fi, below):
  - **Emu68's facts:** `/soc` 0x7e000000 at 0xf2000000 (68k); RAM 0 is bus 0xc0000000 (68k
    addresses are ARM physical ones); the mailbox at 0xf200b880, the 3D core at 0xf2c00000, the
    timer at 0xf2003000; the device tree's memory node one range, 0x01000000-0x2e000000 (what
    Emu68 gave AmigaOS); V3D 2.1, 3 slices of 4 QPUs, VPM 12 KB. The firmware's split (mailbox):
    the ARM's 0x00000000-0x3e000000, the VideoCore's 0x3e000000-0x40000000. **The RTG driver's
    bitmaps (screens) lie outside the memory node**, in the ARM's part the firmware keeps for its
    framebuffer (the Workbench screen at 0x38e00000, a 640x480 screen at 0x395e9000), mapped
    1:1: the GPU reads them at 0xc0000000 + the address word for word (`vc4_draw peekscreen`,
    loads only); so `vc4_os_gpu_reachable` takes the mailbox's two ranges too.
  - **vc4core's cases:** the triangle first drew nothing: `vc4p_emit` put the vertex data
    together as little-endian bytes, so the 68k's float positions reached the GPU byte-swapped
    (fixed: 32-bit components as the host's words, bytes in memory order; the half-float planes
    likewise). Then every case as Mesa's pictures, MSAA, the two-job loads and the overflow one
    included, except the 10 checks against Mesa's mipmap picture, missing from the package (made
    on the Pi 3 under Linux, not copied before the card moved; `build.sh` takes a copy with
    `REF=`). `rtt` crashed the A1200 (SmartCrash, line-F at a wild PC): two `struct vc4p` (165 KB
    each) on a 200 KB stack; now static.
  - **VC4's raster stores ignore the address's low 4 bits** (`vc4_draw rasteroff`: a base 4, 8,
    12 bytes past 16 stores at the base rounded down). An application's `AllocBitMap` gave
    0x08f298b4 and the window mode's render bitmaps 0x...7cc, 0x...53c (fast RAM, after
    the allocator's header): the first clears wrote 4 bytes over that header and the program
    crashed freeing the bitmap (twice, "software failure"). The VC4 backend now stores such a
    frame (or one outside the GPU's memory) into a buffer of the context's in the bitmap's layout
    and copies it in after the render, under the lock; screens are on 16 bytes and drawn into
    directly.
  - **The library:** the smoke test passes (its renderer and semaphore checks made card-aware:
    "VideoCore IV V3D 2.1 (PiStorm)", "VC4 submit"); **`m1_clear`, every mode: 58 of 58** (own
    window 1.6 ms a clear and swap, application window 1.3, application bitmap 0.5, fullscreen
    0.6, screen window 0.6).
- **The packages (2026-11-05),** run by `tests/a1200/p5_run.sh` (the sync-logging library, the
  programs compiled on the PC through the bridge: a stand-in `libs/v3dglslc` of a few bytes,
  since the library asks a compiler only when it finds one's file and the real one, 19 MB, is
  slow over Wi-Fi; `OGLES2Cache:` a drawer on the station's disk for the run, so the compiled
  files do not fill `T:`, the RAM disk, which the packages' memory checks count):
  - `m1-5`: 59 PASS, none FAIL, as on the Pi 4.
  - `m2-4`: 148 PASS, none FAIL, after: `GL_OES_element_index_uint` listed (16-bit indices
    while a draw spans 65536 vertices; the CPU's split of wider ones left); **`UBO0_ADDR` plus
    the recipe's offset** (Mesa's reloc adds it: indirectly indexed uniforms such as skinning's
    `bone[i]` read from the buffer's start before); the debug link log's check card-aware (VC4's
    binary has a triangles fragment variant, others by key); the built-in clear program loaded
    with the device (it stays while the library does; loaded at a context's first masked clear
    it counted as that context's memory); the front end's "no fragment shader variant for
    primitive" not logged on VC4 (its draws choose their own variants by key). Each program is
    compiled on the PC in 0.1 to 1 s a request (`.vc4p`, then `.vc4k` as draws add keys).
    **`ERRSTAT` VCMRE (bit 13, 0x2000)**, the render side's twin of VCMBE, after 8 of the draw
    test's renders, the pictures right: watched (not failed, as VCMBE).
  - `m5-1`: 257 PASS, none FAIL, after: **a mid-frame flush when a job nears the end of its
    blocks** (`VC4_CL_BLOCKS`, 16 of state and of binning list: m4_stress's frame of 100,000
    draws ran out and was dropped; now 20,746 draws/s, the picture right; the Pi 4's lists grow
    instead); **RGBA4 and RGB5_A1 renderbuffers stored as RGBA8 on VC4** (its tile buffer
    stores RGBA8888 or RGB565 only; GL allows the precision; the size queries answer from the
    format).
  - `m7-6`: 36 of 36.
  - The station on Wi-Fi: a 19 MB `PUT` (m6-6's `compiler/v3dglslc`) stalled `amictld` until
    the A1200 was reset; `p5_run.sh` puts the stand-in over every compiler in a package. Twice
    more the link itself dropped during an upload (`wifipi.device`: the Amiga's screen still said
    Online, no ARP answer, "unable to connect" until a restart). amictl since (the shared
    repository, `tools/amictl`): the server drops a client silent or stalled for 120 s
    (`SO_RCVTIMEO`; it had only the send timeout), the client gives up after 15 s with one
    "STATION DOWN" line and status 4, a `PUT`'s time limit grows with its size, and `runpkg`
    waits in 15 s steps printing a line a minute; `p5_run.sh` stops a batch at status 4 and
    ignores result directories older than its run. (The server serves one connection at a time:
    a second client during a `runpkg` times out, so nothing polls it meanwhile.)
    **The station moved to amigent** (github.com/akustikrausch/amigent, MIT; at the developer's
    request a fork in `C:\- CPP\amigent`): amigentd's command channels each run in a process of
    their own (up to four; a fifth pushes out the idlest), new files are written under a temporary
    name and renamed, EXEC has a time limit. The fork: port 6502 (amigactld 6501, TLS 6503: off
    amictl's 6800 to 6803), builds with bebbo's gcc as well as vbcc (gcc's build without TLS),
    READ_AT (amictl's GETFROM), BREAK (ABORT), REBOOT, the ALLOW and LOG options, and
    `amigent-ctl`, amictl's command line on it (files with CRC-32 skips and an optional rate,
    run, start, abort, tail, env, shot, keys and mouse, up, reboot, restart, runpkg with a
    heartbeat from a second channel and a lock against a second run of a package). It runs from
    `Stuff:amigent/`, started after each boot (not in `S:User-Startup`). `tests/a1200/p5_run.sh`
    drives it: the bridge's `v3dglslc-remote` detached (`Run >NIL:`, stopped by `Break` on its CLI)
    so it holds no channel; one batch at a time (a lock). **The cause of the drops: a weak Wi-Fi
    signal at the A1200** (fixed at the access point, 2026-11-05); `tests/a1200/p5_netprobe.py`
    (each step of a package alone, the link checked after each) cleared m2-5's 14 programs.
    Then the drops came only when the Amiga sent megabytes (m2-5's results, 4.6 MB): with 32 KB
    reads or whole files the link went, rate or not; in 8 KB reads it held at 50, 150 and
    400 KB/s (4.6 MB in 16 s, no ping lost): `amigent-ctl` reads and writes in 8 KB pieces
    (`AMIGENT_PIECE`), `p5_run.sh` at 400 KB/s (`AMIGENT_RATE`).
  - **The regression set on the Pi 3 card, 2026-11-05 22:25** (amigent, today's fixes): m1-5
    59/0, m2-4 148/0, m5-1 257/0, m6-8 121/0, m7-2 66/0, m7-3 105/0, m7-6 36/0 (as the Pi 4);
    m6-6 385/6 (half floats with negative values, 3 checks run twice); m6-10 207/3 (4x MSAA:
    depth and stencil across a mid-frame flush); m7-4 119/10 and m7-5 135/27 (`EXT_draw_buffers`,
    `EXT_color_buffer_half_float`: their library passes are left); m2-5's 14 pictures against
    the Pi 4's: Stencil_Test 0 pixels off by more than 8 (67,200 before the write-mask fix),
    ParticleSystem 3 and Orientation 16 (point sprites: 26,591 and 3,374 before the point-size
    fix), MipMap2D 2,296 (VC4's LOD rounding where levels change), the rest edges (under 110).
    Fixed for it: a stencil write mask of 0 (VC4's full-mask word 0 means none: KEEP then), and
    points' variants with the per-vertex size (the search took the one without, which drops the
    shader's gl_PointSize).
  - `m6-8` 121 of 121, `m7-2` 66 of 66, as on the Pi 4.
  - `m6-6` (the extensions) 367 PASS 24 FAIL at first, after the fixes 379 and 12 (6 distinct,
    each run twice): **every variant with a patch 0019 uniform (types 25 to 29) was refused at
    load** (`program_vc4.c`'s last known type was 24) and its draws took the closest variant;
    **the variant search ignored a sampler's wraps** (bytes 9 to 11: only CLAMP and BORDER changed
    vc4's code before patch 0019; now wrap 6, mirror clamp, must agree) **and the vertex key's
    `clip_halfz`** (patch 0020: m7-3's `GL_ZERO_TO_ONE` 4 failures, now passing); **a base level
    above 0 with a non-mipmap filter read level 0**: the TMU takes the explicit LOD only with a mip
    filter, so it is NEAREST_MIP_NEAREST (LINEAR_MIP_NEAREST) then; `ARB_texture_rectangle` listed
    (drawn right already); `glPolygonMode` emulated (a triangle draw's vertices as points, or its
    edges as a line list made on the CPU; face culling cannot come first, so a culled face's edges
    are drawn: a deviation); the tests' anisotropy (4) and rectangle size (2048) card-aware.
    `vc4_draw smallmip` (4x4 to 32x32, every level to 1x1, LT and T) found the TMU's level
    addresses equal to `vc4_texture_layout`'s. Both left here passed on 2026-11-06 (391 of 391):
    float textures with negative values (the TMU drops the sign: §2 "Textures", patch 0022), the
    error after a float `glGenerateMipmap`.
  - `m6-10` 206 PASS 4 FAIL: depth and stencil across a mid-frame flush with 4x MSAA (3: the
    full-resolution tile store and load, left), and the first context's memory (the built-in
    clear, below).
  - `m7-3` 104 PASS 1 FAIL after the fixes; `m7-4` 118 PASS 11 FAIL and `m7-5` 134 PASS 28
    FAIL: `EXT_draw_buffers` (10 each) and `EXT_color_buffer_half_float` (17), whose library
    passes are left (§10), and the memory check.
  - **The built-in clear program and the memory checks:** loaded at a context's first clear of
    part of a frame, kept, and its variants for a partial colour mask or a four-float position
    compiled at run time and kept: the packages counted them against that context (18 to 33 KB).
    Its keys now hold every colour mask with the depth and stencil tests off and both position
    sizes (`tools/clear_vc4/keys.c`: 24,248 bytes, from 9,768), and it is freed with the last
    context.
- **GPU reachability:** `ogles2_device_gpu_reachable` (DEVELOPMENT_PLAN §7.3) checks the RAM
  ranges on both; `vc4core` turns a 68k address into a bus address through the alias.
- **The test station:** `tools/amictl` as now; the same `tests/a1200/` packages on both cards, run
  with `runpkg`. **Found 2026-11-05, first boot with the Pi 3 card:** the A1200 is not on the
  network (no ping at `192.168.1.128`, no new host on the LAN). Its Roadshow interface is Emu68's
  `genet.device`, the Pi 4's Ethernet; the Pi 3's Ethernet sits behind its USB hub, for which
  Emu68 has no driver. The developer set up Emu68's `wifipi.device` (the Pi 3's Wi-Fi) as
  Roadshow's interface on this card: the A1200 answers at the same address (ping 50 to 100 ms
  against about 1 ms on the Pi 4's Ethernet; uploads correspondingly slower), so the station
  and `runpkg` work on both cards.
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
- **PD10 (2026-11-07): betas from `ogles2pi3` first.** The Pi 3 work is released as 1.4 betas from this
  repository (library version 1.4, `OGLES2_RELEASE` "1.4-beta1"); P7, the merge into `ogles2` and the
  release from there, is postponed until the developer says so.

## 10. Status and next steps

**Where it stands, for whoever picks it up (2026-11-07, branch `main`; `pi3-p5` merged into it):** the Pi 3 side is
finished and tested on the card. P5 (the VC4 backend) is done; P6 on the Pi 3 card is done: dEQP-GLES2's
whole list 17,047 of 17,165 pass, its 18 failures all in Mesa vc4's own baseline (item 6 below); every
regression package without a FAIL; Quake III 32.8 fps, glmark2 70; glCopyTex* and glGenerateMipmap on
the GPU; a program compiles once (at its link) for the usual first draw; the disk cache has a size. **The
Pi 4 card's regression and speed against 1.3.3 are done too (2026-11-07, the last entry below): every
package as before, Quake III as fast, gears about 1 % slower; dEQP-GLES2's whole list as the Pi 4's
earlier runs (one case once, not reproduced).** The details are in the dated entries below,
the newest last. **Next, in order:**
1. (Done 2026-11-07.) The Pi 4 card: regression and benchmarks. To repeat it: swap the card (a reboot);
   through amigent with the PC compiling (`BRIDGE_PORT=6804 bash tests/a1200/p5_run.sh <packages>`, as on
   the Pi 3 card: the Pi 4's amigentd answers on the same address), or `bash tests/a1200/p2_checks.sh`
   through amictl (the Amiga compiling: slower); the speed against 1.3.3's library
   (`git show v1.3.3:release/dist/ogles2-1.3.3.lha`'s `Libs/ogles2.library`) with `LIB=<library> p5_run.sh m6-1` and
   `m6-5 ARGS=FRAMES=0`, alternating, or `p2_speed.sh` with `REF_LIB`. dEQP on the Pi 4 is optional before
   P7 (its own baseline is in `DEVELOPMENT_PLAN.md`); `RAMCACHE=1` shortens it (item 6).
2. (Done 2026-11-07.) **Release 1.4-beta1 from this repository (PD10, below)**: tested on both cards,
   its archives in `release/dist/` (1.3.3's in its tag), tag `v1.4-beta1` (a14831e) and a GitHub
   pre-release on `ladislawv/ogles2pi3` with the six archives and `SHA256SUMS`; nothing in `ogles2`.
3. **P7: into `ogles2`** (PD8, §8), **postponed by the developer (2026-11-07)**: merge this branch there
   (the build root back to `~/ogles2-build`), check both cards again, then the release
   (`release/build_release.sh`, one library and one compiler for both, one whole dEQP pass as the
   release's check). Nothing is pushed to `ogles2` before the developer says so.
4. Optional: Mesa's mipmap picture for `vc4_draw` (the Pi 3 under Linux, §6); the Wi-Fi drops of the
   A1200's link on the Pi 3 card (cause unknown, the station's and not the library's; the scripts work
   around them); ERRSTAT VCMRE, logged for some renders with right pictures (item 5).

How to work on the card: CONTRIBUTING.md, "The Pi 3 card and the amigent station" (packages with
`p5_run.sh`, dEQP with `p6_deqp.sh` and `p6_watch.sh`, why a program compiles at a draw with
`p6_keydiff.sh`), and its traps; CLAUDE.md for an agent. On a machine other than the one the scripts
were written on, read that section's "Another machine" first.

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
1. Done: the test Pi holds 5 V (another supply, 5.2 V; §6.0); a heatsink and fan on it
   (2026-11-05: never throttled in a 37-minute burn, §3.1).
2. P2's backend interface and the MIT rewrite of the shared front end's PiStorm3D parts, on
   the Pi 4 card (PD7 decided: option 1): steps 1 to 8 done (§5.1, §5.3); **the acceptance done
   (2026-11-05)**: the packages, the speed and dEQP-GLES2's must-pass list all as 1.3.3 and
   October (§5.1; dEQP's last groups compiled on the PC through the bridge and watched by
   `deqp_watch.py`). **P2 is finished.** Checked again on the library of 2026-11-05 (bf27a90:
   the VC4 emulations, P2's PTYPE): every package as on 2026-11-04 (m5-1 255, m7-6 36 of 36,
   m2-5 14 pictures identical, m1-5 59, m2-4 146, m6-6 389, m6-8 121, m6-10 210, m7-2 66,
   m7-3 105, m7-4 129, m7-5 162 PASS, none FAIL), gears within 1 %, Quake III 89.7 fps
   (`tests/a1200/results/p2-checks-20261105-*.txt`).
3. P4: the limits are settled (§4.1: `-L`, the VC4 backend's caps take them); the corpus
   compiled for VC4 as a whole is done (36 of 36, §4.3, with the draw-buffers variants of patch
   0016); variants compile from given keys (`-k`, patch 0017, §4.2); the request's device field
   and S2 with both drivers are done, S1 and S2 identical (§4.5); the key builder of the VC4
   backend is done and equal to Mesa's (§4.2). **The key manifests' path is done (2026-11-05)**: probe R
   (`ENV:OGLES2/Probe` R, `ogles2/vc4/drawkey_vc4.c`) writes each program's distinct key pairs, as the
   VC4 backend builds them (one key builder for the backend's draw and the probe), to
   `T:ogles2-vc4keys/<hash>.keys` on either card; `vc4_corpus.sh` with `KEYS=<dir>` compiles a
   program with its file (`-k`; checked on the PC: 36 of 36 written, a program with a manifest
   3 variants alone and 17 with it). **The manifests themselves (2026-11-05)**, from runs on
   the A1200 with the Pi 4 card and probe R (`tests/a1200/p2_vc4keys.sh`'s packages with
   today's library: the corpus m2-5, SDL2's and SDL3's tests, Quake III with both renderers):
   47 files (`tests/a1200/results/vc4keys-20261105-1341/`, a record per draw state: a fragment
   key and its vertex key, 596 bytes); `vc4_corpus.sh` with them finds the manifests of all 18
   corpus programs, each then compiled with 6 or 9 variants (3 without); the other 29 are SDL's
   and Quake III's programs, outside the corpus script (the release's examples and Boing 2026
   ran in no probe package). Left: the
   version-2 server path and S2's time and memory on the A1200 with the Pi 3 (P5), and patch
   0017 and the others checked for big-endian differences there (the comparisons so far ran
   on vamos, which is big-endian: they cover it).
4. P3: `vc4core` on Linux (§3.1): the core, its texture layouts and the tests' program loader
   done; §6.3's list against Mesa's pictures done but for the items below; the long soak (§3.1)
   and `ERRSTAT`'s one error (`VCMBE` when the binner runs out of memory) done; first GPU-only
   numbers (§3.1); **clears of part of a frame done (2026-11-05)**: the `pclear` case draws
   them as the VC4 backend does (its built-in clear program, `tools/clear_vc4`, a rectangle in
   the clip window: colour written without the depth test, then depth with the colour masked and
   the test ALWAYS), then the depth case's LESS against them: Mesa's picture (its scissored
   glClears) pixel for pixel; found on the way that a colour-masked clear's vertex variant writes
   no varyings, so a vertex variant must be paired with its fragment one by the inputs (as the
   library does; the tests' loader now too). All cases and a 300-round soak (8,160 jobs) clean.
   **Mipmaps (2026-11-05):** the `mipmap` case samples a 32x32 texture's six levels in vc4core's
   layout (`tiling.c`) with NEAREST_MIPMAP_NEAREST through quads of 32, 16, 8 and 4 pixels (each
   shows its own level): Mesa's picture pixel for pixel; with it all cases and a 200-round soak
   (6,040 jobs) clean.
   The GPU lock needs no `lock.c`: the VC4 backend keeps its own public semaphore
   (`"VC4 submit"`, backend_vc4.c), and the Linux tests are the GPU's only user.
   **The Linux bench's rest (2026-11-05), all on the Pi 3 in the bare boot:** the emulations'
   GPU side (§2.1) with compiler patches 0018 to 0021 and new `vc4_draw` cases: `texhalf`,
   `halfmip` (half floats as S16F planes), `mirror`, `mipemu` (bias, min and max LOD, base and
   max level, a forced level, anisotropy), `anisoquad`, `clipz`, `hdr`, `hdrblend`, `hdrrange`
   (the 64-bit tile buffer), `drawbuf` (`EXT_draw_buffers`' passes), `genmip` (mipmaps drawn on
   the GPU, glGenerateMipmap's path) and `copytex` (glCopyTexSubImage2D's from a T-format
   image); every case against Mesa's pictures (mapped where GL picks other levels) or texels
   worked out; all cases and a 200-round soak (11,440 jobs) clean. Found on the way: P2's
   `BSLOD` counts only with `PTYPE` 1 (the library wrote 0 for 2D textures: an explicit LOD,
   every vertex-shader lookup, was added to the TMU's own; fixed), the RGBA64 type's stale
   second word (so the planes), the cause of the not-reset core's lost tiles (`VPMBASE`
   changed after jobs, §2 "Power"), the TMU's bilinear rounding (§2 "Textures"). The library
   side done with them: the keys' new bytes (`key_vc4.c`, `drawkey_vc4.c`), the half-float
   planes, the uniforms of patch 0019, P2's `PTYPE`, the extensions offered (`OES_texture_half_float`,
   `_linear`, `OES_texture_float`, `ARB_texture_mirror_clamp_to_edge`, `EXT_texture_lod_bias`,
   `SGIS_texture_lod`, `EXT_texture_filter_anisotropic` with 4, `EXT_clip_control`); left for P5
   on the A1200: `EXT_draw_buffers`' passes in `job_vc4.c`, half-float targets (renderbuffers,
   texture levels, the CPU split into planes before sampling), the GPU paths of glGenerateMipmap
   and glCopyTex* (a built-in blit program), and checking all of it there. **Nothing is left on
   Linux**: the speed runs and the burn with the heatsink done too (§3.1). Then P5 (the VC4
   backend); begun
   2026-11-05 with what the PC and the Pi 3 can test: `vc4core/os_amiga.c` (written, compiles
   for the 68k, waits for the A1200 with the Pi 3: the device tree's `/soc` ranges and
   dma-ranges, `/aliases`' mailbox, fast RAM as GPU memory cleaned by `CachePreDMA`, the system
   timer; vc4core now flushes its lists and state before a job, a no-op on Linux) and the key
   builder (§4.2). Then `ogles2/vc4/backend_vc4.c`, the VC4 backend's table: the capabilities
   (§2's Pi 3 limits, the extensions done natively), the device on vc4core (take-over at the
   first context, its own public lock `"VC4 submit"`, the RAM it reaches from the device tree's
   memory nodes), GPU memory through vc4core as the front end's `struct v3d_mem`, a context's
   pools and job (kept in `ctx->v3d`, an opaque handle to the front end); the other groups
   refuse until written; then programs' first part (`program_vc4.c`: a `VC4_VARIANT`'s key,
   kept as a pointer into the file for the choice by key, and prog data, in a 48-byte area
   `struct OGLES2Variant` now has for a backend's own; the recipe's types checked; a first
   choice by stage and primitive). Built into the library (`libvc4core.a`, 267,352 bytes;
   the float-conversion check covers the new files) but **not chosen at device open yet**: the
   Pi 4 runs the V3D backend as before (its checks on the A1200 are owed with the next run).
   **Then (2026-11-05) the backend's other groups, written and built, untested until the A1200
   has the Pi 3 card** (the library 289,096 bytes; still not chosen):
   - jobs (`job_vc4.c`): vc4core's `vc4_job_run` split into its steps (`vc4_job_end_lists`,
     `vc4_job_bin`, `vc4_job_render_start`, `vc4_job_render_wait`; they log and reset nothing,
     the caller does: a bitmap may be locked), so a render runs on while the CPU builds the next
     job, as on the Pi 4 (M6); two jobs a context, one building while the other renders, and the
     blocks freed after a job in the slot that renders after any job that may read them. The
     default framebuffer is rendered straight into the bitmap with the frame as wide as its row
     (`BytesPerRow` / 4, a multiple of 16 bytes; the context's pools have a tile column more for
     it); a bitmap with other rows drops the frame with the reason logged (the copy from a
     buffer of our own: left). Depth and stencil kept across a mid-frame flush in a Z24S8 T image
     (not with 4x MSAA: the general store keeps one sample; left). The GPU's ERRSTAT logged after
     the render, VCMBE excepted (§3.1); nothing is logged under the bitmap lock;
   - draw (`draw_vc4.c`): the draw's keys built (`key_vc4.c`) and the program's variants
     chosen by them (`ogles2_vc4_variant_find`: wraps and the comparison function aside, a vertex
     variant paired with its fragment variant by the inputs it writes, gl_PointSize's flag tried
     both ways since the front end does not know it); **a key the program's file lacks is
     compiled on the miss**: the program's keyed image (`program.c`, `ogles2_image_keyed`: its
     `<hash>.vc4k` in `PROGDIR:shaders/`, `PROGDIR:` or the disk cache, loaded at the first miss;
     else compiled into the cache by a version-2 `LINK` or `LINK_BOUND` request, §4.5, with the
     records of every variant it held and the draw's two, the bindings kept for it), the draw
     waiting for the compile; each set of keys requested once (FNV-1a), a key no compile gives
     drawn with the front end's choice, logged once a program. The records as the library makes
     them (the file's order, coordinate variants left out, a vertex key's fs_inputs as none)
     give the same variants as drawing in those states, 5 of 5 programs
     (`tools/v3dglslc/test/vc4_keys.py`, its second check). The compiler's requests carry the
     device on the Pi 3 (version 2); the Pi 4's stay version 1. A program from
     glProgramBinaryOES is loaded from the copy the library keeps (a VC4 image's keys point into
     its file); no `.v3dw` or `.v3da` is looked for on VC4 (its fragment key holds those states);
     a draw whose two keys equal one of the image's last four (a hash, then every byte) takes
     their variants without the search (which compares 292-byte keys variant by variant: too
     slow on the 68k for every draw); the cache is the image's, freed with it and dropped when
     its keyed image is replaced;
     the
     uniform streams of all of vc4's recipe types GLES2 uses (stencil words as Mesa's
     `vc4_state.c`, the blend colour in the target's byte order, the sample mask, the stream's
     own address); vertex data in the GPU's byte order (the key holds the types; a disabled
     array's value as four floats), the shader record, the state packets (clip window,
     configuration bits with early Z as Mesa's, depth offset, viewport); array draws over 65535
     vertices split as Mesa's `vc4_draw_vbo`; 32-bit indices drawn as 16-bit while they fit;
   - textures (`texture_vc4.c`): the storage table's formats packed from the RGBA8 RAM copies
     (BGRA and ETC1 arrive converted: stored as RGBA8 and RGB8), the miptree in tiling.c's layout,
     whole levels by `vc4_texture_store`, boxes texel by texel, loads back after a job drew into
     one, renaming, a level as a render target (RGBA8888, BGR565), the TMU's words P0, P1, P2
     for the uniform stream; float textures not stored (left). **S3TC (2026-11-05)**: the
     blocks decoded on the CPU as they are stored (`ogles2/vc4/s3tc.c`, our own decoder: equal to
     Mesa's texel for texel on 20,000 random blocks of each of DXT1 RGB, DXT1 RGBA, DXT3 and DXT5,
     `tests/pi3/s3tc`), into RGBA8; the four S3TC/DXT names offered. **Depth textures
     (2026-11-05)**, as Mesa's vc4 has them: Z24S8 words read as RGBA8888 that the shader scales
     (the key's X8Z24 for `DEPTH_COMPONENT` of either type, S8_UINT_Z24 for `DEPTH_STENCIL`,
     sampled as luminance: `-s texd24,texds,texd16` in the compiler, the key builder equal to
     Mesa's); packed from the RAM copies' depth words, a level as a job's depth and stencil image
     (`tex_zs_surface`), read back after a job; sampled nearest (Mesa filters the word's bytes);
     `GL_OES_depth_texture` offered. The `depthtex` case on the Pi 3 (the depth case's triangles
     into a depth texture, then drawn from it) gives Mesa's picture pixel for pixel;
   - framebuffer targets (`fbo_vc4.c`): every depth or stencil renderbuffer a Z24S8 T image
     (the tile buffer loads and stores the two together), a framebuffer object's own pools when
     its frame has more tiles across or down than the context's (`vc4_pools_layout`);
   - the built-in image (clears of part of a framebuffer, `ogles2_draw_clear`): a program with
     the V3D one's interface (a matrix uniform, position and colour), compiled for the Pi 3 with
     the variants of the usual clear states (`tools/clear_vc4`: RGBA8 and BGRA8 targets, depth
     and stencil tests on or off, colour written or not; keys made by the backend's key builder)
     and embedded as a `.vc4p` image (`ogles2/vc4/builtin_vc4p.h`, 9,768 bytes) with its
     sources, so a clear in another state compiles its variants like any program's;
   - **the choice at device open** (`device.c`): the VC4 backend when the device tree's root
     `compatible` names a BCM2835, 2836, 2837 or 2710 (`vc4_os_present`), else the V3D one as
     before (the Pi 4's BCM2711, or a tree that does not say);
   - refused still: glGenerateMipmap and glCopyTex* on the GPU (the front end's CPU paths
     run), the internal programs.
   vc4core's cases on the Pi 3 still all pass after the split (500-round soak, 13,100 jobs).
   The library is 301,740 bytes. A review pass found and fixed two things before any run: a
   framebuffer object's target format for the keys is looked up with its surfaces at the job's
   start (job.c's target has none yet: an RGB565 texture would have drawn with the RGBA8 keys),
   and the display keeps the row length its probe read (the first job sizes its frame by it
   before any lock: a padded row would have dropped the first frame); glCopyTex* falls back to
   the CPU without a log line each time. To verify at the first run with the Pi 3 card: that Emu68 maps
   the peripherals at 0xF2000000 there too (the front end's clock reads the system timer at a
   fixed address), the winding bit (the V3D backend's rule), the point coordinates' origin.
   **P5 run 1 done (2026-11-05, §7):** Emu68's facts, vc4core's cases and the library's clears
   in every display mode on the A1200 with the Pi 3 card (two crashes found and fixed: a stack
   overflow in `vc4_draw`, and VC4's stores ignoring an address's low 4 bits).
5. **Where P5 stands (2026-11-05, end of day; commit after 0ead642)** and the work queue for
   whoever continues. On the A1200 with the Pi 3 card the library runs real programs:
   m1-5, m2-4, m5-1, m6-8, m7-2, m7-3 and m7-6 pass as on the Pi 4; m2-5's 14 pictures match the
   Pi 4's within a few pixels (MipMap2D's level changes aside); **Quake III with ioquake3's GLES2
   renderer renders correctly, 24.2 fps over the "four" demo** (1,260 frames in 52.1 s, none
   dropped; the Pi 4 card 89.7): per frame 42 ms of CPU work before the job, 20 ms of it in
   116 draws (about 175 us a draw), the GPU 0.4 ms binning and 0.16 ms rendering, so the Pi 3 is
   CPU-bound (the 68k under Emu68 on the Pi 3's A53, and the VC4 draw path), measured with the
   sync-logging library (`tests/a1200/results/ogles2-m6-5-20261105-224557`). The station is
   amigent (§7 "The packages", CONTRIBUTING.md "The Pi 3 card"). **On 2026-11-06, with items 1 and 2
   below and the negative half floats done, the Pi 3 card's packages stand at:** m1-5 59, m2-4 148,
   m5-1 257, m6-6 391, m6-8 121, m7-2 66, m7-3 105, m7-4 129, m7-5 162, m7-6 36 PASS and none FAIL;
   m6-10 207 PASS and its 3 known FAIL (item 5); m2-5's 14 pictures PASS against Mesa's references (at
   most 0.18 % of the pixels differ). m5-1's three contexts that end with 40,960, 28,672 and 28,672
   bytes of GPU memory in use do so as on 2026-11-05. The A1200's Wi-Fi link dropped twice that day,
   for minutes each, in mid-run: the Amiga runs on and its compiles fail (`no bridge to the PC`), and
   the run's `runpkg.lock` stays until removed. In this order:
   1. `EXT_draw_buffers`' passes in `job_vc4.c` (§2.1: one binning list per colour attachment,
      each draw recorded into every list with that attachment's fragment variant; the GPU side
      checked on Linux, `vc4_draw drawbuf`): m7-4 and m7-5, 10 failures each. **Done (2026-11-06):
      on the Pi 3 card m7-4 passes 129 of 129 and m7-5 162 of 162
      (`tests/a1200/results/ogles2-m7-[45]-20261106-1108*`); owes the Pi 4 card's regression**
      (the V3D side has only two `device != 21` guards in `draw.c`). The compiler changed too
      (§2.1, patches 0016, 0021, 0022), so both `v3dglslc` builds are needed. The second build
      machine's Mesa tree was brought up to this repository (`vc4` configured, patches 0015 to
      0022; three objects outside vc4 changed, the driver's registration). The first runs' failures
      came partly from **another compile bridge on the network** (192.168.1.32, with the station's
      token and a compiler without the `gl_FragData[1]` fix) taking the requests: the newest
      connection to port 6803 wins. `v3dglslc-remote` now logs each bridge's address; a run on a
      port of its own: `BRIDGE_PORT` (CONTRIBUTING.md).
   2. Half-float render targets (`EXT_color_buffer_half_float`: the 64-bit tile buffer done in
      vc4core, `hdr` cases; the library's targets, renderbuffers and texture levels, and the CPU's
      split into S16F planes before sampling): m7-5, 17 failures. **Done (2026-11-06, §2.1), as item
      1.** Left: a sampler splits a level after waiting for this context's own render only, so
      another context's job drawing into a shared texture is not waited for there.
   3. The VC4 draw path's CPU cost (Quake III above). **Done (2026-11-06): 24.3 to 32.7 fps over the
      whole "four" demo**, a frame's draws 20.1 ms to 10.1 (shaders 6.2 to 1.0, the binning list with
      the indices 3.9 to 1.1, vertex data 5.0 to 2.9): the variants chosen by a signature of what the
      keys read, so the keys are built and searched only on a miss (`draw_vc4.c`; it must stay in
      step with `ogles2_vc4_draw_keys`); a state packet left out when it says what the list's last
      did; vertex data and indices by whole words without branches in the loops, swapped by
      `rol.w`, `swap`. What remains is mostly the game: `PROFILE=1` has all the library's calls at
      about 21 ms a frame, level loading included (10.3 of it `glDrawElements`), against 31 ms a
      frame; the draw stages not touched (textures 1.6, uniforms 1.9, checks 1.3) would give about
      2 ms more at most (`tests/a1200/results/ogles2-m6-5-20261106-1[01]*`). m5-1, m6-6, m7-4, m7-5
      and m2-5's pictures as before.
   4. The remaining packages on the Pi 3 card. **Done (2026-11-06) but dEQP-GLES2 (m7-1, with P6):**
      SDL2 and SDL3 (m6-3, m6-4: 91.3 fps a target texture), Quake III through gl4es (m6-2: 22.8 fps,
      the Pi 4 card 42.7 in October), glmark2 (m6-7: score 69, the Pi 4 card 251), m6-1, m6-9, m7-7
      (2), m7-8 (37), m3-1 (114), m3-2 (143), m4-1 (189), m4-2 (806 fps, 800 triangles), m2-1 (103),
      m2-2 (166), m2-3 (114), m1-1 (59), m1-2 (59), m1-3 (69), m1-4 (14), m1-6 (81), m1-7 (81), m1-8,
      m1-9 (26), m1-10 (74): none FAIL, no frame dropped, every context's memory back. `m1_draw`
      in fullscreen reads through `glReadPixels` now: read directly with QUICK, it saw the frame
      before, since a swap on a screen of the library's lets the render run on (M6), a few checks a
      run as the timing fell. Then the Pi 4 card's regression for the shared changes (`draw.c`,
      `fbo.c`, `display.c`, the card-aware tests: `p2_checks.sh`, expected as on 2026-11-05 morning).
   5. Smaller: 4x MSAA's depth and stencil across a mid-frame flush: **done (2026-11-06)**, the
      full-resolution tile store and load (`vc4_target.zs_full`, 16 KB a tile in tile order, as Linux's
      `vc4_render_cl.c`), m6-10 210 of 210; half floats with negative values: **done (2026-11-06,
      §2 "Textures", patch 0022: m6-6 391 of 391)**; 32-bit indices spanning more than 65,536 vertices:
      **done (2026-11-06)**, `draw_big` in `draw_vc4.c` draws the primitives as plain lists of at most
      65,535 vertices from copies on the CPU, loops and fans of arrays too (`m2_elements`' new cases,
      a list and a strip, client arrays and a static buffer, four vertices 70,000 apart);
      ERRSTAT VCMRE (bit 13) seen in some renders with right pictures: **narrowed (2026-11-06)** to
      draws of many small triangles (`m1_draw`'s timing run, one draw of 600 vertices in 320x240, about
      every such frame), the render-side twin of VCMBE; Mesa sets nothing for it either, and it stays
      logged (8 lines a run) until dEQP or a picture says otherwise; Mesa's mipmap picture for
      `vc4_draw` (the Pi 3 under Linux again); the GPU paths of glGenerateMipmap and glCopyTex*.
   6. S2 on the Pi 3 card: the 19 MB `v3dglslc` on the A1200. **Done (2026-11-06), no bridge:** the
      compiler in the packages' `libs/` for the whole run; the first shader 1.7 to 1.9 s with its
      start, a program about 0.3 s (the largest 2.5 s), about 30 MB while resident, gone after; keyed
      compiles at draws through the version-2 path (m6-6: 42, m7-5: 22). m7-5 162 of 162, m6-6 394 of
      395, m7-4 128 of 129, m2-4 150 of 152; the four failures are the tests' `destroy returns the
      memory` counting the resident compiler, as the packages' scripts warn
      (`tests/a1200/results/ogles2-m{2-4,6-6,7-4,7-5}-20261106-130*`).
   Then P6 (dEQP-GLES2 and the benchmarks on both cards, against Mesa vc4's baseline) and P7
   (into `ogles2`, one release for both cards).
6. **P6 under way: dEQP-GLES2's whole list has run on the Pi 3 card (2026-11-07), the shaders compiled on
   the PC** (`p6_deqp.sh`, summaries `tests/a1200/results/p6-deqp-20261106-{1307,1654,2255}.txt`): each of the
   17,165 cases' newest result, **17,047 pass, 18 fail, none beyond Mesa's baseline** (§3.1: `texture` 11,
   `clipping` 3, `uniform_api` 2, `multisample` 2 in 4x MSAA; Mesa's other two, `depth_stencil_masked` and
   `scissor.outside_render_line`, pass here), 92 not supported (Mesa 234), 8 warnings. These are not one
   run of one library: the groups run before a fix kept their results. No whole pass is owed for that: each
   fix touches a narrow path, rerun in the groups that exercise it; a later change reruns the groups it
   touches (one whole pass before P7's release is a check of the release, not of this). Mesa passes
   16,906. What the first groups found, all fixed and rerun on the card:
   - **an empty scissor box or viewport** (`color_clear` 9, `fbo.render.color_clear` 28): the clip window
     was clamped to no size, and the draw covered the whole target instead of nothing. It is skipped
     now (`draw_vc4.c`). `color_clear` 19 of 19.
   - **`GL_FIXED` attributes** (`draw.random` 6): `vc4_nir_lower_io` has no fixed type, so the attribute
     read as zeros (Mesa's vertex buffers convert before it). `convert` makes floats, and the key says
     floats (`key_vc4.c`); no compiler change.
   - **a line loop of two vertices** (`draw.draw_arrays.line_loop` 2): its line was missing, for a reason
     not found (Mesa passes these); drawn as two lines through `draw_big`. `draw` 101 of 101.
   - **the levels the calls may name** (`negative_api` 6): fixed at 13, for 4096; now
     log2(`GL_MAX_TEXTURE_SIZE`) + 1, so 12 on the Pi 3, still 13 on the Pi 4. 246 of 246.
   - **a stencil renderbuffer beside a recreated depth one** (`fbo.render.recreate_depthbuffer.rebind_*` 7):
     VC4 keeps both in the depth one's image (`fbo_vc4.c`), so its storage took the stencil's values with
     it. They now follow when the pair changes (`fbo.c`'s `stencil_home`, a backend's `rb_stencil_copy`, 0
     on V3D). `fbo` 709 of 709.

   Found by the later groups, all fixed and their groups rerun clean:
   - **sampler arrays in the compiler's reference draw** (`uniform_api`'s `sampler2D_samplerCube` cases
     12): `draw_vc4_reference` set only an array's element 0, the rest stayed on unit 0, and beside a cube
     sampler there the draw was a GL error, no variants, and the link failed ("the program binary could
     not be written"). Every element its own unit now, as `bind_samplers`. Both compiler builds are
     rebuilt; their programs are identical (`compare_vc4p.sh`, 9 of 9).
   - **a program drawn in many states outgrew its keyed file** (`vertex_arrays.multiple_attributes.input_types`
     70): the writer took 64 variants, the reader 64 sections, so at 66 sections the file was refused,
     and every later draw used the closest variant. `ogles2_image_keyed` now compiles at most
     `KEYED_RECORDS` (28) records, the oldest dropped and compiled again if drawn again: 30 variants and
     no refusals since. A program cycling through more states than that compiles often.
   - **the stencil reference masked, not clamped** (`fragment_ops.stencil` 8): -1 was 255 and 256 was 0;
     `draw_v3d.c` always clamped.
   - **the host compiler aborted once** (`fragment_ops.random.25`): `fake_drm.c`'s `MAX_BOS` (4,096 buffer
     objects; VC4 keeps every variant) filled up after thousands of compiles in one process. The bridge
     starts it again and asks once more. On the A1200 the same abort ends the on-target server, which the
     library starts again at its next compile, the draw that hit it using the closest variant.
   Not-supported so far, against Mesa's counts (its case names are on the Pi 3's card, not checked here):
   `shaders` 91 to its 90 (89 loops and `return_in_dynamic_loop_dynamic_vertex`, "Non-uniform loops are
   unsupported in the vertex shader"; the extra one `fragdata.invalid_assign_to_1`, which needs a single draw
   buffer, and we offer `EXT_draw_buffers`).

   **The Wi-Fi link is the run's main obstacle, its cause not found.** The Amiga loses the network in
   mid-run, every 7 to 19 minutes that day and sometimes within two minutes of boot. Seen:
   ping and TCP stop while the machine lives (the pointer moves, netwatch's timers run); at one drop
   Roadshow's counters froze on received IP bytes with its read requests still pending and the link
   still "Up", and only a few frames of unknown type went on arriving; 63 to 69 °C and `throttled`
   0x00000 throughout, so not heat or power. Ruled out as the sole cause: a fixed time since boot, a
   particular test case (`shaders-indexing` and `shaders-loops-p01` got past on their third tries where
   the first two died), the logger polling `ShowNetStatus` (a drop without it), an upload's CRC of every
   file alone (drops went on with `out/ogles2-m7-1.keep-bin`, which leaves `bin/` out of it and saves
   about 20 s a run). Under test: bursts of small packets. Drops were rarest in the slow state below and
   in groups with few compiles, and each compile request was 14 small sends with `TCP_NODELAY`, with
   Roadshow's write requests at their maximum of 32; `v3dglslc-remote` now sends a request's frame in one
   send (5 resets in the 30 minutes before, 1 in the first 22 minutes after: too little to conclude).
   Worked around, not solved: `tools/netwatch` resets the machine after two minutes without a reply,
   and `p6_deqp.sh` retries the part. netwatch's first version did all its work in one task and did not
   reset the machine three times; its timer now depends on neither the network nor the disk (the log's
   process started beforehand too: making one needs the disk). `p6_watch.sh` starts netwatch after each
   boot, and looks every 5 s while the station is down: at its 60 s, a link lost again within a minute
   of the boot at 19:14 left the machine without a netwatch until a hand reset at 19:48.
   **The link also has a slow state**, seen for 42 minutes from 17:40 on 2026-11-06 with no reset in it:
   a compile request 1,950 ms instead of 20 to 40, `amigent-ctl hello` 1,500 ms instead of 95, and from
   the PC's `ss -ti` half the bytes of a quiet connection retransmitted at a round trip of 839 ms, while a
   connection in steady use kept 11 ms, pings lost nothing (5 to 110 ms, the access point 1.1) and
   Roadshow's counters looked clean. A reboot cleared it at once (`shaders-keywords`, 95 cases in 33 s;
   36 cases of `shaders-invariance` had taken 16 minutes). Cause not found. `p6_watch.sh` now reboots a
   station that answers slowly for three polls running.
   **dEQP's shader cache was slowing everything** (as in `ogles3`'s run): `Run-M1` keeps every compiled
   program in the package's `cache/` on disk, never emptied, and one listing of it no longer finished in
   110 s. The PC compiled a program in 2 to 3 ms and the Amiga saw 260 ms; the parts fell from about 100
   cases a minute to about 30. `RAMCACHE=1` keeps them in `RAM:m7-cache`, emptied at each run's start
   (`p5_run.sh` passes it with `keep-bin`): a part of `shaders-operator` 8 min 31 s to 2 min 49 s, compiles
   260 ms to 40, about 89 cases a minute (the Pi 4's whole group, its cache on disk, 48). Delete
   `Stuff:amigent/work/ogles2-m7-1-oldcache` when the pass is done. A user's `OGLES2Cache:` is trimmed
   since (below).

   **glCopyTex* on the GPU (2026-11-07):** the copy program with the variants of the copies it can draw
   (`tools/copy_tex/keys_vc4.c`, `make_header_vc4.sh`; `ogles2/vc4/copy_tex_vc4p.h`) and its sources for any
   other state, so a copy needs no compiler. From an object as on the Pi 4. From the display there is no TFU,
   and the TMU samples raster images right only at power-of-two widths: a job with no draws loads the whole
   raster and stores it tiled into a scratch texture of the frame's size (`vc4_target.color_load_raster`,
   `ogles2_vc4_tex_from_raster`; from the context's buffer when the bitmap is one the stores cannot reach), and
   the quad samples the region. A whole-screen copy at 640x480: 17.3 ms a frame against the CPU's 166.8 (m7-6,
   36 of 36; `texture`, `fbo` as before; m5-1 261 of 261, the same three contexts' memory).
   **glGenerateMipmap on the GPU (2026-11-07):** each level drawn from the level above, LINEAR, a job a level, with
   the copy program (`copy.c`, `ogles2_copy_levels`; force_first's variants in its image), as Mesa's vc4. A
   512x512 target each frame 268 ms to 9.2 (the CPU read it back first), a 256x256 upload 37 ms to 15. Only a
   power-of-two level 0: the TMU takes every level above 0 at its power-of-two size (`tiling.c`), so a level of
   100x60 lies in 128x64, which a render of GL's size does not fill (m6_tfu's 200x120, wrong by 192); those stay
   on the CPU, as do the formats the tile buffer does not store (RGBA4, RGB5_A1, luminance and alpha), cube maps
   and the first call on a chain not laid out, as on the Pi 4. m6-8 121, m6-6 395, m2-4 152, m3-2 147, none FAIL;
   `texture`, `fbo` as before.

   **Every package on the final library, the Pi 3 card (2026-11-07):** m1-1 59, m1-2 59, m1-3 69, m1-4 14, m1-5
   59, m1-6 81, m1-7 81, m1-8 26, m1-9 26, m1-10 74, m2-1 103, m2-2 166, m2-3 118, m2-4 152, m3-1 118, m3-2
   147, m4-1 193, m5-1 261, m6-6 395, m6-8 121, m6-10 210, m7-2 66, m7-3 105, m7-4 129, m7-5 162, m7-6 36, m7-7
   2, m7-8 37: none FAIL; m2-5's 14 pictures PASS against Mesa's (at most 0.18 % differ). (m1-1 lost one check
   and m4-1 six on a first run, to memory freed elsewhere during a check and to a link drop, "no bridge";
   both clean when run again. p5_run.sh counts no "no bridge" itself: read a package's log when it fails.)
   **Benchmarks on the card:** Quake III's "four" with GLES2 32.8 fps (11.0 / 30.5 / 215 ms), as before these
   changes; glmark2 70; es2gears-sized m4-2 799 fps at 800 triangles and 369 at 8,000; SDL2's target texture
   91.7 fps, a new one each frame 70.6. The free memory during dEQP does not fall: it stood at 839 MB through
   a part (17:43 to 17:48 on 2026-11-06); what looked like 2 MB a minute was the first minutes of a run.

   **The link compiles what a first draw is likely to need (2026-11-07):** a program was compiled twice on
   the Pi 3, at its link and at its first draw. Three causes, by the keys of m2-5's files on the card (each
   `.vc4k` against its `.vc4p`):
   - **A compiler bug: the reference draw kept the last program's sampler views.** The state tracker sends a
     stage's views only while its program reads samplers, and the VC4 path never dropped them (V3D's does,
     `bind_samplers`): in a compiler that outlives a program (the library's, the bridge's) a program's link
     keys held the samplers of the one compiled before it, which no draw of it asks for. Each such program
     compiled again at its first draw, and a file depended on what was compiled before. Now
     `vc4_variants_drop_views` after each reference draw; `vc4_keys.py` checks that a file is the same after
     another program (it failed on the old compiler).
   - **The target:** the reference draw is into RGBA8 without depth; of probe V's 63 programs 52 first drew to
     the display (BGRA on RTG), 31 with a depth test. The link sends hints, 12-byte key records of stage 5 to
     15 (`ogles2/vc4/key_vc4.h`; a compiler skips a kind it does not know): stage 5, the reference's fragment
     key with the display's format, and with a depth test when the context has a depth buffer.
   - **A vec4 position fed three floats** (`glVertexAttribPointer` size 3): 10 of m2-5's 15 vertex misses. Stage
     6: the reference's vertex key and its coordinate twin with slot 0 three floats, compiled only when slot 0
     is a vec4.
   `ogles2_vc4_link_hints` (`vc4_key_link_hints`, `link_hints` in the backend table, 0 on V3D). A hint that
   does not compile is left out; a compiler that refuses the records (older than them) gets the link again
   without (`program.c`). A link has one to two fragment variants and a vertex pair more. The file's name
   stays the hash of the sources: a `.vc4p` written before keeps working, its draws compile as before.
   **On the card:** programs compiled again at a draw, against the same packages at 01:35 that day: m2-5 22
   to 11 (14 pictures identical to that run's), m5-1 25 to 19 (261 PASS), m7-6 4 to 2 (36 of 36); m6-3 and
   m6-4 6 and 5 (their earlier runs took files from an old cache: no count). m2-5's 9 programs that still
   compile at a draw all draw in a state a link cannot know: RGB textures (stored rgbx), a depth buffer on an
   FBO, points, blending, stencil. dEQP's `texture` group (807 cases, the programs with samplers) the same as
   the night before, part for part (796 pass, the same 11 known failures), and `fbo` 709 of 709 as before
   (one part again after a Wi-Fi drop). Host against m68k 9 of 9 files identical, `compile_bridge.py --check` 8 of
   8 (V3D unchanged), `vc4_keys.py` all (the library's own hint buffer and keys in the files).
   **The disk cache has a size (2026-11-07):** every program left its files in `OGLES2Cache:` for good, and a
   drawer that grows makes each new file slower (dEQP's above; `ogles3`'s run reached a million files). The
   first compile of a library's load (`compiler.c`, `cache_trim`) looks at the cache: over 8192 files or
   `ENV:OGLES2/CacheKB` (64 MB when not set, 0 for no limit) the oldest by date go, down to 3/4 of both. Only
   its own files, 24 lowercase hex digits and an extension of its own; never `PROGDIR:`. Bounded, 16384 looked
   at and 2048 removed a load. "Oldest" is the date written, not the last use: a program still in use may
   compile once more. This is in the front end, so **it changes the Pi 4 too, a deliberate exception to §5.1
   the user asked for**; V3D's compiler output and the rest stay as 1.3.3's. Test: `tests/m7_cache`, package
   m7-9: 18 of 18 on the card (100 files of 1 KB against 64 KB: the 52 oldest gone; other names, a second
   compile of the load, the limit 0 and a cache under it untouched).

   **The Pi 4 card on the final library (2026-11-07), against release 1.3.3:** through amigent, the
   programs compiled on the PC (the Pi 4's compiler output is byte for byte 1.3.3's, `compile_bridge.py
   --check`): m5-1 259 PASS, m7-6 36 of 36, m2-5's 14 pictures identical to 2026-10-14's, m2-4 152, m7-2
   66, m7-4 129, m7-5 162, m6-6 395, m6-8 121, m6-10 210, m1-5 59, m7-3 105, m7-9 18 of 18 (the cache's
   trim, the one change on the Pi 4 on purpose): none FAIL. **Speed**, 1.3.3's library and this one
   alternating three times each, the plain (not sync-logging) builds: Quake III renderergl2's timedemo
   67.1 against 67.0 fps (66.6, 66.4, 68.2 against 66.5, 67.7, 66.7); gears (m6-1, TEETH=1) 0.3 to 1.3 %
   slower in every mode, every round (bitmap arrays 749.5 against 740.4 fps, screen 939.1 against
   928.2, window 399.7 against 398.5), the size of the placement effects P2 measured and accepted (P2's
   acceptance above: gears within 1 %). Both libraries ran below 4 and 5 November's figures alike (Quake III 89 fps
   then): **the Pi 4 ran at 1.5 GHz**. After the card's boot files were changed for the Pi 3 and back, its
   `config.txt` had no overclock and the default `gpu_mem`; `tools/piclock` (the firmware's clocks,
   temperature and throttling, through the mailbox from AmigaOS) read the ARM at 1,500 MHz, its maximum,
   40 °C, nothing throttled. Every CPU stage of Quake III's frame was slower by the same factor (draws
   2,802 to 3,890 us a frame, uploads 406 to 564), and so was its load (14.0 to 19.2 s); the 5 November
   package and library ran 66.7 fps that day too. `gpu_mem=256` (the 4 November memory map again:
   0x01000000-0x2E9FFFFF) alone changed nothing (67.2 fps); **with the ARM at 2.2 GHz again, Quake III
   89.3 fps on this library, 89.1 on 1.3.3's** (loads 13.93 and 13.92 s), 4 November's figures. The
   gears figures above are at 1.5 GHz; at 2.2 GHz, alternating three times each (59 °C at the end,
   nothing throttled): bitmap immediate 432.9 against 432.1 fps, arrays 783.9 against 774.0 (1.3 %
   slower), list 740.5 against 729.3 (1.5 %), window 452.6 against 448.9 (0.8 %), screen 1,115.1
   against 1,093.7 (1.9 %): the same pattern, every round, a little above P2's 1 % in screen mode.
   Probe P on both, twice each: the draws together cost the same (bitmap arrays 173.9 against 174.9 us a
   frame, screen 181.0 against 181.8); the uniform stage 7 % more (35.1 to 37.6 us), shaders and checks
   less, the wait for binning 2 to 3 us more, the rest of the frame-to-frame time (1,294 to 1,307 us) in
   gears and gl4es. The uniform stage's code is 1.3.3's line for line (`write_uniforms`,
   `vertex_uniforms`, `default_attribs`, `claim`: renamed fields only); the library is 351 KB against
   236 KB (the VC4 backend, never run on the Pi 4). Where code lands under Emu68's translation, as P2
   found at the split: left as it is.
   **dEQP-GLES2's whole list on the Pi 4 card, the final library (2026-11-07, 2.2 GHz)**, through amigent,
   compiled on the PC, in parts of 250 (`CARD="Pi 4" p6_deqp.sh` with `p6_watch.sh`): 2 h 17 min, four Wi-Fi
   drops reset by netwatch and one slow-link reboot, each costing its part a retry. **17,131 of 17,181
   pass; 59 of 60 groups exactly as the Pi 4's newest earlier result of each** (4 and 5 November; 17,132),
   the known ones included (`texture` 24, `shaders-texture_functions` 1: vertex cube maps; `flush_finish` 3
   and `rasterization` 3 warnings; `multisample` without MSAA not supported, 16 of 16 in 4x MSAA). The one
   difference, `texture.format.a8_cube_pot` (one cube face's alpha off by 143, the part's first texture
   case), passed run again three times alone and once in its group (36 of 36): not reproduced.
   `p6_watch.sh` started while the station booted counted the log it could not read as no resets and took
   the last old one for new (it killed nothing: only runs older than the reset are stopped); it counts from a
   log it read now. `piclock`'s ARM query answers nothing under the overclock (the
   other clocks, the voltage, 1.035 V, and the temperature, 55 °C after the demo, do).
   On the way: `p2_checks.sh` took an old results drawer for a run that left none (a station drop, and a
   78-byte compiler stand-in that `runpkg`, which deletes nothing, had left in 12 of the station's package
   drawers: each compile waited 60 s). It stops at a drop now, takes only a run's own results and removes a
   station-side compiler a package does not ship; `p5_run.sh` takes `LIB=` for speed runs.

   Left: P7 (item 2 of the list at this section's start); Mesa's mipmap picture for `vc4_draw` (the Pi 3
   under Linux, optional).

   **Release 1.4-beta1 prepared (2026-11-07, PD10):** library 1.4 (`OGLES2_RELEASE` "1.4-beta1"; GL_VERSION
   and `$VER` name it; m6_ext checks "1.4"). The archive carries the Pi 3's binaries for every example
   (12 `.vc4p` beside the 12 `.v3dp`): `build_release.sh` compiles each with the keys of every draw state the
   examples were seen to use (probe R on the A1200, `release/examples/vc4keys/`, `release/vc4keys.py`).
   ReadMe, Install-ogles2, GettingStarted (shipping `.vc4p`/`.vc4k`), ogles2.doc (the Pi 3's limits and
   deviations), ReadMe-SDL and README.md updated. On the Pi 4 card the archive's examples, unpacked as a
   user would, no compiler anywhere: 38 links from `shaders/`, no compile, no closest variant (boingocean
   55.7/59.8 fps, boing2026 55.3); boing2026's "texture 0 sampled while incomplete" is 1.3.3's too.
   Left: item 2 at this section's start.
   **The archive on the Pi 3 card (2026-11-07):** `bash tests/a1200/beta_run.sh` (new: the archive through
   amigent, unpacked with the Amiga's LhA, `Libs/ogles2.library` into `Examples/libs/` as the ReadMe says,
   nothing in `LIBS:`, an empty `OGLES2Cache:`, no compiler anywhere; `tests/a1200/beta/Run-beta` runs
   triangle, cube, boingocean and boing2026 in a window and full screen, boing2026's TOUR and NOMSAA, and
   the logs are checked). piclock: board revision 0xa02082 (a Pi 3B), ARM 1,200 MHz, nothing throttled.
   **28 links, every one a `.vc4p` from `shaders/` (all 12), nothing compiled, no closest variant, every
   context's GPU memory returned, the cache empty at the end** (`tests/a1200/results/beta-20261107-174215`).
   boingocean 19.0 fps in a window, 23.7 full screen; boing2026 15.0 without MSAA (window), 19.2 full
   screen with 4x MSAA. The logs' other lines are known: ERRSTAT VCMRE (item 4), boing2026's texture 0
   incomplete (1.3.3's too), the examples' mode search refusing two 16-bit modes before a 32-bit one.
   ReadMe ("four programs"), README.md's archive table (1.4-beta1's names, the Pi 3) corrected;
   `release/dist/` holds only the current release's archives (1.3.3's, the Pi 4's reference, in its tag:
   `git show v1.3.3:release/dist/ogles2-1.3.3.lha`). The archives built into `release/dist/`.
   **A demos archive (2026-11-07, asked by the developer):** `ogles2-1.4-beta1-demos.lha` (449 KB), the four
   examples alone, ready to run where unpacked: `ogles2-demos/` with the programs, `boing2026.txt`,
   `shaders/` (both Pis' binaries and the records), `libs/ogles2.library`, a ReadMe of its own
   (`release/ReadMe-Demos`) and the licences; `build_release.sh` makes it beside the others. On the Pi 3
   card (`DEMOS=1 beta_run.sh`, `Run-beta` with `EXAMPLES=`, which now also runs boing2026 in a window
   with MSAA): **34 links from `shaders/`, nothing compiled, no closest variant, memory returned**
   (`tests/a1200/results/beta-20261107-180619`); boing2026 in a window 14.8 fps. On the Pi 4 not run: its
   files are the user archive's examples (tested there) with the library in `libs/`, as that test had it.
   **Workbench icons for the examples (2026-11-07, asked by the developer):** `release/make_icons.py` draws
   them (no image files, no libraries; run by `build_release.sh`): each a 46 x 46 GlowIcons image (AmigaOS
   3.5 and newer, a glow when selected) and a 4-colour one in the Workbench's first pens for older
   systems; the programs are tools (32 KB of stack, their options as ToolTypes in brackets), the texts
   projects for MultiView, `Examples.info` and `ogles2-demos.info` drawers. The programs start from
   Workbench: `release/examples/wbstart.h` (in the SDK beside them) makes the icon's ToolTypes their
   Shell arguments (those in brackets left out) and sends their output to a console window that opens
   only when something is printed; the start-up lines and the frame count (without FRAMES) are not
   printed from Workbench. On the Pi 3 card (CaffeineOS, Directory Opus Magellan as the desktop, icon.library
   51.4) through `WBRun`: boingocean with a test icon (`FRAMES=100`, `(FULLSCREEN)`) ran in a window and
   printed its 100 frames in the console window; boing2026 and boingocean from their own icons ran and quit
   with Esc without a window of messages, linking from `shaders/`, memory returned; a Directory Opus
   lister drew the icons in colour. The Shell's way again with the changed programs
   (`DEMOS=1 beta_run.sh`): 34 links, nothing compiled (`tests/a1200/results/beta-20261107-182315`).
   Not seen: the 4-colour images on a system without GlowIcons; the programs from Workbench on the Pi 4.
   **Published (2026-11-07):** tag `v1.4-beta1` on a14831e and the GitHub pre-release "ogles2 1.4 beta 1" on
   `ladislawv/ogles2pi3` (private: seen only by those with access), its assets the six archives of
   `release/dist/` and `SHA256SUMS`, uploaded through GitHub's API (no `gh` on the build machine).
   **The Raspberry Pi Zero 2 W supported (2026-11-08, the developer):** its chip (BCM2710A1) is the Pi 3's,
   the same VideoCore IV; `vc4_os_present` takes it by the device tree's `brcm,bcm2837`/`brcm,bcm2710`,
   and the memory and the mailbox come from the device tree and the firmware as on the Pi 3, so no code
   changed. The documents name it beside the 3B and 3B+ (README.md, release/ReadMe, ReadMe-Demos,
   ReadMe-SDL, GettingStarted, ogles2.doc, Install-ogles2, CONTRIBUTING.md, CLAUDE.md), "Pi 3" standing
   for both where limits and binaries are concerned; its ARM runs at 1.0 GHz (the 3B's 1.2) and it has
   512 MB (the table in §1). Not on this repository's test station (a Pi 4B and a Pi 3B): no figures of
   its own. The archives in `release/dist/` (and the pre-release's) still carry the 1.4-beta1 texts;
   the next build has these.
   **`ENV:OGLES2/LogSync` (2026-11-08, asked by the developer: "T: is bad for logs if the Amiga crashes"):**
   the log lived in a 32 KB ring in memory, written to its file only when a context is destroyed, the
   library closed or OGLES2Log asked, and to `T:` (RAM) by default, so a crash lost it twice. Now
   `OGLES2/LogSync` set (not "0") writes every line at once, as the `OGLES2_LOG_SYNC` builds always did
   (`log.c`: the same per-line flush, from a process outside `Forbid()`, the lines before it with it). It
   is read at the first line a process logs after an open (the Open vector, under `Forbid()`, only marks
   it unread: `ogles2_log_recheck`), so a change takes effect at the next open of a library still in
   memory. The default stays `T:ogles2.log` (a user's disk is not written unasked); the ReadMe's
   "Logging" says how to catch a crash (`LogFile` on a disk with `LogSync`), `ogles2.doc` lists it.
   Built without warnings; **not run on the A1200** (the developer skipped it): to check, a program with
   `LogSync` 1 must have its link lines in the file while it runs, and none without it until its end,
   and its frame rate with and without.
   **The 1.4-beta1 archives rebuilt (2026-11-08, the developer: the pre-release's files replaced rather than a
   beta 2):** `release/dist/` with `LogSync` and the Zero 2 W texts; the pre-release's seven files replaced
   by them, its notes updated, and the tag `v1.4-beta1` moved to this commit (it was on a14831e).
