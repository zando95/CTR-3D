#include <common.h>

enum RaceFlagScratchConstants
{
	RACE_FLAG_SCREEN_ROWS = 10,
	RACE_FLAG_SCREEN_POINTS_PER_ROW = 3,
};

enum RaceFlagConstants
{
	RACE_FLAG_POSITION_ONSCREEN = 0,
	RACE_FLAG_POSITION_OFFSCREEN = 5000,
	RACE_FLAG_POSITION_OFFSCREEN_LEFT = -5000,
	RACE_FLAG_OFFSCREEN_CHECK_BIAS = 4999,
	RACE_FLAG_OFFSCREEN_CHECK_WIDTH = 9999,
	RACE_FLAG_DRAW_ORDER_AFTER_FLAG = 1,
	RACE_FLAG_DRAW_ORDER_BEFORE_FLAG = -1,
	RACE_FLAG_DRAW_ORDER_DONE = 0,
	RACE_FLAG_TRANSITION_ONSCREEN = 0,
	RACE_FLAG_TRANSITION_OFFSCREEN = 2,
	RACE_FLAG_TRANSITION_BEGIN_ONSCREEN = 1,
	RACE_FLAG_TRANSITION_BEGIN_OFFSCREEN = 2,
	RACE_FLAG_TRANSITION_ONSCREEN_SPEED = 300,
	RACE_FLAG_TRANSITION_OFFSCREEN_SPEED_MAX = 1000,
	RACE_FLAG_TRANSITION_OFFSCREEN_ACCEL_SCALE = 10,
	RACE_FLAG_TRANSITION_ONSCREEN_SNAP_DISTANCE = 8,
	RACE_FLAG_TRANSITION_ONSCREEN_POSITION_SHIFT = 3,
	RACE_FLAG_TRANSITION_OFFSCREEN_POSITION_SHIFT = 2,
	RACE_FLAG_TRANSITION_TIME_SHIFT = 5,
	RACE_FLAG_LOADING_IDLE_SLIDE_LIMIT = -1000,
	RACE_FLAG_LOADING_IDLE_SLIDE_STEP = 0x28,
	RACE_FLAG_LOADING_OFFSCREEN_X = 0x23c,
	RACE_FLAG_LOADING_CENTER_X = 0x100,
	RACE_FLAG_LOADING_Y = 0x6c,
	RACE_FLAG_LOADING_FONT_SIZE = 1,
	RACE_FLAG_LOADING_TEXT_FLAGS = 0,
	RACE_FLAG_LOADING_FIRST_CENTER_FRAME = 4,
	RACE_FLAG_LOADING_LAST_CENTER_FRAME = 0x4a,
	RACE_FLAG_LOADING_EXIT_BASE_FRAME = 0x4b,
	RACE_FLAG_LOADING_LAST_VISIBLE_FRAME = 0x4f,
	RACE_FLAG_LOADING_ANIM_RESET_FRAME = 0x50,
	RACE_FLAG_LOADING_LETTER_SPEED_X = 0x3c,
	RACE_FLAG_LOADING_NEXT_LETTER_START_X = 0xf0,
	RACE_FLAG_LOADING_LETTER_FRAME_STEP = 4,
	RACE_FLAG_LOADING_GLYPH_EXTENDED_MAX = 4,
	RACE_FLAG_LOADING_FRAME_TIME_SHIFT = 5,
	RACE_FLAG_LOADING_MIN_FRAME_ADVANCE = 1,
	RACE_FLAG_LOADING_REPEAT_STAGE_FIRST = 6,
	RACE_FLAG_LOADING_REPEAT_STAGE_COUNT = 2,
	RACE_FLAG_TRIG_TABLE_MASK = 0x3ff,
	RACE_FLAG_TRIG_HIGH_HALF_MASK = 0x400,
	RACE_FLAG_TRIG_NEGATE_MASK = 0x800,
};

CTR_STATIC_ASSERT(RACE_FLAG_POSITION_OFFSCREEN == 5000);
CTR_STATIC_ASSERT(RACE_FLAG_POSITION_OFFSCREEN_LEFT == -5000);
CTR_STATIC_ASSERT(RACE_FLAG_OFFSCREEN_CHECK_WIDTH == 9999);
CTR_STATIC_ASSERT(RACE_FLAG_LOADING_OFFSCREEN_X == 0x23c);
CTR_STATIC_ASSERT(RACE_FLAG_LOADING_CENTER_X == 0x100);
CTR_STATIC_ASSERT(RACE_FLAG_LOADING_ANIM_RESET_FRAME == 0x50);
CTR_STATIC_ASSERT(RACE_FLAG_TRIG_TABLE_MASK == 0x3ff);
CTR_STATIC_ASSERT(RACE_FLAG_TRIG_HIGH_HALF_MASK == 0x400);
CTR_STATIC_ASSERT(RACE_FLAG_TRIG_NEGATE_MASK == 0x800);

struct RaceFlagProjectedRow
{
	u32 xy[RACE_FLAG_SCREEN_POINTS_PER_ROW];
};

union RaceFlagScreenBuffer
{
	struct RaceFlagProjectedRow row[RACE_FLAG_SCREEN_ROWS];
	u32 xy[RACE_FLAG_SCREEN_ROWS * RACE_FLAG_SCREEN_POINTS_PER_ROW];
};

struct RaceFlagScratch
{
	union RaceFlagScreenBuffer screen[2];
};

CTR_STATIC_ASSERT(sizeof(union RaceFlagScreenBuffer) == 0x78);
CTR_STATIC_ASSERT(sizeof(struct RaceFlagScratch) == 0xf0);

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80043e34-0x80043f1c.
int RaceFlag_MoveModels(int frameIndex, int numFrames)
{
	// need a better prefix than TitleFlag,
	// all this does is move the intro logo models
	// from the center of the screen, to the right

	// also used for transitioning driver models
	// on and off the screen in character selection

	int angle;
	int midpoint;
	int result;

	if (frameIndex < 0)
	{
		return 0;
	}

	if (frameIndex > numFrames)
	{
		return 0x1000;
	}

	// cut in half
	midpoint = numFrames / 2;

	// if less than half done
	if (frameIndex < midpoint)
	{
		angle = (midpoint - frameIndex) * 0x400;

		// 50% - sin(angle) / 2
		result = 0x800 - MATH_Sin(angle / midpoint) / 2;
	}
	// if more than half done
	else
	{
		angle = (frameIndex - midpoint) * 0x400;

		// sin(angle) / 2 + 50%
		result = MATH_Sin(angle / midpoint) / 2 + 0x800;
	}
	return result;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80043f1c-0x80043f28.
b32 RaceFlag_IsFullyOnScreen(void)
{
	// return true if flag is fully on screen
	// return false if flag is not fully on screen
	return (sdata->RaceFlag_Position == RACE_FLAG_POSITION_ONSCREEN);
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80043f28-0x80043f44.
b32 RaceFlag_IsFullyOffScreen(void)
{
	// return false, "not true", if flag is < 5000, partially on-screen
	// return true, "not false", if flag is >= 5000, fully off-screen
	return ((((u16)sdata->RaceFlag_Position + RACE_FLAG_OFFSCREEN_CHECK_BIAS) & 0xffff) < RACE_FLAG_OFFSCREEN_CHECK_WIDTH) ^ 1;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80043f44-0x80043f8c.
b32 RaceFlag_IsTransitioning(void)
{
	int pos = sdata->RaceFlag_Position;

	return
	    // if checkered flag is not fully on-screen and not fully off-screen
	    (pos != RACE_FLAG_POSITION_ONSCREEN) && (pos != RACE_FLAG_POSITION_OFFSCREEN_LEFT) && (pos != RACE_FLAG_POSITION_OFFSCREEN) &&

	    // is allowed to render
	    ((sdata->gGT->renderFlags & RENDER_FLAG_CHECKERED_FLAG) != 0);
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80043f8c-0x80043fb0.
void RaceFlag_SetDrawOrder(b32 drawAfterFlag)
{
	sdata->RaceFlag_DrawOrder = (drawAfterFlag != 0) ? RACE_FLAG_DRAW_ORDER_AFTER_FLAG : RACE_FLAG_DRAW_ORDER_BEFORE_FLAG;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80043fb0-0x8004402c.
void RaceFlag_BeginTransition(int direction)
{
	// Begin Transition on-screen
	if (direction == RACE_FLAG_TRANSITION_BEGIN_ONSCREEN)
	{
		sdata->RaceFlag_LoadingTextAnimFrame = -1;

		sdata->RaceFlag_Position = RACE_FLAG_POSITION_OFFSCREEN;

		sdata->RaceFlag_AnimationType = RACE_FLAG_TRANSITION_ONSCREEN;
	}

	// Begin Transition off-screen
	else if (direction == RACE_FLAG_TRANSITION_BEGIN_OFFSCREEN)
	{
		RaceFlag_SetDrawOrder(0);

		sdata->RaceFlag_Position = RACE_FLAG_POSITION_ONSCREEN;

		sdata->RaceFlag_AnimationType = direction;
	}

	// enable loading screen's checkered flag
	sdata->gGT->renderFlags |= RENDER_FLAG_CHECKERED_FLAG;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x8004402c-0x80044058.
void RaceFlag_SetFullyOnScreen()
{
	sdata->RaceFlag_AnimationType = RACE_FLAG_TRANSITION_ONSCREEN;
	sdata->RaceFlag_LoadingTextAnimFrame = -1;

	// flag is now fully on-screen
	sdata->RaceFlag_Position = RACE_FLAG_POSITION_ONSCREEN;

	// enable loading screen's checkered flag
	sdata->gGT->renderFlags |= RENDER_FLAG_CHECKERED_FLAG;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80044058-0x80044088.
void RaceFlag_SetFullyOffScreen()
{
	sdata->RaceFlag_AnimationType = RACE_FLAG_TRANSITION_ONSCREEN;
	sdata->RaceFlag_LoadingTextAnimFrame = -1;

	// flag is now fully off-screen
	sdata->RaceFlag_Position = RACE_FLAG_POSITION_OFFSCREEN;

	// disable loading screen's checkered flag
	sdata->gGT->renderFlags &= ~RENDER_FLAG_CHECKERED_FLAG;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80044088-0x80044094.
void RaceFlag_SetCanDraw(s16 canDraw)
{
	sdata->RaceFlag_CanDraw = canDraw;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80044094-0x800440a0.
s16 RaceFlag_GetCanDraw(void)
{
	return sdata->RaceFlag_CanDraw;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800440a0-0x80044290.
u32 *RaceFlag_GetOT(void)
{
	s16 positionStep;
	int position;
	struct GameTracker *gGT = sdata->gGT;

	u32 *otDrawFirst_FarthestDepth;
	u32 *otDrawLast_ClosestDepth;

	otDrawFirst_FarthestDepth = (u32 *)&gGT->pushBuffer[0].ptrOT[0x3FF];
	otDrawLast_ClosestDepth = gGT->otSwapchainDB[gGT->swapchainIndex];

	if (sdata->RaceFlag_DrawInitialized == 0)
	{
		sdata->RaceFlag_DrawInitialized = 1;
	}

	// transitioning on-screen
	if (sdata->RaceFlag_AnimationType == RACE_FLAG_TRANSITION_ONSCREEN)
	{
		// set fully "off" to start transition "on"
		if (sdata->RaceFlag_Position < 0)
		{
			sdata->RaceFlag_Position = RACE_FLAG_POSITION_OFFSCREEN;
		}

		sdata->RaceFlag_TransitionSpeed = RACE_FLAG_TRANSITION_ONSCREEN_SPEED;

		position = sdata->RaceFlag_Position;

		// if transitioning
		if (position != RACE_FLAG_POSITION_ONSCREEN)
		{
			// skip last 8 frames to zero
			if (position < RACE_FLAG_TRANSITION_ONSCREEN_SNAP_DISTANCE)
			{
				sdata->RaceFlag_Position = RACE_FLAG_POSITION_ONSCREEN;

				// transition for frame >= 8
			}
			else
			{
				// rate of transition

				position = ((u16)sdata->RaceFlag_Position >> RACE_FLAG_TRANSITION_ONSCREEN_POSITION_SHIFT) * gGT->elapsedTimeMS;
				position = position >> RACE_FLAG_TRANSITION_TIME_SHIFT;

				positionStep = -(s16)position;
				if (position < 1)
				{
					positionStep = -1;
				}

				sdata->RaceFlag_Position += positionStep;
			}
		}

		// transition is finished
		else
		{
			if (sdata->RaceFlag_DrawOrder != RACE_FLAG_DRAW_ORDER_AFTER_FLAG)
			{
				if (sdata->RaceFlag_DrawOrder != RACE_FLAG_DRAW_ORDER_BEFORE_FLAG)
				{
					return otDrawFirst_FarthestDepth;
				}

				sdata->RaceFlag_DrawOrder = RACE_FLAG_DRAW_ORDER_DONE;
			}
		}
	}

	// transition off-screen
	if (sdata->RaceFlag_AnimationType == RACE_FLAG_TRANSITION_OFFSCREEN)
	{
		if (sdata->RaceFlag_TransitionSpeed < RACE_FLAG_TRANSITION_OFFSCREEN_SPEED_MAX)
		{
			sdata->RaceFlag_TransitionSpeed += (s16)((gGT->elapsedTimeMS * RACE_FLAG_TRANSITION_OFFSCREEN_ACCEL_SCALE) >> RACE_FLAG_TRANSITION_TIME_SHIFT);
		}

		// If transitioning "off"
		if (sdata->RaceFlag_Position > RACE_FLAG_POSITION_OFFSCREEN_LEFT)
		{
			sdata->RaceFlag_Position -= (((u32)sdata->RaceFlag_TransitionSpeed >> RACE_FLAG_TRANSITION_OFFSCREEN_POSITION_SHIFT) * gGT->elapsedTimeMS) >>
			                            RACE_FLAG_TRANSITION_TIME_SHIFT;
		}

		// finished transitioning off
		else
		{
			sdata->RaceFlag_Position = RACE_FLAG_POSITION_OFFSCREEN;
			sdata->RaceFlag_AnimationType = RACE_FLAG_TRANSITION_ONSCREEN;
			gGT->renderFlags &= ~RENDER_FLAG_CHECKERED_FLAG;
		}
	}

	return otDrawLast_ClosestDepth;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80044290-0x800442a0.
void RaceFlag_ResetTextAnim(void)
{
	sdata->RaceFlag_LoadingTextAnimFrame = -1;
}


// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800442a0-0x800444e8.
void RaceFlag_DrawLoadingString(void)
{
	struct GameTracker *gGT = sdata->gGT;
	int loadingTextBytes;
	int letterAnimFrame;
	int letterX;
	int glyphByteCount;
	int textByteIndex;
	char *loadingText;
	char *nextGlyph;
	int nextLetterStartX;
	int drawX;
	u32 *oldOT;
	char glyph[2];

	loadingText = sdata->lngStrings[LNG_LOADING];

	// pointer to OT mem
	oldOT = (u32 *)gGT->pushBuffer_UI.ptrOT;

	// pointer to OT mem
	gGT->pushBuffer_UI.ptrOT = gGT->otSwapchainDB[gGT->swapchainIndex];

	// get length of "LOADING..." string
	loadingTextBytes = strlen(loadingText);

	int textWidth = DecalFont_GetLineWidth(loadingText, RACE_FLAG_LOADING_FONT_SIZE);

	// loop counter
	textByteIndex = 0;

	// if game is not loading
	if (sdata->Loading.stage == LOAD_IDLE)
	{
		if (RACE_FLAG_LOADING_IDLE_SLIDE_LIMIT < (int)sdata->RaceFlag_Transition)
		{
			sdata->RaceFlag_Transition -= RACE_FLAG_LOADING_IDLE_SLIDE_STEP;
		}
	}
	else
	{
		sdata->RaceFlag_Transition = 0;
	}

	drawX = (sdata->RaceFlag_Transition & 0xffff) - (textWidth >> 1);

	letterAnimFrame = sdata->RaceFlag_LoadingTextAnimFrame;

	if (0 < loadingTextBytes)
	{
		nextLetterStartX = letterAnimFrame * -RACE_FLAG_LOADING_LETTER_SPEED_X + RACE_FLAG_LOADING_OFFSCREEN_X;

		// for each byte in the localized "LOADING..." string
		do
		{
			if (letterAnimFrame < 0)
			{
			DrawLetterOffscreen:

				// draw text off screen
				letterX = RACE_FLAG_LOADING_OFFSCREEN_X;
			}
			else
			{
				letterX = nextLetterStartX;
				if (RACE_FLAG_LOADING_FIRST_CENTER_FRAME < letterAnimFrame)
				{
					letterX = RACE_FLAG_LOADING_CENTER_X;

					if (RACE_FLAG_LOADING_LAST_CENTER_FRAME < letterAnimFrame)
					{
						// if letter is fully off-screen
						if (RACE_FLAG_LOADING_LAST_VISIBLE_FRAME < letterAnimFrame)
						{
							goto DrawLetterOffscreen;
						}

						// letter is moving off-screen
						letterX = (RACE_FLAG_LOADING_EXIT_BASE_FRAME - letterAnimFrame) * RACE_FLAG_LOADING_LETTER_SPEED_X + RACE_FLAG_LOADING_CENTER_X;
					}
				}
			}
			glyph[0] = *loadingText;
			nextGlyph = loadingText + 1;
			glyphByteCount = 1;
			if ((u8)glyph[0] < RACE_FLAG_LOADING_GLYPH_EXTENDED_MAX)
			{
				glyph[1] = *nextGlyph;
				nextGlyph = loadingText + 2;

				// increment loop counter
				textByteIndex = textByteIndex + 1;

				glyphByteCount = 2;
			}
			if ((s16)letterX != RACE_FLAG_LOADING_OFFSCREEN_X)
			{
				DecalFont_DrawLineStrlen(glyph, glyphByteCount, (drawX + letterX), RACE_FLAG_LOADING_Y, RACE_FLAG_LOADING_FONT_SIZE,
				                         RACE_FLAG_LOADING_TEXT_FLAGS);
			}

			letterX = DecalFont_GetLineWidthStrlen(glyph, glyphByteCount, RACE_FLAG_LOADING_FONT_SIZE);

			drawX = drawX + letterX;
			nextLetterStartX = nextLetterStartX + RACE_FLAG_LOADING_NEXT_LETTER_START_X;

			// increment loop counter
			textByteIndex = textByteIndex + 1;

			// treat all letters with 4 frame difference
			letterAnimFrame = letterAnimFrame - RACE_FLAG_LOADING_LETTER_FRAME_STEP;

			loadingText = nextGlyph;
		} while (textByteIndex < loadingTextBytes);
	}

	// pointer to OT mem
	gGT->pushBuffer_UI.ptrOT = (uint32_t *)oldOT;

	if (letterAnimFrame < RACE_FLAG_LOADING_ANIM_RESET_FRAME)
	{
		int frameAdvance = gGT->elapsedTimeMS >> RACE_FLAG_LOADING_FRAME_TIME_SHIFT;

		if (frameAdvance < RACE_FLAG_LOADING_MIN_FRAME_ADVANCE)
		{
			frameAdvance = RACE_FLAG_LOADING_MIN_FRAME_ADVANCE;
		}

		sdata->RaceFlag_LoadingTextAnimFrame += frameAdvance;
	}

	else
	{
		sdata->RaceFlag_LoadingTextAnimFrame = -1;
		if ((u32)(sdata->Loading.stage - RACE_FLAG_LOADING_REPEAT_STAGE_FIRST) < RACE_FLAG_LOADING_REPEAT_STAGE_COUNT)
		{
			sdata->RaceFlag_LoadingTextAnimFrame = 0;
		}
	}
	return;
}

#ifdef CTR_NATIVE
/* Defined platform-side (native_gpu.c) -- file scope, not the block-local
 * idiom used for NativeWide_CullMarginX below, because the use site is deep
 * inside RaceFlag_DrawSelf's nested loops and a block-scoped declaration
 * would not be visible there. 0 = retail checkers, 1 = pride stripes,
 * 2 = purple checkers, 3 = blue-on-blue checkers. */
extern int g_dbg_flagStyle;

/*
 * SOAR / Spyro easter egg: PURPLE AND WHITE checkers (owner's colour and
 * call). Two distinct tones rather than a solid purple flag, because the
 * CHECKS are what still read as CTR. Purple/gold was considered first and
 * rejected as likely ugly at flag scale.
 *
 * Only the purple tile is tinted. The white tile takes the lighting value
 * straight, which makes it EXACTLY retail's white rather than an approximation
 * -- multiplying by a 255 tint would quietly shave a level off every channel.
 *
 * The purple is a TINT, modulated by the same wave lighting the grey checkers
 * use, so it renders from roughly half to full intensity across the cloth.
 * Both tiles take the LIGHT-tile brightness curve (the wider-ranged of retail's
 * two); hue supplies the checker contrast here, so neither tone is stuck in the
 * dimmer half.
 */
static const u8 s_rfSpyroPurpleRGB[3] = {140, 117, 188};

/*
 * Retro-Fueled flag (g_dbg_flagStyle == 3): blue-on-blue checkers matching the
 * middle of Nitro-Fueled's menu flag (#4AA7D0 / #4095C3). The values are the
 * fully lit colours; the wave shading darkens them to about half in the folds.
 * Unlike the Spyro flag BOTH tiles are tinted, since neither is white. The tiles
 * use the checker parity of every other style and the light-tile brightness
 * curve, so the wave shading reads the same.
 */
static const u8 s_rfBlueLightRGB[3] = {74, 167, 208};
static const u8 s_rfBlueDarkRGB[3] = {64, 149, 195};

/*
 * Pride-flag stripe colours, top to bottom (the six-colour flag). Used when
 * g_dbg_flagStyle == 1 -- see the pattern block in RaceFlag_DrawSelf.
 *
 * These are TINTS, not final colours: each is modulated by the same per-quad
 * wave lighting the checkers use, so a stripe runs from roughly half to full
 * intensity across the cloth. That is deliberate and it is the whole trick --
 * flat-filled bands read as a scrolling texture, while the shaded ones read as
 * a waving flag, which is the effect the mesh already earns and the stereo
 * depth now sells.
 */
static const u8 s_rfStripeRGB[6][3] = {
    {0xE4, 0x03, 0x03}, /* red */
    {0xFF, 0x8C, 0x00}, /* orange */
    {0xFF, 0xED, 0x00}, /* yellow */
    {0x00, 0x80, 0x26}, /* green */
    {0x00, 0x4D, 0xFF}, /* blue */
    {0x75, 0x07, 0x87}, /* violet */
};

/* Vertical quad index -> stripe, for the 29-quad-tall mesh. (i * 6) / 29 lays
 * the six bands out 5,5,5,5,5,4 quads deep -- even to within a single quad,
 * which is the finest the mesh can express (checker/stripe edges snap to quad
 * boundaries; see RACE_FLAG_SCREEN_ROWS/POINTS_PER_ROW).
 *
 * INVERTED on purpose. `i` counts quads in the order the mesh is built, which
 * runs UP the screen, not down: data.matrixTitleFlag flips the model-space vy
 * axis the row loop steps along. MEASURED, not assumed -- the first build put
 * red at the bottom and violet at the top, which is upside down for a flag
 * whose stripe order is the point. Indexing from the end puts red on top. */
#define RACE_FLAG_STRIPE_QUADS_TALL 29u

/*
 * Widescreen crops the flag vertically: gte_SetGeomScreen runs at 0x180 rather
 * than 0x100 there, a 3/2 zoom to cover the wide window, which pushes the top
 * and bottom of the mesh off-screen. Spreading six bands over the full mesh
 * then hides two of them -- owner on hardware: red and orange entirely above
 * the top edge, violet a sliver. So in wide mode the bands are packed into the
 * quads that are actually on screen.
 *
 * The visible band is NOT the middle of the mesh. Row positions run
 * vy -910..+1628, so the mesh sits off-centre vertically before the zoom is
 * applied at all -- which is why these are measured constants rather than a
 * ratio derived from 3/2.
 *
 * Bands are a FIXED WIDTH in quads rather than a span divided six ways. The
 * division form left one band a quad wider than its neighbours (14/6 = 2.33,
 * and whichever band's lower edge landed on an exact integer swallowed the
 * remainder -- owner saw it as a fat yellow stripe). A fixed width cannot do
 * that. Everything at or below VIS_LO is violet and everything past the sixth
 * band is red; both of those ends are largely off-screen anyway, which is what
 * absorbs the mesh's cropped extremes.
 *
 * Current layout, owner-tuned on hardware -- quad i counts UP the screen:
 *   violet 0-6, blue 7-8, green 9-10, yellow 11-12, orange 13-14, red 15-28.
 * Red and orange are where he wants them; move VIS_LO to shift the lower four
 * without disturbing those two.
 *
 * 4:3 keeps the full-height mapping.
 */
enum
{
	RACE_FLAG_STRIPE_VIS_LO = 5,
	RACE_FLAG_STRIPE_BAND_QUADS = 2
};

/* Band for a quad row. Inverted (5 - n) because i counts UP the screen: the
 * title-flag matrix flips the model-space vy the row loop steps along, so
 * without this red lands at the bottom. */
force_inline u32 RaceFlag_StripeIndex(int quadY, int wide)
{
	u32 n;

	if (!wide)
	{
		n = ((u32)quadY * 6u) / RACE_FLAG_STRIPE_QUADS_TALL;
	}
	else if (quadY <= RACE_FLAG_STRIPE_VIS_LO)
	{
		n = 0u;
	}
	else
	{
		n = (u32)(quadY - RACE_FLAG_STRIPE_VIS_LO) / (u32)RACE_FLAG_STRIPE_BAND_QUADS;
	}

	if (n > 5u)
	{
		n = 5u;
	}
	return 5u - n;
}

/*
 * Quads per checker square. Retail is 4 -- the original `>> 2` on both axes --
 * and the port uses 3 (owner request, "a little smaller"). The only deliberate
 * departure from retail's flag appearance; everything else about the mesh, the
 * wave and the lighting is untouched.
 *
 * A constant divisor, not a shift, so non-power-of-two sizes are available: gcc
 * turns `/ 3` into a multiply-high, so this costs no more than the shift did
 * and never reaches ARM11's missing hardware divide. Any integer >= 1 works; 1
 * is the finest the mesh can express, since squares snap to quad boundaries.
 */
#define RACE_FLAG_CHECKER_QUADS 3u
#endif

force_inline char RaceFlag_CalculateBrightness(u32 sine, u8 darkTile)
{
	if (darkTile)
	{
		return ((sine * -55 + 0x140000) >> 0xD);
	}
	return ((sine * -125 + 0x1fe000) >> 0xD);
}

force_inline int RaceFlag_Sin(u32 angle)
{
	int sine;

	// approximate trigonometry
	sine = (s32)CTR_ReadU32LE(&data.trigApprox[angle & RACE_FLAG_TRIG_TABLE_MASK]);

	if ((angle & RACE_FLAG_TRIG_HIGH_HALF_MASK) == 0)
	{
		sine = sine << 0x10;
	}

	sine = sine >> 0x10;

	if ((angle & RACE_FLAG_TRIG_NEGATE_MASK) != 0)
	{
		// make negative
		sine = -sine;
	}
	return sine;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x800444e8-0x80044ef8.
void RaceFlag_DrawSelf()
{
	int i, j;
	int column, row;
	int toggle;

	s16 flagPos;
	uint32_t *ot;
	u32 screenlimit;
	u32 dimensions;

	int var2;
	int var3;
	u32 var1;

	POLY_G4 *p;
	struct GameTracker *gGT = sdata->gGT;

	int time;
	int lightL;
	int lightR;

	struct RaceFlagScratch *scratch;
	s32 local[5];
	SVECTOR pos[3] = {0};
#ifdef CTR_NATIVE
	int flagWide = 0;
#endif

	if (sdata->RaceFlag_CanDraw == 0)
	{
		return;
	}

	if (sdata->RaceFlag_LoadingTextAnimFrame < 0)
	{
		if ((5 < sdata->Loading.stage) && (sdata->Loading.stage < 8))
		{
			sdata->RaceFlag_LoadingTextAnimFrame = 0;
		}

		if (sdata->RaceFlag_LoadingTextAnimFrame < 0)
		{
			goto SKIP_LOADING_TEXT;
		}
	}

	RaceFlag_DrawLoadingString();

SKIP_LOADING_TEXT:

	sdata->RaceFlag_CopyLoadStage = sdata->Loading.stage;
	ot = (uint32_t *)RaceFlag_GetOT();

	gte_SetRotMatrix(&data.matrixTitleFlag);
	gte_SetTransMatrix(&data.matrixTitleFlag);
	gte_SetGeomOffset(0x100, 0x78);
#ifdef CTR_NATIVE
	/* s37 widescreen: the flag mesh only spans the 4:3 window -- the
	 * geometry past the old edges simply doesn't exist (owner call:
	 * "stretch the flag", refined to a FLAT ZOOM). gte_SetGeomScreen is
	 * this block's own projection distance, so raising it is a uniform
	 * zoom about the projection centre: squares stay square, and 3/2
	 * (0x180) covers the wide window with enough overshoot to keep the
	 * mesh's ragged wavy border off-screen in every pose (loading AND
	 * menus). Earlier matrix-row X stretch superseded: anamorphic, and
	 * it under-covered the menu pose. Retail 0x100 whenever wide is off,
	 * including all of PC.
	 *
	 * flagWide also feeds two more wide adaptations below: the
	 * quad-granular screen cull bypass (the packed 512x216 test culls
	 * whole quads that now land in the margins -- the owner's "jaggy
	 * edges") and the depth-capture suppression bracket (Crash in front
	 * of the flag, DrawSky's exact pattern). */
	{
		extern int NativeWide_CullMarginX(int rectW);

		flagWide = (NativeWide_CullMarginX(512) != 0);
	}
	gte_SetGeomScreen(flagWide ? 0x180 : 0x100);
#else
	gte_SetGeomScreen(0x100);
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s39 REAL FLAG DEPTH (owner design). This used to be the DrawSky-style
	 * suppression bracket: the wave's true GTE depths cross a menu model's
	 * band (Crash interpenetrated the flag, owner-reported), so stores were
	 * dropped and the packets fell back to flat OT-bucket depth -- which
	 * fixed the layering but turned the wave into cardboard in stereo. The
	 * remap keeps the wave: each stored SZ3 is compressed to ~1/4 relief
	 * and re-anchored just BEHIND the stereo convergence plane, so the flag
	 * ripples at/behind the physical screen while menu models pop out in
	 * front -- the bands cannot cross, the painter's guarantee survives,
	 * and there is no pose conditional (the flag is one continuous
	 * animation across loading, menu and wipe roles; a role-keyed depth
	 * jump would be visible). Constants, bounds and the accepted wipe
	 * caveat: GTECapture_SetStoreDepthRemap (native_gte_capture.c). */
	GTECapture_SetStoreDepthRemap(1);
#endif

	p = (POLY_G4 *)gGT->backBuffer->primMem.cursor;

	scratch = CTR_SCRATCHPAD_PTR(struct RaceFlagScratch, 0);

	dimensions = 0xd80200;
	screenlimit = 0x80008000;

	toggle = 0;

	// === First Loop Iteration ===
	// Remove 36*10 branching instructions,
	// Reduces clock from ~150 to ~130
	{
		union RaceFlagScreenBuffer *writeScreen = &scratch->screen[toggle];
		toggle = toggle ^ 1;

		local[0] = data.checkerFlagVariables[0];
		local[1] = data.checkerFlagVariables[1];
		local[2] = data.checkerFlagVariables[2];
		local[3] = data.checkerFlagVariables[3];
		local[4] = data.checkerFlagVariables[4];

		// === Step 1 ===
		int stepRate = gGT->elapsedTimeMS;
		local[4] += local[3] * stepRate;
		var1 = (int)local[4] >> 5;

		// === Step 2 ===
		if (0xfff < var1)
		{
			// reset counter
			local[4] &= 0x1ffff;
			var1 = (int)local[4] >> 5;

			local[0] += 0x200;
			local[2] += 200;

			int sin0 = RaceFlag_Sin(local[0]) + 0xfff;
			int sin2 = RaceFlag_Sin(local[2]) + 0xfff;

			// reset based on trig
			local[1] = (sin0 * 0x20 >> 0xd) + 0x96;
			local[3] = (sin2 * 0x40 >> 0xd) + 0xb4;
		}

		// === Step 3 ===
		var2 = RaceFlag_Sin(var1) + 0xfff;
		var2 = var2 * local[1];
		var2 = (var2 >> 0xd) + 0x280;

		// === Step 4 ===
		var1 += 0xc80;
		lightL = RaceFlag_Sin(var1) + 0xfff;

		// === Step 5 ===
		pos[0].vy = 0xfc72;
		pos[1].vy = 0xfcd0;
		pos[2].vy = 0xfd2e;

		// === Step 6 ===
		data.checkerFlagVariables[0] = local[0];
		data.checkerFlagVariables[1] = local[1];
		data.checkerFlagVariables[2] = local[2];
		data.checkerFlagVariables[3] = local[3];
		data.checkerFlagVariables[4] = local[4];

		time = sdata->RaceFlag_ElapsedTime >> 5;
		var1 = time;

		flagPos = sdata->RaceFlag_Position;
		flagPos = -0xbbe - flagPos;
		pos[0].vx = flagPos;
		pos[1].vx = flagPos;
		pos[2].vx = flagPos;

		i = 0;
		// === Step 7 ===
		for (row = 0; row < 10; row++)
		{
			SVECTOR *vect;
			for (vect = &pos[0]; vect < &pos[3]; vect++)
			{
				// Range: [1.0, 2.0]
				var3 = RaceFlag_Sin(var1) + 0xfff;
				var1 += 300;

				// change all vector posZ
				vect->vz = (s16)var2 + (s16)(var3 * 0x20 >> 0xd);
			}

			CTR_GteLoadSV3WithPad(&pos[0], &pos[1], &pos[2]);
			gte_rtpt();

			pos[0].vy += 0x11a;
			pos[1].vy += 0x11a;
			pos[2].vy += 0x11a;

			CTR_GteStoreSXY3(&writeScreen->row[row].xy[0], &writeScreen->row[row].xy[1], &writeScreen->row[row].xy[2]);
		}

		lightR = lightL;
	}


	// === Rest of Iterations ===
	// Now executing without branching
	for (column = 1; column < 36; column++)
	{
		union RaceFlagScreenBuffer *writeScreen = &scratch->screen[toggle];
		toggle = toggle ^ 1;
		union RaceFlagScreenBuffer *readScreen = &scratch->screen[toggle];

		// === Step 1 ===
		int stepRate = 0x40;
		local[4] += local[3] * stepRate;
		var1 = (int)local[4] >> 5;

		// === Step 2 ===
		if (0xfff < var1)
		{
			// reset counter
			local[4] &= 0x1ffff;
			var1 = (int)local[4] >> 5;

			local[0] += 0x200;
			local[2] += 200;

			int sin0 = RaceFlag_Sin(local[0]) + 0xfff;
			int sin2 = RaceFlag_Sin(local[2]) + 0xfff;

			// reset based on trig
			local[1] = (sin0 * 0x20 >> 0xd) + 0x96;
			local[3] = (sin2 * 0x40 >> 0xd) + 0xb4;
		}

		// === Step 3 ===
		var2 = RaceFlag_Sin(var1) + 0xfff;
		var2 = var2 * local[1];
		var2 = (var2 >> 0xd) + 0x280;

		// === Step 4 ===
		var1 += 0xc80;
		lightL = RaceFlag_Sin(var1) + 0xfff;

		// === Step 5 ===
		pos[0].vy = 0xfc72;
		pos[1].vy = 0xfcd0;
		pos[2].vy = 0xfd2e;

		// === Step 6 ===
		time += 0x100;
		var1 = time;

		pos[0].vx += 100;
		pos[1].vx += 100;
		pos[2].vx += 100;

		i = 0;
		// === Step 7 ===
		for (row = 0; row < 10; row++)
		{
			SVECTOR *vect;
			for (vect = &pos[0]; vect < &pos[3]; vect++)
			{
				// Range: [1.0, 2.0]
				var3 = RaceFlag_Sin(var1) + 0xfff;
				var1 += 300;

				// change all vector posZ
				vect->vz = (s16)var2 + (s16)(var3 * 0x20 >> 0xd);
			}

			CTR_GteLoadSV3WithPad(&pos[0], &pos[1], &pos[2]);
			gte_rtpt();

			pos[0].vy += 0x11a;
			pos[1].vy += 0x11a;
			pos[2].vy += 0x11a;

			CTR_GteStoreSXY3(&writeScreen->row[row].xy[0], &writeScreen->row[row].xy[1], &writeScreen->row[row].xy[2]);

			int firstSegment = (row == 0) ? 1 : 0;

			for (j = firstSegment; j < RACE_FLAG_SCREEN_POINTS_PER_ROW; j++, i++)
			{
				int pointIndex = row * RACE_FLAG_SCREEN_POINTS_PER_ROW + j - 1;
				u32 read0 = readScreen->xy[pointIndex];
				u32 read1 = readScreen->xy[pointIndex + 1];
				u32 write0 = writeScreen->xy[pointIndex];
				u32 write1 = writeScreen->xy[pointIndex + 1];

				int quadVisible = (((read0 & read1 & write0 & write1 & screenlimit) == 0) &&
				                   (((dimensions - read0) & (dimensions - read1) & (dimensions - write0) & (dimensions - write1) & screenlimit) == 0));

#ifdef CTR_NATIVE
				/* s37 widescreen: this packed test culls whole QUADS
				 * against the 4:3 512x216 window -- in the wide window
				 * that eats every quad fully inside the margins and
				 * stair-steps the flag's edge at checker granularity
				 * (the owner's "jaggy edges"). While wide, draw
				 * everything and let the GPU clip; the full mesh is
				 * ~360 quads, noise. */
				if (flagWide)
				{
					quadVisible = 1;
				}
#endif
				if (quadVisible)
				{
#ifdef CTR_NATIVE
					/* Rainbow stripes (g_dbg_flagStyle == 1, native_gpu.c). The
					 * checker below is a 2D parity test over BOTH quad axes;
					 * a stripe is a 1D band over the vertical axis alone, so
					 * the only structural change is dropping `column` from the
					 * pattern. Lighting, mesh, wave and geometry are identical
					 * to retail's -- only the colour written into the same two
					 * gouraud corners differs. */
					if (g_dbg_flagStyle == 1)
					{
						const u8 *tint = s_rfStripeRGB[RaceFlag_StripeIndex(i, flagWide)];
						/* Lighting with the LIGHT-tile curve for every stripe:
						 * the two curves exist to separate the checker's own
						 * two tones, which a coloured stripe does not need, and
						 * the light curve carries the wider range (the fuller
						 * shading). */
						u8 lumR = RaceFlag_CalculateBrightness(lightR, 0);
						u8 lumL = RaceFlag_CalculateBrightness(lightL, 0);

						setRGB0(p, (u8)((lumR * tint[0]) >> 8), (u8)((lumR * tint[1]) >> 8), (u8)((lumR * tint[2]) >> 8));
						CTR_WriteU32LE(&p->r2, CTR_ReadU32LE(&p->r0));

						setRGB1(p, (u8)((lumL * tint[0]) >> 8), (u8)((lumL * tint[1]) >> 8), (u8)((lumL * tint[2]) >> 8));
						CTR_WriteU32LE(&p->r3, CTR_ReadU32LE(&p->r1));
					}
					else if (g_dbg_flagStyle == 2)
					{
						/* Purple/white checkers -- the SOAR/Spyro egg. Exactly
						 * retail's checker pattern and wave lighting; the only
						 * change is tinting one tile purple, so it still reads
						 * as a chequered flag. Light-tile curve for both (see
						 * the tint table's comment) -- hue, not the curves,
						 * carries the contrast here. */
						const u8 boolDark = (((((u32)column / RACE_FLAG_CHECKER_QUADS) + ((u32)i / RACE_FLAG_CHECKER_QUADS)) & 1U) != 0);
						const u8 *tint = boolDark ? s_rfSpyroPurpleRGB : NULL;
						u8 lumR = RaceFlag_CalculateBrightness(lightR, 0);
						u8 lumL = RaceFlag_CalculateBrightness(lightL, 0);

						if (tint != NULL)
						{
							setRGB0(p, (u8)((lumR * tint[0]) >> 8), (u8)((lumR * tint[1]) >> 8), (u8)((lumR * tint[2]) >> 8));
							setRGB1(p, (u8)((lumL * tint[0]) >> 8), (u8)((lumL * tint[1]) >> 8), (u8)((lumL * tint[2]) >> 8));
						}
						else
						{
							/* White tile: untinted, so identical to retail's. */
							setRGB0(p, lumR, lumR, lumR);
							setRGB1(p, lumL, lumL, lumL);
						}
						CTR_WriteU32LE(&p->r2, CTR_ReadU32LE(&p->r0));
						CTR_WriteU32LE(&p->r3, CTR_ReadU32LE(&p->r1));
					}
					else if (g_dbg_flagStyle == 3)
					{
						/* Retro-Fueled blue-on-blue checkers: same parity and
						 * lighting as style 2, both tiles tinted. */
						const u8 boolDark = (((((u32)column / RACE_FLAG_CHECKER_QUADS) + ((u32)i / RACE_FLAG_CHECKER_QUADS)) & 1U) != 0);
						const u8 *tint = boolDark ? s_rfBlueDarkRGB : s_rfBlueLightRGB;
						u8 lumR = RaceFlag_CalculateBrightness(lightR, 0);
						u8 lumL = RaceFlag_CalculateBrightness(lightL, 0);

						setRGB0(p, (u8)((lumR * tint[0]) >> 8), (u8)((lumR * tint[1]) >> 8), (u8)((lumR * tint[2]) >> 8));
						CTR_WriteU32LE(&p->r2, CTR_ReadU32LE(&p->r0));

						setRGB1(p, (u8)((lumL * tint[0]) >> 8), (u8)((lumL * tint[1]) >> 8), (u8)((lumL * tint[2]) >> 8));
						CTR_WriteU32LE(&p->r3, CTR_ReadU32LE(&p->r1));
					}
					else
#endif
					{
					// TRUE for gray, FALSE for white
					// (retail: (column >> 2) + (i >> 2) -- see RACE_FLAG_CHECKER_QUADS)
					u8 boolDark = (((((u32)column / RACE_FLAG_CHECKER_QUADS) + ((u32)i / RACE_FLAG_CHECKER_QUADS)) & 1U) != 0);

					u8 colorR = RaceFlag_CalculateBrightness(lightR, boolDark);
					setRGB0(p, colorR, colorR, colorR);
					CTR_WriteU32LE(&p->r2, CTR_ReadU32LE(&p->r0));

					u8 colorL = RaceFlag_CalculateBrightness(lightL, boolDark);
					setRGB1(p, colorL, colorL, colorL);
					CTR_WriteU32LE(&p->r3, CTR_ReadU32LE(&p->r1));
					}

					// positions
					CtrGpu_WritePackedXY(&p->x0, read0);
					CtrGpu_WritePackedXY(&p->x2, read1);
					CtrGpu_WritePackedXY(&p->x1, write0);
					CtrGpu_WritePackedXY(&p->x3, write1);

					// prim/code
					setPolyG4(p);

					// Prim/OT
					// addPrim(ot, p); works but uses more instructions.
					p->tag = CtrGpu_PackOTTag(*ot, 0x8000000);
					*ot = CtrGpu_PrimToOTLink24(p);

					p++;
				}
			}
		}

		lightR = lightL;
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* Cleared inside the same draw that set it, after the last projected
	 * quad -- same discipline as DrawSky_Full's bracket. The exempt range
	 * is the remap's walk-side half: it tells the resolver which packets
	 * take their STEREO depth from these (remapped) captures while their
	 * OCCLUSION depth comes from retail's OT layering like it always did
	 * -- flag over everything mid-wipe, behind the models in menus. See
	 * GTECapture_NodeExempt for the
	 * two-channel contract. primMem.cursor still holds this draw's first
	 * prim here (it advances only below), so [cursor, p) is the flag's
	 * packets and nothing else. */
	GTECapture_SetStoreDepthRemap(0);
	GTECapture_SetUIBandExemptRange(gGT->backBuffer->primMem.cursor, p);
#endif

	gGT->backBuffer->primMem.cursor = p;
	sdata->RaceFlag_ElapsedTime += gGT->elapsedTimeMS * 100;
}
