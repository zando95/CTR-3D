#include <platform.h>

#include <macros.h>

#include "platform/native_audio.h"
#include "platform/native_fatal.h"
#include "platform/native_glad.h"
#include "platform/native_gpu.h"
#include "platform/native_input.h"
#include "platform/native_log.h"
#include "platform/native_perf.h"
#include "platform/native_render_queue.h"
#include "platform/native_renderer.h"
#include "platform/native_replay_scheduler.h"
#include "platform/native_savestate.h"

#if !defined(__3DS__)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

SDL_Window *g_window = NULL;
int g_dbg_polygonSelected = 0;

/* Rain-density dial (3DS confirmation instrument). 100 = full retail rain. A
 * per-region-intensity particle count from the game passes through here; the
 * dial scales it so a hardware A/B can prove the rain is the Tiger-Temple perf
 * cost (rainscale 0 = rain off). Identity on PC and at 100, so the shipping
 * build is byte-unchanged until the dial is turned. Set via dumpreq 'rainscale
 * N'. This is a measurement knob, not the fix -- thinning the rain is not the
 * intended remedy; a batched native rain path that keeps full density is. */
int g_rainScalePercent = 100;

s32 NativeWeather_ScaleRainParticles(s32 count)
{
#ifdef __3DS__
	if ((g_rainScalePercent < 100) && (count > 0))
	{
		return (count * g_rainScalePercent) / 100;
	}
#endif
	return count;
}

#ifdef __3DS__
int N3DS_WeatherSetRainScale(int pct)
{
	if ((pct < 0) || (pct > 100))
	{
		pct = 100; /* bare 'rainscale' or out-of-range resets to full density */
	}
	g_rainScalePercent = pct;
	return pct;
}
#endif

extern int g_cfg_bilinearFiltering;
extern int g_dbg_emulatorPaused;
extern int g_dbg_texturelessMode;
extern int g_dbg_wireframeMode;
extern int g_windowHeight;
extern int g_windowWidth;

#define HOST_ALT_LEFT  (1 << 0)
#define HOST_ALT_RIGHT (1 << 1)
global_variable int s_hostAltKeyState = 0;
global_variable int s_platformInitialized = 0;
global_variable int s_platformBeginScene = 0;
global_variable int s_pinnedVramDisplayFrames = 0;
global_variable int s_pinnedVramDisplayCustomRect = 0;
global_variable int s_pinnedVramDisplayX = 0;
global_variable int s_pinnedVramDisplayY = 0;
global_variable int s_pinnedVramDisplayW = 0;
global_variable int s_pinnedVramDisplayH = 0;
#define NATIVE_FPS_REPORT_FRAME_WINDOW 2000
global_variable int s_fpsFrameCount = 0;
global_variable u64 s_fpsLastCounter = 0;

internal void Platform_CalcFPS(void)
{
#if defined(CTR_INTERNAL)
	const u64 freq = SDL_GetPerformanceFrequency();
	const u64 now = SDL_GetPerformanceCounter();

	if (freq == 0)
	{
		return;
	}

	if (s_fpsLastCounter == 0)
	{
		s_fpsLastCounter = now;
		s_fpsFrameCount = 0;
		return;
	}

	s_fpsFrameCount++;
	if (s_fpsFrameCount < NATIVE_FPS_REPORT_FRAME_WINDOW)
	{
		return;
	}

	if (now > s_fpsLastCounter)
	{
		const f64 elapsedSeconds = (f64)(now - s_fpsLastCounter) / (f64)freq;
		const f64 fps = (f64)s_fpsFrameCount / elapsedSeconds;

		Platform_Log("[CTR 3D] FPS: %.2f (last %d frames)\n", fps, s_fpsFrameCount);
	}

	s_fpsFrameCount = 0;
	s_fpsLastCounter = now;
#endif
}

internal void Platform_GetWindowName(const char *appName, char *buffer, size_t bufferSize)
{
#ifdef CTR_INTERNAL
	snprintf(buffer, bufferSize, "%s | Internal", appName);
#else
	snprintf(buffer, bufferSize, "%s", appName);
#endif
}

internal void Platform_HandleWindowResize(int width, int height)
{
	g_windowWidth = width;
	g_windowHeight = height;
	NativeRenderer_ResetDevice();
}

internal void Platform_UpdateCursorVisibility(void)
{
	if (g_window == NULL)
	{
		return;
	}

	if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0)
	{
		SDL_HideCursor();
	}
	else
	{
		SDL_ShowCursor();
	}
}

internal void Platform_HandleFullscreenToggle(void)
{
	int fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;

	SDL_SetWindowFullscreen(g_window, fullscreen == 0);
	SDL_GetWindowSize(g_window, &g_windowWidth, &g_windowHeight);
	Platform_UpdateCursorVisibility();
	NativeRenderer_ResetDevice();
}

internal void Platform_UpdateHostAltKeyState(const s32 key, const s8 down)
{
	s32 altKeyBit = 0;

	if (key == SDL_SCANCODE_LALT)
	{
		altKeyBit = HOST_ALT_LEFT;
	}
	else if (key == SDL_SCANCODE_RALT)
	{
		altKeyBit = HOST_ALT_RIGHT;
	}

	if (altKeyBit == 0)
	{
		return;
	}

	if (down != 0)
	{
		s_hostAltKeyState |= altKeyBit;
	}
	else
	{
		s_hostAltKeyState &= ~altKeyBit;
	}
}

#if defined(CTR_INTERNAL)
internal void Platform_TakeScreenshot(void)
{
#if defined(__3DS__)
	// Reads back the GL default framebuffer, which the 3DS backend does not have.
	// Reinstate against a citro3d render target when one exists.
#else
	u8 *pixels = (u8 *)malloc(g_windowWidth * g_windowHeight * 4);

	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

	SDL_Surface *surface = SDL_CreateSurfaceFrom(g_windowWidth, g_windowHeight, SDL_PIXELFORMAT_BGRA8888, pixels, g_windowWidth * 4);

	SDL_SaveBMP(surface, "SCREENSHOT.BMP");
	SDL_DestroySurface(surface);

	free(pixels);
#endif
}
#endif

#if defined(CTR_INTERNAL) && defined(CTR_NATIVE_LOD_BIAS) && CTR_NATIVE_LOD_BIAS
// Starting scales from the command line, so a measurement run does not depend
// on someone pressing F4 at the right moment. That matters more than it sounds:
// [OTPROF] reports prims and OT nodes per window, those counts are game content
// and so transfer to 3DS unchanged, and two scripted PC replays at different
// scales price the whole idea before anyone spends a netload evening on it.
internal void NativePlatform_LodBiasConfigureFromArgs(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++)
	{
		int *scale = NULL;

		if (strcmp(argv[i], "--lod-geo") == 0)
		{
			scale = &g_ctrLodGeoScale;
		}
		else if (strcmp(argv[i], "--lod-tex") == 0)
		{
			scale = &g_ctrLodTexScale;
		}

		if (scale == NULL)
		{
			continue;
		}

		if ((i + 1) >= argc)
		{
			Platform_LogWarn("[CTR 3D] %s needs a percentage (100 = retail)\n", argv[i]);
			continue;
		}

		i++;
		*scale = CTR_LodBias_Clamp(atoi(argv[i]));
	}

	if ((g_ctrLodGeoScale != 100) || (g_ctrLodTexScale != 100))
	{
		Platform_LogWarn("[CTR 3D] LOD scale: level %d%%, texture %d%%\n", g_ctrLodGeoScale, g_ctrLodTexScale);
	}
}
#endif

#if defined(CTR_INTERNAL) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
// PC surface for g_dbg_trackRender (3DS: dumpreq 'trackrender', or the
// SELECT+START master). Without a PC setter this can never leave 0 here, and PC is where
// the recorder is actually verified.
internal void NativePlatform_TrackRenderConfigureFromArgs(int argc, char *argv[])
{
	extern int g_dbg_trackRender;
	int i;

	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--track-render") != 0)
		{
			continue;
		}

		if ((i + 1) >= argc)
		{
			Platform_LogWarn("[CTR 3D] --track-render needs a state (0=off 1=on 2=both)\n");
			continue;
		}

		i++;
		{
			int n = atoi(argv[i]);

			g_dbg_trackRender = (n < 0) ? 0 : (n > 2) ? 2 : n;
		}
	}

	// --track-near-census: turns on [TRKNEAR], the behind-the-eye / near-band
	// triangle census (platform/native_track_frame.c). Lives in THIS parser,
	// not the model one, because the counter it sets is compiled under
	// CTR_NATIVE_TRACK_RENDERER -- reaching it from the model parser would
	// break any build with models on and the track off.
	//
	// It also implies --track-render: the leaf list it walks is only recorded
	// while that toggle is set, and on PC the toggle costs nothing visible
	// (NativeTrackFrame_SkipRetailDraw returns 0 off-3DS, so retail still
	// draws the level). Implied rather than required so the measurement can't
	// silently report zero triangles because a flag was forgotten.
	{
		extern int g_dbg_trackNearCensus;

		for (i = 1; i < argc; i++)
		{
			if (strcmp(argv[i], "--track-near-census") == 0)
			{
				g_dbg_trackNearCensus = 1;
			}
		}

		if (g_dbg_trackNearCensus)
		{
			if (g_dbg_trackRender == 0)
			{
				g_dbg_trackRender = 1;
			}
			Platform_LogWarn("[CTR 3D] track near census: on\n");
		}
	}

	// --track-depth-check: [TRKDEPTHCHK], ours-vs-retail road depth for the
	// same vertex. Implies --track-render for the same reason the census does
	// (it reads the snapshot's camera, which is only recorded while the toggle
	// is set), and on PC that toggle changes nothing visible.
	{
		extern int g_dbg_trackDepthCheck;

		for (i = 1; i < argc; i++)
		{
			if (strcmp(argv[i], "--track-depth-check") == 0)
			{
				g_dbg_trackDepthCheck = 1;
			}
		}

		if (g_dbg_trackDepthCheck)
		{
			if (g_dbg_trackRender == 0)
			{
				g_dbg_trackRender = 1;
			}
			Platform_LogWarn("[CTR 3D] track depth check: on\n");
		}
	}

	// Always log the resulting state (even the default 0), same as the
	// dumpreq/button handlers do on 3DS -- this is the line the verification
	// loop greps for to confirm the flag actually took.
	Platform_LogWarn("[CTR 3D] track render: %d\n", g_dbg_trackRender);
}
#endif

#if defined(CTR_INTERNAL) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER
// --model-render 0|1|2: PC surface for g_dbg_modelRender, the PICA-native
// model renderer's runtime toggle. A toggle with no PC setter can never leave 0 here, and PC is
// where rung 1 is verified.
internal void NativePlatform_ModelRenderConfigureFromArgs(int argc, char *argv[])
{
	extern int g_dbg_modelRender;
	int i;

	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--model-render") != 0)
		{
			continue;
		}

		if ((i + 1) >= argc)
		{
			Platform_LogWarn("[CTR 3D] --model-render needs a state (0=off 1=on 2=both)\n");
			continue;
		}

		i++;
		{
			int n = atoi(argv[i]);

			g_dbg_modelRender = (n < 0) ? 0 : (n > 2) ? 2 : n;
		}
	}

	// --model-depth-check: turns on [MDLDEPTH], the GTE-vs-ours depth
	// comparison (platform/native_model_emit.c). Separate from --model-render
	// because it only needs the CAPTURE, which state 1 and state 2 both run --
	// so the check can be read on PC without the 3DS draw path existing.
	{
		extern int g_dbg_modelDepthCheck;

		for (i = 1; i < argc; i++)
		{
			if (strcmp(argv[i], "--model-depth-check") == 0)
			{
				g_dbg_modelDepthCheck = 1;
			}
		}

		if (g_dbg_modelDepthCheck)
		{
			Platform_LogWarn("[CTR 3D] model depth check: on\n");
		}
	}

	// Logged even at the default 0 -- this is the line the verification loop
	// greps for to confirm the flag took.
	Platform_LogWarn("[CTR 3D] model render: %d\n", g_dbg_modelRender);
}
#endif

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
// --gte-capture 0|1|2: PC surface for the capture cost ablation (3DS has
// dumpreq 'gtecapture'). The measurement it exists for happens on
// hardware, but the switch is exercised here first: in a capture-ON,
// desnap-OFF build all three states must produce the identical golden
// checksum, which is what proves the ablation only removes writes nobody is
// reading.
internal void NativePlatform_GteCaptureConfigureFromArgs(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--gte-capture") != 0)
		{
			continue;
		}

		if ((i + 1) >= argc)
		{
			Platform_LogWarn("[CTR 3D] --gte-capture needs 0 (ablate), 1 (full) or 2 (store only)\n");
			continue;
		}

		i++;
		{
			int n = atoi(argv[i]);

			g_dbg_gteCapture = (n < 0) ? 0 : (n > 2) ? 2 : n;
		}
	}

	Platform_LogWarn("[CTR 3D] gte capture: %d\n", g_dbg_gteCapture);
}

// --sz-probe arms the [SZPROBE] stamp-time hit-rate counters (native_gpu.c).
// PC-only by intent: content counts transfer off-device, so this never needs
// to cost a netload.
internal void NativePlatform_SzProbeConfigureFromArgs(int argc, char *argv[])
{
	extern int g_dbg_szProbe;
	int i;

	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--sz-probe") == 0)
		{
			g_dbg_szProbe = 1;
		}
	}

	Platform_LogWarn("[CTR 3D] sz probe: %d\n", g_dbg_szProbe);
}
#endif

internal void Platform_HandleKey(int key, char down)
{
	if (down == 0)
	{
		SubmitName_UseKeyboard(0);
	}
	else
	{
		SubmitName_UseKeyboard(key);
	}

#ifdef CTR_INTERNAL
	if (!down)
	{
		switch (key)
		{
		case SDL_SCANCODE_F1:
			g_dbg_wireframeMode ^= 1;
			Platform_LogWarn("[CTR 3D] wireframe mode: %d\n", g_dbg_wireframeMode);
			break;

		case SDL_SCANCODE_F2:
			g_dbg_texturelessMode ^= 1;
			Platform_LogWarn("[CTR 3D] textureless mode: %d\n", g_dbg_texturelessMode);
			break;
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_DOWN:
			if (g_dbg_emulatorPaused)
			{
				g_dbg_polygonSelected += (key == SDL_SCANCODE_UP) ? 3 : -3;
			}
			break;
		case SDL_SCANCODE_F9:
			if (NativeReplayScheduler_RequestStart() != 0)
			{
				break;
			}
			break;
		case SDL_SCANCODE_F10:
			NativeReplayScheduler_RequestStop();
			break;
		case SDL_SCANCODE_F7:
			Platform_LogWarn("[CTR 3D] saving VRAM.TGA\n");
			NativeRenderer_SaveVRAM("VRAM.TGA", 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1);
			break;
		case SDL_SCANCODE_F12:
			Platform_LogWarn("[CTR 3D] Saving screenshot...\n");
			Platform_TakeScreenshot();
			break;
		case SDL_SCANCODE_F3:
			g_cfg_bilinearFiltering ^= 1;
			Platform_LogWarn("[CTR 3D] filtering mode: %d\n", g_cfg_bilinearFiltering);
			break;
		case SDL_SCANCODE_F5:
			NativeSaveState_RequestSave();
			break;
		case SDL_SCANCODE_F8:
			NativeSaveState_RequestLoad();
			break;
#if defined(CTR_NATIVE_LOD_BIAS) && CTR_NATIVE_LOD_BIAS
		// PC counterpart of the 3DS 'lodgeo'/'lodtex' dumpreq words -- but these
		// case labels are UNREACHABLE: this switch runs on key-up only, and
		// Platform_PollHostEvents eats F4/F6 key-ups for controller cycling
		// before Platform_HandleKey is called. Use --lod-geo/--lod-tex instead.
		case SDL_SCANCODE_F4:
			g_ctrLodGeoScale = CTR_LodBias_Step(g_ctrLodGeoScale);
			Platform_LogWarn("[CTR 3D] level LOD scale: %d%%\n", g_ctrLodGeoScale);
			break;
		case SDL_SCANCODE_F6:
			g_ctrLodTexScale = CTR_LodBias_Step(g_ctrLodTexScale);
			Platform_LogWarn("[CTR 3D] texture LOD scale: %d%%\n", g_ctrLodTexScale);
			break;
#endif
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
		// F13 is used because F1-F12 are already claimed in this switch or
		// intercepted earlier in Platform_PollHostEvents (F4/F6
		// controller-cycling, F11 fullscreen). Convenience for a PC dev
		// without a launch script; --track-render on the command line is the
		// interface verification depends on.
		case SDL_SCANCODE_F13:
		{
			extern int g_dbg_trackRender;

			g_dbg_trackRender = (g_dbg_trackRender + 1) % 3;
			Platform_LogWarn("[CTR 3D] track render: %d\n", g_dbg_trackRender);
			break;
		}
#endif
		}
	}
#endif
}

// Every Platform_Init failure is the renderer failing to come up, so one
// message per platform covers them.
const char *NativePlatform_InitFailure(void)
{
#if defined(__3DS__)
	return "Could not start the graphics system.\n\n"
	       "The console may be short of memory.\n"
	       "Close other software, restart the\n"
	       "console and launch CTR 3D again.";
#else
	return "Could not open the game window.\n\n"
	       "CTR 3D needs a graphics driver with\n"
	       "OpenGL 3.3. Update the graphics driver\n"
	       "and try again.";
#endif
}

internal void NativePlatform_PresentFatal(const char *message)
{
#if defined(__3DS__)
	N3DS_ShowFatalScreen(message);
#else
	// SDL allows this before SDL_Init and after SDL_Quit, so it serves every
	// failure point; a NULL window only means the box has no parent.
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CTR 3D", message, g_window);
#endif
}

void NativePlatform_ShowFatal(const char *message)
{
	fprintf(stderr, "[CTR 3D] Fatal error:\n%s\n", message);
	fflush(stderr);
	NativePlatform_PresentFatal(message);
}

void NativePlatform_FatalExit(const char *message)
{
	// The log echoes to stderr, so this one line reaches both.
	Platform_LogError("[CTR 3D] Fatal error:\n%s\n", message);
	Platform_LogFlush();

#if defined(__3DS__)
	// The error screen needs the displays citro3d holds, so the platform goes
	// first. Platform_Shutdown is the same teardown a HOME-menu exit runs.
	Platform_Shutdown();
	NativePlatform_PresentFatal(message);
#else
	// Shown while the window still exists so the box is parented to it and
	// cannot open behind it.
	NativePlatform_PresentFatal(message);
	Platform_Shutdown();
#endif

	exit(1);
}

int Platform_Init(const char *title, int width, int height)
{
	char windowName[128];

	Platform_LogInit(title);
	Platform_GetWindowName(title, windowName, sizeof(windowName));

	Platform_Log("[CTR 3D] Initialising platform\n");

	if (SDL_Init(SDL_INIT_VIDEO) == 0)
	{
		Platform_LogError("[CTR 3D] Failed to initialise SDL\n");
		Platform_LogShutdown();
		return 0;
	}

	s_platformInitialized = 1;

	if (!NativeRenderer_InitialiseRender(windowName, width, height, 0))
	{
		Platform_LogError("[CTR 3D] Failed to initialise window\n");
		Platform_Shutdown();
		return 0;
	}

	if (!NativeRenderer_InitialisePSX())
	{
		Platform_LogError("[CTR 3D] Failed to initialise PSX renderer state\n");
		Platform_Shutdown();
		return 0;
	}

	// After the renderer is fully initialised (init stays on this thread; the
	// worker only ever executes queued frame work). No-op on PC.
	NativeRenderQueue_Init();

	atexit(Platform_Shutdown);
	Platform_UpdateCursorVisibility();
	Platform_InputInit();
	return 1;
}

void Platform_Shutdown(void)
{
	if (s_platformInitialized == 0)
	{
		return;
	}

	s_platformInitialized = 0;

	// Drain and join the render worker before anything below tears down what
	// its jobs touch. Posts STOP synchronously; no-op if it never came up.
	NativeRenderQueue_Shutdown();

#if defined(CTR_INTERNAL)
	NativePerf_Shutdown();
	NativeReplayScheduler_Shutdown();
#endif
	Platform_InputShutdown();

	if (g_window != NULL)
	{
		SDL_DestroyWindow(g_window);
		g_window = NULL;
	}

	NativeAudio_Shutdown();
	NativeRenderer_Shutdown();

	SDL_Quit();

	Platform_LogShutdown();
}

void Platform_BeginFrame(void)
{
	// NOTE(aalhendi): Normal rendering begins from DrawOTag after the current
	// draw env is installed. Starting a host scene here clears the previous env
	// and can force the host GL driver to block before the retail render-submit path.
}

int Platform_BeginScene(void)
{
	if (s_platformBeginScene)
	{
		return 0;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_PLATFORM_BEGIN_SCENE);
	// NOTE(aalhendi): CTR already throttles through the retail VSync/draw-sync
	// path. Do not add a second SDL swap wait; some GL drivers charge that wait
	// to the next frame's first clear instead of SDL_GL_SwapWindow.
	NativeRenderer_UpdateSwapIntervalState(0);

	NativeRenderer_BeginScene();

	if (activeDrawEnv.isbg)
	{
		const RECT16 clipenv = activeDrawEnv.clip;
		const u8 r = activeDrawEnv.r0;
		const u8 g = activeDrawEnv.g0;
		const u8 b = activeDrawEnv.b0;

		NativeRenderer_Clear(clipenv.x, clipenv.y, clipenv.w, clipenv.h, r, g, b);
	}

	s_platformBeginScene = 1;

	Platform_LogFlush();

	NativePerf_EndScope(NATIVE_PERF_BUCKET_PLATFORM_BEGIN_SCENE);
	return 1;
}

void Platform_EndScene(void)
{
	if (!s_platformBeginScene)
	{
		return;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_PLATFORM_END_SCENE);
	s_platformBeginScene = 0;

	NativeRenderer_EndScene();

	if (s_pinnedVramDisplayFrames > 0)
	{
		// NOTE(aalhendi): Direct VRAM presentation skips StoreFrameBuffer.
		// Do not let the next DrawSync read stale framebuffer texture data back
		// into PSX VRAM after a movie/frame upload.
		NativeRenderer_DiscardFramebufferReadback();
		if (s_pinnedVramDisplayCustomRect)
		{
			NativeRenderer_PresentVRAMRect(s_pinnedVramDisplayX, s_pinnedVramDisplayY, s_pinnedVramDisplayW, s_pinnedVramDisplayH);
		}
		else
		{
			NativeRenderer_PresentVRAMDisplay();
		}
		NativeRenderer_SwapWindow();
		s_pinnedVramDisplayFrames--;
		if (s_pinnedVramDisplayFrames <= 0)
		{
			s_pinnedVramDisplayCustomRect = 0;
		}
		NativePerf_EndScope(NATIVE_PERF_BUCKET_PLATFORM_END_SCENE);
		return;
	}

	NativeRenderer_StoreFrameBuffer(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);

	NativeRenderer_SwapWindow();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_PLATFORM_END_SCENE);
}

// NOTE(aalhendi): Frame timing is handled by VSync() in the platform layer,
// matching PS1 hardware behavior. Platform_EndFrame only does buffer swap + FPS.
void Platform_ExecEndFrame(void)
{
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_PLATFORM_END_FRAME);
	Platform_EndScene();
	Platform_CalcFPS();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_PLATFORM_END_FRAME);
}

void Platform_EndFrame(void)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		// Async: C3D_FrameEnd's submit wait and the swap leave the game
		// thread's frame entirely. CalcFPS rides along -- it has to run
		// after the swap to mean anything.
		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_END_FRAME);
		NativeRenderQueue_Post(&job);
		return;
	}

	Platform_ExecEndFrame();
}

void Platform_ExecPresentVRAMDisplay(void)
{
	Platform_BeginScene();
	NativeRenderer_PresentVRAMDisplay();
	Platform_ExecEndFrame();
}

void Platform_PresentVRAMDisplay(void)
{
	if (NativeRenderQueue_Route())
	{
		struct NativeRenderJob job;

		NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_PRESENT_VRAM_DISPLAY);
		NativeRenderQueue_RunSync(&job);
		return;
	}

	Platform_ExecPresentVRAMDisplay();
}

void Platform_PinVRAMDisplayFrames(int frameCount)
{
	// s_pinned* state is read by EndScene on the worker; drain before the
	// game thread writes it (same guard on the Rect variant below).
	NativeRenderQueue_Fence();
	if (frameCount > s_pinnedVramDisplayFrames)
	{
		s_pinnedVramDisplayFrames = frameCount;
		s_pinnedVramDisplayCustomRect = 0;
	}
}

void Platform_PinVRAMDisplayRect(int x, int y, int w, int h, int frameCount)
{
	if ((frameCount <= 0) || (w <= 0) || (h <= 0))
	{
		return;
	}

	NativeRenderQueue_Fence();

	s_pinnedVramDisplayX = x;
	s_pinnedVramDisplayY = y;
	s_pinnedVramDisplayW = w;
	s_pinnedVramDisplayH = h;
	s_pinnedVramDisplayFrames = frameCount;
	s_pinnedVramDisplayCustomRect = 1;
}

void Platform_PollHostEvents(void)
{
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_EVENT_GAMEPAD_ADDED:
			Platform_InputControllerAdded(event.gdevice.which);
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			Platform_InputControllerRemoved(event.gdevice.which);
			break;
		case SDL_EVENT_QUIT:
			exit(0);
			break;
		case SDL_EVENT_WINDOW_RESIZED:
			Platform_HandleWindowResize(event.window.data1, event.window.data2);
			break;
		case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
		case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
			Platform_UpdateCursorVisibility();
			break;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			exit(0);
			break;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		{
			int key = event.key.scancode;
			char down = (event.type == SDL_EVENT_KEY_UP) ? 0 : 1;

			Platform_UpdateHostAltKeyState(key, down);

			if (key == SDL_SCANCODE_F11)
			{
				if ((down != 0) && (event.key.repeat == 0))
				{
					Platform_HandleFullscreenToggle();
				}
				break;
			}

			if (key == SDL_SCANCODE_RETURN)
			{
				if ((s_hostAltKeyState != 0) && (down != 0) && (event.key.repeat == 0))
				{
					Platform_HandleFullscreenToggle();
				}
				break;
			}

			if (key == SDL_SCANCODE_RSHIFT)
			{
				key = SDL_SCANCODE_LSHIFT;
			}
			else if (key == SDL_SCANCODE_RCTRL)
			{
				key = SDL_SCANCODE_LCTRL;
			}
			else if (key == SDL_SCANCODE_RALT)
			{
				key = SDL_SCANCODE_LALT;
			}

			if ((key == SDL_SCANCODE_F4) && (down == 0))
			{
#ifdef CTR_INTERNAL
				Platform_LogWarn("[CTR 3D] Keyboard assigned to player %d\n", Platform_InputCycleKeyboardController());
#endif
				break;
			}

			if ((key == SDL_SCANCODE_F6) && (down == 0))
			{
#ifdef CTR_INTERNAL
				int player = Platform_InputCycleGamepadController();
				if (player == 0)
				{
					Platform_LogWarn("[CTR 3D] No gamepad connected\n");
				}
				else
				{
					Platform_LogWarn("[CTR 3D] Gamepad assigned to player %d\n", player);
				}
#endif
				break;
			}

			Platform_HandleKey(key, down);
			break;
		}
		}
	}
}

int Platform_PollInput(void)
{
	Platform_PollHostEvents();
	Platform_InputUpdate();
	return 1;
}

int NikoGetEnterKey(void)
{
	const bool *kb = SDL_GetKeyboardState(NULL);
	return (kb && kb[SDL_SCANCODE_RETURN]) ? 1 : 0;
}

// NOTE(aalhendi): Native owns the CTR VBlank clock instead of PsyCross's
// autonomous interrupt thread. The retail-shaped VSyncCallback storage lives in
// native_libetc.c; native VSync emits that callback at each emulated VBlank.
#define NATIVE_VSYNC_HZ          60
#define NATIVE_VSYNC_CATCHUP_MAX 8
#define NATIVE_VSYNC_SPIN_US     1000

global_variable u64 s_nextVBlankCounter = 0;
global_variable u64 s_vblankRemainder = 0;
global_variable int s_nativeVBlankCount = 0;

/*
 * ON by default -- see the long comment in VSync for what it does and why it
 * cannot change game speed. It stays a runtime flag rather than a compile-time
 * one purely so the [SWEEP] harness can A/B it on the same lap: this is worth
 * ~8.8ms/frame by construction, which is the same order as the entire resident
 * track renderer, and a claim that size does not get to go in unmeasured.
 */
int g_dbg_vsyncCredit = 1;

internal u64 Native_CounterFromMicroseconds(u64 freq, u64 microseconds)
{
	return (freq * microseconds) / 1000000;
}

internal void Native_AdvanceVBlankTarget(void)
{
	const u64 freq = SDL_GetPerformanceFrequency();
	const u64 hz = NATIVE_VSYNC_HZ;

	s_nextVBlankCounter += freq / hz;
	s_vblankRemainder += freq % hz;
	if (s_vblankRemainder >= hz)
	{
		s_nextVBlankCounter++;
		s_vblankRemainder -= hz;
	}
}

internal void Native_EnsureVBlankTarget(void)
{
	const u64 now = SDL_GetPerformanceCounter();

	if (s_nextVBlankCounter == 0)
	{
		s_nextVBlankCounter = now;
		s_vblankRemainder = 0;
		Native_AdvanceVBlankTarget();
	}
}

internal void Native_WaitUntilVBlankTarget(void)
{
	const u64 freq = SDL_GetPerformanceFrequency();
	const u64 spinWindow = Native_CounterFromMicroseconds(freq, NATIVE_VSYNC_SPIN_US);

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_VSYNC_WAIT);
	while (1)
	{
		const u64 now = SDL_GetPerformanceCounter();
		u64 remaining;
		u64 sleepMs;

		if (now >= s_nextVBlankCounter)
		{
			NativePerf_EndScope(NATIVE_PERF_BUCKET_VSYNC_WAIT);
			return;
		}

		remaining = s_nextVBlankCounter - now;
		if (remaining <= spinWindow)
		{
			// NOTE(aalhendi): SDL_Delay can wake late. Sleep while safely far
			// from the VBlank target, then spin the final small window so the
			// native VBlank emitter is paced by our clock, not the OS scheduler.
			while (SDL_GetPerformanceCounter() < s_nextVBlankCounter)
			{
			}

			NativePerf_EndScope(NATIVE_PERF_BUCKET_VSYNC_WAIT);
			return;
		}

		sleepMs = ((remaining - spinWindow) * 1000) / freq;
		if (sleepMs > 0)
		{
			SDL_Delay((u32)sleepMs);
		}
	}
}

internal void Native_EmitVBlank(void)
{
	NativeRCnt_EmitVBlank();

	if (vsync_callback != NULL)
	{
		vsync_callback();
	}

	NativeAudio_StepVBlank();
	s_nativeVBlankCount++;

#if defined(__3DS__)
	// Proof-of-life on the vblank clock rather than the render path, so the
	// main loop is observable while it is still loading and drawing nothing.
	if ((s_nativeVBlankCount % 120) == 0)
	{
		char line[80];

		snprintf(line, sizeof(line), "[CTR-3DS] vblank=%d state=%d", s_nativeVBlankCount, (int)sdata->mainGameState);
		N3DS_DebugLog(line);
	}
#endif
}

internal int Native_CatchUpDueVBlanks(void)
{
	int emittedVBlanks = 0;

	Native_EnsureVBlankTarget();

	while (SDL_GetPerformanceCounter() >= s_nextVBlankCounter)
	{
		const u64 now = SDL_GetPerformanceCounter();

		Native_EmitVBlank();
		emittedVBlanks++;

		if (emittedVBlanks >= NATIVE_VSYNC_CATCHUP_MAX)
		{
			// NOTE(aalhendi): Debugger stalls can otherwise replay minutes of
			// VBlank callbacks at once. Keep normal late frames faithful, but
			// rebase pathological host pauses.
			s_nextVBlankCounter = now;
			s_vblankRemainder = 0;
			Native_AdvanceVBlankTarget();
			break;
		}

		Native_AdvanceVBlankTarget();
	}

	return emittedVBlanks;
}

internal void Native_WaitAndEmitVBlank(void)
{
	Native_EnsureVBlankTarget();
	Native_WaitUntilVBlankTarget();
	Native_EmitVBlank();
	Native_AdvanceVBlankTarget();
}

int VSync(int mode)
{
	int requestedVBlanks;
	int emittedVBlanks;

	if (mode < 0)
	{
		return s_nativeVBlankCount;
	}

	requestedVBlanks = (mode == 0) ? 1 : mode;
	emittedVBlanks = 0;

#if defined(CTR_INTERNAL)
	if (NativeReplayScheduler_ConsumeVSyncPacket(requestedVBlanks, &emittedVBlanks))
	{
		for (s32 i = 0; i < emittedVBlanks; i++)
		{
			Native_WaitAndEmitVBlank();
		}

		return s_nativeVBlankCount;
	}
#endif

	emittedVBlanks += Native_CatchUpDueVBlanks();

	/*
	 * Credit already-elapsed vblanks: a frame that overran its 60Hz deadline
	 * should not also wait a full period for the next boundary. Emits stay
	 * locked to the 60Hz target counter either way, so vblanks-per-second
	 * (and HOWL's tempo/timers) are unchanged -- only a late frame stops
	 * paying for a deadline that already passed. Under-budget frames are
	 * unaffected. The replay path is deliberately NOT changed: it consumes a
	 * recorded vblank count and must stay bit-reproducible.
	 */
	for (s32 i = g_dbg_vsyncCredit ? emittedVBlanks : 0; i < requestedVBlanks; i++)
	{
		Native_WaitAndEmitVBlank();
		emittedVBlanks++;
	}

#if defined(CTR_INTERNAL)
	NativeReplayScheduler_RecordVSyncPacket(emittedVBlanks);
#endif

	return s_nativeVBlankCount;
}

int Platform_GetVBlankCount(void)
{
	return s_nativeVBlankCount;
}

void Platform_WaitUntilVBlank(int targetVBlank)
{
	int emittedVBlanks = 0;
	int requestedVBlanks = targetVBlank - s_nativeVBlankCount;

	if (requestedVBlanks <= 0)
	{
		return;
	}

#if defined(CTR_INTERNAL)
	if (NativeReplayScheduler_ConsumeVSyncPacket(requestedVBlanks, &emittedVBlanks))
	{
		for (s32 i = 0; i < emittedVBlanks; i++)
		{
			Native_WaitAndEmitVBlank();
		}

		return;
	}
#endif

	emittedVBlanks += Native_CatchUpDueVBlanks();

	while (s_nativeVBlankCount < targetVBlank)
	{
		Native_WaitAndEmitVBlank();
		emittedVBlanks++;
	}

#if defined(CTR_INTERNAL)
	NativeReplayScheduler_RecordVSyncPacket(emittedVBlanks);
#endif
}
