/*
 * Boundary between the game's unity translation unit and libctru.
 *
 * This header deliberately exposes NOTHING from <3ds.h> -- no libctru types,
 * no libctru constants -- and is safe to include from the unity build. That
 * separation is not stylistic. Pulling <3ds.h> into the unity TU collides with
 * the decompilation in three concrete ways:
 *
 *   1. include/psx/r3000.h defines the MIPS assembler aliases `flush` and
 *      `inval`, which mangle libctru's own parameter names (boss.h and nfc.h
 *      both declare parameters called `inval`).
 *   2. libctru typedefs `ThreadFunc` as void(*)(void*); the game already has
 *      its own ThreadFunc taking a struct Thread*.
 *   3. libctru typedefs u8/u32/s32 to the stdint types, which is exactly the
 *      binding include/macros.h has to avoid on ARM (see the note there).
 *
 * n3ds_backend.c is therefore compiled as its own translation unit -- it sees
 * libctru and never sees the game headers. Everything crossing this line uses
 * plain C scalar types.
 */

#ifndef N3DS_BACKEND_H
#define N3DS_BACKEND_H

/* --------------------------------------------------------------- timing --- */
/* Monotonic tick counter and its rate. Backed by the ARM11 system tick, which
 * runs at a fixed frequency regardless of New3DS CPU speed mode. */
unsigned long long N3DS_GetTickCount(void);
unsigned long long N3DS_GetTickFrequency(void);
void N3DS_SleepMs(unsigned int ms);

/* ------------------------------------------------------------- graphics --- */
void N3DS_GfxInit(void);

/* Full-screen error panel for failures that end the run: startup asset and
 * graphics failures, an Old 3DS, a disc read that cannot succeed. Without it
 * the app would exit to the Homebrew Launcher with nothing on screen. Tears
 * down whatever gfx/citro3d state is still up, brings up its own console on
 * the top screen, waits for any button or ~60s, and releases the screen; the
 * caller exits straight after. message is plain text, '\n'-separated lines of
 * at most 44 columns. Lives in this TU because the console and HID calls are
 * libctru, which the unity TU cannot see. */
void N3DS_ShowFatalScreen(const char *message);

/* 1 on a New 3DS / New 3DS XL / New 2DS XL. The port needs that model's memory
 * and CPU; an original 3DS fails its boot-time allocations. */
int N3DS_IsNew3DS(void);
void N3DS_GfxExit(void);
/* Push the current framebuffer to the screen. Does NOT wait for vblank --
 * frame pacing belongs to VSync() in the platform layer. */
void N3DS_GfxPresent(void);

/* -------------------------------------------------------------- render --- */
/* citro3d lives entirely behind this boundary too, for the same reason as
 * everything else here -- see the file comment. Types crossing it are plain
 * scalars only. This is the mechanical GPU boundary for
 * the renderer; it grows as native_renderer_3ds.c needs
 * new operations, phase by phase, rather than being designed complete
 * up front. */

/* <stdint.h>, not <3ds.h> or macros.h, for s16/u8 below -- this header is
 * included directly and EARLY from the unity build (main.c, before
 * <platform.h>/macros.h are reached, so their s16/u8 don't exist yet in that
 * TU at this point), as well as from n3ds_backend.c (which sees libctru's own
 * s16/u8 via <3ds.h>, included before this header there). Typedef'ing them
 * here too, rather than assuming either side already has, is what keeps this
 * header genuinely "safe to include from the unity build" per the file
 * comment above -- not just for the old all-float struct, which never needed
 * a sized type at all. Not a collision risk: GCC accepts a typedef name being
 * redeclared with an identical underlying type (confirmed against this exact
 * toolchain/-std=gnu99), and libctru's 3ds/types.h and macros.h both spell
 * s16/u8 as int16_t/uint8_t too -- verified by reading both, not assumed. */
#include <stdint.h>
typedef int16_t s16;
typedef uint8_t u8;

/* Plain-scalar mirror of the vertex layout the 3DS GPU consumes. Narrower
 * than the shared GrVertex (include/platform/native_renderer_types.h):
 * page/clut never reach the GPU here -- texture resolution happens on the
 * CPU side of native_renderer_3ds.c before a draw call (design doc section
 * 1). Packed to 12 bytes -- GPU_SHORT position/texcoord + GPU_UNSIGNED_BYTE
 * color, down from the original 32-byte all-float layout (docs/
 * VERTEX_FORMAT_DESIGN.md, "Lever 2") -- because PICA200 vertex fetch does
 * NOT normalize integer attribute formats: a GPU_SHORT of 6000 arrives in
 * the shader as 6000.0f, not 6000.0f/32767.0f. Verified against Azahar's own
 * vertex-loader source and a disassembly of citro3d's AttrInfo_AddLoader
 * (design doc finding 4), not assumed. The shader (ctr.v.pica) rescales
 * texcoord/color back to their old numeric range itself instead.
 *
 * Field order (position, texcoord, color) must NOT change: it matches the
 * GPU attribute-register permutation (0x210, set in N3DS_C3DInit below) and
 * is what lands every attribute on its own element-size boundary with zero
 * inter-attribute padding (design doc finding 5) -- reordering would force a
 * gap the struct doesn't provide, which PICA200 hardware can react to by
 * freezing rather than just drawing wrong.
 *
 * The trailing pad byte is not slop: without it the stride is 11 (odd), so
 * odd-indexed vertices' SHORT-typed fields land at odd byte offsets -- an
 * unaligned access real hardware may not tolerate. This risk is specifically
 * NOT visible by testing in Azahar (its vertex loader reads via an x86
 * reinterpret_cast that silently tolerates misalignment ARM hardware might
 * not), so don't drop this byte just because it "looks unused" on an
 * emulator screenshot -- see design doc finding 5 for the full reasoning. */
typedef struct
{
	s16 x, y, z, w;
	s16 u, v;
	u8 r, g, b;
	u8 _pad;
} N3DS_Vertex;

/*
 * z carries the depth substrate (z-break play): GrVertex.emitOrdinal biased
 * into s16 range (ordinal - 32768), so later-submitted primitives are
 * "nearer". Overlay quads drawn outside the OT walk (VRAM present, atlas
 * inspector) use N3DS_DEPTH_TOPMOST so they beat any world depth; they also
 * draw last, so either polarity keeps them on top.
 *
 * w is the PARALLAX channel (GrVertex.parallaxOrdinal, biased identically) --
 * the depth the STEREO shader uses, which is not always the depth the test
 * occludes by. See native_renderer_types.h's field comment;
 * for everything except the menu/loading
 * flag it equals z exactly. It is NOT a homogeneous w: ctr.v.pica sets the
 * position's own w to a constant 1.0 and reads this only for parallax, so
 * nothing here divides by it.
 *
 * 16-byte stride keeps every rule the comment above establishes: position
 * (4 x s16) at 0, texcoord at 8, color at 12 -- each on its element-size
 * boundary, no inter-attribute padding, even stride. The attribute loader in
 * N3DS_C3DInit fetches 4 shorts for v0 to match; changing one without the
 * other is the class of mistake PICA200 answers by FREEZING rather than
 * drawing wrong, and Azahar will not reproduce it (finding 5).
 */
#define N3DS_DEPTH_TOPMOST 32767

/* One-time setup: citro3d itself, the top-screen render target, the
 * compiled vertex shader, attribute layout, and the vertex buffer. Must be
 * called after N3DS_GfxInit (needs gfxInitDefault already done). Returns 0
 * on failure. */
int N3DS_C3DInit(void);
void N3DS_C3DExit(void);

/* Must bracket every N3DS_C3DAppendVertices/DrawTriangles call for a given
 * display frame -- native_renderer_3ds.c calls these from
 * NativeRenderer_BeginScene/EndScene (once per frame, per Platform_Begin/
 * EndScene's own re-entrancy guard), NOT from SwapWindow: real drawing
 * happens earlier, via DrawAllSplits (DrawOTag, and potentially again mid-
 * frame via DrawSync), so the frame bracket has to open before that, not at
 * present time. */
/*
 * Orthographic extents in PSX screen coordinates, not 3DS panel pixels. top is
 * always 0 and bottom > top, reproducing the PSX Y-down convention.
 */
void N3DS_C3DSetProjection(float left, float right, float bottom);

/* Mirror mode (include/native_mirror.h): the world is drawn mirrored this frame. Set before
 * N3DS_C3DBeginFrame, which latches it with the stereo vectors; it reverses the track cull and
 * turns on the packet shader's world flip. The track and model rows mirror themselves CPU-side. */
void N3DS_SetMirrorWorld(int on);
void N3DS_C3DBeginFrame(void);
void N3DS_C3DEndFrame(void);

/*
 * citro3d free-runs two per-frame timers -- processing time
 * (C3D_FrameBegin..C3D_FrameEnd, i.e. the whole worker frame including
 * packet replay) and drawing time (PICA hardware execution, stamped when the
 * GX queue drains). This fetches the window averages in integer microseconds
 * and resets. frames is the backend's own count -- it can differ from the
 * [PERF] window's 64 (menus, loads), so it's reported rather than assumed.
 */
void N3DS_C3DGpuTimeWindow(int *cpuUs, int *gpuUs, unsigned *frames);

/*
 * Cost of the SD-log flush inside N3DS_DebugLog, since the last call (which
 * resets). The flush is throttled to ~2s -- the same cadence as the [PERF]
 * window -- so if it is expensive it lands as exactly one monster frame per
 * window.
 */
void N3DS_LogFlushStats(unsigned *worstUs, unsigned *totalUs, unsigned *count);

/* ------------------------------------------------------------- stereo 3D --- */
/*
 * Depth-layered stereoscopic 3D (PORT_FEASIBILITY_3DS.md "Stereoscopic 3D",
 * Option A grown into per-vertex parallax): the physical 3D slider drives a
 * per-eye horizontal shift computed in all three vertex programs from the
 * shared view.z depth axis. The left eye is
 * the ordinary frame (drawn with the left eye's negative parallax when the
 * slider is up, zero when it is down); the right eye is a REPLAY of the same
 * frame's already-recorded draws -- merge-bin emits by (baseVertex, count)
 * into the same persistent frame VBO, track leaves from the resident mesh,
 * model runs from their already-appended vertices -- against a second render
 * target, with only the parallax uniform flipped. No game code runs twice,
 * no vertices are re-appended, no textures re-resolve.
 *
 * N3DS_C3DBeginFrame latches the slider for the whole frame (journal reset,
 * eye = left); the backend's Track/Model brackets and the renderer's merge
 * emit record into the journal only while a stereo frame is active, so a
 * slider at zero costs nothing over today's frame. The renderer calls
 * N3DS_StereoRenderRightEye once per frame from EndScene, just before
 * N3DS_C3DEndFrame; it no-ops in mono. Right-eye draws are NOT counted in
 * s_drawCallCount / [PERF] draw stats -- those remain per-eye numbers.
 *
 * Record hooks exposed to the renderer TU (both no-op when not recording):
 * RecordPacket mirrors one N3DS_MergeEmit (state resolved, not policy);
 * RecordVram mirrors one PresentVRAM chunk quad (its texture slot contents
 * persist to frame end, so the replay samples this frame's own pixels).
 */
void N3DS_StereoRecordPacket(int blendMode, int doubling, int depthWrite, int atlas, int baseVertex, int vertCount);
void N3DS_StereoRecordVram(int slot, int baseVertex);
void N3DS_StereoRenderRightEye(void);
/* Max far-plane parallax in PSX px, rounded up (0 when stereo is
 * master-disabled): flat sky-class content that must reach the screen edges
 * overscans X by this so a per-eye shift cannot expose the window edge.
 * Constant per stereosep, not slider-tracking. */
int N3DS_StereoSkyPadX(void);

/*
 * One real vertex buffer per FRAME, not per draw. C3D_DrawArrays only
 * appends GPU commands to a command list; the GPU doesn't actually read any
 * vertex data until that whole list is submitted at once, in C3D_FrameEnd
 * (GX_ProcessCommandList) -- confirmed directly against citro3d's upstream
 * renderqueue.c. DrawAllSplits/DrawSplit (native_gpu.c) call
 * NativeRenderer_DrawTriangles once per split -- up to MAX_DRAW_SPLITS
 * (4096) times a frame -- so a frame routinely queues many draws before any
 * of them actually executes. N3DS_C3DAppendVertices therefore writes each
 * caller's vertices into the NEXT free region of one persistent buffer and
 * returns where they landed; the caller must draw from that returned offset,
 * not 0. C3D_FRAME_SYNCDRAW's frame-to-frame block does NOT serialise draws
 * within one frame -- writing to and drawing from a fixed offset both work
 * only if nothing else appends first, since GPU execution happens only
 * after all of a frame's CPU-side writes are done.
 * N3DS_C3DBeginFrame resets the write cursor to 0 every frame.
 *
 * This budget covers a whole FRAME. DrawOTag calls DrawAllSplits once and
 * DrawSync can call it up to two more times, so a naive per-pass ceiling
 * (PC's MAX_VERTEX_BUFFER_SIZE) would under-budget a frame by up to 3x;
 * overflow is silent (N3DS_C3DAppendVertices returns -1, N3DS_FlushRun drops
 * the run) so the budget must cover the true worst case and report a
 * high-water mark.
 *
 * Peak measured occupancy stays well under this budget (single-digit
 * percent), so the ceiling is sized for the theoretical worst case, not
 * observed content; overflow reporting (droppedRuns) exists so real content
 * that does exceed it is loud instead of silently dropping geometry.
 *
 * Do NOT "fix" a future overflow by resetting the cursor per pass. Every
 * draw queued this frame is still unexecuted until C3D_FrameEnd, so reusing
 * offsets within one frame reintroduces exactly the aliasing bug above.
 */
#define N3DS_MAX_FRAME_VERTICES 65536
/* Largest single atlas-run, i.e. the size of native_renderer_3ds.c's
 * conversion scratch. A run flushes whenever the atlas changes or this
 * fills, so it bounds one staging buffer rather than a frame. Stays at PC's
 * per-pass ceiling: no single run can exceed one pass's total geometry. */
#define N3DS_MAX_RUN_VERTICES 65536
/* Returns the vertex offset this call's data was written at (pass to
 * N3DS_C3DDrawTriangles), or -1 if the frame buffer is full -- caller must
 * skip drawing in that case, not draw from a stale/wrong offset. */
int N3DS_C3DAppendVertices(const N3DS_Vertex *vertices, int count);
void N3DS_C3DDrawTriangles(int startVertex, int triangleCount);
/* Highest vertex count reached in any single frame so far. Reported in the
 * stats block: if this ever approaches N3DS_MAX_FRAME_VERTICES the budget
 * above is too small for real content and geometry is about to disappear. */
int N3DS_C3DFrameVertexHighWater(void);

/* ---------------------------------------------------- decoded textures --- */
/* CPU-decoded-texture cache backing store (design doc section 1), organised
 * as a small number of shared texture atlases rather than one dedicated
 * texture per decoded (page,clut,format) combo. Each atlas is a fixed
 * GPU_RGBA5551 texture (see N3DS_TILE_BYTES in
 * n3ds_backend.c), nearest-filtered, clamped, holding a grid of
 * N3DS_DECODE_TEXTURE_DIM-sized cells; native_renderer_3ds.c owns cache-
 * replacement policy (which (page,clut,format) is decoded into which
 * (atlas,cell)) and calls UploadDecodedCell to overwrite one cell's content.
 *
 * Why atlases, not one texture per entry: PC's AddSplit doesn't split by
 * page/clut, so one scene commonly touches many (page,clut) combos in an
 * interleaved triangle stream; citro3d can bind only one texture per draw,
 * so without atlasing every combo change forces a new bind+draw.
 *
 * N3DS_ATLAS_DIM is 1024 because citro3d's own checkTexSize caps any single
 * texture at 1024x1024 -- confirmed directly against citro3d's upstream
 * texture.c, not assumed -- so this is the largest, and therefore
 * highest-cells-per-bind, atlas actually obtainable.
 *
 * IMPORTANT: citro3d's C3D_TexLoadImage (which C3D_TexUpload wraps) is a raw
 * memcpy over a WHOLE mip level -- verified directly against citro3d's
 * upstream source -- with no sub-rect upload API at all, so a single cell's
 * decoded content can't go through it. N3DS_C3DUploadDecodedCell instead
 * writes directly into the atlas's own tiled backing memory, computing the
 * cell's tile-block offset by hand (same PICA200 8x8 Z-order tiling as
 * before -- see the comment on N3DS_DecodeAndTilePage in native_renderer_3ds.c
 * for the verified tile-order derivation this extends). tiledRGBA8 (the
 * per-call input) must already be tiled the same way a whole-texture upload
 * would need to be. */
/*
 * Cell count is the binding constraint: an undersized cache causes both
 * excessive re-decoding and eviction of cells a still-queued draw
 * references, which reads as texture corruption. The working set is far
 * larger than a small cell count can hold, hence the larger
 * N3DS_ATLAS_COUNT/DIM below.
 */
#define N3DS_DECODE_TEXTURE_DIM    256
/*
 * N3DS_ATLAS_COUNT is retuned from the boot log's own "linear free before
 * atlases" line, not guessed -- N3DS_C3DBackend_InitialisePSX logs which
 * atlas index fails and the free KiB remaining, so that log is the source of
 * truth for this constant, not any number recorded here.
 *
 * The old note here said "8 atlases (32MB) does NOT allocate": true, and still
 * true, but that was 8 RGBA8 atlases. The real constraint is the linear
 * ceiling logged at init, not any specific atlas count -- keep reading that
 * log line rather than trusting a fixed number here.
 *
 * Why cells and not something cleverer: playing the menu measured a SATURATED
 * 63/63 working set with ~45 capacity misses per frame and 2052 evictions of
 * cells that a queued draw still pointed at. Cell count was the binding
 * constraint on both the corruption and the decode cost.
 *
 * 11, not 12: the dedicated track-atlas pool (N3DS_TrackAtlas_Alloc, below)
 * competes with these atlases for the same linear heap, and 12 leaves too
 * little free for a typical level's track-atlas units. */
#define N3DS_ATLAS_COUNT           11
#define N3DS_ATLAS_DIM             1024
#define N3DS_ATLAS_CELLS_PER_ROW   (N3DS_ATLAS_DIM / N3DS_DECODE_TEXTURE_DIM)
#define N3DS_ATLAS_CELLS_PER_ATLAS (N3DS_ATLAS_CELLS_PER_ROW * N3DS_ATLAS_CELLS_PER_ROW)

/* tiled256 buffer is RGBA5551 (2 bytes/texel), not RGBA8 -- see N3DS_TILE_BYTES. */
void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256);
/* Sub-page counterpart -- writes an
 * 8x8-tile (64x64 texel) sub-cell at physical cell (physCellX,physCellY)'s
 * (subX,subY) quadrant-of-16, instead of a whole 32x32-tile cell. tiled64 is
 * RGBA5551, same tiling convention as tiled256 above, just a quarter the
 * cell's linear dimension. Both wrap the shared N3DS_C3DUploadTileBlock in
 * n3ds_backend.c. */
void N3DS_C3DUploadDecodedSubCell(int atlasIndex, int physCellX, int physCellY, int subX, int subY, const void *tiled64);
void N3DS_C3DBindAtlas(int atlasIndex);
/* Enables PC's x2 "bright" multiplier on RGB, applied after the texture
 * modulate the way PC's fragment shader does it. Must not be folded into the
 * vertex colour instead -- see the implementation comment for why that loses
 * headroom and darkens everything textured. */
void N3DS_C3DSetColorDoubling(int enable);
/* PS1 semi-transparency: blendMode is the shared BlendMode enum's numeric
 * value (BM_NONE=0, BM_AVERAGE=1, BM_ADD=2, BM_SUBTRACT=3,
 * BM_ADD_QUATER_SOURCE=4 -- native_renderer_types.h), passed as a plain int
 * across this boundary like every other enum here. Deduped internally;
 * takes effect at the next recorded draw. */
void N3DS_C3DSetBlendMode(int blendMode);

/* Depth-WRITE mask only (the LEQUAL test stays on). Translucent content must
 * test but not write; see N3DS_MergeEmit in native_renderer_3ds.c. NOT deduped
 * internally -- the caller memoizes, unlike the blend setter above. */
void N3DS_C3DSetDepthWrite(int enable);

/* ------------------------------------------------------ VRAM display --- */
/* Dedicated (not atlas-shared) textures backing NativeRenderer_PresentVRAMRect/
 * PresentVRAMDisplay -- the boot-time SCEA/copyright VRAM-mirror blit. This
 * is boot-only content that never competes with the decode cache for atlas
 * capacity, so it gets its own small C3D_Tex pool rather than a shared cell.
 * tiled256 is a whole 256x256 RGBA5551 buffer, uploaded via the real
 * C3D_TexUpload -- unlike N3DS_C3DUploadDecodedCell's manual sub-rect write,
 * these textures are never sub-addressed, so the plain whole-texture wrapper
 * applies.
 *
 * A POOL, not one shared texture: citro3d defers GPU command execution to
 * C3D_FrameEnd, but C3D_TexUpload is an immediate CPU memcpy (same hazard
 * N3DS_C3DUploadDecodedCell's eviction policy avoids for the atlas, see its
 * header comment above). A single present call chunks a wide rect into
 * multiple pieces uploaded and drawn in the same still-open frame -- with one
 * shared texture, chunk 2's upload would overwrite chunk 1's texture memory
 * before chunk 1's already-queued draw actually executes, so both draws
 * would sample chunk 2's content. slotIndex selects which pool texture a
 * given chunk uses; the caller is responsible for giving each chunk drawn
 * within the same present call a distinct slot. 4 slots covers a 2x2 grid,
 * i.e. up to 512x512 -- comfortable headroom over the ~512x216 DISPENV this
 * is actually used for. */
#define N3DS_VRAM_DISPLAY_TEX_DIM   256
#define N3DS_VRAM_DISPLAY_TEX_COUNT 4
void N3DS_C3DUploadVramDisplayChunk(int slotIndex, const void *tiled256);
void N3DS_C3DBindVramDisplayTex(int slotIndex);

/* -------------------------------------------------- native track atlas --- */
/* Dedicated pool for the native track renderer's atlas layout
 * (platform/native_track_mesh.c). Separate from the decode-cache atlases above --
 * smaller units (512x512 vs 1024x1024), matching the packer's own
 * TRACK_ATLAS_DIM -- and (re)allocated per level load rather than once at
 * boot, since the packed layout itself is computed per level from that
 * level's own texture working set. Same format/filter/wrap as the decode
 * cache (GPU_RGBA5551, GPU_NEAREST, clamp-to-edge) and the same manual
 * tiled-upload path (no sub-rect C3D_TexUpload -- see the decode-cache
 * section's header comment above for why).
 *
 * N3DS_TRACK_ATLAS_DIM/MAX_UNITS live HERE, not in n3ds_backend.c, because
 * native_track_mesh.c's packer (TRACK_ATLAS_DIM/MAX_UNITS, its own file-
 * local macros) sizes the SAME pool from the other side of the plain-scalar
 * boundary and must never drift from these -- native_track_mesh.c carries a
 * compile-time check (CTR_STATIC_ASSERT) against these two exact macros.
 * Plain integer macros, so exposing them here is
 * ABI-safe -- no libctru type crosses the boundary. */
#define N3DS_TRACK_ATLAS_DIM       512
#define N3DS_TRACK_ATLAS_MAX_UNITS 8

/* Lazily (re)allocates unitCount textures (clamped to N3DS_TRACK_ATLAS_MAX_UNITS
 * above). Frees any previously allocated pool first (level reload) -- the
 * pool is sized per level, not per game session. Logs linear free KiB
 * before allocating and, on failure, which unit index failed and the free
 * KiB remaining at that point -- same idiom as the decode-cache atlas init
 * in N3DS_C3DBackend_InitialisePSX. Returns the number of units actually
 * allocated (< unitCount only on an allocation failure). */
int N3DS_TrackAtlas_Alloc(int unitCount);
void N3DS_TrackAtlas_Free(void);
/* Writes an 8x8-tile-aligned rect into unit's backing memory, via the same
 * shared tile-block writer the decode cache's N3DS_C3DUploadDecodedCell/
 * N3DS_C3DUploadDecodedSubCell use. dstX, dstY, wTexels, and hTexels MUST
 * all be multiples of 8 -- PICA200 tile addressing, the same constraint
 * the decode cache's cellX/cellY/subX/subY satisfy implicitly by
 * construction -- AND the rect must fit inside N3DS_TRACK_ATLAS_DIM; both
 * are validated and rejected (logged, no-op) rather than trusted, since a
 * violation here would silently corrupt neighbouring tile memory instead of
 * just this rect's content. tiledRGBA5551 must already
 * be Z-order tiled with stride wTexels/8 tiles (exactly what
 * N3DS_DecodeAndTilePage produces when called with destStrideTiles =
 * wTexels/8). */
void N3DS_TrackAtlas_UploadRect(int unit, int dstX, int dstY, int wTexels, int hTexels, const void *tiledRGBA5551);
/* For drawing. Same bind pattern as N3DS_C3DBindAtlas. */
void N3DS_TrackAtlas_Bind(int unit);

/* -------------------------------------------------- native track draw --- */
/* The
 * resident-mesh draw path, a SECOND vertex format/shader/attribute layout
 * living alongside the packet path's N3DS_Vertex/ctr.v.pica above. Plain
 * scalars only, same TU-boundary rule as everything else in this header.
 *
 * N3DS_TrackVertex mirrors platform/native_track_mesh.c's struct
 * NativeTrackVert (16B, carries texRef/faceFlags the GPU does not need) into
 * a 14-byte GPU-side layout: model-space position, page-space UV (unused
 * until textured draws), and color + the signed per-face draw-order
 * byte BIASED BY +128 so it can ride in an UNSIGNED byte group without
 * clashing with r/g/b's own 0..255 range (a signed GPU_BYTE group would read
 * 255 as -1 for every color channel too -- see n3ds_backend.h's own
 * N3DS_Vertex comment on why integer attribute formats are never normalized
 * by the vertex fetch). Every s16 field lands on an even offset and the
 * struct's own size is even, same alignment rule N3DS_Vertex's trailing pad
 * byte exists for (n3ds_backend.h's own comment on that struct explains why
 * this is a hardware-only, Azahar-invisible risk class). */
typedef struct
{
	s16 x, y, z; /* offset 0 -- model space, straight from LevVertex */
	s16 u, v;    /* offset 6 -- page-space texel coords */
	u8 r, g, b;  /* offset 10 -- color_hi */
	u8 drawOrder; /* offset 13 -- signed per-face OT byte, biased +128 */
} N3DS_TrackVertex;

/* Uploads one level's resident mesh: ONE linearAlloc block, vertices at
 * offset 0, indices placed after them at an 8-byte-aligned offset (a single
 * block is what makes the index physical address strictly greater than the
 * C3D_BufInfo base by construction, which is what C3D_DrawElements' index
 * pointer needs to encode as an offset from that base -- see
 * N3DS_TrackDraw_Leaf's comment for what could and couldn't be confirmed
 * about that encoding from the headers installed locally). vertStride is
 * always sizeof(N3DS_TrackVertex) today; passed explicitly rather than
 * assumed so a future caller changing the layout can't silently desync the
 * upload from the struct. Frees any previous allocation first (level
 * reload, same idiom as N3DS_TrackAtlas_Alloc). Returns 0 on allocation
 * failure (nothing uploaded, any previous geometry already freed) or when
 * vertCount/indexCount is degenerate. */
int N3DS_TrackGeom_Upload(const void *verts, int vertCount, int vertStride, const void *indices, int indexCount);
/* Called from NativeTrackMesh_Free alongside N3DS_TrackAtlas_Free -- a
 * level's only teardown path. Safe to call with nothing uploaded. */
void N3DS_TrackGeom_Free(void);
/* Rewrite `count` already-uploaded vertices' UV pairs in place, batched.
 * uvPairs holds two shorts per entry, in the same encoding the upload wrote
 * (2x fixed point, texel centre, V flipped -- one implementation of that lives
 * in native_track_mesh.c's NativeTrackAtlas_EncodeUV and BOTH writers go
 * through it). Bounds-checked against the uploaded vertex count and followed
 * by a single cache flush over the touched range; see the .c definition for
 * the thread rule. This is what makes animated track textures -- turbo pads,
 * waterfalls, lava -- move under the resident renderer. */
void N3DS_TrackGeom_PatchUV(const unsigned short *vertIndices, const short *uvPairs, int count);
/* The same in-place batched rewrite for POSITION (three shorts per entry)
 * and COLOR (three bytes per entry) -- the animated-VERTEX case, i.e. water
 * surfaces that physically move. Kept separate from PatchUV because the two
 * populations and cadences are nothing alike; see the .c definition. */
void N3DS_TrackGeom_PatchPosColor(const unsigned short *vertIndices, const short *xyz, const unsigned char *rgb, int count);

/* Flush the GPU's copy of the vertices patched this frame in ONE cache-flush
 * over the union of the ranges PatchUV/PatchPosColor touched. Call once after
 * all patching for the frame; those functions only widen the range now, they
 * no longer flush per chunk (which re-flushed most of the buffer many times a
 * frame -- the animated verts scatter across it). */
void N3DS_TrackGeom_FlushPatched(void);
/* Direct form of the two Patch calls above: the caller writes N3DS_TrackVertex
 * fields in place between Begin and End, staying under vertCount, then names
 * the inclusive vertex range it touched so FlushPatched covers it. Begin
 * returns NULL when no geometry is uploaded. Same thread rule as the Patch
 * calls: only after the previous frame's GPU work has been awaited. */
void *N3DS_TrackGeom_PatchBegin(int *vertStride, int *vertCount);
void N3DS_TrackGeom_PatchEnd(int loVert, int hiVert);

/* Begin/Leaf/End bracket one frame's resident-track draw batch, called from
 * NativeRenderer_EndScene immediately before the packet
 * path's own merge-bin barrier flush. Begin binds the track program/
 * attribute layout/vertex+index buffer, uploads the camera compose (qx/qy/qw
 * -- platform/native_track_frame.c derives these once per frame from the
 * double-buffered snapshot) and H, and configures the fragment combiner for
 * vertex-color-only output (3a is untextured). Leaf draws one BSP leaf's
 * index range with its own per-leaf OT-bucket depth-range uniform (design
 * 4.3/12b's zOrd compose). End restores EVERY piece of packet-path GPU state
 * this bracket touched (program, attribute layout, vertex buffer, fragment
 * combiner) so the packet path's own merge-bin flush, issued right after,
 * draws correctly -- see N3DS_TrackDraw_End's own comment for the exact
 * list. No-op (Leaf silently skips its draw) if Upload was never called or
 * last failed. */
/* -------------------------------------------------- native model draw --- */
/* Model instances drawn from
 * model-space geometry through ctr_model.v.pica, on the same depth axis as the
 * packets and the resident track.
 *
 * Deliberately thinner than the track's API above, because models REUSE the
 * packet path's vertex plumbing rather than owning their own. Model geometry is
 * per-frame dynamic (animation is delta-decoded on the CPU every frame -- see
 * the design's section 3), so there is no resident buffer to upload; it goes
 * through N3DS_C3DAppendVertices like any packet run. And N3DS_Vertex already
 * has exactly the fields a model vertex needs -- position, texcoord, colour --
 * so the packet attribute layout and buffer binding are reused verbatim. The
 * ONLY thing that differs is which vertex program is bound and what its
 * uniforms say, which is what these four calls do.
 *
 * Field reinterpretation, stated because it is the one subtle part: on the
 * packet path N3DS_Vertex.x/y are SCREEN pixels and z is a depth stamp. Here
 * x/y/z are the model-space position the software GTE would have received, and
 * the shader does the projection. Same 14 bytes, same loader, different
 * meaning -- so never mix model and packet vertices in one draw.
 *
 * Bracketing: Begin binds the program and uploads the shared projection;
 * Instance uploads one instance's composed camera rows (see the shader header
 * for the derivation); Run draws one texture-homogeneous span of the vertices
 * appended for that instance; End restores every piece of packet-path state,
 * the same contract N3DS_TrackDraw_End has. Instance may be called many times
 * between one Begin/End pair. */
void N3DS_ModelDraw_Begin(void);
void N3DS_ModelDraw_Instance(const float *qx, const float *qy, const float *qw, float h);
/* startVertex is an N3DS_C3DAppendVertices return value; triangleCount spans
 * 3*triangleCount vertices from it. atlasIndex is a decode-cache atlas (the
 * same pool the packet path binds); textured 0 draws vertex-colour only, which
 * is a third of a real frame (measured, design section 7) and not an edge
 * case. */
void N3DS_ModelDraw_Run(int startVertex, int triangleCount, int atlasIndex, int textured, int blend);
void N3DS_ModelDraw_End(void);

void N3DS_TrackDraw_Begin(const float *qx, const float *qy, const float *qw, float h);
/* atlasBin: which atlas unit these indices sample. At or past the
 * allocated unit count means no placed texture -- drawn untextured. */
void N3DS_TrackDraw_Leaf(int firstIndex, int indexCount, float otIndexLo, float ordinalLo, float ordinalSpan, float invIndexSpan, int atlasBin, int blend, int uvDx, int uvDy);
void N3DS_TrackDraw_End(void);

/* The top-screen CONTENT region: N3DS_C3DBeginFrame pillarboxes every
 * presented frame to a centred 4:3 (320-wide) viewport, so screen columns
 * 0..39 and 360..399 are always black bars, not picture. Consumers that
 * read the presented frame back must sample only this region. Keep in
 * lockstep with the C3D_SetViewport call in N3DS_C3DBeginFrame. */
#define N3DS_TOP_CONTENT_X 40
#define N3DS_TOP_CONTENT_W 320
/* REAL-widescreen fills the whole panel instead of the content region above;
 * the renderer publishes the mode every EndScene so N3DS_CopyDisplayedTopFrame
 * samples the region the game actually drew. */
void N3DS_SetTopContentFullWidth(int fullWidth);

/* Copies the top screen's last completed frame (the gfx framebuffer the
 * present path fills every frame) into dstRGB as row-major RGB triplets,
 * nearest-resampled from the pillarboxed CONTENT region (N3DS_TOP_CONTENT_*,
 * above -- NOT the full 400-wide panel) to dstW x dstH. May be one
 * presentation old. Returns 0 if no framebuffer.
 */
int N3DS_CopyDisplayedTopFrame(unsigned char *dstRGB, int dstW, int dstH);

/* ------------------------------------------------ bottom-screen minimap --- */
/* Second-screen pass for the in-race minimap (renderer's
 * N3DS_DrawBottomMapPass, dumpreq 'btmmap'). Begin lazily creates the bottom
 * render target (non-fatal: returns 0 when unavailable and the caller must
 * skip -- same boots-without-it contract as the stereo right eye), switches
 * drawing onto it for the rest of the frame, sets a full-panel viewport,
 * re-asserts the frame's depth policy, binds the packet draw state, and
 * uploads an ortho window of [left..right] x [top..bottom] PSX units (Y-down,
 * top < bottom) -- the magnification IS the window, no vertex is touched. It
 * also suppresses the debug console's bottom-screen painting for a few
 * presents (the console resumes by itself once the pass goes quiet). End
 * restores the stored top-screen projection: N3DS_C3DBeginFrame re-uploads
 * the STORED matrix every frame and N3DS_C3DSetProjection only rebuilds it on
 * a display-mode change, so leaving the window matrix in place would project
 * every following top frame through the map window. Call strictly between
 * N3DS_StereoRenderRightEye and N3DS_C3DEndFrame. The parallax uniform is
 * left as the last eye set it, deliberately: every routed primitive carries a
 * UI-band stamp, which the shader's own band mask flattens to zero shift. */
int N3DS_BottomMapDraw_Begin(float left, float right, float top, float bottom);
/* Re-aim the window between Begin and End (the FPS overlay's smaller window
 * after the map's). Uniform sequencing is safe mid-frame: citro3d records the
 * dirty uniform into the command list at the next draw, the same pattern the
 * per-instance model uploads rely on. */
void N3DS_BottomMapDraw_SetWindow(float left, float right, float top, float bottom);
void N3DS_BottomMapDraw_End(void);
/* Nonzero while the map pass holds the bottom panel. The stats drip painter
 * must check this and hold off: console glyphs land in the shared framebuffer
 * at PRINT time, so the present-side flush/swap suppression alone cannot stop
 * text from scribbling over the map. Stats composition and the log mirror
 * continue; only the console painting pauses, and the drip's own row rotation
 * scrubs the map's pixels back off within one sweep after release. */
int N3DS_BottomMapOwnsScreen(void);
/* The single console-visibility answer for every glyph painter (the stats
 * drip and Platform_Log's stdout echo): false while the map owns the panel,
 * false once the game is presenting unless the stats console is toggled on
 * (dumpreq 'stats'), true during boot so pre-game errors stay readable. */
int N3DS_ConsolePaintAllowed(void);
/* One console row for a pre-game hold's progress, claimed under the boot text
 * already on the panel and repainted in place -- no clear, no scroll, no SD
 * write and no netload send. Each call blits that row's glyph cells into the
 * column-major rotated console framebuffer, so call it only when the text
 * changes. Deliberately outside N3DS_ConsolePaintAllowed; see the definition.
 * Clear blanks the row and releases it. */
void N3DS_ConsoleStatusLine(const char *text);
void N3DS_ConsoleStatusClear(void);

/* ---------------------------------------------------------------- input --- */

#define N3DS_BTN_A      (1u << 0)
#define N3DS_BTN_B      (1u << 1)
#define N3DS_BTN_X      (1u << 2)
#define N3DS_BTN_Y      (1u << 3)
#define N3DS_BTN_L      (1u << 4)
#define N3DS_BTN_R      (1u << 5)
#define N3DS_BTN_START  (1u << 6)
#define N3DS_BTN_SELECT (1u << 7)
#define N3DS_BTN_UP     (1u << 8)
#define N3DS_BTN_DOWN   (1u << 9)
#define N3DS_BTN_LEFT   (1u << 10)
#define N3DS_BTN_RIGHT  (1u << 11)
#define N3DS_BTN_ZL     (1u << 12) /* PSX L2, see native_input.c kc_l2 */
#define N3DS_BTN_ZR     (1u << 13) /* PSX R2, see native_input.c kc_r2 */

/* Samples the pad and returns held buttons as N3DS_BTN_* bits. D-pad and
 * circle pad are folded together. */
unsigned int N3DS_ReadButtons(void);

/* -------------------------------------------------------------- logging --- */
/* Emits to the console's debug string service. Azahar records these in its log
 * and Luma shows them on hardware, so this is the one output channel that works
 * without a screen or a writable SD card. */
void N3DS_DebugLog(const char *msg);
/* Immediate SD commit of the buffered log (freeze/stall evidence). */
void N3DS_LogFlushNow(void);

/* Points stderr at that same debug channel. Call before anything that can fail,
 * so early startup errors are visible even though the screen console does not
 * exist until the renderer initialises. */
void N3DS_EarlyInit(void);

/* Nonzero when the app was netloaded via 3dslink and the stderr stream back to
 * the sending PC is connected (run 3dslink with -s to receive it). Platform_Log
 * mirrors into that stream so hardware hang diagnostics arrive live over Wi-Fi
 * instead of needing an SD pull. */
int N3DS_NetloadStreamActive(void);

/* Nonzero while the SD-card log sink is enabled (g_dbg_sdLog; SELECT+R or
 * dumpreq `sdlog`, default off). Platform_Log's file sink honours the same
 * switch as N3DS_DebugLog's, so one control governs every SD write this build
 * makes -- see the policy note at Platform_LogWrite. */
int N3DS_SdLogEnabled(void);

/* Turns that same sink on or off. Lives outside the debug-surface gate so a
 * shipped build can still be asked for a log (see the enable-log.txt check at
 * boot in main.c); the SELECT+R chord and dumpreq `sdlog` are the CTR_INTERNAL
 * routes to the same switch. */
void N3DS_SetSdLogEnabled(int enabled);

/* Registers the emulated PSX VRAM buffer so the debug capture (dumpreq `capture`) in
 * N3DS_ReadButtons can write it to sdmc:/vramdump_<frame>.bin. Called once
 * from the renderer TU (which owns vram[]) since the backend TU can't see
 * that global. bytes is sizeof(vram). frameCounter points at the renderer's
 * frame counter (plain unsigned int, so no ABI hazard) and stamps every
 * capture filename, so consecutive captures never overwrite each other.
 * CTR_INTERNAL-only diagnostic (texture-corruption investigation);
 * no-op otherwise. */
void N3DS_DebugSetVRAMForDump(const void *vram, unsigned int bytes, const unsigned int *frameCounter);

/* ---------------------------------------------------------------- memory --- */
/* Bytes currently handed out by malloc, and free space in the linear heap.
 * Reported on screen so runaway allocation is visible as it happens. */
unsigned long N3DS_HeapUsedBytes(void);
unsigned long N3DS_LinearFreeBytes(void);
/* One log line: heap size and bytes in use, linear heap size and bytes free, tagged with `where`,
 * flushed to the card before it returns. Boot and load only. */
void N3DS_LogMemory(const char *where);

/* ------------------------------------------------------------------ app --- */
/* Zero once the user has closed the applet (HOME/power), meaning the process
 * should shut down. */
int N3DS_MainLoopActive(void);

#endif
