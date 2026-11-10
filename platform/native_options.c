/* Native options menu and main-menu row filter.
 *
 * game/230/MM_MenuFlow.c and game/MAIN/MainMain.c call the three NativeOptions_*
 * hooks at the bottom of this file. On PC every hook is a passthrough/no-op --
 * the retail main menu must stay untouched there (replay inputs navigate menus
 * by position, so PC row layout is behavioral surface). On 3DS the main menu
 * loses VS and BATTLE (one screen, one pad -- both modes are dead ends) and
 * gains an OPTIONS row opening a RectMenu of runtime toggles; each row writes
 * the same global its dumpreq word writes, so the menu and dumpreq stay
 * interchangeable. A last QUIT row opens a YES/NO confirm whose YES takes the
 * same exit as a HOME-menu Close (N3DS_RequestQuit). Once mirror mode is unlocked, a MIRROR row
 * above OPTIONS switches it in place (include/native_mirror.h); that switch is never saved.
 *
 * Menu flips persist across launches: the toggles in <base dir>/options.cfg (one `key value`
 * line per s_optToggles row, keyed by its saveKey), the language in <base dir>/language.cfg.
 * Only a menu flip saves; dumpreq writes the same globals and never touches either file.
 *
 * Row labels live in the native string slots past the retail table (include/native_lng.h),
 * repointed at boot, on every toggle, and again after every language switch. Their text comes
 * from s_optText, one entry per language. s_optMenu is not checkpoint-tracked (native_checkpoint.c
 * relocates only its enumerated regions); acceptable because the PC build, the
 * only one with F5/F8 checkpoints, never links this menu into the hierarchy.
 */

/* Master switch of Retro-Fueled mode (include/native_retro_fueled.h). The mode also needs the ARROW
 * cheat, so this only decides whether the cheat brings it along. ON on 3DS; PC stays retail. Nothing
 * writes it at runtime. */
#ifdef __3DS__
int g_retroFueled = 1;
#else
int g_retroFueled = 0;
#endif

#ifdef __3DS__

extern int g_pentaPal;      /* platform/native_gpu.c, dumpreq `pentapal` */
extern int g_reservesMeter; /* platform/native_gpu.c, dumpreq `reserves` */
extern int g_dbg_btmMap;    /* platform/n3ds/native_renderer_3ds.c, dumpreq `btmmap` */
/* platform/native_ghost_best.c; has no dumpreq word */
extern int g_ghostBestEnabled;
/* platform/native_random_cpu.c; has no dumpreq word */
extern int g_randomCpus;
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
/* Both live in the n3ds_backend TU; the flag is plain int and the call is
 * void(void), so they cross the scalar boundary without header help. */
extern int g_dbg_trackBilinear; /* dumpreq `bilinear` */
void N3DS_TrackAtlas_ApplyFilter(void);
#endif

struct NativeOptToggle
{
	s16 lngSlot;
	int *flag;
	void (*applyFn)(void); /* extra work a flip needs beyond the flag write (also run once after options.cfg loads) */
	const char *saveKey;   /* the row's key in options.cfg: the dumpreq word of the same setting, where one exists */
};

/* Row order here is the order of NativeOptText.toggleName below. The visible text of every row
 * is built by NativeOptions_RefreshLabels from the selected language's table.
 *
 * The box is re-measured from the live strings every frame and DecalFont is monospace (every
 * plain character, space included, advances font_charPixWidth; only button glyphs and ':'/'.'
 * differ) -- so the ON and OFF versions of a row MUST have the same character count or the box
 * resizes on toggle. RefreshLabels right-aligns the state word to the longer of the two. Keep
 * labels short enough (small font) that the box clears the screen edge at the main menu's
 * right-of-center anchor: 18 characters is the widest row that is known to fit.
 *
 * Every row is persisted to options.cfg under its saveKey (a flag holds 0 or 1); saveKeys must be
 * unique and stay stable, since they name the setting in files already on players' SD cards. */
static struct NativeOptToggle s_optToggles[] = {
    {NATIVE_LNG_OPT_PENTA_PAL, &g_pentaPal, NULL, "pentapal"},
    {NATIVE_LNG_OPT_RESERVES, &g_reservesMeter, NULL, "reserves"},
    {NATIVE_LNG_OPT_BOTTOM_MAP, &g_dbg_btmMap, NULL, "btmmap"},
    {NATIVE_LNG_OPT_BEST_GHOST, &g_ghostBestEnabled, NULL, "bestghost"},
    {NATIVE_LNG_OPT_RANDOM_CPUS, &g_randomCpus, NULL, "randomcpus"},
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
    {NATIVE_LNG_OPT_TEXTURE_FILTER, &g_dbg_trackBilinear, N3DS_TrackAtlas_ApplyFilter, "bilinear"},
#endif
};
#define OPT_TOGGLE_COUNT ((s32)(sizeof(s_optToggles) / sizeof(s_optToggles[0])))

/* Language selector: the row right after the toggles (NATIVE_LNG_OPT_LANGUAGE). Confirm cycles it.
 *
 * File indices are LOAD_LangFile's: 0 ja, 1 en (NTSC-U), 2 en2 (PAL), 3 fr, 4 de, 5 it, 6 es, 7 nl.
 * Entry 0 is the NTSC-U English file the game boots with, so the default is exactly the old
 * behavior. ja (no Japanese font here) and en2 (a second English) are not offered.
 * Every label is 18 characters -- the width of "RESERVES METER OFF" -- so the box never
 * resizes when the selection changes (see the monospace note above s_optToggles). */
#define OPT_LANG_COUNT 6
#define OPT_LANG_SAVE_FILE "language.cfg"

static const s16 s_optLangFile[OPT_LANG_COUNT] = {1, 3, 4, 5, 6, 7};

/* Every text the OPTIONS menu shows, per language, indexed like s_optLangFile: English, French,
 * German, Italian, Spanish, Dutch.
 *
 * Text rules: uppercase ASCII only (the PAL font has no accented letters). The one exception is
 * "\003", the PAL n-tilde prefix that DecalFont.c draws over the NEXT letter; it takes no room,
 * and it is only drawn while a PAL language file is loaded, so English must never contain it.
 * A row (name + space + state word) should stay within 18 visible characters.
 *
 * English is the original text, character for character. The other five are translations; edit
 * them here. Language names are written in their own language everywhere else (so you can still
 * find your language in a menu you cannot read), which is why these rows also fit in 18.
 */
#define OPT_TEXT_TOGGLES 6

struct NativeOptText
{
	const char *title;                         /* the main-menu row that opens OPTIONS */
	const char *toggleName[OPT_TEXT_TOGGLES];  /* same order as s_optToggles */
	const char *stateOff;
	const char *stateOn;
	const char *languageName;                  /* label of the language row */
	const char *mirrorName;                    /* the main menu's MIRROR row, before the state word */
	const char *langNames[OPT_LANG_COUNT];     /* how each language is written in this menu */
	s32 langNameWidth;                         /* the language name is right-aligned to this */
};

static const struct NativeOptText s_optText[OPT_LANG_COUNT] = {
    /* English (NTSC-U file 1): unchanged */
    {"OPTIONS",
     {"SUPER PENTA", "RESERVES METER", "BOTTOM MAP", "AUTOSAVE GHOST", "RANDOM CPUS", "TEXTURE FILTER"},
     "OFF",
     "ON",
     "LANGUAGE",
     "MIRROR",
     {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "DUTCH"},
     9},
    /* French */
    {"OPTIONS",
     {"SUPER PENTA", "JAUGE RESERVE", "CARTE EN BAS", "SAUV. FANTOME", "CPU ALEATOIRES", "FILTRE TEXTURE"},
     "NON",
     "OUI",
     "LANGUE",
     "MIROIR",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* German */
    {"OPTIONEN",
     {"SUPER PENTA", "RESERVEANZEIGE", "KARTE UNTEN", "AUTOSAVE GEIST", "ZUFALLSGEGNER", "TEXTURFILTER"},
     "AUS",
     "EIN",
     "SPRACHE",
     "SPIEGEL",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* Italian */
    {"OPZIONI",
     {"SUPER PENTA", "BARRA RISERVA", "MAPPA IN BASSO", "SALVA FANTASMA", "CPU CASUALI", "FILTRO TEXTURE"},
     "NO",
     "SI",
     "LINGUA",
     "SPECCHIO",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* Spanish */
    {"OPCIONES",
     {"SUPER PENTA", "MEDIDOR RESERVA", "MAPA INFERIOR", "FANTASMA AUTO", "CPU ALEATORIOS", "FILTRO TEXTURAS"},
     "NO",
     "SI",
     "IDIOMA",
     "ESPEJO",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* Dutch */
    {"OPTIES",
     {"SUPER PENTA", "RESERVEMETER", "KAART ONDER", "SPOOK OPSLAAN", "WILLEKEUR CPU", "TEXTUURFILTER"},
     "UIT",
     "AAN",
     "TAAL",
     "SPIEGEL",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
};

/* The results-screen line announcing N. Oxide as unlocked (game/224.c), per language in the order
 * of s_optLangFile. The disc has no "unlocked" phrase in any language, only N. Tropy's "opened"
 * line. Same text rules as s_optText; Spanish opens with '#', the PAL font's inverted "!". The
 * big font fits 24 characters on one line. */
static const char *const s_optOxideUnlocked[OPT_LANG_COUNT] = {
    "N. OXIDE UNLOCKED!",
    "N. OXIDE DEBLOQUE !",
    "N. OXID FREIGESCHALTET!",
    "N. OXIDE SBLOCCATO!",
    "#N. OXIDE DESBLOQUEADO!",
    "N. OXIDE ONTGRENDELD!",
};

/* The label strings the string table points at. RefreshLabels rewrites them in place. */
#define OPT_LABEL_MAX 48
static char s_optLabelBuf[OPT_TOGGLE_COUNT + 1][OPT_LABEL_MAX];
static char s_optMirrorLabel[OPT_LABEL_MAX];

/* Characters that take room on screen: the PAL tilde prefix (0x03) does not. */
static s32 NativeOptions_VisibleLen(const char *text)
{
	s32 len = 0;

	for (; *text != 0; text++)
	{
		if (*text != 3)
		{
			len++;
		}
	}

	return len;
}

/* dst = name + " " + word, with the word right-aligned in `width` visible characters. */
static void NativeOptions_Compose(char *dst, s32 dstSize, const char *name, const char *word, s32 width)
{
	s32 n = 0;
	s32 pad = width - NativeOptions_VisibleLen(word);

	for (; (*name != 0) && (n < dstSize - 1); name++)
	{
		dst[n++] = *name;
	}

	if (n < dstSize - 1)
	{
		dst[n++] = ' ';
	}

	for (; (pad > 0) && (n < dstSize - 1); pad--)
	{
		dst[n++] = ' ';
	}

	for (; (*word != 0) && (n < dstSize - 1); word++)
	{
		dst[n++] = *word;
	}

	dst[n] = 0;
}

static s32 s_optLangIndex = 0;

void NativeOptions_InstallStrings(void);

/* The chosen language is remembered in <base dir>/language.cfg (a single number: the file index
 * above), beside memcards/. The toggles are remembered separately, in options.cfg (below). */
static s32 NativeOptions_LangSavePath(char *path, size_t pathSize)
{
	return NativePath_Join(path, pathSize, NativeStr8_FromCString(NativeAssets_GetBaseDir()), NATIVE_STR8_LIT(OPT_LANG_SAVE_FILE));
}

/* Written by the save writer, so the menu never waits on the SD card. A failed write (read-only or
 * missing SD) leaves the language applied for this session only. */
static void NativeOptions_SaveLanguage(void)
{
	char path[1024];
	char text[16];
	int len;

	if (!NativeOptions_LangSavePath(path, sizeof(path)))
	{
		return;
	}

	len = snprintf(text, sizeof(text), "%d\n", (int)s_optLangFile[s_optLangIndex]);
	if ((len > 0) && (len < (int)sizeof(text)))
	{
		NativeMemcard_QueueFileWrite(path, (const unsigned char *)text, len);
	}
}

/* Returns the index into s_optLangFile that language.cfg names, or -1. */
static s32 NativeOptions_ReadSavedLanguage(void)
{
	char path[1024];
	FILE *file;
	int fileIndex = -1;

	if (!NativeOptions_LangSavePath(path, sizeof(path)))
	{
		return -1;
	}

	file = fopen(path, "rb");
	if (file == NULL)
	{
		return -1;
	}

	if (fscanf(file, "%d", &fileIndex) != 1)
	{
		fileIndex = -1;
	}
	fclose(file);

	for (s32 i = 0; i < OPT_LANG_COUNT; i++)
	{
		if (s_optLangFile[i] == fileIndex)
		{
			return i;
		}
	}

	return -1;
}

/* Every selectable language file, read once at boot (NativeOptions_PreloadLanguages) and handed to
 * LOAD_LangFile from memory (NativeOptions_LangFileFromMemory). A switch in OPTIONS must not read the
 * disc image: on the console that read, plus the voice manifest probe, stalled the menu for as long
 * as the SD card took. A language missing here (allocation or read failure) still reads the disc. */
static u8 *s_optLangCache[OPT_LANG_COUNT];
static u32 s_optLangCacheSize[OPT_LANG_COUNT];

unsigned long long N3DS_GetTickCount(void);
unsigned long long N3DS_GetTickFrequency(void);

static void NativeOptions_PreloadLanguages(void)
{
	struct BigHeader *bigfile = (struct BigHeader *)sdata->ptrBigfile1;
	struct BigEntry *entries;
	const unsigned long long t0 = N3DS_GetTickCount();
	const unsigned long long freq = N3DS_GetTickFrequency();
	int cached = 0;

	if (bigfile == NULL)
	{
		return;
	}
	entries = BIG_GETENTRY(bigfile);

	for (s32 i = 0; i < OPT_LANG_COUNT; i++)
	{
		const int fileIndex = s_optLangFile[i];
		const u32 readSize = ((u32)entries[BI_LANGUAGEFILE + fileIndex].size + LOAD_CD_DATA_SECTOR_ROUND_MASK) & ~LOAD_CD_DATA_SECTOR_ROUND_MASK;
		u32 size = 0;
		u8 *buf;

		NativeAudio_PreloadVoiceManifest(fileIndex);

		if ((s_optLangCache[i] != NULL) || (readSize == 0))
		{
			continue;
		}
		buf = (u8 *)malloc(readSize);
		if (buf == NULL)
		{
			continue;
		}
		if (LOAD_ReadFile_ex(bigfile, LT_SETADDR, BI_LANGUAGEFILE + fileIndex, buf, &size, NULL) == NULL)
		{
			free(buf);
			continue;
		}
		s_optLangCache[i] = buf;
		s_optLangCacheSize[i] = readSize;
		cached++;
	}

	Platform_LogInstrument("[LANGPRE] %d/%d language files cached in %ums\n", cached, OPT_LANG_COUNT,
	                       (unsigned)(((N3DS_GetTickCount() - t0) * 1000ull) / ((freq != 0ull) ? freq : 1ull)));
}

static void NativeOptions_ApplyLanguage(s32 langIndex)
{
	s_optLangIndex = langIndex;
	LOAD_LangFile((int)sdata->ptrBigfile1, s_optLangFile[s_optLangIndex]);

	/* the labels are per language: write the new language's text into the native slots */
	NativeOptions_InstallStrings();
}

static void NativeOptions_CycleLanguage(void)
{
	NativeOptions_ApplyLanguage((s_optLangIndex + 1) % OPT_LANG_COUNT);
	NativeOptions_SaveLanguage();
}

/* The toggles are remembered in <base dir>/options.cfg, beside language.cfg: one `key value` line
 * per s_optToggles row (the row's saveKey, then 0 or 1), the whole file rewritten on every menu
 * flip. A missing file, an unknown key, a malformed line or a value other than 0 or 1 leaves that
 * toggle at its compiled default. */
#define OPT_SETTINGS_SAVE_FILE "options.cfg"

static s32 NativeOptions_SettingsPath(char *path, size_t pathSize)
{
	return NativePath_Join(path, pathSize, NativeStr8_FromCString(NativeAssets_GetBaseDir()), NATIVE_STR8_LIT(OPT_SETTINGS_SAVE_FILE));
}

/* Written by the save writer, like language.cfg. A failed write leaves the flip applied for this
 * session only. */
static void NativeOptions_SaveSettings(void)
{
	char path[1024];
	char text[512];
	int len = 0;

	if (!NativeOptions_SettingsPath(path, sizeof(path)))
	{
		return;
	}

	for (s32 i = 0; i < OPT_TOGGLE_COUNT; i++)
	{
		int n = snprintf(text + len, sizeof(text) - (size_t)len, "%s %d\n", s_optToggles[i].saveKey, (*s_optToggles[i].flag != 0) ? 1 : 0);

		if ((n < 0) || (n >= (int)sizeof(text) - len))
		{
			return;
		}
		len += n;
	}

	NativeMemcard_QueueFileWrite(path, (const unsigned char *)text, len);
}

/* Applies one options.cfg line ("key 0" or "key 1", LF/CRLF optional) to the toggle whose saveKey
 * it names; any other line is ignored. */
static void NativeOptions_ParseSettingsLine(const char *line)
{
	for (s32 i = 0; i < OPT_TOGGLE_COUNT; i++)
	{
		size_t keyLen = strlen(s_optToggles[i].saveKey);

		if ((strncmp(line, s_optToggles[i].saveKey, keyLen) == 0) && (line[keyLen] == ' ') && ((line[keyLen + 1] == '0') || (line[keyLen + 1] == '1')) &&
		    ((line[keyLen + 2] == 0) || (line[keyLen + 2] == '\n') || (line[keyLen + 2] == '\r')))
		{
			*s_optToggles[i].flag = line[keyLen + 1] - '0';
			return;
		}
	}
}

static void NativeOptions_LoadSettings(void)
{
	char path[1024];
	char line[64];
	FILE *file;

	if (!NativeOptions_SettingsPath(path, sizeof(path)))
	{
		return;
	}

	file = fopen(path, "rb");
	if (file == NULL)
	{
		return;
	}

	while (fgets(line, sizeof(line), file) != NULL)
	{
		NativeOptions_ParseSettingsLine(line);

		/* a line longer than the buffer comes back in pieces; its tail is not a line of its own */
		if (strchr(line, '\n') == NULL)
		{
			int c;

			do
			{
				c = fgetc(file);
			} while ((c != EOF) && (c != '\n'));
		}
	}

	fclose(file);
}

/* Nav targets must mirror s_optToggles row for row; the terminator is not a
 * valid nav target, so the last real row's down points at itself. */
static struct MenuRow s_optMenuRows[] = {
    {NATIVE_LNG_OPT_PENTA_PAL, 0, 1, 0, 0},
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
    {NATIVE_LNG_OPT_RESERVES, 0, 2, 1, 1},
    {NATIVE_LNG_OPT_BOTTOM_MAP, 1, 3, 2, 2},
    {NATIVE_LNG_OPT_BEST_GHOST, 2, 4, 3, 3},
    {NATIVE_LNG_OPT_RANDOM_CPUS, 3, 5, 4, 4},
    {NATIVE_LNG_OPT_TEXTURE_FILTER, 4, 6, 5, 5},
    {NATIVE_LNG_OPT_LANGUAGE, 5, 6, 6, 6}, /* language selector = row OPT_TOGGLE_COUNT */
#else
    {NATIVE_LNG_OPT_RESERVES, 0, 2, 1, 1},
    {NATIVE_LNG_OPT_BOTTOM_MAP, 1, 3, 2, 2},
    {NATIVE_LNG_OPT_BEST_GHOST, 2, 4, 3, 3},
    {NATIVE_LNG_OPT_RANDOM_CPUS, 3, 5, 4, 4},
    {NATIVE_LNG_OPT_LANGUAGE, 4, 5, 5, 5}, /* language selector = row OPT_TOGGLE_COUNT */
#endif
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static void NativeOptions_RefreshLabels(void)
{
	const struct NativeOptText *text = &s_optText[s_optLangIndex];
	s32 stateWidth = NativeOptions_VisibleLen(text->stateOff);

	if (!NativeLng_Ready())
	{
		return;
	}

	if (NativeOptions_VisibleLen(text->stateOn) > stateWidth)
	{
		stateWidth = NativeOptions_VisibleLen(text->stateOn);
	}

	for (s32 i = 0; i < OPT_TOGGLE_COUNT; i++)
	{
		NativeOptions_Compose(s_optLabelBuf[i], OPT_LABEL_MAX, text->toggleName[i], *s_optToggles[i].flag ? text->stateOn : text->stateOff, stateWidth);
		sdata->lngStrings[s_optToggles[i].lngSlot] = s_optLabelBuf[i];
	}

	NativeOptions_Compose(s_optLabelBuf[OPT_TOGGLE_COUNT], OPT_LABEL_MAX, text->languageName, text->langNames[s_optLangIndex], text->langNameWidth);
	sdata->lngStrings[NATIVE_LNG_OPT_LANGUAGE] = s_optLabelBuf[OPT_TOGGLE_COUNT];

	NativeOptions_Compose(s_optMirrorLabel, OPT_LABEL_MAX, text->mirrorName, g_mirrorMode ? text->stateOn : text->stateOff, stateWidth);
	sdata->lngStrings[NATIVE_LNG_MIRROR] = s_optMirrorLabel;
}

static void NativeOptions_MenuProc(struct RectMenu *menu)
{
	/* The engine also invokes funcPtr outside input events; only a real
	 * Cross/Triangle event (funcState INPUT) may toggle, or a held frame
	 * would flip the row every vblank. */
	if (menu->funcState != RECTMENU_FUNC_STATE_INPUT)
	{
		return;
	}

	s16 row = menu->rowSelected;

	/* rowSelected -1 is the engine's back/close signal */
	if (row == -1)
	{
		menu->ptrPrevBox_InHierarchy->state &= ~(ONLY_DRAW_TITLE | DRAW_NEXT_MENU_IN_HIERARCHY);
		return;
	}

	if ((row >= 0) && (row < OPT_TOGGLE_COUNT))
	{
		*s_optToggles[row].flag ^= 1;

		if (s_optToggles[row].applyFn != NULL)
		{
			s_optToggles[row].applyFn();
		}

		NativeOptions_SaveSettings();
		NativeOptions_RefreshLabels();
	}
	else if (row == OPT_TOGGLE_COUNT)
	{
		NativeOptions_CycleLanguage();
	}
}

/* No own title: the parent main menu already renders "OPTIONS" above the child
 * box (retail parents draw their selected row while ONLY_DRAW_TITLE). */
static struct RectMenu s_optMenu = {
    .stringIndexTitle = RECTMENU_STRING_NONE,
    .state = CENTER_ON_X | USE_SMALL_FONT,
    .rows = &s_optMenuRows[0],
    .funcPtr = NativeOptions_MenuProc,
};

/* QUIT confirm, the child box of the main menu's QUIT row. The row, YES and NO are retail's own strings
 * (the pause menu's quit confirm uses the same three), so every language file already carries them.
 * YES leaves through N3DS_RequestQuit, the same exit a HOME-menu Close takes. NO and back close the box.
 * rowSelected persists between visits, so opening the box re-selects NO (HandleMainMenuChoice). */
#define OPT_QUIT_ROW_YES 0
#define OPT_QUIT_ROW_NO  1

static struct MenuRow s_optQuitRows[] = {
    {LNG_YES, 0, 1, 0, 0},
    {LNG_NO, 0, 1, 1, 1},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static void NativeOptions_QuitMenuProc(struct RectMenu *menu)
{
	/* Same rule as NativeOptions_MenuProc: only a real input event may act. */
	if (menu->funcState != RECTMENU_FUNC_STATE_INPUT)
	{
		return;
	}

	if (menu->rowSelected == OPT_QUIT_ROW_YES)
	{
		N3DS_RequestQuit();
		return;
	}

	/* NO, or rowSelected -1 (the engine's back signal) */
	if ((menu->rowSelected == OPT_QUIT_ROW_NO) || (menu->rowSelected == -1))
	{
		menu->ptrPrevBox_InHierarchy->state &= ~(ONLY_DRAW_TITLE | DRAW_NEXT_MENU_IN_HIERARCHY);
	}
}

/* No own title: the main menu draws the selected row ("QUIT") above the child box. */
static struct RectMenu s_optQuitMenu = {
    .stringIndexTitle = RECTMENU_STRING_NONE,
    .state = CENTER_ON_X | USE_SMALL_FONT,
    .rows = &s_optQuitRows[0],
    .funcPtr = NativeOptions_QuitMenuProc,
};

/* 3DS main-menu row sets: retail minus VS/BATTLE, plus OPTIONS and QUIT last. Once mirror mode is
 * unlocked, its MIRROR row sits above OPTIONS. */
static struct MenuRow s_optMainRowsBasic[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {NATIVE_LNG_OPTIONS_TITLE, 3, 5, 4, 4},
    {LNG_QUIT, 4, 5, 5, 5},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static struct MenuRow s_optMainRowsScrapbook[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {LNG_SCRAPBOOK, 3, 5, 4, 4},
    {NATIVE_LNG_OPTIONS_TITLE, 4, 6, 5, 5},
    {LNG_QUIT, 5, 6, 6, 6},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static struct MenuRow s_optMainRowsMirror[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {NATIVE_LNG_MIRROR, 3, 5, 4, 4},
    {NATIVE_LNG_OPTIONS_TITLE, 4, 6, 5, 5},
    {LNG_QUIT, 5, 6, 6, 6},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static struct MenuRow s_optMainRowsScrapbookMirror[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {LNG_SCRAPBOOK, 3, 5, 4, 4},
    {NATIVE_LNG_MIRROR, 4, 6, 5, 5},
    {NATIVE_LNG_OPTIONS_TITLE, 5, 7, 6, 6},
    {LNG_QUIT, 6, 7, 7, 7},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

#endif /* __3DS__ */

/* Called every frame by MM_MenuProc_Main after retail's own scrapbook swap, so
 * `rows` is retail's choice for this frame; returning it unchanged keeps PC
 * retail-identical. */
struct MenuRow *NativeOptions_MainMenuRows(struct MenuRow *rows)
{
#ifdef __3DS__
	s32 mirror = NativeMirror_Unlocked();

	NativeOptions_RefreshLabels();

	if (rows == &D230.rowsMainMenuWithScrapbook[0])
	{
		return mirror ? &s_optMainRowsScrapbookMirror[0] : &s_optMainRowsScrapbook[0];
	}

	return mirror ? &s_optMainRowsMirror[0] : &s_optMainRowsBasic[0];
#else
	return rows;
#endif
}

s32 NativeOptions_HandleMainMenuChoice(struct RectMenu *mainMenu, s32 choose)
{
#ifdef __3DS__
	if (choose == NATIVE_LNG_OPTIONS_TITLE)
	{
		mainMenu->ptrNextBox_InHierarchy = &s_optMenu;
		mainMenu->state |= DRAW_NEXT_MENU_IN_HIERARCHY;
		return 1;
	}

	/* a switch with no box of its own: the main menu stays as it is */
	if (choose == NATIVE_LNG_MIRROR)
	{
		g_mirrorMode ^= 1;
		NativeOptions_RefreshLabels();
		mainMenu->state &= ~ONLY_DRAW_TITLE;
		return 1;
	}

	if (choose == LNG_QUIT)
	{
		s_optQuitMenu.rowSelected = OPT_QUIT_ROW_NO;
		mainMenu->ptrNextBox_InHierarchy = &s_optQuitMenu;
		mainMenu->state |= DRAW_NEXT_MENU_IN_HIERARCHY;
		return 1;
	}
#else
	(void)mainMenu;
	(void)choose;
#endif
	return 0;
}

void NativeOptions_InstallStrings(void)
{
#ifdef __3DS__
	if (!NativeLng_Ready())
	{
		return;
	}

	sdata->lngStrings[NATIVE_LNG_OPTIONS_TITLE] = (char *)s_optText[s_optLangIndex].title;
	NativeOptions_RefreshLabels();
#endif
}

/* Boot hook (MainMain.c StateZero), called right after the default English file and the OPTIONS
 * strings are in place: restore the toggles remembered in options.cfg (running each one's applyFn
 * so its subsystem sees the loaded value), then switch to the language remembered in language.cfg,
 * if there is one. */
void NativeOptions_BootSettings(void)
{
#ifdef __3DS__
	s32 saved;

	NativeOptions_LoadSettings();

	for (s32 i = 0; i < OPT_TOGGLE_COUNT; i++)
	{
		if (s_optToggles[i].applyFn != NULL)
		{
			s_optToggles[i].applyFn();
		}
	}

	NativeOptions_RefreshLabels();

	saved = NativeOptions_ReadSavedLanguage();
	if (saved > 0)
	{
		NativeOptions_ApplyLanguage(saved);
	}

	NativeOptions_PreloadLanguages();
#endif
}

/* LOAD_LangFile asks here before reading a language file from the disc: 1 = the file was copied
 * into dst from the boot cache. Always 0 on PC, which has no language switch. */
int NativeOptions_LangFileFromMemory(int fileIndex, void *dst, u32 dstSize)
{
#ifdef __3DS__
	for (s32 i = 0; i < OPT_LANG_COUNT; i++)
	{
		if ((s_optLangFile[i] == fileIndex) && (s_optLangCache[i] != NULL) && (s_optLangCacheSize[i] <= dstSize))
		{
			memcpy(dst, s_optLangCache[i], s_optLangCacheSize[i]);
			return 1;
		}
	}
#else
	(void)fileIndex;
	(void)dst;
	(void)dstSize;
#endif
	return 0;
}

/* The N. Oxide unlock line for the selected language; PC has no language switch, so it is English. */
const char *NativeOptions_OxideUnlockedLine(void)
{
#ifdef __3DS__
	return s_optOxideUnlocked[s_optLangIndex];
#else
	return "N. OXIDE UNLOCKED!";
#endif
}

/* LOAD_Assets.c sizes the language buffer for a mid-game language file re-read only where the
 * OPTIONS menu can trigger one. */
int NativeOptions_RuntimeLanguageSwitch(void)
{
#ifdef __3DS__
	return 1;
#else
	return 0;
#endif
}
