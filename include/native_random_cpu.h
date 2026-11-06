#ifndef NATIVE_RANDOM_CPU_H
#define NATIVE_RANDOM_CPU_H

/* Random CPU racers (3DS only, OPTIONS row RANDOM CPUS, default ON).
 *
 * The seven CPU racers of a race are drawn at random instead of retail's fixed "the other starting
 * racers" (LOAD_Robots1P, which drops Pura whenever the player is an unlockable):
 *
 *   Arcade     single races, cups and CTR CHALLENGE: every racer unlocked on the save (the select
 *              screen's own unlock flags, N. Oxide included once unlocked), minus the player.
 *   Adventure  trophy races, CTR challenges and the four gem cups: the other starting racers plus each
 *              key boss whose boss key the profile holds (Ripper Roo, Papu Papu, Komodo Joe,
 *              Pinstripe). A track's speed champion starts on pole whenever he is in the field. The
 *              purple gem cup, boss races, relic races and crystal challenges stay retail.
 *
 * A field is drawn when a race is entered from a menu or hub, and kept through RESTART, RETRY and the
 * rest of a cup. Draws use their own generator, so retail's random sequence is unchanged.
 *
 * Engine classes need nothing: VehBirth_SetConsts gives every CPU its own character's class. N. Oxide's
 * boss-race rules (BOTS.c) apply only in boss races, and OxideFit draws him at racer size outside them.
 *
 * Models: a 1P race loads one disc pack per player character holding exactly the default seven CPU
 * models. Any other field loads per racer instead: the 4P pack of Penta, whose shared models (weapons,
 * HUD) and icon table are identical to the 1P packs', then the player's hi model file, then each CPU's hi
 * and med files (merged into one hi/med/low/box model with the drive animation only, the shape the packs
 * hold). A field that happens to be the default seven loads the retail pack. Defined in
 * platform/native_random_cpu.c. */

#ifdef CTR_NATIVE

struct BigHeader;
struct LoadQueueSlot;
struct Model;

/* The OPTIONS switch: 1 on 3DS, 0 on PC (nothing sets it there). */
extern int g_randomCpus;

/* LOAD_DriverMPK, 1P race branch, after LOAD_Robots1P: draws or keeps this race's field in
 * data.characterIDs[1..7]. Returns 1 when it queued the per-racer files (`callback` gets the shared pack's
 * slot once the last file is in), 0 to let the retail arcade pack load. */
int NativeRandomCpu_QueueRace(struct BigHeader *bigfile, void (*callback)(struct LoadQueueSlot *));

/* LOAD_NextQueuedFile, before it takes a slot: queues the next per-racer files while the queue has room. */
void NativeRandomCpu_PumpQueue(void);

/* Load stage 4, beside the driver-extra reset: forgets the previous level's CPU models. */
void NativeRandomCpu_ClearModels(void);

/* VehBirth_GetModelByName, before the pack's model list: a CPU model built for this race, or NULL. */
struct Model *NativeRandomCpu_FindModel(const char *name);

#endif

#endif
