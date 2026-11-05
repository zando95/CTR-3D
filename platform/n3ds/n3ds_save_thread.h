#ifndef N3DS_SAVE_THREAD_H
#define N3DS_SAVE_THREAD_H

/*
 * Save-writer thread primitives. Same TU arrangement as the render worker:
 * this header is a plain-scalar boundary, and the implementation is compiled
 * with -fshort-enums to match libctru's ABI. The write queue itself (job
 * storage, ordering, completion callbacks) lives in the unity TU
 * (platform/native_memcard.c); this side owns only the OS objects: the
 * thread, the queue lock, and the work/done events.
 */

/* Starts the writer on core 0, the game thread's own core, which every 3DS
 * model has. The writer spends its life blocked in FS calls, so sharing the
 * core costs the game thread only the few microseconds between them; it runs
 * one priority step above the game thread so a finished FS call is picked up
 * at once rather than at the game thread's next wait. workerMain is the
 * queue's drain loop; it returns once the queue asks it to stop. Returns 1
 * with the thread up, 0 if threadCreate refuses (the caller then writes
 * inline). Idempotent. */
int N3DS_SaveWorkerStart(void (*workerMain)(void));

/* Joins and frees the writer. The caller must have asked workerMain to
 * return first (the join would otherwise never return). No-op if the writer
 * never came up. */
void N3DS_SaveWorkerStopAndJoin(void);

void N3DS_SaveQueueLock(void);
void N3DS_SaveQueueUnlock(void);

/* Work event: the game thread signals after appending a job; the writer
 * waits, clears, then drains everything it can see. Sticky, so a signal
 * that lands between the writer's drain and its next wait is never lost. */
void N3DS_SaveWorkSignal(void);
void N3DS_SaveWorkWait(void);

/* Done event: the writer signals after each finished job; the game thread
 * waits, then re-checks its condition. Sticky for the same reason; only the
 * game thread ever waits on it. */
void N3DS_SaveDoneSignal(void);
void N3DS_SaveDoneWait(void);

#endif
