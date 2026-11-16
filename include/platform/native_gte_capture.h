#ifndef NATIVE_GTE_CAPTURE_H
#define NATIVE_GTE_CAPTURE_H

#include <macros.h>

// Per-frame side table of GTE sub-pixel fractions discarded by the >>16 in
// GTE_RotTransPers, keyed by the exact truncated screen coordinate written to
// the primitive packet. Declarations are
// always visible; each caller gates its own use behind CTR_NATIVE_GTE_CAPTURE,
// so a flag-off build never emits a call and these never need to be defined.

// Runtime ablation of the capture writes, for measuring what the subsystem
// costs before a feature commits to depending on it. Three states, because the
// two per-vertex calls answer different questions:
//   1 = full capture (NoteMatrix + Store) -- today's compiled-in behavior
//   2 = Store only, no NoteMatrix -- what a depth consumer actually needs
//   0 = neither; every later lookup misses and each consumer takes the exact
//       snapped fallback it already has for collisions and evictions, so 0 is
//       behaviorally today's flag-off build.
// 1-vs-2 prices the matrix-class probe; 2-vs-0 prices the table itself.
// Defined only in CTR_NATIVE_GTE_CAPTURE builds (every caller gates on it).
extern int g_dbg_gteCapture;

// Store hold for one model instance the native model path owns (native draw
// replaces the packet write). Nothing reads the capture entries of a claimed
// triangle, so its projections skip Store; the instance's matrix is noted once
// at Hold so matrix ids advance as if it had stored. Implemented as a
// save-and-zero of g_dbg_gteCapture, so GTE_RotTransPers pays no extra test.
// Game-thread only; Release is idempotent and must run on every instance exit.
void GTECapture_HoldStoresForOwnedModel(void);
void GTECapture_ReleaseStoresForOwnedModel(void);
// Re-runs the GTE projection of one triangle (packed VXY/VZ register pairs,
// as RenderBucket_LoadPrimRTPT loads them) so its three vertices reach the
// table, then restores every GTE register the projection touched. Cold path:
// a triangle the native path declined inside an owned instance still writes a
// packet, and that packet looks its depth up here.
void GTE_CaptureStoreTriangle(const u32 *packedXY3, const u32 *packedZ3);

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz);
int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8);
/* Store one exhaust-smoke billboard corner's depth (sz3) keyed by its
 * truncated screen coordinate, so the packet-depth resolver HITS for a
 * driver-local smoke quad and stamps it onto the shared view.z axis instead
 * of the parse-order ordinal it would otherwise fall back to. Particles
 * project with raw gte_rtps_b and never call GTECapture_Store themselves,
 * so without this a deferred smoke draw reads as infinitely far and loses
 * every depth test. Game-thread only, same ownership as Store. See
 * game/Particle.c. */
void GTECapture_StoreParticleCorner(s16 sx, s16 sy, u16 sz3);
/* Deferred-smoke pass plumbing (native_gpu.c). DrawDeferredSmoke redraws the
 * captured driver-local smoke after the model pass (called from
 * native_renderer_3ds.c); ResetDeferredSmoke clears the capture buffer at the
 * frame boundary (called from MainFrame_ResetDB, beside GTECapture_Clear). */
void NativeGpu_DrawDeferredSmoke(void);
void NativeGpu_ResetDeferredSmoke(void);
/* s42d driver-local smoke packet-address tag (native_gpu.c). Mark: game thread,
 * per smoke packet, during Particle_RenderList. Publish: DrawOTag, read=write
 * before the walk is posted. Clear: MainFrame_ResetDB, flip write at frame
 * start. See the definitions for the double-buffer contract. */
void NativeGpu_MarkSmokePacket(uintptr_t addr);
void NativeGpu_PublishSmokePackets(void);
void NativeGpu_ClearSmokePackets(void);
/* Skybox packet-range tag (native_gpu.c). Pins DrawSky_Full's packets to
 * the far depth plane so translucent track geometry (the waterfalls) stops
 * losing the depth test to the sky's OT-bucket fallback. Mark: game thread,
 * once per frame, around DrawSky_Full's emit. Publish: DrawOTag, read=write
 * before the walk is posted. Clear: MainFrame_ResetDB, flip write at frame
 * start. Same double-buffer contract as the smoke tag above. */
void NativeGpu_MarkSkyRange(uintptr_t start, uintptr_t end);
/* s46: CAM_SkyboxGlow marks through this instead -- unions into the sky span
 * (same consumers) AND records the glow-only slice, which the 3DS emit path
 * stretches to the REAL-wide ortho window (the glow's retail emitter is
 * screen-space, hardcoded 512 wide). Same lifecycle as the sky pair. */
void NativeGpu_MarkGlowRange(uintptr_t start, uintptr_t end);
/* RenderStars marks its tile span through this -- far-pin only, kept OUT of
 * the backdrop union: stars emit at the top of the render frame, so the whole
 * world sits between them and the sky pair and a union would swallow it.
 * Same lifecycle as the sky pair. */
void NativeGpu_MarkStarRange(uintptr_t start, uintptr_t end);
/* UI_RenderFrame_Racing marks its minimap block (map sheet + racer icons)
 * through this so the 3DS can restamp those primitives for the bottom-screen
 * map (PKTDEPTH_UI_STAMP_BTMMAP, dumpreq 'btmmap'). One producer, one
 * contiguous block per frame, so this overwrites rather than unions. Same
 * lifecycle as the sky pair; inert on PC (nothing consumes the tag there).
 * retailTop = nonzero when the game's own HUD condition emitted the block
 * (retail would draw it on the top screen); zero when it exists only because
 * NativeUiMap_RouteActive overrode the speedo-mode skip, or for the hub map.
 * The 3DS draws retailTop blocks on BOTH screens (top copy re-pinned into the
 * ordered-UI list, dumpreq 'mapboth'); override blocks stay bottom-only --
 * routing's legality in speedo mode rests on them never reaching the top. */
void NativeGpu_MarkUiMapRange(uintptr_t start, uintptr_t end, int retailTop);
int NativeGpu_UiMapRetailTop(void);
/* Sub-span of the map block: the FPS overlay text, restamped
 * PKTDEPTH_UI_STAMP_BTMFPS so the bottom pass can draw it through its own
 * (non-magnifying) window. Marked strictly inside the map bracket; the
 * resolver checks this span before the map's. Same lifecycle. */
void NativeGpu_MarkUiFpsRange(uintptr_t start, uintptr_t end);
/* Picture-sprite span (PKTDEPTH_UI_STAMP_UIPIC): 2D picture sprites that get
 * the seam-closing far-edge UV inset but are never routed off the top screen
 * -- the track-select and adventure-hub map sheets. Sheet quads only, never
 * 1:1 glyph text (the inset duplicates rows on 1:1 content). One producer
 * per frame, overwrite semantics; same lifecycle; inert on PC. */
void NativeGpu_MarkUiPicRange(uintptr_t start, uintptr_t end);
/* Juiced-wumpa HUD shine span (PKTDEPTH_UI_STAMP_HUDSHINE): UI_Weapon_DrawBG's
 * quads, which retail links into the player's OT rather than the UI OT. Marked
 * once per player per frame and kept as separate entries -- split-screen HUDs
 * interleave world-anchored primitives between two players' shines, so never a
 * union. Same lifecycle as the sky pair; inert on PC. Runtime A/B: dumpreq
 * 'hudshine' (default ON). */
void NativeGpu_MarkHudShineRange(uintptr_t start, uintptr_t end);
/* Full-window fade span (PKTDEPTH_UI_STAMP_SCREENFADE): PushBuffer_FadeOneWindow's
 * quad, which retail links into each player's OT rather than the UI OT. Marked
 * once per pushbuffer per frame and kept as separate entries, never a union.
 * Same lifecycle as the sky pair; inert on PC. Runtime A/B: dumpreq
 * 'screenfade' (default ON). */
void NativeGpu_MarkScreenFadeRange(uintptr_t start, uintptr_t end);
/* Lock-on bracket span (PKTDEPTH_UI_STAMP_TRACKER): UI_TrackerSelf's prims,
 * which retail links into each player's OT rather than the UI OT. Marked once
 * per player per frame and kept as separate entries, never a union. Same
 * lifecycle as the sky pair; inert on PC. Runtime A/B: dumpreq 'trackerui'
 * (default ON). */
void NativeGpu_MarkTrackerRange(uintptr_t start, uintptr_t end);
/* The character-select diorama backdrop (MM_Characters' RwdBlueRect strips):
 * screen-space quads with no captured depth, which the resolver would
 * otherwise leave on the raw emit ordinal where the menu flag buries them.
 * Stamped PKTDEPTH_MENU_BACKDROP instead -- between the flag's band and the
 * preview model. One producer per frame, overwrite semantics; same lifecycle
 * as the sky pair; inert on PC. */
void NativeGpu_MarkMenuBackdropRange(uintptr_t start, uintptr_t end);
void NativeGpu_PublishSkyRange(void);
void NativeGpu_ClearSkyRange(void);

/* Walk census (CTR_NATIVE_WALK_CENSUS, an instrument, default OFF): the
 * primMem cursor before each producer call in MainFrame_RenderFrame, so the
 * walk can say which producer each node it visits came from. Marks are set in
 * call order; a stage whose call did not run this frame stays 0 and is
 * skipped. Same double-buffer lifecycle as the sky pair. */
#if defined(CTR_NATIVE_WALK_CENSUS) && defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
#define NATIVE_WALK_CENSUS 1
enum NativeWalkCensusStage
{
	WCEN_SETUP,
	WCEN_WEATHER,
	WCEN_CONFETTI,
	WCEN_STARS,
	WCEN_HUD,
	WCEN_BEAKER,
	WCEN_QUEUE,
	WCEN_PARTICLES,
	WCEN_DISPENV,
	WCEN_BURSTS,
	WCEN_TURBO,
	WCEN_SKIDS,
	WCEN_TITLEDPP,
	WCEN_EXECUTE,
	WCEN_TIRES,
	WCEN_SHADOWS,
	WCEN_HEAT,
	WCEN_FADE,
	WCEN_LEVEL,
	WCEN_POSTLEVEL,
	WCEN_REFRESHCARD,
	WCEN_CLEAR,
	WCEN_UI,
	WCEN_END
};
void NativeGpu_WalkCensusMark(int stage, const void *cursor);
#define WALK_CENSUS_MARK(gGT, stage) NativeGpu_WalkCensusMark((stage), (gGT)->backBuffer->primMem.cursor)
#else
#define WALK_CENSUS_MARK(gGT, stage) ((void)0)
#endif
// Model-space vector that produced this screen coordinate (same rendezvous
// and same miss-on-collision safety as GTECapture_Lookup). Consumed by the
// OTWALK_PROFILE retainability instrument; the retained-geometry play proper
// will grow its own keyed-by-content variant.
// True view-space depth (SZ3) for a packet's screen coordinate -- the shared
// depth axis the track renderer already uses.
int GTECapture_LookupDepth(s16 sx, s16 sy, u16 *outSz3);
// Same, plus the matrix-state id the stored projection ran under (s32 sinking
// racers). The packet-depth path uses it as PROVENANCE: an entry stored under
// the frame's dominant matrix came from camera-space world geometry (track,
// scenery), one stored under any other came from a per-object instance (karts,
// crates). A kart primitive whose vertices come back carrying the camera's id
// is reading a depth that belongs to the road it is standing on.
int GTECapture_LookupDepthEx(s16 sx, s16 sy, u16 *outSz3, u8 *outMtxId);
// Miss forensics: stale-generation probe -- was this coordinate projected
// in a recent PRIOR frame? Diagnostic only; see the definition's comment.
int GTECapture_LookupDepthStale(s16 sx, s16 sy, u16 *outSz3, u32 *outAgeFrames);
// Miss forensics iteration 4: raw hash-slot occupant for a coordinate -- distinguishes
// never-stored from clobbered-by-collision. See the definition's comment.
int GTECapture_ProbeSlot(s16 sx, s16 sy, s16 *outOccX, s16 *outOccY, int *outGenState, u16 *outOccSz3);
int GTECapture_LookupModel(s16 sx, s16 sy, s16 outV[3]);
// Model vector plus the frame-scoped matrix-state id the projection ran
// under (matrix-class probe; the retained full form's matrix classes).
int GTECapture_LookupModelEx(s16 sx, s16 sy, s16 outV[3], u8 *outMtxId);
// Per projected vertex, from the GTE: the 8 contiguous CP2C words (R+TR).
void GTECapture_NoteMatrix(const void *words8);
// Read-buffer census: returns dominant-id Store share in percent.
u32 GTECapture_ReadMatrixCensus(u8 *outDomId, u32 *outIdsUsed);
// The 8 CP2C words (R11..R33 packed, TRX/Y/Z) that opened run `id`; 0 if the
// id stored nothing this frame.
int GTECapture_ReadMatrixWords(u8 id, u32 out[8]);

// EVICTION CENSUS -- the direct measurement of the failure this file
// already documents for the skybox (see GTECapture_SetStoreSuppressed): the
// table holds one entry per screen coordinate, last writer wins, so a vertex
// can overwrite ANOTHER OBJECT's depth at the same pixel and every later
// lookup there returns a foreign value. A kart's wheels and the road
// beneath them occupy the same screen coordinates by construction, which
// makes this the leading candidate for the sinking racers.
//
// Counts same-key, same-generation overwrites, split by whether the
// incoming store's matrix state differs from the resident entry's. Returns
// the different-matrix count (the harmful population) and clears the
// accumulator; *outSameMtx is the benign self-overwrite count, *outMaxDz
// the largest sz3 difference any harmful overwrite introduced.
u32 GTECapture_ReadEvictCensus(u32 *outSameMtx, u32 *outMaxDz);

// Instance depth-axis bracket: retail projects RenderBucket instances on
// a regime-scaled depth axis (x4 nearer than 0x1000 view units, /4 for far
// DRAW_HUGE), so their SZ3 stores were 4x wrong on the shared axis and a
// table HIT buried the instance (the miss fallback is OT-bucket-derived and
// was always roughly right). The bracket wraps one instance's draw-func
// execution; stores inside it are classified {4x,1x,1/4x} against the
// instance origin's track-axis view.z and unscaled before anything reads
// them. mvpZIfNormal: idpp->mvp.t[2] for the NORMAL/NORMAL_ALT funcs (exact
// classification), or -1 for split/special/reflection (mvp holds a view or
// identity matrix there; classification then uses the first stored SZ3).
// funcKind: 0=normal 1=normalAlt 2=split 3=special 4=reflection.
// Implementation comment in native_gte_capture.c has the full story.
struct Instance;
struct PushBuffer;
void GTECapture_BeginInstanceDepth(const struct Instance *inst, const struct PushBuffer *pb, s32 mvpZIfNormal, int funcKind);
/* Names the LOD header the next GTECapture_BeginInstanceDepth draws, for the
 * [WATCH] line's lod= field. Call immediately before it. */
void GTECapture_WatchNoteLod(const char *lodName);
/* Fixed-shift variant for producers whose regime is known from source
 * rather than measured -- the ground-decal trio (shadows, skids, tires), which
 * pre-multiply camera-relative position by 4 and run outside RenderBucket.
 * Same shift encoding as the classifier (2 = divide the stored SZ3 by 4).
 * Closed by GTECapture_EndInstanceDepth. */
void GTECapture_BeginFixedDepthShift(int shift);
/* "Under" mode inside a fixed bracket: stores take the decal margin FARTHER
 * instead of nearer, so the content loses the ground depth tie -- for what
 * retail tucks UNDER the opaque road by painter's order (the mirrored
 * ice-reflection tires). Toggle around the DrawTires_Reflection calls; the
 * bracket teardown clears it. No-op on PC (the bias is 3DS-only). */
void GTECapture_SetFixedShiftUnder(int on);
void GTECapture_EndInstanceDepth(void);
// The [PKTAXIS] instrument rides --model-depth-check.

void GTECapture_Clear(void);
// Double-buffer publish: called by the DrawOTag facade on the game
// thread, before the walk is posted (3DS worker) or run (PC sync) -- the walk
// then reads the buffer the frame just finished writing, while ResetDB's
// Clear() flips subsequent Stores to the other one.
void GTECapture_PublishForWalk(void);

// UI suppression. The frame's single DrawOTag walk renders world geometry
// first, then crosses into the swapchain OT's low six entries (the
// pushBuffer_UI band) and draws every UI packet last. UI coordinates come
// from game math, not this frame's GTE writes, so a lookup that HITs for one
// is always a false hit (foreign fraction -> visible shimmer). The walk
// reports each visited node; once one lands inside the band, lookups are
// suppressed until the next walk begins.
void GTECapture_SetUIBand(const void *start, const void *end);
void GTECapture_BeginOTWalk(void);
void GTECapture_MarkOTNode(const void *node);
// A run of untouched OT slots [start, end) the walk stepped over without a
// MarkOTNode per slot. Enters the band if the run overlaps it.
void GTECapture_MarkOTSlotRun(const void *start, const void *end);
// A zero-length node the walk passed without a MarkOTNode. Enters the band if
// the node lies in it.
void GTECapture_MarkOTEmptyNode(const void *node);
int GTECapture_LookupSuppressed(void);

// DEPTHLESS-GEOMETRY suppression. Bracket any producer whose GTE
// projection runs with the translation vector zeroed -- i.e. geometry drawn
// as if infinitely far, whose SZ3 is a function of the mesh's own radius and
// NOT of its distance from the camera. DrawSky_Full is the one such producer
// in the game. Stores are dropped between Set(1) and Set(0); lookups for
// those screen coordinates then miss, and the packet-depth path falls back
// to deriving depth from the OT bucket, which IS retail's own statement of
// how far the content is. See GTECapture_SetStoreSuppressed's definition
// for the bug this closes.
//
// Game-thread only, same ownership as Store itself. Not a runtime toggle:
// the suppressed population has no valid depth to capture in any
// configuration.
void GTECapture_SetStoreSuppressed(int suppressed);

// AFFINE DEPTH-REMAP bracket (the menu/loading checkered flag). Where
// suppression drops a producer's depths, this bracket KEEPS them, compressed
// to ~1/4 relief and re-anchored just behind the stereo convergence plane --
// the flag's wave stays 3D in stereo while its band can no longer cross a
// menu model's. Same game-thread ownership and set-and-clear-in-one-producer
// discipline as SetStoreSuppressed; constants and the full design rationale
// sit on the definition (native_gte_capture.c).
void GTECapture_SetStoreDepthRemap(int active);

// Walk-side partner of the remap: the prim range of the flag's packets for
// THIS frame, registered by RaceFlag_DrawSelf (double-buffered with the UI
// band, cleared by Clear).
//
// Since the occlusion/parallax channel split
// this range no longer changes any OCCLUSION stamp -- it marks the primitives
// whose PARALLAX depth must come from the (remapped) capture entries rather
// than from the stamp they occlude by. The flag's screen coordinates DO come
// from this frame's GTE writes, so the UI band's false-hit rationale never
// applied to them; what did apply, and what the split fixes, is that retail
// wants the flag's LAYERING to change with pose while the stereo eye wants
// its wave to stay put.
void GTECapture_SetUIBandExemptRange(const void *start, const void *end);
// Walk-side query for the above -- see the definition for the two-channel
// contract. Meaningful only while a walk is in progress.
int GTECapture_NodeExempt(void);

// PAINT-ON-TOP models (the talking-mask hint heads). Retail parks these
// instances inside hub geometry and its painter's algorithm carries them --
// drawn after the ground, so they never clip. There is no retail-authored
// signal to key on (depthBiasNormal stays at the birth default), so the class
// is a native allowlist matched by model name. Members' packet ranges are
// registered by the instance depth bracket (no game-code registration, unlike
// the flag's), and the resolver pins their OCCLUSION to the nearest world
// stamp -- under the UI band, so the HUD still covers them -- while PARALLAX
// stays honest, same channel split as the flag. Runtime A/B: dumpreq 'ontop'
// (default ON; OFF is the buried-in-ground behaviour).
// The held shield bubble is the second member kind: retail has no depth buffer,
// so its sphere draws whole over the ground. It is matched by INSTANCE name (the
// thrown shield projectile reuses the models and stays a world object) and pins
// just in front of its own kart rather than at the world maximum, so scenery
// well in front of a distant racer's bubble still hides it.
// The podium prize is the third: matched by INSTANCE name ("prize"), pinned at
// the world maximum like the talking heads, since a tall winner's raised arms
// reach its resting point and retail paints it whole over them.
extern int g_dbg_paintOnTop;
int GTECapture_ModelPaintsOnTop(const char *modelName);
// Instance-level membership: 0 = no, 1 = talking head or podium prize, 2 = held shield bubble layer.
int GTECapture_InstPaintsOnTop(const struct Instance *inst);
// PAINT-UNDER members: the mirror class -- geometry retail hides purely by
// painter's order (coincident with, or a hair OUTSIDE, the surface that covers
// it; the intro crate's plain BOX_02A shell). Members draw through the packet
// path with their captured depth biased farther. Runtime A/B: dumpreq 'under'
// (default ON; OFF paints the intro crate flat orange).
extern int g_dbg_paintUnder;
int GTECapture_InstPaintsUnder(const struct Instance *inst);
// dumpreq 'hide NAME': the watched instance skips its draw func (diagnostic).
extern int g_dbg_watchHide;
int GTECapture_WatchIsHiddenInstance(void);
// Walk-side query: 0 = not a member, 1 = 3D member (parallax from its own
// captures), 2 = flat overlay (parallax flattened to the UI band), 3 = flat
// overlay whose X is also stretched to the wide window (retail sized it for
// the 4:3 screen -- the lens flare).
int GTECapture_NodePaintNear(void);
// Second membership route, for flat 2D overlay art drawn into the WORLD OT
// (stoplight, "FINAL LAP" -- retail links them there so the character icons
// layer over them, which our UI-band suppression therefore never covers):
// the producer registers its own packet range, RaceFlag-style, since there
// is no instance bracket to latch it from. flatParallax = 0/1/2, mapping to
// NodePaintNear's 1/2/3.
void GTECapture_AddPaintOnTopRange(const void *start, const void *end, int flatParallax);
// Same, with an explicit occlusion stamp (0 = the nearest world stamp). Used by the held shield bubble,
// which pins just in front of its own kart so only nearby ground is overdrawn.
void GTECapture_AddPaintOnTopRangePinned(const void *start, const void *end, int flatParallax, u16 pinStamp);
// Occlusion stamp of the registered range the current walk node sits in (valid while NodePaintNear != 0).
int GTECapture_NodePaintNearPin(void);

#endif
