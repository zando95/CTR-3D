#ifndef NATIVE_LNG_H
#define NATIVE_LNG_H

/* String-table slots for native menu text (3DS only).
 *
 * Native menus name their rows by string index, as retail's do, so their text has to be in
 * sdata->lngStrings. It cannot go in the retail table's empty slots: the memory card message
 * screens (SelectProfile, data.messageScreens) each draw the nine strings from a base index and
 * skip only the empty ones, so text in an empty slot inside such a block shows on that screen.
 * Native text gets slots past the end of the retail table instead. After every language load,
 * NativeLng_Extend copies the file's string pointers into a longer table and points
 * sdata->lngStrings at it. The native entries start as "" and keep whatever their owners last
 * wrote across language loads; owners rewrite them for the new language.
 *
 * On PC the table is retail's and every native slot is out of range, so NativeLng_Ready is 0.
 * Defined in platform/native_lng.c. */

enum NativeLngSlot
{
	NATIVE_LNG_BASE = 588, /* the string count of all eight retail language files */

	NATIVE_LNG_OPTIONS_TITLE = NATIVE_LNG_BASE,
	NATIVE_LNG_OPT_PENTA_PAL,
	NATIVE_LNG_OPT_RESERVES,
	NATIVE_LNG_OPT_BOTTOM_MAP,
	NATIVE_LNG_OPT_BEST_GHOST,
	NATIVE_LNG_OPT_TEXTURE_FILTER,
	NATIVE_LNG_OPT_LANGUAGE,
	NATIVE_LNG_BOSS_RACES,
	NATIVE_LNG_SUPER_HARD,
	NATIVE_LNG_ULTRA_HARD,
	NATIVE_LNG_CUP_PROMPT,

	NATIVE_LNG_END,
};

/* LOAD_LangFile, once it has built the loaded file's table. */
void NativeLng_Extend(void);

/* The native slots exist in the current table. */
int NativeLng_Ready(void);

#endif
