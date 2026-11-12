/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/gpu/PsyX_GPU.cpp
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#include <macros.h>
#include "platform/native_gpu.h"

#include <platform.h>

#if !defined(__3DS__)
#include <SDL3/SDL.h>
#endif

#include "platform/native_bake_parity.h"
#include "platform/native_gte_capture.h"
#include "platform/native_log.h"
#include "platform/native_perf.h"
#include "platform/native_renderer.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Platform_PollHostEvents(void);
extern int g_cfg_bilinearFiltering;
extern int g_dbg_emulatorPaused;
extern int g_dbg_polygonSelected;

#define NATIVE_GPU_LOG(fmt, ...)   Platform_Log("[CTR GPU] " fmt, ##__VA_ARGS__)
#define NATIVE_GPU_ERROR(fmt, ...) Platform_LogError("[CTR GPU] [%s] - " fmt, __func__, ##__VA_ARGS__)

#ifndef CTR_NATIVE_DIRECT_EMIT
#define CTR_NATIVE_DIRECT_EMIT 0
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT
// Defined in platform/native_direct_emit.c, included later in this TU (it
// needs this file's and the 3DS renderer's file-scope state in view).
internal int NativeDirectEmit_TryNode(const u8 *packet, u32 tagLength);
internal void DirectEmit_FoldFrame(void);
#endif

// NOTE(aalhendi): Little-endian tag `CTRG` = CTR native GPU snapshot.
#define NATIVE_GPU_STATE_MAGIC     0x47525443
#define NATIVE_GPU_STATE_VERSION   1

#define GET_TPAGE_BLEND(tpage)     ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage)    ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)           ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)           (clut >> 6)

internal TexFormat GetTPageFormat(int tpage)
{
	const int mode = (tpage >> 7) & 0x3;

	// NOTE(aalhendi): ctr-native local divergence from upstream PsyCross. PS1
	// mode 3 is reserved; TF_32_BIT_RGBA is only for explicit native override
	// textures, not raw retail TPAGE mode bits.
	return mode == 3 ? TF_16_BIT : (TexFormat)mode;
}

internal short GetTPageBase(int tpage)
{
	const u16 page = (u16)tpage;

	// NOTE(aalhendi): ctr-native local divergence for CTR retail emitters. The
	// shader wants xPage + yPage * 16, not raw draw-mode bits.
	return (s16)((page & 0xf) | ((page & 0x10) ? 0x10 : 0));
}

internal s16 NativeGpu_SignExtend11(u32 value)
{
	value &= 0x7ff;
	return (s16)((value ^ 0x400) - 0x400);
}

DISPENV activeDispEnv;
DRAWENV activeDrawEnv;
int g_GPUDisabledState = 0;

typedef struct
{
	DRAWENV drawenv;
	DISPENV dispenv;

	BlendMode blendMode;

	TexFormat texFormat;
	TextureID textureId;

	int drawPrimMode;
	bool psxTexturedSemiTrans;
	bool psxTextureOutputSTP;
	bool psxDrawMaskSet;

	u16 startVertex;
	u16 numVerts;

	const char *debugText;

	/* This split is driver-local exhaust smoke (Particle.c tagged its packet
	 * with code bit 0x01). Forces a dedicated split via the AddSplit
	 * coalescing check and makes DrawAllSplits hold it out of the normal pass
	 * for a redraw after the native model pass. Always 0 off 3DS. */
	bool deferredSmoke;
} GPUDrawSplit;

#define MAX_DRAW_SPLITS 4096

/* v2 bucket->ordinal table capacity: 4P swapchain OTs run (numPlyr<<10)|6 =
 * up to 4102 slots (MainFrame.c); headroom on top, and slots past the
 * window are simply not recorded. */
#define OTZ_TABLE_SLOTS 4224

/* Sentinel for otBucketOrdinal entries: 0 is ambiguous with a legitimately-
 * recorded ordinal-0 crossing at the very start of a walk, so 0xFFFF marks
 * "never crossed." Not airtight in principle (65535+ prims stamped in one
 * walk would collide) but unreachable given measured content (~1000-1200
 * prims/frame on the heaviest tracks). */
#define NATIVE_GPU_OTZ_SENTINEL 0xFFFFu

/* Sentinel for otCurrentSlotIdx / minContentBucket (both u32): "no bucket
 * recorded yet". Same idea as NATIVE_GPU_OTZ_SENTINEL just above (0 is a
 * real bucket index -- the walk's very last crossing is bucket 0 -- so it
 * cannot double as "never set"), kept as its own constant because these two
 * fields are a different width/scale than the u16 ordinal table entries. */
#define NATIVE_GPU_NO_CONTENT_BUCKET 0xFFFFFFFFu

typedef struct
{
	const char *currentSplitDebugText;
	TextureID overrideTexture;
	int overrideTextureWidth;
	int overrideTextureHeight;

	int drawPrimMode;
	bool psxDrawMaskSet;
	bool framebufferFeedbackRunActive;

	GrVertex vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
	GPUDrawSplit splits[MAX_DRAW_SPLITS];
	int vertexIndex;
	int splitIndex;

	/*
	 * Next GrVertex.emitOrdinal value. Frame-scoped (reset in ClearSplits,
	 * NOT by mid-frame DrawAllSplits flushes -- depth must stay monotonic
	 * across the whole presented frame). Not part of NativeGpuSnapshot:
	 * it is re-derived every frame, so save states don't carry it.
	 */
	u32 emitOrdinal;

	/*
	 * Ordinal widening (docs/
	 * NATIVE_TRACK_RENDERER_DESIGN.md 12): when g_dbg_otzWiden is on, the
	 * stamp becomes walkZBase + (otSlotsCrossed << OTZ_K | intraClamped)
	 * instead of a bare sequence number. Every zero-length node the OT walk
	 * traverses IS an OT slot, so counting crossings gives the bucket
	 * position relative to the walk head with no knowledge of the OT's base
	 * address. walkZBase carries across multiple DrawOTag passes in one
	 * depth-clear (the loading-screen lesson at NativeGpu_ResetEmitOrdinal):
	 * each new walk starts stamping just past the previous walk's maximum,
	 * so later passes still win the depth test. Both schemes stay monotone
	 * nondecreasing in submission order; the widened form additionally makes
	 * z a linear function of the OT bucket, which is the axis resident track
	 * draws will compute from view depth. Occupancy counters feed the [OTZ]
	 * line so OTZ_K is chosen from measurement, not guessed.
	 */
	u32 otSlotsCrossed;
	u32 lastStampedZ;

	/*
	 * v2 bucket->ordinal table.
	 * v1's widened STAMP was falsified by its own [OTZ] occupancy line
	 * (intraMax 81-168 vs a 6-bit cap; ~1500 "crossings" because len-0
	 * PACKETS are not OT slots; z saturating 65535 mid-walk) -- so v2
	 * leaves stamps untouched and only RECORDS: an OT slot is a walked
	 * node whose address sits in the aligned window descending from the
	 * walk head (the DrawOTag argument IS the head; ClearOTagR links slots
	 * in address-descending order), and the table remembers the first
	 * emit ordinal at each slot. Track draws later compose per leaf by
	 * table lookup on the CPU. The address window is what rejects the
	 * len-0 filler packets that polluted v1's crossing count -- they live
	 * in primMem, far from the OT array.
	 */
	uintptr_t otHead;
	u32 otSlotsDetected;
	u32 otSpuriousLen0;
	u32 otMaxSlotIdx;
	u16 otBucketOrdinal[OTZ_TABLE_SLOTS];

	/*
	 * Exact OT bounds, straight from the game's own ClearOTag/ClearOTagR
	 * calls (native_libgpu.c) -- the address-window heuristic failed
	 * because primMem sits contiguous with the OT arena ([OTZ2] measured
	 * spurious=0, maxSlot=4107 on a ~1030-slot OT). A small ring, deduped
	 * by base: the same OTs are re-cleared every frame. The walk resolves
	 * the range containing its head once per walk; slotIdx is then the
	 * plain array index (addr - base)/4 == the retail otIndex that
	 * GetProjectedOtIndex computes from depth -- exactly the axis the
	 * track compose needs.
	 */
	struct
	{
		uintptr_t base;
		u32 count;
	} otRanges[8];
	u32 otRangeCursor;
	uintptr_t walkOtBase;
	u32 walkOtCount;

	/*
	 * Link token of walkOtBase, valid while walkOtSkip is set: the walk's OT
	 * lies in one link range, so slot i's ClearOTagR word is
	 * walkOtToken + (i - 1) * 4 with length 0. See
	 * NativeGpu_SkipUntouchedOTSlots.
	 */
	u32 walkOtToken;
	int walkOtSkip;

	/*
	 * Whether the bucket->ordinal recorder is active for THIS walk, latched
	 * once at the top of the walk (matching the clear below) rather than
	 * re-evaluated per node: the walk can run on the worker thread while the
	 * game thread flips g_dbg_trackRender at any time, so re-reading live
	 * state at two points could clear with the flag off then record with it
	 * on, filling a table it never cleared.
	 */
	int otRecorderActive;

	/*
	 * otCurrentSlotIdx tracks which OT bucket the walk is currently passing
	 * through (set at each marker-crossing). minContentBucket is stamped from
	 * it only when real primitive content advances the emit ordinal, so it
	 * holds the lowest bucket that ever had real content -- not just the
	 * lowest bucket visited. Needed because an empty near bucket (e.g. bucket
	 * 0, under the camera) otherwise records whatever ordinal the emit
	 * counter reached by the end of the walk, which reads as nearer than
	 * everything in the frame. native_track_frame.c's depth-range compose
	 * clamps a leaf's bucket range to this floor before trusting
	 * otBucketOrdinal.
	 */
	u32 otCurrentSlotIdx;
	u32 minContentBucket;
} NativeGpuState;

int g_dbg_otzWiden = 0;

/*
 * Menu/loading flag STYLE (owner easter eggs):
 *   0 = retail grey checkers (default; the port's look stays close to retail)
 *   1 = six-colour pride stripes   -- cheat WONDER, or a June date check
 *   2 = purple checkers ("respect the dragon") -- cheat SOAR, retail's dead
 *       Spyro 2 demo code repurposed
 *   3 = blue-on-blue checkers -- cheat ARROW (Super Turbo Pads) while Retro-Fueled
 *       mode is on (include/native_retro_fueled.h)
 *
 * Read by RaceFlag_DrawSelf (game/RaceFlag.c); the pattern rules and colour
 * tables live there, this is only the selector.
 *
 * Lives on the platform side of the seam rather than in game/ because it is a
 * native-only presentation choice: the flag mesh, its wave and its lighting are
 * untouched retail behaviour in every style, and style 0 is byte-identical to
 * retail (its branch does no tinting arithmetic at all).
 *
 * A plain int, not a compile-time gate, precisely so the cheat handlers and a
 * startup date check can write to it.
 */
enum
{
	FLAG_STYLE_RETAIL = 0,
	FLAG_STYLE_PRIDE = 1,
	FLAG_STYLE_PURPLE = 2,
	FLAG_STYLE_BLUE = 3
};

int g_dbg_flagStyle = FLAG_STYLE_RETAIL;

/* Give Penta Penguin his PAL/Japan max-stats ("5/5/5") class instead of the
 * NTSC-U Turn class. ON by default -- owner's call, a deliberate divergence from
 * NTSC-U retail so Penta is always the PAL "God Penta". The `pentapal` dumpreq and
 * the future Options menu can turn it off. A plain runtime int, not a compile-time
 * gate. See include/native_penta.h. */
int g_pentaPal = 1;

/* Boost-reserves HUD meter (game/UI/UI_Meter.c, UI_DrawReservesMeter). ON by
 * default -- owner's call, matching the wider CTR mod scene where a reserves
 * meter ships enabled. The `reserves` dumpreq and the future Options menu can
 * turn it off. See include/native_reserves.h. */
int g_reservesMeter = 1;

/* Fix retail's mirrored-tire placement bug (game/DrawTires.c,
 * DrawTiresReflection_BuildWheelLocalPairs). Retail's DrawTires_Reflection
 * loses Instance.scale.x to a branch-delay-slot clobber and computes the
 * wheels' lateral offset from instFlags&0x100 instead -- 9 units, not the
 * ~144 the solid path uses -- so every reflected kart shows its wheel pairs
 * merged on the centerline (real PS1 does this too). ON by default -- owner's
 * call, a deliberate fix of shipped retail behaviour. The `tirefix` dumpreq
 * can restore the retail bug for A/B. */
int g_tireReflectFix = 1;

#ifdef CTR_INTERNAL
/* Invisible-tire hunt: dumpreq `tirepin` pins wheels to sprite 0. Remove with
 * the hunt. */
int g_tirehuntPin0 = 0;
#endif

/* See include/platform/native_gpu.h. Lives here, not in the 3DS renderer that
 * writes it, so the windowed instruments that READ it (native_perf.c, the
 * [OTPROF] block below) do not depend on a 3DS-only file being in the build. */
const char *g_dbg_ablateConfig = "none";

/*
 * The bucket->ordinal recorder above was built for the
 * standalone 'otz' instrument (g_dbg_otzWiden), but the track renderer's
 * depth compose (design 4.3) needs the exact same table -- so recording
 * must also run whenever the track renderer is actually active, not just
 * when the otz instrument is separately toggled on. g_dbg_trackRender is
 * defined in platform/native_track_frame.c, which is only compiled (and
 * only then does that symbol exist) when CTR_NATIVE_TRACK_RENDERER is on;
 * this file has no such gate of its own, so the extern -- and every use of
 * it below -- is wrapped in the same #if, collapsing to a plain 0 when the
 * track renderer isn't built at all. This keeps a PC build (or a 3DS build)
 * with the gate OFF exactly as before: no extern reference to a symbol that
 * doesn't exist, no behavior change.
 */
#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
extern int g_dbg_trackRender;
#define NATIVE_GPU_TRACK_RENDER_ACTIVE (g_dbg_trackRender != 0)
#else
#define NATIVE_GPU_TRACK_RENDER_ACTIVE 0
#endif

global_variable NativeGpuState s_gpu;

#ifdef CTR_NATIVE_OTWALK_PROFILE
/*
 * OT-walk content counters ([OTPROF], every 64 rendered frames). Decision
 * instrument for the render campaign: measures WHAT flows through the walk
 * (retainability, blend classes, state-key ceiling), not how fast -- timings
 * stay native_perf's job. Interpretation caveats the numbers depend on:
 * packet hashing runs inside the walk, so draw_otag_parse_ms is inflated in
 * profile builds -- take counts from a profile build, milliseconds from a
 * clean one; "reuse" compares packet payloads against the previous frame as
 * a multiset-blind set, so N identical packets matching one prior packet all
 * count as reused; a "frame" here is the span between ClearSplits calls that
 * saw content, which is one presented frame in the normal one-DrawOTag flow.
 */
/*
 * On 3DS these lines go to the SAME sink as [PERF] and [MERGEB] (ctr3ds.log,
 * via N3DS_DebugLog) rather than Platform_Log's separate "Crash Team
 * Racing.log". They are only readable together: an [OTPROF] window's content
 * counts are meaningless without the [PERF] window's microseconds beside them,
 * and hand-correlating two files with no shared timestamp is precisely how
 * this project has previously matched the wrong windows to each other.
 * N3DS_DebugLog appends its own newline, so the formats below carry none and
 * the PC path adds one.
 */
#if defined(__3DS__)
#define OTPROF_LOG(...)                                                                                          \
	do                                                                                                            \
	{                                                                                                             \
		char otprofLine[256];                                                                                     \
		snprintf(otprofLine, sizeof(otprofLine), __VA_ARGS__);                                                    \
		N3DS_DebugLog(otprofLine);                                                                                \
	} while (0)
#else
#define OTPROF_LOG(fmt, ...) Platform_Log(fmt "\n", ##__VA_ARGS__)
#endif

#define OTPROF_WINDOW_FRAMES 64
#define OTPROF_MAX_HASHES    16384
#define OTPROF_PREV_SET_SIZE 32768 /* power of two, ~2x OTPROF_MAX_HASHES */
#define OTPROF_KEY_SET_SIZE  512   /* power of two */

typedef struct
{
	/* per-frame, reset by OTProf_FoldFrame */
	u32 frameNodes;
	u32 frameDryNodes;
	u32 framePrims;
	u32 frameBlend[5]; /* indexed by BlendMode */
	u32 frameTextured;
	u32 frameDraws;
	u32 frameKeys;
	u32 frameKeyTransitions;
	u32 lastKey;
	u32 frameHashCount;
	u32 frameHashes[OTPROF_MAX_HASHES];
	u32 frameShapeHashCount;
	u32 frameShapeHashes[OTPROF_MAX_HASHES];
	u32 frameShapeSkip;
	u32 frameMHashCount;
	u32 frameMHashes[OTPROF_MAX_HASHES];
	u32 frameMNoModel;
	/* Matrix-class probe (full-form open Q1): among nodes that got an
	 * mshape hash, how many were projected under ONE matrix state, and
	 * which id. 64 = GTE_CAPTURE_MTX_IDS (native_gte_capture.c). The
	 * per-hash id array lets the fold count the intersection that actually
	 * sizes full-form v1: mshape-REUSED and single-matrix-dominant. 255 =
	 * mixed-matrix node. */
	u8 frameMHashMtxId[OTPROF_MAX_HASHES];
	u32 frameMSingleById[64];
	u32 frameMMulti;
	u32 keySet[OTPROF_KEY_SET_SIZE];

	/* previous frame's packet hashes, open-addressed (0 = empty) */
	u32 prevSet[OTPROF_PREV_SET_SIZE];
	u32 prevShapeSet[OTPROF_PREV_SET_SIZE];
	u32 prevMSet[OTPROF_PREV_SET_SIZE];
	/* fold N-2 packet hashes: a mid-OT DrawAllSplits barrier (FILL/MOVE/
	 * framebuffer-feedback) folds one game frame TWICE, so consecutive folds
	 * are the two disjoint halves of one frame and the N-1 comparison reads 0%
	 * by construction. The same-parity N-2 set recovers the true frame-over-
	 * frame reuse, which distinguishes a barrier-split artifact (N-2 high) from
	 * a genuine cache collapse (N-2 also low). Main packet set only -- that is
	 * the set the amnesia trigger reads. */
	u32 prevSet2[OTPROF_PREV_SET_SIZE];

	/* window accumulators */
	u32 winFrames;
	u32 winNodes;
	u32 winPrims;
	u32 winBlend[5];
	u32 winTextured;
	u32 winDraws;
	u32 winKeys;
	u32 winKeyTransitions;
	u32 winHashed;
	u32 winReused;
	u32 winShapeHashed;
	u32 winShapeReused;
	u32 winShapeSkip;
	u32 winMHashed;
	u32 winMReused;
	u32 winMNoModel;
	u32 winMSingle;
	u32 winMSingleDom;
	u32 winMMulti;
	u32 winMReusedDom;
	u32 winMtxDomShare; /* sum of per-frame dominant-store shares (avg = /frames) */
	u32 winMtxIds;      /* sum of per-frame distinct matrix counts */
	u32 winTruncated;
	u32 winDryMismatch;
	/* g_dbg_ablateConfig as it stood when this window opened; the window is
	 * cut short if it changes, so every reported window has exactly one. */
	const char *winCfg;
} OTWalkProfileState;

global_variable OTWalkProfileState s_otprof;

/*
 * Amnesia probe ([AMNESIA], profile builds): names the game event behind the
 * cache-collapse burst class -- multi-second spans where pkreuse drops to ~0
 * with content BELOW median while draw_all_splits multiplies. Watches the
 * per-fold reuse ratio; when it collapses off a sustained-healthy baseline,
 * logs the fold-level hash forensics (the full/shape/model reuse split
 * localizes WHICH bytes changed: position-only vs appearance vs mesh), the
 * dominant matrix run's actual R+TR words, and the game-thread context
 * mirror (state snapshot + recent event ring, pushed via NativeAmnesia_*).
 * The mirror is written on the game thread and read here on the walk thread
 * unsynchronized: a torn read is one frame loose at worst, which forensic
 * reading tolerates, and a lock could perturb the very timing being hunted.
 */
#define AMN_MIN_HASHED      100 /* below this the ratio is menu/empty noise */
#define AMN_HEALTHY_PCT     25
#define AMN_COLLAPSE_PCT    5
#define AMN_ARM_FOLDS       30 /* sustained health required to arm the trigger */
#define AMN_UNHEALTHY_BREAK 8  /* tolerated ramp-down before the streak resets */
#define AMN_DETAIL_FOLDS    6
#define AMN_RECOVER_FOLDS   2
#define AMN_FORCE_END_FOLDS 3600
#define AMN_MAX_EPISODES    16
#define AMN_EV_RING         16
#define AMN_EV_LOGGED       8

int g_dbg_amnesia = 1; /* dumpreq `amnesia` */

typedef struct
{
	u32 frame; /* s_amnesia.foldIdx at push time */
	u32 kind;  /* AMN_EV_* (native_amnesia.h); 0 = empty slot */
	s32 a;
	s32 b;
} AmnesiaEvent;

typedef struct
{
	/* walk-thread detector */
	u32 foldIdx;
	s32 detLevel; /* walk-side copy of gLevel; a change resets the episode budget */
	int detLevelValid;
	u32 healthyStreak;
	u32 unhealthyRun;
	u32 recoverRun;
	int episodeActive;
	u32 episodeStartFold;
	u32 episodeDetailLeft;
	u32 episodeCount;
	/* game-thread context mirror (unsynchronized; see block comment) */
	u32 evWriteIdx;
	AmnesiaEvent ev[AMN_EV_RING];
	s32 gLevel;
	u32 gMode1;
	u32 gMode2;
	s32 gClock;
	s32 gFade;
	s32 gCamX, gCamY, gCamZ;
	s32 gCamPX, gCamPY, gCamPZ;
	u32 gStateCalls; /* staleness tell: 0 = the game-side hook never ran */
} AmnesiaState;

global_variable AmnesiaState s_amnesia;

void NativeAmnesia_FrameState(s32 level, u32 mode1, u32 mode2, s32 clockFx, s32 fade, s32 camX, s32 camY, s32 camZ)
{
	AmnesiaState *a = &s_amnesia;

	a->gCamPX = a->gCamX;
	a->gCamPY = a->gCamY;
	a->gCamPZ = a->gCamZ;
	a->gLevel = level;
	a->gMode1 = mode1;
	a->gMode2 = mode2;
	a->gClock = clockFx;
	a->gFade = fade;
	a->gCamX = camX;
	a->gCamY = camY;
	a->gCamZ = camZ;
	a->gStateCalls++;
}

void NativeAmnesia_NoteEvent(u32 kind, s32 a, s32 b)
{
	AmnesiaState *st = &s_amnesia;
	AmnesiaEvent *e;

	/* Repeat-guard: per-frame re-assertions (a camera snap held across a
	 * respawn, mask refreshes in stuck states) would flood the ring and
	 * evict the events worth reading. Costs the probe repeat fires of the
	 * same event inside the window; the trigger question only needs one. */
	if (st->evWriteIdx > 0)
	{
		const AmnesiaEvent *last = &st->ev[(st->evWriteIdx - 1) & (AMN_EV_RING - 1)];

		if ((last->kind == kind) && (last->a == a) && ((st->foldIdx - last->frame) < 30))
		{
			return;
		}
	}
	e = &st->ev[st->evWriteIdx & (AMN_EV_RING - 1)];
	e->frame = st->foldIdx;
	e->kind = kind;
	e->a = a;
	e->b = b;
	st->evWriteIdx++;
}

internal void Amnesia_LogGameContext(void)
{
	AmnesiaState *a = &s_amnesia;
	char evbuf[144];
	u32 evlen = 0;
	u32 n = (a->evWriteIdx < AMN_EV_LOGGED) ? a->evWriteIdx : AMN_EV_LOGGED;

	evbuf[0] = '\0';
	for (u32 i = 0; i < n; i++)
	{
		const AmnesiaEvent *e = &a->ev[(a->evWriteIdx - 1 - i) & (AMN_EV_RING - 1)];
		int w;

		if (e->kind == 0)
		{
			break;
		}
		w = snprintf(evbuf + evlen, sizeof(evbuf) - evlen, "%s%lu/%ld/%ld@-%lu", evlen ? " " : "", (unsigned long)e->kind, (long)e->a, (long)e->b,
		             (unsigned long)(a->foldIdx - e->frame));
		if ((w < 0) || ((u32)w >= sizeof(evbuf) - evlen))
		{
			break;
		}
		evlen += (u32)w;
	}
	OTPROF_LOG("[AMNESIA] game lvl=%ld mode=%08lx/%08lx clock=%ld fade=%ld pause=%lu cam=%ld,%ld,%ld dcam=%ld,%ld,%ld st=%lu", (long)a->gLevel,
	           (unsigned long)a->gMode1, (unsigned long)a->gMode2, (long)a->gClock, (long)a->gFade, (unsigned long)(a->gMode1 & 0xFu), (long)a->gCamX,
	           (long)a->gCamY, (long)a->gCamZ, (long)(a->gCamX - a->gCamPX), (long)(a->gCamY - a->gCamPY), (long)(a->gCamZ - a->gCamPZ),
	           (unsigned long)a->gStateCalls);
	OTPROF_LOG("[AMNESIA] ev n=%lu [%s]", (unsigned long)a->evWriteIdx, evbuf);
}

internal void Amnesia_LogDomMatrix(u8 domId, u32 domShare, u32 idsUsed)
{
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	u32 w[8];

	if (GTECapture_ReadMatrixWords(domId, w))
	{
		OTPROF_LOG("[AMNESIA] mtx dom=%lu share=%lu%% ids=%lu R=%08lx,%08lx,%08lx,%08lx,%08lx TR=%ld,%ld,%ld", (unsigned long)domId,
		           (unsigned long)domShare, (unsigned long)idsUsed, (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
		           (unsigned long)w[4], (long)(s32)w[5], (long)(s32)w[6], (long)(s32)w[7]);
	}
	else
	{
		OTPROF_LOG("[AMNESIA] mtx dom=%lu share=%lu%% ids=%lu (no words)", (unsigned long)domId, (unsigned long)domShare, (unsigned long)idsUsed);
	}
#else
	(void)domId;
	(void)domShare;
	(void)idsUsed;
#endif
}

internal void Amnesia_LogDetail(u32 reused, u32 reused2, u32 shapeReused, u32 mReused, u8 domId, u32 domShare, u32 idsUsed)
{
	const u32 h = s_otprof.frameHashCount ? s_otprof.frameHashCount : 1;
	const u32 sh = s_otprof.frameShapeHashCount ? s_otprof.frameShapeHashCount : 1;
	const u32 mh = s_otprof.frameMHashCount ? s_otprof.frameMHashCount : 1;
	u32 single = 0;

	for (u32 i = 0; i < 64; i++)
	{
		single += s_otprof.frameMSingleById[i];
	}
	/* pk2 = same-parity N-2 reuse; split=1 when the dry walk and real walk
	 * disagree, the signature of a barrier-split partial fold. pk~0 with pk2
	 * high and split=1 is the barrier artifact, not a real cache collapse. */
	OTPROF_LOG("[AMNESIA] f=%lu pk=%lu%%/%lu pk2=%lu%% split=%d shp=%lu%% msh=%lu%%/%lu nodes=%lu prims=%lu dry=%lu mtx=%lu@%lu%%/%lu sdom=%lu/%lu multi=%lu",
	           (unsigned long)s_amnesia.foldIdx, (unsigned long)(reused * 100u / h), (unsigned long)s_otprof.frameHashCount,
	           (unsigned long)(reused2 * 100u / h), (s_otprof.frameDryNodes != s_otprof.frameNodes) ? 1 : 0, (unsigned long)(shapeReused * 100u / sh),
	           (unsigned long)(mReused * 100u / mh), (unsigned long)s_otprof.frameMHashCount, (unsigned long)s_otprof.frameNodes,
	           (unsigned long)s_otprof.framePrims, (unsigned long)s_otprof.frameDryNodes, (unsigned long)domId, (unsigned long)domShare,
	           (unsigned long)idsUsed, (unsigned long)s_otprof.frameMSingleById[domId], (unsigned long)single, (unsigned long)s_otprof.frameMMulti);
}

internal void Amnesia_OnFold(u32 reused, u32 reused2, u32 shapeReused, u32 mReused, u8 domId, u32 domShare, u32 idsUsed)
{
	AmnesiaState *a = &s_amnesia;
	const u32 hashed = s_otprof.frameHashCount;
	const u32 pkPct = hashed ? (reused * 100u / hashed) : 0;
	const u32 pkPct2 = hashed ? (reused2 * 100u / hashed) : 0;
	/* A barrier-split partial fold reads 0% against N-1 (the disjoint other
	 * half of the same frame) but high against its same-parity N-2 counterpart.
	 * Count either as healthy so the split does not burn episodes, and require
	 * BOTH to collapse before firing -- a real cache collapse fails both. */
	const int healthy = (hashed >= AMN_MIN_HASHED) && ((pkPct >= AMN_HEALTHY_PCT) || (pkPct2 >= AMN_HEALTHY_PCT));

	a->foldIdx++;

	/* A new level restocks the episode budget: a long single-level sit (a full
	 * intro) can otherwise spend it before the racing that the campaign cares
	 * about. gLevel is the game-thread mirror, read here as everything else in
	 * this block is -- forensic tolerance, no lock. */
	if (!a->detLevelValid || (a->gLevel != a->detLevel))
	{
		a->detLevel = a->gLevel;
		a->detLevelValid = 1;
		a->episodeCount = 0;
		a->episodeActive = 0;
		a->healthyStreak = 0;
		a->unhealthyRun = 0;
	}

	if (!g_dbg_amnesia)
	{
		a->healthyStreak = 0;
		a->episodeActive = 0;
		return;
	}

	if (a->episodeActive)
	{
		if (a->episodeDetailLeft > 0)
		{
			a->episodeDetailLeft--;
			Amnesia_LogDetail(reused, reused2, shapeReused, mReused, domId, domShare, idsUsed);
		}
		a->recoverRun = healthy ? (a->recoverRun + 1) : 0;
		if ((a->recoverRun >= AMN_RECOVER_FOLDS) || (a->foldIdx - a->episodeStartFold >= AMN_FORCE_END_FOLDS))
		{
			OTPROF_LOG("[AMNESIA] END f=%lu len=%lu pk=%lu%% forced=%d", (unsigned long)a->foldIdx, (unsigned long)(a->foldIdx - a->episodeStartFold),
			           (unsigned long)pkPct, (a->recoverRun >= AMN_RECOVER_FOLDS) ? 0 : 1);
			a->episodeActive = 0;
			a->healthyStreak = 0;
			a->unhealthyRun = 0;
		}
		return;
	}

	if (healthy)
	{
		a->healthyStreak++;
		a->unhealthyRun = 0;
		return;
	}

	if ((a->healthyStreak >= AMN_ARM_FOLDS) && (hashed >= AMN_MIN_HASHED) && (pkPct <= AMN_COLLAPSE_PCT) && (pkPct2 <= AMN_COLLAPSE_PCT) &&
	    (a->episodeCount < AMN_MAX_EPISODES))
	{
		a->episodeActive = 1;
		a->episodeCount++;
		a->episodeStartFold = a->foldIdx;
		a->episodeDetailLeft = AMN_DETAIL_FOLDS;
		a->recoverRun = 0;
		OTPROF_LOG("[AMNESIA] BEGIN #%lu f=%lu after=%lu healthy", (unsigned long)a->episodeCount, (unsigned long)a->foldIdx,
		           (unsigned long)a->healthyStreak);
		Amnesia_LogGameContext();
		Amnesia_LogDomMatrix(domId, domShare, idsUsed);
		a->episodeDetailLeft--;
		Amnesia_LogDetail(reused, reused2, shapeReused, mReused, domId, domShare, idsUsed);
		return;
	}

	/* Sub-collapse unhealthy folds (menus, transitions): tolerate a short
	 * ramp so a 2-3 fold slide into collapse still fires the trigger. */
	if (++a->unhealthyRun > AMN_UNHEALTHY_BREAK)
	{
		a->healthyStreak = 0;
	}
}

internal u32 OTProf_HashWords(const u32 *words, u32 count)
{
	u32 h = 2166136261u;
	for (u32 i = 0; i < count; i++)
	{
		h ^= words[i];
		h *= 16777619u;
	}
	return h ? h : 1u; /* 0 is the empty-slot sentinel */
}

internal void OTProf_RecordNode(const u8 *packet, u32 tagLength)
{
	s_otprof.frameNodes++;

	/* payload words only -- the tag word holds the OT link, which changes
	 * every frame even when the packet content does not. Lengths outside
	 * (0,32] are the walk's own error class; don't hash unvalidated bytes. */
	if (tagLength == 0 || tagLength > 32)
	{
		return;
	}
	if (s_otprof.frameHashCount < OTPROF_MAX_HASHES)
	{
		s_otprof.frameHashes[s_otprof.frameHashCount++] = OTProf_HashWords((const u32 *)packet + P_LEN, tagLength);
	}
}

/*
 * Position-masked node hash ("shape"): the same payload OTProf_RecordNode
 * hashes, with every screen-XY word zeroed per the PS1 GPU command encoding.
 * A frame-over-frame match then means "same colors/UVs/state, position free
 * to move" -- the OPTIMISTIC ceiling for model-space retention, where
 * pkreuse (full bytes, camera-sensitive) is the pessimistic floor. The truth
 * for retained geometry sits between the two: this hash cannot see
 * model-space vertex animation hiding behind a stable appearance.
 *
 * Word layouts must agree with ParsePrimitive/ParseTaglessPrimitive. The
 * agreement is checked at runtime instead of trusted: a node whose computed
 * prim lengths do not tile tagLength exactly is left unhashed and counted in
 * shapeSkip -- nonzero shapeSkip means these tables and the real parser have
 * diverged and shape%% is not to be believed.
 */
internal void OTProf_RecordNodeShape(const u8 *packet, u32 tagLength)
{
	if (tagLength == 0 || tagLength > 32)
	{
		return;
	}

	const u32 *words = (const u32 *)packet + P_LEN;
	u32 masked[32];
	u32 w = 0;
	/* Model-identity companion ("mshape"): positions of the XY words this
	 * node's polys carried, so the tail can swap each for its model-space
	 * vector via the GTE-capture rendezvous. Any non-poly positioned prim
	 * (line, sprite, tile, fill, polyline) makes the node model-opaque --
	 * their positions ARE screen content, not projections of a mesh. */
	u8 xyIdx[32];
	u32 xyCount = 0;
	int modelOpaque = 0;

	for (u32 i = 0; i < tagLength; i++)
	{
		masked[i] = words[i];
	}

	while (w < tagLength)
	{
		const u32 code = masked[w] >> 24;
		u32 next;

		if (code >= 0x20 && code <= 0x3F)
		{
			const u32 gouraud = (code & 0x10u) ? 1u : 0u;
			const u32 textured = (code & 0x04u) ? 1u : 0u;
			const u32 numVerts = (code & 0x08u) ? 4u : 3u;
			u32 c = w + 1;

			for (u32 v = 0; v < numVerts; v++)
			{
				if (v > 0 && gouraud)
				{
					c++; /* per-vertex RGB */
				}
				if (c >= tagLength)
				{
					s_otprof.frameShapeSkip++;
					return;
				}
				xyIdx[xyCount++] = (u8)c;
				masked[c++] = 0; /* XY */
				if (textured)
				{
					c++; /* UV + clut/tpage */
				}
			}
			next = c;
		}
		else if (code >= 0x40 && code <= 0x5F)
		{
			modelOpaque = 1;
			if (code & 0x08u)
			{
				/* Polyline: variable length with a terminator scan. Mask the
				 * whole remainder rather than re-implement the scan; merges
				 * any trailing prims into the masked span, which only makes
				 * the ceiling reading slightly more optimistic. */
				for (u32 i = w + 1; i < tagLength; i++)
				{
					masked[i] = 0;
				}
				next = tagLength;
			}
			else if (code & 0x10u)
			{
				next = w + 4; /* G2: cmd+RGB0, XY0, RGB1, XY1 */
				if (next <= tagLength)
				{
					masked[w + 1] = 0;
					masked[w + 3] = 0;
				}
			}
			else
			{
				next = w + 3; /* F2: cmd+RGB, XY0, XY1 */
				if (next <= tagLength)
				{
					masked[w + 1] = 0;
					masked[w + 2] = 0;
				}
			}
		}
		else if (code >= 0x60 && code <= 0x7F)
		{
			/* Tiles/sprites: cmd+RGB, XY, [UV+clut if sprite], [WH if
			 * free-size]. XY is the position; W/H and UV are appearance. */
			const u32 sprite = (code & 0x04u) ? 1u : 0u;
			const u32 freeSize = ((code & 0x18u) == 0) ? 1u : 0u;

			modelOpaque = 1;
			next = w + 2 + sprite + freeSize;
			if (next <= tagLength)
			{
				masked[w + 1] = 0;
			}
		}
		else if (code == 0x02)
		{
			modelOpaque = 1;
			next = w + 3; /* fill: cmd+RGB, XY, WH */
			if (next <= tagLength)
			{
				masked[w + 1] = 0;
			}
		}
		else if (code >= 0xE0 && code <= 0xE7)
		{
			next = w + 1; /* draw-env settings; no position to mask */
		}
		else if (code == 0x00)
		{
			next = w + 1; /* NOP filler */
		}
		else
		{
			/* Unknown to these tables (MoveImage, IRQ, ...): don't guess. */
			s_otprof.frameShapeSkip++;
			return;
		}

		if (next > tagLength)
		{
			s_otprof.frameShapeSkip++;
			return;
		}
		w = next;
	}

	if (s_otprof.frameShapeHashCount < OTPROF_MAX_HASHES)
	{
		s_otprof.frameShapeHashes[s_otprof.frameShapeHashCount++] = OTProf_HashWords(masked, tagLength);
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* Model-identity hash: appearance words as masked above, plus each XY's
	 * model-space vector recovered through the GTE-capture rendezvous. A
	 * repeat of THIS hash frame-over-frame is true retainability -- same
	 * mesh data, same appearance, position free to be a matrix. UI-band
	 * packets and any vertex that misses the table (LERP'd mid-band
	 * geometry, procedural prims, collisions) make the node model-opaque:
	 * counted, not hashed. */
	if (modelOpaque
#if CTR_NATIVE_VERTEX_DESNAP
	    || GTECapture_LookupSuppressed()
#endif
	)
	{
		s_otprof.frameMNoModel++;
		return;
	}
	{
		u32 mwords[96];
		u32 mcount = 0;
		u8 nodeMtxId = 0;
		int mtxSame = 1;

		for (u32 i = 0; i < tagLength; i++)
		{
			mwords[mcount++] = masked[i];
		}
		for (u32 i = 0; i < xyCount; i++)
		{
			const u32 xy = words[xyIdx[i]];
			s16 v[3];
			u8 mtxId;

			if (!GTECapture_LookupModelEx((s16)(xy & 0xFFFFu), (s16)(xy >> 16), v, &mtxId))
			{
				s_otprof.frameMNoModel++;
				return;
			}
			if (i == 0)
			{
				nodeMtxId = mtxId;
			}
			else if (mtxId != nodeMtxId)
			{
				mtxSame = 0;
			}
			mwords[mcount++] = ((u32)(u16)v[1] << 16) | (u32)(u16)v[0];
			mwords[mcount++] = (u32)(u16)v[2];
		}
		if (mtxSame && (xyCount > 0))
		{
			s_otprof.frameMSingleById[nodeMtxId]++;
		}
		else
		{
			s_otprof.frameMMulti++;
		}
		if (s_otprof.frameMHashCount < OTPROF_MAX_HASHES)
		{
			s_otprof.frameMHashMtxId[s_otprof.frameMHashCount] = (mtxSame && (xyCount > 0)) ? nodeMtxId : 255;
			s_otprof.frameMHashes[s_otprof.frameMHashCount++] = OTProf_HashWords(mwords, mcount);
		}
	}
#endif /* CTR_NATIVE_GTE_CAPTURE */
}

internal void OTProf_CountPrim(BlendMode blendMode, TexFormat texFormat, bool textured, int tpage)
{
	s_otprof.framePrims++;
	s_otprof.frameBlend[blendMode]++;
	if (textured)
	{
		s_otprof.frameTextured++;
	}

	/* State key mirrors what a state-sorted submitter would bind: texture
	 * page origin + format + blend class. CLUT variants excluded in v1, so
	 * keys/f is a lower bound on 3DS decode-cache binds. */
	const u32 key = ((u32)tpage & 0x81Fu) | ((u32)texFormat << 12) | ((u32)blendMode << 16) | ((u32)(textured ? 1 : 0) << 20);
	if (key != s_otprof.lastKey)
	{
		s_otprof.frameKeyTransitions++;
		s_otprof.lastKey = key;
	}

	u32 slot = (key * 2654435761u) & (OTPROF_KEY_SET_SIZE - 1);
	for (u32 probes = 0; probes < OTPROF_KEY_SET_SIZE; probes++)
	{
		if (s_otprof.keySet[slot] == key)
		{
			return;
		}
		if (s_otprof.keySet[slot] == 0)
		{
			s_otprof.keySet[slot] = key;
			s_otprof.frameKeys++;
			return;
		}
		slot = (slot + 1) & (OTPROF_KEY_SET_SIZE - 1);
	}
}

internal void OTProf_CountDraw(void)
{
	s_otprof.frameDraws++;
}

internal void OTProf_ReportWindow(void);

internal void OTProf_FoldFrame(void)
{
	if (s_otprof.frameNodes == 0 && s_otprof.framePrims == 0)
	{
		return; /* GPU-disabled or empty scene edge; not a rendered frame */
	}

	/*
	 * Ablation-config boundary: report what is already banked BEFORE this
	 * frame joins it. The sweep applies a config at the end of a frame, so
	 * this frame is the first one that ran under the new one -- folding it in
	 * first and noticing afterwards would put one contaminated frame in the
	 * outgoing window every switch. winCfg is NULL only on the very first
	 * fold after boot, when there is nothing banked to protect.
	 */
	if ((s_otprof.winCfg != g_dbg_ablateConfig) && (s_otprof.winFrames > 0))
	{
		OTProf_ReportWindow();
	}
	if (s_otprof.winFrames == 0)
	{
		s_otprof.winCfg = g_dbg_ablateConfig;
	}

	u32 reused = 0;
	for (u32 i = 0; i < s_otprof.frameHashCount; i++)
	{
		const u32 h = s_otprof.frameHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevSet[slot] != 0)
		{
			if (s_otprof.prevSet[slot] == h)
			{
				reused++;
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	/* Same-parity N-2 reuse: compare this fold against two folds ago, before
	 * prevSet2 is overwritten below. See the prevSet2 declaration. */
	u32 reused2 = 0;
	for (u32 i = 0; i < s_otprof.frameHashCount; i++)
	{
		const u32 h = s_otprof.frameHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevSet2[slot] != 0)
		{
			if (s_otprof.prevSet2[slot] == h)
			{
				reused2++;
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	/* Age the history: this fold's prevSet (N-1) becomes N-2 for the next
	 * fold, then prevSet is rebuilt from this fold's hashes below. */
	memcpy(s_otprof.prevSet2, s_otprof.prevSet, sizeof(s_otprof.prevSet2));

	memset(s_otprof.prevSet, 0, sizeof(s_otprof.prevSet));
	for (u32 i = 0; i < s_otprof.frameHashCount; i++)
	{
		const u32 h = s_otprof.frameHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevSet[slot] != 0 && s_otprof.prevSet[slot] != h)
		{
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
		s_otprof.prevSet[slot] = h;
	}

	u32 shapeReused = 0;
	for (u32 i = 0; i < s_otprof.frameShapeHashCount; i++)
	{
		const u32 h = s_otprof.frameShapeHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevShapeSet[slot] != 0)
		{
			if (s_otprof.prevShapeSet[slot] == h)
			{
				shapeReused++;
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	memset(s_otprof.prevShapeSet, 0, sizeof(s_otprof.prevShapeSet));
	for (u32 i = 0; i < s_otprof.frameShapeHashCount; i++)
	{
		const u32 h = s_otprof.frameShapeHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevShapeSet[slot] != 0 && s_otprof.prevShapeSet[slot] != h)
		{
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
		s_otprof.prevShapeSet[slot] = h;
	}

	u32 mReused = 0;
	u32 mReusedDom = 0;
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	u8 foldDomId = 0;
	u32 foldIdsUsed = 0;
	const u32 foldDomShare = GTECapture_ReadMatrixCensus(&foldDomId, &foldIdsUsed);
#endif
	for (u32 i = 0; i < s_otprof.frameMHashCount; i++)
	{
		const u32 h = s_otprof.frameMHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevMSet[slot] != 0)
		{
			if (s_otprof.prevMSet[slot] == h)
			{
				mReused++;
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
				/* The full-form v1 planning number: stable AND projected
				 * under this frame's dominant (camera) matrix. */
				if (s_otprof.frameMHashMtxId[i] == foldDomId)
				{
					mReusedDom++;
				}
#endif
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	memset(s_otprof.prevMSet, 0, sizeof(s_otprof.prevMSet));
	for (u32 i = 0; i < s_otprof.frameMHashCount; i++)
	{
		const u32 h = s_otprof.frameMHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevMSet[slot] != 0 && s_otprof.prevMSet[slot] != h)
		{
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
		s_otprof.prevMSet[slot] = h;
	}

	Amnesia_OnFold(reused, reused2, shapeReused, mReused,
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	               foldDomId, foldDomShare, foldIdsUsed
#else
	               0, 0, 0
#endif
	);

	s_otprof.winFrames++;
	s_otprof.winNodes += s_otprof.frameNodes;
	s_otprof.winPrims += s_otprof.framePrims;
	for (int i = 0; i < 5; i++)
	{
		s_otprof.winBlend[i] += s_otprof.frameBlend[i];
	}
	s_otprof.winTextured += s_otprof.frameTextured;
	s_otprof.winDraws += s_otprof.frameDraws;
	s_otprof.winKeys += s_otprof.frameKeys;
	s_otprof.winKeyTransitions += s_otprof.frameKeyTransitions;
	s_otprof.winHashed += s_otprof.frameHashCount;
	s_otprof.winReused += reused;
	s_otprof.winShapeHashed += s_otprof.frameShapeHashCount;
	s_otprof.winShapeReused += shapeReused;
	s_otprof.winShapeSkip += s_otprof.frameShapeSkip;
	s_otprof.winMHashed += s_otprof.frameMHashCount;
	s_otprof.winMReused += mReused;
	s_otprof.winMNoModel += s_otprof.frameMNoModel;
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	{
		/* Matrix-class fold: this frame's dominant matrix (census already
		 * read above, before the reuse loop needed it) vs where the
		 * mshape-hashed nodes actually sat. */
		for (int i = 0; i < 64; i++)
		{
			s_otprof.winMSingle += s_otprof.frameMSingleById[i];
		}
		s_otprof.winMSingleDom += s_otprof.frameMSingleById[foldDomId];
		s_otprof.winMMulti += s_otprof.frameMMulti;
		s_otprof.winMtxDomShare += foldDomShare;
		s_otprof.winMtxIds += foldIdsUsed;
	}
#endif
	s_otprof.winMReusedDom += mReusedDom;
	if (s_otprof.frameHashCount >= OTPROF_MAX_HASHES)
	{
		s_otprof.winTruncated++;
	}
	if (s_otprof.frameDryNodes != s_otprof.frameNodes)
	{
		s_otprof.winDryMismatch++;
	}

	s_otprof.frameNodes = 0;
	s_otprof.frameDryNodes = 0;
	s_otprof.framePrims = 0;
	memset(s_otprof.frameBlend, 0, sizeof(s_otprof.frameBlend));
	s_otprof.frameTextured = 0;
	s_otprof.frameDraws = 0;
	s_otprof.frameKeys = 0;
	s_otprof.frameKeyTransitions = 0;
	s_otprof.lastKey = 0;
	s_otprof.frameHashCount = 0;
	s_otprof.frameShapeHashCount = 0;
	s_otprof.frameShapeSkip = 0;
	s_otprof.frameMHashCount = 0;
	s_otprof.frameMNoModel = 0;
	memset(s_otprof.frameMSingleById, 0, sizeof(s_otprof.frameMSingleById));
	s_otprof.frameMMulti = 0;
	memset(s_otprof.keySet, 0, sizeof(s_otprof.keySet));

	if (s_otprof.winFrames < OTPROF_WINDOW_FRAMES)
	{
		return;
	}

	OTProf_ReportWindow();
}

/*
 * Emit one window and reset the accumulators. Split out of OTProf_FoldFrame so
 * a window can also be cut EARLY, when the ablation config changes underneath
 * it (see g_dbg_ablateConfig): a [OTPROF] window that spans "full" and "no-hud"
 * describes neither, and reading one as if it did is exactly the unmatched-
 * exposure mistake PORT_FEASIBILITY_3DS.md's "How to measure" was written
 * about.
 */
internal void OTProf_ReportWindow(void)
{
	const u32 f = s_otprof.winFrames ? s_otprof.winFrames : 1;
	const u32 p = s_otprof.winPrims ? s_otprof.winPrims : 1;
	const u32 hp = s_otprof.winHashed ? s_otprof.winHashed : 1;
	OTPROF_LOG("[OTPROF] cfg=%s f=%lu nodes/f=%lu prims/f=%lu draws/f=%lu keys/f=%lu ktrans/f=%lu pkreuse=%lu%% pk/f=%lu trunc=%lu drymis=%lu",
	             s_otprof.winCfg ? s_otprof.winCfg : "none",
	             (unsigned long)f, (unsigned long)(s_otprof.winNodes / f), (unsigned long)(s_otprof.winPrims / f), (unsigned long)(s_otprof.winDraws / f),
	             (unsigned long)(s_otprof.winKeys / f), (unsigned long)(s_otprof.winKeyTransitions / f), (unsigned long)(s_otprof.winReused * 100u / hp),
	             (unsigned long)(s_otprof.winHashed / f), (unsigned long)s_otprof.winTruncated, (unsigned long)s_otprof.winDryMismatch);
	OTPROF_LOG("[OTPROF] blend%%: opq=%lu avg=%lu add=%lu sub=%lu aq=%lu | tex=%lu%%", (unsigned long)(s_otprof.winBlend[BM_NONE] * 100u / p),
	             (unsigned long)(s_otprof.winBlend[BM_AVERAGE] * 100u / p), (unsigned long)(s_otprof.winBlend[BM_ADD] * 100u / p),
	             (unsigned long)(s_otprof.winBlend[BM_SUBTRACT] * 100u / p), (unsigned long)(s_otprof.winBlend[BM_ADD_QUATER_SOURCE] * 100u / p),
	             (unsigned long)(s_otprof.winTextured * 100u / p));
	{
		const u32 sh = s_otprof.winShapeHashed ? s_otprof.winShapeHashed : 1;
		const u32 mh = s_otprof.winMHashed ? s_otprof.winMHashed : 1;
		OTPROF_LOG("[OTPROF] shape=%lu%% shp/f=%lu shapeSkip=%lu mshape=%lu%% mshp/f=%lu nomdl/f=%lu",
		             (unsigned long)(s_otprof.winShapeReused * 100u / sh), (unsigned long)(s_otprof.winShapeHashed / f), (unsigned long)s_otprof.winShapeSkip,
		             (unsigned long)(s_otprof.winMReused * 100u / mh), (unsigned long)(s_otprof.winMHashed / f), (unsigned long)(s_otprof.winMNoModel / f));
	}
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	{
		/* Full-form open Q1 instrument: singleDom% is the v1 coverage cap --
		 * the share of model-identified nodes whose every vertex projected
		 * under the frame's dominant (camera) matrix. */
		const u32 mn = (s_otprof.winMSingle + s_otprof.winMMulti) ? (s_otprof.winMSingle + s_otprof.winMMulti) : 1;

		/* stableDom% (base: mshape-hashed nodes) is THE full-form v1
		 * coverage number: frame-over-frame stable AND camera-class. */
		const u32 mh2 = s_otprof.winMHashed ? s_otprof.winMHashed : 1;

		OTPROF_LOG("[OTPROF] mtx: single=%lu%% singleDom=%lu%% multi=%lu%% stableDom=%lu%% | ids/f=%lu domStores=%lu%%",
		             (unsigned long)(s_otprof.winMSingle * 100u / mn), (unsigned long)(s_otprof.winMSingleDom * 100u / mn),
		             (unsigned long)(s_otprof.winMMulti * 100u / mn), (unsigned long)(s_otprof.winMReusedDom * 100u / mh2),
		             (unsigned long)(s_otprof.winMtxIds / f), (unsigned long)(s_otprof.winMtxDomShare / f));
	}
#endif

	s_otprof.winFrames = 0;
	s_otprof.winNodes = 0;
	s_otprof.winPrims = 0;
	memset(s_otprof.winBlend, 0, sizeof(s_otprof.winBlend));
	s_otprof.winTextured = 0;
	s_otprof.winDraws = 0;
	s_otprof.winKeys = 0;
	s_otprof.winKeyTransitions = 0;
	s_otprof.winHashed = 0;
	s_otprof.winReused = 0;
	s_otprof.winShapeHashed = 0;
	s_otprof.winShapeReused = 0;
	s_otprof.winShapeSkip = 0;
	s_otprof.winMHashed = 0;
	s_otprof.winMReused = 0;
	s_otprof.winMNoModel = 0;
	s_otprof.winMSingle = 0;
	s_otprof.winMSingleDom = 0;
	s_otprof.winMMulti = 0;
	s_otprof.winMReusedDom = 0;
	s_otprof.winMtxDomShare = 0;
	s_otprof.winMtxIds = 0;
	s_otprof.winTruncated = 0;
	s_otprof.winDryMismatch = 0;
	s_otprof.winCfg = g_dbg_ablateConfig;
}
#endif /* CTR_NATIVE_OTWALK_PROFILE */

struct NativeGpuSnapshot
{
	u32 magic;
	u32 version;
	u32 size;
	DRAWENV drawEnv;
	DISPENV dispEnv;
	s32 gpuDisabledState;
	s32 psxDrawMaskSet;
	u16 vram[VRAM_WIDTH * VRAM_HEIGHT];
};

int NativeGpu_HasPendingSplits(void)
{
	return s_gpu.splitIndex > 0;
}

/*
 * Stamp every vertex the last Parse*Primitive call appended with the current
 * emission ordinal, then advance it. One ordinal per primitive, monotonic in
 * OT-walk (= submission) order. Clamps at 0xFFFF instead of wrapping: a
 * wrap would put late primitives BEHIND early ones; a clamp only collapses
 * the >65535-prims tail to equal depth, where LEQUAL/GEQUAL-with-equal-pass
 * still resolves by draw order -- i.e. degrades to painter, never inverts.
 */
/* Called by ClearOTag/ClearOTagR (native_libgpu.c): remember this OT's
 * exact bounds for the walk's slot detection. Deduped by base -- the same
 * OTs are cleared once per frame, forever. */
void NativeGpu_RegisterOTRange(uint32_t *ot, int n)
{
	if ((ot == NULL) || (n <= 0))
	{
		return;
	}

	for (u32 i = 0; i < 8; i++)
	{
		if (s_gpu.otRanges[i].base == (uintptr_t)ot)
		{
			s_gpu.otRanges[i].count = (u32)n;
			return;
		}
	}

	s_gpu.otRanges[s_gpu.otRangeCursor & 7].base = (uintptr_t)ot;
	s_gpu.otRanges[s_gpu.otRangeCursor & 7].count = (u32)n;
	s_gpu.otRangeCursor++;
}

/* The single source of the depth stamp for BOTH stampers (parse path below,
 * direct emit in native_direct_emit.c). Bare sequence ordinal -- v1's
 * widened stamp was deleted after its own [OTZ] occupancy instrument
 * falsified the encoding (see the v2 table comment in NativeGpuState);
 * v2 records instead of restamping, so this stays byte-identical to the
 * pre-rung-1 behavior. */
/* Escape detector for the depth model.
 * NextDepthStamp is the single stamp source for BOTH emit paths, so counting
 * here gives the true number of stamped primitives per frame. [PKTDEPTH]
 * compares it against the number the resolver actually saw: if any emit path
 * writes a stamp without going through PacketDepth_ResolveXY, that path is
 * still on the ordinal axis and the two numbers diverge.
 *
 * This exists because exactly that happened and a screenshot did not reveal it:
 * native_direct_emit.c carries most of a heavy track's opaque content and was
 * silently bypassing the resolver. Counting is how a third emit path gets
 * caught without waiting for someone to notice geometry through a wall. */
global_variable u32 s_pktDepthStamps;

internal u16 NativeGpu_NextDepthStamp(void)
{
	const u16 ordinal = (u16)(s_gpu.emitOrdinal < 0xFFFFu ? s_gpu.emitOrdinal : 0xFFFFu);

	s_pktDepthStamps++;
	s_gpu.lastStampedZ = ordinal;
	return ordinal;
}

/* ---- Packet-depth debug toggles -------------------------------------------
 *
 * A toggle's DEFINITION is unconditional; only the code it gates is guarded.
 * Most of what these switch lives in the CTR_NATIVE_GTE_CAPTURE block below,
 * but the consumers are not all inside it: DrawSplit further down this file is
 * compiled in every config, and the 3DS backend and renderer TUs extern these
 * without a capture guard. A definition parked inside the guard therefore
 * vanishes in a capture-OFF build while its uses remain, which does not fail
 * until compile or link. Same idiom as g_dbg_trackRender / g_dbg_modelRender.
 *
 * Each keeps its normal default here, so a capture-OFF build behaves exactly
 * as if the toggle had never been flipped.
 */

/* Boot default; see CTR_NATIVE_RENDERER_DEFAULT_ON in CMakeLists.txt. This one
 * toggle drives BOTH sides of the shared depth axis (packets stamp sz3, and the
 * track shader's selector reads it in n3ds_backend.c) -- flipping only one puts
 * the geometry sources on different axes, which is worse than either alone. */
#ifndef CTR_NATIVE_RENDERER_DEFAULT_ON
#define CTR_NATIVE_RENDERER_DEFAULT_ON 0
#endif

int g_dbg_packetDepth = CTR_NATIVE_RENDERER_DEFAULT_ON;

/*
 * Owner-requested depth bias, in view.z units (stamp units are the same
 * scale: stamp = 0xFFFF - sz3, so +1 stamp == 1 unit NEARER the camera).
 *
 * Applies to WORLD packet content only -- this is the single funnel every
 * world stamp passes through, and the 2D/UI band is resolved before it, so UI
 * is untouched. Because it shifts every world packet by the same amount,
 * packet-vs-packet ordering is bit-for-bit unchanged; the only relationship it
 * moves is packet-vs-TRACK, which is exactly the complaint ("racers sinking
 * into the ground").
 *
 * A HACK, deliberately, and it trades one artefact against another: too small
 * and racers still sink, too large and they show through walls, and on a track
 * with thin geometry there may be no value that is clean everywhere. The real
 * fix is the track side -- the near-plane clamp (ctr_track.v.pica's
 * depthMinWorld) and behind-camera leaves, both still open. Default 0 so this
 * costs nothing and changes nothing until it is asked for.
 */
int g_dbg_pktDepthBias = 0;

/* Deferred driver-local exhaust-smoke pass: driver-local smoke is held out of
 * the normal merge and redrawn AFTER the native model pass so it paints over
 * its own kart body (retail's painter's order) while a nearer wall/kart still
 * occludes it.
 *
 * It needs no near-bias: the smoke's own captured view.z depth
 * (GTECapture_StoreParticleCorner), drawn after the body, is sufficient. The
 * depth CAPTURE is essential: without it the smoke falls back to the
 * parse-order ordinal axis, reads as infinitely far, and is occluded by all. */

/* Set per node by the OT walk (beside GTECapture_MarkOTNode): whether the node
 * the walk is about to hand to direct-emit or the parse path is sky-class (the
 * backdrop union, the star slice, or the menu backdrop span). Read by
 * PacketDepth_ResolveXY, which sees only screen coordinates, not the packet
 * address. Self-resetting -- assigned every node -- so it needs no separate
 * per-walk clear.
 *
 * Unconditional for the same reason as the toggles above: native_direct_emit.c
 * reads both under CTR_NATIVE_DIRECT_EMIT, which does not imply capture. The
 * producer stays in the guarded block, so a capture-OFF build leaves them 0 and
 * the direct-emit sky/glow paths simply never trigger. */
global_variable int s_skyNodeActive;
/* s46: same per-packet tagging for the glow slice (subset of the sky span).
 * Read by the direct-emit vertex loop to decide the wide-window X stretch. */
global_variable int s_glowNodeActive;
/* Same per-node tagging for the in-race minimap block's span. Read by
 * PacketDepth_ResolveXY's suppressed-2D leg to restamp map primitives for the
 * 3DS bottom-screen map (PKTDEPTH_UI_STAMP_BTMMAP). */
global_variable int s_uimapNodeActive;
/* FPS-overlay sub-span of the map block (PKTDEPTH_UI_STAMP_BTMFPS). */
global_variable int s_uifpsNodeActive;
/* Picture-sprite span: menu/hub map sheets (PKTDEPTH_UI_STAMP_UIPIC). */
global_variable int s_uipicNodeActive;
/* Juiced-wumpa HUD shine span (PKTDEPTH_UI_STAMP_HUDSHINE): UI_Weapon_DrawBG's
 * quads, which retail links into the player's OT. Read by the resolver and by
 * the direct-emit fast path's UI stand-aside, beside the suppress latch. */
global_variable int s_hudshineNodeActive;
/* Full-window fade span (PKTDEPTH_UI_STAMP_SCREENFADE): PushBuffer_FadeOneWindow's
 * quads, the per-player ones linked into the player's OT. Read by the resolver
 * and by the direct-emit fast path's UI stand-aside, beside the shine's. */
global_variable int s_screenfadeNodeActive;
/* Menu-backdrop span (character-select diorama strips). Read by
 * PacketDepth_ResolveXY, which sees screen coordinates, not packet addresses. */
global_variable int s_menubdNodeActive;

/*
 * Runtime A/B for the HUD shine's UI-band classification (dumpreq 'hudshine').
 * ON = UI_Weapon_DrawBG's quads take the UI band (widescreen X ramp, stereo
 * flatten, far-edge inset) although retail links them into the player's OT.
 * OFF restores the world-bucket fallback: the burst stays at retail X while
 * the fruit and icon it backs move with the wide window.
 */
int g_dbg_hudShine = 1;

/*
 * Runtime A/B for the full-window fade's UI-band classification (dumpreq
 * 'screenfade'). ON = the per-player fade quads (hit flash, finish-line flash)
 * draw in the ordered UI pass across the whole wide window. OFF restores the
 * world-bucket fallback: track and models depth-test through the flash and the
 * widescreen margins stay unflashed.
 */
int g_dbg_screenFade = 1;

/* ------------------------------------------------------------------------- */

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
/* Packet depth from GTE sz3.
 *
 * OFF by default and inert until enabled: with g_dbg_packetDepth == 0 nothing
 * here runs and NativeGpu_StampEmitOrdinal writes exactly what it writes today.
 * The visual half cannot be validated on PC (PC runs no depth test), so it
 * stays off until Azahar and hardware have looked at it.
 *
 * Per-primitive, never per-vertex (design 3.2): one missing vertex disqualifies
 * the whole primitive, because splitting a face across two depth axes puts a
 * seam in the middle of it.
 * Measured cost of the whole-primitive rule: 97.09% of primitives covered.
 *
 * max(), not average, because that is what retail does: both
 * DrawLevelOvr1P_GetProjected*Depth helpers (226_00_DrawLevelOvr1P.c:1922-1947)
 * run a plain running maximum over the primitive's vertices and feed it to
 * GetProjectedOtIndex as maxDepth. Matching it keeps the real value and the
 * otIndex<<6 fallback on the same footing.
 *
 * SENSE INVERSION -- the one thing here reasoned rather than measured, and the
 * first thing to check if this looks wrong in Azahar. sz3 INCREASES with
 * distance (it is view.z). Emit ordinals INCREASE with proximity: the OT walk
 * descends, so later emissions are nearer. The axes run opposite ways, and the
 * depth test's sense is already fixed by the ordinal convention, so the lookup
 * is flipped to match the test rather than the other way round.
 *
 * Declared here, above NativeGpu_StampEmitOrdinal, because that is the consumer.
 */

/* ONE AXIS, FOUR CASES. Every stamp below is "nearer = larger", and every one
 * of them must land on the SAME numeric scale, because the depth test cannot
 * tell an ordinal from a depth -- it just compares u16s.
 *
 * Getting this wrong is not subtle and it is not theoretical: the first Azahar
 * run with the feature on drew N. Gin Labs' animated waterfalls over the entire
 * scene INCLUDING the HUD. Those are AnimateWater vertices, mutated per frame,
 * so they are never GTE-projected and always miss the lookup -- and the miss
 * path was leaving them on the raw emit ordinal, a small integer on a scale
 * that has nothing to do with 0..65535 view depth. Design 3.3 says exactly this
 * ("NOT the ordinal axis. That would reintroduce two axes"); the implementation
 * did it anyway.
 *
 *   world, covered  : 0xFFFF - maxSz3                (true view depth)
 *   world, miss     : 0xFFFF - (otIndex << 6)        (same axis, bucket-coarse;
 *                                                     inverts retail's >>6)
 *   2D / UI         : PKTDEPTH_UI_BASE + ordinal     (reserved near band)
 *   feature off     : the emit ordinal, exactly as before
 *
 * The near band is reserved off the TOP of the range, so world content is
 * clamped to PKTDEPTH_WORLD_MAX. That costs the nearest PKTDEPTH_UI_BAND units
 * of view.z -- and that cost is NOT free: at 256 it was inside the range the
 * stopped-camera reaches, so everything nearer than 256 units flattened onto
 * one depth plane and draw order decided (kart parts through the back of the
 * driver's head). 64 units is nearer than the camera ever
 * sits. UI ordinals are packed 4-per-stamp (>> 2 below) so the band still
 * orders 256 UI prims -- the same capacity and same wrap point as the old
 * 256-wide band. ctr_track.v.pica and ctr_model.v.pica apply the identical
 * world clamp; if one side is changed without the others, the sources stop
 * sharing the axis (their headers say the same thing back). */
enum
{
	PKTDEPTH_UI_BAND = 64,
	/*
	 * 4 UI prims per stamp -> the band orders 64*4 = 256 primitives.
	 *
	 * Do not widen this packing to buy capacity: the draw merge bins opaque
	 * content by state and replays it per bin, so submission order only
	 * holds within a bin. Prims sharing a stamp tie and resolve by
	 * draw order; coarser packing turns that into cross-bin ordering
	 * corruption for UI (glyph/panel swaps), not just lost precision. The
	 * real fix for capacity is to stop UI depending on the depth test for
	 * ordering at all -- draw UI in submission order outside the merge bins.
	 */
	PKTDEPTH_UI_ORD_SHIFT = 2,                        /* 4 UI prims share a stamp */
	PKTDEPTH_UI_BASE = 0xFFFF - PKTDEPTH_UI_BAND + 1, /* 0xFFC0 */
	PKTDEPTH_WORLD_MAX = PKTDEPTH_UI_BASE - 1,        /* 0xFFBF */
	/* Menu backdrop (character-select diorama strips, block above
	 * NativeGpu_MarkMenuBackdropRange). Bounded on both sides and nowhere
	 * near either: it must beat the menu checkered flag, which resolves as a
	 * miss and so sits on the raw emit-ordinal band a few thousand above the
	 * far pin, and must lose to the preview model, whose real stamp is
	 * 0xFFFF - sz3 and lands near the near end for a close diorama pose.
	 * Midband satisfies both with the widest margin available. */
	PKTDEPTH_MENU_BACKDROP = 0x8000,
	PKTDEPTH_OT_SHIFT = 6                             /* retail's maxDepth >> 6 */
};

/*
 * UI-BAND ORDINAL (s40c). Counts only primitives that actually land in the 2D/UI
 * band, restarting each OT walk -- as opposed to s_gpu.emitOrdinal, which counts
 * every primitive in the frame including all world geometry.
 *
 * WHY IT EXISTS. The band is 64 stamps packed 4 prims apiece, so it orders 256
 * primitives before `& (PKTDEPTH_UI_BAND - 1)` wraps. Deriving its stamps from
 * the global ordinal meant two things, both bad:
 *
 *   1. The wrap landed at an ARBITRARY PHASE -- wherever the frame's world prim
 *      count happened to leave the counter. UI content straddling that boundary
 *      inverts against itself, which is a plausible source of the owner's
 *      long-standing "start-light/UI layering varies by race" report (not yet
 *      confirmed; it is listed as an open item, and this may or may not be it).
 *   2. The menu/loading flag, which retail links into the swapchain OT head
 *      during a transition, put ~1000 primitives THROUGH the band -- lapping it
 *      about four times. Owner-visible on hardware as the flag intersecting UI
 *      "in stripes based on how the flag is waving": each lap is a vertical band
 *      (~9 of the flag's 35 columns), and the boundaries slide as the wave's
 *      off-screen quad cull changes which quads emit.
 *
 * With a dedicated counter the 256-prim capacity is real, the phase is fixed at
 * zero every walk, and the flag consumes exactly ONE slot (below).
 */
global_variable u32 s_pktUiOrdinal;
/* The flag's single UI stamp for this walk, latched at its first primitive. */
global_variable u16 s_pktFlagUiStamp;
global_variable int s_pktFlagUiStampValid;

/* High-water UI primitives in a single walk, across the whole run. Reported by
 * [PKTDEPTH] as uiMax. This is the number that says whether the band is near
 * its 1024-prim capacity -- the question that had to be answered by a hardware
 * symptom (pause-menu flicker) the first time, because nothing counted it. */
global_variable u32 s_pktUiOrdinalMax;

/* Per-walk reset. Called beside GTECapture_BeginOTWalk -- same instant, same
 * reason: both describe state that is only meaningful within one walk. */
internal void PacketDepth_BeginWalk(void)
{
	if (s_pktUiOrdinal > s_pktUiOrdinalMax)
	{
		s_pktUiOrdinalMax = s_pktUiOrdinal;
	}
	s_pktUiOrdinal = 0;
	s_pktFlagUiStampValid = 0;
}

/*
 * Stamp for the n'th UI primitive of this walk. SATURATES at the top of the
 * band instead of wrapping.
 *
 * Wrapping is catastrophic and saturating is merely lossy: past capacity, a
 * wrap sends the next primitive to the FAR end of the band, so late UI draws
 * behind early UI and the layering inverts outright. Saturating ties the
 * overflow at the nearest stamp, where submission order under LEQUAL resolves
 * it -- the same tie-break the packing already depends on. Overflow should not
 * happen now (1024 prims), but "should not" is what the 256-prim band said.
 */
internal u16 PacketDepth_UiStamp(u32 uiOrdinal)
{
#if defined(__3DS__)
	/*
	 * ORDERED UI PASS: every UI primitive gets the SAME stamp, the nearest
	 * value in the band. UI layering no longer depends on the depth test at
	 * all -- it is routed out of the state bins into one ordered list and
	 * replayed in submission order at the end of the frame (N3DS_DrawUiPass,
	 * native_renderer_3ds.c).
	 */
	(void)uiOrdinal;
	return (u16)0xFFFF;
#else
	u32 slot = uiOrdinal >> PKTDEPTH_UI_ORD_SHIFT;

	if (slot > (u32)(PKTDEPTH_UI_BAND - 1))
	{
		slot = (u32)(PKTDEPTH_UI_BAND - 1);
	}
	return (u16)(PKTDEPTH_UI_BASE + slot);
#endif
}

global_variable u16 s_pendingDepth;
global_variable int s_pendingDepthValid;
/*
 * PER-VERTEX depth. The flat stamp above is the primitive's FARTHEST
 * vertex (maxSz3, retail's own maxDepth convention -- correct for choosing an
 * OT bucket, wrong for a z-buffer). Against the resident track renderer, which
 * gives every vertex real depth and lets the GPU interpolate it across the
 * surface, a flat-at-its-far-corner kart polygon loses to the road it is
 * sitting on -- owner-visible as "the ground keeps getting drawn over Crash",
 * and structural in a racing game, where the ground runs near-parallel to the
 * view for most of the screen most of the time.
 *
 * The per-vertex depths were already being looked up and then discarded by the
 * max. These keep them. Max 4: every caller's xs/ys arrays are [4] (tri, quad,
 * line, point), which PacketDepth_ResolveXY now enforces rather than assumes.
 */
/* 6, not 4: TriangulateQuad expands a 4-corner face into 6 vertices and
 * expands this array alongside it, in place. See its comment. */
global_variable u16 s_pendingDepthPerVert[6];
/* PARALLAX channel twin. Same length, same
 * indices, same TriangulateQuad permutation -- it rides alongside the
 * occlusion array everywhere rather than being derived from it, because for
 * the flag the two genuinely differ and any "recompute it later" scheme would
 * have to re-answer which pose it is in. Equal to s_pendingDepthPerVert for
 * every other primitive in the game. */
global_variable u16 s_pendingDepthPerVertParallax[6];
global_variable int s_pendingDepthCount;

/* Per-vertex-vs-flat accounting -- see NativeGpu_StampEmitOrdinal. */
global_variable u32 s_pktDepthPerVertHits;
global_variable u32 s_pktDepthFlatFallback;
global_variable int s_pktDepthLastAppended;
global_variable int s_pktDepthLastResolved;

/* s45 miss forensics (gated on g_dbg_modelDepthCheck, PC --model-depth-check): up to
 * 4 sampled misses per [PKTDEPTH] window. Per sample: the screen XY of the
 * vertex that missed, how many vertices hit before it, a 3x3 neighbourhood
 * probe of the capture table (nbr>0 = the depth exists a pixel away = rounding
 * mismatch between store and packet; nbr=0 = never stored = producer gap), the
 * OT slot the fallback read, and the depth it handed out (sentinels: 0xFFFE =
 * no content bucket, kept ordinal).
 * Exempt (flag) primitives are misses BY DESIGN and are not sampled. This is
 * the instrument that decides the warp-pad prize burial's remaining unknown. */
#define PKTMISS_SAMPLE_N 8
global_variable u32 s_pktMissSampCount;
global_variable s16 s_pktMissSampX[PKTMISS_SAMPLE_N];
global_variable s16 s_pktMissSampY[PKTMISS_SAMPLE_N];
global_variable u8 s_pktMissSampVerts[PKTMISS_SAMPLE_N];
global_variable u8 s_pktMissSampHits[PKTMISS_SAMPLE_N];
global_variable u8 s_pktMissSampNbr[PKTMISS_SAMPLE_N];
global_variable u16 s_pktMissSampSlot[PKTMISS_SAMPLE_N];
global_variable u16 s_pktMissSampOut[PKTMISS_SAMPLE_N];
/* s45 iteration 2: the depth of the first vertex that DID hit (0xFFFF = no
 * hits) -- if stale coordinates make hits coincidental strangers, this reads
 * as the content BEHIND the primitive (the mountain), not the primitive. And
 * the stale-generation probe: age>0 means this exact XY was projected age
 * frames ago -- the smoking gun for reused/stale packet coordinates -- with
 * the depth it carried then. Spread cap: at most 2 samples per frame so one
 * frame's first misses cannot monopolise the window. */
global_variable u16 s_pktMissSampHitZ[PKTMISS_SAMPLE_N];
global_variable u16 s_pktMissSampStaleZ[PKTMISS_SAMPLE_N];
global_variable u8 s_pktMissSampStaleAge[PKTMISS_SAMPLE_N]; /* 0 = no stale entry; clamped 255 */
global_variable u32 s_pktMissSampFrameStamp;
global_variable u32 s_pktMissSampThisFrame;
/* Iteration 4: the missing coordinate's hash-slot occupant (see
 * GTECapture_ProbeSlot). occGen 2 + a different occXY = a live clobberer. */
global_variable s16 s_pktMissSampOccX[PKTMISS_SAMPLE_N];
global_variable s16 s_pktMissSampOccY[PKTMISS_SAMPLE_N];
global_variable u16 s_pktMissSampOccZ[PKTMISS_SAMPLE_N];
global_variable u8 s_pktMissSampOccGen[PKTMISS_SAMPLE_N];

/* s42d tag: which POLY_FT4 packets are driver-local exhaust smoke, identified
 * by PACKET ADDRESS (unique per frame) -- NOT a code-byte bit. Every bit of a
 * PS1 primitive code is a real GPU flag; s42c set bit 0x01, which is the
 * raw-texture / shade-OFF bit (ProcessFlatPoly: shadeTexOn = (code & 1) == 0),
 * so the smoke drew unshaded at full brightness = the opaque plume. This side
 * table leaves the packet's rendering completely untouched. Double-buffered
 * exactly like GTECapture: Particle.c Marks on the game thread while building
 * the OT, DrawOTag Publishes (read=write) before the walk is posted,
 * MainFrame_ResetDB Clears (flip write) at the frame boundary; the walk (worker
 * thread on 3DS) reads the published buffer. */
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
#define NATIVE_SMOKE_PKT_MAX 512
global_variable uintptr_t s_smokePktAddr[2][NATIVE_SMOKE_PKT_MAX];
global_variable u32 s_smokePktN[2];
global_variable int s_smokePktWriteIdx = 0;
global_variable int s_smokePktReadIdx = 0;
global_variable u32 s_smokePktDropped = 0;

void NativeGpu_MarkSmokePacket(uintptr_t addr)
{
	u32 n = s_smokePktN[s_smokePktWriteIdx];

	if (n < NATIVE_SMOKE_PKT_MAX)
	{
		s_smokePktAddr[s_smokePktWriteIdx][n] = addr;
		s_smokePktN[s_smokePktWriteIdx] = n + 1;
	}
	else
	{
		s_smokePktDropped++;
	}
}

void NativeGpu_PublishSmokePackets(void)
{
	s_smokePktReadIdx = s_smokePktWriteIdx;
}

void NativeGpu_ClearSmokePackets(void)
{
	s_smokePktWriteIdx ^= 1;
	s_smokePktN[s_smokePktWriteIdx] = 0;
}

internal int NativeGpu_IsSmokePacket(uintptr_t addr)
{
	const uintptr_t *a = s_smokePktAddr[s_smokePktReadIdx];
	u32 n = s_smokePktN[s_smokePktReadIdx];
	u32 i;

	for (i = 0; i < n; i++)
	{
		if (a[i] == addr)
		{
			return 1;
		}
	}
	return 0;
}

/*
 * Skybox packet range. DrawSky_Full's and CAM_SkyboxGlow's packets, identified
 * by their contiguous ADDRESS RANGE in primMem for this frame -- they emit
 * back-to-back with nothing interleaved, so [min start, max end) is exactly
 * the backdrop. The sky has no real depth (GTE translation zeroed) and its
 * stores are suppressed, so pinning it to the absolute far plane (stamp 0)
 * makes it lose to every real surface by construction, matching retail's
 * paint-first behavior.
 */
global_variable uintptr_t s_skyRangeStart[2];
global_variable uintptr_t s_skyRangeEnd[2];
global_variable int s_skyRangeWriteIdx = 0;
global_variable int s_skyRangeReadIdx = 0;

/* s46: CAM_SkyboxGlow's slice of the sky span, tracked separately (same
 * write/read indices as the sky pair). The glow is SCREEN-SPACE geometry
 * hardcoded to the retail 512-wide rect by the ASM-verified emitter, so the
 * REAL-wide window shows bare backdrop in the extra columns; the 3DS emit
 * path stretches exactly these prims to the ortho edges
 * (NativeWide_GlowStretchX). The dome must stay out of this span: it is
 * projected 3D geometry that already reaches the wide margins on its own
 * (DrawSky_IsVisibleWideMargin / the extra yaw sectors). */
global_variable uintptr_t s_glowRangeStart[2];
global_variable uintptr_t s_glowRangeEnd[2];

/* RenderStars' star tiles: sky-class content that must NOT join the union
 * above. Stars emit at the top of the render frame, the whole world away from
 * the backdrop pair, so folding them into [min start, max end) would swallow
 * every packet in between. Own span, same write/read indices and lifecycle.
 * Their captured depth is real but wrong for occlusion: each star is a
 * projected unit direction vector -- camera-adjacent -- while retail hides
 * stars behind the world purely by OT slot, so only the far pin reproduces
 * that. */
global_variable uintptr_t s_starRangeStart[2];
global_variable uintptr_t s_starRangeEnd[2];

/* The character-select diorama backdrop (MM_Characters' RwdBlueRect strips --
 * sky over ground, drawn into the preview window's farthest OT slot). They
 * are synthesized screen-space quads: never projected, so no capture exists,
 * and a menu walk records no OT buckets either, which leaves the resolver's
 * miss exit handing them their raw emit ordinal. That is not survivable
 * here -- the menu checkered flag resolves as a miss too (RaceFlag_DrawSelf
 * marks its packets exempt so they keep retail's painter layering), so it
 * shares that same band and covers them. The span exists to give these
 * primitives a stated depth instead: PKTDEPTH_MENU_BACKDROP, between the
 * flag's band and the preview model's real stamp. NOT sky-class -- the far
 * pin loses to the flag.
 *
 * OVERWRITE semantics, not a union: panel prims emit between the per-player
 * marks in multi-player character select, so a union would swallow them,
 * while overwriting pins only the last player marked -- exact for 1P, the
 * only character select the 3DS menu reaches. Inert on PC (no depth test). */
global_variable uintptr_t s_menubdRangeStart[2];
global_variable uintptr_t s_menubdRangeEnd[2];

/* The in-race minimap block (UI_RenderFrame_Racing: map sheet + racer/ghost/
 * warpball icons, emitted back-to-back with nothing interleaved). Consumed by
 * the resolver's suppressed-2D leg to restamp those primitives
 * PKTDEPTH_UI_STAMP_BTMMAP for the 3DS bottom-screen map. Own span, same
 * write/read indices and lifecycle as the sky pair. */
global_variable uintptr_t s_uimapRangeStart[2];
global_variable uintptr_t s_uimapRangeEnd[2];
/* Nonzero when the frame's map span came from the game's own draw-on-top
 * condition rather than the RouteActive override -- the 3DS copies such a
 * span to both screens instead of moving it. Rides the span's own
 * double-buffer lifecycle so it can never describe a stale range. */
global_variable int s_uimapRangeRetailTop[2];
/* FPS-overlay sub-span of the map block (drawn between the map marks), so the
 * bottom pass can give the text its own non-magnifying window. */
global_variable uintptr_t s_uifpsRangeStart[2];
global_variable uintptr_t s_uifpsRangeEnd[2];
/* Picture-sprite span (PKTDEPTH_UI_STAMP_UIPIC): 2D sprites that want the
 * far-edge UV inset but never route anywhere -- the track-select and
 * adventure-hub map sheets, which show the same half-quad seam the in-race
 * map did. Overwrite semantics like the map span, NOT a union: the two
 * producers are mutually exclusive per frame, and a union of distant marks
 * would swallow unrelated prims between them -- a swallowed 1:1 font glyph
 * would get the inset's gradient change and duplicate rows. */
global_variable uintptr_t s_uipicRangeStart[2];
global_variable uintptr_t s_uipicRangeEnd[2];
/* Juiced-wumpa HUD shine spans (PKTDEPTH_UI_STAMP_HUDSHINE): one per player
 * per frame, kept as separate entries -- split-screen HUDs interleave
 * world-anchored primitives (lock-on reticle, head arrows) between two
 * players' shines, so a union would swallow them. */
#define HUDSHINE_SPANS_MAX 4
global_variable uintptr_t s_hudshineRangeStart[2][HUDSHINE_SPANS_MAX];
global_variable uintptr_t s_hudshineRangeEnd[2][HUDSHINE_SPANS_MAX];
global_variable int s_hudshineRangeCount[2];
/* Full-window fade spans (PKTDEPTH_UI_STAMP_SCREENFADE): one per pushbuffer
 * per frame -- up to four players plus the UI window -- kept as separate
 * entries for the shine's reason: a union of distant marks would swallow the
 * world content between them. */
#define SCREENFADE_SPANS_MAX 5
global_variable uintptr_t s_screenfadeRangeStart[2][SCREENFADE_SPANS_MAX];
global_variable uintptr_t s_screenfadeRangeEnd[2][SCREENFADE_SPANS_MAX];
#ifdef NATIVE_WALK_CENSUS
/* Producer cursor marks, indexed by NativeWalkCensusStage, END inclusive. */
global_variable uintptr_t s_wcMark[2][WCEN_END + 1];

void NativeGpu_WalkCensusMark(int stage, const void *cursor)
{
	if ((stage >= 0) && (stage <= WCEN_END))
	{
		s_wcMark[s_skyRangeWriteIdx][stage] = (uintptr_t)cursor;
	}
}
#endif
global_variable int s_screenfadeRangeCount[2];

void NativeGpu_MarkSkyRange(uintptr_t start, uintptr_t end)
{
	/* UNION across the frame's backdrop producers. Two register their spans:
	 * DrawSky_Full (the dome mesh, 1P only) and CAM_SkyboxGlow (the horizon
	 * gradient, once per player). They emit back-to-back into the same primMem
	 * with no non-sky primitive interleaved -- the track is drawn before them
	 * and the sky is the last world content of the frame -- so [min start, max
	 * end) is exactly the backdrop and nothing else. (0,0) is the cleared-empty
	 * sentinel; a real primMem address is never 0, so the first mark seeds it. */
	if (end <= start)
	{
		return; /* empty span (e.g. DrawSky_Full with skybox==NULL) */
	}
	if (s_skyRangeEnd[s_skyRangeWriteIdx] == 0)
	{
		s_skyRangeStart[s_skyRangeWriteIdx] = start;
		s_skyRangeEnd[s_skyRangeWriteIdx] = end;
		return;
	}
	if (start < s_skyRangeStart[s_skyRangeWriteIdx])
	{
		s_skyRangeStart[s_skyRangeWriteIdx] = start;
	}
	if (end > s_skyRangeEnd[s_skyRangeWriteIdx])
	{
		s_skyRangeEnd[s_skyRangeWriteIdx] = end;
	}
}

/*
 * Sky dome on the GPU (3DS). DrawSky_Full hands its skybox and camera to
 * NativeGpu_SkyGpuSubmit instead of projecting every face through the software
 * GTE and writing packets for the walk. The submit keeps exactly the faces
 * retail's per-face GTE flag test accepts, copied as model-space vertices, and
 * N3DS_DrawSkyDomeGpu draws them through the model program with the camera's
 * rotation (no translation: the sky sits at infinity). The screen-bounds test
 * and the yaw-sector window are left to the GPU's clipper, which keeps a
 * superset of retail's faces and also covers the wide window's margins.
 *
 * Slots follow the sky range's double buffer: written under
 * s_skyRangeWriteIdx, drawn from s_skyRangeReadIdx. The draw consumes its slot,
 * so an EndScene with no walk in front of it cannot redraw a stale dome. The
 * vertices are copied at submit time because the level data can be replaced
 * before the walk's EndScene runs.
 */
int g_dbg_skyGpu = 1;

#if defined(__3DS__) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER
#define NATIVE_SKY_GPU 1
#define SKYGPU_VERT_CAP 3072
#define SKYGPU_SRC_VERT_CAP 2048

typedef struct
{
	int submitted;
	int vertCount;
	s16 rot[3][3];
	s32 h, ofx, ofy;
	N3DS_Vertex verts[SKYGPU_VERT_CAP];
} NativeSkyGpuSlot;

global_variable NativeSkyGpuSlot s_skyGpuSlot[2];
global_variable u8 s_skyGpuVertOk[SKYGPU_SRC_VERT_CAP];

internal void NativeGpu_SkyGpuPutVertex(N3DS_Vertex *dst, const struct ShortVertex *sv)
{
	dst->x = sv->Position.vx;
	dst->y = sv->Position.vy;
	dst->z = sv->Position.vz;
	dst->w = 0;
	dst->u = 0;
	dst->v = 0;
	dst->r = sv->Color.r;
	dst->g = sv->Color.g;
	dst->b = sv->Color.b;
	dst->_pad = 0;
}

int NativeGpu_SkyGpuSubmit(struct Skybox *sky, struct PushBuffer *pb)
{
	NativeSkyGpuSlot *slot = &s_skyGpuSlot[s_skyRangeWriteIdx];
	struct GameTracker *gGT = sdata->gGT;
	const char *vertBase;
	u32 vertLimit;
	s32 h;
	int numVertex, totalFaces = 0, count = 0, seg, i;

	if ((g_dbg_skyGpu == 0) || (sky == NULL) || (pb == NULL) || slot->submitted)
	{
		return 0;
	}
	/* One view: split-screen windows each own a projection the single slot
	 * cannot describe. */
	if ((gGT == NULL) || (gGT->numPlyrCurrGame != 1) || (pb != &gGT->pushBuffer[0]))
	{
		return 0;
	}

	h = pb->distanceToScreen_PREV;
	numVertex = sky->numVertex;
	if ((h <= 0) || (numVertex <= 0) || (numVertex > SKYGPU_SRC_VERT_CAP) || (sky->ptrVertex == NULL))
	{
		return 0;
	}
	for (seg = 0; seg < NUM_SKYBOX_SEGMENTS; seg++)
	{
		totalFaces += (u16)sky->numFaces[seg];
	}
	if ((totalFaces * 3) > SKYGPU_VERT_CAP)
	{
		return 0;
	}

	/* Per-vertex half of retail's face test (DrawSky_IsVisible's GTE flag
	 * bits): RTPT flags SZ3 saturation (view z below 0 or above 0xFFFF) and
	 * divide overflow (H >= 2*SZ3). Translation is zero, so view z is row 2 of
	 * the rotation alone, with RTPT's sf=1 shift. Every kept vertex therefore
	 * sits more than H/2 in front of the eye, so no kept face reaches the w=0
	 * plane the GPU cannot clip against cleanly. */
	{
		const s64 r20 = pb->matrix_ViewProj.m[2][0];
		const s64 r21 = pb->matrix_ViewProj.m[2][1];
		const s64 r22 = pb->matrix_ViewProj.m[2][2];
		const struct ShortVertex *sv = sky->ptrVertex;

		for (i = 0; i < numVertex; i++, sv++)
		{
			const s64 sz = ((r20 * sv->Position.vx) + (r21 * sv->Position.vy) + (r22 * sv->Position.vz)) >> 12;

			s_skyGpuVertOk[i] = (u8)((sz <= 0xFFFF) && ((2 * sz) > (s64)h));
		}
	}

	vertBase = (const char *)sky->ptrVertex;
	vertLimit = (u32)numVertex * (u32)sizeof(struct ShortVertex);
	for (seg = 0; seg < NUM_SKYBOX_SEGMENTS; seg++)
	{
		const struct SkyboxFace *face = sky->ptrFaces[seg];
		const int numFaces = (u16)sky->numFaces[seg];
		int f;

		if (face == NULL)
		{
			continue;
		}
		for (f = 0; f < numFaces; f++, face++)
		{
			const u32 a = face->A, b = face->B, c = face->C;

			if ((a >= vertLimit) || (b >= vertLimit) || (c >= vertLimit))
			{
				continue;
			}
			if (!s_skyGpuVertOk[a / sizeof(struct ShortVertex)] || !s_skyGpuVertOk[b / sizeof(struct ShortVertex)] ||
			    !s_skyGpuVertOk[c / sizeof(struct ShortVertex)])
			{
				continue;
			}
			NativeGpu_SkyGpuPutVertex(&slot->verts[count + 0], (const struct ShortVertex *)(const void *)(vertBase + a));
			NativeGpu_SkyGpuPutVertex(&slot->verts[count + 1], (const struct ShortVertex *)(const void *)(vertBase + b));
			NativeGpu_SkyGpuPutVertex(&slot->verts[count + 2], (const struct ShortVertex *)(const void *)(vertBase + c));
			count += 3;
		}
	}

	for (i = 0; i < 3; i++)
	{
		slot->rot[i][0] = pb->matrix_ViewProj.m[i][0];
		slot->rot[i][1] = pb->matrix_ViewProj.m[i][1];
		slot->rot[i][2] = pb->matrix_ViewProj.m[i][2];
	}
	slot->h = h;
	slot->ofx = (pb->rect.w >> 1) + pb->rect.x;
	slot->ofy = (pb->rect.h >> 1) + pb->rect.y;
	slot->vertCount = count;
	slot->submitted = 1;
	return 1;
}
#else
int NativeGpu_SkyGpuSubmit(struct Skybox *sky, struct PushBuffer *pb)
{
	(void)sky;
	(void)pb;
	return 0;
}
#endif

/* s46: the glow marks through here -- it lands in BOTH spans, so every sky
 * consumer (far pin, sky-first routing) keeps seeing the union while the
 * stretch can target the glow alone. */
void NativeGpu_MarkGlowRange(uintptr_t start, uintptr_t end)
{
	if (end <= start)
	{
		return;
	}
	if (s_glowRangeEnd[s_skyRangeWriteIdx] == 0)
	{
		s_glowRangeStart[s_skyRangeWriteIdx] = start;
		s_glowRangeEnd[s_skyRangeWriteIdx] = end;
	}
	else
	{
		if (start < s_glowRangeStart[s_skyRangeWriteIdx])
		{
			s_glowRangeStart[s_skyRangeWriteIdx] = start;
		}
		if (end > s_glowRangeEnd[s_skyRangeWriteIdx])
		{
			s_glowRangeEnd[s_skyRangeWriteIdx] = end;
		}
	}
	NativeGpu_MarkSkyRange(start, end);
}

void NativeGpu_MarkStarRange(uintptr_t start, uintptr_t end)
{
	if (end <= start)
	{
		return;
	}
	if (s_starRangeEnd[s_skyRangeWriteIdx] == 0)
	{
		s_starRangeStart[s_skyRangeWriteIdx] = start;
		s_starRangeEnd[s_skyRangeWriteIdx] = end;
	}
	else
	{
		if (start < s_starRangeStart[s_skyRangeWriteIdx])
		{
			s_starRangeStart[s_skyRangeWriteIdx] = start;
		}
		if (end > s_starRangeEnd[s_skyRangeWriteIdx])
		{
			s_starRangeEnd[s_skyRangeWriteIdx] = end;
		}
	}
}

void NativeGpu_MarkMenuBackdropRange(uintptr_t start, uintptr_t end)
{
	if (end <= start)
	{
		return;
	}
	/* Overwrite, no union: see the span comment. */
	s_menubdRangeStart[s_skyRangeWriteIdx] = start;
	s_menubdRangeEnd[s_skyRangeWriteIdx] = end;
}

void NativeGpu_MarkUiMapRange(uintptr_t start, uintptr_t end, int retailTop)
{
	if (end <= start)
	{
		return; /* map block skipped this frame (menus, 2P/4P, speedo mode) */
	}
	/* One producer, one contiguous block: overwrite, no union. */
	s_uimapRangeStart[s_skyRangeWriteIdx] = start;
	s_uimapRangeEnd[s_skyRangeWriteIdx] = end;
	s_uimapRangeRetailTop[s_skyRangeWriteIdx] = retailTop;
}

/* Read on the walk, published index -- same cadence as the stamps it
 * qualifies. Zero whenever the frame carried no map span. */
int NativeGpu_UiMapRetailTop(void)
{
	return s_uimapRangeRetailTop[s_skyRangeReadIdx];
}

void NativeGpu_MarkUiFpsRange(uintptr_t start, uintptr_t end)
{
	if (end <= start)
	{
		return;
	}
	s_uifpsRangeStart[s_skyRangeWriteIdx] = start;
	s_uifpsRangeEnd[s_skyRangeWriteIdx] = end;
}

void NativeGpu_MarkUiPicRange(uintptr_t start, uintptr_t end)
{
	if (end <= start)
	{
		return;
	}
	s_uipicRangeStart[s_skyRangeWriteIdx] = start;
	s_uipicRangeEnd[s_skyRangeWriteIdx] = end;
}

void NativeGpu_MarkHudShineRange(uintptr_t start, uintptr_t end)
{
	int n = s_hudshineRangeCount[s_skyRangeWriteIdx];

	if ((end <= start) || (n >= HUDSHINE_SPANS_MAX))
	{
		return;
	}
	s_hudshineRangeStart[s_skyRangeWriteIdx][n] = start;
	s_hudshineRangeEnd[s_skyRangeWriteIdx][n] = end;
	s_hudshineRangeCount[s_skyRangeWriteIdx] = n + 1;
}

void NativeGpu_MarkScreenFadeRange(uintptr_t start, uintptr_t end)
{
	int n = s_screenfadeRangeCount[s_skyRangeWriteIdx];

	if ((end <= start) || (n >= SCREENFADE_SPANS_MAX))
	{
		return;
	}
	s_screenfadeRangeStart[s_skyRangeWriteIdx][n] = start;
	s_screenfadeRangeEnd[s_skyRangeWriteIdx][n] = end;
	s_screenfadeRangeCount[s_skyRangeWriteIdx] = n + 1;
}

void NativeGpu_PublishSkyRange(void)
{
	s_skyRangeReadIdx = s_skyRangeWriteIdx;
}

void NativeGpu_ClearSkyRange(void)
{
	s_skyRangeWriteIdx ^= 1;
	s_skyRangeStart[s_skyRangeWriteIdx] = 0;
	s_skyRangeEnd[s_skyRangeWriteIdx] = 0;
	s_glowRangeStart[s_skyRangeWriteIdx] = 0;
	s_glowRangeEnd[s_skyRangeWriteIdx] = 0;
	s_starRangeStart[s_skyRangeWriteIdx] = 0;
	s_starRangeEnd[s_skyRangeWriteIdx] = 0;
	s_menubdRangeStart[s_skyRangeWriteIdx] = 0;
	s_menubdRangeEnd[s_skyRangeWriteIdx] = 0;
	s_uimapRangeStart[s_skyRangeWriteIdx] = 0;
	s_uimapRangeEnd[s_skyRangeWriteIdx] = 0;
	s_uimapRangeRetailTop[s_skyRangeWriteIdx] = 0;
	s_uifpsRangeStart[s_skyRangeWriteIdx] = 0;
	s_uifpsRangeEnd[s_skyRangeWriteIdx] = 0;
	s_uipicRangeStart[s_skyRangeWriteIdx] = 0;
	s_uipicRangeEnd[s_skyRangeWriteIdx] = 0;
	s_hudshineRangeCount[s_skyRangeWriteIdx] = 0;
	s_screenfadeRangeCount[s_skyRangeWriteIdx] = 0;
#ifdef NATIVE_WALK_CENSUS
	memset(s_wcMark[s_skyRangeWriteIdx], 0, sizeof(s_wcMark[0]));
#endif
#ifdef NATIVE_SKY_GPU
	s_skyGpuSlot[s_skyRangeWriteIdx].submitted = 0;
	s_skyGpuSlot[s_skyRangeWriteIdx].vertCount = 0;
#endif
}

internal int NativeGpu_IsSkyPacket(uintptr_t addr)
{
	return (addr >= s_skyRangeStart[s_skyRangeReadIdx]) && (addr < s_skyRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsGlowPacket(uintptr_t addr)
{
	return (addr >= s_glowRangeStart[s_skyRangeReadIdx]) && (addr < s_glowRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsStarPacket(uintptr_t addr)
{
	return (addr >= s_starRangeStart[s_skyRangeReadIdx]) && (addr < s_starRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsMenuBackdropPacket(uintptr_t addr)
{
	return (addr >= s_menubdRangeStart[s_skyRangeReadIdx]) && (addr < s_menubdRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsUiMapPacket(uintptr_t addr)
{
	return (addr >= s_uimapRangeStart[s_skyRangeReadIdx]) && (addr < s_uimapRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsUiFpsPacket(uintptr_t addr)
{
	return (addr >= s_uifpsRangeStart[s_skyRangeReadIdx]) && (addr < s_uifpsRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsUiPicPacket(uintptr_t addr)
{
	return (addr >= s_uipicRangeStart[s_skyRangeReadIdx]) && (addr < s_uipicRangeEnd[s_skyRangeReadIdx]);
}

internal int NativeGpu_IsHudShinePacket(uintptr_t addr)
{
	int i;

	for (i = 0; i < s_hudshineRangeCount[s_skyRangeReadIdx]; i++)
	{
		if ((addr >= s_hudshineRangeStart[s_skyRangeReadIdx][i]) && (addr < s_hudshineRangeEnd[s_skyRangeReadIdx][i]))
		{
			return 1;
		}
	}
	return 0;
}

internal int NativeGpu_IsScreenFadePacket(uintptr_t addr)
{
	int i;

	for (i = 0; i < s_screenfadeRangeCount[s_skyRangeReadIdx]; i++)
	{
		if ((addr >= s_screenfadeRangeStart[s_skyRangeReadIdx][i]) && (addr < s_screenfadeRangeEnd[s_skyRangeReadIdx][i]))
		{
			return 1;
		}
	}
	return 0;
}

#endif

internal u16 PacketDepth_ClampWorld(u32 stamp)
{
	int biased = (int)stamp + g_dbg_pktDepthBias;

	/* Signed intermediate on purpose: a negative bias must saturate at 0
	 * rather than wrap through 0xFFFF into "nearest", which is the exact
	 * failure this whole session started with. */
	if (biased < 0)
	{
		biased = 0;
	}
	if (biased > (int)PKTDEPTH_WORLD_MAX)
	{
		biased = (int)PKTDEPTH_WORLD_MAX;
	}
	return (u16)biased;
}

/* Live fallback rate (design 4). Distinct from [SZPROBE], which is a PC
 * instrument behind its own switch: this one counts whenever the feature is on,
 * including on hardware, because "97.09% of primitives" was measured on PC
 * content and hardware content is allowed to disagree. If it does, that must
 * arrive as a number, not as an owner noticing something looks wrong. */
global_variable struct
{
	u32 real, fellBack, suppressed2d, frames;
	/* Actual stamp EXTENTS per class. The counts above say how often each path
	 * fired; these say WHERE on the axis it landed, which is the only thing
	 * that explains content vanishing. Larger stamp = nearer (n3ds_backend.c's
	 * empirically-settled polarity: Mtx_OrthoTilt maps larger vertex z to
	 * smaller stored depth, LEQUAL, clear-to-max). Compare against the track's
	 * own zOrd range in [TRKDEPTH0]: if the two do not overlap, one source is
	 * simply in front of the other everywhere and no amount of per-primitive
	 * correctness will help. */
	u16 realMin, realMax;
	u16 backMin, backMax;
	u16 uiMin, uiMax;
} s_pktDepth;

/*
 * PROVENANCE SPLIT -- [PKTDEPTH2]. Frame-wide totals cannot tell whether a
 * wrong depth belongs to the road or the kart, so these counts are split by
 * which object a resolved depth came from (camera matrix = world, other
 * matrix = per-object instance). Two mechanisms are distinguished: EVICTION
 * (a kart vertex and the road vertex under it share a screen coordinate,
 * last writer wins -- signature: 'mixed' nonzero) and FALLBACK QUANTISATION
 * (a miss reconstructs depth with the track's >>6 ruler, wrong scale for
 * model instances -- signature: mdlFellBack large).
 */
global_variable u8 s_pktDepthDomMtx;

global_variable struct
{
	u32 wldReal, wldFellBack;   /* resolved / missed, camera-matrix provenance */
	u32 mdlReal, mdlFellBack;   /* resolved / missed, instance-matrix provenance */
	u32 unkFellBack;            /* missed on vertex 0: no provenance evidence at all */
	u32 mixed;                  /* vertices of one primitive owned by two objects */
	/* Per-vertex vs flat-at-farthest-corner, split the same way. The existing
	 * [PKTDEPTH] perVert/flat pair is frame-wide and therefore cannot say
	 * whether a kart is in the flat group -- which is the only question left. */
	u32 mdlPerVert, mdlFlat;
	u32 wldPerVert, wldFlat;
	u32 mdlFlatMaxSpread;       /* worst (maxSz3-minSz3) a flat model prim ate */
	int mdlFlatLastAppended;    /* the count mismatch that sent it there, sampled */
	int mdlFlatLastResolved;
	/*
	 * SZ3 EXTENTS PER POPULATION -- in the same view.z units [TRKDEPTHCHK]
	 * prints, so the kart's depth and the road's depth can finally be read off
	 * one line and compared. [PKTDEPTHZ] gives the whole packet population's
	 * range, which mixes karts with scenery and has therefore never answered
	 * "is the kart at the same distance as the ground under it".
	 */
	u32 mdlSzLo, mdlSzHi;
	u32 wldSzLo, wldSzHi;
} s_pktDepth2;

global_variable int s_pktDepth2Seeded;

/* Reset helper: the two lo fields need a sentinel a real sz3 can beat, which a
 * plain memset cannot provide. Every reset site must go through this. */
internal void PacketDepth2_Reset(void)
{
	memset(&s_pktDepth2, 0, sizeof(s_pktDepth2));
	s_pktDepth2.mdlSzLo = 0xFFFFFFFFu;
	s_pktDepth2.wldSzLo = 0xFFFFFFFFu;
}

/* Provenance and depth spread of the primitive currently in the pending slot,
 * carried from PacketDepth_ResolveXY to NativeGpu_StampEmitOrdinal because the
 * per-vertex/flat decision is made there and has no other way to know what it
 * is looking at. Consumed alongside s_pendingDepthValid. */
global_variable int s_pendingDepthIsModel;
global_variable u32 s_pendingDepthSpread;

/*
 * Partial-hit fill. The capture table is direct-mapped, so a later same-frame
 * projection can steal any corner's slot; the resolve loop then fails the
 * WHOLE primitive on that one corner and the no-bucket exit leaves it on the
 * raw emit ordinal -- z near the far pin, buried behind everything (the
 * invisible-tire blink: a sprite quad's 4 corners all resolve or none do).
 * When the resolved corners agree on a matrix, their own max depth is the
 * primitive's depth to within its own thickness -- strictly better than either
 * fallback.
 *
 * Fills only on an honest partial hit: at most one corner missing, OR >= 2
 * hits that agree in depth to within PKTDEPTH_PARTIAL_SPREAD_MAX -- one
 * object's corners agree, strangers at screen-quad distances do not. Filling
 * on ANY hit is too loose: a prim whose corners were NEVER stored
 * (synthesized 2D quads; the char-select gradient) can catch a STRANGER's
 * entry at a coincident pixel and surface for just that frame.
 */
#define PKTDEPTH_PARTIAL_SPREAD_MAX 64
global_variable u32 s_pktDepthPartialFilled;

internal void PacketDepth_NoteExtent(u16 stamp, u16 *lo, u16 *hi)
{
	if ((*lo == 0) && (*hi == 0))
	{
		*lo = stamp;
		*hi = stamp;
		return;
	}
	if (stamp < *lo)
	{
		*lo = stamp;
	}
	if (stamp > *hi)
	{
		*hi = stamp;
	}
}

/*
 * PARALLAX-channel fill for flag-range primitives (channel split).
 *
 * These primitives resolved their OCCLUSION above as if their captures did not
 * exist -- that is the point, it hands layering back to retail's OT order --
 * so the stereo depth has to come from a second, independent read of the very
 * entries the occlusion pass ignored. The values there are the store-side
 * remap's output: the flag's true wave relief, compressed to ~1/4 and anchored
 * just behind the convergence plane, identical in every pose.
 *
 * A miss leaves the caller's occlusion stamp in that slot. That is deliberate
 * and it is the safe direction: one flat vertex inside an otherwise rippling
 * face is invisible, whereas a fabricated depth for it would tear the face
 * across two stereo planes.
 */
internal void PacketDepth_FillParallaxFromCapture(const s16 *xs, const s16 *ys, int n, u16 *perVertPar)
{
	int i;

	for (i = 0; i < n; i++)
	{
		u16 sz3;
		u8 mtxId;

		if (GTECapture_LookupDepthEx(xs[i], ys[i], &sz3, &mtxId))
		{
			perVertPar[i] = PacketDepth_ClampWorld(0xFFFFu - sz3); /* same SENSE INVERSION as the occlusion path */
		}
	}
}

/* Shared core. BOTH emit paths must call this: the parse loop's MakeVertex*
 * sites AND native_direct_emit.c, which stamps its own z and handles most of a
 * heavy track's opaque content on 3DS. If only one of them resolves depth, the
 * two populations sit on different axes -- the same defect as the waterfalls,
 * just split by code path instead of by content type, and therefore much harder
 * to see in a screenshot.
 *
 * perVertPar is the PARALLAX channel: it
 * receives a copy of the occlusion depths for every primitive in the game
 * except the checkered flag's, which get their remapped ripple band instead.
 * Callers that cannot carry a second channel pass NULL and lose nothing.
 *
 * Returns 1 and writes *out whenever the feature is on (covered, miss and 2D
 * all produce a stamp); returns 0 only when the feature is off, which is the
 * caller's signal to keep its existing ordinal. */
internal int PacketDepth_ResolveXY(const s16 *xs, const s16 *ys, int n, u16 *out, u16 *perVert, u16 *perVertPar)
{
	u16 sz3s[4];
	u16 maxSz3 = 0;
	u16 minSz3 = 0xFFFFu;
	int i;
	/* Provenance: gathered as the loop runs rather than in a second pass,
	 * because a miss exits early and the vertices resolved BEFORE it are the
	 * only evidence of who the primitive belonged to. Scalars on purpose -- an
	 * array indexed by i would need its own bound, and n is only guaranteed
	 * <= 4 on the perVert path. */
	u8 firstMtx = 0;
	int hits = 0;
	int mixed = 0;
	int missed = 0;
	int firstMissIdx = -1;
	/* Is this primitive part of the checkered flag's registered prim
	 * range? Latched once here rather than re-queried per branch -- it decides
	 * BOTH that the occlusion pass must ignore the captures and that the
	 * parallax pass must read them. */
	int exempt;

	if (!g_dbg_packetDepth)
	{
		return 0;
	}

	exempt = GTECapture_NodeExempt();

	/* perVert is only meaningful for the vertex counts the emit paths actually
	 * append one-for-one; anything else falls back to the flat stamp rather
	 * than writing past sz3s[]. Both channels share the rule: they are indexed
	 * in lockstep and a mismatch would pair a vertex with a stranger's depth. */
	if (n > 4)
	{
		perVert = NULL;
		perVertPar = NULL;
	}

	if (s_skyNodeActive)
	{
		/*
		 * SKYBOX -> absolute far plane (block above NativeGpu_MarkSkyRange).
		 * Taken before the suppressed/exempt/miss machinery below: the sky has no
		 * real depth to resolve, and the OT-bucket fallback those paths would give
		 * it is not guaranteed to lose to distant real geometry -- the waterfall
		 * punch-through. Stamp 0 (farthest on the nearer=larger axis) directly,
		 * NOT through PacketDepth_ClampWorld, so g_dbg_pktDepthBias cannot nudge
		 * the sky forward off the far plane. Flat across the primitive: a backdrop
		 * at infinity has no per-vertex relief. The sky is opaque, so it still
		 * writes depth -- harmless at 0, since nothing is farther.
		 */
		*out = 0;
		if (perVert)
		{
			for (i = 0; i < n; i++)
			{
				perVert[i] = 0;
			}
		}
		if (perVertPar)
		{
			for (i = 0; i < n; i++)
			{
				perVertPar[i] = 0;
			}
		}
		s_pendingDepthIsModel = 0;
		s_pendingDepthSpread = 0;
		return 1;
	}

	if (s_menubdNodeActive)
	{
		/*
		 * MENU BACKDROP (character-select diorama strips). Same shape as the
		 * sky leg above and taken for the same reason: no captured depth
		 * exists, and none of the fallbacks below can supply one -- a menu
		 * walk records no OT buckets, so the miss exit would hand these back
		 * their raw emit ordinal and the flag would bury them. Flat across
		 * the primitive; a backdrop panel has no per-vertex relief.
		 */
		/*
		 * PARALLAX = the UI band, so the shader's band mask flattens these to
		 * the screen plane -- the SAME treatment the window border around them
		 * gets, because that border is UI. Anything else and the two separate
		 * horizontally as the 3D slider comes up: the border sits at zero
		 * disparity while the strips carry their occlusion depth's parallax,
		 * and the backdrop slides out of its own frame. Band, not a depth near
		 * the convergence plane, so the two stay locked if `stereoconv` moves.
		 * Same idiom as the paint-on-top flat-overlay flavor below.
		 */
		const u16 par = PKTDEPTH_UI_BASE;

		*out = PKTDEPTH_MENU_BACKDROP;
		if (perVert)
		{
			for (i = 0; i < n; i++)
			{
				perVert[i] = PKTDEPTH_MENU_BACKDROP;
			}
		}
		if (perVertPar)
		{
			for (i = 0; i < n; i++)
			{
				perVertPar[i] = par;
			}
		}
		PacketDepth_NoteExtent(*out, &s_pktDepth.realMin, &s_pktDepth.realMax);
		s_pendingDepthIsModel = 0;
		s_pendingDepthSpread = 0;
		return 1;
	}

	/* The HUD shine (UI_Weapon_DrawBG) is 2D UI that retail links into the
	 * player's OT, so the suppress latch never sees it; its span mark takes it
	 * down this leg instead, to the band stamp the fruit and icon it backs
	 * already carry. Without this it lands on the world-bucket fallback below:
	 * correct occlusion, but outside the band, so no widescreen X ramp and no
	 * stereo flatten -- the burst stays at retail X while the fruit moves with
	 * the wide window. The per-player full-window fade takes the same leg for
	 * the same reason: on the world-bucket fallback the track and models
	 * depth-test through the flash. */
	if (GTECapture_LookupSuppressed() || (g_dbg_hudShine && s_hudshineNodeActive) || (g_dbg_screenFade && s_screenfadeNodeActive))
	{
		/* 2D in the UI band: no depth exists, and it must stay on top of every
		 * world stamp. Ordinal ordering is preserved WITHIN the band so UI
		 * layers among itself exactly as it does today -- packed 4 prims per
		 * stamp so the 64-wide band still orders 256 of them. A same-stamp tie
		 * (e.g. a glyph and its drop shadow, 1 ordinal apart) resolves by draw
		 * order under LEQUAL, which is submission order within a merge bin --
		 * and near-adjacent UI prims are near-adjacent state, so that is the
		 * order they replay in. */
		if (exempt)
		{
			/*
			 * THE FLAG DURING A TRANSITION (s40c). One stamp for the whole
			 * sheet, latched at its first primitive, instead of one per quad.
			 *
			 * Correct because the flag is a single non-self-overlapping
			 * surface: it has no internal layering to preserve, so spending
			 * ~250 of the band's 64 stamps on it bought nothing and cost
			 * everything (the wrap above). Retail's own order is reproduced
			 * exactly -- UI walked BEFORE the flag (the race HUD, at OT entry
			 * 1) keeps lower stamps and stays behind it, and UI walked AFTER
			 * (the LOADING glyphs, which share OT entry 0 and sit deeper in
			 * its chain) gets higher stamps and draws over it. That is the
			 * owner's requirement -- during the sweep the flag masks the
			 * outgoing race's UI, characters and world alike -- without
			 * burying the loading text that belongs on top of it.
			 */
			if (!s_pktFlagUiStampValid)
			{
				s_pktFlagUiStamp = PacketDepth_UiStamp(s_pktUiOrdinal);
				s_pktFlagUiStampValid = 1;
				/* Round up to the next stamp boundary so whatever the walk
				 * reaches next is strictly nearer than the whole sheet. */
				s_pktUiOrdinal = (s_pktUiOrdinal | ((1u << PKTDEPTH_UI_ORD_SHIFT) - 1u)) + 1u;
			}
			*out = s_pktFlagUiStamp;
		}
		else
		{
			*out = PacketDepth_UiStamp(s_pktUiOrdinal);
			s_pktUiOrdinal++;
#if defined(__3DS__)
			/* Bottom-screen minimap: the map block's primitives (tagged by
			 * s_uimapNodeActive, same node mechanism as the sky span) get their
			 * own band stamp so the renderer can peel them from the ordered UI
			 * list onto the bottom target. Only while the ordered-UI pin owns
			 * the band (stamp == 0xFFFF). */
			{
				extern int g_dbg_btmMap;

				if (*out == 0xFFFFu)
				{
					/* FPS glyphs route only, so they keep the bottom-screen
					 * gate: unrouted they stay ordinary pinned UI. */
					if (g_dbg_btmMap && s_uifpsNodeActive)
					{
						*out = PKTDEPTH_UI_STAMP_BTMFPS;
					}
					/* The map stamp is NOT gated on the bottom-screen toggle:
					 * besides routing the peel (which carries its own gate), it
					 * keys the far-edge UV inset in N3DS_ConvertTriangleVerts,
					 * which must hold wherever the map draws -- the top screen
					 * shows the same half-texel seam, just less magnified. */
					else if (s_uimapNodeActive)
					{
						*out = PKTDEPTH_UI_STAMP_BTMMAP;
					}
					/* Picture-sprite class (menu/hub map sheets): inset
					 * only, never routed -- the renderer re-pins these to
					 * 0xFFFF at the ordered-UI append so pinned layering
					 * stays EQUAL. */
					else if (s_uipicNodeActive)
					{
						*out = PKTDEPTH_UI_STAMP_UIPIC;
					}
					/* HUD shine: same never-routed, re-pinned treatment as the
					 * picture sprites, plus the scale-aware far-edge inset. */
					else if (s_hudshineNodeActive)
					{
						*out = PKTDEPTH_UI_STAMP_HUDSHINE;
					}
					/* Full-window fade: re-pinned likewise; the stamp keys
					 * the screen-edge overscan. */
					else if (g_dbg_screenFade && s_screenfadeNodeActive)
					{
						*out = PKTDEPTH_UI_STAMP_SCREENFADE;
					}
				}
			}
#endif
		}
		PacketDepth_NoteExtent(*out, &s_pktDepth.uiMin, &s_pktDepth.uiMax);
		s_pktDepth.suppressed2d++;
		s_pendingDepthIsModel = 0;
		s_pendingDepthSpread = 0;
		/* 2D has no per-vertex depth by definition -- flat is correct here. */
		if (perVert)
		{
			for (i = 0; i < n; i++)
			{
				perVert[i] = *out;
			}
		}
		/*
		 * THE WIPE POSE (s40b). Genuine 2D gets parallax = its UI stamp, which
		 * the shader's own band mask then flattens to zero shift -- unchanged.
		 * The checkered flag, however, is linked into this same band during a
		 * screen-covering transition (RaceFlag_GetOT -> swapchain OT entry 0),
		 * and it is NOT 2D: it keeps the UI stamp above so it paints over the
		 * whole world exactly as retail does, and takes its real rippling
		 * depth on the parallax channel so the wave does not go flat for the
		 * one second the wipe is on screen.
		 */
		if (perVertPar)
		{
			for (i = 0; i < n; i++)
			{
				perVertPar[i] = *out;
			}
			if (exempt)
			{
				PacketDepth_FillParallaxFromCapture(xs, ys, n, perVertPar);
			}
		}
		return 1;
	}

	{
		int paintNear = GTECapture_NodePaintNear();

		if (paintNear)
		{
			/*
			 * PAINT-ON-TOP (allowlist and design comment on g_dbg_paintOnTop,
			 * native_gte_capture.h). Occlusion pins to the nearest world
			 * stamp: retail parks these members inside/behind world geometry
			 * and its painter draws them after it, so "wins against all world
			 * content, loses to the UI band" IS retail's layering, translated
			 * to the depth axis. PKTDEPTH_WORLD_MAX sits one below the UI
			 * base, so the HUD still covers the member -- and the widescreen
			 * X ramp (keyed on the OCCLUSION stamp, native_renderer_3ds.c)
			 * stays off, which flat overlay members need: they position
			 * themselves from the world viewport and are already wide-aware.
			 * A member's own faces tie at one stamp and resolve by submission
			 * order under LEQUAL, which is retail command order -- the
			 * flag-sheet argument.
			 *
			 * Parallax splits by flavor: a 3D member (talking mask) keeps its
			 * captured real depth (prefilled with the pin so a miss degrades
			 * flat, the safe direction) so the head stays round in stereo; a
			 * flat overlay (stoplight, "FINAL LAP") takes a UI-band value, so
			 * the shader's band mask gives it the HUD's screen-plane stereo
			 * treatment -- band on the PARALLAX channel only, so the X-ramp
			 * exclusion above still holds.
			 */
			u16 pin = (u16)GTECapture_NodePaintNearPin();

			if ((pin == 0) || (pin > PKTDEPTH_WORLD_MAX))
			{
				pin = PKTDEPTH_WORLD_MAX;
			}
			*out = pin;
			if (perVert)
			{
				for (i = 0; i < n; i++)
				{
					perVert[i] = pin;
				}
			}
			if (perVertPar)
			{
				if (paintNear >= 2)
				{
					for (i = 0; i < n; i++)
					{
						perVertPar[i] = PKTDEPTH_UI_BASE;
					}
				}
				else
				{
					for (i = 0; i < n; i++)
					{
						perVertPar[i] = PKTDEPTH_WORLD_MAX;
					}
					PacketDepth_FillParallaxFromCapture(xs, ys, n, perVertPar);
				}
			}
			s_pendingDepthIsModel = 0;
			s_pendingDepthSpread = 0;
			return 1;
		}
	}

	if (exempt)
	{
		/*
		 * THE MENU POSE (s40b). The flag's captures exist and are perfectly
		 * good -- and the occlusion channel ignores them anyway, resolving as
		 * a miss exactly like the pre-split suppression bracket did. The miss
		 * path below then derives depth from the OT bucket retail put the flag
		 * in, which IS retail's statement of where it belongs: behind the menu
		 * models. The parallax channel reads those same captures a few lines
		 * on, so the wave keeps rippling while the layering stays retail's.
		 *
		 * Skipping the loop rather than running it and discarding the result
		 * is what makes "as if suppressed" exact: hits stays 0, so the
		 * provenance accounting takes the same unkFellBack branch a genuinely
		 * suppressed producer takes, and [PKTDEPTH2] keeps meaning what it
		 * meant before the split.
		 */
		missed = 1;
	}

	/* `!missed` guard: an exempt primitive skips the lookups entirely rather
	 * than running them and discarding -- see the block above for why hits
	 * must stay 0. The scan covers ALL vertices even after a miss (it used to
	 * break at the first one): the partial-hit fill below needs every corner
	 * that DID resolve, and a break would report hits=0 whenever the stolen
	 * slot happened to be vertex 0's. */
	if (!missed)
	{
		for (i = 0; i < n; i++)
		{
			u16 sz3;
			u8 thisMtx;

			if (!GTECapture_LookupDepthEx(xs[i], ys[i], &sz3, &thisMtx))
			{
				missed = 1;
				if (firstMissIdx < 0)
				{
					firstMissIdx = i;
				}
				continue;
			}

			if (hits == 0)
			{
				firstMtx = thisMtx;
			}
			else if (thisMtx != firstMtx)
			{
				mixed = 1;
			}
			hits++;

			if (perVert)
			{
				sz3s[i] = sz3;
			}
			if (sz3 > maxSz3)
			{
				maxSz3 = sz3;
			}
			if (sz3 < minSz3)
			{
				minSz3 = sz3;
			}
		}
	}

	/* Mechanism 1's signature. A primitive cannot legitimately span two matrix
	 * states -- one face is projected under one matrix -- so every 'mixed' is
	 * an eviction that has already handed at least one vertex a stranger's
	 * depth. */
	if (mixed)
	{
		s_pktDepth2.mixed++;
	}

	if (missed)
	{
		/* Whole primitive falls back (design 3.2) -- but onto the SAME
		 * axis (design 3.3), never onto the ordinal. The OT bucket the
		 * walk is currently parsing IS a quantisation of this same view
		 * depth (retail builds it as maxDepth >> 6), so shifting it back
		 * up recovers a coarse but correctly-scaled depth. Without this
		 * the miss population lands at an arbitrary point on the scale --
		 * which is what put the waterfalls through the HUD. */
		s_pktDepth.fellBack++;
		/* Stale provenance must not leak into the stamper's accounting: a
		 * bucket-derived stamp is flat by construction, so its spread is zero
		 * and its population is whatever evidence the hits gave us. */
		s_pendingDepthIsModel = (hits > 0) && (firstMtx != s_pktDepthDomMtx);
		s_pendingDepthSpread = 0;
		if (hits == 0)
		{
			s_pktDepth2.unkFellBack++;
		}
		else if (firstMtx == s_pktDepthDomMtx)
		{
			s_pktDepth2.wldFellBack++;
		}
		else
		{
			s_pktDepth2.mdlFellBack++;
		}

		/* s45 miss forensics -- see the sample array declarations. The missing
		 * vertex is xs/ys at firstMissIdx (exempt pre-sets missed with the
		 * loop never run, and is excluded as a miss by design). The `out`
		 * field is patched at whichever of the exits actually fires, so a
		 * sample records not just where the miss was but which fallback the
		 * primitive really took. */
		const int mi = (firstMissIdx >= 0) ? firstMissIdx : 0;
		int missSampIdx = -1;
		if (s_pktMissSampFrameStamp != s_pktDepth.frames)
		{
			s_pktMissSampFrameStamp = s_pktDepth.frames;
			s_pktMissSampThisFrame = 0;
		}
		/* Sample filter (iteration 3): the first two cuts of this sampler were
		 * monopolised by boot-menu quads at (0,0) and by offscreen n=1 point
		 * prims parked at (-1024,-1024) -- both parse early every frame and ate
		 * the whole window. Only tri/quad prims with plausibly on-screen
		 * coordinates are worth a slot. */
		if (g_dbg_modelDepthCheck && !exempt && (s_pktMissSampCount < PKTMISS_SAMPLE_N) && (s_pktMissSampThisFrame < 2) && (firstMissIdx >= 0) &&
		    (n >= 3) && (xs[mi] > -64) && (xs[mi] < 640) && (ys[mi] > 0) && (ys[mi] < 480))
		{
			int dx, dy, nbr = 0;
			u16 staleZ = 0;
			u32 staleAge = 0;

			for (dy = -1; dy <= 1; dy++)
			{
				for (dx = -1; dx <= 1; dx++)
				{
					u16 nbrSz3;
					u8 nbrMtx;

					if (((dx != 0) || (dy != 0)) && GTECapture_LookupDepthEx((s16)(xs[mi] + dx), (s16)(ys[mi] + dy), &nbrSz3, &nbrMtx))
					{
						nbr++;
					}
				}
			}
			(void)GTECapture_LookupDepthStale(xs[mi], ys[mi], &staleZ, &staleAge);
			{
				s16 occX, occY;
				int occGen;
				u16 occZ;

				(void)GTECapture_ProbeSlot(xs[mi], ys[mi], &occX, &occY, &occGen, &occZ);
				s_pktMissSampOccX[s_pktMissSampCount] = occX;
				s_pktMissSampOccY[s_pktMissSampCount] = occY;
				s_pktMissSampOccZ[s_pktMissSampCount] = occZ;
				s_pktMissSampOccGen[s_pktMissSampCount] = (u8)occGen;
			}
			s_pktMissSampThisFrame++;
			missSampIdx = (int)s_pktMissSampCount++;
			s_pktMissSampX[missSampIdx] = xs[mi];
			s_pktMissSampY[missSampIdx] = ys[mi];
			s_pktMissSampVerts[missSampIdx] = (u8)n;
			s_pktMissSampHits[missSampIdx] = (u8)hits;
			s_pktMissSampNbr[missSampIdx] = (u8)nbr;
			s_pktMissSampSlot[missSampIdx] = (u16)s_gpu.otCurrentSlotIdx;
			s_pktMissSampOut[missSampIdx] = 0xFFFEu;
			/* sz3s[0] is only meaningful when vertex 0 itself hit. */
			s_pktMissSampHitZ[missSampIdx] = ((hits > 0) && (perVert != NULL) && (firstMissIdx != 0)) ? sz3s[0] : 0xFFFFu;
			s_pktMissSampStaleZ[missSampIdx] = staleZ;
			s_pktMissSampStaleAge[missSampIdx] = (u8)((staleAge > 255u) ? 255u : staleAge);
		}

		if ((hits > 0) && !mixed && ((hits >= (n - 1)) || ((hits >= 2) && ((u32)(maxSz3 - minSz3) <= PKTDEPTH_PARTIAL_SPREAD_MAX))))
		{
			/* See the partial-hit fill comment above. Flat across the primitive:
			 * the missing corner has no per-vertex depth to contribute, and a
			 * flat sprite/small poly is what this population is. */
			*out = PacketDepth_ClampWorld(0xFFFFu - maxSz3);
			s_pktDepthPartialFilled++;
			PacketDepth_NoteExtent(*out, &s_pktDepth.realMin, &s_pktDepth.realMax);
			s_pendingDepthSpread = 0;
			if (missSampIdx >= 0)
			{
				s_pktMissSampOut[missSampIdx] = *out;
			}
			if (perVert)
			{
				int k;

				for (k = 0; k < n; k++)
				{
					perVert[k] = *out;
				}
			}
			if (perVertPar)
			{
				int k;

				for (k = 0; k < n; k++)
				{
					perVertPar[k] = *out;
				}
			}
			return 1;
		}

		if (s_gpu.otCurrentSlotIdx == NATIVE_GPU_NO_CONTENT_BUCKET)
		{
			/*
			 * MEASURED s32: this branch is not rare. [PKTDEPTHZ] reported
			 * backZ=0..0 through entire races -- meaning the bucket-derived
			 * fallback below never ran and EVERY miss came out here, tens of
			 * thousands of primitives per 256 frames.
			 *
			 * "Keeping the caller's ordinal" was written as the conservative
			 * choice, and on a build with no depth test it was: keeping the
			 * caller's ordinal is at least the behaviour this content had
			 * before the feature existed. */
			return 0;
		}
		*out = PacketDepth_ClampWorld(0xFFFFu - (s_gpu.otCurrentSlotIdx << PKTDEPTH_OT_SHIFT));
		if (missSampIdx >= 0)
		{
			s_pktMissSampOut[missSampIdx] = *out;
		}
		PacketDepth_NoteExtent(*out, &s_pktDepth.backMin, &s_pktDepth.backMax);
		/* Bucket-derived depth is primitive-wide -- there is no per-vertex
		 * information to recover, so the whole primitive stays flat. */
		if (perVert)
		{
			int k;

			for (k = 0; k < n; k++)
			{
				perVert[k] = *out;
			}
		}
		/*
		 * THE MENU POSE, parallax half (s40b). For a genuine miss there is
		 * nothing to recover and both channels stay flat together. For the
		 * flag -- which arrived here because `exempt` declared a miss, not
		 * because its captures were absent -- this is where the wave comes
		 * back: the same entries the occlusion pass just refused to read.
		 */
		if (perVertPar)
		{
			int k;

			for (k = 0; k < n; k++)
			{
				perVertPar[k] = *out;
			}
			if (exempt)
			{
				PacketDepth_FillParallaxFromCapture(xs, ys, n, perVertPar);
			}
		}
		return 1;
	}

	s_pendingDepthIsModel = (firstMtx != s_pktDepthDomMtx);

	*out = PacketDepth_ClampWorld(0xFFFFu - maxSz3); /* see SENSE INVERSION above */
	PacketDepth_NoteExtent(*out, &s_pktDepth.realMin, &s_pktDepth.realMax);
	s_pktDepth.real++;
	s_pendingDepthSpread = (maxSz3 >= minSz3) ? (u32)(maxSz3 - minSz3) : 0u;
	if (s_pendingDepthIsModel)
	{
		s_pktDepth2.mdlReal++;
	}
	else
	{
		s_pktDepth2.wldReal++;
	}
	{
		u32 *lo = s_pendingDepthIsModel ? &s_pktDepth2.mdlSzLo : &s_pktDepth2.wldSzLo;
		u32 *hi = s_pendingDepthIsModel ? &s_pktDepth2.mdlSzHi : &s_pktDepth2.wldSzHi;

		/* lo starts at the sentinel below rather than 0, because sz3 0 is a
		 * legal value (geometry at the eye) and seeding on it would peg the
		 * low end forever. Each population seeds independently. */
		if ((u32)minSz3 < *lo) { *lo = minSz3; }
		if ((u32)maxSz3 > *hi) { *hi = maxSz3; }
	}
	/*
	 * The whole point: every vertex keeps its OWN depth instead of
	 * inheriting the primitive's farthest corner, so the GPU interpolates
	 * across the polygon exactly as it already does for resident track
	 * geometry. *out stays the flat max so callers that cannot carry
	 * per-vertex depth (and the [PKTDEPTHZ] extents) are unchanged.
	 */
	if (perVert)
	{
		for (i = 0; i < n; i++)
		{
			perVert[i] = PacketDepth_ClampWorld(0xFFFFu - sz3s[i]);
		}
	}
	/* Everything that reaches here is ordinary world content -- an exempt
	 * primitive was diverted to the miss path above and never arrives. The two
	 * channels are therefore equal by construction, which is the invariant for
	 * every primitive in the game except the flag's. */
	if (perVertPar)
	{
		for (i = 0; i < n; i++)
		{
			perVertPar[i] = PacketDepth_ClampWorld(0xFFFFu - sz3s[i]);
		}
	}
	return 1;
}

/* Parse-path wrapper: stashes the result for NativeGpu_StampEmitOrdinal, which
 * is where that path's stamp is actually written. */
internal void PacketDepth_ResolvePrim(VERTTYPE **srcs, int n)
{
	s16 xs[4], ys[4];
	int i;

	for (i = 0; i < n; i++)
	{
		xs[i] = (s16)srcs[i][0];
		ys[i] = (s16)srcs[i][1];
	}

	s_pendingDepthValid = PacketDepth_ResolveXY(xs, ys, n, &s_pendingDepth, s_pendingDepthPerVert, s_pendingDepthPerVertParallax);
	s_pendingDepthCount = s_pendingDepthValid ? n : 0;
}

/* Direct-emit wrapper: that path has packed XY words and writes its own z, so
 * it takes the stamp back directly instead of going through the pending slot. */
internal int PacketDepth_ResolvePacked(const u32 *xyPacked, int n, u16 *out, u16 *perVert, u16 *perVertPar)
{
	s16 xs[4], ys[4];
	int i;

	for (i = 0; i < n; i++)
	{
		xs[i] = (s16)(xyPacked[i] & 0xFFFFu);
		ys[i] = (s16)(xyPacked[i] >> 16);
	}

	return PacketDepth_ResolveXY(xs, ys, n, out, perVert, perVertPar);
}
#endif

internal void NativeGpu_StampEmitOrdinal(int firstVertex)
{
	u16 stamp = NativeGpu_NextDepthStamp();

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* Consume-and-clear: the resolution belongs to the primitive the parse loop
	 * just appended, and must never leak to the next one. NextDepthStamp is
	 * still called unconditionally above so the emit ordinal keeps advancing at
	 * exactly the same rate whether or not a primitive took real depth -- the
	 * bucket->ordinal recorder and the miss fallback both depend on that
	 * sequence being undisturbed. */
	if (s_pendingDepthValid)
	{
		stamp = s_pendingDepth;
		s_pendingDepthValid = 0;

		/*
		 * Per-vertex depth. Only when the parse loop appended EXACTLY one
		 * vertex per resolved source vertex, in order -- checked here rather
		 * than assumed, because a path that appends a different count (or
		 * reorders) would otherwise pair each vertex with a stranger's depth,
		 * which looks like subtle z-fighting rather than an obvious break.
		 * Any mismatch silently keeps today's flat stamp below.
		 */
		if ((s_pendingDepthCount > 0) && ((s_gpu.vertexIndex - firstVertex) == s_pendingDepthCount))
		{
			int k;

			for (k = 0; k < s_pendingDepthCount; k++)
			{
				s_gpu.vertexBuffer[firstVertex + k].emitOrdinal = s_pendingDepthPerVert[k];
				/* Parallax channel rides the same guarded index: it is
				 * only trustworthy under the exact one-appended-per-resolved
				 * rule this branch just verified, same as the occlusion depth
				 * beside it. */
				s_gpu.vertexBuffer[firstVertex + k].parallaxOrdinal = s_pendingDepthPerVertParallax[k];
			}
			s_pendingDepthCount = 0;
			s_pktDepthPerVertHits++;
			if (s_pendingDepthIsModel)
			{
				s_pktDepth2.mdlPerVert++;
			}
			else
			{
				s_pktDepth2.wldPerVert++;
			}
			s_gpu.emitOrdinal++;
			return;
		}

		/*
		 * INSTRUMENT. The guard above is silent by design, and silence is
		 * how it hid: falling back writes ONE depth for the whole primitive,
		 * and that depth is the running MAXIMUM over its vertices (retail's
		 * rule, correct for choosing an OT bucket, wrong for a depth buffer).
		 * A primitive stamped at its farthest corner sits as far away as its
		 * own back edge, so a kart made of them reads several of its own
		 * lengths too distant and loses the depth test to the ground it is
		 * standing on -- which is the reported sinking, and which the owner
		 * confirmed on hardware by driving visually INTO a wall before the
		 * game agreed he had reached it.
		 *
		 * Suspected trigger: a 4-sided face resolves 4 vertices but is drawn
		 * as two triangles = 6 appended, so 6 != 4 and every quad silently
		 * takes the flat maximum. Triangles match and are fine. PS1 models are
		 * mostly quads. Counted rather than asserted, and the sample pair is
		 * carried so the ratio is readable instead of inferred.
		 */
		s_pktDepthFlatFallback++;
		s_pktDepthLastAppended = s_gpu.vertexIndex - firstVertex;
		s_pktDepthLastResolved = s_pendingDepthCount;
		s_pendingDepthCount = 0;

		/*
		 * s32: which POPULATION lands here, and how much it costs them.
		 *
		 * The flat stamp is the primitive's farthest vertex, so the damage it
		 * does is exactly the primitive's own depth SPREAD -- a flat-stamped
		 * face is pushed back by (maxSz3 - minSz3) view.z units relative to
		 * where its near edge really is. A kart-sized spread on a kart is the
		 * reported sink; the same spread on a road quad running away from the
		 * camera is huge and harmless, because nothing is trying to stand on
		 * its near edge. Counting the two together is what has kept this
		 * ambiguous for three sessions, so they are counted apart, and the
		 * worst model spread is carried so the error has a magnitude and not
		 * just a rate.
		 */
		if (s_pendingDepthIsModel)
		{
			s_pktDepth2.mdlFlat++;
			if (s_pendingDepthSpread > s_pktDepth2.mdlFlatMaxSpread)
			{
				s_pktDepth2.mdlFlatMaxSpread = s_pendingDepthSpread;
			}
			s_pktDepth2.mdlFlatLastAppended = s_pktDepthLastAppended;
			s_pktDepth2.mdlFlatLastResolved = s_pktDepthLastResolved;
		}
		else
		{
			s_pktDepth2.wldFlat++;
		}
	}
#endif

	for (int i = firstVertex; i < s_gpu.vertexIndex; i++)
	{
		s_gpu.vertexBuffer[i].emitOrdinal = stamp;
		/* Flat path: both channels take the same value. The flag never lands
		 * here -- its quads resolve 4 and append 6, which TriangulateQuad
		 * expands in lockstep so the per-vertex branch above accepts them --
		 * and if some future primitive of its did, the cost is one flat-rather
		 * -than-rippling face, not a wrong depth. */
		s_gpu.vertexBuffer[i].parallaxOrdinal = stamp;
	}
	s_gpu.emitOrdinal++;
}

/*
 * Depth-compose fix: call
 * at every point real primitive content actually advances the emit ordinal --
 * the parse loop's two NativeGpu_StampEmitOrdinal call sites below (tagged
 * first primitive, tagless primitives loop) and the direct-emit fast path's
 * own manual advance (native_direct_emit.c, which stamps through
 * NativeGpu_NextDepthStamp directly rather than through
 * NativeGpu_StampEmitOrdinal -- it has no vertexBuffer range to backfill).
 *
 * Deliberately NOT folded into NativeGpu_NextDepthStamp itself, even though
 * that function's own comment already calls it "the single source of the
 * depth stamp for BOTH stampers": NextDepthStamp is ALSO called from the
 * singlePrimitive (DrawPrim, non-walk) branch of ParsePrimitivesLinkedList
 * below, which runs with no OT walk in progress at all. At that call site
 * otCurrentSlotIdx is leftover state from whichever real walk last ran --
 * typically bucket 0, the walk's final crossing -- and has no relationship
 * to the single primitive being stamped. Hooking NextDepthStamp itself would
 * let an unrelated DrawPrim (e.g. a HUD element drawn after the frame's main
 * DrawOTag) silently reassign minContentBucket down to that stale near-
 * bucket value -- exactly the corruption this fix exists to prevent, just
 * reintroduced one level up. Calling this explicitly only from the walk's
 * own content-emitting sites keeps it scoped to actual OT-bucket-attributed
 * content.
 */
internal void NativeGpu_RecordContentAdvance(void)
{
	if (!s_gpu.otRecorderActive)
	{
		return;
	}

	if (s_gpu.otCurrentSlotIdx == NATIVE_GPU_NO_CONTENT_BUCKET)
	{
		/* Content emitted before the walk crossed its first marker node --
		 * not expected in practice (the walk head is itself the farthest
		 * bucket's marker node, so real content only ever follows an
		 * already-recorded crossing), but there is no bucket to attribute
		 * this to, so don't fabricate one. */
		return;
	}

	/* Slots are visited in strictly descending index order for the whole
	 * walk, so the LAST call here before the walk ends naturally holds the
	 * MINIMUM bucket that ever had real content -- a plain assignment on
	 * every advance is sufficient; no running-min compare needed. */
	s_gpu.minContentBucket = s_gpu.otCurrentSlotIdx;
}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
/* [SZPROBE] -- stamp-time depth-source probe (PC-first; content counts
 * transfer, so this never needs to run on a device).
 *
 * One proposal for the packet path is real depth by looking up the
 * GTE's sz3 at stamp time, keyed by the screen XY already in the packet. That
 * plan rests on a hit rate NOBODY HAS MEASURED: the 60%/47% figures on record
 * are for a different lookup (the model-space SHAPE hash), with a different key
 * over a different population. Measure the real one before the
 * design commits to it.
 *
 * The counting sites are MakeVertexTriangle/Quad/Rect, further down: they
 * receive p0..p3, pointers to the packet's own XY words, which are exactly what
 * GTE_RotTransPers truncated and keyed the table with. The vertexBuffer copies
 * those functions build have already had DrawEnvOffset added and would key a
 * shifted, always-missing coordinate.
 *
 * Classification matters as much as the rate. A miss on world geometry is a
 * depth fallback; a miss on a HUD glyph is correct behavior, since 2D content
 * has no depth and must keep ordinal treatment in a near band. So 2D is
 * counted separately, using the parallelogram discriminator MakeVertexQuad
 * already relies on -- which is itself a candidate answer to the open
 * question about how to classify 2D content.
 *
 * Declared up here rather than beside those sites because the [SZPROBE] report
 * lives in NativeGpu_ResetEmitOrdinal immediately below -- the one hook that
 * fires exactly once per presented frame.
 *
 * Read-only and off by default: it counts, it never writes a vertex. */
int g_dbg_szProbe = 0;

global_variable struct
{
	u32 tri3dHit, tri3dMiss;
	u32 quad3dHit, quad3dMiss;
	u32 quad2d, rect2d; /* vertices classified 2D, never looked up */
	u32 suppressed;     /* vertices inside the UI band */
	/* Per-PRIMITIVE coverage, which is what the fallback policy actually keys
	 * on: a primitive with one missing vertex cannot take real depth for two
	 * corners and ordinal depth for the third, so it falls back whole. The
	 * vertex rate overstates usable coverage and must not be quoted for this
	 * decision -- and the two can't be derived from each other, since misses
	 * cluster within a primitive (a partly off-screen face loses several
	 * corners at once) rather than landing independently. */
	u32 primFull, primPartial;
	u32 frames;
} s_szProbe;

internal void SzProbe_CountVerts(VERTTYPE **srcs, int n, u32 *hit, u32 *miss)
{
	int i;
	int missed = 0;

	if (GTECapture_LookupSuppressed())
	{
		s_szProbe.suppressed += (u32)n;
		return;
	}

	for (i = 0; i < n; i++)
	{
		u8 fracX8, fracY8;

		/* Same key and same generation/collision test the depth lookup would
		 * use -- only the payload read differs, so hit/miss counts here are
		 * the hit/miss counts an sz3 lookup would see. */
		if (GTECapture_Lookup(srcs[i][0], srcs[i][1], &fracX8, &fracY8))
		{
			(*hit)++;
		}
		else
		{
			(*miss)++;
			missed = 1;
		}
	}

	if (missed)
	{
		s_szProbe.primPartial++;
	}
	else
	{
		s_szProbe.primFull++;
	}
}
#endif

/*
 * Reset the emission ordinal. Called by the platform renderer when the DEPTH
 * BUFFER clears (once per presented frame), NOT from ClearSplits: ClearSplits
 * runs per-OT-draw, and frames that draw in multiple passes (the loading
 * screen's flag pass + letters pass) would restart the ordinal mid-frame --
 * the later pass then starts at far depth and LOSES the depth test against
 * the earlier pass's already-written values. Found empirically: the depth
 * build's loading screens rendered the flag but buried the "LOAD" letters.
 * Ordinals must stay monotonic for the lifetime of one depth clear.
 */
void NativeGpu_ResetEmitOrdinal(void)
{
	/* [OTZ2] table-validation line (every 256 depth clears while the
	 * recorder is on): slots should track the OT size (~1030 for 1P),
	 * spurious counts the len-0 filler packets the address window rejects,
	 * and slots+spurious should equal cross. Numbers are from the LAST walk
	 * before this clear (later walks overwrite -- normal frames have one). */
	if (g_dbg_otzWiden)
	{
		static u32 s_otzFrames;

		if (++s_otzFrames >= 256)
		{
			Platform_Log("[OTZ2] frames=%lu slots=%lu spurious=%lu cross=%lu maxSlot=%lu lastOrd=%lu\n", (unsigned long)s_otzFrames,
			             (unsigned long)s_gpu.otSlotsDetected, (unsigned long)s_gpu.otSpuriousLen0, (unsigned long)s_gpu.otSlotsCrossed,
			             (unsigned long)s_gpu.otMaxSlotIdx, (unsigned long)s_gpu.lastStampedZ);
			s_otzFrames = 0;
		}
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* [SZPROBE] every 256 presented frames. hit3d% is THE number the depth
	 * design turns on: the share of world-geometry vertices that would find a
	 * real sz3 at stamp time. flat2d/rect/supp size the near band that keeps
	 * ordinal treatment. Reported here because this is the one hook that fires
	 * exactly once per presented frame (see the comment above). */
	/* [PKTDEPTH] every 256 presented frames while the depth model is on.
	 * realPct is the hardware counterpart of [SZPROBE]'s primFullPct: if the
	 * two disagree, PC content was not representative and the design's 97.09%
	 * needs re-deriving on the platform that matters. */
	if (!g_dbg_packetDepth)
	{
		/* Keep the stamp counter zeroed while the feature is off, so the FIRST
		 * window after enabling reports a real 'escaped' instead of counting
		 * stamps issued before the resolver was ever consulted. A misleading
		 * first reading is worse than no reading -- it is the one most likely
		 * to be acted on. */
		s_pktDepthStamps = 0;
	}
	else
	{
		const u32 prims = s_pktDepth.real + s_pktDepth.fellBack;
		u8 domId = 0;
		u32 idsUsed = 0;

		/* s32: refresh the provenance reference once per presented frame. In
		 * race content the dominant matrix is the camera every frame, so a
		 * one-frame lag here would be harmless anyway -- but this hook fires
		 * after the walk that consumed it, so the value the resolver used was
		 * this frame's. */
		(void)GTECapture_ReadMatrixCensus(&domId, &idsUsed);
		s_pktDepthDomMtx = domId;

		/* First window needs the same sentinel seeding every later reset does;
		 * the struct's zero-initialisation would otherwise pin both lo fields
		 * at 0 and report a range starting at the eye. */
		if (!s_pktDepth2Seeded)
		{
			s_pktDepth2Seeded = 1;
			PacketDepth2_Reset();
		}

		s_pktDepth.frames++;
		if (s_pktDepth.frames >= 256)
		{
			/* stamps vs seen: stamps counts every primitive that got a depth
			 * stamp from either emit path; seen counts the ones the resolver
			 * was actually offered. escaped != 0 means some path is writing
			 * stamps on the ordinal axis while the rest of the frame is on the
			 * true-depth axis -- the defect class that put waterfalls through
			 * the HUD. It should be 0. */
			const u32 seen = prims + s_pktDepth.suppressed2d;

			/* Use Platform_LogInstrument here, not Platform_Log -- Platform_Log
			 * never reaches ctr3ds.log on 3DS, the only platform with a depth
			 * test to validate this against. */
			Platform_LogInstrument("[PKTDEPTHZ] realZ=%u..%u backZ=%u..%u uiZ=%u..%u (larger = nearer)\n", s_pktDepth.realMin, s_pktDepth.realMax,
			             s_pktDepth.backMin, s_pktDepth.backMax, s_pktDepth.uiMin, s_pktDepth.uiMax);
			/* s45 miss forensics flush -- see the sample declarations for the
			 * field meanings. Sampled misses only exist while the depth check is on. */
			{
				u32 mk;

				for (mk = 0; mk < s_pktMissSampCount; mk++)
				{
					/* hitZ: first hitting vertex's depth (0xFFFF = no hits) --
					 * strangers read as the content BEHIND the prim. staleAge>0:
					 * this XY was projected staleAge frames ago carrying staleZ
					 * -- the stale-coordinate smoking gun. */
					Platform_LogInstrument("[PKTMISS] xy=(%d,%d) n=%u hits=%u nbr=%u slot=%u out=%u hitZ=%u staleAge=%u staleZ=%u occ=(%d,%d) occGen=%u occZ=%u\n",
					             (int)s_pktMissSampX[mk], (int)s_pktMissSampY[mk],
					             (unsigned)s_pktMissSampVerts[mk], (unsigned)s_pktMissSampHits[mk], (unsigned)s_pktMissSampNbr[mk],
					             (unsigned)s_pktMissSampSlot[mk], (unsigned)s_pktMissSampOut[mk], (unsigned)s_pktMissSampHitZ[mk],
					             (unsigned)s_pktMissSampStaleAge[mk], (unsigned)s_pktMissSampStaleZ[mk],
					             (int)s_pktMissSampOccX[mk], (int)s_pktMissSampOccY[mk], (unsigned)s_pktMissSampOccGen[mk], (unsigned)s_pktMissSampOccZ[mk]);
				}
				s_pktMissSampCount = 0;
			}
			Platform_LogInstrument("[PKTDEPTH] frames=%lu prims=%lu real=%lu fellBack=%lu realPct=%lu ui2d=%lu stamps=%lu escaped=%ld uiMax=%lu/%d\n",
			             (unsigned long)s_pktDepth.frames, (unsigned long)prims, (unsigned long)s_pktDepth.real,
			             (unsigned long)s_pktDepth.fellBack, (unsigned long)(prims ? ((s_pktDepth.real * 100u) / prims) : 0u),
			             (unsigned long)s_pktDepth.suppressed2d, (unsigned long)s_pktDepthStamps, (long)((long)s_pktDepthStamps - (long)seen),
			             /* uiMax: worst single walk's UI prim count vs the band's
			              * capacity. At or above capacity means overflow is being
			              * saturated and UI layering is degrading. */
			             (unsigned long)s_pktUiOrdinalMax, PKTDEPTH_UI_BAND << PKTDEPTH_UI_ORD_SHIFT);
			{
				/* s32 sinking racers. READ IT LIKE THIS:
				 *   mdlFell large      -> mechanism 2 (karts reconstructed with
				 *                         the track's >>6 ruler)
				 *   mixed/evict large  -> mechanism 1 (karts wearing the road's
				 *                         depth); evictDz is how big the lie is,
				 *                         in the same units as the stamps
				 *   both ~0            -> BOTH mechanisms are dead and the sink
				 *                         is somewhere this instrument cannot
				 *                         see. Say so; do not reach for a third
				 *                         theory without a third measurement.
				 * mdlReal being 0 would mean no instance geometry resolved at
				 * all this window -- check that before reading anything else,
				 * because every other split would then be meaningless. */
				u32 evictSame = 0, evictDz = 0;
				const u32 evictDiff = GTECapture_ReadEvictCensus(&evictSame, &evictDz);

				Platform_LogInstrument("[PKTDEPTH2] dom=%u wld=%lu/%lu mdl=%lu/%lu unkFell=%lu mixed=%lu evict=%lu/%lu evictDz=%lu\n",
				                       (unsigned)s_pktDepthDomMtx, (unsigned long)s_pktDepth2.wldReal, (unsigned long)s_pktDepth2.wldFellBack,
				                       (unsigned long)s_pktDepth2.mdlReal, (unsigned long)s_pktDepth2.mdlFellBack, (unsigned long)s_pktDepth2.unkFellBack,
				                       (unsigned long)s_pktDepth2.mixed, (unsigned long)evictDiff,
				                       (unsigned long)evictSame, (unsigned long)evictDz);
				/* THE LINE THIS BUILD EXISTS FOR. mdlFlat is the count of model
				 * primitives stamped at their own farthest corner; spread is how
				 * far back that pushes the worst of them, in the same view.z
				 * units the stamps use. mdlFlat ~0 kills the theory outright. */
				Platform_LogInstrument("[PKTDEPTH3] mdl perVert=%lu flat=%lu | wld perVert=%lu flat=%lu | mdlFlatSpread=%lu lastMismatch=%d/%d\n",
				                       (unsigned long)s_pktDepth2.mdlPerVert, (unsigned long)s_pktDepth2.mdlFlat,
				                       (unsigned long)s_pktDepth2.wldPerVert, (unsigned long)s_pktDepth2.wldFlat,
				                       (unsigned long)s_pktDepth2.mdlFlatMaxSpread, s_pktDepth2.mdlFlatLastAppended,
				                       s_pktDepth2.mdlFlatLastResolved);
				/* THE COMPARISON. Same units as [TRKDEPTHCHK]'s ours=/retail=,
				 * so the kart's distance and the road's distance can be read
				 * against each other directly for the first time. */
				Platform_LogInstrument("[PKTDEPTHSZ] mdlSz=%lu..%lu wldSz=%lu..%lu (view.z units, larger = farther)\n",
				                       (unsigned long)(s_pktDepth2.mdlSzLo == 0xFFFFFFFFu ? 0u : s_pktDepth2.mdlSzLo),
				                       (unsigned long)s_pktDepth2.mdlSzHi,
				                       (unsigned long)(s_pktDepth2.wldSzLo == 0xFFFFFFFFu ? 0u : s_pktDepth2.wldSzLo),
				                       (unsigned long)s_pktDepth2.wldSzHi);
				PacketDepth2_Reset();
			}
			memset(&s_pktDepth, 0, sizeof(s_pktDepth));
			s_pktDepthStamps = 0;
		}
	}

	if (g_dbg_szProbe)
	{
		s_szProbe.frames++;
		if (s_szProbe.frames >= 256)
		{
			const u32 hit3d = s_szProbe.tri3dHit + s_szProbe.quad3dHit;
			const u32 miss3d = s_szProbe.tri3dMiss + s_szProbe.quad3dMiss;
			const u32 look3d = hit3d + miss3d;

			const u32 prims = s_szProbe.primFull + s_szProbe.primPartial;

			Platform_Log("[SZPROBE] frames=%lu 3dverts=%lu hit=%lu miss=%lu hit3dPct=%lu prims=%lu primFull=%lu primFullPct=%lu tri=%lu/%lu quad=%lu/%lu flat2d=%lu rect=%lu supp=%lu\n",
			             (unsigned long)s_szProbe.frames, (unsigned long)look3d, (unsigned long)hit3d, (unsigned long)miss3d,
			             (unsigned long)(look3d ? ((hit3d * 100u) / look3d) : 0u), (unsigned long)prims, (unsigned long)s_szProbe.primFull,
			             (unsigned long)(prims ? ((s_szProbe.primFull * 100u) / prims) : 0u), (unsigned long)s_szProbe.tri3dHit,
			             (unsigned long)(s_szProbe.tri3dHit + s_szProbe.tri3dMiss), (unsigned long)s_szProbe.quad3dHit,
			             (unsigned long)(s_szProbe.quad3dHit + s_szProbe.quad3dMiss), (unsigned long)s_szProbe.quad2d,
			             (unsigned long)s_szProbe.rect2d, (unsigned long)s_szProbe.suppressed);
			memset(&s_szProbe, 0, sizeof(s_szProbe));
		}
	}
#endif

	s_gpu.emitOrdinal = 0;
	s_gpu.lastStampedZ = 0;
}

void ClearSplits(void)
{
#ifdef CTR_NATIVE_OTWALK_PROFILE
	OTProf_FoldFrame();
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT
	DirectEmit_FoldFrame();
#endif
	// NOTE: the GTE capture table (native_gte_capture.c) is deliberately NOT
	// cleared here -- this runs per-OT-draw, after this frame's captures and
	// before this frame's vertex builds. Its clear lives in MainFrame_ResetDB.
	s_gpu.currentSplitDebugText = NULL;
	s_gpu.vertexIndex = 0;
	s_gpu.splitIndex = 0;
	s_gpu.splits[0].texFormat = (TexFormat)0xFFFF;
	s_gpu.splits[0].psxTexturedSemiTrans = false;
	s_gpu.splits[0].psxTextureOutputSTP = false;
	s_gpu.splits[0].psxDrawMaskSet = false;
	s_gpu.splits[0].deferredSmoke = false;
	s_gpu.framebufferFeedbackRunActive = false;
}

int NativeGpu_GetStateSize(void)
{
	return (int)sizeof(struct NativeGpuSnapshot);
}

int NativeGpu_CaptureState(void *dst, int dstSize)
{
	struct NativeGpuSnapshot *snapshot = (struct NativeGpuSnapshot *)dst;

	if ((dst == NULL) || (dstSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if (NativeRenderer_GetVRAMStateSize() != (int)sizeof(snapshot->vram))
	{
		return 0;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->magic = NATIVE_GPU_STATE_MAGIC;
	snapshot->version = NATIVE_GPU_STATE_VERSION;
	snapshot->size = sizeof(*snapshot);
	snapshot->drawEnv = activeDrawEnv;
	snapshot->dispEnv = activeDispEnv;
	snapshot->gpuDisabledState = g_GPUDisabledState;
	snapshot->psxDrawMaskSet = s_gpu.psxDrawMaskSet;

	return NativeRenderer_CaptureVRAMState(snapshot->vram, sizeof(snapshot->vram));
}

int NativeGpu_RestoreState(const void *src, int srcSize)
{
	const struct NativeGpuSnapshot *snapshot = (const struct NativeGpuSnapshot *)src;

	if ((src == NULL) || (srcSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->magic != NATIVE_GPU_STATE_MAGIC) || (snapshot->version != NATIVE_GPU_STATE_VERSION) || (snapshot->size != sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->gpuDisabledState < 0) || (snapshot->gpuDisabledState > 1) || (snapshot->psxDrawMaskSet < 0) || (snapshot->psxDrawMaskSet > 1))
	{
		return 0;
	}
	if (NativeRenderer_GetVRAMStateSize() != (int)sizeof(snapshot->vram))
	{
		return 0;
	}
	if (!NativeRenderer_RestoreVRAMState(snapshot->vram, sizeof(snapshot->vram)))
	{
		return 0;
	}

	activeDrawEnv = snapshot->drawEnv;
	activeDispEnv = snapshot->dispEnv;
	g_GPUDisabledState = snapshot->gpuDisabledState;
	s_gpu.psxDrawMaskSet = snapshot->psxDrawMaskSet;
	ClearSplits();
	return 1;
}

internal void DrawEnvDimensionsInt(int *width, int *height)
{
	if (activeDrawEnv.dfe)
	{
		*width = activeDispEnv.disp.w;
		*height = activeDispEnv.disp.h;
	}
	else
	{
		*width = activeDrawEnv.clip.w;
		*height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float *ofsX, float *ofsY)
{
	if (activeDrawEnv.dfe)
	{
		int w, h;
		DrawEnvDimensionsInt(&w, &h);

		if (w <= 0)
		{
			w = 1;
		}

		// NOTE(aalhendi): Convert PS1 VRAM-page draw offsets into display-relative host-screen offsets.
		// CTR alternates draw pages at y=0 and y=0x128; using raw modulo VRAM offsets shifts every other native frame vertically.
		*ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		*ofsX = activeDrawEnv.ofs[0] - activeDrawEnv.clip.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDrawEnv.clip.y;
	}
}

void LineSwapSourceVerts(VERTTYPE **p0, VERTTYPE **p1, u8 **c0, u8 **c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if (((*p0)[0] > (*p1)[0]) || ((*p0)[1] > (*p1)[1] && (*p0)[0] == (*p1)[0]))
	{
		VERTTYPE *tmp = *p0;
		*p0 = *p1;
		*p1 = tmp;

		u8 *tmpCol = *c0;
		*c0 = *c1;
		*c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if (dx > abs((s16)dy))
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* Fourth and last geometry builder. Lines are the same story as rects:
	 * screen-space by the time they get here, but their endpoints may or may
	 * not have come from the GTE, and the lookup is what decides. Wired for the
	 * same reason -- an unresolved builder is a slice of the frame on the wrong
	 * depth axis, and [PKTDEPTH]'s 'escaped' counts it. */
	{
		s16 xs[2] = {(s16)p0[0], (s16)p1[0]};
		s16 ys[2] = {(s16)p0[1], (s16)p1[1]};

		s_pendingDepthValid = PacketDepth_ResolveXY(xs, ys, 2, &s_pendingDepth, s_pendingDepthPerVert, s_pendingDepthPerVertParallax);
		s_pendingDepthCount = s_pendingDepthValid ? 2 : 0;
	}
#endif
}

// Draws geometry at its true GTE sub-pixel position instead of snapping to
// whole pixels, by looking up the fraction GTE_RotTransPers discarded
// and carrying it in the otherwise-dead
// GrVertex._p0/_p1 padding bytes. A miss (table cleared, or these vertices
// were never captured -- e.g. built from a synthesized/HUD coordinate) just
// leaves _p0/_p1 at the memset default of 0, i.e. today's exact behavior.
#ifndef CTR_NATIVE_VERTEX_DESNAP
#define CTR_NATIVE_VERTEX_DESNAP 0
#endif

void MakeVertexTriangle(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

#if defined(CTR_NATIVE) && CTR_NATIVE_VERTEX_DESNAP
	if (!GTECapture_LookupSuppressed())
	{
		VERTTYPE *srcs[3] = {p0, p1, p2};
		int i;
		for (i = 0; i < 3; i++)
		{
			u8 fracX8, fracY8;
			if (GTECapture_Lookup(srcs[i][0], srcs[i][1], &fracX8, &fracY8))
			{
				vertex[i]._p0 = (s8)fracX8;
				vertex[i]._p1 = (s8)fracY8;
			}
		}
	}
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* Triangles carry no 2D discriminator the way quads do, but the game emits
	 * them almost exclusively for world geometry, so they count as 3D. */
	{
		VERTTYPE *srcs[3] = {p0, p1, p2};

		if (g_dbg_szProbe)
		{
			SzProbe_CountVerts(srcs, 3, &s_szProbe.tri3dHit, &s_szProbe.tri3dMiss);
		}
		PacketDepth_ResolvePrim(srcs, 3);
	}
#endif
}

void MakeVertexQuad(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2, VERTTYPE *p3)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

#if defined(CTR_NATIVE) && CTR_NATIVE_VERTEX_DESNAP
	{
		// 2D UI quads (font glyphs, panels) must not consult the table: the key
		// is only a screen position, so a UI corner coincidentally sharing a
		// pixel with some projected world vertex falsely HITs and inherits that
		// vertex's fraction (visible as UI shimmer tracking background motion).
		// Discriminator: every 2D-affine transform of a rectangle (CTR's menu
		// text waves/rocks/scales its glyph quads) stays an exact integer
		// parallelogram (p1-p0 == p3-p2 in the PSX TL,TR,BL,BR corner order),
		// while a perspective-projected world quad only forms one when it is
		// screen-parallel. Misclassifying such a world quad just draws it
		// snapped for that frame -- the safe fallback by construction.
		int parallelogram = ((p1[0] - p0[0]) == (p3[0] - p2[0])) && ((p1[1] - p0[1]) == (p3[1] - p2[1]));

		if (!parallelogram && !GTECapture_LookupSuppressed())
		{
			VERTTYPE *srcs[4] = {p0, p1, p2, p3};
			int i;
			for (i = 0; i < 4; i++)
			{
				u8 fracX8, fracY8;
				if (GTECapture_Lookup(srcs[i][0], srcs[i][1], &fracX8, &fracY8))
				{
					vertex[i]._p0 = (s8)fracX8;
					vertex[i]._p1 = (s8)fracY8;
				}
			}
		}
	}
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	{
		/* Recomputed rather than shared with the desnap block above, which
		 * compiles out in a capture-only build -- this has to stand on its
		 * own. Same discriminator, same reasoning (see that comment).
		 *
		 * The discriminator classifies for the [SZPROBE] counters only. It must
		 * NOT gate the depth resolve, which an earlier version did: a
		 * parallelogram is not reliably 2D (a screen-parallel WORLD quad forms
		 * one too, and those need real depth), and skipping the resolver left
		 * genuine UI on the raw ordinal instead of in its reserved near band --
		 * an escape in both directions at once.
		 *
		 * Every quad goes to the resolver, which already distinguishes these
		 * correctly and by evidence rather than by geometry: UI is caught by
		 * band suppression, world quads hit the capture table, and anything
		 * else takes the bucket fallback. */
		int parallelogram = ((p1[0] - p0[0]) == (p3[0] - p2[0])) && ((p1[1] - p0[1]) == (p3[1] - p2[1]));
		VERTTYPE *srcs[4] = {p0, p1, p2, p3};

		if (g_dbg_szProbe)
		{
			if (parallelogram)
			{
				s_szProbe.quad2d += 4;
			}
			else
			{
				SzProbe_CountVerts(srcs, 4, &s_szProbe.quad3dHit, &s_szProbe.quad3dMiss);
			}
		}

		PacketDepth_ResolvePrim(srcs, 4);

		/* Flavor-3 paint-on-top (the lens flare): X stretched to the wide
		 * window, same linear remap as the skybox glow and identity when wide
		 * is off. AFTER the resolve, which must key on the raw coordinates
		 * the stores used. Quad-only on purpose -- the one member emits GT4s;
		 * a triangle-emitting member would need the same lines in
		 * MakeVertexTriangle. */
		if (GTECapture_NodePaintNear() == 3)
		{
			extern int NativeWide_GlowStretchX(int x);
			int vi;

			for (vi = 0; vi < 4; vi++)
			{
				vertex[vi].x = (float)NativeWide_GlowStretchX((int)vertex[vi].x);
			}
		}

		/* Menu backdrop: the diorama strips are authored to the same retail
		 * rect as the window border around them, but the border is UI and
		 * takes the widescreen X ramp while these packet quads do not -- so
		 * wide left the strips short of the border on both sides. Same ramp,
		 * same screen X, so the two agree by construction. AFTER the resolve,
		 * which keys on the raw coordinates the stores used, and identity
		 * whenever wide is off. */
		if (s_menubdNodeActive)
		{
			extern int NativeWide_UiShiftX(int x);
			int vi;

			for (vi = 0; vi < 4; vi++)
			{
				vertex[vi].x = (float)NativeWide_UiShiftX((int)vertex[vi].x);
			}
		}
	}
#endif
}

void MakeVertexRect(GrVertex *vertex, VERTTYPE *p0, s16 w, s16 h)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	if (g_dbg_szProbe)
	{
		s_szProbe.rect2d += 4;
	}

	/* Rects are axis-aligned in SCREEN space, but that does not make them 2D
	 * content: world sprites (particles, shadows, item boxes) get their origin
	 * from a projected point, while HUD panels get theirs from game math. The
	 * capture lookup already tells those apart -- the first hits, the second
	 * misses or is UI-band suppressed -- so resolve on the origin vertex and
	 * let the normal rules sort it out.
	 *
	 * Leaving these unresolved was an escape: [PKTDEPTH] measured 12-14% of all
	 * stamps bypassing the resolver with only tris and quads wired up. Only one
	 * point is available, so a rect's depth is its origin's depth; that is the
	 * same approximation the PS1 makes by giving a SPRT a single Z. */
	{
		s16 x = (s16)p0[0], y = (s16)p0[1];

		/* n == 1: per-vertex and flat are the same value by construction, and
		 * so is parallax -- both channel arrays are NULL and the flat stamp
		 * below feeds z and w alike. Rects are never flag content. */
		s_pendingDepthValid = PacketDepth_ResolveXY(&x, &y, 1, &s_pendingDepth, NULL, NULL);
		s_pendingDepthCount = 0;
	}
#endif
}

void MakeTexcoordQuad(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, u8 *uv3, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
	    vertex[0].tcx = -1;
	    vertex[0].tcy = -1;

	    vertex[1].tcx = -1;
	    vertex[1].tcy = -1;

	    vertex[2].tcx = -1;
	    vertex[2].tcy = -1;

	    vertex[3].tcx = -1;
	    vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordTriangle(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
	    vertex[0].tcx = -1;
	    vertex[0].tcy = -1;

	    vertex[1].tcx = -1;
	    vertex[1].tcy = -1;

	    vertex[2].tcx = -1;
	    vertex[2].tcy = -1;

	    vertex[3].tcx = -1;
	    vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordRect(GrVertex *vertex, u8 *uv, s16 page, s16 clut, s16 w, s16 h)
{
	assert(uv);

	// sim overflow
	if ((int)uv[0] + w > 255)
	{
		w = 255 - uv[0];
	}
	if ((int)uv[1] + h > 255)
	{
		h = 255 - uv[1];
	}

	const u8 bright = 2;
	const u8 dither = 0;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;

	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}
}

void MakeTexcoordLineZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex *vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourLine(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
}

void MakeColourQuad(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2, u8 *col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	s_gpu.vertexBuffer[s_gpu.vertexIndex + 4] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 3];

	s_gpu.vertexBuffer[s_gpu.vertexIndex + 5] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 2];
	s_gpu.vertexBuffer[s_gpu.vertexIndex + 2] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 3];
	s_gpu.vertexBuffer[s_gpu.vertexIndex + 3] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 1];

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/*
	 * Expand the pending PER-VERTEX depths through the identical permutation,
	 * so a 4-corner face keeps its corners' own depths instead of collapsing
	 * onto the flat far-corner stamp. Written as the SAME four statements in
	 * the SAME order as the vertex expansion above (not a lookup table) so
	 * the permutation cannot drift from it -- a mismatched pairing reads as
	 * subtle z-fighting, not an obvious break. Count 4 exactly: lines resolve
	 * 2 and are unaffected; triangles never reach this function.
	 */
	if (s_pendingDepthCount == 4)
	{
		u16 *d = s_pendingDepthPerVert;
		/* s40b: the parallax channel is permuted by the SAME four statements
		 * in the same order, for the reason spelled out above -- a table of
		 * indices would have to be re-derived by hand whenever the expansion
		 * changes, and a vertex paired with a stranger's parallax reads as a
		 * face torn across two stereo planes. Written as a second copy of the
		 * permutation rather than a loop over both arrays so the two stay
		 * literally identical to the vertex expansion they mirror. */
		u16 *p = s_pendingDepthPerVertParallax;

		d[4] = d[3];

		d[5] = d[2];
		d[2] = d[3];
		d[3] = d[1];

		p[4] = p[3];

		p[5] = p[2];
		p[2] = p[3];
		p[3] = p[1];

		s_pendingDepthCount = 6;
	}
#endif
}

//------------------------------------------------------------------------------------------------------------------------

internal bool NativeGpu_RectOverlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return (aw > 0) && (ah > 0) && (bw > 0) && (bh > 0) && (ax < bx + bw) && (bx < ax + aw) && (ay < by + bh) && (by < ay + ah);
}

internal bool NativeGpu_TPageOverlapsActiveDrawPage(int tpage)
{
	const int pageX = (tpage & 0xf) << 6;
	const int pageY = (tpage & 0x10) ? 0x100 : 0;
	const int pageW = 0x100;
	const int pageH = 0x100;

	if (!activeDrawEnv.dfe)
	{
		return false;
	}

	if (GetTPageFormat(tpage) != TF_16_BIT)
	{
		return false;
	}

	return NativeGpu_RectOverlaps(pageX, pageY, pageW, pageH, activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
}

internal void NativeGpu_PrepareFramebufferFeedback(int tpage)
{
	if (!NativeGpu_TPageOverlapsActiveDrawPage(tpage))
	{
		return;
	}

	if (s_gpu.framebufferFeedbackRunActive)
	{
		return;
	}

	// NOTE(aalhendi): PS1 can draw into VRAM and immediately texture from that
	// same draw page. Native batches primitives, so screen-feedback effects
	// like heat warp need an explicit barrier before their framebuffer-sampling
	// polygons consume the VRAM texture.
	if (NativeGpu_HasPendingSplits())
	{
		DrawAllSplits();
	}

	NativeRenderer_StoreFrameBuffer(activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
	s_gpu.framebufferFeedbackRunActive = true;
}

/* s42b one-shot: set by ProcessFlatPoly's FT4 case for a code-bit-0x01
 * (driver-local smoke) packet, consumed by the very next AddSplit. Unity-TU
 * unique name (see CLAUDE.md static-collision hazard). */
global_variable int s_ngpuDeferSmokeNext = 0;

internal void AddSplit(bool semiTrans, bool textured, bool framebufferFeedback)
{
	int tpage = activeDrawEnv.tpage;
	/* Consume the smoke one-shot up front so every early return below leaves it
	 * cleared -- a coalesce that keeps the current split must not let the flag
	 * leak onto the following primitive. */
	const bool wantDeferredSmoke = (s_ngpuDeferSmokeNext != 0);

	s_ngpuDeferSmokeNext = 0;

	if (framebufferFeedback)
	{
		NativeGpu_PrepareFramebufferFeedback(tpage);
	}
	else
	{
		s_gpu.framebufferFeedbackRunActive = false;
	}

	GPUDrawSplit *curSplit = &s_gpu.splits[s_gpu.splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GetTPageFormat(tpage);
	TextureID textureId = textured ? NativeRenderer_GetVRAMTexture() : NativeRenderer_GetWhiteTexture();
	bool psxTexturedSemiTrans = semiTrans && textured && s_gpu.overrideTexture == 0;
	// NOTE(aalhendi): PS1 framebuffer bit 15 follows sampled texture STP for
	// textured draws unless E6 forces it. Recursive screen-copy effects depend
	// on this bit surviving after the blended textured pass.
	bool psxTextureOutputSTP = textured && s_gpu.overrideTexture == 0;

#ifdef CTR_NATIVE_OTWALK_PROFILE
	// Pre-override retail state: the counter wants what the game asked for,
	// not the native debug-text override texture.
	OTProf_CountPrim(blendMode, texFormat, textured, tpage);
#endif

	if (textured && s_gpu.overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = s_gpu.overrideTexture;
		psxTexturedSemiTrans = false;
	}

	// FIXME: compare drawing environment too?
	if (!psxTexturedSemiTrans && curSplit->blendMode == blendMode && curSplit->texFormat == texFormat && curSplit->textureId == textureId &&
	    curSplit->drawPrimMode == s_gpu.drawPrimMode && curSplit->psxTexturedSemiTrans == psxTexturedSemiTrans &&
	    curSplit->psxTextureOutputSTP == psxTextureOutputSTP && curSplit->psxDrawMaskSet == s_gpu.psxDrawMaskSet &&
	    curSplit->drawenv.clip.x == activeDrawEnv.clip.x && curSplit->drawenv.clip.y == activeDrawEnv.clip.y &&
	    curSplit->drawenv.clip.w == activeDrawEnv.clip.w && curSplit->drawenv.clip.h == activeDrawEnv.clip.h && curSplit->drawenv.dfe == activeDrawEnv.dfe &&
	    curSplit->debugText == s_gpu.currentSplitDebugText && curSplit->deferredSmoke == wantDeferredSmoke)
	{
		return;
	}

	curSplit->numVerts = s_gpu.vertexIndex - curSplit->startVertex;

	if (s_gpu.splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		NATIVE_GPU_ERROR("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors\n");
		return;
	}

	GPUDrawSplit *split = &s_gpu.splits[++s_gpu.splitIndex];
	split->blendMode = blendMode;
	split->texFormat = texFormat;
	split->textureId = textureId;
	split->drawPrimMode = s_gpu.drawPrimMode;
	split->psxTexturedSemiTrans = psxTexturedSemiTrans;
	split->psxTextureOutputSTP = psxTextureOutputSTP;
	split->psxDrawMaskSet = s_gpu.psxDrawMaskSet;
	split->drawenv = activeDrawEnv;
	split->dispenv = activeDispEnv;
	split->debugText = s_gpu.currentSplitDebugText;
	split->deferredSmoke = wantDeferredSmoke;

	split->drawenv.tw.w = s_gpu.overrideTextureWidth;
	split->drawenv.tw.h = s_gpu.overrideTextureHeight;

	split->startVertex = s_gpu.vertexIndex;
	split->numVerts = 0;
}

void DrawSplit(const GPUDrawSplit *split)
{
#ifdef CTR_NATIVE_OTWALK_PROFILE
	OTProf_CountDraw();
#endif
	if (split->debugText)
	{
		NativeRenderer_PushDebugLabel(split->debugText);
	}

	const bool drawOnScreen = split->drawenv.dfe;
	if ((split->drawenv.clip.w <= 0) || (split->drawenv.clip.h <= 0))
	{
		// NOTE(aalhendi): VS/Battle end previews can shrink a losing viewport
		// into an empty retail draw area. Empty clips should consume no pixels,
		// and must not leak stale native offscreen/scissor state.
		NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
		NativeRenderer_SetOffscreenState(&split->drawenv.clip, &split->dispenv, 0);
		if (split->debugText)
		{
			NativeRenderer_PopDebugLabel();
		}
		return;
	}

	NativeRenderer_SetStencilMode(split->drawPrimMode); // draw with mask 0x16

	NativeRenderer_SetTexture(split->textureId, split->texFormat);

	if (split->texFormat == TF_32_BIT_RGBA)
	{
		NativeRenderer_SetOverrideTextureSize(split->drawenv.tw.w, split->drawenv.tw.h);
	}

	NativeRenderer_SetPSXDrawMaskSet(split->psxDrawMaskSet);
	NativeRenderer_SetPSXTextureOutputSTP(split->psxTextureOutputSTP);

	NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
	NativeRenderer_SetOffscreenState(&split->drawenv.clip, &split->dispenv, !drawOnScreen);

#if defined(CTR_INTERNAL) && !defined(__3DS__)
	if (!drawOnScreen)
	{
		// After SetOffscreenState so the capture's session (OnBegin) already
		// exists when its first split records.
		NativeOffscreenCapture_OnSplit(&s_gpu.vertexBuffer[split->startVertex], split->numVerts, (int)split->blendMode, (int)split->texFormat,
		                               split->textureId != NativeRenderer_GetWhiteTexture(), split->psxTexturedSemiTrans ? 1 : 0,
		                               split->psxTextureOutputSTP ? 1 : 0, split->psxDrawMaskSet, split->drawPrimMode, split->drawenv.dtd,
		                               &split->drawenv.clip);
	}
	// Every split, any dfe: the --tri-dump addressing records (no-op unless
	// armed for this vblank).
	NativeOffscreenCapture_OnAnySplit(&s_gpu.vertexBuffer[split->startVertex], split->numVerts, (int)split->texFormat,
	                                  split->textureId != NativeRenderer_GetWhiteTexture());
#endif

	if (split->psxTexturedSemiTrans)
	{
		// NOTE(aalhendi): CTR native renderer divergence from upstream PsyCross.
		// PS1 textured ABE only blends texels whose sampled 16-bit color has STP
		// set; non-STP texels remain opaque. Native split state is per draw,
		// so draw this primitive-sized split twice with shader-side STP masks.
		NativeRenderer_SetBlendMode(BM_NONE);
		NativeRenderer_SetPSXTextureSemiTransPass(1);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);

		NativeRenderer_SetBlendMode(split->blendMode);
		NativeRenderer_SetPSXTextureSemiTransPass(2);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);

		NativeRenderer_SetPSXTextureSemiTransPass(0);
	}
	else
	{
		NativeRenderer_SetBlendMode(split->blendMode);
		NativeRenderer_SetPSXTextureSemiTransPass(0);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);
	}

	if (split->debugText)
	{
		NativeRenderer_PopDebugLabel();
	}
}

internal void SetPSXMaskState(u32 code)
{
	s_gpu.psxDrawMaskSet = (code & 1) != 0;
}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
/*
 * s42b deferred driver-local exhaust-smoke pass.
 *
 * DrawAllSplits holds every split flagged deferredSmoke out of the normal draw
 * and copies its vertices (and a rebased split descriptor) into these private
 * buffers. native_renderer_3ds.c then calls NativeGpu_DrawDeferredSmoke() AFTER
 * the native model pass, so the smoke draws over its own kart body (retail's
 * painter's order) while the depth test still lets a nearer wall/kart occlude
 * it -- the smoke carries real view.z depth via GTECapture_StoreParticleCorner.
 *
 * A private vertex copy, not the split's indices into s_gpu.vertexBuffer:
 * ClearSplits (end of DrawAllSplits) resets s_gpu.vertexIndex to 0, so those
 * indices are free for reuse the moment DrawAllSplits returns -- long before
 * the post-model redraw. One kart's exhaust is a handful of quads; the copy is
 * cheap and immune to buffer reuse.
 */
#define NATIVE_DEFERRED_SMOKE_MAX_SPLITS 64
#define NATIVE_DEFERRED_SMOKE_MAX_VERTS 2048
global_variable GPUDrawSplit s_deferredSmokeSplits[NATIVE_DEFERRED_SMOKE_MAX_SPLITS];
global_variable GrVertex s_deferredSmokeVerts[NATIVE_DEFERRED_SMOKE_MAX_VERTS];
global_variable int s_deferredSmokeSplitCount = 0;
global_variable int s_deferredSmokeVertCount = 0;
global_variable u32 s_deferredSmokeDropped = 0; /* splits/verts that didn't fit */

/* Copy one deferred-smoke split's verts into the private buffer with a rebased
 * startVertex. Silently drops (counted) if either buffer is full -- a dropped
 * smoke puff is a cosmetic miss, never a corruption. */
internal void NativeGpu_CaptureDeferredSmoke(const GPUDrawSplit *split)
{
	int n = split->numVerts;
	int base = s_deferredSmokeVertCount;
	int k;

	if ((s_deferredSmokeSplitCount >= NATIVE_DEFERRED_SMOKE_MAX_SPLITS) || (base + n > NATIVE_DEFERRED_SMOKE_MAX_VERTS))
	{
		s_deferredSmokeDropped++;
		return;
	}
	for (k = 0; k < n; k++)
	{
		s_deferredSmokeVerts[base + k] = s_gpu.vertexBuffer[split->startVertex + k];
	}
	s_deferredSmokeSplits[s_deferredSmokeSplitCount] = *split;
	s_deferredSmokeSplits[s_deferredSmokeSplitCount].startVertex = (u16)base;
	s_deferredSmokeSplitCount++;
	s_deferredSmokeVertCount += n;
}

/*
 * Redraw the captured smoke, AFTER the model pass. Called once per frame from
 * native_renderer_3ds.c between NativeModelDraw_IssueForWalk and the trailing
 * N3DS_MergeFlush. Re-points the renderer at the private vertex copy and runs
 * each split through the ordinary DrawSplit path, so the smoke bins back into
 * the (now empty) merge as blends -- which already draw depth-test-on /
 * depth-write-off -- and the caller's MergeFlush drains it onto the command
 * list on top of the body. Draw order among the smoke splits themselves is
 * their capture order, matching retail's painter's pass. No-op when nothing
 * was deferred (the common case: no driver-local smoke on screen).
 */
/* Frame-boundary reset, called from MainFrame_ResetDB alongside
 * GTECapture_Clear -- the authoritative once-per-frame guarantee that captures
 * never accumulate across frames even if a frame-end flush is ever skipped
 * (the after-flush reset in DrawDeferredSmoke handles the common case). */
void NativeGpu_ResetDeferredSmoke(void)
{
	s_deferredSmokeSplitCount = 0;
	s_deferredSmokeVertCount = 0;
}

void NativeGpu_DrawDeferredSmoke(void)
{
	int i;

	if (s_deferredSmokeSplitCount <= 0)
	{
		return;
	}
	NativeRenderer_UpdateVertexBuffer(s_deferredSmokeVerts, s_deferredSmokeVertCount);
	for (i = 0; i < s_deferredSmokeSplitCount; i++)
	{
		DrawSplit(&s_deferredSmokeSplits[i]);
	}
	s_deferredSmokeSplitCount = 0;
	s_deferredSmokeVertCount = 0;
}
#endif /* CTR_NATIVE_GTE_CAPTURE */

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);
#ifdef CTR_INTERNAL
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex *vert = &s_gpu.vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			NATIVE_GPU_LOG("==========================================\n");
			NATIVE_GPU_LOG("POLYGON: %d\n", g_dbg_polygonSelected);
			NATIVE_GPU_LOG("X: %d Y: %d\n", vert->x, vert->y);
			NATIVE_GPU_LOG("U: %d V: %d\n", vert->u, vert->v);
			NATIVE_GPU_LOG("TP: %d CLT: %d\n", vert->page, vert->clut);

			NATIVE_GPU_LOG("==========================================\n");
		}

		Platform_PollHostEvents();
	}
#endif

	// next code ideally should be called before EndScene
	NativeRenderer_UpdateVertexBuffer(s_gpu.vertexBuffer, s_gpu.vertexIndex);

	for (int i = 1; i <= s_gpu.splitIndex; i++)
	{
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		/* Hold driver-local exhaust smoke out of the normal pass and copy it for
		 * the post-model redraw (NativeGpu_DrawDeferredSmoke). */
		if (s_gpu.splits[i].deferredSmoke)
		{
			NativeGpu_CaptureDeferredSmoke(&s_gpu.splits[i]);
			continue;
		}
#endif
		DrawSplit(&s_gpu.splits[i]);
	}

	ClearSplits();

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/*
	 * Per-vertex depth coverage, once every 64 frames. Reports how many
	 * primitives actually received per-vertex depth versus how many silently
	 * fell back to the flat per-primitive MAXIMUM, plus the appended/resolved
	 * vertex counts from the last fallback so the cause is readable rather
	 * than inferred. Only meaningful with g_dbg_packetDepth on -- with it off
	 * nothing upstream resolves depth and both counters stay at zero, which is
	 * correct and not a bug in the instrument.
	 */
	{
		global_variable u32 s_pktDepthLogCountdown;

		if ((s_pktDepthPerVertHits | s_pktDepthFlatFallback) != 0)
		{
			if (s_pktDepthLogCountdown == 0)
			{
				u32 total = s_pktDepthPerVertHits + s_pktDepthFlatFallback;

				Platform_LogInstrument("[PKTDEPTH] perVert=%u flat=%u (%u%% flat) lastFallback appended=%d resolved=%d\n", s_pktDepthPerVertHits,
				             s_pktDepthFlatFallback, (total > 0) ? ((s_pktDepthFlatFallback * 100u) / total) : 0u, s_pktDepthLastAppended,
				             s_pktDepthLastResolved);
				s_pktDepthLogCountdown = 64;
			}
			s_pktDepthLogCountdown--;
		}
	}
#endif

	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);
}

// forward declarations
int ParsePrimitive(P_TAG *polyTag);
int ParseTaglessPrimitive(u32 *command);

internal bool NativeGpu_IsValidOTLink(uintptr_t link)
{
	if (NativeGpuLinks_IsRegisteredHostPointer((const void *)link))
	{
		return (link & (sizeof(u32) - 1)) == 0;
	}

	return false;
}

internal u32 NativeGpu_ReadPacketWordForLog(uintptr_t packet, int wordIndex)
{
	const struct PlatformMempackArena *arena = Platform_GetMempackArena();
	const uintptr_t word = packet + (uintptr_t)wordIndex * sizeof(u32);
	const uintptr_t end = word + sizeof(u32);

	if ((NativeGpuLinks_IsRegisteredHostRange((const void *)word, sizeof(u32))) && ((word & (sizeof(u32) - 1)) == 0))
	{
		return *(const u32 *)word;
	}

	if ((word < (uintptr_t)arena->base) || (end > (uintptr_t)arena->endOfMemory) || ((word & (sizeof(u32) - 1)) != 0))
	{
		return 0xffffffffu;
	}

	return *(const u32 *)word;
}

internal void NativeGpu_FormatPointerRegion(char *dst, size_t dstSize, uintptr_t ptr)
{
	if ((sdata == NULL) || (sdata->gGT == NULL))
	{
		snprintf(dst, dstSize, "no-gGT");
		return;
	}

	struct GameTracker *gGT = sdata->gGT;

	for (int playerIndex = 0; playerIndex < 4; playerIndex++)
	{
		struct PushBuffer *pb = &gGT->pushBuffer[playerIndex];
		const uintptr_t start = (uintptr_t)pb->ptrOT;
		const uintptr_t end = (uintptr_t)pb->renderBucketOTRangeEnd;
		if ((start != 0) && (end != 0) && (ptr >= start) && (ptr <= end))
		{
			snprintf(dst, dstSize, "pb%d.ot+0x%zx", playerIndex, (size_t)(ptr - start));
			return;
		}
	}

	struct PushBuffer *uiPB = &gGT->pushBuffer_UI;
	const uintptr_t uiStart = (uintptr_t)uiPB->ptrOT;
	const uintptr_t uiEnd = (uintptr_t)uiPB->renderBucketOTRangeEnd;
	if ((uiStart != 0) && (uiEnd != 0) && (ptr >= uiStart) && (ptr <= uiEnd))
	{
		snprintf(dst, dstSize, "ui.ot+0x%zx", (size_t)(ptr - uiStart));
		return;
	}

	for (int dbIndex = 0; dbIndex < 2; dbIndex++)
	{
		struct DB *db = &gGT->db[dbIndex];
		const uintptr_t primStart = (uintptr_t)db->primMem.start;
		const uintptr_t primEnd = (uintptr_t)db->primMem.end;
		if ((primStart != 0) && (ptr >= primStart) && (ptr < primEnd))
		{
			snprintf(dst, dstSize, "db%d.prim+0x%zx", dbIndex, (size_t)(ptr - primStart));
			return;
		}

		const uintptr_t otStart = (uintptr_t)db->otMem.start;
		const uintptr_t otEnd = (uintptr_t)db->otMem.end;
		if ((otStart != 0) && (ptr >= otStart) && (ptr < otEnd))
		{
			snprintf(dst, dstSize, "db%d.ot+0x%zx", dbIndex, (size_t)(ptr - otStart));
			return;
		}
	}

	snprintf(dst, dstSize, "unknown");
}

#ifdef CTR_NATIVE_OTWALK_PROFILE
/*
 * Links-only replica of the walk below: tag read, terminator test, link
 * validity, nothing else. Runs FIRST, so the real walk that follows sees a
 * warmed cache -- draw_otag_drywalk_ms is an upper bound on chase cost, not
 * an exact split. Mirrors the real walk's exits (terminator, invalid link,
 * 65536-iteration cycle cap) so frameDryNodes should equal frameNodes;
 * drymis in the [OTPROF] line counts frames where it didn't.
 */
internal void OTProf_DryWalk(u8 *basePacket)
{
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_OTAG_DRYWALK);

	u32 iterations = 0;
	while (true)
	{
		iterations++;
		if (iterations > 65536)
		{
			break;
		}

		(void)getlen(basePacket);

		if (isendprim(basePacket))
		{
			break;
		}

		u8 *nextPacket = nextPrim(basePacket);
		if (!NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
		{
			break;
		}
		basePacket = nextPacket;
	}
	s_otprof.frameDryNodes += iterations;

	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_DRYWALK);
}
#endif /* CTR_NATIVE_OTWALK_PROFILE */

/*
 * Untouched OT slots. A slot of the walk's OT that no AddPrim touched still
 * holds exactly the word ClearOTagR wrote: length 0, link to the slot below.
 * It carries no content, and its only walk effects are the bucket->ordinal
 * record, the slot counters and the UI band entry, so a run of them is
 * consumed here in one loop instead of a full node visit each, with those
 * effects applied for the whole run. A slot whose word differs in any bit
 * (content linked in, a hand-edited link, a forward-linked ClearOTag OT) is
 * returned for the full node path, as is slot 0, the terminator. Returns the
 * first node not consumed.
 */
#ifdef NATIVE_WALK_CENSUS
/*
 * [WALKCENSUS]: per producer, the nodes the full walk path visited, their
 * payload words, how many the direct-emit path took, and the time from the
 * top of the node's visit to its link decode. Classes past the producer
 * stages: sky (the sky/star span, whatever stage drew it), slot (zero-length
 * nodes: touched OT slots and fillers), other (outside the frame's marked
 * primMem span). Logged every WCEN_LOG_WALKS walks, per walk.
 */
#define WCEN_CLASS_SKY   (WCEN_END)
#define WCEN_CLASS_SLOT  (WCEN_END + 1)
#define WCEN_CLASS_OTHER (WCEN_END + 2)
#define WCEN_CLASSES     (WCEN_END + 3)
#define WCEN_LOG_WALKS   512u

global_variable const char *const s_wcNames[WCEN_CLASSES] = {
    "setup",  "weather", "confetti", "stars",   "hud",  "beaker",    "queue",       "particles", "dispenv", "bursts", "turbo", "skids", "titledpp",
    "execute", "tires",  "shadows",  "heat",    "fade", "level",     "postlevel",   "refreshcard", "clear", "ui",     "sky",   "slot",  "other"};
global_variable u32 s_wcWalks;
global_variable u32 s_wcNodes[WCEN_CLASSES];
global_variable u32 s_wcWords[WCEN_CLASSES];
global_variable u32 s_wcDirect[WCEN_CLASSES];
global_variable u64 s_wcTicks[WCEN_CLASSES];

internal int WalkCensus_Classify(uintptr_t addr, int tagLength)
{
	const uintptr_t *mark = s_wcMark[s_skyRangeReadIdx];

	if (tagLength == 0)
	{
		return WCEN_CLASS_SLOT;
	}
	if (s_skyNodeActive)
	{
		return WCEN_CLASS_SKY;
	}
	if ((mark[WCEN_SETUP] == 0) || (addr < mark[WCEN_SETUP]) || ((mark[WCEN_END] != 0) && (addr >= mark[WCEN_END])))
	{
		return WCEN_CLASS_OTHER;
	}
	for (int stage = WCEN_END - 1; stage >= 0; stage--)
	{
		if ((mark[stage] != 0) && (addr >= mark[stage]))
		{
			return stage;
		}
	}
	return WCEN_CLASS_OTHER;
}

internal void WalkCensus_RecordNode(const u8 *node, int tagLength, int direct, u64 ticks)
{
	const int cls = WalkCensus_Classify((uintptr_t)node, tagLength);

	s_wcNodes[cls]++;
	s_wcWords[cls] += (u32)tagLength;
	s_wcDirect[cls] += (u32)(direct != 0);
	s_wcTicks[cls] += ticks;
}

internal void WalkCensus_EndWalk(void)
{
	u64 freq;
	u64 totalTicks = 0;
	int lvl = -1;

	s_wcWalks++;
	if (s_wcWalks < WCEN_LOG_WALKS)
	{
		return;
	}

	freq = SDL_GetPerformanceFrequency();
	if ((sdata != NULL) && (sdata->gGT != NULL))
	{
		lvl = sdata->gGT->levelID;
	}
	for (int cls = 0; cls < WCEN_CLASSES; cls++)
	{
		totalTicks += s_wcTicks[cls];
	}
	Platform_LogInstrument("[WALKCENSUS] lvl=%d walks=%u total_us/walk=%u | per walk: nodes words direct us\n", lvl, s_wcWalks,
	                       (u32)((totalTicks * 1000000ull / freq) / s_wcWalks));
	for (int cls = 0; cls < WCEN_CLASSES; cls++)
	{
		if (s_wcNodes[cls] == 0)
		{
			continue;
		}
		Platform_LogInstrument("[WALKCENSUS] %-11s %6.1f %7.1f %6.1f %7.1f\n", s_wcNames[cls], (double)s_wcNodes[cls] / s_wcWalks,
		                       (double)s_wcWords[cls] / s_wcWalks, (double)s_wcDirect[cls] / s_wcWalks,
		                       (double)s_wcTicks[cls] * 1000000.0 / (double)freq / s_wcWalks);
	}

	s_wcWalks = 0;
	memset(s_wcNodes, 0, sizeof(s_wcNodes));
	memset(s_wcWords, 0, sizeof(s_wcWords));
	memset(s_wcDirect, 0, sizeof(s_wcDirect));
	memset(s_wcTicks, 0, sizeof(s_wcTicks));
}
#endif

#ifdef CTR_INTERNAL
/* [OTSKIP] totals since the last line: walks, slots consumed by the skip,
 * nodes the walk loop visited, and how many of those were zero-length nodes
 * passed without the full node path. */
global_variable u32 s_otSkipWalks;
global_variable u32 s_otSkipSlots;
global_variable u32 s_otSkipVisited;
global_variable u32 s_otSkipEmpty;
#endif

internal u8 *NativeGpu_SkipUntouchedOTSlots(u8 *node)
{
	const uintptr_t base = s_gpu.walkOtBase;
	const uintptr_t addr = (uintptr_t)node;
	const u8 *slots = (const u8 *)base;
	u32 first;
	u32 idx;
	u32 expect;
	u32 run;

	if ((addr < base) || (addr >= base + (uintptr_t)s_gpu.walkOtCount * 4u) || ((addr & 3u) != 0))
	{
		return node;
	}

	first = (u32)((addr - base) >> 2);
	idx = first;
	while (idx >= 1u)
	{
		expect = s_gpu.walkOtToken + (idx - 1u) * 4u;
		if (CTR_GPU_ReadTagWord(slots + (uintptr_t)idx * 4u) != expect)
		{
			break;
		}
		idx--;
	}

	if (idx == first)
	{
		return node;
	}

	/* Slots first down to idx + 1 were consumed, in walk order. */
	run = first - idx;
	s_gpu.otSlotsCrossed += run;
#ifdef CTR_INTERNAL
	s_otSkipSlots += run;
#endif

	if (s_gpu.otRecorderActive)
	{
		const u32 lo = idx + 1u;
		const u32 hi = (first < OTZ_TABLE_SLOTS) ? first : (OTZ_TABLE_SLOTS - 1u);

		if (lo <= hi)
		{
			const u16 ordinal = (u16)(s_gpu.emitOrdinal < 0xFFFFu ? s_gpu.emitOrdinal : 0xFFFFu);

			for (u32 slot = lo; slot <= hi; slot++)
			{
				s_gpu.otBucketOrdinal[slot] = ordinal;
			}
			s_gpu.otSlotsDetected += hi - lo + 1u;
			s_gpu.otCurrentSlotIdx = lo;
			if (hi > s_gpu.otMaxSlotIdx)
			{
				s_gpu.otMaxSlotIdx = hi;
			}
		}
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	GTECapture_MarkOTSlotRun(slots + (uintptr_t)(idx + 1u) * 4u, slots + (uintptr_t)(first + 1u) * 4u);
#endif

#ifdef CTR_NATIVE_OTWALK_PROFILE
	s_otprof.frameNodes += run;
#endif

	return (u8 *)(slots + (uintptr_t)idx * 4u);
}

/*
 * The walk effects of a zero-length node: an OT slot (one with content linked
 * in, or the terminator) or a len-0 filler packet. v1 conflated the two;
 * primMem sits contiguous with the OT arena, so only the game's own ClearOTag
 * bounds discriminate (measured, [OTZ2]). slotIdx is the plain array index,
 * the retail otIndex GetProjectedOtIndex computes from depth.
 */
internal void NativeGpu_RecordEmptyNode(const u8 *node)
{
	s_gpu.otSlotsCrossed++;

	/* Read the SAME latched value the walk's clear used -- never re-evaluate
	 * g_dbg_otzWiden / NATIVE_GPU_TRACK_RENDER_ACTIVE live here, see
	 * otRecorderActive's field comment for why. */
	if (s_gpu.otRecorderActive)
	{
		uintptr_t addr = (uintptr_t)node;

		if ((s_gpu.walkOtBase != 0) && (addr >= s_gpu.walkOtBase) && (addr < s_gpu.walkOtBase + (uintptr_t)s_gpu.walkOtCount * 4u))
		{
			u32 slotIdx = (u32)((addr - s_gpu.walkOtBase) >> 2);

			if (slotIdx < OTZ_TABLE_SLOTS)
			{
				s_gpu.otBucketOrdinal[slotIdx] = (u16)(s_gpu.emitOrdinal < 0xFFFFu ? s_gpu.emitOrdinal : 0xFFFFu);
				s_gpu.otSlotsDetected++;

				/* Depth-compose fix: remember which bucket the walk is
				 * passing through right now, so a later content advance
				 * (NativeGpu_RecordContentAdvance) knows which bucket to
				 * attribute itself to. Slots are visited in descending
				 * order, so whichever bucket was crossed most recently is
				 * simply the current one -- no comparison needed here, only
				 * at the point that reads it. */
				s_gpu.otCurrentSlotIdx = slotIdx;

				if (slotIdx > s_gpu.otMaxSlotIdx)
				{
					s_gpu.otMaxSlotIdx = slotIdx;
				}
			}
		}
		else
		{
			s_gpu.otSpuriousLen0++;
		}
	}
}

void ParsePrimitivesLinkedList(u32 *p, int singlePrimitive)
{
	if (!p)
	{
		return;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);

#ifdef CTR_NATIVE
	if (!singlePrimitive && !NativeGpuLinks_IsRegisteredHostPointer(p) && !isendprim(p))
	{
		char packetRegion[64];
		NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)p);
		NATIVE_GPU_ERROR("unregistered linked DrawOTag packet: packet=%p region=%s addr=%06x len=%d code=%02x words=%08x %08x %08x %08x\n", (void *)p,
		                 packetRegion, getaddr(p), getlen(p), getcode(p), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 0),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 2),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 3));
		NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);
		return;
	}
#endif

	// setup single primitive flag (needed for AddSplits)
	s_gpu.drawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG *polyTag = (P_TAG *)p;
		const int firstVertex = s_gpu.vertexIndex;
		ParsePrimitive(polyTag);
		NativeGpu_StampEmitOrdinal(firstVertex);

		GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
		lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;
	}
	else
	{
		// walk OT_TAG linked list
		u8 *basePacket = (u8 *)p;

#ifdef CTR_NATIVE_OTWALK_PROFILE
		OTProf_DryWalk(basePacket);
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		/* Gate widened from VERTEX_DESNAP to GTE_CAPTURE: the UI band
		 * describes the capture TABLE -- which screen coordinates in it are
		 * trustworthy -- not the one feature that read it first. Desnap builds
		 * are unaffected (CMake forces CAPTURE on for them), and any other
		 * consumer, starting with the stamp-time depth probe below, needs the
		 * same band or it counts UI packets as world geometry. */
		GTECapture_BeginOTWalk();
		/* UI-band ordinal + the flag's latched stamp: same walk scope, same
		 * instant. See PacketDepth_BeginWalk. */
		PacketDepth_BeginWalk();
#endif

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_BAKE_PARITY) && CTR_NATIVE_BAKE_PARITY
		/* Bake-parity: (re)index the latched track mesh and emit the periodic
		 * [BAKEPAR] report. Same walk-side placement as the capture publish
		 * above -- the taps in the poly cases below read the capture buffer
		 * this walk was handed. */
		BakeParity_BeginWalk();
#endif

		/*
		 * This loop's only exits are the terminator tag (isendprim) and the
		 * invalid-link check below -- neither catches a genuine CYCLE among
		 * otherwise-legitimate, individually-registered pointers, which would
		 * pass NativeGpu_IsValidOTLink forever and spin with no log output.
		 * Real OTs are at most a few thousand entries, so a 65536-iteration
		 * cap turns an unrecoverable freeze into a diagnosable one at
		 * negligible cost.
		 */
		u32 otWalkIterations = 0;

		/* v2 recorder: resolve which registered OT this walk belongs to
		 * (the head must lie inside a ClearOTag'd range) and restart the
		 * per-walk counters. The table itself is only cleared while the
		 * instrument is on -- recording costs one compare per zero-length
		 * node otherwise. */
		s_gpu.otHead = (uintptr_t)p;
		s_gpu.otSlotsCrossed = 0;
		s_gpu.otSlotsDetected = 0;
		s_gpu.otSpuriousLen0 = 0;
		s_gpu.otMaxSlotIdx = 0;
		s_gpu.walkOtBase = 0;
		s_gpu.walkOtCount = 0;

		for (u32 rangeIdx = 0; rangeIdx < 8; rangeIdx++)
		{
			uintptr_t base = s_gpu.otRanges[rangeIdx].base;
			u32 count = s_gpu.otRanges[rangeIdx].count;

			if ((base != 0) && (s_gpu.otHead >= base) && (s_gpu.otHead < base + (uintptr_t)count * 4u))
			{
				s_gpu.walkOtBase = base;
				s_gpu.walkOtCount = count;
				break;
			}
		}

		s_gpu.walkOtSkip = 0;
		if ((s_gpu.walkOtBase != 0) && ((s_gpu.walkOtBase & 3u) == 0) && (s_gpu.walkOtCount >= 2))
		{
			u32 token;

			if (NativeGpuLinks_TokenForHostSpan((const void *)s_gpu.walkOtBase, (size_t)s_gpu.walkOtCount * 4u, &token))
			{
				s_gpu.walkOtToken = token;
				s_gpu.walkOtSkip = 1;
			}
		}

		/* Recording (not just the standalone 'otz' instrument) also runs when
		 * the track renderer is active -- the depth compose (design 4.3)
		 * consumes this same table. Latch the condition ONCE here, at the top
		 * of the walk, into otRecorderActive rather than re-evaluating the OR
		 * expression at the clear and again at every zero-length node (review
		 * fix, FIX 7): this walk can run on the worker thread while the game
		 * thread flips g_dbg_trackRender from a button/dumpreq handler at any
		 * time, and re-reading the live globals at two different points in
		 * the same walk let a toggle flip land BETWEEN them -- clear skipped
		 * (condition false), then recording ON moments later at the first
		 * node (condition now true), which fills a table that was never
		 * cleared this walk. That is the exact stale-ordinal failure mode
		 * the clear-must-match-record comment already warns about, just
		 * reintroduced through a mid-walk toggle flip instead of a code
		 * mismatch. A single latch read at one instant closes that window.
		 * [OTZ2] below stays tied to g_dbg_otzWiden alone -- that log line is
		 * the standalone instrument's own validation output, and turning the
		 * track renderer on should not start spamming it. */
		s_gpu.otRecorderActive = (g_dbg_otzWiden || NATIVE_GPU_TRACK_RENDER_ACTIVE) ? 1 : 0;

		if (s_gpu.otRecorderActive)
		{
			/* Fill with the sentinel, not 0 -- see
			 * NATIVE_GPU_OTZ_SENTINEL's comment for why 0 is ambiguous with a
			 * real recorded ordinal. memset's byte value 0xFF replicated
			 * across both bytes of every u16 entry gives exactly 0xFFFF. */
			memset(s_gpu.otBucketOrdinal, 0xFF, sizeof(s_gpu.otBucketOrdinal));

			/* Per-walk reset for the depth-compose fix -- "no bucket crossed
			 * yet" / "no content seen yet this walk". See the field
			 * comments on NativeGpuState. Gated on otRecorderActive same as
			 * the memset above, so a build/toggle with the feature off pays
			 * nothing. */
			s_gpu.otCurrentSlotIdx = NATIVE_GPU_NO_CONTENT_BUCKET;
			s_gpu.minContentBucket = NATIVE_GPU_NO_CONTENT_BUCKET;
		}

		while (true)
		{
			if (s_gpu.walkOtSkip)
			{
				basePacket = NativeGpu_SkipUntouchedOTSlots(basePacket);
			}

#ifdef NATIVE_WALK_CENSUS
			const u64 wcStart = SDL_GetPerformanceCounter();
			int wcDirect = 0;
#endif

			otWalkIterations++;
			if (otWalkIterations > 65536)
			{
				char packetRegion[64];
				char startRegion[64];

				NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
				NativeGpu_FormatPointerRegion(startRegion, sizeof(startRegion), (uintptr_t)p);
				NATIVE_GPU_ERROR(
				    "OT WALK DID NOT TERMINATE after %lu iterations -- likely a cycle. start=%p startRegion=%s "
				    "current=%p currentRegion=%s addr=%06x len=%d code=%02x words=%08x %08x %08x %08x\n",
				    (unsigned long)otWalkIterations, (void *)p, startRegion, (void *)basePacket, packetRegion, getaddr(basePacket), getlen(basePacket),
				    getcode(basePacket), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1),
				    NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				Platform_Log("[CTR GPU] OT WALK CYCLE after %lu iters, current=%p addr=%06x len=%d code=%02x\n", (unsigned long)otWalkIterations,
				             (void *)basePacket, getaddr(basePacket), getlen(basePacket), getcode(basePacket));
				break;
			}

			const int tagLength = getlen(basePacket);

#ifdef CTR_NATIVE_OTWALK_PROFILE
			OTProf_RecordNode(basePacket, (u32)tagLength);
			OTProf_RecordNodeShape(basePacket, (u32)tagLength);
#endif

			/* A zero-length node with a valid onward link emits nothing, so of
			 * the full node path below it needs only the slot record, the UI
			 * band entry and the link. The per-node packet flags and the split's
			 * vertex count are skipped: every node that emits recomputes the
			 * flags before it draws and stores the count after, and the walk
			 * ends on the full path (terminator or bad link), which does both.
			 * Cycle protection is unchanged: these nodes still count as
			 * iterations. */
			if ((tagLength == 0) && !isendprim(basePacket))
			{
				u8 *nextPacket = nextPrim(basePacket);

				if (NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
				{
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
					GTECapture_MarkOTEmptyNode(basePacket);
#endif
					NativeGpu_RecordEmptyNode(basePacket);
#ifdef CTR_INTERNAL
					s_otSkipEmpty++;
#endif
#ifdef NATIVE_WALK_CENSUS
					WalkCensus_RecordNode(basePacket, 0, 0, SDL_GetPerformanceCounter() - wcStart);
#endif
					basePacket = nextPacket;
					continue;
				}
			}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
			GTECapture_MarkOTNode(basePacket); /* gate widened with BeginOTWalk above */
			/* Sky-class tag (backdrop union or star slice), and the menu-backdrop
			 * tag beside it: set before the direct-emit fast path AND the parse
			 * path below, so whichever one resolves this node's depth sees it.
			 * Self-resetting -- 0 for every non-sky node -- so no per-walk clear.
			 * (Verified the range does NOT over-catch UI-band nodes in-race:
			 * a walk-order guard here counted zero rescues across a full test run
			 * -- the minimap black box was the sky PASS clobbering the open blend
			 * run's state memo, fixed in N3DS_DrawSkyPass.) */
			s_skyNodeActive = NativeGpu_IsSkyPacket((uintptr_t)basePacket) || NativeGpu_IsStarPacket((uintptr_t)basePacket);
			s_menubdNodeActive = NativeGpu_IsMenuBackdropPacket((uintptr_t)basePacket);
			s_glowNodeActive = NativeGpu_IsGlowPacket((uintptr_t)basePacket);
			s_uimapNodeActive = NativeGpu_IsUiMapPacket((uintptr_t)basePacket);
			s_uifpsNodeActive = NativeGpu_IsUiFpsPacket((uintptr_t)basePacket);
			s_uipicNodeActive = NativeGpu_IsUiPicPacket((uintptr_t)basePacket);
			s_hudshineNodeActive = NativeGpu_IsHudShinePacket((uintptr_t)basePacket);
			s_screenfadeNodeActive = NativeGpu_IsScreenFadePacket((uintptr_t)basePacket);
#endif
			if (tagLength > 0)
			{
				if (tagLength > 32)
				{
					char packetRegion[64];
					NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
					NATIVE_GPU_ERROR("got invalid tag length %d, code %d packet=%p region=%s words=%08x %08x %08x %08x\n", tagLength,
					                 ((P_TAG *)basePacket)->code, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
					break;
				}

#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT
				// Fast path: an all-opaque-poly node goes packet -> merge bin
				// directly (3DS; PC builds only count and always parse). A
				// handled node emitted its triangles and advanced the emit
				// ordinals itself, so the parse loop must not see it. See
				// native_direct_emit.c.
				if (NativeDirectEmit_TryNode(basePacket, (u32)tagLength))
				{
					// handled: nothing to parse
#ifdef NATIVE_WALK_CENSUS
					wcDirect = 1;
#endif
				}
				else
#endif
				{
					u8 *currentPacket = basePacket;
					u8 *endPacket = basePacket + (tagLength + P_LEN) * sizeof(u32);
					int primLength = 0;
					int firstVertex = s_gpu.vertexIndex;
					if (currentPacket < endPacket)
					{
						primLength = ParsePrimitive((P_TAG *)currentPacket);
						currentPacket += (primLength + P_LEN) * sizeof(u32);
						NativeGpu_StampEmitOrdinal(firstVertex);
						NativeGpu_RecordContentAdvance();
					}

					while (currentPacket < endPacket)
					{
						firstVertex = s_gpu.vertexIndex;
						primLength = ParseTaglessPrimitive((u32 *)currentPacket);
						currentPacket += primLength * sizeof(u32);
						NativeGpu_StampEmitOrdinal(firstVertex);
						NativeGpu_RecordContentAdvance();
					}

					if (currentPacket != endPacket)
					{
						char packetRegion[64];
						NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
						NATIVE_GPU_ERROR("did not output valid primitive or ptag length is not valid (diff=%d packet=%p region=%s words=%08x %08x %08x %08x)\n",
						                 endPacket - currentPacket, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
						                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
						                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
					}
				}
			}
			else
			{
				/* Zero-length node on the full path: the terminator, or one whose
				 * link the check below rejects. */
				NativeGpu_RecordEmptyNode(basePacket);
			}

			GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
			lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;

			if (isendprim(basePacket))
			{
				break;
			}

			u8 *nextPacket = nextPrim(basePacket);
			if (!NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
			{
				char packetRegion[64];
				char nextRegion[64];
				NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
				NativeGpu_FormatPointerRegion(nextRegion, sizeof(nextRegion), (uintptr_t)nextPacket);
				NATIVE_GPU_ERROR("invalid OT link: packet=%p region=%s addr=%06x next=%p nextRegion=%s len=%d code=%02x words=%08x %08x %08x %08x\n",
				                 (void *)basePacket, packetRegion, getaddr(basePacket), (void *)nextPacket, nextRegion, getlen(basePacket), getcode(basePacket),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				break;
			}

#ifdef NATIVE_WALK_CENSUS
			WalkCensus_RecordNode(basePacket, tagLength, wcDirect, SDL_GetPerformanceCounter() - wcStart);
#endif

			basePacket = nextPacket;
		}
#ifdef NATIVE_WALK_CENSUS
		WalkCensus_EndWalk();
#endif

#ifdef CTR_INTERNAL
		s_otSkipWalks++;
		s_otSkipVisited += otWalkIterations;
		if (s_otSkipWalks >= 1024u)
		{
			Platform_LogInstrument("[OTSKIP] walks=%u skipped/walk=%u visited/walk=%u empty/walk=%u\n", s_otSkipWalks, s_otSkipSlots / s_otSkipWalks,
			                       s_otSkipVisited / s_otSkipWalks, s_otSkipEmpty / s_otSkipWalks);
			s_otSkipWalks = 0;
			s_otSkipSlots = 0;
			s_otSkipVisited = 0;
			s_otSkipEmpty = 0;
		}
#endif
	}

	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);
}

/*
 * Track-renderer accessor (design 12/12b): per-draw CPU-side
 * lookup of the bucket->ordinal table the walk above records into
 * s_gpu.otBucketOrdinal. Step 2's draw side looks up a leaf's OT bucket ONCE
 * PER DRAW (not per fragment) to compose its depth against the packet path's
 * ordinals, so this stays a narrow read accessor, not a bulk copy.
 *
 * Returns 0 (nothing written to the outputs) when
 * the table has nothing trustworthy for this slot:
 *   - the recorder was not latched active for the LAST walk (otRecorderActive
 *     false) -- previously this accessor ignored that entirely and reported
 *     success reading whatever garbage/stale table contents happened to be
 *     sitting in otBucketOrdinal, since walkOtBase/walkOtCount are resolved
 *     on EVERY walk regardless of the recorder condition (they're just OT
 *     bounds lookup, unconditional); now checked explicitly;
 *   - no walk has run yet this depth-clear (walkOtCount == 0), or slotIdx is
 *     outside the walk's own OT size;
 *   - the slot still holds NATIVE_GPU_OTZ_SENTINEL, i.e. it was never
 *     crossed this walk -- previously a cleared-to-zero table made "crossed
 *     at ordinal 0" and "never crossed" both read back as 0, silently hand-
 *     ing step 2's depth compose a plausible but meaningless bucket depth
 *     for content that was never actually on this frame's OT.
 * Returns 1 otherwise, with *outOrdinal holding the recorded emit ordinal for
 * that slot and *outWalkOtCount holding the walk's total slot count (so a
 * caller can sanity-check its own otIndex range against the same OT the
 * recorder saw).
 */
int NativeGpu_GetOtBucketOrdinal(u32 slotIdx, u16 *outOrdinal, u32 *outWalkOtCount)
{
	if (!s_gpu.otRecorderActive)
	{
		return 0;
	}

	if ((s_gpu.walkOtCount == 0) || (slotIdx >= s_gpu.walkOtCount) || (slotIdx >= OTZ_TABLE_SLOTS))
	{
		return 0;
	}

	if (s_gpu.otBucketOrdinal[slotIdx] == (u16)NATIVE_GPU_OTZ_SENTINEL)
	{
		return 0;
	}

	*outOrdinal = s_gpu.otBucketOrdinal[slotIdx];
	*outWalkOtCount = s_gpu.walkOtCount;
	return 1;
}

/*
 * Track-renderer accessor (docs/
 * NATIVE_TRACK_RENDERER_DESIGN.md 4.3): the lowest OT bucket index that held
 * real primitive content during the last walk the recorder ran for -- see
 * minContentBucket's field comment on NativeGpuState for why
 * otBucketOrdinal alone (which records every crossed bucket, content or not)
 * is not sufficient for this. Same failure contract as
 * NativeGpu_GetOtBucketOrdinal: returns 0 (nothing written to
 * *outMinContentBucket) when the recorder was not latched active for the
 * last walk, or no content-advance was ever recorded this walk (a genuinely
 * empty walk, or still holding NATIVE_GPU_NO_CONTENT_BUCKET). Returns 1
 * otherwise with *outMinContentBucket valid.
 */
int NativeGpu_GetMinContentBucket(u32 *outMinContentBucket)
{
	if (!s_gpu.otRecorderActive)
	{
		return 0;
	}

	if (s_gpu.minContentBucket == NATIVE_GPU_NO_CONTENT_BUCKET)
	{
		return 0;
	}

	*outMinContentBucket = s_gpu.minContentBucket;
	return 1;
}

/*
 * DIAGNOSTIC ONLY (uncommitted occlusion investigation, no behavior
 * change) -- see the header comment (include/platform/native_gpu.h) for the
 * full contract. emitOrdinal is the NEXT ordinal NativeGpu_StampEmitOrdinal
 * will hand out, so the highest ordinal actually STAMPED this frame is one
 * less; guard against underflow on a frame that stamped nothing (emitOrdinal
 * still 0 after NativeGpu_ResetEmitOrdinal, e.g. a menu frame with no OT
 * walk at all).
 */
u32 NativeGpu_GetFrameEmitOrdinal(void)
{
	return (s_gpu.emitOrdinal > 0) ? (s_gpu.emitOrdinal - 1) : 0;
}

internal inline int IsNull(POLY_FT3 *poly)
{
	return poly->x0 == -1 && poly->y0 == -1 && poly->x1 == -1 && poly->y1 == -1 && poly->x2 == -1 && poly->y2 == -1;
}

internal int ProcessFlatLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2 *poly = (LINE_F2 *)polyTag;

		AddSplit(semiTrans, false, false);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = c0;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3 *poly = (LINE_F3 *)polyTag;

		AddSplit(semiTrans, false, false);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 5;
	}
	case 0xc:
	{
		LINE_F4 *poly = (LINE_F4 *)polyTag;

		AddSplit(semiTrans, false, false);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x2;
			VERTTYPE *p1 = &poly->x3;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 6;
	}
	}
	return 0;
}

internal int ProcessGouraudLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2 *poly = (LINE_G2 *)polyTag;

		AddSplit(semiTrans, false, false);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = &poly->r1;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

internal int ProcessFlatPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3 *poly = (POLY_F3 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		s_gpu.vertexIndex += 3;

		return 4;
	}
	case 0x4:
	{
		POLY_FT3 *poly = (POLY_FT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
#if defined(CTR_NATIVE) && defined(CTR_NATIVE_BAKE_PARITY) && CTR_NATIVE_BAKE_PARITY
			BakeParity_NotePolyFT3(poly);
#endif
			AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
			                     GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			s_gpu.vertexIndex += 3;
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4 *poly = (POLY_F4 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;
		return 5;
	}
	case 0xC:
	{
		POLY_FT4 *poly = (POLY_FT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_BAKE_PARITY) && CTR_NATIVE_BAKE_PARITY
		BakeParity_NotePolyFT4(poly);
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		/* s42d: Particle.c registered this packet's address as driver-local
		 * exhaust smoke (NativeGpu_MarkSmokePacket). Route it to its own split,
		 * held out of the merge for a redraw after the model pass. The one-shot
		 * is consumed by the AddSplit immediately below. Address-keyed, not a
		 * code bit -- the code byte has no free bits (0x01 is raw-texture). */
		if (NativeGpu_IsSmokePacket((uintptr_t)poly))
		{
			s_ngpuDeferSmokeNext = 1;
		}
#endif
		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 9;
	}
	}
	return 0;
}

internal int ProcessGouraudPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3 *poly = (POLY_G3 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 6;
	}
	case 0x4:
	{
		POLY_GT3 *poly = (POLY_GT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_BAKE_PARITY) && CTR_NATIVE_BAKE_PARITY
		BakeParity_NotePolyGT3(poly);
#endif
		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 9;
	}
	case 0x8:
	{
		POLY_G4 *poly = (POLY_G4 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 8;
	}
	case 0xC:
	{
		POLY_GT4 *poly = (POLY_GT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_BAKE_PARITY) && CTR_NATIVE_BAKE_PARITY
		BakeParity_NotePolyGT4(poly);
#endif
		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 12;
	}
	}
	return 0;
}

internal int ProcessTileAndSprt(P_TAG *polyTag)
{
	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE *poly = (TILE *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x64:
	{
		SPRT *poly = (SPRT *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x68:
	{
		TILE_1 *poly = (TILE_1 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x70:
	{
		TILE_8 *poly = (TILE_8 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x74:
	{
		SPRT_8 *poly = (SPRT_8 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x78:
	{
		TILE_16 *poly = (TILE_16 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x7C:
	{
		SPRT_16 *poly = (SPRT_16 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	}
	return 0;
}

internal int ProcessDrawEnv(P_TAG *polyTag)
{
	const u32 *codePtr = (u32 *)&polyTag->pad0;
	int processedLongs = 0;
	bool fullDrawEnvPacket = false;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u32 code = codePtr[i];
		const int primType = code >> 24 & 0xF0;
		const int primSubType = code >> 24 & 0x0F;

		// NOTE(aalhendi): CTR can pack draw-env commands, tagless geometry,
		// and more draw-env commands into one OT entry. Stop at the first
		// non-E command so ParseTaglessPrimitive owns the geometry payload.
		if (primType != 0xE0)
		{
			return processedLongs;
		}

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			// NOTE(aalhendi): Standalone DR_TPAGE packets use the same E1 word
			// for blend changes; only full DRAWENV packets retarget native
			// on-screen/offscreen rendering.
			if (fullDrawEnvPacket)
			{
				activeDrawEnv.dfe = (code >> 10) & 1;
			}
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			fullDrawEnvPacket = true;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
			activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
			fullDrawEnvPacket = true;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
			activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
			fullDrawEnvPacket = true;
			break;
		}
		case 0x6:
		{
			SetPSXMaskState(code);
			break;
		}
		case 0:
			// NOTE(aalhendi): ctr-native local divergence for CTR OTs. A zero
			// word can be terminal draw-env padding, or the tag word for the
			// next primitive packed into the same OT entry.
			// return processedLongs;
			if (i + 1 != polyTag->len)
			{
				return processedLongs;
			}
			break;
		}
		++processedLongs;
	}

	return processedLongs;
}

internal void ProcessDrawEnvCommand(u32 code)
{
	const int primSubType = code >> 24 & 0x0F;

	switch (primSubType)
	{
	case 0x1:
		activeDrawEnv.tpage = (code & 0x1FF);
		activeDrawEnv.dtd = (code >> 9) & 1;
		break;
	case 0x2:
		activeDrawEnv.tw.w = (code & 0x1F);
		activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
		activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
		activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
		break;
	case 0x3:
		activeDrawEnv.clip.x = code & 1023;
		activeDrawEnv.clip.y = (code >> 10) & 1023;
		break;
	case 0x4:
		activeDrawEnv.clip.w = code & 1023;
		activeDrawEnv.clip.h = (code >> 10) & 1023;
		activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
		activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
		break;
	case 0x5:
		activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
		activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
		break;
	case 0x6:
		SetPSXMaskState(code);
		break;
	}
}

internal int ProcessPsyXPrims(P_TAG *polyTag)
{
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX *psytex = (DR_PSYX_TEX *)polyTag;
		s_gpu.overrideTexture = psytex->code[0] & 0xFFFFFF;
		s_gpu.overrideTextureWidth = psytex->code[1] & 0xFFF;
		s_gpu.overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom debug marker packet
		DR_PSYX_DBGMARKER *psydbg = (DR_PSYX_DBGMARKER *)polyTag;
		s_gpu.currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG *polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;
	bool handledZeroLength = false;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		const u32 *codePtr = (u32 *)&polyTag->pad0;
		// NOTE(aalhendi): ctr-native local divergence. CTR RenderWeather can
		// emit a retail length-2 zero packet when weather is enabled but the
		// level has no fill-mode payload. The PSX consumes it by tag length;
		// the native parser must advance past it too.
		if (polyTag->len == 2 && codePtr[0] == 0 && codePtr[1] == 0)
		{
			primLength = 2;
		}
		else if (polyTag->len == 0 && *(u32 *)polyTag == 0)
		{
			// CTR ghost transparency packets include raw GPU NOP words between
			// draw-mode changes and triangle commands. They consume exactly one
			// command word; ParsePrimitivesLinkedList adds P_LEN to the return.
			handledZeroLength = true;
		}
		else if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE *drmove = (DR_MOVE *)polyTag;
			const u32 rectPos = drmove->code[2];
			const u32 rectSize = drmove->code[4];

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
			MoveImage(&rect, x, y);
			primLength = 5;
		}
		else if (primSubType == 0x2)
		{
			// NOTE(aalhendi): ctr-native local divergence. CTR emits retail
			// FILL packets in OTs; the old PsyCross parser did not consume them, which caused
			// zero-length primitive spam.
			TILE *fill = (TILE *)polyTag;
			RECT16 rect;

			rect.x = fill->x0;
			rect.y = fill->y0;
			rect.w = fill->w;
			rect.h = fill->h;

			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
			ClearImage(&rect, fill->r0, fill->g0, fill->b0);
			primLength = 3;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD *drload = (DR_LOAD *)polyTag;
			const u32 rectPos = drload->code[1];
			const u32 rectSize = drload->code[2];

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

			LoadImage(&rect, (uint32_t *)drload->p);

			// TODO(aalhendi): Audit whether CTR ever appends additional GPU
			// commands after a DR_LOAD payload in the same packet.
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
		// default:
		//	NATIVE_GPU_ERROR("got %0x primitive\n", primType);
	}

	if (primLength == 0 && !handledZeroLength)
	{
		NATIVE_GPU_ERROR("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}

int ParseTaglessPrimitive(u32 *command)
{
	const u32 code = *command;
	const int primType = (code >> 24) & 0xF0;

	if (code == 0)
	{
		return 1;
	}

	if (primType == 0xE0)
	{
		ProcessDrawEnvCommand(code);
		return 1;
	}

	P_TAG *polyTag = (P_TAG *)(command - P_LEN);
	int primLength = ParsePrimitive(polyTag);

	if (primLength == 0)
	{
		NATIVE_GPU_ERROR("Unhandled tagless primitive %08x\n", code);
		return 1;
	}

	return primLength;
}

/*
 * Widescreen: how much wider (PSX px, per side) a game-side screen-bounds
 * cull should accept than the retail viewport. Non-zero only while the 3DS
 * REAL-wide ortho window is live, and exactly its extension -- rectW *
 * (400-320)/(2*320) = rectW/8, the -64..576 window at rectW=512 -- so what
 * the culls accept and what the window shows stay one number. Callers:
 * RenderBucket_AcceptProjectedBounds (instance queue), DrawSky's
 * second-chance face test, and PushBuffer_UpdateFrustum (BSP frustum corner
 * X -- the most upstream widened cull; everything the BSP walk rejects is
 * invisible to every later gate). Called on the game thread; the flag is a
 * plain int flipped by the same thread's button/dumpreq poll. PC: always 0,
 * every retail comparison is restored verbatim.
 */
int NativeWide_CullMarginX(int rectW)
{
#if defined(__3DS__)
	extern int g_dbg_widescreen; /* n3ds_backend.c (separate TU, link-visible) */

	if (g_dbg_widescreen == 2)
	{
		return rectW >> 3;
	}
#else
	(void)rectW;
#endif
	return 0;
}

/*
 * Strips hunt: per-mechanism kill switches for the widescreen margin
 * machinery, so a live repro can bisect which mechanism draws garbage
 * without rebuilding. Bits: 1 = instance bbox margin
 * (RenderBucket_AcceptProjectedBounds), 2 = per-triangle margin
 * (RenderBucket_TriangleInWideMargin), 4 = sky per-face margin
 * (DrawSky_IsVisibleWideMargin), 8 = sky extra yaw sectors (DrawSky_Full),
 * 16 = BSP frustum corner X (PushBuffer_UpdateFrustum -- widens both the
 * frustum planes and pb->bbox, so a kill here re-culls whole BSP leaves).
 * Default all-on. Set via dumpreq `widemask N` (n3ds_backend.c). Non-static:
 * written by the backend TU's dumpreq poll, read here and by the game-side
 * call sites.
 */
int g_dbg_wideMarginMask = 0x1F;

int NativeWide_MarginClassOn(int bit)
{
	return (g_dbg_wideMarginMask & bit) != 0;
}

/*
 * Widescreen UI relocation (owner request): remap a UI-band vertex's
 * screen X so the HUD occupies the wide window instead of floating at the
 * old 4:3 edges. Piecewise-linear ramp about screen centre:
 *
 *   shift(x) = clamp((x - 256) / 3, -m, +m),  m = the wide margin (64)
 *
 * saturating at x <= 64 / x >= 448 -- so edge-anchored elements (timer,
 * lap counter, minimap, portraits) translate RIGIDLY by the full margin
 * with zero distortion, centre elements stay put, and anything in between
 * moves proportionally (stretch bounded by width/3). Per-VERTEX and
 * continuous, so shared vertices of adjacent triangles always agree -- no
 * cracks inside sprites; full-width quads (menu backgrounds, pause dim)
 * stretch to fill the window, which is the desired look for those.
 * Returns x unchanged whenever the wide window is off, so PC and 4:3
 * behavior are untouched. Callers gate on the UI-band depth stamp
 * (raw >= 0xFFC0, the same d <= 63 band ctr.v.pica's stereo mask uses)
 * and on packet depth being live (with it off, stamps are emit ordinals
 * and the band test would be meaningless).
 */
int NativeWide_UiShiftX(int x)
{
	const int m = NativeWide_CullMarginX(512); /* 64 while REAL wide, else 0 */
	int shift;

	if (m == 0)
	{
		return x;
	}
	shift = (x - 256) / 3;
	if (shift < -m)
	{
		shift = -m;
	}
	if (shift > m)
	{
		shift = m;
	}
	return x + shift;
}

/*
 * s46: stretch CAM_SkyboxGlow's screen-space verts to the wide ortho window.
 * The glow's ASM-verified emitter spans x 0..512 by construction, so REAL
 * wide (-64..576) showed bare backdrop in the margins wherever no dome
 * geometry covers them (dome-less levels like Polar Pass most of all).
 * Linear about screen centre -- x=0 lands on -m, x=512 on 512+m -- which is
 * exact for the untilted horizontal bands and imperceptibly relaxes the
 * tilt slope inside the margins for the tilted cases. Callers gate on the
 * glow span (NativeGpu_IsGlowPacket / s_glowNodeActive), never the whole
 * sky span: the dome is projected 3D geometry that already reaches the
 * margins on its own and must not be distorted. Identity whenever the wide
 * window is off, so PC and 4:3 output are untouched.
 */
int NativeWide_GlowStretchX(int x)
{
	int m = NativeWide_CullMarginX(512); /* 64 while REAL wide, else 0 */

#ifdef __3DS__
	if (m != 0)
	{
		/* Stereo shifts far-pinned content horizontally by up to the far-plane
		 * parallax per eye; stretch that much past the window edge so neither
		 * eye can pull the gradient's edge on-screen. Mono clips the overshoot
		 * -- the wide window edge IS the physical screen edge. */
		extern int N3DS_StereoSkyPadX(void);

		m += N3DS_StereoSkyPadX();
	}
#endif
	return x + (((x - 256) * m) / 256);
}

/* ---------------- bottom-screen minimap: game-side hooks ---------------- */
/* Called unconditionally from game/ (UI_RenderFrame_Racing, MainMain) via the
 * local-extern hook idiom; every PC leg returns 0, so retail behavior there is
 * byte-identical. */

/* Nonzero while the HUD shine's far-edge UV inset is live: the shine is
 * stamped into the UI band (g_dbg_hudShine) and the stamp the inset keys on
 * exists, which needs packet depth and the ordered-UI pin owning the band.
 * UI_WeaponBG_DrawShine then makes its mirrored halves tile exactly instead
 * of overlapping by one unit per whole unit of scale: that overlap hid the
 * PS1's past-edge first column, and under the inset it draws the seam column
 * twice. PC returns 0 and keeps retail's geometry. */
int NativeUiShine_ExactTiling(void)
{
#if defined(__3DS__)
	return g_dbg_hudShine && g_dbg_packetDepth;
#else
	return 0;
#endif
}

/* Nonzero when the resolver is actually restamping the map block for the
 * bottom screen (PKTDEPTH_UI_STAMP_BTMMAP). The game uses this to run the map
 * block even in speedometer mode: routed primitives never reach the top
 * screen, so retail's map-or-speedo exclusivity is preserved up there while
 * the map still feeds the bottom panel. */
int NativeUiMap_RouteActive(void)
{
#if defined(__3DS__)
	extern int g_dbg_btmMap;

	return g_dbg_btmMap && g_dbg_packetDepth;
#else
	return 0;
#endif
}

/* HudAndDebugFlags bits OR'd in once at boot (MainMain). Bit 8 = speedometer
 * mode: with the map on the bottom screen, the top screen's map slot is empty,
 * so the speedometer is the better retail-shaped default there. Retail (and
 * PC) boot to map mode; Triangle still toggles. */
int NativeUiMap_BootHudFlags(void)
{
#if defined(__3DS__)
	return 8;
#else
	return 0;
#endif
}

/* Fills buf with the FPS overlay drawn inside the map block, in the game's
 * own HUD font -- it allocates from the same bracketed primMem span as the
 * map, so it routes to the bottom screen with it and shares its window.
 * Returns 0 (draw nothing) until the first measurement window lands, whenever
 * routing is off, and always on PC. */
int NativeUiMap_FpsOverlayText(char *buf, int bufSize)
{
#if defined(__3DS__)
	extern unsigned long g_n3dsFpsX10;
	extern int g_dbg_tireVtxLog;
	extern unsigned long g_tirelogFrameNow;

	if (!NativeUiMap_RouteActive() || (g_n3dsFpsX10 == 0))
	{
		return 0;
	}
	/* Invisible-tire hunt (remove with the hunt): while the tirelog trace is
	 * armed, show the renderer frame counter instead of FPS so a screen
	 * capture can be matched to the [TV]/[TIREUP] log lines exactly. */
	if (g_dbg_tireVtxLog)
	{
		snprintf(buf, (size_t)bufSize, "F %lu", g_tirelogFrameNow);
		return 1;
	}
	snprintf(buf, (size_t)bufSize, "FPS %lu.%lu", g_n3dsFpsX10 / 10UL, g_n3dsFpsX10 % 10UL);
	return 1;
#else
	(void)buf;
	(void)bufSize;
	return 0;
#endif
}
