#include <macros.h>
#include "platform/native_gte_capture.h"

#ifndef CTR_NATIVE_GTE_CAPTURE
#define CTR_NATIVE_GTE_CAPTURE 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE

#include <string.h>

// Open-addressed, verify-by-stored-key side table: a key lives in the first of
// GTE_CAPTURE_PROBES consecutive slots from its hash that was empty or already
// held it. A stale (last frame's) entry or a key pushed out past the probe
// window is always a miss, never a wrong value -- callers fall back to today's
// exact snapped behavior on a miss.
//
// DOUBLE-BUFFERED: the game thread writes frame N+1's captures while the
// walk worker may still be reading frame N's table. Two buffers;
// MainFrame_ResetDB's GTECapture_Clear() is the flip point, and DrawOTag
// publishes the write buffer as the walk's read buffer at post time. One
// walk in flight ever (the game's own DrawSync fence), so a flipped-back
// buffer can never still be held by a walk. PC's synchronous walk reads the
// buffer it just wrote (publish makes read==write), byte-identical to
// single-buffered behavior.
//
// Sizing: eviction misses are not free visually -- on a STATIC scene (menus),
// a vertex that hits most frames but is evicted on some frames pops by up to
// 1px on exactly those frames. PC stays at 2^18 entries (4 MiB/buffer). The
// 3DS -- which consumes this table for the retained-geometry full form, not
// desnap -- takes 2^16 (1.5 MiB/buffer): there an eviction only downgrades a
// prim from retained to walked for a frame, invisible by construction.
#ifdef __3DS__
#define GTE_CAPTURE_TABLE_BITS 16
#else
#define GTE_CAPTURE_TABLE_BITS 18
#endif
#define GTE_CAPTURE_TABLE_SIZE (1u << GTE_CAPTURE_TABLE_BITS)

typedef struct
{
	u32 key;        // (u16)sy << 16 | (u16)sx -- same halves as the real C2_SXYn register (gtereg.h)
	u32 generation; // stamped from the owning buffer's generation; 0 = never written, always a miss
	u8 fracX8;      // top 8 of the 16 discarded fractional bits, i.e. 1/256px resolution
	u8 fracY8;
	u16 sz3;      // reserved for stereo (unused by vertex de-snapping)
	s32 hOverSz3; // reserved for perspective-correct UV (unused by vertex de-snapping)
	s16 vx, vy, vz; // model-space input vector (retainability rendezvous)
	u8 mtxId; // frame-scoped matrix-state id this projection ran under (matrix-class probe / full-form matrix classes)
	u8 _pad;
} GTECaptureEntry;

// Matrix-state ids: which distinct GTE R+TR state a Store's projection ran
// under, numbered 0.. in order of first appearance within the frame. The
// full retained form needs to know whether a stable node was projected
// under the frame's dominant (camera) matrix or a per-object one -- that
// decides its coverage in the full retained form.
// Ids above the cap collapse into the last slot; a frame with >63 real
// matrix states would make "dominant share" mushy, not wrong.
#define GTE_CAPTURE_MTX_IDS 64

#define GTE_CAPTURE_PAINTNEAR_MAX 32

typedef struct
{
	GTECaptureEntry entries[GTE_CAPTURE_TABLE_SIZE];
	u32 generation;
	// UI band travels with the buffer: the band pointers describe the frame
	// whose captures live here, and the walk that reads them may run a frame
	// behind the game thread's current SetUIBand call.
	const void *bandStart;
	const void *bandEnd;
	// UI-band EXEMPTION: the one prim range inside the band that is world
	// content -- the checkered flag's packets, which retail links into the
	// swapchain OT's entry 0 during transitions. Travels with the buffer for
	// the same frame-behind reason as the band itself; cleared every frame by
	// Clear (unlike the band, which ResetDB re-sets), so it is only ever live
	// for a frame whose RaceFlag_DrawSelf registered it.
	const void *exemptStart;
	const void *exemptEnd;
	// Per-frame matrix census (reset by Clear): Store count per matrix id,
	// and how many ids this frame actually used.
	u32 mtxStores[GTE_CAPTURE_MTX_IDS];
	u32 mtxUsed;
	// PAINT-ON-TOP prim ranges (header comment on g_dbg_paintOnTop). Same
	// frame-behind travel rule as the exempt range; registered by the instance
	// depth bracket for allowlisted models, cleared by Clear. Appended at the
	// END of the struct so the positional initializer above the buffer pair
	// stays valid.
	const void *paintNearStart[GTE_CAPTURE_PAINTNEAR_MAX];
	const void *paintNearEnd[GTE_CAPTURE_PAINTNEAR_MAX];
	/* Per range: 0 = parallax from the member's own captures (3D models --
	 * the talking masks), 1 = parallax flattened to the UI band (flat overlay
	 * art in the world OT -- stoplight, "FINAL LAP"), 2 = flat AND X-stretched
	 * to the wide window (the finish/weapon lens flare -- retail sizes it for
	 * the 4:3 screen). */
	u8 paintNearFlat[GTE_CAPTURE_PAINTNEAR_MAX];
	/* Per range: the occlusion stamp members pin to; 0 = PKTDEPTH_WORLD_MAX (beats all world content). */
	u16 paintNearPin[GTE_CAPTURE_PAINTNEAR_MAX];
	u32 paintNearN;
	// The 8 CP2C words that opened each matrix run, so a census consumer can
	// identify WHICH matrix a run id is (R+TR values, not just a count).
	// Slots behind a zero mtxStores count are stale from prior frames; the
	// saturation slot (last id) holds only its most recent run's words.
	u32 mtxWords[GTE_CAPTURE_MTX_IDS][8];
} GTECaptureBuffer;

// Cost ablation. This switch removes both per-vertex calls without a
// rebuild, so the capture subsystem's cost can be measured by an in-place
// A/B on identical content instead of two separate builds/runs. It ablates
// COST only, not the table's contents: stale entries fail their
// generation/key check on lookup, the same miss path collisions and
// evictions already take.
int g_dbg_gteCapture = 1;

global_variable GTECaptureBuffer s_gteCaptureBuf[2] = {{{{0}}, 1, NULL, NULL, NULL, NULL}, {{{0}}, 1, NULL, NULL, NULL, NULL}};
// Game-thread cursor: which buffer Store/SetUIBand/Clear touch.
global_variable int s_gteCaptureWriteIdx;
// Walk cursor: which buffer Lookup*/MarkOTNode read. Written only by
// GTECapture_PublishForWalk on the game thread at DrawOTag time, before the
// walk job is posted (or before the synchronous walk runs).
global_variable int s_gteCaptureReadIdx;

internal u32 GTECapture_PackKey(s16 sx, s16 sy)
{
	return ((u32)(u16)sy << 16) | (u32)(u16)sx;
}

internal u32 GTECapture_Hash(u32 key)
{
	// Fibonacci multiplicative hash over the whole packed key -- both sx and
	// sy must contribute, since sy lives in the high 16 bits and a plain mask
	// would only see sx. A bare multiply is LINEAR, so its collisions form a
	// lattice of fixed screen-space vectors; XOR-fold the key before
	// multiplying so the low half perturbs the high half first, breaking the
	// linearity and the fixed collision vectors.
	key ^= key >> 16;
	return (key * 0x9E3779B1u) >> (32u - GTE_CAPTURE_TABLE_BITS);
}

/* Probe window. A single slot per key loses a few percent of a busy frame's
 * entries to collisions, and the packet path drops the depth of any primitive
 * missing two corners, so every lost entry can blink a triangle out for a frame.
 * Entries are never removed within a frame (the generation bump clears them all
 * at once), so a lookup may stop at the first slot not written this frame. */
#define GTE_CAPTURE_PROBES 4

internal GTECaptureEntry *GTECapture_FindLive(const GTECaptureBuffer *buf, u32 key)
{
	const u32 home = GTECapture_Hash(key);
	int i;

	for (i = 0; i < GTE_CAPTURE_PROBES; i++)
	{
		const GTECaptureEntry *entry = &buf->entries[(home + (u32)i) & (GTE_CAPTURE_TABLE_SIZE - 1u)];

		if (entry->generation != buf->generation)
		{
			return NULL;
		}
		if (entry->key == key)
		{
			return (GTECaptureEntry *)entry;
		}
	}
	return NULL;
}

// Matrix-change detector state: game-thread only, continuity across the
// buffer flip is irrelevant (ids are frame-scoped; the first projection of a
// frame always starts id 0).
global_variable u32 s_gteMtxShadow[8];
global_variable int s_gteMtxShadowValid;
global_variable u32 s_gteMtxCurId;
/* Watch instrument frame counter -- bumped in GTECapture_Clear, consumed by
 * the [WATCH] throttle; the rest of the watch state lives further down with
 * g_dbg_watchName. */
global_variable u32 s_watchFrameCtr;

/* Instance depth-axis bracket -- full block sits above GTECapture_Store;
 * these exist because Clear (the frame boundary) runs their window flush. */
void GTECapture_EndInstanceDepth(void);
internal void GTECapture_InstanceAxisFlush(void);
extern int g_dbg_modelDepthCheck;
global_variable u32 s_instAxisFrames;

/* PAINT-ON-TOP class (design comment in the header). Two routes in. Membership
 * by MODEL name covers the talking-head models: the mask hint instance is
 * literally named "head", which is far too generic to key on, and the in-race
 * shield masks (akuaku/ukauka) orbit the kart at honest positions and keep the
 * real depth test. Membership by INSTANCE name covers the held shield bubble,
 * whose three layers share model names with the thrown shield projectile (a
 * world object that must stay occluded); only the held layers carry these
 * instance names. The podium prize is also matched by INSTANCE name and pins
 * at the world maximum like the talking heads: it rests where a tall winner's
 * raised arms reach, and retail paints it whole over the winner. Its models
 * (trophy, relic, key, gem) are also the hub warp-pad prizes, named
 * "prize1"/"prize2", which stay world objects; the match is exact. */
int g_dbg_paintOnTop = 1;
global_variable const char *const s_paintOnTopModels[] = {"ukabig", "akubig"};
global_variable const char *const s_paintOnTopInstances[] = {"shield", "shielddark", "highlight"};
global_variable const char *const s_paintOnTopWorldMaxInstances[] = {"prize"};
/* Bracket latch: set when BeginInstanceDepth matches the allowlist, consumed
 * by EndInstanceDepth into the write buffer's range array. Game-thread only,
 * same ownership as the bracket itself. */
global_variable int s_paintNearArmed;
global_variable const void *s_paintNearStartCursor;
global_variable u16 s_paintNearPinArmed;

int GTECapture_ModelPaintsOnTop(const char *modelName)
{
	u32 i;

	if (modelName == NULL)
	{
		return 0;
	}
	for (i = 0; i < (sizeof(s_paintOnTopModels) / sizeof(s_paintOnTopModels[0])); i++)
	{
		if (strncmp(modelName, s_paintOnTopModels[i], 15) == 0)
		{
			return 1;
		}
	}
	return 0;
}

/* 0 = not a member, 1 = talking head or podium prize (pins to the nearest
 * world stamp), 2 = held-shield member (pins just in front of its own kart). */
int GTECapture_InstPaintsOnTop(const struct Instance *inst)
{
	u32 i;

	if (inst == NULL)
	{
		return 0;
	}
	if ((inst->model != NULL) && GTECapture_ModelPaintsOnTop(inst->model->name))
	{
		return 1;
	}
	for (i = 0; i < (sizeof(s_paintOnTopWorldMaxInstances) / sizeof(s_paintOnTopWorldMaxInstances[0])); i++)
	{
		if (strncmp(inst->name, s_paintOnTopWorldMaxInstances[i], 15) == 0)
		{
			return 1;
		}
	}
	for (i = 0; i < (sizeof(s_paintOnTopInstances) / sizeof(s_paintOnTopInstances[0])); i++)
	{
		if (strncmp(inst->name, s_paintOnTopInstances[i], 15) == 0)
		{
			return 2;
		}
	}
	return 0;
}

/* Occlusion stamp for a held-shield layer (PKTDEPTH stamps are 0xFFFF minus
 * view.z, nearer = larger). The bubble pins a margin in front of its own
 * kart's origin: the margin covers the ground under the lower half of the
 * sphere, which the camera sees from above, so it needs a few radii of depth
 * to clear it; a world object nearer than that still hides the bubble. The
 * margin follows the layer's instance scale, which tracks the sphere radius.
 * Near the camera the pin saturates at the world maximum (the 64-unit floor
 * below the UI band), the same stamp the talking heads use. */
#define GTE_PAINT_NEAR_SHIELD_MARGIN_SHIFT 3
#define GTE_PAINT_NEAR_FLOOR_Z 64
internal u16 GTECapture_ShieldPinStamp(const struct Instance *inst, s64 trueZ)
{
	s64 margin = (s64)(u16)inst->scale.x >> GTE_PAINT_NEAR_SHIELD_MARGIN_SHIFT;
	s64 z = trueZ - margin;

	if (z < GTE_PAINT_NEAR_FLOOR_Z)
	{
		z = GTE_PAINT_NEAR_FLOOR_Z;
	}
	if (z > 0xFFFF)
	{
		z = 0xFFFF;
	}
	return (u16)(0xFFFF - z);
}

/* PAINT-UNDER class: members are geometry retail parks INSIDE or coincident
 * with other geometry and hides purely by painter's order (same OT bucket,
 * drawn first). Under a real depth test they win by their honest depth -- the
 * intro's plain "BOX_02A" cube is a shell ~2 units OUTSIDE the wooden crate,
 * so it paints the whole crate flat orange. Members draw through the packet
 * path with their captured depth pushed farther by an eighth plus
 * GTE_PAINT_UNDER_BIAS (stored-axis units) -- retail's layering translated to
 * the depth axis; the model path applies the matching push in
 * NativeModelDraw_ComposeRows. dumpreq `under` (g_dbg_paintUnder) A/Bs it. */
#define GTE_PAINT_UNDER_BIAS 8
int g_dbg_paintUnder = 1;
global_variable const char *const s_paintUnderNames[] = {"BOX_02A"};
global_variable int s_paintUnderActive;

int GTECapture_InstPaintsUnder(const struct Instance *inst)
{
	u32 i;
	const char *modelName;

	if ((g_dbg_paintUnder == 0) || (inst == NULL))
	{
		return 0;
	}
	modelName = (inst->model != NULL) ? inst->model->name : "";
	for (i = 0; i < (sizeof(s_paintUnderNames) / sizeof(s_paintUnderNames[0])); i++)
	{
		if ((strncmp(inst->name, s_paintUnderNames[i], 15) == 0) || (strncmp(modelName, s_paintUnderNames[i], 15) == 0))
		{
			return 1;
		}
	}
	return 0;
}

void GTECapture_Clear(void)
{
	GTECaptureBuffer *buf;

	// Flip first, then invalidate the NEW write buffer by generation bump.
	// The buffer being abandoned may still be read by an in-flight walk; it
	// is left untouched.
	s_gteCaptureWriteIdx ^= 1;
	buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->generation++;
	if (buf->generation == 0) // wrapped after ~2^32 frames (2+ years continuous at 60fps)
	{
		memset(buf->entries, 0, sizeof(buf->entries));
		buf->generation = 1;
	}

	memset(buf->mtxStores, 0, sizeof(buf->mtxStores));
	buf->mtxUsed = 0;
	buf->exemptStart = NULL;
	buf->exemptEnd = NULL;
	buf->paintNearN = 0;
	s_gteMtxShadowValid = 0;
	s_gteMtxCurId = 0;

	/* [PKTAXIS] window boundary (instance-axis bracket, below). Also closes a
	 * bracket a mid-frame early-out could otherwise leave open -- the same
	 * leak the model capture closes in its EndFrame. Disarm the paint-near
	 * latch first: the buffer just flipped, so a leaked bracket's cursor pair
	 * would register a stale range into the NEW frame's buffer. */
	s_paintNearArmed = 0;
	s_watchFrameCtr++;
	GTECapture_EndInstanceDepth();
	if (g_dbg_modelDepthCheck && (++s_instAxisFrames >= 256))
	{
		s_instAxisFrames = 0;
		GTECapture_InstanceAxisFlush();
	}
}

// Called by GTE_RotTransPers with the 8 contiguous CP2C words holding
// R11..R33 and TRX/Y/Z -- one memcmp per projected vertex detects the
// game switching matrices (camera -> instance -> ...).
void GTECapture_NoteMatrix(const void *words8)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	if (s_gteMtxShadowValid && (memcmp(s_gteMtxShadow, words8, sizeof(s_gteMtxShadow)) == 0))
	{
		return;
	}

	memcpy(s_gteMtxShadow, words8, sizeof(s_gteMtxShadow));
	if (!s_gteMtxShadowValid)
	{
		s_gteMtxShadowValid = 1;
		s_gteMtxCurId = 0;
	}
	else if (s_gteMtxCurId < (GTE_CAPTURE_MTX_IDS - 1))
	{
		s_gteMtxCurId++;
	}
	if ((s_gteMtxCurId + 1) > buf->mtxUsed)
	{
		buf->mtxUsed = s_gteMtxCurId + 1;
	}
	memcpy(buf->mtxWords[s_gteMtxCurId], words8, sizeof(buf->mtxWords[0]));
}

void GTECapture_PublishForWalk(void)
{
	s_gteCaptureReadIdx = s_gteCaptureWriteIdx;
}

/*
 * Depthless-geometry suppression. Game-thread only, set/cleared inside one
 * producer's call so no walk observes it half-applied. DrawSky_Full projects
 * the skybox with the GTE translation vector zeroed, so its SZ3 is a
 * function of the sky mesh's own radius, unrelated to real depth -- and the
 * table is keyed by screen coordinate, so a sky vertex would evict whatever
 * world vertex hashed to its slot. Suppressing the store makes lookups miss,
 * falling back to PacketDepth_ResolveXY's OT-bucket depth (retail's own
 * painter's-algorithm layering). Cost: sky vertices lose sub-pixel
 * de-snapping and cannot be retained; both acceptable, neither shipping.
 */
global_variable int s_gteCaptureStoreSuppressed;

void GTECapture_SetStoreSuppressed(int suppressed)
{
	s_gteCaptureStoreSuppressed = suppressed;
}

/*
 * Affine depth-remap bracket (menu/loading checkered flag), same
 * one-producer discipline as the suppression flag above; RaceFlag_DrawSelf
 * is the only client. Remaps stored sz3 affinely (out = OUT_NEAR +
 * ((sz3 - RAW_NEAR) >> SHIFT)) so the flag's wave relief survives at reduced
 * amplitude in a band anchored just behind the stereo convergence plane --
 * preserving depth-correctness without any pose-conditional switch. Bounds
 * are structural: remapped values always land inside the world depth band,
 * never the UI band.
 *
 * Known caveat: during screen-covering wipes, content nearer than the flag's
 * band can depth-test in front of it, a departure from retail's
 * flag-over-everything (see RaceFlag_IsFullyOnScreen / RaceFlag_DrawOrder
 * for the documented fallback if this reads wrong on hardware).
 */
enum
{
	GTE_FLAGREMAP_RAW_NEAR = 60928, /* measured min 61069 minus guard margin */
	GTE_FLAGREMAP_SHIFT = 2,        /* relief compressed to 1/4 */
	GTE_FLAGREMAP_OUT_NEAR = 1232   /* 32 view units behind s_stereoConv 1200 */
};

global_variable int s_gteCaptureDepthRemap;

void GTECapture_SetStoreDepthRemap(int active)
{
	s_gteCaptureDepthRemap = active;
}

/*
 * EVICTION CENSUS -- header comment on GTECapture_ReadEvictCensus has the
 * argument. Counted at the one place a slot can change owner; the extra
 * branch costs nothing measurable since the entry line is already being
 * fetched for the next write.
 */
global_variable struct
{
	u32 diffMtx;  /* same pixel, different object -- the harmful population */
	u32 sameMtx;  /* same pixel, same object: a real self-overwrite, benign */
	u32 maxDz;    /* biggest sz3 delta a harmful overwrite introduced */
} s_gteEvict;

u32 GTECapture_ReadEvictCensus(u32 *outSameMtx, u32 *outMaxDz)
{
	u32 diff = s_gteEvict.diffMtx;

	*outSameMtx = s_gteEvict.sameMtx;
	*outMaxDz = s_gteEvict.maxDz;
	s_gteEvict.diffMtx = 0;
	s_gteEvict.sameMtx = 0;
	s_gteEvict.maxDz = 0;
	return diff;
}

/*
 * INSTANCE DEPTH-AXIS BRACKET. Retail's instance pipeline is regime-scaled
 * (RenderBucket_AdjustViewPositionForMvp: <<2 under 0x1000 view units,
 * DRAW_HUGE >>2), so instance SZ3 lands on a 4x/1x/quarter axis relative to
 * the track's view.z. The native model path corrects this per instance
 * (native_model_emit.c, NativeModelEmitInstance.depthShift -- keep
 * classifiers in lockstep); instance content still drawn as packets stores
 * its scaled SZ3 here, and a table hit is what buries it, since the
 * resolve-miss fallback derives depth from the unshifted OT bucket.
 *
 * Correction happens at the store, inside a bracket the RenderBucket dispatch
 * opens around each instance's draw func; classification is self-calibrating
 * from mvp.t[2] or the first SZ3 seen, since candidates are a factor of 4
 * apart.
 *
 * Known residual: queue-phase bbox corner projections
 * (RenderBucket_ProjectFrameBounds) run outside this bracket and still store
 * scaled depths.
 */

/* WATCH instrument (dumpreq 'watch <name>', bare 'watch' = off): put a tap
 * on ONE NAMED OBJECT instead of inferring from anonymous primitives -- the
 * owner's ask after a night of the latter. Matches the instance name (e.g.
 * "prize1"/"prize2", INSTANCE_Birth3D's argument) OR the model name, so
 * objects whose instance name is unknown (the mask hint, the start lights)
 * can be watched by model. While the watched object's draw bracket is open,
 * every GTE store is tallied: raw SZ3 range as projected, corrected range
 * after the axis unscale, plus the classify inputs (trueZ, mvp.t[2]) and the
 * final shift. One [WATCH] line per bracket every 32 frames tells the whole
 * store-side story: if corr ~= trueZ the stores are honest and the lie is
 * downstream (walk/stamp side); if not, the wrong step is named by which
 * number disagrees. */
char g_dbg_watchName[16];
/* dumpreq `hide NAME`: the watched instance's draw func is skipped entirely,
 * so a layered prop can be peeled piece by piece. */
int g_dbg_watchHide;
global_variable int s_watchActive;

int GTECapture_WatchIsHiddenInstance(void)
{
	return (g_dbg_watchHide != 0) && (s_watchActive != 0);
}

/* Prefix match on instance OR model name, so `watch BOX` taps every BOX_*
 * piece of a multi-instance prop; a trailing '$' (`watch BOX_02$`) demands the
 * whole name. */
internal int GTECapture_WatchMatches(const struct Instance *inst)
{
	const char *modelName;
	size_t wlen;

	if ((g_dbg_watchName[0] == '\0') || (inst == NULL))
	{
		return 0;
	}
	modelName = (inst->model != NULL) ? inst->model->name : "";
	wlen = strlen(g_dbg_watchName);
	if (g_dbg_watchName[wlen - 1] == '$')
	{
		wlen--;
		return ((strncmp(inst->name, g_dbg_watchName, wlen) == 0) && (inst->name[wlen] == '\0')) ||
		       ((strncmp(modelName, g_dbg_watchName, wlen) == 0) && (modelName[wlen] == '\0'));
	}
	return (strncmp(inst->name, g_dbg_watchName, wlen) == 0) || (strncmp(modelName, g_dbg_watchName, wlen) == 0);
}
global_variable char s_watchInstName[16];
global_variable s64 s_watchTrueZ;
global_variable s32 s_watchMvpZ;
global_variable int s_watchFunc;
global_variable u32 s_watchStores;
global_variable u16 s_watchRawMin;
global_variable u16 s_watchRawMax;
global_variable u16 s_watchCorrMin;
global_variable u16 s_watchCorrMax;
/* v2: the exact inputs the classify reference was computed from -- when
 * trueZ is garbage, these name WHICH input lied (instance matrix.t vs
 * camera pos), which is the difference between designing the fix blind and
 * naming the corrupter. */
global_variable s32 s_watchInstT[3];
global_variable s32 s_watchCamPos[3];
/* LOD header of the watched draw: a model's hi/lo swap and a draw-func switch
 * both happen at fixed distances, and only this field tells them apart. */
global_variable const char *s_watchPendingLod;
global_variable char s_watchLodName[16];

void GTECapture_WatchNoteLod(const char *lodName)
{
	s_watchPendingLod = lodName;
}
/* Ground-decal nearer bias, in view.z units, applied to every store inside a
 * FIXED-shift bracket (shadows/skids/tires and the ordinary particle list) --
 * the 3DS analogue of retail's painter's order for content that sits ON the
 * road. A decal quad is planar between its corners; the road under it is not
 * (a quadblock's centre vertex is displaced), so at exact per-vertex depth the
 * road's bumps rise above the decal's plane between its corners and win the
 * depth test in patches. Pulling only this content nearer by more than the
 * road's local relief keeps it on top, the way retail drew it, without moving
 * karts, pickups or the world. 32 clears Oxide Station's relief; 16 did not
 * (owner-judged in Azahar). dumpreq `decalbias N`. 3DS only: the PC renderer
 * does not depth-test packets, and its golden output must not move. */
int g_dbg_decalNearBias = 32;
/* "Under" mode inside a fixed bracket: content that retail draws UNDER the
 * road by painter's order (the mirrored ice-reflection tires), so it must
 * LOSE the ground depth tie, not win it -- the exact mirror of the decal
 * rule above. Stores take the same relief-clearing margin FARTHER instead of
 * nearer; one knob (`decalbias N`) tunes both directions. Toggled around the
 * DrawTires_Reflection calls; cleared by the bracket teardown so a mid-frame
 * early-out cannot leak it. */
global_variable int s_instAxisFixedUnder;

void GTECapture_SetFixedShiftUnder(int on)
{
	s_instAxisFixedUnder = (on != 0);
}

#define GTE_INSTAXIS_UNRESOLVED 99
#define GTE_INSTAXIS_MTX_NONE 0xFFFFFFFFu

global_variable int s_instAxisActive;
/* Set by GTECapture_BeginFixedDepthShift -- the shift is known from source
 * rather than measured, so the store skips classify/reclassify/stats and
 * only applies it. See the block in GTECapture_Store. */
global_variable int s_instAxisFixed;
global_variable int s_instAxisShift = GTE_INSTAXIS_UNRESOLVED;
global_variable s64 s_instAxisTrueZ;
global_variable int s_instAxisFunc;
/* Matrix state the current shift was classified under. The split/reflection
 * writers project the BODY and the MIRRORED copy under different GTE
 * matrices within one bracket, and the two passes need not share a regime,
 * so classification re-runs at every matrix change inside the bracket.
 * Depends on NoteMatrix's change detector (g_dbg_gteCapture == 1); capture
 * state 2 degrades to one shift per instance. The mirror pass classifies
 * against the BODY origin's trueZ, an approximation with margin since the
 * candidates are a factor of 4 apart. */
global_variable u32 s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;

/* [PKTAXIS] window accumulators, per draw func (0=normal 1=normalAlt 2=split
 * 3=special 4=reflection). Stats gated behind g_dbg_modelDepthCheck -- the
 * axis-investigation flag -- but classification and the fix itself are not. */
extern int g_dbg_modelDepthCheck;
global_variable u32 s_instAxisInstN[5];
global_variable u32 s_instAxisShiftN[5][3]; /* [func][0]=shift2 [1]=shift0 [2]=shift-2 */
global_variable u32 s_instAxisUnresolvedN[5];
global_variable u32 s_instAxisReclassN[5]; /* matrix-change re-classifications (mirror passes) */
global_variable u32 s_instAxisStoreN[5];
global_variable double s_instAxisRatioSum[5];
global_variable double s_instAxisRatioMin[5];
global_variable double s_instAxisRatioMax[5];
/* s_instAxisFrames is declared above GTECapture_Clear, which drives the flush. */

/* Nearest of {4x, 1x, 1/4x}: which regime this mvp-axis depth actually is,
 * measured against the track-axis view.z of the same point. MUST stay in
 * lockstep with the classifier in NativeModelEmit_BeginInstance. */
internal int GTECapture_ClassifyShift(s64 mvpAxisZ, s64 trueZ)
{
	s64 eNear = mvpAxisZ - (trueZ << 2);
	s64 eFar = mvpAxisZ - trueZ;
	s64 eHuge = (mvpAxisZ << 2) - trueZ;

	if (eNear < 0) { eNear = -eNear; }
	if (eFar < 0) { eFar = -eFar; }
	if (eHuge < 0) { eHuge = -eHuge; }

	return (eNear <= eFar) ? ((eNear <= eHuge) ? 2 : -2) : ((eFar <= eHuge) ? 0 : -2);
}

void GTECapture_BeginInstanceDepth(const struct Instance *inst, const struct PushBuffer *pb, s32 mvpZIfNormal, int funcKind)
{
	s64 trueZ;

	if ((inst == NULL) || (pb == NULL))
	{
		return;
	}

	/* Track-axis view.z of the instance origin -- same integer arithmetic as
	 * RenderBucket_GetViewPosition's GTE path and the native classifier's. */
	if ((inst->flags & SCREENSPACE_INSTANCE) != 0)
	{
		trueZ = (s64)inst->matrix.t[2];
	}
	else
	{
		/* TRUNCATE THE DELTAS TO s16, exactly as the GTE's MTC2 vertex registers
		 * do. The prize orbit's u32-sine spin math can carry ~+2^20 of
		 * upper-bit garbage in matrix.t on negative phases; that's invisible
		 * to retail (2^20 = 0 mod 2^16, every GTE consumer truncates), but
		 * this 64-bit-dot-product classifier would see it and misclassify the
		 * regime. Truncating reproduces retail's own semantics bit-for-bit.
		 * MUST stay in lockstep with NativeModelEmit_BeginInstance. */
		s64 dx = (s64)(s16)((s64)inst->matrix.t[0] - (s64)pb->pos.x);
		s64 dy = (s64)(s16)((s64)inst->matrix.t[1] - (s64)pb->pos.y);
		s64 dz = (s64)(s16)((s64)inst->matrix.t[2] - (s64)pb->pos.z);

		trueZ = ((s64)pb->matrix_ViewProj.m[2][0] * dx + (s64)pb->matrix_ViewProj.m[2][1] * dy + (s64)pb->matrix_ViewProj.m[2][2] * dz) >> 12;
	}

	s_instAxisActive = 1;
	s_instAxisTrueZ = trueZ;
	s_instAxisFunc = ((funcKind >= 0) && (funcKind < 5)) ? funcKind : 0;
	s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
	s_paintUnderActive = GTECapture_InstPaintsUnder(inst);

	/* Watch tap -- see g_dbg_watchName. Matched per bracket so multiple
	 * same-named instances (the prize ring is three "prize1"s) each report. */
	s_watchActive = 0;
	{
		if (GTECapture_WatchMatches(inst))
		{
			int wi;

			s_watchActive = 1;
			for (wi = 0; wi < 15; wi++)
			{
				s_watchInstName[wi] = inst->name[wi];
			}
			s_watchInstName[15] = '\0';
			s_watchTrueZ = trueZ;
			s_watchMvpZ = mvpZIfNormal;
			s_watchFunc = s_instAxisFunc;
			s_watchInstT[0] = (s32)inst->matrix.t[0];
			s_watchInstT[1] = (s32)inst->matrix.t[1];
			s_watchInstT[2] = (s32)inst->matrix.t[2];
			s_watchCamPos[0] = (s32)pb->pos.x;
			s_watchCamPos[1] = (s32)pb->pos.y;
			s_watchCamPos[2] = (s32)pb->pos.z;
			for (wi = 0; wi < 15; wi++)
			{
				s_watchLodName[wi] = (s_watchPendingLod != NULL) ? s_watchPendingLod[wi] : '\0';
				if (s_watchLodName[wi] == '\0')
				{
					break;
				}
			}
			s_watchLodName[(wi < 15) ? wi : 15] = '\0';
			s_watchStores = 0;
			s_watchRawMin = 0xFFFFu;
			s_watchRawMax = 0;
			s_watchCorrMin = 0xFFFFu;
			s_watchCorrMax = 0;
		}
	}

	if (trueZ < 128)
	{
		/* Too near (or behind) for the nearest-candidate test to be safe,
		 * and a 3z scale error under 128 units is inside the shared 256-unit
		 * near-band floor. Store untouched. */
		s_instAxisShift = 0;
	}
	else if (mvpZIfNormal > 0)
	{
		s_instAxisShift = GTECapture_ClassifyShift((s64)mvpZIfNormal, trueZ);
	}
	else
	{
		s_instAxisShift = GTE_INSTAXIS_UNRESOLVED; /* classify on first store */
	}

	/* Paint-on-top latch. The bracket wraps exactly one instance's draw-func
	 * execution, and the retail draw funcs advance primMem.cursor per prim, so
	 * the cursor here and at EndInstanceDepth bracket this instance's packets
	 * and nothing else. The pool is the one RenderBucket_Execute draws into
	 * (MainFrame_RenderFrame passes &gGT->backBuffer->primMem), which is the
	 * same pool the OT walk later visits. */
	if (g_dbg_paintOnTop)
	{
		int paintClass = GTECapture_InstPaintsOnTop(inst);

		if (paintClass != 0)
		{
			s_paintNearArmed = 1;
			s_paintNearStartCursor = sdata->gGT->backBuffer->primMem.cursor;
			s_paintNearPinArmed = (paintClass == 2) ? GTECapture_ShieldPinStamp(inst, trueZ) : 0;
		}
	}
}

/*
 * Open a bracket whose regime shift is KNOWN, not measured. `shift` uses the
 * same encoding as s_instAxisShift (2 = stored SZ3 is 4x the track axis, -2 =
 * quarter axis, 0 = already correct). Closed by GTECapture_EndInstanceDepth,
 * and by GTECapture_Clear at the frame edge, so a mid-frame early-out cannot
 * leak the shift into unrelated content. Never nests with the instance
 * bracket: its three callers (VehGroundShadow, VehGroundSkids, DrawTires) all
 * run outside the RenderBucket dispatch.
 */
void GTECapture_BeginFixedDepthShift(int shift)
{
	s_instAxisActive = 1;
	s_instAxisFixed = 1;
	s_instAxisShift = shift;
	s_instAxisTrueZ = 0;
	s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
}

void GTECapture_EndInstanceDepth(void)
{
	if (!s_instAxisActive)
	{
		return;
	}

	/* Watch report -- one line per watched bracket, throttled to every
	 * 32nd frame so three orbiting prizes cost ~3 lines/sec. stores=0 is
	 * printed too: "the draw func rejected every triangle" is a finding. */
	if (s_watchActive)
	{
		if ((s_watchFrameCtr & 7u) == 0u)
		{
			Platform_LogInstrument("[WATCH] f=%lu name=%s lod=%s func=%d trueZ=%ld mvpZ=%ld shift=%d stores=%lu raw=%u..%u corr=%u..%u t=(%ld,%ld,%ld) cam=(%ld,%ld,%ld)\n",
			                       (unsigned long)s_watchFrameCtr, s_watchInstName, s_watchLodName, s_watchFunc,
			                       (long)s_watchTrueZ, (long)s_watchMvpZ, (s_instAxisShift == GTE_INSTAXIS_UNRESOLVED) ? 99 : s_instAxisShift,
			                       (unsigned long)s_watchStores, (unsigned)s_watchRawMin, (unsigned)s_watchRawMax, (unsigned)s_watchCorrMin,
			                       (unsigned)s_watchCorrMax, (long)s_watchInstT[0], (long)s_watchInstT[1], (long)s_watchInstT[2], (long)s_watchCamPos[0],
			                       (long)s_watchCamPos[1], (long)s_watchCamPos[2]);
		}
		s_watchActive = 0;
	}

	/* Paint-on-top registration -- second half of the latch in
	 * BeginInstanceDepth. An empty range (draw func rejected every prim, or
	 * the model path intercepted the writes) registers nothing. */
	if (s_paintNearArmed)
	{
		GTECapture_AddPaintOnTopRangePinned(s_paintNearStartCursor, sdata->gGT->backBuffer->primMem.cursor, 0, s_paintNearPinArmed);
		s_paintNearArmed = 0;
	}

	if (s_instAxisFixed)
	{
		/* Fixed brackets contribute no [PKTAXIS] statistics by construction --
		 * there is nothing measured to report, and borrowing one of the five
		 * func buckets would corrupt a line the instance work is still read
		 * against. */
		s_instAxisActive = 0;
		s_instAxisFixed = 0;
		s_instAxisFixedUnder = 0;
		s_instAxisShift = GTE_INSTAXIS_UNRESOLVED;
		s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
		s_paintUnderActive = 0;
		return;
	}

	if (g_dbg_modelDepthCheck)
	{
		s_instAxisInstN[s_instAxisFunc]++;
		if (s_instAxisShift == 2)
		{
			s_instAxisShiftN[s_instAxisFunc][0]++;
		}
		else if (s_instAxisShift == 0)
		{
			s_instAxisShiftN[s_instAxisFunc][1]++;
		}
		else if (s_instAxisShift == -2)
		{
			s_instAxisShiftN[s_instAxisFunc][2]++;
		}
		else
		{
			/* Ended without a single store to classify from -- the draw func
			 * rejected every triangle. Nothing was scaled, nothing was wrong. */
			s_instAxisUnresolvedN[s_instAxisFunc]++;
		}
	}

	s_instAxisActive = 0;
	s_instAxisShift = GTE_INSTAXIS_UNRESOLVED;
	s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
	s_paintUnderActive = 0;
}

/* Per-frame window flush, driven by GTECapture_Clear below. 256 content-ish
 * frames per line keeps it to a few lines a minute at 30fps. */
internal void GTECapture_InstanceAxisFlush(void)
{
	static const char *const funcName[5] = {"normal", "normalAlt", "split", "special", "reflection"};
	int f;

	for (f = 0; f < 5; f++)
	{
		if ((s_instAxisInstN[f] == 0) && (s_instAxisStoreN[f] == 0))
		{
			continue;
		}
		Platform_LogInstrument("[PKTAXIS] func=%s inst=%lu shift4x=%lu shift1x=%lu shiftQ=%lu unresolved=%lu reclass=%lu stores=%lu ratio=%.3f (%.3f..%.3f)\n",
		                       funcName[f], (unsigned long)s_instAxisInstN[f], (unsigned long)s_instAxisShiftN[f][0], (unsigned long)s_instAxisShiftN[f][1],
		                       (unsigned long)s_instAxisShiftN[f][2], (unsigned long)s_instAxisUnresolvedN[f], (unsigned long)s_instAxisReclassN[f],
		                       (unsigned long)s_instAxisStoreN[f], s_instAxisStoreN[f] ? (s_instAxisRatioSum[f] / (double)s_instAxisStoreN[f]) : 0.0,
		                       s_instAxisRatioMin[f], s_instAxisRatioMax[f]);
		s_instAxisReclassN[f] = 0;
		s_instAxisInstN[f] = 0;
		s_instAxisShiftN[f][0] = 0;
		s_instAxisShiftN[f][1] = 0;
		s_instAxisShiftN[f][2] = 0;
		s_instAxisUnresolvedN[f] = 0;
		s_instAxisStoreN[f] = 0;
		s_instAxisRatioSum[f] = 0.0;
		s_instAxisRatioMin[f] = 0.0;
		s_instAxisRatioMax[f] = 0.0;
	}
}

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const u32 home = GTECapture_Hash(key);
	GTECaptureEntry *entry = &buf->entries[home];

	/* First slot in the probe window that is free this frame or already holds
	 * this key (GTECapture_FindLive's contract). A full window overwrites the
	 * home slot, which is the old direct-mapped eviction. */
	{
		int i;

		for (i = 0; i < GTE_CAPTURE_PROBES; i++)
		{
			GTECaptureEntry *probe = &buf->entries[(home + (u32)i) & (GTE_CAPTURE_TABLE_SIZE - 1u)];

			if ((probe->generation != buf->generation) || (probe->key == key))
			{
				entry = probe;
				break;
			}
		}
	}

	/* Suppressed producers must not even EVICT -- see the comment above; the
	 * eviction is the half of the bug that does not look like a skybox
	 * problem. Tested here rather than at the GTE_RotTransPers call site so
	 * there is exactly one place a store can be dropped. */
	if (s_gteCaptureStoreSuppressed)
	{
		return;
	}

	/* Flag depth remap (block above SetStoreDepthRemap). Applied before
	 * anything else reads sz3 -- census, ratios, the entry itself -- so every
	 * consumer sees one consistent axis, same placement rule as the instance
	 * bracket below. The two brackets never nest. */
	if (s_gteCaptureDepthRemap)
	{
		u32 t = ((u32)sz3 > (u32)GTE_FLAGREMAP_RAW_NEAR) ? ((u32)GTE_FLAGREMAP_OUT_NEAR + (((u32)sz3 - (u32)GTE_FLAGREMAP_RAW_NEAR) >> GTE_FLAGREMAP_SHIFT))
		                                                 : (u32)GTE_FLAGREMAP_OUT_NEAR;

		sz3 = (u16)t;
	}

	/* Instance depth-axis bracket (block comment above): put this store on the
	 * track's view.z axis before ANYTHING sees it -- including the eviction
	 * census a few lines down, whose dz would otherwise mix units. */
	if (s_instAxisActive)
	{
		/*
		 * FIXED-SHIFT BRACKET -- ground-decal producers. VehGroundShadow,
		 * VehGroundSkids and DrawTires each pre-multiply their camera-relative
		 * position by 4 and project with zero translation, so their SZ3 is
		 * always 4x the track's view.z; these run outside RenderBucket, so the
		 * dispatch-site bracket never corrected them. No classifier needed
		 * here (unlike the instance path, whose scale is conditional): the
		 * shift these three apply is unconditional and visible in source.
		 */
		if (!s_instAxisFixed)
		{
			/* Per-matrix re-classification -- see s_instAxisMtxAtClassify. The
			 * first matrix state adopts Begin's exact mvp-based shift; each later
			 * state inside the bracket (the mirror pass) re-classifies from its
			 * own first store. */
			if (s_instAxisMtxAtClassify == GTE_INSTAXIS_MTX_NONE)
			{
				s_instAxisMtxAtClassify = s_gteMtxCurId;
			}
			else if (s_gteMtxCurId != s_instAxisMtxAtClassify)
			{
				s_instAxisMtxAtClassify = s_gteMtxCurId;
				if (s_instAxisTrueZ >= 128)
				{
					s_instAxisShift = GTECapture_ClassifyShift((s64)sz3, s_instAxisTrueZ);
				}
				if (g_dbg_modelDepthCheck)
				{
					s_instAxisReclassN[s_instAxisFunc]++;
				}
			}

			if (s_instAxisShift == GTE_INSTAXIS_UNRESOLVED)
			{
				/* First store of a split/special/reflection instance: SZ3 is
				 * TRZ plus a model-extent term, and the candidates are 4x apart,
				 * so nearest-candidate on the raw SZ3 names the regime. */
				s_instAxisShift = GTECapture_ClassifyShift((s64)sz3, s_instAxisTrueZ);
			}

			if (g_dbg_modelDepthCheck && (s_instAxisTrueZ >= 128))
			{
				double ratio = (double)sz3 / (double)s_instAxisTrueZ;
				int f = s_instAxisFunc;

				if (s_instAxisStoreN[f] == 0)
				{
					s_instAxisRatioMin[f] = ratio;
					s_instAxisRatioMax[f] = ratio;
				}
				else
				{
					if (ratio < s_instAxisRatioMin[f]) { s_instAxisRatioMin[f] = ratio; }
					if (ratio > s_instAxisRatioMax[f]) { s_instAxisRatioMax[f] = ratio; }
				}
				s_instAxisStoreN[f]++;
				s_instAxisRatioSum[f] += ratio;
			}
		} /* !s_instAxisFixed -- shared shift application follows */

		u16 watchRaw = sz3;

		if (s_instAxisShift == 2)
		{
			u32 t = (u32)sz3 >> 2;

			sz3 = (u16)((t != 0) ? t : 1); /* keep a hit distinguishable from empty */
		}
		else if (s_instAxisShift == -2)
		{
			u32 t = (u32)sz3 << 2;

			sz3 = (t > 0xFFFFu) ? (u16)0xFFFFu : (u16)t;
		}

#if defined(__3DS__)
		/* Ground-decal nearer bias (g_dbg_decalNearBias) -- fixed brackets only,
		 * after the axis unscale so it is in the track's view.z units. Floor of
		 * 1 keeps a hit distinguishable from empty, same as the unscale. In
		 * "under" mode (the mirrored ice-reflection tires) the same margin is
		 * applied FARTHER instead: that content must lose the ground tie, the
		 * way retail's painter's order tucked it beneath the opaque road. */
		if (s_instAxisFixed && (g_dbg_decalNearBias > 0))
		{
			if (s_instAxisFixedUnder)
			{
				u32 b = (u32)sz3 + (u32)g_dbg_decalNearBias;

				sz3 = (b > 0xFFFFu) ? (u16)0xFFFFu : (u16)b;
			}
			else
			{
				int b = (int)sz3 - g_dbg_decalNearBias;

				sz3 = (u16)((b >= 1) ? b : 1);
			}
		}

		/* Paint-under farther bias, same units and placement as the decal bias. */
		if (s_paintUnderActive)
		{
			u32 b = (u32)sz3 + ((u32)sz3 >> 3) + GTE_PAINT_UNDER_BIAS;

			sz3 = (b > 0xFFFFu) ? (u16)0xFFFFu : (u16)b;
		}
#endif

		/* Watch tap: raw = as the draw projected it, corr = as stored. */
		if (s_watchActive && !s_instAxisFixed)
		{
			s_watchStores++;
			if (watchRaw < s_watchRawMin) { s_watchRawMin = watchRaw; }
			if (watchRaw > s_watchRawMax) { s_watchRawMax = watchRaw; }
			if (sz3 < s_watchCorrMin) { s_watchCorrMin = sz3; }
			if (sz3 > s_watchCorrMax) { s_watchCorrMax = sz3; }
		}
	}

	/* Same key AND live generation means this slot already holds a projection
	 * of THIS SAME screen coordinate from earlier in the frame -- not a hash
	 * collision (different key), which the lookup's key check already turns
	 * into a clean miss. Whoever wrote it is about to lose their depth. */
	if ((entry->generation == buf->generation) && (entry->key == key))
	{
		if (entry->mtxId != (u8)s_gteMtxCurId)
		{
			u32 dz = (entry->sz3 > sz3) ? (u32)(entry->sz3 - sz3) : (u32)(sz3 - entry->sz3);

			s_gteEvict.diffMtx++;
			if (dz > s_gteEvict.maxDz)
			{
				s_gteEvict.maxDz = dz;
			}
		}
		else
		{
			s_gteEvict.sameMtx++;
		}
	}

	entry->key = key;
	entry->generation = buf->generation;
	entry->fracX8 = (u8)((sxFull & 0xFFFF) >> 8);
	entry->fracY8 = (u8)((syFull & 0xFFFF) >> 8);
	entry->sz3 = sz3;
	entry->hOverSz3 = hOverSz3;
	entry->vx = vx;
	entry->vy = vy;
	entry->vz = vz;
	entry->mtxId = (u8)s_gteMtxCurId;
	buf->mtxStores[s_gteMtxCurId]++;
}

/* Exhaust-smoke billboard corner. Particles project with raw gte_rtps_b and
 * never reach GTECapture_Store on their own, so a driver-local smoke quad
 * would miss the resolver and fall back to the parse-order ordinal axis. This
 * stores each corner's sz3 under its exact truncated screen key so the
 * resolver hits and stamps the quad onto the shared view.z axis. Sub-pixel/
 * hOverSz3/model-vector fields feed desnap/model lookups only, so they're
 * left at defaults. Runs on the game thread inside Particle_RenderList,
 * outside every other Store bracket. */
void GTECapture_StoreParticleCorner(s16 sx, s16 sy, u16 sz3)
{
	GTECapture_Store(sx, sy, (s64)sx << 16, (s64)sy << 16, sz3, 0, 0, 0, 0);
}

int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = GTECapture_FindLive(buf, key);

	if (entry == NULL)
	{
		return 0;
	}

	*fracX8 = entry->fracX8;
	*fracY8 = entry->fracY8;
	return 1;
}

int GTECapture_LookupModel(s16 sx, s16 sy, s16 outV[3])
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = GTECapture_FindLive(buf, key);

	if (entry == NULL)
	{
		return 0;
	}

	outV[0] = entry->vx;
	outV[1] = entry->vy;
	outV[2] = entry->vz;
	return 1;
}

// True view-space depth for a packet's screen coordinate -- the field that has
// sat here unread since capture was written ("reserved for stereo").
//
// This is the SAME axis the track renderer's shader calls view.z and the same
// axis retail quantises into an OT index: ctr_track.v.pica's header and
// native_gte_core.c:319-328 (C2_SZ3 = Lm_D(m_mac3, 1), clamped to unsigned
// 16-bit) establish that SZ3 is a linear view depth, NOT a reciprocal. That is
// what makes one shared depth axis possible at all.
//
// Same key, same generation/collision test, same miss semantics as every other
// lookup here: a miss is always a miss, never a wrong value.
int GTECapture_LookupDepth(s16 sx, s16 sy, u16 *outSz3)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = GTECapture_FindLive(buf, key);

	if (entry == NULL)
	{
		return 0;
	}

	*outSz3 = entry->sz3;
	return 1;
}

// Provenance variant -- see the header. Deliberately a separate entry point
// rather than an out-param bolted onto the above, so the existing consumers'
// generated code is unchanged and any behaviour difference this session sees is
// attributable to the new path alone.
int GTECapture_LookupDepthEx(s16 sx, s16 sy, u16 *outSz3, u8 *outMtxId)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = GTECapture_FindLive(buf, key);

	if (entry == NULL)
	{
		return 0;
	}

	*outSz3 = entry->sz3;
	*outMtxId = entry->mtxId;
	return 1;
}

/* Miss forensics: was this exact screen coordinate projected in a RECENT
 * prior frame? Entries are never wiped between frames -- only outdated by the
 * generation bump -- so a stale-generation hit at the key IS last-projected
 * evidence, with its age in frames and the depth it carried. A hit with age
 * 1-2 convicts the stale-coordinates theory (a packet drawn with screen XY
 * projected in an earlier frame); the sz3 it returns is the primitive's REAL
 * depth at that time. Diagnostic only -- never feed this into a stamp: an aged
 * depth for moving content is exactly the lie the resolver exists to avoid. */
int GTECapture_LookupDepthStale(s16 sx, s16 sy, u16 *outSz3, u32 *outAgeFrames)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->key != key) || (entry->generation == 0) || (entry->generation >= buf->generation))
	{
		return 0;
	}

	*outSz3 = entry->sz3;
	*outAgeFrames = buf->generation - entry->generation;
	return 1;
}

/* Miss forensics iteration 4: what does the missing coordinate's hash SLOT
 * actually hold? The table is direct-mapped, so a key-mismatch lookup cannot
 * tell "never stored" from "stored and then clobbered by a different pixel
 * hashing to the same slot" -- this probe returns the occupant so the log can.
 * genState: 2 = occupant written THIS frame (a live clobberer -- its key names
 * the thief's screen pixel), 1 = an older frame's leftover, 0 = never written
 * since boot. Diagnostic only. */
int GTECapture_ProbeSlot(s16 sx, s16 sy, s16 *outOccX, s16 *outOccY, int *outGenState, u16 *outOccSz3)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	*outOccX = (s16)(entry->key & 0xFFFFu);
	*outOccY = (s16)(entry->key >> 16);
	*outOccSz3 = entry->sz3;
	*outGenState = (entry->generation == 0) ? 0 : (entry->generation >= buf->generation) ? 2 : 1;
	return entry->key != key; /* 1 = the slot belongs to someone else */
}

int GTECapture_LookupModelEx(s16 sx, s16 sy, s16 outV[3], u8 *outMtxId)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = GTECapture_FindLive(buf, key);

	if (entry == NULL)
	{
		return 0;
	}

	outV[0] = entry->vx;
	outV[1] = entry->vy;
	outV[2] = entry->vz;
	*outMtxId = entry->mtxId;
	return 1;
}

// Read-buffer matrix census for the walk: dominant id (most Stores -- in
// race content, the camera), its share in percent of all Stores, and how
// many distinct ids the frame used. Share 0 means the frame stored nothing.
u32 GTECapture_ReadMatrixCensus(u8 *outDomId, u32 *outIdsUsed)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 total = 0, best = 0, bestId = 0, i;

	for (i = 0; i < GTE_CAPTURE_MTX_IDS; i++)
	{
		total += buf->mtxStores[i];
		if (buf->mtxStores[i] > best)
		{
			best = buf->mtxStores[i];
			bestId = i;
		}
	}

	*outDomId = (u8)bestId;
	*outIdsUsed = buf->mtxUsed;
	return total ? (best * 100u) / total : 0;
}

// Read-buffer companion to the census: the CP2C words behind a run id.
// Returns 0 for an id that stored nothing this frame (its words slot may be
// stale from an earlier frame).
int GTECapture_ReadMatrixWords(u8 id, u32 out[8])
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];

	if ((id >= GTE_CAPTURE_MTX_IDS) || (buf->mtxStores[id] == 0))
	{
		return 0;
	}
	memcpy(out, buf->mtxWords[id], sizeof(buf->mtxWords[0]));
	return 1;
}

// Walk-thread state: BeginOTWalk/MarkOTNode/LookupSuppressed all run on
// whichever thread owns the walk (worker on 3DS, main on PC) -- one walk in
// flight, so a single flag needs no buffering. s_gteCaptureExemptNode is
// per-NODE, not sticky: it holds only while the walk is inside the flag's
// registered prim range (see the exempt fields on the buffer).
global_variable int s_gteCaptureSuppress;
global_variable int s_gteCaptureExemptNode;
global_variable int s_gteCapturePaintNearNode;
global_variable u16 s_gteCapturePaintNearPin;

void GTECapture_SetUIBand(const void *start, const void *end)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->bandStart = start;
	buf->bandEnd = end;
}

void GTECapture_SetUIBandExemptRange(const void *start, const void *end)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->exemptStart = start;
	buf->exemptEnd = end;
}

/*
 * Register one paint-on-top prim range for this frame (header comment on
 * g_dbg_paintOnTop). Game-thread only, same ownership as Store; the range
 * must be a [start, end) pair of gGT->backBuffer->primMem cursors bracketing
 * exactly the member's own packets -- the pool the OT walk visits.
 * flatParallax: 0 = 3D member, stereo depth from its own captures; 1 = flat
 * overlay art, stereo flattened to the UI band's screen-plane treatment.
 */
void GTECapture_AddPaintOnTopRange(const void *start, const void *end, int flatParallax)
{
	GTECapture_AddPaintOnTopRangePinned(start, end, flatParallax, 0);
}

/*
 * Same registration with an explicit occlusion stamp (0 = PKTDEPTH_WORLD_MAX).
 * A range that continues the previous one with the same flavor and pin extends
 * it: a held shield's three layers draw back to back, and the walk tests every
 * OT node against every range.
 */
void GTECapture_AddPaintOnTopRangePinned(const void *start, const void *end, int flatParallax, u16 pinStamp)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];
	u8 flavor = (u8)(((flatParallax >= 0) && (flatParallax <= 2)) ? flatParallax : 1);

	if ((g_dbg_paintOnTop == 0) || (end <= start))
	{
		return;
	}
	if ((buf->paintNearN > 0) && (buf->paintNearEnd[buf->paintNearN - 1] == start) && (buf->paintNearFlat[buf->paintNearN - 1] == flavor) &&
	    (buf->paintNearPin[buf->paintNearN - 1] == pinStamp))
	{
		buf->paintNearEnd[buf->paintNearN - 1] = end;
		return;
	}
	if (buf->paintNearN >= GTE_CAPTURE_PAINTNEAR_MAX)
	{
		return;
	}
	buf->paintNearStart[buf->paintNearN] = start;
	buf->paintNearEnd[buf->paintNearN] = end;
	buf->paintNearFlat[buf->paintNearN] = flavor;
	buf->paintNearPin[buf->paintNearN] = pinStamp;
	buf->paintNearN++;
}

void GTECapture_BeginOTWalk(void)
{
	s_gteCaptureSuppress = 0;
	s_gteCaptureExemptNode = 0;
	s_gteCapturePaintNearNode = 0;
}

void GTECapture_MarkOTNode(const void *node)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];

	// Sticky: the UI band sits at the tail of the reversed OT walk, so once
	// entered, every later packet this walk is UI. Reset by BeginOTWalk.
	if ((node >= buf->bandStart) && (node < buf->bandEnd))
	{
		s_gteCaptureSuppress = 1;
	}
	// Exemption is per NODE and evaluated on EVERY node, not only inside the
	// UI band: since the channel split its consumer is the PARALLAX channel
	// (native_gpu.c's resolver), and the flag needs its ripple in the menu
	// pose too -- where its packets sit in an ordinary world bucket and the
	// band was never entered. Two pointer compares per node against a range
	// that is NULL/NULL on any frame RaceFlag_DrawSelf did not register.
	s_gteCaptureExemptNode = ((node >= buf->exemptStart) && (node < buf->exemptEnd));

	// Paint-on-top ranges, same per-node discipline. paintNearN is 0 on any
	// frame no member drew, so the loop body never runs then. The stored
	// value is the flavor (1 = 3D member, 2 = flat overlay, 3 = flat +
	// wide-stretch), not just a hit.
	{
		u32 i;

		s_gteCapturePaintNearNode = 0;
		for (i = 0; i < buf->paintNearN; i++)
		{
			if ((node >= buf->paintNearStart[i]) && (node < buf->paintNearEnd[i]))
			{
				s_gteCapturePaintNearNode = 1 + (int)buf->paintNearFlat[i];
				s_gteCapturePaintNearPin = buf->paintNearPin[i];
				break;
			}
		}
	}
}

int GTECapture_LookupSuppressed(void)
{
	// Deliberately the RAW sticky flag. The flag-range exemption used to be
	// subtracted here, which put flag packets on world OCCLUSION stamps; the
	// channel split moved that job to the parallax channel and handed
	// occlusion back to retail's own layering (see GTECapture_NodeExempt).
	return s_gteCaptureSuppress;
}

/*
 * Is the walk currently inside the registered flag prim range? (channel
 * split.)
 *
 * Read by PacketDepth_ResolveXY for TWO things, and the pairing is the whole
 * design:
 *   - OCCLUSION: an exempt primitive resolves exactly as if its stores had
 *     been suppressed -- a miss. That is retail's own painter's statement:
 *     the flag paints over everything during a
 *     screen-covering wipe (its packets are in the UI band there) and sits
 *     behind the menu models when parked (bucket-derived far depth). Pose
 *     changes are retail draw order, not a stereo jump.
 *   - PARALLAX: the same primitive's captures ARE read, and they carry the
 *     compressed ripple band the store-side remap wrote, in every pose,
 *     continuously. This is the channel the owner's no-pose-conditional rule
 *     governs.
 */
int GTECapture_NodeExempt(void)
{
	return s_gteCaptureExemptNode;
}

/*
 * Is the walk inside a registered paint-on-top range? Consumed by
 * PacketDepth_ResolveXY: OCCLUSION pins to the nearest world stamp (the
 * member is retail-parked inside geometry, and retail's painter drew it
 * after the ground -- the pin is that draw-order statement translated to the
 * depth axis, kept under the UI band so the HUD still covers it), PARALLAX
 * reads the member's own captures so stereo keeps its real shape.
 */
int GTECapture_NodePaintNear(void)
{
	return s_gteCapturePaintNearNode;
}

// Occlusion stamp of the range NodePaintNear matched; 0 = the world maximum.
int GTECapture_NodePaintNearPin(void)
{
	return (int)s_gteCapturePaintNearPin;
}

#endif /* defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE */
