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
#include <platform/native_log.h>
#include <platform/native_memcard.h>

#include <string.h>

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

/* The slots file the memory card holds (RefreshCard's 0x1680-byte buffer, sdata->ptrToMemcardBuffer1) starts with a
 * struct MemcardProfile: header[2] (version -18, size 0x1600), the four Adventure profiles' struct AdvProgress at 0x4..0x144
 * (0x50 bytes each, rewards[6] first), then the card-wide gameProgress. The buffer is the card as last read or saved. */
CTR_STATIC_ASSERT(sizeof(struct MemcardProfile) == 0x1600);
CTR_STATIC_ASSERT(OFFSETOF(struct MemcardProfile, advProgress) == 0x4);

/* True when anyone on this save file has the Adventure reward bit (ADV_REWARD_*): the profile loaded for Adventure, or any
 * used profile slot of the card. Read only; never writes either. */
static int NativeCard_AnyProfileHasBit(s32 bit)
{
	const struct MemcardProfile *card = (const struct MemcardProfile *)sdata->ptrToMemcardBuffer1;

	if (((u16)sdata->advProfileIndex < MEMCARD_ADV_PROFILE_COUNT) && CHECK_ADV_BIT(sdata->advProgress.rewards, bit))
	{
		return 1;
	}

	/* no buffer, or a slots file that is not a current-version profile (no card, a failed load): nothing to read */
	if ((card == NULL) || (card->header[0] != MEMCARD_PROFILE_VERSION) || (card->header[1] != (s16)sizeof(struct MemcardProfile)))
	{
		return 0;
	}

	for (s32 slot = 0; slot < MEMCARD_ADV_PROFILE_COUNT; slot++)
	{
		const struct AdvProgress *adv = &card->advProgress[slot];

		/* an empty slot has no character (GAMEPROG_InitFullMemcard) */
		if ((adv->characterID >= 0) && CHECK_ADV_BIT(adv->rewards, bit))
		{
			return 1;
		}
	}

	return 0;
}

static s32 NativeRelic_RewardBit(s32 level, s32 tier)
{
	return ADV_REWARD_FIRST_SAPPHIRE_RELIC + tier * ADV_REWARD_RELIC_TIER_STRIDE + level;
}

/* A relic tier is earned when anyone on this save file has it: the best time in the memory card's relic high scores (the
 * table Adventure and RELIC RACES both record into) is at or under its RelicTime, or any Adventure profile on the card holds
 * its relic bit. The default time of an untouched track is above every RelicTime. */
static int NativeRelic_Earned(s32 level, s32 tier)
{
	u32 bestTime = sdata->gameProgress.highScoreTracks[level].scoreEntry[NAM_RELIC_BEST_ENTRY].time;

	return (bestTime <= (u32)data.RelicTime[level * NAM_RELIC_TIERS + tier]) || NativeCard_AnyProfileHasBit(NativeRelic_RewardBit(level, tier));
}

/* The relic bits of every track from NativeRelic_Earned. Run at every start of a RELIC RACES race (NativeArcade_OnRaceStart),
 * so the HUD goal and the award test read the card as it is then and a relic that was never saved does not carry over. */
static void NativeRelic_Prepare(void)
{
	for (s32 level = 0; level < MEMCARD_HIGH_SCORE_TRACK_COUNT; level++)
	{
		for (s32 tier = 0; tier < NAM_RELIC_TIERS; tier++)
		{
			s32 bit = NativeRelic_RewardBit(level, tier);

			if (NativeRelic_Earned(level, tier))
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

/* CTR CHALLENGE's token marks. A track (levels 0-15) shows its CTR token when anyone on this save file has earned it: any
 * Adventure profile on the card (the reward bit ADV_REWARD_FIRST_CTR_TOKEN + level) or an Arcade CTR CHALLENGE win with all
 * three letters, recorded here. The wins are a 16-bit mask kept in the file CTR3DTOKENS of memory-card slot 0, written through
 * the same .tmp/.bak path as the best-ghost files and named outside the retail BASCUS-94426* pattern. File: a zero icon
 * header (NCT_ICON_BYTES), then the payload
 *   0  char[4]  "CTKN"
 *   4  u16      version (NCT_VERSION)
 *   6  u16      mask, bit n = a win on level n
 *   8  u16      CRC16 (MEMCARD_CRC16, two zero bytes appended) of bytes 0..7
 * all little-endian. A file that is absent, or read fine but fails its checks, starts the mask fresh. A read that fails (I/O
 * or open error) keeps the RAM mask and refuses every write until a later read succeeds, so the file is never overwritten from
 * a failed read; wins recorded meanwhile are merged into the file's mask by that read. The file is read on first use. */
#define NCT_SAVE_NAME     "CTR3DTOKENS"
#define NCT_SAVE_SLOT     0
#define NCT_NAME_BYTES    64 /* MEMCARD_StringSet's destination size */
#define NCT_ICON_BYTES    0x100
#define NCT_PAYLOAD_BYTES 10
#define NCT_VERSION       1
#define NCT_LEVELS        ADV_REWARD_CTR_TOKEN_TRACK_COUNT

enum NativeCtrTokensState
{
	NCT_NOT_READ = 0,
	NCT_READY = 1,       /* the file has been read (or is absent): writes allowed */
	NCT_READ_FAILED = 2, /* writes refused until a read succeeds */
};

static u16 s_nctMask;
static s32 s_nctState;
static s32 s_nctDirty;

static u16 NativeCtrTokens_Crc(const u8 *bytes, s32 count)
{
	u32 crc = 0;

	for (s32 i = 0; i < count; i++)
	{
		crc = MEMCARD_CRC16(crc, bytes[i]);
	}

	crc = MEMCARD_CRC16(crc, 0);
	crc = MEMCARD_CRC16(crc, 0);
	return (u16)crc;
}

static void NativeCtrTokens_Pack(u8 *dst, u16 mask)
{
	u16 crc;

	memcpy(dst, "CTKN", 4);
	dst[4] = (u8)(NCT_VERSION & 0xff);
	dst[5] = (u8)(NCT_VERSION >> 8);
	dst[6] = (u8)(mask & 0xff);
	dst[7] = (u8)(mask >> 8);
	crc = NativeCtrTokens_Crc(dst, 8);
	dst[8] = (u8)(crc & 0xff);
	dst[9] = (u8)(crc >> 8);
}

static int NativeCtrTokens_Unpack(const u8 *src, u16 *maskOut)
{
	if ((memcmp(src, "CTKN", 4) != 0) || ((u16)(src[4] | (src[5] << 8)) != NCT_VERSION))
	{
		return 0;
	}

	if ((u16)(src[8] | (src[9] << 8)) != NativeCtrTokens_Crc(src, 8))
	{
		return 0;
	}

	*maskOut = (u16)(src[6] | (src[7] << 8));
	return 1;
}

static void NativeCtrTokens_SaveName(char *dst)
{
	char fileName[16] = NCT_SAVE_NAME;

	MEMCARD_StringSet(dst, NCT_SAVE_SLOT, fileName);
}

/* Reads the file into the RAM mask (merged, never replacing a win recorded this session) and sets the state. */
static void NativeCtrTokens_Load(void)
{
	char name[NCT_NAME_BYTES];
	u8 bytes[NCT_PAYLOAD_BYTES];
	u16 fileMask = 0;
	enum NativeMemcardResult primary;
	enum NativeMemcardResult backup;

	NativeCtrTokens_SaveName(name);
	primary = NativeMemcard_ReadSaveData(name, bytes, NCT_PAYLOAD_BYTES, NCT_ICON_BYTES);

	if ((primary == NATIVE_MEMCARD_OK) && NativeCtrTokens_Unpack(bytes, &fileMask))
	{
		s_nctMask |= fileMask;
		s_nctState = NCT_READY;
		Platform_LogInstrument("[CTRTOKENS] read mask=%04x\n", (unsigned)s_nctMask);
		return;
	}

	backup = NativeMemcard_ReadBackupSaveData(name, bytes, NCT_PAYLOAD_BYTES, NCT_ICON_BYTES);

	if ((backup == NATIVE_MEMCARD_OK) && NativeCtrTokens_Unpack(bytes, &fileMask))
	{
		s_nctMask |= fileMask;
		s_nctState = NCT_READY;
		Platform_LogInstrument("[CTRTOKENS] primary unreadable, read its backup mask=%04x\n", (unsigned)s_nctMask);
		return;
	}

	/* no usable copy: absent, or read fine and failed its checks, starts fresh; an I/O or open error must never lead to an overwrite */
	if (((primary == NATIVE_MEMCARD_NOT_FOUND) || (primary == NATIVE_MEMCARD_OK)) && ((backup == NATIVE_MEMCARD_NOT_FOUND) || (backup == NATIVE_MEMCARD_OK)))
	{
		s_nctState = NCT_READY;
		Platform_LogInstrument("[CTRTOKENS] no usable file, starting fresh\n");
		return;
	}

	s_nctState = NCT_READ_FAILED;
	Platform_LogInstrument("[CTRTOKENS] read FAILED (primary=%d backup=%d), writes refused\n", (int)primary, (int)backup);
}

/* retryFailed: also try again after a failed read (track-select entry, a write); otherwise only the first read is made */
static void NativeCtrTokens_Ensure(int retryFailed)
{
	if ((s_nctState == NCT_NOT_READ) || ((s_nctState == NCT_READ_FAILED) && (retryFailed != 0)))
	{
		NativeCtrTokens_Load();
	}
}

static u32 NativeCtrToken_TintOf(s32 level)
{
	s32 group = data.metaDataLEV[level].ctrTokenGroupID;

	if ((group < 0) || (group >= (s32)(sizeof(data.AdvCups) / sizeof(data.AdvCups[0]))))
	{
		group = 0;
	}

	/* the colour Adventure's warp pads and HUD give the track's token */
	return INST_PackColorRGB(data.AdvCups[group].color[0], data.AdvCups[group].color[1], data.AdvCups[group].color[2]);
}

/* Track select's icons: one instance per visible row slot, in the UI pushbuffer so the 3DS UI band applies. RELIC RACES shows a
 * relic tinted by the track's highest tier, CTR CHALLENGE the CTR token tinted as Adventure tints it; the modes are exclusive, so
 * one array serves both. The main menu's instance pool is 32, so a failed birth just leaves that row without its icon. Both
 * models live in the packs the menu level loads (1P arcade, Adventure, Time Trial, 2P) and their textures in resident VRAM, so
 * neither needs a load. */
#define NRT_SLOTS       9     /* MM_TRACK_SELECT_VISIBLE_ROWS */
#define NRT_ICON_X      12    /* centre, right of the row box */
#define NRT_RELIC_SCALE 0x400 /* the load/save boxes' relic scale: tilted, ~29 units tall and ~19 wide (units are 512x216 display pixels at NRT_DEPTH) */
/* The token disc is 64 units across at scale 0x1000 against the relic's 127 tall, so the same scale draws it half the relic's size; the
 * HUD's scale-per-depth ratio between the two is no guide to that. 0x700 makes a 28-unit disc, about the tilted relic's height. */
#define NRT_TOKEN_SCALE 0x700
#define NRT_SCREEN_X    0x100 /* screen centre; at depth NRT_DEPTH one translation unit is one pixel */
#define NRT_SCREEN_Y    0x6c
#define NRT_DEPTH       0x100
#define NRT_SPIN_STEP   0x40
#define NRT_RELIC_TILT  0x155 /* the load/save boxes' relic tilt */
#define NRT_TOKEN_TILT  0     /* the token stands upright as on Adventure's warp pads */

static struct Instance *s_nrtIcon[NRT_SLOTS];
static char s_nrtRelicName[16] = "trackrelic";
static char s_nrtTokenName[16] = "tracktoken";
static s16 s_nrtSpin = 0;

static const u32 s_nrtTierColor[NAM_RELIC_TIERS] = {INST_COLOR_SAPPHIRE_RELIC, INST_COLOR_GOLD_RELIC, INST_COLOR_PLATINUM_RELIC};

/* Adventure's lightDirToken (D232.lightDirToken; all five token groups share it) */
static const SVec3 s_nrtTokenLight = {{-0xA0A, 0x787, 0xA0A}};

static struct Instance *NativeArcade_TrackIconGet(s32 slot, s32 modelID)
{
	struct GameTracker *gGT = sdata->gGT;
	struct Model *model = gGT->modelPtr[modelID];
	struct InstDrawPerPlayer *idpp;
	struct Instance *inst = s_nrtIcon[slot];
	s16 scale = (modelID == STATIC_TOKEN) ? NRT_TOKEN_SCALE : NRT_RELIC_SCALE;

	if ((inst != NULL) || (model == NULL))
	{
		return inst;
	}

	inst = INSTANCE_Birth3D(model, (modelID == STATIC_TOKEN) ? s_nrtTokenName : s_nrtRelicName, NULL);

	if (inst == NULL)
	{
		return NULL;
	}

	/* a token is translucent and specular lit, as Adventure's HUD and warp pad draw it */
	inst->flags |= HIDE_MODEL | SCREENSPACE_INSTANCE | USE_SPECULAR_LIGHT | ((modelID == STATIC_TOKEN) ? DRAW_TRANSPARENT : 0);
	inst->scale.x = scale;
	inst->scale.y = scale;
	inst->scale.z = scale;

	idpp = INST_GETIDPP(inst);
	idpp[0].pushBuffer = &gGT->pushBuffer_UI;

	for (s32 player = 1; player < gGT->numPlyrCurrGame; player++)
	{
		idpp[player].pushBuffer = NULL;
	}

	s_nrtIcon[slot] = inst;
	return inst;
}

#endif /* __3DS__ */

int NativeRelic_TierOfTrack(s32 levID)
{
#ifdef __3DS__
	if ((levID < 0) || (levID >= MEMCARD_HIGH_SCORE_TRACK_COUNT))
	{
		return 0;
	}

	for (s32 tier = NAM_RELIC_TIERS - 1; tier >= 0; tier--)
	{
		if (NativeRelic_Earned(levID, tier))
		{
			return tier + 1;
		}
	}
#else
	(void)levID;
#endif

	return 0;
}

int NativeCtrToken_Earned(s32 levID)
{
#ifdef __3DS__
	if ((levID < 0) || (levID >= NCT_LEVELS))
	{
		return 0;
	}

	if (NativeCard_AnyProfileHasBit(ADV_REWARD_FIRST_CTR_TOKEN + levID))
	{
		return 1;
	}

	NativeCtrTokens_Ensure(0);
	return ((s_nctMask >> levID) & 1) != 0;
#else
	(void)levID;
	return 0;
#endif
}

void NativeArcade_OnCtrWin(void)
{
#ifdef __3DS__
	struct GameTracker *gGT = sdata->gGT;
	s32 level = gGT->levelID;

	/* a cheat that can change the race records no win (the relic and best-ghost rule) */
	if (!NativeArcade_CtrChallenge() || (level < 0) || (level >= NCT_LEVELS) || ((gGT->gameMode2 & NATIVE_GHOST_BEST_BLOCKING_CHEATS) != 0))
	{
		return;
	}

	NativeCtrTokens_Ensure(0);

	if (((s_nctMask >> level) & 1) != 0)
	{
		return;
	}

	s_nctMask |= (u16)(1u << level);
	s_nctDirty = 1;
	Platform_LogInstrument("[CTRTOKENS] win level=%d mask=%04x\n", (int)level, (unsigned)s_nctMask);
#endif
}

void NativeCtrTokens_Flush(void)
{
#ifdef __3DS__
	char name[NCT_NAME_BYTES];
	u8 icon[NCT_ICON_BYTES];
	u8 payload[NCT_PAYLOAD_BYTES];
	enum NativeMemcardResult result;

	if (s_nctDirty == 0)
	{
		return;
	}

	/* a failed first read is retried here: its merge keeps wins recorded since, and a success lifts the refusal */
	NativeCtrTokens_Ensure(1);

	if (s_nctState != NCT_READY)
	{
		Platform_LogInstrument("[CTRTOKENS] write refused: the file could not be read\n");
		return;
	}

	NativeCtrTokens_SaveName(name);
	memset(icon, 0, sizeof(icon));
	NativeCtrTokens_Pack(payload, s_nctMask);
	result = NativeMemcard_WriteSaveData(name, icon, NCT_ICON_BYTES, payload, NCT_PAYLOAD_BYTES);

	if (result == NATIVE_MEMCARD_OK)
	{
		s_nctDirty = 0;
	}

	Platform_LogInstrument("[CTRTOKENS] write mask=%04x %s\n", (unsigned)s_nctMask, (result == NATIVE_MEMCARD_OK) ? "ok" : "FAILED");
#endif
}

void NativeArcade_TrackIconsInit(void)
{
#ifdef __3DS__
	/* A level load resets the instance pool, so a pointer left from the last visit can be stale: forget it, never free it */
	for (s32 slot = 0; slot < NRT_SLOTS; slot++)
	{
		s_nrtIcon[slot] = NULL;
	}

	/* CTR CHALLENGE's marks read the token file: read it here, once per visit, rather than mid-frame (a failed read is retried) */
	if (g_nativeArcadeMode == NATIVE_ARCADE_CTR)
	{
		NativeCtrTokens_Ensure(1);
	}
#endif
}

void NativeArcade_TrackIconsBegin(void)
{
#ifdef __3DS__
	for (s32 slot = 0; slot < NRT_SLOTS; slot++)
	{
		if (s_nrtIcon[slot] != NULL)
		{
			s_nrtIcon[slot]->flags |= HIDE_MODEL;
		}
	}

	s_nrtSpin = (s16)(s_nrtSpin + NRT_SPIN_STEP);
#endif
}

void NativeArcade_TrackIconsRow(s32 slot, s32 levID, const RECT16 *row)
{
#ifdef __3DS__
	struct Instance *inst;
	const SVec3 *light;
	SVec3 rot;
	s32 modelID;

	/* the mode alone, not NativeArcade_RelicRace(): the RELIC_RACE bit is only added when the race is queued */
	if (((g_nativeArcadeMode != NATIVE_ARCADE_RELIC) && (g_nativeArcadeMode != NATIVE_ARCADE_CTR)) ||
	    ((sdata->gGT->gameMode1 & (BATTLE_MODE | TIME_TRIAL)) != 0) || (slot < 0) || (slot >= NRT_SLOTS))
	{
		return;
	}

	if (g_nativeArcadeMode == NATIVE_ARCADE_RELIC)
	{
		int tier = NativeRelic_TierOfTrack(levID);

		if (tier == 0)
		{
			return;
		}

		modelID = STATIC_RELIC;
		inst = NativeArcade_TrackIconGet(slot, modelID);

		if (inst == NULL)
		{
			return;
		}

		rot.z = NRT_RELIC_TILT;
		light = &data.MetaDataLoadSave[0].vec3_specular_inverted;
		inst->colorRGBA = s_nrtTierColor[tier - 1];
	}
	else
	{
		if (!NativeCtrToken_Earned(levID))
		{
			return;
		}

		modelID = STATIC_TOKEN;
		inst = NativeArcade_TrackIconGet(slot, modelID);

		if (inst == NULL)
		{
			return;
		}

		rot.z = NRT_TOKEN_TILT;
		light = &s_nrtTokenLight;
		inst->colorRGBA = NativeCtrToken_TintOf(levID);
	}

	rot.x = 0;
	rot.y = s_nrtSpin;

	ConvertRotToMatrix(&inst->matrix, &rot);
	inst->matrix.t[0] = row->x + row->w + NRT_ICON_X - NRT_SCREEN_X;
	inst->matrix.t[1] = row->y + (row->h >> 1) - NRT_SCREEN_Y;
	inst->matrix.t[2] = NRT_DEPTH;
	Vector_SpecLightSpin3D(inst, &rot, light);

	inst->flags &= ~HIDE_MODEL;
#else
	(void)slot;
	(void)levID;
	(void)row;
#endif
}

void NativeArcade_TrackIconsEnd(void)
{
#ifdef __3DS__
	for (s32 slot = 0; slot < NRT_SLOTS; slot++)
	{
		if (s_nrtIcon[slot] != NULL)
		{
			INSTANCE_Death(s_nrtIcon[slot]);
			s_nrtIcon[slot] = NULL;
		}
	}
#endif
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
		/* ARCADE_MODE goes for the race: the end-of-race overlay and results screen are chosen ARCADE_MODE first,
		 * which would hide the relic results (retail relic races never carry it). */
		sdata->Loading.OnBegin.AddBitsConfig0 |= RELIC_RACE;
		sdata->Loading.OnBegin.RemBitsConfig0 |= ARCADE_MODE;
	}
#endif
}

void NativeArcade_OnRaceStart(void)
{
#ifdef __3DS__
	/* Once per race world: after a load and after RETRY (LOAD_RESTART), before the HUD reads the bits */
	if (NativeArcade_RelicRace())
	{
		NativeRelic_Prepare();
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
