/*
 * Save-writer thread OS primitives -- see n3ds_save_thread.h for the boundary
 * contract. This TU is compiled with -fshort-enums (libctru's ABI), like the
 * other worker TUs; it must never include a decomp header.
 */

#include <3ds.h>

#include "n3ds_save_thread.h"
#include "n3ds_backend.h"

/* Applications may not run threads above priority 0x18. */
#define N3DS_SAVE_PRIORITY_CEILING 0x18

static Thread s_saveThread;
static LightLock s_saveQueueLock;
static LightEvent s_saveWorkEvent;
static LightEvent s_saveDoneEvent;
static int s_saveWorkerUp;

static void (*s_saveWorkerMainFn)(void);

static void N3DS_SaveThreadEntry(void *arg)
{
	(void)arg;

	s_saveWorkerMainFn();
	threadExit(0);
}

int N3DS_SaveWorkerStart(void (*workerMain)(void))
{
	s32 mainPriority;
	s32 workerPriority;

	if (s_saveWorkerUp)
	{
		return 1;
	}

	LightLock_Init(&s_saveQueueLock);
	LightEvent_Init(&s_saveWorkEvent, RESET_STICKY);
	LightEvent_Init(&s_saveDoneEvent, RESET_STICKY);

	mainPriority = 0x30;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	workerPriority = (mainPriority > N3DS_SAVE_PRIORITY_CEILING) ? mainPriority - 1 : mainPriority;

	s_saveWorkerMainFn = workerMain;
	s_saveThread = threadCreate(N3DS_SaveThreadEntry, NULL, 32 * 1024, workerPriority, 0, false);

	s_saveWorkerUp = (s_saveThread != NULL);
	N3DS_DebugLog(s_saveWorkerUp ? "[CTR-3DS] save writer: UP (core 0)" : "[CTR-3DS] save writer: NO (threadCreate failed) -- saves write inline");
	return s_saveWorkerUp;
}

void N3DS_SaveWorkerStopAndJoin(void)
{
	if (!s_saveWorkerUp)
	{
		return;
	}

	threadJoin(s_saveThread, U64_MAX);
	threadFree(s_saveThread);
	s_saveThread = NULL;
	s_saveWorkerUp = 0;
}

void N3DS_SaveQueueLock(void)
{
	LightLock_Lock(&s_saveQueueLock);
}

void N3DS_SaveQueueUnlock(void)
{
	LightLock_Unlock(&s_saveQueueLock);
}

void N3DS_SaveWorkSignal(void)
{
	LightEvent_Signal(&s_saveWorkEvent);
}

void N3DS_SaveWorkWait(void)
{
	LightEvent_Wait(&s_saveWorkEvent);
	LightEvent_Clear(&s_saveWorkEvent);
}

void N3DS_SaveDoneSignal(void)
{
	LightEvent_Signal(&s_saveDoneEvent);
}

void N3DS_SaveDoneWait(void)
{
	LightEvent_Wait(&s_saveDoneEvent);
	LightEvent_Clear(&s_saveDoneEvent);
}
