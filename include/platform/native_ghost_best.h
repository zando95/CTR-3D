#ifndef PLATFORM_NATIVE_GHOST_BEST_H
#define PLATFORM_NATIVE_GHOST_BEST_H

#include <macros.h>

/* The "best ghost": one saved Time Trial ghost per track, separate from the retail four-slot
 * ghost menu. A finished Time Trial run that beats the track's stored best (or finds none)
 * replaces it, with any character. With the setting on, picking a Time Trial track skips the
 * ghost menu and races the track's best ghost (no ghost when none is stored). On PC every
 * function is a no-op or returns 0, so the retail ghost menu stays. */

struct GhostHeader;

/* OPTIONS toggle "AUTOSAVE GHOST" (options.cfg key "bestghost"). Off means retail behavior and
 * no file access. */
extern int g_ghostBestEnabled;

/* Nonzero when track select must skip the ghost menu and load the best ghost itself. */
int NativeGhostBest_SkipsGhostMenu(void);

/* Called on the finish frame with the tape GhostTape_End just finalized. Copies it aside and
 * marks it pending; performs no file I/O. Ignored when the setting is off, when a cheat that
 * can change a Time Trial time is active, or when the header is not a finished tape. */
void NativeGhostBest_OnRaceEnd(s16 levelID, const struct GhostHeader *gh);

/* Writes the pending ghost if it beats the stored best. Call it wherever a finished race is
 * left: the results screen's exits and the platform shutdown. No-op when nothing is pending. */
void NativeGhostBest_Flush(void);

/* Flushes, then reads the track's best ghost into dst (GHOST_RECORD_BUFFER_SIZE bytes, the
 * ghost tape buffer track select allocates), checking its CRC and falling back to the backup
 * copy. Returns 1 on success; on failure returns 0 and leaves dst zeroed. */
int NativeGhostBest_TryLoad(s16 levelID, struct GhostHeader *dst);

#endif
