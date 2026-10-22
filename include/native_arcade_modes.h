#ifndef NATIVE_ARCADE_MODES_H
#define NATIVE_ARCADE_MODES_H

/* Arcade-menu modes (3DS only): extra rows of the ARCADE RACE TYPE menu, each a retail event shape run
 * without its Adventure side.
 *
 *   BOSS RACES    ARCADE_MODE|ADVENTURE_BOSS, the boss of the track's hub (Ripper Roo, Papu Papu,
 *                 Komodo Joe, Pinstripe; Oxide for Oxide Station and the two bonus tracks),
 *                 1 player + 1 boss, 3 laps
 *   CTR CHALLENGE ARCADE_MODE plus gameMode2 TOKEN_RACE: the C-T-R letters are on the track and a
 *                 first place finish with all three is a win; 3 laps. No token is awarded.
 *   RELIC RACES   RELIC_RACE with ARCADE_MODE taken off for the race (the results screen and
 *                 overlay choice test ARCADE_MODE first), 3 laps. The best times are the memory
 *                 card's relic high scores (the table Adventure relic races record into and the
 *                 main menu's HIGH SCORE shows), ranked, named and saved by retail's own new-high-
 *                 score flow. The relics are a bit view derived from them (g_nativeRelicRewards);
 *                 Adventure's advProgress.rewards is never read or set.
 *
 * g_nativeArcadeMode is the single switch. Only the 3DS menu rows set it, so on PC it stays 0 and
 * every site below runs the retail path. The retail flags are armed when the race load is queued
 * (NativeArcade_ArmRace) and removed when a load of the main-menu level is queued
 * (NativeArcade_OnLoadRequest); the mode itself survives CHANGE LEVEL and CHANGE CHARACTER and is
 * cleared whenever the top-level main menu takes a choice or comes up (NativeArcade_ResetMode).
 * No mode writes Adventure progress (advProgress) or an unlock: sites that would are guarded by
 * the predicates here. RELIC RACES alone records, into the card-wide relic high scores. Defined in
 * platform/native_arcade_modes.c. */

enum NativeArcadeMode
{
	NATIVE_ARCADE_NONE = 0,
	NATIVE_ARCADE_BOSS = 1,
	NATIVE_ARCADE_CTR = 2,
	NATIVE_ARCADE_RELIC = 3,
};

/* Words of the relic reward-bit view: bits ADV_REWARD_FIRST_SAPPHIRE_RELIC .. +3 tiers * 18 tracks. */
#define NATIVE_RELIC_REWARD_WORDS 3

#ifdef CTR_NATIVE

extern int g_nativeArcadeMode;

/* True while a BOSS RACES race is running (mode set and the retail boss flag armed). */
static inline int NativeArcade_BossRace(void)
{
	return (g_nativeArcadeMode == NATIVE_ARCADE_BOSS) && ((sdata->gGT->gameMode1 & ADVENTURE_BOSS) != 0);
}

/* True while a CTR CHALLENGE race is running (mode set and the retail token-race flag armed). */
static inline int NativeArcade_CtrChallenge(void)
{
	return (g_nativeArcadeMode == NATIVE_ARCADE_CTR) && ((sdata->gGT->gameMode2 & TOKEN_RACE) != 0);
}

/* True while a RELIC RACES race is running (mode set and the retail relic flag armed). */
static inline int NativeArcade_RelicRace(void)
{
	return (g_nativeArcadeMode == NATIVE_ARCADE_RELIC) && ((sdata->gGT->gameMode1 & RELIC_RACE) != 0);
}

extern u32 g_nativeRelicRewards[NATIVE_RELIC_REWARD_WORDS];

/* The reward bits the relic code reads and sets: in a RELIC RACES race the relics derived from the memory
 * card's relic high scores (set at each race load), Adventure's otherwise. */
static inline u32 *NativeArcade_RelicRewards(void)
{
	return NativeArcade_RelicRace() ? g_nativeRelicRewards : sdata->advProgress.rewards;
}

/* The highest relic earned on a track by the memory card's relic high scores (the table Adventure and
 * RELIC RACES both record into): 0 none, 1 sapphire, 2 gold, 3 platinum. 0 for a level that is not one
 * of the 18 race tracks, and always 0 on PC. */
int NativeRelic_TierOfTrack(s32 levID);

/* Track select (game/230/MM_TrackSelect.c) in a RELIC RACES run: a spinning relic beside each track row
 * that has one, tinted with the track's highest tier. Every hook is a no-op outside that run and on PC. */
void NativeRelic_TrackIconsInit(void);                                  /* menu entry: forgets the icons without freeing them */
void NativeRelic_TrackIconsBegin(void);                                 /* each frame, before the rows: hides every icon, advances the spin */
void NativeRelic_TrackIconsRow(s32 slot, s32 levID, const RECT16 *row); /* one visible row: shows its icon if the track has a relic */
void NativeRelic_TrackIconsEnd(void);                                   /* menu exit: frees the icons */

/* A cheat that can change a time (the best ghost's set) records nothing and awards no relic in a RELIC
 * RACES race. Takes back the finish frame's NEW HIGH SCORE / NEW BEST LAP, so retail's name entry and
 * memory-card save never open, and returns 1 when the award is to be skipped. */
int NativeRelic_SkipAward(void);

/* Retail's per-boss loss counter feeds boss difficulty and weapons; a BOSS RACES race starts from zero
 * and never reads or writes the Adventure counter. */
static inline int NativeArcade_BossTimesLost(int bossID)
{
	if (NativeArcade_BossRace())
	{
		return 0;
	}

	return sdata->advProgress.timesLostBossRace[bossID];
}

/* True while a BOSS RACES race runs on a track that is not its boss's retail boss track. */
int NativeArcade_BossOffHome(void);

/* Menu hooks (game/230): passthroughs on PC. */
struct MenuRow *NativeArcade_RaceTypeRows(struct MenuRow *rows);
s32 NativeArcade_HandleRaceTypeRow(struct RectMenu *menu, s32 row);
s32 NativeArcade_TrackAllowed(s16 levID);
s32 NativeArcade_FixedLaps(void);
void NativeArcade_ResetMode(void);

/* Race start and exit (game/QueueLoadTrack.c, game/MAIN/MainRaceTrack.c). */
void NativeArcade_ArmRace(void);
void NativeArcade_OnLoadRequest(s16 levelID);

/* Called by LOAD_LangFile after the string table is rebuilt. */
void NativeArcade_OnLanguage(int langFileIndex);

#else

static inline int NativeArcade_BossRace(void)
{
	return 0;
}

static inline int NativeArcade_CtrChallenge(void)
{
	return 0;
}

static inline int NativeArcade_RelicRace(void)
{
	return 0;
}

static inline u32 *NativeArcade_RelicRewards(void)
{
	return sdata->advProgress.rewards;
}

static inline int NativeRelic_SkipAward(void)
{
	return 0;
}

static inline int NativeRelic_TierOfTrack(s32 levID)
{
	(void)levID;
	return 0;
}

static inline int NativeArcade_BossTimesLost(int bossID)
{
	return sdata->advProgress.timesLostBossRace[bossID];
}

static inline int NativeArcade_BossOffHome(void)
{
	return 0;
}

#endif /* CTR_NATIVE */

#endif /* NATIVE_ARCADE_MODES_H */
