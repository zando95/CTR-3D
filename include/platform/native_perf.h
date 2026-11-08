#ifndef NATIVE_PERF_H
#define NATIVE_PERF_H

#include <macros.h>

enum NativePerfBucket
{
	NATIVE_PERF_BUCKET_GAME_LOGIC,
	NATIVE_PERF_BUCKET_RENDER_FRAME,
	NATIVE_PERF_BUCKET_MAINFRAME_SETUP,
	NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS,
	NATIVE_PERF_BUCKET_MAINFRAME_HUD,
	NATIVE_PERF_BUCKET_MAINFRAME_QUEUE_INSTANCES,
	NATIVE_PERF_BUCKET_MAINFRAME_EXECUTE_INSTANCES,
	NATIVE_PERF_BUCKET_MAINFRAME_LEVEL_GEOMETRY,
	NATIVE_PERF_BUCKET_MAINFRAME_POST_LEVEL,
	NATIVE_PERF_BUCKET_MAINFRAME_REFRESHCARD,
	NATIVE_PERF_BUCKET_MAINFRAME_CLEAR_SCREEN,
	NATIVE_PERF_BUCKET_MAINFRAME_UI,
	NATIVE_PERF_BUCKET_MAINFRAME_RENDER_VSYNC,
	NATIVE_PERF_BUCKET_PLATFORM_END_FRAME,
	NATIVE_PERF_BUCKET_PLATFORM_END_SCENE,
	NATIVE_PERF_BUCKET_RENDER_SUBMIT,
	NATIVE_PERF_BUCKET_PLATFORM_BEGIN_SCENE,
	NATIVE_PERF_BUCKET_DRAW_OTAG,
	NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE,
	NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS,
	NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE,
	NATIVE_PERF_BUCKET_RENDERER_UPDATE_VRAM,
	NATIVE_PERF_BUCKET_RENDERER_VERTEX_UPLOAD,
	NATIVE_PERF_BUCKET_RENDERER_DRAW_TRIANGLES,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_STORE,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_RESIZE,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_BLIT,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_FLUSH,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_ALLOC,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_READBACK,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_PACK,
	NATIVE_PERF_BUCKET_FRAMEBUFFER_VRAM_UPLOAD,
	NATIVE_PERF_BUCKET_SWAP_WINDOW,
	NATIVE_PERF_BUCKET_VSYNC_WAIT,
	NATIVE_PERF_BUCKET_AUDIO_VBLANK,
	/*
	 * 3DS-only. The citro3d renderer replaces platform/native_renderer.c
	 * wholesale on 3DS, so none of the framebuffer_* buckets above ever fire
	 * there and its two most interesting costs -- the GPU submit at
	 * C3D_FrameEnd and the bottom-screen console present -- had nowhere to
	 * land. Appended rather than inserted: s_bucketInfo in native_perf.c is
	 * indexed by this enum and must stay in the same order.
	 */
	NATIVE_PERF_BUCKET_N3DS_FRAME_END,
	NATIVE_PERF_BUCKET_N3DS_GFX_PRESENT,
	/*
	 * Voice-start ADPCM decode/scan on the key-on path (NativeAudio_
	 * UpdateVoiceSample, both call sites). Runs on the game thread inside
	 * game logic, so without its own bucket its cost lands invisibly in
	 * game_logic_ms -- which is how the sound-start frame spikes hid.
	 * Appended, never inserted:
	 * s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_AUDIO_VOICE_START,
	/*
	 * Sub-phases of RENDERER_DRAW_TRIANGLES (3DS renderer): per-triangle
	 * atlas-cell resolution (decode-cache lookup/decode), per-triangle
	 * GrVertex->N3DS_Vertex conversion, and N3DS_FlushRun's two halves --
	 * the CPU copy into the shared frame vertex buffer (append) vs. the
	 * atlas bind + draw enqueue (flush). They nest inside the parent
	 * bucket, so their sum <= renderer_draw_triangles_ms; the remainder is
	 * loop/scope overhead. Appended, never inserted: s_bucketInfo is
	 * indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_DRAW_TRIANGLES_RESOLVE,
	NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT,
	NATIVE_PERF_BUCKET_DRAW_TRIANGLES_APPEND,
	NATIVE_PERF_BUCKET_DRAW_TRIANGLES_FLUSH,
	/*
	 * OT-walk profile (CTR_NATIVE_OTWALK_PROFILE): a links-only pre-walk of
	 * the same OT the real walk is about to parse, isolating pointer-chase
	 * cost from translate/submit. It runs BEFORE the real walk, so the real
	 * walk then runs cache-warm: read this bucket as an upper bound on chase
	 * and (draw_otag_parse_ms − draw_otag_drywalk_ms) as a rough translate
	 * cost. Appended, never inserted: s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_DRAW_OTAG_DRYWALK,
	/*
	 * Direct-emit fast path (CTR_NATIVE_DIRECT_EMIT): the emit pass of
	 * NativeDirectEmit_TryNode, i.e. the per-triangle resolve + bin-write
	 * work running INSIDE the walk. Behind g_dbg_perfTriScopes (one scope
	 * per handled node, ~1-1.5k/frame in a race) because this work is what
	 * grows draw_otag_parse_ms when the fast path is ON -- this bucket splits
	 * true parse from moved emit.
	 */
	NATIVE_PERF_BUCKET_DIRECT_EMIT,
	/*
	 * Per-instance bounding-box projection + OT range allocation
	 * (RenderBucket_BuildDepthRange), nested inside
	 * mainframe_queue_instances_ms. Exists to answer: of the per-object setup
	 * cost, how much SURVIVES a PICA-native model path? This slice does not --
	 * the bbox's OT-bucket-range purpose is erased by a depth buffer, and its
	 * off-screen reject becomes two dot products on the GPU instead of nine
	 * projections through the software-emulated GTE. What stays (matrix
	 * compose, LOD pick, anim-frame pick) is the remainder of the parent
	 * bucket. Reading the parent alone would price work intended for deletion
	 * as work that must be kept. Appended, never inserted: s_bucketInfo is
	 * indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_MODEL_DEPTH_RANGE,
	/*
	 * Per-frame track-animation patch (NativeTrackAnim_PatchFrame): the
	 * scatter writes that make turbo pads, waterfalls and lava move under the
	 * resident renderer, plus the cache flush each batch needs before the GPU
	 * reads it. Measured because the flush cost is range-proportional and the
	 * whole resident renderer is only worth ~6% -- a patch that quietly spends
	 * a millisecond a frame would be eating most of the feature it decorates.
	 * Sits INSIDE the walk (draw_otag_ms), right after its BeginScene. Appended, never
	 * inserted: s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_TRACK_ANIM,
	/*
	 * The resident renderers' own GPU-issue passes, and the merge flush that
	 * precedes them, all sit INSIDE platform_end_scene_ms, so dominantCandidate
	 * is 0 for all three: they are slices of a bucket that is already a
	 * candidate. Appended, never inserted: s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_NATIVE_TRACK_DRAW,
	NATIVE_PERF_BUCKET_NATIVE_MODEL_DRAW,
	NATIVE_PERF_BUCKET_MERGE_FLUSH,
	/*
	 * DrawSync(0) inside RenderVSYNC's poll loop -- the game thread waiting for
	 * the RENDER WORKER, as opposed to waiting for the 30fps clock (which is
	 * vsync_wait_ms). Splitting them distinguishes worker-bound stalls from
	 * clock-bound idle time within mainframe_render_vsync_ms.
	 * Appended, never inserted: s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_RENDER_VSYNC_DRAWSYNC,
	/*
	 * Total time the render worker spends INSIDE job executors, summed over
	 * every job in the frame. If this lands near the game thread's DrawSync
	 * block time, the worker is genuinely busy and the missing time is inside
	 * an executor with no scope of its own. If it lands well under that, the
	 * gap is the queue handshake (lock/unlock + signal per job), fixed by
	 * batching rather than by drawing less.
	 * Appended, never inserted: s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_WORKER_EXEC,
	/*
	 * Torch_Main -- the heat-warp renderer (missiles/fire/warp pads). Its
	 * prims texture from the LIVE FRAMEBUFFER with a fresh tpage per ring
	 * sector, so its true cost lands downstream in draw_otag_parse /
	 * renderer_begin_scene where it is indistinguishable from everything
	 * else's; this bucket plus the [TORCH] window line exist to correlate
	 * those spikes with torch activity. Nests inside mainframe_effects_ms.
	 * Appended, never inserted: s_bucketInfo is indexed by this enum.
	 */
	NATIVE_PERF_BUCKET_TORCH_MAIN,
	/*
	 * Main-thread disc read (native_disc_image.c ReadRawRun, owner=MAIN only --
	 * the audio-thread XA reads use owner=AUDIO and are deliberately excluded,
	 * both to avoid racing this main-thread scope and because they don't stall
	 * the main frame). Exists to attribute the ~50ms UNTIMED main-thread spikes
	 * the spike catcher found on Tiger Temple: a synchronous mid-race disc read
	 * is the prime suspect. Appended, never inserted: s_bucketInfo is indexed by
	 * this enum.
	 */
	NATIVE_PERF_BUCKET_DISC_READ_MAIN,
	NATIVE_PERF_BUCKET_COUNT
};

struct NativePerfFrameInfo
{
	s32 frameCounter;
	s32 timer;
	s32 levelID;
	s32 gameMode1;
	s32 loadingStage;
	s32 boolDemoMode;
	s32 numPlyrCurrGame;
	s32 elapsedTimeMS;
	s32 vsyncTillFlip;
	s32 vSync_between_drawSync;
	s32 frameTimer_VsyncCallback;
};

#if defined(CTR_INTERNAL)
int NativePerf_ConfigureFromArgs(int argc, char **argv);
void NativePerf_Shutdown(void);
/*
 * The levelID [PERF] itself stamps on every window (s_frameBeginInfo, captured
 * at frame begin). Exposed so other instruments tag their lines from the SAME
 * source rather than a lookalike level field, which can silently diverge.
 */
int NativePerf_CurrentLevelID(void);

void NativePerf_BeginFrame(const struct NativePerfFrameInfo *info);
void NativePerf_EndFrame(const struct NativePerfFrameInfo *info);
void NativePerf_BeginScope(enum NativePerfBucket bucket);
void NativePerf_EndScope(enum NativePerfBucket bucket);
/*
 * Torch (heat-warp) activity counters, folded into the [TORCH] window line on
 * 3DS. listed = particles walked (including ones culled before emit),
 * emitted = particles that actually produced their 14 framebuffer-textured
 * prims. Split so a spike can be attributed to emission (prims in the OT)
 * rather than mere list presence (GTE projection only).
 */
void NativePerf_CountTorch(int listed, int emitted);

/*
 * Flush the spike-catcher ring to sdmc:/ctr_spikes.log now (3DS only; no-op on
 * PC and when perf is compiled out). Normally the ring auto-flushes on a level
 * change; this is the on-demand path for dumpreq 'spikes'.
 */
void NativePerf_DumpSpikesNow(void);

/*
 * Wall-clock milliseconds on the perf counter, for instruments in game/ that
 * time spans outside the frame scopes (the load stages run before
 * NativePerf_BeginFrame).
 */
double NativePerf_WallMs(void);

#ifdef CTR_INTERNAL
/*
 * The DrawOTag job (the ~35ms "walk" the main thread waits behind) split into
 * its three phases with the worker's OWN clock -- the NativePerf buckets for
 * these undercount, since the job runs async on core 2 outside the main thread's
 * perf frame. patch = NativeTrackAnim_PatchFrame (moving water/lava rewrite),
 * parse = ParsePrimitivesLinkedList (the packet OT walk itself), splits =
 * DrawAllSplits (flush bins to the GPU). Defined in native_libgpu.c where the
 * job body lives. TakeOtagSplit copies and zeroes.
 */
struct NativeOtagSplitStats
{
	unsigned long patchUs, parseUs, splitsUs;
	unsigned int calls;
};

void NativePerf_TakeOtagSplit(struct NativeOtagSplitStats *out);
#endif
#else
/*
 * Level id for instruments that outlive the profiler. Instrument log lines
 * that are not themselves part of the debug surface still stamp a level, so
 * the latch survives the gate even though nothing else in the profiler does;
 * NativePerf_BeginFrame is the one writer either way.
 */
extern int g_nativePerfLevelID;

static inline int NativePerf_CurrentLevelID(void)
{
	return g_nativePerfLevelID;
}

static inline int NativePerf_ConfigureFromArgs(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

static inline void NativePerf_Shutdown(void)
{
}

static inline void NativePerf_DumpSpikesNow(void)
{
}

static inline void NativePerf_BeginFrame(const struct NativePerfFrameInfo *info)
{
	if (info != NULL)
	{
		g_nativePerfLevelID = (int)info->levelID;
	}
}

static inline void NativePerf_EndFrame(const struct NativePerfFrameInfo *info)
{
	(void)info;
}

static inline void NativePerf_BeginScope(enum NativePerfBucket bucket)
{
	(void)bucket;
}

static inline void NativePerf_EndScope(enum NativePerfBucket bucket)
{
	(void)bucket;
}

static inline void NativePerf_CountTorch(int listed, int emitted)
{
	(void)listed;
	(void)emitted;
}
#endif

#endif
