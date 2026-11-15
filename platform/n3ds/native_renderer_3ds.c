/*
 * Nintendo 3DS renderer backend -- phase 2a real data path.
 *
 * Replaces platform/native_renderer.c (OpenGL 3.3 + glad), which cannot run on
 * the PICA200. This file satisfies every signature in
 * include/platform/native_renderer.h so the rest of the unity build compiles
 * unmodified.
 *
 * What is REAL here: the CPU-side PSX VRAM mirror and every operation that
 * only touches it (clear/copy/read/save-state); citro3d init, the compiled
 * vertex shader, and the top-screen render target (behind n3ds_backend.h --
 * this file is part of the unity build and can never see libctru/citro3d
 * types directly, same reason as everything else that crosses that
 * boundary); the CPU texture decode cache (design doc section 1, all three
 * VRAM-CLUT formats plus the untextured/flat-color fast path); and the real
 * GrVertex -> N3DS_Vertex conversion with per-(page,clut) draw batching
 * (design doc section 6) that DrawAllSplits/DrawSplit (native_gpu.c) now
 * actually drives every frame via UpdateVertexBuffer/DrawTriangles.
 *
 * What is STILL STUBBED here, per the design doc's own staging: blending
 * beyond BM_NONE and the STP 2-pass (2b), stencil (real no-op, provably dead
 * code -- section 3), offscreen render targets (section 4, deferred),
 * framebuffer readback (section 5, deferred), stereo (2c), clip/scissor
 * (SetupClipMode -- not staged into 2a by the design doc, not touched this
 * pass), and the 32-bit RGBA override-texture path (TF_32_BIT_RGBA --
 * provably unreachable from any real game/ code path today, zero DR_PSYX_TEX
 * call sites; DrawTriangles skips a split entirely rather than guess at an
 * untested path).
 *
 * The bottom screen hosts a libctru text console (the stats block) so a
 * running main loop stays observable while the top screen is citro3d's.
 */

#include <stdio.h>
#include <string.h>

#include <macros.h>
#include <platform/native_gpu.h>
#include <platform/native_perf.h>
#include <platform/native_render_queue.h>
#include <platform/native_renderer.h>

#include "n3ds_backend.h"

/*
 * PSX VRAM mirror: 1024x512 16-bit. native_renderer.c owns this on the host and
 * nothing outside the renderer references it, so the 3DS backend owns its own.
 */
u16 vram[VRAM_WIDTH * VRAM_HEIGHT];

/*
 * Renderer config the PSX GPU layer reads directly (native_gpu.c consults it
 * when building texcoords). Defined by native_renderer.c on the host, so the
 * 3DS backend has to carry it too. Nearest-neighbour matches PSX output.
 */
int g_cfg_bilinearFiltering = 0;

global_variable int s_rendererInitialised = 0;
global_variable u32 s_frameCounter = 0;
/* Shadow of s_frameCounter as a plain unsigned int -- the type the
 * n3ds_backend.h dump-registration boundary expects (see SwapWindow). */
global_variable unsigned int s_frameCounterForDump = 0;

#ifdef CTR_INTERNAL
/* Per-triangle addressing dump: one frame of (page, clut, fmt, UVs,
 * positions, resolved atlas cell) records, armed alongside the capture set,
 * for diffing the ADDRESSING side against the same records from PC
 * (--tri-dump) at a matched scene. Record layout (LE, 36 bytes, mirrored in
 * native_offscreen_capture.c):
 *   u16 page, clut; u8 fmt, textured; u8 u0,u1,u2, v0,v1,v2;
 *   s16 x0,x1,x2, y0,y1,y2;
 *   s16 atlasIdx, pixelBaseU, pixelBaseV, originU, originV; u16 pad
 */
global_variable int s_triDumpArm = 0;
global_variable FILE *s_triDumpFile = NULL;

void NativeRenderer3DS_DebugArmTriangleDump(void)
{
	s_triDumpArm = 1;
}

internal void N3DS_TriDumpPutS16(u8 *dst, int v)
{
	dst[0] = (u8)(v & 0xFF);
	dst[1] = (u8)((v >> 8) & 0xFF);
}

internal void N3DS_TriDumpRecord(const GrVertex *t, int textured, int fmt, int atlasIdx, int pixelBaseU, int pixelBaseV, int originU, int originV)
{
	u8 rec[36];
	int i;

	if (s_triDumpFile == NULL)
	{
		return;
	}

	N3DS_TriDumpPutS16(&rec[0], (u16)t[0].page);
	N3DS_TriDumpPutS16(&rec[2], (u16)t[0].clut);
	rec[4] = (u8)fmt;
	rec[5] = (u8)textured;
	for (i = 0; i < 3; i++)
	{
		rec[6 + i] = t[i].u;
		rec[9 + i] = t[i].v;
	}
	for (i = 0; i < 3; i++)
	{
		N3DS_TriDumpPutS16(&rec[12 + i * 2], t[i].x);
		N3DS_TriDumpPutS16(&rec[18 + i * 2], t[i].y);
	}
	N3DS_TriDumpPutS16(&rec[24], atlasIdx);
	N3DS_TriDumpPutS16(&rec[26], pixelBaseU);
	N3DS_TriDumpPutS16(&rec[28], pixelBaseV);
	N3DS_TriDumpPutS16(&rec[30], originU);
	N3DS_TriDumpPutS16(&rec[32], originV);
	rec[34] = 0;
	rec[35] = 0;
	fwrite(rec, sizeof(rec), 1, s_triDumpFile);
}
#endif
global_variable unsigned long s_lastStatFrames = 0;
global_variable unsigned long s_lastStatVblanks = 0;

internal u16 NativeRenderer3DS_PackBGR555(u8 r, u8 g, u8 b)
{
	return (u16)(((r >> 3) & 31) | (((g >> 3) & 31) << 5) | (((b >> 3) & 31) << 10));
}

/*
 * ------------------------------------------------------------------------
 * CPU texture decode cache (design doc section 1) and real vertex
 * submission (section 6).
 * ------------------------------------------------------------------------
 *
 * TextureID is a sentinel, not a GPU handle -- SetTexture only ever receives
 * NativeRenderer_GetVRAMTexture() (real page/CLUT content, decode-cached) or
 * NativeRenderer_GetWhiteTexture() (untextured Gouraud/flat-color splits;
 * see native_gpu.c's AddSplit) in every reachable path. Both sentinels must
 * be distinct and non-zero -- the previous stub returned 0 for both, which
 * would have made them indistinguishable.
 */
#define N3DS_TEXTURE_ID_VRAM  ((TextureID)1)
#define N3DS_TEXTURE_ID_WHITE ((TextureID)2)

global_variable TextureID s_currentTextureId = N3DS_TEXTURE_ID_VRAM;
global_variable TexFormat s_currentTexFormat = TF_16_BIT;
/*
 * Latched draw state from DrawSplit (native_gpu.c), consumed by
 * DrawTriangles. s_currentBlendMode is the split's PS1 ABR mode as a
 * BlendMode value; s_semiTransPass is PC's two-pass shader STP split
 * (0 = normal, 1 = opaque-texels-only pass, 2 = STP-texels-only pass).
 *
 * PER-TEXEL STP: PC discriminates the passes per-texel in its shader; the
 * PICA has no fragment shader, so the pass split is baked into the decode
 * cache instead -- s_semiTransPass selects which N3DS_TexView a triangle's
 * texture resolves through (0->OPAQUE, 1->PASS1, 2->PASS2; see the
 * N3DS_TexView comment for the predicates). Pass 1 arrives with BM_NONE
 * (opaque draw of non-STP texels), pass 2 with the split's ABR mode (blend
 * of STP texels); the alpha test is the same GEQUAL-1 in every pass. Fixes
 * mixed-texel elements (wooden track arrows, tires, character-select
 * portraits) rendering wholly translucent.
 */
global_variable int s_currentBlendMode = 0; /* BM_NONE */
global_variable int s_semiTransPass = 0;
/*
 * Pending-run state, FRAME scope, not per-DrawTriangles-call (cross-split
 * run persistence). The survey's finding:
 * draw count ~= split count because every DrawTriangles
 * call ended with an unconditional flush, while the whole frame's texture
 * working set lives in ~3 atlases -- so consecutive splits usually bind
 * the SAME atlas and their runs can merge. A run now stays open across
 * calls and flushes only on: atlas change, blend-mode change, the
 * textured/untextured color-doubling flip, run-buffer budget, any
 * non-atlas draw (PresentVRAMRect/Display, atlas inspector), and EndScene. Those are
 * ALL the states that vary between 3DS draws today -- clip/stencil/mask
 * are stubs here (they'd each join the flush set when implemented).
 * Within a pending run, merging is only ever "don't flush yet", never
 * reorder. The draw merge below is what retires that rule for OPAQUE
 * content: the emit-ordinal depth test resolves paint order, so opaque
 * submission order is free and this run machinery carries the BLEND stream
 * only.
 */
global_variable int s_runAtlas = -1;
global_variable int s_runVertCount = 0;
/* -1 = unknown, forces the first call of each frame to assert TEV scale. */
global_variable int s_runUntextured = -1;
/* Blend mode the pending run was opened under. */
global_variable int s_runBlendMode = 0;

/*
 * Cross-bucket draw merge. The depth substrate makes opaque submission order
 * irrelevant -- the depth test against per-prim emit ordinals reproduces
 * painter's order regardless of issue order. So instead of flushing a draw
 * at every OT-walk state transition, opaque (BM_NONE) triangles accumulate
 * in per-state BINS for the whole frame and flush as one draw per bin at
 * the barrier.
 *
 * Bin key: on 3DS the only draw-relevant states are (atlas, TEV doubling,
 * blend mode) -- clip/stencil/mask/STP-output are stubs or global here.
 * Doubling is textured<=>1 without exception (native_gpu.c), and untextured
 * always resolves to the white cell, so (atlas x doubling) collapses to 13
 * bins: one per atlas for textured, plus one untextured/white bin.
 *
 * Blend (BM != NONE) content cannot reorder against itself -- BM_AVERAGE/
 * BM_SUBTRACT do not commute -- so it keeps the existing run accumulation,
 * but a closed run is RECORDED (verts appended to the frame VBO, draw
 * deferred) instead of drawn. The barrier then emits: all opaque bins,
 * then any spilled opaque segments, then blend segments in original order,
 * then the still-open blend run. Opaque-vs-blend layering is resolved by
 * the depth test (a blend prim fails LEQUAL wherever a later-ordinal opaque
 * prim already wrote), which is exactly painter's semantics; blend-vs-blend
 * order is preserved verbatim. A mid-frame barrier is ALWAYS legal: every
 * prim parsed after it carries a larger ordinal than everything drawn by it.
 *
 * Barrier sites: EndScene, PresentVRAMRect (foreign draw), bin/segment
 * capacity escalation. The blend run may stay OPEN across interleaved
 * opaque calls -- adjacent same-state blend runs merge across them, which
 * is legal for the same reason the barrier is.
 */
#define N3DS_MERGE_BIN_COUNT (N3DS_ATLAS_COUNT + 1) /* [0..11] textured per atlas, [12] untextured/white */
#define N3DS_MERGE_UNTEX_BIN N3DS_ATLAS_COUNT
/* Multiple of 3. Sized so typical frames (measured peak 2937 verts TOTAL,
 * n3ds_backend.h sizing note) never spill even into a single bin; overflow
 * spills to a segment (one extra draw), never drops. */
#define N3DS_MERGE_BIN_CAP_VERTS 3072
typedef struct
{
	u8 blendMode; /* BlendMode as int; BM_NONE for opaque spills */
	/* (see N3DS_MergeEmit for the two globals and the state below) */
	u8 doubling;  /* TEV color doubling to set for this draw */
	u8 atlas;
	int baseVertex; /* already appended to the frame VBO */
	int vertCount;
} N3DS_MergeSegment;
/* Opaque segments only exist when a bin spills mid-frame -- rare. */
#define N3DS_MERGE_MAX_OPAQUE_SEGS 64
/* Blend runs per frame; list full escalates to a (legal) mid-frame barrier. */
#define N3DS_MERGE_MAX_BLEND_SEGS 1024
global_variable N3DS_Vertex s_mergeBinVerts[N3DS_MERGE_BIN_COUNT][N3DS_MERGE_BIN_CAP_VERTS];
global_variable int s_mergeBinUsed[N3DS_MERGE_BIN_COUNT];
global_variable N3DS_MergeSegment s_mergeOpaqueSegs[N3DS_MERGE_MAX_OPAQUE_SEGS];
global_variable int s_mergeOpaqueSegCount = 0;
global_variable N3DS_MergeSegment s_mergeBlendSegs[N3DS_MERGE_MAX_BLEND_SEGS];
global_variable int s_mergeBlendSegCount = 0;

/*
 * The skybox is opaque but must draw UNDER translucent track surfaces
 * (waterfalls write no depth, so a later opaque draw at equal/farther depth
 * paints over them). Fix: collect sky triangles here instead of the opaque
 * bins, and draw them at the TOP of EndScene before the track pass; every
 * other packet effect keeps its normal post-track order. Sky content is
 * always untextured, so one draw via N3DS_WHITE_ATLAS/doubling 0 covers it.
 * Depth is still stamped far (PacketDepth_ResolveXY) so later
 * opaque cliffs occlude it correctly. Walk-thread-local, no double buffer;
 * overflow silently drops excess sky triangles rather than flushing
 * mid-walk.
 */
#define N3DS_SKY_VERT_CAP 2048
global_variable N3DS_Vertex s_skyVerts[N3DS_SKY_VERT_CAP];
global_variable int s_skyVertUsed = 0;
/* Where the GPU dome goes in s_skyVerts: the first triangle that is not
 * horizon glow, -1 while there is none. Retail links the glow into the sky's
 * OT slot after the sky, so the walk meets it first and the sky paints over
 * it; everything else in the sky class (stars) comes after the sky. */
global_variable int s_skyVertDomeAt = -1;

/* s46 BACKDROP accumulator -- the layer UNDER the sky accumulator. Retail
 * clears the frame with GPU FILL packets (CAM_ClearScreen: flat clearColor
 * split at the horizon) before painting the horizon gradient and dome over
 * them; on 3DS those fills reach NativeRenderer_Clear, which appends them
 * here as far-pin quads. A separate buffer from s_skyVerts on purpose: the
 * first fix appended into s_skyVerts assuming the walk visits the fill
 * before the sky, and it does NOT (bucket 0x3FF is walked head-first, and
 * the sky prims are prepended after the fills), so the backdrop painted
 * over the dome. Own buffer + own drain slot makes the order structural:
 * N3DS_DrawBackdropPass runs immediately before N3DS_DrawSkyPass at the same
 * far-pin depth, and LEQUAL's pass-on-equal lets each later layer win -- bg
 * color, then gradient/dome, then everything real. (The first attempts also
 * wrote vertex z=0 = RAW 32768, a mid-range depth that buried the sky no
 * matter the order -- the far pin on the vertex axis is -32768.) 2 fills x
 * 6 verts x 4 players bounds the cap; overflow drops. Same lifecycle as
 * s_skyVerts. */
#define N3DS_BACKDROP_VERT_CAP 48
global_variable N3DS_Vertex s_backdropVerts[N3DS_BACKDROP_VERT_CAP];
global_variable int s_backdropVertUsed = 0;

/*
 * UI cannot use the state-bin depth-substrate reorder that world content uses:
 * a glyph and the panel behind it are different GPU states, and binning-by-state
 * is exactly what would separate them and break their relative order.
 *
 * Fix (both parts required together): (1) every UI-band primitive is pinned to
 * ONE depth stamp (PacketDepth_UiStamp), so GPU_LEQUAL passes on equal and
 * later-submitted UI always wins -- no ordinal budget/wrap/saturation; (2) UI
 * primitives are routed OUT of the state bins into this ordered list and
 * replayed last, in submission order (N3DS_DrawUiPass).
 *
 * The stamp is still read as a UI classifier elsewhere (>= 0xFFC0, stereo
 * flatten, widescreen X ramp) -- do not remove the band.
 *
 * EXACTLY ONE PRODUCER is load-bearing: appends must arrive in OT-walk order,
 * so only NativeRenderer_DrawTriangles may append here; the direct-emit fast
 * path declines 2D/UI nodes for this reason. A second producer would interleave
 * out of walk order without a per-prim sequence number to sort on.
 *
 * Opaque and blended UI share the list: blended UI used to be emitted after all
 * opaque UI, so a translucent panel submitted before a glyph still landed on
 * top of it.
 *
 * Runs coalesce while state (blend/doubling/atlas) holds and close to a segment
 * on any change, so a text-heavy menu costs a handful of draws, not one per
 * glyph. Walk-thread-local and drawn on the same thread in EndScene, same
 * lifecycle as s_mergeBinVerts.
 *
 * This fixes a broad class of UI layering defects (menu and character-select
 * text layering, the selected portrait being covered by its own highlight).
 *
 * The character-select diorama gradient is NOT this pass's content: the
 * backdrop strips never enter this list. They are screen-space quads with no
 * captured depth, pinned to the far plane through the menu-backdrop span
 * (native_gpu.c, NativeGpu_MarkMenuBackdropRange) like the rest of the
 * backdrop class.
 */
/* Multiple of 3. A text-heavy menu (track lists, high scores) is the peak
 * producer; overflow closes the open run early and keeps going, so the cost of
 * a small buffer is extra draws, never dropped or misordered UI. */
#define N3DS_UIORD_RUN_CAP 3072
#define N3DS_UIORD_MAX_SEGS 512
global_variable N3DS_Vertex s_uiOrdRunVerts[N3DS_UIORD_RUN_CAP];
global_variable int s_uiOrdRunUsed = 0;
global_variable int s_uiOrdRunBlend = -1;
global_variable int s_uiOrdRunDoubling = -1;
global_variable int s_uiOrdRunAtlas = -1;
global_variable N3DS_MergeSegment s_uiOrdSegs[N3DS_UIORD_MAX_SEGS];
global_variable int s_uiOrdSegCount = 0;
/* Prims that could not be recorded (segment list full). Reported by the stats
 * block: silently dropping UI would read as "covered everything" when it did not. */
global_variable u32 s_uiOrdDropped = 0;
/* High-water UI prims in one frame -- the replacement for [PKTDEPTH] uiMax, which
 * measured a budget that no longer exists. Kept because it is the number that says
 * whether this list is near ITS capacity. */
global_variable u32 s_uiOrdPrimsMax = 0;
global_variable u32 s_uiOrdPrims = 0;

/*
 * BOTTOM-SCREEN MINIMAP (dumpreq 'btmmap', default ON).
 *
 * The in-race map block (UI_RenderFrame_Racing: sheet + racer/ghost/warpball
 * icons) is address-tagged at emission (NativeGpu_MarkUiMapRange, the sky-span
 * mechanism) and restamped PKTDEPTH_UI_STAMP_BTMMAP by the resolver -- still
 * inside the >= 0xFFC0 UI band, so the stereo flatten and widescreen ramp are
 * unchanged. The UI branch below peels those triangles into this parallel
 * ordered list instead of s_uiOrd*, and N3DS_DrawBottomMapPass replays them
 * onto the bottom render target with an ortho window fitted around their own
 * screen rect -- same untouched vertices, magnified by the window, so the dots
 * cannot drift off the sheet. Runs after the stereo right eye (nothing here is
 * journaled) and before FrameEnd. The resolver only retags while the
 * ordered-UI pin owns the band. The stamp itself is applied regardless
 * of 'btmmap' -- it also keys the seam-closing far-edge UV inset
 * (N3DS_ConvertTriangleVerts) -- so the PEEL below carries the gate: with
 * 'btmmap' off the stamped map flows into the ordinary ordered-UI list and
 * draws on the top screen as before, inset included.
 */
int g_dbg_btmMap = 1;
/*
 * Map on BOTH screens (dumpreq 'mapboth', default ON). When the map span was
 * emitted by the game's own draw-on-top condition (map mode / 3P --
 * NativeGpu_UiMapRetailTop, carried on the span itself), the peel below
 * COPIES each map triangle: one into the bottom list as ever, one back into
 * the ordinary ordered-UI list re-pinned like the unrouted leg, so the top
 * screen keeps retail's map at retail's position while the bottom panel
 * magnifies it. Speedo-mode spans (RouteActive override) are never copied --
 * the override is legal precisely because routed prims stay off the top
 * screen there. Off = today's move-only routing.
 */
int g_dbg_mapBoth = 1;
/* Copy-leg forensics for the [BTMMAP] on line: total top copies emitted since
 * boot. Nonzero proves the copy leg fires; the on line samples it with the
 * retailTop flag so a missing top map can be attributed to the mode signal vs
 * the leg itself. */
global_variable unsigned long s_mbTopCopyTris = 0;
/*
 * Bottom-console visibility (dumpreq 'stats', default OFF -- release posture).
 * Gates every painter that writes glyphs into the bottom framebuffer: the
 * stats drip (N3DS_StatsDripRow) and Platform_Log's stdout echo
 * (native_log.c), both through N3DS_ConsolePaintAllowed. Composition, the
 * [PERF]/SD sinks and the netload stderr mirror are untouched -- only the
 * PANEL goes quiet. Boot text stays visible until the first present
 * (N3DS_GfxPresent wipes and arms the quiet then), so an asset error that
 * aborts before the game runs is still readable on the screen.
 */
int g_dbg_statsConsole = 0;
/* Last measured fps x10 (the stats block's own 64-frame window number),
 * published for the bottom-screen FPS overlay (NativeUiMap_FpsOverlayText,
 * native_gpu.c) -- which draws it in the game's own font inside the map
 * block. External linkage on purpose: the consumer sits earlier in the TU. */
unsigned long g_n3dsFpsX10 = 0;
/* Multiple of 3. The map block is ~20 quads worst case (8 drivers + ghosts +
 * warpball pair + 2 sheet halves); overflow closes the run early, extra draws
 * not lost content, same policy as the UI list above. */
#define N3DS_BTMM_RUN_CAP 384
#define N3DS_BTMM_MAX_SEGS 16
global_variable N3DS_Vertex s_btmmRunVerts[N3DS_BTMM_RUN_CAP];
global_variable int s_btmmRunUsed = 0;
global_variable int s_btmmRunBlend = -1;
global_variable int s_btmmRunDoubling = -1;
global_variable int s_btmmRunAtlas = -1;
global_variable N3DS_MergeSegment s_btmmSegs[N3DS_BTMM_MAX_SEGS];
global_variable int s_btmmSegCount = 0;
global_variable u32 s_btmmDropped = 0;
/* Union bbox (PSX coords) of everything peeled this frame -- the raw input to
 * the display window fit. */
global_variable short s_btmmFrameBox[4];
global_variable int s_btmmFrameHasBox = 0;
/* Latched display window. Grows to include the raw box immediately (a dot
 * stepping outside must never clip), re-centers only when the raw box jumps
 * (mode/track change) -- so racer dots wandering near the sheet edge cannot
 * make the window shimmy every frame. */
global_variable short s_btmmWin[4];
global_variable int s_btmmWinValid = 0;
global_variable int s_btmmWasActive = 0;
/* FPS-overlay class (PKTDEPTH_UI_STAMP_BTMFPS): the map block's text glyphs,
 * kept out of the map's segment list and window so they can be drawn through
 * their own non-magnifying window at the panel's top-left. Sized for GLYPHS,
 * not draws: the sub-page decode tier can hand every glyph a different atlas
 * cell, so a short string legally splits into one segment per glyph -- the
 * first sizing (4 segs) saturated on "FPS 25.2", silently dropping the F and
 * producing the sitting's dropped=896 ([BTMMAP] forensics line fpsSegs=4 at
 * the cap convicted it). One segment per glyph plus slack is the honest
 * budget. */
#define N3DS_BTMF_RUN_CAP 192
#define N3DS_BTMF_MAX_SEGS 16
global_variable N3DS_Vertex s_btmfRunVerts[N3DS_BTMF_RUN_CAP];
global_variable int s_btmfRunUsed = 0;
global_variable int s_btmfRunBlend = -1;
global_variable int s_btmfRunDoubling = -1;
global_variable int s_btmfRunAtlas = -1;
global_variable N3DS_MergeSegment s_btmfSegs[N3DS_BTMF_MAX_SEGS];
global_variable int s_btmfSegCount = 0;
global_variable short s_btmfFrameBox[4];
global_variable int s_btmfFrameHasBox = 0;
/* Drop forensics: a nonzero dropped total appeared the first Arcade race after
 * the FPS split landed (dropped=896, source unknown). Split the total by which
 * limit fired so the [BTMMAP] release line convicts a specific cap instead of
 * inviting theory. Peaks say how close the seg lists run in normal play. */
global_variable u32 s_btmmDropSegFull = 0;
global_variable u32 s_btmmDropRunCap = 0;
global_variable u32 s_btmmDropVbo = 0;
global_variable u32 s_btmfDropSegFull = 0;
global_variable u32 s_btmfDropRunCap = 0;
global_variable u32 s_btmfDropVbo = 0;
global_variable int s_btmmSegsPeak = 0;
global_variable int s_btmfSegsPeak = 0;
/* Last FPS-overlay window actually used (PSX units), for the activation log --
 * the clipped-F hunt needs the window the GPU really got, not the theory. */
global_variable short s_btmfWinDbg[4];

/* Diagnostics for the stats block: draws emitted by the merge path split
 * by class, plus the events that cost extra draws or a mid-frame barrier. */
global_variable u32 s_mergeEmitOpaque = 0;
global_variable u32 s_mergeEmitBlend = 0;
global_variable u32 s_mergeBinSpills = 0;
global_variable u32 s_mergeEscalations = 0;

/*
 * UI probe. The counters above say HOW MANY draws each class costs; these
 * say how BIG they are and WHY the blend ones end where they do.
 *
 * The question they exist to answer: opaque content is binned by state across
 * the whole OT walk, so it collapses to ~one draw per atlas, but blended
 * content cannot join a bin -- blending is order-dependent and the bins are
 * order-free -- so it stays a sequence of runs, each ended by the next state
 * change and each becoming its own draw. If the HUD's cost is that structure,
 * blend draws will be numerous and TINY (verts/draw near the per-call floor)
 * while opaque draws are few and huge. If instead verts/draw is comparable
 * across the two, the draw structure is not where the HUD's milliseconds are
 * and the blend hypothesis is dead -- which is a result, not a null.
 *
 * The close-cause split then says what to fix if it survives: an atlas-change
 * majority is a texture-locality problem (reachable by atlas packing), a
 * state-change majority is a content problem (the HUD really does alternate
 * blend modes), and frame-end is just the tail run every frame pays once.
 */
/* Accumulated at each emit CALL SITE, next to the draw counter it will be
 * divided by, rather than inside N3DS_MergeEmit keyed off the blend mode: the
 * first version did the latter and immediately reported 43 blend draws holding
 * 60 vertices, because the mode it keyed off had been clobbered before the
 * emit. Counting the two together at one site makes that class of drift
 * impossible. */
global_variable u32 s_mergeVertsOpaque = 0;
global_variable u32 s_mergeVertsBlend = 0;
global_variable u32 s_blendCloseAtlas = 0;
global_variable u32 s_blendCloseState = 0;
global_variable u32 s_blendCloseFrameEnd = 0;
/* Previous window's values, for the [MERGEB] deltas. */
global_variable u32 s_lastStatMergeEmitOpaque = 0;
global_variable u32 s_lastStatMergeEmitBlend = 0;
global_variable u32 s_lastStatMergeVertsOpaque = 0;
global_variable u32 s_lastStatMergeVertsBlend = 0;
global_variable u32 s_lastStatBlendCloseAtlas = 0;
global_variable u32 s_lastStatBlendCloseState = 0;
global_variable u32 s_lastStatBlendCloseFrameEnd = 0;

enum N3DS_BlendCloseCause
{
	N3DS_BLEND_CLOSE_ATLAS = 0, /* mid-run atlas change (texture locality)   */
	N3DS_BLEND_CLOSE_STATE,     /* blend mode or textured/untextured change  */
};

/*
 * Bottom-screen verbosity (dumpreq 'verbose'). OFF by default: the
 * stats block had accreted every instrument this port has ever needed --
 * decode-cache hits/misses/evictions/stale, sub-page routing, hazard
 * counters, scene begin/end, audio thread microsecond profile, per-bucket perf
 * -- and on real hardware that screen is the ONLY readout there is. The owner
 * could not find the numbers that matter in the wall of ones that don't.
 *
 * This gates the DIAGNOSTIC lines only. What stays unconditional: frames/fps,
 * heap, game state, the toggle block, draws/frame, merge state, and total
 * frame/work time -- i.e. everything you steer by. Each gated group is pure
 * output; no counter update or reset lives inside one, so flipping this cannot
 * change what any instrument measures, only whether it is displayed.
 */
int g_dbg_statsVerbose = 0;

#ifdef CTR_INTERNAL
/*
 * AUTOMATED CONFIG SWEEP (dumpreq 'sweep').
 *
 * The hardware run that motivated this produced retail n=13 windows against
 * ours n=137, clustered in different stretches of the same race -- so the one
 * number that mattered (is the resident track renderer faster than the OT walk
 * it replaces?) came out unmatched, and the honest answer had to be "direction
 * yes, magnitude untrustworthy". Hand-driving four chords mid-race cannot fix
 * that; only equal exposure on the same content can.
 *
 * While on, this walks the config table below, holding each for
 * N3DS_SWEEP_WINDOWS stats windows and emitting one [SWEEP] line per config
 * with that config's OWN averaged frame/work microseconds. Every config sees
 * the same lap, in rotation, so scene content averages out instead of being
 * confounded with the variable under test -- the exact failure mode
 * PORT_FEASIBILITY_3DS.md's "How to measure" section was written about.
 *
 * The first window after a config change is DISCARDED (s_sweepSettling): the
 * stats block averages over the window it just finished, so the window
 * spanning a switch contains both configs.
 */
int g_dbg_sweep = 0;

typedef struct
{
	const char *name;
	int trackRender;    /* g_dbg_trackRender       */
	int packetDepth;    /* g_dbg_packetDepth       */
	int skipInstances;  /* gCtrDebugSkipInstances  */
	int skipEffects;    /* gCtrDebugSkipEffects    */
	int skipHUD;        /* gCtrDebugSkipHUD        */
	int vsyncCredit;    /* g_dbg_vsyncCredit       */
} N3DS_SweepConfig;

/*
 * CONTENT ABLATION. Depth-bias configs are not included: an offset has
 * nothing to act on once every near primitive already pins to
 * depthMinWorld (near-clamp saturation). Each row below removes exactly
 * one content class; frame-time delta from "full" is that class's entire
 * cost, game-side build plus submission. "retail" stays first as the
 * anchor that validates the harness every run.
 */
static const N3DS_SweepConfig s_sweepConfigs[] = {
    {"retail", 0, 0, 0, 0, 0, 1},     /* the baseline every fps claim must beat   */
    {"full", 1, 1, 0, 0, 0, 1},       /* shipping config; ablation baseline       */
    {"no-vsyncfx", 1, 1, 0, 0, 0, 0}, /* vblank credit fix, OFF -- not content    */
    {"no-inst", 1, 1, 1, 0, 0, 1},    /* karts, drivers, crates, weapons, pickups */
    {"no-hud", 1, 1, 0, 0, 1, 1},     /* race HUD                                 */
};

/*
 * no-fx (particles, weather, tires, shadows) is GONE, not disabled: it crashed
 * a hardware run. Those RenderAll* calls advance state the rest of the
 * frame reads, so removing the draw removes a side effect something else
 * depends on. Ablating them needs the state/draw split done first.
 */

#define N3DS_SWEEP_CONFIG_COUNT ((int)(sizeof(s_sweepConfigs) / sizeof(s_sweepConfigs[0])))

/*
 * ONE measured window per config, NOT four.
 *
 * Four windows held each config for ~12 seconds of a lap, so with only one
 * cycle completed each config was measured on a DIFFERENT stretch of track --
 * and a stretch of Polar Pass differs from another stretch by more than the
 * variables under test. That is not a smaller version of the right experiment;
 * it is the same confound that made an earlier report claim "ours is slower"
 * from a log that actually said the opposite, reintroduced by the harness
 * written to prevent it. Long blocks only average out if the run lasts many
 * cycles, and runs get cut short.
 *
 * Short blocks instead: a config change costs one discarded settling window, so
 * one measured window is ~6s of wall clock per config and a full 5-config cycle
 * fits in ~30s. Several cycles per lap, adjacent configs seeing near-identical
 * track, and a run that crashes early still leaves matched cycles behind.
 * Average the per-cycle [SWEEP] lines afterwards; do NOT read a single cycle.
 */
#define N3DS_SWEEP_WINDOWS 1

static int s_sweepIndex = 0;
static int s_sweepWindowsLeft = 0;
static int s_sweepSettling = 0;
static int s_sweepWasOn = 0;
static unsigned long s_sweepTotalUs = 0;
static unsigned long s_sweepWorkUs = 0;
static int s_sweepSamples = 0;

static void N3DS_SweepApply(int idx)
{
	extern int g_dbg_packetDepth;
	extern int g_dbg_pktDepthBias;
	extern int g_dbg_trackRender;
	extern volatile int gCtrDebugSkipInstances;
	extern volatile int gCtrDebugSkipEffects;
	extern volatile int gCtrDebugSkipHUD;
	extern int g_dbg_vsyncCredit;
	const N3DS_SweepConfig *cfg = &s_sweepConfigs[idx];

	g_dbg_trackRender = cfg->trackRender;
	g_dbg_packetDepth = cfg->packetDepth;
	gCtrDebugSkipInstances = cfg->skipInstances;
	gCtrDebugSkipEffects = cfg->skipEffects;
	gCtrDebugSkipHUD = cfg->skipHUD;
	g_dbg_vsyncCredit = cfg->vsyncCredit;

	/* Publish the config to the windowed instruments so [PERF] and [OTPROF]
	 * cut their windows here too -- see include/platform/native_gpu.h. The
	 * table's name literals are the identity, so this is a pointer store. */
	g_dbg_ablateConfig = cfg->name;

	/*
	 * Forced, not carried: the sweep no longer varies bias, so a stray bias set
	 * before the sweep started would otherwise ride along through every config
	 * and silently make the whole run un-comparable to the last one.
	 */
	g_dbg_pktDepthBias = 0;
}
#endif /* CTR_INTERNAL */

/*
 * Debug-only, mirrors PC's g_dbg_texturelessMode (native_renderer.c:79,
 * F2 key). No keyboard here -- toggled by ZL, see n3ds_backend.c's
 * N3DS_ReadButtons. Forces every draw through the same untextured/white
 * path real Gouraud splits already use (see NativeRenderer_SetTexture),
 * so it also zeroes decode-cache activity -- useful for isolating whether
 * a slow frame is texture-related at all.
 */
int g_dbg_texturelessMode = 0;

/*
 * Diagnostic: force EVERY textured triangle to sample ONE fixed
 * texture, captured fresh each frame from the first textured triangle (so it is
 * always a currently-loaded, valid texture) and routed through the simple
 * full-page path. If the whole world then shows that single texture cleanly and
 * stably, the base path -- decode, atlas write, GPU sample, cache coherency --
 * is proven sound and the corruption lives entirely in the dynamic
 * per-triangle cache/eviction/sub-page machinery. If it's still garbled, the
 * base path itself is broken. s_forcedTexValid is reset once per frame in
 * NativeRenderer_SwapWindow. Toggle off to restore normal rendering.
 */
/* Cost of the 64-frame bottom-screen stats block, previous window, in us.
 * See the timer at the top of that block. Prefixed s_stats*
 * per the unity-TU rule (two same-named file-scope statics in different
 * included .c files silently merge into one object). */
global_variable unsigned long s_statsBlockUs = 0;

int g_dbg_forceSingleTexture = 0;
global_variable s16 s_forcedPage = 0;
global_variable s16 s_forcedClut = 0;
global_variable TexFormat s_forcedFormat = TF_4_BIT;
global_variable int s_forcedTexValid = 0;

/*
 * Diagnostic: route EVERY textured triangle through the full-page
 * tier, bypassing the sub-page tier entirely. The forced-single-texture test
 * proved the base decode/atlas/sample/coherency path renders one texture
 * cleanly across all geometry -- but it only ever exercised the full-page path,
 * while normal rendering sends ~92% of triangles through sub-page. This isolates
 * whether the sub-page routing/sub-cell-remap machinery is the corruption source.
 * Paired with a raised N3DS_FULLPAGE_RESERVED_CELLS so the now-100%-full-page
 * traffic doesn't evict and confound the result.
 */
int g_dbg_disableSubPage = 0;

/* Diagnostic: 0 = off; k = draw atlas k-1 full-screen
 * instead of the game (see NativeRenderer_EndScene). Cycled by dumpreq
 * 'atlasview'. Compares the GPU's live view of atlas memory against the
 * capture set's CPU-side atlas dumps. */
int g_dbg_atlasView = 0;

/*
 * Per-triangle resolve memo. The triscopes hardware decomposition put
 * draw_triangles_resolve at ~25-28ms of a ~59-70ms Labs-class frame -- ~90%
 * of the whole draw stage -- while convert/append/flush never charted.
 * Consecutive triangles overwhelmingly ask the same question (same page,
 * clut, format, view, and sub-page tile), so one remembered answer short-
 * circuits the hash-probe chain that costs ~2700 ARM11 cycles a call.
 *
 * Correctness: the memo is exactly as fresh as the decode caches' own
 * invariants. Guard stamp = evictions(sum) + s_vramDirtyBumps; any eviction
 * re-keys cells (coords may now belong to other content) and any VRAM write
 * bumps cell generations (FindOrDecode would re-decode where the memo would
 * not) -- either bumps the stamp and kills the memo. VRAM mutators fence the
 * render worker before writing (see NativeRenderer_UpdateVRAM), so the stamp
 * cannot move mid-walk under the memo's feet. LRU impact: lastUsedFrame is
 * frame-granular, so skipping repeat touches within a frame changes nothing.
 * Stats impact: tier hit counters advance on memo hits (routing counters
 * stay honest); the caches' own hit counters undercount by design.
 */

/* PROBE: ablate the per-triangle fb-mirror overlap test inside
 * N3DS_ResolveTriangleTexture. Platform_EndScene re-arms s_fbMirrorStale
 * every frame (native_platform.c StoreFrameBuffer call), so the footprint
 * test RUNS on every textured triangle of every frame even though it only
 * FIRES for freeze-frame consumers (pause background, screen-copy wipes).
 * The earlier memo A/B bounded the resolve floor at ~7-8us/call with the hash
 * probe itself worth only ~1us -- this test plus its function call is one
 * of the remaining preamble suspects. Toggle ON = skip the test entirely:
 * pause backgrounds/wipes may show stale frames while ON (instrument, not
 * a shipping mode). Runtime A/B: dumpreq 'fbtest' (dumpreq-only, no chord).
 * If the A/B pays, the shippable form is arm-time invalidation
 * of display-region cache entries (once per frame) instead of a per-
 * triangle test. */
int g_dbg_skipFbTriTest = 0;
global_variable int s_rmemoValid = 0;
global_variable u8 s_rmemoTier; /* 1 = sub-page, 2 = full-page */
global_variable s16 s_rmemoPage, s_rmemoClut;
global_variable u8 s_rmemoFormat, s_rmemoView;
global_variable s16 s_rmemoTileX, s_rmemoTileY;
global_variable int s_rmemoAtlas, s_rmemoPbU, s_rmemoPbV;
global_variable int s_rmemoCell;
global_variable u32 s_rmemoStamp;
/* The decode cell the last N3DS_ResolveTriangleTexture answered from: tier 1
 * = sub-page, 2 = full-page, 0 = the framebuffer-capture route (no cell). */
global_variable int s_resolveLastTier;
global_variable int s_resolveLastCell;
global_variable u32 s_resolveMemoHits = 0;

/* ABLATION A: boot-splash present submits clear +
 * display transfer but no draw calls. Used to prove the real-hardware boot
 * hang lived in the draw path, not clear/transfer -- which led to the
 * partial-output-write fix in shaders/ctr.v.pica (outtc0.zw). Keep at 0. */
int g_dbg_skipSplashDraws = 0;

/*
 * Per-primitive perf scopes: every NativePerf scope
 * boundary is an svcGetSystemTick SYSCALL (sdl3_compat_3ds.c ->
 * N3DS_GetTickCount), and the DrawTriangles-path scopes run per CALL (up to
 * thousands/frame), per TRIANGLE (RESOLVE/CONVERT: 4 reads x ~2600 triangles
 * in-race) and per RUN (APPEND/FLUSH) -- order 10k syscalls/frame of pure
 * instrumentation tax, paid in every measured build to date, and the reason
 * Azahar's draw_triangles_resolve ms never tracked hardware (emulated guest
 * syscalls inflate far worse than native ones). Default OFF. Per-frame
 * buckets (BEGIN_SCENE/FRAME_END/SWAP/GFX_PRESENT) and every event counter
 * stay live either way; toggle via dumpreq 'triscopes' when the
 * resolve/convert split is actually being read -- windows carrying the split
 * in their bucket ranking are self-labelling as taxed.
 */
int g_dbg_perfTriScopes = 0;

global_variable const GrVertex *s_pendingVertices = NULL;
global_variable int s_pendingVertexCount = 0;

/*
 * VRAM is 1024x512 = 16x8 64x64 dirty-generation cells (shrunk from the
 * original 4x2 256x256 page-sized grid -- design doc
 * section 1 still describes the coarse version). With only 8 cells
 * covering all of VRAM, any write anywhere in an eighth of it staled
 * every decoded page sourced from that eighth, unrelated or not; race
 * content measured stale=6418 (+272/w) against that grid. This is a
 * diagnostic experiment to see how much of that is genuine per-texture
 * animation vs. collateral invalidation from cells coarser than the
 * pages decoded from them. A decode cache entry records the cell
 * generation(s) it was decoded against and is stale if any has since moved.
 */
/*
 * VRAM dirty-tracking grid. 64 wide, but only 8 TALL -- the two axes are
 * not symmetric and one of them is load-bearing.
 *
 * CTR's display buffers are 512x216 at y=0 and y=296 (MainMain.c's
 * SetDefDrawEnv/SetDefDispEnv), leaving the strip y=216..295 free, and that is
 * exactly where the HUD's sprite sheets and their CLUTs live. With 64-tall
 * cells, row 3 covered y=192..255 -- straddling the bottom of display buffer A
 * AND the top of the free strip -- so every frame the picture changed, cell
 * row 3's generation moved and every HUD texture sourced from the free strip
 * was declared stale and re-decoded. 216 is a multiple of 8, so an 8-tall grid
 * puts the last framebuffer row (215) in cell row 26 and the first free row
 * (216) in cell row 27, and the false sharing disappears by construction.
 *
 * Cost is 1024 u32s instead of 128, and N3DS_DirtyVRAMRect bumping ~8x more
 * cells per write -- both trivial against one 65536-texel page decode.
 */
#define N3DS_VRAM_CELL_COLS  16
#define N3DS_VRAM_CELL_ROWS  64
#define N3DS_VRAM_CELL_DIM   64 /* X only; kept for the atlas-coincidence note below */
#define N3DS_VRAM_CELL_DIM_X 64
#define N3DS_VRAM_CELL_DIM_Y 8
global_variable u32 s_vramCellGen[N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS];

/*
 * Times N3DS_DirtyVRAMRect actually bumped a generation. Read with the
 * capacity/stale miss split below: if this stays ~0 then stale misses are
 * impossible by construction, and any thrashing is purely a capacity problem.
 */
global_variable u32 s_vramDirtyBumps = 0;

internal int N3DS_VRAMCellIndex(int x, int y)
{
	int col = x / N3DS_VRAM_CELL_DIM_X;
	int row = y / N3DS_VRAM_CELL_DIM_Y;

	if (col < 0) { col = 0; } else if (col >= N3DS_VRAM_CELL_COLS) { col = N3DS_VRAM_CELL_COLS - 1; }
	if (row < 0) { row = 0; } else if (row >= N3DS_VRAM_CELL_ROWS) { row = N3DS_VRAM_CELL_ROWS - 1; }

	return row * N3DS_VRAM_CELL_COLS + col;
}

/*
 * Bumps the generation of every cell touched by a [x, x+w) x [y, y+h) VRAM
 * write, so any decode cache entry sourced from that region re-decodes on
 * next use. Called from the only two currently-real VRAM mutators
 * (ClearVRAM, CopyVRAM) -- UpdateVRAM/framebuffer readback are still stubs,
 * so they cannot dirty anything yet; wire this in alongside them when they
 * become real (design doc section 5).
 */
internal void N3DS_DirtyVRAMRect(int x, int y, int w, int h)
{
	int x1, y1, cx, cy;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	x1 = x + w - 1;
	y1 = y + h - 1;

	for (cy = y / N3DS_VRAM_CELL_DIM_Y; cy <= y1 / N3DS_VRAM_CELL_DIM_Y; cy++)
	{
		for (cx = x / N3DS_VRAM_CELL_DIM_X; cx <= x1 / N3DS_VRAM_CELL_DIM_X; cx++)
		{
			s_vramCellGen[N3DS_VRAMCellIndex(cx * N3DS_VRAM_CELL_DIM_X, cy * N3DS_VRAM_CELL_DIM_Y)]++;
			s_vramDirtyBumps++;
		}
	}
}

/*
 * Deferred mirror fills. NativeRenderer_ClearVRAM records a fill here and
 * writes it into the mirror only when something reads that region or writes
 * part of it. CAM_ClearScreen's backdrop FILL covers the display every frame
 * and nothing reads the display region back during a race, so writing it each
 * frame cost about 1.4 ms at Old 3DS speed for pixels nothing reads. The fill
 * still bumps the cell generations when it is recorded, so the decode cache
 * sees the change at the same moment as before.
 *
 * Rules that keep this exact:
 *  - Pending fills are pairwise disjoint: a new fill trims the parts of older
 *    ones it covers (the overwritten pixels are dead), so settling order
 *    never matters.
 *  - Every mirror READ settles its footprint first (N3DS_VramSettle):
 *    CLUT hash, page decode, display presentation, ReadVRAM, CopyVRAM's
 *    source, state capture.
 *  - Every mirror WRITE other than a fill settles its footprint first, so it
 *    lands on top of the older fill: CopyVRAM's destination, the framebuffer
 *    refresh. Whole-mirror overwrites (init, state restore) drop the list.
 *  - The debug VRAM dump reads the mirror without settling, so it can show a
 *    pending region's previous content.
 */
#define N3DS_VRAM_PENDING_MAX 8

typedef struct
{
	s16 x, y, w, h;
	u16 colour;
} N3DS_PendingFill;

global_variable N3DS_PendingFill s_vramPending[N3DS_VRAM_PENDING_MAX];
global_variable int s_vramPendingCount;

internal void N3DS_FillVRAMRectNow(int x, int y, int w, int h, u16 colour)
{
	int row;

	for (row = 0; row < h; row++)
	{
		u16 *dst = vram + x + ((y + row) * VRAM_WIDTH);
		int col;

		for (col = 0; col < w; col++)
		{
			dst[col] = colour;
		}
	}
}

internal int N3DS_PendingOverlaps(const N3DS_PendingFill *e, int x, int y, int w, int h)
{
	return (e->x < (x + w)) && (x < (e->x + e->w)) && (e->y < (y + h)) && (y < (e->y + e->h));
}

/* Write and drop every pending fill overlapping [x, x+w) x [y, y+h). */
internal void N3DS_VramSettle(int x, int y, int w, int h)
{
	int i = 0;

	while (i < s_vramPendingCount)
	{
		const N3DS_PendingFill *e = &s_vramPending[i];

		if (N3DS_PendingOverlaps(e, x, y, w, h))
		{
			N3DS_FillVRAMRectNow(e->x, e->y, e->w, e->h, e->colour);
			s_vramPending[i] = s_vramPending[s_vramPendingCount - 1];
			s_vramPendingCount--;
		}
		else
		{
			i++;
		}
	}
}

/* Same, for a footprint whose columns wrap at the 1024-wide VRAM row. */
internal void N3DS_VramSettleWrapped(int x, int y, int w, int h)
{
	if (s_vramPendingCount == 0)
	{
		return;
	}
	x &= (VRAM_WIDTH - 1);
	if (w >= VRAM_WIDTH)
	{
		N3DS_VramSettle(0, y, VRAM_WIDTH, h);
	}
	else if ((x + w) > VRAM_WIDTH)
	{
		N3DS_VramSettle(x, y, VRAM_WIDTH - x, h);
		N3DS_VramSettle(0, y, (x + w) - VRAM_WIDTH, h);
	}
	else
	{
		N3DS_VramSettle(x, y, w, h);
	}
}

internal void N3DS_VramSettleAll(void)
{
	int i;

	for (i = 0; i < s_vramPendingCount; i++)
	{
		const N3DS_PendingFill *e = &s_vramPending[i];

		N3DS_FillVRAMRectNow(e->x, e->y, e->w, e->h, e->colour);
	}
	s_vramPendingCount = 0;
}

/*
 * Record a fill. Each older pending fill it overlaps keeps only the part
 * outside the new rect when that part is one or two full bands (the new rect
 * spans the old one's whole width, or whole height); otherwise the old fill is
 * written now. A list that would overflow writes its oldest kept entries.
 */
internal void N3DS_VramDeferFill(int x, int y, int w, int h, u16 colour)
{
	N3DS_PendingFill kept[N3DS_VRAM_PENDING_MAX * 2];
	int keptCount = 0;
	int i;

	for (i = 0; i < s_vramPendingCount; i++)
	{
		const N3DS_PendingFill e = s_vramPending[i];

		if (!N3DS_PendingOverlaps(&e, x, y, w, h))
		{
			kept[keptCount++] = e;
		}
		else if ((x <= e.x) && ((x + w) >= (e.x + e.w)))
		{
			if (y > e.y)
			{
				kept[keptCount] = e;
				kept[keptCount].h = (s16)(y - e.y);
				keptCount++;
			}
			if ((y + h) < (e.y + e.h))
			{
				kept[keptCount] = e;
				kept[keptCount].y = (s16)(y + h);
				kept[keptCount].h = (s16)((e.y + e.h) - (y + h));
				keptCount++;
			}
		}
		else if ((y <= e.y) && ((y + h) >= (e.y + e.h)))
		{
			if (x > e.x)
			{
				kept[keptCount] = e;
				kept[keptCount].w = (s16)(x - e.x);
				keptCount++;
			}
			if ((x + w) < (e.x + e.w))
			{
				kept[keptCount] = e;
				kept[keptCount].x = (s16)(x + w);
				kept[keptCount].w = (s16)((e.x + e.w) - (x + w));
				keptCount++;
			}
		}
		else
		{
			N3DS_FillVRAMRectNow(e.x, e.y, e.w, e.h, e.colour);
		}
	}

	i = 0;
	while ((keptCount - i) > (N3DS_VRAM_PENDING_MAX - 1))
	{
		N3DS_FillVRAMRectNow(kept[i].x, kept[i].y, kept[i].w, kept[i].h, kept[i].colour);
		i++;
	}
	s_vramPendingCount = keptCount - i;
	memcpy(s_vramPending, &kept[i], (size_t)s_vramPendingCount * sizeof(kept[0]));

	s_vramPending[s_vramPendingCount].x = (s16)x;
	s_vramPending[s_vramPendingCount].y = (s16)y;
	s_vramPending[s_vramPendingCount].w = (s16)w;
	s_vramPending[s_vramPendingCount].h = (s16)h;
	s_vramPending[s_vramPendingCount].colour = colour;
	s_vramPendingCount++;
}

/*
 * Lazy framebuffer-region mirror refresh.
 *
 * PC refreshes the mirror's framebuffer region from the rendered frame every
 * Platform_EndScene (StoreFrameBuffer -> readback -> pack, the FBREAD writer).
 * Doing that per frame here would cost a multi-ms readback+pack every frame,
 * so this is pull-based instead: StoreFrameBuffer (called unconditionally
 * every EndScene) only records the display rect and arms a stale flag, and
 * the consumers that actually READ the region -- the decode cache resolving
 * a page inside it, StoreImage's ReadVRAM, MoveImage's VRAM-source copy --
 * trigger one refresh before their read. Frames where nothing samples the
 * framebuffer region (nearly all of them) pay two integer stores.
 *
 * The refresh source is the last presented top-screen frame (see
 * N3DS_CopyDisplayedTopFrame), nearest-resampled to the armed rect and packed
 * to PSX 1555 with bit15 clear -- PC packs the GL backbuffer's alpha (the
 * PS1 mask bit) there, but the pause/wipe consumers draw the result as plain
 * opaque textures, so the mask bit carries nothing for this path.
 *
 * TWO armed slots, not one (hardware finding): CTR double-buffers
 * the display, so EndScene's arm alternates between the two buffers' rects
 * (y=0 / y=296) -- and the pause BG samples the buffer armed the frame
 * BEFORE the most recent arm. A single most-recent rect therefore misses
 * every pause-era read (measured: a real hardware pause produced ZERO
 * refreshes and an amber stale-mirror background). Arms update the slot
 * with a matching rect anchor, or evict the older slot for a new rect;
 * consumers overlap-test both slots and refresh each stale one they touch.
 *
 * Pause loops re-sample the region every frame while re-arming it every
 * frame, so the refreshed content feeds back through the renderer (as it
 * does on PS1, where the region IS the framebuffer). Two properties keep
 * that loop stable: atlas sampling and this resample are both NEAREST (a
 * pure pixel re-selection, no progressive bilinear smear), and the pause
 * menu is redrawn on top each frame exactly as on PS1. Verify on hardware;
 * if column drift ever shows up in a long pause, the follow-up is
 * freeze-until-rect-changes, not bilinear.
 */
typedef struct
{
	int x, y, w, h;
	int stale;
} N3DS_FBStoreSlot;

global_variable N3DS_FBStoreSlot s_fbStore[2];
/* Which slot the most recent arm landed in; a new, third distinct rect
 * evicts the OTHER (older) slot. */
global_variable int s_fbStoreLastArmed = 0;
/* Any-slot gate so the per-triangle decode path pays one load when nothing
 * is armed. Kept in sync by arm/refresh/discard below. */
global_variable int s_fbMirrorStale = 0;
/* Refresh count, surfaced via a rate-limited log line (below): expected 0
 * during normal play, small during pauses/wipes. A large steady number means
 * some race content samples the display region every frame and this path
 * needs revisiting. */
global_variable u32 s_fbMirrorRefreshes = 0;

#define N3DS_FB_COPY_MAX_W 640
#define N3DS_FB_COPY_MAX_H 256
global_variable u8 s_fbCopyScratch[N3DS_FB_COPY_MAX_W * N3DS_FB_COPY_MAX_H * 3];

internal void N3DS_RefreshFramebufferMirrorSlot(N3DS_FBStoreSlot *slot)
{
	int x = slot->x, y = slot->y, w = slot->w, h = slot->h;
	int row, col;

	/* Cleared even if the copy fails below: StoreFrameBuffer re-arms every
	 * EndScene, so a failed attempt retries next frame instead of looping
	 * on every consumer this frame. */
	slot->stale = 0;
	s_fbMirrorStale = s_fbStore[0].stale || s_fbStore[1].stale;

	if (w > N3DS_FB_COPY_MAX_W) { w = N3DS_FB_COPY_MAX_W; }
	if (h > N3DS_FB_COPY_MAX_H) { h = N3DS_FB_COPY_MAX_H; }
	if (w > (VRAM_WIDTH - x)) { w = VRAM_WIDTH - x; }
	if (h > (VRAM_HEIGHT - y)) { h = VRAM_HEIGHT - y; }
	if ((x < 0) || (y < 0) || (w <= 0) || (h <= 0))
	{
		return;
	}

	if (!N3DS_CopyDisplayedTopFrame(s_fbCopyScratch, w, h))
	{
		return;
	}

	N3DS_VramSettle(x, y, w, h);

	/*
	 * Change-detecting write: only cells whose content actually moved get
	 * their generation bumped. This is load-bearing, not an optimization
	 * nicety (measured in Azahar): menu/idle content samples the
	 * display region EVERY frame (the same screen-feedback blur PC's
	 * per-frame FBREAD exists to serve), so this refresh runs per frame
	 * there -- a blanket N3DS_DirtyVRAMRect(x,y,w,h) then re-decoded ~32
	 * cells' worth of cache every frame and cost several fps. A static
	 * screen converges after one refresh (zero bumps, zero re-decodes);
	 * FMV-style content keeps changing and pays the honest re-decode cost.
	 */
	{
		u32 changedMask[N3DS_VRAM_CELL_ROWS];
		int cellRow, cellCol;

		memset(changedMask, 0, sizeof(changedMask));

		for (row = 0; row < h; row++)
		{
			const u8 *srcRow = s_fbCopyScratch + ((size_t)row * (size_t)w * 3);
			u16 *dstRow = vram + ((size_t)(y + row) * VRAM_WIDTH) + x;
			u32 *rowMask = &changedMask[(y + row) / N3DS_VRAM_CELL_DIM_Y];

			for (col = 0; col < w; col++)
			{
				const u8 *p = srcRow + (col * 3);
				const u16 packed = (u16)((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));

				if (dstRow[col] != packed)
				{
					dstRow[col] = packed;
					*rowMask |= (1u << ((u32)(x + col) / N3DS_VRAM_CELL_DIM_X));
				}
			}
		}

		for (cellRow = 0; cellRow < N3DS_VRAM_CELL_ROWS; cellRow++)
		{
			for (cellCol = 0; cellCol < N3DS_VRAM_CELL_COLS; cellCol++)
			{
				if (changedMask[cellRow] & (1u << (u32)cellCol))
				{
					s_vramCellGen[(cellRow * N3DS_VRAM_CELL_COLS) + cellCol]++;
					s_vramDirtyBumps++;
				}
			}
		}
	}

	s_fbMirrorRefreshes++;

	/* First refresh and every 32nd after: enough to prove the path fired
	 * (and which rect) without an SD-card flush per pause frame. */
	if ((s_fbMirrorRefreshes & 31u) == 1u)
	{
		char msg[96];

		snprintf(msg, sizeof(msg), "[CTR-3DS] fb-mirror refresh #%lu rect=%d,%d %dx%d", (unsigned long)s_fbMirrorRefreshes, x, y, w, h);
		N3DS_DebugLog(msg);
	}
}

/* Overlap-test the read rect against BOTH armed slots and refresh each stale
 * slot it touches (see the two-slot rationale in the block comment above).
 * All three freeze-frame consumers funnel through here. */
internal void N3DS_FBRefreshIfOverlaps(int x, int y, int w, int h)
{
	int i;

	for (i = 0; i < 2; i++)
	{
		N3DS_FBStoreSlot *slot = &s_fbStore[i];

		if (slot->stale && (x < (slot->x + slot->w)) && ((x + w) > slot->x) && (y < (slot->y + slot->h)) && ((y + h) > slot->y))
		{
			N3DS_RefreshFramebufferMirrorSlot(slot);
		}
	}
}

/*
 * Which per-texel visibility predicate a decoded cell was built with -- the
 * per-texel STP mechanism. RGBA5551 has ONE alpha bit, but PS1
 * semi-transparent textured prims need THREE distinct texel-visibility views
 * (PC discriminates them in its shader, discardForSemiTransPass -- the
 * predicates below are that function's, verbatim):
 *
 *   OPAQUE: ABE=0 draws. Every non-zero texel visible; 0x0000 punch-through.
 *   PASS1:  semi-trans pass 1 (drawn with blending OFF). Only non-STP texels
 *           visible -- the opaque body of mixed-texel elements (wooden track
 *           arrows, tires, character portraits).
 *   PASS2:  semi-trans pass 2 (blended). Only STP-bit texels visible
 *           (0x8000 semi-transparent black included, exactly as PS1).
 *
 * PASS1|PASS2 partition OPAQUE, so one prim drawn twice composes the full
 * PS1 result. The view is part of the cache key: a texture drawn only one
 * way materializes one cell (the overwhelmingly common case); mixed usage
 * lazily materializes what its draws actually request. Chosen over RGBA4444
 * (a global 5->4-bit color loss to fix a small element set) and RGBA8
 * (doubles atlas memory, doesn't fit).
 */
typedef enum
{
	N3DS_TEXVIEW_OPAQUE = 0,
	N3DS_TEXVIEW_PASS1  = 1,
	N3DS_TEXVIEW_PASS2  = 2,
} N3DS_TexView;

typedef struct
{
	int used;         /* 0 = empty cell, never decoded into */
	s16 page;
	s16 clut;
	TexFormat format;
	u8 view;           /* N3DS_TexView the cell was decoded under (cache key) */
	u32 pageGen;
	u32 clutGenA;
	u32 clutGenB;      /* == clutGenA when the CLUT doesn't straddle two VRAM cells */
	u32 clutHash;      /* FNV-1a of the CLUT halfwords this cell decoded with (0 for
	                    * CLUT-less formats) -- the variant-retarget key, see
	                    * N3DS_HashClutContent */
	u32 lastUsedFrame;
	u32 serial;        /* s_cellDecodeSerial at the cell's last decode: changes
	                    * whenever its pixels or key do (the model texture
	                    * table keys on it) */
} N3DS_DecodeCacheEntry;

/*
 * The decode cache is addressed as a flat array of atlas cells (switched
 * from one dedicated texture per entry to atlas cells -- see the
 * header comment on N3DS_C3DUploadDecodedCell in n3ds_backend.h for why).
 * Flat index 0 is reserved for a permanent flat opaque-white texel
 * (untextured/Gouraud primitives -- N3DS_WHITE_ATLAS/CELL_X/CELL_Y below),
 * never entered into the LRU cache; indices 1..N3DS_TOTAL_CELLS-1 are real,
 * split between two tiers as of the sub-page cache below.
 */
#define N3DS_TOTAL_CELLS (N3DS_ATLAS_COUNT * N3DS_ATLAS_CELLS_PER_ATLAS)
#define N3DS_WHITE_ATLAS  0
#define N3DS_WHITE_CELL_X 0
#define N3DS_WHITE_CELL_Y 0

/*
 * Sub-page decode cache: a static split
 * of the same N3DS_TOTAL_CELLS physical atlas cells, not a separate memory
 * budget. Cell 0 stays the reserved white cell; cells 1..N3DS_FULLPAGE_RESERVED_CELLS
 * back the whole-page tier below (unchanged logic, just a smaller array); the
 * remaining physical cells back a new sub-page tier, each physical cell
 * further subdivided into a 4x4 grid of 64x64 sub-cells.
 *
 * 96 is a corrected first-pass number, not yet a fully measured one -- the
 * design doc's original guess of 32 was shipped, boot-tested, and measured
 * WRONG the same day: a menu background scene (hills/road/tunnel/sky) alone
 * peaked at 25/32 full-page cells with evictInUse=11846, i.e. severe
 * mid-frame-eviction-hazard pressure -- the exact mechanism tied
 * directly to visible corruption. Meanwhile the sub-page tier peaked at only
 * ~86/1520 slots across the same content, so it had enormous unused headroom
 * to give back. Growing this further necessarily shrinks the sub-page tier
 * (both draw from the same fixed 127-cell pool, see the design doc's
 * capacity-tension note) -- 96/31 leaves the sub-page tier 31*16=496 slots,
 * still ~6x its observed peak. Race content (the scenario the design doc
 * predicted would stress this tier hardest) has not yet been measured against
 * either number.
 */
#define N3DS_FULLPAGE_RESERVED_CELLS    96
/*
 * The LAST N3DS_FBCAP_RESERVED_CELLS physical cells are carved out of the
 * sub-page tier for the framebuffer-capture region (N3DS_FBCaptureEnsure
 * below): two horizontally adjacent cells forming one contiguous 512x256
 * texel rect that holds the last presented top-screen frame. Content whose
 * sampled footprint lies inside an armed display rect (heat-warp rings,
 * pause background, menu feedback) resolves straight into this region
 * instead of round-tripping through the VRAM mirror + decode cache -- one
 * capture per frame replaces per-page mirror refreshes, dirty-generation
 * bumps and the full-page decodes that evict the live working set.
 */
#define N3DS_FBCAP_RESERVED_CELLS       2
#define N3DS_SUBPAGE_PHYSICAL_CELLS     (N3DS_TOTAL_CELLS - 1 - N3DS_FULLPAGE_RESERVED_CELLS - N3DS_FBCAP_RESERVED_CELLS)
#define N3DS_SUBPAGE_CELLS_PER_PHYSICAL 16
#define N3DS_SUBPAGE_TOTAL_CELLS        (N3DS_SUBPAGE_PHYSICAL_CELLS * N3DS_SUBPAGE_CELLS_PER_PHYSICAL)
#define N3DS_SUBPAGE_DIM                64
#define N3DS_SUBPAGE_PHYSICAL_BASE      (1 + N3DS_FULLPAGE_RESERVED_CELLS)

#define N3DS_FBCAP_CELL_FIRST (N3DS_TOTAL_CELLS - N3DS_FBCAP_RESERVED_CELLS)
#define N3DS_FBCAP_ATLAS      (N3DS_FBCAP_CELL_FIRST / N3DS_ATLAS_CELLS_PER_ATLAS)
#define N3DS_FBCAP_CELL_X     ((N3DS_FBCAP_CELL_FIRST % N3DS_ATLAS_CELLS_PER_ATLAS) % N3DS_ATLAS_CELLS_PER_ROW)
#define N3DS_FBCAP_CELL_Y     ((N3DS_FBCAP_CELL_FIRST % N3DS_ATLAS_CELLS_PER_ATLAS) / N3DS_ATLAS_CELLS_PER_ROW)
/* Atlas-texel origin of the capture region; the reserved cells must be
 * horizontally adjacent in ONE atlas for the region to be contiguous. */
#define N3DS_FBCAP_REGION_X   (N3DS_FBCAP_CELL_X * N3DS_DECODE_TEXTURE_DIM)
#define N3DS_FBCAP_REGION_Y   (N3DS_FBCAP_CELL_Y * N3DS_DECODE_TEXTURE_DIM)
CTR_STATIC_ASSERT((N3DS_FBCAP_CELL_X + N3DS_FBCAP_RESERVED_CELLS) <= N3DS_ATLAS_CELLS_PER_ROW);

/* Sized for every physical cell, not just the full-page partition: with
 * g_dbg_disableSubPage the full-page tier scans the idle sub-tier cells too
 * (N3DS_FullPageCellBound below), so the A/B isn't confounded by
 * eviction thrash -- 96 cells hold less than one busy scene's working set,
 * which is exactly what invalidated the first run of that experiment. */
global_variable N3DS_DecodeCacheEntry s_decodeCache[N3DS_TOTAL_CELLS];

internal int N3DS_FullPageCellBound(void)
{
	/* The capture cells stay reserved even under the disable-sub-page
	 * diagnostic -- the full-page tier must never allocate them. */
	return g_dbg_disableSubPage ? (N3DS_TOTAL_CELLS - N3DS_FBCAP_RESERVED_CELLS) : (1 + N3DS_FULLPAGE_RESERVED_CELLS);
}

/* Flat cell index -> which atlas texture, and which cell within its grid. */
internal void N3DS_CellIndexToAtlasCoord(int cellIndex, int *outAtlas, int *outCellX, int *outCellY)
{
	const int within = cellIndex % N3DS_ATLAS_CELLS_PER_ATLAS;

	*outAtlas = cellIndex / N3DS_ATLAS_CELLS_PER_ATLAS;
	*outCellX = within % N3DS_ATLAS_CELLS_PER_ROW;
	*outCellY = within / N3DS_ATLAS_CELLS_PER_ROW;
}

/*
 * Sub-page cache entry -- same shape as N3DS_DecodeCacheEntry plus tileX/tileY,
 * the key's sub-page component (which 64x64 tile of the page, 0-3 each).
 * pageGen is the generation of the ONE VRAM dirty-cell exactly covering this
 * tile (a 64-aligned sub-tile's absolute VRAM origin is always exactly one
 * N3DS_VRAM_CELL_DIM cell -- see the design doc's "useful coincidence" note);
 * clutGenA/B are the identical two-cell check the full-page tier uses (same
 * pre-existing straddle gap, inherited verbatim, not fixed here).
 */
typedef struct
{
	int used;
	s16 page;
	s16 clut;
	TexFormat format;
	u8 view;          /* N3DS_TexView, same key role as the full-page tier */
	u8 tileX, tileY;
	u32 pageGen;
	u32 clutGenA, clutGenB;
	u32 clutHash;     /* same variant-retarget role as the full-page tier's field */
	u32 lastUsedFrame;
	u32 serial;       /* same role as the full-page tier's field */
} N3DS_SubPageCacheEntry;

/* Bumped by every decode into a cell of either tier. */
global_variable u32 s_cellDecodeSerial = 0;

global_variable N3DS_SubPageCacheEntry s_subPageCache[N3DS_SUBPAGE_TOTAL_CELLS];
/* Mirrors s_lastCellHit. -1 = no memo yet -- unlike the full-page array, index
 * 0 is a real, valid sub-page slot here (no reserved white cell at 0), so 0
 * can't double as the "no memo" sentinel the way it implicitly does not for
 * s_lastCellHit either (that one guards with `> 0`, this one with `>= 0`). */
global_variable int s_lastSubPageCellHit = -1;

/*
 * Hash index over each tier's entry array. Hardware measured
 * resolve at ~36us/triangle -- ~85% of DrawTriangles and ~half the frame --
 * and nearly all of it is the linear key scans below: the one-entry memo
 * only catches same-texture runs, and race content alternates textures and
 * sub-page tiles constantly. Each table maps key-hash -> the cell LAST KNOWN
 * to hold that key. A probe re-verifies the FULL key + freshness before
 * trusting the bucket, so a stale bucket (evicted cell, colliding key) is
 * never wrong, only slow: it falls through to the linear scan, which repairs
 * the bucket on any fresh find or new decode. The entry arrays stay the only
 * ground truth; the index needs no invalidation anywhere -- eviction,
 * VRAM dirtying, and NativeRenderer3DS_DebugInvalidateCaches all leave
 * buckets pointing at entries the probe's own checks reject.
 *
 * Sizes: load factor <= ~0.5 against each tier's live-entry bound
 * (97 full-page entries / 256 buckets, 1520 sub-page / 4096). s16 buckets,
 * -1 = never written (only meaningful for the sub-page table, where cell 0
 * is a real slot; the full-page probe also rejects cand 0 = white cell).
 */
#define N3DS_FULLPAGE_HASH_SIZE 256
#define N3DS_SUBPAGE_HASH_SIZE  4096

/*
 * Atlas allocation affinity. Race content keeps all 12 atlases
 * live with ~190 mid-run atlas-switch flushes/frame (hardware) -- every
 * switch is a draw-call boundary, and PICA per-draw overhead is the
 * frame-end wait's main feeder. When a miss must pick a victim cell, prefer
 * one in the atlas the CURRENT pending run is bound to: the just-decoded
 * texture then extends the open run instead of breaking it (an immediately
 * saved switch), and over frames co-drawn content migrates into shared
 * atlases. The preference only fires when the same-atlas victim hasn't been
 * used for at least this many frames -- affinity must never evict the live
 * working set (that is thrash, which costs far more than a switch saves).
 */
#define N3DS_ATLAS_AFFINITY_MIN_AGE 2
/* Placements the preference actually redirected (vs the global-LRU pick),
 * per tier -- proves the mechanism fires without a hardware A/B. */
global_variable u32 s_fullPageAffinityPlacements = 0;
global_variable u32 s_subPageAffinityPlacements = 0;
global_variable s16 s_fullPageHashIdx[N3DS_FULLPAGE_HASH_SIZE];
global_variable s16 s_subPageHashIdx[N3DS_SUBPAGE_HASH_SIZE];
/* Probe outcomes, windowed in the stats block: probe hits should carry the
 * overwhelming share of lookups once warm; scan fallbacks that FIND a fresh
 * entry (s_*HashRepairs) mean bucket collisions/recycling, and their rate
 * bounds how much scan cost the index failed to remove. */
global_variable u32 s_fullPageHashHits = 0;
global_variable u32 s_fullPageHashRepairs = 0;
global_variable u32 s_subPageHashHits = 0;
global_variable u32 s_subPageHashRepairs = 0;
global_variable u32 s_lastStatFullPageHashHits = 0;
global_variable u32 s_lastStatSubPageHashHits = 0;

/*
 * Stale-miss attribution. Steady-state race content re-decodes
 * ~100k texels/frame per tier from staleness alone (full-page ~1.5/frame x
 * 65k texels, sub-page ~19/frame x 4k), and the CAUSE
 * picks the fix: pageGen movement = the sampled VRAM region was written
 * (real texture animation, or false sharing from an unrelated write landing
 * in the same 64x64 gen cell -- note the full-page tier keys freshness off
 * the page's ORIGIN cell only, see N3DS_FindOrDecodeTexture); clutGen
 * movement = palette animation, where every texel legitimately changes and
 * a full re-decode is unavoidable. Both can move on one miss; both bump.
 * The per-page histograms NAME the re-decoding pages (texPage & 0x1F),
 * windowed -- reset after each stats print -- so the top offender is the
 * current window's, not boot's.
 */
global_variable u32 s_fullPageStaleByPageGen = 0;
global_variable u32 s_fullPageStaleByClutGen = 0;
global_variable u32 s_subPageStaleByPageGen = 0;
global_variable u32 s_subPageStaleByClutGen = 0;
global_variable u32 s_lastStatFpStaleByPageGen = 0;
global_variable u32 s_lastStatFpStaleByClutGen = 0;
global_variable u32 s_lastStatSpStaleByPageGen = 0;
global_variable u32 s_lastStatSpStaleByClutGen = 0;
global_variable u32 s_fpStaleByPageId[32];
global_variable u32 s_spStaleByPageId[32];

internal u32 N3DS_TexKeyHash(s16 page, s16 clut, TexFormat format, N3DS_TexView view, int tileX, int tileY)
{
	/* Knuth/Murmur-style odd-constant multiplies. format/view/tile occupy
	 * distinct low nibbles BEFORE the final multiply so the multiply+shift
	 * spreads them across the whole word -- a tile bit parked above the
	 * bucket mask would otherwise make vertical neighbour tiles of one page
	 * permanent collision partners. */
	u32 h = ((u32)(u16)page) * 0x9E3779B1u;

	h ^= ((u32)(u16)clut) * 0x85EBCA77u;
	h ^= ((u32)format) ^ ((u32)view << 4) ^ ((u32)tileX << 8) ^ ((u32)tileY << 12);
	h *= 0x9E3779B1u;
	h ^= h >> 16;
	return h;
}

/*
 * CLUT-content variant caching. The staleattr counters showed
 * CLUT-generation movement driving the majority of stale re-decodes in race
 * content (sub-page ~80%, staleWindow ~14/frame; full-page ~60% of ~1.5/frame
 * x 65k texels) -- palette ANIMATION, i.e. the game cycling a small, bounded
 * set of palettes through one CLUT address every frame or two (water/glow,
 * pages 15/6 sub-page and 2/3 full-page on Crash Cove). Address+generation
 * keying makes every cycle step look like new content forever, so the same
 * few palettes re-decode the same texels indefinitely.
 *
 * Fix: each cell also remembers the FNV-1a hash of the CLUT halfwords it
 * decoded with. On a lookup whose key matches but whose CLUT generation
 * moved (page data still fresh), hash the CURRENT palette bytes and rescan:
 * a sibling cell holding this key decoded-with-identical-CLUT-content is
 * byte-identical to what a fresh decode would produce (same source indices,
 * same palette bytes, same format/view), so it is retargeted and its clut
 * generations revalidated in place -- after one full cycle every palette
 * step is a hit and the steady-state decode cost of the animation is zero.
 * The hash is computed lazily, at most once per lookup, and only on the
 * already-rare gen-stale path -- never on the hot fresh-hit paths.
 *
 * Hashing MUST mirror N3DS_DecodeAndTilePage's CLUT fetch exactly, including
 * the S-wrap within the row (GL_REPEAT parity) -- 16 entries at 4bpp, 256 at
 * 8bpp. CLUT-less formats return 0 and are additionally exempted from CLUT
 * generation checks entirely at the lookup sites (a 16bpp page has no
 * palette, so its old spurious clut-gen staleness was 100% false).
 */
internal u32 N3DS_HashClutContent(s16 clut, TexFormat format)
{
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	const int entries = (format == TF_4_BIT) ? 16 : ((format == TF_8_BIT) ? 256 : 0);
	u32 h = 0x811C9DC5u;
	int i;

	if (entries > 0)
	{
		N3DS_VramSettleWrapped(clutOriginX, clutOriginY, entries, 1);
	}

	for (i = 0; i < entries; i++)
	{
		const u16 c = vram[clutOriginY * VRAM_WIDTH + ((clutOriginX + i) & (VRAM_WIDTH - 1))];

		h = (h ^ (u32)(c & 0xFF)) * 0x01000193u;
		h = (h ^ (u32)(c >> 8)) * 0x01000193u;
	}
	return (entries == 0) ? 0u : h;
}

/* Variant retargets that avoided a re-decode, per tier -- the mechanism's
 * receipt, windowed in the staleattr lines. */
global_variable u32 s_fullPageClutVariantHits = 0;
global_variable u32 s_subPageClutVariantHits = 0;
global_variable u32 s_lastStatFpClutVariantHits = 0;
global_variable u32 s_lastStatSpClutVariantHits = 0;

/* Sub-page counterpart to N3DS_CellIndexToAtlasCoord -- reuses it unchanged,
 * offset into the physical-cell range the sub-page tier owns, then further
 * resolves which of the 16 sub-cells within that physical cell. */
internal void N3DS_SubCellIndexToAtlasCoord(int subIndex, int *outAtlas, int *outPhysCellX, int *outPhysCellY,
                                             int *outSubX, int *outSubY)
{
	const int physCellIndex = N3DS_SUBPAGE_PHYSICAL_BASE + (subIndex / N3DS_SUBPAGE_CELLS_PER_PHYSICAL);
	const int within = subIndex % N3DS_SUBPAGE_CELLS_PER_PHYSICAL;

	N3DS_CellIndexToAtlasCoord(physCellIndex, outAtlas, outPhysCellX, outPhysCellY);
	*outSubX = within % 4;
	*outSubY = within / 4;
}

/*
 * Cache diagnostics (added to investigate a real/reported-fps
 * mismatch -- SwapWindow's own fps_now is a frames-per-emulated-vblank
 * ratio, which can look fine on average even when per-frame cost is spiky;
 * a decode is only expensive on a miss, so these numbers say directly
 * whether the cache is thrashing rather than leaving it a guess).
 * s_decodeCacheEvictions counts only misses that discarded a still-valid,
 * different entry -- i.e. genuine thrashing -- as opposed to a miss that
 * just filled a never-yet-used slot (expected once, during cache warm-up).
 */
global_variable u32 s_decodeCacheHits = 0;
global_variable u32 s_decodeCacheMisses = 0;
global_variable u32 s_decodeCacheEvictions = 0;
global_variable u32 s_lastStatCacheMisses = 0;
global_variable u32 s_lastStatCacheEvictions = 0;

/*
 * WHY a miss happened, which the counters above cannot distinguish and which
 * decides the whole architecture. Menu content measures ~1151
 * misses / 1088 evictions per 32-frame window -- nearly one eviction per miss
 * -- but "the cache is too small" and "the cache keeps being invalidated" look
 * IDENTICAL in that pair, and they have opposite fixes:
 *
 *   - CAPACITY: no entry for this (page,clut,format) at all. The working set
 *     genuinely exceeds this tier's cell budget (N3DS_TOTAL_CELLS at the time
 *     this was measured; now 1+N3DS_FULLPAGE_RESERVED_CELLS for this counter
 *     specifically, after the sub-page split below). Fix = more/smaller
 *     cells, or decode a level's set once at load into persistent atlases.
 *   - STALE: an entry for exactly this (page,clut,format) IS resident, but a
 *     VRAM cell generation moved under it, so it re-decodes. Fix = dirty VRAM
 *     at finer granularity than 256x256, or stop the mutator. Decoding once at
 *     load would be WRONG here -- the source pixels really are changing.
 *
 * Decode-once-at-load is only viable if STALE is near zero. Measure before
 * committing to it: this project has already paid twice for optimising against
 * a premise that was never isolated (the atlas rework, and an earlier
 * "ruled out by measurement" list).
 */
global_variable u32 s_decodeCacheStaleMisses = 0;
global_variable u32 s_decodeCacheCapacityMisses = 0;
global_variable u32 s_lastStatStaleMisses = 0;
global_variable u32 s_lastStatCapacityMisses = 0;

/*
 * Distinct atlas cells touched in one frame -- an estimate of the live working
 * set, to compare against this tier's own cell budget (N3DS_FULLPAGE_RESERVED_CELLS
 * after the sub-page split below; was N3DS_TOTAL_CELLS before). Counted by
 * noticing an entry whose lastUsedFrame was not already this frame, so it
 * SATURATES at the cache size when capacity binds: reading the budget means
 * "at least that many", not "exactly that many". Read it together with the
 * capacity/stale split, not alone.
 */
global_variable u32 s_frameTouchedCells = 0;
global_variable u32 s_frameTouchedPeak = 0;

/*
 * Last cell that satisfied a lookup. Checked before the full scan because
 * consecutive triangles in a split overwhelmingly share one (page,clut) --
 * MakeTexcoordTriangle/Quad emit runs of primitives off the same texture --
 * so this turns the common case from a 63-entry scan into one compare.
 *
 * Measured, and this is why it exists: renderer_draw_triangles_ms
 * was 16-18ms of a 36-63ms frame (~70% of render_submit_ms, the top bucket)
 * at ~2040 triangles/frame, i.e. ~8us per triangle -- while the cache was
 * hitting 362,396 times against 1,326 misses. The cost was never decoding;
 * it was the linear scan on the way to a hit. Safe against eviction without
 * any invalidation step: the memo re-checks the full key, so a stale index
 * just falls through to the scan.
 */
global_variable int s_lastCellHit = 0;

/*
 * Misses that had to evict a cell already referenced by a draw QUEUED earlier
 * this same frame. citro3d executes nothing until C3D_FrameEnd, but the cell
 * upload is an immediate CPU memcpy, so overwriting such a cell silently
 * changes the texture an already-queued triangle will sample. Nonzero here
 * means on-screen corruption; see the victim search in FindOrDecodeTexture.
 */
global_variable u32 s_evictHazards = 0;

/*
 * Sub-page tier diagnostics -- field-for-field mirror of the full-page
 * counters above; see those comments for what each one means. lastStat*
 * variants exist only to compute the same windowed (+N/w) deltas the
 * stats block already prints for the full-page tier.
 */
global_variable u32 s_subPageCacheHits = 0;
global_variable u32 s_subPageCacheMisses = 0;
global_variable u32 s_subPageCacheEvictions = 0;
global_variable u32 s_subPageLastStatCacheMisses = 0;
global_variable u32 s_subPageLastStatCacheEvictions = 0;
global_variable u32 s_subPageCacheStaleMisses = 0;
global_variable u32 s_subPageCacheCapacityMisses = 0;
global_variable u32 s_subPageLastStatStaleMisses = 0;
global_variable u32 s_subPageLastStatCapacityMisses = 0;
global_variable u32 s_subPageFrameTouchedCells = 0;
global_variable u32 s_subPageFrameTouchedPeak = 0;
global_variable u32 s_subPageEvictHazards = 0;

/*
 * Per-triangle routing outcome (N3DS_ResolveTriangleTexture) -- the single
 * most important new number: whether the straddling/large-surface fallback
 * to the full-page tier dominates real content or is genuinely the minority
 * case this design assumes (design doc "Instrumentation").
 */
global_variable u32 s_trianglesRoutedSubPage = 0;

/* Always 0: the merge-stage tire trace that used to drive this has been
 * removed. Kept because platform/native_gpu.c reads it via extern (the
 * [PDFILL] forensics block and the FPS-overlay frame bridge in
 * NativeUiMap_FpsOverlayText). */
int g_dbg_tireVtxLog = 0;
/* Always 0, for the same reason: platform/native_gpu.c's
 * NativeUiMap_FpsOverlayText reads this via extern. */
unsigned long g_tirelogFrameNow = 0;
global_variable u32 s_trianglesRoutedFullPage = 0;

internal int N3DS_TvboCheckedAppend(const N3DS_Vertex *verts, int count)
{
	return N3DS_C3DAppendVertices(verts, count);
}

/*
 * Per-frame texture-decode attribution ([TEXDEC]). A scene cut's first draws
 * can miss the whole new cast's pages in one frame, and the window averages
 * smear that single frame away -- these split each frame's miss cost into
 * decode (CPU texel loop) vs upload, plus the whole model pass's wall time,
 * so a freeze frame is attributable from the log alone. All accumulation
 * happens on the thread that runs the draws; folded, printed past the
 * thresholds below, and reset at the top of NativeRenderer_SwapWindow on
 * that same call chain.
 */
global_variable u32 s_texdecFrameSub = 0;
global_variable u32 s_texdecFrameFull = 0;
global_variable u64 s_texdecFrameDecTicks = 0;
global_variable u64 s_texdecFrameUpTicks = 0;
global_variable u64 s_texdecFrameMdlTicks = 0;
global_variable u32 s_texdecWinSub = 0;
global_variable u32 s_texdecWinFull = 0;
global_variable u32 s_texdecWinUs = 0;
global_variable u32 s_texdecWinMaxUs = 0;
global_variable u32 s_texdecWinMaxSub = 0;
global_variable u32 s_texdecWinMaxFull = 0;
global_variable u32 s_texdecWinMaxMdlUs = 0;
global_variable u32 s_texdecWinMaxFrame = 0;
/* Worst single decode+upload this frame -- names WHICH texture is churning,
 * not just how much. Ticks (not us) so the per-decode compare stays a
 * subtraction and a compare; converted once at fold time. */
global_variable u64 s_texdecFrameWorstTicks = 0;
global_variable u16 s_texdecFrameWorstPage = 0;
global_variable u16 s_texdecFrameWorstClut = 0;
global_variable u8 s_texdecFrameWorstIsSub = 0;
global_variable s8 s_texdecFrameWorstTileX = -1;
global_variable s8 s_texdecFrameWorstTileY = -1;

/* One increment per actual C3D_DrawArrays call (N3DS_MergeEmit) -- checks
 * the design doc's own predicted "more, smaller draw calls than PC" risk
 * directly. */
global_variable u32 s_drawCallCount = 0;

/*
 * Draw-call structure counters. An early real-hardware race put the
 * frame-end GPU wait (platform_end_frame_ms,
 * 231-257ms of a ~350ms frame) on top with ~383 draws/frame, and real-PICA
 * per-draw overhead far exceeds Azahar's -- so draws/frame is the one
 * Azahar-measurable number that transfers. These split it into its
 * mechanisms: s_atlasSwitchFlushes counts run flushes forced by an ATLAS
 * CHANGE between consecutive triangles mid-split (the potentially reducible
 * kind -- cell-to-atlas allocation affinity could shrink them; submission
 * order itself must never change, layering depends on it);
 * s_distinctAtlasTotal accumulates each frame's popcount of distinct atlases
 * actually bound (the switching floor for the frame's content as allocated).
 */
global_variable u32 s_atlasSwitchFlushes = 0;
global_variable u32 s_frameAtlasMask = 0;
global_variable u32 s_distinctAtlasTotal = 0;
global_variable u32 s_frameDistinctAtlasPeak = 0;
global_variable u32 s_lastStatAtlasSwitchFlushes = 0;
global_variable u32 s_lastStatDistinctAtlasTotal = 0;
/* Reached only through DrawAllSplits, so uvb==0 means the OT walk never ran at
 * all, rather than DrawTriangles running and bailing. See the stats block. */
global_variable u32 s_updateVertexBufferCalls = 0;
global_variable u32 s_drawTrianglesCalls = 0;
/* Last DISPENV the projection was built for; -1 forces it on the first frame. */
global_variable int s_projectionDispW = -1;
global_variable int s_projectionDispH = -1;
/* Widescreen mode the projection was built for -- mode 2 (REAL) changes the
 * ortho window inside N3DS_C3DSetProjection, so a toggle must rebuild even
 * though the DISPENV didn't move. */
global_variable int s_projectionWideMode = -1;
global_variable u32 s_lastStatDrawCallCount = 0;

/* Scratch for one decoded+tiled page; reused every decode, never persisted.
 * One u16 per texel (GPU_RGBA5551) -- see N3DS_TILE_BYTES in n3ds_backend.c. */
global_variable u16 s_decodeScratch[N3DS_DECODE_TEXTURE_DIM * N3DS_DECODE_TEXTURE_DIM];

/* Scratch for one decoded+tiled 64x64 sub-cell; mirrors s_decodeScratch's
 * role, sized for N3DS_SUBPAGE_DIM instead of N3DS_DECODE_TEXTURE_DIM. */
global_variable u16 s_subpageDecodeScratch[N3DS_SUBPAGE_DIM * N3DS_SUBPAGE_DIM];

/* Per-call CLUT resolve table for N3DS_DecodeAndTilePage -- file-scope like
 * the decode scratches above (one decode runs at a time on the draw thread;
 * never persisted between calls). */
global_variable u16 s_decodeClutLut[256];

/* Scratch for one converted atlas-run. Bounded by N3DS_MAX_RUN_VERTICES,
 * NOT the whole-frame budget: a run is flushed whenever the atlas changes
 * or this fills, so it only ever stages one run at a time, and sizing it
 * per-frame would cost megabytes of BSS for space no single run can use. */
global_variable N3DS_Vertex s_convertScratch[N3DS_MAX_RUN_VERTICES];

/*
 * citro3d frame brackets actually opened/closed, versus s_frameCounter's
 * count of SwapWindow calls. These three should track each other 1:1:1.
 * They are counted because Azahar's own host-side readout said "App: 1 FPS"
 * while the in-guest counter reported ~13 -- a 13x gap that means either the
 * top screen is being presented far less often than the game believes it is
 * drawing, or the in-guest number is measuring the wrong thing. Guessing
 * which is exactly the mistake this port keeps paying for; count it instead.
 */
global_variable u32 s_beginSceneCount = 0;
global_variable u32 s_endSceneCount = 0;

/* Runs dropped because the frame's vertex budget was exhausted. This is
 * silent geometry loss -- whole draws vanish with nothing on screen to say
 * so -- which is exactly how a too-small budget hid for a session. Any
 * nonzero value here means content is missing. */
global_variable u32 s_droppedRuns = 0;

/*
 * Decodes PSX texture page `texPage` (through CLUT `clut` for 4/8-bit) into
 * a 256x256 RGBA8 buffer, applying the PICA200 8x8 Z-order (Morton) tile
 * swizzle inline -- citro3d's C3D_TexLoadImage is a raw memcpy and does not
 * tile for you (verified directly against citro3d's upstream
 * source). The intra-tile bit-interleave
 * below (x supplies the even bits, y the odd bits) was cross-checked
 * position-by-position against tex3ds's own reference swizzle table
 * (source/swizzle.cpp) rather than assumed.
 *
 * Addressing matches native_gpu.c exactly: page origin in VRAM word/pixel
 * coordinates is ((texPage & 0xF) * 64, (texPage & 0x10) ? 256 : 0) --
 * GetTPageBase/NativeGpu_TPageOverlapsActiveDrawPage agree, and the PC
 * fragment shader's v_page_clut derivation (native_renderer.c:613-616)
 * reduces to the same formula. CLUT origin is ((clut & 0x3F) * 16,
 * clut >> 6), same as the GET_CLUT_X/GET_CLUT_Y macros (native_gpu.c:43-44).
 * u/v (0-255) are native-texel coordinates within the page. A page is 256
 * TEXELS wide in every colour mode -- 8bpp spans two tpage columns of VRAM
 * words, 16bpp four. (An earlier revision claimed 8bpp could only address
 * u=0..127, conflating VRAM-word width with texel width, and under-decoded
 * every wide 8/16bpp texture -- fixed; see the decodeWidth
 * comment at the full-page call site.)
 *
 * Alpha is ONE bit (RGBA5551), so the caller picks WHICH texels get it via
 * `view` (N3DS_TexView): the per-texel STP information PC keeps as a 3-level
 * alpha and discriminates in-shader is here baked into per-view decoded
 * cells instead, one predicate per view (see the N3DS_TexView comment). The
 * single C3D_AlphaTest(GPU_GEQUAL, 1) then serves every pass unchanged.
 *
 * Generalized for the sub-page decode cache (docs/
 * SUBPAGE_DECODE_CACHE_DESIGN.md): originU/originV/width/height say what
 * page-relative rectangle to decode (the page's own absolute VRAM origin is
 * still added internally, below); destStrideTiles says how wide the
 * DESTINATION buffer is, in tiles -- a property of which scratch buffer the
 * caller is writing into, and NOT derivable from width. They only happen to
 * agree for the sub-page tier (both always 64px/8 tiles); the full-page
 * tier's width is format-clamped (256/128/64) while its destination
 * (s_decodeScratch) is always 256px/32 tiles, so deriving stride from width
 * would silently desync from the unchanged uploader on every 8bpp/16bpp page
 * (caught in review before this shipped -- see the design doc's "What
 * changed from v1" section). Pass destStrideTiles explicitly, always.
 */
internal void N3DS_DecodeAndTilePage(s16 texPage, s16 clut, TexFormat format, N3DS_TexView view, int originU,
                                      int originV, int width, int height, int destStrideTiles, u16 *outTiled)
{
	const int pageOriginX = (texPage & 0xF) * 64;
	const int pageOriginY = (texPage & 0x10) ? 256 : 0;
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	/* Per-view texel visibility, hoisted to two masks so the hot loop pays
	 * the same two ALU ops for every view (65k texels per full-page decode
	 * at 268 MHz -- a per-texel switch is real money here):
	 *   visible = ((c & visMask) != 0) && ((c & killMask) == 0)
	 * OPAQUE: any non-zero texel        (vis 0xFFFF, kill 0x0000)
	 * PASS1:  non-zero AND bit15 clear  (vis 0x7FFF, kill 0x8000 -- bit15
	 *         clear makes the 0x7FFF test equivalent to c != 0)
	 * PASS2:  bit15 set                 (vis 0x8000, kill 0x0000; 0x8000
	 *         "semi-transparent black" is visible, exactly as on PS1/PC). */
	const u16 visMask = (view == N3DS_TEXVIEW_PASS2) ? 0x8000u : ((view == N3DS_TEXVIEW_PASS1) ? 0x7FFFu : 0xFFFFu);
	const u16 killMask = (view == N3DS_TEXVIEW_PASS1) ? 0x8000u : 0x0000u;
	/*
	 * PSX BGR555 -> PICA200 GPU_RGBA5551. Both are 16-bit and both keep
	 * 5 bits per channel, so this is a bit shuffle with no expansion or
	 * loss.
	 *
	 * Source (PSX): bit 15 = STP, 14-10 = B, 9-5 = G, 4-0 = R.
	 * Dest (PICA):  15-11 = R, 10-6 = G, 5-1 = B, 0 = A.
	 *
	 * Destination layout verified EMPIRICALLY, not inferred from the
	 * format name -- inferring is precisely what once ruled
	 * out a byte-order bug that turned out to be the bug. Converting a
	 * solid R=0xF8,A=0xFF PNG with `tex3ds -r -z none -f rgba5551`
	 * emits `01 f8` per texel, i.e. 0xF801 little-endian = R=31, G=0,
	 * B=0, A=1. That pins R to the top 5 bits and alpha to bit 0.
	 *
	 * Alpha is one bit, set per the caller's view predicate (visMask/
	 * killMask) -- punch-through for OPAQUE cells, the per-texel STP split
	 * for PASS1/PASS2 cells. The single C3D_AlphaTest(GPU_GEQUAL, 1)
	 * consumes all three unchanged.
	 */
#define N3DS_DECODE_CONVERT(c)                                                                                        \
	((u16)((((c) & 0x001Fu) << 11) | ((((c) >> 5) & 0x1Fu) << 6) | ((((c) >> 10) & 0x1Fu) << 1) |                     \
	       ((((c) & visMask) != 0) && (((c) & killMask) == 0) ? 1u : 0u)))
	/*
	 * CLUT formats resolve the palette ONCE per call into this table --
	 * palette lookup, channel shuffle and the view predicate all collapse to
	 * one indexed load per texel. This loop runs 65k texels per full-page
	 * decode on a 268 MHz ARM11; the per-texel CLUT chase it replaces was
	 * measured at ~16 ms per page ([TEXDEC]), which is what turned a scene
	 * cut's cold-miss burst into a second-long freeze.
	 *
	 * CLUT entries wrap within the CLUT's own VRAM row: PC's VRAM sampler
	 * inherits GL_REPEAT and wraps S independently of T, so it can never walk
	 * into the next scanline. A 4-bit CLUT (16 entries, 16-aligned origin)
	 * can't actually overflow -- masked for symmetry with the 8-bit case,
	 * which reaches 239 texels into the next scanline unmasked.
	 */
	u16 *const lut = s_decodeClutLut;
	/* Morton low bits contributed by u (u&7): precomputed; the v half is
	 * constant per row and hoisted below. */
	static const u8 uMort[8] = {0, 1, 4, 5, 16, 17, 20, 21};
	int v;

	if (s_vramPendingCount != 0)
	{
		const int pageWords = (format == TF_4_BIT) ? 64 : ((format == TF_8_BIT) ? 128 : 256);

		if ((format == TF_4_BIT) || (format == TF_8_BIT))
		{
			N3DS_VramSettleWrapped(clutOriginX, clutOriginY, (format == TF_4_BIT) ? 16 : 256, 1);
		}
		N3DS_VramSettleWrapped(pageOriginX, pageOriginY + originV, pageWords, height);
	}

	if ((format == TF_4_BIT) || (format == TF_8_BIT))
	{
		const u16 *clutRow = &vram[clutOriginY * VRAM_WIDTH];
		const int n = (format == TF_4_BIT) ? 16 : 256;
		int i;

		for (i = 0; i < n; i++)
		{
			const u16 c = clutRow[(clutOriginX + i) & (VRAM_WIDTH - 1)];

			lut[i] = N3DS_DECODE_CONVERT(c);
		}
	}

	for (v = 0; v < height; v++)
	{
		/* Page-relative source column (originU + u) -- VRAM addressing uses
		 * it, but the Morton swizzle and destination tile indexing stay on
		 * the local, 0-based u/v. VRAM word columns wrap the 1024-wide row
		 * per WORD, exactly as before (a bottom-right page's 8bpp span does
		 * wrap). */
		const u16 *vramRow = &vram[(pageOriginY + originV + v) * VRAM_WIDTH];
		const int vMort = ((v & 1) << 1) | ((v & 2) << 2) | ((v & 4) << 3);
		const int rowTileBase = (v >> 3) * destStrideTiles;
		int u = 0;

		switch (format)
		{
		case TF_4_BIT:
		{
			/* Word-aligned fast body only when page-U and local u agree on
			 * alignment (originU % 4 == 0 -- true for the full-page and
			 * sub-page tiers, the miss-storm paths). The track-atlas rects
			 * pass arbitrary srcU and take the per-texel leg below. */
			if ((originU & 3) == 0)
			{
				for (; (u + 3) < width; u += 4)
				{
					const u16 word = vramRow[(pageOriginX + ((originU + u) >> 2)) & (VRAM_WIDTH - 1)];
					u16 *dst = &outTiled[((rowTileBase + (u >> 3)) * 64) + vMort + ((u & 4) << 2)];

					dst[0] = lut[word & 0xF];
					dst[1] = lut[(word >> 4) & 0xF];
					dst[4] = lut[(word >> 8) & 0xF];
					dst[5] = lut[(word >> 12) & 0xF];
				}
			}
			for (; u < width; u++)
			{
				const int pageU = originU + u;
				const u16 word = vramRow[(pageOriginX + (pageU >> 2)) & (VRAM_WIDTH - 1)];

				outTiled[((rowTileBase + (u >> 3)) * 64) + vMort + uMort[u & 7]] = lut[(word >> ((pageU & 3) * 4)) & 0xF];
			}
			break;
		}
		case TF_8_BIT:
		{
			/* Texel pairs share one VRAM word; within a pair the odd texel's
			 * Morton index is always the even one's + 1 (uMort steps by one
			 * across each even/odd pair). Same alignment condition as above. */
			if ((originU & 1) == 0)
			{
				for (; (u + 1) < width; u += 2)
				{
					const u16 word = vramRow[(pageOriginX + ((originU + u) >> 1)) & (VRAM_WIDTH - 1)];
					u16 *dst = &outTiled[((rowTileBase + (u >> 3)) * 64) + vMort + uMort[u & 7]];

					dst[0] = lut[word & 0xFF];
					dst[1] = lut[word >> 8];
				}
			}
			for (; u < width; u++)
			{
				const int pageU = originU + u;
				const u16 word = vramRow[(pageOriginX + (pageU >> 1)) & (VRAM_WIDTH - 1)];

				outTiled[((rowTileBase + (u >> 3)) * 64) + vMort + uMort[u & 7]] = lut[(word >> ((pageU & 1) * 8)) & 0xFF];
			}
			break;
		}
		case TF_16_BIT:
		case TF_32_BIT_RGBA: /* unreachable -- DrawTriangles never calls this path for it, see file header */
		default:
		{
			for (; u < width; u++)
			{
				const u16 c = vramRow[(pageOriginX + originU + u) & (VRAM_WIDTH - 1)];

				outTiled[((rowTileBase + (u >> 3)) * 64) + vMort + uMort[u & 7]] = N3DS_DECODE_CONVERT(c);
			}
			break;
		}
		}
	}
#undef N3DS_DECODE_CONVERT
}

/* (u,v) -> flat index in a Morton-tiled (PICA 8x8 Z-order) buffer, the same
 * mapping N3DS_DecodeAndTilePage writes with. Shared so the dilation pass below
 * addresses the exact texels the decode produced. */
static inline int N3DS_TiledTexelIndex(int u, int v, int destStrideTiles)
{
	int morton = (u & 1) | ((v & 1) << 1) | ((u & 2) << 1) | ((v & 2) << 2) | ((u & 4) << 2) | ((v & 4) << 3);
	return (((v >> 3) * destStrideTiles + (u >> 3)) * 64) + morton;
}

/*
 * Bilinear-cutout fix for the track atlas: dilate opaque RGB one texel into
 * transparent (alpha-0) texels, in place, on a Morton-tiled RGBA5551 buffer.
 *
 * Punch-through texels decode to 0x0000 -- BLACK, alpha 0 (in the OPAQUE view a
 * texel is transparent only when its source is exactly zero). Under GPU_LINEAR a
 * kernel straddling a cutout edge blends that black in, and the alpha test still
 * passes the darkened pixel, so cutouts wear a black rim. Giving each edge-
 * adjacent transparent texel an opaque 4-neighbour's RGB (alpha kept 0) leaves
 * the kernel real edge colour to blend toward instead of black.
 *
 * Alpha is never touched, so GPU_NEAREST -- which alpha-tests these texels away
 * regardless of their RGB -- renders byte-identically; only bilinear sees it.
 * One-texel dilation suffices: a bilinear kernel reaches only one texel past an
 * edge. In place is safe because a filled texel keeps alpha 0, so it is never
 * itself picked as a dilation source (sources require alpha 1). Load-time only,
 * and called only on the track atlas -- the model decode cache is never
 * bilinear-sampled, so it does not pay this scan.
 */
internal void N3DS_DilateTransparentRGB(u16 *tiled, int width, int height, int destStrideTiles)
{
	static const int dx[4] = {-1, 1, 0, 0};
	static const int dy[4] = {0, 0, -1, 1};
	int v, u, k;

	for (v = 0; v < height; v++)
	{
		for (u = 0; u < width; u++)
		{
			int idx = N3DS_TiledTexelIndex(u, v, destStrideTiles);

			if ((tiled[idx] & 1u) != 0u)
			{
				continue; /* opaque -- a source, never a target */
			}

			for (k = 0; k < 4; k++)
			{
				int nu = u + dx[k];
				int nv = v + dy[k];
				u16 nc;

				if ((nu < 0) || (nu >= width) || (nv < 0) || (nv >= height))
				{
					continue;
				}

				nc = tiled[N3DS_TiledTexelIndex(nu, nv, destStrideTiles)];
				if ((nc & 1u) != 0u)
				{
					tiled[idx] = (u16)(nc & 0xFFFEu); /* neighbour RGB, alpha still 0 */
					break;
				}
			}
		}
	}
}

/*
 * Finds an atlas cell already decoded for (texPage, clut, format) and still
 * fresh (matching VRAM dirty-cell generations), or decodes into the
 * least-recently-used atlas cell on a miss. Returns a flat cell index (see
 * N3DS_CellIndexToAtlasCoord). Linear scan over 1+N3DS_FULLPAGE_RESERVED_CELLS
 * (this tier's whole-page cell budget since the
 * two-tier split -- was N3DS_TOTAL_CELLS before) -- fine for a
 * correctness-first pass at this count; the design doc explicitly leaves
 * perf tuning for after on-device measurement. Note "cell" means two
 * different things in this function and both are real: VRAM dirty-tracking
 * cells (N3DS_VRAM_CELL_*, 256x256 regions of PSX VRAM) versus atlas cells
 * (256x256 regions of a shared GPU texture) -- they are unrelated grids that
 * happen to share a name and a size.
 */
/*
 * sampV0 is the FIRST PAGE ROW THIS LOOKUP ACTUALLY READS (the triangle's UV
 * bbox minV), and it is what page freshness is keyed on -- not the page
 * anchor. CTR's HUD sheets live in the free VRAM strip below display
 * buffer A, in pages ANCHORED at y=0, i.e. inside the framebuffer. Keying off
 * the anchor cell therefore watched live picture on behalf of a texture that
 * sits 216 rows below it, and every HUD page re-decoded 65536 texels every
 * frame. The sibling test at N3DS_ResolveTriangleTexture's mirror-refresh call
 * already learned this -- its comment says so in as many words -- and
 * this is the same correction applied to the freshness key.
 *
 * Consequence to know: an entry's stored pageGen belongs to the row band of
 * whichever lookup decoded it. A later lookup reading a DIFFERENT band of the
 * same (page,clut) computes a different cell and will usually miss, costing
 * one re-decode. That is bounded (it is exactly today's behaviour) and does
 * not happen for the content this fixes, where each sheet has one stable
 * footprint. Pass 0 when no footprint is known -- that restores the old
 * anchor-keyed behaviour for that caller.
 */
internal int N3DS_FindOrDecodeTexture(s16 texPage, s16 clut, TexFormat format, N3DS_TexView view, int sampV0)
{
	const int pageCell = N3DS_VRAMCellIndex((texPage & 0xF) * 64, ((texPage & 0x10) ? 256 : 0) + sampV0);
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	const int clutCellA = N3DS_VRAMCellIndex(clutOriginX, clutOriginY);
	/* An 8-bit CLUT is 256 texels wide and can straddle a 256-wide VRAM cell
	 * boundary when its 16-aligned origin isn't itself cell-aligned (a
	 * 4-bit CLUT is only 16 wide and can never cross one). Track the far
	 * edge's cell too so a change there isn't missed. */
	const int clutCellB = N3DS_VRAMCellIndex(clutOriginX + 255, clutOriginY);
	/* Hoisted out of the scan: these three were re-read from s_vramCellGen on
	 * every one of up to 63 iterations, per triangle. */
	const u32 pageGen = s_vramCellGen[pageCell];
	/* CLUT-less formats have no palette to go stale: pin their clut gens to a
	 * constant so palette-area VRAM writes can never fake-stale them. The
	 * decode store below writes these same hoisted values, keeping lookup and
	 * entry permanently consistent for such formats. */
	const int hasClut = (format == TF_4_BIT) || (format == TF_8_BIT);
	const u32 clutGenA = hasClut ? s_vramCellGen[clutCellA] : 0;
	const u32 clutGenB = hasClut ? s_vramCellGen[clutCellB] : 0;
	u32 curClutHash = 0;
	int haveClutHash = 0;
	int i, oldestSlot;
	u32 oldestFrame;
	u32 hashBucket;
	/* Set when a cell holds exactly this (page,clut,format) but its source VRAM
	 * generation has moved -- i.e. the texture IS resident and we are re-decoding
	 * it anyway. Separates the two miss causes; see s_decodeCacheStaleMisses.
	 * The slot itself is remembered too: a stale twin is the natural victim
	 * (it holds a dead copy of exactly this texture), and reusing it in place
	 * both skips the whole victim walk and stops animated textures from
	 * churning a fresh slot per re-decode while their old cell decays into
	 * LRU garbage (~19 stale re-decodes/frame in race content, hardware). */
	int sawStaleKeyMatch = 0;
	int staleSawPageGen = 0;
	int staleSawClutGen = 0;
	int staleMatchSlot = -1;

	/* Fast path: the cell that answered the previous lookup. See s_lastCellHit. */
	if (s_lastCellHit > 0)
	{
		N3DS_DecodeCacheEntry *e = &s_decodeCache[s_lastCellHit];

		if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) && (e->view == (u8)view) &&
		    (e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_frameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_decodeCacheHits++;
			return s_lastCellHit;
		}
	}

	/* Hash probe -- O(1) for everything the memo doesn't catch. Verified
	 * against the full key + generations, so a wrong bucket only costs the
	 * scan fallback below (see the hash-index comment). */
	hashBucket = N3DS_TexKeyHash(texPage, clut, format, view, 0, 0) & (N3DS_FULLPAGE_HASH_SIZE - 1);
	{
		const int cand = s_fullPageHashIdx[hashBucket];

		if ((cand > 0) && (cand < N3DS_FullPageCellBound()))
		{
			N3DS_DecodeCacheEntry *e = &s_decodeCache[cand];

			if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) && (e->view == (u8)view) &&
			    (e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
			{
				if (e->lastUsedFrame != s_frameCounter)
				{
					s_frameTouchedCells++;
				}
				e->lastUsedFrame = s_frameCounter;
				s_decodeCacheHits++;
				s_fullPageHashHits++;
				s_lastCellHit = cand;
				return cand;
			}
		}
	}

	for (i = 1; i < N3DS_FullPageCellBound(); i++)
	{
		N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

		/* Key first, cheapest rejection first. The LRU victim search that used
		 * to share this loop has moved below: it is only needed on a miss, and
		 * misses are rare (measured 0-25 per 32-frame window against ~65,000
		 * lookups), so it was pure overhead on every hit. */
		if (!e->used || (e->page != texPage) || (e->clut != clut) || (e->format != format) || (e->view != (u8)view))
		{
			continue;
		}

		if ((e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_frameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_decodeCacheHits++;
			s_lastCellHit = i;
			/* Reached only when the probe missed: repair the bucket. */
			s_fullPageHashIdx[hashBucket] = (s16)i;
			s_fullPageHashRepairs++;
			return i;
		}

		/* CLUT gens moved but page data is fresh: palette-animation
		 * candidate. Content match means a fresh decode would be
		 * byte-identical (same source indices, same palette bytes, same
		 * format/view), so adopt the cell and revalidate its gens in place
		 * -- see N3DS_HashClutContent's block comment. */
		if (hasClut && (e->pageGen == pageGen))
		{
			if (!haveClutHash)
			{
				curClutHash = N3DS_HashClutContent(clut, format);
				haveClutHash = 1;
			}
			if (e->clutHash == curClutHash)
			{
				if (e->lastUsedFrame != s_frameCounter)
				{
					s_frameTouchedCells++;
				}
				e->clutGenA = clutGenA;
				e->clutGenB = clutGenB;
				e->lastUsedFrame = s_frameCounter;
				s_decodeCacheHits++;
				s_fullPageClutVariantHits++;
				s_lastCellHit = i;
				s_fullPageHashIdx[hashBucket] = (s16)i;
				return i;
			}
		}

		/* Key matched but generations moved. Attribute the cause here, with
		 * the entry at hand, and pick the reuse policy: a page-DATA change
		 * makes the old copy worthless, so it stays the in-place stale-twin
		 * victim. A pure CLUT change leaves a viable palette-step
		 * variant -- claiming it for in-place re-decode would purge the
		 * variant library on every cycle step and the retarget above could
		 * never fire (the cycle needs one cell PER palette step to
		 * materialize), so it is left alive and the miss below takes a
		 * normal LRU victim instead. */
		sawStaleKeyMatch = 1;
		if (e->pageGen != pageGen)
		{
			staleSawPageGen = 1;
			staleMatchSlot = i;
		}
		if ((e->clutGenA != clutGenA) || (e->clutGenB != clutGenB))
		{
			staleSawClutGen = 1;
		}
	}

	s_decodeCacheMisses++;
	if (sawStaleKeyMatch)
	{
		s_decodeCacheStaleMisses++;
		if (staleSawPageGen)
		{
			s_fullPageStaleByPageGen++;
		}
		if (staleSawClutGen)
		{
			s_fullPageStaleByClutGen++;
		}
		s_fpStaleByPageId[(u32)(u16)texPage & 0x1F]++;
	}
	else
	{
		s_decodeCacheCapacityMisses++;
	}

	/*
	 * Pick the victim. Skip any cell already stamped with this frame: a draw
	 * referencing it is queued but unexecuted (citro3d runs the whole command
	 * list at C3D_FrameEnd) while N3DS_C3DUploadDecodedCell memcpys over it
	 * immediately, so evicting one silently repoints an already-queued triangle
	 * at a different texture. That is the mid-frame eviction hazard, and it is
	 * the same class of bug as the vertex-buffer aliasing fixed earlier.
	 *
	 * The old code could not avoid it and made it worse: it initialised
	 * oldestSlot=1 and compared with strict <, so once every cell carried the
	 * current frame's stamp, every further miss re-evicted slot 1 specifically.
	 *
	 * Must also check `used`: an entry that has never been decoded into can
	 * never be referenced by a queued draw, so it is always a safe victim
	 * regardless of its lastUsedFrame value. Without this, frame 0 breaks --
	 * s_frameCounter starts at 0 and every entry's zero-initialised
	 * lastUsedFrame also reads 0, so EVERY cell (not just genuinely-touched
	 * ones) matches the skip condition on the very first frame, oldestSlot
	 * never gets set, and the code falls into the "every cell spoken for"
	 * hazard branch below on every miss that frame -- exactly the same-slot
	 * re-eviction bug this rewrite was meant to fix, reintroduced for frame 0.
	 */
	/* Stale twin not referenced by any queued draw this frame: re-decode
	 * into it directly (see staleMatchSlot's comment). The same-frame guard
	 * is the mid-frame eviction hazard rule from the victim walk below. */
	if ((staleMatchSlot >= 0) && (s_decodeCache[staleMatchSlot].lastUsedFrame != s_frameCounter))
	{
		oldestSlot = staleMatchSlot;
		goto decodeIntoSlot;
	}

	oldestSlot = -1;
	oldestFrame = 0xFFFFFFFFu;
	{
		int preferredSlot = -1;
		u32 preferredFrame = 0xFFFFFFFFu;
		const int preferredAtlas = s_runAtlas; /* -1 when no run is open */

		for (i = 1; i < N3DS_FullPageCellBound(); i++)
		{
			N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

			if (e->used && (e->lastUsedFrame == s_frameCounter))
			{
				continue;
			}

			if (e->lastUsedFrame < oldestFrame)
			{
				oldestFrame = e->lastUsedFrame;
				oldestSlot = i;
			}
			if (((i / N3DS_ATLAS_CELLS_PER_ATLAS) == preferredAtlas) && (e->lastUsedFrame < preferredFrame))
			{
				preferredFrame = e->lastUsedFrame;
				preferredSlot = i;
			}
		}

		/* Atlas affinity (see N3DS_ATLAS_AFFINITY_MIN_AGE): redirect to the
		 * run's own atlas when its best victim is old enough. */
		if ((preferredSlot >= 0) && (preferredSlot != oldestSlot) &&
		    ((preferredFrame + N3DS_ATLAS_AFFINITY_MIN_AGE) <= s_frameCounter))
		{
			oldestSlot = preferredSlot;
			s_fullPageAffinityPlacements++;
		}
	}

	if (oldestSlot < 0)
	{
		/*
		 * Every cell is spoken for by a queued draw. Corruption is unavoidable
		 * without splitting the frame (C3D_FrameSplit, which would flush the
		 * queued draws first); that is the real fix and is not done here. Count
		 * it so the condition is visible rather than silent -- if this stays 0
		 * in real content, the frame split is not worth building.
		 */
		s_evictHazards++;
		oldestSlot = 1;
	}

	if (s_decodeCache[oldestSlot].used)
	{
		/* Not counted for the stale-twin reuse path (the goto skips here):
		 * replacing a dead copy of this same key is not thrashing, which is
		 * what this counter exists to measure. */
		s_decodeCacheEvictions++;
	}

decodeIntoSlot:
	if (s_decodeCache[oldestSlot].lastUsedFrame != s_frameCounter)
	{
		s_frameTouchedCells++;
	}
	s_lastCellHit = oldestSlot;

	{
		unsigned long long texdecEventT0;

		/*
		 * A PSX texture page is 256 TEXELS wide in EVERY colour mode -- the
		 * mode only changes how many VRAM words those 256 texels occupy (64 at
		 * 4bpp, 128 at 8bpp, 256 at 16bpp), i.e. how many tpage columns the
		 * texture spans, NOT how far U can reach. U/V are always 0-255. A prior
		 * version decoded only 128 columns for 8bpp / 64 for 16bpp on the theory
		 * that "the game never samples beyond that" -- that was wrong (it
		 * confused word-width with texel-width), and it left the right half (or
		 * three-quarters) of every wide 8/16bpp texture stale, which is exactly
		 * the large-surface corruption. Decode the full 256 always; the wider
		 * VRAM reads are wrap-guarded in N3DS_DecodeAndTilePage (matching PC's
		 * GL_REPEAT). destStrideTiles stays N3DS_DECODE_TEXTURE_DIM/8, the
		 * destination scratch's own width in tiles.
		 */
		{
			const int decodeWidth = N3DS_DECODE_TEXTURE_DIM;
			const unsigned long long texdecT0 = N3DS_GetTickCount();

			texdecEventT0 = texdecT0;
			N3DS_DecodeAndTilePage(texPage, clut, format, view, 0, 0, decodeWidth, N3DS_DECODE_TEXTURE_DIM,
			                        N3DS_DECODE_TEXTURE_DIM / 8, s_decodeScratch);
			s_texdecFrameDecTicks += N3DS_GetTickCount() - texdecT0;
		}
		{
			int atlasIdx, cellX, cellY;
			const unsigned long long texdecT1 = N3DS_GetTickCount();
			unsigned long long texdecT2;

			N3DS_CellIndexToAtlasCoord(oldestSlot, &atlasIdx, &cellX, &cellY);
			N3DS_C3DUploadDecodedCell(atlasIdx, cellX, cellY, s_decodeScratch);
			texdecT2 = N3DS_GetTickCount();
			s_texdecFrameUpTicks += texdecT2 - texdecT1;
			s_texdecFrameFull++;
			if ((texdecT2 - texdecEventT0) > s_texdecFrameWorstTicks)
			{
				s_texdecFrameWorstTicks = texdecT2 - texdecEventT0;
				s_texdecFrameWorstPage = (u16)texPage;
				s_texdecFrameWorstClut = (u16)clut;
				s_texdecFrameWorstIsSub = 0;
			}
		}
	}

	s_decodeCache[oldestSlot].used = 1;
	s_decodeCache[oldestSlot].page = texPage;
	s_decodeCache[oldestSlot].clut = clut;
	s_decodeCache[oldestSlot].format = format;
	s_decodeCache[oldestSlot].view = (u8)view;
	/* The hoisted gens, NOT re-reads: for CLUT-less formats these are the
	 * pinned constants the lookup compares against (see the hoist). */
	s_decodeCache[oldestSlot].pageGen = pageGen;
	s_decodeCache[oldestSlot].clutGenA = clutGenA;
	s_decodeCache[oldestSlot].clutGenB = clutGenB;
	if (!haveClutHash)
	{
		curClutHash = N3DS_HashClutContent(clut, format);
	}
	s_decodeCache[oldestSlot].clutHash = curClutHash;
	s_decodeCache[oldestSlot].lastUsedFrame = s_frameCounter;
	s_decodeCache[oldestSlot].serial = ++s_cellDecodeSerial;
	s_fullPageHashIdx[hashBucket] = (s16)oldestSlot;

	return oldestSlot;
}

/*
 * Sub-page counterpart to N3DS_FindOrDecodeTexture above -- same memo/scan/
 * stale-vs-capacity/hazard-aware-LRU logic, additionally keyed on which 64x64
 * tile of the page (tileX,tileY), operating over s_subPageCache/
 * N3DS_SUBPAGE_TOTAL_CELLS instead of s_decodeCache/(1+N3DS_FULLPAGE_RESERVED_CELLS).
 * Every bound below is 0-based, NOT 1-based like the full-page tier: index 0
 * is a real, valid sub-page slot here (no reserved white cell at 0 in this
 * array -- see s_lastSubPageCellHit's own comment). Copy-pasting the
 * full-page tier's 1-based bounds here would be silently wrong: the memo
 * guard `> 0` must be `>= 0`, both scan loops start at `i = 0` not `i = 1`,
 * and the hazard-fallback victim is slot 0, not 1.
 */
internal int N3DS_FindOrDecodeSubPageTexture(s16 texPage, s16 clut, TexFormat format, N3DS_TexView view, int tileX,
                                              int tileY, int sampV0)
{
	const int pageOriginX = (texPage & 0xF) * 64;
	const int pageOriginY = (texPage & 0x10) ? 256 : 0;
	/* Keyed on the SAMPLED row, same correction as the full-page tier.
	 * This used to key on the tile's top row and note that a tile maps exactly
	 * onto one dirty cell -- true when cells were 64 tall, no longer true now
	 * they are 8, and not the property that matters anyway: a 64-tall tile
	 * whose top rows are live framebuffer and whose sampled rows are not was
	 * exactly the false-sharing case this fixes. */
	const int pageCell = N3DS_VRAMCellIndex(pageOriginX + (tileX * 64), pageOriginY + sampV0);
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	const int clutCellA = N3DS_VRAMCellIndex(clutOriginX, clutOriginY);
	const int clutCellB = N3DS_VRAMCellIndex(clutOriginX + 255, clutOriginY);
	const u32 pageGen = s_vramCellGen[pageCell];
	/* Same CLUT-less pinning as the full-page tier's hoist -- see there. */
	const int hasClut = (format == TF_4_BIT) || (format == TF_8_BIT);
	const u32 clutGenA = hasClut ? s_vramCellGen[clutCellA] : 0;
	const u32 clutGenB = hasClut ? s_vramCellGen[clutCellB] : 0;
	u32 curClutHash = 0;
	int haveClutHash = 0;
	int i, oldestSlot;
	u32 oldestFrame;
	u32 hashBucket;
	int sawStaleKeyMatch = 0;
	int staleSawPageGen = 0;
	int staleSawClutGen = 0;
	/* Stale-twin reuse, same rationale as the full-page tier -- and this is
	 * the tier where it matters: the ~19 stale re-decodes/frame of race
	 * content (hardware) are nearly all here, and each one otherwise
	 * pays a full N3DS_SUBPAGE_TOTAL_CELLS victim walk. It
	 * only claims PAGE-data-stale twins -- CLUT-stale cells are live palette
	 * variants, see the full-page scan's comment. */
	int staleMatchSlot = -1;

	/* Fast path: the cell that answered the previous lookup. See s_lastSubPageCellHit. */
	if (s_lastSubPageCellHit >= 0)
	{
		N3DS_SubPageCacheEntry *e = &s_subPageCache[s_lastSubPageCellHit];

		if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) && (e->view == (u8)view) &&
		    (e->tileX == tileX) && (e->tileY == tileY) && (e->pageGen == pageGen) && (e->clutGenA == clutGenA) &&
		    (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_subPageFrameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_subPageCacheHits++;
			return s_lastSubPageCellHit;
		}
	}

	/* Hash probe -- the sub-page tier is where the scan hurt most (up to
	 * 496 entries per miss of the one-entry memo, per triangle). Same
	 * verify-then-trust contract as the full-page probe. */
	hashBucket = N3DS_TexKeyHash(texPage, clut, format, view, tileX, tileY) & (N3DS_SUBPAGE_HASH_SIZE - 1);
	{
		const int cand = s_subPageHashIdx[hashBucket];

		if ((cand >= 0) && (cand < N3DS_SUBPAGE_TOTAL_CELLS))
		{
			N3DS_SubPageCacheEntry *e = &s_subPageCache[cand];

			if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) && (e->view == (u8)view) &&
			    (e->tileX == tileX) && (e->tileY == tileY) && (e->pageGen == pageGen) && (e->clutGenA == clutGenA) &&
			    (e->clutGenB == clutGenB))
			{
				if (e->lastUsedFrame != s_frameCounter)
				{
					s_subPageFrameTouchedCells++;
				}
				e->lastUsedFrame = s_frameCounter;
				s_subPageCacheHits++;
				s_subPageHashHits++;
				s_lastSubPageCellHit = cand;
				return cand;
			}
		}
	}

	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		N3DS_SubPageCacheEntry *e = &s_subPageCache[i];

		if (!e->used || (e->page != texPage) || (e->clut != clut) || (e->format != format) || (e->view != (u8)view) ||
		    (e->tileX != tileX) || (e->tileY != tileY))
		{
			continue;
		}

		if ((e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_subPageFrameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_subPageCacheHits++;
			s_lastSubPageCellHit = i;
			/* Reached only when the probe missed: repair the bucket. */
			s_subPageHashIdx[hashBucket] = (s16)i;
			s_subPageHashRepairs++;
			return i;
		}

		/* Palette-animation variant retarget, mirroring the full-page
		 * tier's check -- see N3DS_HashClutContent's block comment. */
		if (hasClut && (e->pageGen == pageGen))
		{
			if (!haveClutHash)
			{
				curClutHash = N3DS_HashClutContent(clut, format);
				haveClutHash = 1;
			}
			if (e->clutHash == curClutHash)
			{
				if (e->lastUsedFrame != s_frameCounter)
				{
					s_subPageFrameTouchedCells++;
				}
				e->clutGenA = clutGenA;
				e->clutGenB = clutGenB;
				e->lastUsedFrame = s_frameCounter;
				s_subPageCacheHits++;
				s_subPageClutVariantHits++;
				s_lastSubPageCellHit = i;
				s_subPageHashIdx[hashBucket] = (s16)i;
				return i;
			}
		}

		/* Same split policy as the full-page scan: page-data stales reuse
		 * in place, CLUT-stale cells stay alive as palette variants. */
		sawStaleKeyMatch = 1;
		if (e->pageGen != pageGen)
		{
			staleSawPageGen = 1;
			staleMatchSlot = i;
		}
		if ((e->clutGenA != clutGenA) || (e->clutGenB != clutGenB))
		{
			staleSawClutGen = 1;
		}
	}

	s_subPageCacheMisses++;
	if (sawStaleKeyMatch)
	{
		s_subPageCacheStaleMisses++;
		if (staleSawPageGen)
		{
			s_subPageStaleByPageGen++;
		}
		if (staleSawClutGen)
		{
			s_subPageStaleByClutGen++;
		}
		s_spStaleByPageId[(u32)(u16)texPage & 0x1F]++;
	}
	else
	{
		s_subPageCacheCapacityMisses++;
	}

	/* Stale twin not referenced this frame: re-decode into it directly
	 * (see staleMatchSlot's comment; same-frame guard = the hazard rule). */
	if ((staleMatchSlot >= 0) && (s_subPageCache[staleMatchSlot].lastUsedFrame != s_frameCounter))
	{
		oldestSlot = staleMatchSlot;
		goto decodeIntoSubSlot;
	}

	/* Victim search -- see N3DS_FindOrDecodeTexture's own comment above for
	 * the full reasoning (mid-frame eviction hazard; frame-0 all-zero-
	 * lastUsedFrame guard). Identical here except the hazard-fallback victim
	 * is slot 0, not 1 -- see this function's own header comment. */
	oldestSlot = -1;
	oldestFrame = 0xFFFFFFFFu;
	{
		int preferredSlot = -1;
		u32 preferredFrame = 0xFFFFFFFFu;
		const int preferredAtlas = s_runAtlas;

		for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
		{
			N3DS_SubPageCacheEntry *e = &s_subPageCache[i];

			if (e->used && (e->lastUsedFrame == s_frameCounter))
			{
				continue;
			}

			if (e->lastUsedFrame < oldestFrame)
			{
				oldestFrame = e->lastUsedFrame;
				oldestSlot = i;
			}
			if ((((N3DS_SUBPAGE_PHYSICAL_BASE + (i / N3DS_SUBPAGE_CELLS_PER_PHYSICAL)) / N3DS_ATLAS_CELLS_PER_ATLAS) ==
			     preferredAtlas) &&
			    (e->lastUsedFrame < preferredFrame))
			{
				preferredFrame = e->lastUsedFrame;
				preferredSlot = i;
			}
		}

		/* Atlas affinity -- see the full-page walk above. */
		if ((preferredSlot >= 0) && (preferredSlot != oldestSlot) &&
		    ((preferredFrame + N3DS_ATLAS_AFFINITY_MIN_AGE) <= s_frameCounter))
		{
			oldestSlot = preferredSlot;
			s_subPageAffinityPlacements++;
		}
	}

	if (oldestSlot < 0)
	{
		s_subPageEvictHazards++;
		oldestSlot = 0;
	}

	if (s_subPageCache[oldestSlot].used)
	{
		/* Not counted for stale-twin reuse (the goto skips here) -- see the
		 * full-page tier's comment. */
		s_subPageCacheEvictions++;
	}

decodeIntoSubSlot:
	if (s_subPageCache[oldestSlot].lastUsedFrame != s_frameCounter)
	{
		s_subPageFrameTouchedCells++;
	}
	s_lastSubPageCellHit = oldestSlot;

	/*
	 * originU/originV are PAGE-RELATIVE (0-based within the page), matching
	 * the convention N3DS_ResolveTriangleTexture's outOriginU/V use below --
	 * N3DS_DecodeAndTilePage adds the page's own absolute VRAM origin
	 * internally, so do NOT add pageOriginX/Y here too (that double-counts
	 * the page origin and reads off the bottom of VRAM for any bottom-half
	 * page -- caught in review before this shipped, see the design doc's
	 * "What changed from v1" section). width=height=64 always exactly fills
	 * s_subpageDecodeScratch, unlike the full-page tier's format-clamped
	 * decodeWidth<256 cases, which leave stale columns from a prior decode.
	 */
	{
		unsigned long long texdecEventT0;

		{
			const unsigned long long texdecT0 = N3DS_GetTickCount();

			texdecEventT0 = texdecT0;
			N3DS_DecodeAndTilePage(texPage, clut, format, view, tileX * 64, tileY * 64, N3DS_SUBPAGE_DIM, N3DS_SUBPAGE_DIM,
			                        N3DS_SUBPAGE_DIM / 8, s_subpageDecodeScratch);
			s_texdecFrameDecTicks += N3DS_GetTickCount() - texdecT0;
		}
		{
			int atlasIdx, physCellX, physCellY, subX, subY;
			const unsigned long long texdecT1 = N3DS_GetTickCount();
			unsigned long long texdecT2;

			N3DS_SubCellIndexToAtlasCoord(oldestSlot, &atlasIdx, &physCellX, &physCellY, &subX, &subY);
			N3DS_C3DUploadDecodedSubCell(atlasIdx, physCellX, physCellY, subX, subY, s_subpageDecodeScratch);
			texdecT2 = N3DS_GetTickCount();
			s_texdecFrameUpTicks += texdecT2 - texdecT1;
			s_texdecFrameSub++;
			if ((texdecT2 - texdecEventT0) > s_texdecFrameWorstTicks)
			{
				s_texdecFrameWorstTicks = texdecT2 - texdecEventT0;
				s_texdecFrameWorstPage = (u16)texPage;
				s_texdecFrameWorstClut = (u16)clut;
				s_texdecFrameWorstIsSub = 1;
				s_texdecFrameWorstTileX = (s8)tileX;
				s_texdecFrameWorstTileY = (s8)tileY;
			}
		}
	}

	s_subPageCache[oldestSlot].used = 1;
	s_subPageCache[oldestSlot].page = texPage;
	s_subPageCache[oldestSlot].clut = clut;
	s_subPageCache[oldestSlot].format = format;
	s_subPageCache[oldestSlot].view = (u8)view;
	s_subPageCache[oldestSlot].tileX = (u8)tileX;
	s_subPageCache[oldestSlot].tileY = (u8)tileY;
	/* Hoisted gens, not re-reads -- same CLUT-less pinning contract as the
	 * full-page tier's store. */
	s_subPageCache[oldestSlot].pageGen = pageGen;
	s_subPageCache[oldestSlot].clutGenA = clutGenA;
	s_subPageCache[oldestSlot].clutGenB = clutGenB;
	if (!haveClutHash)
	{
		curClutHash = N3DS_HashClutContent(clut, format);
	}
	s_subPageCache[oldestSlot].clutHash = curClutHash;
	s_subPageCache[oldestSlot].lastUsedFrame = s_frameCounter;
	s_subPageCache[oldestSlot].serial = ++s_cellDecodeSerial;
	s_subPageHashIdx[hashBucket] = (s16)oldestSlot;

	return oldestSlot;
}

internal void N3DS_MergeFlush(void);

/*
 * One deferred draw actually hitting the command list. Doubling is gated by
 * the caller's tracker because N3DS_C3DSetColorDoubling dirties the TEV
 * unconditionally; blend is set unconditionally because the backend dedups
 * it internally (n3ds_backend.h). s_runBlendMode is overwritten as a side
 * effect, so a caller with an open blend run closes it first.
 */
/* s_mergeDepthWriteState is the depth-write memo (see N3DS_MergeEmit); -1
 * forces the first draw to assert. */
int g_dbg_noFx;
global_variable int s_mergeDepthWriteState = -1;

internal void N3DS_MergeEmit(int blendMode, int doubling, int atlas, int baseVertex, int vertCount, int *lastDoubling)
{
	/*
	 * A blended packet (smoke, sparks, shadows) is translucent: it must TEST
	 * depth so things in front still hide it, but it must not WRITE depth, or
	 * every pixel it covers becomes opaque to whatever is drawn afterwards. The
	 * model path (N3DS_ModelDraw_Run) and the track's blended ranges follow the
	 * same rule.
	 *
	 * g_dbg_noFx drops blended packets entirely -- a judging aid, so effects
	 * cannot obscure what is being looked at while something else is tuned.
	 */
	if ((blendMode != 0) && g_dbg_noFx)
	{
		return;
	}

	if (*lastDoubling != doubling)
	{
		N3DS_C3DSetColorDoubling(doubling);
		*lastDoubling = doubling;
	}
	{
		/* Memoized here rather than in the backend so a frame of all-opaque
		 * merge segments costs zero extra GPU state changes, matching what the
		 * doubling tracker above already does. */
		const int wantWrite = (blendMode != 0) ? 0 : 1;

		if (wantWrite != s_mergeDepthWriteState)
		{
			s_mergeDepthWriteState = wantWrite;
			N3DS_C3DSetDepthWrite(wantWrite);
		}
	}
	N3DS_C3DSetBlendMode(blendMode);
	s_runBlendMode = blendMode;
	N3DS_C3DBindAtlas(atlas);
	N3DS_C3DDrawTriangles(baseVertex, vertCount / 3);
	s_drawCallCount++;
	s_frameAtlasMask |= (1u << (atlas & 31));

	/* Stereo journal (n3ds_backend.h): one entry per emit that actually drew,
	 * with doubling and depth-write RESOLVED -- the g_dbg_noFx and
	 * fxNoDepthWrite policy above ran once, here, and the right-eye replay
	 * re-issues the outcome. vertCount is raw vertices, same as this call's
	 * own parameter (the replay's C3D_DrawArrays takes vertices directly).
	 * No-op unless this frame latched stereo. */
	N3DS_StereoRecordPacket(blendMode, doubling, (blendMode != 0) ? 0 : 1, atlas, baseVertex, vertCount);
}

/*
 * Closes the open blend run into a recorded segment (verts to the frame
 * VBO now, draw deferred to the barrier). Only DrawTriangles calls this --
 * the barrier itself emits the open run directly as its last draw, which
 * is what makes the capacity escalation below non-recursive.
 */
internal void N3DS_MergeCloseBlendRun(enum N3DS_BlendCloseCause cause)
{
	int baseVertex;

	if (s_runVertCount <= 0)
	{
		s_runAtlas = -1;
		return;
	}

	/* Counted before the escalation branch: the run really is being closed
	 * for this reason either way, and escalation is counted separately. */
	if (cause == N3DS_BLEND_CLOSE_ATLAS)
	{
		s_blendCloseAtlas++;
	}
	else
	{
		s_blendCloseState++;
	}

	if (s_mergeBlendSegCount >= N3DS_MERGE_MAX_BLEND_SEGS)
	{
		/* Mid-frame barrier is always legal (every later prim carries a
		 * larger ordinal) and drains the open run as its last draw. */
		s_mergeEscalations++;
		N3DS_MergeFlush();
		return;
	}

	baseVertex = N3DS_TvboCheckedAppend(s_convertScratch, s_runVertCount);
	if (baseVertex < 0)
	{
		s_droppedRuns++;
	}
	else
	{
		N3DS_MergeSegment *seg = &s_mergeBlendSegs[s_mergeBlendSegCount++];

		seg->blendMode = (u8)s_runBlendMode;
		seg->doubling = (u8)(s_runUntextured ? 0 : 1);
		seg->atlas = (u8)s_runAtlas;
		seg->baseVertex = baseVertex;
		seg->vertCount = s_runVertCount;
	}
	s_runVertCount = 0;
	s_runAtlas = -1;
}

/*
 * The barrier: everything deferred this frame reaches the command list.
 * Emission order is the correctness argument in the state block's header
 * comment: opaque (bins, then spilled segments) first -- internally
 * order-free, resolved by the depth test -- then blend segments verbatim,
 * then the still-open blend run, which is by construction the latest blend
 * content.
 */
internal void N3DS_MergeFlush(void)
{
	int lastDoubling = -1;
	int i;
	/*
	 * The still-open run's OWN state, snapshotted before anything else emits.
	 * N3DS_MergeEmit assigns s_runBlendMode as a side effect, so by the time
	 * the trailing-run block below runs, the opaque bin emits have already
	 * overwritten it with BM_NONE. s_runUntextured is not clobbered the same
	 * way, but is snapshotted with it so the pair is read from one point in
	 * time.
	 */
	const int pendingBlendMode = s_runBlendMode;
	const int pendingUntextured = s_runUntextured;

	/* [MERGEDBG] removed: it existed to answer ONE question -- whether
	 * content reached the merge bins and was emitted, or never got binned. It
	 * answered it (emitted, and invisible), which located the uniform-register
	 * collision fixed in N3DS_BindPacketDrawState. Question closed, instrument
	 * deleted rather than left to accrete on a screen the owner has to read. */

	for (i = 0; i < N3DS_MERGE_BIN_COUNT; i++)
	{
		int baseVertex;

		if (s_mergeBinUsed[i] <= 0)
		{
			continue;
		}
		baseVertex = N3DS_TvboCheckedAppend(s_mergeBinVerts[i], s_mergeBinUsed[i]);
		if (baseVertex < 0)
		{
			s_droppedRuns++;
		}
		else
		{
			N3DS_MergeEmit(0 /* BM_NONE */, (i == N3DS_MERGE_UNTEX_BIN) ? 0 : 1,
			               (i == N3DS_MERGE_UNTEX_BIN) ? N3DS_WHITE_ATLAS : i, baseVertex, s_mergeBinUsed[i], &lastDoubling);
			s_mergeEmitOpaque++;
			s_mergeVertsOpaque += (u32)s_mergeBinUsed[i];
		}
		s_mergeBinUsed[i] = 0;
	}

	for (i = 0; i < s_mergeOpaqueSegCount; i++)
	{
		const N3DS_MergeSegment *seg = &s_mergeOpaqueSegs[i];

		N3DS_MergeEmit((int)seg->blendMode, (int)seg->doubling, (int)seg->atlas, seg->baseVertex, seg->vertCount, &lastDoubling);
		s_mergeEmitOpaque++;
		s_mergeVertsOpaque += (u32)seg->vertCount;
	}
	s_mergeOpaqueSegCount = 0;

	for (i = 0; i < s_mergeBlendSegCount; i++)
	{
		const N3DS_MergeSegment *seg = &s_mergeBlendSegs[i];

		N3DS_MergeEmit((int)seg->blendMode, (int)seg->doubling, (int)seg->atlas, seg->baseVertex, seg->vertCount, &lastDoubling);
		s_mergeEmitBlend++;
		s_mergeVertsBlend += (u32)seg->vertCount;
	}
	s_mergeBlendSegCount = 0;

	if (s_runVertCount > 0)
	{
		int baseVertex = N3DS_TvboCheckedAppend(s_convertScratch, s_runVertCount);

		if (baseVertex < 0)
		{
			s_droppedRuns++;
		}
		else
		{
			N3DS_MergeEmit(pendingBlendMode, pendingUntextured ? 0 : 1, s_runAtlas, baseVertex, s_runVertCount, &lastDoubling);
			s_mergeEmitBlend++;
			s_mergeVertsBlend += (u32)s_runVertCount;
			s_blendCloseFrameEnd++;
		}
	}
	s_runVertCount = 0;
	s_runAtlas = -1;
	/* TEV doubling was left wherever the last emit put it; -1 forces the
	 * next consumer (either path) to re-assert it. */
	s_runUntextured = -1;
}

/* s46: draw the accumulated backdrop fills, immediately BEFORE N3DS_DrawSkyPass
 * (equal far-pin depth + LEQUAL means the later sky draw layers over this).
 * Runs earliest of all, so on frames where it has content IT owns the
 * close-the-open-blend-run duty -- emitting over an open run would re-label
 * that run opaque (the s46 minimap black box). Same emit triple as the sky
 * pass. */
internal void N3DS_DrawBackdropPass(void)
{
	int baseVertex;
	int lastDoubling = -1;

	if (s_backdropVertUsed <= 0)
	{
		return;
	}
	N3DS_MergeCloseBlendRun(N3DS_BLEND_CLOSE_STATE);
	baseVertex = N3DS_TvboCheckedAppend(s_backdropVerts, s_backdropVertUsed);
	/* baseVertex > 0 doubles as the splash-frame gate: boot/load-splash
	 * frames clear via the same FILL packets but must present with ZERO
	 * draws (the session-14 hang class lived in the draw path -- an
	 * ungated version of this pass hung Azahar boot at TenStages stage 5).
	 * Position 0 in the frame VBO means nothing else drew this frame,
	 * which is exactly that class; every real frame has walk content
	 * appended long before EndScene. Cheaper and more honest than the
	 * earlier requires-sky-content gate, which would have left a
	 * hypothetical dome-less-and-gradient-less level black where retail
	 * shows flat clearColor. */
	if (baseVertex > 0)
	{
		N3DS_MergeEmit(0, 0, N3DS_WHITE_ATLAS, baseVertex, s_backdropVertUsed, &lastDoubling);
	}
	s_backdropVertUsed = 0;
}

/* Draw the accumulated skybox triangles. Called from NativeRenderer_EndScene
 * BEFORE the resident track pass, so the translucent waterfalls (which write no
 * depth) paint over the sky instead of the sky painting over them. See the
 * s_skyVerts block above. Same emit triple as the UNTEX bin (BM_NONE, untextured,
 * white atlas); the local lastDoubling=-1 and N3DS_MergeEmit's depth-write memo
 * make it self-contained regardless of the walk's leftover state. */
#ifdef NATIVE_SKY_GPU
/*
 * The sky dome NativeGpu_SkyGpuSubmit recorded for the frame this EndScene
 * draws, as one model-program draw: the camera's rotation with no
 * translation, composed exactly as native_model_emit.c composes an instance
 * (mirror included), every vertex pinned behind all world content at the
 * infinite-distance parallax. It writes no depth, so the horizon glow and the
 * stars, far-pinned at the backdrop's depth and drawn after it, still layer
 * over it.
 */
internal int N3DS_SkyDomeGpuPending(void)
{
	return s_skyGpuSlot[s_skyRangeReadIdx].submitted;
}

internal void N3DS_DrawSkyDomeGpu(void)
{
	NativeSkyGpuSlot *slot = &s_skyGpuSlot[s_skyRangeReadIdx];
	float r[3][3], qx[4], qy[4], qw[4];
	float h, ofxOverH, ofyOverH;
	int baseVertex, row, col;

	slot->submitted = 0;
	if ((slot->vertCount < 3) || (slot->h <= 0))
	{
		return;
	}

	h = (float)slot->h;
	for (row = 0; row < 3; row++)
	{
		for (col = 0; col < 3; col++)
		{
			r[row][col] = (float)slot->rot[row][col] * (1.0f / 4096.0f);
		}
	}
	ofxOverH = (float)slot->ofx / h;
	ofyOverH = (float)slot->ofy / h;
	if (NativeMirror_WorldFrame())
	{
		/* mirror mode: x' = axisSum - x, the view-x row negated about the mirrored centre */
		ofxOverH = (NativeMirror_AxisSum() - (float)slot->ofx) / h;
		r[0][0] = -r[0][0];
		r[0][1] = -r[0][1];
		r[0][2] = -r[0][2];
	}
	for (col = 0; col < 3; col++)
	{
		qx[col] = r[0][col] + (ofxOverH * r[2][col]);
		qy[col] = r[1][col] + (ofyOverH * r[2][col]);
		qw[col] = r[2][col] / h;
	}
	qx[3] = 0.0f;
	qy[3] = 0.0f;
	qw[3] = 0.0f;

	baseVertex = N3DS_C3DAppendVertices(slot->verts, slot->vertCount);
	if (baseVertex < 0)
	{
		return;
	}
	N3DS_ModelDraw_Begin();
	N3DS_ModelDraw_InstanceSkyDome(qx, qy, qw, h);
	N3DS_ModelDraw_Run(baseVertex, slot->vertCount / 3, -1, 0, 0);
	N3DS_ModelDraw_End();
}
#else
internal int N3DS_SkyDomeGpuPending(void)
{
	return 0;
}

internal void N3DS_DrawSkyDomeGpu(void)
{
}
#endif

internal void N3DS_DrawSkyPass(void)
{
	int baseVertex;
	int split;
	int lastDoubling = -1;
	const int domePending = N3DS_SkyDomeGpuPending();

	if ((s_skyVertUsed <= 0) && !domePending)
	{
		return;
	}
	/* The walk's LAST blend run can still be open when EndScene reaches this
	 * pass -- in-race that run is the HUD map sheet's blended half, because UI
	 * is the tail of the walk. N3DS_MergeEmit below stamps s_runBlendMode = 0
	 * as a side effect, and N3DS_MergeCloseBlendRun reads s_runBlendMode at
	 * CLOSE time -- so emitting the sky over an open run re-labels that run
	 * opaque, and its STP texels replay solid black (the minimap black box).
	 * Close the run first, so it is recorded under the state it actually
	 * accumulated with. */
	N3DS_MergeCloseBlendRun(N3DS_BLEND_CLOSE_STATE);
	/* The dome takes the place retail's sky packets hold in walk order: over
	 * the horizon glow, under the stars (see s_skyVertDomeAt). */
	split = ((s_skyVertDomeAt >= 0) && (s_skyVertDomeAt < s_skyVertUsed)) ? s_skyVertDomeAt : s_skyVertUsed;
	baseVertex = (s_skyVertUsed > 0) ? N3DS_TvboCheckedAppend(s_skyVerts, s_skyVertUsed) : -1;
	if ((baseVertex >= 0) && (split > 0))
	{
		N3DS_MergeEmit(0, 0, N3DS_WHITE_ATLAS, baseVertex, split, &lastDoubling);
	}
	if (domePending)
	{
		N3DS_DrawSkyDomeGpu();
		lastDoubling = -1;
	}
	if ((baseVertex >= 0) && (s_skyVertUsed > split))
	{
		N3DS_MergeEmit(0, 0, N3DS_WHITE_ATLAS, baseVertex + split, s_skyVertUsed - split, &lastDoubling);
	}
	s_skyVertUsed = 0;
	s_skyVertDomeAt = -1;
}

/*
 * Ordered UI pass (see the s_uiOrd* block near the top for the whole design).
 *
 * Closes the open UI run into a recorded segment: vertices to the frame VBO now,
 * the draw itself deferred to N3DS_DrawUiPass so submission order is replayed
 * intact at the end of the frame. Same shape as N3DS_MergeCloseBlendRun, minus
 * the capacity escalation -- a mid-frame barrier is exactly what this pass must
 * NOT do, since drawing part of the UI early would put it under world content.
 * A full segment list therefore drops (counted) rather than reordering.
 */
internal void N3DS_UiOrderCloseRun(void)
{
	int baseVertex;

	if (s_uiOrdRunUsed <= 0)
	{
		s_uiOrdRunAtlas = -1;
		return;
	}
	if (s_uiOrdSegCount >= N3DS_UIORD_MAX_SEGS)
	{
		s_uiOrdDropped += (u32)(s_uiOrdRunUsed / 3);
		s_uiOrdRunUsed = 0;
		s_uiOrdRunAtlas = -1;
		return;
	}
	baseVertex = N3DS_TvboCheckedAppend(s_uiOrdRunVerts, s_uiOrdRunUsed);
	if (baseVertex < 0)
	{
		s_uiOrdDropped += (u32)(s_uiOrdRunUsed / 3);
	}
	else
	{
		N3DS_MergeSegment *seg = &s_uiOrdSegs[s_uiOrdSegCount];
		seg->blendMode = (u8)s_uiOrdRunBlend;
		seg->doubling = (u8)s_uiOrdRunDoubling;
		seg->atlas = (u8)s_uiOrdRunAtlas;
		seg->baseVertex = baseVertex;
		seg->vertCount = s_uiOrdRunUsed;

		s_uiOrdSegCount++;
	}
	s_uiOrdRunUsed = 0;
	s_uiOrdRunAtlas = -1;
}

/*
 * Reserve room for one UI triangle and return where to write its 3 vertices,
 * or NULL if it could not be recorded. Coalesces into the open run while
 * (blend, doubling, atlas) holds; any change closes the run first, which is what
 * keeps a text-heavy menu at a handful of draws instead of one per glyph.
 *
 * Called by BOTH UI producers -- NativeRenderer_DrawTriangles and the direct-emit
 * fast path -- during the OT walk, so appends arrive in parse order and the list
 * IS submission order.
 */
internal N3DS_Vertex *N3DS_UiOrderAppend(int blendMode, int doubling, int atlas)
{
	N3DS_Vertex *dst;

	if ((atlas != s_uiOrdRunAtlas) || (blendMode != s_uiOrdRunBlend) || (doubling != s_uiOrdRunDoubling) ||
	    ((s_uiOrdRunUsed + 3) > N3DS_UIORD_RUN_CAP))
	{
		N3DS_UiOrderCloseRun();
		s_uiOrdRunBlend = blendMode;
		s_uiOrdRunDoubling = doubling;
		s_uiOrdRunAtlas = atlas;
	}
	if ((s_uiOrdRunUsed + 3) > N3DS_UIORD_RUN_CAP)
	{
		/* Run buffer smaller than one triangle cannot happen (cap is a
		 * multiple of 3 >= 3); the close above always empties it. Guard
		 * anyway so a future cap change cannot overrun the buffer. */
		s_uiOrdDropped++;
		return NULL;
	}
	dst = &s_uiOrdRunVerts[s_uiOrdRunUsed];
	s_uiOrdRunUsed += 3;
	s_uiOrdPrims++;
	return dst;
}

/* Bottom-screen minimap: same run/segment shape as the ordered UI pass above
 * (see the s_btmm* block near the top), just a much smaller list and a union
 * bbox instead of per-segment boxes -- the box is a display-window input here,
 * not a diagnostic. */
internal void N3DS_BtmMapCloseRun(void)
{
	int baseVertex;

	if (s_btmmRunUsed <= 0)
	{
		s_btmmRunAtlas = -1;
		return;
	}
	if (s_btmmSegCount >= N3DS_BTMM_MAX_SEGS)
	{
		s_btmmDropped += (u32)(s_btmmRunUsed / 3);
		s_btmmDropSegFull += (u32)(s_btmmRunUsed / 3);
		s_btmmRunUsed = 0;
		s_btmmRunAtlas = -1;
		return;
	}
	baseVertex = N3DS_TvboCheckedAppend(s_btmmRunVerts, s_btmmRunUsed);
	if (baseVertex < 0)
	{
		s_btmmDropped += (u32)(s_btmmRunUsed / 3);
		s_btmmDropVbo += (u32)(s_btmmRunUsed / 3);
	}
	else
	{
		N3DS_MergeSegment *seg = &s_btmmSegs[s_btmmSegCount];
		int k;

		seg->blendMode = (u8)s_btmmRunBlend;
		seg->doubling = (u8)s_btmmRunDoubling;
		seg->atlas = (u8)s_btmmRunAtlas;
		seg->baseVertex = baseVertex;
		seg->vertCount = s_btmmRunUsed;
		s_btmmSegCount++;

		for (k = 0; k < s_btmmRunUsed; k++)
		{
			const short vx = (short)s_btmmRunVerts[k].x;
			const short vy = (short)s_btmmRunVerts[k].y;

			if (!s_btmmFrameHasBox)
			{
				s_btmmFrameBox[0] = vx;
				s_btmmFrameBox[1] = vy;
				s_btmmFrameBox[2] = vx;
				s_btmmFrameBox[3] = vy;
				s_btmmFrameHasBox = 1;
			}
			else
			{
				if (vx < s_btmmFrameBox[0]) { s_btmmFrameBox[0] = vx; }
				if (vy < s_btmmFrameBox[1]) { s_btmmFrameBox[1] = vy; }
				if (vx > s_btmmFrameBox[2]) { s_btmmFrameBox[2] = vx; }
				if (vy > s_btmmFrameBox[3]) { s_btmmFrameBox[3] = vy; }
			}
		}
	}
	s_btmmRunUsed = 0;
	s_btmmRunAtlas = -1;
}

internal N3DS_Vertex *N3DS_BtmMapAppend(int blendMode, int doubling, int atlas)
{
	N3DS_Vertex *dst;

	if ((atlas != s_btmmRunAtlas) || (blendMode != s_btmmRunBlend) || (doubling != s_btmmRunDoubling) ||
	    ((s_btmmRunUsed + 3) > N3DS_BTMM_RUN_CAP))
	{
		N3DS_BtmMapCloseRun();
		s_btmmRunBlend = blendMode;
		s_btmmRunDoubling = doubling;
		s_btmmRunAtlas = atlas;
	}
	if ((s_btmmRunUsed + 3) > N3DS_BTMM_RUN_CAP)
	{
		s_btmmDropped++;
		s_btmmDropRunCap++;
		return NULL;
	}
	dst = &s_btmmRunVerts[s_btmmRunUsed];
	s_btmmRunUsed += 3;
	return dst;
}

/* FPS-overlay twin of the pair above (see the s_btmf* block). */
internal void N3DS_BtmFpsCloseRun(void)
{
	int baseVertex;

	if (s_btmfRunUsed <= 0)
	{
		s_btmfRunAtlas = -1;
		return;
	}
	if (s_btmfSegCount >= N3DS_BTMF_MAX_SEGS)
	{
		s_btmmDropped += (u32)(s_btmfRunUsed / 3);
		s_btmfDropSegFull += (u32)(s_btmfRunUsed / 3);
		s_btmfRunUsed = 0;
		s_btmfRunAtlas = -1;
		return;
	}
	baseVertex = N3DS_TvboCheckedAppend(s_btmfRunVerts, s_btmfRunUsed);
	if (baseVertex < 0)
	{
		s_btmmDropped += (u32)(s_btmfRunUsed / 3);
		s_btmfDropVbo += (u32)(s_btmfRunUsed / 3);
	}
	else
	{
		N3DS_MergeSegment *seg = &s_btmfSegs[s_btmfSegCount];
		int k;

		seg->blendMode = (u8)s_btmfRunBlend;
		seg->doubling = (u8)s_btmfRunDoubling;
		seg->atlas = (u8)s_btmfRunAtlas;
		seg->baseVertex = baseVertex;
		seg->vertCount = s_btmfRunUsed;
		s_btmfSegCount++;

		for (k = 0; k < s_btmfRunUsed; k++)
		{
			const short vx = (short)s_btmfRunVerts[k].x;
			const short vy = (short)s_btmfRunVerts[k].y;

			if (!s_btmfFrameHasBox)
			{
				s_btmfFrameBox[0] = vx;
				s_btmfFrameBox[1] = vy;
				s_btmfFrameBox[2] = vx;
				s_btmfFrameBox[3] = vy;
				s_btmfFrameHasBox = 1;
			}
			else
			{
				if (vx < s_btmfFrameBox[0]) { s_btmfFrameBox[0] = vx; }
				if (vy < s_btmfFrameBox[1]) { s_btmfFrameBox[1] = vy; }
				if (vx > s_btmfFrameBox[2]) { s_btmfFrameBox[2] = vx; }
				if (vy > s_btmfFrameBox[3]) { s_btmfFrameBox[3] = vy; }
			}
		}
	}
	s_btmfRunUsed = 0;
	s_btmfRunAtlas = -1;
}

internal N3DS_Vertex *N3DS_BtmFpsAppend(int blendMode, int doubling, int atlas)
{
	N3DS_Vertex *dst;

	if ((atlas != s_btmfRunAtlas) || (blendMode != s_btmfRunBlend) || (doubling != s_btmfRunDoubling) ||
	    ((s_btmfRunUsed + 3) > N3DS_BTMF_RUN_CAP))
	{
		N3DS_BtmFpsCloseRun();
		s_btmfRunBlend = blendMode;
		s_btmfRunDoubling = doubling;
		s_btmfRunAtlas = atlas;
	}
	if ((s_btmfRunUsed + 3) > N3DS_BTMF_RUN_CAP)
	{
		s_btmmDropped++;
		s_btmfDropRunCap++;
		return NULL;
	}
	dst = &s_btmfRunVerts[s_btmfRunUsed];
	s_btmfRunUsed += 3;
	return dst;
}

/*
 * Draw the frame's UI, in submission order, LAST -- after the merge barrier, the
 * model pass and the deferred smoke, so nothing can paint over it. Every prim
 * carries the same pinned stamp, so GPU_LEQUAL passes on equal and the later
 * draw wins: painter's order, which is what retail does and what the depth-test
 * ordering could never reliably reproduce.
 *
 * Emits through N3DS_MergeEmit so the stereo journal records these draws too and
 * the right-eye replay reissues them identically. lastDoubling = -1 makes the
 * pass self-contained against whatever state the model/smoke passes left.
 */
internal void N3DS_DrawUiPass(void)
{
	int lastDoubling = -1;
	int i;

	N3DS_UiOrderCloseRun(); /* the frame's last run is still open */

	if (s_uiOrdPrims > s_uiOrdPrimsMax)
	{
		s_uiOrdPrimsMax = s_uiOrdPrims;
	}
	for (i = 0; i < s_uiOrdSegCount; i++)
	{
		const N3DS_MergeSegment *seg = &s_uiOrdSegs[i];

		N3DS_MergeEmit((int)seg->blendMode, (int)seg->doubling, (int)seg->atlas, seg->baseVertex, seg->vertCount, &lastDoubling);
	}
	s_uiOrdSegCount = 0;
	s_uiOrdPrims = 0;
	/* Leave the depth-write memo unknown: this pass may have emitted blended
	 * UI (write off), and the next frame's first consumer must reassert. */
	s_mergeDepthWriteState = -1;
}

/*
 * Draw the frame's peeled minimap segments onto the BOTTOM render target (see
 * the s_btmm* block near the top for the whole design). Called from EndScene
 * strictly after N3DS_StereoRenderRightEye (nothing here is journaled, so the
 * right eye can never replay a bottom-screen draw) and before N3DS_C3DEndFrame
 * (the draws must join this frame's command list, against this frame's VBO).
 * Unconditional at the call site: it drains and resets its own state even when
 * it draws nothing, so a toggle flip or a menu frame cannot leave stale
 * segments for the next frame.
 */
internal void N3DS_DrawBottomMapPass(void)
{
	int mapActive;
	int fpsActive;
	int active;

	N3DS_BtmMapCloseRun();
	N3DS_BtmFpsCloseRun();

	if (s_btmmSegCount > s_btmmSegsPeak)
	{
		s_btmmSegsPeak = s_btmmSegCount;
	}
	if (s_btmfSegCount > s_btmfSegsPeak)
	{
		s_btmfSegsPeak = s_btmfSegCount;
	}

	mapActive = (s_btmmSegCount > 0) && s_btmmFrameHasBox && g_dbg_btmMap;
	/* FPS overlay is ALWAYS-ON (owner call): any frame that emitted it draws
	 * it -- menus, loads, pauses, races -- with or without the map. No stats-
	 * console carve-out, also owner call ("useless to me, drawn in an
	 * expensive way"): the glyph drip survives only as a pre-game boot-error
	 * surface, and with this pass owning the panel every frame it effectively
	 * never paints again. */
	fpsActive = (s_btmfSegCount > 0) && s_btmfFrameHasBox && g_dbg_btmMap;
	active = mapActive || fpsActive;
	if (active)
	{
		const int dspW = (s_projectionDispW > 0) ? s_projectionDispW : 512;
		const int dspH = (s_projectionDispH > 0) ? s_projectionDispH : 216;
		/* Begin()'s window: the map's fitted window when the map is on screen;
		 * a plain full-display window on FPS-only frames (the FPS block aims
		 * its own window either way, and the clear doesn't care). */
		int wl = 0;
		int wt = 0;
		int wr = dspW;
		int wb = dspH;

		if (mapActive)
		{
			/* Raw window: the frame's own content box, padded so the sheet's
			 * outline never kisses the panel edge. */
			int x0 = (int)s_btmmFrameBox[0] - 8;
			int y0 = (int)s_btmmFrameBox[1] - 8;
			int x1 = (int)s_btmmFrameBox[2] + 8;
			int y1 = (int)s_btmmFrameBox[3] + 8;
			int w;
			int h;

			if (!s_btmmWinValid)
			{
				s_btmmWin[0] = (short)x0;
				s_btmmWin[1] = (short)y0;
				s_btmmWin[2] = (short)x1;
				s_btmmWin[3] = (short)y1;
				s_btmmWinValid = 1;
			}
			else
			{
				/* Re-center on a jump (map repositioned: 1P<->3P, mode change);
				 * otherwise grow-only, so wandering racer dots can enlarge the
				 * window but never make it shimmy. */
				const int wcx = ((int)s_btmmWin[0] + (int)s_btmmWin[2]) / 2;
				const int wcy = ((int)s_btmmWin[1] + (int)s_btmmWin[3]) / 2;
				const int rcx = (x0 + x1) / 2;
				const int rcy = (y0 + y1) / 2;
				int dx = wcx - rcx;
				int dy = wcy - rcy;

				if (dx < 0) { dx = -dx; }
				if (dy < 0) { dy = -dy; }
				if ((dx > 48) || (dy > 48))
				{
					s_btmmWin[0] = (short)x0;
					s_btmmWin[1] = (short)y0;
					s_btmmWin[2] = (short)x1;
					s_btmmWin[3] = (short)y1;
				}
				else
				{
					if (x0 < (int)s_btmmWin[0]) { s_btmmWin[0] = (short)x0; }
					if (y0 < (int)s_btmmWin[1]) { s_btmmWin[1] = (short)y0; }
					if (x1 > (int)s_btmmWin[2]) { s_btmmWin[2] = (short)x1; }
					if (y1 > (int)s_btmmWin[3]) { s_btmmWin[3] = (short)y1; }
				}
			}

			/* Expand the latched window so the map keeps its APPARENT
			 * proportions. PSX units are anamorphic -- the game authors for a
			 * dispW x dispH (512x216) display stretched onto a 4:3 screen, so
			 * horizontal units are ~1.78x denser than vertical ones. A window
			 * shaped 4:3 in raw units therefore rendered every map ~1.6x too
			 * wide (owner-caught). Matching the window's unit ratio to the
			 * display's own (dispW:dispH) reproduces the top screen's pixel
			 * aspect exactly, for any track and either widescreen mode. */
			wl = (int)s_btmmWin[0];
			wt = (int)s_btmmWin[1];
			wr = (int)s_btmmWin[2];
			wb = (int)s_btmmWin[3];
			w = wr - wl;
			h = wb - wt;

			if ((w * dspH) >= (h * dspW))
			{
				const int grow = (((w * dspH) + dspW - 1) / dspW) - h;

				wt -= grow / 2;
				wb += grow - (grow / 2);
			}
			else
			{
				const int grow = (((h * dspW) + dspH - 1) / dspH) - w;

				wl -= grow / 2;
				wr += grow - (grow / 2);
			}
		}

		if (N3DS_BottomMapDraw_Begin((float)wl, (float)wr, (float)wt, (float)wb))
		{
			int lastDoubling = -1;
			int lastDepthWrite = -1;
			int i;

			for (i = 0; i < s_btmmSegCount; i++)
			{
				const N3DS_MergeSegment *seg = &s_btmmSegs[i];
				const int wantWrite = (seg->blendMode != 0) ? 0 : 1;

				if ((int)seg->doubling != lastDoubling)
				{
					N3DS_C3DSetColorDoubling((int)seg->doubling);
					lastDoubling = (int)seg->doubling;
				}
				if (wantWrite != lastDepthWrite)
				{
					N3DS_C3DSetDepthWrite(wantWrite);
					lastDepthWrite = wantWrite;
				}
				N3DS_C3DSetBlendMode((int)seg->blendMode);
				N3DS_C3DBindAtlas((int)seg->atlas);
				N3DS_C3DDrawTriangles(seg->baseVertex, seg->vertCount / 3);
			}

			if (fpsActive)
			{
					/* FPS overlay through its OWN window: one display's worth
				 * of units across the panel = the text renders at its
				 * top-screen size (scale 1.0; raise for bigger), anchored
				 * so its own top-RIGHT corner lands 6px off the panel's
				 * right edge, 4px down (owner call). Its stamp is nearer
				 * than the map's, so it wins the depth test over the map
				 * pixels it overlays. */
				const float fscale = 1.0f;
				const float fw = (float)dspW / fscale;
				const float fh = (float)dspH / fscale;
				const float fl = (float)s_btmfFrameBox[2] - fw + (6.0f * fw / 320.0f);
				const float ft = (float)s_btmfFrameBox[1] - (4.0f * fh / 240.0f);

				s_btmfWinDbg[0] = (short)fl;
				s_btmfWinDbg[1] = (short)ft;
				s_btmfWinDbg[2] = (short)(fl + fw);
				s_btmfWinDbg[3] = (short)(ft + fh);
				N3DS_BottomMapDraw_SetWindow(fl, fl + fw, ft, ft + fh);
				for (i = 0; i < s_btmfSegCount; i++)
				{
					const N3DS_MergeSegment *seg = &s_btmfSegs[i];
					const int wantWrite = (seg->blendMode != 0) ? 0 : 1;

					if ((int)seg->doubling != lastDoubling)
					{
						N3DS_C3DSetColorDoubling((int)seg->doubling);
						lastDoubling = (int)seg->doubling;
					}
					if (wantWrite != lastDepthWrite)
					{
						N3DS_C3DSetDepthWrite(wantWrite);
						lastDepthWrite = wantWrite;
					}
					N3DS_C3DSetBlendMode((int)seg->blendMode);
					N3DS_C3DBindAtlas((int)seg->atlas);
					N3DS_C3DDrawTriangles(seg->baseVertex, seg->vertCount / 3);
				}
			}
			N3DS_BottomMapDraw_End();
			/* This pass touched doubling/depth-write/blend behind the
			 * frame memos; force the next frame's first consumer to
			 * reassert, same rule as the UI pass above. */
			s_mergeDepthWriteState = -1;
		}
		else
		{
			/* No bottom target (allocation failed, logged by the backend):
			 * stop retagging so the map returns to the top screen instead
			 * of vanishing. */
			g_dbg_btmMap = 0;
			active = 0;
		}
	}

	if (active != s_btmmWasActive)
	{
		char line[224];

		if (active)
		{
			snprintf(line, sizeof(line),
			         "[BTMMAP] on: segs=%d box=%d,%d..%d,%d win=%d,%d..%d,%d fpsSegs=%d fbox=%d,%d..%d,%d fwin=%d,%d..%d,%d retail=%d both=%d copies=%lu",
			         s_btmmSegCount, (int)s_btmmFrameBox[0], (int)s_btmmFrameBox[1], (int)s_btmmFrameBox[2], (int)s_btmmFrameBox[3],
			         (int)s_btmmWin[0], (int)s_btmmWin[1], (int)s_btmmWin[2], (int)s_btmmWin[3], s_btmfSegCount,
			         s_btmfFrameHasBox ? (int)s_btmfFrameBox[0] : 0, s_btmfFrameHasBox ? (int)s_btmfFrameBox[1] : 0,
			         s_btmfFrameHasBox ? (int)s_btmfFrameBox[2] : 0, s_btmfFrameHasBox ? (int)s_btmfFrameBox[3] : 0, (int)s_btmfWinDbg[0],
			         (int)s_btmfWinDbg[1], (int)s_btmfWinDbg[2], (int)s_btmfWinDbg[3], NativeGpu_UiMapRetailTop(), g_dbg_mapBoth,
			         (unsigned long)s_mbTopCopyTris);
		}
		else
		{
			snprintf(line, sizeof(line), "[BTMMAP] off (dropped=%lu src mSeg=%lu mRun=%lu mVbo=%lu fSeg=%lu fRun=%lu fVbo=%lu peak m=%d f=%d)",
			         (unsigned long)s_btmmDropped, (unsigned long)s_btmmDropSegFull, (unsigned long)s_btmmDropRunCap,
			         (unsigned long)s_btmmDropVbo, (unsigned long)s_btmfDropSegFull, (unsigned long)s_btmfDropRunCap,
			         (unsigned long)s_btmfDropVbo, s_btmmSegsPeak, s_btmfSegsPeak);
		}
		N3DS_DebugLog(line);
		s_btmmWasActive = active;
		if (!active)
		{
			/* The off line just reported this activation span's peak segment
			 * counts; zero them so the next span reports its own peak. Left as a
			 * boot-lifetime max, peak m= reads 0 at every pre-race FPS-only
			 * toggle (map segs=0) and gets misread as "0 across a race the map
			 * drew" -- the counter is right, its scope was wrong. */
			s_btmmSegsPeak = 0;
			s_btmfSegsPeak = 0;
		}
	}
	if (!mapActive)
	{
		/* A race's window has no business surviving into the next one: the
		 * sheet size is per-track, and menus always sit between (during which
		 * the pass now stays active for the FPS overlay alone). */
		s_btmmWinValid = 0;
	}

	s_btmmSegCount = 0;
	s_btmmFrameHasBox = 0;
	s_btmfSegCount = 0;
	s_btmfFrameHasBox = 0;
}

/* A bin hit capacity mid-frame: spill it as one opaque segment (order-free,
 * so WHERE it lands in the segment list is irrelevant) and keep binning. */
internal void N3DS_MergeSpillBin(int bin)
{
	int baseVertex;

	s_mergeBinSpills++;
	if (s_mergeOpaqueSegCount >= N3DS_MERGE_MAX_OPAQUE_SEGS)
	{
		s_mergeEscalations++;
		N3DS_MergeFlush(); /* drains every bin, including this one */
		return;
	}

	baseVertex = N3DS_TvboCheckedAppend(s_mergeBinVerts[bin], s_mergeBinUsed[bin]);
	if (baseVertex < 0)
	{
		s_droppedRuns++;
	}
	else
	{
		N3DS_MergeSegment *seg = &s_mergeOpaqueSegs[s_mergeOpaqueSegCount++];

		seg->blendMode = 0; /* BM_NONE */
		seg->doubling = (u8)((bin == N3DS_MERGE_UNTEX_BIN) ? 0 : 1);
		seg->atlas = (u8)((bin == N3DS_MERGE_UNTEX_BIN) ? N3DS_WHITE_ATLAS : bin);
		seg->baseVertex = baseVertex;
		seg->vertCount = s_mergeBinUsed[bin];
	}
	s_mergeBinUsed[bin] = 0;
}

/* This codebase has no 3-way min/max anywhere -- native_libgte.c's
 * fst_min/fst_max are 2-argument and, being defined in a different
 * translation unit's file scope, not usable from here regardless. Trivial
 * enough not to warrant sharing; kept local to where N3DS_ResolveTriangleTexture
 * needs them. */
internal inline int MIN3(int a, int b, int c)
{
	return (a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c);
}

internal inline int MAX3(int a, int b, int c)
{
	return (a > b) ? ((a > c) ? a : c) : ((b > c) ? b : c);
}

/*
 * Converts one triangle's three GrVertex into dst[0..2] (any destination:
 * the pending-run scratch, or a merge bin). Extracted from
 * the DrawTriangles loop so the merge path shares the exact
 * conversion -- especially the UV numerator formulas, whose history of
 * subtle wrongness (half-texel, V-flip) lives in the comments below. Keep
 * ONE copy of these formulas, ever.
 */
internal void N3DS_ConvertTriangleVerts(N3DS_Vertex *dst3, const GrVertex *triVerts, int untextured, int cellX, int cellY,
                                        int pixelBaseU, int pixelBaseV, int originU, int originV)
{
	int v;

	/* Far-edge UV inset for the map-sheet sprite classes: the in-race map
	 * (PKTDEPTH_UI_STAMP_BTMMAP, both routes -- bottom panel and top
	 * screen) and the never-routed picture sprites (PKTDEPTH_UI_STAMP_UIPIC:
	 * track-select and hub map sheets). Texel-centre numerators make a quad's sampled
	 * span [a0+0.5, a1+0.5], so any non-1:1-scaled sprite shows a
	 * half-texel band of the texel PAST its far edge -- for the map
	 * sheet's two stacked half-icons that band is whatever VRAM holds
	 * below each icon, which adds nothing under the map's additive blend
	 * and reads as a dark seam line between the halves. Pull the far-edge
	 * vertices (the triangle's per-axis maxima) back a whole texel to
	 * a1-0.5: nothing past the authored rect is ever sampled. The NEAR
	 * edge keeps the centre form -- stepping outward there can cross into
	 * a neighbouring decode cell when the rect starts on a cell boundary.
	 * Map class only: on 1:1 glyph text this gradient change would
	 * duplicate rows. */
	int insetMaxU = -1;
	int insetMaxV = -1;

	if (!untextured && ((triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_BTMMAP) || (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_UIPIC) ||
	                    (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_HUDSHINE)))
	{
		int minU = triVerts[0].u, maxU = triVerts[0].u;
		int minV = triVerts[0].v, maxV = triVerts[0].v;
		int k;

		for (k = 1; k < 3; k++)
		{
			if (triVerts[k].u < minU)
			{
				minU = triVerts[k].u;
			}
			if (triVerts[k].u > maxU)
			{
				maxU = triVerts[k].u;
			}
			if (triVerts[k].v < minV)
			{
				minV = triVerts[k].v;
			}
			if (triVerts[k].v > maxV)
			{
				maxV = triVerts[k].v;
			}
		}
		/* Degenerate axes (all three verts on one texel row/column) keep
		 * the centre form: insetting there would invert the span. */
		if (maxU > minU)
		{
			insetMaxU = maxU;
		}
		if (maxV > minV)
		{
			insetMaxV = maxV;
		}
		/* HUD shine: four mirrored quadrant quads meeting at their far edges,
		 * inset on every axis. Retail overlaps each mirrored pair by one unit
		 * per whole unit of scale to hide the PS1's past-edge first column;
		 * under texel-centre sampling that overlap draws the seam column twice
		 * (the subtractive pass darkens it), so the game drops it whenever
		 * NativeUiShine_ExactTiling (native_gpu.c) reports this inset live. */
	}

	for (v = 0; v < 3; v++)
	{
		const GrVertex *src = &triVerts[v];
		N3DS_Vertex *dst = &dst3[v];
		/* Vertex colour is the raw PSX colour only. The "bright" x2
		 * multiplier (MakeTexcoordQuad/Triangle/Rect set bright=2 for
		 * every textured primitive, MakeTexcoordZero* set 1 for every
		 * untextured one -- native_gpu.c) is applied by the TEV output
		 * scale instead, AFTER the texture modulate, matching PC. Baking
		 * it here clamped the colour to 1.0 before the multiply and lost
		 * all the headroom above it, which is what made textured surfaces
		 * render darker than PC. See N3DS_C3DSetColorDoubling. */

		{
			/* Widescreen UI relocation: UI-band verts (raw stamp
			 * 0xFFC0.. = the same d <= 63 band the stereo mask flattens)
			 * get their X remapped toward the wide window edges -- see
			 * NativeWide_UiShiftX (native_gpu.c) for the ramp and why it
			 * cannot crack sprites. Gated on packet depth being live:
			 * with it off the stamp is an emit ordinal, not a depth, and
			 * the band test would misfire. No-op whenever wide is off. */
			extern int NativeWide_UiShiftX(int x);
			extern int g_dbg_packetDepth;
			s16 sx = (s16)src->x;

			if (g_dbg_packetDepth && (src->emitOrdinal >= 0xFFC0u))
			{
				sx = (s16)NativeWide_UiShiftX((int)sx);

				/* Full-window fade: the ramp lands its window-edge vertices
				 * exactly on the wide window's edges, where the UI pop-out
				 * shifts one eye's copy inward and bares a strip. Push
				 * vertices ON the 512-wide window's outer edges a further
				 * stereo pad outward; the viewport clips the overshoot.
				 * Inner edges (split-screen windows) stay put. */
				if (src->emitOrdinal == PKTDEPTH_UI_STAMP_SCREENFADE)
				{
					const int pad = N3DS_StereoSkyPadX();

					if (src->x <= 0)
					{
						sx = (s16)(sx - pad);
					}
					else if (src->x >= 512)
					{
						sx = (s16)(sx + pad);
					}
				}
			}
			dst->x = sx;
		}
		dst->y = (s16)src->y;
		/* Emit-ordinal depth substrate, biased into s16 (see
		 * n3ds_backend.h). Later-parsed prims get larger z. */
		dst->z = (s16)((int)src->emitOrdinal - 32768);
		/* Parallax channel, biased identically (s40b channel split). Equal to
		 * z for everything except the checkered flag, whose stereo depth is
		 * deliberately not the depth it occludes by.
		 * Read by ctr.v.pica as inpos.w. */
		dst->w = (s16)((int)src->parallaxOrdinal - 32768);

		/* Offset into the resolved region of the atlas, not the whole
		 * atlas. Untextured uses the fixed white cell's centre
		 * (cellX,cellY); textured uses pixelBaseU/V -- a whole 256x256
		 * full-page cell or a 64x64 sub-page sub-cell, whichever
		 * N3DS_ResolveTriangleTexture resolved -- plus the triangle's own
		 * (u,v) re-based against originU/V (0 on the full-page path, so
		 * this reduces to exactly the old cellX*256+src->u there). Stored
		 * as the exact integer atlas-texel numerator, range [0,1023] --
		 * comfortably inside s16 -- with the old /N3DS_ATLAS_DIM divide
		 * moved into the shader (ctr.v.pica's texcoordScale, design doc
		 * finding 2, Option A) instead of done here. Do NOT divide here
		 * too -- that would double-scale every UV. */
		/*
		 * Texel-CENTER numerators: 2*texel + 1, scaled by 1/2048 in the
		 * shader, so the GPU samples at (texel + 0.5) -- the dead centre
		 * of the intended texel, where nearest-neighbour needs no
		 * boundary tie-break at all.
		 *
		 * CORRECTED RECORD. The previous 2*texel - 1 form
		 * ("half-texel", e7ffe8d) cited PC vertices carrying tcx=tcy=-1;
		 * that claim was wrong -- PC's tcx/tcy block is COMMENTED OUT
		 * (native_gpu.c, g_cfg_bilinearFiltering), so PC simply floors
		 * the centre-interpolated UV nudged by a small positive fudge
		 * (c_UVFudge). Sampling at (texel - 0.5) put every 1:1-mapped
		 * fragment (font glyphs, sprites, UI quads -- integer UV deltas
		 * matching integer pixel deltas) EXACTLY ON a texel boundary,
		 * where the pick is ambiguous, and at a sub-page tile's left/top
		 * edge the losing side is the NEIGHBOURING 64x64 cache cell -- a
		 * different texture entirely. Fresh-boot Azahar capture that led
		 * here: VRAM byte-identical to PC, every fresh cache cell
		 * byte-correct, screen still quilted garbage -- content was
		 * fine, addressing was not.
		 */
		/*
		 * V FLIP -- proven by direct observation (atlas
		 * inspector, dumpreq 'atlasview'): the GPU's live view of an
		 * atlas is the CPU-side memory VERTICALLY MIRRORED (white cell
		 * at memory top renders at screen bottom; cell rows in exact
		 * reverse order). PICA T=0 reads our tiler's LAST row: the
		 * tex3ds parity probe used `tex3ds -r` (raw, unflipped), so it
		 * validated Morton/tile math while silently inheriting the
		 * wrong orientation, and PresentVRAMRect's empirical quad-V
		 * swap was this same fact discovered locally.
		 * Sample-side fix: numerator 2048-(2a+1) = 2047-2a targets
		 * memory row a from the bottom-up T axis. Applies to the white
		 * cell too -- the earlier "white renders fine unflipped"
		 * counterargument fails because unflipped sampling read cell
		 * (0,3)'s lightish content, which multiplies vertex colors
		 * plausibly enough to pass eyeballing; the inspector picture
		 * is the ground truth. U is NOT flipped (horizontal order
		 * matches between the views).
		 */
		if (untextured)
		{
			dst->u = (s16)(((cellX * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) + 1);
			dst->v = (s16)(2047 - ((cellY * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1));
		}
		else
		{
			int nu = ((pixelBaseU + ((int)src->u - originU)) << 1) + 1;
			int nv = 2047 - ((pixelBaseV + ((int)src->v - originV)) << 1);

			/* One texel = 2 numerator units; the V axis is flipped, so
			 * "back toward the near edge" is -2 in U but +2 in V. */
			if ((int)src->u == insetMaxU)
			{
				nu -= 2;
			}
			if ((int)src->v == insetMaxV)
			{
				nv += 2;
			}

			dst->u = (s16)nu;
			dst->v = (s16)nv;
		}

		/* Raw u8 copy, no scale -- the *1/255 reciprocal-multiply that
		 * used to live here (and the hot-path reasoning for why it was a
		 * multiply, not a divide) moved into the shader's colorScale
		 * constant instead (design doc finding 3). Alpha is no longer a
		 * struct field: color is fetched at count=3 (RGB only) now, and
		 * the shader writes a constant 1.0 for alpha -- dst->a was
		 * already a hardcoded 1.0f here, never read from src->a, so
		 * dropping the field loses no real value. */
		dst->r = src->r;
		dst->g = src->g;
		dst->b = src->b;
	}
}

/*
 * Routes one triangle to the sub-page tier (common case: its UV bounding box
 * fits one 64x64 grid-aligned tile) or the full-page tier (fallback: it
 * straddles a tile boundary or is simply larger than 64x64 -- e.g. a large
 * floor/wall/sky surface). GrVertex.u/.v are raw u8 (0-255) with no alignment
 * guarantee, so this bbox-vs-tile test must run per triangle, not once per
 * (page,clut): two triangles sharing a texture can still have different UV
 * footprints. Both branches resolve to
 * the same output shape (pixelBase/origin), so the caller's UV assignment is
 * one formula for both tiers -- see NativeRenderer_DrawTriangles.
 */
/*
 * Framebuffer-capture region (the N3DS_FBCAP_* reserved cells above).
 *
 * Display-region texturing -- heat-warp rings (game/Torch.c), the pause
 * background, menu feedback blur -- samples the last PRESENTED frame. The
 * mirror+decode path serves that by re-packing the presented frame into the
 * PSX VRAM mirror and decoding framebuffer pages through the LRU cache:
 * per-page decodes whose keys churn every frame, evicting the live working
 * set (measured: heat-warp windows tripled the track walk's resolve cost
 * while the GPU sat idle). This path instead captures the presented frame
 * ONCE per frame into the reserved atlas region and resolves qualifying
 * triangles straight at it -- same convert formula, same merge/blend/stereo
 * machinery, one texture state shared by every such triangle.
 *
 * One capture rect per frame: a second consumer wanting a DIFFERENT display
 * rect in the same frame falls back to the mirror path rather than
 * overwriting a region already-queued draws reference.
 */
global_variable u32 s_fbCapFrame;
global_variable int s_fbCapValid;
global_variable s16 s_fbCapRectX, s_fbCapRectY, s_fbCapRectW, s_fbCapRectH;
global_variable u32 s_fbCaptures;
global_variable u32 s_fbCapRoutedTris;
/* Frame number of the most recent capture-routed triangle. */
global_variable u32 s_fbCapLastRoutedFrame;

internal int N3DS_FBCaptureEnsure(const N3DS_FBStoreSlot *slot)
{
	int w = slot->w, h = slot->h;
	int cell, row, col;
	/* Dims only, NOT the rect origin: the display rect alternates between
	 * the two buffers (y=0/y=296) every frame, but the capture holds ONE
	 * picture -- the presented frame -- and the consumer's pixelBase math
	 * subtracts its own slot origin, so the same capture serves either
	 * rect. Keying the memo on origin made the feedback hold below never
	 * match two frames running (the march survived it, owner-observed). */
	const int rectMatches = s_fbCapValid && (s_fbCapRectW == (s16)slot->w) && (s_fbCapRectH == (s16)slot->h);

	if (s_fbCapValid && (s_fbCapFrame == s_frameCounter))
	{
		/* Same-frame memo -- hit only when the rect matches; a different
		 * rect must NOT recapture (queued draws reference the region). */
		return rectMatches;
	}

	/*
	 * Feedback breaker: while consumers sampled the capture last frame (the
	 * effect is continuously live), HOLD the existing capture instead of
	 * re-capturing. The presented frame contains the effect's own output --
	 * re-ingesting it every frame made displaced copies of whatever the
	 * rings touched (HUD portraits, minimap) MARCH across the screen, one
	 * ring-displacement per frame (owner-observed; retail never had this
	 * because it sampled the frame mid-draw, before HUD or effect output
	 * existed in it). Freezing at effect onset serves a clean pre-effect
	 * picture for the effect's whole run: no feedback, no recognizable-copy
	 * artifacts; the cost is staleness bounded by the effect's duration.
	 * For the pause background this freeze IS the wanted semantics.
	 */
	if (rectMatches && ((s_fbCapLastRoutedFrame + 1u) >= s_frameCounter))
	{
		return 1;
	}

	if (w > (N3DS_FBCAP_RESERVED_CELLS * N3DS_DECODE_TEXTURE_DIM)) { w = N3DS_FBCAP_RESERVED_CELLS * N3DS_DECODE_TEXTURE_DIM; }
	if (h > N3DS_DECODE_TEXTURE_DIM) { h = N3DS_DECODE_TEXTURE_DIM; }
	if ((w <= 0) || (h <= 0))
	{
		return 0;
	}

	/* The capture is at the display rect's own PSX resolution, so capture
	 * texel (x,y) == VRAM texel (rect.x+x, rect.y+y) and the resolve's
	 * pixelBase arithmetic maps sampled coords 1:1. Borrows the mirror
	 * path's RGB scratch; both run on the walk thread, never concurrently. */
	if (!N3DS_CopyDisplayedTopFrame(s_fbCopyScratch, w, h))
	{
		return 0;
	}

	for (cell = 0; cell < N3DS_FBCAP_RESERVED_CELLS; cell++)
	{
		const int colBase = cell * N3DS_DECODE_TEXTURE_DIM;

		if (colBase >= w)
		{
			break;
		}

		/* Pack+tile one 256x256 cell from the RGB capture. Same memory
		 * orientation as N3DS_DecodeAndTilePage (source row v -> tile row v;
		 * the sampler's 2047-2a V flip handles PICA's bottom-up T axis).
		 * Out-of-rect texels get opaque black; nothing samples them (the
		 * routing test requires the footprint inside the display rect). */
		for (row = 0; row < N3DS_DECODE_TEXTURE_DIM; row++)
		{
			const u8 *srcRow = s_fbCopyScratch + ((size_t)row * (size_t)w * 3);

			for (col = 0; col < N3DS_DECODE_TEXTURE_DIM; col++)
			{
				const int srcX = colBase + col;
				u16 packed = 1u;
				int morton, tileIndex;

				if ((srcX < w) && (row < h))
				{
					const u8 *p = srcRow + (srcX * 3);

					packed = (u16)(((u16)(p[0] >> 3) << 11) | ((u16)(p[1] >> 3) << 6) | ((u16)(p[2] >> 3) << 1) | 1u);
				}

				morton = (col & 1) | ((row & 1) << 1) | ((col & 2) << 1) | ((row & 2) << 2) | ((col & 4) << 2) | ((row & 4) << 3);
				tileIndex = (((row >> 3) * (N3DS_DECODE_TEXTURE_DIM / 8)) + (col >> 3)) * 64;
				s_decodeScratch[tileIndex + morton] = packed;
			}
		}

		N3DS_C3DUploadDecodedCell(N3DS_FBCAP_ATLAS, N3DS_FBCAP_CELL_X + cell, N3DS_FBCAP_CELL_Y, s_decodeScratch);
	}

	s_fbCapValid = 1;
	s_fbCapFrame = s_frameCounter;
	s_fbCapRectX = (s16)slot->x;
	s_fbCapRectY = (s16)slot->y;
	s_fbCapRectW = (s16)slot->w;
	s_fbCapRectH = (s16)slot->h;
	s_fbCaptures++;

	if ((s_fbCaptures & 63u) == 1u)
	{
		char msg[96];

		snprintf(msg, sizeof(msg), "[CTR-3DS] fb-capture #%lu rect=%d,%d %dx%d routedTris=%lu", (unsigned long)s_fbCaptures, slot->x, slot->y, w, h,
		         (unsigned long)s_fbCapRoutedTris);
		N3DS_DebugLog(msg);
	}

	return 1;
}

internal int N3DS_ResolveTriangleTexture(const GrVertex *v0, const GrVertex *v1, const GrVertex *v2, s16 texPage,
                                          s16 clut, TexFormat format, N3DS_TexView view, int *outAtlasIdx,
                                          int *outPixelBaseU, int *outPixelBaseV, int *outOriginU, int *outOriginV)
{
	const int minU = MIN3(v0->u, v1->u, v2->u), maxU = MAX3(v0->u, v1->u, v2->u);
	const int minV = MIN3(v0->v, v1->v, v2->v), maxV = MAX3(v0->v, v1->v, v2->v);

	/*
	 * Display-region fast path: a 16-bit opaque-view triangle whose sampled
	 * footprint lies entirely inside an armed display rect reads the last
	 * presented frame -- serve it from the capture region instead of the
	 * mirror+decode path (see N3DS_FBCaptureEnsure). 16-bit only: display
	 * buffers are 1555, so framebuffer sampling always resolves texel==
	 * halfword; opaque view only (PASS1/PASS2 STP semantics differ from the
	 * capture's all-opaque texels -- those rare consumers keep the mirror
	 * path). Tested against the slot RECTS, not the stale flags: a rect
	 * stays a display buffer whether or not the mirror is current.
	 */
	if ((format == TF_16_BIT) && (view == N3DS_TEXVIEW_OPAQUE))
	{
		const int pageX = (texPage & 0xF) * 64;
		const int pageY = (texPage & 0x10) ? 256 : 0;
		const int absX0 = pageX + minU, absX1 = pageX + maxU;
		const int absY0 = pageY + minV, absY1 = pageY + maxV;
		int slotIdx;

		for (slotIdx = 0; slotIdx < 2; slotIdx++)
		{
			const N3DS_FBStoreSlot *slot = &s_fbStore[slotIdx];

			if ((slot->w > 0) && (absX0 >= slot->x) && (absX1 < (slot->x + slot->w)) && (absY0 >= slot->y) && (absY1 < (slot->y + slot->h)))
			{
				if (N3DS_FBCaptureEnsure(slot))
				{
					*outAtlasIdx = N3DS_FBCAP_ATLAS;
					*outPixelBaseU = N3DS_FBCAP_REGION_X + (pageX - slot->x);
					*outPixelBaseV = N3DS_FBCAP_REGION_Y + (pageY - slot->y);
					*outOriginU = 0;
					*outOriginV = 0;
					s_fbCapRoutedTris++;
					s_fbCapLastRoutedFrame = s_frameCounter;
					s_resolveLastTier = 0;
					return 0;
				}
				break;
			}
		}
	}

	/* Freeze-frame consumer trigger: this triangle is about to be decoded
	 * from VRAM the armed display rect covers (pause background, screen-copy
	 * wipes) -- refresh the mirror region first so the decode reads the last
	 * presented frame, not boot-era content. The test uses the triangle's
	 * actual sampled footprint (UV bbox mapped to VRAM halfwords), NOT the
	 * whole 256-tall page: CTR's display buffers sit at y=0..216 and
	 * y=296..512 with ordinary textures living in the y-gap between them, so
	 * any page anchored at y=0 overlaps the display rect even when every
	 * texel it actually samples lies below it -- the whole-page form of this
	 * test fired on essentially every frame of menu and race content
	 * (measured in Azahar). Costs a few integer ops per textured
	 * triangle when it doesn't fire. */
	if (s_fbMirrorStale && !g_dbg_skipFbTriTest)
	{
		const int pageX = (texPage & 0xF) * 64;
		const int pageY = (texPage & 0x10) ? 256 : 0;
		/* Texel -> VRAM halfword column: 4 texels/halfword at 4-bit, 2 at
		 * 8-bit, 1:1 at 16-bit. */
		const int shift = (format == TF_16_BIT) ? 0 : ((format == TF_8_BIT) ? 1 : 2);
		const int wordX0 = pageX + (minU >> shift);
		const int wordX1 = pageX + (maxU >> shift);

		N3DS_FBRefreshIfOverlaps(wordX0, pageY + minV, (wordX1 - wordX0) + 1, (maxV - minV) + 1);
	}
	const int tileX0 = minU / N3DS_SUBPAGE_DIM, tileX1 = maxU / N3DS_SUBPAGE_DIM;
	const int tileY0 = minV / N3DS_SUBPAGE_DIM, tileY1 = maxV / N3DS_SUBPAGE_DIM;

	if (!g_dbg_disableSubPage && (tileX0 == tileX1) && (tileY0 == tileY1))
	{
		int atlasIdx, physCellX, physCellY, subX, subY;
		const u32 stamp = s_decodeCacheEvictions + s_subPageCacheEvictions + s_vramDirtyBumps;

		if (s_rmemoValid && (s_rmemoTier == 1) && (s_rmemoStamp == stamp) && (s_rmemoPage == texPage) && (s_rmemoClut == clut) &&
		    (s_rmemoFormat == (u8)format) && (s_rmemoView == (u8)view) && (s_rmemoTileX == (s16)tileX0) && (s_rmemoTileY == (s16)tileY0))
		{
			*outAtlasIdx = s_rmemoAtlas;
			*outPixelBaseU = s_rmemoPbU;
			*outPixelBaseV = s_rmemoPbV;
			*outOriginU = tileX0 * N3DS_SUBPAGE_DIM;
			*outOriginV = tileY0 * N3DS_SUBPAGE_DIM;
			s_resolveMemoHits++;
			s_trianglesRoutedSubPage++;
			s_resolveLastTier = 1;
			s_resolveLastCell = s_rmemoCell;
			return 1;
		}

		{
			const int subCell = N3DS_FindOrDecodeSubPageTexture(texPage, clut, format, view, tileX0, tileY0, minV);

			N3DS_SubCellIndexToAtlasCoord(subCell, &atlasIdx, &physCellX, &physCellY, &subX, &subY);
			s_resolveLastTier = 1;
			s_resolveLastCell = subCell;
			s_rmemoCell = subCell;
		}
		*outAtlasIdx = atlasIdx;
		*outPixelBaseU = (physCellX * N3DS_DECODE_TEXTURE_DIM) + (subX * N3DS_SUBPAGE_DIM);
		*outPixelBaseV = (physCellY * N3DS_DECODE_TEXTURE_DIM) + (subY * N3DS_SUBPAGE_DIM);
		*outOriginU = tileX0 * N3DS_SUBPAGE_DIM;
		*outOriginV = tileY0 * N3DS_SUBPAGE_DIM;
		s_trianglesRoutedSubPage++;

		/* Stamp AFTER the decode: FindOrDecode may itself have evicted,
		 * which moves the sum -- a memo stored with the pre-decode stamp
		 * would die on its first probe. */
		s_rmemoValid = 1;
		s_rmemoTier = 1;
		s_rmemoPage = texPage;
		s_rmemoClut = clut;
		s_rmemoFormat = (u8)format;
		s_rmemoView = (u8)view;
		s_rmemoTileX = (s16)tileX0;
		s_rmemoTileY = (s16)tileY0;
		s_rmemoAtlas = *outAtlasIdx;
		s_rmemoPbU = *outPixelBaseU;
		s_rmemoPbV = *outPixelBaseV;
		s_rmemoStamp = s_decodeCacheEvictions + s_subPageCacheEvictions + s_vramDirtyBumps;
		return 1;
	}

	{
		int atlasIdx, cellX, cellY;
		const u32 stamp = s_decodeCacheEvictions + s_subPageCacheEvictions + s_vramDirtyBumps;

		if (s_rmemoValid && (s_rmemoTier == 2) && (s_rmemoStamp == stamp) && (s_rmemoPage == texPage) && (s_rmemoClut == clut) &&
		    (s_rmemoFormat == (u8)format) && (s_rmemoView == (u8)view))
		{
			*outAtlasIdx = s_rmemoAtlas;
			*outPixelBaseU = s_rmemoPbU;
			*outPixelBaseV = s_rmemoPbV;
			*outOriginU = 0;
			*outOriginV = 0;
			s_resolveMemoHits++;
			s_trianglesRoutedFullPage++;
			s_resolveLastTier = 2;
			s_resolveLastCell = s_rmemoCell;
			return 0;
		}

		{
			const int fullCell = N3DS_FindOrDecodeTexture(texPage, clut, format, view, minV);

			N3DS_CellIndexToAtlasCoord(fullCell, &atlasIdx, &cellX, &cellY);
			s_resolveLastTier = 2;
			s_resolveLastCell = fullCell;
			s_rmemoCell = fullCell;
		}
		*outAtlasIdx = atlasIdx;
		*outPixelBaseU = cellX * N3DS_DECODE_TEXTURE_DIM;
		*outPixelBaseV = cellY * N3DS_DECODE_TEXTURE_DIM;
		*outOriginU = 0;
		*outOriginV = 0;
		s_trianglesRoutedFullPage++;

		s_rmemoValid = 1;
		s_rmemoTier = 2;
		s_rmemoPage = texPage;
		s_rmemoClut = clut;
		s_rmemoFormat = (u8)format;
		s_rmemoView = (u8)view;
		s_rmemoAtlas = atlasIdx;
		s_rmemoPbU = *outPixelBaseU;
		s_rmemoPbV = *outPixelBaseV;
		s_rmemoStamp = s_decodeCacheEvictions + s_subPageCacheEvictions + s_vramDirtyBumps;
		return 0;
	}
}

/*
 * Model draw texture table: one entry per texture key (tpage, clut, view,
 * tier, sub-page tile), holding the decode cell N3DS_ResolveTriangleTexture
 * answered that key from and the cell's atlas placement.
 *
 * An entry is trusted for a triangle only while the cell holds the pixels it
 * held when the entry was filled (same decode serial) and those pixels are
 * fresh for THIS triangle: the cell's page generation equals the generation of
 * the VRAM cell under the triangle's own sampled row, and its CLUT generations
 * equal the CLUT's current ones. That is the test the decode tiers apply on a
 * hit, so a trusted entry names a cell the full resolve could have returned. A
 * hit stamps the cell's lastUsedFrame as a decode-tier hit does, which keeps the
 * eviction-hazard rule protecting it for the rest of the frame.
 *
 * Validity is per cell, not a global stamp: the backdrop fill moves VRAM
 * generations every frame.
 *
 * A triangle whose sampled area reaches a display rect always takes the full
 * resolve, which owns the framebuffer-capture route and the mirror refresh;
 * every other triangle can use the table, whatever the rest of its page holds.
 *
 * A race holds several hundred live keys (each kart's palettes times its
 * sub-page tiles), and keys from earlier scenes stay until overwritten, so the
 * table is sized well past that; entries are 16 bytes to keep it at 64 KiB.
 * Sets of two entries: two hot keys that hash alike would otherwise evict each
 * other on alternate faces. A new key goes in front and the front entry moves
 * back.
 */
#define N3DS_MTC_BITS 12
#define N3DS_MTC_SIZE (1 << N3DS_MTC_BITS)

typedef struct
{
	u32 keyA; /* tpage | clut << 16 */
	u32 cellSerial;
	s16 cell;
	s16 offU, offV; /* pixel base minus the tile origin */
	s8 atlas;
	u8 keyB; /* view | tier << 2 | tileX << 4 | tileY << 6; 0 = empty (tier is never 0) */
} N3DS_ModelTexEntry;

global_variable N3DS_ModelTexEntry s_mtcTable[N3DS_MTC_SIZE];
global_variable u32 s_mtcHits, s_mtcMisses, s_mtcUnstored;
/* Why a lookup went to the full resolve: no entry for the key, the cell was
 * re-decoded or freed, the page row's generation moved, or a CLUT generation
 * moved with different palette content. s_mtcClutAdopts counts CLUT
 * generation moves with equal content, served from the table; s_mtcDisplay
 * triangles that reach a display rect; s_mtcEvicts stores over another key. */
global_variable u32 s_mtcMissKey, s_mtcMissCell, s_mtcMissPage, s_mtcMissClut, s_mtcClutAdopts, s_mtcDisplay, s_mtcEvicts;

/* Does this triangle's sampled area touch either display rect? A superset of
 * both full-resolve side paths: the capture route (texels read as halfwords,
 * area inside a rect) and the mirror refresh (texels shifted to halfwords,
 * area overlapping a rect). */
internal int N3DS_ModelTex_TriReachesDisplay(s16 tpage, TexFormat format, int minU, int maxU, int minV, int maxV)
{
	const int pageX = (tpage & 0xF) * 64;
	const int pageY = (tpage & 0x10) ? 256 : 0;
	const int shift = (format == TF_16_BIT) ? 0 : ((format == TF_8_BIT) ? 1 : 2);
	const int x0 = pageX + (minU >> shift), x1 = pageX + maxU;
	const int y0 = pageY + minV, y1 = pageY + maxV;
	int i;

	for (i = 0; i < 2; i++)
	{
		const N3DS_FBStoreSlot *slot = &s_fbStore[i];

		if ((slot->w > 0) && (x0 < (slot->x + slot->w)) && (x1 >= slot->x) && (y0 < (slot->y + slot->h)) && (y1 >= slot->y))
		{
			return 1;
		}
	}
	return 0;
}

/*
 * The model draw's texture lookup: the atlas and the offsets that turn a page
 * texel (u, v) into the vertex encoding, u' = ((offU + u) << 1) + 1 and
 * v' = 2047 - ((offV + v) << 1). Same answer as N3DS_ResolveTriangleTexture,
 * which it calls on a table miss.
 */
internal void N3DS_ModelTex_Resolve(s16 tpage, s16 clut, N3DS_TexView view, int u0, int v0, int u1, int v1, int u2, int v2, int *outAtlas, int *outOffU,
                                    int *outOffV)
{
	const int minU = MIN3(u0, u1, u2), maxU = MAX3(u0, u1, u2);
	const int minV = MIN3(v0, v1, v2), maxV = MAX3(v0, v1, v2);
	const int tileX0 = minU / N3DS_SUBPAGE_DIM, tileY0 = minV / N3DS_SUBPAGE_DIM;
	const int tier = (!g_dbg_disableSubPage && (tileX0 == (maxU / N3DS_SUBPAGE_DIM)) && (tileY0 == (maxV / N3DS_SUBPAGE_DIM))) ? 1 : 2;
	const int tx = (tier == 1) ? tileX0 : 0, ty = (tier == 1) ? tileY0 : 0;
	const u32 keyA = (u32)(u16)tpage | ((u32)(u16)clut << 16);
	const u32 keyB = (u32)view | ((u32)tier << 2) | ((u32)tx << 4) | ((u32)ty << 6);
	N3DS_ModelTexEntry *set = &s_mtcTable[(((keyA ^ (keyB * 0x9E3779B1u)) * 0x85EBCA6Bu) >> (32 - N3DS_MTC_BITS)) & ~1u];
	N3DS_ModelTexEntry *e = ((set[1].keyA == keyA) && (set[1].keyB == keyB)) ? &set[1] : &set[0];
	const TexFormat format = GetTPageFormat((int)tpage);
	const int hasClut = (format == TF_4_BIT) || (format == TF_8_BIT);
	const int display = N3DS_ModelTex_TriReachesDisplay(tpage, format, minU, maxU, minV, maxV);

	if (display)
	{
		s_mtcDisplay++;
	}
	else if ((e->keyA == keyA) && (e->keyB == keyB))
	{
		/* The decode tiers' own pageCell: the page column plus the tile, the
		 * row under the triangle's lowest sampled V. */
		const u32 pageGen = s_vramCellGen[N3DS_VRAMCellIndex(((tpage & 0xF) * 64) + (tx * N3DS_SUBPAGE_DIM), ((tpage & 0x10) ? 256 : 0) + minV)];
		/* The tiers' CLUT cells, and their pinned 0 for CLUT-less formats. */
		const u32 genA = hasClut ? s_vramCellGen[N3DS_VRAMCellIndex((clut & 0x3F) * 16, clut >> 6)] : 0;
		const u32 genB = hasClut ? s_vramCellGen[N3DS_VRAMCellIndex(((clut & 0x3F) * 16) + 255, clut >> 6)] : 0;
		int used, serial, cPageGen;
		u32 *cClutGenA, *cClutGenB, *cLastUsed, cClutHash;

		if (tier == 1)
		{
			N3DS_SubPageCacheEntry *c = &s_subPageCache[e->cell];

			used = c->used;
			serial = (c->serial == e->cellSerial);
			cPageGen = (c->pageGen == pageGen);
			cClutGenA = &c->clutGenA;
			cClutGenB = &c->clutGenB;
			cLastUsed = &c->lastUsedFrame;
			cClutHash = c->clutHash;
		}
		else
		{
			N3DS_DecodeCacheEntry *c = &s_decodeCache[e->cell];

			used = c->used;
			serial = (c->serial == e->cellSerial);
			cPageGen = (c->pageGen == pageGen);
			cClutGenA = &c->clutGenA;
			cClutGenB = &c->clutGenB;
			cLastUsed = &c->lastUsedFrame;
			cClutHash = c->clutHash;
		}

		if (!used || !serial)
		{
			s_mtcMissCell++;
		}
		else if (!cPageGen)
		{
			s_mtcMissPage++;
		}
		else
		{
			int fresh = (*cClutGenA == genA) && (*cClutGenB == genB);

			/* The tiers' palette adoption: a CLUT cell rewritten with the same
			 * palette leaves the decode valid, so take the new generations in
			 * place exactly as N3DS_FindOrDecodeTexture does. */
			if (!fresh && hasClut && (N3DS_HashClutContent(clut, format) == cClutHash))
			{
				*cClutGenA = genA;
				*cClutGenB = genB;
				s_mtcClutAdopts++;
				fresh = 1;
			}
			if (fresh)
			{
				if (tier == 1)
				{
					if (*cLastUsed != s_frameCounter)
					{
						s_subPageFrameTouchedCells++;
					}
					s_trianglesRoutedSubPage++;
				}
				else
				{
					if (*cLastUsed != s_frameCounter)
					{
						s_frameTouchedCells++;
					}
					s_trianglesRoutedFullPage++;
				}
				*cLastUsed = s_frameCounter;
				s_mtcHits++;
				*outAtlas = e->atlas;
				*outOffU = e->offU;
				*outOffV = e->offV;
				return;
			}
			s_mtcMissClut++;
		}
	}
	else
	{
		s_mtcMissKey++;
	}

	{
		GrVertex uvOnly[3];
		int atlas = -1, pbU = 0, pbV = 0, oU = 0, oV = 0;

		/* N3DS_ResolveTriangleTexture reads only .u/.v. */
		memset(uvOnly, 0, sizeof(uvOnly));
		uvOnly[0].u = (u8)u0;
		uvOnly[0].v = (u8)v0;
		uvOnly[1].u = (u8)u1;
		uvOnly[1].v = (u8)v1;
		uvOnly[2].u = (u8)u2;
		uvOnly[2].v = (u8)v2;
		N3DS_ResolveTriangleTexture(&uvOnly[0], &uvOnly[1], &uvOnly[2], tpage, clut, format, view, &atlas, &pbU, &pbV, &oU, &oV);
		*outAtlas = atlas;
		*outOffU = pbU - oU;
		*outOffV = pbV - oV;
		s_mtcMisses++;

		if (display)
		{
			return;
		}
		if (s_resolveLastTier != tier)
		{
			s_mtcUnstored++;
			return;
		}
		if (((e->keyA != keyA) || (e->keyB != keyB)) && (set[0].keyB != 0))
		{
			if (set[1].keyB != 0)
			{
				s_mtcEvicts++;
			}
			set[1] = set[0];
		}

		e->keyA = keyA;
		e->keyB = (u8)keyB;
		e->cell = (s16)s_resolveLastCell;
		e->cellSerial = (tier == 1) ? s_subPageCache[s_resolveLastCell].serial : s_decodeCache[s_resolveLastCell].serial;
		e->atlas = (s8)atlas;
		e->offU = (s16)(pbU - oU);
		e->offV = (s16)(pbV - oV);
	}
}

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen)
{
	(void)fullscreen;

	N3DS_GfxInit();

	if (!N3DS_C3DInit())
	{
		N3DS_DebugLog("[CTR-3DS] citro3d init FAILED");
		return 0;
	}

	/* All-bits-set = -1 in every bucket (see the hash-index comment). */
	memset(s_fullPageHashIdx, 0xFF, sizeof(s_fullPageHashIdx));
	memset(s_subPageHashIdx, 0xFF, sizeof(s_subPageHashIdx));

	s_rendererInitialised = 1;

	printf("\x1b[2J");
	printf("CTR 3D / Nintendo 3DS\n");
	printf("---------------------\n");
	printf("%s\n", (windowName != NULL) ? windowName : "(no title)");
	/* width/height here are the PC path's SDL window request (main.c passes
	 * 800x600) -- nothing on 3DS renders at them, and echoing them as "PSX
	 * display" read as a live 800x600 pipeline (user-flagged).
	 * Print what this platform actually does instead. */
	(void)width;
	(void)height;
	printf("render: PSX-res -> 400x240 top screen (4:3 pillarbox)\n");
	printf("VRAM mirror: %d KiB\n", (int)(sizeof(vram) / 1024));
	printf("\nPhase 2a: citro3d is live on the top\nscreen (see file header for scope).\n\n");

	N3DS_DebugLog("[CTR-3DS] renderer init ok, citro3d ready");

	/* Hand the backend TU the VRAM buffer so its debug capture (dumpreq `capture`) can
	 * write it to SD (see N3DS_DebugSetVRAMForDump). vram[] and the frame
	 * counter shadow are stable globals, so registering their addresses once
	 * is safe. */
	N3DS_DebugSetVRAMForDump(vram, (unsigned int)sizeof(vram), &s_frameCounterForDump);

	return 1;
}

int NativeRenderer_InitialisePSX(void)
{
	int i;

	memset(vram, 0, sizeof(vram));
	s_vramPendingCount = 0;

	for (i = 0; i < (N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS); i++)
	{
		s_vramCellGen[i] = 0;
	}
	for (i = 0; i < (1 + N3DS_FULLPAGE_RESERVED_CELLS); i++)
	{
		s_decodeCache[i].used = 0;
		s_decodeCache[i].lastUsedFrame = 0;
	}
	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		s_subPageCache[i].used = 0;
		s_subPageCache[i].lastUsedFrame = 0;
	}

	/*
	 * White cell: flat opaque texel for untextured/Gouraud splits. Uniform
	 * colour makes the 8x8 tile swizzle a no-op (every permutation of identical
	 * values is identical), so this just fills linearly. Uploaded once here
	 * into atlas 0's cell (0,0); that cell is never entered into the LRU cache
	 * and never re-decoded.
	 *
	 * Must use the same GPU_RGBA5551 layout as N3DS_DecodeAndTilePage -- it
	 * feeds every untextured primitive, so a mismatch here inverts the bug onto
	 * flat-shaded geometry instead of textured. 0xFFFF = R,G,B all 31, alpha 1.
	 */
	for (i = 0; i < (N3DS_DECODE_TEXTURE_DIM * N3DS_DECODE_TEXTURE_DIM); i++)
	{
		s_decodeScratch[i] = 0xFFFFu;
	}
	N3DS_C3DUploadDecodedCell(N3DS_WHITE_ATLAS, N3DS_WHITE_CELL_X, N3DS_WHITE_CELL_Y, s_decodeScratch);

	return 1;
}

void NativeRenderer_Shutdown(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	s_rendererInitialised = 0;
	N3DS_C3DExit();
	N3DS_GfxExit();
}

void NativeRenderer_ResetDevice(void)
{
}

void NativeRenderer_BeginScene(void)
{
	/*
	 * Opens the citro3d frame bracket. Must happen here, not in SwapWindow:
	 * real drawing happens via DrawAllSplits, which DrawOTag calls once near
	 * the start of the frame (and DrawSync can call again mid-frame) -- long
	 * before SwapWindow runs at frame end. Platform_BeginScene (the sole
	 * caller) already guards re-entry within a frame, so this only opens the
	 * bracket once regardless of how many times DrawAllSplits runs.
	 */
	if (!s_rendererInitialised)
	{
		return;
	}

	s_beginSceneCount++;

#ifdef CTR_INTERNAL
	/* Armed by the capture set (N3DS_DebugCaptureAll): dump the coming
	 * frame's triangles. Opened here (frame start), closed next BeginScene. */
	if (s_triDumpFile != NULL)
	{
		fclose(s_triDumpFile);
		s_triDumpFile = NULL;
		N3DS_DebugLog("[CTR-3DS] triangle dump closed");
	}
	if (s_triDumpArm)
	{
		char triPath[64];

		s_triDumpArm = 0;
		snprintf(triPath, sizeof(triPath), "sdmc:/tridump_%u.bin", (unsigned)s_frameCounter);
		s_triDumpFile = fopen(triPath, "wb");
		N3DS_DebugLog(s_triDumpFile ? "[CTR-3DS] triangle dump armed for this frame" : "[CTR-3DS] triangle dump fopen FAILED");
	}
#endif

	/* Working-set estimate is per frame, so latch its peak and restart the
	 * count at the frame boundary. BeginScene is guarded against re-entry by
	 * Platform_BeginScene, so this runs exactly once per frame. */
	if (s_frameTouchedCells > s_frameTouchedPeak)
	{
		s_frameTouchedPeak = s_frameTouchedCells;
	}
	s_frameTouchedCells = 0;

	/* Sub-page tier counterpart -- same latch-and-reset, see above. */
	if (s_subPageFrameTouchedCells > s_subPageFrameTouchedPeak)
	{
		s_subPageFrameTouchedPeak = s_subPageFrameTouchedCells;
	}
	s_subPageFrameTouchedCells = 0;

	/* Distinct-atlas-per-frame latch (see the counter block's header). */
	{
		u32 distinct = (u32)__builtin_popcount(s_frameAtlasMask);

		s_distinctAtlasTotal += distinct;
		if (distinct > s_frameDistinctAtlasPeak)
		{
			s_frameDistinctAtlasPeak = distinct;
		}
		s_frameAtlasMask = 0;
	}

	/*
	 * Fit the game's own display area to the panel, cropped to the PSX's
	 * intended 4:3 aspect rather than widened to the top screen's 5:3.
	 * CTR runs a 512x216 DISPENV; previously this widened the
	 * ortho range to fill the full 400px panel width, which showed ~40 extra
	 * PSX-space columns of world per edge that were never meant to be on
	 * screen -- the source of the "can see past the edge of the track"
	 * jaggies. That's gone now: the range below is unwidened (native dispW),
	 * matched by a viewport crop in N3DS_C3DBeginFrame (n3ds_backend.c) so
	 * this native range isn't just re-stretched across the full 400px
	 * instead (which would trade the extra-content problem for a
	 * ~25%-too-wide distortion problem).
	 *
	 * Read from the live DISPENV rather than hardcoding 512x216: the game
	 * double-buffers two dispEnvs and could change modes, and this costs two
	 * integer compares a frame.
	 */
	{
		extern int g_dbg_widescreen;
		const int dispW = (activeDispEnv.disp.w > 0) ? activeDispEnv.disp.w : 512;
		const int dispH = (activeDispEnv.disp.h > 0) ? activeDispEnv.disp.h : 216;

		if ((dispW != s_projectionDispW) || (dispH != s_projectionDispH) || (g_dbg_widescreen != s_projectionWideMode))
		{
			/* Mirrors the extension N3DS_C3DSetProjection applies in mode 2,
			 * so the log states the window the GPU actually got. */
			const int extra = (g_dbg_widescreen == 2) ? (dispW * (400 - N3DS_TOP_CONTENT_W)) / (2 * N3DS_TOP_CONTENT_W) : 0;
			char line[96];

			s_projectionDispW = dispW;
			s_projectionDispH = dispH;
			s_projectionWideMode = g_dbg_widescreen;
			N3DS_C3DSetProjection(0.0f, (float)dispW, (float)dispH);

			snprintf(line, sizeof(line), "[CTR-3DS] projection: disp %dx%d -> ortho %d..%d x 0..%d (%s)", dispW, dispH, -extra, dispW + extra, dispH,
			         (g_dbg_widescreen == 2) ? "REAL wide FOV" : (g_dbg_widescreen == 1) ? "STRETCH, no crop" : "4:3, viewport-cropped");
			N3DS_DebugLog(line);
		}
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE);
	N3DS_SetMirrorWorld(NativeMirror_LatchFrame(s_projectionDispW));
	N3DS_C3DBeginFrame();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE);
	/* The depth buffer just cleared (C3D_RenderTargetClear in BeginFrame),
	 * so this is the one legal place to restart the emission ordinal --
	 * see NativeGpu_ResetEmitOrdinal's comment for the multi-pass trap. */
	NativeGpu_ResetEmitOrdinal();

	/* Fresh frame, fresh vertex-buffer cursor: no run can carry across
	 * (EndScene flushed it). Reset the frame-scope run state and force the
	 * first DrawTriangles to re-assert the TEV scale -- the atlas inspector
	 * and PresentVRAMRect may have left it at 0-doubling. */
	s_runAtlas = -1;
	s_runVertCount = 0;
	s_runUntextured = -1;

	/* Defensively drop any deferred state a frame that skipped EndScene
	 * could have stranded -- the VBO cursor just reset, so stale recorded
	 * baseVertex values must never survive into this frame. */
	{
		int bin;

		for (bin = 0; bin < N3DS_MERGE_BIN_COUNT; bin++)
		{
			s_mergeBinUsed[bin] = 0;
		}
	}
	s_skyVertUsed = 0; /* sky accumulator shares the bins' frame lifecycle */
	s_skyVertDomeAt = -1;
	s_backdropVertUsed = 0; /* s46: backdrop accumulator, same lifecycle */
	s_mergeOpaqueSegCount = 0;
	s_mergeBlendSegCount = 0;
	/* Same reason as the bins above -- the VBO cursor just reset, so any
	 * baseVertex a skipped EndScene stranded must not survive into this frame. */
	s_uiOrdSegCount = 0;
	s_uiOrdRunUsed = 0;
	s_uiOrdRunAtlas = -1;
	s_uiOrdRunBlend = -1;
	s_uiOrdRunDoubling = -1;
	s_uiOrdPrims = 0;
}

global_variable int s_vramPresentQueued;
global_variable int s_vramPresentQueuedX;
global_variable int s_vramPresentQueuedY;
global_variable int s_vramPresentQueuedW;
global_variable int s_vramPresentQueuedH;

void NativeRenderer_QueueVRAMPresent(int x, int y, int w, int h)
{
	s_vramPresentQueued = 1;
	s_vramPresentQueuedX = x;
	s_vramPresentQueuedY = y;
	s_vramPresentQueuedW = w;
	s_vramPresentQueuedH = h;
}

void NativeRenderer_EndScene(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	/*
	 * Draw the backdrop fill and the skybox FIRST -- before the resident track
	 * pass and before the merge barrier -- so the translucent waterfalls (which
	 * write no depth) paint over the sky instead of it painting over them. The
	 * sky was routed out of the opaque bins into s_skyVerts during the walk
	 * (native_direct_emit.c), so it is not double-drawn by MergeFlush below.
	 * Packet state from the walk is still active here, which is what
	 * N3DS_MergeEmit assumes.
	 *
	 * Backdrop first, sky second: both sit at the far pin (vertex -32768), LEQUAL
	 * passes on equal, so the later sky draw wins the tie and layers over the
	 * fill -- bg color, then gradient/dome, then everything real, retail's
	 * stack.
	 */
	N3DS_DrawBackdropPass();
	N3DS_DrawSkyPass();

	/*
	 * Resident track draws, issued IMMEDIATELY BEFORE the merge-bin barrier
	 * below. MergeFlush emits opaque bins/segments first and blend segments
	 * last, so track geometry submitted just before it is opaque-with-opaque
	 * (order-free under the emit-ordinal depth test) and still lands UNDER
	 * every blended packet -- which is what surface-riding content (shadows,
	 * skids) draws over the surface it rides needs. Drawn after the flush, the
	 * track would shove shadows and skids behind itself. Forward-declared here,
	 * defined later (platform/native_track_frame.c), because main.c's unity
	 * chain includes this file before that one.
	 */
	{
		extern void NativeTrackDraw_IssueForWalk(void);

		NativePerf_BeginScope(NATIVE_PERF_BUCKET_NATIVE_TRACK_DRAW);
		NativeTrackDraw_IssueForWalk();
		NativePerf_EndScope(NATIVE_PERF_BUCKET_NATIVE_TRACK_DRAW);

		/* NativeTrackDraw_IssueForWalk's own N3DS_TrackDraw_End (called once per
		 * leaf-draw bracket, even when zero leaves drew) already restored the
		 * packet path's program/attrInfo/bufInfo/texture-combiner state via
		 * N3DS_BindPacketDrawState -- required before MergeFlush below, whose
		 * draws assume that exact state. */
	}

	/* Last flush point of the frame: whatever the final splits left
	 * deferred (merge bins/segments, or just the pending run) has to be
	 * recorded before C3D_FrameEnd submits the frame. */
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_MERGE_FLUSH);
	N3DS_MergeFlush();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_MERGE_FLUSH);

	/*
	 * PICA-native model instances (rung 2).
	 *
	 * AFTER the merge flush, deliberately, unlike the track draw above. Model
	 * draws append into the SAME per-frame vertex buffer the merge bins hold
	 * offsets into, and they rebind the program and attribute layout to draw.
	 * Issuing them before the flush would mean the bins' own draws execute
	 * against whatever state the model pass left -- N3DS_ModelDraw_End restores
	 * it, but ordering the two so the question cannot arise is cheaper than
	 * relying on that restore being exhaustive. Depth ordering is unaffected:
	 * models carry real depth and the depth test owns layering, which is the
	 * entire point of putting them on this axis.
	 *
	 * Costs nothing at toggle 0 -- NativeModelDraw_IssueForWalk returns before
	 * touching any GPU state, the same early-out shape the track draw uses.
	 */
	{
		extern void NativeModelDraw_IssueForWalk(void);
		const unsigned long long texdecMdlT0 = N3DS_GetTickCount();

		NativePerf_BeginScope(NATIVE_PERF_BUCKET_NATIVE_MODEL_DRAW);
		NativeModelDraw_IssueForWalk();
		NativePerf_EndScope(NATIVE_PERF_BUCKET_NATIVE_MODEL_DRAW);
		s_texdecFrameMdlTicks += N3DS_GetTickCount() - texdecMdlT0;
	}

	/*
	 * Driver-local exhaust smoke, deliberately held out of the merge
	 * above (DrawAllSplits flagged its splits deferredSmoke), is redrawn HERE --
	 * last of all, after the model pass -- so it paints over its own kart body
	 * (retail's painter's order) while the depth test still lets a genuinely
	 * nearer wall or kart occlude it. The smoke carries real view.z depth
	 * (GTECapture_StoreParticleCorner) so the LEQUAL test is meaningful against
	 * the body's own view.z depth; without it the smoke would fall back to the
	 * parse-order ordinal axis and read as infinitely far.
	 *
	 * NativeGpu_DrawDeferredSmoke re-runs the captured splits through the normal
	 * DrawSplit path, binning them back into the now-empty merge as blends --
	 * which already draw depth-test-on / depth-write-off (the anti-carve
	 * rule) -- and the MergeFlush below drains them onto the command list on top
	 * of the body. s_mergeDepthWriteState is forced to "unknown" first so the
	 * smoke's first emit re-asserts the LEQUAL depth state the model pass left.
	 * No-op on any frame with no driver-local smoke on screen.
	 */
	{
		extern void NativeGpu_DrawDeferredSmoke(void);
		NativeGpu_DrawDeferredSmoke();
		s_mergeDepthWriteState = -1;
		N3DS_MergeFlush();
	}

	/*
	 * The frame's UI, in submission order, LAST -- after the merge barrier,
	 * the model pass and the deferred smoke, so nothing can paint over it. UI
	 * layering is painter's order (what retail does), not an artefact of which
	 * state bin each glyph and panel happened to land in.
	 */
	N3DS_DrawUiPass();

#ifdef CTR_INTERNAL
	/*
	 * Atlas inspector (dumpreq 'atlasview'): draw atlas
	 * (g_dbg_atlasView - 1) full-screen INSTEAD of the game (DrawTriangles
	 * suppresses normal emission while active). The screen then shows what
	 * the GPU actually samples from that atlas; the capture set's
	 * atlas_<frame>_<i>.bin shows what the CPU wrote. Any visible
	 * difference = the GPU/emulator texture view is stale relative to
	 * memory -- the one link every CPU-side dump is structurally blind to,
	 * and the only remaining suspect after VRAM content, decode content,
	 * keys, UVs and grouping all byte-verified clean while the screen
	 * quilts. (The GSPGPU_FlushDataCache comment in n3ds_backend.c records
	 * a previous fight with exactly this staleness class.)
	 */
	if (g_dbg_atlasView > 0)
	{
		N3DS_Vertex quad[6];
		const int atlasIndex = g_dbg_atlasView - 1;
		int baseVertex;
		int i;

		/* Screen-space rect: the 240px-tall square at the screen's left
		 * (mapped through the same projection every draw uses -- x right,
		 * y down, 400x240). Numerators span the full atlas [0,2048). */
		const s16 x0 = 0, y0 = 0, x1 = 240, y1 = 240;
		const s16 u0 = 1, v0 = 1, u1 = 2047, v1 = 2047;

		quad[0].x = x0; quad[0].y = y0; quad[0].u = u0; quad[0].v = v0;
		quad[1].x = x1; quad[1].y = y0; quad[1].u = u1; quad[1].v = v0;
		quad[2].x = x0; quad[2].y = y1; quad[2].u = u0; quad[2].v = v1;
		quad[3].x = x1; quad[3].y = y0; quad[3].u = u1; quad[3].v = v0;
		quad[4].x = x1; quad[4].y = y1; quad[4].u = u1; quad[4].v = v1;
		quad[5].x = x0; quad[5].y = y1; quad[5].u = u0; quad[5].v = v1;
		for (i = 0; i < 6; i++)
		{
			quad[i].z = N3DS_DEPTH_TOPMOST;
			/* Overlay quads carry no stereo depth: parallax = z puts them in
			 * the near band the shader's mask flattens to zero shift, which
			 * is what a debug overlay wants. */
			quad[i].w = N3DS_DEPTH_TOPMOST;
			quad[i].r = 255;
			quad[i].g = 255;
			quad[i].b = 255;
			quad[i]._pad = 0;
		}

		/* Modulate off (bright=1 pipeline) so texels show unscaled. */
		N3DS_C3DSetColorDoubling(0);
		baseVertex = N3DS_TvboCheckedAppend(quad, 6);
		if (baseVertex >= 0)
		{
			N3DS_C3DBindAtlas(atlasIndex);
			N3DS_C3DDrawTriangles(baseVertex, 2);
		}
	}
#endif

	/* A queued VRAM present (pinned display: the Scrapbook) covers the screen,
	 * so it draws after every scene pass, and before the right-eye replay and
	 * FrameEnd so both eyes and this frame's command list carry it. */
	if (s_vramPresentQueued)
	{
		s_vramPresentQueued = 0;
		NativeRenderer_PresentVRAMRect(s_vramPresentQueuedX, s_vramPresentQueuedY, s_vramPresentQueuedW, s_vramPresentQueuedH);
	}

	/*
	 * Stereoscopic right eye (n3ds_backend.h's stereo block): replay this
	 * frame's recorded draws against the right target with the right eye's
	 * parallax. LAST, after every left-eye draw including the atlas
	 * inspector above (which intentionally stays left-eye-only -- it is a
	 * debug screen takeover, not content), and before FrameEnd submits both
	 * eyes as one command list. No-op when the slider is down, so the mono
	 * frame -- every fps baseline -- is bit-for-bit today's. Right-eye
	 * draws are not counted in s_drawCallCount: [PERF] draw counts stay
	 * per-eye numbers.
	 */
	N3DS_StereoRenderRightEye();

	/* Bottom-screen minimap, LAST of all: after the right-eye replay so its
	 * target/state switches can never leak into the journaled eyes, before
	 * FrameEnd so its draws join this frame's command list. Self-guarding --
	 * drains its own state even when there is nothing to draw. */
	N3DS_DrawBottomMapPass();

	s_endSceneCount++;
	// C3D_FrameEnd: the whole frame's queued GPU commands actually execute
	// here, so this is where any GPU-side stall or submit cost shows up.
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_N3DS_FRAME_END);
	N3DS_C3DEndFrame();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_N3DS_FRAME_END);
}

void NativeRenderer_UpdateSwapIntervalState(int swapInterval)
{
	(void)swapInterval;
}

/*
 * Measured on hardware: the stats block cost ~50ms, landing on one frame
 * every 64. libctru's console renders every cell as a glyph blit into a
 * column-major rotated framebuffer, and `\x1b[2J` clears by printing a
 * space into all 40x30=1200 cells before any text is drawn -- three
 * quarters of the cost was erasing cells about to be redrawn with
 * unchanged text.
 *
 * Fix below: keep a shadow of each row's text and repaint only rows that
 * differ, positioning the cursor per row instead of clearing; a full clear
 * still happens when the layout itself changes (verbose/sweep toggled).
 */
#define N3DS_STATS_ROWS 40
#define N3DS_STATS_COLS 41 /* 40 columns + NUL */
/*
 * The block below does not draw at all any more -- it composes text into
 * s_statsPending, and ONE row per frame is painted from there (StatsDripRow).
 * Owner's idea, and it is strictly better than the shadow-diff it replaces:
 *
 *  - No hitch at all. The whole screen costs ~40 glyph cells per frame,
 *    spread evenly, instead of arriving in one lump every 64 frames.
 *  - It scrubs foreign text for free. The diff version only repainted rows
 *    IT had changed, so anything else printing to this console (boot banners,
 *    another session's debug lines) sat there permanently -- owner: "noise on
 *    the lines that don't get updated". Repainting every row in rotation
 *    overwrites that within one sweep, no clear needed.
 *
 * A full sweep is 40 rows, so at 30fps any given row refreshes about every
 * 1.3s -- comfortably faster than the 64-frame cadence that changes it.
 */
global_variable char s_statsPending[N3DS_STATS_ROWS][N3DS_STATS_COLS];
global_variable int s_statsRow = 0;
global_variable int s_statsCursor = 0;

internal void N3DS_StatsEmitRow(const char *text)
{
	if (s_statsRow < N3DS_STATS_ROWS)
	{
		/* Padded to the full 40 columns: a line that got SHORTER must still
		 * erase the tail of whatever it replaces. */
		snprintf(s_statsPending[s_statsRow], N3DS_STATS_COLS, "%-40.40s", text);
	}

	s_statsRow++;
}

internal void N3DS_StatsDripRow(void)
{
	const int r = s_statsCursor;

	/* Consolidated console-visibility gate (map owns the panel / stats console
	 * off / boot text still up) -- see g_dbg_statsConsole. Glyphs land in the
	 * shared framebuffer at print time, so this must be gated at the painter,
	 * not at present. Composition and the log mirror continue regardless. */
	if (!N3DS_ConsolePaintAllowed())
	{
		return;
	}

	s_statsCursor = (s_statsCursor + 1) % N3DS_STATS_ROWS;
	/* Unconditional, not diffed -- repainting an unchanged row is what makes
	 * this self-cleaning. Rows the block never filled are blank by static
	 * init, so they scrub to spaces rather than keeping stale text. */
	printf("\x1b[%d;1H%-40.40s", r + 1, s_statsPending[r]);
}

/*
 * Drop-in for printf inside the stats block only (see the #define below it).
 * Splits on embedded newlines so the existing call sites -- several of which
 * print leading or trailing blank lines -- keep their exact layout.
 */
internal void N3DS_StatsPrintf(const char *fmt, ...)
{
	char buf[256];
	char *p = buf;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	for (;;)
	{
		char *nl = strchr(p, '\n');

		if (nl != NULL)
		{
			*nl = '\0';
			N3DS_StatsEmitRow(p);
			p = nl + 1;
		}
		else
		{
			if (*p != '\0')
			{
				N3DS_StatsEmitRow(p);
			}
			break;
		}
	}
}

void NativeRenderer_SwapWindow(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	/* Re-capture the forced diagnostic texture next frame (see
	 * g_dbg_forceSingleTexture) so it always tracks a currently-loaded one. */
	s_forcedTexValid = 0;

	/*
	 * [TEXDEC] fold: attribute the frame that just drew (s_frameCounter is
	 * pre-increment here, so frame= matches). Per-frame line only past the
	 * thresholds -- steady content (a few ms of stale re-decodes, model pass
	 * under ~20ms) stays silent; a first-draw storm or a no-decode model
	 * spike prints itself. Window totals/max feed the 64-frame stats block.
	 */
	{
		const unsigned long long texdecFreq = N3DS_GetTickFrequency();
		const unsigned long decUs = (unsigned long)((s_texdecFrameDecTicks * 1000000ULL) / texdecFreq);
		const unsigned long upUs = (unsigned long)((s_texdecFrameUpTicks * 1000000ULL) / texdecFreq);
		const unsigned long mdlUs = (unsigned long)((s_texdecFrameMdlTicks * 1000000ULL) / texdecFreq);
		const unsigned long missUs = decUs + upUs;

		s_texdecWinSub += s_texdecFrameSub;
		s_texdecWinFull += s_texdecFrameFull;
		s_texdecWinUs += (u32)missUs;
		if ((missUs > mdlUs ? missUs : mdlUs) > s_texdecWinMaxUs)
		{
			s_texdecWinMaxUs = (u32)(missUs > mdlUs ? missUs : mdlUs);
			s_texdecWinMaxSub = s_texdecFrameSub;
			s_texdecWinMaxFull = s_texdecFrameFull;
			s_texdecWinMaxMdlUs = (u32)mdlUs;
			s_texdecWinMaxFrame = s_frameCounter;
		}
		if ((missUs >= 5000) || (mdlUs >= 20000))
		{
			char texdecLine[224];
			int len;

			len = snprintf(texdecLine, sizeof(texdecLine), "[TEXDEC] frame=%lu sub=%lu full=%lu decUs=%lu upUs=%lu mdlUs=%lu",
			               (unsigned long)s_frameCounter, (unsigned long)s_texdecFrameSub, (unsigned long)s_texdecFrameFull,
			               decUs, upUs, mdlUs);
			if ((len > 0) && ((size_t)len < sizeof(texdecLine)) && (s_texdecFrameWorstTicks > 0))
			{
				const unsigned long worstUs = (unsigned long)((s_texdecFrameWorstTicks * 1000000ULL) / texdecFreq);

				if (s_texdecFrameWorstIsSub)
				{
					snprintf(texdecLine + len, sizeof(texdecLine) - (size_t)len, " worst=p%x c%x t%d,%d us=%lu", (unsigned)s_texdecFrameWorstPage,
					         (unsigned)s_texdecFrameWorstClut, (int)s_texdecFrameWorstTileX, (int)s_texdecFrameWorstTileY, worstUs);
				}
				else
				{
					snprintf(texdecLine + len, sizeof(texdecLine) - (size_t)len, " worst=p%x c%x full us=%lu", (unsigned)s_texdecFrameWorstPage,
					         (unsigned)s_texdecFrameWorstClut, worstUs);
				}
			}
			N3DS_DebugLog(texdecLine);
		}
		s_texdecFrameSub = 0;
		s_texdecFrameFull = 0;
		s_texdecFrameDecTicks = 0;
		s_texdecFrameUpTicks = 0;
		s_texdecFrameMdlTicks = 0;
		s_texdecFrameWorstTicks = 0;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_SWAP_WINDOW);

	/*
	 * Frame pacing lives in VSync() (platform/native_platform.c), which runs off
	 * the tick clock. Only push the console's backing buffer here; do not wait on
	 * a second vblank source or the 30fps gate would be paced twice.
	 */
	s_frameCounter++;
	/* Plain-unsigned shadow for the backend's capture-filename stamp: u32 is
	 * unsigned long on this ABI, and the n3ds_backend.h boundary carries only
	 * plain scalar types (no u32), so hand it a real unsigned int instead of
	 * an aliased pointer. */
	s_frameCounterForDump = (unsigned int)s_frameCounter;

	/*
	 * `& 63` halves SD traffic versus a per-second cadence while keeping the
	 * on-screen toggle readout responsive enough to dial by eye. Windowed
	 * counters below derive their deltas from the previous sample, so they
	 * stay correct at any cadence -- only the sampling period changes.
	 */
	if ((s_frameCounter & 63) == 0)
	{
		/* Time this whole block. Killing the SD flush took the worst frame
		 * per window from ~150ms to 55-80ms (track by track), so
		 * something ELSE lands on this same 64-frame beat -- and this block is
		 * the candidate: \x1b[2J clears 230KB, then ~20 lines of glyphs are
		 * drawn into a COLUMN-major rotated framebuffer, about as cache-hostile
		 * as ARM11 memory work gets. Shown next window as `stats`; if it tracks
		 * (worst - 33ms), the readout itself is the remaining hitch. */
		const unsigned long long statsT0 = N3DS_GetTickCount();

		/*
		 * Every printf from here to the end of this block goes through the
		 * shadow-diff emitter above -- textual, scoped, #undef'd at the end of
		 * the block. Done as a macro rather than editing ~40 call sites so the
		 * lines themselves stay readable and merge cleanly with other
		 * sessions' edits to this screen. snprintf/N3DS_DebugLog are
		 * untouched.
		 */
		s_statsRow = 0;

#define printf(...) N3DS_StatsPrintf(__VA_ARGS__)

		/*
		 * On-screen stats block. Getting files off the console is awkward (the
		 * SD is under the backplate and only one homebrew app runs at a time,
		 * so an FTP server cannot be up while this is), and a photo of the top
		 * screen carries everything worth knowing. Also mirrored to the log.
		 */
		const unsigned long vblanks = (unsigned long)Platform_GetVBlankCount();
		const unsigned long frames = (unsigned long)s_frameCounter;
		const unsigned long heapKiB = N3DS_HeapUsedBytes() / 1024UL;
		unsigned long avgX10 = 0;
		unsigned long nowX10 = 0;
		char line[128];

		if (vblanks > 0)
		{
			// The retail gate is two vblanks per flip, i.e. 30fps.
			avgX10 = (600UL * frames) / vblanks;
		}

		/*
		 * Report the rate over the last window as well as the average since
		 * boot. A falling average hides how bad the current rate is, because
		 * every earlier good frame keeps dragging it up.
		 */
		if (vblanks > s_lastStatVblanks)
		{
			nowX10 = (600UL * (frames - s_lastStatFrames)) / (vblanks - s_lastStatVblanks);
		}
		s_lastStatFrames = frames;
		s_lastStatVblanks = vblanks;
		/* Publish for the bottom-screen FPS overlay -- computed here whether or
		 * not the console block below ever paints. */
		g_n3dsFpsX10 = nowX10;

		/*
		 * Row 1, not row 16: the block started halfway down when the console
		 * shared the TOP screen with game output. citro3d owns the top screen
		 * now, so the whole bottom console is ours -- and starting at 16 left
		 * too few rows, so the block overflowed, scrolled, and then redrew
		 * over its own scrolled remains. Unreadable on hardware, which is
		 * where this is the only output there is.
		 */
		// \x1b[2J (clear) then home. Redrawing in place left fragments of the
		// previous, longer values behind wherever a new line was shorter,
		// which reads as garbled overlapping text -- useless in a photo, and a
		// photo of this screen is the whole readout on real hardware.
		printf("---- 3DS port stats ----------------\n");
		printf("frames %-8lu vblanks %-8lu\n", frames, vblanks);
		printf("fps    %lu.%lu now / %lu.%lu avg (target 30)   \n",
		       nowX10 / 10UL, nowX10 % 10UL, avgX10 / 10UL, avgX10 % 10UL);
		/* Break the heap down: XA now reports ring occupancy (sectors buffered
		 * / ring depth) and underrun count instead of arena KiB -- this is the
		 * same instrumentation that originally proved the old arena-growth bug;
		 * after the fix it should show a
		 * small number that stays flat across track transitions instead of
		 * one that doubles over a session. */
		printf("heap   %-6lu KiB  xa %d/%d ur %-4lu\n", heapKiB, NativeAudio_XAStreamSectorsBuffered(), NATIVE_AUDIO_XA_RING_SECTORS,
		       (unsigned long)s_audio.xaStream.underrunEvents);
		printf("state  %-4d                        \n", (int)sdata->mainGameState);

		/*
		 * TOGGLES -- the only lines on this screen that are ACTIONABLE mid-race,
		 * so they sit at the top where they can be read at a glance on real
		 * hardware. Everything below is diagnostic and now hides behind
		 * g_dbg_statsVerbose. The depth-bias readout that lived here is gone with
		 * the knob itself; what replaced it is which content class the sweep is
		 * currently ablating, because "the crates are missing" is otherwise
		 * indistinguishable from a renderer bug when read off a photo.
		 */
		{
			extern int g_dbg_packetDepth;
			extern int g_dbg_trackRender;
			extern int g_dbg_modelRender;
			extern int g_dbg_widescreen;
			static const char *const trackNames[3] = {"retail", "OURS", "both"};
			int tr = g_dbg_trackRender;

			/* The console is 40 columns. Every line here is written to fit
			 * inside that WITHOUT wrapping -- a wrapped line costs two rows and
			 * reads as garbage in a photo, which is how this screen actually
			 * gets used on hardware.
			 *
			 * Declutter: just the SELECT+START master
			 * switch and the three renderers it drives. Every other chord
			 * still works; it's only the hints that are gone. */
			printf("\n>> SEL+START = ours <-> retail\n");
			printf(">> track %-6s models %-3s dmodel %-3s\n", trackNames[(tr >= 0 && tr <= 2) ? tr : 0], g_dbg_modelRender ? "ON" : "off",
			       g_dbg_packetDepth ? "ON" : "off");
			{
				static const char *const wideNames[3] = {"off", "strch", "REAL"};
				int wm = g_dbg_widescreen;

				printf(">> wide  %-5s\n", wideNames[(wm >= 0 && wm <= 2) ? wm : 0]);
			}
			{
				extern int g_dbg_sdLog;

				/* Logging earns a line of its own with its chord spelled out:
				 * ON costs a ~100-170ms hitch every 2s (the SD flush), so
				 * this is the one toggle whose state silently invalidates a
				 * "how does it feel" verdict. Reads one window late like every
				 * line here -- the block only redraws every 64 frames. */
				printf(">> log   %-3s   SEL+R\n\n", g_dbg_sdLog ? "ON" : "off");
			}
		}

		snprintf(line, sizeof(line),
		         "[CTR-3DS] frames=%lu vbl=%lu fps_now=%lu.%lu heapKiB=%lu xaBuf=%d/%d underrun=%lu voiceKiB=%d state=%d",
		         frames, vblanks, nowX10 / 10UL, nowX10 % 10UL, heapKiB,
		         NativeAudio_XAStreamSectorsBuffered(), NATIVE_AUDIO_XA_RING_SECTORS, s_audio.xaStream.underrunEvents,
		         s_audio.voicePcmArena.capacity / 1024, (int)sdata->mainGameState);
		N3DS_DebugLog(line);

		/*
		 * Texture decode cache diagnostics -- added to check whether the cache
		 * is thrashing (real, ongoing evictions of still-wanted textures, which
		 * is expensive every single time) versus a one-time warm-up cost
		 * (misses into never-yet-used slots, cheap after the first frame or
		 * two). fps_now above is a frames-per-emulated-vblank ratio and can
		 * look better than the real experience if per-frame cost is spiky --
		 * this says directly whether that's what's happening here.
		 */
		{
			const unsigned long missesWindow = (unsigned long)(s_decodeCacheMisses - s_lastStatCacheMisses);
			const unsigned long evictionsWindow = (unsigned long)(s_decodeCacheEvictions - s_lastStatCacheEvictions);
			const unsigned long drawsWindow = (unsigned long)(s_drawCallCount - s_lastStatDrawCallCount);
			/* Report interval is always exactly 64 frames -- the (s_frameCounter & 63)
			 * gate above. This divided by 32 until the gate was changed from & 31 to
			 * & 63 (a leftover), which overstated draws/frame 2x on the bottom screen. */
			const unsigned long drawsPerFrameAvgX10 = (drawsWindow * 10UL) / 64UL;
			char cacheLine[128];

			const unsigned long staleWindow = (unsigned long)(s_decodeCacheStaleMisses - s_lastStatStaleMisses);
			const unsigned long capacityWindow = (unsigned long)(s_decodeCacheCapacityMisses - s_lastStatCapacityMisses);

			s_lastStatCacheMisses = s_decodeCacheMisses;
			s_lastStatCacheEvictions = s_decodeCacheEvictions;
			s_lastStatDrawCallCount = s_drawCallCount;
			s_lastStatStaleMisses = s_decodeCacheStaleMisses;
			s_lastStatCapacityMisses = s_decodeCacheCapacityMisses;

			if (g_dbg_statsVerbose)
			printf("cache  hits %-8lu miss %-6lu (+%-4lu/w) evict %-4lu (+%-4lu/w)\n", (unsigned long)s_decodeCacheHits,
			       (unsigned long)s_decodeCacheMisses, missesWindow, (unsigned long)s_decodeCacheEvictions, evictionsWindow);
			/* WHY the misses happened. cap = working set exceeds the cache;
			 * stale = the texture was resident but its VRAM moved. These have
			 * opposite fixes, so never read the miss count without this split.
			 * set = distinct cells touched in the worst frame (saturates at
			 * N3DS_FULLPAGE_RESERVED_CELLS, the whole-page tier's own budget
			 * since the two-tier split -- was N3DS_TOTAL_CELLS-1 before);
			 * dirty = generation bumps since boot. */
			if (g_dbg_statsVerbose)
			printf("miss   cap %-6lu (+%-4lu/w) stale %-6lu (+%-4lu/w)\n", (unsigned long)s_decodeCacheCapacityMisses,
			       capacityWindow, (unsigned long)s_decodeCacheStaleMisses, staleWindow);
			if (g_dbg_statsVerbose)
			printf("set    peak %-4lu /%-4d cells   vramDirty %-8lu\n", (unsigned long)s_frameTouchedPeak,
			       (int)N3DS_FULLPAGE_RESERVED_CELLS, (unsigned long)s_vramDirtyBumps);
			/* Nonzero = a cell was overwritten while a queued draw still
			 * referenced it, i.e. visible texture corruption. */
			if (g_dbg_statsVerbose)
			printf("hazard evictInUse %-8lu\n", (unsigned long)s_evictHazards);
			/* 40 columns: draws/frame is the number worth steering by, the
			 * running total is not. Total moves behind verbose with the rest. */
			printf("draws  ~%lu.%lu/frame  (+%lu/w)      \n", drawsPerFrameAvgX10 / 10UL, drawsPerFrameAvgX10 % 10UL, drawsWindow);
			if (g_dbg_statsVerbose)
			{
				printf("draws  total %-8lu\n", (unsigned long)s_drawCallCount);
			}
			/* op/bl split says where the remaining draws come from;
			 * spill/esc nonzero means capacity tuning is needed. */
			if (g_dbg_statsVerbose)
			printf("merge  op %-8lu bl %-8lu spill %-4lu esc %-4lu\n", (unsigned long)s_mergeEmitOpaque, (unsigned long)s_mergeEmitBlend,
			       (unsigned long)s_mergeBinSpills, (unsigned long)s_mergeEscalations);
			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] merge emitOpaque=%lu emitBlend=%lu spills=%lu escalations=%lu",
			         (unsigned long)s_mergeEmitOpaque, (unsigned long)s_mergeEmitBlend, (unsigned long)s_mergeBinSpills,
			         (unsigned long)s_mergeEscalations);
			N3DS_DebugLog(cacheLine);

			/*
			 * Ordered UI pass. primsMax is the replacement for [PKTDEPTH]
			 * uiMax, which measured a 256-prim budget that no longer exists --
			 * this list's capacity is the live one. dropped MUST stay 0: it counts
			 * UI that could not be recorded (segment list full), and silently
			 * losing UI would read as "everything drew" when it did not.
			 */
			if (g_dbg_statsVerbose)
			{
				printf("uiord  primsMax %-6lu segs %-4d dropped %-6lu\n", (unsigned long)s_uiOrdPrimsMax, N3DS_UIORD_MAX_SEGS,
				       (unsigned long)s_uiOrdDropped);
			}
			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] uiorder primsMax=%lu maxSegs=%d dropped=%lu",
			         (unsigned long)s_uiOrdPrimsMax, N3DS_UIORD_MAX_SEGS, (unsigned long)s_uiOrdDropped);
			N3DS_DebugLog(cacheLine);

			/*
			 * UI probe: the same split as WINDOW DELTAS, with the size of
			 * each class of draw and why the blend ones ended. Deltas, not the
			 * running totals above, because the question is per-config -- the
			 * [SWEEP] harness changes what is on screen every few windows and a
			 * since-boot total cannot follow it. v/d is the number to read:
			 * opaque should be in the hundreds or thousands, and blend near 6
			 * means the blend path is paying a full draw call per handful of
			 * triangles. cfg tags which ablation config was live at the end of
			 * this window; a config change mid-window makes one line mixed, so
			 * compare lines that share a cfg with the [PERF] window next to
			 * them rather than reading any single line in isolation.
			 */

			{
				char blendLine[224];
				const unsigned long opDraws = (unsigned long)(s_mergeEmitOpaque - s_lastStatMergeEmitOpaque);
				const unsigned long blDraws = (unsigned long)(s_mergeEmitBlend - s_lastStatMergeEmitBlend);
				const unsigned long opVerts = (unsigned long)(s_mergeVertsOpaque - s_lastStatMergeVertsOpaque);
				const unsigned long blVerts = (unsigned long)(s_mergeVertsBlend - s_lastStatMergeVertsBlend);

				s_lastStatMergeEmitOpaque = s_mergeEmitOpaque;
				s_lastStatMergeEmitBlend = s_mergeEmitBlend;
				s_lastStatMergeVertsOpaque = s_mergeVertsOpaque;
				s_lastStatMergeVertsBlend = s_mergeVertsBlend;

				snprintf(blendLine, sizeof(blendLine),
				         "[MERGEB] cfg=%s opDraws=%lu opVerts=%lu opV/D=%lu blDraws=%lu blVerts=%lu blV/D=%lu closeAtlas=%lu closeState=%lu closeFrameEnd=%lu",
				         g_dbg_ablateConfig, opDraws, opVerts, opDraws ? (opVerts / opDraws) : 0UL, blDraws, blVerts,
				         blDraws ? (blVerts / blDraws) : 0UL,
				         (unsigned long)(s_blendCloseAtlas - s_lastStatBlendCloseAtlas),
				         (unsigned long)(s_blendCloseState - s_lastStatBlendCloseState),
				         (unsigned long)(s_blendCloseFrameEnd - s_lastStatBlendCloseFrameEnd));
				N3DS_DebugLog(blendLine);

				s_lastStatBlendCloseAtlas = s_blendCloseAtlas;
				s_lastStatBlendCloseState = s_blendCloseState;
				s_lastStatBlendCloseFrameEnd = s_blendCloseFrameEnd;
			}

			snprintf(cacheLine, sizeof(cacheLine),
			         "[CTR-3DS] cache hits=%lu misses=%lu missesWindow=%lu evictions=%lu evictionsWindow=%lu draws=%lu drawsWindow=%lu",
			         (unsigned long)s_decodeCacheHits, (unsigned long)s_decodeCacheMisses, missesWindow,
			         (unsigned long)s_decodeCacheEvictions, evictionsWindow, (unsigned long)s_drawCallCount, drawsWindow);
			N3DS_DebugLog(cacheLine);

			/* Hash-index effectiveness: hashWindow ~= lookups the probe
			 * served this window (the scan never ran); repairs = scan
			 * fallbacks that DID find a fresh entry (bucket collision or
			 * eviction churn re-pointing the bucket). */
			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] cache hashHits=%lu hashWindow=%lu hashRepairs=%lu",
			         (unsigned long)s_fullPageHashHits, (unsigned long)(s_fullPageHashHits - s_lastStatFullPageHashHits),
			         (unsigned long)s_fullPageHashRepairs);
			s_lastStatFullPageHashHits = s_fullPageHashHits;
			N3DS_DebugLog(cacheLine);

			snprintf(cacheLine, sizeof(cacheLine),
			         "[CTR-3DS] miss capacity=%lu capacityWindow=%lu stale=%lu staleWindow=%lu setPeak=%lu/%d vramDirty=%lu",
			         (unsigned long)s_decodeCacheCapacityMisses, capacityWindow, (unsigned long)s_decodeCacheStaleMisses,
			         staleWindow, (unsigned long)s_frameTouchedPeak, (int)N3DS_FULLPAGE_RESERVED_CELLS,
			         (unsigned long)s_vramDirtyBumps);
			N3DS_DebugLog(cacheLine);

			/* Stale attribution + windowed top-offender pages (see the
			 * counter block's comment for what the split means). */
			{
				const unsigned long fpPageW = (unsigned long)(s_fullPageStaleByPageGen - s_lastStatFpStaleByPageGen);
				const unsigned long fpClutW = (unsigned long)(s_fullPageStaleByClutGen - s_lastStatFpStaleByClutGen);
				int topId = -1;
				int nextId = -1;
				u32 topN = 0;
				u32 nextN = 0;
				int p;

				s_lastStatFpStaleByPageGen = s_fullPageStaleByPageGen;
				s_lastStatFpStaleByClutGen = s_fullPageStaleByClutGen;
				for (p = 0; p < 32; p++)
				{
					if (s_fpStaleByPageId[p] > topN)
					{
						nextN = topN;
						nextId = topId;
						topN = s_fpStaleByPageId[p];
						topId = p;
					}
					else if (s_fpStaleByPageId[p] > nextN)
					{
						nextN = s_fpStaleByPageId[p];
						nextId = p;
					}
				}
				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] staleattr fp pageGenW=%lu clutGenW=%lu varHitW=%lu topPage=%d x%lu next=%d x%lu",
				         fpPageW, fpClutW, (unsigned long)(s_fullPageClutVariantHits - s_lastStatFpClutVariantHits),
				         topId, (unsigned long)topN, nextId, (unsigned long)nextN);
				s_lastStatFpClutVariantHits = s_fullPageClutVariantHits;
				N3DS_DebugLog(cacheLine);
				memset(s_fpStaleByPageId, 0, sizeof(s_fpStaleByPageId));
			}

			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] hazard evictInUse=%lu", (unsigned long)s_evictHazards);
			N3DS_DebugLog(cacheLine);

			/* Draw-call structure: window deltas (32-frame window, so /32 for
			 * per-frame; x10 for one decimal without floats). See the counter
			 * block's header for what each mechanism means. */
			{
				const unsigned long switchWindow = (unsigned long)(s_atlasSwitchFlushes - s_lastStatAtlasSwitchFlushes);
				const unsigned long distinctWindow = (unsigned long)(s_distinctAtlasTotal - s_lastStatDistinctAtlasTotal);

				s_lastStatAtlasSwitchFlushes = s_atlasSwitchFlushes;
				s_lastStatDistinctAtlasTotal = s_distinctAtlasTotal;

				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] batch atlasSwitchX10=%lu/frame distinctAtlasX10=%lu/frame distinctPeak=%lu dtriCallsTotal=%lu",
				         (switchWindow * 10UL) / 32UL, (distinctWindow * 10UL) / 32UL,
				         (unsigned long)s_frameDistinctAtlasPeak, (unsigned long)s_drawTrianglesCalls);
				N3DS_DebugLog(cacheLine);

				/* Affinity placements are cumulative (not windowed): the
				 * mechanism only fires on misses, so per-window deltas are
				 * mostly 0 outside warm-up/churn phases. */
				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] affinity fullPage=%lu subPage=%lu",
				         (unsigned long)s_fullPageAffinityPlacements, (unsigned long)s_subPageAffinityPlacements);
				N3DS_DebugLog(cacheLine);
			}

			/*
			 * Sub-page tier diagnostics -- mirrors the full-page block just
			 * above, field-for-field. Full per-tier breakdown goes to the log
			 * only; the console gets one compact routing-summary line below
			 * instead of a full duplicate block -- this console has already
			 * hit its practical width/line limit once before (see the
			 * audio-stats line's own history).
			 */
			{
				const unsigned long subMissesWindow = (unsigned long)(s_subPageCacheMisses - s_subPageLastStatCacheMisses);
				const unsigned long subEvictionsWindow =
				    (unsigned long)(s_subPageCacheEvictions - s_subPageLastStatCacheEvictions);
				const unsigned long subStaleWindow =
				    (unsigned long)(s_subPageCacheStaleMisses - s_subPageLastStatStaleMisses);
				const unsigned long subCapacityWindow =
				    (unsigned long)(s_subPageCacheCapacityMisses - s_subPageLastStatCapacityMisses);

				s_subPageLastStatCacheMisses = s_subPageCacheMisses;
				s_subPageLastStatCacheEvictions = s_subPageCacheEvictions;
				s_subPageLastStatStaleMisses = s_subPageCacheStaleMisses;
				s_subPageLastStatCapacityMisses = s_subPageCacheCapacityMisses;

				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] subpage cache hits=%lu misses=%lu missesWindow=%lu evictions=%lu evictionsWindow=%lu",
				         (unsigned long)s_subPageCacheHits, (unsigned long)s_subPageCacheMisses, subMissesWindow,
				         (unsigned long)s_subPageCacheEvictions, subEvictionsWindow);
				N3DS_DebugLog(cacheLine);

				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] rmemo hits=%lu", (unsigned long)s_resolveMemoHits);
				N3DS_DebugLog(cacheLine);

				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] subpage miss capacity=%lu capacityWindow=%lu stale=%lu staleWindow=%lu setPeak=%lu/%d",
				         (unsigned long)s_subPageCacheCapacityMisses, subCapacityWindow,
				         (unsigned long)s_subPageCacheStaleMisses, subStaleWindow, (unsigned long)s_subPageFrameTouchedPeak,
				         (int)N3DS_SUBPAGE_TOTAL_CELLS);
				N3DS_DebugLog(cacheLine);

				/* Same attribution split as the full-page tier's staleattr
				 * line -- see the counter block's comment. */
				{
					const unsigned long spPageW = (unsigned long)(s_subPageStaleByPageGen - s_lastStatSpStaleByPageGen);
					const unsigned long spClutW = (unsigned long)(s_subPageStaleByClutGen - s_lastStatSpStaleByClutGen);
					int topId = -1;
					int nextId = -1;
					u32 topN = 0;
					u32 nextN = 0;
					int p;

					s_lastStatSpStaleByPageGen = s_subPageStaleByPageGen;
					s_lastStatSpStaleByClutGen = s_subPageStaleByClutGen;
					for (p = 0; p < 32; p++)
					{
						if (s_spStaleByPageId[p] > topN)
						{
							nextN = topN;
							nextId = topId;
							topN = s_spStaleByPageId[p];
							topId = p;
						}
						else if (s_spStaleByPageId[p] > nextN)
						{
							nextN = s_spStaleByPageId[p];
							nextId = p;
						}
					}
					snprintf(cacheLine, sizeof(cacheLine),
					         "[CTR-3DS] staleattr sp pageGenW=%lu clutGenW=%lu varHitW=%lu topPage=%d x%lu next=%d x%lu",
					         spPageW, spClutW, (unsigned long)(s_subPageClutVariantHits - s_lastStatSpClutVariantHits),
					         topId, (unsigned long)topN, nextId, (unsigned long)nextN);
					s_lastStatSpClutVariantHits = s_subPageClutVariantHits;
					N3DS_DebugLog(cacheLine);
					memset(s_spStaleByPageId, 0, sizeof(s_spStaleByPageId));
				}

				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] subpage hazard evictInUse=%lu",
				         (unsigned long)s_subPageEvictHazards);
				N3DS_DebugLog(cacheLine);

				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] subpage hashHits=%lu hashWindow=%lu hashRepairs=%lu",
				         (unsigned long)s_subPageHashHits, (unsigned long)(s_subPageHashHits - s_lastStatSubPageHashHits),
				         (unsigned long)s_subPageHashRepairs);
				s_lastStatSubPageHashHits = s_subPageHashHits;
				N3DS_DebugLog(cacheLine);

				/* s_trianglesRoutedSubPage/FullPage: the single most important
				 * new number (design doc) -- whether the straddling/large-
				 * surface fallback to the full-page tier dominates real
				 * content or stays the minority case this design assumes. */
				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] routing subPage=%lu fullPage=%lu",
				         (unsigned long)s_trianglesRoutedSubPage, (unsigned long)s_trianglesRoutedFullPage);
				N3DS_DebugLog(cacheLine);

				/* [TEXDEC] window roll-up: totals plus the worst single
				 * frame's split, so a one-frame storm stays visible even when
				 * its per-frame line was below threshold. */
				snprintf(cacheLine, sizeof(cacheLine),
				         "[TEXDEC] win sub=%lu full=%lu us=%lu | max frame=%lu us=%lu sub=%lu full=%lu mdlUs=%lu",
				         (unsigned long)s_texdecWinSub, (unsigned long)s_texdecWinFull, (unsigned long)s_texdecWinUs,
				         (unsigned long)s_texdecWinMaxFrame, (unsigned long)s_texdecWinMaxUs,
				         (unsigned long)s_texdecWinMaxSub, (unsigned long)s_texdecWinMaxFull,
				         (unsigned long)s_texdecWinMaxMdlUs);
				N3DS_DebugLog(cacheLine);
				s_texdecWinSub = 0;
				s_texdecWinFull = 0;
				s_texdecWinUs = 0;
				s_texdecWinMaxUs = 0;
				s_texdecWinMaxSub = 0;
				s_texdecWinMaxFull = 0;
				s_texdecWinMaxMdlUs = 0;
				s_texdecWinMaxFrame = 0;

				if (g_dbg_statsVerbose)
				printf("route  sub %-8lu full %-8lu  subMiss %-5lu subEvict %-5lu\n",
				       (unsigned long)s_trianglesRoutedSubPage, (unsigned long)s_trianglesRoutedFullPage,
				       (unsigned long)s_subPageCacheMisses, (unsigned long)s_subPageCacheEvictions);
			}

			/*
			 * Vertex budget occupancy. dropped != 0 means whole draws were
			 * discarded for want of buffer space, i.e. geometry is missing
			 * from the screen -- the failure that previously had no symptom
			 * other than absent models. peak approaching the ceiling is the
			 * early warning for the same thing.
			 */
			if (g_dbg_statsVerbose)
			printf("verts  peak %-7d /%-7d  dropped %-6lu\n", N3DS_C3DFrameVertexHighWater(),
			       (int)N3DS_MAX_FRAME_VERTICES, (unsigned long)s_droppedRuns);

			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] vertsPeak=%d/%d droppedRuns=%lu",
			         N3DS_C3DFrameVertexHighWater(), (int)N3DS_MAX_FRAME_VERTICES, (unsigned long)s_droppedRuns);
			N3DS_DebugLog(cacheLine);

			if (g_dbg_statsVerbose)
			printf("scene  begin %-8lu end %-8lu (vs %lu swaps)\n", (unsigned long)s_beginSceneCount,
			       (unsigned long)s_endSceneCount, frames);

			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] beginScene=%lu endScene=%lu swaps=%lu",
			         (unsigned long)s_beginSceneCount, (unsigned long)s_endSceneCount, frames);
			N3DS_DebugLog(cacheLine);

#if defined(CTR_INTERNAL) && CTR_NATIVE_RENDER_THREAD
			/*
			 * qstall: where the frame's "waiting for the render worker" time
			 * actually lands. waitUs is total main-thread block in RunSync/Wait
			 * over the window; postBlkUs is the slice of that spent stuck on a
			 * full ring (worker behind). Divide by `frames` for per-frame us.
			 * If waitUs/frames approaches render_vsync_drawsync while worker_exec
			 * stays small, the wait is real worker/GPU drain, not queue mechanics;
			 * if postBlkUs is a large share, the ring is the bottleneck.
			 */
			{
				struct NativeRenderQueueDebugStats qs;
				char qline[192];

				NativeRenderQueue_DebugTakeStats(&qs);
				snprintf(qline, sizeof(qline),
				         "[QSTALL] frames=%lu waitCalls=%u waitUs=%lu postCalls=%u postBlk=%u postBlkUs=%lu peakDepth=%u/%u",
				         frames, qs.waitCalls, qs.waitBlockUs, qs.postCalls, qs.postBlockedCalls, qs.postBlockUs, qs.peakDepth,
				         qs.ringSize);
				N3DS_DebugLog(qline);
			}

			/*
			 * workprof: worker-thread wall clock split by job kind, plus idle.
			 * Divide us by `frames` for per-frame. If otag+sync+endFrame+image
			 * sums near render_vsync_drawsync while worker_exec_ms stays ~5ms,
			 * the old bucket was undercounting async work; the largest us here is
			 * where Crash Cove's extra frame time lives. High idleUs instead means
			 * the worker sleeps while the main thread waits -- a signaling stall.
			 */
			{
				struct NativeRenderWorkerDebugStats wp;
				char wpline[192];

				NativeRenderQueue_DebugTakeWorkerStats(&wp);
				snprintf(wpline, sizeof(wpline),
				         "[WORKPROF] frames=%lu otag=%lu/%u sync=%lu/%u endFrame=%lu/%u image=%lu/%u other=%lu/%u idle=%lu/%u",
				         frames, wp.otagUs, wp.otagCalls, wp.syncUs, wp.syncCalls, wp.endFrameUs, wp.endFrameCalls,
				         wp.imageUs, wp.imageCalls, wp.otherUs, wp.otherCalls, wp.idleUs, wp.idleWaits);
				N3DS_DebugLog(wpline);
			}

			/*
			 * otagsplit: the DrawOTag job (WORKPROF's otag=) broken into its
			 * three phases -- patch (moving water/lava rewrite), parse (the
			 * packet OT walk), splits (DrawAllSplits flush). Divide us by
			 * `frames`. This is what tells us whether Crash Cove's extra ~19ms
			 * is the walk-chase (parse) or the flush (splits) -- the resident
			 * track+water draw is in endFrame, not here.
			 */
			{
				struct NativeOtagSplitStats os;
				char osline[160];

				NativePerf_TakeOtagSplit(&os);
				snprintf(osline, sizeof(osline),
				         "[OTAGSPLIT] frames=%lu calls=%u patchUs=%lu parseUs=%lu splitsUs=%lu",
				         frames, os.calls, os.patchUs, os.parseUs, os.splitsUs);
				N3DS_DebugLog(osline);
			}
#endif

			/*
			 * Audio mixer workload shape. The profiler showed audio_vblank_ms
			 * taking 48-81% of the frame, and NativeAudio_MixFrame runs both
			 * the 24-voice loop and the reverb chain once per sample frame
			 * (735 of them per vblank) -- so voicesX10 and reverbSamples say
			 * which of those two is actually carrying the cost.
			 */
			{
				const unsigned long shapeVblanks = s_audioShapeVblanks;
				const unsigned long voicesX10 = (shapeVblanks > 0) ? ((s_audioShapeVoiceAccum * 10UL) / shapeVblanks) : 0UL;

				s_audioShapeVoiceAccum = 0;
				s_audioShapeVblanks = 0;

				if (g_dbg_statsVerbose)
				printf("audio  voices ~%lu.%lu  reverbSmp %-6d xa %d  ndspQ %d drop %d\n", voicesX10 / 10UL, voicesX10 % 10UL,
				       s_audioShapeReverbSamples, s_audioShapeXaActive, N3DS_NdspGetQueuedBlocks(), N3DS_NdspGetDroppedBlocks());

				/* ndspQ = blocks queued at the sink right now (0..16; steady
				 * nonzero = healthy cushion, pinned at 16 = drops imminent);
				 * ndspDrop = cumulative refused submits (pool full / sink
				 * down). Worker-side lag lives in the [PERF] header's
				 * audioBacklog/audioDropped, not here. */
				/* hwKiB = decoded-sample bytes the store is holding right now,
				 * hwHiKiB its tail-cursor high-water mark (the gap is space
				 * the free list reclaimed). denied = key-ons that found all 23
				 * DSP channels sounding; rebind = key-ons that reused an idle
				 * channel another voice held. storeFull = allocations the
				 * store refused, storeLost = key-ons silenced because even
				 * reclaiming could not serve them. */
				{
					char audioLine[256];

					snprintf(audioLine, sizeof(audioLine),
					         "[CTR-3DS] audio voicesX10=%lu vblanks=%lu reverbSamples=%d xaActive=%d ndspQ=%d ndspDrop=%d hwKiB=%d hwHiKiB=%d denied=%d "
					         "rebind=%d storeFull=%d storeLost=%lu storeResets=%d",
					         voicesX10, shapeVblanks, s_audioShapeReverbSamples, s_audioShapeXaActive, N3DS_NdspGetQueuedBlocks(),
					         N3DS_NdspGetDroppedBlocks(), N3DS_HwVoiceStoreGetUsedKiB(), N3DS_HwVoiceStoreGetHiWaterKiB(), N3DS_HwVoiceGetDenied(),
					         N3DS_HwVoiceGetRebinds(), N3DS_HwVoiceStoreGetDrops(), (unsigned long)s_hwStoreLostKeyOns, N3DS_HwVoiceStoreGetResets());
					N3DS_DebugLog(audioLine);

#ifdef CTR_INTERNAL
					/* dupAddr is the store's leak tripwire: a fresh slot taken
					 * for an address the table already covers means the cache
					 * key has stopped being the SPU address alone. */
					snprintf(audioLine, sizeof(audioLine), "[HWSTORE] allocs=%lu allocKiB=%lu dupAddr=%lu dupKiB=%lu grow=%lu released=%lu reclaims=%lu",
					         (unsigned long)s_hwStoreCensus.allocs, (unsigned long)(s_hwStoreCensus.allocBytes >> 10), (unsigned long)s_hwStoreCensus.dupAddr,
					         (unsigned long)(s_hwStoreCensus.dupBytes >> 10), (unsigned long)s_hwStoreCensus.growReleases, (unsigned long)s_hwStoreCensus.releases,
					         (unsigned long)s_hwStoreCensus.reclaimRetries);
					N3DS_DebugLog(audioLine);
#endif
				}

				/*
				 * The per-wake budget breakdown. An earlier measurement established
				 * THAT the worker governs frame rate on XA-music tracks; this
				 * line is what says WHICH job does it. Read wakes first: a
				 * healthy worker wakes ~60x per second of window, so
				 * wakes << 60*seconds means the loop body itself is overrunning
				 * the 16.7ms budget, and the largest of the five microsecond
				 * buckets is then the thing to cut. lockWait is not work at all
				 * -- it is the game thread holding the render lock against us.
				 */
				{
					struct N3DS_AudioWorkerProfile prof;
					int fullFlushes = 0;
					int earlyFlushes = 0;
					/* Its own buffer: nine fields with six-digit microsecond
					 * values overrun cacheLine[128], and the first hardware
					 * run of this line lost pumpUs to truncation. */
					char profLine[224];

					N3DS_NdspGetFlushCounts(&fullFlushes, &earlyFlushes);
					snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] sink aggFull=%d aggEarly=%d dspEmpty=%d", fullFlushes, earlyFlushes, N3DS_NdspGetDspEmptyEvents());
					N3DS_DebugLog(cacheLine);

					N3DS_AudioThreadGetProfile(&prof);
					snprintf(profLine, sizeof(profLine),
					         "[CTR-3DS] audioprof wakes=%d vbl=%d parcels=%d lockWaitUs=%d renderUs=%d lifeUs=%d mirrorUs=%d parcelUs=%d pumpUs=%d", prof.wakes,
					         prof.vblanks, prof.parcels, prof.lockWaitUs, prof.renderUs, prof.lifecycleUs, prof.mirrorUs, prof.parcelUs, prof.pumpUs);
					N3DS_DebugLog(profLine);

					{
						unsigned spanFills = 0;
						unsigned spanSectors = 0;
						unsigned long long spanTicks = 0;

						NativeAudio_TakeXaSpanProfile(&spanFills, &spanSectors, &spanTicks);
						if (spanFills != 0)
						{
							/* SYSCLOCK_ARM11 / 1e6 ticks per microsecond. */
							snprintf(profLine, sizeof(profLine), "[CTR-3DS] xaspan fills=%u sectors=%u readUs=%u", spanFills, spanSectors,
							         (unsigned)(spanTicks / 268ULL));
							N3DS_DebugLog(profLine);
						}
					}

					if (g_dbg_statsVerbose)
					{
						printf("aprof  wake %-5d life %-6d mir %-5d\n", prof.wakes, prof.lifecycleUs, prof.mirrorUs);
						printf("aprof  parc %-6d pump %-6d lock %-6d\n", prof.parcelUs, prof.pumpUs, prof.lockWaitUs);
					}
				}
			}

			/*
			 * Profiler attribution on screen, not only in sdmc:/ctr3ds.log. On
			 * real hardware the SD card is behind the backplate and the top
			 * screen may still be blank, so this console is often the only
			 * output that can be read at all -- a photo of it should answer
			 * "where did the frame go", not just "how fast was it".
			 */
			/*
			 * Why-is-nothing-drawn diagnostics. A hardware run showed draws=0,
			 * cache 0/0, verts=0 while scene begin/end still ran every frame --
			 * so the citro3d bracket opens and closes around nothing, and the
			 * blank top screen is upstream of the renderer entirely.
			 *
			 * MainMain's case 3 can `break` out before Platform_BeginFrame when
			 * Loading.stage != LOAD_IDLE and the VLC table has not arrived,
			 * which would skip the whole render path AND the perf bracket --
			 * matching the observed perf=0.0ms. stage/vlc below say whether
			 * that is what is happening. LOAD_IDLE is -1; LOAD_VLC is -6.
			 *
			 * uvb/dtri separate "the OT walk never ran" from "it ran and
			 * DrawTriangles bailed": uvb counts UpdateVertexBuffer (reached
			 * only via DrawAllSplits), dtri counts DrawTriangles entries.
			 */
			if (g_dbg_statsVerbose)
			printf("load   stage %-4d vlc %-4d           \n", (int)sdata->Loading.stage, (int)sdata->bool_IsLoaded_VlcTable);
			if (g_dbg_statsVerbose)
			printf("call   uvb %-8lu dtri %-8lu    \n", (unsigned long)s_updateVertexBufferCalls, (unsigned long)s_drawTrianglesCalls);

#ifdef CTR_INTERNAL
			/* Everything from here to the end of the block reads the frame
			 * profiler, which is part of the debug surface and is not compiled
			 * into a release build. */
			printf("perf   frame %lu.%lums  work %lu.%lums        \n", (unsigned long)s_perfConsoleTotalUs / 1000UL,
			       ((unsigned long)s_perfConsoleTotalUs % 1000UL) / 100UL, (unsigned long)s_perfConsoleWorkUs / 1000UL,
			       ((unsigned long)s_perfConsoleWorkUs % 1000UL) / 100UL);

			/* WORST frame of the window, beside the average. The average alone
			 * cannot distinguish "every frame is 35ms" from "63 frames at cap
			 * plus one 150ms hitch" -- and those need opposite fixes.
			 * `stats` is what THIS block cost last window (see statsT0). */
			printf("worst  %lu.%lums   stats %lu.%lums      \n", (unsigned long)s_perfConsoleWorstUs / 1000UL,
			       ((unsigned long)s_perfConsoleWorstUs % 1000UL) / 100UL, s_statsBlockUs / 1000UL, (s_statsBlockUs % 1000UL) / 100UL);

			/*
			 * Sweep step. Driven from HERE, not from a frame counter, so a
			 * config is always held for a whole number of stats windows and
			 * its [SWEEP] average is built from exactly the windows it owned.
			 */
			if (g_dbg_sweep)
			{
				char sweepLine[128];

				if (!s_sweepWasOn)
				{
					/* Entering the sweep: start at config 0 rather than
					 * wherever the owner's manual dials happened to be. */
					s_sweepWasOn = 1;
					s_sweepIndex = 0;
					s_sweepWindowsLeft = N3DS_SWEEP_WINDOWS;
					s_sweepSettling = 1;
					s_sweepTotalUs = 0;
					s_sweepWorkUs = 0;
					s_sweepSamples = 0;
					N3DS_SweepApply(s_sweepIndex);
				}
				else if (s_sweepSettling)
				{
					/* Discard the window that straddled the config change. */
					s_sweepSettling = 0;
				}
				else
				{
					s_sweepTotalUs += (unsigned long)s_perfConsoleTotalUs;
					s_sweepWorkUs += (unsigned long)s_perfConsoleWorkUs;
					s_sweepSamples++;

					if (--s_sweepWindowsLeft <= 0)
					{
						if (s_sweepSamples > 0)
						{
							unsigned long t = s_sweepTotalUs / (unsigned long)s_sweepSamples;
							unsigned long w = s_sweepWorkUs / (unsigned long)s_sweepSamples;
							unsigned long fpsX10 = (t > 0) ? (10000000UL / t) : 0UL;

							/* lvl is in the line so windows from different
							 * tracks are never averaged together -- the
							 * confound that made the hand-driven run's
							 * retail-vs-ours comparison untrustworthy. */
							snprintf(sweepLine, sizeof(sweepLine), "[SWEEP] cfg=%s lvl=%d n=%d totalUs=%lu workUs=%lu fps=%lu.%lu",
							         s_sweepConfigs[s_sweepIndex].name, NativePerf_CurrentLevelID(), s_sweepSamples, t, w, fpsX10 / 10UL, fpsX10 % 10UL);
							N3DS_DebugLog(sweepLine);
						}
						s_sweepIndex = (s_sweepIndex + 1) % N3DS_SWEEP_CONFIG_COUNT;
						s_sweepWindowsLeft = N3DS_SWEEP_WINDOWS;
						s_sweepSettling = 1;
						s_sweepTotalUs = 0;
						s_sweepWorkUs = 0;
						s_sweepSamples = 0;
						N3DS_SweepApply(s_sweepIndex);
					}
				}
				printf("SWEEP  %-10s  %d/%d win\n", s_sweepConfigs[s_sweepIndex].name, N3DS_SWEEP_WINDOWS - s_sweepWindowsLeft + 1, N3DS_SWEEP_WINDOWS);
			}
			else if (s_sweepWasOn)
			{
				/*
				 * Leaving the sweep restores every content class. Without this,
				 * switching it off during no-inst leaves the karts gone and the
				 * next thing the owner sees looks like a renderer bug.
				 */
				extern volatile int gCtrDebugSkipInstances;
				extern volatile int gCtrDebugSkipEffects;
				extern volatile int gCtrDebugSkipHUD;
				extern int g_dbg_vsyncCredit;

				gCtrDebugSkipInstances = 0;
				gCtrDebugSkipEffects = 0;
				gCtrDebugSkipHUD = 0;
				g_dbg_vsyncCredit = 1;
				g_dbg_ablateConfig = "none";
				s_sweepWasOn = 0;
			}
			/* The per-bucket breakdown is the single biggest block of text on
			 * this screen and is only meaningful during a profiling pass. */
			if (g_dbg_statsVerbose)
			{
				for (int i = 0; i < NATIVE_PERF_3DS_CONSOLE_BUCKETS; i++)
				{
					printf(" %-24s %lu.%lums   \n", (s_perfConsoleName[i] != NULL) ? s_perfConsoleName[i] : "-",
					       (unsigned long)s_perfConsoleUs[i] / 1000UL, ((unsigned long)s_perfConsoleUs[i] % 1000UL) / 100UL);
				}
			}
#endif /* CTR_INTERNAL */
		}

#undef printf

		/* Blank every row the block did not fill this time. Turning verbose or
		 * the sweep off shortens the layout, and without this the rows they
		 * used would keep their last text forever -- the drip would faithfully
		 * repaint stale lines. */
		while (s_statsRow < N3DS_STATS_ROWS)
		{
			N3DS_StatsEmitRow("");
		}

		/* Closing the timer opened at the top of the block. Displayed on the
		 * NEXT window, so this frame's own cost never inflates the number it
		 * is reporting. */
		{
			const unsigned long long freq = N3DS_GetTickFrequency();

			if (freq != 0ULL)
			{
				s_statsBlockUs = (unsigned long)(((N3DS_GetTickCount() - statsT0) * 1000000ULL) / freq);
			}
		}
	}

	/* One row of the bottom screen, every frame -- see N3DS_StatsDripRow.
	 * Sits here, just before the present that pushes the console framebuffer,
	 * so the row is on screen the same frame it is drawn. */
	N3DS_StatsDripRow();

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_N3DS_GFX_PRESENT);
	N3DS_GfxPresent();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_N3DS_GFX_PRESENT);

	NativePerf_EndScope(NATIVE_PERF_BUCKET_SWAP_WINDOW);
}

void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h)
{
	N3DS_FBStoreSlot *slot;

	/* Runs unconditionally every EndScene, so it doubles as the publish
	 * point for the displayed-content region (see N3DS_SetTopContentFullWidth). */
	{
		extern int g_dbg_widescreen;

		N3DS_SetTopContentFullWidth(g_dbg_widescreen == 2);
	}

	/* Record-and-arm only; the actual refresh is pull-based -- see
	 * N3DS_RefreshFramebufferMirrorSlot. Slot selection by rect anchor:
	 * re-arming a known buffer updates its slot in place, a new distinct
	 * rect evicts the older slot (two live display buffers is the whole
	 * point -- see the two-slot block comment). */
	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	if ((s_fbStore[0].x == x) && (s_fbStore[0].y == y))
	{
		slot = &s_fbStore[0];
	}
	else if ((s_fbStore[1].x == x) && (s_fbStore[1].y == y))
	{
		slot = &s_fbStore[1];
	}
	else
	{
		slot = &s_fbStore[1 - s_fbStoreLastArmed];
	}

	slot->x = x;
	slot->y = y;
	slot->w = w;
	slot->h = h;
	slot->stale = 1;
	s_fbStoreLastArmed = (int)(slot - s_fbStore);
	s_fbMirrorStale = 1;
}

/* Scratch for one tiled VRAM-display chunk; reused every call, never
 * persisted across frames. Mirrors s_decodeScratch's role for
 * N3DS_DecodeAndTilePage above. */
global_variable u16 s_vramDisplayScratch[N3DS_VRAM_DISPLAY_TEX_DIM * N3DS_VRAM_DISPLAY_TEX_DIM];

/*
 * Tiles a [x,y,w,h) rect of the emulated VRAM into a 256x256 RGBA5551 buffer
 * for NativeRenderer_PresentVRAMRect below. Reuses N3DS_DecodeAndTilePage's
 * Morton/Z-order tile addressing and BGR555->RGBA5551 channel packing
 * verbatim -- same bug class a byte-order fix already landed in for that
 * function, not worth re-deriving here. Alpha is forced always-opaque
 * (bit set) rather than PSX's conditional punch-through test: this is a
 * straight present of a VRAM rect, not a masked game texture, matching PC's
 * NativeRenderer_BuildDisplayChunkRGBA's unconditional 0xff.
 */
internal void N3DS_TileVRAMDisplayChunk(int srcX, int srcY, int w, int h, u16 *outTiled256)
{
	int v;

	N3DS_VramSettle(srcX, srcY, (w < N3DS_VRAM_DISPLAY_TEX_DIM) ? w : N3DS_VRAM_DISPLAY_TEX_DIM, (h < N3DS_VRAM_DISPLAY_TEX_DIM) ? h : N3DS_VRAM_DISPLAY_TEX_DIM);

	for (v = 0; v < N3DS_VRAM_DISPLAY_TEX_DIM; v++)
	{
		int u;

		for (u = 0; u < N3DS_VRAM_DISPLAY_TEX_DIM; u++)
		{
			u16 color16 = 0;
			int morton, tileIndex, dstIndex;

			if ((u < w) && (v < h))
			{
				color16 = vram[(srcY + v) * VRAM_WIDTH + (srcX + u)];
			}

			morton = (u & 1) | ((v & 1) << 1) | ((u & 2) << 1) | ((v & 2) << 2) | ((u & 4) << 2) | ((v & 4) << 3);
			tileIndex = ((v >> 3) * (N3DS_VRAM_DISPLAY_TEX_DIM / 8) + (u >> 3)) * 64;
			dstIndex = tileIndex + morton;

			outTiled256[dstIndex] = (u16)(((color16 & 0x001Fu) << 11) |      /* R */
			                              (((color16 >> 5) & 0x1Fu) << 6) |   /* G */
			                              (((color16 >> 10) & 0x1Fu) << 1) |  /* B */
			                              1u);                                /* always opaque */
		}
	}
}

void NativeRenderer_PresentVRAMRect(int displayX, int displayY, int displayW, int displayH)
{
	const int maxChunk = N3DS_VRAM_DISPLAY_TEX_DIM;
	int y;
	int chunkSlot = 0;
	/* DIAGNOSTIC: bounded entry/exit trace pairing
	 * with the FrameBegin/FrameEnd traces in n3ds_backend.c -- together they
	 * bracket every stage of the boot-splash present that real hardware
	 * freezes inside. */
	static int s_presentTraceBudget = 16;
	const int trace = (s_presentTraceBudget > 0);

	if (trace)
	{
		s_presentTraceBudget--;
		N3DS_DebugLog("[CTR-3DS] PresentVRAMRect: enter");
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		return;
	}

	/* Foreign (non-atlas) draw: everything deferred must be recorded
	 * first, both for paint order and because the TEV change below would
	 * otherwise apply to it. The TEV latch is invalidated so the next
	 * DrawTriangles re-asserts its own scale. */
	N3DS_MergeFlush();
	s_runUntextured = -1;

	N3DS_C3DSetColorDoubling(0);

	for (y = 0; y < displayH; y += maxChunk)
	{
		const int chunkH = ((displayH - y) > maxChunk) ? maxChunk : (displayH - y);
		int x;

		for (x = 0; x < displayW; x += maxChunk)
		{
			const int chunkW = ((displayW - x) > maxChunk) ? maxChunk : (displayW - x);
			/*
			 * Pre-scaled by (1024/maxChunk), not left as a plain [0,1]
			 * fraction like the old float u1/v1 were: ctr.v.pica now applies
			 * a single, unconditional *1/1024 to every vertex's texcoord
			 * (design doc "Lever 2") because one
			 * vertex shader is shared by every draw call in this renderer,
			 * not just DrawTriangles' atlas-cell path. This quad samples a
			 * dedicated, non-atlas s_vramDisplayTex directly in [0,1]
			 * (N3DS_C3DBindVramDisplayTex -- no atlas-cell offset involved),
			 * so what has to reach the shader is the corner's normalized UV
			 * pre-multiplied back up by the shader's texcoord denominator --
			 * 2048 since the half-texel numerator change (the atlas path
			 * carries 2*texel-1; this edge-normalized [0,1] quad just doubles
			 * its numerators to keep the same normalized value, no bias).
			 * Exact, not approximated: maxChunk (256) divides 2048 evenly.
			 */
			const s16 u1 = (s16)(chunkW * (2048 / maxChunk));
			/* PICA's V = 1 is the texture's first stored row, which the tiler
			 * filled with the chunk's top row. Rows past chunkH are padding,
			 * so the quad spans V = 1 down to 1 - chunkH/256, not 0..chunkH. */
			const s16 vTop = 2048;
			const s16 vBottom = (s16)(2048 - (chunkH * (2048 / maxChunk)));
			/* Which pool texture this chunk uploads to and draws from. Must
			 * be distinct per chunk within this call -- see the header
			 * comment on N3DS_C3DUploadVramDisplayChunk for why reusing one
			 * texture across chunks in the same still-open frame silently
			 * makes every chunk's draw sample whichever chunk uploaded last.
			 * Wraps defensively rather than assuming displayW/H never
			 * exceeds the pool's 2x2-chunk (512x512) coverage. */
			const int slot = chunkSlot % N3DS_VRAM_DISPLAY_TEX_COUNT;
			N3DS_Vertex verts[6];
			int baseVertex;
			int i;

			chunkSlot++;

			N3DS_TileVRAMDisplayChunk(displayX + x, displayY + y, chunkW, chunkH, s_vramDisplayScratch);
			N3DS_C3DUploadVramDisplayChunk(slot, s_vramDisplayScratch);

			verts[0].x = (s16)x;
			verts[0].y = (s16)y;
			verts[0].u = 0;
			verts[0].v = vTop;

			verts[1].x = (s16)x;
			verts[1].y = (s16)(y + chunkH);
			verts[1].u = 0;
			verts[1].v = vBottom;

			verts[2].x = (s16)(x + chunkW);
			verts[2].y = (s16)(y + chunkH);
			verts[2].u = u1;
			verts[2].v = vBottom;

			verts[3] = verts[0];
			verts[4] = verts[2];

			verts[5].x = (s16)(x + chunkW);
			verts[5].y = (s16)y;
			verts[5].u = u1;
			verts[5].v = vTop;

			for (i = 0; i < 6; i++)
			{
				/* Full-scale white -- ctr.v.pica's colorScale turns raw 255
				 * back into 1.0, same as every other vertex's r/g/b now (see
				 * the main conversion loop above). Alpha is no longer a
				 * struct field: the shader supplies a constant 1.0 for it
				 * instead (design doc finding 3). */
				verts[i].z = N3DS_DEPTH_TOPMOST;
				/* Parallax = z: the VRAM/splash present quad sits flat at
				 * screen depth in stereo (near-band mask), same as the atlas
				 * inspector's overlay. */
				verts[i].w = N3DS_DEPTH_TOPMOST;
				verts[i].r = 255;
				verts[i].g = 255;
				verts[i].b = 255;
			}

			/* ABLATION A (hardware GPU-wedge hunt):
			 * submit the splash frames with clear+transfer but ZERO draws.
			 * Real hardware never completes the first submitted command list
			 * (FrameBegin #2 waits forever); this splits {clear/transfer}
			 * from {draw state}. If boot proceeds past the splash with this
			 * on, the wedge is in the draw path -- and the first real menu
			 * frame (DrawTriangles, not skipped here) then tests the shader
			 * itself. REVERT after the test. */
			if (g_dbg_skipSplashDraws)
			{
				continue;
			}

			N3DS_C3DBindVramDisplayTex(slot);
			baseVertex = N3DS_TvboCheckedAppend(verts, 6);
			if (baseVertex >= 0)
			{
				N3DS_C3DDrawTriangles(baseVertex, 2);
				/* Stereo journal: the right eye redraws this chunk from the
				 * same slot -- whose texels persist until the next
				 * PresentVRAMRect call, i.e. past frame end -- at
				 * N3DS_DEPTH_TOPMOST, which the shader's UI-band mask pins
				 * to screen depth in both eyes. Without this, a splash/
				 * loading frame with the slider up would show content to one
				 * eye only. */
				N3DS_StereoRecordVram(slot, baseVertex);
			}
		}
	}

	if (trace)
	{
		N3DS_DebugLog(g_dbg_skipSplashDraws ? "[CTR-3DS] PresentVRAMRect: done (draws SKIPPED, ablation A)"
		                                    : "[CTR-3DS] PresentVRAMRect: done (draws queued)");
	}
}

void NativeRenderer_PresentVRAMDisplay(void)
{
	NativeRenderer_PresentVRAMRect(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
}

void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int readFromFramebuffer)
{
	(void)outputFileName; (void)x; (void)y; (void)width; (void)height; (void)readFromFramebuffer;
}

void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	/* s46: retail's backdrop is a GPU FILL packet (CAM_ClearScreen -- flat
	 * clearColor split at the horizon) reaching here via ClearImage; PC
	 * honours it as a scissored framebuffer clear, and ignoring it left raw
	 * black above the skybox dome's rim. Emitted as a far-pin quad into the
	 * BACKDROP accumulator -- NOT s_skyVerts: the fill is walked AFTER the
	 * sky (bucket 0x3FF is head-first and the sky prims are prepended later
	 * in the frame), so an append there painted over the dome. See the
	 * s_backdropVerts block for the structural-order argument.
	 *
	 * The overlap math against the live DISPENV mirrors the PC implementation
	 * (native_renderer.c) so the two renderers agree on which part of a fill
	 * is visible backdrop vs off-screen VRAM housekeeping. */
	int displayX = activeDispEnv.disp.x;
	int displayY = activeDispEnv.disp.y;
	int displayW = activeDispEnv.disp.w;
	int displayH = activeDispEnv.disp.h;
	int overlapX, overlapY, overlapRight, overlapBottom;
	int relX, relY, relRight, relBottom;
	int uWhite, vWhite, k;
	N3DS_Vertex *dst;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	/* Game-thread direct caller (Platform_BeginScene's isbg clear) while the
	 * render worker owns the accumulator: skip, as the stub always did. FILL
	 * packets arrive via the walk and queued ClearImage jobs, both of which
	 * execute on the walk's thread. */
	if (NativeRenderQueue_Route())
	{
		return;
	}

	if ((s_backdropVertUsed + 6) > N3DS_BACKDROP_VERT_CAP)
	{
		return;
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		displayX = activeDrawEnv.clip.x;
		displayY = activeDrawEnv.clip.y;
		displayW = activeDrawEnv.clip.w;
		displayH = activeDrawEnv.clip.h;
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		return;
	}

	overlapX = (x > displayX) ? x : displayX;
	overlapY = (y > displayY) ? y : displayY;
	overlapRight = ((x + w) < (displayX + displayW)) ? (x + w) : (displayX + displayW);
	overlapBottom = ((y + h) < (displayY + displayH)) ? (y + h) : (displayY + displayH);

	if ((overlapRight <= overlapX) || (overlapBottom <= overlapY))
	{
		return;
	}

	relX = overlapX - displayX;
	relY = overlapY - displayY;
	relRight = overlapRight - displayX;
	relBottom = overlapBottom - displayY;

	/* REAL-widescreen (mode 2) widens the ortho window past the display rect
	 * (N3DS_C3DSetProjection); a backdrop fill that reaches a display edge
	 * must keep going to the ortho edge or the widened margin shows black. */
	{
		extern int g_dbg_widescreen;

		if (g_dbg_widescreen == 2)
		{
			/* + stereo overscan: each eye shifts the far-pinned fill
			 * horizontally by up to the far-plane parallax, which would pull
			 * an exactly-fitted edge on-screen. Mono clips the overshoot --
			 * the wide window edge is the physical screen edge. */
			const int extra = (displayW * (400 - N3DS_TOP_CONTENT_W)) / (2 * N3DS_TOP_CONTENT_W) + N3DS_StereoSkyPadX();

			if (relX == 0)
			{
				relX = -extra;
			}
			if (relRight == displayW)
			{
				relRight = displayW + extra;
			}
		}
	}

	uWhite = ((N3DS_WHITE_CELL_X * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) + 1;
	vWhite = 2047 - ((N3DS_WHITE_CELL_Y * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1);

	dst = &s_backdropVerts[s_backdropVertUsed];
	/* Far pin: RAW stamp 0, and N3DS_Vertex carries z = raw - 32768 (the
	 * direct-emit mapping), so the vertex value is -32768 -- z=0 here would
	 * be raw 32768, a MID-RANGE depth that buried the whole sky (the first
	 * two attempts' actual defect). w likewise: parallax at infinity is raw
	 * 0 on the same biased axis. */
	dst[0].x = (s16)relX;
	dst[0].y = (s16)relY;
	dst[1].x = (s16)relRight;
	dst[1].y = (s16)relY;
	dst[2].x = (s16)relX;
	dst[2].y = (s16)relBottom;
	dst[3].x = (s16)relRight;
	dst[3].y = (s16)relY;
	dst[4].x = (s16)relX;
	dst[4].y = (s16)relBottom;
	dst[5].x = (s16)relRight;
	dst[5].y = (s16)relBottom;
	for (k = 0; k < 6; k++)
	{
		dst[k].z = -32768;
		dst[k].w = -32768;
		dst[k].u = (s16)uWhite;
		dst[k].v = (s16)vWhite;
		dst[k].r = r;
		dst[k].g = g;
		dst[k].b = b;
	}
	s_backdropVertUsed += 6;
}

void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	const u16 colour = NativeRenderer3DS_PackBGR555(r, g, b);

	/* Called directly from game code (MM_Scrapbook) as well as from ClearImage
	 * jobs on the worker; the fence no-ops on the worker and when the queue is
	 * inactive, so only a game-thread caller pays it -- and must, or it races
	 * the in-flight walk's VRAM reads. */
	NativeRenderQueue_Fence();

	if ((w <= 0) || (h <= 0) || (x < 0) || (y < 0))
	{
		return;
	}

	if (((x + w) > VRAM_WIDTH) || ((y + h) > VRAM_HEIGHT))
	{
		return;
	}

	N3DS_DirtyVRAMRect(x, y, w, h);
	N3DS_VramDeferFill(x, y, w, h, colour);
}

void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY)
{
	int stride = w;
	u16 *dst;
	int row;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	if (src == NULL)
	{
		/* MoveImage: VRAM-to-VRAM. If the source overlaps an armed display
		 * rect, pull the last presented frame into the mirror first --
		 * screen-copy effects MoveImage the display area. */
		N3DS_FBRefreshIfOverlaps(x, y, w, h);
		N3DS_VramSettle(x, y, w, h);
		src = vram;
		stride = VRAM_WIDTH;
	}

	N3DS_VramSettle(dstX, dstY, w, h);

	src += x + (y * stride);
	dst = vram + dstX + (dstY * VRAM_WIDTH);

	N3DS_DirtyVRAMRect(dstX, dstY, w, h);

	for (row = 0; row < h; row++)
	{
		memcpy(dst, src, (size_t)w * sizeof(u16));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH)
{
	u16 *src;
	int row;

	/* StoreImage reading the display area (screenshot-style effects): same
	 * freeze-frame pull as the decode-path trigger. */
	N3DS_FBRefreshIfOverlaps(x, y, dstW, dstH);
	N3DS_VramSettle(x, y, dstW, dstH);

	src = vram + x + (VRAM_WIDTH * y);

	for (row = 0; row < dstH; row++)
	{
		memcpy(dst, src, (size_t)dstW * sizeof(u16));
		dst += dstW;
		src += VRAM_WIDTH;
	}
}

void NativeRenderer_UpdateVRAM(void)
{
}

void NativeRenderer_ReadFramebufferDataToVRAM(void)
{
}

void NativeRenderer_DiscardFramebufferReadback(void)
{
	/* Pinned-VRAM presentation (movies, direct frame uploads) skips
	 * StoreFrameBuffer, and the display-region VRAM already holds exact
	 * LoadImage'd content -- a lingering armed slot from before the movie
	 * would overwrite it with a resampled screen copy on the next
	 * overlapping read. Disarm both slots (PC's version discards its
	 * pending GPU readback for the same reason -- native_platform.c). */
	s_fbStore[0].stale = 0;
	s_fbStore[1].stale = 0;
	s_fbMirrorStale = 0;
}

int NativeRenderer_GetVRAMStateSize(void)
{
	return (int)sizeof(vram);
}

int NativeRenderer_CaptureVRAMState(void *dst, int dstSize)
{
	if ((dst == NULL) || (dstSize < (int)sizeof(vram)))
	{
		return 0;
	}

	N3DS_VramSettleAll();
	memcpy(dst, vram, sizeof(vram));
	return 1;
}

int NativeRenderer_RestoreVRAMState(const void *src, int srcSize)
{
	if ((src == NULL) || (srcSize < (int)sizeof(vram)))
	{
		return 0;
	}

	memcpy(vram, src, sizeof(vram));
	s_vramPendingCount = 0;
	return 1;
}

TextureID NativeRenderer_GetVRAMTexture(void)
{
	return N3DS_TEXTURE_ID_VRAM;
}

TextureID NativeRenderer_GetWhiteTexture(void)
{
	return N3DS_TEXTURE_ID_WHITE;
}

void NativeRenderer_SetBlendMode(BlendMode blendMode)
{
	s_currentBlendMode = (int)blendMode;
}

void NativeRenderer_SetStencilMode(int drawPrim)
{
	(void)drawPrim;
}

void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, const DISPENV *displayEnv, int enable)
{
	(void)offscreenRect; (void)displayEnv; (void)enable;
}

void NativeRenderer_SetupClipMode(const RECT16 *clipRect, const DISPENV *displayEnv, int enable)
{
	(void)clipRect; (void)displayEnv; (void)enable;
}

void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat)
{
#ifdef CTR_INTERNAL
	if (g_dbg_texturelessMode)
	{
		texture = N3DS_TEXTURE_ID_WHITE;
	}
#endif
	s_currentTextureId = texture;
	s_currentTexFormat = texFormat;
}

void NativeRenderer_SetOverrideTextureSize(int width, int height)
{
	(void)width; (void)height;
}

void NativeRenderer_SetPSXTextureSemiTransPass(int pass)
{
	s_semiTransPass = pass;
}

void NativeRenderer_SetPSXTextureOutputSTP(int enabled)
{
	(void)enabled;
}

void NativeRenderer_SetPSXDrawMaskSet(int maskSet)
{
	(void)maskSet;
}

void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int count)
{
	/*
	 * Mirrors PC's own pattern (DrawAllSplits -> UpdateVertexBuffer once,
	 * then DrawTriangles many times at different offsets into it): just
	 * remember where the frame's vertices live. s_gpu.vertexBuffer
	 * (native_gpu.c) is a persistent static array, not stack memory, so
	 * holding this pointer across the DrawSplit loop that follows is safe.
	 * The real GrVertex -> N3DS_Vertex conversion happens lazily in
	 * DrawTriangles, which is where texFormat (from SetTexture) and each
	 * triangle's own page/clut are actually known.
	 */
	s_updateVertexBufferCalls++;
	s_pendingVertices = vertices;
	s_pendingVertexCount = count;
}

void NativeRenderer_DrawTriangles(int startVertex, int triangles)
{
	int i;
	const int untextured = (s_currentTextureId == N3DS_TEXTURE_ID_WHITE);
	/* Per-texel STP: which decode view this call's triangles resolve
	 * through -- constant for the whole call, since s_semiTransPass is
	 * latched per DrawSplit pass (see its comment). */
	const N3DS_TexView texView = (s_semiTransPass == 1)   ? N3DS_TEXVIEW_PASS1
	                           : ((s_semiTransPass == 2) ? N3DS_TEXVIEW_PASS2 : N3DS_TEXVIEW_OPAQUE);
	/* Read once per call: keeps every Begin/End below paired even if the
	 * dumpreq toggle flips between calls (it polls in N3DS_ReadButtons, so
	 * never mid-call -- this is belt-and-braces, not load-bearing). */
	const int perfScopes = g_dbg_perfTriScopes;

	s_drawTrianglesCalls++;

	if (!s_rendererInitialised || (s_pendingVertices == NULL) || (triangles <= 0))
	{
		return;
	}

#ifdef CTR_INTERNAL
	if (g_dbg_atlasView > 0)
	{
		/* Atlas inspector active: suppress the game's draws so the screen
		 * shows ONLY the atlas (EndScene draws it). Decode/caching still
		 * runs untouched -- the point is to view its output memory. */
		return;
	}
#endif

	if ((s_currentTextureId != N3DS_TEXTURE_ID_VRAM) && (s_currentTextureId != N3DS_TEXTURE_ID_WHITE))
	{
		/* TF_32_BIT_RGBA override texture -- provably unreachable from any
		 * real game/ code path (zero DR_PSYX_TEX call sites; the boot
		 * splash uses NativeRenderer_PresentVRAMDisplay/PresentVRAMRect
		 * instead, a separate presentation path entirely). Skip rather
		 * than guess at an untested path -- see the file header. */
		return;
	}

	if ((startVertex < 0) || ((startVertex + (triangles * 3)) > s_pendingVertexCount))
	{
		return;
	}

	// Opened only after every early return above, so the scope is always
	// balanced without an end on each bail-out path.
	if (perfScopes)
	{
		NativePerf_BeginScope(NATIVE_PERF_BUCKET_RENDERER_DRAW_TRIANGLES);
	}

	/*
	 * Merge frame: no immediate GPU state calls at all -- state is set at the
	 * barrier, per emitted draw. Opaque calls touch nothing here (bins are
	 * self-keyed and the blend run legally stays open across them); a blend
	 * call closes the run only on a real state change, then stamps the run
	 * descriptors with this call's state -- N3DS_MergeCloseBlendRun reads them
	 * at close time, so they must always describe what the run is
	 * accumulating.
	 */
	if (s_currentBlendMode != 0 /* BM_NONE */)
	{
		if ((s_runVertCount > 0) && ((untextured != s_runUntextured) || (s_currentBlendMode != s_runBlendMode)))
		{
			N3DS_MergeCloseBlendRun(N3DS_BLEND_CLOSE_STATE);
		}
		s_runUntextured = untextured;
		s_runBlendMode = s_currentBlendMode;
	}

	/*
	 * PC's AddSplit coalesces draw calls by blend/format/textureId/mask/
	 * STP/clip/dfe but never by page or CLUT (native_gpu.c:895-901, a
	 * literal FIXME) -- one PC draw call can legitimately span several
	 * texture pages/palettes, relying on true per-vertex resolution in its
	 * shader. citro3d can only bind one texture per draw call, so this walks
	 * the split's triangles (each one's 3 vertices always share a single
	 * page/clut -- see MakeTexcoordTriangle/Quad/Rect), resolves each to its
	 * atlas cell, and flushes a new citro3d draw only when the ATLAS changes
	 * -- not the cell -- since multiple cells of the same atlas share one
	 * bind (see n3ds_backend.h's header comment on N3DS_C3DUploadDecodedCell
	 * for why this exists: measured ~150-290 draws/frame before it did).
	 * Vertex conversion happens inline per triangle (not deferred to a
	 * separate per-run pass like the single-texture design this replaced)
	 * because a run can now span multiple distinct cells, each needing its
	 * own UV offset -- there is no single (page,clut) left to convert against
	 * once triangles from different cells share a run. Triangles are never
	 * reordered, only grouped by contiguous atlas -- draw order is preserved
	 * exactly, which matters because this renderer has no depth test enabled
	 * (matching PC) and relies on draw order alone for correct layering.
	 */
	for (i = 0; i < triangles; i++)
	{
		const GrVertex *triVerts = &s_pendingVertices[startVertex + (i * 3)];
		int atlasIdx, cellX = 0, cellY = 0, pixelBaseU = 0, pixelBaseV = 0, originU = 0, originV = 0;

		/*
		 * A zero-area triangle covers no pixels on the PS1 and none here, so it
		 * is dropped before its texture is resolved. Its UVs still name texels:
		 * the race standings portraits squash to a flat line while they flip,
		 * with V reaching up into the display buffer, and resolving that
		 * footprint costs a framebuffer mirror refresh and a full-page decode
		 * every frame for a primitive that draws nothing.
		 */
		if ((((s32)triVerts[1].x - triVerts[0].x) * ((s32)triVerts[2].y - triVerts[0].y)) ==
		    (((s32)triVerts[2].x - triVerts[0].x) * ((s32)triVerts[1].y - triVerts[0].y)))
		{
			continue;
		}

		if (perfScopes)
		{
			NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_RESOLVE);
		}
		if (untextured)
		{
			atlasIdx = N3DS_WHITE_ATLAS;
			cellX = N3DS_WHITE_CELL_X;
			cellY = N3DS_WHITE_CELL_Y;
		}
		else
		{
			/* Routes this triangle independently to the sub-page tier
			 * (common case) or the full-page tier (fallback for a UV
			 * footprint that doesn't fit one 64x64 tile) -- must run per
			 * triangle, not once per (page,clut): two triangles sharing a
			 * texture can still have different UV footprints (e.g. a small
			 * decal vs. a large floor triangle both sampling the same page).
			 * See N3DS_ResolveTriangleTexture. */
			if (g_dbg_forceSingleTexture)
			{
				/* Diagnostic: pin every textured triangle to one fixed texture
				 * (first textured triangle of the frame), full-page path, real
				 * UVs. See g_dbg_forceSingleTexture's declaration. */
				int fCellX = 0, fCellY = 0;

				if (!s_forcedTexValid)
				{
					s_forcedPage = triVerts[0].page;
					s_forcedClut = triVerts[0].clut;
					s_forcedFormat = s_currentTexFormat;
					s_forcedTexValid = 1;
				}

				N3DS_CellIndexToAtlasCoord(N3DS_FindOrDecodeTexture(s_forcedPage, s_forcedClut, s_forcedFormat, N3DS_TEXVIEW_OPAQUE, 0),
				                            &atlasIdx, &fCellX, &fCellY);
				pixelBaseU = fCellX * N3DS_DECODE_TEXTURE_DIM;
				pixelBaseV = fCellY * N3DS_DECODE_TEXTURE_DIM;
				originU = 0;
				originV = 0;
			}
			else
			{
				N3DS_ResolveTriangleTexture(&triVerts[0], &triVerts[1], &triVerts[2], triVerts[0].page, triVerts[0].clut,
				                             s_currentTexFormat, texView, &atlasIdx, &pixelBaseU, &pixelBaseV, &originU,
				                             &originV);
			}
		}
		if (perfScopes)
		{
			NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_RESOLVE);
		}

		/*
		 * UI leaves the state bins HERE, before the opaque branch below, so
		 * that blended UI is caught by the same test (the opaque branch takes
		 * BM_NONE only) and keeps its true order against opaque UI. Replayed in
		 * submission order at the end of the frame -- see the s_uiOrd* block.
		 *
		 * Gated on packet depth being live: with it off the stamp is an emit
		 * ordinal, not a depth, and the band test would misfire -- the same gate
		 * the widescreen ramp and the stereo mask already use.
		 */
		{
			extern int g_dbg_packetDepth;

			if (g_dbg_packetDepth && (triVerts[0].emitOrdinal >= 0xFFC0u))
			{
				N3DS_Vertex *uiDst;

				/* Bottom-screen minimap: the resolver stamps the map block's
				 * primitives one below the pin (see the s_btmm* block); peel
				 * them into the parallel list BEFORE run coalescing, so a map
				 * triangle can never share a segment with ordinary UI. The FPS
				 * overlay's glyphs carry their own stamp and their own list,
				 * because they draw through a different window. */
				if (g_dbg_btmMap && (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_BTMMAP))
				{
					uiDst = N3DS_BtmMapAppend(s_currentBlendMode, untextured ? 0 : 1, atlasIdx);
				}
				else if (g_dbg_btmMap && (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_BTMFPS))
				{
					uiDst = N3DS_BtmFpsAppend(s_currentBlendMode, untextured ? 0 : 1, atlasIdx);
				}
				else
				{
					uiDst = N3DS_UiOrderAppend(s_currentBlendMode, untextured ? 0 : 1, atlasIdx);
				}

				if (uiDst != NULL)
				{
					if (perfScopes)
					{
						NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
					}
					N3DS_ConvertTriangleVerts(uiDst, triVerts, untextured, cellX, cellY, pixelBaseU, pixelBaseV, originU, originV);
					/* Unrouted map triangles (bottom screen off) and
					 * picture-sprite triangles (menu/hub map sheets -- never
					 * routed) draw in the ordinary ordered-UI list, where
					 * layering is draw order under EQUAL depth -- the pin.
					 * Their stamps are off-pin (kept for the UV inset), so
					 * re-pin z/w here: leaving them a step farther would let
					 * any earlier-drawn pinned quad's rect occlude them. The
					 * routed leg keeps 0xFFFD -- the bottom pass needs the
					 * map a step behind the FPS text's 0xFFFE. */
					if ((triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_UIPIC) || (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_HUDSHINE) ||
					    (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_SCREENFADE) ||
					    (!g_dbg_btmMap && (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_BTMMAP)))
					{
						const s16 pinned = (s16)(0xFFFFu - 32768);

						uiDst[0].z = pinned;
						uiDst[1].z = pinned;
						uiDst[2].z = pinned;
						uiDst[0].w = pinned;
						uiDst[1].w = pinned;
						uiDst[2].w = pinned;
					}
					if (perfScopes)
					{
						NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
					}
				}
				/* Map mode: the routed copy above feeds the bottom panel;
				 * emit a SECOND copy into the ordinary ordered-UI list so
				 * the top screen keeps retail's map (see g_dbg_mapBoth).
				 * Re-pin exactly like the unrouted leg -- an off-pin stamp
				 * among 0xFFFF pins would let any earlier-drawn quad's rect
				 * occlude the map. The far-edge UV inset keys off the stamp
				 * inside ConvertTriangleVerts, so this copy gets it too.
				 * Speedo-mode spans report retailTop=0 and are never copied
				 * (RouteActive's legality rests on routed prims staying off
				 * the top screen there). Independent of the bottom append's
				 * success: a full bottom list must not hole the top map. */
				if (g_dbg_btmMap && g_dbg_mapBoth && (triVerts[0].emitOrdinal == PKTDEPTH_UI_STAMP_BTMMAP) &&
				    ((g_dbg_mapBoth >= 2) || NativeGpu_UiMapRetailTop()))
				{
					N3DS_Vertex *topDst = N3DS_UiOrderAppend(s_currentBlendMode, untextured ? 0 : 1, atlasIdx);

					s_mbTopCopyTris++;
					if (topDst != NULL)
					{
						const s16 pinned = (s16)(0xFFFFu - 32768);

						if (perfScopes)
						{
							NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
						}
						N3DS_ConvertTriangleVerts(topDst, triVerts, untextured, cellX, cellY, pixelBaseU, pixelBaseV, originU,
						                          originV);
						topDst[0].z = pinned;
						topDst[1].z = pinned;
						topDst[2].z = pinned;
						topDst[0].w = pinned;
						topDst[1].w = pinned;
						topDst[2].w = pinned;
						if (perfScopes)
						{
							NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
						}
					}
				}
				continue;
			}
		}

		if (s_currentBlendMode == 0 /* BM_NONE */)
		{
			/* Opaque under merge: no run, no order -- straight into the
			 * per-state bin; the depth test owns layering. Untextured
			 * always resolves to the white cell, so it gets the one
			 * non-atlas-keyed bin. */
			const int bin = untextured ? N3DS_MERGE_UNTEX_BIN : atlasIdx;

			if ((s_mergeBinUsed[bin] + 3) > N3DS_MERGE_BIN_CAP_VERTS)
			{
				N3DS_MergeSpillBin(bin);
			}

#ifdef CTR_INTERNAL
			N3DS_TriDumpRecord(triVerts, !untextured, untextured ? 255 : (int)s_currentTexFormat, atlasIdx, pixelBaseU, pixelBaseV, originU, originV);
#endif

			if (perfScopes)
			{
				NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
			}
			N3DS_ConvertTriangleVerts(&s_mergeBinVerts[bin][s_mergeBinUsed[bin]], triVerts, untextured, cellX, cellY, pixelBaseU,
			                          pixelBaseV, originU, originV);
			if (perfScopes)
			{
				NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
			}
			s_mergeBinUsed[bin] += 3;
			continue;
		}

		if ((atlasIdx != s_runAtlas) || ((s_runVertCount + 3) > N3DS_MAX_RUN_VERTICES))
		{
			/* Attribute the flush: only a MID-RUN atlas change is the
			 * potentially-reducible kind (s_runVertCount > 0 excludes the
			 * frame's first triangle, which flushes nothing). Blend/TEV
			 * changes were already handled at call entry, so an atlas change
			 * is the only break left here. */
			if ((atlasIdx != s_runAtlas) && (s_runVertCount > 0))
			{
				s_atlasSwitchFlushes++;
			}
			/* Blend run: recorded segment, not a draw. */
			N3DS_MergeCloseBlendRun(N3DS_BLEND_CLOSE_ATLAS);
			s_runAtlas = atlasIdx;
		}

#ifdef CTR_INTERNAL
		N3DS_TriDumpRecord(triVerts, !untextured, untextured ? 255 : (int)s_currentTexFormat, atlasIdx, pixelBaseU, pixelBaseV, originU, originV);
#endif

		if (perfScopes)
		{
			NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
		}
		/* Conversion (and the UV-numerator record) lives in
		 * N3DS_ConvertTriangleVerts, shared with the merge bins. */
		N3DS_ConvertTriangleVerts(&s_convertScratch[s_runVertCount], triVerts, untextured, cellX, cellY, pixelBaseU, pixelBaseV,
		                          originU, originV);
		if (perfScopes)
		{
			NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_TRIANGLES_CONVERT);
		}

		s_runVertCount += 3;
	}

	/* No flush here -- the run stays open for the next split (the whole
	 * point of frame-scope run state). EndScene and the state guards above
	 * are the flush points. */

	if (perfScopes)
	{
		NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDERER_DRAW_TRIANGLES);
	}
}

void NativeRenderer_PushDebugLabel(const char *label)
{
	(void)label;
}

void NativeRenderer_PopDebugLabel(void)
{
}

#ifdef CTR_INTERNAL
/*
 * Little-endian u32 field writer for the cachemeta dump below. ARM11 and the
 * x86 host are both little-endian, but serializing field-by-field through an
 * explicit byte view keeps the on-disk format independent of struct padding
 * and enum width -- TexFormat is 4 bytes in this TU and 1 byte in the backend
 * TU (-fshort-enums), so a raw struct fwrite is exactly the ABI trap the
 * unity-build notes warn about. Never let it near a file format.
 */
internal void N3DS_MetaWriteU32(FILE *f, u32 v)
{
	u8 b[4];

	b[0] = (u8)(v & 0xFFu);
	b[1] = (u8)((v >> 8) & 0xFFu);
	b[2] = (u8)((v >> 16) & 0xFFu);
	b[3] = (u8)((v >> 24) & 0xFFu);
	fwrite(b, 1, 4, f);
}

/*
 * Dumps both decode-cache tiers' metadata + the VRAM dirty-cell generation
 * grid to sdmc:/cachemeta.bin, for offline cell-by-cell verification.
 * Captured alongside the VRAM + atlas dumps
 * so all three describe the same instant: every `used` entry's atlas bytes
 * were uploaded synchronously before the entry was stamped, so entry+atlas
 * are consistent per-cell at any capture point.
 *
 * Format (all fields little-endian u32; page/clut widened via u16 so tpage
 * attribute bits above the page index survive unsigned):
 *   magic 'CTCM', version, frameCounter,
 *   FULLPAGE_RESERVED_CELLS, SUBPAGE_TOTAL_CELLS, SUBPAGE_PHYSICAL_BASE,
 *   ATLAS_COUNT, ATLAS_DIM, DECODE_TEXTURE_DIM, SUBPAGE_DIM,
 *   VRAM_CELL_DIM, VRAM_CELL_COLS, VRAM_CELL_ROWS, vramDirtyBumps,
 *   vramCellGen[COLS*ROWS],
 *   fullpage entries [0 .. FULLPAGE_RESERVED_CELLS]:
 *       used, page, clut, format, view, pageGen, clutGenA, clutGenB,
 *       lastUsedFrame
 *   subpage entries [0 .. SUBPAGE_TOTAL_CELLS):
 *       used, page, clut, format, view, tileX, tileY, pageGen, clutGenA,
 *       clutGenB, lastUsedFrame
 * version 2: inserted `view` (N3DS_TexView, the per-texel STP
 * decode variant) after `format` in both entry records.
 */
/* Backend-callable (dumpreq 'subpage' toggle): drop every cached decode in
 * both tiers. Required on each toggle -- the tiers share physical atlas
 * cells under N3DS_FullPageCellBound's diagnostic reach, so entries recorded
 * by one routing regime can describe cells the other regime has since
 * rewritten. */
void NativeRenderer3DS_DebugInvalidateCaches(void)
{
	int i;

	for (i = 0; i < N3DS_TOTAL_CELLS; i++)
	{
		s_decodeCache[i].used = 0;
	}
	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		s_subPageCache[i].used = 0;
	}
	N3DS_DebugLog("[CTR-3DS] decode caches invalidated (tier toggle)");
}

void NativeRenderer3DS_DebugDumpCacheMeta(void)
{
	FILE *f;
	char path[64];
	char msg[96];
	int i;

	/* Same frame stamp as the vramdump/atlas files written alongside this
	 * (n3ds_backend.c N3DS_DebugCaptureAll), so one capture instant forms a
	 * self-identifying file set and captures never overwrite each other. */
	snprintf(path, sizeof(path), "sdmc:/cachemeta_%u.bin", (unsigned)s_frameCounter);
	f = fopen(path, "wb");
	if (f == NULL)
	{
		snprintf(msg, sizeof(msg), "[CTR-3DS] cachemeta dump FAILED: fopen %s", path);
		N3DS_DebugLog(msg);
		return;
	}

	N3DS_MetaWriteU32(f, 0x4D435443u); /* 'CTCM' little-endian */
	N3DS_MetaWriteU32(f, 2u); /* v2: +view field per entry (per-texel STP) */
	N3DS_MetaWriteU32(f, s_frameCounter);
	N3DS_MetaWriteU32(f, (u32)N3DS_FULLPAGE_RESERVED_CELLS);
	N3DS_MetaWriteU32(f, (u32)N3DS_SUBPAGE_TOTAL_CELLS);
	N3DS_MetaWriteU32(f, (u32)N3DS_SUBPAGE_PHYSICAL_BASE);
	N3DS_MetaWriteU32(f, (u32)N3DS_ATLAS_COUNT);
	N3DS_MetaWriteU32(f, (u32)N3DS_ATLAS_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_DECODE_TEXTURE_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_SUBPAGE_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_VRAM_CELL_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_VRAM_CELL_COLS);
	N3DS_MetaWriteU32(f, (u32)N3DS_VRAM_CELL_ROWS);
	N3DS_MetaWriteU32(f, s_vramDirtyBumps);

	for (i = 0; i < (N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS); i++)
	{
		N3DS_MetaWriteU32(f, s_vramCellGen[i]);
	}

	for (i = 0; i < (1 + N3DS_FULLPAGE_RESERVED_CELLS); i++)
	{
		const N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

		N3DS_MetaWriteU32(f, (u32)e->used);
		N3DS_MetaWriteU32(f, (u32)(u16)e->page);
		N3DS_MetaWriteU32(f, (u32)(u16)e->clut);
		N3DS_MetaWriteU32(f, (u32)e->format);
		N3DS_MetaWriteU32(f, (u32)e->view);
		N3DS_MetaWriteU32(f, e->pageGen);
		N3DS_MetaWriteU32(f, e->clutGenA);
		N3DS_MetaWriteU32(f, e->clutGenB);
		N3DS_MetaWriteU32(f, e->lastUsedFrame);
	}

	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		const N3DS_SubPageCacheEntry *e = &s_subPageCache[i];

		N3DS_MetaWriteU32(f, (u32)e->used);
		N3DS_MetaWriteU32(f, (u32)(u16)e->page);
		N3DS_MetaWriteU32(f, (u32)(u16)e->clut);
		N3DS_MetaWriteU32(f, (u32)e->format);
		N3DS_MetaWriteU32(f, (u32)e->view);
		N3DS_MetaWriteU32(f, (u32)e->tileX);
		N3DS_MetaWriteU32(f, (u32)e->tileY);
		N3DS_MetaWriteU32(f, e->pageGen);
		N3DS_MetaWriteU32(f, e->clutGenA);
		N3DS_MetaWriteU32(f, e->clutGenB);
		N3DS_MetaWriteU32(f, e->lastUsedFrame);
	}

	fclose(f);
	snprintf(msg, sizeof(msg), "[CTR-3DS] cachemeta dumped to %s", path);
	N3DS_DebugLog(msg);
}
#endif
