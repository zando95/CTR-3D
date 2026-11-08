#ifndef PLATFORM_NATIVE_CHECKPOINT_H
#define PLATFORM_NATIVE_CHECKPOINT_H

#include <macros.h>

#if !defined(__3DS__)
int NativeCheckpoint_GetSize(void);
int NativeCheckpoint_Capture(void *dst, int dstSize);
int NativeCheckpoint_Restore(const void *src, int srcSize);
#if defined(CTR_INTERNAL)
void NativeCheckpoint_OnMempackArenaReset(void);
void NativeCheckpoint_RegisterPointerSlot(void *slot);
#endif
#elif defined(CTR_INTERNAL)
/* Checkpoints serve only PC save states and replays, so the 3DS build compiles
 * them out. Level loads still report every pointer they patch; on 3DS nothing
 * records them. */
static inline void NativeCheckpoint_OnMempackArenaReset(void)
{
}
static inline void NativeCheckpoint_RegisterPointerSlot(void *slot)
{
	(void)slot;
}
#endif

#endif
