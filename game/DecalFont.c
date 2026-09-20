#include <common.h>

#if defined(CTR_NATIVE) && (BUILD != EurRetail)

// Set by LOAD_LangFile: non-zero while a PAL language file (index >= 2) is loaded.
//
// PAL text encodes a few characters with codes that only the European font code knows how to
// draw. This NTSC-U build has neither that code nor the European font data tables, so they are
// emulated here from icons the NTSC-U font already has (the same trick the retail PAL build
// uses: existing icons, shifted / scaled / rotated):
//   0x03 -> "~" over the NEXT character (n-tilde): the underscore icon, shrunk, zero advance
//   '#'  -> inverted exclamation mark: the "!" icon rotated 180 degrees
//   '$'  -> inverted question mark:    the "?" icon rotated 180 degrees
//   '&'  -> ordinal indicator:         the "." icon raised
//   '"'  -> quotation mark:            two apostrophe icons side by side
// With the flag clear (English, and every PC build) none of this runs and text is unchanged.
int gNativePalGlyphs = 0;

enum
{
	NATIVE_PAL_GLYPH_TILDE = 0x03,

	// icon IDs inside the font icon groups (same IDs the PAL build uses)
	NATIVE_FONT_ICON_APOSTROPHE = 0x24,
	NATIVE_FONT_ICON_EXCLAMATION = 0x25,
	NATIVE_FONT_ICON_PERIOD = 0x2c,
	NATIVE_FONT_ICON_QUESTION = 0x2e,
	NATIVE_FONT_ICON_UNDERSCORE = 0x2f,

	// angle units: 0x1000 is a full turn
	NATIVE_PAL_ROTATE_180 = 0x800,
};

// Quotation mark placement: identical to the values in CTR-ModSDK's ExpandedFont module
// (which adds these very quotation marks to NTSC-U). Indexed by fontType: DEBUG, BIG, SMALL, CREDITS.
static const s16 s_nativeQuoteOffsetX[FONT_NUM] = {0, -3, -2, -3};
static const s16 s_nativeQuoteStepX[FONT_NUM] = {0, 6, 4, 5};

// TUNING KNOBS. The PAL exe's own offsets for these glyphs are not available to this build, so
// they are derived from each icon's size at runtime. If a glyph looks a pixel or two off on
// screen, these are the numbers to change.
//   Tilde: the underscore icon is shrunk to this scale (0x1000 = 100%)...
#define NATIVE_TILDE_SCALE 0x999
//   ...and lifted so it sits (charPixHeight / this) pixels above the letter, at least 1.
#define NATIVE_TILDE_GAP_DIVISOR 8
//   Ordinal indicator: the period icon is raised by this fraction of its own height.
#define NATIVE_ORDINAL_RAISE_NUM 3
#define NATIVE_ORDINAL_RAISE_DEN 4

// Returns 1 if the character was handled here (drawn, or intentionally drawn as nothing),
// 0 to let the normal NTSC-U path deal with it.
static s32 DecalFont_NativePalGlyph(u8 c, s16 fontType, int posX, int posY, u32 *ptrColor)
{
	struct GameTracker *gGT = sdata->gGT;
	struct IconGroup *iconGroup;
	struct Icon **icons;
	struct Icon *icon = 0;
	s16 iconGroupID;
	int iconID;
	int width;
	int height;

	if ((c != NATIVE_PAL_GLYPH_TILDE) && (c != '#') && (c != '$') && (c != '&') && (c != '"'))
	{
		return 0;
	}

	if ((fontType <= 0) || (fontType >= FONT_NUM))
	{
		return 0;
	}

	iconGroupID = data.font_IconGroupID[fontType];
	iconGroup = gGT->iconGroup[iconGroupID];

	// same guard the normal path has: boot can run before every icon group is loaded
	if (iconGroup == 0)
	{
		return 0;
	}

	icons = ICONGROUP_GETICONS(iconGroup);

	switch (c)
	{
	case NATIVE_PAL_GLYPH_TILDE:
		iconID = NATIVE_FONT_ICON_UNDERSCORE;
		break;
	case '#':
		iconID = NATIVE_FONT_ICON_EXCLAMATION;
		break;
	case '$':
		iconID = NATIVE_FONT_ICON_QUESTION;
		break;
	case '&':
		iconID = NATIVE_FONT_ICON_PERIOD;
		break;
	default: // '"'
		iconID = NATIVE_FONT_ICON_APOSTROPHE;
		break;
	}

	if (iconID < iconGroup->numIcons)
	{
		icon = icons[iconID];
	}

	if (icon == 0)
	{
		return 1;
	}

	width = icon->texLayout.u1 - icon->texLayout.u0;
	height = icon->texLayout.v2 - icon->texLayout.v0;

	if (c == NATIVE_PAL_GLYPH_TILDE)
	{
		int scale = NATIVE_TILDE_SCALE;
		int gap = data.font_charPixHeight[fontType] / NATIVE_TILDE_GAP_DIVISOR;

		if (gap < 1)
		{
			gap = 1;
		}

		// centre the shrunk bar over the character that follows, and sit it above that character
		DecalHUD_DrawPolyGT4(icon, posX + ((width * (FP(1.0) - scale)) >> 13), posY - FP_Mult(height, scale) - gap, &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,
		                     ptrColor[0], ptrColor[1], ptrColor[2], ptrColor[3], 0, scale);
	}
	else if ((c == '#') || (c == '$'))
	{
		// DecalHUD_Arrow2D rotates about the icon's centre, so pass the centre of the glyph cell.
		// Vertex colours are swapped top/bottom, exactly as the PAL build does, so the gradient stays upright.
		DecalHUD_Arrow2D(icon, posX + ((width * FP(1.0)) >> 13), posY + ((height * FP(1.0)) >> 13), &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT, ptrColor[2],
		                 ptrColor[3], ptrColor[0], ptrColor[1], 0, FP(1.0), NATIVE_PAL_ROTATE_180);
	}
	else if (c == '&')
	{
		DecalHUD_DrawPolyGT4(icon, posX, posY - ((height * NATIVE_ORDINAL_RAISE_NUM) / NATIVE_ORDINAL_RAISE_DEN), &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,
		                     ptrColor[0], ptrColor[1], ptrColor[2], ptrColor[3], 0, FP(1.0));
	}
	else
	{
		int quoteX = posX + s_nativeQuoteOffsetX[fontType];

		for (int mark = 0; mark < 2; mark++, quoteX += s_nativeQuoteStepX[fontType])
		{
			DecalHUD_DrawPolyGT4(icon, quoteX, posY, &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT, ptrColor[0], ptrColor[1], ptrColor[2], ptrColor[3], 0,
			                     FP(1.0));
		}
	}

	return 1;
}

#endif

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800223f4-0x800224d0.
int DecalFont_GetLineWidthStrlen(char *character, int len, int fontType)
{
	s16 font_charPixWidth;
	s16 font_buttonPixWidth;
	s16 font_puncPixWidth;
	int pixLength;
	char c;
#if BUILD == JpnRetail
	u32 isRacingWheel;
#endif

	font_charPixWidth = data.font_charPixWidth[fontType];
	font_buttonPixWidth = data.font_buttonPixWidth[fontType];
	font_puncPixWidth = data.font_puncPixWidth[fontType];
	pixLength = 0;

	while ((*character != 0) && (len != 0))
	{
		c = *character;

		// do not use "switch" or "else if" that increases the number of bytes, and makes the function too large

		// if the character is one of the PSX buttons
		// @ is circle, [ is square, ^ is triangle, * is cross
		if ((c == '@') || (c == '[') || (c == '^') || (c == '*'))
		{
#if BUILD == JpnRetail

			isRacingWheel = DecalFont_boolRacingWheel();

			if ((isRacingWheel & 0xffff) == 0)
				pixLength += font_buttonPixWidth;
			else
				pixLength += font_charPixWidth;

#else

			// character width, plus extra spacing for button
			pixLength += font_buttonPixWidth; // + font_charPixWidth

#endif
		}

		// colon or period
		if ((c == ':') || (c == '.'))
		{
			// punctuation spacing
			pixLength += font_puncPixWidth - font_charPixWidth; // + font_charPixWidth
		}

#if BUILD > UsaRetail
		if (c == '~')
		{
			character += 2;
			len -= 2;

			// dont add charPixWidth
			goto NextIteration;
		}
#endif

// if normal character
#if BUILD != EurRetail
#if defined(CTR_NATIVE)
		// the PAL tilde diacritic (0x03) takes no room, exactly like the retail PAL build
		if ((c > 2) && !((c == NATIVE_PAL_GLYPH_TILDE) && (gNativePalGlyphs != 0)))
#else
		if (c > 2)
#endif
#else
		if (c > 3)
#endif
		{
			// normal character spacing
			// this will be added on top of button,
			// and colon, and period, so dont "else if"
			pixLength += font_charPixWidth;
		}

#if BUILD >= JpnTrial
	NextIteration:
#endif
		character++;
		len--;
	}

	return pixLength;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800224d0-0x800224fc.
int DecalFont_GetLineWidth(char *str, s16 fontType)
{
	return (s16)DecalFont_GetLineWidthStrlen(str, -1, fontType);
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800224fc-0x80022878 for the retail path.
void DecalFont_DrawLineStrlen(char *str, s16 len, int posX, s16 posY, s16 fontType, int flags)
{
	struct GameTracker *gGT = sdata->gGT;

	// text is justified left by default
	if (flags & (JUSTIFY_CENTER | JUSTIFY_RIGHT))
	{
		int alignX = DecalFont_GetLineWidthStrlen(str, len, fontType);

		if (flags & JUSTIFY_CENTER)
		{
			alignX /= 2;
		}

		posX -= alignX;
	}

// bug fix exclusive to versions after USA Retail
#if BUILD >= JpnTrial

	flags &= 0x7ff;

#else

	flags &= 0xfff;

#endif

	for (; *str != 0 && len != 0; str++, len--)
	{
		u8 *strcopy = (u8 *)str;
		u16 iconID = 0xff;
		s16 charWidth = data.font_charPixWidth[fontType];
		s16 pixWidthExtra = 0;
		s16 pixHeightExtra = 0;
		s16 iconScale = FP(1.0);

#if BUILD == EurRetail

		int numCharacters = 1;
		b32 upsideDownCharacter = false;

#endif

		u32 *ptrColor = data.ptrColor[flags];

#if defined(CTR_NATIVE) && (BUILD != EurRetail)
		if ((gNativePalGlyphs != 0) && DecalFont_NativePalGlyph(*strcopy, fontType, posX, posY, ptrColor))
		{
			// the tilde diacritic has zero advance; everything else takes a normal character cell
			if (*strcopy != NATIVE_PAL_GLYPH_TILDE)
			{
				posX += charWidth;
			}
			continue;
		}
#endif

#if BUILD >= JpnTrial

		if (*strcopy == '~')
		{
			// if the current character in the string is a tilde, delete the next two characters and color the rest of the word depending on the characters
			// being deleted used with numbers according to the color ID, e.g. ~01 gives the blue-ish gray color seen in the race lap count

			u8 *strnext = (u8 *)str + 1;
			u8 *strnextnext = (u8 *)str + 2;
			str += 2;
			len -= 2;
			flags = (*strnextnext + (*strnext - 0x30) * 10) - 0x30;
			charWidth = 0;

			continue;
		}

#endif

		if (*strcopy == ':' || *strcopy == '.')
		{
			charWidth = data.font_puncPixWidth[fontType];
		}

		// if the character is one of the PSX buttons
		// @ is circle, [ is square, ^ is triangle, * is X
		if ((((*strcopy == '@') || (*strcopy == '[')) || (*strcopy == '^')) || (*strcopy == '*'))
		{
#if BUILD != JpnRetail

			iconScale = data.font_buttonScale[fontType];
			pixHeightExtra = data.font_buttonPixHeight[fontType];
			charWidth = data.font_charPixWidth[fontType] + data.font_buttonPixWidth[fontType];

			// use neutral vertex color for button characters
			ptrColor = data.ptrColor[GRAY];

#else

			// japan retail adds support for the Mad Catz MC2 Racing Wheel (probably the best thing Naughty Dog added to this version to be honest)
			// this replaces the regular face button characters with ones that match the buttons on the steering wheel and recolors them accordingly

			s16 isRacingWheel = DecalFont_boolRacingWheel();

			if (!(isRacingWheel))
			{
				iconScale = data.font_buttonScale[fontType];
				pixHeightExtra = data.font_buttonPixHeight[fontType];
				charWidth = data.font_charPixWidth[fontType] + data.font_buttonPixWidth[fontType];

				// use neutral vertex color for button characters
				ptrColor = data.ptrColor[GRAY];
			}
			else
			{
				// note: the Mad Catz MC2 Racing Wheel does not use the traditional PSX buttons
				// it uses A, B, 1, and 2 instead
				int racingWheelButtonColor = PERIWINKLE;

				if (*strcopy == '@')
					*strcopy = 'A';
				if (*strcopy == '^')
					*strcopy = 'B';

				if (*strcopy == '*' || *strcopy == '[')
				{
					if (*strcopy == '*')
						*strcopy = '1';
					if (*strcopy == '[')
						*strcopy = '2';

					racingWheelButtonColor = RED;
				}

				ptrColor = data.ptrColor[racingWheelButtonColor];
			}

#endif
		}

		// Set character sprite (icon) IDs
		// The first 0x21 (counting 0) ASCII characters don't have icon IDs assigned to them
		// Europe has additional characters composed of existing characters rearranged to resemble new ones
		// USA Retail and prototypes have placeholder data for additional characters found in the Japanese versions

#if BUILD != EurRetail

		// ascii characters + japanese characters
		// 0xE0 characters from 0x20 to 0x100
		// TO DO: figure out why the cast to u32 is necessary --Super
		if (((u32)*strcopy - 0x21) < 0xdf)
		{
			// get iconID based on ascii character
			iconID = data.font_characterIconID[*strcopy - 0x21];
		}

		// Japanese dakuten and handakuten
		// unused in NTSC-U and PAL
		if (*strcopy < 3)
		{
			charWidth = 0;
			iconID = data.font_indentIconID[fontType * 2 + *strcopy - 1];
			pixWidthExtra = data.font_indentPixDimensions[fontType * 2];
			pixHeightExtra = data.font_indentPixDimensions[(fontType * 2) + 1];
		}

#else

		// europe only has 0x60 characters
		if (((u32)*strcopy - 0x21) < 0x5f)
		{
			// get iconID based on ascii character
			iconID = data.font_characterIconID[*strcopy - 0x21];
		}

		if (*strcopy == '')
		{
			// The tilde as a diacritical mark
			// It's implemented in CTR using the underscore character, shrunk down and placed above the character that follows
			// Seen in the Spanish language for the Ñ, although because of the above mentioned you can use it on anything

			iconID = 0x2f;
			pixWidthExtra = data.font_EurDiacriticalTilde[fontType * 3];
			pixHeightExtra = data.font_EurDiacriticalTilde[(fontType * 3) + 1];
			iconScale = data.font_EurDiacriticalTilde[(fontType * 3) + 2];
			charWidth = 0;
		}
		if (*strcopy == '"')
		{
			// Quotation mark
			// Unlike the other new characters its representing character is actually used for a quotation mark
			// Quotation marks in the PAL version are two apostrophes joined together
			iconID = 0x24;
			numCharacters = 2;
			pixWidthExtra = data.font_EurQuotationMarkWidth[fontType];
		}

		// the following characters make use of additional width and height padding
		if (*strcopy < 3 || *strcopy == '#' || *strcopy == '$' || *strcopy == '&')
		{
			s16 *characterExtraDimensionsArray = 0;

			if (*strcopy < 3)
			{
				// Japanese dakuten and handakuten
				// unused in NTSC-U and PAL
				charWidth = 0;
				iconID = data.font_indentIconID[fontType * 2 + *strcopy - 1];
				characterExtraDimensionsArray = data.font_indentPixDimensions;
			}
			if (*strcopy == '#')
			{
				// Inverted exclamation mark, used in the Spanish language
				upsideDownCharacter = true;
				iconID = 0x25;
				characterExtraDimensionsArray = data.font_EurInvertedExclamationMarkData;
			}
			if (*strcopy == '$')
			{
				// Inverted question mark, used in the Spanish language
				upsideDownCharacter = true;
				iconID = 0x2e;
				characterExtraDimensionsArray = data.font_EurInvertedExclamationMarkData;
			}
			if (*strcopy == '&')
			{
				// Upwards period
				// Supposed to be the ordinal indicator character in Spanish and French
				iconID = 0x2c;
				characterExtraDimensionsArray = data.font_EurOrdinalIndicatorData;
			}

			pixWidthExtra = characterExtraDimensionsArray[fontType * 2];
			pixHeightExtra = characterExtraDimensionsArray[(fontType * 2) + 1];
		}

#endif

		// if iconID is valid
		if (iconID != 0xff)
		{
			s16 iconGroupID = data.font_IconGroupID[fontType];

#if BUILD <= UsaRetail

			// incomplete implementation of japanese font, unused
			// see below for more details
			if (iconID > 0x7f)
			{
				iconID -= 0x80;
				s16 kanaIconGroupID = 15;
				if (iconGroupID == 4)
				{
					kanaIconGroupID = 14;
				}
				iconGroupID = kanaIconGroupID;
			}

#elif BUILD == JpnTrial || BUILD == JpnRetail

			// defined here as a fallback for if the character in question isn't kana
			u16 kanaID = iconID;

			struct Icon *iconStruct = 0;

			// if icon ID goes over 0x7f, then this is a japanese character (i.e. kana)
			if (iconID > 0x7f)
			{
				// kana icon IDs are in a separate icon group from other font characters
				u16 kanaID = iconID - 0x80;

				// the "big" and "small" font icon groups for kana are 14 and 15 respectively
				s16 kanaIconGroupID = 15;

				// if icon group is non-japanese big font, set to japanese
				if (iconGroupID == 4)
					kanaIconGroupID = 14;

				iconGroupID = kanaIconGroupID;

				iconStruct = &sdata->font_icon;

				if (kanaIconGroupID == 14)
				{
					sdata->font_icon.texLayout.clut = sdata->font_jfontBigIconData.clut;

					if (kanaID & 1)
						sdata->font_icon.texLayout.clut += 0x40;

					sdata->font_icon.texLayout.u0 = kanaID * 8 & 0xf0;
					sdata->font_icon.texLayout.u1 = sdata->font_icon.texLayout.u0 + 15;

					s16 whateverThisIs_big = kanaID / 2 & 0x10;

					sdata->font_icon.texLayout.v0 = whateverThisIs_big + 8;
					sdata->font_icon.texLayout.v2 = whateverThisIs_big + 23;

					sdata->font_icon.texLayout.tpage = sdata->font_jfontBigIconData.tpage + ((kanaID < 0x40) ^ 1);

					sdata->font_icon.texLayout.v1 = sdata->font_icon.texLayout.v0;
					sdata->font_icon.texLayout.u2 = sdata->font_icon.texLayout.u0;
					sdata->font_icon.texLayout.u3 = sdata->font_icon.texLayout.u1;
					sdata->font_icon.texLayout.v3 = sdata->font_icon.texLayout.v2;
				}
				else // i.e. small font
				{
					if (kanaID < 24)
					{
						sdata->font_icon.texLayout.clut = sdata->font_jfontSmallIconData.clut;

						if (kanaID & 1)
							sdata->font_icon.texLayout.clut += 0x40;

						sdata->font_icon.texLayout.tpage = sdata->font_jfontSmallIconData.tpage;

						s16 whateverThisIs_small = ((kanaID & 0xfe) + kanaID / 2) * 4; // this is the one

						if (kanaID < 12)
							sdata->font_icon.texLayout.u0 = whateverThisIs_small + 176;
						else
							sdata->font_icon.texLayout.u0 = whateverThisIs_small + 104;

						sdata->font_icon.texLayout.u1 = sdata->font_icon.texLayout.u0 + 11;

						if (kanaID < 12)
							sdata->font_icon.texLayout.v0 = 24;
						else
							sdata->font_icon.texLayout.v0 = 32;

						sdata->font_icon.texLayout.v2 = sdata->font_icon.texLayout.v0 + 7;

						sdata->font_icon.texLayout.v1 = sdata->font_icon.texLayout.v0;
						sdata->font_icon.texLayout.u2 = sdata->font_icon.texLayout.u0;
						sdata->font_icon.texLayout.u3 = sdata->font_icon.texLayout.u1;
						sdata->font_icon.texLayout.v3 = sdata->font_icon.texLayout.v2;
					}
					else
					{
						kanaID = (u8)iconID - 0x98; // if you remove the u8 cast the dakuten and handakuten diacritics break. why??? --Super
						sdata->font_icon.texLayout.clut = sdata->font_jfontSmall0x18IconData.clut;

						if (kanaID & 1)
							sdata->font_icon.texLayout.clut += 0x40;

						sdata->font_icon.texLayout.tpage = sdata->font_jfontSmall0x18IconData.tpage;

						sdata->font_icon.texLayout.u0 = data.font_X1Y1data[kanaID * 2 + 1] / 2 * 12;
						sdata->font_icon.texLayout.u1 = sdata->font_icon.texLayout.u0 + 11;

						sdata->font_icon.texLayout.v0 = data.font_X1Y1data[kanaID * 2] * 8 + 8;
						sdata->font_icon.texLayout.v2 = data.font_X1Y1data[kanaID * 2] * 8 + 15;

						sdata->font_icon.texLayout.v1 = sdata->font_icon.texLayout.v0;
						sdata->font_icon.texLayout.u2 = sdata->font_icon.texLayout.u0;
						sdata->font_icon.texLayout.u3 = sdata->font_icon.texLayout.u1;
						sdata->font_icon.texLayout.v3 = sdata->font_icon.texLayout.v2;
					}
				}
			}

#endif

#if BUILD <= UsaRetail

// NOTE(aalhendi): Native can boot before every retail icon group is loaded.
#ifdef CTR_NATIVE
			if (gGT->iconGroup[iconGroupID] != 0)
			{
#endif

				if (iconID < gGT->iconGroup[iconGroupID]->numIcons)
				{
					struct Icon **iconPtrArray = ICONGROUP_GETICONS(gGT->iconGroup[iconGroupID]);

					DecalHUD_DrawPolyGT4(iconPtrArray[iconID],

					                     posX + pixWidthExtra, posY + pixHeightExtra,

					                     &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,

					                     ptrColor[0], ptrColor[1], ptrColor[2], ptrColor[3],

					                     0, iconScale);
				}
			}

#elif BUILD == JpnTrial || BUILD == JpnRetail

			if (iconStruct == 0)
			{
				struct Icon **iconPtrArray = ICONGROUP_GETICONS(gGT->iconGroup[iconGroupID]);
				if (kanaID < gGT->iconGroup[iconGroupID]->numIcons)
					iconStruct = iconPtrArray[kanaID];
			}
			if (iconStruct != 0)
			{
				DecalHUD_DrawPolyGT4(iconStruct,

				                     posX + pixWidthExtra, posY + pixHeightExtra,

				                     &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,

				                     ptrColor[0], ptrColor[1], ptrColor[2], ptrColor[3],

				                     0, iconScale);
			}

#else // i.e. european build

			struct Icon **iconPtrArray = ICONGROUP_GETICONS(gGT->iconGroup[iconGroupID]);

			for (; numCharacters > 0; numCharacters--, pixWidthExtra += data.font_EurPixWidthExtra[fontType])
			{
				if (upsideDownCharacter)
				{
					DecalHUD_Arrow2D(iconPtrArray[iconID],

					                 posX + pixWidthExtra, posY + pixHeightExtra,

					                 &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,

					                 ptrColor[2], ptrColor[3], ptrColor[0], ptrColor[1],

					                 0, iconScale, 0x800);
				}
				else
				{
					DecalHUD_DrawPolyGT4(iconPtrArray[iconID],

					                     posX + pixWidthExtra, posY + pixHeightExtra,

					                     &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,

					                     ptrColor[0], ptrColor[1], ptrColor[2], ptrColor[3],

					                     0, iconScale);
				}
			}

#endif
		}
		posX += charWidth;
	}
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80022878-0x800228c4.
void DecalFont_DrawLine(char *str, int posX, int posY, s16 fontType, int flags)
{
	DecalFont_DrawLineStrlen(str, -1, (s16)posX, (s16)posY, fontType, (s16)flags);
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800228c4-0x80022930.
void DecalFont_DrawLineOT(char *str, int posX, int posY, s16 fontType, int flags, uint32_t *ot)
{
	struct GameTracker *gGT;
	uint32_t *backupOT;

	gGT = sdata->gGT;

	// backup
	backupOT = gGT->pushBuffer_UI.ptrOT;

	// alter
	gGT->pushBuffer_UI.ptrOT = ot;

	// draw
	DecalFont_DrawLine(str, (s16)posX, (s16)posY, fontType, (s16)flags);

	// reset
	gGT->pushBuffer_UI.ptrOT = backupOT;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80022930-0x80022b34.
int DecalFont_DrawMultiLineStrlen(char *str, s16 len, s16 posX, s16 posY, s16 maxPixLen, s16 fontType, s16 flags)
{
	char strCharacter;
	s16 lineLen;
	char *currPointer;
	s16 lettersRemaining;
	char *strPointer;
	int totalPassageHeight;

	totalPassageHeight = 0;

	do
	{
		// pointer to string
		strPointer = str;

		// rather than using \n for new lines, CTR uses \r, which is similar if you try it with printf

		// while you've not reached the end of the line
		if (*str != '\r')
		{
			// get the first character
			strCharacter = *str;

			while (1)
			{
				// pointer to current letter
				currPointer = strPointer;

				// number of letters remaining
				lettersRemaining = len;

				// if you reached a space, and you're
				// not out of letters yet
				if ((strCharacter == ' ') && (len != 0))
				{
					// increment pointer to next letter
					currPointer = strPointer + 1;

					// one letter less
					lettersRemaining = len - 1;
				}

				// get next character
				strCharacter = *currPointer;

				// if nullptr, or out of letters, quit the loop
				if ((strCharacter == '\0') || (lettersRemaining == 0))
				{
					break;
				}

				// if this is a letter, number, or symbol
				if ((strCharacter != ' ') && (strCharacter != '\r'))
				{
					// get the length of the next word
					while (lettersRemaining != 0)
					{
						// increment pointer to next letter
						currPointer = currPointer + 1;

						// get value of next character
						strCharacter = *currPointer;

						// reduce number of remaining characters
						lettersRemaining = lettersRemaining + -1;

						// stop counting at a nullptr,
						// or a space (end of word),
						// or the end of the line '\r'
						if (((strCharacter == '\0') || (strCharacter == ' ')) || (strCharacter == '\r'))
						{
							break;
						}
					}
				}

				lineLen = DecalFont_GetLineWidthStrlen(str, (u32)currPointer - (u32)str, (int)fontType);

				if (
				    // if parameter line length is longer than string line length
				    (maxPixLen <= lineLen) || (
				                                  // get character
				                                  strCharacter = *currPointer,

				                                  // update pointer
				                                  strPointer = currPointer,

				                                  // update number of remaining characters
				                                  len = lettersRemaining,

				                                  // check if this is new line
				                                  strCharacter == '\r'))
				{
					break;
				}
			}
		}

#if BUILD > UsaRetail
		if (!(flags & 0x800))
#endif
			DecalFont_DrawLineStrlen(str, (u32)strPointer - (u32)str, (int)posX, posY + totalPassageHeight, (int)fontType, (int)flags);

#if BUILD > SepReview

		totalPassageHeight += data.font_charPixHeight[fontType];

#else

		// did font_charPixHeight not exist in 903?

		lettersRemaining = (s16)totalPassageHeight;
		totalPassageHeight = (int)lettersRemaining + 8;
		if ((int)fontType == 1)
		{
			totalPassageHeight = (int)lettersRemaining + 0x11;
		}
		strCharacter = *strPointer;
		str = strPointer;
		if (strCharacter == '\r')
		{
			if (len != 0)
			{
				strPointer++;
				len--;
			}
			strCharacter = *strPointer;
			str = strPointer;
		}

#endif

		if (*strPointer == '\0')
		{
		EndFunction:
			return totalPassageHeight;
		}

		if (len != 0)
		{
			strPointer = strPointer + 1;
			len = len + -1;
		}
		if ((*strPointer == '\0') || (str = strPointer, len == 0))
		{
			goto EndFunction;
		}
	} while (1);
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80022b34-0x80022b94.
int DecalFont_DrawMultiLine(char *str, int posX, int posY, int maxPixLen, s16 fontType, int flags)
{
	return (s16)DecalFont_DrawMultiLineStrlen(str, -1, (s16)posX, (s16)posY, (s16)maxPixLen, fontType, (s16)flags);
}
