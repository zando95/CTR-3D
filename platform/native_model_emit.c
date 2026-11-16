/*
 * native_model_emit.c -- per-frame model triangle capture.
 * documents the capture hooks and design; this file is the buffer and its ownership rules.
 * Produces a double-buffered snapshot of one frame's model geometry in model space plus
 * per-instance camera records, captured alongside the packet path so it can be checked against a
 * renderer already known correct before any shader exists.
 */

#include "platform/native_model_emit.h"

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER

/* 0 unless CTR_NATIVE_RENDERER_DEFAULT_ON is configured -- see that option's
 * comment in CMakeLists.txt for why the boot default lives there and not here.
 * The runtime toggle is unaffected either way. */
#ifndef CTR_NATIVE_RENDERER_DEFAULT_ON
#define CTR_NATIVE_RENDERER_DEFAULT_ON 0
#endif

int g_dbg_modelRender = CTR_NATIVE_RENDERER_DEFAULT_ON;

/*
 * CAPS. Measured via [MODELFRAME] on hardware: ~1500 triangles/frame across ~9 instances in
 * Arcade on N. Gin Labs, ~440 across ~5 in Time Trial. That figure is the topology maximum of
 * the headers selected, not the post-rejection capture rate, so treat 1500 as an upper bound,
 * not typical. 4096 is ~2.7x the measured worst case; overflow is reported, never silent.
 */
#define MODELEMIT_MAX_TRIS      4096
#define MODELEMIT_MAX_VERTS     (MODELEMIT_MAX_TRIS * 3)
#define MODELEMIT_MAX_INSTANCES 128
/* Paint-under depth push (see NativeModelEmitInstance.paintUnder). 1.10 puts a
 * shell 2 units outside a crate ~8 units behind its skin at the intro close-up. */
#define NATIVE_MODEL_PAINT_UNDER_SCALE 1.10f

/* Matches [DEMIT]/[MODELFRAME] cadence -- one line per 64 content frames. */
#define MODELEMIT_WINDOW_FRAMES 64

/*
 * 16 bytes. Not yet the GPU-side layout: rung 2 mirrors this into an
 * N3DS_ModelVertex across the plain-scalar boundary the same way
 * N3DS_TrackVertex mirrors NativeTrackVert, because the GPU form has to satisfy
 * PICA200 attribute-alignment rules that have nothing to do with what is
 * convenient here (see n3ds_backend.h's N3DS_Vertex comment for why that
 * matters on hardware and is invisible in Azahar).
 *
 * Position is the GTE's own model-space input, straight from the register pair
 * the projection was about to consume -- see the header on why it is taken from
 * there rather than re-derived.
 */
struct NativeModelEmitVert
{
	s16 x, y, z;
	/* Page-space texel coordinates, 0..255, exactly as the TextureLayout
	 * stores them. NOT atlas coordinates: which atlas cell this face resolves
	 * to is a render-side decision that changes frame to frame as the decode
	 * cache evicts, so resolving here would bake a stale answer into a
	 * snapshot the walk reads later. The track renderer can pre-bake its UVs
	 * because its atlas is packed once per level; this cannot. */
	s16 u, v;
	u8 r, g, b;
	u8 _pad;
};

/* 8 bytes, one per triangle. tpage/clut are the pair that costs a state change
 * -- the same batching key the packet path's resolver takes. */
struct NativeModelEmitFace
{
	u16 tpage;
	u16 clut;
	u8 textured;
	/* Writer-forced blend for this face as a BlendMode (0 = opaque, else
	 * ABR+1); 0xFF = no override, classify by tpage. The specular writers
	 * choose ABE per SIDE, so neither the instance nor the tpage can carry
	 * this -- see NativeModelDraw_FaceBlend. */
	u8 blendOverride;
	u8 _pad[2];
};

/*
 * One instance's camera record. RAW retail values, no pre-divide -- same
 * convention as struct NativeTrackFrameSnapshot, and for the same reason: the
 * draw side composes the projection rows from these, so there is exactly one
 * copy of that derivation to keep correct.
 *
 * rot/trans are idpp->mvp, the instance's model->VIEW matrix. h/ofx/ofy are the
 * camera terms, identical for every instance sharing a PushBuffer but recorded
 * per instance so a split-screen frame needs no extra bookkeeping.
 */
struct NativeModelEmitInstance
{
	u32 firstTri;
	u32 triCount;
	s16 rot[3][3];
	s32 trans[3];
	s32 h;
	s32 ofx, ofy;
	/*
	 * This instance's primitive writer forces semi-transparency regardless of what its textures
	 * say. Two of the seven model primitive writers (RB_RETAIL_INST_PRIM_GHOST,
	 * RB_RETAIL_INST_PRIM_DEPTH_FADE) write semi-transparent code 0x36 unconditionally without
	 * consulting the texture's ABR field, so this must be a per-instance flag driven by the writer
	 * (inst->funcPtr[1]), not derived per-face from the texture page.
	 */
	u8 forceSemiTrans;

	/*
	 * GHOST-writer composite (RenderBucket_DrawInstPrim_GhostAtRange). Every primitive of that
	 * writer is TWO packets: a flat B-F mask of the scratch fadeColor over the triangle's
	 * footprint, then an additive body -- textured prims get their tpage ABR field overwritten
	 * to 1, untextured ones draw behind the writer's own ABR-1 DR_TPAGE -- whose colours went
	 * through the writer's per-prim gte_dpct (IR0 = alphaScale, FC = the setup callback's far
	 * colour). Subtract-then-add is the whole ghost look; either half alone reads as a washed-out
	 * solid. Implies forceSemiTrans. fade* is the mask colour, captured at BeginInstance because
	 * only the instance setup callback writes the scratch word.
	 */
	u8 ghostComposite;
	u8 fadeR, fadeG, fadeB;

	/*
	 * Depth-axis regime of this instance's mvp. Retail's instance pipeline is regime-scaled:
	 * RenderBucket_AdjustViewPositionForMvp shifts MVP translation <<2 when the instance origin is
	 * nearer than 0x1000 view units, and DRAW_HUGE shifts >>2. trueViewZ = mvpViewZ >> depthShift;
	 * 2 = near regime (mvp is 4x), 0 = far regime (mvp is true), -2 = far DRAW_HUGE (mvp is
	 * true/4). The draw side divides hParam.x by the factor to correct depth without touching
	 * screen position.
	 */
	s8 depthShift;
	/* Paint-under member (GTECapture_InstPaintsUnder): the draw side scales
	 * hParam.x UP so this instance's depth reads farther than its cover's,
	 * the depth-axis translation of retail drawing it first. */
	u8 paintUnder;

	/*
	 * Instance fade inputs. RenderBucket_LoadPrimColors loads IR0 from idpp->alphaScale (not the
	 * projection's depth-cue) and gte_dpct interpolates the three primitive colours toward the far
	 * colour in RFC/GFC/BFC -- the per-instance fade/tint mechanism, constant across the instance's
	 * primitives. Captured colours are pre-dpct; alphaScale == 0 means the writer skips dpct (and
	 * doubles as this struct's "no fade" state).
	 */
	s16 alphaScale;
	s32 fcR, fcG, fcB;
};

struct NativeModelEmitSnapshot
{
	struct NativeModelEmitVert verts[MODELEMIT_MAX_VERTS];
	struct NativeModelEmitFace faces[MODELEMIT_MAX_TRIS];
	struct NativeModelEmitInstance insts[MODELEMIT_MAX_INSTANCES];

	u32 triCount;
	u32 instCount;

	/* Textured subset of triCount. Per-frame rather than accumulated straight
	 * into the window, so a toggle flipped off mid-frame cannot leave the
	 * window's textured total counting triangles from a frame the window then
	 * refuses to fold in. */
	u32 texTris;

	u32 droppedTris;
	u32 droppedInstances;

	/* Same contract as NativeTrackFrameSnapshot: `degraded` means the capture
	 * was truncated and a consumer must draw nothing rather than a partial
	 * frame; `valid` means a capture actually ran since the last Clear. */
	int degraded;
	int valid;
	u32 frameSeq;
};

global_variable struct NativeModelEmitSnapshot s_modelEmitBuf[2];
global_variable int s_modelEmitWriteIdx;
global_variable int s_modelEmitReadIdx;
global_variable u32 s_modelEmitSeq;

/* Open instance, game thread only. -1 = none open. */
global_variable int s_modelEmitCurInst = -1;

/* The open instance holds its capture-table stores (NativeModelEmit_OwnGteStores):
 * every triangle it offers is either claimed or re-stored on decline. */
global_variable int s_modelEmitOwnsStores;
/* Triangles declined inside a store-holding instance; each was re-stored. */
global_variable u32 s_modelEmitOwnedDeclinesTotal;

internal void NativeModelEmit_DropStoreHold(void)
{
	if (s_modelEmitOwnsStores)
	{
		s_modelEmitOwnsStores = 0;
#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_ReleaseStoresForOwnedModel();
#endif
	}
}

/* Session-lifetime totals, and a one-shot warning the first time anything is
 * dropped -- a periodic line lands on 1 frame in 64 and would miss the other
 * 63, the same gap NativeTrackFrame_Record's drop accounting exists to close. */
global_variable u32 s_modelEmitDroppedTrisTotal;
global_variable u32 s_modelEmitDroppedInstTotal;
global_variable int s_modelEmitDropWarned;

/*
 * ---------------------------------------------------------------- [MDLDEPTH]
 *
 * Depth self-check. Answers one question and nothing else: is the view.z this
 * path hands the shader the SAME number retail's GTE computed for that exact
 * vertex?
 *
 * It can be exact rather than approximate because of where the capture sits.
 * NativeModelEmit_Triangle runs immediately after RenderBucket_ProjectPrim_
 * Normal, so SZ1/SZ2/SZ3 still hold the hardware's projected depth for
 * tempPacked[1]/[2]/[3] -- the very three vertices being captured. RTPT writes
 * all three; RTPS writes SZ3 and RenderBucket_LoadPrimRTPS explicitly re-seeds
 * SZ1/SZ2 for the reuse case (:2678), so the correspondence holds on both
 * paths. That makes this a comparison against ground truth, not against a
 * second derivation of the same guess.
 *
 * Why it exists: the player kart sinks into the road under dmodel, and the
 * model path is now the only thing drawing it (state 1), computing depth from
 * its own instance matrix with no lookup table anywhere near it. Either that
 * computation disagrees with the GTE -- in which case the fault is here and
 * this line says so with a number -- or it agrees, and the disagreement is on
 * the ROAD's side. s29b already ruled a height offset out by reading; this
 * rules the model side in or out by measuring.
 *
 * Comparison is done in integer, in the GTE's own shape: MAC3 = TRZ*4096 +
 * R2.v, SZ3 = clamp(MAC3 >> 12, 0, 65535) (native_gte_core.c's Lm_D). The
 * shader does the same arithmetic in float and skips the floor, so a residual
 * of <1 unit is the expected agreement, not a defect -- err>1 is the signal.
 */
/* Not `global_variable` (= static): --model-depth-check sets this from
 * native_platform.c, which precedes this file in the unity TU. */
int g_dbg_modelDepthCheck;

/* Writer lighting for the two specular primitive writers (KEY_TOKEN / LIT_TEXTURE) on the
 * native model path: the capture site runs the writer's own lcir + far-colour dpcs + glint
 * ramp and hands this file pre-lit colours plus a per-face blend override (additive lit
 * front faces). */
int NativeModelEmit_SpecularEnabled(void)
{
	return 1;
}

/*
 * One DPCS component, mirroring native_gte_core.c's case 0x2a EXACTLY
 * (gte_dpct is op 0x0F8002A: sf=1 so A() is >>12, lm=0 so Lm_B clamps to
 * full s16). Kept as integer arithmetic, not float: the writer's packet
 * colours come from the real GTE core, and the state-2 overlay is only an
 * A/B if the two paths round identically.
 *
 *   t   = Lm_B(((FC << 12) - (C << 16)) >> 12)      clamp -32768..32767
 *   mac = ((C << 16) + IR0 * t) >> 12
 *   out = Lm_C(mac >> 4)                            clamp 0..255
 */
internal u8 NativeModelEmit_FadeComponent(int col, s32 farCol, int ir0)
{
	s64 t64 = (((s64)farCol << 12) - ((s64)col << 16)) >> 12;
	s32 t = (t64 > 32767) ? 32767 : (t64 < -32768) ? -32768 : (s32)t64;
	s64 mac = (((s64)col << 16) + ((s64)ir0 * t)) >> 12;
	s32 out = (s32)(mac >> 4);

	if (out < 0)
	{
		out = 0;
	}
	if (out > 255)
	{
		out = 255;
	}
	return (u8)out;
}
global_variable u32 s_modelDepthSamples;

/*
 * ---------------------------------------------------------------- [AXISCHK]
 *
 * Cross-CLASS depth-axis check, and the one comparison no earlier instrument
 * ever made. [MDLDEPTH] proves our instance depth equals the GTE's SZ for the
 * instance's own projection; [TRKDEPTHCHK] proves the road's equals the GTE's
 * for the road's. Both pass at once even if the two GTE runs are in DIFFERENT
 * UNITS -- retail never compares them either (instances are OT-placed by the
 * queue-phase depth bin computed from the UNSHIFTED view position, never by
 * per-prim SZ), so a class-wide unit mismatch is invisible on PS1 by
 * construction and only surfaces under a real z-buffer (dmodel).
 *
 * Retail is explicit that the instance pipeline is regime-scaled:
 * RenderBucket_AdjustViewPositionForMvp shifts the MVP translation <<2 when
 * viewPos->vz < 0x1000 ("to match the full-scale model matrix"), and
 * RenderBucket_BuildM3x3's depthShift applies the matching >>2 to the model
 * scale in the far regime. So mvp.t[2] -- the axis every SZ this capture
 * records AND every packet stamp inherits -- is NOT self-evidently the axis
 * matrix_ViewProj puts the track on.
 *
 * This measures exactly that: the instance ORIGIN's view.z computed the
 * track's way (matrix_ViewProj rotation row 2 against the camera-relative
 * world position, integer, mirroring RenderBucket_GetViewPosition) against
 * mvp.t[2]. Ratio per distance band; a ratio of 1.0 everywhere kills the
 * theory, 4.0 (or a regime-split 4/1) names the sink's mechanism and its
 * un-scale is the fix.
 */
global_variable u32 s_axisChkN[4];        /* bands by trueZ: <1024, <4096, <16384, rest */
global_variable double s_axisChkSum[4];   /* sum of mvpZ/trueZ per band */
global_variable double s_axisChkMin[4];
global_variable double s_axisChkMax[4];
global_variable u32 s_axisChkHuge;        /* DRAW_HUGE instances seen (they carry their own >>2) */
global_variable u32 s_axisChkBehind;      /* trueZ < 128: too near/behind to ratio meaningfully */
global_variable s32 s_axisChkRawTrue[4];  /* first raw pairs of the window, for exact numbers */
global_variable s32 s_axisChkRawMvp[4];
global_variable u32 s_axisChkRawN;
global_variable u32 s_axisChkMaxResid;    /* worst |unscaled mvp - trueZ|, in true view.z units */

/* Classified regime for the instance BeginInstance is currently opening --
 * bridged into the snapshot slot at the field-store site because the slot is
 * only allocated after the capacity check. Game-thread only, like the rest of
 * the capture. */
global_variable s8 s_modelEmitPendingDepthShift;
global_variable u32 s_modelDepthOver1;
global_variable double s_modelDepthSumAbsErr;
global_variable s32 s_modelDepthMaxAbsErr;
global_variable s32 s_modelDepthPredLo;
global_variable s32 s_modelDepthPredHi;
global_variable s32 s_modelDepthActLo;
global_variable s32 s_modelDepthActHi;

/* Window accumulators for [MDLEMIT]. */
global_variable u32 s_modelEmitWinFrames;
global_variable u32 s_modelEmitWinTris;
global_variable u32 s_modelEmitWinInsts;
global_variable u32 s_modelEmitWinTextured;
global_variable u32 s_modelEmitWinPeakTris;
global_variable u32 s_modelEmitWinSemiTris; /* declined to the packet path -- see NativeModelEmit_Triangle */
global_variable u32 s_modelEmitWinSnapRescued; /* snap-culled triangles NativeModelEmit_FrontFacing passed */

/*
 * Is this triangle's texture semi-transparent on PS1?
 *
 * Retail's own test, not a heuristic. DrawLevelOvr1P_SelectRawPrimitiveCode
 * (game/226/226_00_DrawLevelOvr1P.c:2133, six call sites) picks the
 * semi-transparent primitive code when (uv1 & 0x00600000) != 0x00600000, and
 * uv1's high halfword IS the layout's tpage (:677) -- so the field being tested
 * is tpage bits 5-6, the PSX ABR (semi-transparency mode). ABR 3 is CTR's "this
 * page is opaque" sentinel; every other value marks the face semi-transparent
 * AND names the blend mode (0 = B/2+F/2, 1 = B+F, 2 = B-F).
 *
 * That rule is written for TRACK faces, and models are a different draw path --
 * but it is a property of the TEXTURE PAGE, not of the geometry that samples it,
 * and the tpage halfword reaching here comes out of the same TextureLayout
 * struct. The model writer's own semi-transparency comes from the primitive code
 * it selects rather than from this field, so treat this as a CONSERVATIVE
 * classifier: it is used only to decline triangles to the packet path, and the
 * cost of a false positive is one triangle drawn by retail instead of by us --
 * invisible in output, negligible in cost. A false NEGATIVE would be a solid
 * block where a plume should be, which is why the test errs toward declining.
 *
 * Untextured faces (tex == NULL) have no tpage and cannot be semi-transparent
 * by this rule; retail's untextured model primitives carry their own ABE bit,
 * which this capture never sees.
 * Closing that gap is what turns this decline into a real blended draw.
 */
internal int NativeModelEmit_TexIsSemiTrans(const struct TextureLayout *tex)
{
	if (tex == NULL)
	{
		return 0;
	}

	return (((CTR_ReadU16LE(&tex->tpage) >> 5) & 3) != 3);
}

int NativeModelEmit_BeginInstance(const struct InstDrawPerPlayer *idpp, const struct PushBuffer *pb, int forceSemiTrans, int specularLit,
                                  const struct Instance *worldInst, int ghostComposite, u32 fadeColorWord)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];
	struct NativeModelEmitInstance *inst;
	int r, c;

	NativeModelEmit_DropStoreHold();

	if ((g_dbg_modelRender == 0) || (idpp == NULL) || (pb == NULL))
	{
		return 0;
	}

	/*
	 * HUD models are UI, not world geometry. Drawing into the UI pushbuffer (pb == &gGT->
	 * pushBuffer_UI or &sdata->pushBuffer_DecalMP) is what makes a thing UI: its packets land in
	 * the UI OT, and everything downstream that tests the UI-band stamp (stereo flatten,
	 * widescreen X ramp) depends on that. Declining capture routes these instances to the retail
	 * packet path so those rules still apply to them. Not keyed on SCREENSPACE_INSTANCE: that flag
	 * is also set on menu instances born through Birth3D, which must NOT be dropped to the retail
	 * path.
	 */
	{
		const struct GameTracker *gGT = sdata->gGT;

		if ((gGT != NULL) && ((pb == &gGT->pushBuffer_UI) || (pb == &sdata->pushBuffer_DecalMP)))
		{
			return 0;
		}
	}

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	/* Paint-on-top members (the talking-mask models and the held shield
	 * bubble -- allowlists on g_dbg_paintOnTop, native_gte_capture.h) draw via the retail packet path:
	 * the walk-side range pin governs their occlusion there, whereas this
	 * path's depth is shader-computed per pixel with no per-prim stamp to pin.
	 * Declining also lets the depth bracket's prim-cursor latch see the
	 * packets it registers -- an intercepted draw writes none. */
	if (g_dbg_paintOnTop && (worldInst != NULL) && (GTECapture_InstPaintsOnTop(worldInst) != 0))
	{
		return 0;
	}
#endif

	/*
	 * Depth-axis regime classification (see the depthShift field comment) --
	 * runs for every captured instance, because the draw side needs the
	 * shift to put this instance's depth on the track's axis.
	 *
	 * Self-calibrating rather than re-deriving retail's near/huge branches:
	 * compute the instance origin's TRUE view.z on the track's axis, then
	 * pick whichever of {4x, 1x, 1/4x} mvp.t[2] actually is. That measures
	 * what retail's regime logic DID for this instance instead of predicting
	 * what it should have done, so a rounding disagreement at the 0x1000
	 * boundary cannot misclassify -- the candidates are a factor of 4 apart
	 * and the residual is a handful of units.
	 *
	 * trueZ: matrix_ViewProj row 2 against the camera-relative world
	 * position, >>12 -- the same integer arithmetic RenderBucket_
	 * GetViewPosition runs through the GTE light matrix (MVMVA adds no
	 * translation; retail subtracts the camera position explicitly first,
	 * so this does too). SCREENSPACE_INSTANCE bypasses that transform in
	 * retail (its matrix.t IS the view position), so it does here too --
	 * the main-menu character is one of these, and it sinks like the rest.
	 */
	{
		s8 shift = 0;

		if (worldInst != NULL)
		{
			s64 trueZ;
			s64 mvpZ = (s64)idpp->mvp.t[2];

			if ((worldInst->flags & SCREENSPACE_INSTANCE) != 0)
			{
				trueZ = (s64)worldInst->matrix.t[2];
			}
			else
			{
				/* s16-truncate the deltas like the GTE's vertex registers do: retail-faithful
				 * upper-bit garbage exists in instance matrices (e.g. warp-pad spin writes
				 * ~+2^20 into negative-phase components) that every retail consumer truncates
				 * away; a full-width read misclassifies that content. Lockstep with the twin
				 * fix in GTECapture_BeginInstanceDepth. */
				s64 dx = (s64)(s16)((s64)worldInst->matrix.t[0] - (s64)pb->pos.x);
				s64 dy = (s64)(s16)((s64)worldInst->matrix.t[1] - (s64)pb->pos.y);
				s64 dz = (s64)(s16)((s64)worldInst->matrix.t[2] - (s64)pb->pos.z);

				trueZ = ((s64)pb->matrix_ViewProj.m[2][0] * dx + (s64)pb->matrix_ViewProj.m[2][1] * dy + (s64)pb->matrix_ViewProj.m[2][2] * dz) >> 12;
			}

			if (trueZ > 0)
			{
				s64 eNear = mvpZ - (trueZ << 2); /* near regime: mvp = 4x true */
				s64 eFar = mvpZ - trueZ;         /* far regime: mvp = true */
				s64 eHuge = (mvpZ << 2) - trueZ; /* far DRAW_HUGE: mvp = true/4 */

				if (eNear < 0) { eNear = -eNear; }
				if (eFar < 0) { eFar = -eFar; }
				if (eHuge < 0) { eHuge = -eHuge; }

				shift = (eNear <= eFar) ? ((eNear <= eHuge) ? 2 : -2) : ((eFar <= eHuge) ? 0 : -2);
			}

			/* [AXISCHK] -- the measurement that found the regime scale. Kept
			 * as the fix's own verifier: resid is |unscaled mvp - trueZ| in
			 * true view.z units, so a nonzero window names a content class
			 * the classification above got wrong. */
			if (g_dbg_modelDepthCheck && (trueZ >= 128))
			{
				double ratio = (double)mvpZ / (double)trueZ;
				s64 unscaled = (shift == 2) ? (mvpZ >> 2) : (shift == -2) ? (mvpZ << 2) : mvpZ;
				s64 resid = unscaled - trueZ;
				int band = (trueZ < 1024) ? 0 : (trueZ < 4096) ? 1 : (trueZ < 16384) ? 2 : 3;

				if (resid < 0)
				{
					resid = -resid;
				}
				if ((u32)resid > s_axisChkMaxResid)
				{
					s_axisChkMaxResid = (u32)resid;
				}

				if (s_axisChkN[band] == 0)
				{
					s_axisChkMin[band] = ratio;
					s_axisChkMax[band] = ratio;
				}
				else
				{
					if (ratio < s_axisChkMin[band]) { s_axisChkMin[band] = ratio; }
					if (ratio > s_axisChkMax[band]) { s_axisChkMax[band] = ratio; }
				}
				s_axisChkN[band]++;
				s_axisChkSum[band] += ratio;

				if ((worldInst->flags & DRAW_HUGE) != 0)
				{
					s_axisChkHuge++;
				}

				if (s_axisChkRawN < 4)
				{
					s_axisChkRawTrue[s_axisChkRawN] = (s32)trueZ;
					s_axisChkRawMvp[s_axisChkRawN] = (s32)mvpZ;
					s_axisChkRawN++;
				}
			}
			else if (g_dbg_modelDepthCheck)
			{
				s_axisChkBehind++;
			}
		}

		s_modelEmitPendingDepthShift = shift;
	}

	if (dst->instCount >= MODELEMIT_MAX_INSTANCES)
	{
		dst->droppedInstances++;
		s_modelEmitDroppedInstTotal++;
		dst->degraded = 1;
		return 0;
	}

	inst = &dst->insts[dst->instCount];
	inst->firstTri = dst->triCount;
	inst->triCount = 0;
	inst->forceSemiTrans = (u8)(forceSemiTrans ? 1 : 0);
	inst->ghostComposite = (u8)(ghostComposite ? 1 : 0);
	inst->fadeR = (u8)(fadeColorWord & 0xff);
	inst->fadeG = (u8)((fadeColorWord >> 8) & 0xff);
	inst->fadeB = (u8)((fadeColorWord >> 16) & 0xff);
	inst->depthShift = s_modelEmitPendingDepthShift;
#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	inst->paintUnder = (u8)GTECapture_InstPaintsUnder(worldInst);
#else
	inst->paintUnder = 0;
#endif

	/* Fade inputs (see the field comment). BeginInstance runs after the
	 * instance setup callback in RenderBucket_DispatchDrawFunc's order, so
	 * the far-colour registers hold exactly what the writer's gte_dpct will
	 * read for this instance. CFC2 is the facade's own control-reg read --
	 * a read, not a GTE op; nothing here perturbs writer-visible state. */
	/* Specular writers never consume LoadPrimColors' dpct -- their colour is
	 * their own lcir/dpcs on the raw rolling value, applied by the capture
	 * site -- so the fade replay must not run on top of it. alphaScale 0 is
	 * the existing "no fade" state. */
	inst->alphaScale = specularLit ? 0 : idpp->alphaScale;
	inst->fcR = (s32)CFC2(21);
	inst->fcG = (s32)CFC2(22);
	inst->fcB = (s32)CFC2(23);

	for (r = 0; r < 3; r++)
	{
		for (c = 0; c < 3; c++)
		{
			inst->rot[r][c] = idpp->mvp.m[r][c];
		}
	}
	inst->trans[0] = idpp->mvp.t[0];
	inst->trans[1] = idpp->mvp.t[1];
	inst->trans[2] = idpp->mvp.t[2];

	/*
	 * Camera terms. h and the half-extents match NativeTrackFrame_Record's, but the viewport
	 * origin (rect.x/rect.y) is added here and is NOT optional: the GTE gets OFX/OFY = half the
	 * viewport (RenderBucket_PrepareDrawContext's gte_SetGeomOffset), relative to the viewport's
	 * own top-left, while this path projects straight to final screen coordinates. Without adding
	 * the origin, a 2P split-screen viewport not anchored at the screen origin renders relative to
	 * the screen corner instead of its own window (a 1P race viewport happens to sit at the
	 * origin, which is why the omission is invisible there). platform/native_track_frame.c has the
	 * same omission and the same latent 2P bug.
	 */
	inst->h = pb->distanceToScreen_PREV;
	inst->ofx = (pb->rect.w >> 1) + pb->rect.x;
	inst->ofy = (pb->rect.h >> 1) + pb->rect.y;

	s_modelEmitCurInst = (int)dst->instCount;
	return 1;
}

/*
 * Unpack one GTE vertex register pair into model-space shorts.
 *
 * The packed form is what RenderBucket_PackModelVertexXY built and what
 * MTC2(.., 0/1) was about to load: VX in the low halfword of xy, VY in the
 * high, VZ in the low halfword of z. Reading them back as s16 is not a
 * reinterpretation -- those halfwords ARE the GTE's 16-bit V registers
 * (include/psx/gtereg.h: C2_VX0 is CP2D.p[0].sw.l, C2_VY0 its .sw.h, C2_VZ0
 * CP2D.p[1].sw.l), so this recovers exactly the three signed values the
 * hardware would have multiplied by the rotation matrix.
 */
internal void NativeModelEmit_UnpackVertex(u32 packedXY, u32 packedZ, s16 *outX, s16 *outY, s16 *outZ)
{
	*outX = (s16)(u16)(packedXY & 0xffffu);
	*outY = (s16)(u16)(packedXY >> 16);
	*outZ = (s16)(u16)(packedZ & 0xffffu);
}

/*
 * Returns 1 when the native path has CLAIMED this triangle, i.e. the caller may
 * skip its packet write. Returns 0 to mean "not ours -- draw it the retail way",
 * which is the safe answer and the one every early-out below gives.
 *
 * The return value exists because "captured" and "will be drawn" can diverge:
 * a snapshot that overflows MODELEMIT_MAX_TRIS drops the triangle here, and
 * without the return the caller would skip the packet too, deleting the
 * triangle from the screen entirely instead of degrading to the retail draw.
 * Cap overflow is a performance event, not a correctness one. Semi-transparent
 * triangles are claimed like any other: the draw side does real two-pass PS1
 * blending (NativeModelDraw_IssueForWalk).
 */
/*
 * Retail's NCLIP facing test, redone unsnapped against the open instance's
 * captured transform: the sign of the screen-space cross product of the three
 * projected vertices, before the GTE truncates them to whole pixels. H and the
 * screen offset scale and shift all three points alike, so they drop out.
 * frontNegative is the sign retail requires of a front face. A vertex at or
 * behind the eye has no screen position, so the triangle stays culled.
 */
int NativeModelEmit_FrontFacing(const u32 *packedXY3, const u32 *packedZ3, int frontNegative)
{
	const struct NativeModelEmitInstance *mi;
	float sx[3], sy[3];
	float cross;
	int i;

	if (s_modelEmitCurInst < 0)
	{
		return 0;
	}
	mi = &s_modelEmitBuf[s_modelEmitWriteIdx].insts[s_modelEmitCurInst];

	for (i = 0; i < 3; i++)
	{
		s16 vx, vy, vz;
		float x, y, z;

		NativeModelEmit_UnpackVertex(packedXY3[i], packedZ3[i], &vx, &vy, &vz);
		x = (float)mi->trans[0] + ((float)mi->rot[0][0] * vx + (float)mi->rot[0][1] * vy + (float)mi->rot[0][2] * vz) * (1.0f / 4096.0f);
		y = (float)mi->trans[1] + ((float)mi->rot[1][0] * vx + (float)mi->rot[1][1] * vy + (float)mi->rot[1][2] * vz) * (1.0f / 4096.0f);
		z = (float)mi->trans[2] + ((float)mi->rot[2][0] * vx + (float)mi->rot[2][1] * vy + (float)mi->rot[2][2] * vz) * (1.0f / 4096.0f);
		if (z <= 0.0f)
		{
			return 0;
		}
		sx[i] = x / z;
		sy[i] = y / z;
	}

	cross = sx[0] * (sy[1] - sy[2]) + sx[1] * (sy[2] - sy[0]) + sx[2] * (sy[0] - sy[1]);
	if ((cross == 0.0f) || ((cross < 0.0f) != (frontNegative != 0)))
	{
		return 0;
	}
	s_modelEmitWinSnapRescued++;
	return 1;
}

int NativeModelEmit_Triangle(const u32 *packedXY3, const u32 *packedZ3, const int *color3, const struct TextureLayout *tex, int blendOverride)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];
	struct NativeModelEmitFace *face;
	struct NativeModelEmitVert *v;
	int i;

	if (s_modelEmitCurInst < 0)
	{
		return 0;
	}

	/* Counted, not declined. An earlier cut of this handed semi-transparent
	 * triangles back to the packet path because the model draw could only draw
	 * opaque; the draw side now does real two-pass PS1 blending
	 * (NativeModelDraw_IssueForWalk), so the native path owns this content like
	 * any other. The counter stays because it sizes the blend pass. */
	if (NativeModelEmit_TexIsSemiTrans(tex))
	{
		s_modelEmitWinSemiTris++;
	}

	if (dst->triCount >= MODELEMIT_MAX_TRIS)
	{
		dst->droppedTris++;
		s_modelEmitDroppedTrisTotal++;
		dst->degraded = 1;

		if (!s_modelEmitDropWarned)
		{
			s_modelEmitDropWarned = 1;
			Platform_Log("[MDLEMIT] WARNING: first triangle-cap drop this session (cap=%d) -- snapshot marked degraded\n", MODELEMIT_MAX_TRIS);
		}

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
		/* The caller writes this triangle's packet, which resolves depth from the
		 * capture table, but the instance projected it under the store hold.
		 * Lift the hold for the rest of the instance (the split writer projects
		 * clip vertices of its own) and store this triangle's corners now. Every
		 * later triangle of the instance declines too (the cap only fills), and
		 * each re-stores: strip and fan corners were projected under the hold. */
		if (s_modelEmitOwnsStores)
		{
			GTECapture_ReleaseStoresForOwnedModel();
			GTE_CaptureStoreTriangle(packedXY3, packedZ3);
			s_modelEmitOwnedDeclinesTotal++;
		}
#endif
		return 0;
	}

	face = &dst->faces[dst->triCount];
	v = &dst->verts[dst->triCount * 3];

	/*
	 * UV corner i goes to vertex i, no strip-parity swap. That is not an
	 * assumption: RenderBucket_DrawInstPrim_NormalAtOTEntry writes
	 * TextureLayout words 0/1/2 into a POLY_GT3's u0/u1/u2 in that fixed order
	 * for every primitive, and the three positions it writes alongside them
	 * come from the same tempPacked[1..3] window this capture is handed. Model
	 * faces carry their UVs per FACE, not per vertex, so the layout entry is
	 * the whole story.
	 *
	 * Read by field name into a local rather than by stride arithmetic off
	 * &tex->u0. The three pairs do sit 4 bytes apart today, but that spacing is
	 * a consequence of the interleaved clut/tpage halfwords, and retail's own
	 * code computes it with offsetof (RENDER_BUCKET_TEX_WORD1_OFFSET) instead
	 * of writing 4 -- so neither does this.
	 */
	{
		u8 uv[3][2];

		if (tex != NULL)
		{
			uv[0][0] = tex->u0;
			uv[0][1] = tex->v0;
			uv[1][0] = tex->u1;
			uv[1][1] = tex->v1;
			uv[2][0] = tex->u2;
			uv[2][1] = tex->v2;

			face->tpage = tex->tpage;
			face->clut = tex->clut;
			face->textured = 1;
		}
		else
		{
			memset(uv, 0, sizeof(uv));

			face->tpage = 0;
			face->clut = 0;
			face->textured = 0;
		}
		face->blendOverride = (blendOverride >= 0) ? (u8)blendOverride : 0xFF;
		face->_pad[0] = 0;
		face->_pad[1] = 0;

		const struct NativeModelEmitInstance *mi = &dst->insts[s_modelEmitCurInst];

		for (i = 0; i < 3; i++)
		{
			NativeModelEmit_UnpackVertex(packedXY3[i], packedZ3[i], &v[i].x, &v[i].y, &v[i].z);

			v[i].u = (s16)uv[i][0];
			v[i].v = (s16)uv[i][1];

			/*
			 * Colour is the rolling per-vertex value, which is what the packet
			 * writer feeds gte_dpct -- so this is the colour BEFORE the depth
			 * cue, even though the capture site runs after the writer (dpct
			 * leaves its result in GTE registers and never touches
			 * ctx->tempColor). Specular instances are the exception: the
			 * capture site hands this function WRITER-LIT colours, and their
			 * alphaScale is forced 0 at BeginInstance so the fade below stays
			 * off (their writers never run the dpct being replayed).
			 *
			 * The dpct is replayed here in C: IR0 is idpp->alphaScale
			 * and FC is whatever the instance's setup callback loaded, both
			 * captured at BeginInstance. The stored colour is therefore
			 * POST-fade, matching the packet the writer is about to build, and
			 * the draw side needs no new state. alphaScale == 0 mirrors the
			 * writer's own skip gate.
			 */
			v[i].r = (u8)(u32)(color3[i] & 0xff);
			v[i].g = (u8)(((u32)color3[i] >> 8) & 0xff);
			v[i].b = (u8)(((u32)color3[i] >> 16) & 0xff);
			v[i]._pad = 0;

			/* The GHOST writer's own per-prim gte_dpct is THIS transform --
			 * same op, same IR0 (alphaScale), same far colour -- so ghost
			 * instances take the replay. DEPTH_FADE stays excluded: its fade
			 * is per-vertex SZ through RenderBucket_DepthFadeColor, a
			 * different transform, and replaying this one on its content (Hot
			 * Air Skyway's barriers) applies a colour change retail never
			 * makes. Its captured colours stay pre-fade -- the remaining
			 * known gap recorded in the field comment above. */
			if ((mi->alphaScale != 0) && (!mi->forceSemiTrans || mi->ghostComposite))
			{
				v[i].r = NativeModelEmit_FadeComponent(v[i].r, mi->fcR, mi->alphaScale);
				v[i].g = NativeModelEmit_FadeComponent(v[i].g, mi->fcG, mi->alphaScale);
				v[i].b = NativeModelEmit_FadeComponent(v[i].b, mi->fcB, mi->alphaScale);
			}
		}
	}

	/* [MDLDEPTH] -- see the accumulator block's comment. Reads the GTE regs
	 * the projection just wrote; must stay inside this function, because it is
	 * the only place where "the vertex we captured" and "the SZ the hardware
	 * produced for it" are both in hand. */
	if (g_dbg_modelDepthCheck)
	{
		const struct NativeModelEmitInstance *mi = &dst->insts[s_modelEmitCurInst];

		for (i = 0; i < 3; i++)
		{
			/* MAC3 = TRZ*4096 + R20*vx + R21*vy + R22*vz, then >>12 and the
			 * unsigned 16-bit clamp -- native_gte_core.c's RTPS, in integer. */
			s64 mac3 = ((s64)mi->trans[2] * 4096) + ((s64)mi->rot[2][0] * v[i].x) + ((s64)mi->rot[2][1] * v[i].y) + ((s64)mi->rot[2][2] * v[i].z);
			s32 pred = (s32)(mac3 >> 12);
			s32 act = (s32)(u32)MFC2(17 + i); /* SZ1, SZ2, SZ3 */
			s32 err;

			if (pred < 0) { pred = 0; }
			if (pred > 65535) { pred = 65535; }

			err = pred - act;
			if (err < 0) { err = -err; }

			if (s_modelDepthSamples == 0)
			{
				s_modelDepthPredLo = pred;
				s_modelDepthPredHi = pred;
				s_modelDepthActLo = act;
				s_modelDepthActHi = act;
			}
			else
			{
				if (pred < s_modelDepthPredLo) { s_modelDepthPredLo = pred; }
				if (pred > s_modelDepthPredHi) { s_modelDepthPredHi = pred; }
				if (act < s_modelDepthActLo) { s_modelDepthActLo = act; }
				if (act > s_modelDepthActHi) { s_modelDepthActHi = act; }
			}

			s_modelDepthSamples++;
			s_modelDepthSumAbsErr += (double)err;
			if (err > s_modelDepthMaxAbsErr) { s_modelDepthMaxAbsErr = err; }
			if (err > 1) { s_modelDepthOver1++; }
		}
	}

	dst->triCount++;
	dst->texTris += (u32)face->textured;
	dst->insts[s_modelEmitCurInst].triCount++;
	return 1;
}

void NativeModelEmit_OwnGteStores(void)
{
#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	if ((s_modelEmitCurInst >= 0) && NativeModelEmit_SkipRetailDraw())
	{
		s_modelEmitOwnsStores = 1;
		GTECapture_HoldStoresForOwnedModel();
	}
#endif
}

void NativeModelEmit_EndInstance(void)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];

	NativeModelEmit_DropStoreHold();

	if (s_modelEmitCurInst < 0)
	{
		return;
	}

	/*
	 * An instance that captured nothing is dropped rather than kept as an
	 * empty range. Every one costs a uniform upload and a bind at rung 2, and
	 * retail's own walk routinely rejects every triangle of an instance that
	 * is on screen but facing away -- keeping those would put empty draws in
	 * the frame that no counter would explain.
	 */
	if (dst->insts[s_modelEmitCurInst].triCount == 0)
	{
		s_modelEmitCurInst = -1;
		return;
	}

	dst->instCount++;
	s_modelEmitCurInst = -1;
}

/*
 * ---------------------------------------------------------------- model frame cache
 *
 * A model's command list is frame-invariant and its vertex positions depend
 * only on the frame pair it draws (RenderBucket_PackModelVertexXY reads
 * nothing but the frames), so every instance drawing the same (command list,
 * frame, next frame, colour table, texture table) captures the same
 * triangles. The first draw of a key runs the retail walk and records every
 * triangle with a usable texture, culled or not; later draws copy the record
 * into the snapshot and skip the walk: no vertex decode, no GTE projection,
 * no NCLIP.
 *
 * Back faces are culled here, in model space, by retail's own rule. With a
 * view-space vertex v = R p / 4096 + t, the GTE's screen cross product (NCLIP)
 * has the sign of det(R) * n . (p0 - eye), where n = (p1 - p0) x (p2 - p0)
 * and eye = -4096 R^-1 t. Retail draws a face when (NCLIP < 0) equals the
 * instance REVERSE_CULL bit XOR command bit 29, skips the test when command
 * bit 28 is clear, and rejects NCLIP == 0. The test is float, so thin faces
 * retail's whole-pixel NCLIP drops are drawn, the population
 * RenderBucket_CaptureSnapCulledTriangle already rescues on the walk.
 *
 * Off-screen and near faces are not rejected: the GPU clips them, and models
 * have no near plane.
 *
 * A record holds each face's texture-table index, never the layout it named:
 * the table entry and the layout are both read again on every draw, since
 * animated textures (CTR_CycleTex_Model, e.g. a blink) rewrite table entries
 * while the table pointer in the key stays the same. The colour table's contents are hashed into the key on every draw,
 * since a setup callback can rewrite the table between instances; each colour
 * variant is its own entry. Every entry is dropped when the level changes:
 * frame and command pointers are MEMPACK addresses that a later load reuses.
 *
 * A race's live keys outgrow 1.5 MiB, and a refill costs a full retail walk
 * of the instance, so the arena is 3 MiB and eviction gives a second chance:
 * an oldest entry drawn since it was last placed slides to the ring head
 * (a memmove of its record) instead of being dropped. dumpreq `modelcache
 * [N]`: 1 (default) 3 MiB with second chance, 0 1.5 MiB oldest-first.
 *
 * Pose builds. Racers draw halfway between an animation frame and the next
 * one, so every (frame, next) pair is its own key, and a spin-out or hit
 * plays a run of pairs no fill has seen. The first fill of a model records
 * its topology beside the record: per triangle, the vertex each corner was
 * decoded as (1-based decode order), plus its colours, texture and flags,
 * which depend on the command list, texture layout and colour table but
 * never on the frame. A later miss on a key whose topology is known decodes
 * the pose's vertices with retail's decode and pack arithmetic
 * (RenderBucket_NativeDecodePose) and gathers them through the topology into
 * a record, without walking the instance. A topology is kept only when every
 * corner names a vertex this instance decoded. dumpreq `modelpose [N]`: 1
 * (default) builds, 0 walks every new key; `posecheck [N]` (internal) walks
 * anyway and compares the built record with the walked one.
 */
#define MODELCACHE_ARENA_BYTES      (3072u * 1024u)
#define MODELCACHE_ARENA_BYTES_FIFO (1536u * 1024u)
/* Record bytes one Alloc may slide forward before it drops drawn entries
 * too: a memmove per recycle, bounded well under the cost of one refill. */
#define MODELCACHE_RECYCLE_BYTES_PER_ALLOC (32u * 1024u)
#define MODELCACHE_MAX_ENTRIES 1024
#define MODELCACHE_BUCKETS     512
/* Largest instance the fill records; larger ones keep the walk. */
#define MODELCACHE_FILL_MAX_TRIS 2048

#define MODELCACHE_TRI_DOUBLE_SIDED 0x01
#define MODELCACHE_TRI_FLIP         0x02 /* command bit 29 */

struct NativeModelCacheTri
{
	s16 p[3][3];
	u8 rgb[3][3];
	u8 flags;
	u16 texIndex; /* command's texture-table index, 0 = untextured */
	float n[3];
};

struct NativeModelCacheKey
{
	u32 cmdList;
	const void *frame;
	const void *next;
	u32 colorLayout;
	const void *texLayout;
	u32 deltaArray;
	u32 colorHash;
};

struct NativeModelCacheEntry
{
	struct NativeModelCacheKey key;
	u32 offset;
	u32 bytes;
	u16 triCount;
	s16 nextInBucket;
	u8 live;
	u8 ref; /* drawn since it was placed at the head */
};

global_variable u8 *s_mcArena;
global_variable int s_mcArenaFailed;
global_variable u32 s_mcArenaHead;
global_variable struct NativeModelCacheEntry s_mcEntries[MODELCACHE_MAX_ENTRIES];
global_variable s16 s_mcBuckets[MODELCACHE_BUCKETS];
global_variable u32 s_mcHeadIdx;  /* next metadata slot, allocation order */
global_variable u32 s_mcTailIdx;  /* oldest live metadata slot */
global_variable u32 s_mcLiveCount;
global_variable int s_mcLevel = -1;
global_variable const void *s_mcLevelPtr;
global_variable u32 s_mcArenaBytes = MODELCACHE_ARENA_BYTES;
global_variable int s_mcMode = -1;
global_variable u32 s_mcWinRecycles;
int g_dbg_modelCache = 1;

#define MODELPOSE_TOPO_BYTES (256u * 1024u)
#define MODELPOSE_MAX_TOPOS  128
#define MODELPOSE_MAX_VERTS  1024

/* texIndex: the command's texture-table index (0 = untextured). */
struct NativeModelPoseTri
{
	u16 v[3]; /* 1-based decode order */
	u16 texIndex;
	u8 rgb[3][3];
	u8 flags;
};

struct NativeModelPoseTopo
{
	u32 cmdList;
	const void *texLayout;
	u32 colorLayout;
	u32 colorHash;
	u32 offset;
	u16 triCount;
	u16 vertCount;
};

global_variable u8 *s_mpArena;
global_variable u32 s_mpArenaHead;
global_variable struct NativeModelPoseTopo s_mpTopos[MODELPOSE_MAX_TOPOS];
global_variable u32 s_mpTopoCount;
global_variable const struct NativeModelPoseTopo *s_mpCur;
global_variable struct NativeModelCacheKey s_mpCurKey;
global_variable u32 s_mpWinBuilds, s_mpWinTopos, s_mpWinTopoRefused;
/* The last TryEmit found no record for its key: the one case a pose build
 * can serve (a record that exists but cannot replay keeps the walk). */
global_variable int s_mcLastWasMiss;
int g_dbg_modelPose = 1;
#ifdef CTR_INTERNAL
int g_dbg_modelPoseCheck = 0;
global_variable u32 s_mpCheckCount;
global_variable int s_mpCheckPending;
global_variable u32 s_mpWinCheckMatch, s_mpWinCheckMismatch;
#endif

global_variable struct NativeModelCacheTri s_mcFillTris[MODELCACHE_FILL_MAX_TRIS];
global_variable u32 s_mcFillCount;
global_variable int s_mcFillActive;
global_variable struct NativeModelCacheKey s_mcFillKey;
/* The open fill's eye and cull terms, for the [MDLCACHE] agreement check
 * against retail's NCLIP on the same triangles. */
global_variable float s_mcFillEye[3];
global_variable float s_mcFillDet;
global_variable int s_mcFillInstNeg;
/* The open fill's topology: each triangle's corner vertices in decode order.
 * Cleared when a corner names no vertex this instance decoded. */
global_variable u16 s_mcFillVid[MODELCACHE_FILL_MAX_TRIS][3];
global_variable int s_mcFillVidOk;
global_variable u32 s_mcFillMaxVid;
#ifdef CTR_INTERNAL
global_variable struct NativeModelCacheTri s_mpCheckTris[MODELCACHE_FILL_MAX_TRIS];
#endif

global_variable u32 s_mcWinHits, s_mcWinMisses, s_mcWinFills, s_mcWinEvicts, s_mcWinNoRoom;
global_variable u32 s_mcWinCullAgree, s_mcWinCullWeDrawRetailCulled, s_mcWinCullWeCullRetailDrew;
global_variable u32 s_mcWinTrisIn, s_mcWinTrisOut;
/* Normal-func instances the cache does not take, by reason: not captured
 * (HUD, paint-on-top), specular writer, ghost or depth-fade writer, split or
 * reflect decode. */
global_variable u32 s_mcWinSkipNoCapture, s_mcWinSkipSpecular, s_mcWinSkipBlend, s_mcWinSkipSplit;

/* Draw funcs dispatched this window: normal, normalAlt, split, special, other. */
global_variable u32 s_mcWinFunc[5];

#ifdef CTR_INTERNAL
/* [MDLNEW]: each fill of a key the cache has never held, by instance name and
 * by which key fields differ from the nearest live key with the same command
 * list. Mask bits: 1 frame, 2 next, 4 colorLayout, 8 texLayout, 16 deltaArray,
 * 32 colorHash, 64 no live key shares the command list. */
#define MCNEW_SLOTS 24
struct NativeModelCacheNewTally
{
	char name[16];
	u32 mask;
	u32 count;
};
global_variable char s_mcFillName[16];
global_variable struct NativeModelCacheNewTally s_mcNewTally[MCNEW_SLOTS];
global_variable u32 s_mcNewTallyUsed, s_mcNewOverflow, s_mcNewFieldCount[7];

internal void NativeModelCache_NoteNewKey(const struct NativeModelCacheKey *k)
{
	u32 best = 64u;
	int bestBits = 99;
	u32 i;

	for (i = 0; i < MODELCACHE_MAX_ENTRIES; i++)
	{
		const struct NativeModelCacheEntry *e = &s_mcEntries[i];
		u32 m = 0;
		int bits = 0;
		u32 j;

		if (!e->live || (e->key.cmdList != k->cmdList))
		{
			continue;
		}
		m |= (e->key.frame != k->frame) ? 1u : 0u;
		m |= (e->key.next != k->next) ? 2u : 0u;
		m |= (e->key.colorLayout != k->colorLayout) ? 4u : 0u;
		m |= (e->key.texLayout != k->texLayout) ? 8u : 0u;
		m |= (e->key.deltaArray != k->deltaArray) ? 16u : 0u;
		m |= (e->key.colorHash != k->colorHash) ? 32u : 0u;
		for (j = 0; j < 6; j++)
		{
			bits += (int)((m >> j) & 1u);
		}
		if (bits < bestBits)
		{
			bestBits = bits;
			best = m;
		}
	}
	for (i = 0; i < 7; i++)
	{
		if (best & (1u << i))
		{
			s_mcNewFieldCount[i]++;
		}
	}
	for (i = 0; i < s_mcNewTallyUsed; i++)
	{
		if ((s_mcNewTally[i].mask == best) && (memcmp(s_mcNewTally[i].name, s_mcFillName, 16) == 0))
		{
			s_mcNewTally[i].count++;
			return;
		}
	}
	if (s_mcNewTallyUsed < MCNEW_SLOTS)
	{
		memcpy(s_mcNewTally[s_mcNewTallyUsed].name, s_mcFillName, 16);
		s_mcNewTally[s_mcNewTallyUsed].name[15] = 0;
		s_mcNewTally[s_mcNewTallyUsed].mask = best;
		s_mcNewTally[s_mcNewTallyUsed].count = 1;
		s_mcNewTallyUsed++;
	}
	else
	{
		s_mcNewOverflow++;
	}
}

internal void NativeModelCache_LogNewKeys(void)
{
	u32 i;

	Platform_LogInstrument("[MDLNEW] lvl=%d fields frame=%lu next=%lu colorLayout=%lu texLayout=%lu delta=%lu colorHash=%lu noCmdList=%lu overflow=%lu\n",
	                       NativePerf_CurrentLevelID(), (unsigned long)s_mcNewFieldCount[0], (unsigned long)s_mcNewFieldCount[1],
	                       (unsigned long)s_mcNewFieldCount[2], (unsigned long)s_mcNewFieldCount[3], (unsigned long)s_mcNewFieldCount[4],
	                       (unsigned long)s_mcNewFieldCount[5], (unsigned long)s_mcNewFieldCount[6], (unsigned long)s_mcNewOverflow);
	for (i = 0; i < s_mcNewTallyUsed; i++)
	{
		Platform_LogInstrument("[MDLNEW]   %-15s mask=%02lx n=%lu\n", s_mcNewTally[i].name, (unsigned long)s_mcNewTally[i].mask,
		                       (unsigned long)s_mcNewTally[i].count);
	}
	s_mcNewTallyUsed = 0;
	s_mcNewOverflow = 0;
	memset(s_mcNewFieldCount, 0, sizeof(s_mcNewFieldCount));
}
#endif

void NativeModelCache_NoteFunc(int kind)
{
	if ((kind >= 0) && (kind < 5))
	{
		s_mcWinFunc[kind]++;
	}
}

void NativeModelCache_NoteSkipped(int captured, int specular, int forceBlend)
{
	if (!captured)
	{
		s_mcWinSkipNoCapture++;
	}
	else if (specular)
	{
		s_mcWinSkipSpecular++;
	}
	else if (forceBlend)
	{
		s_mcWinSkipBlend++;
	}
	else
	{
		s_mcWinSkipSplit++;
	}
}

internal u32 NativeModelCache_HashKey(const struct NativeModelCacheKey *k)
{
	u32 h = k->cmdList * 2654435761u;

	h ^= (u32)(uintptr_t)k->frame * 40503u;
	h ^= (u32)(uintptr_t)k->next * 2246822519u;
	h ^= k->colorLayout * 3266489917u;
	h ^= (u32)(uintptr_t)k->texLayout;
	h ^= k->colorHash * 2654435761u;
	h ^= h >> 15;
	return h & (MODELCACHE_BUCKETS - 1);
}

internal int NativeModelCache_SameKey(const struct NativeModelCacheKey *a, const struct NativeModelCacheKey *b)
{
	return (a->cmdList == b->cmdList) && (a->frame == b->frame) && (a->next == b->next) && (a->colorLayout == b->colorLayout) &&
	       (a->texLayout == b->texLayout) && (a->deltaArray == b->deltaArray) && (a->colorHash == b->colorHash);
}

internal u32 NativeModelCache_ColorHash(const struct InstDrawPerPlayer *idpp);

internal void NativeModelCache_KeyFromIdpp(const struct InstDrawPerPlayer *idpp, struct NativeModelCacheKey *k)
{
	k->cmdList = idpp->ptrCommandList;
	k->frame = idpp->ptrCurrFrame;
	k->next = idpp->ptrNextFrame;
	k->colorLayout = idpp->ptrColorLayout;
	k->texLayout = idpp->ptrTexLayout;
	k->deltaArray = (u32)idpp->ptrDeltaArray;
	k->colorHash = NativeModelCache_ColorHash(idpp);
}

/* FNV-1a over the colour table the walk reads (count in command word 0). */
internal u32 NativeModelCache_ColorHash(const struct InstDrawPerPlayer *idpp)
{
	const u32 *cmd = (const u32 *)(uintptr_t)idpp->ptrCommandList;
	const u8 *col = (const u8 *)(uintptr_t)idpp->ptrColorLayout;
	u32 count;
	u32 h = 2166136261u;
	u32 i;

	if ((cmd == NULL) || (col == NULL))
	{
		return 0;
	}
	count = cmd[0];
	if (count > 256)
	{
		count = 256;
	}
	for (i = 0; i < count * 4; i++)
	{
		h = (h ^ col[i]) * 16777619u;
	}
	return h;
}

internal void NativeModelCache_Unlink(u32 idx)
{
	struct NativeModelCacheEntry *e = &s_mcEntries[idx];
	const u32 b = NativeModelCache_HashKey(&e->key);
	s16 *link = &s_mcBuckets[b];

	while (*link >= 0)
	{
		if ((u32)*link == idx)
		{
			*link = e->nextInBucket;
			break;
		}
		link = &s_mcEntries[*link].nextInBucket;
	}
	e->live = 0;
}

internal void NativeModelCache_EvictOldest(void)
{
	if (s_mcLiveCount == 0)
	{
		return;
	}
	if (s_mcEntries[s_mcTailIdx].live)
	{
		NativeModelCache_Unlink(s_mcTailIdx);
		s_mcWinEvicts++;
	}
	s_mcTailIdx = (s_mcTailIdx + 1) % MODELCACHE_MAX_ENTRIES;
	s_mcLiveCount--;
	if (s_mcLiveCount == 0)
	{
		s_mcArenaHead = 0;
	}
}

void NativeModelCache_Reset(void)
{
	u32 i;

	for (i = 0; i < MODELCACHE_BUCKETS; i++)
	{
		s_mcBuckets[i] = -1;
	}
	for (i = 0; i < MODELCACHE_MAX_ENTRIES; i++)
	{
		s_mcEntries[i].live = 0;
	}
	s_mcHeadIdx = 0;
	s_mcTailIdx = 0;
	s_mcLiveCount = 0;
	s_mcArenaHead = 0;
	s_mcFillActive = 0;
	s_mpArenaHead = 0;
	s_mpTopoCount = 0;
	s_mpCur = NULL;
#ifdef CTR_INTERNAL
	s_mpCheckPending = 0;
#endif
}

/* Drops every entry when the level changes; see the section comment. */
internal void NativeModelCache_CheckLevel(void)
{
	const struct GameTracker *gGT = sdata->gGT;
	const int lvl = (gGT != NULL) ? gGT->levelID : -1;
	const void *lp = (gGT != NULL) ? (const void *)gGT->level1 : NULL;

	if ((lvl != s_mcLevel) || (lp != s_mcLevelPtr) || (g_dbg_modelCache != s_mcMode))
	{
		s_mcLevel = lvl;
		s_mcLevelPtr = lp;
		s_mcMode = g_dbg_modelCache;
		s_mcArenaBytes = s_mcMode ? MODELCACHE_ARENA_BYTES : MODELCACHE_ARENA_BYTES_FIFO;
		NativeModelCache_Reset();
	}
}

/* Moves the oldest entry to the newest position. Its record slides to the
 * ring head: with the oldest entry's bytes counted free, the head always has
 * room for them, at the head itself or, when the ring has not wrapped and the
 * arena's end is too short, at offset 0. Both destinations lie in space that
 * is free once the record leaves, so an overlapping memmove is safe. */
internal void NativeModelCache_RecycleOldest(void)
{
	const u32 tail = s_mcTailIdx;
	struct NativeModelCacheEntry moved = s_mcEntries[tail];
	const u32 tailOff = moved.offset;
	u32 dst = s_mcArenaHead;
	u32 idx;
	u32 b;

	if ((s_mcArenaHead > tailOff) && ((s_mcArenaBytes - s_mcArenaHead) < moved.bytes))
	{
		dst = 0;
	}
	if (dst != tailOff)
	{
		memmove(s_mcArena + dst, s_mcArena + tailOff, moved.bytes);
	}

	s_mcTailIdx = (tail + 1) % MODELCACHE_MAX_ENTRIES;
	idx = s_mcHeadIdx;
	s_mcHeadIdx = (idx + 1) % MODELCACHE_MAX_ENTRIES;
	s_mcArenaHead = dst + moved.bytes;
	s_mcWinRecycles++;

	if (idx == tail)
	{
		s_mcEntries[tail].offset = dst;
		s_mcEntries[tail].ref = 0;
		return;
	}
	NativeModelCache_Unlink(tail);
	moved.offset = dst;
	moved.ref = 0;
	moved.live = 1;
	s_mcEntries[idx] = moved;
	b = NativeModelCache_HashKey(&moved.key);
	s_mcEntries[idx].nextInBucket = s_mcBuckets[b];
	s_mcBuckets[b] = (s16)idx;
}

/* Contiguous arena bytes for a new entry. The arena is a ring: live data runs
 * from the oldest entry's offset to s_mcArenaHead, wrapping once. The oldest
 * entry is dropped, or in second-chance mode recycled when it was drawn since
 * its placement; recycling stops at MODELCACHE_RECYCLE_BYTES_PER_ALLOC, so a
 * ring full of drawn entries still frees space. Returns the offset, or -1 when
 * the request can never fit. */
internal s32 NativeModelCache_Alloc(u32 bytes)
{
	u32 recycleBytes = 0;

	if ((s_mcArena == NULL) || (bytes > s_mcArenaBytes))
	{
		return -1;
	}

	for (;;)
	{
		u32 tailOff;

		if (s_mcLiveCount >= MODELCACHE_MAX_ENTRIES)
		{
			NativeModelCache_EvictOldest();
			continue;
		}
		if (s_mcLiveCount == 0)
		{
			s_mcArenaHead = 0;
			return 0;
		}

		tailOff = s_mcEntries[s_mcTailIdx].offset;
		if (s_mcArenaHead > tailOff)
		{
			if ((s_mcArenaBytes - s_mcArenaHead) >= bytes)
			{
				return (s32)s_mcArenaHead;
			}
			if (tailOff >= bytes)
			{
				return 0;
			}
		}
		else if ((s_mcArenaHead < tailOff) && ((tailOff - s_mcArenaHead) >= bytes))
		{
			return (s32)s_mcArenaHead;
		}
		if (s_mcMode && (s_mcLiveCount >= 2) && s_mcEntries[s_mcTailIdx].live && s_mcEntries[s_mcTailIdx].ref &&
		    ((recycleBytes + s_mcEntries[s_mcTailIdx].bytes) <= MODELCACHE_RECYCLE_BYTES_PER_ALLOC))
		{
			recycleBytes += s_mcEntries[s_mcTailIdx].bytes;
			NativeModelCache_RecycleOldest();
			continue;
		}
		NativeModelCache_EvictOldest();
	}
}

internal struct NativeModelCacheEntry *NativeModelCache_Find(const struct NativeModelCacheKey *k)
{
	s16 i = s_mcBuckets[NativeModelCache_HashKey(k)];

	while (i >= 0)
	{
		struct NativeModelCacheEntry *e = &s_mcEntries[i];

		if (e->live && NativeModelCache_SameKey(&e->key, k))
		{
			return e;
		}
		i = e->nextInBucket;
	}
	return NULL;
}

/* The instance's eye in model space and det(R), from the open instance's
 * captured mvp. Returns 0 for a singular matrix. */
internal int NativeModelCache_InstanceEye(const struct NativeModelEmitInstance *mi, float eye[3], float *outDet)
{
	const float s = 1.0f / 4096.0f;
	const float a = mi->rot[0][0] * s, b = mi->rot[0][1] * s, c = mi->rot[0][2] * s;
	const float d = mi->rot[1][0] * s, e = mi->rot[1][1] * s, f = mi->rot[1][2] * s;
	const float g = mi->rot[2][0] * s, h = mi->rot[2][1] * s, i = mi->rot[2][2] * s;
	const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	const float det = a * A + b * B + c * C;
	const float tx = (float)mi->trans[0], ty = (float)mi->trans[1], tz = (float)mi->trans[2];
	float invDet;

	if ((det > -1e-9f) && (det < 1e-9f))
	{
		return 0;
	}
	invDet = 1.0f / det;
	/* eye = -R^-1 t, R^-1 = adj(R) / det (adj = transposed cofactors). */
	eye[0] = -(A * tx + (-(b * i - c * h)) * ty + (b * f - c * e) * tz) * invDet;
	eye[1] = -(B * tx + (a * i - c * g) * ty + (-(a * f - c * d)) * tz) * invDet;
	eye[2] = -(C * tx + (-(a * h - b * g)) * ty + (a * e - b * d) * tz) * invDet;
	*outDet = det;
	return 1;
}

/* Signed stand-in for retail's NCLIP on this face for the given eye, or 0
 * when the face is edge-on. */
internal float NativeModelCache_NclipSign(const struct NativeModelCacheTri *t, const float eye[3], float det)
{
	const float qx = (float)t->p[0][0] - eye[0];
	const float qy = (float)t->p[0][1] - eye[1];
	const float qz = (float)t->p[0][2] - eye[2];
	const float dot = t->n[0] * qx + t->n[1] * qy + t->n[2] * qz;

	return (det > 0.0f) ? dot : -dot;
}

internal int NativeModelCache_FaceDrawn(const struct NativeModelCacheTri *t, const float eye[3], float det, int instNeg)
{
	float v;

	if (t->flags & MODELCACHE_TRI_DOUBLE_SIDED)
	{
		return 1;
	}
	v = NativeModelCache_NclipSign(t, eye, det);
	if (v == 0.0f)
	{
		return 0;
	}
	return (v < 0.0f) == ((instNeg ^ ((t->flags & MODELCACHE_TRI_FLIP) ? 1 : 0)) != 0);
}

int NativeModelCache_TryEmit(const struct InstDrawPerPlayer *idpp)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];
	struct NativeModelEmitInstance *mi;
	struct NativeModelCacheKey key;
	struct NativeModelCacheEntry *e;
	const struct NativeModelCacheTri *tris;
	const struct TextureLayout *const *texTable;
	float eye[3], det;
	int instNeg;
	u32 t;

	NativeModelCache_CheckLevel();
#ifdef CTR_INTERNAL
	s_mpCheckPending = 0;
#endif
	s_mcLastWasMiss = 0;
	if ((s_modelEmitCurInst < 0) || (s_mcArena == NULL))
	{
		return 0;
	}

	NativeModelCache_KeyFromIdpp(idpp, &key);
	e = NativeModelCache_Find(&key);
	if (e == NULL)
	{
		s_mcWinMisses++;
		s_mcLastWasMiss = 1;
		return 0;
	}
	/* Not enough snapshot room for the whole record: keep the walk, which
	 * degrades per triangle to the packet path. */
	if ((dst->triCount + e->triCount) > MODELEMIT_MAX_TRIS)
	{
		s_mcWinNoRoom++;
		return 0;
	}

	mi = &dst->insts[s_modelEmitCurInst];
	if (!NativeModelCache_InstanceEye(mi, eye, &det))
	{
		return 0;
	}
	instNeg = ((s16)(u16)idpp->instFlags < 0);
	tris = (const struct NativeModelCacheTri *)(s_mcArena + e->offset);
	texTable = (const struct TextureLayout *const *)e->key.texLayout;

	for (t = 0; t < e->triCount; t++)
	{
		const struct NativeModelCacheTri *src = &tris[t];
		struct NativeModelEmitFace *face;
		struct NativeModelEmitVert *v;
		const struct TextureLayout *tex = ((src->texIndex != 0) && (texTable != NULL)) ? texTable[src->texIndex - 1] : NULL;
		int k;

		if (!NativeModelCache_FaceDrawn(src, eye, det, instNeg))
		{
			continue;
		}

		face = &dst->faces[dst->triCount];
		v = &dst->verts[dst->triCount * 3];

		if (tex != NULL)
		{
			face->tpage = tex->tpage;
			face->clut = tex->clut;
			face->textured = 1;
			v[0].u = tex->u0;
			v[0].v = tex->v0;
			v[1].u = tex->u1;
			v[1].v = tex->v1;
			v[2].u = tex->u2;
			v[2].v = tex->v2;
		}
		else
		{
			face->tpage = 0;
			face->clut = 0;
			face->textured = 0;
			v[0].u = v[0].v = v[1].u = v[1].v = v[2].u = v[2].v = 0;
		}
		face->blendOverride = 0xFF;
		face->_pad[0] = 0;
		face->_pad[1] = 0;

		for (k = 0; k < 3; k++)
		{
			v[k].x = src->p[k][0];
			v[k].y = src->p[k][1];
			v[k].z = src->p[k][2];
			v[k].r = src->rgb[k][0];
			v[k].g = src->rgb[k][1];
			v[k].b = src->rgb[k][2];
			v[k]._pad = 0;
			if (mi->alphaScale != 0)
			{
				v[k].r = NativeModelEmit_FadeComponent(v[k].r, mi->fcR, mi->alphaScale);
				v[k].g = NativeModelEmit_FadeComponent(v[k].g, mi->fcG, mi->alphaScale);
				v[k].b = NativeModelEmit_FadeComponent(v[k].b, mi->fcB, mi->alphaScale);
			}
		}

		if ((tex != NULL) && NativeModelEmit_TexIsSemiTrans(tex))
		{
			s_modelEmitWinSemiTris++;
		}
		dst->triCount++;
		dst->texTris += (u32)face->textured;
		mi->triCount++;
	}

	e->ref = 1;
	s_mcWinHits++;
	s_mcWinTrisIn += e->triCount;
	s_mcWinTrisOut += mi->triCount;
	return 1;
}

int NativeModelCache_BeginFill(const struct InstDrawPerPlayer *idpp)
{
	const struct NativeModelEmitInstance *mi;

	s_mcFillActive = 0;
	if (s_modelEmitCurInst < 0)
	{
		return 0;
	}
	if (s_mcArena == NULL)
	{
		if (s_mcArenaFailed)
		{
			return 0;
		}
		s_mcArena = (u8 *)malloc(MODELCACHE_ARENA_BYTES);
		if (s_mcArena == NULL)
		{
			s_mcArenaFailed = 1;
			Platform_Log("[MDLCACHE] arena allocation failed (%u bytes); models keep the walk\n", MODELCACHE_ARENA_BYTES);
			return 0;
		}
		s_mpArena = (u8 *)malloc(MODELPOSE_TOPO_BYTES);
		if (s_mpArena == NULL)
		{
			Platform_Log("[MDLPOSE] topology arena allocation failed (%u bytes); new poses keep the walk\n", MODELPOSE_TOPO_BYTES);
		}
		NativeModelCache_Reset();
		NativeModelCache_CheckLevel();
	}

	mi = &s_modelEmitBuf[s_modelEmitWriteIdx].insts[s_modelEmitCurInst];
	if (!NativeModelCache_InstanceEye(mi, s_mcFillEye, &s_mcFillDet))
	{
		return 0;
	}
	s_mcFillInstNeg = ((s16)(u16)idpp->instFlags < 0);
	NativeModelCache_KeyFromIdpp(idpp, &s_mcFillKey);
#ifdef CTR_INTERNAL
	memcpy(s_mcFillName, ((const struct Instance *)((const u8 *)idpp - sizeof(struct Instance)))->name, 16);
#endif
	s_mcFillCount = 0;
	s_mcFillVidOk = 1;
	s_mcFillMaxVid = 0;
	s_mcFillActive = 1;
	return 1;
}

/* retailVerdict: 1 = retail's NCLIP passed this face, 2 = retail's NCLIP
 * rejected it, 0 = not tested or rejected for another reason. */
internal void NativeModelCache_TriNormal(struct NativeModelCacheTri *t)
{
	float e1[3], e2[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		e1[k] = (float)t->p[1][k] - (float)t->p[0][k];
		e2[k] = (float)t->p[2][k] - (float)t->p[0][k];
	}
	t->n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	t->n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	t->n[2] = e1[0] * e2[1] - e1[1] * e2[0];
}

/* vid3: each corner's vertex in decode order, 1-based; 0 = not decoded by
 * this instance (a stack slot read before any write). */
void NativeModelCache_FillTriangle(const u32 *packedXY3, const u32 *packedZ3, const int *color3, u32 drawCommand, int retailVerdict, const int *vid3)
{
	struct NativeModelCacheTri *t;
	int k;

	if (!s_mcFillActive)
	{
		return;
	}
	if (s_mcFillCount >= MODELCACHE_FILL_MAX_TRIS)
	{
		s_mcFillActive = 0;
		return;
	}

	for (k = 0; k < 3; k++)
	{
		const int v = vid3[k];

		if ((v <= 0) || (v > (int)MODELPOSE_MAX_VERTS))
		{
			s_mcFillVidOk = 0;
		}
		else if ((u32)v > s_mcFillMaxVid)
		{
			s_mcFillMaxVid = (u32)v;
		}
		s_mcFillVid[s_mcFillCount][k] = (u16)v;
	}

	t = &s_mcFillTris[s_mcFillCount++];
	for (k = 0; k < 3; k++)
	{
		NativeModelEmit_UnpackVertex(packedXY3[k], packedZ3[k], &t->p[k][0], &t->p[k][1], &t->p[k][2]);
		t->rgb[k][0] = (u8)(color3[k] & 0xff);
		t->rgb[k][1] = (u8)((color3[k] >> 8) & 0xff);
		t->rgb[k][2] = (u8)((color3[k] >> 16) & 0xff);
	}
	t->flags = 0;
	if ((s32)(drawCommand << 3) >= 0)
	{
		t->flags |= MODELCACHE_TRI_DOUBLE_SIDED;
	}
	if ((s32)(drawCommand << 2) < 0)
	{
		t->flags |= MODELCACHE_TRI_FLIP;
	}
	t->texIndex = (u16)(drawCommand & 0x1ffu);
	NativeModelCache_TriNormal(t);

	if (retailVerdict != 0)
	{
		const int ours = NativeModelCache_FaceDrawn(t, s_mcFillEye, s_mcFillDet, s_mcFillInstNeg);

		if (ours == (retailVerdict == 1))
		{
			s_mcWinCullAgree++;
		}
		else if (ours)
		{
			s_mcWinCullWeDrawRetailCulled++;
		}
		else
		{
			s_mcWinCullWeCullRetailDrew++;
		}
	}
}

void NativeModelCache_AbortFill(void)
{
	s_mcFillActive = 0;
}

internal const struct NativeModelPoseTopo *NativeModelCache_FindTopo(const struct NativeModelCacheKey *k)
{
	u32 i;

	for (i = 0; i < s_mpTopoCount; i++)
	{
		const struct NativeModelPoseTopo *t = &s_mpTopos[i];

		if ((t->cmdList == k->cmdList) && (t->texLayout == k->texLayout) && (t->colorLayout == k->colorLayout) && (t->colorHash == k->colorHash))
		{
			return t;
		}
	}
	return NULL;
}

/* Places a record at the ring head under its key, replacing an older record
 * of the same key. Returns 0 when the arena cannot hold it. */
internal int NativeModelCache_InsertRecord(const struct NativeModelCacheKey *key, const struct NativeModelCacheTri *tris, u32 triCount)
{
	struct NativeModelCacheEntry *e;
	struct NativeModelCacheEntry *old;
	const u32 bytes = triCount * (u32)sizeof(struct NativeModelCacheTri);
	s32 off;
	u32 idx;
	u32 b;

	old = NativeModelCache_Find(key);
	if (old != NULL)
	{
		NativeModelCache_Unlink((u32)(old - s_mcEntries));
	}

	off = NativeModelCache_Alloc(bytes);
	if (off < 0)
	{
		return 0;
	}

	idx = s_mcHeadIdx;
	s_mcHeadIdx = (s_mcHeadIdx + 1) % MODELCACHE_MAX_ENTRIES;
	s_mcLiveCount++;

	e = &s_mcEntries[idx];
	e->key = *key;
	e->offset = (u32)off;
	e->bytes = bytes;
	e->triCount = (u16)triCount;
	e->live = 1;
	e->ref = 0;
	memcpy(s_mcArena + off, tris, bytes);
	s_mcArenaHead = (u32)off + bytes;

	b = NativeModelCache_HashKey(&e->key);
	e->nextInBucket = s_mcBuckets[b];
	s_mcBuckets[b] = (s16)idx;
	return 1;
}

/* Keeps the open fill's topology when its model has none yet. */
internal void NativeModelCache_StoreTopo(void)
{
	struct NativeModelPoseTopo *t;
	struct NativeModelPoseTri *dst;
	const u32 bytes = s_mcFillCount * (u32)sizeof(struct NativeModelPoseTri);
	u32 i;

	if ((s_mpArena == NULL) || NativeModelCache_FindTopo(&s_mcFillKey))
	{
		return;
	}
	if (!s_mcFillVidOk || (s_mpTopoCount >= MODELPOSE_MAX_TOPOS) || ((s_mpArenaHead + bytes) > MODELPOSE_TOPO_BYTES))
	{
		s_mpWinTopoRefused++;
		return;
	}

	t = &s_mpTopos[s_mpTopoCount++];
	t->cmdList = s_mcFillKey.cmdList;
	t->texLayout = s_mcFillKey.texLayout;
	t->colorLayout = s_mcFillKey.colorLayout;
	t->colorHash = s_mcFillKey.colorHash;
	t->offset = s_mpArenaHead;
	t->triCount = (u16)s_mcFillCount;
	t->vertCount = (u16)s_mcFillMaxVid;
	dst = (struct NativeModelPoseTri *)(void *)(s_mpArena + s_mpArenaHead);
	for (i = 0; i < s_mcFillCount; i++)
	{
		memcpy(dst[i].v, s_mcFillVid[i], sizeof(dst[i].v));
		dst[i].texIndex = s_mcFillTris[i].texIndex;
		memcpy(dst[i].rgb, s_mcFillTris[i].rgb, sizeof(dst[i].rgb));
		dst[i].flags = s_mcFillTris[i].flags;
	}
	s_mpArenaHead += (bytes + 3u) & ~3u;
	s_mpWinTopos++;
}

#ifdef CTR_INTERNAL
/* posecheck: the record built from the decoded pose against the record the
 * walk just filled for the same key, field by field. */
internal void NativeModelCache_CheckPose(void)
{
	u32 i;
	int same = (s_mpCheckCount == s_mcFillCount);

	for (i = 0; same && (i < s_mcFillCount); i++)
	{
		const struct NativeModelCacheTri *a = &s_mpCheckTris[i];
		const struct NativeModelCacheTri *b = &s_mcFillTris[i];

		same = (memcmp(a->p, b->p, sizeof(a->p)) == 0) && (memcmp(a->rgb, b->rgb, sizeof(a->rgb)) == 0) && (a->flags == b->flags) &&
		       (a->texIndex == b->texIndex) && (memcmp(a->n, b->n, sizeof(a->n)) == 0);
		if (!same && (s_mpWinCheckMismatch < 4))
		{
			const int pm = memcmp(a->p, b->p, sizeof(a->p)) != 0;
			const int cm = memcmp(a->rgb, b->rgb, sizeof(a->rgb)) != 0;

			Platform_LogInstrument("[MDLPOSE] mismatch %.15s tri %lu/%lu: p%d rgb%d flags%d tex%d | p %d,%d,%d %d,%d,%d %d,%d,%d vs %d,%d,%d %d,%d,%d %d,%d,%d | rgb0 "
			                       "%02x%02x%02x vs %02x%02x%02x\n",
			                       s_mcFillName, (unsigned long)i, (unsigned long)s_mcFillCount, pm, cm, a->flags != b->flags, a->texIndex != b->texIndex, a->p[0][0],
			                       a->p[0][1], a->p[0][2], a->p[1][0], a->p[1][1], a->p[1][2], a->p[2][0], a->p[2][1], a->p[2][2], b->p[0][0], b->p[0][1],
			                       b->p[0][2], b->p[1][0], b->p[1][1], b->p[1][2], b->p[2][0], b->p[2][1], b->p[2][2], a->rgb[0][0], a->rgb[0][1],
			                       a->rgb[0][2], b->rgb[0][0], b->rgb[0][1], b->rgb[0][2]);
		}
	}
	if (!same && (s_mpCheckCount != s_mcFillCount) && (s_mpWinCheckMismatch < 4))
	{
		Platform_LogInstrument("[MDLPOSE] mismatch %.15s: built %lu tris, walked %lu\n", s_mcFillName, (unsigned long)s_mpCheckCount,
		                       (unsigned long)s_mcFillCount);
	}
	if (same)
	{
		s_mpWinCheckMatch++;
	}
	else
	{
		s_mpWinCheckMismatch++;
	}
}
#endif

void NativeModelCache_EndFill(void)
{
#ifdef CTR_INTERNAL
	const int checkPending = s_mpCheckPending;

	s_mpCheckPending = 0;
#endif
	if (!s_mcFillActive)
	{
		return;
	}
	s_mcFillActive = 0;
	if (s_mcFillCount == 0)
	{
		return;
	}

#ifdef CTR_INTERNAL
	if (checkPending)
	{
		NativeModelCache_CheckPose();
	}
	if (NativeModelCache_Find(&s_mcFillKey) == NULL)
	{
		NativeModelCache_NoteNewKey(&s_mcFillKey);
	}
#endif
	NativeModelCache_StoreTopo();
	if (NativeModelCache_InsertRecord(&s_mcFillKey, s_mcFillTris, s_mcFillCount))
	{
		s_mcWinFills++;
	}
}

/* The vertex count to decode for this instance's pose, 0 when its model has
 * no topology yet or pose builds are off. Remembers the topology and key for
 * NativeModelCache_InsertPose. */
u32 NativeModelCache_PoseVertexCount(const struct InstDrawPerPlayer *idpp)
{
	s_mpCur = NULL;
	if ((s_mpArena == NULL) || (s_modelEmitCurInst < 0) || !s_mcLastWasMiss)
	{
		return 0;
	}
#ifdef CTR_INTERNAL
	if (!g_dbg_modelPose && !g_dbg_modelPoseCheck)
#else
	if (!g_dbg_modelPose)
#endif
	{
		return 0;
	}
	NativeModelCache_KeyFromIdpp(idpp, &s_mpCurKey);
	s_mpCur = NativeModelCache_FindTopo(&s_mpCurKey);
	return (s_mpCur != NULL) ? s_mpCur->vertCount : 0;
}

/* Gathers the decoded pose (packed retail vertex words, decode order) through
 * the remembered topology into a record and places it. Returns 1 when the
 * record was placed. */
int NativeModelCache_InsertPose(const u32 *packedXY, const u32 *packedZ)
{
	const struct NativeModelPoseTri *src;
	struct NativeModelCacheTri *out = s_mcFillTris;
	u32 i;
	int k;

	if (s_mpCur == NULL)
	{
		return 0;
	}
#ifdef CTR_INTERNAL
	if (g_dbg_modelPoseCheck)
	{
		out = s_mpCheckTris;
	}
#endif
	src = (const struct NativeModelPoseTri *)(const void *)(s_mpArena + s_mpCur->offset);
	for (i = 0; i < s_mpCur->triCount; i++)
	{
		struct NativeModelCacheTri *t = &out[i];

		for (k = 0; k < 3; k++)
		{
			const u32 v = (u32)src[i].v[k] - 1u;

			NativeModelEmit_UnpackVertex(packedXY[v], packedZ[v], &t->p[k][0], &t->p[k][1], &t->p[k][2]);
		}
		memcpy(t->rgb, src[i].rgb, sizeof(t->rgb));
		t->flags = src[i].flags;
		t->texIndex = src[i].texIndex;
		NativeModelCache_TriNormal(t);
	}
#ifdef CTR_INTERNAL
	if (g_dbg_modelPoseCheck)
	{
		s_mpCheckCount = s_mpCur->triCount;
		s_mpCheckPending = 1;
		return 0;
	}
#endif
	if (!NativeModelCache_InsertRecord(&s_mpCurKey, out, s_mpCur->triCount))
	{
		return 0;
	}
	s_mpWinBuilds++;
	return 1;
}

/*
 * [FADESELFTEST] -- proves NativeModelEmit_FadeComponent mirrors the real GTE
 * core, by running the core. One shot, gated behind --model-depth-check, and
 * placed at EndFrame: the frame's writers are done with the GTE, and the next
 * frame reloads everything it reads before reading it. Every register this
 * touches (colour FIFO, IR0-3, far colour) is saved and restored anyway, so
 * even that ordering argument is belt-and-braces.
 *
 * Sweeps IR0 through the fade range including both clamp directions and FC
 * through per-channel-distinct values, so a channel swap or a missed clamp in
 * the C mirror cannot pass. mismatches=0 is the pass; any other number means
 * the replay diverges from what the writer's own dpct produces.
 */
internal void NativeModelEmit_FadeSelfTest(void)
{
	static const int irs[] = {1, 37, 128, 1024, 2048, 4095, 4096, 8191};
	static const int cols[] = {0, 1, 127, 200, 255};
	static const s32 fcs[] = {0, 16, 255, 1200, 4080};
	u32 savedRgb[3], savedIr[4];
	s32 savedFc[3];
	u32 n = 0, bad = 0;
	int i, c, f, k;

	savedRgb[0] = MFC2(20);
	savedRgb[1] = MFC2(21);
	savedRgb[2] = MFC2(22);
	for (k = 0; k < 4; k++)
	{
		savedIr[k] = MFC2(8 + k);
	}
	savedFc[0] = (s32)CFC2(21);
	savedFc[1] = (s32)CFC2(22);
	savedFc[2] = (s32)CFC2(23);

	for (i = 0; i < (int)(sizeof(irs) / sizeof(irs[0])); i++)
	{
		for (c = 0; c < (int)(sizeof(cols) / sizeof(cols[0])); c++)
		{
			for (f = 0; f < (int)(sizeof(fcs) / sizeof(fcs[0])); f++)
			{
				/* Distinct per channel: R gets cols[c]/fcs[f], G the next
				 * entry of each, B the one after -- wraps within the table. */
				const int cr = cols[c], cg = cols[(c + 1) % 5], cb = cols[(c + 2) % 5];
				const s32 fr = fcs[f], fg = fcs[(f + 1) % 5], fb = fcs[(f + 2) % 5];
				const u32 packed = (u32)cr | ((u32)cg << 8) | ((u32)cb << 16);
				u32 out;

				CTC2((u32)fr, 21);
				CTC2((u32)fg, 22);
				CTC2((u32)fb, 23);
				MTC2(packed, 20);
				MTC2(packed, 21);
				MTC2(packed, 22);
				MTC2((u32)irs[i], 8);
				doCOP2(0x0F8002A); /* gte_dpct, the writer's own op word */

				for (k = 0; k < 3; k++)
				{
					out = MFC2(20 + k);
					n++;
					if (((out & 0xff) != NativeModelEmit_FadeComponent(cr, fr, irs[i])) || (((out >> 8) & 0xff) != NativeModelEmit_FadeComponent(cg, fg, irs[i])) ||
					    (((out >> 16) & 0xff) != NativeModelEmit_FadeComponent(cb, fb, irs[i])))
					{
						bad++;
					}
				}
			}
		}
	}

	MTC2(savedRgb[0], 20);
	MTC2(savedRgb[1], 21);
	MTC2(savedRgb[2], 22);
	for (k = 0; k < 4; k++)
	{
		MTC2(savedIr[k], (s32)(8 + k));
	}
	CTC2((u32)savedFc[0], 21);
	CTC2((u32)savedFc[1], 22);
	CTC2((u32)savedFc[2], 23);

	Platform_LogInstrument("[FADESELFTEST] n=%lu mismatches=%lu\n", (unsigned long)n, (unsigned long)bad);
}

void NativeModelEmit_EndFrame(void)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];

	/*
	 * Left open by a draw func that returned early (PrimMem exhaustion has a
	 * real `return` in the middle of DrawFunc_Normal). Close it here rather
	 * than leaking the open index into the next frame, where its triangles
	 * would be attributed to whichever instance opened next.
	 */
	if (s_modelEmitCurInst >= 0)
	{
		NativeModelEmit_EndInstance();
	}

	if (g_dbg_modelRender == 0)
	{
		return;
	}

	if (g_dbg_modelDepthCheck)
	{
		static int s_fadeSelfTestDone;

		if (!s_fadeSelfTestDone)
		{
			s_fadeSelfTestDone = 1;
			NativeModelEmit_FadeSelfTest();
		}
	}

	dst->frameSeq = ++s_modelEmitSeq;
	dst->valid = 1;

	/*
	 * Frames that captured nothing -- menus, load screens, the pause overlay --
	 * are dropped from the window rather than folded in. Averaging them drags
	 * every figure toward zero and makes a heavy race look cheap, the same
	 * shape of error as averaging a sweep across different stretches of track.
	 * Same rule native_model_frame.c already applies to [MODELFRAME].
	 */
	if (dst->triCount == 0)
	{
		return;
	}

	s_modelEmitWinFrames++;
	s_modelEmitWinTris += dst->triCount;
	s_modelEmitWinTextured += dst->texTris;
	s_modelEmitWinInsts += dst->instCount;
	if (dst->triCount > s_modelEmitWinPeakTris)
	{
		s_modelEmitWinPeakTris = dst->triCount;
	}

	if (s_modelEmitWinFrames < MODELEMIT_WINDOW_FRAMES)
	{
		return;
	}

	{
		u32 f = s_modelEmitWinFrames;

		/*
		 * peakTris is the number that decides MODELEMIT_MAX_TRIS, and it is
		 * reported next to the cap on purpose: a high-water mark quoted
		 * without the ceiling it is approaching is how a silent overflow gets
		 * read as headroom.
		 *
		 * texTris/f against tris/f says how much of a frame is the untextured
		 * G3 path, which decides whether rung 3 needs an untextured draw
		 * variant or can route everything through the atlas.
		 */
		/*
		 * semiTris/f is the population handed back to the packet path, and it is
		 * the number that says how much a real blended model draw is worth: it is
		 * both what native-only mode currently cannot draw AND what rung 3 would
		 * take over. Quoted per frame like its neighbours so it can be read
		 * against tris/f directly.
		 */
		Platform_LogInstrument("[MDLEMIT] semiTris/f=%lu snapRescued=%lu\n", (unsigned long)(s_modelEmitWinSemiTris / (f ? f : 1)),
		                       (unsigned long)s_modelEmitWinSnapRescued);

		/* hits/misses count instances; trisIn is what the hits held, trisOut
		 * what survived the model-space cull. cull= compares that cull with
		 * retail's NCLIP on the triangles of each fill: weCull (a face retail
		 * drew and this cull would drop) is the number that must stay 0. */
		Platform_LogInstrument("[MDLCACHE] mode=%d recycles=%lu\n", s_mcMode, (unsigned long)s_mcWinRecycles);
		s_mcWinRecycles = 0;
#ifdef CTR_INTERNAL
		NativeModelCache_LogNewKeys();
		Platform_LogInstrument("[MDLPOSE] mode=%d builds=%lu topos=%lu (+%lu, refused %lu) topoKiB=%lu check=%d match=%lu mismatch=%lu\n", g_dbg_modelPose,
		                       (unsigned long)s_mpWinBuilds, (unsigned long)s_mpTopoCount, (unsigned long)s_mpWinTopos, (unsigned long)s_mpWinTopoRefused,
		                       (unsigned long)(s_mpArenaHead / 1024u), g_dbg_modelPoseCheck, (unsigned long)s_mpWinCheckMatch,
		                       (unsigned long)s_mpWinCheckMismatch);
		s_mpWinCheckMatch = s_mpWinCheckMismatch = 0;
#else
		Platform_LogInstrument("[MDLPOSE] mode=%d builds=%lu topos=%lu (+%lu, refused %lu) topoKiB=%lu\n", g_dbg_modelPose, (unsigned long)s_mpWinBuilds,
		                       (unsigned long)s_mpTopoCount, (unsigned long)s_mpWinTopos, (unsigned long)s_mpWinTopoRefused,
		                       (unsigned long)(s_mpArenaHead / 1024u));
#endif
		s_mpWinBuilds = s_mpWinTopos = s_mpWinTopoRefused = 0;
		Platform_LogInstrument("[MDLCACHE] hits=%lu misses=%lu fills=%lu noRoom=%lu evicts=%lu live=%lu arenaKiB=%lu trisIn=%lu trisOut=%lu cull agree=%lu weDraw=%lu weCull=%lu\n",
		                       (unsigned long)s_mcWinHits, (unsigned long)s_mcWinMisses, (unsigned long)s_mcWinFills,
		                       (unsigned long)s_mcWinNoRoom, (unsigned long)s_mcWinEvicts, (unsigned long)s_mcLiveCount,
		                       (unsigned long)(s_mcArena ? (s_mcArenaBytes / 1024u) : 0u), (unsigned long)s_mcWinTrisIn, (unsigned long)s_mcWinTrisOut,
		                       (unsigned long)s_mcWinCullAgree, (unsigned long)s_mcWinCullWeDrawRetailCulled, (unsigned long)s_mcWinCullWeCullRetailDrew);
		Platform_LogInstrument("[MDLCACHE] skipped: noCapture=%lu specular=%lu blendWriter=%lu split=%lu | funcs normal=%lu alt=%lu split=%lu special=%lu other=%lu\n",
		                       (unsigned long)s_mcWinSkipNoCapture, (unsigned long)s_mcWinSkipSpecular, (unsigned long)s_mcWinSkipBlend,
		                       (unsigned long)s_mcWinSkipSplit, (unsigned long)s_mcWinFunc[0], (unsigned long)s_mcWinFunc[1], (unsigned long)s_mcWinFunc[2],
		                       (unsigned long)s_mcWinFunc[3], (unsigned long)s_mcWinFunc[4]);
		memset(s_mcWinFunc, 0, sizeof(s_mcWinFunc));
		s_mcWinSkipNoCapture = s_mcWinSkipSpecular = s_mcWinSkipBlend = s_mcWinSkipSplit = 0;
		s_mcWinHits = s_mcWinMisses = s_mcWinFills = s_mcWinNoRoom = s_mcWinEvicts = 0;
		s_mcWinTrisIn = s_mcWinTrisOut = 0;
		s_mcWinCullAgree = s_mcWinCullWeDrawRetailCulled = s_mcWinCullWeCullRetailDrew = 0;
		s_modelEmitWinSemiTris = 0;
		s_modelEmitWinSnapRescued = 0;

		Platform_LogInstrument("[MDLEMIT] lvl=%d f=%lu tris/f=%lu texTris/f=%lu inst/f=%lu peakTris=%lu/%d droppedTris=%lu droppedInst=%lu ownedDeclines=%lu\n",
		                       NativePerf_CurrentLevelID(), (unsigned long)f, (unsigned long)(s_modelEmitWinTris / f),
		                       (unsigned long)(s_modelEmitWinTextured / f), (unsigned long)(s_modelEmitWinInsts / f), (unsigned long)s_modelEmitWinPeakTris,
		                       MODELEMIT_MAX_TRIS, (unsigned long)s_modelEmitDroppedTrisTotal, (unsigned long)s_modelEmitDroppedInstTotal,
		                       (unsigned long)s_modelEmitOwnedDeclinesTotal);

		/* Emitted on the same window boundary so the two lines describe the
		 * same stretch of play. Ranges are printed alongside the error because
		 * "maxErr=0" over a sample that never left one depth is not a result --
		 * predLo/predHi have to show the content actually spanned some depth. */
		if (g_dbg_modelDepthCheck && (s_modelDepthSamples > 0))
		{
			Platform_LogInstrument("[MDLDEPTH] lvl=%d n=%lu maxErr=%ld meanErr=%.3f over1=%lu pred=%ld..%ld gte=%ld..%ld\n", NativePerf_CurrentLevelID(),
			                       (unsigned long)s_modelDepthSamples, (long)s_modelDepthMaxAbsErr, s_modelDepthSumAbsErr / (double)s_modelDepthSamples,
			                       (unsigned long)s_modelDepthOver1, (long)s_modelDepthPredLo, (long)s_modelDepthPredHi, (long)s_modelDepthActLo,
			                       (long)s_modelDepthActHi);

			s_modelDepthSamples = 0;
			s_modelDepthOver1 = 0;
			s_modelDepthSumAbsErr = 0.0;
			s_modelDepthMaxAbsErr = 0;
		}

		/* [AXISCHK] window flush -- same boundary as [MDLDEPTH] so the two
		 * lines describe the same stretch of play. One line per distance band
		 * with samples; ratio = mvp.t[2] / trueZ-on-the-track-axis. */
		if (g_dbg_modelDepthCheck)
		{
			static const char *bandName[4] = {"<1k", "1k-4k", "4k-16k", ">=16k"};
			int b;
			u32 total = s_axisChkN[0] + s_axisChkN[1] + s_axisChkN[2] + s_axisChkN[3];

			if (total > 0)
			{
				for (b = 0; b < 4; b++)
				{
					if (s_axisChkN[b] == 0)
					{
						continue;
					}
					Platform_LogInstrument("[AXISCHK] lvl=%d band=%s n=%lu ratio=%.4f (%.4f..%.4f)\n", NativePerf_CurrentLevelID(), bandName[b],
					                       (unsigned long)s_axisChkN[b], s_axisChkSum[b] / (double)s_axisChkN[b], s_axisChkMin[b], s_axisChkMax[b]);
					s_axisChkN[b] = 0;
					s_axisChkSum[b] = 0.0;
				}
				Platform_LogInstrument("[AXISCHK] lvl=%d huge=%lu behind=%lu maxResid=%lu raw: (%ld,%ld) (%ld,%ld) (%ld,%ld) (%ld,%ld)\n", NativePerf_CurrentLevelID(),
				                       (unsigned long)s_axisChkHuge, (unsigned long)s_axisChkBehind, (unsigned long)s_axisChkMaxResid, (long)s_axisChkRawTrue[0],
				                       (long)s_axisChkRawMvp[0], (long)s_axisChkRawTrue[1], (long)s_axisChkRawMvp[1], (long)s_axisChkRawTrue[2],
				                       (long)s_axisChkRawMvp[2], (long)s_axisChkRawTrue[3], (long)s_axisChkRawMvp[3]);
				s_axisChkHuge = 0;
				s_axisChkBehind = 0;
				s_axisChkMaxResid = 0;
				s_axisChkRawN = 0;
			}
		}
	}

	s_modelEmitWinFrames = 0;
	s_modelEmitWinTris = 0;
	s_modelEmitWinInsts = 0;
	s_modelEmitWinTextured = 0;
	s_modelEmitWinPeakTris = 0;
}

void NativeModelEmit_Clear(void)
{
	struct NativeModelEmitSnapshot *dst;

	/* Flip THEN invalidate the new write buffer -- the idiom GTECapture_Clear
	 * and NativeTrackFrame_Clear both use. Flipping first leaves the buffer
	 * being abandoned untouched for an in-flight walk that may still be
	 * reading it; the reset only ever touches the buffer this frame is about
	 * to write. */
	s_modelEmitWriteIdx ^= 1;
	dst = &s_modelEmitBuf[s_modelEmitWriteIdx];

	dst->triCount = 0;
	dst->instCount = 0;
	dst->texTris = 0;
	dst->droppedTris = 0;
	dst->droppedInstances = 0;
	dst->degraded = 0;
	dst->valid = 0;
	s_modelEmitCurInst = -1;
	NativeModelEmit_DropStoreHold();
}

void NativeModelEmit_PublishForWalk(void)
{
	s_modelEmitReadIdx = s_modelEmitWriteIdx;
}

/*
 * Does the native model path REPLACE the packet draw for this instance?
 *
 * States 1 and 2 mean: 0 = packets only (baseline), 1 = native only (packet write skipped),
 * 2 = both drawn (overlay diagnostic -- a position disagreement between the two renderers shows
 * as a visible double image). The platform test lives here rather than in game/ because native
 * model DRAW is 3DS-only; on PC, state 1 would disable the packet path with nothing to replace
 * it, and models would vanish.
 */
int NativeModelEmit_SkipRetailDraw(void)
{
#if defined(__3DS__)
	return (g_dbg_modelRender == 1);
#else
	return 0;
#endif
}


#if defined(__3DS__)
/*
 * ===========================================================================
 * Rung 2: the draw side.
 * ===========================================================================
 *
 * Turns the snapshot above into real PICA draws, via the plain-scalar
 * N3DS_ModelDraw_* API. Called from NativeRenderer_EndScene, which on
 * render-thread builds runs on the core-2 WORKER -- so everything read here is
 * either a copy-by-value out of the double-buffered snapshot or renderer-owned
 * state, never a live gGT/PushBuffer/Instance pointer. Same boundary the track
 * draw already observes.
 *
 * This file sits after platform/n3ds/native_renderer_3ds.c and
 * platform/native_gpu.c in main.c's unity include order, so
 * N3DS_ResolveTriangleTexture and GetTPageFormat are ordinary in-TU calls here,
 * exactly as native_track_frame.c reaches s_runUntextured.
 */

/* Compose one instance's projection rows. Identical derivation to
 * NativeTrackDraw_IssueForWalk's, with idpp->mvp (model->view) in place of the
 * camera's world->view matrix -- see platform/n3ds/shaders/ctr_model.v.pica for
 * the algebra. Kept as its own function so the two copies of this maths can be
 * diffed against each other and against the shader comment. */
internal void NativeModelDraw_ComposeRows(const struct NativeModelEmitInstance *inst, float qx[4], float qy[4], float qw[4], float *outH)
{
	float r00 = (float)inst->rot[0][0] * (1.0f / 4096.0f);
	float r01 = (float)inst->rot[0][1] * (1.0f / 4096.0f);
	float r02 = (float)inst->rot[0][2] * (1.0f / 4096.0f);
	float r10 = (float)inst->rot[1][0] * (1.0f / 4096.0f);
	float r11 = (float)inst->rot[1][1] * (1.0f / 4096.0f);
	float r12 = (float)inst->rot[1][2] * (1.0f / 4096.0f);
	float r20 = (float)inst->rot[2][0] * (1.0f / 4096.0f);
	float r21 = (float)inst->rot[2][1] * (1.0f / 4096.0f);
	float r22 = (float)inst->rot[2][2] * (1.0f / 4096.0f);
	float trx = (float)inst->trans[0];
	float trY = (float)inst->trans[1];
	float trz = (float)inst->trans[2];
	float h = (float)inst->h;
	float ofxOverH, ofyOverH;

	*outH = h;

	if (h == 0.0f)
	{
		/* Degenerate camera (distanceToScreen_PREV == 0). Every division below
		 * would be by zero; the caller skips the instance. */
		return;
	}

	ofxOverH = (float)inst->ofx / h;
	ofyOverH = (float)inst->ofy / h;

	if (NativeMirror_WorldFrame())
	{
		/* mirror mode: x' = axisSum - x, i.e. the view-x row negated about the mirrored centre */
		ofxOverH = (NativeMirror_AxisSum() - (float)inst->ofx) / h;
		r00 = -r00;
		r01 = -r01;
		r02 = -r02;
		trx = -trx;
	}

	qx[0] = r00 + (ofxOverH * r20);
	qx[1] = r01 + (ofxOverH * r21);
	qx[2] = r02 + (ofxOverH * r22);
	qx[3] = trx + (ofxOverH * trz);

	qy[0] = r10 + (ofyOverH * r20);
	qy[1] = r11 + (ofyOverH * r21);
	qy[2] = r12 + (ofyOverH * r22);
	qy[3] = trY + (ofyOverH * trz);

	qw[0] = r20 / h;
	qw[1] = r21 / h;
	qw[2] = r22 / h;
	qw[3] = trz / h;

	/*
	 * Depth-axis correction (see the depthShift field comment). The shader recovers depth as
	 * viewZ = hParam.x * dp4(qw, v); handing the draw side h scaled by the inverse regime factor
	 * puts this instance's depth on the track's view.z axis while leaving screen position
	 * bit-identical.
	 */
	if (inst->depthShift > 0)
	{
		*outH = h / (float)(1 << inst->depthShift);
	}
	else if (inst->depthShift < 0)
	{
		*outH = h * (float)(1 << -inst->depthShift);
	}
	/* Paint-under: depth reads NATIVE_MODEL_PAINT_UNDER_SCALE farther than
	 * honest (multiplicative -- the member is a shell around its cover, so a
	 * proportional push keeps it inside at every distance). */
	if (inst->paintUnder)
	{
		*outH *= NATIVE_MODEL_PAINT_UNDER_SCALE;
	}
}

/* Open run of triangles sharing one atlas + textured state. Runs never span
 * instances: each instance has its own MVP uniform, so a run that crossed one
 * would draw the second instance's triangles with the first one's matrix. */
struct NativeModelDrawRun
{
	int startVertex;
	int triCount;
	int atlas;
	int textured;
	int blend; /* BlendMode: 0 = opaque, else ABR+1 (see NativeModelDraw_FaceBlend) */
};

internal void NativeModelDraw_FlushRun(struct NativeModelDrawRun *run, u32 *drawsOut)
{
	if (run->triCount <= 0)
	{
		return;
	}

	N3DS_ModelDraw_Run(run->startVertex, run->triCount, run->atlas, run->textured, run->blend);
	(*drawsOut)++;
	run->triCount = 0;
}

/*
 * A face's PSX ABR (semi-transparency mode), or 3 for opaque.
 *
 * Same rule and same citation as the track's:
 * tpage bits 5-6, with 3 as CTR's "this page is opaque" sentinel. Reads the
 * face's own captured tpage, so nothing new had to be recorded at emit time.
 *
 * Untextured faces have no tpage and cannot be classified by it. They report
 * opaque, which is what retail's untextured model primitives overwhelmingly are;
 * the ABE bit an untextured G3 could carry is not visible at this capture point
 * (the primitive code is chosen by the writer this capture runs ahead of). That
 * is a known, bounded gap -- 35% of model triangles are untextured, but the
 * semi-transparent ones among them are effects that already looked opaque
 * throughout rung 1-2, so this changes nothing about them either way. Closing it
 * needs the capture to carry the writer's primitive code, which is a rung 3
 * change to the hook, not to this function.
 */
internal int NativeModelDraw_FaceBlend(const struct NativeModelEmitFace *face, const struct NativeModelEmitInstance *inst)
{
	/*
	 * Returns a BlendMode directly (0 = opaque, else PSX ABR + 1, matching native_gpu.c:49's
	 * GET_TPAGE_BLEND) rather than an ABR: "ABR 3" and "opaque" are not the same thing once a
	 * writer can force blending -- a forced face whose page carries ABR 3 blends as B+F/4
	 * (BM_ADD_QUATER_SOURCE), it does not become opaque, and only a BlendMode return can express
	 * that state.
	 */
	/* A per-face override outranks everything: the specular writers choose
	 * ABE by SIDE (lit front faces additive, the rest opaque), so neither the
	 * per-instance flag below nor the tpage can answer for them. */
	if (face->blendOverride != 0xFF)
	{
		return (int)face->blendOverride;
	}

	if (inst->ghostComposite)
	{
		/* The ghost writer forces its body additive regardless of the page:
		 * textured prims get their tpage ABR field overwritten to 1, untextured
		 * ones draw behind the writer's own ABR-1 DR_TPAGE. The B-F mask under
		 * it is emitted by the draw loop, not classified here. */
		return BM_ADD;
	}

	if (inst->forceSemiTrans)
	{
		/* The writer decided ABE; the tpage still decides HOW (the DEPTH_FADE
		 * writer emits the texture's own tpage word untouched). */
		return (int)((face->tpage >> 5) & 3) + 1;
	}

	if (!face->textured)
	{
		return 0;
	}

	{
		const int abr = (int)((face->tpage >> 5) & 3);

		/* ABR 3 is CTR's "this page is opaque" sentinel for the normal writer. */
		return (abr == 3) ? 0 : (abr + 1);
	}
}

/*
 * How the draw turns a face into vertices. 0: N3DS_ResolveTriangleTexture per
 * textured face, each face appended through N3DS_C3DAppendVertices. 1: the
 * texture through N3DS_ModelTex_Resolve's per-key table, appended the same way.
 * 2 (default): the table, with each instance's faces written straight into the
 * frame vertex buffer, and pass 1 skipping instances with no blended face.
 * dumpreq `modelfast [N]`.
 */
int g_dbg_modelFast = 2;

/* Pass 0 records which instances have a blended face; pass 1 visits only those
 * (and ghosts, whose mask pass 1 draws). */
global_variable u8 s_mdHasBlend[MODELEMIT_MAX_INSTANCES];

void NativeModelDraw_IssueForWalk(void)
{
	const struct NativeModelEmitSnapshot *snap = &s_modelEmitBuf[s_modelEmitReadIdx];
	static u32 s_logCountdown = 512;
	static u32 s_totalAppendFail = 0;
	u32 drawsIssued = 0, trisDrawn = 0, instDrawn = 0, appendFailed = 0, blendTrisDrawn = 0, maskTrisDrawn = 0;
	u32 i;
	const int fast = g_dbg_modelFast;

	if (g_dbg_modelRender == 0)
	{
		return;
	}

	/* Same contract as the track's: a degraded snapshot was TRUNCATED, so
	 * drawing it would show a frame missing arbitrary objects. Draw nothing
	 * rather than something incomplete. */
	if ((snap->valid == 0) || (snap->degraded != 0) || (snap->instCount == 0))
	{
		return;
	}

	N3DS_ModelDraw_Begin();

	/*
	 * TWO PASSES, opaque then blended -- the same shape as the resident track's
	 * and for the same reason: a blended
	 * surface must composite over everything opaque behind it, and "behind"
	 * spans instances, so the split cannot be per instance.
	 *
	 * What each pass draws, per PS1 textured semi-transparency: a semi-transparent
	 * primitive blends only the texels whose sampled colour has the STP bit set;
	 * the rest stay opaque. So a semi-transparent face is drawn TWICE --
	 *
	 *   pass 0: PASS1 view (its non-STP texels), blending OFF, depth written
	 *   pass 1: PASS2 view (its STP texels), blending ON, depth NOT written
	 *
	 * -- which is exactly what the packet path has done per-primitive
	 * (native_gpu.c:2670). Opaque faces draw once, in pass 0, with the OPAQUE
	 * view, unchanged from before this feature.
	 *
	 * Unlike the track, this needs no atlas planning at all: the decode cache
	 * keys cells on (page, clut, format, VIEW) and materialises them lazily, so
	 * asking it for PASS1/PASS2 simply produces two cells on first use. That is
	 * the whole reason models were the cheap half of this work.
	 *
	 * Cost when a frame has no semi-transparent model content: one extra walk
	 * over the instance list whose inner loop `continue`s on the first test, and
	 * no GPU state changes at all (the run never opens, so nothing flushes).
	 */
	for (int pass = 0; pass < 2; pass++)
	{
	for (i = 0; i < snap->instCount; i++)
	{
		const struct NativeModelEmitInstance *inst = &snap->insts[i];
		struct NativeModelDrawRun run = {0, 0, -1, 0, 0};
		float qx[4], qy[4], qw[4], h;
		int instanceBound = 0;
		u32 t;
		/* fast 2: this instance-pass's faces go straight into the frame vertex
		 * buffer, reserved at the first face drawn. Nothing else appends until
		 * the commit below. */
		N3DS_Vertex *direct = NULL;
		int directBase = 0, directRefused = 0;
		u32 directTris = 0;

		if (pass == 0)
		{
			s_mdHasBlend[i] = 0;
		}
		else if ((fast >= 2) && !s_mdHasBlend[i] && !inst->ghostComposite)
		{
			continue;
		}

		NativeModelDraw_ComposeRows(inst, qx, qy, qw, &h);

		if (h == 0.0f)
		{
			continue;
		}

		/*
		 * GHOST composite, mask half: under every one of its primitives the
		 * ghost writer draws a flat B-F triangle of the scratch fade colour;
		 * the additive body then relights the darkened footprint. One
		 * untextured BM_SUBTRACT run per instance, ahead of its bodies.
		 * Batching every mask before every body differs from retail's per-prim
		 * interleave only where the instance's triangles overlap on screen and
		 * the subtraction saturates at 0; the faces cannot depth-occlude each
		 * other in either order (blended runs write no depth).
		 */
		if ((pass == 1) && inst->ghostComposite)
		{
			struct NativeModelDrawRun maskRun = {0, 0, -1, 0, BM_SUBTRACT};

			for (t = 0; t < inst->triCount; t++)
			{
				const struct NativeModelEmitVert *src = &snap->verts[(inst->firstTri + t) * 3];
				N3DS_Vertex out[3];
				int offset;
				int v;

				for (v = 0; v < 3; v++)
				{
					out[v].x = src[v].x;
					out[v].y = src[v].y;
					out[v].z = src[v].z;
					out[v].w = src[v].z;
					out[v].u = 0;
					out[v].v = 0;
					out[v].r = inst->fadeR;
					out[v].g = inst->fadeG;
					out[v].b = inst->fadeB;
					out[v]._pad = 0;
				}

				if (!instanceBound)
				{
					instanceBound = 1;
					N3DS_ModelDraw_Instance(qx, qy, qw, h);
					instDrawn++;
				}

				offset = N3DS_C3DAppendVertices(out, 3);
				if (offset < 0)
				{
					appendFailed++;
					break;
				}

				if ((maskRun.triCount > 0) && (offset == (maskRun.startVertex + (maskRun.triCount * 3))))
				{
					maskRun.triCount++;
				}
				else
				{
					NativeModelDraw_FlushRun(&maskRun, &drawsIssued);
					maskRun.startVertex = offset;
					maskRun.triCount = 1;
				}

				maskTrisDrawn++;
			}

			NativeModelDraw_FlushRun(&maskRun, &drawsIssued);
		}

		for (t = 0; t < inst->triCount; t++)
		{
			const u32 triIndex = inst->firstTri + t;
			const struct NativeModelEmitFace *face = &snap->faces[triIndex];
			const struct NativeModelEmitVert *src = &snap->verts[triIndex * 3];
			const int faceBlendMode = NativeModelDraw_FaceBlend(face, inst);
			const int faceIsBlended = (faceBlendMode != 0);
			N3DS_Vertex out[3];
			int atlasIdx = -1, offU = 0, offV = 0;
			int textured = face->textured;
			int offset;
			int v;
			N3DS_TexView view;
			int runBlend;

			if (faceIsBlended)
			{
				s_mdHasBlend[i] = 1;
			}

			/*
			 * Pass 0 draws EVERY face -- opaque ones whole, semi-transparent ones
			 * as their PASS1 (non-STP) half. Pass 1 revisits only the
			 * semi-transparent faces to add their blended half.
			 *
			 * NOT `faceIsBlended != pass`, which is the obvious-looking version
			 * and is wrong: it would skip semi-transparent faces in pass 0 and so
			 * never draw their opaque body at all -- a waterfall or a plume would
			 * lose everything except its translucent texels.
			 */
			if ((pass == 1) && !faceIsBlended)
			{
				continue;
			}

			/*
			 * An untextured blended face has no PASS1 half: with no texels
			 * there is no STP split, the writer's whole primitive is the blend
			 * (the ghost's G3 body). Drawing it in pass 0 would paint it solid
			 * under its own pass-1 blend.
			 */
			if ((pass == 0) && faceIsBlended && !face->textured)
			{
				continue;
			}

			if (!faceIsBlended)
			{
				view = N3DS_TEXVIEW_OPAQUE;
				runBlend = 0;
			}
			else if (pass == 0)
			{
				view = N3DS_TEXVIEW_PASS1;
				runBlend = 0;
			}
			else
			{
				view = N3DS_TEXVIEW_PASS2;
				runBlend = faceBlendMode; /* already a BlendMode, see FaceBlend */
			}

			/* Deferred until a triangle actually survives the pass filter, so a
			 * pass that draws nothing for this instance costs no uniform upload
			 * and does not inflate instDrawn. */
			if (!instanceBound)
			{
				instanceBound = 1;
				N3DS_ModelDraw_Instance(qx, qy, qw, h);
				instDrawn++;
			}

			if (textured && fast)
			{
				/* Same answer as the per-face resolve below, from a table
				 * keyed on the texture; see N3DS_ModelTex_Resolve. */
				N3DS_ModelTex_Resolve((s16)face->tpage, (s16)face->clut, view, (u8)src[0].u, (u8)src[0].v, (u8)src[1].u, (u8)src[1].v, (u8)src[2].u,
				                      (u8)src[2].v, &atlasIdx, &offU, &offV);
			}
			else if (textured)
			{
				/*
				 * Resolve through the packet path's own decode cache -- same
				 * function, same memo, same eviction policy. Models arrive at
				 * the cache today as packets, so this adds no working set;
				 * design section 5.
				 *
				 * N3DS_ResolveTriangleTexture reads only .u/.v from the three
				 * GrVertex arguments, so a stack triple carrying just those is
				 * a faithful call and not a shortcut. Format comes from the
				 * face's own tpage (GetTPageFormat, native_gpu.c) rather than
				 * the renderer's s_currentTexFormat, which tracks the PACKET
				 * path's GPU state and means nothing here.
				 */
				GrVertex uvOnly[3];
				int pixelBaseU = 0, pixelBaseV = 0, originU = 0, originV = 0;

				for (v = 0; v < 3; v++)
				{
					memset(&uvOnly[v], 0, sizeof(uvOnly[v]));
					uvOnly[v].u = (u8)src[v].u;
					uvOnly[v].v = (u8)src[v].v;
				}

				N3DS_ResolveTriangleTexture(&uvOnly[0], &uvOnly[1], &uvOnly[2], (s16)face->tpage, (s16)face->clut, GetTPageFormat((int)(s16)face->tpage),
				                            view, &atlasIdx, &pixelBaseU, &pixelBaseV, &originU, &originV);
				offU = pixelBaseU - originU;
				offV = pixelBaseV - originV;
			}

			for (v = 0; v < 3; v++)
			{
				out[v].x = src[v].x;
				out[v].y = src[v].y;
				out[v].z = src[v].z;
				/* Single-channel for now: the model path computes its own depth rather than
				 * reading a packet stamp, so occlusion and parallax are the same number here. */
				out[v].w = src[v].z;

				if (textured)
				{
					/* Identical encoding to N3DS_ConvertTriangleVerts: texel-centre numerators
					 * (2a+1) against a 1/2048 shader scale, with V flipped as 2047-2a because the
					 * PICA samples the atlas bottom-up. See that function's comments for the
					 * derivation. */
					out[v].u = (s16)(((offU + (int)src[v].u) << 1) + 1);
					out[v].v = (s16)(2047 - ((offV + (int)src[v].v) << 1));
				}
				else
				{
					/* Untextured draws replace the fragment with the vertex
					 * colour outright (the combiner never samples), so the UV
					 * is genuinely unread. Zeroed rather than left
					 * uninitialised. */
					out[v].u = 0;
					out[v].v = 0;
				}

				out[v].r = src[v].r;
				out[v].g = src[v].g;
				out[v].b = src[v].b;
				out[v]._pad = 0;
			}

			if ((fast >= 2) && (direct == NULL) && !directRefused)
			{
				direct = N3DS_C3DReserveVertices((int)inst->triCount * 3, &directBase);
				directRefused = (direct == NULL);
			}

			if (direct != NULL)
			{
				memcpy(&direct[directTris * 3], out, sizeof(out));
				offset = directBase + (int)(directTris * 3);
				directTris++;
			}
			else
			{
				offset = N3DS_C3DAppendVertices(out, 3);
			}

			if (offset < 0)
			{
				/* Frame vertex budget exhausted. Counted and reported, never
				 * silent -- a silent version of this is what once deleted
				 * geometry with no log line at all (n3ds_backend.h's SIZING
				 * comment). Stop this instance; later instances would fail the
				 * same way. Pass 1 then visits it, since its faces were not all
				 * classified. */
				appendFailed++;
				s_mdHasBlend[i] = 1;
				break;
			}

			/* Extend the open run when this triangle is contiguous in the
			 * vertex buffer AND shares the run's state. Appends are sequential
			 * so contiguity normally holds; testing it rather than assuming it
			 * means a future change to the allocator cannot silently make one
			 * draw cover another run's vertices. */
			if ((run.triCount > 0) && (run.atlas == atlasIdx) && (run.textured == textured) && (run.blend == runBlend)
			    && (offset == (run.startVertex + (run.triCount * 3))))
			{
				run.triCount++;
			}
			else
			{
				NativeModelDraw_FlushRun(&run, &drawsIssued);
				run.startVertex = offset;
				run.triCount = 1;
				run.atlas = atlasIdx;
				run.textured = textured;
				run.blend = runBlend;
			}

			trisDrawn++;

			if (faceIsBlended && (pass == 1))
			{
				blendTrisDrawn++;
			}
		}

		if (direct != NULL)
		{
			N3DS_C3DCommitVertices((int)directTris * 3);
		}

		NativeModelDraw_FlushRun(&run, &drawsIssued);
	}
	}

	N3DS_ModelDraw_End();

	s_totalAppendFail += appendFailed;

	if (--s_logCountdown == 0)
	{
		s_logCountdown = 512;
		/* blendTris= is the pass-1 population: the blended half of semi-transparent
		 * model faces, drawn by US rather than handed back. Zero here while
		 * [MDLEMIT] semiTris/f is non-zero means the blend pass is not running --
		 * which is a different failure from "it runs and looks wrong", and the two
		 * are indistinguishable by eye. inst= counts instance BINDS, so it now
		 * exceeds the instance count on frames with blended content: an instance
		 * that appears in both passes binds twice, by design. */
		Platform_LogInstrument("[MDLDRAW] toggle=%d inst=%lu draws=%lu tris=%lu blendTris=%lu ghostMask=%lu appendFail=%lu appendFailTotal=%lu\n", g_dbg_modelRender,
		                       (unsigned long)instDrawn, (unsigned long)drawsIssued, (unsigned long)trisDrawn, (unsigned long)blendTrisDrawn,
		                       (unsigned long)maskTrisDrawn, (unsigned long)appendFailed, (unsigned long)s_totalAppendFail);
		/* Texture table lookups since the last line: hits, misses (each one a
		 * full resolve) split by cause, misses whose key the table may not
		 * hold, and CLUT generation moves served by palette adoption. */
		Platform_LogInstrument("[MDLTEX] fast=%d hits=%lu misses=%lu (key=%lu cell=%lu page=%lu clut=%lu display=%lu) unstored=%lu evict=%lu adopt=%lu\n", fast,
		                       (unsigned long)s_mtcHits, (unsigned long)s_mtcMisses, (unsigned long)s_mtcMissKey, (unsigned long)s_mtcMissCell,
		                       (unsigned long)s_mtcMissPage, (unsigned long)s_mtcMissClut, (unsigned long)s_mtcDisplay, (unsigned long)s_mtcUnstored,
		                       (unsigned long)s_mtcEvicts, (unsigned long)s_mtcClutAdopts);
		s_mtcHits = 0;
		s_mtcMisses = 0;
		s_mtcUnstored = 0;
		s_mtcMissKey = 0;
		s_mtcMissCell = 0;
		s_mtcMissPage = 0;
		s_mtcMissClut = 0;
		s_mtcClutAdopts = 0;
		s_mtcDisplay = 0;
		s_mtcEvicts = 0;
	}
}
#endif /* __3DS__ */

#else

int g_dbg_modelRender = 0;
/* Referenced by native_gte_capture.c's instance-depth bracket, which is gated on GTE_CAPTURE, not
 * MODEL_RENDERER -- so the definition must exist in a capture-on/model-off build (the PC golden
 * config), or the link fails. Stays 0 here; the --model-depth-check arg that sets it lives with
 * the real implementation above. */
int g_dbg_modelDepthCheck = 0;

int NativeModelEmit_BeginInstance(const struct InstDrawPerPlayer *idpp, const struct PushBuffer *pb, int forceSemiTrans, int specularLit,
                                  const struct Instance *worldInst, int ghostComposite, u32 fadeColorWord)
{
	(void)idpp;
	(void)pb;
	(void)forceSemiTrans;
	(void)specularLit;
	(void)worldInst;
	(void)ghostComposite;
	(void)fadeColorWord;
	return 0;
}

int NativeModelEmit_SpecularEnabled(void)
{
	return 0;
}

int NativeModelEmit_Triangle(const u32 *packedXY3, const u32 *packedZ3, const int *color3, const struct TextureLayout *tex, int blendOverride)
{
	(void)packedXY3;
	(void)packedZ3;
	(void)color3;
	(void)tex;
	(void)blendOverride;
	/* 0 = "not claimed", so a compiled-out build always writes the packet. */
	return 0;
}

void NativeModelEmit_OwnGteStores(void)
{
}

void NativeModelEmit_EndInstance(void)
{
}

void NativeModelEmit_EndFrame(void)
{
}

void NativeModelEmit_Clear(void)
{
}

void NativeModelEmit_PublishForWalk(void)
{
}

int NativeModelEmit_SkipRetailDraw(void)
{
	return 0;
}

#endif
