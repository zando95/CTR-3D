#ifndef OXIDE_FIT_H
#define OXIDE_FIT_H

// Nitros Oxide as a racer (native-only; retail only races him as a boss or ghost). His model is far larger than
// a kart, and everything the game sizes for a kart (the Aku/Uka mask, the shield bubble, the ground shadow, the
// exhaust pipes) is derived here from one number.
//
// OXIDE_RACE_SCALE is the size a regular racer Oxide is drawn at, as a 12-bit fraction of his model file: 4096 is
// the size the model file carries, 2731 is two-thirds of it. Change this one line to resize him; the effects
// below follow. It applies per craft at draw time, never to the shared model: the Adventure boss and the disc's
// own N. Oxide ghost are drawn at 4096, and a regular racer Oxide (human or CPU, in any mode) or a ghost a
// player recorded as Oxide is drawn at OXIDE_RACE_SCALE. See OxideFit_InstanceSize.
#define OXIDE_RACE_SCALE 2731

// His effects at full model size, as 12-bit multiples of the kart-sized value: the shield bubble, the ground
// shadow and the Aku/Uka mask (head, orbit and beam) grow by these when the craft is drawn at 4096, and by these
// times its size otherwise. The mask stays smaller than the bubble: the race camera sits close behind him, and a
// larger mask orbit passes through it.
#define OXIDE_FIT_SHIELD_AT_FULL_SIZE 8192
#define OXIDE_FIT_SHADOW_AT_FULL_SIZE 8192
#define OXIDE_FIT_MASK_AT_FULL_SIZE   6144

// His exhaust pipe openings. The hi-LOD racer model has four stacks, two per side; the exhaust leaves the rear
// one on each side. Its top is a quad of four vertices, named here by index into the model's vertex stream (the
// n-th command without the cached-vertex bit), which OxideFit_PipeOffsets looks up in the animation frame being
// drawn so the smoke and the boost fire follow his banking. OXIDE_FIT_PIPE_VERTS[0] is the +x pipe, [1] the -x.
#define OXIDE_FIT_VERTEX_COUNT 594
#define OXIDE_FIT_PIPE_VERTS_MAX 55
#define OXIDE_FIT_PIPE_VERTS \
	{ \
		{52, 53, 54, 55}, {40, 41, 42, 43} \
	}

// The centre of each pipe top in the model's neutral pose (animation 0, middle frame), in vertex units of the
// model file. A built table that disagrees with these by more than OXIDE_FIT_PIPE_TOLERANCE is rejected, and
// they are the pipe positions used when no table is available.
#define OXIDE_FIT_EXHAUST_X         304
#define OXIDE_FIT_EXHAUST_Y         532
#define OXIDE_FIT_EXHAUST_Z         -370
#define OXIDE_FIT_PIPE_TOLERANCE    24
#define OXIDE_FIT_PIPE_MAX_ANIMS    4
#define OXIDE_FIT_PIPE_MAX_FRAMES   24

#if defined(CTR_NATIVE)

struct Driver;
struct Instance;

// How this instance is drawn if it is an Oxide craft (a player, CPU or ghost thread whose model is his), as a
// 12-bit fraction of his model file's size; 0 for everything else.
s32 OxideFit_InstanceSize(const struct Instance *inst);

// The same for a driver's own instance.
s32 OxideFit_DriverSize(const struct Driver *driver);

// Multiplies a kart-sized value by an effect's growth for this driver's craft (OXIDE_FIT_*_AT_FULL_SIZE; every
// other racer is unchanged).
s32 OxideFit_Grow(const struct Driver *driver, s32 value, s32 atFullSize);

// Both pipe openings of this Oxide instance in the pose it is drawing (its animation and frame), as offsets from
// the instance origin in emitter units (an emitter position is a world position times 256), through the scale the
// renderer draws it at. out[0] is the +x pipe, out[1] the -x pipe. Returns 0 and leaves out alone for any other
// model; for his model with no usable frame data it returns the neutral-pose pipes.
int OxideFit_PipeOffsets(const struct Instance *inst, s32 out[2][3]);

// An offset as the halfword the emitter and the fire transform take, saturated.
static inline s16 OxideFit_ToS16(s32 value)
{
	return (s16)((value > 0x7fff) ? 0x7fff : ((value < -0x7fff) ? -0x7fff : value));
}

#else

struct Driver;

static inline s32 OxideFit_Grow(const struct Driver *driver, s32 value, s32 atFullSize)
{
	(void)driver;
	(void)atFullSize;
	return value;
}

#endif

#endif
