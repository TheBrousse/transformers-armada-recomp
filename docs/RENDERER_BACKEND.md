# The native renderer's graphics backend

The native renderer (`transformers/native_render/`) draws the game with the PC's GPU in place of the emulated PS2
graphics hardware. It has two halves:

- **The shared half** turns what the game draws into a frame's draw list: the capture of the game's display lists and
  device calls (`nr_capture.cpp`, `nr_vif.cpp`, `nr_gif.cpp`, `nr_n5.cpp`), the meshes, textures and passes it builds
  from them (`nr_texture.cpp`, `nr_hires.cpp`, `nr_passes.cpp`, `nr_grass.cpp`, `nr_particles.cpp`, `nr_rain.cpp`,
  `nr_movie.cpp`, `nr_warp.cpp`, `nr_occlusion.cpp`), and the render thread that hands each frame on
  (`nr_thread.cpp`). Every rendering fix lives here, for every backend.
- **The backend** takes a frame's draw list and draws it with one graphics API, then hands the picture to the window.
  Today that is Direct3D 12: `nr_d3d.cpp`, plus the window's side of the hand-over (`nr_present.cpp`, `nr_glshare.cpp`).

This document is the backend's contract: what a second backend (Vulkan) has to provide, what the Direct3D 12 one does
today, and the test a new backend has to pass to count as done. The split of the code along this line is in progress
(steps S1-S4 at the end); until it is finished, "the interface" below is the set of `nr::d3d` functions declared in
`nr_types.h` plus the parts of `nr_d3d.cpp` that the shared code will call.

## 1. What a frame is

The render thread calls the backend once per frame, `render(const Frame &frame, const std::string &shotPath)`, with a
`Frame` (`nr_types.h`). The parts the backend reads:

- `draws`: the frame's draws in the game's order. Each `Draw` names its render target by the PS2's frame register
  (`target`: base page, width, pixel format), its depth buffer (`zbuf`), the GS state the game set (TEX0, TEX1, CLAMP,
  ALPHA, TEST, FBMSK, scissor), a mesh (`Mesh`: vertices with position, colour, texture coordinates, normal, point
  size; indices; or none, for a pass computed on the GPU), a texture (`Texture`: RGBA8 pixels, or a large image's
  RGBA8 / BC7 data, with the game's mip levels when it has them) and palettes for the depth fog and the top-byte
  reads, the passes' constants (`nr_passes.cpp`: matrices, modes, the routines' parameters), and flags (instanced,
  sprites, clipped, the camera's turn).
- `dispfb`, `display`, `pmode`: the PS2's display registers - which target is the picture and how it is shown.
- `pictures`: the pictures to make from this frame (one usually; with moment pictures, several - each the same draw
  list at its own pose, `PictureConstants`), each with the time it is due at and, for a check run, a file to save to.
- `tick`, `dueNs`: the frame's VBlank and its moment, for pacing.

The draw list is complete and immutable when `render` gets it. The backend never reads guest memory.

## 2. The backend's calls today (`nr::d3d`, declared in `nr_types.h` and `nr_glshare.h`)

| Call | Thread | What it does |
|---|---|---|
| `start()` / `prepare()` | render thread | Make the device, queues, heaps, root signature, shaders and pipelines. `false`: no backend (the emulated renderer stays). |
| `drawsPictures()` | any | The backend is up and drawing. |
| `render(frame, shotPath)` | render thread | Record and submit the frame; with `shotPath` (or a picture's `shot`), save that picture as a BMP once drawn. |
| `waitForFrameSlot()` | render thread | Wait until the next frame slot is free (the frame two before finished); returns the ms waited. |
| `deviceLost()` | any | The device was removed (a GPU hang): nothing more is drawn, the emulated picture comes back. |
| `grassGpuReady()` | render thread | The grass compute pass exists, so the shared half may ask for grass grown on the GPU. |
| `latestPicture(pixels, w, h, serial, has3D)` | window thread | The newest picture as CPU pixels (the hand-over every system can take). |
| `setSharedPictures(on)`, `latestShared(picture, serial)`, `sharedHeld(index)`, `kSharedPictures` | window thread | The newest picture as a GPU texture the window imports (no copy through the CPU), and which one the window holds. |
| `waitPicture(serial, untilNs, lastNs)`, `pacingMode()` | window thread | Pacing: wait for the next picture by its due time. |
| `setAntiAliasing(mode)`, `setSupersampling(on)`, `setWindowLines(lines)`, `setSpeedupsForced(on)` | any | Settings, applied from the next frame. |
| `largeImageCap()`, `largeImageBudget()`, `largeImageBytes`, `preUploadImpl(texture)` | any | The HD textures' video-memory budget, and uploading one ahead of its first draw. |
| `slotWaitMs`, `gpuPictureMs`, `cpuPictureMs`, `gpuExtraMs`, `replayWaitMs`, `pacerDelayMs` | render / window | Timing figures for the frame pacer and the stats lines. |

A second backend implements the same calls with the same meaning. The split (S1) gives them one interface type instead
of a namespace.

## 3. Resources

- **Render targets.** One per PS2 frame buffer the game draws into, keyed by its frame register (base page, width,
  format) and grown to the largest height drawn. Colour targets are RGBA8 (the picture shown is RGBA8 or BGRA8); depth
  targets are 32-bit float, also readable as a texture (the depth fog and the top-byte reads sample them: an
  `R32_TYPELESS` resource with a depth view and an `R32_FLOAT` read view). Each target has a read-only view for the
  draws that sample it while another target is bound. Targets are drawn at the window's scale (the "Resolution"
  setting) times the PS2's size, and live until their frame register is no longer drawn.
- **Textures.** RGBA8 with a mip chain (the game's own levels, or made by halving), or BC7 for the large images (HD
  textures). Up to 8192 texture slots, slot 0 a white texture; a texture not yet uploaded draws with a stand-in.
- **Meshes.** Vertex and 32-bit index buffers, uploaded once per mesh and kept while the mesh is drawn; meshes made
  every frame go through a per-frame streaming ring (64 MB). Per-draw constants go through a 32 MB ring per frame,
  instance data through a ring of 32,768 entries per frame.
- **Small fixed resources.** The picture's constants (one 256-byte block per frame slot), the anti-aliasing lookup
  tables (SMAA's area and search textures, R8G8 / R8), the sun's occlusion probe (a 64 x 64 R32_FLOAT target read
  back to the game), timestamp query heaps, and an 8-byte-per-slot predicate buffer.
- **Formats used:** `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `BC7_UNORM`, `R32_FLOAT`, `R32_TYPELESS` / `D32_FLOAT`,
  `R8G8_UNORM`, `R8_UNORM`, `R32_UINT` (indices), vertex attributes in `R32G32B32A32_FLOAT`, `R32G32B32_FLOAT`,
  `R32G32_FLOAT`, `R32_FLOAT`, `R8G8B8A8_UNORM`.

## 4. Shaders and pipelines

- One shader source, HLSL, inside `nr_d3d.cpp`, compiled at start with Shader Model 5.1 (`vs_5_1`, `ps_5_1`), plus a
  geometry shader (`gs_5_1`) that clips triangles the way the PS2's microcode does, and a compute shader (`cs_5_0`)
  that grows the grass on the GPU bit-identical to the CPU's version. The anti-aliasing passes (FXAA, SMAA) and the
  picture passes are separate small shaders. Compiled shaders and pipelines are kept on disk (a shader cache and a
  pipeline library) and warmed at start.
- **Root signature** (the binding layout every draw uses): `b0` the draw's constants (a CBV into the constant ring),
  a descriptor table with `t0` the draw's texture, a table with `t1` the palette, a table with `t2` the depth read,
  `b1` the picture's constants, and six static samplers: `s0` anisotropic wrap (4x-16x by the quality preset), `s1`
  trilinear, `s2` point with linear mips (the GS's nearest filter with TEX1's level), `s3` anisotropic at a test
  setting, `s4` / `s5` linear and point clamped without mips (SMAA). The grass compute pass has its own root signature
  (constants, structured buffers in, unordered-access buffers out).
- **Pipeline states** come from the GS state of each draw: the blend (the GS's ALPHA register turned into source and
  destination factors, add / subtract / reverse subtract, or a product done in the shader when the GPU's factors stop
  at 1.0), the depth test and write (the GS's ZTST, ZMSK), the colour write mask (FBMSK, per byte), the topology
  (triangle lists; strips for a few passes), the clipper on or off, and the target's format. They are made on first
  use and cached by a key of those values.

## 5. Threading and the frame's life

- **Threads.** The render thread owns the device and records every command list. Texture conversion runs on a worker
  pool and gives the backend CPU pixels; the backend uploads them on the render thread (or ahead of use,
  `preUploadImpl`). A pacing thread waits on each picture's fence and hands it to the window at its due time. A replay
  thread submits a frame's extra pictures (below). The window thread only takes finished pictures.
- **Frame slots.** Two frames in flight (`State::kFrames = 2`). Each slot has its command allocator, its rings'
  thirds and its fence value; `waitForFrameSlot` waits until the slot the next frame uses is done.
- **One frame:** wait for the slot; upload the new textures and meshes; walk the draw list - bind the target, set the
  pipeline from the draw's state, bind its texture and palette, write its constants, draw - with copies between
  targets where the game reads one it drew (the field weave, the glow's buffers, the shadow maps); after the picture's
  target is complete, run the anti-aliasing pass and copy the picture into the texture the window shows; close the list
  and submit it. Resource state transitions are tracked per resource inside the backend.
- **Moment pictures.** A frame with several pictures records its list once and submits it once per picture, each time
  after a small list that writes that picture's constants (its pose between the frame before and this one, the
  camera's turn) and before a small list that copies the picture out; targets that no extra picture needs are skipped
  by predication; the resources the list moved are set back to their starting states before it runs again. Direct3D
  12 does not allow a list to be submitted again while an earlier submission may still run, so an extra picture is
  submitted only after the one before has finished (the replay thread does that wait).
- **Saving a picture** (`shotPath`, `Frame::pictures[].shot`): the picture is copied to a read-back buffer and written
  as a BMP once its fence has passed. The check runs below rely on it.

## 6. How a picture reaches the window

The window (raylib / OpenGL today, `nr_present.cpp`) shows the newest finished picture. Two ways, chosen at start:

- **Shared texture** (the default where it works): a ring of RGBA8 textures (`kSharedPictures`, 24) created as
  shareable, each with an NT handle; the window imports them into OpenGL with `GL_EXT_memory_object_win32`
  (`nr_glshare.cpp`) and draws the one handed over. The backend never overwrites a texture the window holds
  (`sharedHeld`) or the newest one handed over.
- **CPU pixels**: the picture read back into memory (`latestPicture`) and uploaded by the window. This is the fallback
  when the driver cannot import the texture (for example under Wine/Proton, where the import fails with a GL error;
  the window then switches to this for the session).

A Vulkan backend can do the same: export its picture textures (`VK_KHR_external_memory_win32` on Windows,
`VK_KHR_external_memory_fd` on Linux and Android) and let the window import them (`GL_EXT_memory_object_win32` /
`GL_EXT_memory_object_fd`), with the same CPU fallback. Once the window layer moves to SDL3 (roadmap P2), a Vulkan
backend may instead present to the window directly.

## 7. Direct3D 12 features a Vulkan backend has to replace

| Direct3D 12 | Used for | Vulkan |
|---|---|---|
| Root signature, descriptor heaps (8192 shader-visible texture slots, RTV/DSV heaps) | every draw's bindings | One descriptor set layout matching section 4 (a large texture array or descriptor indexing for the slots), push constants or a dynamic uniform buffer for `b0` |
| Static samplers | the six samplers | Immutable samplers in the layout |
| HLSL Shader Model 5.1, compiled at run time (`D3DCompile`) | all shaders | The same HLSL compiled to SPIR-V (DXC with `-spirv`), at build time or run time; one source for both backends |
| Geometry shader (`gs_5_1`) | the PS2's triangle clipping | Geometry shaders (a desktop feature; optional on mobile - Android needs another path, e.g. clipping in a compute pre-pass) |
| Compute shader (`cs_5_0`) | grass grown on the GPU | Compute pipeline; the CPU path stays as the fallback |
| Predication (`SetPredication` on a buffer) | skipping targets in extra moment pictures | `VK_EXT_conditional_rendering`, or recording a second list without them |
| Re-submitting one command list per picture | moment pictures | Re-submitting a command buffer is allowed (`SIMULTANEOUS_USE` or after its fence); the restore-state list becomes image layout transitions |
| Resource states and transition barriers | every target and texture | Image layouts and pipeline barriers (tracked per image as today) |
| Typeless depth (`R32_TYPELESS` with depth and float views) | sampling depth buffers | A `D32_SFLOAT` image with `SAMPLED` usage and a depth image view |
| BC7 textures | the HD textures | Supported on desktop GPUs; most Android GPUs (and the Fire TV's) need ASTC / ETC2 instead, or decompressed RGBA8 |
| Timestamp and pipeline-statistics queries | the stats lines, the pacer's GPU times | Query pools (timestamps; pipeline statistics are optional) |
| Shared resources with NT handles | the window hand-over | External memory (section 6) |
| Pipeline library on disk (`ID3D12PipelineLibrary`) | start-up time | `VkPipelineCache` saved to disk |
| Device removed, DRED breadcrumbs | a GPU hang: back to the emulated picture | `VK_ERROR_DEVICE_LOST`; `VK_NV_device_diagnostic_checkpoints` / `VK_AMD_buffer_marker` for breadcrumbs where available |
| `D3D12CreateDevice` at feature level 11.0 | the device | Vulkan 1.1 or later (the lowest target device has 1.1) |

Behaviour to keep exactly, because the pictures depend on it: the blend factor products done in the shader (the PS2's
factors go to 2.0), the depth comparison direction and the 24-bit depth the PS2 compares (written as float), the
texture filtering per TEX1 (which sampler, which mip level), the order of draws within a target, and the copies
between targets at the points the game reads them.

## 8. Acceptance test: when a new backend is done

A new backend is done when it draws the same pictures as the Direct3D 12 one on the check routes, and the frame rate is
in the same range on the same PC.

- **Routes.** Each is played the same way by both backends, with the game's movies skipped (`TF_SKIP_FMV=1`), the
  frame rate locked (`TF_FPS_LOCK=60`), invulnerability on (`TF_GOD=1`) and a fresh memory card:
  1. **The HQ**: a cold boot through the menus to the HQ's warp gate (scripted input, `TF_INPUT`), pictures at the
     menus, the HQ room and the warp gate's tunnel.
  2. **The first level**: the same cold boot on into Amazon Basin and a walk forward, pictures along the path (scenery,
     forest, grass, water, the HUD).
  3. **The check states** made by `tools/make_check_states.ps1` (the first level's start, the walk to the dropship and
     the first Mini-Con with enemies firing, the Drop Zones room with its movie, the Mini-Con menu), pictures at fixed
     VBlanks after loading each. These states are made from the player's own disc, so anyone can make them.
- **Pictures.** Saved with `TF_NATIVE_SHOT=<VBlank>,...` from each backend (`runs/native_<tick>.bmp`).
- **Comparison.** `tools/native_compare.py <reference> <candidate>` per route (the Direct3D 12 pictures as the
  reference). Two different graphics APIs on the same GPU should give the same pictures; the limits allow for rounding
  only: `--max-mean 0.5 --max-flat16 0.05 --min-ssim 0.995` per case, and every difference larger than that looked at
  with `--heatmaps` before it is accepted (a missing draw or a wrong blend shows as a flat-area difference).
- **Speed.** The same routes at a 120 fps cap, the stats lines' GPU and game times per frame, within 10% of the
  Direct3D 12 backend on the same PC.

The same routes and comparison are what each step of the split itself is checked with: before and after a step, the
Direct3D 12 pictures must be identical bit for bit (`native_compare.py` reporting PSNR inf on every picture).

A caveat on the cold-boot routes (1 and 2): they are not repeatable from run to run, even with the same build. While
loading, the game's clock and the renderer's pace can line up two ways. The HQ's idle animations land a frame apart,
and the Amazon pictures take one of two outcomes, decided by whether the game waits about 66 ms on its clock right
after the level's load (the same pipelines are made either way). So for bit-for-bit checks use save states: the HQ
from a state in the HQ (its ring and its options screen) in place of the cold boot, and the levels from the check
states. Use the cold boot only to see that the route still plays, and compare its pictures against several runs of
the reference build, not one.

## 9. The split, step by step

- **S1** - `nr_backend.h`: the interface as one abstract type, and API-neutral types for what the shared half passes
  (blend factors and operations, compare functions, write masks, the formats above, a pipeline key, resource handles).
- **S2** - the API-neutral helpers move out of `nr_d3d.cpp` into a shared file: the GS state's translation into blend,
  depth and write-mask settings (as neutral types the backend turns into its own), TEX1's filtering constants, mesh
  bounds and clip runs, the constant blocks' layouts.
- **S3** - `render()` is split: the walk over the draw list (targets by frame register, the order of passes, the field
  weave, the glow, the anti-aliasing decision, the pictures and their hand-over) becomes shared code that only calls
  the interface; `nr_d3d.cpp` keeps the Direct3D 12 calls as the interface's implementation.
  The narrow device interface is `nr_device.h` (`nr::gfx::Device`), done in sub-steps, each bit-identical:
  (1) targets: the Direct3D 12 device owns the targets' surfaces, views and states; `render()` keys them by GS
  address and reaches them only by `gfx::TargetId` (create, resize, release, clear, use, view, snapshot). The
  picture is a target of its own (`TargetDesc::shared`), its surfaces the window's. (2) textures, meshes and the
  rings: the device owns textures (`gfx::TextureId` is opaque; the device maps it to its own view), meshes (by id) and
  the frame's rings; `render()` makes the mip chains, keeps its textures and meshes by the game's ids, and writes its
  per-draw data where `ringAlloc` points. Three calls were added to the approved draft so that the step stays a pure
  refactor:
  - `uploadTextureLater(texture, source, urgent)`: the texture is made and uploaded off the drawing thread when the
    device gets to it, its texels read from `source` then (gone by then: skipped; a failed upload is tried again while
    the source lives); `urgent` ones (the game's own textures over the frame's budget) go before the large HD images;
    `textureReady` tells when it can be drawn (until then the shared half draws the game's texture or a stand-in). The
    Direct3D 12 device does it on a copy queue of its own (the uploader thread: batches of at most 48 MB of staging,
    an HD image's texels let go of once on the GPU) and writes the texture's view at a frame's start. A Vulkan device
    does the same on a transfer queue (queue family ownership released to the graphics queue when done), or, without
    one, synchronously at its next frame start.
  - `setIndices(span)`: 32-bit indices in a ring (the frame's own meshes: 2D draws, primitives, the ocean, small
    meshes; the clipped draws' kept triangles).
  - `ringAlloc(kind, size, align, out)` with `RingKind` Constants (the draws' constants, 256-byte aligned), Instances
    (the passes' entries, 144 bytes each, read as a per-instance vertex stream; `out.first` is the entry's index, a
    draw's first instance) and Vertices (vertices and indices made this frame). Each is its own ring, a frame slot's
    part, begun again with the frame. The Direct3D 12 device writes the constants into an upload heap and, before the
    frame's list runs, copies the frame's part into video memory (a small list of its own; `TF_NATIVE_CBVRAM`) - the
    draws read the video memory copy. A Vulkan device can write a host-visible, device-local buffer directly where the
    GPU offers one (resizable BAR), or keep the same copy with `vkCmdCopyBuffer` and a barrier before the frame.
  (3) state and the draws: `render()` sets every draw's state and records every draw through the device - target
  and viewport, scissor, pipeline (a `gfx::PipelineDesc`; the device makes the pipeline the first time it is asked
  for and keys it by a number of its own), blend factor, the three sampled slots, the draw's constants, the
  geometry, the draw. The device leaves out a set equal to the last one (`TF_NATIVE_STATEFILTER`: on some GPUs each
  set is a register write), and forgets what it set when anything recorded around it sets its own (binding a target
  again forgets all). Four points were added:
  - `setPipeline` returns false when the pipeline can't be made: the draw is left out, never drawn with the last
    draw's state.
  - `setPreviousVertices(span)`: the frame before's vertices of the same mesh, the third vertex stream (the 3D's
    motion between frames); an empty span is none.
  - `drawIndirect(groundMesh, firstInstance)`: a ground's grass blades, their count written by the GPU (the device's
    own compute pass). A Vulkan device: `vkCmdDrawIndexedIndirect` on the same arguments buffer.
  - `gfx::kKeepTexture` in `bindTextures`: that slot stays bound to what it was. Slots 1 and 2 are read only by the
    draws that set them (the palette, the target's own copy, the fog's depth).
  Until sub-step 6 the draw loop stays in `nr_d3d.cpp`, calling only the device for its state and draws; it moves to
  `nr_frame.cpp` once copies, weave, anti-aliasing and queries go through the device too.
  (4) copies, the weave and the anti-aliasing: the field weave was already draws (a field copied into the picture
  is a GS draw that samples the field's target), so it needed nothing new. `copyWhole` copies a target into another
  of the same size and format. Targets can be made without depth (`TargetDesc::depth = false`) and in the formats the
  passes need (`RGBA8`, `RG8`, `R8`, `R32Float`; the GS's targets stay `BGRA8`). The passes that are not GS draws are
  `gfx::Effect`s, set with `setPipeline` (a `PipelineDesc` with `effect` set; its other fields unused) and drawn with
  `draw(3)` as a full-screen triangle: FXAA, SMAA's three passes, and the warp tunnel's sparks (`Effect::Cloud`: a
  strip of four a point, the points per instance - the device switches to triangle strips for it and back). The
  anti-aliasing pass is now the frame builder's: it copies the picture into a target of its own, then draws FXAA, or
  SMAA's edges, weights and blend, into its targets and back into the picture. SMAA's area and search tables are the
  effect's own: the device binds them with `Effect::SmaaWeights` (a Vulkan device keeps them as part of the shared
  shader set's data). The pass's GPU time stamps stay as they are until sub-step 5.
  Next: queries and stats (time stamps, the occlusion probe and its read-back, predication); publish.
- **S4** - the window hand-over takes a neutral picture: CPU pixels, or an exported texture with its handle type.

Each step leaves the pictures bit-identical and the frame rate unchanged (section 8).
