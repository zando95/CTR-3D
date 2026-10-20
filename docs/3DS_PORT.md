# The 3DS port

How CTR 3D runs Crash Team Racing on a New Nintendo 3DS. This describes the
shipped design. The PS1-shaped memory model the game keeps on every platform is
covered by `MEMORY_MODEL.md`, `DATA_SECTIONS.md` and `OVERLAYS.md`.

## Layout

- The game logic is the CTR-ModSDK decompilation. It reaches the host only
  through the PlayStation library surface (`libgpu`, `libspu`, `libpad`, ...),
  the small `Platform_*` contract in `include/platform.h`, and a set of named
  `Native*` hooks. There are no platform `#ifdef`s inside `game/`.
- `main.c` and every `platform/native_*.c` file form one unity translation
  unit; `game/game_unity.h` includes all of `game/` into it in dependency order.
- `platform/n3ds/n3ds_backend.c` is the one separate translation unit, so that
  libctru's headers never meet the decompilation's own. The game's struct
  layouts assume 4-byte enums, so the port is compiled with `-fno-short-enums`;
  the backend is compiled with `-fshort-enums` to match libctru's ABI. The two
  sides talk through plain scalars declared in `n3ds_backend.h`.
- `platform/n3ds/sdl3_compat_3ds.c` provides the few SDL entry points the shared
  platform code calls, so `platform/` stays source-compatible with the PC build.

## Threads and cores

- The game runs on the main thread, on core 0.
- A render worker on core 2 walks the ordering table and drives the GPU, so the
  game's next frame overlaps the current frame's draw. If the core is refused,
  drawing runs on the main thread.
- An audio worker on core 3 (core 2 as a fallback, then the main thread) feeds
  the DSP.
- Core 1 belongs to the system. The port never creates a thread there and never
  raises the application CPU time limit; either one hangs the console when the
  Rosalina overlay opens.
- Anything the game waits on that a host audio callback would normally advance
  (the SPU/XA sample clock) is driven from the vblank instead
  (`NativeAudio_SetDeterministicRenderMode(1)`), because nothing on the 3DS
  consumes audio at a rate the game can block on.

## Renderer

The PICA200 has no fragment shaders and no indexed-texture format, so the PC
renderer's per-pixel palette decode cannot be reproduced. The 3DS renderer
(`platform/n3ds/native_renderer_3ds.c`) implements the same contract
(`include/platform/native_renderer.h`) differently:

- **Texture decode cache.** PS1 VRAM is decoded on the CPU into RGBA textures
  in sub-page cells, keyed by page, palette and the freshness of the VRAM rows
  each cell covers. A per-palette lookup table converts a cell in one pass.
- **Track.** Each level's track is baked into a resident mesh at load time and
  drawn by the GPU with a true perspective projection
  (`shaders/ctr_track.v.pica`), with a depth range fitted per BSP leaf. There is
  no PS1 vertex snapping or affine texture warping on the road. Animated
  textures and vertex animation (water, turbo pads) patch the resident mesh.
- **Models.** Karts, drivers and instances draw through a native model path on
  the GPU (`shaders/ctr_model.v.pica`) with the same projection.
- **Everything else.** UI, particles, effects and any remaining PS1 primitive go
  through the packet path: the game's ordering table is walked and converted to
  GPU triangles.
- **Depth.** The PS1 sorts by draw order; the 3DS uses a depth test. Every
  emitted primitive carries a depth resolved through `PacketDepth_ResolveXY`:
  GTE-derived depth where the geometry has it, otherwise its emit ordinal, so
  retail draw order survives as depth. Ground decals (shadows, skid marks, tire
  contact) get fixed depth brackets so they neither sink nor float. The UI is
  drawn off the depth test, in order. An emit path that skips the resolver draws
  through walls; the `[PKTDEPTH] escaped` counter must stay zero.
- **Draw merge.** Opaque primitives are binned by GPU state across the walk and
  submitted in a handful of draws; blended primitives are replayed in order. A
  frame is around twenty draws.
- **Transparency.** Semi-transparent surfaces draw twice: once opaque with depth
  writes, once blended without.
- **Widescreen.** The top screen renders a wider field of view. Widening the
  orthographic window the game projects through is widening the field of view;
  the HUD ramps out to the new edges.
- **Stereoscopic 3D.** The frame's draw commands are journaled and the right eye
  replays the journal through a second projection. The slider sets the
  separation frame by frame; the UI sits on the screen plane with a small
  pop-out.
- **Bottom screen.** The map's primitives are marked in the UI stream, peeled
  off at conversion time, and replayed on a bottom-screen target through an
  orthographic window fitted to their bounds. In map mode a copy stays on the
  top screen as well. The frame-rate readout is drawn in the game's own font
  inside the same bracket.
- **Not rendered.** The close-up "mosaic" texture tier, and retail's full-screen
  feedback blur (the clock item's screen effect and the intro flyby's ghost
  trail). Texture filtering is an option, off by default.

Rules the hardware enforces and emulators do not:

- Write every component of every declared shader output. A vertex is emitted
  only once all components of all `.out` registers are written; a
  partially-written output stalls the shader unit and the next frame begin
  waits forever on a blank screen.
- Vertex-shader float uniforms are physical registers shared across every
  shader program. Declare `projection[4]` first in every program, or re-assert
  it after a state bind.
- `mul` cannot take a uniform or constant as its second operand; swap the
  operand order.
- Route every geometry emit path through `PacketDepth_ResolveXY`.
- Azahar runs the CPU at real New 3DS speed but hides all of the above, and its
  frame timing is not the console's. A change that only ran in the emulator is
  unproven.

## Audio

- The SPU's voices play as hardware voices on DSP channels 0 to 22 through
  ndsp. Channel 23 carries the XA music stream at its native sample rate, or
  the whole software mix when the hardware voices are unavailable. Software
  mixing on the ARM11 is not fast enough to be the primary path.
- XA music streams from the disc image on the SD card. The XA voice lines are
  cached while the Sony logo is on screen (`Loading audio`), so a race never
  waits on the card for speech.
- Output gain is twice retail's master level, because the PS1's master volume
  of 0x3fff is half of digital full scale.

## Disc, saves and memory

- The game reads `assets/ctr-u.bin` from the first of `sdmc:/3ds/ctr_native`,
  `sdmc:/3ds/ctr-native`, `sdmc:/ctr-native` and `sdmc:/` that contains it, and
  changes directory there. Saves live in `memcards/` beside it. A read cache and
  prefetch cover the XA streams and level loads.
- The heap keeps the PS1's 2 MB MEMPACK arena size on purpose: it flags any
  change that needs more memory than the original had. Memory is not the
  constraint on this hardware; CPU time is.

## Debug surface and measuring

- `CTR_INTERNAL` (CMake option, default ON; the release is built with it OFF)
  compiles the debug surface: `sdmc:/dumpreq.txt`, a file of one word per line
  polled on the first frame and every 64 frames after (`stereo`, `sdlog`,
  `bilinear`, ...), SELECT chords, `[PERF]` scope windows, screenshots and probes.
  OFF strips all of it from the binary and changes no default.
- In any build, an empty `enable-log.txt` beside the `.3dsx` turns on the SD log
  at `sdmc:/ctr3ds.log`. It is flushed on a timer, because a flush per line
  stalls the game on the card's latency.
- Frame-rate claims count only from a wall clock on hardware: the `[PERF]`
  windows in the SD log, or the on-screen readout.

## Build options

| Option | 3DS default | Effect |
|---|---|---|
| `CTR_INTERNAL` | ON (release: OFF) | The debug surface above |
| `CTR_NATIVE_TRACK_RENDERER`, `CTR_NATIVE_MODEL_RENDERER`, `CTR_NATIVE_GTE_CAPTURE` | ON | The resident track and model paths and the sub-pixel capture table stereo is built on; the OFF legs do not link on 3DS |
| `CTR_NATIVE_DIRECT_EMIT` | ON | Direct packet-to-bin emit for all-opaque ordering-table nodes |
| `CTR_NATIVE_FEEDBACK_BLUR` | OFF | Retail's full-screen feedback blur; PC keeps it ON |
| `CTR_NATIVE_AUDIO_DECODE_PROFILE`, `CTR_NATIVE_DISC_IO_PROFILE`, `CTR_NATIVE_OTWALK_PROFILE`, `CTR_NATIVE_PRIMMEM_WATERMARK` | OFF | Counters and log lines for profiling builds |
