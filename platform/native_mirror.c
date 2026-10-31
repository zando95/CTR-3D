/* Mirror mode (3DS only); the contract is in include/native_mirror.h. */

#include <common.h>
#include <native_extra_difficulty.h>
#include <native_mirror.h>

int g_mirrorMode = 0;

#ifdef CTR_INTERNAL
/* dumpreq `mirror`: treat mirror mode as unlocked without touching the card, for testing. */
int g_dbg_mirrorUnlock = 0;
#endif

static int s_mirWorldFrame = 0;
static float s_mirAxisSum = 512.0f;

int NativeMirror_Unlocked(void)
{
#ifdef CTR_INTERNAL
	if (g_dbg_mirrorUnlock != 0)
	{
		return 1;
	}
#endif
	return NativeXDiff_UltraHardComplete();
}

int NativeMirror_Active(void)
{
	return (g_mirrorMode != 0) && NativeMirror_Unlocked();
}

int NativeMirror_World(void)
{
	struct GameTracker *gGT = sdata->gGT;

	return NativeMirror_Active() && (gGT != NULL) && ((gGT->gameMode1 & MAIN_MENU) == 0);
}

int NativeMirror_SteerStick(int stickLX)
{
	if (!NativeMirror_World() || (sdata->gGT->boolDemoMode != 0))
	{
		return stickLX;
	}

	/* the same distance from centre on the other side; 0 (full left) maps to 0x100, kept in a byte */
	stickLX = 0x100 - stickLX;
	return (stickLX > 0xFF) ? 0xFF : stickLX;
}

u32 NativeMirror_SfxLR(u32 lr)
{
	/* the volume tables are indexed by 0xFF - lr (left) and lr (right), so this swaps the two exactly */
	return NativeMirror_World() ? (0xFFu - (lr & 0xFFu)) : lr;
}

int NativeMirror_KeepUnmirrored(struct Instance *inst)
{
	struct Thread *t = inst->thread;
	s16 model;

	if (!NativeMirror_World() || (t == NULL) || (t->inst != inst))
	{
		return NATIVE_MIRROR_KEEP_NONE;
	}

	model = t->modelIndex;

	/* kart and driver are one instance, for the player, the AI and every ghost */
	if ((model == DYNAMIC_PLAYER) || (model == DYNAMIC_ROBOT_CAR) || (model == DYNAMIC_GHOST))
	{
		return (inst->animIndex == VEH_FRAME_ANIM_DRIVE) ? NATIVE_MIRROR_KEEP_RACER_DRIVING : NATIVE_MIRROR_KEEP_CHARACTER;
	}

	/* the held mask and the talking hint mask */
	if ((model == STATIC_AKUAKU) || (model == STATIC_UKAUKA))
	{
		return NATIVE_MIRROR_KEEP_CHARACTER;
	}

	/* podium and credits dancers, and the bosses' heads in their hub cutscenes */
	if (((model >= STATIC_CRASHDANCE) && (model <= STATIC_OXIDEDANCE)) || ((model >= STATIC_PINHEAD) && (model <= STATIC_OXIDEHEAD)))
	{
		return NATIVE_MIRROR_KEEP_CHARACTER;
	}

	return NATIVE_MIRROR_KEEP_NONE;
}

int NativeMirror_ProjectedX(int sx)
{
	if (!NativeMirror_World())
	{
		return sx;
	}

	/* OFX is 16.16 */
	return 2 * (CFC2_S(24) >> 16) - sx;
}

int NativeMirror_LatchFrame(int dispW)
{
	s_mirAxisSum = (float)dispW;
	s_mirWorldFrame = NativeMirror_World();
	return s_mirWorldFrame;
}

int NativeMirror_WorldFrame(void)
{
	return s_mirWorldFrame;
}

float NativeMirror_AxisSum(void)
{
	return s_mirAxisSum;
}
