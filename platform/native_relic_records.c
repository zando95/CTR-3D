/* RELIC RACES records (3DS only); the mode is described in include/native_arcade_modes.h.
 *
 * Retail's relic race keeps its best times in the retail profile (gameProgress.highScoreTracks, six
 * HighScoreEntry per track and mode) and its relics as bits in advProgress.rewards. A RELIC RACES
 * race must write neither, so the retail code runs against storage of its own:
 *
 *   - the records are s_rrFile.entries[level][6], the same layout as one track's relic half of the
 *     retail table (entry 0 the unused best lap, entries 1-5 the five best times). The game's
 *     ptrActiveHighScoreEntry points at them (GAMEPROG_GetPtrHighScoreTrack), so the retail end-of-
 *     race code ranks, flags NEW HIGH SCORE, draws the best-times box and inserts the run unchanged.
 *   - the relics are the bits of g_nativeRelicRewards, derived from the best time of each track
 *     (a relic is owned when the best time is at or under its RelicTime). The relic code that reads
 *     or sets reward bits is handed this view instead of advProgress.rewards, so the HUD goal, the
 *     RELIC AWARDED animation and the colour of the relic follow these records.
 *
 * The file is CTR3DRELIC in memory-card slot 0, written like the best ghosts (native_ghost_best.c):
 * a zero 0x100-byte icon header, then NativeRelicFile with a CRC16 in its last two bytes, through
 * the .tmp/.bak swap. Its name is outside the retail ghost list's BASCUS-94426G* pattern. A run is
 * recorded where retail would open the name entry and the memory-card save for a new high score
 * (NativeRelic_Commit, from MainFrame_GameLogic) and written when the results are left or the
 * platform shuts down (NativeRelic_Flush); the finish frame does no file I/O. A cheat that can
 * change a time (the same set that blocks the best ghost) records nothing and awards no relic.
 * On PC every function here is a no-op or returns 0. */

#include <common.h>
#include <native_arcade_modes.h>
#include <platform/native_log.h>
#include <platform/native_memcard.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

u32 g_nativeRelicRewards[NATIVE_RELIC_REWARD_WORDS];

#ifdef __3DS__

#define NRR_SAVE_NAME       "CTR3DRELIC"
#define NRR_SAVE_NAME_BYTES 64 /* MEMCARD_StringSet's destination size */
#define NRR_SLOT            0
#define NRR_ICON_BYTES      0x100
#define NRR_LEVELS          MEMCARD_HIGH_SCORE_TRACK_COUNT
#define NRR_ENTRIES         MEMCARD_HIGH_SCORE_ENTRIES_PER_MODE
#define NRR_MAGIC           0x53434c52u /* "RLCS" */
#define NRR_VERSION         1u
#define NRR_BEST_ENTRY      1 /* entry 0 is the best lap; entries 1-5 the five best race times */
#define NRR_TIERS           3 /* sapphire, gold, platinum */
#define NRR_NAME_COPY_BYTES 0x11
#define NRR_CHARACTER_COUNT 16 /* Crash .. Nitros Oxide */

struct NativeRelicFile
{
	u32 magic;
	u32 version;
	struct HighScoreEntry entries[NRR_LEVELS][NRR_ENTRIES];
	u8 crc[2];
};

/* the file is the struct up to and including crc: trailing struct padding is not written */
#define NRR_FILE_BYTES ((int)(offsetof(struct NativeRelicFile, crc) + 2))

CTR_STATIC_ASSERT(NATIVE_RELIC_REWARD_WORDS * 32 > ADV_REWARD_FIRST_PLATINUM_RELIC + NRR_LEVELS);

static struct NativeRelicFile s_rrFile;
static s32 s_rrLoaded;
static s32 s_rrDirty;

static void NativeRelic_SaveName(char *dst)
{
	char fileName[16];

	snprintf(fileName, sizeof(fileName), "%s", NRR_SAVE_NAME);
	MEMCARD_StringSet(dst, NRR_SLOT, fileName);
}

/* Retail's empty table: the default time and one character per row (GAMEPROG_ResetHighScores). */
static void NativeRelic_FillDefaults(void)
{
	memset(&s_rrFile, 0, sizeof(s_rrFile));
	s_rrFile.magic = NRR_MAGIC;
	s_rrFile.version = NRR_VERSION;

	for (s32 level = 0; level < NRR_LEVELS; level++)
	{
		for (s32 k = 0; k < NRR_ENTRIES; k++)
		{
			struct HighScoreEntry *entry = &s_rrFile.entries[level][k];
			s32 characterID = level + 1 + k;

			characterID -= PENTA_PENGUIN * (characterID / PENTA_PENGUIN);

			entry->time = MEMCARD_HIGH_SCORE_DEFAULT_TIME;
			entry->characterID = (u16)characterID;

			if ((sdata->lngStrings != NULL) && (sdata->numLngStrings > data.MetaDataCharacters[characterID].name_LNG_short))
			{
				strncpy(entry->name, sdata->lngStrings[data.MetaDataCharacters[characterID].name_LNG_short], MEMCARD_HIGH_SCORE_NAME_LENGTH - 1);
			}
		}
	}
}

/* A file the retail screens can draw from: right magic and version, a known character per entry and
 * a terminated name. (A record can belong to any selectable character, Penta Penguin and Oxide
 * included, while retail's empty table only uses the first 13.) */
static int NativeRelic_FileValid(void)
{
	if ((s_rrFile.magic != NRR_MAGIC) || (s_rrFile.version != NRR_VERSION))
	{
		return 0;
	}

	for (s32 level = 0; level < NRR_LEVELS; level++)
	{
		for (s32 k = 0; k < NRR_ENTRIES; k++)
		{
			const struct HighScoreEntry *entry = &s_rrFile.entries[level][k];

			if ((entry->characterID >= NRR_CHARACTER_COUNT) || (memchr(entry->name, 0, MEMCARD_HIGH_SCORE_NAME_LENGTH) == NULL))
			{
				return 0;
			}
		}
	}

	return 1;
}

static int NativeRelic_ReadFile(void)
{
	char name[NRR_SAVE_NAME_BYTES];

	NativeRelic_SaveName(name);

	if ((NativeMemcard_ReadSaveData(name, (unsigned char *)&s_rrFile, NRR_FILE_BYTES, NRR_ICON_BYTES) == NATIVE_MEMCARD_OK) &&
	    MEMCARD_NativeChecksumMatches((u8 *)&s_rrFile, NRR_FILE_BYTES) && NativeRelic_FileValid())
	{
		return 1;
	}

	if ((NativeMemcard_ReadBackupSaveData(name, (unsigned char *)&s_rrFile, NRR_FILE_BYTES, NRR_ICON_BYTES) == NATIVE_MEMCARD_OK) &&
	    MEMCARD_NativeChecksumMatches((u8 *)&s_rrFile, NRR_FILE_BYTES) && NativeRelic_FileValid())
	{
		Platform_LogWarn("[CTR 3D] relic records: %s unreadable, loaded its backup\n", name);
		return 1;
	}

	return 0;
}

/* The reward bits of one track from its best time. */
static void NativeRelic_RebuildRewards(s32 level)
{
	u32 bestTime = s_rrFile.entries[level][NRR_BEST_ENTRY].time;

	for (s32 tier = 0; tier < NRR_TIERS; tier++)
	{
		s32 bit = ADV_REWARD_FIRST_SAPPHIRE_RELIC + tier * ADV_REWARD_RELIC_TIER_STRIDE + level;

		if (bestTime <= (u32)data.RelicTime[level * NRR_TIERS + tier])
		{
			UNLOCK_ADV_BIT(g_nativeRelicRewards, bit);
		}
		else
		{
			g_nativeRelicRewards[MEMCARD_BIT_WORD(bit)] &= ~MEMCARD_BIT_MASK(bit);
		}
	}
}

static void NativeRelic_EnsureLoaded(void)
{
	if (s_rrLoaded != 0)
	{
		return;
	}

	s_rrLoaded = 1;

	if (!NativeRelic_ReadFile())
	{
		NativeRelic_FillDefaults();
	}

	for (s32 level = 0; level < NRR_LEVELS; level++)
	{
		NativeRelic_RebuildRewards(level);
	}
}

#endif /* __3DS__ */

void NativeRelic_Prepare(void)
{
#ifdef __3DS__
	NativeRelic_EnsureLoaded();
#endif
}

struct HighScoreEntry *NativeRelic_Entries(s32 levelID)
{
#ifdef __3DS__
	if ((levelID >= 0) && (levelID < NRR_LEVELS))
	{
		NativeRelic_EnsureLoaded();
		return s_rrFile.entries[levelID];
	}
#endif
	(void)levelID;
	return NULL;
}

int NativeRelic_SkipAward(void)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;

	/* Adventure's own relic races are not ours to restrict */
	if (!NativeArcade_RelicRace() || ((gGT->gameMode2 & NATIVE_GHOST_BEST_BLOCKING_CHEATS) == 0))
	{
		return 0;
	}

	/* the finish frame already flagged a record from the retail table: take the banners back */
	gGT->newHighScoreIndex = -1;
	gGT->gameModeEnd &= ~(NEW_HIGH_SCORE | NEW_BEST_LAP);
	return 1;
#else
	return 0;
#endif
}

void NativeRelic_Commit(void)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;
	struct Driver *player = gGT->drivers[0];
	char savedName[NRR_NAME_COPY_BYTES];
	s32 level = gGT->levelID;
	s32 characterID;

	if (NativeArcade_RelicRace() && (level >= 0) && (level < NRR_LEVELS) && (player != NULL) &&
	    ((gGT->gameMode2 & NATIVE_GHOST_BEST_BLOCKING_CHEATS) == 0))
	{
		NativeRelic_EnsureLoaded();

		/* The retail insert stamps the typed profile name; the records carry the character's name. */
		characterID = data.characterIDs[(u8)player->driverID];
		memcpy(savedName, gGT->prevNameEntered, NRR_NAME_COPY_BYTES);
		memset(gGT->prevNameEntered, 0, NRR_NAME_COPY_BYTES);

		if ((sdata->lngStrings != NULL) && (sdata->numLngStrings > data.MetaDataCharacters[characterID].name_LNG_short))
		{
			strncpy(gGT->prevNameEntered, sdata->lngStrings[data.MetaDataCharacters[characterID].name_LNG_short], NRR_NAME_COPY_BYTES - 1);
		}

		/* inserts into ptrActiveHighScoreEntry, which is this level's records; once per race */
		MainGameEnd_SoloRaceSaveHighScore();

		memcpy(gGT->prevNameEntered, savedName, NRR_NAME_COPY_BYTES);

		NativeRelic_RebuildRewards(level);
		s_rrDirty = 1;
	}

	/* The retail results wait for these flags to clear; the name entry and the card save clear them
	 * there. The insert above sets them again, so they are cleared last. */
	gGT->gameModeEnd &= ~(NEW_BEST_LAP | NEW_HIGH_SCORE);
#endif
}

void NativeRelic_Flush(void)
{
#ifdef __3DS__
	char name[NRR_SAVE_NAME_BYTES];
	unsigned char icon[NRR_ICON_BYTES];
	unsigned long long startTick;
	unsigned long long ticks;
	unsigned long long freq;
	enum NativeMemcardResult result;

	if ((s_rrLoaded == 0) || (s_rrDirty == 0))
	{
		return;
	}

	s_rrFile.magic = NRR_MAGIC;
	s_rrFile.version = NRR_VERSION;
	MEMCARD_ChecksumSave((u8 *)&s_rrFile, NRR_FILE_BYTES);

	memset(icon, 0, sizeof(icon));
	NativeRelic_SaveName(name);

	startTick = N3DS_GetTickCount();
	result = NativeMemcard_WriteSaveData(name, icon, (int)sizeof(icon), (const unsigned char *)&s_rrFile, NRR_FILE_BYTES);
	ticks = N3DS_GetTickCount() - startTick;

	freq = N3DS_GetTickFrequency();
	if (freq == 0ull)
	{
		freq = 1ull;
	}

	/* a failed write stays pending for the next exit */
	s_rrDirty = (result == NATIVE_MEMCARD_OK) ? 0 : 1;

	Platform_LogInstrument("[RELICREC] write=%ums %s\n", (unsigned)((ticks * 1000ull) / freq), (result == NATIVE_MEMCARD_OK) ? "ok" : "FAILED");
#endif
}
