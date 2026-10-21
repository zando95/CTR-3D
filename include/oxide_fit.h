#ifndef OXIDE_FIT_H
#define OXIDE_FIT_H

// Nitros Oxide as a racer (native-only; retail only races him as a boss or ghost). His model is far larger than
// a kart, and everything the game sizes for a kart (the Aku/Uka mask, the shield bubble, the ground shadow, the
// exhaust pipes) is derived here from one number.
//
// OXIDE_RACE_SCALE is his race size as a 12-bit fraction of his model file: 4096 is the size the model file
// carries (the shipped default), 2731 is two-thirds of it. Change this one line to resize him; the effects
// below follow.
#define OXIDE_RACE_SCALE 4096

// His effects at full model size, as 12-bit multiples of the kart-sized value: the shield bubble, the ground
// shadow and the Aku/Uka mask (head, orbit and beam) grow by these when OXIDE_RACE_SCALE is 4096, and by these
// times OXIDE_RACE_SCALE otherwise. The mask stays smaller than the bubble: the race camera sits close behind
// him, and a larger mask orbit passes through it.
#define OXIDE_FIT_SHIELD_AT_FULL_SIZE 8192
#define OXIDE_FIT_SHADOW_AT_FULL_SIZE 8192
#define OXIDE_FIT_MASK_AT_FULL_SIZE   6144

// His exhaust pipe openings, in the vertex units of his model file at full model size (the right pipe mirrors the
// left), and the emitter's units per model unit (an emitter position is a world position times 256; one model
// unit is about 0.2 world units at full size).
#define OXIDE_FIT_EXHAUST_X 290
#define OXIDE_FIT_EXHAUST_Y 540
#define OXIDE_FIT_EXHAUST_Z -350
#define OXIDE_FIT_EMITTER_PER_MODEL_UNIT 51

#if defined(CTR_NATIVE)

// Multiplies a kart-sized value by an effect's growth for this character (OXIDE_FIT_*_AT_FULL_SIZE; other
// characters are unchanged).
static inline s32 OxideFit_Grow(s32 characterID, s32 value, s32 atFullSize)
{
	if (characterID != NITROS_OXIDE)
	{
		return value;
	}

	return (s32)(((s64)value * ((atFullSize * (s64)OXIDE_RACE_SCALE) >> FRACTIONAL_BITS)) >> FRACTIONAL_BITS);
}

// An exhaust pipe coordinate in his model file's units, as an emitter offset at the size he is raced at.
static inline s32 OxideFit_ModelPos(s32 modelUnits)
{
	return (s32)(((s64)modelUnits * OXIDE_FIT_EMITTER_PER_MODEL_UNIT * OXIDE_RACE_SCALE) >> FRACTIONAL_BITS);
}

// Applies OXIDE_RACE_SCALE to a freshly loaded racer model (no effect on any other model). Idempotent: a header
// that carries the applied scale in its padding halfword is left alone.
void OxideFit_SizeRacerModel(struct Model *model);

#else

static inline s32 OxideFit_Grow(s32 characterID, s32 value, s32 atFullSize)
{
	(void)characterID;
	(void)atFullSize;
	return value;
}

static inline s32 OxideFit_ModelPos(s32 modelUnits)
{
	return modelUnits;
}

#endif

#endif
