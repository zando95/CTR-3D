#define _CRT_SECURE_NO_WARNINGS
#define SDL_MAIN_HANDLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__3DS__)
/*
 * The 3DS build does not link SDL. This header supplies the handful of SDL
 * types and entry points the platform layer still names, backed by libctru
 * where the behaviour is load-bearing (timing) and by stubs elsewhere.
 */
#include "platform/n3ds/n3ds_backend.h"
#include "platform/n3ds/sdl3_compat_3ds.h"
#define _EnterCriticalSection(x)
#define EnterCriticalSection(x)
#define ExitCriticalSection()
#elif __GNUC__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#define _EnterCriticalSection(x)
#define EnterCriticalSection(x)
#define ExitCriticalSection()
#endif

#include "platform/native_assets.h"
#include "platform/native_fatal.h"
#include "platform/native_log.h"
#include "platform/native_memory.h"
#include "platform/native_offscreen_capture.h"
#include "platform/native_perf.h"
#include "platform/native_replay_scheduler.h"
#include "platform/native_savestate.h"

#ifndef __GNUC__
#define __attribute__(x)
#endif

#include <platform.h>

#include "game/game_unity.h"

#include "game/zGlobal_RDATA.c"
#include "game/zGlobal_DATA.c"
#include "game/zGlobal_SDATA.c"

#undef RECT

#if defined(__3DS__)
#include "platform/n3ds/sdl3_compat_3ds.c"
#endif

#include "platform/native_disc_image.c"
#include "platform/native_assets.c"
#include "platform/native_audio.c"
#include "platform/native_memory.c"
#include "platform/native_checkpoint.c"
#include "platform/native_checkpoint_file.c"
#include "platform/native_cd.c"
#include "platform/native_gpu_links.c"
#include "platform/native_gte_capture.c"
#include "platform/native_offscreen_capture.c"
#include "platform/native_gpu.c"
#include "platform/native_gte_core.c"
#if !defined(__3DS__)
// OpenGL function loader. The PICA200 has no GL driver to load.
#include "platform/native_glad.c"
#endif
#include "platform/native_input.c"
#include "platform/native_inline_c.c"
#include "platform/native_libapi.c"
#include "platform/native_libetc.c"
#include "platform/native_libgte.c"
#include "platform/native_libgpu.c"
#include "platform/native_libpad.c"
#include "platform/native_libspu.c"
#include "platform/native_log.c"
#include "platform/native_memcard.c"
#include "platform/native_memcard_adapter.c"
#include "platform/native_ghost_best.c"
#include "platform/native_arcade_modes.c"
#include "platform/native_perf.c"
#include "platform/native_platform.c"
#include "platform/native_render_queue.c"
#include "platform/native_replay_scheduler.c"
#if defined(__3DS__)
#include "platform/n3ds/native_renderer_3ds.c"
#else
#include "platform/native_renderer.c"
#endif
// After native_gpu.c and the renderer: the direct-emit fast path reads their
// file-scope state on 3DS, and is count-only on PC (see its header comment).
#include "platform/native_direct_emit.c"
// Track censuses + resident mesh builder for the native-track-renderer
// campaign. Both read game level structs, so they must come after
// game_unity.h like everything here.
#include "platform/native_track_census.c"
// Model/keyframe accounting for the retained-models question. Sits with
// the track censuses because it is the same kind of thing -- a once-per-level
// read of game data that answers a "does this fit" question with a number
// instead of an estimate. Called from MainFrame_RenderFrame, NOT from the
// track mesh builder -- it must run on a plain PC build, where
// CTR_NATIVE_TRACK_RENDERER is off.
// Model topology scan + per-frame instance sampler for the PICA-native model
// renderer. The scanner comes FIRST so
// the census below can fold each header's triangle counts into the same walk
// that already has the deduped model population; the frame sampler comes after
// both because it looks scans up rather than producing them. Both are
// count-only and stubbed out when CTR_NATIVE_MODEL_RENDERER is off.
#include "platform/native_model_mesh.c"
#include "platform/native_model_census.c"
#include "platform/native_model_frame.c"
// Per-frame model triangle capture.
// After the two above only for readability -- it shares no symbols with them,
// unlike the track pair below.
#include "platform/native_model_emit.c"
#include "platform/native_track_mesh.c"
// Per-frame camera/visibility snapshot recorder -- after
// native_track_mesh.c so it can call the mesh accessors (NativeTrackMesh_*)
// directly, same include-order reasoning as the line above.
#include "platform/native_track_frame.c"
// Bake-parity diff: after native_track_mesh.c because its perm report
// reads that file's sTrackUvPerm table directly (same same-TU reasoning as
// the frame recorder above). Whole body gated on CTR_NATIVE_BAKE_PARITY.
#include "platform/native_bake_parity.c"
#include "platform/native_savestate.c"
#include "platform/native_state.c"
#include "platform/native_str.c"
#include "platform/native_options.c"

#ifndef CC
#if __GNUC__
#if _WIN32
#ifndef __clang__
#define CC "MINGW-GCC"
#else
#define CC "MINGW-CLANG"
#endif
#else
#ifndef __clang__
#define CC "GCC"
#else
#define CC "CLANG"
#endif
#endif
#elif defined(_MSC_VER)
#define CC "MSVC"
#else
#define CC "Unknown"
#endif
#endif

#ifndef CTR_NATIVE_VERSION
#define CTR_NATIVE_VERSION "0.0.0-dev"
#endif

#ifndef CTR_NATIVE_BUILD_ID
#define CTR_NATIVE_BUILD_ID "unknown"
#endif

static int NativeConsole_ShouldPauseOnError(void)
{
#if defined(_WIN32)
	DWORD consoleProcesses[2];
	DWORD consoleProcessCount;

	if (GetConsoleWindow() == NULL)
		return 0;

	consoleProcessCount = GetConsoleProcessList(consoleProcesses, (DWORD)(sizeof(consoleProcesses) / sizeof(consoleProcesses[0])));
	return (consoleProcessCount == 1) && (consoleProcesses[0] == GetCurrentProcessId());
#else
	return 0;
#endif
}

static s32 NativeConsole_Return(const u32 result)
{
	if ((result != 0) && NativeConsole_ShouldPauseOnError())
	{
		fflush(stdout);
		fflush(stderr);
		fprintf(stderr, "\n[CTR 3D] Press Enter to close this window...");
		fflush(stderr);

		while (getchar() != '\n' && !feof(stdin))
		{
		}
	}

	return (s32)result;
}

// TODO(aalhendi): just make an argparser?
static int NativeArg_IsVersion(const char *arg)
{
	return (arg != NULL) && ((strcmp(arg, "--version") == 0) || (strcmp(arg, "-v") == 0));
}


int main(int argc, char *argv[])
{
	for (int argIndex = 1; argIndex < argc; argIndex++)
	{
		if (NativeArg_IsVersion(argv[argIndex]))
		{
			printf("CTR 3D %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
			return 0;
		}
	}

#if defined(__3DS__)
	// Route stderr to the console debug service before anything can fail: the
	// screen console does not exist until the renderer starts, and startup
	// errors below report through stderr.
	N3DS_EarlyInit();

	/*
	 * SD logging is off by default because the flush costs a stalled frame per
	 * window. A release build has no chord and no dumpreq to turn it back on,
	 * so an empty enable-log.txt dropped beside the .3dsx is the switch: the
	 * check runs before the first header line, so an enabled log is complete
	 * from boot.
	 */
	if (N3DS_AppDirHasFile("enable-log.txt"))
	{
		N3DS_SetSdLogEnabled(1);
		N3DS_DebugLog("[CTR-3DS] logging: enabled by enable-log.txt");
	}

	N3DS_DebugLog("[CTR-3DS] main() entered");

	// An original 3DS fails the renderer's boot-time allocations partway
	// through, so it is turned away before any of them run.
	if (!N3DS_IsNew3DS())
	{
		N3DS_DebugLog("[CTR-3DS] FAIL: not a New 3DS");
		NativePlatform_ShowFatal("This console is not a New Nintendo 3DS.\n\n"
		                         "CTR 3D needs a New Nintendo 3DS,\n"
		                         "New 3DS XL or New 2DS XL. The original\n"
		                         "3DS, 3DS XL and 2DS do not have the\n"
		                         "memory or CPU speed it needs.");
		return 1;
	}
#endif

	printf("[CTR 3D] Starting...\n");
	fflush(stdout);

	const char *sdlBasePath = SDL_GetBasePath();
	printf("[CTR 3D] SDL base path: %s\n", sdlBasePath ? sdlBasePath : "(null)");
	fflush(stdout);

	if (!NativeAssets_Init(sdlBasePath))
	{
		fprintf(stderr, "[CTR 3D] Failed to initialize asset paths.\n");
#if defined(__3DS__)
		N3DS_DebugLog("[CTR-3DS] FAIL: NativeAssets_Init");
		NativePlatform_ShowFatal(NativeAssets_GetFailureMessage());
#endif
		return NativeConsole_Return(1);
	}
#if defined(__3DS__)
	N3DS_DebugLog("[CTR-3DS] assets init ok");
	N3DS_DebugLog(NativeAssets_GetBaseDir());
	N3DS_DebugLog(NativeAssets_GetAssetDir());

	/*
	 * Stamp the build's identity into BOTH SD logs at boot. Platform_Log and
	 * Platform_LogInstrument write to different files ("Crash Team Racing.log"
	 * and ctr3ds.log), and whichever one gets read first must identify itself
	 * without needing the other. CTR_NATIVE_BUILD_ID is the git hash plus
	 * -dirty-<timestamp> for an unclean tree, so it distinguishes two builds
	 * from the same commit as well as two commits.
	 */
	Platform_Log("[BUILD] %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
	Platform_LogInstrument("[BUILD] %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
#endif

	printf("[CTR 3D] Version: %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
	printf("[CTR 3D] Built with: " CC "\n");
	printf("[CTR 3D] Base: %s\n", NativeAssets_GetBaseDir());
	printf("[CTR 3D] Assets: %s\n", NativeAssets_GetAssetDir());
	fflush(stdout);

	if (chdir(NativeAssets_GetBaseDir()) != 0)
	{
		fprintf(stderr, "[CTR 3D] Failed to enter base directory: %s\n", NativeAssets_GetBaseDir());
#if defined(__3DS__)
		N3DS_DebugLog("[CTR-3DS] FAIL: chdir to base dir");
#endif
		return NativeConsole_Return(1);
	}

	if (!NativeAssets_Validate())
	{
#if defined(__3DS__)
		N3DS_DebugLog("[CTR-3DS] FAIL: NativeAssets_Validate");
		N3DS_DebugLog(NativeAssets_GetFailureMessage());
		NativePlatform_ShowFatal(NativeAssets_GetFailureMessage());
#endif
		return NativeConsole_Return(1);
	}
#if defined(__3DS__)
	N3DS_DebugLog("[CTR-3DS] assets validated; entering Platform_Init");
#endif

#if defined(CTR_INTERNAL)
	if (NativeReplayScheduler_PrepareReportFromArgs(argc, argv) != 0)
	{
		return NativeConsole_Return(1);
	}
#endif

#ifdef USE_16BY9
	printf("[CTR 3D] Widescreen\n");
	const int platformReady = Platform_Init("Crash Team Racing", 1280, 720);
#else
	printf("[CTR 3D] 4:3\n");
	const int platformReady = Platform_Init("Crash Team Racing", 800, 600);
#endif

	if (!platformReady)
	{
#if defined(__3DS__)
		N3DS_DebugLog("[CTR-3DS] FAIL: Platform_Init");
#endif
		NativePlatform_ShowFatal(NativePlatform_InitFailure());
		return NativeConsole_Return(1);
	}

#if defined(CTR_INTERNAL)
	if (NativePerf_ConfigureFromArgs(argc, argv) != 0)
	{
		Platform_LogFlush();
		Platform_Shutdown();
		return NativeConsole_Return(1);
	}

	// --audio-golden replays a captured SPU state through the mixer and exits
	// without starting the game; --audio-capture just arms the capture.
	if (NativeAudio_GoldenConfigureFromArgs(argc, argv) != 0)
	{
		Platform_LogFlush();
		Platform_Shutdown();
		return NativeConsole_Return(0);
	}

#if !defined(__3DS__)
	// --vram-capture arms a raw VRAM-mirror dump at a target vblank (PC side
	// of the PC-vs-3DS same-scene VRAM diff; format matches the 3DS
	// vramdump.bin). PC-only: the 3DS captures via SELECT+R / dumpreq.txt.
	NativeRenderer_VramCaptureConfigureFromArgs(argc, argv);
#endif

	// --offscreen-capture arms the dfe=0 split inventory + golden stream.
	// No-op stub on 3DS.
	NativeOffscreenCapture_ConfigureFromArgs(argc, argv);

#if defined(CTR_NATIVE_LOD_BIAS) && CTR_NATIVE_LOD_BIAS
	// --lod-geo / --lod-tex start the run at a non-retail LOD distance, so a
	// scripted replay can be re-run at another scale and the [OTPROF] prim
	// counts compared. F4/F6 step the same two scales mid-run.
	NativePlatform_LodBiasConfigureFromArgs(argc, argv);
#endif

#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
	// --track-render N (0/1/2) is the PC surface for the native track
	// renderer's runtime toggle -- see NativePlatform_TrackRenderConfigureFromArgs
	// for why this is what makes the recorder verifiable at all. F13 steps the
	// same toggle mid-run.
	NativePlatform_TrackRenderConfigureFromArgs(argc, argv);
#endif

#if defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER
	// --model-render N (0/1/2): the PC surface for the model capture's runtime
	// toggle, verified here, so this is what makes it drivable.
	NativePlatform_ModelRenderConfigureFromArgs(argc, argv);
#endif

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	// --gte-capture 0|1 ablates the capture writes; the golden loop runs both
	// states to prove the ablation is output-neutral before hardware prices it.
	NativePlatform_GteCaptureConfigureFromArgs(argc, argv);
	NativePlatform_SzProbeConfigureFromArgs(argc, argv);
#endif
#endif

	Platform_InitScratchpad();
	Platform_RepairResidentPointers(0);

#if defined(CTR_INTERNAL)
	if (NativeReplayScheduler_ConfigureFromArgs(argc, argv) != 0)
	{
		Platform_LogFlush();
		Platform_Shutdown();
		return NativeConsole_Return(1);
	}
#else
	(void)argc;
	(void)argv;
#endif

	const int result = CTR_Main();

	Platform_Shutdown();
	return NativeConsole_Return(result);
}
