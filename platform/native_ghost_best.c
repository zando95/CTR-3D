/* Time Trial "best ghost": one saved ghost per track, kept apart from the retail four-slot ghost menu.
 *
 * game/MAIN/MainGameEnd.c hands the finished tape to NativeGhostBest_OnRaceEnd on the finish frame, which only
 * copies it aside. The write happens in NativeGhostBest_Flush, called wherever a finished race is left (the
 * results screen's exits, TryLoad, the platform shutdown), so the SD card is never touched on the finish frame.
 *
 * With the setting on, game/230/MM_TrackSelect.c skips the Time Trial ghost menu (SkipsGhostMenu) and loads the
 * track's best ghost with TryLoad, so the ghost raced is always the stored best, and no ghost when none exists.
 * Since the loaded ghost is the stored best, a finish that CheckTimeTrialGhost rejects as slower than it can never
 * have beaten the stored best.
 *
 * Each track's ghost is the file CTR3DBEST<levelID, two digits> in memory-card slot 0, in the retail ghost file's
 * layout (0x100 icon header, then the 0x3e00 tape buffer ending in its CRC16) and written through the same
 * .tmp/.bak path. The name stays outside the retail ghost list's BASCUS-94426G* pattern, so retail menus and older
 * builds never see these files. Names are uppercase because the card is case-insensitive FAT.
 *
 * With the setting off nothing here reads or writes a file. On PC every function is a no-op or returns 0: replays
 * must stay deterministic and the PC build gains no file access.
 */

#include <common.h>
#include <platform/native_ghost_best.h>
#include <platform/native_log.h>
#include <platform/native_memcard.h>

#include <stdio.h>
#include <string.h>

int g_ghostBestEnabled = 1;

CTR_STATIC_ASSERT(sizeof(struct GhostHeader) == 0x28);

#ifdef __3DS__

#define NATIVE_GHOST_BEST_SAVE_NAME_FORMAT "CTR3DBEST%02d"
#define NATIVE_GHOST_BEST_SAVE_NAME_BYTES  64 /* MEMCARD_StringSet's destination size */
#define NATIVE_GHOST_BEST_SLOT             0
#define NATIVE_GHOST_BEST_ICON_BYTES       0x100
#define NATIVE_GHOST_BEST_LEVEL_COUNT      64 /* covers every Time Trial track (levelID 0..17); the file name carries two digits */

/* Each entry of s_gbStoredMs: 0 not looked up yet, -1 nothing usable stored, otherwise the stored time in ms. */
#define NATIVE_GHOST_BEST_UNKNOWN 0
#define NATIVE_GHOST_BEST_NONE    (-1)

/* gameMode2 cheats that can change a Time Trial time; any of them blocks saving.
 *   WUMPA                start with the maximum fruit count, which raises base speed
 *   MASK, TURBO, BOMBS   start with a held item that is never used up
 *   INVISIBLE            permanent invisibility timer; not shown to be time-neutral, so it blocks
 *   ENGINE               permanent super engine
 *   ICY                  every surface counts as ice
 *   TURBOPAD             every turbo pad is a super turbo pad
 *   ONELAP               shortens the race
 * Not blocking: ADV and SUPERHARD only tune AI drivers (BOTS.c), and Time Trial has none; TURBOCOUNT only draws a
 * turbo counter on the HUD. */
#define NATIVE_GHOST_BEST_BLOCKING_CHEATS \
	(CHEAT_WUMPA | CHEAT_MASK | CHEAT_TURBO | CHEAT_INVISIBLE | CHEAT_ENGINE | CHEAT_ICY | CHEAT_TURBOPAD | CHEAT_BOMBS | CHEAT_ONELAP)

static s32 s_gbStoredMs[NATIVE_GHOST_BEST_LEVEL_COUNT];
static u8 s_gbSnapshot[GHOST_RECORD_BUFFER_SIZE];
static s32 s_gbPending;
static s32 s_gbPendingLevel;
static s32 s_gbPendingMs;

static int NativeGhostBest_LevelInRange(s16 levelID)
{
	return (levelID >= 0) && (levelID < NATIVE_GHOST_BEST_LEVEL_COUNT);
}

/* A usable tape header: the retail tape version, this track, a non-empty tape and a positive time. */
static int NativeGhostBest_HeaderValid(const struct GhostHeader *gh, s16 levelID)
{
	return (gh->version == GHOST_TAPE_VERSION_RETAIL) && (gh->levelID == levelID) && (gh->size > 0) && (gh->size <= GHOST_RECORD_BUFFER_SIZE) &&
	       (gh->timeElapsedInRace > 0);
}

/* dst is the platform save name in the form the memcard adapter builds ("bu00:CTR3DBEST05"). */
static void NativeGhostBest_SaveName(char *dst, s16 levelID)
{
	char fileName[16];

	snprintf(fileName, sizeof(fileName), NATIVE_GHOST_BEST_SAVE_NAME_FORMAT, (int)levelID);
	MEMCARD_StringSet(dst, NATIVE_GHOST_BEST_SLOT, fileName);
}

/* The track's stored best time in ms, or NATIVE_GHOST_BEST_NONE. A lookup reads only the file's header (primary,
 * then backup) and is remembered. levelID must be in range. */
static s32 NativeGhostBest_StoredMs(s16 levelID)
{
	char name[NATIVE_GHOST_BEST_SAVE_NAME_BYTES];
	struct GhostHeader header;

	if (s_gbStoredMs[levelID] != NATIVE_GHOST_BEST_UNKNOWN)
	{
		return s_gbStoredMs[levelID];
	}

	NativeGhostBest_SaveName(name, levelID);
	s_gbStoredMs[levelID] = NATIVE_GHOST_BEST_NONE;

	if (((NativeMemcard_ReadSaveData(name, (unsigned char *)&header, (int)sizeof(header), NATIVE_GHOST_BEST_ICON_BYTES) == NATIVE_MEMCARD_OK) &&
	     NativeGhostBest_HeaderValid(&header, levelID)) ||
	    ((NativeMemcard_ReadBackupSaveData(name, (unsigned char *)&header, (int)sizeof(header), NATIVE_GHOST_BEST_ICON_BYTES) == NATIVE_MEMCARD_OK) &&
	     NativeGhostBest_HeaderValid(&header, levelID)))
	{
		s_gbStoredMs[levelID] = header.timeElapsedInRace;
	}

	return s_gbStoredMs[levelID];
}

/* Mirrors the adapter's MEMCARD_Load: the primary must pass its CRC, else the backup is tried. A copy only counts
 * if its header is a finished tape of this track. */
static int NativeGhostBest_ReadFile(s16 levelID, struct GhostHeader *dst)
{
	char name[NATIVE_GHOST_BEST_SAVE_NAME_BYTES];

	NativeGhostBest_SaveName(name, levelID);

	if ((NativeMemcard_ReadSaveData(name, (unsigned char *)dst, GHOST_RECORD_BUFFER_SIZE, NATIVE_GHOST_BEST_ICON_BYTES) == NATIVE_MEMCARD_OK) &&
	    MEMCARD_NativeChecksumMatches((u8 *)dst, GHOST_RECORD_BUFFER_SIZE) && NativeGhostBest_HeaderValid(dst, levelID))
	{
		return 1;
	}

	if ((NativeMemcard_ReadBackupSaveData(name, (unsigned char *)dst, GHOST_RECORD_BUFFER_SIZE, NATIVE_GHOST_BEST_ICON_BYTES) == NATIVE_MEMCARD_OK) &&
	    MEMCARD_NativeChecksumMatches((u8 *)dst, GHOST_RECORD_BUFFER_SIZE) && NativeGhostBest_HeaderValid(dst, levelID))
	{
		Platform_LogWarn("[CTR 3D] best ghost: %s unreadable, loaded its backup\n", name);
		return 1;
	}

	return 0;
}

#endif /* __3DS__ */

int NativeGhostBest_SkipsGhostMenu(void)
{
#ifdef __3DS__
	return g_ghostBestEnabled != 0;
#else
	return 0;
#endif
}

void NativeGhostBest_OnRaceEnd(s16 levelID, const struct GhostHeader *gh)
{
#ifdef __3DS__
	if ((g_ghostBestEnabled == 0) || (gh == NULL) || !NativeGhostBest_LevelInRange(levelID))
	{
		return;
	}

	if ((sdata->gGT->gameMode2 & NATIVE_GHOST_BEST_BLOCKING_CHEATS) != 0)
	{
		return;
	}

	if (!NativeGhostBest_HeaderValid(gh, levelID))
	{
		return;
	}

	/* A second finish that arrives before the first was flushed keeps the faster of the two on the same track. */
	if ((s_gbPending != 0) && (s_gbPendingLevel == levelID) && (s_gbPendingMs <= gh->timeElapsedInRace))
	{
		return;
	}

	memcpy(s_gbSnapshot, gh, GHOST_RECORD_BUFFER_SIZE);
	s_gbPendingLevel = levelID;
	s_gbPendingMs = gh->timeElapsedInRace;
	s_gbPending = 1;
#else
	(void)levelID;
	(void)gh;
#endif
}

void NativeGhostBest_Flush(void)
{
#ifdef __3DS__
	char name[NATIVE_GHOST_BEST_SAVE_NAME_BYTES];
	unsigned char icon[NATIVE_GHOST_BEST_ICON_BYTES];
	unsigned long long startTick;
	unsigned long long ticks;
	unsigned long long freq;
	enum NativeMemcardResult result;
	s16 levelID;
	s32 storedMs;

	if (s_gbPending == 0)
	{
		return;
	}

	s_gbPending = 0;

	if (g_ghostBestEnabled == 0)
	{
		return;
	}

	levelID = (s16)s_gbPendingLevel;
	storedMs = NativeGhostBest_StoredMs(levelID);
	if ((storedMs != NATIVE_GHOST_BEST_NONE) && (s_gbPendingMs >= storedMs))
	{
		return;
	}

	MEMCARD_ChecksumSave(s_gbSnapshot, GHOST_RECORD_BUFFER_SIZE);

	memset(icon, 0, sizeof(icon));
	NativeGhostBest_SaveName(name, levelID);

	startTick = N3DS_GetTickCount();
	result = NativeMemcard_WriteSaveData(name, icon, (int)sizeof(icon), s_gbSnapshot, GHOST_RECORD_BUFFER_SIZE);
	ticks = N3DS_GetTickCount() - startTick;

	freq = N3DS_GetTickFrequency();
	if (freq == 0ull)
	{
		freq = 1ull;
	}

	if (result == NATIVE_MEMCARD_OK)
	{
		s_gbStoredMs[levelID] = s_gbPendingMs;
	}

	Platform_LogInstrument("[GHOSTBEST] level=%d time=%dms write=%ums %s\n", (int)levelID, (int)s_gbPendingMs, (unsigned)((ticks * 1000ull) / freq),
	                       (result == NATIVE_MEMCARD_OK) ? "ok" : "FAILED");
#endif
}

int NativeGhostBest_TryLoad(s16 levelID, struct GhostHeader *dst)
{
	int loaded = 0;

	if (dst == NULL)
	{
		return 0;
	}

#ifdef __3DS__
	if ((g_ghostBestEnabled != 0) && NativeGhostBest_LevelInRange(levelID))
	{
		NativeGhostBest_Flush();

		loaded = NativeGhostBest_ReadFile(levelID, dst);
		s_gbStoredMs[levelID] = loaded ? dst->timeElapsedInRace : NATIVE_GHOST_BEST_NONE;
	}
#else
	(void)levelID;
#endif

	if (!loaded)
	{
		memset(dst, 0, GHOST_RECORD_BUFFER_SIZE);
	}

	return loaded;
}
