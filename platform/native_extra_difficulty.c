/* The Arcade cup ladder and the SUPER HARD and ULTRA HARD difficulties (3DS only); the contract is in
 * include/native_extra_difficulty.h.
 *
 * The row labels are "SUPER " and "ULTRA " before the loaded language's own HARD (LNG_HARD), so every
 * language keeps retail's word and its PAL spelling. They and the podium prompt live in native string
 * slots (include/native_lng.h). The labels are rebuilt from NativeXDiff_OnLanguage and reassigned each
 * frame the box is served. */

#include <common.h>
#include <native_extra_difficulty.h>
#include <native_lng.h>

#include <string.h>

CTR_STATIC_ASSERT(GAME_UNLOCK_BIT_EXTRA_CUP_WIN_FIRST > GAME_UNLOCK_BIT_OXIDE);
CTR_STATIC_ASSERT(GAME_UNLOCK_BIT_EXTRA_CUP_SAVED_FIRST == GAME_UNLOCK_BIT_EXTRA_CUP_WIN_FIRST + 2 * GAME_PROGRESS_CUP_COUNT);
CTR_STATIC_ASSERT(GAME_UNLOCK_BIT_EXTRA_CUP_SAVED_FIRST + 2 * GAME_PROGRESS_CUP_COUNT <= GAME_PROGRESS_UNLOCK_WORD_COUNT * 32);

#ifdef __3DS__

#define NXD_RETAIL_TIERS   GAME_PROGRESS_CUP_DIFFICULTY_COUNT /* EASY, MEDIUM, HARD */
#define NXD_TIER_SUPER     3                                  /* also the row index in the box */
#define NXD_TIER_ULTRA     4
#define NXD_TIER_COUNT     5
#define NXD_LANG_COUNT     6
#define NXD_LABEL_MAX      40
#define NXD_PROMPT_MAX     192
#define NXD_TIER_STEP      0x50 /* arcadeDifficulty per tier, EASY 0x50 to ULTRA HARD 0x190 */

/* Cup select stars once SUPER HARD is unlocked: five slots 12 px apart, centred on retail's middle star
 * (retail: three slots 16 px apart from +16). */
#define NXD_STAR_Y_OFFSET 8
#define NXD_STAR_Y_STEP   12

/* A podium unlock line is prefix + name + suffix, the name being a tier's label or the language's own
 * LNG_PENTA_PENGUIN, per language in LOAD_LangFile order 1 en, 3 fr, 4 de, 5 it, 6 es, 7 nl. Each line
 * must fit one big-font line, 27 characters (TakeCupProgress wraps at 460 px, 17 px a character), with
 * the disc's longest name: German and Spanish use short forms for that. Uppercase ASCII only; '#' is the
 * PAL font's inverted "!" (Spanish), drawn only while a PAL language file is loaded. */
static const char *const s_nxdUnlockPrefix[NXD_LANG_COUNT] = {"", "", "NEU: ", "", "#", ""};
static const char *const s_nxdUnlockSuffix[NXD_LANG_COUNT] = {
    " UNLOCKED!",    /* English */
    " DEBLOQUE !",   /* French */
    "!",             /* German */
    " SBLOCCATO!",   /* Italian */
    " DISPONIBLE!",  /* Spanish */
    " ONTGRENDELD!", /* Dutch */
};

/* Mirror mode's name in its unlock line. */
static const char *const s_nxdMirrorName[NXD_LANG_COUNT] = {"MIRROR MODE", "MODE MIROIR", "SPIEGELMODUS", "MODO SPECCHIO", "MODO ESPEJO", "SPIEGELMODUS"};

/* The arena line for a cup that opened more than one battle arena: the disc's own line (what follows the
 * blank line in LNG_SAVE_YOUR_CUP_PROGRESS_NEW_BATTLE_ARENA_OPENED) in the plural. French fills the line's
 * 27 characters and, like the disc's, has no "!"; Spanish takes two lines, as the disc's does. */
static const char *const s_nxdArenasOpened[NXD_LANG_COUNT] = {
    "NEW BATTLE ARENAS OPENED!",            /* English */
    "NOUVEAUX CHAMPS DE BATAILLE",          /* French */
    "NEUE KAMPFARENEN OFFEN!",              /* German */
    "NUOVE ARENE DI BATTAGLIA!",            /* Italian */
    "#NUEVOS CIRCUITOS DE PELEA ABIERTOS!", /* Spanish */
    "NIEUWE STRIJDARENA'S OPEN!",           /* Dutch */
};

/* The new tiers' star colours: SUPER HARD red, ULTRA HARD black. */
static const s16 s_nxdStarColor[NXD_TIER_COUNT - NXD_RETAIL_TIERS] = {CORTEX_RED, BLACK};

static s32 s_nxdLang = 0;
static char s_nxdLabel[NXD_TIER_COUNT - NXD_RETAIL_TIERS][NXD_LABEL_MAX];
static char s_nxdPrompt[NXD_PROMPT_MAX];

/* What the cup just won unlocked, for the podium prompt: a tier (NXD_TIER_SUPER / NXD_TIER_ULTRA) or 0,
 * whether Penta came with it, whether it was the fourth ULTRA HARD cup (mirror mode), and how many
 * battle arenas it opened. */
static s32 s_nxdUnlocked = 0;
static s32 s_nxdUnlockedPenta = 0;
static s32 s_nxdUnlockedMirror = 0;
static s32 s_nxdArenas = 0;

static struct MenuRow s_nxdRows4[] = {
    {LNG_EASY, 0, 1, 0, 0},
    {LNG_MEDIUM, 0, 2, 1, 1},
    {LNG_HARD, 1, 3, 2, 2},
    {NATIVE_LNG_SUPER_HARD, 2, 3, 3, 3},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static struct MenuRow s_nxdRows5[] = {
    {LNG_EASY, 0, 1, 0, 0},
    {LNG_MEDIUM, 0, 2, 1, 1},
    {LNG_HARD, 1, 3, 2, 2},
    {NATIVE_LNG_SUPER_HARD, 2, 4, 3, 3},
    {NATIVE_LNG_ULTRA_HARD, 3, 4, 4, 4},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

/* First of a tier's four cup-win bits (tier 0 EASY .. 4 ULTRA HARD). */
static s32 NativeXDiff_WinBit(s32 tier)
{
	if (tier < NXD_RETAIL_TIERS)
	{
		return GAME_PROGRESS_CUP_CURRENT_WIN_FIRST_BIT + tier * GAME_PROGRESS_CUP_COUNT;
	}

	return GAME_UNLOCK_BIT_EXTRA_CUP_WIN_FIRST + (tier - NXD_RETAIL_TIERS) * GAME_PROGRESS_CUP_COUNT;
}

/* First of a tier's four saved-win bits: the card's copy, which decides whether a win asks to save. */
static s32 NativeXDiff_SavedBit(s32 tier)
{
	if (tier < NXD_RETAIL_TIERS)
	{
		return NativeXDiff_WinBit(tier) + GAME_PROGRESS_CUP_PREVIOUS_WIN_OFFSET;
	}

	return GAME_UNLOCK_BIT_EXTRA_CUP_SAVED_FIRST + (tier - NXD_RETAIL_TIERS) * GAME_PROGRESS_CUP_COUNT;
}

/* All four cups of a tier won. */
static int NativeXDiff_TierComplete(s32 tier)
{
	for (s32 cup = 0; cup < GAME_PROGRESS_CUP_COUNT; cup++)
	{
		if (!CHECK_ADV_BIT(sdata->gameProgress.unlocks, NativeXDiff_WinBit(tier) + cup))
		{
			return 0;
		}
	}

	return 1;
}

/* Credits cupID's star on one tier. The tier the cup was won on (wonTier) follows retail: a win the card
 * has not saved asks to save, credited already or not. A tier below it is credited only when its star is
 * new, so news from an earlier unsaved win is not announced again. A tier's fourth cup opens what it
 * unlocks: each retail tier a battle arena, HARD also SUPER HARD; SUPER HARD opens ULTRA HARD and Penta;
 * ULTRA HARD mirror mode. */
static void NativeXDiff_CreditTier(s32 tier, s32 cupID, int wonTier)
{
	struct GameTracker *gGT = sdata->gGT;
	u32 *unlocks = sdata->gameProgress.unlocks;
	s32 winBit = NativeXDiff_WinBit(tier) + cupID;
	s32 newBit = wonTier ? (NativeXDiff_SavedBit(tier) + cupID) : winBit;

	if (CHECK_ADV_BIT(unlocks, newBit))
	{
		return;
	}

	gGT->gameMode2 |= CUP_NEW_WIN;
	UNLOCK_ADV_BIT(unlocks, winBit);

	if (!NativeXDiff_TierComplete(tier))
	{
		return;
	}

	if (tier < NXD_RETAIL_TIERS)
	{
		UNLOCK_ADV_BIT(unlocks, sdata->UnlockBitIndex.UnlockBattleMap[tier]);
		gGT->gameMode2 |= CUP_NEW_BATTLE;
		s_nxdArenas++;

		if (tier == NXD_RETAIL_TIERS - 1)
		{
			s_nxdUnlocked = NXD_TIER_SUPER;
		}
	}
	else if (tier == NXD_TIER_SUPER)
	{
		s_nxdUnlocked = NXD_TIER_ULTRA;

		/* saved with the cup progress, like every character unlock */
		if ((sdata->gameProgress.unlockFlags & UNLOCK_PENTA) == 0)
		{
			sdata->gameProgress.unlockFlags |= UNLOCK_PENTA;
			s_nxdUnlockedPenta = 1;
		}
	}
	else
	{
		s_nxdUnlockedMirror = 1;
	}
}

/* Rows the box has beyond retail's three: 0, 1 (SUPER HARD) or 2 (and ULTRA HARD). */
static s32 NativeXDiff_ExtraRows(void)
{
	if (!NativeXDiff_TierComplete(NXD_RETAIL_TIERS - 1))
	{
		return 0;
	}

	return NativeXDiff_TierComplete(NXD_TIER_SUPER) ? 2 : 1;
}

static void NativeXDiff_Append(char *dst, s32 cap, const char *src)
{
	s32 len = (s32)strlen(dst);

	while ((*src != 0) && (len < cap - 1))
	{
		dst[len++] = *src++;
	}

	dst[len] = 0;
}

/* One podium unlock line: prefix + name + suffix in the loaded language. */
static void NativeXDiff_AppendUnlockLine(const char *name)
{
	NativeXDiff_Append(s_nxdPrompt, NXD_PROMPT_MAX, s_nxdUnlockPrefix[s_nxdLang]);
	NativeXDiff_Append(s_nxdPrompt, NXD_PROMPT_MAX, name);
	NativeXDiff_Append(s_nxdPrompt, NXD_PROMPT_MAX, s_nxdUnlockSuffix[s_nxdLang]);
}

/* Starts the podium prompt's next news line: the first below retail's question after a blank line, or
 * straight below it when there are two or more; each later one straight below the one before. */
static void NativeXDiff_NewsBreak(s32 *newsDone, s32 newsLines)
{
	NativeXDiff_Append(s_nxdPrompt, NXD_PROMPT_MAX, ((*newsDone > 0) || (newsLines >= 2)) ? "\r" : "\r\r");
	(*newsDone)++;
}

static void NativeXDiff_InstallLabels(void)
{
	if (!NativeLng_Ready())
	{
		return;
	}

	sdata->lngStrings[NATIVE_LNG_SUPER_HARD] = s_nxdLabel[NXD_TIER_SUPER - NXD_RETAIL_TIERS];
	sdata->lngStrings[NATIVE_LNG_ULTRA_HARD] = s_nxdLabel[NXD_TIER_ULTRA - NXD_RETAIL_TIERS];
}

static void NativeXDiff_ClampRow(struct RectMenu *menu, s32 rowCount)
{
	if (menu->rowSelected >= rowCount)
	{
		menu->rowSelected = (s16)(rowCount - 1);
	}
}

#endif

void NativeXDiff_ServeDifficultyBox(struct RectMenu *menu)
{
#ifdef __3DS__
	s32 extra = NativeXDiff_ExtraRows();
	struct MenuRow *rows = (extra == 2) ? &s_nxdRows5[0] : &s_nxdRows4[0];

	if (extra == 0)
	{
		menu->rows = &D230.rowsDifficulty[0];
		menu->state &= ~(USE_SMALL_FONT | BIG_TEXT_IN_TITLE);
		NativeXDiff_ClampRow(menu, NXD_RETAIL_TIERS);
		return;
	}

	/* the retail rows' labels as MM_ToggleRows_Difficulty left them this frame */
	for (s32 i = 0; i < NXD_RETAIL_TIERS; i++)
	{
		rows[i].stringIndex = D230.rowsDifficulty[i].stringIndex;
	}

	NativeXDiff_InstallLabels();

	menu->rows = rows;
	menu->state |= USE_SMALL_FONT | BIG_TEXT_IN_TITLE;
	NativeXDiff_ClampRow(menu, NXD_RETAIL_TIERS + extra);
#else
	(void)menu;
#endif
}

int NativeXDiff_HandleDifficultyRow(struct RectMenu *menu, s16 row)
{
#ifdef __3DS__
	if (((menu->rows != &s_nxdRows4[0]) && (menu->rows != &s_nxdRows5[0])) || (row < NXD_RETAIL_TIERS))
	{
		return 0;
	}

	sdata->gGT->arcadeDifficulty = (row == NXD_TIER_SUPER) ? NATIVE_XDIFF_SUPER_HARD : NATIVE_XDIFF_ULTRA_HARD;

	/* on to character select, as MM_MenuProc_Difficulty leaves a retail row */
	D230.titleMenuState = TITLE_MENU_STATE_EXITING;
	D230.desiredMenuIndex = MM_EXIT_ROUTE_CHARACTER_SELECT;
	menu->state |= ONLY_DRAW_TITLE;
	return 1;
#else
	(void)menu;
	(void)row;
	return 0;
#endif
}

int NativeXDiff_LockCupTiers(void)
{
#ifdef __3DS__
	return 0;
#else
	return 1;
#endif
}

int NativeXDiff_CreditCupWin(int arcadeDifficulty, int cupID)
{
#ifdef __3DS__
	/* retail's own tier formula, carried past HARD */
	s32 won = arcadeDifficulty / NXD_TIER_STEP - 1;

	if (won < 0)
	{
		won = 0;
	}
	if (won > NXD_TIER_COUNT - 1)
	{
		won = NXD_TIER_COUNT - 1;
	}

	s_nxdUnlocked = 0;
	s_nxdUnlockedPenta = 0;
	s_nxdUnlockedMirror = 0;
	s_nxdArenas = 0;

	/* lowest first, so a tier completed on the way names the highest unlock last */
	for (s32 tier = 0; tier <= won; tier++)
	{
		NativeXDiff_CreditTier(tier, cupID, tier == won);
	}

	return 1;
#else
	(void)arcadeDifficulty;
	(void)cupID;
	return 0;
#endif
}

s16 NativeXDiff_CupProgressString(s16 stringIndex)
{
#ifdef __3DS__
	s32 tier = s_nxdUnlocked;
	s32 penta = s_nxdUnlockedPenta;
	s32 mirror = s_nxdUnlockedMirror;
	s32 arenas = s_nxdArenas;
	const char *arena = NULL;

	s_nxdUnlocked = 0;
	s_nxdUnlockedPenta = 0;
	s_nxdUnlockedMirror = 0;
	s_nxdArenas = 0;

	if (((tier == 0) && !mirror && (arenas < 2)) || !NativeLng_Ready())
	{
		return stringIndex;
	}

	/* the arena line: retail's (what follows the blank line in its question-plus-arena string), or its
	 * plural when this cup opened more than one arena */
	if (stringIndex == LNG_SAVE_YOUR_CUP_PROGRESS_NEW_BATTLE_ARENA_OPENED)
	{
		if (arenas >= 2)
		{
			arena = s_nxdArenasOpened[s_nxdLang];
		}
		else
		{
			arena = strstr(sdata->lngStrings[stringIndex], "\r\r");
			if (arena == NULL)
			{
				return stringIndex;
			}
			arena += 2;
		}
	}

	/* retail's question, a blank line, then the news a line each. Two news lines drop the blank line: the
	 * Spanish question and arena line take two lines each, and the save box starts at line 146. */
	s32 newsLines = (arena != NULL) + (tier != 0) + penta + mirror;
	s32 newsDone = 0;

	s_nxdPrompt[0] = 0;
	NativeXDiff_Append(s_nxdPrompt, NXD_PROMPT_MAX, sdata->lngStrings[LNG_SAVE_YOUR_CUP_PROGRESS]);

	if (arena != NULL)
	{
		NativeXDiff_NewsBreak(&newsDone, newsLines);
		NativeXDiff_Append(s_nxdPrompt, NXD_PROMPT_MAX, arena);
	}

	if (tier != 0)
	{
		NativeXDiff_NewsBreak(&newsDone, newsLines);
		NativeXDiff_AppendUnlockLine(s_nxdLabel[tier - NXD_RETAIL_TIERS]);
	}

	if (penta)
	{
		NativeXDiff_NewsBreak(&newsDone, newsLines);
		NativeXDiff_AppendUnlockLine(sdata->lngStrings[LNG_PENTA_PENGUIN]);
	}

	if (mirror)
	{
		NativeXDiff_NewsBreak(&newsDone, newsLines);
		NativeXDiff_AppendUnlockLine(s_nxdMirrorName[s_nxdLang]);
	}

	sdata->lngStrings[NATIVE_LNG_CUP_PROMPT] = s_nxdPrompt;
	return NATIVE_LNG_CUP_PROMPT;
#else
	return stringIndex;
#endif
}

void NativeXDiff_SaveCupProgress(u32 *unlocks)
{
#ifdef __3DS__
	for (s32 tier = NXD_TIER_SUPER; tier < NXD_TIER_COUNT; tier++)
	{
		for (s32 cup = 0; cup < GAME_PROGRESS_CUP_COUNT; cup++)
		{
			if (CHECK_ADV_BIT(unlocks, NativeXDiff_WinBit(tier) + cup))
			{
				UNLOCK_ADV_BIT(unlocks, NativeXDiff_SavedBit(tier) + cup);
			}
		}
	}
#else
	(void)unlocks;
#endif
}

int NativeXDiff_DrawCupStars(int cupIndex, int starX, int startY)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;

	if (NativeXDiff_ExtraRows() == 0)
	{
		return 0;
	}

	for (s32 tier = 0; tier < NXD_TIER_COUNT; tier++)
	{
		if (!CHECK_ADV_BIT(sdata->gameProgress.unlocks, NativeXDiff_WinBit(tier) + cupIndex))
		{
			continue;
		}

		s16 colorIndex = (tier < NXD_RETAIL_TIERS) ? D230.cupSelectStars.colorIndex[tier] : s_nxdStarColor[tier - NXD_RETAIL_TIERS];
		u32 *starColor = data.ptrColor[colorIndex];
		struct Icon **iconPtrArray = ICONGROUP_GETICONS(gGT->iconGroup[MM_CUP_SELECT_STAR_ICON_GROUP]);

		DecalHUD_DrawPolyGT4(iconPtrArray[MM_CUP_SELECT_STAR_ICON_ID], starX, startY + NXD_STAR_Y_OFFSET + tier * NXD_STAR_Y_STEP, &gGT->backBuffer->primMem,
		                     gGT->pushBuffer_UI.ptrOT, starColor[0], starColor[1], starColor[2], starColor[3], 0, FP(1.0));
	}

	return 1;
#else
	(void)cupIndex;
	(void)starX;
	(void)startY;
	return 0;
#endif
}

void NativeXDiff_OnLanguage(int langFileIndex)
{
#ifdef __3DS__
	static const s8 fileToIndex[8] = {-1, 0, -1, 1, 2, 3, 4, 5};
	const char *hard = "HARD";

	s_nxdLang = 0;

	if ((langFileIndex >= 0) && (langFileIndex < 8) && (fileToIndex[langFileIndex] >= 0))
	{
		s_nxdLang = fileToIndex[langFileIndex];
	}

	if ((sdata->lngStrings != NULL) && (sdata->numLngStrings > LNG_HARD) && (sdata->lngStrings[LNG_HARD] != NULL))
	{
		hard = sdata->lngStrings[LNG_HARD];
	}

	s_nxdLabel[NXD_TIER_SUPER - NXD_RETAIL_TIERS][0] = 0;
	NativeXDiff_Append(s_nxdLabel[NXD_TIER_SUPER - NXD_RETAIL_TIERS], NXD_LABEL_MAX, "SUPER ");
	NativeXDiff_Append(s_nxdLabel[NXD_TIER_SUPER - NXD_RETAIL_TIERS], NXD_LABEL_MAX, hard);

	s_nxdLabel[NXD_TIER_ULTRA - NXD_RETAIL_TIERS][0] = 0;
	NativeXDiff_Append(s_nxdLabel[NXD_TIER_ULTRA - NXD_RETAIL_TIERS], NXD_LABEL_MAX, "ULTRA ");
	NativeXDiff_Append(s_nxdLabel[NXD_TIER_ULTRA - NXD_RETAIL_TIERS], NXD_LABEL_MAX, hard);

	NativeXDiff_InstallLabels();
#else
	(void)langFileIndex;
#endif
}

int NativeXDiff_UltraHardComplete(void)
{
#ifdef __3DS__
	return NativeXDiff_TierComplete(NXD_TIER_ULTRA);
#else
	return 0;
#endif
}
