#include <common.h>

#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
#include <platform/native_perf.h>
#define MAINFRAME_PERF_BEGIN(bucket) NativePerf_BeginScope(bucket)
#define MAINFRAME_PERF_END(bucket)   NativePerf_EndScope(bucket)
#else
#define MAINFRAME_PERF_BEGIN(bucket) ((void)0)
#define MAINFRAME_PERF_END(bucket)   ((void)0)
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
/* s41 ground-decal depth-axis brackets. Declared here rather than pulled in via
 * the platform header for the same reason RenderBucket_QueueExecute.c declares
 * the instance pair by hand: game/ does not include platform/ headers. */
extern void GTECapture_BeginFixedDepthShift(int shift);
extern void GTECapture_EndInstanceDepth(void);
#endif

#ifdef CTR_INTERNAL
volatile int gCtrDebugSkipLevelGeometry = 0;

/*
 * Content-ablation switches (s29). With the resident track renderer on, a Polar
 * Pass frame is ~55ms of which draw_all_splits (15.3) + draw_otag_parse (12.5)
 * = ~28ms is packet-path work, and by construction NONE of it is the track any
 * more -- but nothing said WHICH content it is. Rather than build a
 * per-primitive tagging side-table and an attribution model to get wrong,
 * subtract one class at a time and read the frame time: the delta IS that
 * class's whole cost, game-side build plus GPU submission.
 *
 * Driven by the [SWEEP] harness (platform/n3ds/native_renderer_3ds.c) so every
 * class sees the same lap -- the confounding that made s29's first three perf
 * readings disagree came from unmatched exposure, not from bad timers.
 *
 * Measurement only; never on in a shipping build. Each gate covers additive
 * draw calls ONLY -- RenderDispEnv_World and PushBuffer_FadeAllWindows sit
 * inside the same perf scopes but set up state the rest of the frame needs, so
 * they stay outside the gates.
 */
volatile int gCtrDebugSkipInstances = 0;
volatile int gCtrDebugSkipEffects = 0;
volatile int gCtrDebugSkipHUD = 0;
#define CTR_DEBUG_ABLATE(flag) if ((flag) == 0)
#else
#define CTR_DEBUG_ABLATE(flag)
#endif

#ifdef CTR_NATIVE
/*
 * DIAGNOSTIC (2026-07-20, session 10): hardware hangs somewhere after
 * LOAD_TenStages reaches stage 2 (confirmed via game/LOAD/LOAD_TenStages.c's
 * own checkpoint logging -- that log stops cold right after "stage 2 ->
 * called", and LOAD_OvrLOD (what stage 2 calls) is a two-field-write no-op on
 * CTR_NATIVE, so it cannot itself be blocking). MainFrame_RenderFrame runs
 * unconditionally every frame regardless of load state (see MainMain.c's
 * case 3), so it is the next thing that runs after the loading-stage check
 * completes for this frame -- tag each of its existing perf-scope boundaries
 * with a checkpoint log, bounded to the first few calls, to localize a hang
 * to one section instead of guessing which of ~1300 lines is responsible.
 */
static int s_mainFrameRenderCalls = 0;
#define MAINFRAME_LOG_CHECKPOINT(name)                                                                            \
	do                                                                                                             \
	{                                                                                                              \
		if (s_mainFrameRenderCalls <= 3)                                                                          \
		{                                                                                                          \
			Platform_Log("[CTR RENDER] MainFrame_RenderFrame call=%d checkpoint=%s\n", s_mainFrameRenderCalls,     \
			             name);                                                                                   \
		}                                                                                                         \
	} while (0)
#else
#define MAINFRAME_LOG_CHECKPOINT(name) ((void)0)
#endif

void MainFrame_RenderFrame(struct GameTracker *gGT, struct GamepadSystem *gGamepads)
{
	struct Level *lev = gGT->level1;
	struct mesh_info *ptr_mesh_info = 0;

#ifdef CTR_NATIVE
	s_mainFrameRenderCalls++;
	MAINFRAME_LOG_CHECKPOINT("entry");
#endif

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_SETUP);
	MAINFRAME_LOG_CHECKPOINT("SETUP");
	DrawUnpluggedMsg(gGT, gGamepads);
	DrawFinalLap(gGT);

	ElimBG_HandleState(gGT);

	if (lev != 0)
	{
		ptr_mesh_info = lev->ptr_mesh_info;
	}

	if ((gGT->renderFlags & RENDER_FLAG_VISMEM_REFRESH_MASK) != 0)
	{
		MainFrame_VisMemFullFrame(gGT, gGT->level1);
	}


	if ((gGT->renderFlags & RENDER_FLAG_DRAW_LEVEL) != 0)
	{
		if (gGT->visMem1 != 0)
		{
			if (lev != 0)
			{
				CTR_CycleTex_LEV(lev->ptr_anim_tex, gGT->timer);
			}
		}
	}

	if ((sdata->ptrActiveMenu != 0) || ((gGT->gameMode1 & END_OF_RACE) != 0))
	{
		RECTMENU_CollectInput();
	}

	if (sdata->ptrActiveMenu != 0)
	{
		if (sdata->Loading.stage == LOAD_IDLE)
		{
			RECTMENU_ProcessState();
		}
	}

	RainLogic(gGT);
	DropRain_MakeSound(gGT);
	MenuHighlight();
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_SETUP);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);
	MAINFRAME_LOG_CHECKPOINT("EFFECTS_1_weather");
	CTR_DEBUG_ABLATE(gCtrDebugSkipEffects)
	{
		RenderAllWeather(gGT);
		RenderAllConfetti(gGT);
		// NOTE(aalhendi): ASM-verified NTSC-U 926 subrange 0x800364f8-0x80036538.
		if (((gGT->renderFlags & RENDER_FLAG_STARS) != 0) && (gGT->stars.numStars != 0))
		{
			RenderStars(&gGT->pushBuffer[0], &gGT->backBuffer->primMem, &gGT->stars, gGT->numPlyrCurrGame);
		}

		if (((gGT->renderFlags & RENDER_FLAG_MULTIPLAYER_DECALS) != 0) && (gGT->numPlyrCurrGame > 1))
		{
			DecalMP_01(gGT);
		}
	}
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_HUD);
	MAINFRAME_LOG_CHECKPOINT("HUD");
	CTR_DEBUG_ABLATE(gCtrDebugSkipHUD)
	{
		RenderAllHUD(gGT);
	}
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_HUD);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);
	MAINFRAME_LOG_CHECKPOINT("EFFECTS_2_beakerrain");
	CTR_DEBUG_ABLATE(gCtrDebugSkipEffects)
	{
		RenderAllBeakerRain(gGT);

		RenderAllBoxSceneSplitLines(gGT);
	}
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_QUEUE_INSTANCES);
	MAINFRAME_LOG_CHECKPOINT("QUEUE_INSTANCES");
	CTR_DEBUG_ABLATE(gCtrDebugSkipInstances)
	{
		RenderBucket_QueueAllInstances(gGT);
	}
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_QUEUE_INSTANCES);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);
	MAINFRAME_LOG_CHECKPOINT("EFFECTS_3_particles");
	CTR_DEBUG_ABLATE(gCtrDebugSkipEffects)
	{
		RenderAllNormalParticles(gGT);
	}

	RenderDispEnv_World(gGT); // == RenderDispEnv_World ==

	CTR_DEBUG_ABLATE(gCtrDebugSkipEffects)
	{
		if (((gGT->renderFlags & RENDER_FLAG_MULTIPLAYER_DECALS) != 0) && (gGT->numPlyrCurrGame > 1))
		{
			DecalMP_02(gGT);
		}

		RenderAllFlag0x40(gGT); // I need a better name
		RenderAllTitleDPP(gGT);
	}
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_EXECUTE_INSTANCES);
	MAINFRAME_LOG_CHECKPOINT("EXECUTE_INSTANCES");
	CTR_DEBUG_ABLATE(gCtrDebugSkipInstances)
	{
		RenderBucket_ExecuteAllInstances(gGT);
	}
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_EXECUTE_INSTANCES);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);
	MAINFRAME_LOG_CHECKPOINT("EFFECTS_4_tires");
	CTR_DEBUG_ABLATE(gCtrDebugSkipEffects)
	{
		RenderAllTires(gGT);

		RenderAllShadows(gGT);

		RenderAllHeatParticles(gGT);
	}

	PushBuffer_FadeAllWindows();
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_EFFECTS);

	if (((gGT->renderFlags & RENDER_FLAG_DRAW_LEVEL) != 0) && (ptr_mesh_info != 0))
	{
#ifdef CTR_INTERNAL
		if (gCtrDebugSkipLevelGeometry == 0)
#endif
		{
			MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_LEVEL_GEOMETRY);
			MAINFRAME_LOG_CHECKPOINT("LEVEL_GEOMETRY");
			RenderAllLevelGeometry(gGT, lev, ptr_mesh_info);
			MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_LEVEL_GEOMETRY);
		}

		MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_POST_LEVEL);
		MAINFRAME_LOG_CHECKPOINT("POST_LEVEL");
		RenderDispEnv_World(gGT); // == RenderDispEnv_World ==

		if (((gGT->hudFlags & HUD_FLAG_RACE_HUD) != 0) && (gGT->numPlyrCurrGame > 1))
		{
			UI_RenderFrame_Wumpa3D_2P3P4P(gGT);
		}

		if (((gGT->renderFlags & RENDER_FLAG_MULTIPLAYER_DECALS) != 0) && (gGT->numPlyrCurrGame > 1))
		{
			DecalMP_03(gGT);
		}

		int dotLightsLoadReady = sdata->Loading.stage != LOAD_REQUESTED;

		if (
		    // if not cutscene
		    // if not in adventure arena
		    // if not in main menu
		    ((gGT->gameMode1 & (GAME_CUTSCENE | ADVENTURE_ARENA | MAIN_MENU)) == 0) && dotLightsLoadReady)
		{
			DotLights_AudioAndVideo(gGT);
		}

		if ((gGT->renderFlags & RENDER_FLAG_SPLIT_SCREEN_LINES) != 0)
		{
			WindowBoxLines(gGT);

			WindowDivsionLines(gGT);
		}

		// if game is not loading
		if (sdata->Loading.stage == LOAD_IDLE)
		{
			// If game is not paused
			if ((gGT->gameMode1 & PAUSE_ALL) == 0)
			{
				PickupBots_Update();
			}

#if defined(CTR_NATIVE)
			// NOTE(aalhendi): Native menu/adventure-hub LEVs may publish no
			// restart table. Retail lap stats assume the table exists whenever
			// this caller reaches them; keep the ASM-verified lap function intact.
			if ((gGT->level1 != NULL) && (gGT->level1->ptr_restart_points != NULL) && (gGT->level1->cnt_restart_points != 0))
			{
				PlayLevel_UpdateLapStats();
			}
#else
			PlayLevel_UpdateLapStats();
#endif
		}
		MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_POST_LEVEL);
	}

	// If in main menu, or in adventure arena,
	// or in End-Of-Race menu
	if ((gGT->gameMode1 & (ADVENTURE_ARENA | END_OF_RACE | MAIN_MENU)) != 0)
	{
		MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_REFRESHCARD);
		MAINFRAME_LOG_CHECKPOINT("REFRESHCARD");
		RefreshCard_Entry();
		MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_REFRESHCARD);
	}

	MAINFRAME_LOG_CHECKPOINT("pre_clear_swapchain_lev_deref");
	// clear swapchain
	// DIAGNOSTIC (2026-07-20, session 10): lev is gGT->level1, only ever set in
	// LOAD_TenStages case 7 -- unlike every other lev-derived pointer in this
	// function (ptr_mesh_info, the level-geometry block), this dereferences
	// lev directly with no "lev != 0" guard. If RENDER_FLAG_CLEAR_BACK_BUFFER
	// is ever set while a level is still loading (lev == NULL), this is an
	// unguarded null deref -- a plausible hardware-only hang/crash if the
	// timing that lets it happen never occurs on Azahar's much faster load.
	if (((gGT->renderFlags & RENDER_FLAG_CLEAR_BACK_BUFFER) != 0) && ((lev->clearColor[0].enable != 0) || (lev->clearColor[1].enable != 0)))
	{
		MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_CLEAR_SCREEN);
		MAINFRAME_LOG_CHECKPOINT("CLEAR_SCREEN");
		CAM_ClearScreen(gGT);
		MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_CLEAR_SCREEN);
	}

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_UI);
	MAINFRAME_LOG_CHECKPOINT("UI");
	if ((gGT->renderFlags & RENDER_FLAG_CHECKERED_FLAG) != 0)
	{
		RaceFlag_DrawSelf();
	}

#ifdef CTR_NATIVE
	// 3DS bottom-screen FPS overlay, emitted EVERY frame -- menus, loads,
	// pauses, races -- so the readout never blinks out with the minimap
	// (owner call). The hook returns 0 on PC and whenever routing is off, so
	// nothing ever draws here on the top screen or on PC. The x/y feed only
	// the text's own bbox; the bottom pass re-anchors it top-right.
	{
		extern int NativeUiMap_FpsOverlayText(char *buf, int bufSize);
		char fpsStr[16];

		if (NativeUiMap_FpsOverlayText(&fpsStr[0], (int)sizeof(fpsStr)))
		{
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
			extern void NativeGpu_MarkUiFpsRange(uintptr_t start, uintptr_t end);

			uintptr_t fpsPrimStart = (uintptr_t)gGT->backBuffer->primMem.cursor;
#endif
			DecalFont_DrawLine(&fpsStr[0], 438, 197, FONT_SMALL, WHITE);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
			NativeGpu_MarkUiFpsRange(fpsPrimStart, (uintptr_t)gGT->backBuffer->primMem.cursor);
#endif
		}
	}
#endif

	RenderDispEnv_UI(gGT);
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_UI);

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_MAINFRAME_RENDER_VSYNC);
	MAINFRAME_LOG_CHECKPOINT("RENDER_VSYNC");
	RenderVSYNC(gGT);
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_MAINFRAME_RENDER_VSYNC);

#ifndef CTR_NATIVE
	RenderFMV();
#endif

	MAINFRAME_LOG_CHECKPOINT("pre_RenderSubmit");
	RenderSubmit(gGT);
	MAINFRAME_LOG_CHECKPOINT("exit_post_RenderSubmit");
}

void DrawUnpluggedMsg(struct GameTracker *gGT, struct GamepadSystem *gGamepads)
{
	int posY;
	int lngArrStart;
	int skipMainMenuTopLevel;
	RECT window;
	int i;

	skipMainMenuTopLevel = 0;

	if (LOAD_IsOpen_MainMenu())
	{
		// if main menu is open, assume 230 loaded,
		// quit if menu is at highest level (no ptrNext to draw)
		// maybe a member of D230.c?
		if (sdata->ptrActiveMenu == (struct RectMenu *)0x800B4540)
		{
			skipMainMenuTopLevel = ((*(int *)0x800b4548 & 0x10) == 0);
		}
	}

	// dont draw error in cutscene, if no controllers are missing currently,
	// in demo mode, or at the highest main-menu level.
	if ((gGT->gameMode1 & GAME_CUTSCENE) != 0)
	{
		return;
	}

	if (MainFrame_HaveAllPads(gGT->numPlyrNextGame))
	{
		return;
	}

	if (gGT->boolDemoMode != 0)
	{
		return;
	}

	if (skipMainMenuTopLevel != 0)
	{
		return;
	}

	// position of error
	posY = data.errorPosY[sdata->errorMessagePosIndex];

	// "Controller 1" or "Controller 2"
	lngArrStart = 0;

	window.x = 0xffec;
	window.y = posY - 3;
	window.w = 0x228;
	window.h = 0;

	// if more than 2 players, or if multitap used
	if ((gGT->numPlyrNextGame > 2) || (gGamepads->slotBuffer[0].controllerData == (PAD_ID_MULTITAP << 4)))
	{
		// change to "1A", "1B", "1C", "1D",
		lngArrStart = 2;
	}

	for (i = 0; i < gGT->numPlyrNextGame; i++)
	{
		struct ControllerPacket *ptrControllerPacket = gGamepads->gamepad[i].ptrControllerPacket;

		if (ptrControllerPacket != 0)
		{
			if (ptrControllerPacket->plugged == PLUGGED)
			{
				continue;
			}
		}

		// if controller is unplugged

		DecalFont_DrawLine(sdata->lngStrings[data.lngIndex_gamepadUnplugged[lngArrStart + i]], 0x100, posY + window.h, FONT_SMALL, (JUSTIFY_CENTER | ORANGE));

		// add for each line
		window.h += 8;
	}

	DecalFont_DrawLine(sdata->lngStrings[LNG_PLEASE_INSERT_A_CONTROLLER], 0x100, posY + window.h, FONT_SMALL, (JUSTIFY_CENTER | ORANGE));

	// add for each line
	window.h += 8;

	// add 3 pixels above, 3 pixels bellow
	window.h += 6;

	RECTMENU_DrawInnerRect(&window, 1, gGT->backBuffer->otMem.uiOT);
}

void DrawFinalLap(struct GameTracker *gGT)
{
	int i;
	int textTimer;
	struct PushBuffer *pb;

	int startX;
	int endX;
	int posY;

	SVec2 resultPos;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	// Flat 2D overlay linked into the WORLD OT (the retail comment at the
	// draw call below explains the layering intent) -- registered
	// paint-on-top so the depth test reproduces that layering instead of
	// letting the track occlude the text.
	const void *paintTopStart = gGT->backBuffer->primMem.cursor;
#endif

	// number of players
	for (i = 0; i < 4; i++)
	{
		// time remaining in animation
		textTimer = sdata->finalLapTextTimer[i];

		// skip if not drawing "FINAL LAP"
		if (textTimer == 0)
		{
			continue;
		}

		// turn "time remaining" into "time elapsed",
		// 90 frames total in animation, 1.5 seconds
		textTimer = 90 - textTimer;

		// camera
		pb = &gGT->pushBuffer[i];

		// << 0x10, >> 0x12
		posY = pb->rect.h / 4;

		// fly from right to center
		if (textTimer <= 10)
		{
			startX = pb->rect.w + 100;
			endX = pb->rect.w / 2;

			goto DrawFinalLapString;
		}

		// sit in center
		if (textTimer <= 0x50)
		{
			startX = pb->rect.w / 2;
			endX = startX;

			// for duration
			textTimer -= 10;

			goto DrawFinalLapString;
		}

		// fly from center to left
		startX = pb->rect.w / 2;
		endX = -100;
		textTimer -= 0x50;

	DrawFinalLapString:

		UI_Lerp2D_Linear(resultPos.v, (s16)startX, (s16)posY, (s16)endX, (s16)posY, textTimer, 10);

		// need to specify OT, or else "FINAL LAP" will draw on top of character icons,
		// and by doing this, "FINAL LAP" draws under the character icons instead
		DecalFont_DrawLineOT(sdata->lngStrings[LNG_FINAL_LAP], resultPos.x, resultPos.y, FONT_BIG, (JUSTIFY_CENTER | ORANGE), pb->ptrOT);

		sdata->finalLapTextTimer[i]--;
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	GTECapture_AddPaintOnTopRange(paintTopStart, gGT->backBuffer->primMem.cursor, 1);
#endif
}

void RainLogic(struct GameTracker *gGT)
{
	int i;
	struct QuadBlock *camQB;
	int numPlyrCurrGame;

	numPlyrCurrGame = gGT->numPlyrCurrGame;

	for (i = 0; i < numPlyrCurrGame; i++)
	{
		PushBuffer_UpdateFrustum(&gGT->pushBuffer[i]);

		camQB = gGT->cameraDC[i].ptrQuadBlock;

		// skip if camera isn't over quadblock
		if (camQB == 0)
		{
			continue;
		}

		// assume numPlayers is never zero,
		// assume weather_intensity is always valid

		{
			// Rain-density dial: identity on PC and at full density, so this is
			// byte-unchanged until the 3DS 'rainscale' knob is turned down.
			extern s32 NativeWeather_ScaleRainParticles(s32 count);
			gGT->rainBuffer[i].numParticles_max = NativeWeather_ScaleRainParticles((camQB->weather_intensity << 2) / numPlyrCurrGame);
		}

		gGT->rainBuffer[i].vanishRate = (camQB->weather_vanishRate << 2) / numPlyrCurrGame;
	}
}

void MenuHighlight()
{
	int fc;
	int trig;

	fc = sdata->frameCounter << 7;
	trig = MATH_Sin(fc);

	trig = (trig << 6) >> 0xc;

	// sine curve of green, plus base color
	sdata->menuRowHighlight_Normal.self = ((trig + 0x40) * 0x100) | 0x80;
	sdata->menuRowHighlight_Green.self = ((trig + 0xA0) * 0x100) | 0x400040;
}

void RenderAllWeather(struct GameTracker *gGT)
{
	int numPlyrCurrGame = gGT->numPlyrCurrGame;

	// only if rain is enabled
	if ((gGT->renderFlags & RENDER_FLAG_RAIN) == 0)
	{
		return;
	}

	// only for single player,
	// probably Naughty Dog's last-minute hack
	if (numPlyrCurrGame > 1)
	{
		return;
	}

	RenderWeather(&gGT->pushBuffer[0], &gGT->backBuffer->primMem, &gGT->rainBuffer[0], numPlyrCurrGame, gGT->gameMode1 & PAUSE_ALL);
}

void RenderAllConfetti(struct GameTracker *gGT)
{
	int i;
	int numWinners = gGT->numWinners;

	// only if confetti is enabled
	if ((gGT->renderFlags & RENDER_FLAG_CONFETTI) == 0)
	{
		return;
	}

	// only if someone needs confetti
	if (numWinners == 0)
	{
		return;
	}

	for (i = 0; i < numWinners; i++)
	{
		DrawConfetti(&gGT->pushBuffer[gGT->winnerIndex[i]], &gGT->backBuffer->primMem, &gGT->confetti, gGT->frameTimer_Confetti, gGT->gameMode1 & PAUSE_ALL);
	}
}

void RenderAllHUD(struct GameTracker *gGT)
{
	int hudFlags;
	int gameMode1;

	hudFlags = gGT->hudFlags;
	gameMode1 = gGT->gameMode1;

	// if drawing intro-race title bars
	if ((gGT->numPlyrCurrGame == 1) && ((hudFlags & HUD_FLAG_INTRO_RACE_TITLE_BARS) != 0) && ((gameMode1 & START_OF_RACE) != 0))
	{
		UI_RaceStart_IntroText1P();
	}

	// if not drawing intro-race title bars
	else
	{
		// if no hud
		if ((hudFlags & HUD_FLAG_RACE_HUD) == 0)
		{
			// if standings
			if ((hudFlags & HUD_FLAG_CUP_STANDINGS) != 0)
			{
				UI_CupStandings_InputAndDraw();
			}
		}

		// if hud
		else
		{
			// if not adv hub
			if ((gameMode1 & ADVENTURE_ARENA) == 0)
			{
				// if not drawing end of race
				if (
				    // end of race is not reached
				    ((gameMode1 & END_OF_RACE) == 0) ||

				    // cooldown after end of race not expired
				    (gGT->timerEndOfRaceVS != 0))
				{
					// not crystal challenge
					if ((gameMode1 & CRYSTAL_CHALLENGE) == 0)
					{
						UI_RenderFrame_Racing();
					}

					// if crystal challenge
					else
					{
						UI_RenderFrame_CrystChall();
					}
				}

				// drawing end of race
				else
				{
					if ((u32)(sdata->Loading.stage + 5) > 1)
					{
						if ((gameMode1 & CRYSTAL_CHALLENGE) == 0)
						{
							if ((gameMode1 & TIME_TRIAL) == 0)
							{
								if ((gameMode1 & ARCADE_MODE) == 0)
								{
									if ((gameMode1 & RELIC_RACE) == 0)
									{
										if ((gameMode1 & ADVENTURE_MODE) != 0)
										{
											AA_EndEvent_DrawMenu();
										}
										else if ((gGT->gameMode2 & CUP_ANY_KIND) == 0)
										{
											VB_EndEvent_DrawMenu();
										}
										else
										{
											gGT->hudFlags = (hudFlags & HUD_FLAG_CLEAR_RACE_HUD_MASK) | HUD_FLAG_CUP_STANDINGS;
										}
									}
									else
									{
										RR_EndEvent_DrawMenu();
									}
								}
								else
								{
									AA_EndEvent_DrawMenu();
								}
							}
							else
							{
								TT_EndEvent_DrawMenu();
							}
						}
						else
						{
							CC_EndEvent_DrawMenu();
						}
					}

					return;
				}
			}

			// if adv hub
			else
			{
				// load on last frame of waiting to load 232,
				// leave transition at 1 (see later in func),
				// and load the 232 overlay
				if (gGT->overlayTransition > 1)
				{
					gGT->overlayTransition--;
					if (gGT->overlayTransition == 1)
					{
						LOAD_OvrThreads(2);
					}
				}

				// if 233 is still loaded
				if (!LOAD_IsOpen_AdvHub())
				{
					// if any transition is over
					if (gGT->pushBuffer_UI.fadeFromBlack_currentValue > 0xfff)
					{
						UI_RenderFrame_AdvHub();
					}
				}

				// if 232 overlay is loaded
				else
				{
					// if any transition is over
					if (gGT->pushBuffer_UI.fadeFromBlack_currentValue > 0xfff)
					{
						AH_Map_Main();

						if (sdata->AkuHint_RequestedHint != -1)
						{
							AH_MaskHint_Start(sdata->AkuHint_RequestedHint, sdata->AkuHint_boolInterruptWarppad);

							// erase submitted request
							sdata->AkuHint_RequestedHint = -1;
							sdata->AkuHint_boolInterruptWarppad = 0;
						}
					}

					// if first frame of transition to 232
					if (gGT->overlayTransition != 0)
					{
						gGT->overlayTransition = 0;

						INSTANCE_LevDelayedLInBs(gGT->level1->ptrInstDefs, gGT->level1->numInstances);

						// allow instances again
						gGT->gameMode2 &= ~(NO_LEV_INSTANCE);

						// fade transition
						gGT->pushBuffer_UI.fadeFromBlack_desiredResult = 0x1000;
						gGT->pushBuffer_UI.fade_step = 0x2aa;
					}
				}
			}
		}
	}
}

void RenderAllBeakerRain(struct GameTracker *gGT)
{
	int numPlyrCurrGame = gGT->numPlyrCurrGame;

	// only if beaker rain is enabled
	if ((gGT->renderFlags & RENDER_FLAG_BEAKER_RAIN) == 0)
	{
		return;
	}

	// only for 1P/2P
	if (numPlyrCurrGame > 2)
	{
		return;
	}

	RedBeaker_RenderRain(&gGT->pushBuffer[0], &gGT->backBuffer->primMem, &gGT->JitPools.rain, numPlyrCurrGame, gGT->gameMode1 & PAUSE_ALL);
}

void RenderAllBoxSceneSplitLines(struct GameTracker *gGT)
{
	// Check 233 overlay, cause levelID is set and MainFrame_RenderFrame runs before 233 loads.
	if (LOAD_IsOpen_Podiums())
	{
		if (gGT->levelID == NAUGHTY_DOG_CRATE)
		{
			CS_BoxScene_InstanceSplitLines();
		}
	}
}

void RenderBucket_QueueAllInstances(struct GameTracker *gGT)
{
	int lod;
	int *RBI;
	int numPlyrCurrGame = gGT->numPlyrCurrGame;

	if ((gGT->renderFlags & RENDER_FLAG_RENDER_BUCKET) == 0)
	{
		return;
	}

	lod = numPlyrCurrGame - 1;
	if ((gGT->gameMode1 & RELIC_RACE) != 0)
	{
		lod |= 4;
	}

	RBI = RenderBucket_QueueLevInstances(&gGT->cameraDC[0], &gGT->backBuffer->otMem, gGT->ptrRenderBucketInstance, (u8)sdata->LOD[lod], numPlyrCurrGame,
	                                     gGT->gameMode1 & PAUSE_ALL);

	RBI = RenderBucket_QueueNonLevInstances(gGT->JitPools.instance.taken.first, &gGT->backBuffer->otMem, (void *)RBI, (u8)sdata->LOD[lod], numPlyrCurrGame,
	                                        gGT->gameMode1 & PAUSE_ALL);

	// Aug prototype
#if 0
		// ptrEnd of otmem is less than ptrCurr otmem
    if (*(uint32_t *)(*(int *)(PTR_DAT_8008d2ac + 0x10) + 0x98) <
        *(uint32_t *)(*(int *)(PTR_DAT_8008d2ac + 0x10) + 0x9c)) {
      printf("OTMEM OVERFLOW!\n");
    }
#endif

	// null terminator at end of list
	*RBI = 0;

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER
	// PICA-native model renderer (s30 rung 0): frame edge for the per-instance
	// sampler above. This function is the ONE place per frame that queues every
	// instance for every player (MainFrame_RenderFrame:166), so its tail is the
	// frame boundary -- folding anywhere per-player would double-count split
	// screen, and folding from the level-geometry path would miss frames that
	// draw instances without drawing track.
	// Implementation: platform/native_model_frame.c.
	{
		extern void NativeModelFrame_EndFrame(void);
		NativeModelFrame_EndFrame();
	}
#endif
}

void RenderAllNormalParticles(struct GameTracker *gGT)
{
	int i;

	if ((gGT->renderFlags & RENDER_FLAG_PARTICLES) == 0)
	{
		return;
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s41b: the whole particle position space is 4x world units -- the camera
	 * offset is `camera.t << 2` (Particle.c:1144) and a driver-local particle
	 * adds `inst.t << 2` (Particle.c:1238). The game says so itself at
	 * Particle.c:1282, where the draw-distance limit is scaled `<< 2` to meet
	 * `depth` in its own units. Same 4x regime axis as tires/shadows/skids,
	 * and likewise outside RenderBucket.
	 *
	 * Torch's heat-warp list is NOT in this bracket and must not be: it reads
	 * the same particle fields with `>> 8` instead of `>> 6` (Torch.c:565),
	 * which already divides the 4x back out.
	 */
	GTECapture_BeginFixedDepthShift(2);
#endif

	for (i = 0; i < gGT->numPlyrCurrGame; i++)
	{
		Particle_RenderList(&gGT->pushBuffer[i], gGT->particleList_ordinary);
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	GTECapture_EndInstanceDepth();
#endif
}

void RenderDispEnv_World(struct GameTracker *gGT)
{
	int i;
	struct PushBuffer *pb;
	for (i = 0; i < gGT->numPlyrCurrGame; i++)
	{
		pb = &gGT->pushBuffer[i];
		PushBuffer_SetDrawEnv_Normal(&pb->ptrOT[0x3ff], pb, gGT->backBuffer, 0, 0);
	}
}

// I need a better name
void RenderAllFlag0x40(struct GameTracker *gGT)
{
	if ((gGT->renderFlags & RENDER_FLAG_EFFECT_BUCKETS) == 0)
	{
		return;
	}

	if (LOAD_IsOpen_RacingOrBattle())
	{
		RB_Player_ToggleInvisible();
		RB_Player_ToggleFlicker();
		RB_Burst_ProcessBucket(gGT->threadBuckets[BURST].thread);
		RB_Blowup_ProcessBucket(gGT->threadBuckets[BLOWUP].thread);

		RB_Spider_DrawWebs(gGT->threadBuckets[SPIDER].thread, &gGT->pushBuffer[0]);
		RB_Follower_ProcessBucket(gGT->threadBuckets[FOLLOWER].thread);
		RB_StartText_ProcessBucket(gGT->threadBuckets[STARTTEXT].thread);
	}

	if (LOAD_IsOpen_AdvHub())
	{
		if ((gGT->gameMode1 & ADVENTURE_ARENA) != 0)
		{
			AH_WarpPad_AllWarppadNum();
		}
	}

	VehTurbo_ProcessBucket(gGT->threadBuckets[TURBO].thread);

	int i;
	struct PushBuffer *pb;
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s41: VehGroundSkids scales camera-relative points by 4
	 * (VEH_GROUND_SKIDS_PROJECT_SCALE_SHIFT) and projects with a zero
	 * translation vector -- 4x regime axis, outside RenderBucket. Brackets the
	 * whole player loop: every iteration projects the same way, and the
	 * bracket carries no per-instance state.
	 */
	GTECapture_BeginFixedDepthShift(2);
#endif
	for (i = 0; i < gGT->numPlyrCurrGame; i++)
	{
		pb = &gGT->pushBuffer[i];
		VehGroundSkids_Main(gGT->threadBuckets[PLAYER].thread, pb);
		VehGroundSkids_Main(gGT->threadBuckets[ROBOT].thread, pb);
	}
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	GTECapture_EndInstanceDepth();
#endif
}

void RenderAllTitleDPP(struct GameTracker *gGT)
{
	if ((gGT->gameMode1 & MAIN_MENU) == 0)
	{
		return;
	}
	if (!LOAD_IsOpen_MainMenu())
	{
		return;
	}
	MM_Title_SetTrophyDPP();
}

void RenderBucket_ExecuteAllInstances(struct GameTracker *gGT)
{
	if ((gGT->renderFlags & RENDER_FLAG_RENDER_BUCKET) == 0)
	{
		return;
	}

	RenderBucket_Execute(gGT->ptrRenderBucketInstance, &gGT->backBuffer->primMem);

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER
	// s32 rung 1 frame edge for the emit capture. NOT the same edge as
	// NativeModelFrame_EndFrame's, which sits at the tail of the QUEUE pass --
	// the capture is filled by RenderBucket_Execute just above, which runs a
	// separate pass later in the frame (MainFrame_RenderFrame:195), so sealing
	// it at the queue edge would report every frame as empty.
	// Implementation: platform/native_model_emit.c.
	{
		extern void NativeModelEmit_EndFrame(void);
		NativeModelEmit_EndFrame();
	}
#endif
}

void RenderAllTires(struct GameTracker *gGT)
{
	int numPlyrCurrGame;
	struct PrimMem *gGT_primMem;

	if ((gGT->renderFlags & RENDER_FLAG_TIRES) == 0)
	{
		return;
	}

	gGT_primMem = &gGT->backBuffer->primMem;
	numPlyrCurrGame = gGT->numPlyrCurrGame;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s41: DrawTires projects camera-relative positions pre-multiplied by 4
	 * (DrawTiresSolid_SetupGteState, `<< 2`) with a zero translation vector,
	 * so its SZ3 lands on the 4x regime axis. Outside RenderBucket, so the
	 * dispatch-site bracket never covered it.
	 */
	GTECapture_BeginFixedDepthShift(2);
#endif

	/* The reflection legs run in "under" depth mode: retail tucks the mirrored
	 * tires beneath the opaque road by painter's order, so on the 3DS depth
	 * substrate they must LOSE the ground tie the solid-leg decal bias exists
	 * to win. Toggled per call rather than hoisting the reflections out of the
	 * bucket order -- OT links are head-inserted, so reordering emission would
	 * reorder same-bucket draws on the PC/retail path too. */
	if (gGT->threadBuckets[PLAYER].thread != 0)
	{
		DrawTires_Solid(gGT->threadBuckets[PLAYER].thread, gGT_primMem, numPlyrCurrGame);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_SetFixedShiftUnder(1);
#endif
		DrawTires_Reflection(gGT->threadBuckets[PLAYER].thread, gGT_primMem, numPlyrCurrGame);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_SetFixedShiftUnder(0);
#endif
	}

	if (gGT->numBotsNextGame != 0)
	{
		DrawTires_Solid(gGT->threadBuckets[ROBOT].thread, gGT_primMem, numPlyrCurrGame);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_SetFixedShiftUnder(1);
#endif
		DrawTires_Reflection(gGT->threadBuckets[ROBOT].thread, gGT_primMem, numPlyrCurrGame);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_SetFixedShiftUnder(0);
#endif
	}

	if (gGT->threadBuckets[GHOST].thread != 0)
	{
		DrawTires_Solid(gGT->threadBuckets[GHOST].thread, gGT_primMem, numPlyrCurrGame);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_SetFixedShiftUnder(1);
#endif
		DrawTires_Reflection(gGT->threadBuckets[GHOST].thread, gGT_primMem, numPlyrCurrGame);
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		GTECapture_SetFixedShiftUnder(0);
#endif
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	GTECapture_EndInstanceDepth();
#endif
}

void RenderAllShadows(struct GameTracker *gGT)
{
	if ((gGT->renderFlags & RENDER_FLAG_SHADOWS) == 0)
	{
		return;
	}
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s41: VehGroundShadow scales its camera delta by 4
	 * (VEH_GROUND_SHADOW_CAMERA_DELTA_SCALE) and projects with a zero
	 * translation vector -- 4x regime axis, outside RenderBucket.
	 */
	GTECapture_BeginFixedDepthShift(2);
	VehGroundShadow_Main();
	GTECapture_EndInstanceDepth();
#else
	VehGroundShadow_Main();
#endif
}

void RenderAllHeatParticles(struct GameTracker *gGT)
{
	if ((gGT->renderFlags & RENDER_FLAG_HEAT_EFFECT) == 0)
	{
		return;
	}

	MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_TORCH_MAIN);
	Torch_Main(gGT->particleList_heatWarp, &gGT->pushBuffer[0], &gGT->backBuffer->primMem, gGT->numPlyrCurrGame, gGT->swapchainIndex * 0x128);
	MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_TORCH_MAIN);
}

static s32 RenderAllLevelGeometry_ScaleDistanceShift8(s32 distToScreen, s32 scale)
{
	s32 product = CTR_MipsMulLo(distToScreen, scale);

	if (product < 0)
	{
		product = CTR_MipsAddLo(product, 0xff);
	}

	return CTR_MipsSra(product, 8);
}

// Native LOD bias. CTR already switches track detail by distance on its own:
// RenderLists_Select1P2PSlot (game/RenderLevel/RenderLists.c:137) routes a BSP
// leaf past bspLodDistanceThreshold into the FULL_DYNAMIC list, which overlay
// 226 draws at DRAW_LEVEL_OVR1P_LOD_LOW instead of the subdivided 4x4/4x2/
// dynamic lists, and the two texture thresholds pick a finer texture variant
// per quadblock (226_00_DrawLevelOvr1P.c:637). Both distances are seeded once
// per frame from distanceToScreen below, which makes this the single place a
// native build can move them without touching the decision sites themselves.
//
// Why it exists: the 3DS frame is OT-walk-bound and the walk's cost tracks the
// prim count it is handed (draw_otag 46.9ms of a 70.2ms N. Gin Labs frame,
// hardware 2026-07-28), so the hope was to scale the input to the expensive
// part rather than optimize the part itself.
//
// MEASURED RESULT, PC attract race, 2026-07-28 -- THE GEOMETRY SCALE DOES NOT
// SHED PRIMS. At 50% the slot distribution moves exactly as designed (leaves
// pour out of the subdivided lists into FULL_DYNAMIC; see the [LODPROBE] line
// below), and prims/f does not fall with it: 1092/f at retail vs 1215/f at 50%
// on the same track. FULL_DYNAMIC is a different DRAWING STYLE for a quadblock,
// not a lower-density one -- its bucket even reserves the largest prim block of
// any list (DRAW_LEVEL_OVR1P_BUCKET_RESERVE_FULL_DYNAMIC). Whatever controls
// quadblock density in CTR, it is not this threshold, and the OT walk is handed
// the same work either way.
//
// So this is kept as a measuring instrument and a negative result, not as an
// fps lever: it is the rig that answers "would pulling the draw distance in
// help?" without rebuilding it from scratch every time someone asks. Before
// using it for anything else, re-read the [LODPROBE] counters -- the answer so
// far is no. Owner ruling the same day does put fps ahead of visual parity on
// 3DS, so a lever like this would have been allowed had it worked.
//
// Scales are percentages of the retail distance: 100 = retail, lower = detail
// drops nearer the camera = less work. At 100 nothing is written at all, so an
// enabled build with untouched scales is byte-identical to a disabled one.
// Scope: 1P only. RenderLists uses its own 0x1540 constant when numPlyr != 1,
// so split-screen stays retail no matter what these say.
#ifndef CTR_NATIVE_LOD_BIAS
#define CTR_NATIVE_LOD_BIAS 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_LOD_BIAS
// Runtime A/B: flipping mid-race brackets [PERF]/[OTPROF] windows by state on
// identical content. Cross-run comparison is not an option here -- this
// project's fps numbers have confounded on track, game mode and AI count often
// enough that only a mid-run flip is trusted.
int g_ctrLodGeoScale = 100;
int g_ctrLodTexScale = 100;

// Clamping and the A/B ladder live here rather than at the two toggle sites
// (PC keys, 3DS dumpreq) so the value a toggle logs is always the value the
// frame will actually use.
int CTR_LodBias_Clamp(int percent)
{
	if (percent < 10)
	{
		return 10;
	}

	if (percent > 200)
	{
		return 200;
	}

	return percent;
}

// 100 (retail) -> 75 -> 50 -> 35 -> back to retail. Coarse on purpose: the
// point of a mid-race step is to bracket [PERF] windows far enough apart that
// the delta clears run-to-run noise, not to find a final number.
int CTR_LodBias_Step(int percent)
{
	if (percent > 75)
	{
		return 75;
	}

	if (percent > 50)
	{
		return 50;
	}

	if (percent > 35)
	{
		return 35;
	}

	return 100;
}

static s32 RenderAllLevelGeometry_ScaleLodDistance(s32 distance, int percent)
{
	// 64-bit intermediate: the race path seeds these as distanceToScreen * 0x1a,
	// and a scale above 100 is free to overflow s32 on the way back down.
	return (s32)(((s64)distance * (s64)CTR_LodBias_Clamp(percent)) / 100);
}

static void RenderAllLevelGeometry_ApplyLodBias(struct MainRenderLevelGeometryScratch *scratch)
{
	if (g_ctrLodGeoScale != 100)
	{
		scratch->bspLodDistanceThreshold = RenderAllLevelGeometry_ScaleLodDistance(scratch->bspLodDistanceThreshold, g_ctrLodGeoScale);

		// Both retail branches derive the fade start from the threshold. Leaving
		// it stale would detach the FULL_DYNAMIC fade band (consumed at
		// 226_00_DrawLevelOvr1P.c:2252) from the list it fades, and distant
		// geometry would pop in at full brightness instead of fading up.
		scratch->fullDynamicFadeDepthStart = CTR_MipsAddLo(scratch->bspLodDistanceThreshold, MAIN_RENDER_LEVEL_GEOMETRY_FULL_DYNAMIC_FADE_OFFSET);
	}

	if (g_ctrLodTexScale != 100)
	{
		scratch->textureLodDepthThreshold0 = RenderAllLevelGeometry_ScaleLodDistance(scratch->textureLodDepthThreshold0, g_ctrLodTexScale);
		scratch->textureLodDepthThreshold1 = RenderAllLevelGeometry_ScaleLodDistance(scratch->textureLodDepthThreshold1, g_ctrLodTexScale);
	}

	// Deliberate omissions:
	// - topLevelNearDepthThreshold is a third texture-variant bump
	//   (226_00_DrawLevelOvr1P.c:647), not a geometry control, so it belongs with
	//   the texture scale rather than the geometry one; left alone so the two
	//   scales above stay separable. depthScale and recursiveNearDepthThreshold
	//   are seeded by retail but have NO reader anywhere in game/ -- do not add
	//   them here expecting an effect.
	// - DrawLevelOvr1P_SetSplitGroundThresholdScratch (226_00_DrawLevelOvr1P.c:8074)
	//   re-seeds the texture thresholds from fixed constants for split-ground
	//   blocks, so g_ctrLodTexScale does not reach those. It does not touch
	//   bspLodDistanceThreshold, so the geometry scale is unaffected.
}
#endif

void RenderAllLevelGeometry(struct GameTracker *gGT, struct Level *level1, struct mesh_info *ptr_mesh_info)
{
	int i;
	s32 distToScreen;
	int numPlyrCurrGame;
	struct MainRenderLevelGeometryScratch *scratch;
	struct PushBuffer *pushBuffer;

	if (level1 == 0)
	{
		return;
	}

	if (ptr_mesh_info == 0)
	{
		return;
	}

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_CENSUS) && CTR_NATIVE_TRACK_CENSUS
	// Native-track-renderer censuses (s27): once per level, read-only, logs a
	// [TRKCENSUS] block. Same instrument-in-game-code precedent as [LODPROBE]
	// below. Implementation: platform/native_track_census.c.
	{
		extern void NativeTrackCensus_Frame(struct Level *level, struct mesh_info *mesh);
		NativeTrackCensus_Frame(level1, ptr_mesh_info);
	}
#endif

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
	// Native track renderer (s27+): rebuilds the resident track mesh when the
	// level changes. Count-only at ladder step 1 -- draws nothing yet.
	// Implementation: platform/native_track_mesh.c.
	{
		extern void NativeTrackMesh_Frame(struct Level *level, struct mesh_info *mesh);
		NativeTrackMesh_Frame(level1, ptr_mesh_info);
	}
#endif

#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
	// Model/keyframe accounting, once per level (platform/native_model_census.c).
	// Deliberately OUTSIDE the track-renderer gate above: it answers a question
	// about karts, not track, and has to be readable from a plain PC build.
	{
		extern void NativeModelCensus_Report(struct GameTracker *gGT, struct Level *level);
		NativeModelCensus_Report(gGT, level1);
	}
#endif

	numPlyrCurrGame = gGT->numPlyrCurrGame;

	if (numPlyrCurrGame == 1)
	{
		CTR_ClearRenderLists_1P2P(gGT, 1);

		// === Temporary 60FPS macros ===
		// Emulate 30fps on 60fps for SCVert and OVert

		// if no SCVert
		if ((level1->configFlags & 4) == 0)
		{
			// assume OVert (no primitives generated here)
			AnimateWater1P(gGT->timer, level1->numWaterVertices, level1->ptr_water, level1->ptr_tex_waterEnvMap, gGT->visMem1->visOVertList[0]);
		}

		// if SCVert
		else
		{
			// draw SCVert (no primitives generated here
			AnimateQuad(gGT->timer << 7, level1->numSCVert, level1->ptrSCVert, gGT->visMem1->visSCVertList[0]);
		}

		// camera of player 1
		pushBuffer = &gGT->pushBuffer[0];
		scratch = CTR_SCRATCHPAD_PTR(struct MainRenderLevelGeometryScratch, 0);

		if (
		    // adv character selection screen
		    (gGT->levelID == ADVENTURE_GARAGE) ||

		    // cutscene that's not Crash Bandicoot intro
		    // where he's sleeping and snoring on a hill
		    (((gGT->gameMode1 & GAME_CUTSCENE) != 0) && (gGT->levelID != INTRO_CRASH)))
		{
			// relationship between near-clip and far-clip,
			// for each RenderList LOD set in the level
			scratch->depthScale = 0x1e00;
			scratch->bspLodDistanceThreshold = 0x640;
			scratch->textureLodDepthThreshold0 = 0x640;
			scratch->textureLodDepthThreshold1 = 0x500;
			scratch->topLevelNearDepthThreshold = 0x280;
			scratch->recursiveNearDepthThreshold = 0x140;
			scratch->fullDynamicFadeDepthStart = scratch->bspLodDistanceThreshold + MAIN_RENDER_LEVEL_GEOMETRY_FULL_DYNAMIC_FADE_OFFSET;
		}

		// every non-cutscene,
		// except for Crash Bandicoot intro
		else
		{
			// 0x1c2 in 1P mode
			distToScreen = pushBuffer->distanceToScreen_PREV;

			scratch->depthScale = RenderAllLevelGeometry_ScaleDistanceShift8(distToScreen, 0x2080);
			scratch->bspLodDistanceThreshold = CTR_MipsMulLo(distToScreen, 0x1a);
			scratch->textureLodDepthThreshold0 = CTR_MipsMulLo(distToScreen, 0x18);
			scratch->textureLodDepthThreshold1 = CTR_MipsMulLo(distToScreen, 0xc);
			scratch->topLevelNearDepthThreshold = CTR_MipsMulLo(distToScreen, 7);
			scratch->recursiveNearDepthThreshold = RenderAllLevelGeometry_ScaleDistanceShift8(distToScreen, 0x380);
			scratch->fullDynamicFadeDepthStart = CTR_MipsAddLo(scratch->bspLodDistanceThreshold, MAIN_RENDER_LEVEL_GEOMETRY_FULL_DYNAMIC_FADE_OFFSET);
		}

#if defined(CTR_NATIVE) && CTR_NATIVE_LOD_BIAS
		// After both branches, before the lists are built: RenderLists_Init1P2P
		// reads bspLodDistanceThreshold straight out of this scratch, so the bias
		// lands on a whole frame or not at all. Covers the cutscene branch too --
		// the intro is in-engine track flybys (hardware, 2026-07-28), not video.
		RenderAllLevelGeometry_ApplyLodBias(scratch);
#endif

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_LOD_BIAS) && CTR_NATIVE_LOD_BIAS
		{
			extern int g_lodProbeSlot[6];
			extern int g_lodProbeEarlyOut;
			extern int g_lodProbeDistTested;
			static int s_lodProbeFrames;

			if (++s_lodProbeFrames >= 64)
			{
				Platform_Log("[LODPROBE] thr=%d early=%d distTested=%d | 4x4=%d subdiv=%d 4x2=%d 4x1=%d water=%d fulldyn=%d\n",
				             scratch->bspLodDistanceThreshold, g_lodProbeEarlyOut, g_lodProbeDistTested, g_lodProbeSlot[0], g_lodProbeSlot[1],
				             g_lodProbeSlot[2], g_lodProbeSlot[3], g_lodProbeSlot[4], g_lodProbeSlot[5]);
				s_lodProbeFrames = 0;
				g_lodProbeEarlyOut = 0;
				g_lodProbeDistTested = 0;
				memset(g_lodProbeSlot, 0, sizeof(g_lodProbeSlot));
			}
		}
#endif

		RenderLists_PreInit();
		gGT->bspLeafsDrawn = 0;

		gGT->bspLeafsDrawn += RenderLists_Init1P2P(ptr_mesh_info->bspRoot, gGT->visMem1->visLeafList[0], pushBuffer, (u32)&gGT->LevRenderLists[0],
		                                           gGT->visMem1->bspList[0], numPlyrCurrGame);

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
		// Native track renderer (rung 3a step 1): per-frame camera + visibility
		// snapshot, recorded from the SAME render-list traversal DrawLevelOvr1P
		// is about to consume below -- gGT->LevRenderLists[0] is only valid
		// after RenderLists_Init1P2P just above has populated it. Count-only at
		// this ladder step -- nothing is drawn from this snapshot yet.
		// Implementation: platform/native_track_frame.c.
		{
			extern void NativeTrackFrame_Record(struct GameTracker *gGT, struct mesh_info *mesh, struct DrawLevelOvr1PRenderList *renderList,
			                                     struct PushBuffer *pushBuffer);
			// gGT->LevRenderLists[0] is declared as an anonymous struct in
			// namespace_Main.h (identical layout: list[5] of {ptrQuadBlocksRendered,
			// bspListStart} + bspListStart_FullDynamic/ptrQuadBlocksRendered_FullDynamic
			// -- see namespace_DrawLevel.h), so the cast is layout-safe, not a
			// reinterpretation of different data. Routed through (void *) first,
			// same idiom as game/DecalMP.c and game/Vehicle/VehPhysCrash.c, so
			// the compiler doesn't flag a direct incompatible-pointer-type cast
			// as a strict-aliasing hazard for what is actually the same object
			// DrawLevelOvr1P below already reads through its own void* parameter.
			NativeTrackFrame_Record(gGT, ptr_mesh_info, (struct DrawLevelOvr1PRenderList *)(void *)&gGT->LevRenderLists[0], pushBuffer);
		}
#endif

		// 226-229
		//
		// Rung 3a step 2's gate:
		// toggle state 1 (resident) REPLACES this retail draw -- the resident
		// path (platform/native_track_frame.c's NativeTrackDraw_IssueForWalk,
		// called later from NativeRenderer_EndScene) draws the track instead.
		// State 2 (both) leaves this call in place as the projection
		// diagnostic overlay's retail baseline. skipRetailTrackDraw is a
		// compile-time-const 0 in every OTHER configuration (state 0, or the
		// gate compiled out entirely), so the compiler folds `if (!0)` away
		// and DrawLevelOvr1P is called exactly as it is today -- ONE call
		// site rather than two copies that could drift apart.
#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
		int skipRetailTrackDraw;
		{
			extern int NativeTrackFrame_SkipRetailDraw(void);

			/*
			 * The toggle state alone does NOT decide this -- see
			 * NativeTrackFrame_SkipRetailDraw's own comment. It also weighs
			 * whether a resident draw exists on this platform at all (it is
			 * 3DS-only) and whether gap fill needs the retail call kept alive
			 * for the leaves residency excluded. Asking one function keeps that
			 * policy in platform/ where it belongs instead of spreading a
			 * platform test through the decomp.
			 */
			skipRetailTrackDraw = NativeTrackFrame_SkipRetailDraw();
		}
#else
		const int skipRetailTrackDraw = 0;
#endif

		if (!skipRetailTrackDraw)
		{
			DrawLevelOvr1P(&gGT->LevRenderLists[0], pushBuffer, (struct BSP *)ptr_mesh_info, &gGT->backBuffer->primMem, gGT->visMem1->visFaceList[0],
			               level1->ptr_tex_waterEnvMap); // waterEnvMap?
		}

		DrawSky_Full(level1->ptr_skybox, pushBuffer, &gGT->backBuffer->primMem);

		// skybox gradient
		if ((level1->configFlags & 1) != 0)
		{
			goto SkyboxGlow;
		}

		return;
	}

	if (numPlyrCurrGame == 2)
	{
		CTR_ClearRenderLists_1P2P(gGT, 2);

		// if no SCVert
		if ((level1->configFlags & 4) == 0)
		{
			// assume OVert (no primitives generated here)
			AnimateWater2P(gGT->timer, level1->numWaterVertices, level1->ptr_water, level1->ptr_tex_waterEnvMap, gGT->visMem1->visOVertList[0],
			               gGT->visMem1->visOVertList[1]);
		}

		RenderLists_PreInit();
		gGT->bspLeafsDrawn = 0;

		for (i = 0; i < numPlyrCurrGame; i++)
		{
			gGT->bspLeafsDrawn += RenderLists_Init1P2P(ptr_mesh_info->bspRoot, gGT->visMem1->visLeafList[i], &gGT->pushBuffer[i], (u32)&gGT->LevRenderLists[i],
			                                           gGT->visMem1->bspList[i], numPlyrCurrGame);
		}

		// 226-229
		DrawLevelOvr2P(&gGT->LevRenderLists[0], &gGT->pushBuffer[0], (struct BSP *)ptr_mesh_info, &gGT->backBuffer->primMem, gGT->visMem1->visFaceList[0],
		               gGT->visMem1->visFaceList[1],
		               level1->ptr_tex_waterEnvMap); // waterEnvMap?

		goto SkyboxGlow;
	}

	// 3P or 4P
	CTR_ClearRenderLists_3P4P(gGT, numPlyrCurrGame);

	// if no SCVert
	if ((level1->configFlags & 4) == 0)
	{
		if (numPlyrCurrGame == 3)
		{
			// assume OVert (no primitives generated here)
			AnimateWater3P(gGT->timer, level1->numWaterVertices, level1->ptr_water, level1->ptr_tex_waterEnvMap, gGT->visMem1->visOVertList[0],
			               gGT->visMem1->visOVertList[1], gGT->visMem1->visOVertList[2]);
		}

		else // 4P mode
		{
			// assume OVert (no primitives generated here)
			AnimateWater4P(gGT->timer, level1->numWaterVertices, level1->ptr_water, level1->ptr_tex_waterEnvMap, gGT->visMem1->visOVertList[0],
			               gGT->visMem1->visOVertList[1], gGT->visMem1->visOVertList[2], gGT->visMem1->visOVertList[3]);
		}
	}

	RenderLists_PreInit();
	gGT->bspLeafsDrawn = 0;

	for (i = 0; i < numPlyrCurrGame; i++)
	{
		gGT->bspLeafsDrawn += RenderLists_Init3P4P(ptr_mesh_info->bspRoot, gGT->visMem1->visLeafList[i], &gGT->pushBuffer[i], (u32)&gGT->LevRenderLists[i],
		                                           gGT->visMem1->bspList[i]);
	}

	if (numPlyrCurrGame == 3)
	{
		// 226-229
		DrawLevelOvr3P(&gGT->LevRenderLists[0], &gGT->pushBuffer[0], (struct BSP *)ptr_mesh_info, &gGT->backBuffer->primMem, gGT->visMem1->visFaceList[0],
		               gGT->visMem1->visFaceList[1], gGT->visMem1->visFaceList[2],
		               level1->ptr_tex_waterEnvMap); // waterEnvMap?
	}

	else // 4P mode
	{
		// 226-229
		DrawLevelOvr4P(&gGT->LevRenderLists[0], &gGT->pushBuffer[0], (struct BSP *)ptr_mesh_info, &gGT->backBuffer->primMem, gGT->visMem1->visFaceList[0],
		               gGT->visMem1->visFaceList[1], gGT->visMem1->visFaceList[2], gGT->visMem1->visFaceList[3],
		               level1->ptr_tex_waterEnvMap); // waterEnvMap?
	}

SkyboxGlow:

	// skybox gradient
	for (i = 0; i < numPlyrCurrGame; i++)
	{
		pushBuffer = &gGT->pushBuffer[i];
		CAM_SkyboxGlow(&level1->glowGradient[0], pushBuffer, &gGT->backBuffer->primMem, &pushBuffer->ptrOT[0x3ff]);
	}

	return;
}

void WindowBoxLines(struct GameTracker *gGT)
{
	int i;

	// only battle and 3P4P mode allowed
	if ((gGT->gameMode1 & BATTLE_MODE) == 0)
	{
		return;
	}
	if (gGT->numPlyrCurrGame < 3)
	{
		return;
	}

	for (i = 0; i < gGT->numPlyrCurrGame; i++)
	{
		Color color;
		color.self = *data.ptrColor[gGT->drivers[i]->BattleHUD.teamID + PLAYER_BLUE];
		RECTMENU_DrawOuterRect_LowLevel(

		    // dimensions, thickness
		    &gGT->pushBuffer[i].rect, 4, 2,

		    // color data
		    color,

		    0,

		    // pushBuffer_UI = 0x1388
		    &gGT->pushBuffer_UI.ptrOT[3]);
	}
}

void WindowDivsionLines(struct GameTracker *gGT)
{
	POLY_F4 *p;
	int numPlyrCurrGame;

	numPlyrCurrGame = gGT->numPlyrCurrGame;

	// horizontal bar
	if (numPlyrCurrGame > 1)
	{
		p = gGT->backBuffer->primMem.cursor;

		// set R, G, B, CODE, all to zero,
		// this makes black color, and invalid CODE
		CtrGpu_WriteColorCode(&p->r0, 0);

		// this sets CODE to the proper value
		setPolyF4(p);

		// Make four (x,y) coordinates
		p->y0 = 0x6a;
		p->y1 = 0x6a;
		p->x0 = 0;
		p->x1 = 0x200;

		p->x2 = 0;
		p->y2 = 0x6e;
		p->x3 = 0x200;
		p->y3 = 0x6e;

		// Draw a bar from left to right,
		// dividing the screen in half on top and bottom
		AddPrim(&gGT->pushBuffer_UI.ptrOT[3], p);

		gGT->backBuffer->primMem.cursor = (void *)(p + 1);
	}

	// vertical bar
	if (numPlyrCurrGame > 2)
	{
#if 0
		gGT->drivers[0]->numWumpas = DRIVER_WUMPA_JUICED_COUNT;
		gGT->drivers[0]->heldItemID = HELD_ITEM_TNT;
		gGT->drivers[1]->numWumpas = DRIVER_WUMPA_JUICED_COUNT;
		gGT->drivers[1]->heldItemID = HELD_ITEM_TNT;
#endif

		p = gGT->backBuffer->primMem.cursor;

		// set R, G, B, CODE, all to zero,
		// this makes black color, and invalid CODE
		CtrGpu_WriteColorCode(&p->r0, 0);

		// this sets CODE to the proper value
		setPolyF4(p);

		// Make four (x,y) coordinates
		p->x0 = 0xfd;
		p->x2 = 0xfd;
		p->y0 = 0;
		p->x1 = 0x103;

		p->y1 = 0;
		p->y2 = 0xd8;
		p->x3 = 0x103;
		p->y3 = 0xd8;

		// Draw a bar from left to right,
		// dividing the screen in half on top and bottom
		AddPrim(&gGT->pushBuffer_UI.ptrOT[3], p);

		// backBuffer->primMem.cursor
		gGT->backBuffer->primMem.cursor = (void *)(p + 1);
	}

	// if numPlyrCurrGame is 3
	if (numPlyrCurrGame == '\x03')
	{
		// This is useless, cause it gets cleared
		// to black anyway, even without this block,
		// at least it does it Crash Cove, does it always?

		p = gGT->backBuffer->primMem.cursor;

		// set R, G, B, CODE, all to zero,
		// this makes black color, and invalid CODE
		CtrGpu_WriteColorCode(&p->r0, 0);

		// this sets CODE to the proper value
		setPolyF4(p);

		// xy0
		p->x0 = 0x100;
		p->x2 = 0x100;
		p->y0 = 0x6c;
		p->y1 = 0x6c;
		p->x1 = 0x200;
		p->y2 = 0xd8;
		p->x3 = 0x200;
		p->y3 = 0xd8;

		// Draw a bar from left to right,
		// dividing the screen in half on top and bottom
		AddPrim(&gGT->pushBuffer_UI.ptrOT[3], p);

		// backBuffer->primMem.cursor
		gGT->backBuffer->primMem.cursor = (void *)(p + 1);
	}
}

void RenderDispEnv_UI(struct GameTracker *gGT)
{
	struct PushBuffer *pb = &gGT->pushBuffer_UI;

	PushBuffer_SetDrawEnv_Normal(&pb->ptrOT[4], pb, gGT->backBuffer, 0, 0);
}

CTR_GCC_OPTIMIZE_O0 int ReadyToFlip(struct GameTracker *gGT)
{
	return
	    // two VSYNCs passed, 30fps lock
	    (sdata->vsyncTillFlip < 1) &&

	    // if DrawOTag finished
	    (gGT->bool_DrawOTag_InProgress == 0);
}

CTR_GCC_OPTIMIZE_O0 int ReadyToBreak(struct GameTracker *gGT)
{
	return

	    // if more than 6 VSYNCs passed since
	    // the last successful draw, FPS < 10fps
	    gGT->vSync_between_drawSync > 6;
}

void RenderVSYNC(struct GameTracker *gGT)
{
	gGT->clockDurationStall = Timer_GetTime_Total();

	// render checkered flag
	if ((gGT->renderFlags & RENDER_FLAG_CHECKERED_FLAG) != 0)
	{
		// Wait until "next" vsync,
		// Main Menu at 45fps will cap to 30fps
		// Levels+RaceFlag at 25fps will cap to 20fps
		VSync(0);
	}


	while (1)
	{
#ifdef CTR_NATIVE
		// NOTE(aalhendi): Native host sync needs DrawSync polling here; retail
		// falls through to the BreakDraw guard below instead.
		// must be called in the loop,
		// or else it wont properly sync
		// s36: bracketed to separate "waiting for the render worker" from
		// "waiting for the 30fps clock" (vsync_wait_ms, inside VSync below).
		// The parent bucket alone cannot tell those apart, and they have
		// completely different fixes.
		MAINFRAME_PERF_BEGIN(NATIVE_PERF_BUCKET_RENDER_VSYNC_DRAWSYNC);
		DrawSync(0);
		MAINFRAME_PERF_END(NATIVE_PERF_BUCKET_RENDER_VSYNC_DRAWSYNC);
#endif

		if (ReadyToFlip(gGT))
		{
			// quit, end of stall
			return;
		}

#ifdef CTR_NATIVE
		// NOTE(aalhendi): Retail waits on GPU/vblank hardware here. Native
		// owns that wait in VSync(), which also emits the VBlank callback.
		VSync(0);
#endif

#ifndef CTR_NATIVE
		if (ReadyToBreak(gGT))
		{
			// just quit and try the next frame
			BreakDraw();
			return;
		}
#endif
	}
}

#ifndef CTR_NATIVE
void RenderFMV()
{
	if (sdata->boolPlayVideoSTR == 1)
	{
		MM_Video_CheckIfFinished(1);

		MoveImage(&sdata->videoSTR_src_vramRect, sdata->videoSTR_dst_vramX, sdata->videoSTR_dst_vramY);

		DrawSync(0);
	}
}
#endif

void RenderSubmit(struct GameTracker *gGT)
{
	// 1 VSYNC = 60fps
	// 2 VSYNCs = 30fps

#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_RENDER_SUBMIT);
#endif

	gGT->clockDurationStall = Timer_GetTime_Elapsed(gGT->clockDurationStall, 0);

#if defined(CTR_NATIVE)

	sdata->vsyncTillFlip = 2;

	// Native still renders immediately through PsyCross, so keep the host GPU's
	// active draw/display envs in step with the retail DB selected this frame.
	PutDrawEnv(&gGT->backBuffer->drawEnv);
	gGT->frontBuffer = &gGT->db[1 - gGT->swapchainIndex];
	PutDispEnv(&gGT->frontBuffer->dispEnv);

#else

	// do I need the "if"? will it ever be nullptr?
	if (gGT->frontBuffer != 0)
	{
		sdata->vsyncTillFlip = 2;
		gGT->unk1cc4[5] = gGT->unk1cc4[0];

		if ((sdata->boolDebugDispEnv & 1) != 0)
			PutDispEnv(&sdata->blank_debug_DispEnv);
		else
			PutDispEnv(&gGT->frontBuffer->dispEnv);
		PutDrawEnv(&gGT->frontBuffer->drawEnv);
		gGT->frontBuffer = 0;
	}

	// swap=0, get db[1]
	// swap=1, get db[0]
	gGT->frontBuffer = &gGT->db[1 - gGT->swapchainIndex];
#endif

	gGT->bool_DrawOTag_InProgress = 1;

	void *ot = &gGT->pushBuffer[0].ptrOT[0x3ff];

	DrawOTag(ot);

	gGT->frameTimer_notPaused = gGT->frameTimer_VsyncCallback;

#if defined(CTR_NATIVE) && defined(CTR_INTERNAL)
	NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDER_SUBMIT);
#endif
}
