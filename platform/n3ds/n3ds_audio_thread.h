/*
 * Boundary between the game's unity translation unit and libctru, for the
 * 3DS audio mixer thread. Same rule as n3ds_backend.h (see that file's
 * top-of-file comment for the full three-way header collision this avoids):
 * this header exposes NOTHING from <3ds.h> -- no libctru types, no libctru
 * constants -- and is safe to include from the unity build.
 *
 * n3ds_audio_thread.c is therefore compiled as its own translation unit,
 * same as n3ds_backend.c -- it sees libctru and never sees the game headers.
 * Everything crossing this line uses plain C scalar types.
 */

#ifndef N3DS_AUDIO_THREAD_H
#define N3DS_AUDIO_THREAD_H

/*
 * Try to create the New3DS core-1 mixer worker thread, and bring up the
 * ndsp output sink (see the block of N3DS_Ndsp* functions below). Call
 * exactly once per "3DS audio subsystem is finishing initialization" event
 * -- safe to call more than once across the process lifetime (e.g.
 * NativeAudio_SpuInit runs on every XA play request, not just at boot): if
 * a worker thread is already running, this is a cheap no-op rather than
 * re-initializing locks that may be in use or creating a second thread that
 * would orphan the first.
 *
 * Locks/event/counters are initialized unconditionally on the first real
 * call -- N3DS_AudioLock/N3DS_AudioUnlock must be safe to call from the ~30
 * existing native_audio.c call sites no matter what this function decides
 * about the worker thread.
 *
 * Falls back cleanly (leaves N3DS_AudioThreadIsRunning() false, never spawns
 * a same-core thread) if this is not a New3DS, or if
 * APT_SetAppCpuTimeLimit/threadCreate fails to secure real core-1 time. The
 * ndsp sink comes up (or fails) independently of the worker: the StepVBlank
 * fallback submits from the main thread when no worker exists.
 */
void N3DS_AudioThreadInit(void);

/*
 * O(1): bump the target-vblank counter under the small counter lock and
 * signal the worker. Call once per vblank from the main thread when
 * N3DS_AudioThreadIsRunning() is true. Never blocks on the render lock.
 */
void N3DS_AudioThreadNotifyVBlank(void);

/*
 * Stop and join the worker thread, if one was ever created, but leave the
 * wake handle and ndsp sink up. No-op (null-safe) if no worker is running.
 * N3DS_AudioThreadIsRunning() reads false immediately after, so
 * NativeAudio_StepVBlank's main-thread fallback takes over submitting to the
 * same sink from the next vblank -- a runtime bisect switch, not a shutdown;
 * there is no way back to the worker in this process lifetime.
 */
void N3DS_AudioThreadStopAndJoin(void);

/*
 * Stop and join the worker thread (via N3DS_AudioThreadStopAndJoin), then
 * close the wake handle and tear down the ndsp sink. No-op (null-safe) if
 * N3DS_AudioThreadInit was never called, or never reached a running thread --
 * safe to call unconditionally from NativeAudio_Shutdown.
 */
void N3DS_AudioThreadShutdown(void);

/*
 * Rosalina-freeze bisect: N3DS_AudioThreadShutdown, then latch the sink
 * permanently down -- unlike a plain Shutdown, a later NativeAudio_SpuInit
 * call (every XA play request) will NOT bring ndsp back up. One-way for the
 * rest of the process lifetime; no restart path.
 */
void N3DS_NdspForceOff(void);

/* Did Init actually secure a real core-1 worker thread? If false, the
 * caller must render (and discard) synchronously on the main thread instead
 * -- see NativeAudio_StepVBlank's 3DS fallback branch. */
int N3DS_AudioThreadIsRunning(void);

/* Diagnostic: target - rendered, i.e. how many vblanks of mixing the worker
 * is currently behind. Reads under the small counter lock. */
int N3DS_AudioThreadGetBacklog(void);

/* Diagnostic: cumulative count of vblanks whose render request was dropped
 * because the backlog was already at the cap -- see
 * NATIVE_AUDIO_THREAD_MAX_BACKLOG_VBLANKS in n3ds_audio_thread.c. */
int N3DS_AudioThreadGetDropped(void);

/*
 * Where one reporting window of the worker's time went, in microseconds --
 * the decomposition hardware run 6 asked for (audioDropped 7,661 on an
 * XA-music track proved the worker is the frame governor there, but not
 * which of its jobs is the cost). Reading this DRAINS it: every field is
 * per-window, not since-boot, so the renderer can print it directly.
 *
 * Read the fields against each other, not in isolation: `wakes` should be
 * ~60 per second of window -- materially fewer means the loop itself
 * overruns its 16.7ms budget -- and lockWaitUs is time the game thread held
 * the render lock against the worker, i.e. contention, not work.
 * `renderUs` is the render call alone (lock wait excluded); `lifecycleUs` +
 * `mirrorUs` are its two halves, timed on the game side of the boundary.
 *
 * `parcelUs` and `pumpUs` are wall time INCLUDING any wait for the render
 * lock, because both take it deep inside native_audio.c. lockWaitUs is the
 * worker's total wait across all three, so subtract it before calling either
 * of them expensive -- a worker stuck behind a main-thread bulk load
 * otherwise reads as costly XA work when it is really contention.
 */
struct N3DS_AudioWorkerProfile
{
	int wakes;
	int vblanks;
	int parcels;
	int lockWaitUs;
	int renderUs;
	int lifecycleUs;
	int mirrorUs;
	int parcelUs;
	int pumpUs;
};

void N3DS_AudioThreadGetProfile(struct N3DS_AudioWorkerProfile *out);

/* svcGetSystemTick, exposed so the unity TU can time its own side of the
 * boundary on the same clock the worker uses (it cannot include <3ds.h>).
 * Returns 0 off-3DS callers never see -- this header is 3DS-only. */
unsigned long long N3DS_AudioTicksNow(void);

/*
 * Real lock backing SDL_LockAudioStream/SDL_UnlockAudioStream on 3DS (see
 * sdl3_compat_3ds.c). Protects s_audio (native_audio.c) during rendering --
 * the same role the real SDL_AudioStream lock already plays for PC via
 * NativeAudio_LockOutput/UnlockOutput. Held by the worker thread only for
 * one vblank's render call at a time, never across its whole backlog-drain
 * loop.
 */
void N3DS_AudioLock(void);
void N3DS_AudioUnlock(void);

/*
 * ndsp output sink for one vblank's worth of interleaved stereo PCM (plain
 * short, not libctru types, to keep this header platform-free).
 *
 * sampleRate is the rate the samples were produced at, not assumed 44100:
 * the XA path feeds this channel source PCM at its own 37800/18900 rate and
 * lets the DSP resample in hardware, avoiding a CPU resampler. The worker's
 * XA path submits 630 (or 315) frames at 37800 (or 18900); the no-worker
 * StepVBlank fallback submits 735 at 44100 (full software mixer). Exactly
 * one producer exists per process lifetime.
 *
 * gainLeft/gainRight scale the channel (ndspChnSetMix); the fallback passes
 * 1.0f since its mixer already applied volume.
 *
 * frameCount must be 1..N3DS_NDSP_PARCEL_FRAMES_MAX. Returns 1 if queued, 0
 * if dropped (counted).
 */
int N3DS_NdspSubmitMusicParcel(const short *frames, int frameCount, int sampleRate, float gainLeft, float gainRight);

/* Did ndspInit succeed? False also before audio init and after shutdown.
 * When false, submits are refused and the build behaves like the old muted
 * one (mixer still renders; nothing is audible). */
int N3DS_NdspIsUp(void);

/* Diagnostics for the renderer's periodic audio shape line: current
 * queued/playing parcel count (one vblank of audio each, whatever the source
 * rate), and cumulative parcels dropped because the pool was full (or a
 * submit was mis-sized). */
int N3DS_NdspGetQueuedBlocks(void);
int N3DS_NdspGetDroppedBlocks(void);

/*
 * 3DS-only output-stage gain, in percent of SPU-faithful amplitude: applied
 * where mix levels meet ndsp (music channel + hardware voices), never inside
 * the mixer, so PC byte-identity is untouched. Retail's 0x3fff master is
 * half of full digital scale, so the default is 200. `percent` 0..400 sets;
 * negative resets to the default. Returns the percent now in effect.
 * Runtime: dumpreq `vol N`.
 */
int N3DS_AudioSetOutputGainPercent(int percent);

/* Sink aggregation mode receipts: cumulative counts of full
 * 8-parcel aggregate submissions vs early anti-underrun small flushes. On
 * hardware, full >> early means the seam-rate fix actually engaged; early
 * dominating means the DSP kept running dry and the sink degenerated to
 * crumbs (Azahar always looks like the latter -- its HLE gulps the queue --
 * so only a hardware log can score this). */
void N3DS_NdspGetFlushCounts(int *fullFlushes, int *earlyFlushes);

/* Cumulative count of submits where the DSP-side queue was already empty -- a
 * real playback gap, the XA-pop signal (distinct from aggEarly, which counts
 * the recovery flushes). Trends toward 0 when the handoff keeps the DSP fed. */
int N3DS_NdspGetDspEmptyEvents(void);

/*
 * Hardware voices (plan B): the PS1's 24 SPU voices play on ndsp channels
 * 0..22, dynamically mapped -- a voice holds a channel from key-on until it
 * stops or the DSP consumes its last buffer, and a key-on with no channel of
 * its own takes the idle channel unused longest. Channel 23 is the parcel
 * sink above (music/XA). Every function here is called under native_audio.c's
 * output lock, from either the mixing thread's mirror or the game thread's
 * SpuSetKey/SpuWrite; pointers passed here must come from
 * N3DS_HwVoiceStoreAlloc, whose linear memory the DSP reads directly.
 *
 * Store contract: Alloc a slot, decode into it, Commit (cache flush) --
 * then it may be handed to N3DS_HwVoiceStart. Free returns a slot to the
 * store; silence any voice still pointing at it first, because the DSP
 * reads the block until its channel is cleared. StoreReset silences every
 * hardware channel and recycles the whole store (SPU bank re-upload);
 * callers must re-Alloc afterwards.
 */
short *N3DS_HwVoiceStoreAlloc(int sampleCount);
void N3DS_HwVoiceStoreFree(const short *pcm);
void N3DS_HwVoiceStoreCommit(const short *pcm, int sampleCount);
void N3DS_HwVoiceStoreReset(void);
void N3DS_HwVoiceStart(int voice, const short *pcm, int sampleCount, int loopStart, int loopEnd, int loopEnabled, int startSample, float rateHz);
void N3DS_HwVoiceSetRate(int voice, float rateHz);
void N3DS_HwVoiceSetMix(int voice, float left, float right);
void N3DS_HwVoiceStop(int voice);
/* Key-ons that found every channel still sounding (a lost sound), and key-ons
 * that reused an idle channel another voice held (the map doing its job). */
int N3DS_HwVoiceGetDenied(void);
int N3DS_HwVoiceGetRebinds(void);
/* Bytes currently held by live slots, and the tail cursor's high-water mark --
 * the gap between them is reclaimed space. Drops are allocations the store
 * could not serve; resets are whole-store recycles at a bank reload. */
int N3DS_HwVoiceStoreGetUsedKiB(void);
int N3DS_HwVoiceStoreGetHiWaterKiB(void);
int N3DS_HwVoiceStoreGetDrops(void);
int N3DS_HwVoiceStoreGetResets(void);

#endif
