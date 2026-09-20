/* Native options menu and main-menu row filter.
 *
 * game/230/MM_MenuFlow.c and game/MAIN/MainMain.c call the three NativeOptions_*
 * hooks at the bottom of this file. On PC every hook is a passthrough/no-op --
 * the retail main menu must stay untouched there (replay inputs navigate menus
 * by position, so PC row layout is behavioral surface). On 3DS the main menu
 * loses VS and BATTLE (one screen, one pad -- both modes are dead ends) and
 * gains an OPTIONS row opening a RectMenu of runtime toggles; each row writes
 * the same global its dumpreq word writes, so the menu and dumpreq stay
 * interchangeable.
 *
 * Row labels live in retail's own string table: LNG_UNUSED_0E4..0E9 are real
 * slots inside the loaded LANG range that no retail code references, repointed
 * at boot, on every toggle, and again after every language switch (LOAD_LangFile
 * rebuilds the whole table). Their text comes from s_optText, one entry per language. s_optMenu is not checkpoint-tracked (native_checkpoint.c
 * relocates only its enumerated regions); acceptable because the PC build, the
 * only one with F5/F8 checkpoints, never links this menu into the hierarchy.
 */

#ifdef __3DS__

extern int g_pentaPal;      /* platform/native_gpu.c, dumpreq `pentapal` */
extern int g_reservesMeter; /* platform/native_gpu.c, dumpreq `reserves` */
extern int g_dbg_btmMap;    /* platform/n3ds/native_renderer_3ds.c, dumpreq `btmmap` */
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
	void (*applyFn)(void); /* extra work a flip needs beyond the flag write */
};

/* Row order here is the order of NativeOptText.toggleName below. The visible text of every row
 * is built by NativeOptions_RefreshLabels from the selected language's table.
 *
 * The box is re-measured from the live strings every frame and DecalFont is monospace (every
 * plain character, space included, advances font_charPixWidth; only button glyphs and ':'/'.'
 * differ) -- so the ON and OFF versions of a row MUST have the same character count or the box
 * resizes on toggle. RefreshLabels right-aligns the state word to the longer of the two. Keep
 * labels short enough (small font) that the box clears the screen edge at the main menu's
 * right-of-center anchor: 18 characters is the widest row that is known to fit. */
static struct NativeOptToggle s_optToggles[] = {
    {LNG_UNUSED_0E5, &g_pentaPal, NULL},
    {LNG_UNUSED_0E6, &g_reservesMeter, NULL},
    {LNG_UNUSED_0E7, &g_dbg_btmMap, NULL},
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
    {LNG_UNUSED_0E8, &g_dbg_trackBilinear, N3DS_TrackAtlas_ApplyFilter},
#endif
};
#define OPT_TOGGLE_COUNT ((s32)(sizeof(s_optToggles) / sizeof(s_optToggles[0])))

/* Language selector: the row right after the toggles (LNG_UNUSED_0E9). Confirm cycles it.
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
#define OPT_TEXT_TOGGLES 4

struct NativeOptText
{
	const char *title;                         /* the main-menu row that opens OPTIONS */
	const char *toggleName[OPT_TEXT_TOGGLES];  /* same order as s_optToggles */
	const char *stateOff;
	const char *stateOn;
	const char *languageName;                  /* label of the language row */
	const char *langNames[OPT_LANG_COUNT];     /* how each language is written in this menu */
	s32 langNameWidth;                         /* the language name is right-aligned to this */
};

static const struct NativeOptText s_optText[OPT_LANG_COUNT] = {
    /* English (NTSC-U file 1): unchanged */
    {"OPTIONS",
     {"PAL PENTA", "RESERVES METER", "BOTTOM MAP", "TEXTURE FILTER"},
     "OFF",
     "ON",
     "LANGUAGE",
     {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "DUTCH"},
     9},
    /* French */
    {"OPTIONS",
     {"PENTA PAL", "JAUGE RESERVE", "CARTE EN BAS", "FILTRE TEXTURE"},
     "NON",
     "OUI",
     "LANGUE",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* German */
    {"OPTIONEN",
     {"PAL PENTA", "RESERVEANZEIGE", "KARTE UNTEN", "TEXTURFILTER"},
     "AUS",
     "EIN",
     "SPRACHE",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* Italian */
    {"OPZIONI",
     {"PENTA PAL", "BARRA RISERVA", "MAPPA IN BASSO", "FILTRO TEXTURE"},
     "NO",
     "SI",
     "LINGUA",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* Spanish */
    {"OPCIONES",
     {"PENTA PAL", "MEDIDOR RESERVA", "MAPA INFERIOR", "FILTRO TEXTURAS"},
     "NO",
     "SI",
     "IDIOMA",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
    /* Dutch */
    {"OPTIES",
     {"PAL PENTA", "RESERVEMETER", "KAART ONDER", "TEXTUURFILTER"},
     "UIT",
     "AAN",
     "TAAL",
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", "ESPA\003NOL", "NEDERLANDS"},
     10},
};

/* The label strings the string table points at. RefreshLabels rewrites them in place. */
#define OPT_LABEL_MAX 48
static char s_optLabelBuf[OPT_TOGGLE_COUNT + 1][OPT_LABEL_MAX];

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
 * above), beside memcards/. The other toggles deliberately reset on every launch; a language you
 * have to re-pick every boot is not a setting anyone wants. */
static s32 NativeOptions_LangSavePath(char *path, size_t pathSize)
{
	return NativePath_Join(path, pathSize, NativeStr8_FromCString(NativeAssets_GetBaseDir()), NATIVE_STR8_LIT(OPT_LANG_SAVE_FILE));
}

static void NativeOptions_SaveLanguage(void)
{
	char path[1024];
	FILE *file;

	if (!NativeOptions_LangSavePath(path, sizeof(path)))
	{
		return;
	}

	file = fopen(path, "wb");
	if (file == NULL)
	{
		return; /* read-only or missing SD: the language still applies for this session */
	}

	fprintf(file, "%d\n", (int)s_optLangFile[s_optLangIndex]);
	fclose(file);
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

static void NativeOptions_ApplyLanguage(s32 langIndex)
{
	s_optLangIndex = langIndex;
	LOAD_LangFile((int)sdata->ptrBigfile1, s_optLangFile[s_optLangIndex]);

	/* LOAD_LangFile rebuilt the string table, dropping every slot repointed at boot */
	NativeOptions_InstallStrings();
}

static void NativeOptions_CycleLanguage(void)
{
	NativeOptions_ApplyLanguage((s_optLangIndex + 1) % OPT_LANG_COUNT);
	NativeOptions_SaveLanguage();
}

/* Nav targets must mirror s_optToggles row for row; the terminator is not a
 * valid nav target, so the last real row's down points at itself. */
static struct MenuRow s_optMenuRows[] = {
    {LNG_UNUSED_0E5, 0, 1, 0, 0},
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
    {LNG_UNUSED_0E6, 0, 2, 1, 1},
    {LNG_UNUSED_0E7, 1, 3, 2, 2},
    {LNG_UNUSED_0E8, 2, 4, 3, 3},
    {LNG_UNUSED_0E9, 3, 4, 4, 4}, /* language selector = row OPT_TOGGLE_COUNT */
#else
    {LNG_UNUSED_0E6, 0, 2, 1, 1},
    {LNG_UNUSED_0E7, 1, 3, 2, 2},
    {LNG_UNUSED_0E9, 2, 3, 3, 3}, /* language selector = row OPT_TOGGLE_COUNT */
#endif
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static void NativeOptions_RefreshLabels(void)
{
	const struct NativeOptText *text = &s_optText[s_optLangIndex];
	s32 stateWidth = NativeOptions_VisibleLen(text->stateOff);

	/* every slot written below must exist in the loaded language's string table */
	if ((sdata->lngStrings == NULL) || (sdata->numLngStrings <= LNG_UNUSED_0E9))
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
	sdata->lngStrings[LNG_UNUSED_0E9] = s_optLabelBuf[OPT_TOGGLE_COUNT];
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

/* 3DS main-menu row sets: retail minus VS/BATTLE, plus OPTIONS last. */
static struct MenuRow s_optMainRowsBasic[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {LNG_UNUSED_0E4, 3, 4, 4, 4},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static struct MenuRow s_optMainRowsScrapbook[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {LNG_SCRAPBOOK, 3, 5, 4, 4},
    {LNG_UNUSED_0E4, 4, 5, 5, 5},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

#endif /* __3DS__ */

/* Called every frame by MM_MenuProc_Main after retail's own scrapbook swap, so
 * `rows` is retail's choice for this frame; returning it unchanged keeps PC
 * retail-identical. */
struct MenuRow *NativeOptions_MainMenuRows(struct MenuRow *rows)
{
#ifdef __3DS__
	NativeOptions_RefreshLabels();

	if (rows == &D230.rowsMainMenuWithScrapbook[0])
	{
		return &s_optMainRowsScrapbook[0];
	}

	return &s_optMainRowsBasic[0];
#else
	return rows;
#endif
}

s32 NativeOptions_HandleMainMenuChoice(struct RectMenu *mainMenu, s32 choose)
{
#ifdef __3DS__
	if (choose == LNG_UNUSED_0E4)
	{
		mainMenu->ptrNextBox_InHierarchy = &s_optMenu;
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
	if ((sdata->lngStrings == NULL) || (sdata->numLngStrings <= LNG_UNUSED_0E9))
	{
		return;
	}

	sdata->lngStrings[LNG_UNUSED_0E4] = (char *)s_optText[s_optLangIndex].title;
	NativeOptions_RefreshLabels();
#endif
}

/* Boot hook (MainMain.c StateZero), called right after the default English file and the OPTIONS
 * strings are in place: switch to the language remembered in language.cfg, if there is one. */
void NativeOptions_BootLanguage(void)
{
#ifdef __3DS__
	s32 saved = NativeOptions_ReadSavedLanguage();

	if (saved > 0)
	{
		NativeOptions_ApplyLanguage(saved);
	}
#endif
}
