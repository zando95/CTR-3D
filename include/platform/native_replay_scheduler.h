#ifndef PLATFORM_NATIVE_REPLAY_SCHEDULER_H
#define PLATFORM_NATIVE_REPLAY_SCHEDULER_H

#include <macros.h>

#if defined(CTR_INTERNAL)
struct NativeReplaySchedulerFrameInfo
{
	s32 frameTimer;
	s32 frameCounter;
	s32 timer;
	s32 framesInThisLEV;
	s32 elapsedTimeMS;
	s32 msInThisLEV;
	s32 elapsedEventTime;
	s32 mainGameState;
	s32 loadingStage;
	s32 levelID;
	u32 mixRandomNumber;
	u32 audioRNG;
	u32 deadcoed0;
	u32 deadcoed1;
	u32 advRng0;
	u32 advRng1;
};

#if !defined(__3DS__)
int NativeReplayScheduler_PrepareReportFromArgs(int argc, char **argv);
int NativeReplayScheduler_ConfigureFromArgs(int argc, char **argv);
void NativeReplayScheduler_Shutdown(void);
int NativeReplayScheduler_RequestStart(void);
int NativeReplayScheduler_RequestStop(void);
int NativeReplayScheduler_BeginFrame(const struct NativeReplaySchedulerFrameInfo *info);
int NativeReplayScheduler_ConsumeVSyncPacket(int requestedVBlanks, int *emittedVBlanks);
int NativeReplayScheduler_ConsumeFrameElapsedTimeMS(int *elapsedTimeMS);
int NativeReplayScheduler_EndFrame(const struct NativeReplaySchedulerFrameInfo *info);
void NativeReplayScheduler_RecordVSyncPacket(int emittedVBlanks);
#else
/* Replays are PC-only: they are armed from command-line arguments or a PC key,
 * neither of which a 3DS launch has, so the 3DS build compiles the scheduler
 * out. These stand-ins return what the scheduler returns with no replay
 * configured. */
static inline int NativeReplayScheduler_PrepareReportFromArgs(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}
static inline int NativeReplayScheduler_ConfigureFromArgs(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}
static inline void NativeReplayScheduler_Shutdown(void)
{
}
static inline int NativeReplayScheduler_RequestStart(void)
{
	return 0;
}
static inline int NativeReplayScheduler_RequestStop(void)
{
	return 0;
}
static inline int NativeReplayScheduler_BeginFrame(const struct NativeReplaySchedulerFrameInfo *info)
{
	(void)info;
	return 0;
}
static inline int NativeReplayScheduler_ConsumeVSyncPacket(int requestedVBlanks, int *emittedVBlanks)
{
	(void)requestedVBlanks;
	(void)emittedVBlanks;
	return 0;
}
static inline int NativeReplayScheduler_ConsumeFrameElapsedTimeMS(int *elapsedTimeMS)
{
	(void)elapsedTimeMS;
	return 0;
}
static inline int NativeReplayScheduler_EndFrame(const struct NativeReplaySchedulerFrameInfo *info)
{
	(void)info;
	return 0;
}
static inline void NativeReplayScheduler_RecordVSyncPacket(int emittedVBlanks)
{
	(void)emittedVBlanks;
}
#endif
#endif

#endif
