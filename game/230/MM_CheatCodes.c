#include <common.h>
#include <native_retro_fueled.h>

#if defined(CTR_NATIVE)
/*
 * Native-only flag easter eggs. The style selector lives
 * platform-side (native_gpu.c): 0 retail checkers, 1 pride stripes, 2 purple,
 * 3 blue (set by ARROW, below, while Retro-Fueled mode is on).
 *
 * MM_Cheat_SpyroFlag REPLACES MainKillGame_LaunchSpyro2 as the SOAR handler,
 * and doing so fixes a hard-lock rather than merely repurposing a dead code:
 * retail's SOAR chain-loaded the Spyro 2 demo off the disc, native cannot
 * chain-load an executable, and that function tears the game down (error
 * screen, Music_Stop, howl_StopAudio, Bank_DestroyAll, howl_Disable,
 * MainKillGame_StopCTR) and then sits in `while (1) {}`. Entering SOAR on a
 * native build killed audio and froze the console. The D230 cheat table was its
 * only caller, so repointing that one row removes the hazard entirely.
 *
 * Both play the same success SFX as every retail cheat, so the feedback is
 * indistinguishable from a stock code.
 */
void MM_Cheat_PrideFlag(void)
{
	extern int g_dbg_flagStyle;

	g_dbg_flagStyle = (g_dbg_flagStyle == 1) ? 0 : 1;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

void MM_Cheat_SpyroFlag(void)
{
	extern int g_dbg_flagStyle;

	g_dbg_flagStyle = (g_dbg_flagStyle == 2) ? 0 : 2;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

/*
 * ROSWELL: Nitros Oxide joins the character-select grid (MM_Characters.c). Native-only, so it is
 * matched in MM_ParseCheatCodes next to WONDER rather than added to D230.cheats.
 */
void MM_Cheat_UnlockOxide(void)
{
	UNLOCK_ADV_BIT(sdata->gameProgress.unlocks, GAME_UNLOCK_BIT_OXIDE);
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}
#endif

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800ac9fc-0x800aca34.
void MM_Cheat_MaxWumpa(void)
{
	sdata->gGT->gameMode2 |= CHEAT_WUMPA;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800aca34-0x800aca6c.
void MM_Cheat_UnlockRoo(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_ROO;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800aca6c-0x800acaa4.
void MM_Cheat_UnlockPapu(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_PAPU;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acaa4-0x800acadc.
void MM_Cheat_UnlockJoe(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_JOE;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acadc-0x800acb14.
void MM_Cheat_UnlockPinstripe(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_PINSTRIPE;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acb14-0x800acb4c.
void MM_Cheat_UnlockFakeCrash(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_FAKE_CRASH;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acb4c-0x800acb84.
void MM_Cheat_UnlockPenta(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_PENTA;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acb84-0x800acbbc.
void MM_Cheat_UnlockTropy(void)
{
	sdata->gameProgress.unlockFlags |= UNLOCK_TROPY;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acbbc-0x800acbf4.
void MM_Cheat_UnlockScrapbook(void)
{
	UNLOCK_ADV_BIT(sdata->gameProgress.unlocks, GAME_UNLOCK_BIT_SCRAPBOOK);
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acbf4-0x800acc2c.
void MM_Cheat_UnlockTracks(void)
{
	sdata->gameProgress.unlockFlags |= GAME_UNLOCK_TRACKS_MASK;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acc2c-0x800acc64.
void MM_Cheat_InfiniteMasks(void)
{
	sdata->gGT->gameMode2 |= CHEAT_MASK;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acc64-0x800acc9c.
void MM_Cheat_MaxTurbos(void)
{
	sdata->gGT->gameMode2 |= CHEAT_TURBO;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acc9c-0x800accd4.
void MM_Cheat_MaxInvisibility(void)
{
	sdata->gGT->gameMode2 |= CHEAT_INVISIBLE;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800accd4-0x800acd10.
void MM_Cheat_MaxEngine(void)
{
	sdata->gGT->gameMode2 |= CHEAT_ENGINE;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acd10-0x800acd4c.
void MM_Cheat_MaxBombs(void)
{
	sdata->gGT->gameMode2 |= CHEAT_BOMBS;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acd4c-0x800acd88.
void MM_Cheat_AdvDifficulty(void)
{
	sdata->gGT->gameMode2 |= CHEAT_ADV;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acd88-0x800acdc4.
void MM_Cheat_SuperHard(void)
{
	sdata->gGT->gameMode2 |= CHEAT_SUPERHARD;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800acdc4-0x800ace00.
void MM_Cheat_IcyTracks(void)
{
	sdata->gGT->gameMode2 |= CHEAT_ICY;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800ace00-0x800ace3c.
void MM_Cheat_SuperTurboPads(void)
{
	sdata->gGT->gameMode2 |= CHEAT_TURBOPAD;

#ifdef CTR_NATIVE
	// Retro-Fueled: the cheat also turns the menu flag blue (flag style 3). The last flag cheat entered wins.
	if (g_retroFueled)
	{
		extern int g_dbg_flagStyle;

		g_dbg_flagStyle = 3;
	}
#endif

	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800ace3c-0x800ace78.
void MM_Cheat_OneLap(void)
{
	sdata->gGT->gameMode2 |= CHEAT_ONELAP;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800ace78-0x800aceb4.
void MM_Cheat_TurboCounter(void)
{
	sdata->gGT->gameMode2 |= CHEAT_TURBOCOUNT;
	OtherFX_Play(MM_CHEAT_SUCCESS_SFX, 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800aceb4-0x800acff4.
void MM_ParseCheatCodes(void)
{
	struct GamepadBuffer *gpad = &sdata->gGamepads->gamepad[0];

	// if not holding L1 and R1
	if ((gpad->buttonsHeldCurrFrame & (BTN_L1 | BTN_R1)) != (BTN_L1 | BTN_R1))
	{
		// skip function
		return;
	}

	u32 tap = (u32)gpad->buttonsTapped;
	if (tap == 0)
	{
		return;
	}

	// at this point, must be holding L1 and R1,
	// and also must have tapped a buttons

	// shift the loop
	for (s32 historyIndex = MM_CHEAT_BUTTON_HISTORY_COUNT - 1; historyIndex > 0; historyIndex--)
	{
		D230.cheatButtonHistory[historyIndex] = D230.cheatButtonHistory[historyIndex - 1];
	}

	// add to input
	D230.cheatButtonHistory[0] = tap;

	// loop through all cheats
	for (s32 cheatIndex = 0; cheatIndex < MM_CHEAT_COUNT; cheatIndex++)
	{
		b32 cheatMatches = true;

		// check if buttons match this cheat
		for (s32 buttonIndex = 0; buttonIndex < D230.cheats[cheatIndex].buttonCount; buttonIndex++)
		{
			// remember, inputButtons is backward
			s32 expectedButtonIndex = D230.cheats[cheatIndex].buttonCount - buttonIndex - 1;
			if ((D230.cheatButtonHistory[buttonIndex] & D230.cheats[cheatIndex].buttons[expectedButtonIndex]) == 0)
			{
				cheatMatches = false;
				break;
			}
		}

		// skip to next cheat if needed
		if (!cheatMatches)
		{
			continue;
		}

		if (D230.cheats[cheatIndex].handler != NULL)
		{
			D230.cheats[cheatIndex].handler();
		}
	}

#if defined(CTR_NATIVE)
	/*
	 * NATIVE-ONLY CHEATS, matched here rather than added to D230.cheats.
	 *
	 * Not a stylistic choice: that array is sized `cheats[MM_CHEAT_COUNT]`
	 * inside OverlayDATA_230, it is already exactly full (22 rows against
	 * MM_CHEAT_COUNT 0x16), and SIX offsetof static asserts guard fields
	 * declared AFTER it (videoCol, cupSel_Color, battleWeaponEnabledColor,
	 * highscore_iconColor, characterSelectPlayerState,
	 * characterSelectTransitionFrame). Growing the array by one 48-byte row
	 * shifts every one of them and breaks the build. Matching alongside costs
	 * a few compares on frames where a cheat button was tapped at all.
	 *
	 * Same matching rule as the retail loop above: cheatButtonHistory is
	 * newest-first, so the table is indexed backwards.
	 *
	 * WONDER  = Left(W) Circle(O) Up(N) Down(D) Right(E) Right(R)
	 * ROSWELL = Right(R) Circle(O) Down(S) Left(W) Right(E) Left(L) Left(L)
	 * The letter scheme is retail's own (game/230/D230.c's CHEAT_* defines); spelled
	 * with BTN_* here because those defines are local to that file. A code must not
	 * end with another code's buttons (here or in D230.cheats), or one tap fires both.
	 */
	{
		static const u32 nativeCheatWonder[6] = {BTN_LEFT, BTN_CIRCLE, BTN_UP, BTN_DOWN, BTN_RIGHT, BTN_RIGHT};
		static const u32 nativeCheatRoswell[7] = {BTN_RIGHT, BTN_CIRCLE, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_LEFT, BTN_LEFT};
		static const struct
		{
			const u32 *buttons;
			s32 buttonCount;
			void (*handler)(void);
		} nativeCheats[2] = {
		    {nativeCheatWonder, 6, MM_Cheat_PrideFlag},
		    {nativeCheatRoswell, 7, MM_Cheat_UnlockOxide},
		};

		for (s32 nativeIndex = 0; nativeIndex < 2; nativeIndex++)
		{
			b32 nativeMatches = true;

			for (s32 buttonIndex = 0; buttonIndex < nativeCheats[nativeIndex].buttonCount; buttonIndex++)
			{
				if ((D230.cheatButtonHistory[buttonIndex] & nativeCheats[nativeIndex].buttons[nativeCheats[nativeIndex].buttonCount - buttonIndex - 1]) == 0)
				{
					nativeMatches = false;
					break;
				}
			}

			if (nativeMatches)
			{
				nativeCheats[nativeIndex].handler();
			}
		}
	}
#endif

	return;
}
