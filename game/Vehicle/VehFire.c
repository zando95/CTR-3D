#include <common.h>
#include <native_retro_fueled.h>

enum
{
	VEH_FIRE_AUDIO_HIGH_THRESHOLD = 0x80,
	VEH_FIRE_AUDIO_MEDIUM_THRESHOLD = 0x40,
	VEH_FIRE_AUDIO_VOLUME_LOW = 0x80,
	VEH_FIRE_AUDIO_VOLUME_MEDIUM = 0xc0,
	VEH_FIRE_AUDIO_VOLUME_HIGH = 0xff,
	VEH_FIRE_AUDIO_DISTORT_LOW = 0x94,
	VEH_FIRE_AUDIO_DISTORT_HIGH = 0x6c,
	VEH_FIRE_VOICELINE_HIGH_BOOST_ID = 0x10,
	VEH_FIRE_VOICELINE_PRIORITY = 0x10,
	VEH_FIRE_AUDIO_SFX = 0xd,
	VEH_FIRE_AUDIO_COOLDOWN = 0xf0,

	VEH_FIRE_POWER_SLIDE_DISAPPEAR_FRAMES = 2,
	VEH_FIRE_NO_DISAPPEAR = -1,
	VEH_FIRE_VISIBILITY_COOLDOWN = 0x60,
	VEH_FIRE_SPEED_CAP_SHIFT = 8,
	VEH_FIRE_SIZE_SHIFT = 6,
	VEH_FIRE_SIZE_BASE = 5,
	VEH_FIRE_SIZE_MAX = 8,
	VEH_FIRE_CAMERA_SHAKE_FLAG = 0x80,
	VEH_FIRE_RUMBLE_CHANNEL = 8,
	VEH_FIRE_RUMBLE_FORCE = 0x7f,
};

enum
{
	VEH_FIRE_INITIAL_INSTANCE_FLAGS = DEPTH_FADE | DRAW_BILLBOARD | HIDE_MODEL,
};

CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_HIGH_THRESHOLD == 0x80);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_MEDIUM_THRESHOLD == 0x40);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_VOLUME_LOW == 0x80);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_VOLUME_MEDIUM == 0xc0);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_VOLUME_HIGH == 0xff);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_DISTORT_LOW == 0x94);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_DISTORT_HIGH == 0x6c);
CTR_STATIC_ASSERT(VEH_FIRE_VOICELINE_HIGH_BOOST_ID == 0x10);
CTR_STATIC_ASSERT(VEH_FIRE_VOICELINE_PRIORITY == 0x10);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_SFX == 0xd);
CTR_STATIC_ASSERT(VEH_FIRE_AUDIO_COOLDOWN == 0xf0);
CTR_STATIC_ASSERT(VEH_FIRE_POWER_SLIDE_DISAPPEAR_FRAMES == 2);
CTR_STATIC_ASSERT(VEH_FIRE_NO_DISAPPEAR == -1);
CTR_STATIC_ASSERT(VEH_FIRE_VISIBILITY_COOLDOWN == 0x60);
CTR_STATIC_ASSERT(VEH_FIRE_SPEED_CAP_SHIFT == 8);
CTR_STATIC_ASSERT(VEH_FIRE_SIZE_SHIFT == 6);
CTR_STATIC_ASSERT(VEH_FIRE_SIZE_BASE == 5);
CTR_STATIC_ASSERT(VEH_FIRE_SIZE_MAX == 8);
CTR_STATIC_ASSERT(VEH_FIRE_CAMERA_SHAKE_FLAG == 0x80);
CTR_STATIC_ASSERT(VEH_FIRE_RUMBLE_CHANNEL == 8);
CTR_STATIC_ASSERT(VEH_FIRE_RUMBLE_FORCE == 0x7f);
CTR_STATIC_ASSERT(VEH_FIRE_INITIAL_INSTANCE_FLAGS == 0x1040080);

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x8005ab24-0x8005abfc.
void VehFire_Audio(struct Driver *driver, int speed_cap)
{
	// if turbo audio cooldown is not done
	if (driver->VehFire_AudioCooldown != 0)
	{
		return;
	}

	u32 distortion = VEH_FIRE_AUDIO_DISTORT_LOW;
	u32 volume = VEH_FIRE_AUDIO_VOLUME_LOW;
	u32 echo = 0;

	if (speed_cap >= VEH_FIRE_AUDIO_HIGH_THRESHOLD)
	{
		// max volume
		volume = VEH_FIRE_AUDIO_VOLUME_HIGH;

		// distort
		distortion = VEH_FIRE_AUDIO_DISTORT_HIGH;

		Voiceline_RequestPlay(VEH_FIRE_VOICELINE_HIGH_BOOST_ID, data.characterIDs[driver->driverID], VEH_FIRE_VOICELINE_PRIORITY);

		goto Skip;
	}

	if (speed_cap >= VEH_FIRE_AUDIO_MEDIUM_THRESHOLD)
	{
		// 3/4 volume
		volume = VEH_FIRE_AUDIO_VOLUME_MEDIUM;

		// no distort
		distortion = HOWL_SFX_DISTORTION_NONE;

		goto Skip;
	}

Skip:

	// if echo is required
	if ((driver->actionsFlagSet & ACTION_ENGINE_ECHO) != 0)
	{
		echo = 1;
	}

	OtherFX_Play_LowLevel(VEH_FIRE_AUDIO_SFX, 1, HowlSfx_Pack(HOWL_SFX_LR_CENTER, distortion, volume, echo));

	// turbo audio cooldown 0.24s
	driver->VehFire_AudioCooldown = VEH_FIRE_AUDIO_COOLDOWN;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x8005abfc-0x8005b0c4.

// param1 - driver
// param2 - reserves to add
// param3 - add type
// param4 - fire level
void VehFire_Increment(struct Driver *driver, int reserves, u32 type, int fireLevel)
{
	u8 kartState;
	s8 count;

	int newFireSpeedCap;
	int newFireSize;
	int oldOTT;

	u32 addFlags;
	struct Turbo *turboObj;
	struct Thread *turboThread;
	struct Instance *turboInst1;
	struct Instance *turboInst2;

	struct GameTracker *gGT = sdata->gGT;
	if (
	    // if this is a turbo pad
	    ((type & TURBO_PAD) != 0) &&

	    // racer is in accel prevention (holding square)
	    ((driver->actionsFlagSet & ACTION_ACCEL_PREVENTION) != 0))
	{
		// do nothing
		return;
	}

	if (
	    // Player / AI structure + 0x4a shows driver index (0-7)

	    // If this is the first driver (P1) and
	    (driver->driverID == '\0') &&

	    // player of any kind
	    (driver->instSelf->thread->modelIndex == DYNAMIC_PLAYER))
	{
		// Add Reserves to ghost buffer
		GhostTape_WriteBoosts(reserves, (u8)type, fireLevel);
	}

	kartState = driver->kartState;

	if (kartState == KS_SPINNING)
	{
		return;
	}
	if (kartState == KS_MASK_GRABBED)
	{
		return;
	}
	if (kartState == KS_BLASTED)
	{
		return;
	}

	// Clear the turbo input latch and mark an outside turbo.
	driver->actionsFlagSet = (driver->actionsFlagSet & ~ACTION_TURBO_INPUT_LATCH) | ACTION_NEW_BOOST;

	// turbo thread bucket
	turboThread = gGT->threadBuckets[TURBO].thread;

	// check all turbo threads
	while (turboThread != 0)
	{
		// if this turbo thread is owned by this driver
		if (((struct Turbo *)turboThread->object)->driver == driver)
		{
			// quit, turboThread is now this driver's turbo thread
			break;
		}

		// next turbo thread in bucket
		turboThread = turboThread->siblingThread;
	}

	// if no turbo exists, create one
	if (turboThread == 0)
	{
#if BUILD < JpnRetail

		driver->numTurbos = 1;

#else

		// Japan retail gates this through the extra turbo state byte.
		if (driver->japanTurboUnknown == 0)
		{
			driver->numTurbos = 1;
			if ((driver->numTurbosHighScore < 1) && ((gGT->gameMode1 & END_OF_RACE) == 0))
			{
				driver->numTurbosHighScore = 1;
			}
		}
		else
		{
			driver->numTurbos = (s16)CTR_MipsAddLo((u16)driver->numTurbos, 1);
			if ((driver->numTurbosHighScore < driver->numTurbos) && ((gGT->gameMode1 & END_OF_RACE) == 0))
			{
				driver->numTurbosHighScore = driver->numTurbos;
			}
		}

#endif

#if defined(CTR_NATIVE)
		turboInst1 = INSTANCE_BirthWithThread(STATIC_TURBO_EFFECT, 0, SMALL, TURBO, VehTurbo_ThTick, sizeof(struct Turbo), 0);

		turboObj = 0;

		if (turboInst1 != 0)
		{
			// get thread, ignore all collisions
			turboThread = turboInst1->thread;
			turboThread->flags |= THREAD_FLAG_DISABLE_COLLISION;

			// get object, set essentials
			turboObj = turboThread->object;
			turboObj->driver = driver;
			turboObj->fireVisibilityCooldown = 0;

			// make flame disappear after
			// 	- powerslide: two frames (quick death)
			//	- all others: -1 frames (255 = 'no' death)
			if ((type & POWER_SLIDE_HANG_TIME) != 0)
			{
				count = VEH_FIRE_POWER_SLIDE_DISAPPEAR_FRAMES;
			}
			else
			{
				count = VEH_FIRE_NO_DISAPPEAR;
			}
			turboObj->fireDisappearCountdown = count;

			// player of any kind
			if (driver->instSelf->thread->modelIndex == DYNAMIC_PLAYER)
			{
				turboObj->fireAudioDistort = 0;

				if (driver->kartState != KS_CRASHING)
				{
					VehFire_Audio(driver, fireLevel);
				}
			}

			turboThread->funcThDestroy = VehTurbo_ThDestroy;

			// turbo #2
			turboInst2 = INSTANCE_Birth3D(gGT->modelPtr[STATIC_TURBO_EFFECT], // model
			                              &sdata->s_turbo2[0],                // name
			                              turboThread                         // parent thread
			);

			// 2P 3P 4P flags
			addFlags = 0;

			turboObj->inst = turboInst2;
			turboObj->fireAnimIndex = 0;

			// 1P flags
			if (gGT->numPlyrCurrGame == 1)
			{
				addFlags = VISIBLE_DURING_GAMEPLAY;
			}

			// Initial fire instances are billboarded but hidden until the turbo tick reveals them.
			turboInst1->flags = turboInst1->flags | addFlags | VEH_FIRE_INITIAL_INSTANCE_FLAGS;
			turboInst2->flags = turboInst2->flags | addFlags | VEH_FIRE_INITIAL_INSTANCE_FLAGS;
		}
#else
		turboObj = 0;
		turboInst1 = 0;
#endif
	}

	// if turbo exists, modify it
	else
	{
		// get the turbo's object
		turboObj = turboThread->object;

		// get the turbo's instances
		turboInst1 = turboThread->inst;
		turboInst2 = turboObj->inst;

		// remove "dead thread" flag
		turboThread->flags &= ~THREAD_FLAG_DEAD;

		// turbo pad
		if ((type & TURBO_PAD) != 0)
		{
			// only increase counter on the first frame of turbo pad

			if ((driver->actionsFlagSetPrevFrame & ACTION_NEW_BOOST) == 0)
			{
				driver->numTurbos = (s16)CTR_MipsAddLo((u16)driver->numTurbos, 1);

#if BUILD == JpnRetail
				// the japanese version of the game keeps track of your highest turbo chain in a race
				if (driver->numTurbosHighScore < driver->numTurbos && (gGT->gameMode1 & END_OF_RACE) == 0)
					driver->numTurbosHighScore = driver->numTurbos;
#endif
			}
		}

		// all other boosts
		else
		{
			// make fire invisible for the sake of the visibility cooldown as explained in common.h
			turboInst1->flags |= DEPTH_FADE | HIDE_MODEL;
			turboInst2->flags |= DEPTH_FADE | HIDE_MODEL;

			turboObj->fireVisibilityCooldown = VEH_FIRE_VISIBILITY_COOLDOWN;
			driver->numTurbos = (s16)CTR_MipsAddLo((u16)driver->numTurbos, 1);
#if BUILD == JpnRetail
			// the japanese version of the game keeps track of your highest turbo chain in a race
			if (driver->numTurbosHighScore < driver->numTurbos && (gGT->gameMode1 & END_OF_RACE) == 0)
				driver->numTurbosHighScore = driver->numTurbos;
#endif
		}

		turboObj->fireDisappearCountdown = VEH_FIRE_NO_DISAPPEAR;
		turboInst1->alphaScale = 0;
		turboInst2->alphaScale = 0;

		// player of any kind
		if (driver->instSelf->thread->modelIndex == DYNAMIC_PLAYER)
		{
			if (
			    // if racer is not getting an Outside turbo (turbo pad or powerup),
			    // this prevents audio-spam from multiple boosts
			    ((driver->actionsFlagSet & ACTION_NEW_BOOST) == 0) || ((driver->actionsFlagSetPrevFrame & ACTION_NEW_BOOST) == 0))

			{
				turboObj->fireAudioDistort = 0;
				VehFire_Audio(driver, fireLevel);
			}
		}
	}

	newFireSpeedCap = CTR_MipsAddLo(
	    CTR_MipsSra(CTR_MipsMulLo(fireLevel, CTR_MipsSubLo(driver->const_SacredFireSpeed, driver->const_SingleTurboSpeed)), VEH_FIRE_SPEED_CAP_SHIFT),
	    driver->const_SingleTurboSpeed);

	if (
	    // any gain in boost,
	    // resize to gain boost
	    (
	        // Reserves are equal to zero
	        // OR
	        // speed cap has been raised
	        (driver->reserves == 0) || (driver->fireSpeedCap < newFireSpeedCap)) ||

	    // OR

	    // you have USF, and boosted on a non-STP,
	    // resize fire to lose size
	    (
#ifdef CTR_NATIVE
	        // Retro-Fueled: a boost never shrinks a USF fire
	        !NativeRetroFueled_Active() &&
#endif
	        // Current speed cap is greater than 0x1000
	        // AND
	        // You are not on a super turbo pad
	        (int)driver->const_SacredFireSpeed < (int)driver->fireSpeedCap && ((driver->stepFlagSet & COLL_STEP_TRIGGER_SUPER_TURBO_PAD) == 0)))

	{
		driver->fireSpeedCap = (s16)newFireSpeedCap;

		if (turboObj != 0)
		{
			// modify, cap, and save the size of the fire
			newFireSize = CTR_MipsAddLo(CTR_MipsSra(fireLevel, VEH_FIRE_SIZE_SHIFT), VEH_FIRE_SIZE_BASE);
			if (newFireSize > VEH_FIRE_SIZE_MAX)
			{
				newFireSize = VEH_FIRE_SIZE_MAX;
			}
			turboObj->fireSize = (s16)newFireSize;
		}
	}

	// turbo item boost
	if ((type & TURBO_ITEM) != 0)
	{
		driver->actionsFlagSet |= ACTION_TURBO_ITEM;
	}

	// turbo pad, boost powerup
	if ((type & FREEZE_RESERVES_ON_TURBO_PAD) != 0)
	{
		// this adds reserves on the first frame you touch the turbo pad,
		// then prevent reserves from decreasing until the first frame
		// you leave the turbo pad

		oldOTT = driver->turbo_outsideTimer;

		if (oldOTT < reserves)
		{
			int reserveDelta = CTR_MipsSubLo(reserves, (u16)driver->turbo_outsideTimer);
			driver->reserves = (s16)CTR_MipsAddLo((u16)driver->reserves, reserveDelta);
			driver->turbo_outsideTimer = (s16)reserves;
		}
	}

	// startline, hang time, powerslide
	else if ((type & SUPER_ENGINE) == 0)
	{
		// increase reserves BY param2
		driver->reserves = (s16)CTR_MipsAddLo((u16)driver->reserves, reserves);
	}

	// super engine
	else
	{
		// set reserves to reserves parameter instead of incrementing
		if (driver->reserves < reserves)
		{
			driver->reserves = (s16)reserves;
		}
	}

	// player of any kind
	if (driver->instSelf->thread->modelIndex == DYNAMIC_PLAYER)
	{
		// CameraDC flag
		gGT->cameraDC[driver->driverID].flags |= VEH_FIRE_CAMERA_SHAKE_FLAG;

		// gamepad vibration
		GAMEPAD_ShockForce1(driver, VEH_FIRE_RUMBLE_CHANNEL, VEH_FIRE_RUMBLE_FORCE);
	}
	// #endif
}

#ifdef CTR_NATIVE
// Retro-Fueled blue fire. The exhaust flame and plume palettes are 16-colour rows in NTSC-U VRAM, stored
// as little-endian 1555 pairs (the layout LoadImage takes). The rows live in shared.vrm, which is written
// once at boot, so the tracked state always matches VRAM. Every kart's fire samples the same two rows, so
// the swap follows the local kart (drivers[0]) alone.
static const u32 s_retroFuelFlamesRed[8] = {0x80DA809A, 0x805A80BA, 0x811A803A, 0x819A81DA, 0x813A817A, 0x829A821A, 0x835A839A, 0x82DA831A};
static const u32 s_retroFuelFlamesBlue[8] = {0xFF25FF65, 0xFFA5FF45, 0xFEE5FFC5, 0xFE65FE25, 0xFEC5FE85, 0xFD65FDE5, 0xFCA5FC65, 0xFD25FCE5};
static const u32 s_retroFuelPlumesRed[8] = {0x000083FF, 0x821B833E, 0x829D81B8, 0x80F6800D, 0x819A8054, 0x80D98011, 0x80078019, 0x84008015};
static const u32 s_retroFuelPlumesBlue[8] = {0x7FFFF260, 0xFC20F980, 0xF400FC20, 0xF980AC00, 0xF260F980, 0xB400FCA0, 0x8800FC20, 0x8800FC20};

enum
{
	RETRO_FUEL_FLAME_CLUT_X = 176,
	RETRO_FUEL_FLAME_CLUT_Y = 256,
	RETRO_FUEL_PLUME_CLUT_X = 464,
	RETRO_FUEL_PLUME_CLUT_Y = 257,
	RETRO_FUEL_CLUT_WIDTH = 16,

	// What the two rows hold. Boot VRAM is red.
	RETRO_FUEL_PAL_RED = 0,
	RETRO_FUEL_PAL_BLUE = 1,
};

static s32 s_retroFuelPalState = RETRO_FUEL_PAL_RED;

static void NativeRetroFueled_UploadFlamePalette(const u32 *flames, const u32 *plumes)
{
	RECT rect;

	rect.w = RETRO_FUEL_CLUT_WIDTH;
	rect.h = 1;

	rect.x = RETRO_FUEL_FLAME_CLUT_X;
	rect.y = RETRO_FUEL_FLAME_CLUT_Y;
	LoadImage(&rect, (void *)flames);

	rect.x = RETRO_FUEL_PLUME_CLUT_X;
	rect.y = RETRO_FUEL_PLUME_CLUT_Y;
	LoadImage(&rect, (void *)plumes);
}

// Runs once per gameplay frame. Uploads only when the wanted palette differs from the tracked one: every
// VRAM write invalidates the 3DS texture decode cache for the rows' cells.
void NativeRetroFueled_UpdateFlamePalette(void)
{
	struct Driver *driver;
	s32 wanted = RETRO_FUEL_PAL_RED;
	int active = NativeRetroFueled_Active();

	if (!active && (s_retroFuelPalState == RETRO_FUEL_PAL_RED))
	{
		return;
	}

	if (active)
	{
		driver = sdata->gGT->drivers[0];

		// no reserves, no fire: keep whatever the rows hold until the next boost decides
		if ((driver == NULL) || (driver->reserves == 0))
		{
			return;
		}

		// same USF test as VehFire_Increment's fire-resize clause
		if ((int)driver->fireSpeedCap > (int)driver->const_SacredFireSpeed)
		{
			wanted = RETRO_FUEL_PAL_BLUE;
		}
	}

	// wanted stays red while the mode is off, so switching it off at runtime restores red
	if (s_retroFuelPalState != wanted)
	{
		if (wanted == RETRO_FUEL_PAL_BLUE)
		{
			NativeRetroFueled_UploadFlamePalette(s_retroFuelFlamesBlue, s_retroFuelPlumesBlue);
		}
		else
		{
			NativeRetroFueled_UploadFlamePalette(s_retroFuelFlamesRed, s_retroFuelPlumesRed);
		}
		s_retroFuelPalState = wanted;
	}
}
#endif
