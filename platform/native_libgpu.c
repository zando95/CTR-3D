/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/psx/LIBGPU.C
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#include <psx/libgte.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>

#include <platform/native_renderer.h>
#include <platform/native_gpu.h>
#include <platform/native_perf.h>
#include <platform/native_render_queue.h>
#include <platform/native_model_emit.h>
#include <platform/native_track_frame.h>
#include <platform/native_track_mesh.h>
#include <gpu.h>
#include <platform.h>

#include <string.h>

#ifdef CTR_INTERNAL
#if !defined(__3DS__)
#include <SDL3/SDL.h> /* SDL_GetPerformanceCounter/Frequency */
#else
#include "n3ds/sdl3_compat_3ds.h" /* the same clock, shimmed */
#endif

/* Worker-own-clock split of the DrawOTag job -- see NativeOtagSplitStats. The
 * job runs on core 2 outside the main perf frame, so these are timed directly
 * rather than through NativePerf's gated scopes. */
global_variable u64 s_otagPatchTicks;
global_variable u64 s_otagParseTicks;
global_variable u64 s_otagSplitsTicks;
global_variable unsigned int s_otagSplitCalls;

/* Never-reset cumulative twins of the above, for the spike catcher: it peeks
 * these each frame and deltas them, so it needs values TakeOtagSplit does not
 * zero out from under it. Lets a worker-spike line say whether the ~47ms was
 * the walk (parse/splits) or the GPU-sync remainder. */
global_variable u64 s_otagPatchCum;
global_variable u64 s_otagParseCum;
global_variable u64 s_otagSplitsCum;

void NativeOtag_PeekWalkTicks(u64 *patch, u64 *parse, u64 *splits)
{
	if (patch != NULL)
	{
		*patch = s_otagPatchCum;
	}
	if (parse != NULL)
	{
		*parse = s_otagParseCum;
	}
	if (splits != NULL)
	{
		*splits = s_otagSplitsCum;
	}
}

void NativePerf_TakeOtagSplit(struct NativeOtagSplitStats *out)
{
	const u64 freq = SDL_GetPerformanceFrequency();

	if (out == NULL)
	{
		return;
	}

	out->patchUs = freq ? (unsigned long)((s_otagPatchTicks * 1000000ull) / freq) : 0UL;
	out->parseUs = freq ? (unsigned long)((s_otagParseTicks * 1000000ull) / freq) : 0UL;
	out->splitsUs = freq ? (unsigned long)((s_otagSplitsTicks * 1000000ull) / freq) : 0UL;
	out->calls = s_otagSplitCalls;

	s_otagPatchTicks = 0;
	s_otagParseTicks = 0;
	s_otagSplitsTicks = 0;
	s_otagSplitCalls = 0;
}
#endif

// NOTE(aalhendi): Native libgpu preserves the retail-shaped PsyQ GPU facade
// while keeping the game-facing GPU symbols under ctr-native ownership. The
// primitive parser and GL backend live behind this native platform boundary.
//
// Render-thread builds (3DS): every entry point that touches renderer state
// splits into a facade wrapper (game thread: post to the core-2 worker when
// NativeRenderQueue_Route says so) and a CtrGpu_Exec* body (the pre-queue
// implementation, unchanged -- the direct path when the queue is inactive,
// which is always the case on PC). Exec bodies call only other Exec bodies
// and renderer internals, never wrappers.

int g_dbg_emulatorPaused = 0;
void (*drawsync_callback)(void) = NULL;

int CtrGpu_ExecClearImage(RECT16 *rect, unsigned char r, unsigned char g, unsigned char b)
{
#if defined(CTR_INTERNAL) && !defined(__3DS__)
	NativeVramTrace_Op("CLEAR", rect->x, rect->y, rect->w, rect->h, -1, -1);
#endif
	NativeRenderer_ClearVRAM(rect->x, rect->y, rect->w, rect->h, r, g, b);
	NativeRenderer_Clear(rect->x, rect->y, rect->w, rect->h, r, g, b);
	return 0;
}

int ClearImage(RECT16 *rect, uint8_t r, uint8_t g, uint8_t b)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_CLEAR_IMAGE);
		job.rect = *rect;
		job.r = r;
		job.g = g;
		job.b = b;
		NativeRenderQueue_RunSync(&job);
		return 0;
	}

	return CtrGpu_ExecClearImage(rect, r, g, b);
}

int ClearImage2(RECT16 *rect, uint8_t r, uint8_t g, uint8_t b)
{
	return ClearImage(rect, r, g, b);
}

void CtrGpu_ExecDrawSync(void)
{
	NativeRenderer_UpdateVRAM();
	if (NativeGpu_HasPendingSplits())
	{
		DrawAllSplits();
	}
	NativeRenderer_ReadFramebufferDataToVRAM();
}

int DrawSync(int mode)
{
	(void)mode;

	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_DRAW_SYNC);
		NativeRenderQueue_RunSync(&job);
	}
	else
	{
		CtrGpu_ExecDrawSync();
	}

	// The callback flips game state (bool_DrawOTag_InProgress), so it always
	// runs here on the game thread, after the queued work has completed --
	// same order retail's GPU IRQ delivered it in.
	if (drawsync_callback != NULL)
	{
		drawsync_callback();
	}

	return 0;
}

void CtrGpu_ExecLoadImage(RECT16 *rect, void *p)
{
#if defined(CTR_INTERNAL) && !defined(__3DS__)
	NativeVramTrace_Op("LOAD", rect->x, rect->y, rect->w, rect->h, -1, -1);
#endif
	NativeRenderer_CopyVRAM((unsigned short *)p, 0, 0, rect->w, rect->h, rect->x, rect->y);
}

int LoadImage(RECT16 *rect, void *p)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_LOAD_IMAGE);
		job.rect = *rect;
		job.ptr = p;
		NativeRenderQueue_RunSync(&job);
		return 0;
	}

	CtrGpu_ExecLoadImage(rect, p);
	return 0;
}

void CtrGpu_ExecLoadImage2(RECT16 *rect, void *p)
{
	CtrGpu_ExecLoadImage(rect, p);
	NativeRenderer_UpdateVRAM();
	NativeRenderer_ReadFramebufferDataToVRAM();
}

int LoadImage2(RECT16 *rect, void *p)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_LOAD_IMAGE2);
		job.rect = *rect;
		job.ptr = p;
		NativeRenderQueue_RunSync(&job);
		return 0;
	}

	CtrGpu_ExecLoadImage2(rect, p);
	return 0;
}

void CtrGpu_ExecMoveImage(RECT16 *rect, int x, int y)
{
#if defined(CTR_INTERNAL) && !defined(__3DS__)
	NativeVramTrace_Op("MOVE", x, y, rect->w, rect->h, rect->x, rect->y);
#endif
	NativeRenderer_CopyVRAM(NULL, rect->x, rect->y, rect->w, rect->h, x, y);
}

int MoveImage(RECT16 *rect, int x, int y)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_MOVE_IMAGE);
		job.rect = *rect;
		job.i0 = x;
		job.i1 = y;
		NativeRenderQueue_RunSync(&job);
		return 0;
	}

	CtrGpu_ExecMoveImage(rect, x, y);
	return 0;
}

void CtrGpu_ExecStoreImage(RECT16 *rect, void *p)
{
#if defined(CTR_INTERNAL) && !defined(__3DS__)
	NativeVramTrace_Op("STORE", rect->x, rect->y, rect->w, rect->h, -1, -1);
#endif
	NativeRenderer_ReadVRAM((unsigned short *)p, rect->x, rect->y, rect->w, rect->h);
}

int StoreImage(RECT16 *rect, uint32_t *p)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_STORE_IMAGE);
		job.rect = *rect;
		job.ptr = p;
		NativeRenderQueue_RunSync(&job);
		return 0;
	}

	CtrGpu_ExecStoreImage(rect, p);
	return 0;
}

int StoreImage2(RECT16 *rect, uint32_t *p)
{
	return StoreImage(rect, p);
}

void CtrGpu_ExecResetGraph(int mode)
{
	if (mode == 0)
	{
		g_GPUDisabledState = 0;
		CtrGpu_ExecClearImage(&activeDrawEnv.clip, 0, 0, 0);
		ClearSplits();
		Platform_EndScene();
	}
	else if (mode == 1)
	{
		ClearSplits();
		Platform_EndScene();
	}
}

int ResetGraph(int mode)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_RESET_GRAPH);
		job.i0 = mode;
		NativeRenderQueue_RunSync(&job);
		return 0;
	}

	CtrGpu_ExecResetGraph(mode);
	return 0;
}

int SetGraphDebug(int level)
{
	(void)level;
	return 0;
}

uint32_t *ClearOTag(uint32_t *ot, int n)
{
	OT_TAG *ptag_list;

	if (n == 0)
	{
		return NULL;
	}

	/* Track-renderer depth compose: the walk needs exact OT bounds to tell
	 * real slots from len-0 filler packets (native_gpu.c, [OTZ2]). */
	{
		extern void NativeGpu_RegisterOTRange(uint32_t *ot, int n);
		NativeGpu_RegisterOTRange(ot, n);
	}

	ptag_list = (OT_TAG *)ot;

	termPrim(&ptag_list[n - 1]);
	setlen(&ptag_list[n - 1], 0);

	for (int i = n - 2; i >= 0; --i)
	{
		setaddr(&ptag_list[i], &ptag_list[i + 1]);
		setlen(&ptag_list[i], 0);
	}

	return NULL;
}

uint32_t *ClearOTagR(uint32_t *ot, int n)
{
	OT_TAG *ptag_list;

	if (n == 0)
	{
		return NULL;
	}

	/* Same bounds registration as ClearOTag above. */
	{
		extern void NativeGpu_RegisterOTRange(uint32_t *ot, int n);
		NativeGpu_RegisterOTRange(ot, n);
	}

	ptag_list = (OT_TAG *)ot;

	termPrim(ptag_list);
	setlen(ptag_list, 0);

	for (int i = 1; i < n; ++i)
	{
		setaddr(&ptag_list[i], &ptag_list[i - 1]);
		setlen(&ptag_list[i], 0);
	}

	return NULL;
}

void CtrGpu_ExecSetDispMask(int mask)
{
	g_GPUDisabledState = (mask == 0);
}

void SetDispMask(int mask)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		// Ordered-async: the walk job behind it in the FIFO sees the same
		// enable/disable sequence the game issued.
		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_SET_DISP_MASK);
		job.i0 = mask;
		NativeRenderQueue_Post(&job);
		return;
	}

	CtrGpu_ExecSetDispMask(mask);
}

DISPENV *GetDispEnv(DISPENV *env)
{
	// activeDispEnv is worker-owned while the queue is active; drain any
	// pending env installs so this read observes the game's own order.
	NativeRenderQueue_Fence();
	memcpy(env, &activeDispEnv, sizeof(DISPENV));
	return env;
}

void CtrGpu_ExecPutDispEnv(const DISPENV *env)
{
	memcpy(&activeDispEnv, env, sizeof(DISPENV));
}

DISPENV *PutDispEnv(DISPENV *env)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_PUT_DISPENV);
		job.dispEnv = *env;
		NativeRenderQueue_Post(&job);
		return 0;
	}

	CtrGpu_ExecPutDispEnv(env);
	return 0;
}

DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h)
{
	env->disp.x = x;
	env->disp.y = y;
	env->disp.w = w;
	env->disp.h = h;

	env->screen.x = 0;
	env->screen.y = 0;
	env->screen.w = 0;
	env->screen.h = 0;

	env->isrgb24 = 0;
	env->isinter = 0;
	env->pad1 = 0;
	env->pad0 = 0;

	return 0;
}

void CtrGpu_ExecPutDrawEnv(const DRAWENV *env)
{
	memcpy(&activeDrawEnv, env, sizeof(DRAWENV));
}

DRAWENV *PutDrawEnv(DRAWENV *env)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_PUT_DRAWENV);
		job.drawEnv = *env;
		NativeRenderQueue_Post(&job);
		return 0;
	}

	CtrGpu_ExecPutDrawEnv(env);
	return 0;
}

DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h)
{
	env->clip.x = x;
	env->clip.y = y;
	env->clip.w = w;
	env->clip.h = h;

	env->tw.x = 0;
	env->tw.y = 0;
	env->tw.w = 0;
	env->tw.h = 0;
	env->r0 = 0;
	env->g0 = 0;
	env->b0 = 0;
	env->dtd = 1;

	if (GetVideoMode() == MODE_NTSC)
	{
		env->dfe = h < 289 ? 1 : 0;
	}
	else
	{
		env->dfe = h < 257 ? 1 : 0;
	}

	env->ofs[0] = x;
	env->ofs[1] = y;
	env->tpage = 10;
	env->isbg = 0;

	return env;
}

void SetDrawEnv(DR_ENV *dr_env, DRAWENV *env)
{
	dr_env->code[0] = ((env->clip.y & 0x3FF) << 10) | (env->clip.x & 0x3FF) | 0xE3000000;
	dr_env->code[1] = (((env->clip.y + env->clip.h - 1) & 0x3FF) << 10) | ((env->clip.x + env->clip.w - 1) & 0x3FF) | 0xE4000000;
	dr_env->code[2] = ((env->ofs[1] & 0x7FF) << 11) | (env->ofs[0] & 0x7FF) | 0xE5000000;
	dr_env->code[3] = 32 * (((256 - env->tw.h) >> 3) & 0x1F) | (((256 - env->tw.w) >> 3) & 0x1F) | (((env->tw.y >> 3) & 0x1F) << 15) |
	                  (((env->tw.x >> 3) & 0x1F) << 10) | 0xE2000000;
	dr_env->code[4] = ((env->dtd != 0) << 9) | ((env->dfe != 0) << 10) | (env->tpage & 0x1FF) | 0xE1000000;

	setlen(dr_env, 5);
}

void SetDrawMove(DR_MOVE *p, RECT16 *rect, int x, int y)
{
	char len = 5;

	if (rect->w == 0 || rect->h == 0)
	{
		len = 0;
	}

	p->code[0] = 0x1000000;
	p->code[1] = 0x80000000;
	p->code[2] = ((u32)(u16)rect->x) | ((u32)(u16)rect->y << 16);
	p->code[3] = (y << 0x10) | (x & 0xffffU);
	p->code[4] = ((u32)(u16)rect->w) | ((u32)(u16)rect->h << 16);

	setlen(p, len);
}

uint32_t DrawSyncCallback(void (*func)(void))
{
	drawsync_callback = func;
	return 0;
}

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
// Advances animated track geometry (water, turbo pads, waterfalls, lava) to the
// frame the game has cycled to, by writing into the GPU-resident track vertex
// block. That block is still being read by the previous frame's command list
// until Platform_BeginScene has waited for it, so this must run after
// BeginScene and never before. It lives in the walk rather than next to the
// game-thread publishes in DrawOTag because the walk is the only thread that
// knows the GPU is done with the block.
static void CtrGpu_PatchTrackAnimForWalk(void)
{
#ifdef CTR_INTERNAL
	const u64 patchStart = SDL_GetPerformanceCounter();
	u64 patchDelta;

	NativeTrackAnim_PatchFrame();
	patchDelta = SDL_GetPerformanceCounter() - patchStart;
	s_otagPatchTicks += patchDelta;
	s_otagPatchCum += patchDelta;
#else
	NativeTrackAnim_PatchFrame();
#endif
}
#endif

void CtrGpu_ExecDrawOTag(void *p)
{
#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
	// Once per walk, not once per paused-emulator repaint: the patch is
	// idempotent per game frame (the per-chain frame latch makes a repeat a
	// no-op), so a repeat would cost time and buy nothing.
	int trackAnimPatched = 0;
#endif

#ifdef CTR_INTERNAL
	s_otagSplitCalls++;
#endif

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_OTAG);
	do
	{
		if (g_GPUDisabledState)
		{
			ClearSplits();
			NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG);
			return;
		}

		if (Platform_BeginScene())
		{
			ClearSplits();
		}

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
		if (!trackAnimPatched)
		{
			trackAnimPatched = 1;
			CtrGpu_PatchTrackAnimForWalk();
		}
#endif

#ifdef CTR_INTERNAL
		{
			const u64 parseStart = SDL_GetPerformanceCounter();
			u64 splitsStart;

			u64 splitsEnd;

			ParsePrimitivesLinkedList((uint32_t *)p, 0);
			splitsStart = SDL_GetPerformanceCounter();
			s_otagParseTicks += splitsStart - parseStart;
			s_otagParseCum += splitsStart - parseStart;
			DrawAllSplits();
			splitsEnd = SDL_GetPerformanceCounter();
			s_otagSplitsTicks += splitsEnd - splitsStart;
			s_otagSplitsCum += splitsEnd - splitsStart;
		}
#else
		ParsePrimitivesLinkedList((uint32_t *)p, 0);
		DrawAllSplits();
#endif
	} while (g_dbg_emulatorPaused);
	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG);
}

void DrawOTag(void *p)
{
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	// Hand the walk the capture buffer this frame just wrote -- game thread,
	// before the post, so the queue's own lock orders it for the worker.
	GTECapture_PublishForWalk();
	// Same publish point for the deferred-smoke packet-address set --
	// same game-thread-before-post ordering guarantee.
	NativeGpu_PublishSmokePackets();
	// Same publish point for the skybox packet range.
	NativeGpu_PublishSkyRange();
#endif

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
	// Same reasoning as the GTE capture publish just above: hand the walk
	// this frame's camera/visibility snapshot -- game thread, before the
	// post, so the render queue's own lock/signal orders it for whichever
	// thread ends up walking (worker on 3DS render-thread builds).
	NativeTrackFrame_PublishForWalk();
#endif

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER
	// Same publish point and ordering argument for the model capture, which
	// the model draw side (NativeModelDraw_IssueForWalk) reads on the walk.
	NativeModelEmit_PublishForWalk();
#endif

	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		// The async post that pays for this whole file's plumbing: the game
		// thread returns to building frame N+1 while the worker walks N. The
		// OT's lifetime is the double-buffer protocol's problem, and it
		// already solves it (MainFrame_ResetDB flips before touching cursors,
		// RenderVSYNC fences through DrawSync before the next submit).
		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_DRAW_OTAG);
		job.ptr = p;
		NativeRenderQueue_Post(&job);
		return;
	}

	CtrGpu_ExecDrawOTag(p);
}

void CtrGpu_ExecDrawPrim(void *p)
{
	if (g_GPUDisabledState)
	{
		ClearSplits();
		return;
	}

	if (Platform_BeginScene())
	{
		ClearSplits();
	}

	ParsePrimitivesLinkedList((uint32_t *)p, 1);
}

void DrawPrim(void *p)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_DRAW_PRIM);
		job.ptr = p;
		NativeRenderQueue_RunSync(&job);
		return;
	}

	CtrGpu_ExecDrawPrim(p);
}

void AddPrim(void *ot, void *p)
{
	u32 otTag = CTR_GPU_ReadTagWord(ot);
	u32 primTag = CTR_GPU_ReadTagWord(p);

	CTR_GPU_WriteTagWord(p, CtrGpu_PackOTTag(otTag, primTag & 0xff000000u));
	CTR_GPU_WriteTagWord(ot, CtrGpu_PrimToOTLink24(p));
}
