/* Arcade-menu modes (3DS only); the contract is in include/native_arcade_modes.h.
 *
 * The ARCADE RACE TYPE menu (D230.menuRaceType) keeps retail's two rows, SINGLE and CUP, and gains
 * one row per mode below them. The row set is swapped in by NativeArcade_RaceTypeRows every frame
 * from MM_MenuProc_Main, the same way NativeOptions_MainMenuRows swaps the main-menu rows, and the
 * row choice is taken in NativeArcade_HandleRaceTypeRow before retail's SingleCup handler sees it.
 * On PC every hook is a passthrough: replays navigate these menus by row position, so the PC row
 * layout is behavioral surface.
 *
 * BOSS RACES has no retail string. Its label is written into LNG_UNUSED_0F4, a slot of the loaded
 * string table that no retail code and no other native menu reads. Every language load rebuilds
 * the table, so the text is reassigned from NativeArcade_OnLanguage and again each frame the rows
 * are served. */

#include <common.h>
#include <native_arcade_modes.h>

int g_nativeArcadeMode = NATIVE_ARCADE_NONE;

/* The relic bits a RELIC RACES race reads and sets in place of advProgress.rewards (NativeRelic_Prepare). */
u32 g_nativeRelicRewards[NATIVE_RELIC_REWARD_WORDS];

#ifdef __3DS__

#define NAM_LNG_BOSS_RACES LNG_UNUSED_0F4
#define NAM_LANG_COUNT     6
#define NAM_RETAIL_ROWS    2 /* SINGLE, CUP */

/* The bosses' hub numbers are metaDataLEV[].hubID with Oxide Station forced to 0, as PickupBots_Init
 * does. Hub 0 (Oxide, and the two bonus tracks) through hub 4 index R232.bossCharacterIDs and
 * R232.bossRaceLevelIDs. */
#define NAM_HUB_COUNT 5

/* LOAD_LangFile file indices 1 en, 3 fr, 4 de, 5 it, 6 es, 7 nl, in this order. Uppercase ASCII
 * only, at most 13 characters (the widest retail row label, CTR CHALLENGE, is 13). */
static const char *const s_namBossRaces[NAM_LANG_COUNT] = {
    "BOSS RACES",   /* English */
    "COURSES BOSS", /* French */
    "BOSS-RENNEN",  /* German */
    "GARE BOSS",    /* Italian */
    "CARRERA JEFE", /* Spanish */
    "BOSSRACES",    /* Dutch */
};

static s32 s_namLang = 0;

static struct MenuRow s_namRaceTypeRows[] = {
    {LNG_SINGLE, 0, 1, 0, 0},
    {LNG_CUP, 0, 2, 1, 1},
    {NAM_LNG_BOSS_RACES, 1, 3, 2, 2},
    {LNG_CTR_CHALLENGE_TITLE, 2, 4, 3, 3},
    {LNG_RELIC_RACE, 3, 4, 4, 4},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static void NativeArcade_InstallStrings(void)
{
	if ((sdata->lngStrings == NULL) || (sdata->numLngStrings <= NAM_LNG_BOSS_RACES))
	{
		return;
	}

	sdata->lngStrings[NAM_LNG_BOSS_RACES] = (char *)s_namBossRaces[s_namLang];
}

/* Hub of a race track as the boss tables number it. */
static s32 NativeArcade_HubOf(s32 levelID)
{
	s32 hub = data.metaDataLEV[levelID].hubID;

	if (levelID == OXIDE_STATION)
	{
		hub = 0;
	}

	if ((hub < 0) || (hub >= NAM_HUB_COUNT))
	{
		hub = 0;
	}

	return hub;
}

#define NAM_RELIC_TIERS      3                                       /* sapphire, gold, platinum */
#define NAM_RELIC_BEST_ENTRY (MEMCARD_HIGH_SCORE_ENTRIES_PER_MODE + 1) /* the relic half of a track's high scores: entry 0 the best lap, 1-5 the race times */

CTR_STATIC_ASSERT(NATIVE_RELIC_REWARD_WORDS * 32 > ADV_REWARD_FIRST_PLATINUM_RELIC + MEMCARD_HIGH_SCORE_TRACK_COUNT);

/* The relic bits of every track from the best time in the memory card's relic high scores (the table Adventure relic
 * races record into): a relic is owned when the best time is at or under its RelicTime. Run when a RELIC RACES race is
 * queued, so the HUD goal and the award test read the card as it is then. */
static void NativeRelic_Prepare(void)
{
	for (s32 level = 0; level < MEMCARD_HIGH_SCORE_TRACK_COUNT; level++)
	{
		u32 bestTime = sdata->gameProgress.highScoreTracks[level].scoreEntry[NAM_RELIC_BEST_ENTRY].time;

		for (s32 tier = 0; tier < NAM_RELIC_TIERS; tier++)
		{
			s32 bit = ADV_REWARD_FIRST_SAPPHIRE_RELIC + tier * ADV_REWARD_RELIC_TIER_STRIDE + level;

			if (bestTime <= (u32)data.RelicTime[level * NAM_RELIC_TIERS + tier])
			{
				UNLOCK_ADV_BIT(g_nativeRelicRewards, bit);
			}
			else
			{
				g_nativeRelicRewards[MEMCARD_BIT_WORD(bit)] &= ~MEMCARD_BIT_MASK(bit);
			}
		}
	}
}

#endif /* __3DS__ */

int NativeRelic_SkipAward(void)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;

	/* Adventure's own relic races are not ours to restrict */
	if (!NativeArcade_RelicRace() || ((gGT->gameMode2 & NATIVE_GHOST_BEST_BLOCKING_CHEATS) == 0))
	{
		return 0;
	}

	/* the finish frame already flagged a record against the card's table: take the banners back */
	gGT->newHighScoreIndex = -1;
	gGT->gameModeEnd &= ~(NEW_HIGH_SCORE | NEW_BEST_LAP);
	return 1;
#else
	return 0;
#endif
}

int NativeArcade_BossOffHome(void)
{
#ifdef __3DS__
	s32 levelID = sdata->gGT->levelID;

	return NativeArcade_BossRace() && (levelID != R232.bossRaceLevelIDs[NativeArcade_HubOf(levelID)]);
#else
	return 0;
#endif
}

struct MenuRow *NativeArcade_RaceTypeRows(struct MenuRow *rows)
{
#ifdef __3DS__
	(void)rows;
	NativeArcade_InstallStrings();
	return &s_namRaceTypeRows[0];
#else
	return rows;
#endif
}

s32 NativeArcade_HandleRaceTypeRow(struct RectMenu *menu, s32 row)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;
	s16 choose;

	if ((menu->rows != &s_namRaceTypeRows[0]) || (row < 0))
	{
		return 0;
	}

	/* SINGLE and CUP are retail's: they leave any mode behind and fall through to its handler */
	if (row < NAM_RETAIL_ROWS)
	{
		g_nativeArcadeMode = NATIVE_ARCADE_NONE;
		return 0;
	}

	choose = (s16)(menu->rows[row].stringIndex & MENU_ROW_LNG_MASK);

	/* a single race, one player, as SingleCup's row 0 and the Players box leave it */
	gGT->gameMode2 &= ~(CUP_ANY_KIND);
	gGT->numPlyrNextGame = 1;

	if ((choose == NAM_LNG_BOSS_RACES) || (choose == LNG_RELIC_RACE))
	{
		g_nativeArcadeMode = (choose == LNG_RELIC_RACE) ? NATIVE_ARCADE_RELIC : NATIVE_ARCADE_BOSS;

		/* no difficulty: a boss race uses the boss's own and a relic race has no opponents */
		D230.titleMenuState = TITLE_MENU_STATE_EXITING;
		D230.desiredMenuIndex = MM_EXIT_ROUTE_CHARACTER_SELECT;
		menu->state |= ONLY_DRAW_TITLE;
		return 1;
	}

	if (choose == LNG_CTR_CHALLENGE_TITLE)
	{
		g_nativeArcadeMode = NATIVE_ARCADE_CTR;

		/* an Arcade race with the AI of the chosen difficulty: on to the difficulty box */
		menu->ptrNextBox_InHierarchy = &D230.menuDifficulty;
		menu->state |= ONLY_DRAW_TITLE | DRAW_NEXT_MENU_IN_HIERARCHY;
		return 1;
	}

	return 0;
#else
	(void)menu;
	(void)row;
	return 0;
#endif
}

s32 NativeArcade_TrackAllowed(s16 levID)
{
#ifdef __3DS__
	/* the 16 tracks of the hubs carry the C-T-R letters; Slide Coliseum and Turbo Track have none */
	if (g_nativeArcadeMode == NATIVE_ARCADE_CTR)
	{
		return levID < SLIDE_COLISEUM;
	}
#endif

	(void)levID;
	return 1;
}

s32 NativeArcade_FixedLaps(void)
{
#ifdef __3DS__
	return g_nativeArcadeMode != NATIVE_ARCADE_NONE;
#else
	return 0;
#endif
}

void NativeArcade_ResetMode(void)
{
	g_nativeArcadeMode = NATIVE_ARCADE_NONE;
}

void NativeArcade_ArmRace(void)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;

	if (g_nativeArcadeMode == NATIVE_ARCADE_NONE)
	{
		return;
	}

	/* three laps, as the menu leaves them; the lap box is skipped for these modes */
	gGT->numLaps = ((gGT->gameMode2 & CHEAT_ONELAP) != 0) ? MM_ONE_LAP_CHEAT_COUNT : MM_DEFAULT_LAP_COUNT;

	if (g_nativeArcadeMode == NATIVE_ARCADE_BOSS)
	{
		s32 hub = NativeArcade_HubOf(gGT->currLEV);

		/* the same boss and boss character the Adventure garage of this hub starts (AH_Garage.c) */
		gGT->bossID = R232.bossCharacterIDs[hub];
		data.characterIDs[1] = data.metaDataLEV[R232.bossRaceLevelIDs[hub]].characterID_Boss;
		sdata->Loading.OnBegin.AddBitsConfig0 |= ADVENTURE_BOSS;
	}
	else if (g_nativeArcadeMode == NATIVE_ARCADE_CTR)
	{
		sdata->Loading.OnBegin.AddBitsConfig8 |= TOKEN_RACE;
	}
	else if (g_nativeArcadeMode == NATIVE_ARCADE_RELIC)
	{
		/* The relic bits derived from the card's relic high scores have to be in place before the race's HUD
		 * is built. ARCADE_MODE goes for the race: the end-of-race overlay and results screen are chosen
		 * ARCADE_MODE first, which would hide the relic results (retail relic races never carry it). */
		NativeRelic_Prepare();
		sdata->Loading.OnBegin.AddBitsConfig0 |= RELIC_RACE;
		sdata->Loading.OnBegin.RemBitsConfig0 |= ARCADE_MODE;
	}
#endif
}

void NativeArcade_OnLoadRequest(s16 levelID)
{
#ifdef __3DS__
	if ((g_nativeArcadeMode == NATIVE_ARCADE_NONE) || (levelID != MAIN_MENU_LEVEL))
	{
		return;
	}

	/* The menus the race returns to run without the race's flags. The mode stays: CHANGE LEVEL and
	 * CHANGE CHARACTER lead back to a race of the same kind, and QueueLoadTrack arms it again. */
	sdata->Loading.OnBegin.RemBitsConfig0 |= ADVENTURE_BOSS | RELIC_RACE;
	sdata->Loading.OnBegin.RemBitsConfig8 |= TOKEN_RACE;

	if (g_nativeArcadeMode == NATIVE_ARCADE_RELIC)
	{
		/* the menus are Arcade menus again */
		sdata->Loading.OnBegin.AddBitsConfig0 |= ARCADE_MODE;
	}
#else
	(void)levelID;
#endif
}

void NativeArcade_OnLanguage(int langFileIndex)
{
#ifdef __3DS__
	static const s8 fileToIndex[8] = {-1, 0, -1, 1, 2, 3, 4, 5};

	s_namLang = 0;

	if ((langFileIndex >= 0) && (langFileIndex < 8) && (fileToIndex[langFileIndex] >= 0))
	{
		s_namLang = fileToIndex[langFileIndex];
	}

	NativeArcade_InstallStrings();
#else
	(void)langFileIndex;
#endif
}
