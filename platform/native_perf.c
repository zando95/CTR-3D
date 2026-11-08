#include "platform/native_perf.h"

#if defined(CTR_INTERNAL)
#include <macros.h>

#include "platform/native_audio.h"
#include "platform/native_log.h"
#include "platform/native_path.h"
#include "platform/native_render_queue.h"

#if !defined(__3DS__)
#include <SDL3/SDL.h>
#else
#include "n3ds/n3ds_audio_thread.h"
#include "n3ds/n3ds_render_thread.h" /* N3DS_RenderWorkerIsSelf */
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define NATIVE_PERF_PATH_MAX         1024
#define NATIVE_PERF_DEFAULT_DIR      "debug/perf/perf-latest"
#define NATIVE_PERF_FRAME_CSV_NAME   "frame_times.csv"
#define NATIVE_PERF_SUMMARY_NAME     "summary.txt"
#define NATIVE_PERF_TOP_FRAME_COUNT  20
#define NATIVE_PERF_FLUSH_INTERVAL   120
#define NATIVE_PERF_FRAME_BUDGET_MS  (1000.0 / 30.0)
#define NATIVE_PERF_FRAME_WARN_40_MS 40.0
#define NATIVE_PERF_FRAME_WARN_50_MS 50.0

#if defined(__3DS__)
/*
 * The 3DS has no command line, so there is no --perf flag to opt in with, and
 * no host-side place to drop a CSV: writing one would mean a formatted SD-card
 * write every frame, which is itself expensive enough to distort what we came
 * to measure. So on 3DS the profiler self-enables and reports a WINDOWED
 * summary to the existing sdmc:/ctr3ds.log instead.
 *
 * Windowed, not cumulative, on purpose: a running average since boot smears
 * the loading phase into the steady state and hides exactly the phase changes
 * we are hunting. Reporting is also the only output path that survives the
 * usual end of a session -- the app is killed, so NativePerf_Shutdown never
 * runs and a shutdown-only summary would produce nothing at all.
 */
#define NATIVE_PERF_3DS_REPORT_FRAMES 64
/* 12 buckets: the report ranks ALL buckets together, and the top of that
 * ranking is chronically saturated by the ~6 nested aggregates that wrap
 * every frame (render_frame > render_submit > draw_otag > draw_all_splits >
 * renderer_draw_triangles, plus mainframe_render_vsync) -- at a lower count,
 * leaf buckets like the draw_triangles_* sub-slices could never print at
 * all. 12 = four full 3-per-line rows; the extra visibility costs two more
 * SD-card log lines per 64-frame window. */
#define NATIVE_PERF_3DS_TOP_BUCKETS   12
#endif

struct NativePerfBucketInfo
{
	const char *name;
	s32 dominantCandidate;
};

struct NativePerfWorstFrame
{
	u32 frameIndex;
	f64 totalMs;
	f64 workMs;
	f64 overBudgetMs;
	f64 workOverBudgetMs;
	enum NativePerfBucket dominantBucket;
	f64 dominantMs;
	struct NativePerfFrameInfo info;
	s32 audioUnderrunDelta;
	s32 audioOverflowDelta;
	s32 audioQueuedFrames;
};

global_variable const struct NativePerfBucketInfo s_bucketInfo[NATIVE_PERF_BUCKET_COUNT] = {
    {"game_logic_ms", 1},
    {"render_frame_ms", 0},
    {"mainframe_setup_ms", 1},
    {"mainframe_effects_ms", 1},
    {"mainframe_hud_ms", 1},
    {"mainframe_queue_instances_ms", 1},
    {"mainframe_execute_instances_ms", 1},
    {"mainframe_level_geometry_ms", 1},
    {"mainframe_post_level_ms", 1},
    {"mainframe_refreshcard_ms", 1},
    {"mainframe_clear_screen_ms", 1},
    {"mainframe_ui_ms", 1},
    {"mainframe_render_vsync_ms", 0},
    {"platform_end_frame_ms", 0},
    {"platform_end_scene_ms", 0},
    {"render_submit_ms", 1},
    {"platform_begin_scene_ms", 1},
    {"draw_otag_ms", 0},
    {"draw_otag_parse_ms", 1},
    {"draw_all_splits_ms", 1},
    {"renderer_begin_scene_ms", 1},
    {"renderer_update_vram_ms", 1},
    {"renderer_vertex_upload_ms", 1},
    {"renderer_draw_triangles_ms", 1},
    {"framebuffer_store_ms", 0},
    {"framebuffer_resize_ms", 1},
    {"framebuffer_blit_ms", 1},
    {"framebuffer_flush_ms", 1},
    {"framebuffer_alloc_ms", 1},
    {"framebuffer_readback_ms", 1},
    {"framebuffer_pack_ms", 1},
    {"framebuffer_vram_upload_ms", 1},
    {"swap_window_ms", 1},
    {"vsync_wait_ms", 0},
    {"audio_vblank_ms", 1},
    {"n3ds_c3d_frame_end_ms", 1},
    {"n3ds_gfx_present_ms", 1},
    {"audio_voice_start_ms", 1},
    /* dominantCandidate 0: these nest inside renderer_draw_triangles_ms,
     * which is already a candidate -- letting a sub-slice win "dominant"
     * would double-report the same time. */
    {"draw_triangles_resolve_ms", 0},
    {"draw_triangles_convert_ms", 0},
    {"draw_triangles_append_ms", 0},
    {"draw_triangles_flush_ms", 0},
    /* dominantCandidate 0: the dry walk re-times part of draw_otag_ms, which
     * is already a candidate -- letting it win "dominant" would double-report
     * the same traversal. */
    {"draw_otag_drywalk_ms", 0},
    /* dominantCandidate 0: a sub-slice of draw_otag_parse_ms (see the
     * bucket's header comment) -- letting it win "dominant" would
     * double-report the same time. */
    {"direct_emit_ms", 0},
    /* dominantCandidate 0: nests inside mainframe_queue_instances_ms, which is
     * already a candidate -- letting a sub-slice win "dominant" would
     * double-report the same time. */
    {"model_depth_range_ms", 0},
    /* dominantCandidate 0: nests inside draw_otag_ms (it runs at the head of
     * the walk, on the walk thread) -- see the enum comment. */
    {"track_anim_ms", 0},
    /* dominantCandidate 0: all three nest inside platform_end_scene_ms -- see
     * the enum comment. Named native_* so they never read as the retail
     * packet path's draw_* buckets. */
    {"native_track_draw_ms", 0},
    {"native_model_draw_ms", 0},
    {"merge_flush_ms", 0},
    /* dominantCandidate 0: nests inside mainframe_render_vsync_ms. */
    {"render_vsync_drawsync_ms", 0},
    /* dominantCandidate 0: a re-timing of work already inside draw_otag_ms /
     * platform_end_scene_ms, from the worker's side of the fence. */
    {"worker_exec_ms", 0},
    /* dominantCandidate 0: nests inside mainframe_effects_ms, which is
     * already a candidate. */
    {"torch_main_ms", 0},
    /* Main-thread disc read; a dominant candidate -- it IS the thing we want to
     * see win "dominant" on a stalled frame. Appended to match the enum. */
    {"disc_read_main_ms", 1},
};

global_variable FILE *s_csvFile;
global_variable s32 s_enabled;
global_variable s32 s_frameOpen;
global_variable s32 s_rowsSinceFlush;
global_variable u32 s_frameIndex;
global_variable u32 s_framesOverBudget;
global_variable u32 s_workFramesOverBudget;
global_variable u32 s_framesOver40;
global_variable u32 s_framesOver50;
global_variable f64 s_totalFrameMs;
global_variable f64 s_totalWorkMs;
global_variable f64 s_maxFrameMs;
global_variable f64 s_maxWorkMs;
global_variable f64 s_bucketTotals[NATIVE_PERF_BUCKET_COUNT];
global_variable f64 s_frameBucketMs[NATIVE_PERF_BUCKET_COUNT];
global_variable u64 s_frameStartCounter;
/* End of the last closed frame, for [OUTFRAME]: time the main thread spends
 * between frames (load stages, queued file loads) is otherwise unmeasured. */
global_variable u64 s_perfLastFrameEndCounter;
global_variable u64 s_scopeStartCounter[NATIVE_PERF_BUCKET_COUNT];
global_variable s32 s_scopeDepth[NATIVE_PERF_BUCKET_COUNT];
global_variable struct NativePerfFrameInfo s_frameBeginInfo;
/* int, not s32: these are written through NativeAudio_GetOutputStats(int *...). */
global_variable int s_beginUnderrunFrames;
global_variable int s_beginOverflowFrames;
global_variable int s_beginQueuedFrames;
global_variable char s_outputDir[NATIVE_PERF_PATH_MAX];
global_variable char s_csvPath[NATIVE_PERF_PATH_MAX];
global_variable char s_summaryPath[NATIVE_PERF_PATH_MAX];
global_variable struct NativePerfWorstFrame s_worstFrames[NATIVE_PERF_TOP_FRAME_COUNT];
global_variable s32 s_worstFrameCount;
/* Torch (heat-warp) activity, see NativePerf_CountTorch. Frame counters reset
 * every BeginFrame on all platforms; the window aggregates below them feed the
 * 3DS [TORCH] line. */
global_variable s32 s_torchFrameListed;
global_variable s32 s_torchFrameEmitted;
#if defined(__3DS__)
/*
 * This variable's name is part of its correctness. A same-named
 * uninitialized file-scope static in another unity-TU file silently MERGES
 * with this one (C tentative definitions). Keep cross-file static names
 * unique; see CLAUDE.md's unity-build hazard note.
 */
global_variable u32 s_windowFrames;
global_variable f64 s_windowTotalMs;
global_variable f64 s_windowWorkMs;
global_variable f64 s_windowBucketMs[NATIVE_PERF_BUCKET_COUNT];

/*
 * Spike catcher: the windowed [PERF] average hides the frames that actually
 * hurt -- a 30fps-median track still feels like the mid-20s when a handful of
 * frames stall past budget. This records only the DROPPED frames (total > 40ms)
 * with the perf bucket that dominated each, into a small RAM ring, then flushes
 * the ring in ONE batched write to sdmc:/ctr_spikes.log on a level change (a
 * load screen, where a stall is invisible). It deliberately does NOT ride the
 * per-frame SD log path, so it runs with g_dbg_sdLog OFF -- which matters
 * because that log's own writes are themselves a top spike source, and would
 * otherwise be the loudest thing in the data. dumpreq 'spikes' dumps/toggles.
 */
#define NATIVE_PERF_SPIKE_RING 96
struct NativePerfSpike
{
	u32 frameCounter;
	f32 totalMs;
	f32 workMs;
	f32 dominantMs;
	/* Top two buckets by RAW time, aggregates included (render_frame,
	 * game_logic). The dominant bucket above is the biggest LEAF candidate and
	 * reads ~3ms on these spikes; the raw top tells which HALF of the frame the
	 * unaccounted ~50ms lives in even when it sits between leaf scopes. */
	f32 rawTop1Ms;
	f32 rawTop2Ms;
	/* Render-worker walk breakdown for THIS frame (core-2 DrawOTag job, peeked
	 * and deltaed on the main thread): patch = track-anim rewrite, parse = the
	 * OT walk, splits = DrawAllSplits (queueing draws to the GPU). If these sum
	 * near the 47ms wait, the walk is the cost; if they stay small, the wait is
	 * the GPU-sync/present remainder past the walk. */
	f32 walkPatchMs;
	f32 walkParseMs;
	f32 walkSplitsMs;
	s32 levelID;
	u32 gameMode1;
	enum NativePerfBucket dominantBucket;
	enum NativePerfBucket rawTop1;
	enum NativePerfBucket rawTop2;
};
global_variable struct NativePerfSpike s_spikeRing[NATIVE_PERF_SPIKE_RING];
global_variable u32 s_spikeCount;
global_variable u32 s_spikeSeenTotal;
global_variable s32 s_spikeLastLevel;
global_variable s32 s_spikeLevelValid;
global_variable s32 s_spikePostFlushSkip;
int g_dbg_spikeCatch = 1; /* dumpreq 'spikes' (default on for the spike hunt) */
/* Last peek of the worker walk's cumulative ticks, for per-frame deltas. */
global_variable u64 s_spikeLastPatchTicks;
global_variable u64 s_spikeLastParseTicks;
global_variable u64 s_spikeLastSplitsTicks;
global_variable f32 s_spikeWalkPatchMs;
global_variable f32 s_spikeWalkParseMs;
global_variable f32 s_spikeWalkSplitsMs;

#if defined(__3DS__) && CTR_NATIVE_RENDER_THREAD
/*
 * Worker-thread scope accumulators. The render worker runs jobs async on core 2,
 * so its scopes can't use the main thread's path: that one gates on s_frameOpen
 * (usually closed when the worker runs) and folds through a per-frame array the
 * main thread resets underneath it -- which is why worker_exec, draw_otag,
 * track_anim, native_track_draw, merge_flush all read ~6x low. The worker
 * instead accumulates straight into its own window total here, ungated except by
 * s_enabled and using its own depth/start arrays (never touched by main, so no
 * BeginFrame-memset race). Folded into s_windowBucketMs in Report3DSWindow
 * behind that function's existing NativeRenderQueue_Fence() -- the one moment
 * the worker is provably idle -- so the read+zero cannot tear.
 */
global_variable f64 s_workerWindowMs[NATIVE_PERF_BUCKET_COUNT];
global_variable s32 s_workerScopeDepth[NATIVE_PERF_BUCKET_COUNT];
global_variable u64 s_workerScopeStart[NATIVE_PERF_BUCKET_COUNT];
#endif

global_variable u32 s_torchWindowListed;
global_variable u32 s_torchWindowEmitted;
/* Worst single frame's emit count: the window average smears a one-frame
 * missile volley into nothing, same failure mode the voice-start profiler
 * exists for. */
global_variable s32 s_torchWindowMaxFrameEmitted;
/* g_dbg_ablateConfig as it stood when this window opened; a change cuts the
 * window so no report ever averages two ablation configs together. */
global_variable const char *s_windowCfg;

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
/*
 * Worst single frame in the window plus the key-on count ON THAT FRAME --
 * the voice-start correlation test.
 * The window's 64-frame averages smear a one-frame decode spike into
 * nothing, which is exactly how it went unnoticed; the hypothesis is
 * confirmed iff worst frames consistently coincide with keyOns > 0 in race
 * content. Defined in native_audio.c later in this same unity TU.
 */
internal int NativeAudio_DebugConsumeFrameKeyOns(void);
internal void NativeAudio_DebugFormatVoiceProfWindow(char *buf, int bufSize, int maxFrameUs, int maxFrameKeyOns);
global_variable int s_windowMaxFrameKeyOns;
#endif

/* Worst single frame in the window, in ms. Outside the profile gate above
 * because the bottom-screen console reads it in every config -- only the
 * key-on correlation beside it needs the audio profiler compiled in. */
global_variable f64 s_windowMaxTotalMs;

/*
 * Last reported window, mirrored for the bottom-screen console in
 * native_renderer_3ds.c. On real hardware the top screen may be blank and the
 * SD card lives behind the backplate, so the console is often the only
 * readable output -- these let a photo of it carry the attribution, not just
 * the frame rate.
 */
#define NATIVE_PERF_3DS_CONSOLE_BUCKETS 2
global_variable int s_perfConsoleTotalUs;
global_variable int s_perfConsoleWorkUs;
global_variable int s_perfConsoleWorstUs;
global_variable const char *s_perfConsoleName[NATIVE_PERF_3DS_CONSOLE_BUCKETS];
global_variable int s_perfConsoleUs[NATIVE_PERF_3DS_CONSOLE_BUCKETS];
#endif

internal f64 NativePerf_CounterToMs(u64 counterDelta)
{
	const u64 freq = SDL_GetPerformanceFrequency();

	if (freq == 0)
	{
		return 0.0;
	}

	return ((f64)counterDelta * 1000.0) / (f64)freq;
}

internal const char *NativePerf_BucketName(enum NativePerfBucket bucket)
{
	if ((bucket < 0) || (bucket >= NATIVE_PERF_BUCKET_COUNT))
	{
		return "unknown_ms";
	}

	return s_bucketInfo[bucket].name;
}

#if !defined(__3DS__)
/*
 * CSV plumbing and argument parsing, all PC-only: the 3DS build self-enables
 * and reports through the log, so leaving these visible there is just an
 * unused-function warning per helper.
 */
internal s32 NativePerf_PathExists(const char *path)
{
	struct stat st;

	if (path == NULL)
	{
		return 0;
	}

	return stat(path, &st) == 0;
}

internal s32 NativePerf_MakeDir(const char *path)
{
	if ((path == NULL) || (path[0] == '\0'))
	{
		return 1;
	}

#if defined(_WIN32)
	if (_mkdir(path) == 0)
		return 1;
#else
	if (mkdir(path, 0777) == 0)
	{
		return 1;
	}
#endif

	return (errno == EEXIST) && NativePerf_PathExists(path);
}

internal s32 NativePerf_CreateDirs(const char *path)
{
	char copy[NATIVE_PERF_PATH_MAX];
	char *cursor;
	s32 ok = 1;

	if ((path == NULL) || (path[0] == '\0'))
	{
		return 0;
	}

	if (!NativePath_NormalizeSlashes(copy, sizeof(copy), NativeStr8_FromCString(path)))
	{
		return 0;
	}

	cursor = copy;
	if (NativePath_IsSeparator((u8)cursor[0]))
	{
		cursor++;
	}
#if defined(_WIN32)
	if ((cursor[0] != '\0') && (cursor[1] == ':') && NativePath_IsSeparator((u8)cursor[2]))
		cursor += 3;
#endif

	while (*cursor != '\0')
	{
		if (NativePath_IsSeparator((u8)*cursor))
		{
			char saved = *cursor;

			*cursor = '\0';
			if ((copy[0] != '\0') && !NativePerf_MakeDir(copy))
			{
				ok = 0;
				break;
			}
			*cursor = saved;
		}
		cursor++;
	}

	if ((ok != 0) && !NativePerf_MakeDir(copy))
	{
		ok = 0;
	}

	return ok;
}

internal s32 NativePerf_SetOutputDir(const char *path)
{
	if (!NativePath_NormalizeSlashes(s_outputDir, sizeof(s_outputDir), NativeStr8_FromCString(path)))
	{
		return 0;
	}

	if (!NativePath_Join(s_csvPath, sizeof(s_csvPath), NativeStr8_FromCString(s_outputDir), NATIVE_STR8_LIT(NATIVE_PERF_FRAME_CSV_NAME)))
	{
		return 0;
	}

	if (!NativePath_Join(s_summaryPath, sizeof(s_summaryPath), NativeStr8_FromCString(s_outputDir), NATIVE_STR8_LIT(NATIVE_PERF_SUMMARY_NAME)))
	{
		return 0;
	}

	return 1;
}

internal void NativePerf_WriteCsvHeader(FILE *file)
{
	fprintf(file, "frame_index,total_ms,work_ms,over_budget_ms,work_over_budget_ms,dominant_bucket,dominant_ms,frameCounter,timer,level_id,game_mode1,"
	              "loading_stage,bool_demo_mode,num_plyr,elapsed_time_ms,vsync_till_flip,vSync_between_drawSync,frameTimer_VsyncCallback,"
	              "audio_underrun_delta,audio_overflow_delta,audio_queued_frames");

	for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
	{
		fprintf(file, ",%s", NativePerf_BucketName((enum NativePerfBucket)bucket));
	}

	fprintf(file, "\n");
}
#endif /* !__3DS__ */

internal enum NativePerfBucket NativePerf_FindDominantBucket(f64 *dominantMsOut)
{
	enum NativePerfBucket dominantBucket = NATIVE_PERF_BUCKET_GAME_LOGIC;
	f64 dominantMs = 0.0;

	for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
	{
		if (s_bucketInfo[bucket].dominantCandidate == 0)
		{
			continue;
		}

		if (s_frameBucketMs[bucket] > dominantMs)
		{
			dominantBucket = (enum NativePerfBucket)bucket;
			dominantMs = s_frameBucketMs[bucket];
		}
	}

	*dominantMsOut = dominantMs;
	return dominantBucket;
}

internal void NativePerf_InsertWorstFrame(const struct NativePerfWorstFrame *frame)
{
	s32 insertAt = s_worstFrameCount;

	if (s_worstFrameCount == NATIVE_PERF_TOP_FRAME_COUNT && frame->workMs <= s_worstFrames[s_worstFrameCount - 1].workMs)
	{
		return;
	}

	while ((insertAt > 0) && (frame->workMs > s_worstFrames[insertAt - 1].workMs))
	{
		if (insertAt < NATIVE_PERF_TOP_FRAME_COUNT)
		{
			s_worstFrames[insertAt] = s_worstFrames[insertAt - 1];
		}
		insertAt--;
	}

	if (insertAt < NATIVE_PERF_TOP_FRAME_COUNT)
	{
		s_worstFrames[insertAt] = *frame;
	}

	if (s_worstFrameCount < NATIVE_PERF_TOP_FRAME_COUNT)
	{
		s_worstFrameCount++;
	}
}

#if defined(__3DS__)
/*
 * Integer microseconds throughout, never %f: devkitARM's newlib only links
 * float formatting when explicitly asked to, and a silently-blank %f would
 * cost a whole build-and-run cycle to notice. %d always works.
 */
internal void NativePerf_Report3DSWindow(void)
{
	char header[224];
	char acc[192];
	char out[256];
	s32 taken[NATIVE_PERF_BUCKET_COUNT];
	s32 offset = 0;
	s32 emitted = 0;
	f64 toAvgUs;

	if (s_windowFrames == 0)
	{
		return;
	}

	// Render-thread builds: draw_otag/begin_scene/end_scene accumulate on the
	// worker. Drain it before reading, or a mid-write u64 on the other core
	// can print a garbage spike -- this project has chased enough phantom
	// numbers. Costs at most one walk, once per 64-frame window; no-op when
	// the queue is inactive.
	NativeRenderQueue_Fence();

#if defined(__3DS__) && CTR_NATIVE_RENDER_THREAD
	/* Worker is now idle (fenced): fold its window totals into the shared array
	 * the report reads, then zero them for the next window. Safe here and only
	 * here -- the worker cannot be mid-write. */
	for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
	{
		s_windowBucketMs[bucket] += s_workerWindowMs[bucket];
	}
	memset(s_workerWindowMs, 0, sizeof(s_workerWindowMs));
#endif

	toAvgUs = 1000.0 / (f64)s_windowFrames;

	// spuMix is stamped into every window on purpose: these numbers mean
	// something very different depending on whether real mixing + ndsp output
	// are live ("ndsp") or the sink failed to come up and the build is
	// effectively muted ("mute") -- a perf figure recorded without that
	// context has already misled this project once.
	s_perfConsoleTotalUs = (int)(s_windowTotalMs * toAvgUs);
	/* The window's WORST single frame, for the bottom screen. With SD logging
	 * off (g_dbg_sdLog, the default) the console is the only readout, and an
	 * average cannot show a hitch. Not scaled by toAvgUs: it is one frame, not
	 * a per-frame mean. */
	s_perfConsoleWorstUs = (int)(s_windowMaxTotalMs * 1000.0);
	s_perfConsoleWorkUs = (int)(s_windowWorkMs * toAvgUs);

	/*
	 * lvl/mode identify WHAT was on screen for this window -- without them a log
	 * cannot distinguish a menu/attract window from a race window when comparing
	 * performance numbers.
	 */
	snprintf(header, sizeof(header), "[PERF] cfg=%s frames=%u avgTotalUs=%d avgWorkUs=%d spuMix=%s lvl=%d mode=%d load=%d audioBacklog=%d audioDropped=%d",
	         s_windowCfg ? s_windowCfg : "none", s_windowFrames, s_perfConsoleTotalUs,
	         s_perfConsoleWorkUs, N3DS_NdspIsUp() ? "ndsp" : "mute", s_frameBeginInfo.levelID, s_frameBeginInfo.gameMode1,
	         s_frameBeginInfo.loadingStage, N3DS_AudioThreadGetBacklog(), N3DS_AudioThreadGetDropped());
	N3DS_DebugLog(header);

	memset(taken, 0, sizeof(taken));
	acc[0] = '\0';

	for (s32 rank = 0; rank < NATIVE_PERF_3DS_TOP_BUCKETS; rank++)
	{
		s32 best = -1;
		f64 bestMs = 0.0;
		int written;

		for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
		{
			if ((taken[bucket] == 0) && (s_windowBucketMs[bucket] > bestMs))
			{
				best = bucket;
				bestMs = s_windowBucketMs[bucket];
			}
		}

		if (best < 0)
		{
			break;
		}

		taken[best] = 1;
		if (rank < NATIVE_PERF_3DS_CONSOLE_BUCKETS)
		{
			s_perfConsoleName[rank] = NativePerf_BucketName((enum NativePerfBucket)best);
			s_perfConsoleUs[rank] = (int)(bestMs * toAvgUs);
		}
		written = snprintf(acc + offset, sizeof(acc) - (size_t)offset, " %s=%d", NativePerf_BucketName((enum NativePerfBucket)best),
		                   (int)(bestMs * toAvgUs));
		if ((written > 0) && ((size_t)(offset + written) < sizeof(acc)))
		{
			offset += written;
		}
		emitted++;

		// Three buckets per line: every N3DS_DebugLog call is an fflush to the
		// SD card, so the report costs less the fewer lines it takes.
		if (emitted == 3)
		{
			snprintf(out, sizeof(out), "[PERF] avgUs%s", acc);
			N3DS_DebugLog(out);
			offset = 0;
			acc[0] = '\0';
			emitted = 0;
		}
	}

	if (emitted > 0)
	{
		snprintf(out, sizeof(out), "[PERF] avgUs%s", acc);
		N3DS_DebugLog(out);
	}

	/* CPU-vs-GPU split (see N3DS_C3DGpuTimeWindow). cpuUs ~ the worker's
	 * whole frame (FrameBegin..FrameEnd), gpuUs = PICA hardware execution --
	 * the number no CPU bucket can see. gpuUs approaching avgWorkUs means the
	 * frame is GPU-bound and walk-side optimization cannot help. */
	{
		int gtCpuUs;
		int gtGpuUs;
		unsigned gtFrames;

		N3DS_C3DGpuTimeWindow(&gtCpuUs, &gtGpuUs, &gtFrames);
		snprintf(out, sizeof(out), "[GPUTIME] cpuUs=%d gpuUs=%d frames=%u", gtCpuUs, gtGpuUs, gtFrames);
		N3DS_DebugLog(out);
	}

	/* Only when the effect was live: quiet windows would otherwise pay an SD
	 * log line each to report zeros, and the absence of the line IS the null
	 * reading. avgUs is torch_main_ms (the CPU emit walk only); the effect's
	 * downstream cost stays in the parse/begin_scene buckets above -- this
	 * line exists so those spikes can be correlated with emitted here. */
	if (s_torchWindowListed > 0)
	{
		snprintf(out, sizeof(out), "[TORCH] listed=%u emitted=%u maxFrameEmitted=%d avgUs=%d", (unsigned)s_torchWindowListed, (unsigned)s_torchWindowEmitted,
		         (int)s_torchWindowMaxFrameEmitted, (int)(s_windowBucketMs[NATIVE_PERF_BUCKET_TORCH_MAIN] * toAvgUs));
		N3DS_DebugLog(out);
	}
	s_torchWindowListed = 0;
	s_torchWindowEmitted = 0;
	s_torchWindowMaxFrameEmitted = 0;

	/* Trailing by one window by construction: these are the flushes that
	 * happened DURING the window just reported, and this line is itself
	 * logged before the flush it may trigger. Compare worstUs against the
	 * window's own worst frame (the [PERF-VOICE] maxFrameUs field) -- if they
	 * track each other, the hitch is the instrument, not the game. */
	{
		unsigned lfWorst = 0;
		unsigned lfTotal = 0;
		unsigned lfCount = 0;

		N3DS_LogFlushStats(&lfWorst, &lfTotal, &lfCount);
		snprintf(out, sizeof(out), "[LOGFLUSH] worstUs=%u totalUs=%u n=%u", lfWorst, lfTotal, lfCount);
		N3DS_DebugLog(out);
	}

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
	/* Separate line, deliberately not appended to `header` -- its 224-byte
	 * buffer is already nearly full. Counters live in native_audio.c and are
	 * window-reset inside the format call. */
	NativeAudio_DebugFormatVoiceProfWindow(out, (int)sizeof(out), (int)(s_windowMaxTotalMs * 1000.0), s_windowMaxFrameKeyOns);
	N3DS_DebugLog(out);
	s_windowMaxFrameKeyOns = 0;
#endif

	/* Reset unconditionally, outside the profile gate above: the bottom
	 * screen reads this every window now, so it must not depend on whether
	 * the audio profiler happens to be compiled in. */
	s_windowMaxTotalMs = 0.0;

#if CTR_NATIVE_DISC_IO_PROFILE
	/* One line per disc-reading thread rather than one combined line: the
	 * whole question this counter exists to answer is how the two divide the
	 * SD card between them, and `out` is 256 bytes. */
	NativeDiscImage_DebugFormatIoProfWindow(out, (int)sizeof(out), NATIVE_DISC_IMAGE_OWNER_MAIN);
	N3DS_DebugLog(out);
	NativeDiscImage_DebugFormatIoProfWindow(out, (int)sizeof(out), NATIVE_DISC_IMAGE_OWNER_AUDIO);
	N3DS_DebugLog(out);
#endif

	s_windowFrames = 0;
	s_windowTotalMs = 0.0;
	s_windowWorkMs = 0.0;
	memset(s_windowBucketMs, 0, sizeof(s_windowBucketMs));
	{
		extern const char *g_dbg_ablateConfig;

		s_windowCfg = g_dbg_ablateConfig;
	}
}
#endif

#if !defined(__3DS__)
internal void NativePerf_WriteSummary(FILE *file)
{
	f64 averageMs = (s_frameIndex == 0) ? 0.0 : s_totalFrameMs / (f64)s_frameIndex;
	f64 averageWorkMs = (s_frameIndex == 0) ? 0.0 : s_totalWorkMs / (f64)s_frameIndex;

	fprintf(file, "frames=%u\n", s_frameIndex);
	fprintf(file, "average_ms=%.3f\n", averageMs);
	fprintf(file, "average_work_ms=%.3f\n", averageWorkMs);
	fprintf(file, "max_ms=%.3f\n", s_maxFrameMs);
	fprintf(file, "max_work_ms=%.3f\n", s_maxWorkMs);
	fprintf(file, "frames_over_33_33ms=%u\n", s_framesOverBudget);
	fprintf(file, "work_frames_over_33_33ms=%u\n", s_workFramesOverBudget);
	fprintf(file, "frames_over_40ms=%u\n", s_framesOver40);
	fprintf(file, "frames_over_50ms=%u\n", s_framesOver50);
	fprintf(file, "csv_path=%s\n", s_csvPath);

#if CTR_NATIVE_DISC_IO_PROFILE
	/* Same counters the 3DS prints per window, summarised once at exit. The
	 * us fields read 0 here (the tick source is 3DS-only), but perCall does
	 * not -- and perCall is what says whether the disc reader is batching, so
	 * it can be checked on PC without spending a netload. */
	{
		char discIoLine[256];

		fprintf(file, "\ndisc_io:\n");
		NativeDiscImage_DebugFormatIoProfWindow(discIoLine, (int)sizeof(discIoLine), NATIVE_DISC_IMAGE_OWNER_MAIN);
		fprintf(file, "  %s\n", discIoLine);
		NativeDiscImage_DebugFormatIoProfWindow(discIoLine, (int)sizeof(discIoLine), NATIVE_DISC_IMAGE_OWNER_AUDIO);
		fprintf(file, "  %s\n", discIoLine);
	}
#endif

	fprintf(file, "\nbucket_totals_ms:\n");
	for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
	{
		fprintf(file, "  %s: %.3f\n", NativePerf_BucketName((enum NativePerfBucket)bucket), s_bucketTotals[bucket]);
	}

	fprintf(file, "\ntop_worst_work_frames:\n");
	fprintf(file, "frame,total_ms,work_ms,over_budget_ms,work_over_budget_ms,dominant_bucket,dominant_ms,frameCounter,timer,level_id,game_mode1,loading_stage,"
	              "bool_demo_mode,num_plyr,elapsed_time_ms,vsync_till_flip,vSync_between_drawSync,frameTimer_VsyncCallback,audio_underrun_delta,"
	              "audio_overflow_delta,audio_queued_frames\n");

	for (s32 i = 0; i < s_worstFrameCount; i++)
	{
		const struct NativePerfWorstFrame *frame = &s_worstFrames[i];

		fprintf(file, "%u,%.3f,%.3f,%.3f,%.3f,%s,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", frame->frameIndex, frame->totalMs, frame->workMs,
		        frame->overBudgetMs, frame->workOverBudgetMs, NativePerf_BucketName(frame->dominantBucket), frame->dominantMs, frame->info.frameCounter,
		        frame->info.timer, frame->info.levelID, frame->info.gameMode1, frame->info.loadingStage, frame->info.boolDemoMode, frame->info.numPlyrCurrGame,
		        frame->info.elapsedTimeMS, frame->info.vsyncTillFlip, frame->info.vSync_between_drawSync, frame->info.frameTimer_VsyncCallback,
		        frame->audioUnderrunDelta, frame->audioOverflowDelta, frame->audioQueuedFrames);
	}
}

internal const char *NativePerf_ArgValue(int argc, char **argv, const char *name)
{
	for (s32 i = 1; i < argc - 1; i++)
	{
		if (strcmp(argv[i], name) == 0)
		{
			return argv[i + 1];
		}
	}

	return NULL;
}

internal s32 NativePerf_ArgPresent(int argc, char **argv, const char *name)
{
	for (s32 i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], name) == 0)
		{
			return 1;
		}
	}

	return 0;
}
#endif /* !__3DS__ */

int NativePerf_ConfigureFromArgs(int argc, char **argv)
{
#if defined(__3DS__)
	// A .3dsx launched from the homebrew menu gets no arguments, so the --perf
	// opt-in below can never fire here. Self-enable instead, and leave
	// s_csvFile NULL so the per-frame CSV row is skipped -- output goes to the
	// log via NativePerf_Report3DSWindow.
	(void)argc;
	(void)argv;
	s_enabled = 1;
	N3DS_DebugLog("[PERF] frame profiler enabled (windowed report every 64 frames)");
	return 0;
#else
	const char *outputDir = NativePerf_ArgValue(argc, argv, "--perf-dir");
	const s32 perfEnabled = NativePerf_ArgPresent(argc, argv, "--perf") || (outputDir != NULL);

	if (!perfEnabled)
	{
		return 0;
	}

	if (outputDir == NULL)
	{
		outputDir = NATIVE_PERF_DEFAULT_DIR;
	}

	if (!NativePerf_SetOutputDir(outputDir))
	{
		fprintf(stderr, "[CTR Perf] invalid perf output path: %s\n", outputDir);
		return 1;
	}

	if (!NativePerf_CreateDirs(s_outputDir))
	{
		fprintf(stderr, "[CTR Perf] failed to create perf output directory: %s\n", s_outputDir);
		return 1;
	}

	s_csvFile = fopen(s_csvPath, "wb");
	if (s_csvFile == NULL)
	{
		fprintf(stderr, "[CTR Perf] failed to open frame CSV: %s\n", s_csvPath);
		return 1;
	}

	NativePerf_WriteCsvHeader(s_csvFile);
	fflush(s_csvFile);
	s_enabled = 1;

	Platform_Log("[CTR Perf] capturing frame times: %s\n", s_csvPath);
	return 0;
#endif
}

void NativePerf_Shutdown(void)
{
	FILE *summaryFile;

	if (!s_enabled)
	{
		return;
	}

	if (s_frameOpen)
	{
		NativePerf_EndFrame(&s_frameBeginInfo);
	}

#if defined(__3DS__)
	// No CSV and no summary file here -- flush whatever the last partial
	// window held and stop. In practice this rarely runs at all: the app is
	// usually killed rather than exited, which is why reporting is windowed.
	NativePerf_Report3DSWindow();
	s_enabled = 0;
	return;
#else
	if (s_csvFile != NULL)
	{
		fflush(s_csvFile);
		fclose(s_csvFile);
		s_csvFile = NULL;
	}

	summaryFile = fopen(s_summaryPath, "wb");
	if (summaryFile != NULL)
	{
		NativePerf_WriteSummary(summaryFile);
		fclose(summaryFile);
	}
	else
	{
		Platform_Log("[CTR Perf] failed to write summary: %s\n", s_summaryPath);
	}

	Platform_Log("[CTR Perf] frames=%u avg=%.3fms avgWork=%.3fms max=%.3fms maxWork=%.3fms over33.33=%u workOver33.33=%u over40=%u over50=%u\n", s_frameIndex,
	             (s_frameIndex == 0) ? 0.0 : s_totalFrameMs / (f64)s_frameIndex, (s_frameIndex == 0) ? 0.0 : s_totalWorkMs / (f64)s_frameIndex, s_maxFrameMs,
	             s_maxWorkMs, s_framesOverBudget, s_workFramesOverBudget, s_framesOver40, s_framesOver50);
	Platform_Log("[CTR Perf] summary: %s\n", s_summaryPath);

	s_enabled = 0;
#endif
}

int NativePerf_CurrentLevelID(void)
{
	return (int)s_frameBeginInfo.levelID;
}

void NativePerf_BeginFrame(const struct NativePerfFrameInfo *info)
{
	/*
	 * levelID is latched BEFORE the enabled check: NativePerf_CurrentLevelID is
	 * the single source every instrument tags from, including instruments that
	 * run in builds with the frame profiler switched off. Latching behind the
	 * guard would silently read the wrong level in those builds.
	 */
	if (info != NULL)
	{
		s_frameBeginInfo.levelID = info->levelID;
	}

	if (!s_enabled || (info == NULL))
	{
		return;
	}

	if (s_frameOpen)
	{
		NativePerf_EndFrame(info);
	}

	memset(s_frameBucketMs, 0, sizeof(s_frameBucketMs));
	memset(s_scopeDepth, 0, sizeof(s_scopeDepth));
	memset(s_scopeStartCounter, 0, sizeof(s_scopeStartCounter));
	s_torchFrameListed = 0;
	s_torchFrameEmitted = 0;

	s_frameBeginInfo = *info;
	NativeAudio_GetOutputStats(&s_beginUnderrunFrames, &s_beginOverflowFrames, &s_beginQueuedFrames);
	s_frameStartCounter = SDL_GetPerformanceCounter();
	s_frameOpen = 1;

	if (s_perfLastFrameEndCounter != 0)
	{
		const f64 gapMs = NativePerf_CounterToMs(s_frameStartCounter - s_perfLastFrameEndCounter);

		if (gapMs > 100.0)
		{
			Platform_LogInstrument("[OUTFRAME] gap=%.1fms lvl=%ld stage=%ld f=%ld\n", (double)gapMs, (long)info->levelID, (long)info->loadingStage,
			                       (long)info->frameCounter);
		}
	}
}

#if defined(__3DS__)
internal void NativePerf_SpikeFlush(void)
{
	FILE *f;

	if (s_spikeCount == 0)
	{
		return;
	}
	/* Own file, own single open/write/close -- NOT the per-frame sdlog path. */
	f = fopen("sdmc:/ctr_spikes.log", "a");
	if (f != NULL)
	{
		u32 i;

		for (i = 0; i < s_spikeCount; i++)
		{
			const struct NativePerfSpike *s = &s_spikeRing[i];

			fprintf(f, "[SPIKE] f=%lu total=%.1f work=%.1f dom=%s domMs=%.1f raw1=%s/%.1f raw2=%s/%.1f walk(p/pa/sp)=%.1f/%.1f/%.1f lvl=%ld mode=%08lx\n",
			        (unsigned long)s->frameCounter, (double)s->totalMs, (double)s->workMs, NativePerf_BucketName(s->dominantBucket), (double)s->dominantMs,
			        NativePerf_BucketName(s->rawTop1), (double)s->rawTop1Ms, NativePerf_BucketName(s->rawTop2), (double)s->rawTop2Ms, (double)s->walkPatchMs,
			        (double)s->walkParseMs, (double)s->walkSplitsMs, (long)s->levelID, (unsigned long)s->gameMode1);
		}
		fclose(f);
	}
	s_spikeCount = 0;
	/* The fclose SD write stalls the frame right after this; do not let that
	 * self-record as a spike. */
	s_spikePostFlushSkip = 3;
}

internal void NativePerf_SpikeRecord(f64 totalMs, f64 workMs, enum NativePerfBucket dom, f64 domMs, const struct NativePerfFrameInfo *ri)
{
	struct NativePerfSpike *s;

	/* Delta the worker walk's cumulative ticks to this frame -- every frame, so
	 * a recorded spike carries the split for the walk it actually waited on. */
	{
		extern void NativeOtag_PeekWalkTicks(u64 * patch, u64 * parse, u64 * splits);
		u64 pc = 0;
		u64 pa = 0;
		u64 sp = 0;

		NativeOtag_PeekWalkTicks(&pc, &pa, &sp);
		s_spikeWalkPatchMs = (f32)NativePerf_CounterToMs(pc - s_spikeLastPatchTicks);
		s_spikeWalkParseMs = (f32)NativePerf_CounterToMs(pa - s_spikeLastParseTicks);
		s_spikeWalkSplitsMs = (f32)NativePerf_CounterToMs(sp - s_spikeLastSplitsTicks);
		s_spikeLastPatchTicks = pc;
		s_spikeLastParseTicks = pa;
		s_spikeLastSplitsTicks = sp;
	}

	if (!g_dbg_spikeCatch)
	{
		return;
	}
	/* Level change (race<->menu/load) is a gameplay-free flush point. */
	if (!s_spikeLevelValid || (ri->levelID != s_spikeLastLevel))
	{
		NativePerf_SpikeFlush();
		s_spikeLastLevel = ri->levelID;
		s_spikeLevelValid = 1;
	}
	if (s_spikePostFlushSkip > 0)
	{
		s_spikePostFlushSkip--;
		return;
	}
	if (totalMs <= NATIVE_PERF_FRAME_WARN_40_MS)
	{
		return;
	}
	if (s_spikeCount >= NATIVE_PERF_SPIKE_RING)
	{
		NativePerf_SpikeFlush(); /* backstop: a race this spiky is its own answer */
		return;
	}
	s = &s_spikeRing[s_spikeCount++];
	s->frameCounter = (u32)ri->frameCounter;
	s->totalMs = (f32)totalMs;
	s->workMs = (f32)workMs;
	s->dominantBucket = dom;
	s->dominantMs = (f32)domMs;
	s->levelID = ri->levelID;
	s->gameMode1 = (u32)ri->gameMode1;

	/* Two biggest buckets by raw time, aggregates included -- see the struct. */
	{
		enum NativePerfBucket t1 = NATIVE_PERF_BUCKET_GAME_LOGIC;
		enum NativePerfBucket t2 = NATIVE_PERF_BUCKET_GAME_LOGIC;
		f64 t1ms = -1.0;
		f64 t2ms = -1.0;
		s32 b;

		for (b = 0; b < NATIVE_PERF_BUCKET_COUNT; b++)
		{
			const f64 v = s_frameBucketMs[b];

			if (v > t1ms)
			{
				t2ms = t1ms;
				t2 = t1;
				t1ms = v;
				t1 = (enum NativePerfBucket)b;
			}
			else if (v > t2ms)
			{
				t2ms = v;
				t2 = (enum NativePerfBucket)b;
			}
		}
		s->rawTop1 = t1;
		s->rawTop1Ms = (f32)t1ms;
		s->rawTop2 = t2;
		s->rawTop2Ms = (f32)t2ms;
	}
	s->walkPatchMs = s_spikeWalkPatchMs;
	s->walkParseMs = s_spikeWalkParseMs;
	s->walkSplitsMs = s_spikeWalkSplitsMs;
	s_spikeSeenTotal++;
}
#endif

void NativePerf_DumpSpikesNow(void)
{
#if defined(__3DS__)
	NativePerf_SpikeFlush();
#endif
}

double NativePerf_WallMs(void)
{
	return NativePerf_CounterToMs(SDL_GetPerformanceCounter());
}

void NativePerf_EndFrame(const struct NativePerfFrameInfo *info)
{
	f64 totalMs;
	f64 workMs;
	f64 overBudgetMs;
	f64 workOverBudgetMs;
	f64 dominantMs;
	enum NativePerfBucket dominantBucket;
	int endUnderrunFrames = 0;
	int endOverflowFrames = 0;
	int endQueuedFrames = 0;
	struct NativePerfWorstFrame worstFrame;
	const struct NativePerfFrameInfo *rowInfo;

	if (!s_enabled || !s_frameOpen)
	{
		return;
	}

	totalMs = NativePerf_CounterToMs(SDL_GetPerformanceCounter() - s_frameStartCounter);
	workMs = totalMs - s_frameBucketMs[NATIVE_PERF_BUCKET_VSYNC_WAIT];
	if (workMs < 0.0)
	{
		workMs = 0.0;
	}

	overBudgetMs = (totalMs > NATIVE_PERF_FRAME_BUDGET_MS) ? (totalMs - NATIVE_PERF_FRAME_BUDGET_MS) : 0.0;
	workOverBudgetMs = (workMs > NATIVE_PERF_FRAME_BUDGET_MS) ? (workMs - NATIVE_PERF_FRAME_BUDGET_MS) : 0.0;
	dominantBucket = NativePerf_FindDominantBucket(&dominantMs);

	NativeAudio_GetOutputStats(&endUnderrunFrames, &endOverflowFrames, &endQueuedFrames);

	rowInfo = (info != NULL) ? info : &s_frameBeginInfo;

#if defined(__3DS__)
	/* Catch dropped frames with their dominant bucket, out-of-band from the
	 * per-frame sdlog path (see the spike-catcher block above). */
	NativePerf_SpikeRecord(totalMs, workMs, dominantBucket, dominantMs, rowInfo);
#endif

	// s_csvFile is NULL on 3DS, which reports through the log instead; the
	// bucket accounting below still has to run either way.
	if (s_csvFile != NULL)
	{
		fprintf(s_csvFile, "%u,%.3f,%.3f,%.3f,%.3f,%s,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", s_frameIndex, totalMs, workMs, overBudgetMs,
		        workOverBudgetMs, NativePerf_BucketName(dominantBucket), dominantMs, rowInfo->frameCounter, rowInfo->timer, rowInfo->levelID,
		        rowInfo->gameMode1, rowInfo->loadingStage, rowInfo->boolDemoMode, rowInfo->numPlyrCurrGame, rowInfo->elapsedTimeMS, rowInfo->vsyncTillFlip,
		        rowInfo->vSync_between_drawSync, rowInfo->frameTimer_VsyncCallback, endUnderrunFrames - s_beginUnderrunFrames,
		        endOverflowFrames - s_beginOverflowFrames, endQueuedFrames);
	}

	for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
	{
		if (s_csvFile != NULL)
		{
			fprintf(s_csvFile, ",%.3f", s_frameBucketMs[bucket]);
		}
		s_bucketTotals[bucket] += s_frameBucketMs[bucket];
	}

	if (s_csvFile != NULL)
	{
		fprintf(s_csvFile, "\n");

		s_rowsSinceFlush++;
		if (s_rowsSinceFlush >= NATIVE_PERF_FLUSH_INTERVAL)
		{
			fflush(s_csvFile);
			s_rowsSinceFlush = 0;
		}
	}

#if defined(__3DS__)
	/*
	 * Ablation-config boundary (see include/platform/native_gpu.h). The sweep
	 * holds a config for two 32-frame stats windows while this report is on a
	 * 64-frame cycle of its own, so without the cut a [PERF] window routinely
	 * straddled two configs and its per-bucket microseconds belonged to
	 * neither -- which is precisely the attribution the sweep exists to make.
	 * Reported BEFORE this frame is folded in: the config changed at the end
	 * of the previous frame, so this one already ran under the new one.
	 */
	{
		extern const char *g_dbg_ablateConfig;

		if ((s_windowCfg != g_dbg_ablateConfig) && (s_windowFrames > 0))
		{
			NativePerf_Report3DSWindow();
		}
		if (s_windowFrames == 0)
		{
			s_windowCfg = g_dbg_ablateConfig;
		}
	}

	s_windowFrames++;
	s_windowTotalMs += totalMs;
	s_windowWorkMs += workMs;
	for (s32 bucket = 0; bucket < NATIVE_PERF_BUCKET_COUNT; bucket++)
	{
		s_windowBucketMs[bucket] += s_frameBucketMs[bucket];
	}

	s_torchWindowListed += (u32)s_torchFrameListed;
	s_torchWindowEmitted += (u32)s_torchFrameEmitted;
	if (s_torchFrameEmitted > s_torchWindowMaxFrameEmitted)
	{
		s_torchWindowMaxFrameEmitted = s_torchFrameEmitted;
	}

	{
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
		/* Consumed (and so reset) EVERY frame, not just on a new max --
		 * otherwise key-ons accumulate across frames and the correlation
		 * reads spuriously high. */
		int frameKeyOns = NativeAudio_DebugConsumeFrameKeyOns();
#endif

		/* The max itself is tracked in every config -- the bottom screen
		 * reports it whether or not the audio profiler is compiled in, and a
		 * worst-frame readout pinned at zero would read as "no hitches". */
		if (totalMs > s_windowMaxTotalMs)
		{
			s_windowMaxTotalMs = totalMs;
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
			s_windowMaxFrameKeyOns = frameKeyOns;
#endif
		}
	}

	if (s_windowFrames >= NATIVE_PERF_3DS_REPORT_FRAMES)
	{
		NativePerf_Report3DSWindow();
	}
#endif

	s_totalFrameMs += totalMs;
	s_totalWorkMs += workMs;
	if (totalMs > s_maxFrameMs)
	{
		s_maxFrameMs = totalMs;
	}
	if (workMs > s_maxWorkMs)
	{
		s_maxWorkMs = workMs;
	}
	if (totalMs > NATIVE_PERF_FRAME_BUDGET_MS)
	{
		s_framesOverBudget++;
	}
	if (workMs > NATIVE_PERF_FRAME_BUDGET_MS)
	{
		s_workFramesOverBudget++;
	}
	if (totalMs > NATIVE_PERF_FRAME_WARN_40_MS)
	{
		s_framesOver40++;
	}
	if (totalMs > NATIVE_PERF_FRAME_WARN_50_MS)
	{
		s_framesOver50++;
	}

	worstFrame.frameIndex = s_frameIndex;
	worstFrame.totalMs = totalMs;
	worstFrame.workMs = workMs;
	worstFrame.overBudgetMs = overBudgetMs;
	worstFrame.workOverBudgetMs = workOverBudgetMs;
	worstFrame.dominantBucket = dominantBucket;
	worstFrame.dominantMs = dominantMs;
	worstFrame.info = *rowInfo;
	worstFrame.audioUnderrunDelta = endUnderrunFrames - s_beginUnderrunFrames;
	worstFrame.audioOverflowDelta = endOverflowFrames - s_beginOverflowFrames;
	worstFrame.audioQueuedFrames = endQueuedFrames;
	NativePerf_InsertWorstFrame(&worstFrame);

	s_frameIndex++;
	s_frameOpen = 0;
	s_perfLastFrameEndCounter = SDL_GetPerformanceCounter();
}

void NativePerf_BeginScope(enum NativePerfBucket bucket)
{
	if (!s_enabled || (bucket < 0) || (bucket >= NATIVE_PERF_BUCKET_COUNT))
	{
		return;
	}

#if defined(__3DS__) && CTR_NATIVE_RENDER_THREAD
	/* Worker thread: its own accumulators, not gated on the main thread's
	 * per-frame window. See s_workerWindowMs. */
	if (N3DS_RenderWorkerIsSelf())
	{
		if (s_workerScopeDepth[bucket]++ == 0)
		{
			s_workerScopeStart[bucket] = SDL_GetPerformanceCounter();
		}
		return;
	}
#endif

	if (!s_frameOpen)
	{
		return;
	}

	if (s_scopeDepth[bucket]++ == 0)
	{
		s_scopeStartCounter[bucket] = SDL_GetPerformanceCounter();
	}
}

void NativePerf_EndScope(enum NativePerfBucket bucket)
{
	if (!s_enabled || (bucket < 0) || (bucket >= NATIVE_PERF_BUCKET_COUNT))
	{
		return;
	}

#if defined(__3DS__) && CTR_NATIVE_RENDER_THREAD
	if (N3DS_RenderWorkerIsSelf())
	{
		if (s_workerScopeDepth[bucket] <= 0)
		{
			return;
		}
		if (--s_workerScopeDepth[bucket] == 0)
		{
			s_workerWindowMs[bucket] += NativePerf_CounterToMs(SDL_GetPerformanceCounter() - s_workerScopeStart[bucket]);
		}
		return;
	}
#endif

	if (!s_frameOpen)
	{
		return;
	}

	if (s_scopeDepth[bucket] <= 0)
	{
		return;
	}

	s_scopeDepth[bucket]--;
	if (s_scopeDepth[bucket] == 0)
	{
		s_frameBucketMs[bucket] += NativePerf_CounterToMs(SDL_GetPerformanceCounter() - s_scopeStartCounter[bucket]);
	}
}

void NativePerf_CountTorch(int listed, int emitted)
{
	if (!s_enabled || !s_frameOpen)
	{
		return;
	}

	s_torchFrameListed += listed;
	s_torchFrameEmitted += emitted;
}
#else
/*
 * The only piece of the profiler that outlives the debug-surface gate: the
 * level id instruments stamp their lines with. NativePerf_BeginFrame (the
 * inline stub in native_perf.h) is the writer.
 */
int g_nativePerfLevelID = 0;
#endif
