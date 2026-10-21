#ifndef NATIVE_RETRO_FUELED_H
#define NATIVE_RETRO_FUELED_H

/* Retro-Fueled mode: the retail ARROW cheat (Super Turbo Pads) also switches on a
 * set of boost-economy changes, ported from the CTR-ModSDK Retro-Fueled module
 * (TheRedhotbr; blue fire palette by Avery):
 *
 *   - a boost that isn't a pad (power slide, hang time, turbo item, start boost)
 *     no longer knocks the kart out of USF (VehFire_Increment);
 *   - every super-turbo pad grants a 1 s reserve instead of retail's 0x78
 *     (VehPhysForce_CollideDrivers);
 *   - Square+Down, or Square during a landing boost, keeps the reserves, and when
 *     Cross is not held the kart brakes/reverses instead of assuming Cross
 *     (VehPhysProc_Driving_PhysLinear);
 *   - the exhaust flame and plume palettes turn blue while the kart is at USF
 *     speed (NativeRetroFueled_UpdateFlamePalette), and the menu/loading flag
 *     turns blue (RaceFlag.c, flag style 3).
 *
 * The game is one-player only, so every check reads the cheat flag and, where a
 * driver is needed, drivers[0].
 *
 * g_retroFueled is the master switch, defined in platform/native_options.c: ON on
 * 3DS, OFF on PC (PC stays retail). NativeRetroFueled_Active() is the gate every
 * site tests; with it false each site behaves exactly as retail. */

#ifdef CTR_NATIVE

extern int g_retroFueled;

static inline int NativeRetroFueled_Active(void)
{
	return (g_retroFueled != 0) && ((sdata->gGT->gameMode2 & CHEAT_TURBOPAD) != 0);
}

/* Once per gameplay frame (end of BOTS_UpdateGlobals). Defined in game/Vehicle/VehFire.c. */
void NativeRetroFueled_UpdateFlamePalette(void);

#endif /* CTR_NATIVE */

#endif /* NATIVE_RETRO_FUELED_H */
