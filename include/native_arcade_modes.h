#ifndef NATIVE_ARCADE_MODES_H
#define NATIVE_ARCADE_MODES_H

/* Arcade-menu modes (3DS only): extra rows of the ARCADE RACE TYPE menu, each a retail event shape run
 * without its Adventure side.
 *
 *   BOSS RACES    ARCADE_MODE|ADVENTURE_BOSS, the boss of the track's hub (Ripper Roo, Papu Papu,
 *                 Komodo Joe, Pinstripe; Oxide for Oxide Station and the two bonus tracks),
 *                 1 player + 1 boss, 3 laps
 *
 * g_nativeArcadeMode is the single switch. Only the 3DS menu rows set it, so on PC it stays 0 and
 * every site below runs the retail path. The retail flags are armed when the race load is queued
 * (NativeArcade_ArmRace) and removed when a load of the main-menu level is queued
 * (NativeArcade_OnLoadRequest); the mode itself survives CHANGE LEVEL and CHANGE CHARACTER and is
 * cleared whenever the top-level main menu takes a choice or comes up (NativeArcade_ResetMode).
 * No mode writes Adventure progress, the retail profile, or an unlock: sites that would are
 * guarded by the predicates here. Defined in platform/native_arcade_modes.c. */

enum NativeArcadeMode
{
	NATIVE_ARCADE_NONE = 0,
	NATIVE_ARCADE_BOSS = 1,
};

#ifdef CTR_NATIVE

extern int g_nativeArcadeMode;

/* True while a BOSS RACES race is running (mode set and the retail boss flag armed). */
static inline int NativeArcade_BossRace(void)
{
	return (g_nativeArcadeMode == NATIVE_ARCADE_BOSS) && ((sdata->gGT->gameMode1 & ADVENTURE_BOSS) != 0);
}

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
