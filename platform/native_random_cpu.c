/* Random CPU racers: the field draw and the per-racer model load (include/native_random_cpu.h). */

#include <common.h>
#include <native_random_cpu.h>

#include <string.h>

#ifdef __3DS__
int g_randomCpus = 1;
#else
int g_randomCpus = 0;
#endif

enum
{
	RCPU_RACERS = 8,
	RCPU_CPUS = RCPU_RACERS - 1,
	RCPU_STARTING_RACERS = 8, /* characterIDs 0-7: the racers every save has */
	RCPU_CHARACTERS = NITROS_OXIDE + 1,
	RCPU_FILES_PER_CPU = 2,       /* the hi model file, then the med file (med, low, box) */
	RCPU_HEADER_CAP = 4,          /* hi + the med file's three rungs */
	RCPU_HI_MAX_DISTANCE = 320,   /* the 1P arcade packs' CPU hi rung; their med, low and box rungs are the med file's */
	RCPU_PACK_FILE = 0,
	RCPU_PLAYER_FILE = 1,
	RCPU_FIRST_CPU_FILE = 2,
	RCPU_FILE_CAP = RCPU_FIRST_CPU_FILE + (RCPU_CPUS * RCPU_FILES_PER_CPU),
};

#define RCPU_RNG_FALLBACK_SEED 0x6d2b79f5u
#define RCPU_RNG_SEED_MIX      0x9e3779b9u

struct RcpuModel
{
	struct Model model;
	struct ModelHeader headers[RCPU_HEADER_CAP];
};

/* The field: [0] the player it was drawn for, [1..7] the CPU racers, kept across races until a new one is drawn. */
static s16 s_rcField[RCPU_RACERS];
static u32 s_rcFieldPool; /* ARCADE_MODE or ADVENTURE_MODE: which pool it came from */
static b32 s_rcFieldValid;
static u32 s_rcRngState;

/* The per-racer load: the shared pack, the player's hi file, then each CPU's hi and med files. */
static struct BigHeader *s_rcBigfile;
static void (*s_rcPackCallback)(struct LoadQueueSlot *);
static struct LoadQueueSlot s_rcPackSlot; /* the pack's slot as it loaded, handed to s_rcPackCallback at the end */
static s32 s_rcFileIndex[RCPU_FILE_CAP];
static s32 s_rcFileCount;
static s32 s_rcFileNext;
static struct Model *s_rcFileModel[RCPU_CPUS][RCPU_FILES_PER_CPU];
static struct RcpuModel *s_rcStore; /* in mempack beside the files it points into, so the level load frees both */
static struct Model *s_rcModels[RCPU_CPUS];
static b32 s_rcSetPlayerExtra; /* this load put the player's model in driverModelExtras[0] */

static u32 NativeRandomCpu_Next(void)
{
	u32 x = s_rcRngState;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s_rcRngState = x;
	return x;
}

static b32 NativeRandomCpu_InList(s16 characterID, const s16 *list, s32 count)
{
	for (s32 i = 0; i < count; i++)
	{
		if (list[i] == characterID)
		{
			return true;
		}
	}

	return false;
}

/* The pool this race draws from, or 0 when it keeps retail's field. */
static u32 NativeRandomCpu_RacePool(void)
{
	struct GameTracker *gGT = sdata->gGT;
	u32 mode1 = (u32)gGT->gameMode1;

	if ((g_randomCpus == 0) || (gGT->boolDemoMode != 0))
	{
		return 0;
	}

	if ((mode1 & (u32)(MAIN_MENU | TIME_TRIAL | BATTLE_MODE | ADVENTURE_ARENA | ADVENTURE_BOSS | RELIC_RACE | CRYSTAL_CHALLENGE | GAME_CUTSCENE)) != 0)
	{
		return 0;
	}

	if ((gGT->levelID < DINGO_CANYON) || (gGT->levelID > TURBO_TRACK))
	{
		return 0;
	}

	if ((mode1 & ADVENTURE_MODE) != 0)
	{
		return ADVENTURE_MODE;
	}

	if ((mode1 & ARCADE_MODE) != 0)
	{
		return ARCADE_MODE;
	}

	return 0;
}

static s32 NativeRandomCpu_BuildPool(s16 player, u32 pool, s16 *out)
{
	s32 count = 0;

	if (pool == ADVENTURE_MODE)
	{
		for (s16 c = 0; c < RCPU_STARTING_RACERS; c++)
		{
			if (c != player)
			{
				out[count++] = c;
			}
		}

		/* hubs 1-4 each award one boss key; the key's bit is the boss index R232 gives the hub */
		for (s32 hub = 1; hub <= ADV_REWARD_BOSS_KEY_COUNT; hub++)
		{
			s32 bossIndex = R232.bossCharacterIDs[hub];
			s16 boss = data.metaDataLEV[R232.bossRaceLevelIDs[hub]].characterID_Boss;

			if ((bossIndex < ADV_REWARD_BOSS_KEY_COUNT) && CHECK_ADV_BIT(sdata->advProgress.rewards, ADV_REWARD_FIRST_BOSS_KEY + bossIndex) &&
			    (boss != player) && !NativeRandomCpu_InList(boss, out, count))
			{
				out[count++] = boss;
			}
		}

		return count;
	}

	/* the select screen's own rule: an icon is unlocked always, or by its bit of the card-wide unlocks */
	for (s32 i = 0; i < (s32)len(D230.characterSelectMeta1P2P); i++)
	{
		const struct CharacterSelectMeta *meta = &D230.characterSelectMeta1P2P[i];
		s16 unlock = (s16)meta->unlockFlags;

		if ((meta->characterID != player) && ((unlock == MM_CHARACTER_UNLOCK_ALWAYS) || CHECK_ADV_BIT(sdata->gameProgress.unlocks, unlock)))
		{
			out[count++] = meta->characterID;
		}
	}

	if ((player != NITROS_OXIDE) && CHECK_ADV_BIT(sdata->gameProgress.unlocks, GAME_UNLOCK_BIT_OXIDE))
	{
		out[count++] = NITROS_OXIDE;
	}

	return count;
}

/* A race entered from a menu or hub gets a new field; one entered from a race (RESTART, RETRY, the next
 * race of a cup) keeps the field it had, as long as it still fits the player and the pool. */
static b32 NativeRandomCpu_KeepField(s16 player, u32 pool, const s16 *poolList, s32 poolCount)
{
	s32 prevLEV = sdata->gGT->prevLEV;

	if (!s_rcFieldValid || (s_rcField[0] != player) || (s_rcFieldPool != pool))
	{
		return false;
	}

	if ((prevLEV < DINGO_CANYON) || (prevLEV > TURBO_TRACK))
	{
		return false;
	}

	for (s32 i = 1; i < RCPU_RACERS; i++)
	{
		if (!NativeRandomCpu_InList(s_rcField[i], poolList, poolCount))
		{
			return false;
		}
	}

	return true;
}

static void NativeRandomCpu_DrawField(s16 player, u32 pool, s16 *poolList, s32 poolCount)
{
	struct GameTracker *gGT = sdata->gGT;

	/* the vsync count differs with every second spent in the menus */
	s_rcRngState ^= ((u32)gGT->frameTimer_VsyncCallback * RCPU_RNG_SEED_MIX) ^ (u32)gGT->timer;
	if (s_rcRngState == 0)
	{
		s_rcRngState = RCPU_RNG_FALLBACK_SEED;
	}

	for (s32 i = 0; i < RCPU_CPUS; i++)
	{
		s32 j = i + (s32)(NativeRandomCpu_Next() % (u32)(poolCount - i));
		s16 pick = poolList[j];

		poolList[j] = poolList[i];
		poolList[i] = pick;
		s_rcField[1 + i] = pick;
	}

	s_rcField[0] = player;
	s_rcFieldPool = pool;
	s_rcFieldValid = true;
}

/* AH_WarpPad put the track's speed champion on pole by slot, for the default field. Redo it for the field
 * that races: the champion starts on pole whenever he is in it, bosses included. Gem cups keep the grid
 * retail gave them. */
static void NativeRandomCpu_PlaceChampion(void)
{
	struct GameTracker *gGT = sdata->gGT;
	s16 champion = data.metaDataLEV[gGT->levelID].characterID_Champion;
	s32 championSlot = 0;

	if ((gGT->gameMode1 & ADVENTURE_CUP) != 0)
	{
		return;
	}

	for (s32 i = 1; i < RCPU_RACERS; i++)
	{
		if (data.characterIDs[i] == champion)
		{
			championSlot = i;
		}
	}

	if (championSlot == 0)
	{
		return;
	}

	for (s32 i = 1; i < RCPU_RACERS; i++)
	{
		if (i == championSlot)
		{
			sdata->kartSpawnOrderArray[i] = 0;
		}
		else if (i == RCPU_RACERS - 1)
		{
			sdata->kartSpawnOrderArray[i] = (char)championSlot;
		}
		else
		{
			sdata->kartSpawnOrderArray[i] = (char)i;
		}
	}

	/* the player starts at the back */
	sdata->kartSpawnOrderArray[0] = RCPU_RACERS - 1;
}

/* One CPU model the shape of a 1P arcade pack's: the hi file's hi rung, then the med file's rungs, every
 * rung with the drive animation only. Each rung keeps its own vertex scale and frames, so the rungs from
 * two files draw consistently. */
static void NativeRandomCpu_BuildModel(s32 cpu)
{
	const struct Model *hi = s_rcFileModel[cpu][0];
	const struct Model *med = s_rcFileModel[cpu][1];
	struct RcpuModel *dst = &s_rcStore[cpu];
	s32 count = 0;

	if ((hi->numHeaders < 1) || (hi->headers == NULL) || (med->numHeaders < 1) || (med->headers == NULL))
	{
		return;
	}

	memcpy(dst->model.name, hi->name, sizeof(dst->model.name));
	dst->model.id = hi->id;

	dst->headers[count] = hi->headers[0];
	dst->headers[count].maxDistanceLOD = RCPU_HI_MAX_DISTANCE;
	count++;

	for (s32 h = 0; (h < med->numHeaders) && (count < RCPU_HEADER_CAP); h++)
	{
		dst->headers[count++] = med->headers[h];
	}

	for (s32 h = 0; h < count; h++)
	{
		if (dst->headers[h].numAnimations > 1)
		{
			dst->headers[h].numAnimations = 1;
		}
	}

	dst->model.numHeaders = (s16)count;
	dst->model.headers = dst->headers;
	s_rcModels[cpu] = &dst->model;
}

/* Load callback of every CPU model file: the slot's destination is the relocated file's model. */
static void NativeRandomCpu_OnModelFile(struct LoadQueueSlot *lqs)
{
	struct Model *model = (struct Model *)lqs->ptrDestination;

	if ((model == NULL) || (s_rcStore == NULL))
	{
		return;
	}

	for (s32 cpu = 0; cpu < RCPU_CPUS; cpu++)
	{
		for (s32 f = 0; f < RCPU_FILES_PER_CPU; f++)
		{
			if ((s_rcFileIndex[RCPU_FIRST_CPU_FILE + (cpu * RCPU_FILES_PER_CPU) + f] != (s32)lqs->subfileIndex) || (s_rcFileModel[cpu][f] != NULL))
			{
				continue;
			}

			s_rcFileModel[cpu][f] = model;

			if ((s_rcFileModel[cpu][0] != NULL) && (s_rcFileModel[cpu][1] != NULL))
			{
				NativeRandomCpu_BuildModel(cpu);
			}

			return;
		}
	}
}

/* Load callback of the shared pack. The fonts point into the previous level's pack until load stage 5, its
 * memory is free from stage 0, and text is drawn on every frame of a load (the bottom screen's frame-rate
 * readout), so nothing may overwrite that pack while a frame can still draw from it. This load spans many
 * frames, so the pack loads first and the fonts move to it here, inside the read, before the next frame.
 * Stage 5 repeats the same move from sdata->ptrMPK when the load finishes. */
static void NativeRandomCpu_OnPackFile(struct LoadQueueSlot *lqs)
{
	struct GameTracker *gGT = sdata->gGT;

	s_rcPackSlot = *lqs;

	DecalGlobal_Clear(gGT);

	if (lqs->ptrDestination != NULL)
	{
		struct LevTexLookup *icons = (struct LevTexLookup *)(intptr_t)(*(int *)lqs->ptrDestination);

		if (icons != NULL)
		{
			DecalGlobal_Store(gGT, icons);
		}
	}
}

/* Load callback of the last CPU model file: the load may advance now that every file is in. */
static void NativeRandomCpu_OnLastFile(struct LoadQueueSlot *lqs)
{
	NativeRandomCpu_OnModelFile(lqs);
	s_rcPackCallback(&s_rcPackSlot);
}

/* The queue holds eight slots and the load needs sixteen, so the rest is fed in as slots free up. A load
 * callback cannot add a slot itself: native reads finish inside LOAD_NextQueuedFile, before it releases the
 * slot it is serving, so an append from there would land past the queue's end. */
void NativeRandomCpu_PumpQueue(void)
{
	while ((s_rcFileNext < s_rcFileCount) && (sdata->queueLength < LOAD_QUEUE_SLOT_COUNT))
	{
		s32 n = s_rcFileNext++;

		if (n == RCPU_PACK_FILE)
		{
			LOAD_AppendQueue(s_rcBigfile, LT_GETADDR, s_rcFileIndex[n], NULL, NativeRandomCpu_OnPackFile);
		}
		else if (n == RCPU_PLAYER_FILE)
		{
			/* the player's hi model, where retail's Time Trial load puts it (load stage 6 turns it into the model) */
			LOAD_AppendQueue(s_rcBigfile, LT_GETADDR, s_rcFileIndex[n], &data.driverModelExtras[0].fileBase, LOAD_QUEUE_CALLBACK_SET_POINTER);
			s_rcSetPlayerExtra = true;
		}
		else if (n == s_rcFileCount - 1)
		{
			LOAD_AppendQueue(s_rcBigfile, LT_GETADDR, s_rcFileIndex[n], NULL, NativeRandomCpu_OnLastFile);
		}
		else
		{
			LOAD_AppendQueue(s_rcBigfile, LT_GETADDR, s_rcFileIndex[n], NULL, NativeRandomCpu_OnModelFile);
		}
	}
}

int NativeRandomCpu_QueueRace(struct BigHeader *bigfile, void (*callback)(struct LoadQueueSlot *))
{
	s16 player = data.characterIDs[0];
	s16 poolList[RCPU_CHARACTERS];
	b32 isDefaultField = true;
	u32 pool = NativeRandomCpu_RacePool();
	s32 poolCount;

	if (pool == 0)
	{
		return 0;
	}

	poolCount = NativeRandomCpu_BuildPool(player, pool, poolList);
	if (poolCount < RCPU_CPUS)
	{
		return 0;
	}

	if (!NativeRandomCpu_KeepField(player, pool, poolList, poolCount))
	{
		NativeRandomCpu_DrawField(player, pool, poolList, poolCount);
	}

	/* data.characterIDs[1..7] still hold LOAD_Robots1P's field: exactly the CPU models of the player's pack */
	for (s32 i = 1; i < RCPU_RACERS; i++)
	{
		if (!NativeRandomCpu_InList(s_rcField[i], &data.characterIDs[1], RCPU_CPUS))
		{
			isDefaultField = false;
		}
	}

	for (s32 i = 1; i < RCPU_RACERS; i++)
	{
		data.characterIDs[i] = s_rcField[i];
	}

	if (pool == ADVENTURE_MODE)
	{
		NativeRandomCpu_PlaceChampion();
	}

	if (isDefaultField)
	{
		return 0;
	}

	s_rcStore = (struct RcpuModel *)MEMPACK_AllocMem((int)(sizeof(struct RcpuModel) * RCPU_CPUS));
	if (s_rcStore == NULL)
	{
		return 0;
	}

	s_rcBigfile = bigfile;
	s_rcPackCallback = callback;
	s_rcFileCount = 0;
	s_rcFileNext = 0;

	/* any 4P pack carries the 1P packs' shared models and icons; Penta's racer in it is a lone low-detail model */
	s_rcFileIndex[s_rcFileCount++] = BI_4PARCADEPACK + PENTA_PENGUIN;
	s_rcFileIndex[s_rcFileCount++] = BI_RACERMODELHI + player;

	for (s32 cpu = 0; cpu < RCPU_CPUS; cpu++)
	{
		s_rcFileIndex[s_rcFileCount++] = BI_RACERMODELHI + s_rcField[1 + cpu];
		s_rcFileIndex[s_rcFileCount++] = BI_RACERMODELMED + s_rcField[1 + cpu];
	}

	NativeRandomCpu_PumpQueue();
	return 1;
}

void NativeRandomCpu_ClearModels(void)
{
	/* load stage 4 clears only the slot's file; its model would still point into the freed level, and
	 * VehBirth_GetModelByName reads that slot before any pack */
	if (s_rcSetPlayerExtra)
	{
		data.driverModelExtras[0].model = NULL;
		s_rcSetPlayerExtra = false;
	}

	s_rcStore = NULL;
	s_rcFileCount = 0;
	s_rcFileNext = 0;
	memset(s_rcFileModel, 0, sizeof(s_rcFileModel));
	memset(s_rcModels, 0, sizeof(s_rcModels));
}

struct Model *NativeRandomCpu_FindModel(const char *name)
{
	for (s32 cpu = 0; cpu < RCPU_CPUS; cpu++)
	{
		const struct Model *model = s_rcModels[cpu];
		b32 same = (model != NULL);

		for (s32 w = 0; same && (w < MODEL_NAME_WORD_COUNT); w++)
		{
			same = ModelName_ReadWord(model->name, w) == ModelName_ReadWord(name, w);
		}

		if (same)
		{
			return s_rcModels[cpu];
		}
	}

	return NULL;
}
