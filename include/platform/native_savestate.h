#ifndef PLATFORM_NATIVE_SAVESTATE_H
#define PLATFORM_NATIVE_SAVESTATE_H

#include <macros.h>

#if defined(CTR_INTERNAL) && !defined(__3DS__)
void NativeSaveState_RequestSave(void);
void NativeSaveState_RequestLoad(void);
void NativeSaveState_BeginFrame(void);
#elif defined(CTR_INTERNAL)
/* Save states are PC-only (F5/F8): nothing on 3DS can request one, so the 3DS
 * build compiles them out and keeps no-op stand-ins for their callers. */
static inline void NativeSaveState_RequestSave(void)
{
}
static inline void NativeSaveState_RequestLoad(void)
{
}
static inline void NativeSaveState_BeginFrame(void)
{
}
#endif

#endif
